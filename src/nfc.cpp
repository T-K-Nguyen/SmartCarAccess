#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include "nfc.h"
#include "provisioning.h"
#include "ble.h"

namespace {
  // I2C pins and PN532 control pins (keep defaults from existing code)
  const int I2C_SDA_PIN = 11;
  const int I2C_SCL_PIN = 12;
  // PN532 I2C address can vary by board; 0x24 (7-bit) is common, some clones appear at 0x48.
  uint8_t s_pn532Addr = 0x24;
  // No IRQ or RST pin used; rely on I2C polling
  const int PN532_IRQ_PIN = -1;
  const int PN532_RST_PIN = -1; // set to -1 if not wired
  Adafruit_PN532 nfc(PN532_IRQ_PIN, PN532_RST_PIN);

  bool s_nfcReady = false;
  bool s_nfcInitAttempted = false;
  bool s_nfcRetried = false;
  uint8_t s_lastUid[7];
  uint8_t s_lastUidLen = 0;
  volatile bool s_uidReady = false;

  // Helpers
  void hwResetPN532() {
    #if defined(PN532_RST_PIN) // compile-time check won't work for const int; rely on value
    #endif
    if (PN532_RST_PIN >= 0) {
      pinMode(PN532_RST_PIN, OUTPUT);
      digitalWrite(PN532_RST_PIN, LOW);
      delay(10);
      digitalWrite(PN532_RST_PIN, HIGH);
      delay(10);
    }
  }

  void i2cBusRelease() {
    // Attempt to free a stuck I2C bus (SDA held low). Use GPIO bit-bang on SCL.
    // End Wire to allow direct control
    Wire.end();
    pinMode(I2C_SCL_PIN, OUTPUT);
    pinMode(I2C_SDA_PIN, INPUT_PULLUP);
    // Clock SCL up to 9 times to release any stuck slave
    for (int i = 0; i < 9; ++i) {
      digitalWrite(I2C_SCL_PIN, HIGH);
      delayMicroseconds(5);
      digitalWrite(I2C_SCL_PIN, LOW);
      delayMicroseconds(5);
    }
    // Generate a STOP
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(5);
    // SDA as output to drive a STOP if needed
    pinMode(I2C_SDA_PIN, OUTPUT);
    digitalWrite(I2C_SDA_PIN, HIGH);
    delayMicroseconds(5);
  }

  bool i2cDevicePresent(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
  }

  void i2cScanOnce() {
    uint8_t count = 0;
    bool saw24 = false, saw48 = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      uint8_t err = Wire.endTransmission();
      if (err == 0) {
        Serial.printf("[I2C] Found device at 0x%02X\n", addr);
        count++;
        if (addr == 0x24) saw24 = true;
        if (addr == 0x48) saw48 = true;
      }
    }
    if (count == 0) {
      Serial.println("[I2C] No devices found. Check SDA/SCL wiring and power.");
    } else {
      // Prefer 0x24; fall back to 0x48 if needed.
      if (saw24) s_pn532Addr = 0x24; else if (saw48) s_pn532Addr = 0x48;
      Serial.printf("[I2C] Using PN532 addr 0x%02X\n", s_pn532Addr);
    }
  }

  bool recoverNFC(unsigned int maxAttempts = 5) {
    Serial.println("[NFC] Attempting recovery...");
    for (unsigned int attempt = 1; attempt <= maxAttempts; ++attempt) {
      // Re-init I2C bus
      i2cBusRelease();
      // If we have a hardware reset line, pulse it
      hwResetPN532();
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setTimeOut(50);
  // Keep at standard mode for stability
  Wire.setClock(100000);
      // Check PN532 presence quickly
      if (!i2cDevicePresent(s_pn532Addr)) {
        Serial.printf("[NFC] PN532 not seen at 0x%02X (attempt %u/%u)\n", s_pn532Addr, attempt, maxAttempts);
        vTaskDelay(pdMS_TO_TICKS(250));
        continue;
      }
      // Try to re-begin and query firmware
      nfc.begin();
      uint32_t versiondata = nfc.getFirmwareVersion();
      if (versiondata) {
        Serial.printf("[NFC] Recovery OK. IC: 0x%02X, Ver: %u.%u\n",
                      (unsigned)((versiondata >> 24) & 0xFF),
                      (unsigned)((versiondata >> 16) & 0xFF),
                      (unsigned)((versiondata >> 8) & 0xFF));
        nfc.SAMConfig();
  // Stay at 100kHz for robustness (avoid 400kHz on marginal wiring)
        Serial.println("[NFC] Waiting for NFC tag...");
        s_nfcReady = true;
        return true;
      } else {
        Serial.printf("[NFC] Firmware query failed (attempt %u/%u)\n", attempt, maxAttempts);
        vTaskDelay(pdMS_TO_TICKS(300));
      }
    }
    s_nfcReady = false;
    return false;
  }

  void TaskNFC(void *pvParameters) {
    uint8_t uid[7];
    uint8_t uidLength = 0;
    uint32_t lastGood = millis();
    int failCount = 0;
    // De-duplicate repeated logs for the same tag
    static uint8_t lastPrintedUid[7] = {0};
    static uint8_t lastPrintedLen = 0;
    static uint32_t lastPrintMs = 0;
    const uint32_t printCooldownMs = 1500; // reprint same UID at most every 1.5s
    for(;;) {
      if (!s_nfcReady) {
        // Retry recovery while not ready
        i2cScanOnce();
        recoverNFC(5);
        vTaskDelay(pdMS_TO_TICKS(1500));
        continue;
      }
      bool found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);
      if (found) {
        bool sameAsLast = (uidLength == lastPrintedLen) && (memcmp(uid, lastPrintedUid, uidLength) == 0);
        bool cooldownElapsed = (millis() - lastPrintMs) >= printCooldownMs;
        if (!sameAsLast || cooldownElapsed) {
          bool hasAuth = Provisioning::hasAuthorizedTag();
          bool authorized = hasAuth ? Provisioning::isTagAuthorized(uid, uidLength) : false;
          Serial.print("[NFC] Tag UID ");
          Serial.print(authorized ? "(AUTHORIZED) ":"(UNAUTHORIZED) ");
          for (uint8_t i = 0; i < uidLength; i++) {
            if (uid[i] < 0x10) Serial.print("0");
            Serial.print(uid[i], HEX);
            if (i + 1 < uidLength) Serial.print(":");
          }
          Serial.println();
          memcpy(lastPrintedUid, uid, uidLength);
          lastPrintedLen = uidLength;
          lastPrintMs = millis();
        }
        // Admin mode actions: enroll/remove on next seen tag
        BLEMod::AdminMode mode = BLEMod::getAdminMode();
        if (mode == BLEMod::ADMIN_ENROLL) {
          if (Provisioning::addAuthorizedTag(uid, uidLength)) {
            Serial.print("[NFC] Enrolled tag: ");
            for (uint8_t i = 0; i < uidLength; ++i) { if (uid[i] < 0x10) Serial.print("0"); Serial.print(uid[i], HEX); if (i+1<uidLength) Serial.print(":"); }
            Serial.println();
            BLEMod::adminNotify("ENROLL_OK");
          } else {
            BLEMod::adminNotify("ENROLL_FAIL");
          }
          BLEMod::setAdminMode(BLEMod::ADMIN_NORMAL);
        } else if (mode == BLEMod::ADMIN_REMOVE) {
          if (Provisioning::removeAuthorizedTag(uid, uidLength)) {
            Serial.print("[NFC] Removed tag: ");
            for (uint8_t i = 0; i < uidLength; ++i) { if (uid[i] < 0x10) Serial.print("0"); Serial.print(uid[i], HEX); if (i+1<uidLength) Serial.print(":"); }
            Serial.println();
            BLEMod::adminNotify("REMOVE_OK");
          } else {
            BLEMod::adminNotify("REMOVE_FAIL");
          }
          BLEMod::setAdminMode(BLEMod::ADMIN_NORMAL);
        }
        // Cache UID for provisioning or app logic
        if (uidLength <= sizeof(s_lastUid)) {
          memcpy(s_lastUid, uid, uidLength);
          s_lastUidLen = uidLength;
          s_uidReady = true;
        }
        lastGood = millis();
        failCount = 0;
        vTaskDelay(pdMS_TO_TICKS(800));
      } else {
        failCount++;
        // If we've been failing for a while, probe and attempt recovery
        if ((millis() - lastGood > 1000) || failCount > 8) {
          uint32_t v = nfc.getFirmwareVersion();
          if (!v) {
            Serial.println("[NFC] Lost PN532, attempting re-init...");
            s_nfcReady = false;
            // allow retry path above; next loop will call recoverNFC
          } else {
            // Still responsive; just continue
            lastGood = millis();
            failCount = 0;
          }
        }
        vTaskDelay(pdMS_TO_TICKS(150));
      }
    }
  }
}

namespace NFCMod {
  void begin() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(50);
    Wire.setClock(100000); // start stable and keep at 100kHz
    Serial.println("[I2C] Scanning bus...");
    i2cScanOnce();

    nfc.begin();
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (!versiondata) {
      Serial.println("[NFC] Failed to find PN532! Check wiring and I2C mode.");
      s_nfcReady = false;
    } else {
      Serial.printf("[NFC] PN532 found. IC: 0x%02X, Ver: %u.%u\n",
                    (unsigned)((versiondata >> 24) & 0xFF),
                    (unsigned)((versiondata >> 16) & 0xFF),
                    (unsigned)((versiondata >> 8) & 0xFF));
      nfc.SAMConfig();
      Serial.println("[NFC] Waiting for NFC tag...");
      s_nfcReady = true;
    }
    s_nfcInitAttempted = true;
  }

  void startTask() {
    xTaskCreate(TaskNFC, "NFC Poll", 4096, NULL, 2, NULL);
  }

  bool isReady() { return s_nfcReady; }

  bool getLastTag(uint8_t* uid, uint8_t* uidLen) {
    if (!uid || !uidLen) return false;
    if (!s_uidReady) return false;
    uint8_t n = s_lastUidLen;
    if (n > 7) n = 7;
    memcpy(uid, s_lastUid, n);
    *uidLen = n;
    s_uidReady = false; // consume once
    return true;
  }
}
