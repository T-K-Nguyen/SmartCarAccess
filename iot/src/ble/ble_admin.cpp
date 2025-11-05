#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string>

#include "../../include/ble/ble_admin.h"
#include "provisioning.h"

namespace {
  // Admin service (preserved for NFC admin operations)
  const char* kAdminServiceUUID   = "9a9b9c9d-0000-4000-8000-9a9b9c9d0000";
  const char* kAdminModeCharUUID  = "9a9b9c9d-0001-4000-8000-9a9b9c9d0001"; // 0=normal,1=enroll,2=remove
  const char* kAdminCmdCharUUID   = "9a9b9c9d-0002-4000-8000-9a9b9c9d0002"; // write-only small commands
  const char* kAdminInfoCharUUID  = "9a9b9c9d-0003-4000-8000-9a9b9c9d0003"; // read/notify info
  const char* kAdminPhoneKeyUUID  = "9a9b9c9d-0004-4000-8000-9a9b9c9d0004"; // write-only: phone PEM (supports chunking)

  // Admin state
  BLEMod::AdminMode s_adminMode = BLEMod::ADMIN_NORMAL;
  NimBLECharacteristic* g_cAdminMode = nullptr;
  NimBLECharacteristic* g_cAdminInfo = nullptr;

  // Admin mode characteristic: read/write 1 byte
  class AdminModeCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      std::string v = c->getValue(); if (v.empty()) return;
      uint8_t m = static_cast<uint8_t>(v[0]); if (m > 2) m = 0;
      s_adminMode = static_cast<BLEMod::AdminMode>(m);
      Serial.printf("[BLE-Admin] Mode set to %u\n", (unsigned)m);
      if (g_cAdminMode) g_cAdminMode->setValue(&m, 1);
    }
  };

  // Admin command characteristic
  class AdminCmdCallbacks : public NimBLECharacteristicCallbacks {
    NimBLECharacteristic* _info;
   public:
    explicit AdminCmdCallbacks(NimBLECharacteristic* info): _info(info) {}
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      std::string v = c->getValue(); if (v.empty()) return;
      uint8_t cmd = static_cast<uint8_t>(v[0]);
      switch (cmd) {
        case 0x01: Provisioning::clearKeys(); _info->setValue("CLEARED_KEYS"); break;
        case 0x02: Provisioning::clearAll(); _info->setValue("CLEARED_ALL"); break;
        case 0x10: _info->setValue("LISTED_TAGS"); Provisioning::listAuthorizedTags(); break;
        case 0x20: 
          _info->setValue("STARTING_PROVISIONING"); 
          _info->notify();
          Serial.println("[BLE Admin] Provisioning requested via BLE command 0x20");
          Provisioning::runNfcProvisioning(); 
          break;
        default: _info->setValue("UNKNOWN_CMD"); break;
      }
    }
  };

  // Admin phone public key upload (PEM). Supports chunked writes; commit when END line detected.
  class AdminPhoneKeyCallbacks : public NimBLECharacteristicCallbacks {
    NimBLECharacteristic* _info; std::string _buf;
   public:
    explicit AdminPhoneKeyCallbacks(NimBLECharacteristic* info): _info(info) {}
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      std::string v = c->getValue(); if (v.empty()) return;
      // If a full PEM arrives in one write, store directly.
      if (v.find("-----BEGIN PUBLIC KEY-----") != std::string::npos && v.find("-----END PUBLIC KEY-----") != std::string::npos) {
        bool ok = Provisioning::setPhonePublicKey(v.c_str()); _info->setValue(ok ? "PHONEKEY_OK" : "PHONEKEY_FAIL"); if (ok) _info->notify(); _buf.clear(); return;
      }
      // Otherwise, accumulate and commit when END appears.
      if (_buf.empty() && v.find("-----BEGIN PUBLIC KEY-----") == std::string::npos) { _info->setValue("PHONEKEY_WAIT_BEGIN"); return; }
      _buf.append(v);
      if (_buf.size() > 4096) { _buf.clear(); _info->setValue("PHONEKEY_TOO_LARGE"); return; }
      if (_buf.find("-----END PUBLIC KEY-----") != std::string::npos) {
        bool ok = Provisioning::setPhonePublicKey(_buf.c_str()); _info->setValue(ok ? "PHONEKEY_OK" : "PHONEKEY_FAIL"); if (ok) _info->notify(); _buf.clear();
      } else {
        _info->setValue("PHONEKEY_BUFFERING");
      }
    }
  };
}

namespace BLEAdmin {
  void registerService(NimBLEServer* server) {
    NimBLEService* pAdmin = server->createService(kAdminServiceUUID);
    // Mode
    NimBLECharacteristic* cAdminMode = pAdmin->createCharacteristic(kAdminModeCharUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    static AdminModeCallbacks adminModeCb; cAdminMode->setCallbacks(&adminModeCb);
    uint8_t initAdmin = (uint8_t)s_adminMode; cAdminMode->setValue(&initAdmin, 1); g_cAdminMode = cAdminMode;
    // Info
    NimBLECharacteristic* cAdminInfo = pAdmin->createCharacteristic(kAdminInfoCharUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    cAdminInfo->setValue("READY"); g_cAdminInfo = cAdminInfo;
    // Commands
    NimBLECharacteristic* cAdminCmd = pAdmin->createCharacteristic(kAdminCmdCharUUID, NIMBLE_PROPERTY::WRITE);
    static AdminCmdCallbacks adminCmdCb(cAdminInfo); cAdminCmd->setCallbacks(&adminCmdCb);
    // Phone public key upload
    NimBLECharacteristic* cPhoneKey = pAdmin->createCharacteristic(kAdminPhoneKeyUUID, NIMBLE_PROPERTY::WRITE);
    static AdminPhoneKeyCallbacks phoneKeyCb(cAdminInfo); cPhoneKey->setCallbacks(&phoneKeyCb);
    pAdmin->start();
  }

  BLEMod::AdminMode getAdminMode() { return s_adminMode; }
  void setAdminMode(BLEMod::AdminMode mode) { s_adminMode = mode; uint8_t m = (uint8_t)mode; if (g_cAdminMode) g_cAdminMode->setValue(&m, 1); }
  void notify(const char* msg) { if (g_cAdminInfo && msg) { g_cAdminInfo->setValue(msg); g_cAdminInfo->notify(); } }
}
