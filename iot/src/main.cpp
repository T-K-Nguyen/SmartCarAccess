// Main with FSM integration for smart car access control
#include <Arduino.h>
#include "nfc_session.h"
#include "ccc_mailbox.h"
#include "ble/ble.h"
#include "fsm/fsm.h"
#include "fsm/fsm_integration.h"
#include "test/test_fsm.h"
#include "wifi_manager.h"
#include "firebase_logger.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ===== CONFIGURATION =====
// WiFi Settings (Change these to your 4G hotspot credentials)
const char* WIFI_SSID = "Access_point";
const char* WIFI_PASSWORD = "14022025";

// Firebase Settings (Change these to your Firebase project details)
const char* FIREBASE_DB_URL = "https://smart-car-de21b-default-rtdb.asia-southeast1.firebasedatabase.app/";
const char* FIREBASE_API_KEY = "AIzaSyAswGMO_3aO3o-C5zPBFiqHVTYi5y4pKxc";

namespace {
TaskHandle_t g_fsmTaskHandle = nullptr;
TaskHandle_t g_nfcTaskHandle = nullptr;
TaskHandle_t g_wifiTaskHandle = nullptr;

void fsmTask(void* parameter) {
  (void)parameter;
  for (;;) {
    FSM::tick();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void nfcTask(void* parameter) {
  (void)parameter;
  for (;;) {
    NfcSession::tick();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void wifiTask(void* parameter) {
  (void)parameter;
  for (;;) {
    WiFiManager::tick();
    FirebaseLogger::tick();
    vTaskDelay(pdMS_TO_TICKS(100));  // Check WiFi and Firebase every 100ms
  }
}
}  // Close namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  while (!Serial) {
    delay(10);
  }
  
  Serial.println("╔═══════════════════════════════════════════════════════════╗");
  Serial.println("║        Smart Car Access - FSM + Firebase Logging         ║");
  Serial.println("╚═══════════════════════════════════════════════════════════╝");
  
  // Initialize WiFi with auto-reconnect
  WiFiManager::WiFiConfig wifiConfig = {
    .ssid = WIFI_SSID,
    .password = WIFI_PASSWORD,
    .reconnectInterval = 5000,      // Try reconnect every 5 seconds
    .connectionTimeout = 15000,     // Timeout after 15 seconds
    .maxRetries = 5                 // Max 5 retry attempts before reset
  };
  WiFiManager::begin(wifiConfig);
  Serial.println("[Setup] WiFi manager initialized");
  
  // Initialize Firebase logger
  FirebaseLogger::Config firebaseConfig = {
    .databaseUrl = FIREBASE_DB_URL,
    .apiKey = FIREBASE_API_KEY
  };
  FirebaseLogger::begin(firebaseConfig);
  Serial.println("[Setup] Firebase logger initialized");
  
  // Initialize CCC mailbox before FSM and BLE
  if (!CCCMailbox::begin()) {
    Serial.println("[CCC] Mailbox init failed");
  }

  // Initialize FSM (must be first to set up state machine)
  FSM::begin();
  Serial.println("[FSM] State machine initialized");
  
  // TEMPORARILY DISABLED - BLE causes crash
  // Start BLE (admin/auth/echo services)
  // BLEMod::begin();
  // Serial.println("[BLE] Services started.");
  
  // UART2 on ESP32-S3: RX=44, TX=43 for PN532 (HSU)
  // NfcSession::begin(Serial2, 44, 43, 115200);

  // Run FSM, NFC, and WiFi in dedicated tasks
  xTaskCreatePinnedToCore(fsmTask, "FSMTask", 8192, nullptr, 6, &g_fsmTaskHandle, 1);  // Increased stack to 8KB
  // xTaskCreatePinnedToCore(nfcTask, "NFCTask", 6144, nullptr, 4, &g_nfcTaskHandle, 1);
  xTaskCreatePinnedToCore(wifiTask, "WiFiTask", 4096, nullptr, 3, &g_wifiTaskHandle, 0);  // WiFi on Core 0
  
  Serial.println("\n[System] Ready. FSM + WiFi active (BLE/NFC disabled for debugging)...\n");
}

void loop() {
  // BLE advertising profile demotion (fast -> slow) is polled from here.
  BLEMod::tick();

  // Main loop remains mostly idle; FSM/NFC run in dedicated tasks.
  vTaskDelay(pdMS_TO_TICKS(50));
}