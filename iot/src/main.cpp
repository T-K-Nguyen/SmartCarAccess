// Main with FSM integration for smart car access control
#include <Arduino.h>
#include "nfc_session.h"
#include "ccc_mailbox.h"
#include "ble/ble.h"
#include "fsm/fsm.h"
#include "fsm/fsm_integration.h"
#include "test/test_fsm.h"
#include "uwb/uci_session_manager.h"
#include "uwb/uci_uart_link.h"
#include "uwb/uci_host_bridge.h"
#include "uwb/uci_door_unlock.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifndef UCI_UART_RX_PIN
#define UCI_UART_RX_PIN 17
#endif

#ifndef UCI_UART_TX_PIN
#define UCI_UART_TX_PIN 18
#endif

#ifndef UCI_UART_BAUD
#define UCI_UART_BAUD 115200
#endif

namespace {
TaskHandle_t g_fsmTaskHandle = nullptr;
TaskHandle_t g_nfcTaskHandle = nullptr;
TaskHandle_t g_uwbTaskHandle = nullptr;

UwbUci::UciUartLink g_uciLink;
UwbUci::UciSessionManager* g_uciManager = nullptr;

bool parseMacShort(const String& in, uint16_t* outMac) {
  if (outMac == nullptr || in.length() != 5 || in.charAt(2) != ':') {
    return false;
  }

  auto hexNibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };

  const int h0 = hexNibble(in.charAt(0));
  const int h1 = hexNibble(in.charAt(1));
  const int h2 = hexNibble(in.charAt(3));
  const int h3 = hexNibble(in.charAt(4));
  if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
    return false;
  }

  const uint8_t msb = static_cast<uint8_t>((h0 << 4) | h1);
  const uint8_t lsb = static_cast<uint8_t>((h2 << 4) | h3);
  *outMac = static_cast<uint16_t>((static_cast<uint16_t>(lsb) << 8) | msb);
  return true;
}

void runUciDemo(const String& destMacText) {
  if (!g_uciManager) {
    Serial.println("[UCI] manager not initialized");
    return;
  }

  uint16_t destMac = 0;
  if (!parseMacShort(destMacText, &destMac)) {
    Serial.println("[UCI] Invalid MAC format. Expected XX:XX");
    return;
  }

  UwbUci::UciRunConfig cfg;
  cfg.controlee = false;
  cfg.localMac = 0x0000;
  cfg.destMac = destMac;

  Serial.printf("[UCI] Running sequence with dest_mac=%s (uci=0x%04X)\n", destMacText.c_str(), cfg.destMac);
  const bool ok = g_uciManager->runOnce(cfg);
  Serial.printf("[UCI] Sequence result: %s\n", ok ? "SUCCESS" : "FAIL");
}

void handleConsole() {
  static String line;

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      line.trim();
      if (line.length() > 0) {
        if (line.startsWith("uci_run")) {
          int sep = line.indexOf(' ');
          if (sep < 0) {
            Serial.println("[UCI] Usage: uci_run XX:XX");
          } else {
            String mac = line.substring(sep + 1);
            mac.trim();
            runUciDemo(mac);
          }
        } else if (line == "uci_help") {
          Serial.println("[UCI] Commands:");
          Serial.println("  uci_run XX:XX  -> run full UCI sequence and wait one ranging notification");
        }
      }
      line = "";
    } else {
      line += c;
      if (line.length() > 128) {
        line = "";
      }
    }
  }
}

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

void uwbTask(void* parameter) {
  (void)parameter;
  for (;;) {
    if (g_uciManager) {
      g_uciManager->poll();
    }
    UwbUciHost::tick();
    UwbDoorUnlock::tick();
    vTaskDelay(pdMS_TO_TICKS(5));
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

  // Dedicated UART1 on ESP32-S3 for UCI traffic to nRF52840 + DW3000.
  g_uciLink.begin(Serial1, UCI_UART_RX_PIN, UCI_UART_TX_PIN, UCI_UART_BAUD);
  static UwbUci::UciSessionManager uciManager(g_uciLink);
  g_uciManager = &uciManager;
  UwbUciHost::init(g_uciManager);
  Serial.printf("[UCI] UART1 ready RX=%d TX=%d baud=%lu\n", UCI_UART_RX_PIN, UCI_UART_TX_PIN, static_cast<unsigned long>(UCI_UART_BAUD));
  Serial.println("[UCI] Type 'uci_help' in serial monitor for commands.");

  // Initialize door unlock module with hysteresis logic
  UwbDoorUnlock::begin();
  Serial.println("[System] Door unlock module initialized");

  // Run FSM and NFC in dedicated tasks so BLE callbacks are not delayed by NFC work.
  xTaskCreatePinnedToCore(fsmTask, "FSMTask", 4096, nullptr, 6, &g_fsmTaskHandle, 1);
  xTaskCreatePinnedToCore(nfcTask, "NFCTask", 6144, nullptr, 4, &g_nfcTaskHandle, 1);
  xTaskCreatePinnedToCore(uwbTask, "UWBTask", 6144, nullptr, 5, &g_uwbTaskHandle, 1);
  
  Serial.println("\n[System] Ready. FSM active, waiting for events...\n");
}

void loop() {
  // BLE advertising profile demotion (fast -> slow) is polled from here.
  BLEMod::tick();

  handleConsole();

  // Main loop remains mostly idle; FSM/NFC run in dedicated tasks.
  vTaskDelay(pdMS_TO_TICKS(50));
}