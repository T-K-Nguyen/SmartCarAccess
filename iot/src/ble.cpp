#include <Arduino.h>
#include <NimBLEDevice.h>
#include "../include/ble.h"
#include "../include/provisioning.h"

// Shared LED state provided by main.cpp
extern volatile uint8_t g_ledMode;
extern volatile uint16_t g_blinkIntervalMs;
extern const int LED_PIN;

namespace {
  // UUIDs
  const char* kServiceUUID      = "12345678-1234-5678-1234-56789abcdef0";
  const char* kCharModeUUID     = "12345678-1234-5678-1234-56789abcdef1";
  const char* kCharIntervalUUID = "12345678-1234-5678-1234-56789abcdef2";
  // Admin service and characteristics
  const char* kAdminServiceUUID   = "9a9b9c9d-0000-4000-8000-9a9b9c9d0000";
  const char* kAdminModeCharUUID  = "9a9b9c9d-0001-4000-8000-9a9b9c9d0001"; // 0=normal,1=enroll,2=remove
  const char* kAdminCmdCharUUID   = "9a9b9c9d-0002-4000-8000-9a9b9c9d0002"; // write-only small commands
  const char* kAdminInfoCharUUID  = "9a9b9c9d-0003-4000-8000-9a9b9c9d0003"; // read public info
  const char* kDeviceName       = "YoloUNO BLE LED";

  bool s_bleStarted = false;
  BLEMod::AdminMode s_adminMode = BLEMod::ADMIN_NORMAL;
  NimBLECharacteristic* g_cAdminMode = nullptr;
  NimBLECharacteristic* g_cAdminInfo = nullptr;

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

  // Admin mode characteristic: read/write 1 byte
  class AdminModeCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      std::string v = c->getValue();
      if (v.empty()) return;
      uint8_t m = static_cast<uint8_t>(v[0]);
      if (m > 2) m = 0;
      s_adminMode = static_cast<BLEMod::AdminMode>(m);
      Serial.printf("[BLE-Admin] Mode set to %u\n", (unsigned)m);
      if (g_cAdminMode) g_cAdminMode->setValue(&m, 1);
    }
  };

  // Admin command characteristic: write small commands
  // Commands:
  // 0x01: clear keys (ec_priv + cert)
  // 0x02: clear all (keys+cert+tags)
  // 0x10: list tags (response via AdminInfo)
  // 0x11: add last seen tag (uses NFCMod::getLastTag)
  // 0x12: remove last seen tag
  class AdminCmdCallbacks : public NimBLECharacteristicCallbacks {
    NimBLECharacteristic* _info;
   public:
    explicit AdminCmdCallbacks(NimBLECharacteristic* info): _info(info) {}
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      std::string v = c->getValue();
      if (v.empty()) return;
      uint8_t cmd = static_cast<uint8_t>(v[0]);
      switch (cmd) {
        case 0x01:
          Provisioning::clearKeys();
          _info->setValue("CLEARED_KEYS");
          break;
        case 0x02:
          Provisioning::clearAll();
          _info->setValue("CLEARED_ALL");
          break;
        case 0x10: {
          // List tags into a compact text
          // We'll call listAuthorizedTags() via serial and also provide a brief count
          _info->setValue("LISTED_TAGS");
          Provisioning::listAuthorizedTags();
          break; }
        default:
          _info->setValue("UNKNOWN_CMD");
          break;
      }
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

    // Admin service
    NimBLEService* pAdmin = pServer->createService(kAdminServiceUUID);
    NimBLECharacteristic* cAdminMode = pAdmin->createCharacteristic(
      kAdminModeCharUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    static AdminModeCallbacks adminModeCb;
    cAdminMode->setCallbacks(&adminModeCb);
    uint8_t initAdmin = (uint8_t)s_adminMode;
    cAdminMode->setValue(&initAdmin, 1);
    g_cAdminMode = cAdminMode;

    NimBLECharacteristic* cAdminInfo = pAdmin->createCharacteristic(
      kAdminInfoCharUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    cAdminInfo->setValue("READY");
    g_cAdminInfo = cAdminInfo;

    NimBLECharacteristic* cAdminCmd = pAdmin->createCharacteristic(
      kAdminCmdCharUUID, NIMBLE_PROPERTY::WRITE);
    static AdminCmdCallbacks adminCmdCb(cAdminInfo);
    cAdminCmd->setCallbacks(&adminCmdCb);

    pAdmin->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    pAdvertising->setAdvertisementData(advData);
    NimBLEAdvertisementData scanResp;
    scanResp.setName(kDeviceName);
    pAdvertising->setScanResponseData(scanResp);
  pAdvertising->addServiceUUID(kServiceUUID);
  pAdvertising->addServiceUUID(kAdminServiceUUID);
    pAdvertising->start();

    Serial.printf("[BLE] Advertising started: %s\n", kDeviceName);
    s_bleStarted = true;
  }

  bool isStarted() { return s_bleStarted; }
  const char* deviceName() { return kDeviceName; }

  BLEMod::AdminMode getAdminMode() { return s_adminMode; }
  void setAdminMode(BLEMod::AdminMode mode) {
    s_adminMode = mode;
    uint8_t m = (uint8_t)mode;
    if (g_cAdminMode) g_cAdminMode->setValue(&m, 1);
  }

  void adminNotify(const char* msg) {
    if (g_cAdminInfo && msg) {
      g_cAdminInfo->setValue(msg);
      g_cAdminInfo->notify();
    }
  }
}
