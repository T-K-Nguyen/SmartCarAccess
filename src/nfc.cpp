#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include "nfc.h"

namespace {
  // I2C pins and PN532 control pins (keep defaults from existing code)
  const int I2C_SDA_PIN = 11;
  const int I2C_SCL_PIN = 12;
  const int PN532_IRQ_PIN = 4;
  const int PN532_RST_PIN = 5; // set to -1 if not wired

  Adafruit_PN532 nfc(PN532_IRQ_PIN, PN532_RST_PIN);

  bool s_nfcReady = false;
  bool s_nfcInitAttempted = false;
  bool s_nfcRetried = false;

  void TaskNFC(void *pvParameters) {
    uint8_t uid[7];
    uint8_t uidLength = 0;
    for(;;) {
      if (!s_nfcReady) {
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      bool found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);
      if (found) {
        Serial.print("[NFC] Tag UID: ");
        for (uint8_t i = 0; i < uidLength; i++) {
          if (uid[i] < 0x10) Serial.print("0");
          Serial.print(uid[i], HEX);
          if (i + 1 < uidLength) Serial.print(":");
        }
        Serial.println();
        vTaskDelay(pdMS_TO_TICKS(800));
      } else {
        vTaskDelay(pdMS_TO_TICKS(150));
      }
    }
  }
}

namespace NFCMod {
  void begin() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(400000);
    Serial.println("[I2C] Scanning bus...");
    uint8_t count = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      uint8_t err = Wire.endTransmission();
      if (err == 0) {
        Serial.printf("[I2C] Found device at 0x%02X\n", addr);
        count++;
      }
    }
    if (count == 0) Serial.println("[I2C] No devices found. Check SDA/SCL wiring and power.");

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
}
