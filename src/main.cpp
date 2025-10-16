#include <Arduino.h>
#include "ble.h"
#include "nfc.h"

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
  // Wait briefly for USB-CDC host to open the port so early logs are visible
  #ifdef ARDUINO_USB_CDC_ON_BOOT
  {
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < 3000) {
      delay(10);
    }
  }
  #endif
  delay(200);
  Serial.println("Booting...");

  // Start LED task
  xTaskCreate(TaskLEDControl, "LED Control", 4096, NULL, 2, NULL);

  // Start BLE
  BLEMod::begin();

  // Start NFC (I2C PN532) and polling task
  NFCMod::begin();
  NFCMod::startTask();
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