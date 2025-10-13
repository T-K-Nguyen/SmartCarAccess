#include <Arduino.h>
#include <NimBLEDevice.h>

// LED control shared state
static const int LED_PIN = GPIO_NUM_48; // Board LED pin
static volatile uint8_t g_ledMode = 2;  // 0=OFF, 1=ON, 2=BLINK
static volatile uint16_t g_blinkIntervalMs = 100; // Blink interval in ms (default fast blink)

// UUIDs (random custom UUIDs)
static const char* kServiceUUID      = "12345678-1234-5678-1234-56789abcdef0";
static const char* kCharModeUUID     = "12345678-1234-5678-1234-56789abcdef1";
static const char* kCharIntervalUUID = "12345678-1234-5678-1234-56789abcdef2";

// Forward declarations
void startBLE();

class LEDModeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& /*connInfo*/) override {
    std::string val = pCharacteristic->getValue();
    if (val.empty()) return;
    uint8_t mode = static_cast<uint8_t>(val[0]);
    if (mode > 2) mode = 2; // clamp
    g_ledMode = mode;
    // Apply immediate effect for ON/OFF
    if (mode == 0) {
      digitalWrite(LED_PIN, LOW);
    } else if (mode == 1) {
      digitalWrite(LED_PIN, HIGH);
    }
    Serial.printf("[BLE] LED mode set to %u\n", static_cast<unsigned>(mode));
  }
};

class LEDIntervalCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& /*connInfo*/) override {
    std::string val = pCharacteristic->getValue();
    if (val.size() < 2) return;
    // Expect little-endian uint16 in milliseconds
    uint16_t ms = static_cast<uint8_t>(val[0]) | (static_cast<uint16_t>(static_cast<uint8_t>(val[1])) << 8);
    // Constrain to sane bounds
    if (ms < 50) ms = 50;          // at least 50ms
    if (ms > 10000) ms = 10000;    // at most 10s
    g_blinkIntervalMs = ms;
    Serial.printf("[BLE] Blink interval set to %u ms\n", static_cast<unsigned>(ms));
  }
};

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

void startBLE() {
  NimBLEDevice::init("YoloUNO LED");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // max power; adjust if needed
  NimBLEDevice::setMTU(185);              // reasonable MTU

  NimBLEServer *pServer = NimBLEDevice::createServer();
  NimBLEService *pService = pServer->createService(kServiceUUID);

  // LED Mode characteristic (uint8) R/W
  NimBLECharacteristic *pCharMode = pService->createCharacteristic(
    kCharModeUUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
  );
  static LEDModeCallbacks modeCallbacks;
  pCharMode->setCallbacks(&modeCallbacks);
  uint8_t initMode = g_ledMode;
  pCharMode->setValue(&initMode, 1);

  // Blink interval characteristic (uint16, ms) R/W
  NimBLECharacteristic *pCharInterval = pService->createCharacteristic(
    kCharIntervalUUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
  );
  static LEDIntervalCallbacks intervalCallbacks;
  pCharInterval->setCallbacks(&intervalCallbacks);
  uint16_t initInterval = g_blinkIntervalMs;
  // NimBLE expects byte buffer; send little-endian
  uint8_t intervalBuf[2] = { (uint8_t)(initInterval & 0xFF), (uint8_t)(initInterval >> 8) };
  pCharInterval->setValue(intervalBuf, sizeof(intervalBuf));

  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(kServiceUUID);
  // Some versions of NimBLE-Arduino do not support setScanResponse; omit it if unavailable.
  pAdvertising->start();

  Serial.println("[BLE] Advertising started: YoloUNO LED");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Booting...");

  // Start LED task
  xTaskCreate(TaskLEDControl, "LED Control", 4096, NULL, 2, NULL);

  // Start BLE
  startBLE();
}

void loop() {
  // Nothing here; work is done in tasks and BLE stack
  vTaskDelay(pdMS_TO_TICKS(500));
}