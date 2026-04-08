// Main with FSM integration for smart car access control
#include <Arduino.h>
#include "nfc_session.h"
#include "ccc_mailbox.h"
#include "ble/ble.h"
#include "fsm/fsm.h"
#include "fsm/fsm_integration.h"
#include "test/test_fsm.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
TaskHandle_t g_fsmTaskHandle = nullptr;
TaskHandle_t g_nfcTaskHandle = nullptr;

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
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  while (!Serial) {
    delay(10);
  }
  
  Serial.println("╔═══════════════════════════════════════════════════════════╗");
  Serial.println("║        Smart Car Access - FSM Integrated                 ║");
  Serial.println("╚═══════════════════════════════════════════════════════════╝");
  
  // Initialize CCC mailbox before FSM and BLE
  if (!CCCMailbox::begin()) {
    Serial.println("[CCC] Mailbox init failed");
  }

  // Initialize FSM (must be first to set up state machine)
  FSM::begin();
  Serial.println("[FSM] State machine initialized");
  
  // Start BLE (admin/auth/echo services)
  BLEMod::begin();
  Serial.println("[BLE] Services started.");
  
  // UART2 on ESP32-S3: RX=44, TX=43 for PN532 (HSU)
  NfcSession::begin(Serial2, 44, 43, 115200);

  // Run FSM and NFC in dedicated tasks so BLE callbacks are not delayed by NFC work.
  xTaskCreatePinnedToCore(fsmTask, "FSMTask", 4096, nullptr, 6, &g_fsmTaskHandle, 1);
  xTaskCreatePinnedToCore(nfcTask, "NFCTask", 6144, nullptr, 4, &g_nfcTaskHandle, 1);
  
  Serial.println("\n[System] Ready. FSM active, waiting for events...\n");
}

void loop() {
  // BLE advertising profile demotion (fast -> slow) is polled from here.
  BLEMod::tick();

  // Main loop remains mostly idle; FSM/NFC run in dedicated tasks.
  vTaskDelay(pdMS_TO_TICKS(50));
}