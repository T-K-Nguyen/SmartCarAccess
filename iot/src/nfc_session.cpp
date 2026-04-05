// NFC session implementation: encapsulates PN532 setup, polling, provisioning APDUs, and admin commands.
// Refactored for readability: clearer function names, smaller helpers, consistent naming and comments.

#include "nfc_session.h"
#include "provisioning_phase.h"
#include "fsm/fsm.h"
#include "fsm/fsm_integration.h"
#include "ccc_mailbox.h"
#include <esp_system.h>
#include <esp_log.h>
#include <cstring>
#include <mbedtls/md.h>

namespace {
    const bool kDebugApdu = true;
    // PN532 objects and session state
    PN532_HSU* hsu = nullptr;
    PN532* nfc = nullptr;
    bool samConfigured = false;

    // Startup guard to ignore early serial noise
    uint32_t bootMillis = 0;

    // HCE AID used when SELECTing the phone applet (CCC Release 3)
    uint8_t aid[] = {0xA0,0x00,0x00,0x08,0x09,0x43,0x43,0x43,0x44,0x4B,0x46,0x76,0x31};

    // Master card binding fallback (used only if CCC mailbox vehicleId is missing)
    const uint8_t EXPECTED_VEHICLE_ID[8] = {0x56, 0x4E, 0x38, 0x38, 0x38, 0x38, 0x42, 0x4B};
    const uint8_t MASTER_SECRET[32] = {
        0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
        0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
        0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
        0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18
    };

    const uint8_t kInsSpake2Request = 0x30;
    const uint8_t kInsSpake2Verify = 0x32;
    const uint8_t kInsOpControl = 0x3C;
    const uint8_t kTlvTagChallenge = 0x50;
    const uint8_t kTlvTagMac = 0x58;

    // Small utility: print hex bytes to Serial
    void printHex(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            Serial.printf("%02X", data[i]);
            if (i + 1 < len) Serial.print(' ');
        }
        Serial.println();
    }

    bool hmacSha256(const uint8_t* key, size_t keyLen,
                    const uint8_t* data, size_t dataLen,
                    uint8_t* out32) {
        if (!key || !data || !out32) return false;
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (!info) { mbedtls_md_free(&ctx); return false; }
        if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return false; }
        int rc = mbedtls_md_hmac_starts(&ctx, key, keyLen);
        if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, data, dataLen);
        if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, out32);
        mbedtls_md_free(&ctx);
        return rc == 0;
    }

    bool apduRetry(const uint8_t* apdu, uint8_t apduLen, uint8_t* resp, uint8_t* respLen,
                                 uint8_t attempts, uint16_t backoffMs);
    bool ensureSelected(uint32_t timeoutMs);

    bool unwrap7F24(const uint8_t* payload, int payloadLen, const uint8_t** out, int* outLen) {
        if (!payload || payloadLen < 3 || !out || !outLen) return false;
        if (payload[0] != 0x7F || payload[1] != 0x24) return false;
        int idx = 2;
        if (idx >= payloadLen) return false;
        uint8_t lenByte = payload[idx++];
        int len = 0;
        if (lenByte == 0x81) {
            if (idx >= payloadLen) return false;
            len = payload[idx++];
        } else if (lenByte <= 0x7F) {
            len = lenByte;
        } else {
            return false;
        }
        if (idx + len > payloadLen) return false;
        *out = payload + idx;
        *outLen = len;
        return true;
    }

    bool extractTlv(uint8_t tag, const uint8_t* buf, int len, const uint8_t** value, int* valueLen) {
        if (!buf || len < 2 || !value || !valueLen) return false;
        int idx = 0;
        while (idx + 2 <= len) {
            uint8_t t = buf[idx++];
            uint8_t l = buf[idx++];
            if (idx + l > len) return false;
            if (t == tag) {
                *value = buf + idx;
                *valueLen = l;
                return true;
            }
            idx += l;
        }
        return false;
    }

    bool sendSpake2Mastercard(const uint8_t* challenge, size_t challengeLen) {
        if (!challenge || challengeLen == 0 || challengeLen > 32) return false;

        // SPAKE2+ REQUEST (INS 0x30) with TLV tag 0x50 carrying the challenge
        if (!ensureSelected(2500)) {
            Serial.println("[PhaseA] SELECT before SPAKE2+ request failed");
            return false;
        }
        uint8_t reqApdu[5 + 2 + 32 + 1];
        uint8_t idx = 0;
        uint8_t tlvLen = (uint8_t)(2 + challengeLen);
        reqApdu[idx++] = 0x00; reqApdu[idx++] = kInsSpake2Request; reqApdu[idx++] = 0x00; reqApdu[idx++] = 0x00;
        reqApdu[idx++] = tlvLen;
        reqApdu[idx++] = kTlvTagChallenge;
        reqApdu[idx++] = (uint8_t)challengeLen;
        memcpy(reqApdu + idx, challenge, challengeLen); idx += (uint8_t)challengeLen;
        reqApdu[idx++] = 0x00; // Le

        uint8_t reqResp[64]; uint8_t reqLen = sizeof(reqResp);
        bool reqOk = apduRetry(reqApdu, idx, reqResp, &reqLen, 3, 120)
            && reqLen >= 2 && reqResp[reqLen - 2] == 0x90 && reqResp[reqLen - 1] == 0x00;
        if (!reqOk) {
            Serial.println("[PhaseA] SPAKE2+ REQUEST failed");
            return false;
        }

        // SPAKE2+ VERIFY (INS 0x32) expects TLV tag 0x58 with MAC
        if (!ensureSelected(2500)) {
            Serial.println("[PhaseA] SELECT before SPAKE2+ verify failed");
            return false;
        }
        uint8_t verApdu[] = {0x00, kInsSpake2Verify, 0x00, 0x00, 0x00};
        uint8_t verResp[96]; uint8_t verLen = sizeof(verResp);
        bool verOk = apduRetry(verApdu, sizeof(verApdu), verResp, &verLen, 3, 120)
            && verLen >= 2 && verResp[verLen - 2] == 0x90 && verResp[verLen - 1] == 0x00;
        if (!verOk) {
            Serial.println("[PhaseA] SPAKE2+ VERIFY failed");
            return false;
        }

        int payloadLen = verLen - 2;
        const uint8_t* payload = verResp;
        const uint8_t* mac = nullptr;
        int macLen = 0;
        if (!extractTlv(kTlvTagMac, payload, payloadLen, &mac, &macLen) || macLen != 32) {
            Serial.println("[PhaseA] SPAKE2+ VERIFY missing MAC tag 0x58");
            return false;
        }

        uint8_t macExpect[32];
        if (!hmacSha256(MASTER_SECRET, sizeof(MASTER_SECRET), challenge, challengeLen, macExpect)) {
            Serial.println("[PhaseA] SPAKE2+ HMAC compute failed");
            return false;
        }
        bool macOk = (memcmp(mac, macExpect, 32) == 0);
        Serial.printf("[PhaseA] SPAKE2+ MAC verify=%s\n", macOk ? "OK" : "FAIL");
        return macOk;
    }

    bool sendOpControlFlowSuccess() {
        if (!ensureSelected(1500)) {
            Serial.println("[PhaseA] SELECT before OP CONTROL FLOW failed");
            return false;
        }
        uint8_t apdu[] = {0x00, kInsOpControl, 0x11, 0x00, 0x00};
        uint8_t resp[16]; uint8_t rlen = sizeof(resp);
        bool ok = apduRetry(apdu, sizeof(apdu), resp, &rlen, 2, 80)
            && rlen >= 2 && resp[rlen - 2] == 0x90 && resp[rlen - 1] == 0x00;
        if (!ok) {
            Serial.println("[PhaseA] OP CONTROL FLOW failed");
        }
        return ok;
    }

    bool getVehicleIdBytes(uint8_t out[8]) {
        const char* vid = CCCMailbox::vehicleId();
        if (vid && strlen(vid) == 8) {
            memcpy(out, vid, 8);
            return true;
        }
        memcpy(out, EXPECTED_VEHICLE_ID, 8);
        return false;
    }

    // Repeatedly try an INDATAEXCHANGE (APDU) with retry/backoff
    bool apduRetry(const uint8_t* apdu, uint8_t apduLen, uint8_t* resp, uint8_t* respLen,
                                 uint8_t attempts = 3, uint16_t backoffMs = 60) {
        for (uint8_t attempt = 1; attempt <= attempts; ++attempt) {
            uint8_t len = *respLen;
            bool ok = nfc->inDataExchange((uint8_t*)apdu, apduLen, resp, &len);
            if (kDebugApdu) {
                Serial.printf("[APDU] try=%u ok=%s len=%u\n", attempt, ok ? "YES" : "NO", len);
                if (ok && len >= 2) {
                    Serial.printf("[APDU] SW=%02X%02X\n", resp[len - 2], resp[len - 1]);
                }
                if (ok && len > 0 && len <= 40) {
                    Serial.print("[APDU] resp: "); printHex(resp, len);
                }
            }
            if (ok && len >= 2) {
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
        bool ok = nfc->inRelease();
        if (kDebugApdu) {
            Serial.printf("[PN532] inRelease=%s\n", ok ? "OK" : "FAIL");
        }
        return ok;
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

    bool selectAid(uint8_t& sw1, uint8_t& sw2, int& payloadLen);

    bool ensureSelected(uint32_t timeoutMs = 2500) {
        // Fast path: try SELECT immediately in case target is already active.
        uint8_t sw1 = 0, sw2 = 0; int payloadLen = 0;
        if (selectAid(sw1, sw2, payloadLen) && sw1 == 0x90 && sw2 == 0x00) {
            return true;
        }

        const uint32_t start = millis();
        while (millis() - start < timeoutMs) {
            if (!nfc->inListPassiveTarget()) {
                delay(50);
                continue;
            }
            sw1 = 0; sw2 = 0; payloadLen = 0;
            if (selectAid(sw1, sw2, payloadLen) && sw1 == 0x90 && sw2 == 0x00) {
                return true;
            }
            releaseTarget();
            delay(80);
        }
        return false;
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
        uint8_t sel[32];
        uint8_t idx = 0;
        sel[idx++] = 0x00; sel[idx++] = 0xA4; sel[idx++] = 0x04; sel[idx++] = 0x00;
        sel[idx++] = sizeof(aid);
        for (size_t i = 0; i < sizeof(aid); ++i) sel[idx++] = aid[i];
        sel[idx++] = 0x00; // Le

        uint8_t resp[64];
        uint8_t rlen = sizeof(resp);
        if (!apduRetry(sel, idx, resp, &rlen, 3, 80)) {
            if (kDebugApdu) Serial.println("[APDU] SELECT failed (no response)");
            return false;
        }

        sw1 = (rlen >= 2) ? resp[rlen - 2] : 0x00;
        sw2 = (rlen >= 2) ? resp[rlen - 1] : 0x00;
        payloadLen = (rlen >= 2) ? (rlen - 2) : 0;
        if (kDebugApdu) {
            Serial.printf("[APDU] SELECT SW=%02X%02X payloadLen=%d\n", sw1, sw2, payloadLen);
        }
        return true;
    }

    bool sendProvisionResult() {
        // Notify phone HCE that provisioning succeeded: INS=0xDA, data=0x01
        uint8_t sw1a = 0, sw2a = 0; int pla = 0;
        if (!selectAid(sw1a, sw2a, pla) || sw1a != 0x90 || sw2a != 0x00) {
            Serial.println("[PhaseA] SELECT before provision result failed");
            return false;
        }
        delay(80);
        uint8_t apdu[] = {0x00, 0xDA, 0x00, 0x00, 0x01, 0x01};
        uint8_t resp[16];
        uint8_t rlen = sizeof(resp);
        bool ok = apduRetry(apdu, sizeof(apdu), resp, &rlen, 2, 80);
        if (ok && rlen >= 2) {
            Serial.printf("[PhaseA] Provision result ack SW=%02X%02X len=%u\n", resp[rlen - 2], resp[rlen - 1], rlen);
        } else {
            Serial.println("[PhaseA] Provision result ack failed");
        }
        return ok && rlen >= 2 && resp[rlen - 2] == 0x90 && resp[rlen - 1] == 0x00;
    }

    // Parse the "Base GET_CHALLENGE" response layout (keyId is ignored in CCC flow):
    // [keyIdLen][keyId..][pubKey65..][certLen(2)][cert..]
    struct BaseInfo {
        uint8_t keyIdLen = 0;
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
        idx += keyIdLen; // keyId is ignored in CCC flow

        if (idx + 65 + 2 > dataLen) return false;
        const uint8_t* pubKey65 = data + idx; idx += 65;
        uint16_t certLen = (uint16_t)(data[idx] << 8 | data[idx + 1]); idx += 2;
        if (idx + certLen > dataLen) return false;
        const uint8_t* cert = data + idx;

        out.keyIdLen = keyIdLen;
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
        
        // If already provisioned and force mode is OFF, reject provisioning attempt
        if (alreadyProvisioned && !shouldForce) {
            Serial.println("[PhaseA] ⚠️  Device already provisioned. Use 'f' or 'F' command to force re-provisioning.");
            return;
        }
        
        bool shouldProvision = (shouldForce || !alreadyProvisioned) && (sw1 == 0x90 && sw2 == 0x00);
        if (!shouldProvision) return;

        // ------------------------------
        // Step 1: Base GET DATA (Lc=0)
        // ------------------------------
        Serial.println("[PhaseA] Step1: Base GET DATA (Lc=0)");
        if (!ensureSelected(2500)) {
            Serial.println("[PhaseA] SELECT before base failed");
            return;
        }
        uint8_t baseApduNoLe[] = {0x00, 0xCA, 0x00, 0x00};
        uint8_t baseApduLe0[] = {0x00, 0xCA, 0x00, 0x00, 0x00};
        uint8_t baseResp[255]; uint8_t baseLen = sizeof(baseResp);

        Serial.print("[PhaseA] OUT Base APDU: "); printHex(baseApduNoLe, sizeof(baseApduNoLe));
        delay(120);
        bool baseOk = apduRetry(baseApduNoLe, sizeof(baseApduNoLe), baseResp, &baseLen, 5, 150)
            && baseLen >= 2 && baseResp[baseLen - 2] == 0x90 && baseResp[baseLen - 1] == 0x00;
        if (!baseOk) {
            baseLen = sizeof(baseResp);
            Serial.print("[PhaseA] OUT Base APDU (Le=0): "); printHex(baseApduLe0, sizeof(baseApduLe0));
            delay(120);
            baseOk = apduRetry(baseApduLe0, sizeof(baseApduLe0), baseResp, &baseLen, 5, 150)
                && baseLen >= 2 && baseResp[baseLen - 2] == 0x90 && baseResp[baseLen - 1] == 0x00;
        }
        if (!baseOk) {
            Serial.println("[PhaseA] Base GET DATA failed (reselect + retry)");
            if (ensureSelected(2500)) {
                delay(160);
                baseLen = sizeof(baseResp);
                Serial.print("[PhaseA] OUT Base APDU (retry): "); printHex(baseApduNoLe, sizeof(baseApduNoLe));
                baseOk = apduRetry(baseApduNoLe, sizeof(baseApduNoLe), baseResp, &baseLen, 5, 150)
                    && baseLen >= 2 && baseResp[baseLen - 2] == 0x90 && baseResp[baseLen - 1] == 0x00;
                if (!baseOk) {
                    baseLen = sizeof(baseResp);
                    Serial.print("[PhaseA] OUT Base APDU (retry Le=0): "); printHex(baseApduLe0, sizeof(baseApduLe0));
                    baseOk = apduRetry(baseApduLe0, sizeof(baseApduLe0), baseResp, &baseLen, 5, 150)
                        && baseLen >= 2 && baseResp[baseLen - 2] == 0x90 && baseResp[baseLen - 1] == 0x00;
                }
            }
        }
        if (!baseOk) {
            Serial.println("[PhaseA] Base GET DATA failed");
            Serial.print("[PhaseA] Last base resp: "); printHex(baseResp, baseLen);
            return;
        }

        Serial.printf("[PhaseA] IN Base Resp len=%u SW=%02X%02X\n", baseLen, baseResp[baseLen - 2], baseResp[baseLen - 1]);
        int payloadLen = baseLen - 2;
        const uint8_t* payload = baseResp;
        const uint8_t* basePayload = payload;
        int basePayloadLen = payloadLen;
        if (payloadLen >= 2 && payload[0] == 0x7F && payload[1] == 0x24) {
            const uint8_t* inner = nullptr;
            int innerLen = 0;
            if (!unwrap7F24(payload, payloadLen, &inner, &innerLen)) {
                Serial.println("[PhaseA] GET DATA 7F24 parse failed");
                return;
            }
            basePayload = inner;
            basePayloadLen = innerLen;
            Serial.printf("[PhaseA] GET DATA 7F24 unwrapped len=%d\n", basePayloadLen);
        }

        BaseInfo info;
        if (!parseBaseResponse(basePayload, basePayloadLen, info)) {
            Serial.printf("[PhaseA] Base payload parse error (len=%d)\n", basePayloadLen);
            Serial.print("[PhaseA] Base payload: "); printHex(basePayload, basePayloadLen);
            return;
        }

        Serial.printf("[PhaseA] Parsed Base: keyIdLen=%u pubKeyFirst=%02X certLen=%u\n",
                        info.keyIdLen, info.pubKey65 ? info.pubKey65[0] : 0, info.certLen);
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
        // Step 2: Signature GET DATA (Lc=24)
        // ------------------------------
        // Some phones deselect the AID after the base response; reselect to stabilize the link.
        if (!ensureSelected(2500)) {
            Serial.println("[PhaseA] SELECT before signature failed");
            return;
        }
        delay(120);
        Serial.println("[PhaseA] Step2: Signature GET DATA (Lc=24 challenge)");
        uint8_t nonce[16];
        for (int i = 0; i < 16; ++i) nonce[i] = (uint8_t)esp_random();

        uint8_t challenge[24];
        uint8_t vehicleId[8];
        bool vidOk = getVehicleIdBytes(vehicleId);
        memcpy(challenge, vehicleId, 8);
        memcpy(challenge + 8, nonce, 16);

        if (!vidOk) {
            Serial.println("[PhaseA] WARNING: CCC vehicleId missing; using fallback");
        }
        Serial.print("[PhaseA] vehicleId: "); printHex(vehicleId, 8);
        Serial.print("[PhaseA] nonce: "); printHex(nonce, 16);
        Serial.print("[PhaseA] challenge(24): "); printHex(challenge, 24);

        // Build signature APDU: CLA INS P1 P2 Lc [challenge]
        uint8_t sigApdu[5 + sizeof(challenge)];
        uint8_t sIdx = 0;
        sigApdu[sIdx++] = 0x00; sigApdu[sIdx++] = 0xCA; sigApdu[sIdx++] = 0x00; sigApdu[sIdx++] = 0x00;
        sigApdu[sIdx++] = sizeof(challenge);
        memcpy(sigApdu + sIdx, challenge, sizeof(challenge)); sIdx += sizeof(challenge);

        Serial.print("[PhaseA] OUT Sig APDU: "); printHex(sigApdu, sIdx);
        uint8_t sigResp[255]; uint8_t sigTotalLen = sizeof(sigResp);
        bool sigOk = apduRetry(sigApdu, sIdx, sigResp, &sigTotalLen, 5, 150)
            && sigTotalLen >= 2 && sigResp[sigTotalLen - 2] == 0x90 && sigResp[sigTotalLen - 1] == 0x00;
        if (!sigOk) {
            Serial.println("[PhaseA] Signature GET DATA failed (reselect + retry)");
            uint8_t sw1c = 0, sw2c = 0; int plc = 0;
            releaseTarget();
            delay(80);
            if (selectAid(sw1c, sw2c, plc) && sw1c == 0x90 && sw2c == 0x00) {
                delay(160);
                sigTotalLen = sizeof(sigResp);
                sigOk = apduRetry(sigApdu, sIdx, sigResp, &sigTotalLen, 5, 150)
                    && sigTotalLen >= 2 && sigResp[sigTotalLen - 2] == 0x90 && sigResp[sigTotalLen - 1] == 0x00;
            }
        }
        if (!sigOk) {
            Serial.println("[PhaseA] Signature GET DATA failed");
            Serial.print("[PhaseA] Last sig resp: "); printHex(sigResp, sigTotalLen);
            return;
        }

        Serial.printf("[PhaseA] IN Sig Resp len=%u SW=%02X%02X\n", sigTotalLen, sigResp[sigTotalLen - 2], sigResp[sigTotalLen - 1]);
        int sigPayloadLen = sigTotalLen - 2;
        if (sigPayloadLen < 2) { Serial.println("[PhaseA] Signature payload too short"); return; }

        const uint8_t* sigDer = nullptr;
        uint16_t sigLen = 0;
        sigLen = (uint16_t)(sigResp[0] << 8 | sigResp[1]);
        if (2 + sigLen > sigPayloadLen) { Serial.println("[PhaseA] sigLen OOB"); return; }
        sigDer = sigResp + 2;

        Serial.print("[PhaseA] sigDer (first up to 32 bytes): ");
        size_t showSig = sigLen < 32 ? sigLen : 32;
        printHex(sigDer, showSig);

        bool sigOK = false;
        if (sigLen > 0 && info.hasPub) {
            sigOK = ProvisioningPhase::verifySignatureP256(info.pubKey65, challenge, sizeof(challenge), sigDer, sigLen);
        } else if (sigLen == 0) {
            sigOK = true; // fallback acceptance
        }

        Serial.printf("[PhaseA] sigLen=%u verify=%s\n", sigLen, sigOK ? "OK" : "FAIL");
        if (!sigOK) { Serial.println("[PhaseA] Credentials NOT stored (signature failure)"); return; }

        if (!sendSpake2Mastercard(challenge, sizeof(challenge))) {
            Serial.println("[PhaseA] MasterCard SPAKE2+ verify failed");
            return;
        }

        // Decide whether to store provisioning data
        if (!info.hasPub) {
            Serial.println("[PhaseA] Missing endpoint public key; provisioning rejected");
            return;
        }

        if (!ProvisioningPhase::setOwnerProvisioned(info.pubKey65, shouldForce)) {
            Serial.println("[PhaseA] Owner provisioning rejected (already provisioned?)");
            return;
        }
        if (info.certLen > 0) ProvisioningPhase::storeCertChain(info.cert, info.certLen);
        Serial.println("[PhaseA] Provisioning data persisted (CCC mailbox owner slot)");

        if (!sendProvisionResult()) {
            Serial.println("[PhaseA] Warning: failed to notify phone of provisioning result");
        }
        sendOpControlFlowSuccess();

        // Trigger FSM provision success event
        FSMIntegration::NFC::onCredentialsStored();

        // Auto-disable force mode after successful provisioning to prevent automatic re-provisioning
        if (shouldForce) {
            ProvisioningPhase::setForceProvisioningFlag(false);
            ProvisioningPhase::setOneShotForce(false);
            Serial.println("[PhaseA] ✓ Force provisioning auto-disabled after successful provisioning");
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
