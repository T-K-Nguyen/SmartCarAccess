#include <Arduino.h>
#include "fsm.h"
#include "fsm_integration.h"
#include "nfc_session.h"
#include "ble/ble.h"

void handleSerialCommands();

void setup() {
  Serial.begin(115200);
  delay(2000);
  while (!Serial) delay(10);
  
  Serial.println("\n========================================");
  Serial.println("   Smart Car Access System v2.0 FSM");
  Serial.println("========================================\n");
  
  Serial.println("[MAIN] Initializing NFC (PN532)...");
  NfcSession::begin(Serial2, 44, 43, 115200);
  
  Serial.println("[MAIN] Initializing FSM...");
  FSM::begin();
  
  Serial.println("[MAIN] System ready! Press 'h' for help\n");
}

void loop() {
  FSM::tick();
  handleSerialCommands();
}

// Handle serial commands to control FSM
void handleSerialCommands() {
  if (!Serial.available()) return;
  
  char cmd = Serial.read();
  
  switch (cmd) {
    case 's':
      FSMIntegration::SerialCmd::printFSMStatus();
      break;
    case 'p':
      FSMIntegration::SerialCmd::startProvisioning();
      break;
    case 'f':
      FSMIntegration::SerialCmd::toggleForceProvision();
      break;
    case 'F':
      FSMIntegration::SerialCmd::armOneShotForce();
      break;
    case 'r':
      FSMIntegration::SerialCmd::clearKeys();
      break;
    case 'C':
      FSMIntegration::SerialCmd::clearAll();
      break;
    case 'h':
      Serial.println("\n=== Commands ===");
      Serial.println("s - FSM status");
      Serial.println("p - Start provisioning");
      Serial.println("f - Toggle force provision");
      Serial.println("F - Arm one-shot force");
      Serial.println("r - Clear keys");
      Serial.println("C - Clear all");
      Serial.println("u - Test unlock");
      Serial.println("h - Help\n");
      break;
    case 'u':
      Serial.println("[MAIN] Simulating unlock...");
      FSM::triggerEvent(FSM::UNLOCK_REQUESTED);
      break;
    default:
      if (cmd >= 32 && cmd <= 126) {
        Serial.printf("[MAIN] Unknown: '%c'. Press 'h' for help.\n", cmd);
      }
      break;
  }
}
