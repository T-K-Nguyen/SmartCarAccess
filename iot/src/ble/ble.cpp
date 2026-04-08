// BLE entrypoint: initializes NimBLE and registers services.
// Heavy service logic is split into modules under src/ble_*.cpp for readability.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include "ble/ble.h"
#include "ble/ble_rollout.h"
#include "ble/pke_telemetry.h"
#include "ble/ble_admin.h"
#include "ble/ble_auth.h"
#include "ble/ble_attestation.h"
#include "ble/ble_echo.h"
#include "ccc_mailbox.h"

namespace {
  // Device name and services
  const char* kDeviceName = "ESP-Smart-Car-ECU";
  constexpr uint8_t kOwnAddrTypeRpaPublicDefault = 0x02;
  constexpr uint8_t kSmIoCapNoIo = 0x03;
  constexpr uint8_t kSmKeyDistEnc = 0x01;
  constexpr uint8_t kSmKeyDistId = 0x02;

  bool s_bleStarted = false;

  void logRolloutFlags() {
    BLERollout::Flags f = BLERollout::flags();
    Serial.printf(
      "[BLE][ROLL] flags background=%u fast_tx=%u bonding_enforce=%u rssi_monitor_only=%u rssi_threshold_dbm=%d\n",
      f.backgroundMode ? 1 : 0,
      f.fastTransaction ? 1 : 0,
      f.bondingEnforce ? 1 : 0,
      f.rssiMonitorOnly ? 1 : 0,
      f.rssiThresholdDbm
    );
  }

  void configureSecurity() {
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(kSmIoCapNoIo);
    NimBLEDevice::setSecurityInitKey(kSmKeyDistEnc | kSmKeyDistId);
    NimBLEDevice::setSecurityRespKey(kSmKeyDistEnc | kSmKeyDistId);

    bool rpaOk = NimBLEDevice::setOwnAddrType(kOwnAddrTypeRpaPublicDefault);
    Serial.printf("[BLE][SEC] own_addr_type=RPA_PUBLIC_DEFAULT set=%s\n", rpaOk ? "OK" : "FAIL");
    Serial.println("[BLE][SEC] pairing config: BOND=1 MITM=0 SC=1 IO=NO_INPUT_OUTPUT KEY_DIST=ENC|ID");
  }

  void logBondTable() {
    int count = NimBLEDevice::getNumBonds();
    Serial.printf("[BLE][SEC] restored_bonds=%d\n", count);
    for (int i = 0; i < count; ++i) {
      NimBLEAddress addr = NimBLEDevice::getBondedAddress(i);
      Serial.printf("[BLE][SEC] restored_bond[%d]=%s\n", i, addr.toString().c_str());
    }
  }
}

namespace BLEMod {
  void begin() {
    NimBLEDevice::init(kDeviceName);
    logRolloutFlags();
    configureSecurity();
    logBondTable();

    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(512);  // Increased from 185 to reduce packet count for certificate chain

    // Init RNG for ephemeral keys and nonces
    static mbedtls_entropy_context s_entropy;
    static mbedtls_ctr_drbg_context s_drbg;
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    const char* pers = "ble-auth";
    mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                          (const unsigned char*)pers, strlen(pers));

    NimBLEServer *pServer = NimBLEDevice::createServer();
    // Keep advertising after connects and restart after disconnects
    class ServerCallbacks : public NimBLEServerCallbacks {
      void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        Serial.println("[BLE] Central connected. Keeping advertising active.");
        PKETelemetry::startAttempt(CCCMailbox::vehicleId());
        PKETelemetry::emit(PKETelemetry::Event::Connect);
        Serial.printf(
          "[BLE][SEC] onConnect peer=%s id=%s bonded=%u enc=%u auth=%u\n",
          connInfo.getAddress().toString().c_str(),
          connInfo.getIdAddress().toString().c_str(),
          connInfo.isBonded() ? 1 : 0,
          connInfo.isEncrypted() ? 1 : 0,
          connInfo.isAuthenticated() ? 1 : 0
        );
        if (!connInfo.isBonded()) {
          Serial.println("[BLE][SEC] POLICY progressive: unbonded peer allowed during rollout");
        }
        bool secStarted = NimBLEDevice::startSecurity(connInfo.getConnHandle());
        Serial.printf("[BLE][SEC] startSecurity requested=%s conn_handle=%u\n",
                      secStarted ? "YES" : "NO",
                      connInfo.getConnHandle());
        
        // Update connection parameters for lower latency
        // Interval: 15ms (12 * 1.25ms), Latency: 0, Timeout: 2000ms (200 * 10ms)
        pServer->updateConnParams(connInfo.getConnHandle(), 12, 12, 0, 200);
        Serial.println("[BLE] Requested connection params: interval=15ms, latency=0, timeout=2000ms");
        
        NimBLEDevice::startAdvertising();
      }
      void onDisconnect(NimBLEServer* /*pServer*/, NimBLEConnInfo& /*connInfo*/, int reason) override {
        Serial.printf("[BLE] Central disconnected (reason=%d). Restarting advertising and resetting session.\n", reason);
        BLEAuth::resetSession();
        NimBLEDevice::startAdvertising();
      }
      
      void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
        Serial.printf("[BLE] MTU negotiated: %u bytes (conn_handle=%u)\n", MTU, connInfo.getConnHandle());
      }

      void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        Serial.printf(
          "[BLE][SEC] auth_complete peer=%s bonded=%u enc=%u auth=%u key_size=%u\n",
          connInfo.getAddress().toString().c_str(),
          connInfo.isBonded() ? 1 : 0,
          connInfo.isEncrypted() ? 1 : 0,
          connInfo.isAuthenticated() ? 1 : 0,
          connInfo.getSecKeySize()
        );
      }

      void onIdentity(NimBLEConnInfo& connInfo) override {
        Serial.printf(
          "[BLE][SEC] identity_resolved ota=%s id=%s\n",
          connInfo.getAddress().toString().c_str(),
          connInfo.getIdAddress().toString().c_str()
        );
      }
    };
    static ServerCallbacks s_serverCbs;
    pServer->setCallbacks(&s_serverCbs);

    // Register services (Auth/Echo share the same service UUID)
    BLEAdmin::registerService(pServer);
    BLEEcho::registerService(pServer, &s_drbg);
    BLEAuth::registerService(pServer, &s_drbg);
    BLEAttestation::registerService(pServer);

    if (pServer->getServiceByUUID("9a9b9c9d-0000-4000-8000-9a9b9c9d0000")) Serial.println("[BLE] Admin service registered");
    if (pServer->getServiceByUUID("d0d0d0d0-0000-4000-8000-d0d0d0d00000")) Serial.println("[BLE] Auth/Echo service registered");
    if (pServer->getServiceByUUID("555a0001-00aa-1111-2222-333344445555")) Serial.println("[BLE] Attestation service registered");

    // Start the Auth service once after all characteristics are added
    if (auto pAuth = pServer->getServiceByUUID("d0d0d0d0-0000-4000-8000-d0d0d0d00000")) {
      pAuth->start();
    }

    // Advertising
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData; advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    pAdvertising->setAdvertisementData(advData);
    NimBLEAdvertisementData scanResp; scanResp.setName(kDeviceName); pAdvertising->setScanResponseData(scanResp);
    pAdvertising->addServiceUUID("d0d0d0d0-0000-4000-8000-d0d0d0d00000"); // Auth/Echo service UUID
    pAdvertising->addServiceUUID("9a9b9c9d-0000-4000-8000-9a9b9c9d0000"); // Admin service UUID
    pAdvertising->addServiceUUID("555a0001-00aa-1111-2222-333344445555"); // Attestation service UUID
    pAdvertising->start();

    Serial.printf("[BLE] Advertising started: %s\n", kDeviceName);
    s_bleStarted = true;
  }

  bool isStarted() { return s_bleStarted; }
  const char* deviceName() { return kDeviceName; }
  bool isSessionReady() { return BLEAuth::isSessionReady(); }

  BLEMod::AdminMode getAdminMode() { return BLEAdmin::getAdminMode(); }
  void setAdminMode(BLEMod::AdminMode mode) { BLEAdmin::setAdminMode(mode); }
  void adminNotify(const char* msg) { BLEAdmin::notify(msg); }
}
