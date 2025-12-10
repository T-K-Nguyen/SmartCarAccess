// NFC session implementation: encapsulates PN532 setup, polling, provisioning APDUs, and admin commands.
// Refactored for readability: clearer function names, smaller helpers, consistent naming and comments.

#include "nfc_session.h"
#include "provisioning_phase.h"
#include "fsm/fsm.h"
#include "fsm/fsm_integration.h"
#include <esp_system.h>
#include <esp_log.h>
#include <cstring>

namespace {
    // PN532 objects and session state
    PN532_HSU* hsu = nullptr;
    PN532* nfc = nullptr;
    bool samConfigured = false;

    // Startup guard to ignore early serial noise
    uint32_t bootMillis = 0;

    // HCE AID used when SELECTing the phone applet
    uint8_t aid[] = {0xF0,0x01,0x02,0x03,0x04,0x05};

    // Small utility: print hex bytes to Serial
    void printHex(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            Serial.printf("%02X", data[i]);
            if (i + 1 < len) Serial.print(' ');
        }
        Serial.println();
    }

    // Repeatedly try an INDATAEXCHANGE (APDU) with retry/backoff
    bool apduRetry(const uint8_t* apdu, uint8_t apduLen, uint8_t* resp, uint8_t* respLen,
                                 uint8_t attempts = 3, uint16_t backoffMs = 60) {
        for (uint8_t attempt = 1; attempt <= attempts; ++attempt) {
            uint8_t len = *respLen;
            bool ok = nfc->inDataExchange((uint8_t*)apdu, apduLen, resp, &len);
            if (ok) {
                *respLen = len;
                return true;
            }
            if (attempt < attempts) delay(backoffMs);
        }
        return false;
    }

    // Try to configure the SAM (Secure Access Module) with retries
    bool ensureSAMConfig(uint8_t attempts = 5, uint16_t backoffMs = 150) {
        for (uint8_t i = 1; i <= attempts; ++i) {
            if (nfc->SAMConfig()) return true;
            delay(backoffMs);
        }
        return false;
    }

    // Explicitly release any active target/session on the PN532 to return to idle.
    // This prevents the next poll from requiring a "priming" tap.
    bool releaseTarget() {
        // Some libraries return bool; ignore result if not supported.
        return nfc->inRelease();
    }

    // Wait for a card to appear using adaptive polling. Returns true when stable detected.
    bool waitForCard(uint32_t timeoutMs = 15000, uint8_t stableReads = 2, uint16_t pollMs = 60) {
        // Ensure previous sessions are fully released before starting a new poll loop
        releaseTarget();

        const uint32_t start = millis();
        uint8_t stableCount = 0;
        uint32_t lastDot = 0;
        Serial.println("Waiting for card (adaptive poll)...");
        while (true) {
            bool present = nfc->inListPassiveTarget();
            if (present) {
                if (++stableCount >= stableReads) {
                    Serial.println("Card detected (stable)");
                    return true;
                }
            } else {
                stableCount = 0;
            }
            uint32_t now = millis();
            if (now - lastDot >= 2000) { Serial.print('.'); lastDot = now; }
            if (timeoutMs && now - start >= timeoutMs) {
                Serial.println("\nTimeout waiting for card");
                return false;
            }
            delay(pollMs);
        }
    }

    // Wait for a card to be removed. Returns true when stable absent detected.
    bool waitForRemoval(uint32_t timeoutMs = 8000, uint8_t stableAbsentReads = 3, uint16_t pollMs = 80) {
        const uint32_t start = millis();
        uint8_t absentCount = 0;
        while (true) {
            bool present = nfc->inListPassiveTarget();
            if (!present) {
                if (++absentCount >= stableAbsentReads) {
                    Serial.println("Card removed (stable)\n");
                    return true;
                }
            } else {
                absentCount = 0;
            }
            if (timeoutMs && millis() - start >= timeoutMs) {
                Serial.println("Removal wait timeout");
                return false;
            }
            delay(pollMs);
        }
    }

    // After a session (successful SELECT and optional provisioning), either wait briefly for removal
    // or continue to the next cycle to keep UX snappy. For failed SELECT, just short backoff.
    void postSessionCooldown(bool hadSelectSuccess) {
        if (hadSelectSuccess) {
            // Wait up to ~1.5s for user to remove phone; if not, continue anyway
            bool removed = waitForRemoval(1500, 2, 80);
            if (!removed) {
                Serial.println("[Session] Proceeding without removal (phone still present)");
            }
            // In any case, release the target to ensure the next poll starts cleanly
            releaseTarget();
        } else {
            // Wrong tag / SELECT failed: small backoff to avoid tight loop
            delay(200);
            // Release just in case there is a half-open session
            releaseTarget();
        }
    }

    // Build and send SELECT AID APDU; returns SW1, SW2 and payload length
    bool selectAid(uint8_t& sw1, uint8_t& sw2, int& payloadLen) {
        uint8_t sel[16];
        uint8_t idx = 0;
        sel[idx++] = 0x00; sel[idx++] = 0xA4; sel[idx++] = 0x04; sel[idx++] = 0x00;
        sel[idx++] = sizeof(aid);
        for (size_t i = 0; i < sizeof(aid); ++i) sel[idx++] = aid[i];
        sel[idx++] = 0x00; // Le

        uint8_t resp[32];
        uint8_t rlen = sizeof(resp);
        if (!apduRetry(sel, idx, resp, &rlen, 3, 80)) return false;

        sw1 = (rlen >= 2) ? resp[rlen - 2] : 0x00;
        sw2 = (rlen >= 2) ? resp[rlen - 1] : 0x00;
        payloadLen = (rlen >= 2) ? (rlen - 2) : 0;
        return true;
    }

    // Parse the "Base GET_CHALLENGE" response layout:
    // [keyIdLen][keyId..][pubKey65..][certLen(2)][cert..]
    struct BaseInfo {
        String keyId;
        const uint8_t* pubKey65 = nullptr;
        uint16_t certLen = 0;
        const uint8_t* cert = nullptr;
        bool hasPub = false;
    };

    bool parseBaseResponse(const uint8_t* data, int dataLen, BaseInfo& out) {
        if (dataLen < 1 + 65 + 2) return false; // minimal sizes
        int idx = 0;
        uint8_t keyIdLen = data[idx++];
        if (keyIdLen > dataLen - idx) return false;
        String keyId;
        for (uint8_t k = 0; k < keyIdLen; ++k) keyId += (char)data[idx + k];
        idx += keyIdLen;

        if (idx + 65 + 2 > dataLen) return false;
        const uint8_t* pubKey65 = data + idx; idx += 65;
        uint16_t certLen = (uint16_t)(data[idx] << 8 | data[idx + 1]); idx += 2;
        if (idx + certLen > dataLen) return false;
        const uint8_t* cert = data + idx;

        out.keyId = keyId;
        out.pubKey65 = pubKey65;
        out.certLen = certLen;
        out.cert = cert;
        out.hasPub = (pubKey65[0] == 0x04);
        return true;
    }

    // Execute the two-step provisioning exchange when appropriate.
    void performProvisioningIfNeeded(uint8_t sw1, uint8_t sw2) {
        bool alreadyProvisioned = ProvisioningPhase::isProvisioned();
        bool shouldForce = ProvisioningPhase::isForceProvisioning();
        bool shouldProvision = (shouldForce || !alreadyProvisioned) && (sw1 == 0x90 && sw2 == 0x00);
        if (!shouldProvision) return;

        // ------------------------------
        // Step 1: Base GET_CHALLENGE (Lc=0)
        // ------------------------------
        Serial.println("[PhaseA] Step1: Base GET_CHALLENGE (Lc=0)");
        uint8_t baseApdu[] = {0x00, 0xCA, 0x00, 0x00, 0x00, 0x00};
        uint8_t baseResp[255]; uint8_t baseLen = sizeof(baseResp);

        Serial.print("[PhaseA] OUT Base APDU: "); printHex(baseApdu, sizeof(baseApdu));
        if (!(apduRetry(baseApdu, sizeof(baseApdu), baseResp, &baseLen, 5, 150)
                    && baseLen >= 2 && baseResp[baseLen - 2] == 0x90 && baseResp[baseLen - 1] == 0x00)) {
            Serial.println("[PhaseA] Base GET_CHALLENGE failed");
            return;
        }

        Serial.printf("[PhaseA] IN Base Resp len=%u SW=%02X%02X\n", baseLen, baseResp[baseLen - 2], baseResp[baseLen - 1]);
        int payloadLen = baseLen - 2;
        const uint8_t* payload = baseResp;
        BaseInfo info;
        if (!parseBaseResponse(payload, payloadLen, info)) {
            Serial.println("[PhaseA] Base payload parse error");
            return;
        }

        Serial.printf("[PhaseA] Parsed Base: keyId='%s' pubKeyFirst=%02X certLen=%u\n",
                                    info.keyId.c_str(), info.pubKey65 ? info.pubKey65[0] : 0, info.certLen);
        if (info.hasPub) {
            Serial.print("[PhaseA] pubKey65: "); printHex(info.pubKey65, 65);
        }
        if (info.certLen > 0) {
            Serial.print("[PhaseA] cert (first up to 32 bytes): ");
            size_t show = info.certLen < 32 ? info.certLen : 32;
            printHex(info.cert, show);
        } else {
            Serial.println("[PhaseA] cert: <none>");
        }

        // ------------------------------
        // Step 2: Signature GET_CHALLENGE (Lc=24)
        // ------------------------------
        Serial.println("[PhaseA] Step2: Signature GET_CHALLENGE (Lc=24 challenge)");
        uint8_t vehicleId[8];
        uint8_t nonce[16];
        for (int i = 0; i < 8; ++i) vehicleId[i] = (uint8_t)esp_random();
        for (int i = 0; i < 16; ++i) nonce[i] = (uint8_t)esp_random();

        uint8_t challenge[24];
        memcpy(challenge, vehicleId, 8);
        memcpy(challenge + 8, nonce, 16);

        Serial.print("[PhaseA] vehicleId: "); printHex(vehicleId, 8);
        Serial.print("[PhaseA] nonce: "); printHex(nonce, 16);
        Serial.print("[PhaseA] challenge(24): "); printHex(challenge, 24);

        // Build signature APDU: CLA INS P1 P2 Lc [challenge] Le
        uint8_t sigApdu[5 + sizeof(challenge) + 1];
        uint8_t sIdx = 0;
        sigApdu[sIdx++] = 0x00; sigApdu[sIdx++] = 0xCA; sigApdu[sIdx++] = 0x00; sigApdu[sIdx++] = 0x00;
        sigApdu[sIdx++] = sizeof(challenge);
        memcpy(sigApdu + sIdx, challenge, sizeof(challenge)); sIdx += sizeof(challenge);
        sigApdu[sIdx++] = 0x00; // Le

        Serial.print("[PhaseA] OUT Sig APDU: "); printHex(sigApdu, sIdx);
        uint8_t sigResp[255]; uint8_t sigTotalLen = sizeof(sigResp);
        if (!(apduRetry(sigApdu, sIdx, sigResp, &sigTotalLen, 5, 150)
                    && sigTotalLen >= 2 && sigResp[sigTotalLen - 2] == 0x90 && sigResp[sigTotalLen - 1] == 0x00)) {
            Serial.println("[PhaseA] Signature GET_CHALLENGE failed");
            return;
        }

        Serial.printf("[PhaseA] IN Sig Resp len=%u SW=%02X%02X\n", sigTotalLen, sigResp[sigTotalLen - 2], sigResp[sigTotalLen - 1]);
        int sigPayloadLen = sigTotalLen - 2;
        if (sigPayloadLen < 2) { Serial.println("[PhaseA] Signature payload too short"); return; }

        uint16_t sigLen = (uint16_t)(sigResp[0] << 8 | sigResp[1]);
        if (2 + sigLen > sigPayloadLen) { Serial.println("[PhaseA] sigLen OOB"); return; }
        const uint8_t* sigDer = sigResp + 2;

        Serial.print("[PhaseA] sigDer (first up to 32 bytes): ");
        size_t showSig = sigLen < 32 ? sigLen : 32;
        printHex(sigDer, showSig);

        // Verify signature if we have the public key. Fallback: accept empty signature (sigLen==0)
        bool sigOK = false;
        if (sigLen > 0 && info.hasPub) {
            sigOK = ProvisioningPhase::verifySignatureP256(info.pubKey65, challenge, sizeof(challenge), sigDer, sigLen);
        } else if (sigLen == 0) {
            sigOK = true; // fallback acceptance
        }

        Serial.printf("[PhaseA] sigLen=%u verify=%s\n", sigLen, sigOK ? "OK" : "FAIL");
        if (!sigOK) { Serial.println("[PhaseA] Credentials NOT stored (signature failure)"); return; }

        // Decide whether to store provisioning data
        String existingKeyId;
        bool haveExisting = ProvisioningPhase::getKeyId(existingKeyId);
        bool sameKey = haveExisting && existingKeyId == info.keyId;

        if (!alreadyProvisioned || shouldForce) {
            // Store keyId according to force flags and whether it differs from stored value
            if (!sameKey) {
                if (shouldForce) {
                    ProvisioningPhase::storeKeyIdAsciiForce(info.keyId.c_str());
                } else {
                    ProvisioningPhase::storeKeyIdAsciiIfEmpty(info.keyId.c_str());
                }
            } else {
                Serial.println("[PhaseA] Same keyId as stored; skipping keyId re-store");
            }

            if (info.hasPub) ProvisioningPhase::storePhonePubRaw(info.pubKey65);
            if (info.certLen > 0) ProvisioningPhase::storeCertChain(info.cert, info.certLen);
            Serial.println("[PhaseA] Provisioning data persisted (keyId/pub/cert as available)");

            // Trigger FSM provision success event
            FSMIntegration::NFC::onCredentialsStored();
            
            // Note: One-shot force is auto-cleared by ProvisioningPhase::storeKeyIdAsciiIfEmpty()
        } else {
            Serial.println("[PhaseA] Already provisioned; skipping store (toggle 'f' to force)");
        }
    }

    // Process incoming serial admin commands (debounced and guarded by startup time)
    void handleSerialCommands() {
        static uint32_t lastCmdMs = 0;
        static char lastCmd = 0;

        // Ignore serial input for the first 1500ms after boot to avoid garbage triggering destructive commands
        if (millis() - bootMillis < 1500) {
            while (Serial.available()) Serial.read(); // flush
            return;
        }

        while (Serial.available()) {
            char c = Serial.read();
            if (c < 32 || c > 126) continue; // non-printable guard

            uint32_t now = millis();
            if (c == lastCmd && now - lastCmdMs < 250) continue; // debounce bursts
            lastCmd = c; lastCmdMs = now;

            if (c == 'p') {
                // Show FSM diagnostics
                FSMIntegration::SerialCmd::printDiagnostics();
            } else if (c == 'f') {
                FSMIntegration::SerialCmd::toggleForceProvision();
            } else if (c == 'F') {
                FSMIntegration::SerialCmd::armOneShotForce();
            } else if (c == 'r') {
                FSMIntegration::SerialCmd::clearKeys();
            } else if (c == 'C') {
                FSMIntegration::SerialCmd::clearAll();
            } else if (c == 'v') {
                bool ok = ProvisioningPhase::validateStoredCertMatchesStoredPub();
                Serial.printf("[Admin] Cert vs pub match: %s\n", ok ? "YES" : "NO/UNAVAILABLE");
            } else if (c == 'x') {
                uint8_t pub[65]; size_t pubLen = ProvisioningPhase::getPhonePubRaw(pub, sizeof(pub));
                if (pubLen == 65) { Serial.print("[Admin] pubKey65: "); printHex(pub, 65); }
                else Serial.println("[Admin] pubKey not stored");
            } else if (c == 'h') {
                Serial.println("Commands: p f F r C v x h");
            }
        }
    }
} // anonymous namespace

namespace NfcSession {
    void begin(HardwareSerial& uart, int rxPin, int txPin, uint32_t baud) {
        uart.begin(baud, SERIAL_8N1, rxPin, txPin);
        delay(100);

        hsu = new PN532_HSU(uart);
        nfc = new PN532(*hsu);

        Serial.println("\n=== PN532 HCE READER (refactored) ===");
        nfc->begin();

        ProvisioningPhase::begin();
        esp_log_level_set("Preferences", ESP_LOG_NONE); // suppress noisy NVS NOT_FOUND logs

        bootMillis = millis(); // mark startup time to guard serial noise

        uint32_t ver = nfc->getFirmwareVersion();
        if (!ver) {
            Serial.println("PN532 NOT FOUND!");
            return;
        }
        Serial.printf("PN532 Firmware %d.%d\n", (int)((ver >> 16) & 0xFF), (int)((ver >> 8) & 0xFF));

        Serial.print("Configuring SAM...");
        samConfigured = ensureSAMConfig(5, 200);
        Serial.println(samConfigured ? "OK" : "SAM CONFIG FAILED (will still attempt to poll)");

        Serial.printf("[PhaseA] Provisioned=%s  Force=%s\n",
                                    ProvisioningPhase::isProvisioned() ? "YES" : "NO",
                                    ProvisioningPhase::isForceProvisioning() ? "ON" : "OFF");

        Serial.println("[PhaseA] Serial: p=print, f=toggle persistent force, F=one-shot force, r=reset phone, C=clear ALL, v=validate cert, x=dump pub, h=help");
    }

    void tick() {
        // Process admin serial commands
        handleSerialCommands();

        // Wait for card; if none, try to refresh SAMConfig occasionally
        if (!waitForCard(15000, 2, 60)) {
            if (!samConfigured) {
                Serial.println("Re-attempting SAMConfig...");
                samConfigured = ensureSAMConfig(3, 200);
                if (samConfigured) Serial.println("SAM configured successfully");
            }
            return;
        }

        // SELECT the HCE AID on the phone
        uint8_t sw1 = 0, sw2 = 0;
        int payloadLen = 0;
        bool selectOk = selectAid(sw1, sw2, payloadLen);
        if (!selectOk) {
            Serial.println("SELECT AID failed");
            postSessionCooldown(false);
            return;
        }

        Serial.printf("SELECT SW1SW2=%02X%02X payloadLen=%d  Provisioned=%s  Force=%s\n",
                                    sw1, sw2, payloadLen,
                                    ProvisioningPhase::isProvisioned() ? "YES" : "NO",
                                    ProvisioningPhase::isForceProvisioning() ? "ON" : "OFF");

        // Possibly perform provisioning sequence after a successful SELECT
        performProvisioningIfNeeded(sw1, sw2);

        // Keep flow smooth: wait briefly for removal only when SELECT succeeded
        postSessionCooldown(sw1 == 0x90 && sw2 == 0x00);
    }

    // ---- Admin control API (delegates to ProvisioningPhase) ----
    void setPersistentForce(bool on) {
        ProvisioningPhase::setForceProvisioningFlag(on);
    }
    void armOneShotForce() {
        ProvisioningPhase::setOneShotForce(true);
    }
    bool getPersistentForce() { 
        return ProvisioningPhase::isForceProvisioning(); 
    }
    bool isOneShotArmed() { 
        return ProvisioningPhase::isForceProvisioning(); 
    }
}
