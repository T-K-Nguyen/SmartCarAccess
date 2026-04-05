// Main with FSM integration for smart car access control
#include <Arduino.h>
#include "nfc_session.h"
#include "ccc_mailbox.h"
#include "ble/ble.h"
#include "fsm/fsm.h"
#include "fsm/fsm_integration.h"
#include "test/test_fsm.h"

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
  
  Serial.println("\n[System] Ready. FSM active, waiting for events...\n");
}

void loop() {
  // Check for test command
  // if (Serial.available()) {
  //   char cmd = Serial.read();
  //   if (cmd == 't' || cmd == 'T') {
  //     Serial.println("\n[Test] Running FSM unit tests...");
  //     run_fsm_tests();
  //     Serial.println("[Test] Tests completed. Press 't' to run again.\n");
  //   }
  // }
  
  // FSM tick - processes events and manages state transitions
  FSM::tick();
  
  // NFC tick - handles card detection and APDU exchanges
  NfcSession::tick();
  
  // Small yield to prevent watchdog issues
  yield();
}