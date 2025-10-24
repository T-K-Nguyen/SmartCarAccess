// BLE entrypoint: initializes NimBLE and registers services.
// Heavy service logic is split into modules under src/ble_*.cpp for readability.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include "ble/ble.h"
#include "ble/ble_admin.h"
#include "ble/ble_auth.h"
#include "ble/ble_echo.h"

namespace {
  // Device name and services
  const char* kDeviceName = "ESP-Smart-Car-ECU";

  bool s_bleStarted = false;
}

namespace BLEMod {
  void begin() {
    NimBLEDevice::init(kDeviceName);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(185);

    // Init RNG for ephemeral keys and nonces
    static mbedtls_entropy_context s_entropy;
    static mbedtls_ctr_drbg_context s_drbg;
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    const char* pers = "ble-auth";
    mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                          (const unsigned char*)pers, strlen(pers));

    NimBLEServer *pServer = NimBLEDevice::createServer();

    // Register services
    BLEAdmin::registerService(pServer);
    BLEAuth::registerService(pServer, &s_drbg);
    BLEEcho::registerService(pServer, &s_drbg);

    // Advertising
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData; advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    pAdvertising->setAdvertisementData(advData);
    NimBLEAdvertisementData scanResp; scanResp.setName(kDeviceName); pAdvertising->setScanResponseData(scanResp);
    pAdvertising->addServiceUUID("d0d0d0d0-0000-4000-8000-d0d0d0d00000"); // Auth/Echo service UUID
    pAdvertising->addServiceUUID("9a9b9c9d-0000-4000-8000-9a9b9c9d0000"); // Admin service UUID
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
