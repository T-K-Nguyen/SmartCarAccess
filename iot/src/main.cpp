#include <Arduino.h>
#include "../include/ble.h"
#include "../include/nfc.h"
#include "../include/provisioning.h"

//[NFC] Tag UID: 11:DA:37:06,      [NFC] Tag UID: EE:DE:6E:06
// Expose LED state for BLE module
extern const int LED_PIN = GPIO_NUM_48;
volatile uint8_t g_ledMode = 2;
volatile uint16_t g_blinkIntervalMs = 100;

// BLE callbacks moved to ble.cpp

void TaskLEDControl(void *pvParameters) {
  pinMode(LED_PIN, OUTPUT); // Initialize LED pin
  bool ledState = false;
  for(;;) {
    uint8_t mode = g_ledMode;
    uint16_t interval = g_blinkIntervalMs;

    switch (mode) {
      case 0: // OFF
        digitalWrite(LED_PIN, LOW);
        vTaskDelay(pdMS_TO_TICKS(100));
        break;
      case 1: // ON
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(100));
        break;
      case 2: // BLINK
      default:
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        vTaskDelay(pdMS_TO_TICKS(interval));
        break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(3000);
  // Wait briefly for USB-CDC host to open the port so early logs are visible
  #ifdef ARDUINO_USB_CDC_ON_BOOT
  {
    unsigned long t0 = millis();
    // Give a few extra seconds to reconnect the monitor after resets
    while (!Serial && (millis() - t0) < 6000) {
      delay(10);
    }
  }
  #endif
  delay(200);
  Serial.println("Booting...");

  // Start LED task
  xTaskCreate(TaskLEDControl, "LED Control", 4096, NULL, 2, NULL);
  // Start NFC (I2C PN532) and polling task first so it's ready for provisioning
  NFCMod::begin();
  NFCMod::startTask();

  // Start provisioning (ECC keypair init)
  Provisioning::begin();

  // Give NFC a brief moment to become ready before provisioning
  {
    const uint32_t waitStart = millis();
    while (!NFCMod::isReady() && (millis() - waitStart) < 2000) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  if (!Provisioning::isProvisioned()) {
    Serial.println("[Main] ECU not provisioned; running NFC provisioning");
    Provisioning::runNfcProvisioning();
  }
  Provisioning::printInfo();

  // Start BLE after provisioning setup
  BLEMod::begin();
}

void loop() {
  // Nothing here; work is done in tasks and BLE stack
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 2000) {
    Serial.println("[HB] alive");
    lastBeat = millis();
  }
  // Keep loop light; modules handle their own logs/tasks
  vTaskDelay(pdMS_TO_TICKS(100));
}