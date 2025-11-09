#include <Arduino.h>
#include <PN532_HSU.h>
#include <PN532.h>
#include <esp_system.h> // for esp_random()
#include <esp_log.h>
#include "provisioning_phase.h"

PN532_HSU pn532hsu(Serial2);
PN532 nfc(pn532hsu);

// Provisioning / HCE application identifier (AID) shared with phone HCE service
uint8_t aid[] = {0xF0, 0x01, 0x02, 0x03, 0x04, 0x05}; // length = 6

// Polling helpers to make card detection more robust and less noisy
static bool waitForCard(uint32_t timeoutMs = 15000, uint8_t stableReads = 2,
                        uint16_t pollIntervalMs = 60) {
  const uint32_t start = millis();
  uint8_t stable = 0;
  uint32_t lastLog = 0;

  // Single header line, then throttle logs
  Serial.println("Waiting for card (adaptive poll)...");

  while (true) {
    bool present = nfc.inListPassiveTarget();

    if (present) {
      if (++stable >= stableReads) {
        Serial.println("Card detected (stable)");
        return true;
      }
    } else {
      stable = 0; // lost signal, reset stability counter
    }

    const uint32_t now = millis();
    if (now - lastLog >= 2000) { // every 2 seconds
      Serial.print('.');
      lastLog = now;
    }

    if (timeoutMs && (now - start >= timeoutMs)) {
      Serial.println("\nTimeout waiting for card");
      return false;
    }
    delay(pollIntervalMs);
  }
}

static bool waitForRemoval(uint32_t timeoutMs = 8000, uint8_t stableAbsentReads = 3,
                           uint16_t pollIntervalMs = 80) {
  const uint32_t start = millis();
  uint8_t stableAbsent = 0;

  while (true) {
    bool present = nfc.inListPassiveTarget();
    if (!present) {
      if (++stableAbsent >= stableAbsentReads) {
        Serial.println("Card removed (stable)\n");
        return true;
      }
    } else {
      stableAbsent = 0; // still present
    }

    if (timeoutMs && (millis() - start >= timeoutMs)) {
      Serial.println("Removal wait timeout");
      return false;
    }
    delay(pollIntervalMs);
  }
}

static bool apduExchangeWithRetry(const uint8_t *apdu, uint8_t apduLen,
                                  uint8_t *response, uint8_t *respLen,
                                  uint8_t attempts = 3,
                                  uint16_t backoffMs = 60) {
  for (uint8_t i = 1; i <= attempts; ++i) {
    uint8_t len = *respLen; // in case driver changes it on failure
    bool ok = nfc.inDataExchange((uint8_t *)apdu, apduLen, response, &len);
    if (ok) {
      *respLen = len;
      return true;
    }
    if (i < attempts) {
      delay(backoffMs);
    }
  }
  return false;
}

// Try to put PN532 into a mode where it can poll for ISO14443 targets
static bool ensureSAMConfig(uint8_t attempts = 5, uint16_t backoffMs = 150) {
  for (uint8_t i = 1; i <= attempts; ++i) {
    if (nfc.SAMConfig()) {
      return true;
    }
    delay(backoffMs);
  }
  return false;
}

static bool gSamConfigured = false;

// Set to true to always run provisioning GET_CHALLENGE after a successful SELECT (for testing)
static bool kForceProvisioning = false; // default false: keep first successful provisioning

// Simple hex printer for outgoing APDUs (separate from PN532 helper which expects buffer layout)
static void printHex(const uint8_t* d, size_t n) {
  for (size_t i=0;i<n;i++) {
    Serial.printf("%02X", d[i]);
    if (i+1<n) Serial.print(' ');
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial2.begin(115200, SERIAL_8N1, 44, 43);
  delay(100);

  Serial.println("\n=== PN532 HCE READER (SAMConfig retry) ===");

  nfc.begin();
  ProvisioningPhase::begin();

  // Reduce noisy error prints from Preferences when probing or clearing non-existent keys
  esp_log_level_set("Preferences", ESP_LOG_NONE);

  uint32_t version = nfc.getFirmwareVersion();
  if (!version) {
    Serial.println("PN532 NOT FOUND!");
    while (1) delay(1000);
  }

  Serial.print("PN532 Found - Firmware: ");
  Serial.print((version >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((version >> 8) & 0xFF, DEC);

  // Configure SAM (required on many modules to enable ISO14443 polling)
  Serial.print("Configuring SAM...");
  gSamConfigured = ensureSAMConfig(5, 200);
  if (gSamConfigured) {
    Serial.println(" OK");
  } else {
    Serial.println(" FAILED (will still attempt to poll)");
    Serial.println("Hint: Ensure PN532 is in HSU mode (DIP OFF/OFF), RX/TX crossed, and 5V/3V3 per module spec.");
  }

  Serial.println("Ready! Hold Android phone (screen ON)...\n");
  Serial.printf("[PhaseA] Provisioned=%s  ForceProvisioning=%s\n",
                ProvisioningPhase::isProvisioned()?"YES":"NO",
                kForceProvisioning?"YES":"NO");
  Serial.println("[PhaseA] Serial commands: 'p'=print stored, 'f'=toggle force provisioning, 'r'=reset provisioned only, 'C'=clear ALL, 'v'=validate cert vs pub");

  // Removed boot provisioning window; provisioning now happens directly after SELECT + GET_CHALLENGE.
}

void loop() {
  // Handle simple serial admin commands without blocking NFC polling
  while (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'p') {
      String kid; bool haveId = ProvisioningPhase::getKeyId(kid);
      uint8_t pub[65]; size_t pubLen = ProvisioningPhase::getPhonePubRaw(pub, sizeof(pub));
      size_t certLen = ProvisioningPhase::getCertChain(nullptr, 0);
      Serial.println("[Admin] Stored provisioning state:");
      Serial.printf("  keyId: %s\n", haveId? kid.c_str():"<none>");
      Serial.printf("  pubKey: %s (len=%u)\n", (pubLen==65 && pub[0]==0x04)?"present":"<none>", (unsigned)pubLen);
      Serial.printf("  certLen: %u\n", (unsigned)certLen);
    } else if (cmd == 'f') {
      kForceProvisioning = !kForceProvisioning;
      Serial.printf("[Admin] Force provisioning toggled: %s\n", kForceProvisioning?"ON":"OFF");
    } else if (cmd == 'r') {
      ProvisioningPhase::clearProvisionedOnly();
      Serial.println("[Admin] Cleared phone provisioning data (kept device keypair)");
    } else if (cmd == 'C') {
      ProvisioningPhase::clearAll();
      Serial.println("[Admin] CLEARED ALL (including device keypair)");
    } else if (cmd == 'v') {
      bool ok = ProvisioningPhase::validateStoredCertMatchesStoredPub();
      Serial.printf("[Admin] Cert vs pub match: %s\n", ok?"YES":"NO/UNAVAILABLE");
    }
  }
  // Wait up to 15s for a card with 2 consecutive confirmations
  if (!waitForCard(15000, 2, 60)) {
    // If we timed out and SAM wasn't confirmed, try to (re)configure once more
    if (!gSamConfigured) {
      Serial.println("Re-attempting SAMConfig after timeout...");
      gSamConfigured = ensureSAMConfig(3, 200);
      if (gSamConfigured) Serial.println("SAM configured successfully.");
    }
    // No card detected in this window; loop and keep waiting (throttled logs inside)
    return;
  }

  // Card is present - build SELECT AID APDU dynamically for our 6-byte AID
  uint8_t selectApdu[16];
  uint8_t sIdx = 0;
  selectApdu[sIdx++] = 0x00; // CLA
  selectApdu[sIdx++] = 0xA4; // INS (SELECT)
  selectApdu[sIdx++] = 0x04; // P1 (select by name)
  selectApdu[sIdx++] = 0x00; // P2
  selectApdu[sIdx++] = sizeof(aid); // Lc = AID length
  for (size_t i = 0; i < sizeof(aid); ++i) selectApdu[sIdx++] = aid[i];
  selectApdu[sIdx++] = 0x00; // Le (expect response, Android HCE friendly)
  uint8_t response[32];
  uint8_t len = sizeof(response);

  if (apduExchangeWithRetry(selectApdu, sIdx, response, &len, 3, 80)) {
    Serial.print("HCE SELECT OK (len=");
    Serial.print(len);
    Serial.println(") raw:");
    nfc.PrintHexChar(response, len);

    uint8_t selectSw1 = (len >= 2) ? response[len - 2] : 0x00;
    uint8_t selectSw2 = (len >= 1) ? response[len - 1] : 0x00;
    int selectPayloadLen = (len >= 2) ? (len - 2) : 0;
    uint8_t selectPayload[32];
    if (selectPayloadLen > 0 && selectPayloadLen <= (int)sizeof(selectPayload)) {
      memcpy(selectPayload, response, selectPayloadLen);
    }

    if (len >= 2) {
      Serial.printf("SELECT SW1SW2=%02X%02X payloadLen=%d\n", selectSw1, selectSw2, selectPayloadLen);
    }

    // Phase A split flow: 1) Base GET_CHALLENGE (Lc=0) returns identifiers 2) Signature GET_CHALLENGE (Lc=24)
    bool alreadyProv = ProvisioningPhase::isProvisioned();
    if ((kForceProvisioning || !alreadyProv) && selectSw1 == 0x90 && selectSw2 == 0x00) {
      Serial.println("[PhaseA] Step1: Base GET_CHALLENGE (Lc=0)");
      uint8_t baseApdu[] = {0x00,0xCA,0x00,0x00,0x00,0x00}; // Case 4: Lc=0, Le=0
      uint8_t baseResp[255]; uint8_t baseLen = sizeof(baseResp);
      if (apduExchangeWithRetry(baseApdu, sizeof(baseApdu), baseResp, &baseLen, 5, 150)) {
        if (baseLen >= 2 && baseResp[baseLen-2] == 0x90 && baseResp[baseLen-1] == 0x00) {
          int payloadLen = baseLen - 2; const uint8_t* p = baseResp; int idx=0;
          if (payloadLen < 1 + 65 + 2) {
            Serial.println("[PhaseA] Base payload too short");
          } else {
            uint8_t keyIdLen = p[idx++];
            if (keyIdLen > payloadLen - idx) {
              Serial.println("[PhaseA] keyIdLen OOB base");
            } else {
              String keyIdAscii; for (uint8_t k=0;k<keyIdLen;k++) keyIdAscii += (char)p[idx+k];
              idx += keyIdLen;
              if (idx + 65 + 2 > payloadLen) {
                Serial.println("[PhaseA] Layout error before pubKey base");
              } else {
                const uint8_t* pubKey65 = p + idx; idx += 65;
                uint16_t certLen = (uint16_t)(p[idx] << 8 | p[idx+1]); idx += 2;
                const uint8_t* cert = p + idx;
                if (idx + certLen > payloadLen) {
                  Serial.println("[PhaseA] certLen OOB base");
                } else {
                  Serial.printf("[PhaseA] Base keyId='%s' pubKey[0]=%02X certLen=%u\n", keyIdAscii.c_str(), pubKey65[0], certLen);
                  // Keep data for step2
                  bool havePub = pubKey65[0] == 0x04;
                  // Step2: Signature challenge
                  Serial.println("[PhaseA] Step2: Signature GET_CHALLENGE (Lc=24)");
                  uint8_t vehicleId[8]; uint8_t nonce[16];
                  for (int i=0;i<8;i++) vehicleId[i] = (uint8_t)esp_random();
                  for (int i=0;i<16;i++) nonce[i] = (uint8_t)esp_random();
                  uint8_t challenge[24]; memcpy(challenge, vehicleId, 8); memcpy(challenge+8, nonce, 16);
                  uint8_t sigApdu[5 + sizeof(challenge) + 1]; uint8_t sIdx2=0;
                  sigApdu[sIdx2++] = 0x00; sigApdu[sIdx2++] = 0xCA; sigApdu[sIdx2++] = 0x00; sigApdu[sIdx2++] = 0x00;
                  sigApdu[sIdx2++] = sizeof(challenge); memcpy(sigApdu+sIdx2, challenge, sizeof(challenge)); sIdx2 += sizeof(challenge); sigApdu[sIdx2++] = 0x00; // Le
                  Serial.print("[PhaseA] OUT SIG APDU (len="); Serial.print(sIdx2); Serial.println(")"); printHex(sigApdu, sIdx2);
                  delay(30);
                  uint8_t sigResp[255]; uint8_t sigLenTotal = sizeof(sigResp);
                  if (apduExchangeWithRetry(sigApdu, sIdx2, sigResp, &sigLenTotal, 5, 150)) {
                    if (sigLenTotal >= 2 && sigResp[sigLenTotal-2] == 0x90 && sigResp[sigLenTotal-1] == 0x00) {
                      int sigPayloadLen = sigLenTotal - 2; const uint8_t* sp = sigResp; int sIdx=0;
                      if (sigPayloadLen < 2) {
                        Serial.println("[PhaseA] Signature payload too short");
                      } else {
                        uint16_t sigLen = (uint16_t)(sp[sIdx] << 8 | sp[sIdx+1]); sIdx += 2;
                        const uint8_t* sigDer = sp + sIdx;
                        if (sIdx + sigLen > sigPayloadLen) {
                          Serial.println("[PhaseA] sigLen OOB");
                        } else {
                          Serial.printf("[PhaseA] sigLen=%u\n", sigLen);
                          bool sigOk = false;
                          if (sigLen > 0 && havePub) {
                            sigOk = ProvisioningPhase::verifySignatureP256(pubKey65, challenge, sizeof(challenge), sigDer, sigLen);
                            Serial.printf("[PhaseA] Signature verify %s\n", sigOk?"OK":"FAIL");
                          } else if (sigLen == 0) {
                            Serial.println("[PhaseA] No signature provided (sigLen=0) - allowing fallback provisioning");
                            sigOk = true;
                          }
                          if (sigOk) {
                            if (!alreadyProv || kForceProvisioning) {
                              if (kForceProvisioning) ProvisioningPhase::storeKeyIdAsciiForce(keyIdAscii.c_str());
                              else ProvisioningPhase::storeKeyIdAsciiIfEmpty(keyIdAscii.c_str());
                              if (havePub) ProvisioningPhase::storePhonePubRaw(pubKey65);
                              if (certLen > 0) ProvisioningPhase::storeCertChain(cert, certLen);
                            } else {
                              Serial.println("[PhaseA] Already provisioned; skipping re-store (toggle 'f' to force)");
                            }
                          } else {
                            Serial.println("[PhaseA] Credentials NOT stored (signature failure)");
                          }
                        }
                      }
                    } else {
                      Serial.println("[PhaseA] Signature APDU failed SW");
                    }
                  } else {
                    Serial.println("[PhaseA] Signature GET_CHALLENGE exchange failed");
                  }
                }
              }
            }
          }
        } else {
          Serial.println("[PhaseA] Base GET_CHALLENGE failed SW");
        }
      } else {
        Serial.println("[PhaseA] Base GET_CHALLENGE exchange failed");
      }
    }

  // Read UID with retry (secondary APDU)
    uint8_t getUid[] = {0x00, 0xB0, 0x00, 0x00, 0x04};
    len = sizeof(response);
    bool uidOk = apduExchangeWithRetry(getUid, sizeof(getUid), response, &len, 3, 80);
    if (uidOk && len >= 2) {
      uint8_t sw1 = response[len - 2];
      uint8_t sw2 = response[len - 1];
      int payloadLen = len - 2;
      if (payloadLen > 0 && sw1 == 0x90 && sw2 == 0x00) {
        Serial.print("HCE UID (payloadLen=");
        Serial.print(payloadLen);
        Serial.print("): ");
        for (int i = 0; i < payloadLen; i++) Serial.printf("%02X", response[i]);
        Serial.printf("  SW1SW2=%02X%02X\n", sw1, sw2);
      } else {
        Serial.printf("UID APDU returned no payload or non-success SW1SW2=%02X%02X len=%u\n", sw1, sw2, len);
        // Fallback: treat SELECT payload as persistent ID if non-empty and SELECT was successful
        if (selectPayloadLen > 0 && selectSw1 == 0x90 && selectSw2 == 0x00) {
          Serial.print("Using SELECT payload as ID: ");
          for (int i = 0; i < selectPayloadLen; i++) Serial.printf("%02X", selectPayload[i]);
          Serial.println();
        }
      }
    } else {
      Serial.println("Failed to read UID after retries or response too short");
      // Fallback path
      if (selectPayloadLen > 0 && selectSw1 == 0x90 && selectSw2 == 0x00) {
        Serial.print("Using SELECT payload as ID: ");
        for (int i = 0; i < selectPayloadLen; i++) Serial.printf("%02X", selectPayload[i]);
        Serial.println();
      }
    }
  } else {
    Serial.println("SELECT AID failed after retries");
  }

  // Debounced removal detection
  waitForRemoval(8000, 3, 80);
}