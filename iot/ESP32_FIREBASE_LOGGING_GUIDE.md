# Firebase Access Logging Implementation Guide

## Overview
This guide explains how to integrate Firebase Realtime Database access logging on ESP32-S3 via 4G hotspot.

## System Architecture

```
ESP32-S3 (Car Device)
    ↓
    ├── WiFiManager (auto-reconnect to 4G hotspot)
    ├── FirebaseLogger (sends logs to Firebase)
    ├── FSM (State Machine - triggers unlock events)
    └── UART2 ↔ PN532 NFC Reader
    
WiFi Hotspot (4G)
    ↓
    Firebase Realtime Database
    ├── /access_logs
    │   ├── log_id_1
    │   │   ├── event: "CAR_UNLOCKED"
    │   │   ├── method: "NFC_PHASE_0" | "BLE_FAST" | "UWB_RANGING"
    │   │   ├── timestamp: <Firebase server timestamp>
    │   │   └── data: {...}
    │   └── ...
    └── /anomaly_decisions (optional, for AI analysis)
```

## Phase 1: Firebase Cloud Setup ✓ COMPLETED

### Step 1.1: Create Realtime Database
1. Go to [Firebase Console](https://console.firebase.google.com)
2. Select your project → Realtime Database
3. Create database (Start in Test Mode, upgrade later)
4. Note the Database URL: `https://your-project.firebaseio.com`

### Step 1.2: Configure Security Rules (Permissive for Testing)
```json
{
  "rules": {
    "access_logs": {
      ".read": true,
      ".write": true
    },
    "anomaly_decisions": {
      ".read": true,
      ".write": true
    }
  }
}
```

**⚠️ NOTE:** These rules are for testing only. In production:
- Restrict `.write` to authenticated users
- Validate data structure
- Add timestamp validation

### Step 1.3: Get Web API Key
1. Project Settings → General
2. Copy **Web API Key**
3. Add to `iot/src/main.cpp` as `FIREBASE_API_KEY`

## Phase 2: 4G Hotspot Setup ✓ USER RESPONSIBILITY

### Step 2.1: Enable Personal Hotspot
- **iOS:** Settings → Personal Hotspot → ON
- **Android:** Settings → Tethering & Portable Hotspot → Personal Hotspot → ON

### Step 2.2: Configure Band (Critical!)
**ESP32-S3 works best on 2.4 GHz band:**

**iOS:**
- Settings → Personal Hotspot → Maximize Compatibility → ON

**Android:**
- Mobile Hotspot settings → Band preference → 2.4 GHz

### Step 2.3: Note WiFi Credentials
- SSID: _________________________
- Password: _________________________

## Phase 3: ESP32 Firmware ✓ COMPLETED

### Step 3.1: Update WiFi & Firebase Credentials
Edit `iot/src/main.cpp`:

```cpp
// ===== CONFIGURATION =====
const char* WIFI_SSID = "Your_4G_Hotspot_SSID";           // Change this
const char* WIFI_PASSWORD = "Your_4G_Hotspot_Password";   // Change this

const char* FIREBASE_DB_URL = "https://your-project.firebaseio.com";  // Change this
const char* FIREBASE_API_KEY = "YOUR_WEB_API_KEY";                    // Change this
```

### Step 3.2: WiFi Manager Features
```cpp
#include "wifi_manager.h"

// Auto-reconnect configuration in main.cpp setup():
WiFiManager::WiFiConfig wifiConfig = {
  .ssid = WIFI_SSID,
  .password = WIFI_PASSWORD,
  .reconnectInterval = 5000,      // Retry every 5 seconds
  .connectionTimeout = 15000,     // Timeout after 15 seconds
  .maxRetries = 5                 // Reset after 5 failed attempts
};
WiFiManager::begin(wifiConfig);
```

Features:
- ✅ Automatic reconnection on connection loss
- ✅ Periodic retry with configurable backoff
- ✅ Graceful handling of transient network issues
- ✅ RSSI monitoring for signal strength
- ✅ Power saving mode disabled for stability

### Step 3.3: Firebase Logger Features
```cpp
#include "firebase_logger.h"

// Initialize with Firebase credentials
FirebaseLogger::Config firebaseConfig = {
  .databaseUrl = FIREBASE_DB_URL,
  .apiKey = FIREBASE_API_KEY
};
FirebaseLogger::begin(firebaseConfig);
```

Event Types:
```cpp
enum EventType {
  EVENT_CAR_UNLOCKED,    // Log unlock events
  EVENT_CAR_LOCKED,      // Log lock events
  EVENT_KEY_SHARED,      // Share digital key with friend
  EVENT_KEY_REVOKED      // Revoke shared key
};

enum AuthMethod {
  METHOD_NFC_PHASE_0,    // Traditional NFC
  METHOD_BLE_FAST,       // Fast BLE authentication
  METHOD_UWB_RANGING     // Ultra-Wideband ranging
};
```

### Step 3.4: JSON Log Structure
Each log entry in Firebase:
```json
{
  "event": "CAR_UNLOCKED",
  "method": "NFC_PHASE_0",
  "timestamp": 1619827200000,
  "data": "optional_additional_info"
}
```

### Step 3.5: Integration with FSM
The Firebase logger is integrated into the FSM state machine transitions. When the vehicle enters the `UNLOCKING_COMPLETE` state after a successful unlock, the logger automatically sends:

```cpp
// Called from FSM when unlock completes
FirebaseLogger::logUnlock(FirebaseLogger::METHOD_NFC_PHASE_0);
```

**Location:** To add manual logging in FSM states, include `firebase_logger.h` and call:
```cpp
#include "firebase_logger.h"

// In FSM::tick() when entering UNLOCKING_COMPLETE state:
if (newState == State::UNLOCKING_COMPLETE) {
  // Determine auth method from context
  FirebaseLogger::AuthMethod method = FirebaseLogger::METHOD_UNKNOWN;
  if (context.ble_authenticated) {
    method = FirebaseLogger::METHOD_BLE_FAST;
  } else if (context.nfc_authenticated) {
    method = FirebaseLogger::METHOD_NFC_PHASE_0;
  }
  
  FirebaseLogger::logUnlock(method);
}
```

## Phase 4: Testing & Monitoring ✓ OPERATIONAL GUIDANCE

### Step 4.1: Check Serial Monitor
```
[Setup] WiFi manager initialized
[WiFi] ╔═══════════════════════════════════════════╗
[WiFi] ║  WiFi Manager with Auto-Reconnect        ║
[WiFi] ╚═══════════════════════════════════════════╝
[WiFi] SSID: Your_4G_Hotspot_SSID
[WiFi] → Connecting to 'Your_4G_Hotspot_SSID' (attempt 1/5)...
[WiFi] ✓ Connected! IP: 192.168.1.xxx
[WiFi] RSSI: -45 dBm

[Firebase] ╔═══════════════════════════════════════════╗
[Firebase] ║  Firebase Logger - Access Logs            ║
[Firebase] ╚═══════════════════════════════════════════╝
[Firebase] Database URL: https://your-project.firebaseio.com
[Firebase] → POST https://your-project.firebaseio.com/access_logs.json?auth=...
[Firebase] ✓ Log sent successfully (HTTP 200)
```

### Step 4.2: Monitor WiFi Connection
```cpp
// In your debug loop:
Serial.printf("WiFi: %s, RSSI: %d dBm\n",
              WiFiManager::isConnected() ? "✓" : "✗",
              WiFiManager::getRSSI());
```

### Step 4.3: Observe Firebase Logs in Real-time
1. Open Firebase Console
2. Go to Realtime Database → Data tab
3. Expand `/access_logs`
4. Each unlock should appear as a new entry:
   ```
   /access_logs
   ├── -M5aB7cD8eF9gH0i1jK2
   │   ├── event: "CAR_UNLOCKED"
   │   ├── method: "NFC_PHASE_0"
   │   ├── timestamp: 1716856440000
   │   └── data: null
   ```

### Step 4.4: Test Scenarios
| Scenario | Expected Behavior |
|----------|-------------------|
| ESP32 boots, 4G available | WiFi connects in <5s, Firebase ready |
| Device moves away, signal drops | Auto-reconnect after 5s delay |
| 4G connection temporarily lost | Queued logs sent when reconnected |
| NFC unlock while connected | Log entry appears in Firebase immediately |
| No WiFi available (offline mode) | Logs queued in memory, sent when online |

### Step 4.5: Debugging Failed Logs
If logs don't appear in Firebase:

```cpp
// Check WiFi status
Serial.printf("WiFi: %s\n", WiFiManager::isConnected() ? "Connected" : "Disconnected");
Serial.printf("RSSI: %d dBm\n", WiFiManager::getRSSI());

// Check Firebase error
Serial.printf("Firebase Error: %s\n", FirebaseLogger::getLastError());

// Manually retry
FirebaseLogger::retryLastRequest();
```

## Troubleshooting

### Issue: "WiFi not connected"
- **Solution:** Check SSID and password in `main.cpp`
- **Check:** Is hotspot on? Is 2.4 GHz band enabled?
- **Test:** `Serial.println(WiFi.SSID());` to verify

### Issue: "HTTP 401: Unauthorized"
- **Solution:** Update API Key in `main.cpp`
- **Check:** API Key from Firebase Console is correct
- **Verify:** Database Rules allow `.write: true` (for testing)

### Issue: "HTTP 400: Bad Request"
- **Solution:** JSON structure is invalid
- **Check:** `ArduinoJson.h` library is updated
- **Log:** Print `jsonString` to serial for debugging

### Issue: Logs appear slowly or not at all
- **Check:** Serial Monitor for "Rate limiting" messages (min 100ms between logs)
- **Check:** RSSI value (-100 = no signal, 0 = perfect)
- **Solution:** Ensure phone is within range and hotspot is active

## Next Steps

1. **Deploy to Production:**
   - Update Firebase Rules to be more restrictive
   - Add user authentication (Firebase Auth)
   - Implement timestamp validation

2. **Enhance Logging:**
   - Add GPS coordinates if available
   - Log unlock duration
   - Track failed unlock attempts

3. **Data Analysis:**
   - Export logs to BigQuery
   - Analyze unlock patterns
   - Detect anomalies with AI

4. **User Alerts:**
   - Notify owner on unusual access
   - Show access history in app
   - Privacy-preserving audit trail

## Files Modified/Created

| File | Purpose |
|------|---------|
| `iot/include/wifi_manager.h` | WiFi manager interface |
| `iot/src/wifi_manager.cpp` | WiFi auto-reconnect implementation |
| `iot/include/firebase_logger.h` | Firebase logger interface |
| `iot/src/firebase_logger.cpp` | Firebase HTTP POST implementation |
| `iot/src/main.cpp` | Configuration & initialization |
| `iot/src/fsm/fsm.cpp` | (Optional) Add logging to FSM transitions |

## Dependencies Required

Add to `platformio.ini`:
```ini
lib_deps =
    ArduinoJson
    HTTPClient
```

## References

- Firebase Realtime Database: https://firebase.google.com/docs/database
- ESP32-S3 WiFi API: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_wifi.html
- ArduinoJson: https://arduinojson.org/
