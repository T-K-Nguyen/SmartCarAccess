#include <Arduino.h>
#include <NimBLEDevice.h>
#include "ble.h"

// Shared LED state provided by main.cpp
extern volatile uint8_t g_ledMode;
extern volatile uint16_t g_blinkIntervalMs;
extern const int LED_PIN;

namespace {
  // UUIDs
  const char* kServiceUUID      = "12345678-1234-5678-1234-56789abcdef0";
  const char* kCharModeUUID     = "12345678-1234-5678-1234-56789abcdef1";
  const char* kCharIntervalUUID = "12345678-1234-5678-1234-56789abcdef2";
  const char* kDeviceName       = "YoloUNO BLE LED";

  bool s_bleStarted = false;

  class LEDModeCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& /*connInfo*/) override {
      std::string val = pCharacteristic->getValue();
      if (val.empty()) return;
      uint8_t mode = static_cast<uint8_t>(val[0]);
      if (mode > 2) mode = 2;
      g_ledMode = mode;
      if (mode == 0) {
        digitalWrite(LED_PIN, LOW);
      } else if (mode == 1) {
        digitalWrite(LED_PIN, HIGH);
      }
      Serial.printf("[BLE] LED mode set to %u\n", static_cast<unsigned>(mode));
    }
  };

  class LEDIntervalCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& /*connInfo*/) override {
      std::string val = pCharacteristic->getValue();
      if (val.size() < 2) return;
      uint16_t ms = static_cast<uint8_t>(val[0]) | (static_cast<uint16_t>(static_cast<uint8_t>(val[1])) << 8);
      if (ms < 50) ms = 50;
      if (ms > 10000) ms = 10000;
      g_blinkIntervalMs = ms;
      Serial.printf("[BLE] Blink interval set to %u ms\n", static_cast<unsigned>(ms));
    }
  };
}

namespace BLEMod {
  void begin() {
    NimBLEDevice::init(kDeviceName);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(185);

    NimBLEServer *pServer = NimBLEDevice::createServer();
    NimBLEService *pService = pServer->createService(kServiceUUID);

    NimBLECharacteristic *pCharMode = pService->createCharacteristic(
      kCharModeUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    static LEDModeCallbacks modeCallbacks;
    pCharMode->setCallbacks(&modeCallbacks);
    uint8_t initMode = g_ledMode;
    pCharMode->setValue(&initMode, 1);

    NimBLECharacteristic *pCharInterval = pService->createCharacteristic(
      kCharIntervalUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    static LEDIntervalCallbacks intervalCallbacks;
    pCharInterval->setCallbacks(&intervalCallbacks);
    uint16_t initInterval = g_blinkIntervalMs;
    uint8_t intervalBuf[2] = { (uint8_t)(initInterval & 0xFF), (uint8_t)(initInterval >> 8) };
    pCharInterval->setValue(intervalBuf, sizeof(intervalBuf));

    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    pAdvertising->setAdvertisementData(advData);
    NimBLEAdvertisementData scanResp;
    scanResp.setName(kDeviceName);
    pAdvertising->setScanResponseData(scanResp);
    pAdvertising->addServiceUUID(kServiceUUID);
    pAdvertising->start();

    Serial.printf("[BLE] Advertising started: %s\n", kDeviceName);
    s_bleStarted = true;
  }

  bool isStarted() { return s_bleStarted; }
  const char* deviceName() { return kDeviceName; }
}
