#include <Arduino.h>
#include <PN532_HSU.h>
#include <PN532.h>
#include <esp_system.h> // for esp_random()
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
static const bool kForceProvisioning = true;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial2.begin(115200, SERIAL_8N1, 44, 43);
  delay(100);

  Serial.println("\n=== PN532 HCE READER (SAMConfig retry) ===");

  nfc.begin();
  ProvisioningPhase::begin();

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

  // Removed boot provisioning window; provisioning now happens directly after SELECT + GET_CHALLENGE.
}

void loop() {
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

    // Phase A: GET_CHALLENGE (only if not yet provisioned and SELECT succeeded with 9000)
    if ((kForceProvisioning || !ProvisioningPhase::isProvisioned()) && selectSw1 == 0x90 && selectSw2 == 0x00) {
      Serial.println("[PhaseA] Initiating GET_CHALLENGE APDU...");
      // Build challenge = vehicleId(8 random bytes) || nonce(16 random bytes)
      uint8_t vehicleId[8]; uint8_t nonce[16];
      for (int i=0;i<8;i++) vehicleId[i] = (uint8_t)esp_random();
      for (int i=0;i<16;i++) nonce[i] = (uint8_t)esp_random();
      uint8_t challenge[24]; memcpy(challenge, vehicleId, 8); memcpy(challenge+8, nonce, 16);

      uint8_t getChal[5 + sizeof(challenge)];
      uint8_t cIdx = 0;
      getChal[cIdx++] = 0x00; // CLA
      getChal[cIdx++] = 0xCA; // INS (custom GET_CHALLENGE)
      getChal[cIdx++] = 0x00; // P1
      getChal[cIdx++] = 0x00; // P2
      getChal[cIdx++] = sizeof(challenge); // Lc
      memcpy(getChal + cIdx, challenge, sizeof(challenge)); cIdx += sizeof(challenge);

      uint8_t chalResp[200]; uint8_t chalLen = sizeof(chalResp);
      if (apduExchangeWithRetry(getChal, cIdx, chalResp, &chalLen, 3, 120)) {
        if (chalLen >= 2 && chalResp[chalLen-2] == 0x90 && chalResp[chalLen-1] == 0x00) {
          int payloadLen = chalLen - 2; const uint8_t* p = chalResp; int idx = 0;
          if (payloadLen < 1 + 65 + 2) {
            Serial.println("[PhaseA] GET_CHALLENGE payload too short");
          } else {
            uint8_t keyIdLen = p[idx++];
            if (keyIdLen > payloadLen - idx) {
              Serial.println("[PhaseA] keyId length OOB");
            } else {
              String keyIdAscii;
              for (uint8_t k=0; k<keyIdLen; ++k) keyIdAscii += (char)p[idx+k];
              idx += keyIdLen;
              if (idx + 65 + 2 > payloadLen) {
                Serial.println("[PhaseA] Layout error after keyId");
              } else {
                const uint8_t* pubKey65 = p + idx; idx += 65;
                uint16_t certLen = (uint16_t)(p[idx] << 8 | p[idx+1]); idx += 2;
                const uint8_t* cert = p + idx;
                if (idx + certLen > payloadLen) {
                  Serial.println("[PhaseA] cert length OOB");
                } else {
                  Serial.printf("[PhaseA] keyId='%s' pubKey[0]=%02X certLen=%u\n", keyIdAscii.c_str(), pubKey65[0], certLen);
                  if (pubKey65[0] != 0x04) {
                    Serial.println("[PhaseA] WARNING: pubKey is not uncompressed (expected 0x04 prefix)");
                  }
                  // Store keyId (force overwrite if testing)
                  if (kForceProvisioning) {
                    ProvisioningPhase::storeKeyIdAsciiForce(keyIdAscii.c_str());
                  } else {
                    ProvisioningPhase::storeKeyIdAsciiIfEmpty(keyIdAscii.c_str());
                  }
                }
              }
            }
          }
        } else {
          Serial.println("[PhaseA] GET_CHALLENGE failed SW");
        }
      } else {
        Serial.println("[PhaseA] GET_CHALLENGE exchange failed");
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