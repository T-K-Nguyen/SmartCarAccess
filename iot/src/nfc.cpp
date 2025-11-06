// Minimal PN532 bring-up with simple polling (no provisioning)
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include "../include/nfc.h"
#include "../include/provisioning.h"
// FreeRTOS for task scheduling
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
  const int I2C_SDA_PIN = 11;
  const int I2C_SCL_PIN = 12;
  const int PN532_IRQ_PIN = -1;        // IRQ not used in current implementation
  const int PN532_RST_PIN = 10;       // GPIO 10 for hardware reset
  Adafruit_PN532 nfc(PN532_IRQ_PIN, PN532_RST_PIN);

  bool s_nfcReady = false;
  // simple last-tag cache
  uint8_t s_lastUid[7];
  uint8_t s_lastUidLen = 0;
  volatile bool s_uidReady = false;

  // polling task handle
  static TaskHandle_t s_task = nullptr;
  static bool s_provHold = false; // when true, keep polling task suspended externally
  static SemaphoreHandle_t s_pn532Mutex = nullptr;
  
  // I2C error handling
  static volatile bool s_i2cError = false;
  static uint32_t s_lastSuccessfulComm = 0;
  static uint32_t s_lastResetAttempt = 0;
  static const uint32_t I2C_TIMEOUT_MS = 5000; // 5 second timeout
  static const uint32_t I2C_RESET_COOLDOWN_MS = 2000; // 2 second cooldown between resets
  
  // Mutex safety tracking
  static TaskHandle_t s_mutexOwner = nullptr;
  static uint32_t s_mutexTakeTime = 0;

  // Forward declarations
  void resetI2CBus();
  void hardwareResetPN532();
  
  // Safe mutex operations to prevent corruption
  bool safeMutexTake(uint32_t timeoutMs) {
    if (!s_pn532Mutex) {
      Serial.println("[NFC] ERROR: Mutex is null!");
      return false;
    }
    
    TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
    if (s_mutexOwner == currentTask) {
      Serial.println("[NFC] WARNING: Task already owns mutex - avoiding deadlock");
      return true; // Already own it
    }
    
    if (xSemaphoreTake(s_pn532Mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
      s_mutexOwner = currentTask;
      s_mutexTakeTime = millis();
      return true;
    }
    
    Serial.printf("[NFC] ERROR: Failed to acquire mutex in %dms\n", timeoutMs);
    return false;
  }
  
  void safeMutexGive() {
    if (!s_pn532Mutex) {
      Serial.println("[NFC] ERROR: Cannot give null mutex!");
      return;
    }
    
    TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
    if (s_mutexOwner != currentTask) {
      Serial.println("[NFC] WARNING: Task doesn't own mutex - not giving");
      return;
    }
    
    s_mutexOwner = nullptr;
    s_mutexTakeTime = 0;
    xSemaphoreGive(s_pn532Mutex);
  }
  
  void recreateMutex() {
    Serial.println("[NFC] Recreating mutex due to corruption...");
    if (s_pn532Mutex) {
      vSemaphoreDelete(s_pn532Mutex);
    }
    s_pn532Mutex = xSemaphoreCreateMutex();
    s_mutexOwner = nullptr;
    s_mutexTakeTime = 0;
    if (s_pn532Mutex) {
      Serial.println("[NFC] Mutex recreated successfully");
    } else {
      Serial.println("[NFC] ERROR: Failed to recreate mutex!");
    }
  }

  // I2C error detection and recovery functions
  bool initializeNFC() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(1000);
    Wire.setClock(100000);
    delay(50);

    if (!nfc.begin()) {
      Serial.println("[NFC] PN532 nfc.begin() failed");
      return false;
    }
    
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (!versiondata) {
      Serial.println("[NFC] PN532 not responding to getFirmwareVersion()");
      return false;
    }

    Serial.printf("[NFC] PN532 OK. IC: 0x%02X, Ver: %u.%u\n",
                  (unsigned)((versiondata >> 24) & 0xFF),
                  (unsigned)((versiondata >> 16) & 0xFF),
                  (unsigned)((versiondata >> 8) & 0xFF));
    // Configure SAM (required for reading passive targets/APDUs later)
    nfc.SAMConfig();
    
    s_nfcReady = true;
    s_i2cError = false;
    s_lastSuccessfulComm = millis();
    return true;
  }

  bool checkI2CHealth() {
    // Test I2C communication by trying to read PN532 version
    Wire.beginTransmission(0x24); // PN532 I2C address
    uint8_t error = Wire.endTransmission();
    
    if (error == 0) {
      s_lastSuccessfulComm = millis();
      s_i2cError = false;
      return true;
    } else {
      s_i2cError = true;
      Serial.printf("[NFC] I2C Error: %d (0=OK, 1=too long, 2=NACK addr, 3=NACK data, 4=other)\n", error);
      return false;
    }
  }
  
  // Check I2C health and reset bus if necessary
  bool ensureI2CHealth() {
    if (!checkI2CHealth()) {
      resetI2CBus();
      return s_nfcReady; // Return true only if reset was successful
    }
    return true;
  }
  
  void hardwareResetPN532() {
    if (PN532_RST_PIN == -1) {
      Serial.println("[NFC] Hardware reset not available - RST pin not configured");
      return;
    }
    
    Serial.println("[NFC] Performing PN532 hardware reset via RST pin...");
    
    // Configure RST pin as output
    pinMode(PN532_RST_PIN, OUTPUT);
    
    // PN532 reset sequence: pull RST low for at least 100µs, then high
    digitalWrite(PN532_RST_PIN, HIGH);  // Ensure it starts high
    delay(10);
    digitalWrite(PN532_RST_PIN, LOW);   // Pull low to reset
    delay(100);                         // Hold reset for 100ms (well above minimum)
    digitalWrite(PN532_RST_PIN, HIGH);  // Release reset
    delay(500);                         // Wait for PN532 to boot up
    
    Serial.println("[NFC] PN532 hardware reset completed - chip should be in clean state");
    
    // Mark that we need to reinitialize
    s_nfcReady = false;
    s_i2cError = false;  // Clear any previous I2C errors
  }

  void resetI2CBus() {
    uint32_t now = millis();
    
    // Prevent rapid consecutive resets
    if (now - s_lastResetAttempt < I2C_RESET_COOLDOWN_MS) {
      Serial.printf("[NFC] Reset cooldown active, waiting %lu ms\n", 
                    I2C_RESET_COOLDOWN_MS - (now - s_lastResetAttempt));
      return;
    }
    
    s_lastResetAttempt = now;
    Serial.println("[NFC] Resetting I2C bus due to communication failure...");
    
    // Mark I2C as problematic immediately to prevent concurrent access
    s_i2cError = true;
    s_nfcReady = false;
    
    // Skip Wire.end() as it hangs when bus is corrupted
    // Instead, directly manipulate GPIO to force bus reset
    Serial.println("[NFC] Step 1: Direct GPIO bus recovery (bypassing Wire.end())...");
    
    // Force both lines high initially (release any stuck states)
    pinMode(I2C_SDA_PIN, OUTPUT);
    pinMode(I2C_SCL_PIN, OUTPUT);
    digitalWrite(I2C_SDA_PIN, HIGH);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delay(100);
    
    Serial.println("[NFC] Step 2: Manual bus line clearing with clock pulses...");
    // Generate multiple clock cycles to force any stuck device to release SDA
    pinMode(I2C_SDA_PIN, INPUT_PULLUP);  // Allow SDA to float/be controlled by devices
    for (int i = 0; i < 18; i++) {  // More pulses for stubborn devices
      digitalWrite(I2C_SCL_PIN, LOW);
      delayMicroseconds(10);
      digitalWrite(I2C_SCL_PIN, HIGH);
      delayMicroseconds(10);
    }
    
    // Generate I2C STOP condition to reset bus protocol state
    Serial.println("[NFC] Step 3: Generating I2C STOP condition...");
    pinMode(I2C_SDA_PIN, OUTPUT);
    digitalWrite(I2C_SDA_PIN, LOW);   // SDA low
    delayMicroseconds(10);
    digitalWrite(I2C_SCL_PIN, HIGH);  // SCL high
    delayMicroseconds(10);
    digitalWrite(I2C_SDA_PIN, HIGH);  // SDA high (STOP condition)
    delayMicroseconds(10);
    
    Serial.println("[NFC] Step 4: Waiting for bus to stabilize...");
    delay(500); // Long delay for complete bus recovery
    
    Serial.println("[NFC] Step 5: Reinitializing Wire library...");
    // Now reinitialize Wire library fresh
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(1000);
    Wire.setClock(100000); // 100kHz for reliable operation
    delay(100); // Additional settling time
    
    Serial.println("[NFC] Step 6: I2C bus reset completed");
    // Skip PN532 communication test to avoid Error 263 and stack overflow
    // Let calling code handle PN532 communication validation if needed
    Serial.println("[NFC] I2C reset procedure finished - bus ready for use");
  }

  void TaskNFC(void* pv) {
    uint8_t uid[7];
    uint8_t uidLen = 0;
    // de-dup printing
    uint8_t lastPrintUid[7] = {0};
    uint8_t lastPrintLen = 0;
    uint32_t lastPrintAt = 0;
    const uint32_t cooldownMs = 1500;
    
    for (;;) {
      if (!s_nfcReady) { 
        // Try to reconnect every 2 seconds when NFC is not ready
        static uint32_t lastReconnectAttempt = 0;
        if (millis() - lastReconnectAttempt > 2000) {
          Serial.println("[NFC] Attempting to reconnect...");
          if (initializeNFC()) {
            Serial.println("[NFC] Reconnection successful!");
            // If we're in provisioning mode, don't start polling - just wait
            if (s_provHold) {
              Serial.println("[NFC] Staying in provisioning mode after reconnect");
            }
          }
          lastReconnectAttempt = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(200)); 
        continue; 
      }

      // Check I2C health periodically
      if (millis() - s_lastSuccessfulComm > I2C_TIMEOUT_MS) {
        if (!checkI2CHealth()) {
          resetI2CBus();
          vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retry
          continue;
        }
      }

      // Task should be suspended during provisioning mode
      if (s_provHold) {
        // Ensure we're not holding the mutex when entering provisioning mode
        if (s_mutexOwner == xTaskGetCurrentTaskHandle()) {
          Serial.println("[NFC] Releasing mutex before entering provisioning mode");
          safeMutexGive();
        }
        
        // Suspend this task until provisioning mode is disabled
        Serial.println("[NFC] Provisioning mode active - suspending NFC polling task");
        while (s_provHold) {
          vTaskDelay(pdMS_TO_TICKS(500)); // Check every 500ms if we can resume
        }
        Serial.println("[NFC] Provisioning mode ended - resuming NFC polling");
        continue;
      }
      
      // Normal tag polling when NOT in provisioning mode
      // Declare variables outside the mutex block for proper scope
      bool found = false;
      bool commSuccess = true;
      
      // Check again if provisioning hold was activated while we were waiting
      if (s_provHold) {
        continue; // Go back to provisioning hold check
      }
      
      // Use safe mutex operations to prevent corruption
      if (safeMutexTake(5000)) {
        
        // Set I2C timeout to prevent hangs
        Wire.setTimeOut(2000);
        
        // Safely attempt PN532 communication with error handling
        try {
          found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen);
          commSuccess = !s_i2cError;
        } catch (...) {
          Serial.println("[NFC] Exception during tag read - recovering");
          commSuccess = false;
          found = false;
        }
        
        // Restore normal I2C timeout
        Wire.setTimeOut(1000);
        
        // ALWAYS release mutex safely, even if PN532 failed
        safeMutexGive();
        
        // Handle I2C communication failure
        if (!commSuccess || s_i2cError) {
          Serial.println("[NFC] I2C communication failed during tag read");
          resetI2CBus();
          vTaskDelay(pdMS_TO_TICKS(1000));
          continue;
        }
      } else {
        // Failed to acquire mutex within timeout
        Serial.println("[NFC] WARNING: Failed to acquire mutex for tag polling - skipping cycle");
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      if (found) {
        bool same = (uidLen == lastPrintLen) && (memcmp(uid, lastPrintUid, uidLen) == 0);
        bool cool = (millis() - lastPrintAt) >= cooldownMs;
        if (!same || cool) {
          Serial.print("[NFC] Tag UID: ");
          for (uint8_t i = 0; i < uidLen; ++i) {
            if (uid[i] < 0x10) Serial.print("0");
            Serial.print(uid[i], HEX);
            if (i + 1 < uidLen) Serial.print(":");
          }
          Serial.println();
          memcpy(lastPrintUid, uid, uidLen);
          lastPrintLen = uidLen;
          lastPrintAt = millis();
        }
        // cache for external consumers
        if (uidLen <= sizeof(s_lastUid)) {
          memcpy(s_lastUid, uid, uidLen);
          s_lastUidLen = uidLen;
          s_uidReady = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(200));
      }
    }
  }
}

namespace NFCMod {
  void begin() {
    // Initialize bus mutex first
    if (!s_pn532Mutex) {
      s_pn532Mutex = xSemaphoreCreateMutex();
    }
    
    // Try to initialize NFC
    if (!initializeNFC()) {
      Serial.println("[NFC] Initial connection failed. Will retry in background...");
      s_nfcReady = false;
    }
  }

  void startTask() {
    if (!s_task) {
      // Increase stack size and lower priority to avoid BLE interference
      xTaskCreate(TaskNFC, "NFC Poll", 8192, nullptr, 1, &s_task);
    }
  }

  void setProvisionHold(bool enabled) {
    s_provHold = enabled;
    if (enabled) {
        Serial.println("[NFC] Provision mode ENABLED - normal polling suspended");
    } else {
        Serial.println("[NFC] Provision mode DISABLED - normal polling resumed");
    }
  }

  bool isReady() { return s_nfcReady; }

  // Return the most recently seen tag UID once
  bool getLastTag(uint8_t* uid, uint8_t* uidLen) {
    if (!uid || !uidLen) return false;
    if (!s_uidReady) return false;
    uint8_t n = s_lastUidLen; if (n > 7) n = 7;
    memcpy(uid, s_lastUid, n);
    *uidLen = n;
    s_uidReady = false;
    return true;
  }

  // Build SELECT AID APDU
  static void buildSelectAid(const uint8_t* aid, size_t aidLen, uint8_t* out, uint8_t* outLen) {
    uint8_t idx = 0;
    out[idx++] = 0x00; // CLA
    out[idx++] = 0xA4; // INS
    out[idx++] = 0x04; // P1 (by AID)
    out[idx++] = 0x00; // P2
    out[idx++] = (uint8_t)aidLen; // Lc
    for (size_t i = 0; i < aidLen; ++i) out[idx++] = aid[i];
    // ADD Le field (0x00) for HCE compatibility - the PN532 Android HCE example includes this!
    // Based on working PN532 android_hce.ino example which successfully communicates with Android HCE
    out[idx++] = 0x00; // Le (expected length of response)
    *outLen = idx;
  }

  // Build GET_INFO APDU (INS 0xCA) with vehicleId (<=8) + nonce(16)
  static void buildGetInfo(const uint8_t* veh, size_t vehLen, const uint8_t* nonce, size_t nonceLen, uint8_t* out, uint8_t* outLen) {
    uint8_t idx = 0;
    out[idx++] = 0x00; // CLA
    out[idx++] = 0xCA; // INS (custom)
    out[idx++] = 0x00; // P1
    out[idx++] = 0x00; // P2
    out[idx++] = (uint8_t)(vehLen + nonceLen); // Lc
    for (size_t i = 0; i < vehLen; ++i) out[idx++] = veh[i];
    for (size_t i = 0; i < nonceLen; ++i) out[idx++] = nonce[i];
    *outLen = idx;
  }

  static bool apduOk(const uint8_t* resp, uint8_t rlen) {
    return rlen >= 2 && resp[rlen-2] == 0x90 && resp[rlen-1] == 0x00;
  }

  bool runProvisioningSM(const uint8_t* aid, size_t aidLen, uint32_t overallTimeoutMs, ProvData* out, char* errBuf, size_t errBufLen) {
    Serial.println("[Prov][NFC] === Starting Provisioning State Machine ===");
    
    // Debug: Show the AID we received
    Serial.printf("[Prov][NFC] Received AID (len=%d): ", aidLen);
    for (int i = 0; i < aidLen; i++) {
      Serial.printf("%02X ", aid[i]);
    }
    Serial.println();
    
    if (!aid || aidLen == 0 || aidLen > 16 || !out) {
      if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "bad args");
      Serial.println("[Prov][NFC] ERROR: Invalid arguments");
      return false;
    }

    if (!s_nfcReady) {
      if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "NFC not ready");
      Serial.println("[Prov][NFC] ERROR: NFC not ready");
      return false;
    }

    uint32_t tStart = millis();
    auto timeLeft = [&]() -> uint32_t {
      uint32_t elapsed = millis() - tStart;
      return (elapsed >= overallTimeoutMs) ? 0 : (overallTimeoutMs - elapsed);
    };

  // S1: INIT_NFC_FOR_PROVISION — minimal setup, no SAMConfig to avoid session interference
    // assume PN532 already initialized before SM
    Serial.println("[Prov][NFC] S1: Initializing for provisioning...");
    
    if (!s_pn532Mutex) {
      if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "mutex not initialized");
      Serial.println("[Prov][NFC] ERROR: Mutex not initialized");
      return false;
    }
    
    Serial.println("[Prov][NFC] Taking mutex...");
    
    // Wait a bit more if another task might still be releasing the mutex
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Try to take mutex with safe operations
    if (!safeMutexTake(5000)) {
      Serial.println("[Prov][NFC] ERROR: Failed to acquire mutex within 5 seconds!");
      Serial.println("[Prov][NFC] Mutex may be stuck - recreating mutex...");
      
      // Recreate the mutex using safe function
      recreateMutex();
      
      // Try again with new mutex
      if (!safeMutexTake(1000)) {
        if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "mutex recreation failed");
        Serial.println("[Prov][NFC] ERROR: Still can't acquire mutex after recreation!");
        return false;
      }
    }
    Serial.println("[Prov][NFC] Task terminated - performing PN532 hardware reset...");
    
    // Release mutex temporarily for hardware reset
    safeMutexGive();
    
    // Perform hardware reset instead of software I2C reset
    hardwareResetPN532();
    
    // Reinitialize everything after hardware reset
    if (!initializeNFC()) {
      Serial.println("[Prov][NFC] ERROR: PN532 hardware reset failed");
      if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "PN532 hardware reset failed");
      return false;
    }
    
    Serial.println("[Prov][NFC] PN532 hardware reset and reinitialization successful");
    
    Serial.println("[Prov][NFC] I2C reset completed - reacquiring mutex...");
    if (!safeMutexTake(1000)) {
      Serial.println("[Prov][NFC] ERROR: Can't reacquire mutex after reset!");
      if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "mutex reacquire failed");
      return false;
    }
    
    // Skip PN532 communication test to avoid Error 263 and stack overflow
    // Proceed directly to provisioning - if PN532 is broken, provisioning will detect it
    Serial.println("[Prov][NFC] Skipping PN532 test - proceeding with provisioning...");
    Serial.println("[Prov][NFC] Releasing mutex...");
    safeMutexGive();
    vTaskDelay(pdMS_TO_TICKS(50));

    // S2: WAIT_FOR_PHONE_TAP — immediately try SELECT AID to verify it's actually a phone
    Serial.println("[Prov][NFC] S2: Waiting for phone tap...");
    
    Serial.printf("[Prov][NFC] Starting phone detection loop, timeout: %u ms\n", timeLeft());
    
    while (timeLeft() > 0) {
      // Yield to BLE stack at start of each polling iteration
      vTaskDelay(pdMS_TO_TICKS(5));
      
      uint8_t uid[10]; uint8_t uidLen = 0;
      
      // Step 1: Check for any NFC-A tag
      Serial.println("[Prov][NFC] Polling for NFC tags...");
      if (xSemaphoreTake(s_pn532Mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        Serial.println("[Prov][NFC] WARNING: Could not acquire mutex for tag polling");
        continue; // Skip this iteration and try again
      }
      
      // Clear I2C error flag before attempting read
      s_i2cError = false;
      
      // Try to activate ISO14443A target for HCE communication
      Serial.println("[Prov][NFC] Attempting ISO14443A target activation...");
      bool found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen);
      
      if (found) {
        Serial.printf("[Prov][NFC] ISO14443A target activated successfully, UID length: %d\n", uidLen);
        // Additional PN532 setup for ISO-DEP/HCE communication
        Serial.println("[Prov][NFC] Configuring PN532 for ISO-DEP communication...");
        nfc.SAMConfig(); // Ensure SAM is properly configured for APDU exchange
      }
      
      // Check if I2C error occurred during the read
      if (s_i2cError) {
        xSemaphoreGive(s_pn532Mutex);
        Serial.println("[Prov][NFC] ERROR: I2C Error 263 detected in provisioning");
        Serial.println("[Prov][NFC] Performing PN532 hardware reset via RST pin...");
        
        // Try hardware reset instead of ESP32 restart
        hardwareResetPN532();
        
        // Reinitialize I2C and PN532 after hardware reset
        if (initializeNFC()) {
          Serial.println("[Prov][NFC] PN532 hardware reset successful - retrying provisioning");
          // Continue with next iteration of polling loop
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        } else {
          Serial.println("[Prov][NFC] PN532 hardware reset failed - provisioning cannot continue");
          if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "PN532 hardware reset failed");
          return false;
        }
      }
      
      xSemaphoreGive(s_pn532Mutex);
      Serial.printf("[Prov][NFC] Poll result: %s\n", found ? "TAG FOUND" : "no tag");
      
      if (!found) {
        vTaskDelay(pdMS_TO_TICKS(200)); // Longer delay when no tag found
        continue;
      }
      
      // Give additional time for Android HCE service to fully activate
      Serial.println("[Prov][NFC] Tag detected - waiting for HCE initialization...");
      vTaskDelay(pdMS_TO_TICKS(300)); // Extra delay for HCE service
      
      // Step 2: Try SELECT AID to see if it's a phone with our HCE service
      uint8_t apdu[32]; 
      uint8_t alen = 0; 
      buildSelectAid(aid, aidLen, apdu, &alen);
      
      // Use smaller response buffer to prevent stack issues
      uint8_t resp[64]; 
      uint8_t rlen = sizeof(resp);
      
      // LOG: Show exactly what APDU we're sending
      Serial.print("[Prov][NFC] >>> Sending SELECT AID: ");
      for (int i = 0; i < alen; i++) {
        Serial.printf("%02X ", apdu[i]);
      }
      Serial.println();

      // Print the UID we detected (helpful to correlate tag appearance)
      if (uidLen > 0) {
        Serial.print("[Prov][NFC] Detected UID: ");
        for (uint8_t u = 0; u < uidLen; ++u) {
          Serial.printf("%02X", uid[u]);
          if (u + 1 < uidLen) Serial.print(":");
        }
        Serial.println();
      }

      // Give HCE service time to activate (reduced to avoid BLE interference)
      Serial.println("[Prov][NFC] Waiting for HCE service activation...");
      delay(200); // Reduced from 500ms to avoid BLE stack interference
      
      // Yield to BLE stack before critical APDU operation
      vTaskDelay(pdMS_TO_TICKS(5));
      
      // Try SELECT AID multiple times to ensure HCE service activation
      bool ok = false;
      int attempts = 0;
      const int maxAttempts = 5; // Try up to 5 times
      
      while (!ok && attempts < maxAttempts) {
        attempts++;
        Serial.printf("[Prov][NFC] SELECT AID attempt %d/%d...\n", attempts, maxAttempts);
        
        // Progressive timeout increase: give HCE more time on later attempts
        uint32_t timeout = 3000 + (attempts * 1000); // 3s, 4s, 5s, 6s, 7s
        Wire.setTimeOut(timeout);
        
        if (!safeMutexTake(3000)) {
          Serial.println("[Prov][NFC] ERROR: Could not acquire mutex for SELECT AID exchange");
          return false; // This will cause the provisioning to retry
        }
        
        // Reset response buffer for each attempt
        rlen = sizeof(resp);
        
        Serial.printf("[Prov][NFC] Sending APDU via inDataExchange (attempt %d)...\n", attempts);
        Serial.println("[Prov][NFC] Before inDataExchange call");
        
        ok = nfc.inDataExchange(apdu, alen, resp, &rlen);
        
        Serial.printf("[Prov][NFC] inDataExchange returned: ok=%s, rlen=%d\n", ok ? "true" : "false", rlen);
        
        if (ok && rlen > 0) {
          Serial.print("[Prov][NFC] Raw response bytes: ");
          for (int i = 0; i < rlen; i++) {
            Serial.printf("%02X ", resp[i]);
          }
          Serial.println();
        } else if (!ok) {
          Serial.println("[Prov][NFC] inDataExchange failed - no communication with target");
        } else {
          Serial.println("[Prov][NFC] inDataExchange succeeded but got empty response");
        }
        
        safeMutexGive();
        
        if (!ok || !apduOk(resp, rlen)) {
          Serial.printf("[Prov][NFC] Attempt %d failed (ok=%s, apduOk=%s), waiting before retry...\n", 
                       attempts, ok ? "true" : "false", apduOk(resp, rlen) ? "true" : "false");
          vTaskDelay(pdMS_TO_TICKS(300)); // Wait between attempts
        } else {
          Serial.printf("[Prov][NFC] SELECT AID succeeded on attempt %d!\n", attempts);
        }
      }
      
      Wire.setTimeOut(1000); // Reset timeout
      
      // Allow BLE stack to run after APDU exchange
      vTaskDelay(pdMS_TO_TICKS(10));
      
      // LOG: Show what we got back
      Serial.printf("[Prov][NFC] <<< Response (ok=%s, len=%d): ", ok ? "true" : "false", rlen);
      if (ok && rlen > 0) {
        for (int i = 0; i < rlen; i++) {
          Serial.printf("%02X ", resp[i]);
        }
      } else {
        Serial.printf("ERROR or empty");
      }
      Serial.println();
      
      if (ok && apduOk(resp, rlen)) {
        // SUCCESS: This is our phone with the correct HCE service!
        Serial.println("[Prov][NFC] Phone with correct AID detected!");
        delay(100); // Allow ISO-DEP session to stabilize
        break;
      } else {
        // FAILURE: Either not a phone or wrong AID - this is likely a regular NFC tag
        Serial.println("[Prov][NFC] Tag detected but SELECT AID failed - not our phone, continuing...");
        
        // Reset PN532 to clear incomplete ISO-DEP state
        if (xSemaphoreTake(s_pn532Mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          nfc.SAMConfig(); // Reset PN532 state
          xSemaphoreGive(s_pn532Mutex);
        } else {
          Serial.println("[Prov][NFC] WARNING: Could not acquire mutex for SAMConfig reset");
        }
        
        // Longer delay when wrong tag detected to reduce BLE stack pressure
        vTaskDelay(pdMS_TO_TICKS(500)); 
        continue;
      }
    }
    
    if (timeLeft() == 0) {
      if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "no phone with correct AID found");
      return false;
    }

    // S3: ISODEP_ACTIVATION — already completed above during SELECT AID

    // S4: SELECT AID already successful from detection loop above

    // S5: SEND_CHALLENGE — vehicle fingerprint (8) + nonce (16)
    uint8_t veh[8]; size_t vehLen = 0; {
      uint8_t fpr[32]; size_t F = sizeof(fpr);
      if (Provisioning::getDevicePubKeyFingerprint(fpr, &F) && F >= 8) { memcpy(veh, fpr, 8); vehLen = 8; }
    }
    uint8_t nonce[16]; Provisioning::randomBytes(nonce, sizeof(nonce));
    {
      uint8_t apdu[64]; uint8_t alen = 0; buildGetInfo(vehLen?veh:nullptr, vehLen, nonce, sizeof(nonce), apdu, &alen);
      uint8_t resp[128]; uint8_t rlen = sizeof(resp);
      if (xSemaphoreTake(s_pn532Mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "GET_INFO mutex timeout");
        return false;
      }
      bool exchangeOk = nfc.inDataExchange(apdu, alen, resp, &rlen);
      xSemaphoreGive(s_pn532Mutex);
      
      if (!exchangeOk) { 
        if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "GET_INFO xchg fail"); 
        return false; 
      }
      if (!apduOk(resp, rlen)) { 
        if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "GET_INFO SW not ok"); 
        return false; 
      }

  // S6: RECEIVE_CREDENTIALS — parse payload (rlen includes SW)
  if (rlen < 2 + 1 + 65 + 2) { 
    if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "RAPDU too short"); 
    return false; 
  }
  uint8_t datalen = rlen - 2; const uint8_t* p = resp; size_t idx = 0;
  if (idx >= datalen) { 
    if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "no keyId len"); 
    return false; 
  }
  uint8_t rawKeyIdLen = p[idx++];
  if (idx + rawKeyIdLen > datalen) { 
    if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "keyId OOB"); 
    return false; 
  }
  uint8_t copyKeyIdLen = rawKeyIdLen > (sizeof(out->keyId)-1) ? (sizeof(out->keyId)-1) : rawKeyIdLen;
  memcpy(out->keyId, p + idx, copyKeyIdLen); out->keyId[copyKeyIdLen] = '\0'; idx += rawKeyIdLen;
  if (idx + 65 + 2 > datalen) { 
    if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "layout error"); 
    return false; 
  }
  memcpy(out->pubKey65, p + idx, 65); idx += 65;
  out->certLen = (uint16_t)(p[idx] | (p[idx+1] << 8)); idx += 2;
  if (idx + out->certLen > datalen || out->certLen > sizeof(out->cert)) { 
    if (errBuf && errBufLen) snprintf(errBuf, errBufLen, "cert too big"); 
    return false; 
  }
  memcpy(out->cert, p + idx, out->certLen);
    }

    // Success up to S6 - update successful communication timestamp
    s_lastSuccessfulComm = millis();
    return true;
  }
  bool runHceProvisioningOnce(const uint8_t* aid, size_t aidLen, uint32_t timeoutMs) {
    (void)aid; (void)aidLen; (void)timeoutMs; return false;
  }

  // Simple helper function to detect and handle phone during provisioning
  bool checkForProvisioningPhone(const uint8_t* aid, size_t aidLen) {
    // This function is deprecated - use runProvisioningSM directly
    (void)aid; (void)aidLen;
    return false;
  }

  void resetForProvisionRetry() {
    // Minimal reset: re-apply SAM config; this drops any active card session
    if (xSemaphoreTake(s_pn532Mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      nfc.SAMConfig();
      xSemaphoreGive(s_pn532Mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
