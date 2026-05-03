# ESP32-S3 Firebase Logging - Quick Start Checklist

## Pre-Flight Checklist (Before Upload)

### Firebase Cloud Setup
- [ ] Created Realtime Database in Firebase Console
- [ ] Copied Database URL: `https://your-project.firebaseio.com`
- [ ] Copied Web API Key from Project Settings
- [ ] Set temporary Rules: `{".read": true, ".write": true}`

### 4G Hotspot Preparation
- [ ] Enabled Personal Hotspot on mobile device
- [ ] Set 2.4 GHz band (Critical for ESP32-S3!)
- [ ] Noted SSID: ____________________________
- [ ] Noted Password: ____________________________

### Firmware Configuration
Edit `iot/src/main.cpp` lines 9-14:
```cpp
const char* WIFI_SSID = "Your_4G_Hotspot_SSID";           // ← Replace with your SSID
const char* WIFI_PASSWORD = "Your_4G_Hotspot_Password";   // ← Replace with your password
const char* FIREBASE_DB_URL = "https://your-project.firebaseio.com";  // ← Replace
const char* FIREBASE_API_KEY = "YOUR_WEB_API_KEY";        // ← Replace
```

## Upload & Testing

### Step 1: Build & Upload
```bash
cd iot/
pio run -t upload
```

### Step 2: Monitor Serial Output
```bash
pio device monitor -b 115200
```

### Expected Output (First 10 seconds):
```
╔═══════════════════════════════════════════════════════════╗
║        Smart Car Access - FSM + Firebase Logging         ║
╚═══════════════════════════════════════════════════════════╝
[Setup] WiFi manager initialized
[Setup] Firebase logger initialized
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
[Firebase] Waiting for WiFi connection...
[System] Ready. FSM active, WiFi connecting, waiting for events...
```

### Step 3: Verify WiFi Connection
In Serial Monitor, you should see:
```
[WiFi] ✓ Connected! IP: 192.168.xxx.xxx
```

If you see repeated connection attempts:
- ❌ SSID incorrect → Update `WIFI_SSID` in main.cpp
- ❌ Password incorrect → Update `WIFI_PASSWORD` in main.cpp
- ❌ Hotspot off → Enable hotspot on mobile device
- ❌ Wrong band → Set mobile hotspot to 2.4 GHz

### Step 4: Trigger an Unlock Event
- **NFC:** Tap NFC card with provisioned key
- **BLE:** Connect with authorized phone via BLE

### Step 5: Verify Log in Firebase
```
Expected Serial Output:
[Firebase] → POST https://your-project.firebaseio.com/access_logs.json?auth=...
[Firebase] Payload: {"event":"CAR_UNLOCKED","method":"NFC_PHASE_0","timestamp":{...}}
[Firebase] ✓ Log sent successfully (HTTP 200)

Expected Firebase Console:
/access_logs
├── -M5aB7cD8eF9gH0i1jK2
│   ├── event: "CAR_UNLOCKED"
│   ├── method: "NFC_PHASE_0"
│   ├── timestamp: 1716856440000
│   └── data: null
```

## Troubleshooting

### WiFi Connection Issues

**Problem:** "→ Connecting... (attempt 1/5)" repeats indefinitely
```
[WiFi] → Connecting to 'MySSID' (attempt 1/5)...
[WiFi] ✗ Connection timeout
[WiFi] → Retrying in 5000ms...
```

| Symptom | Fix |
|---------|-----|
| Tries wrong SSID | Check `WIFI_SSID` in main.cpp matches hotspot name |
| Hangs after "Connecting..." | Verify password is correct in main.cpp |
| Timeout/no connection | Check hotspot is on and within WiFi range |
| Reconnects repeatedly | Check WiFi signal strength (RSSI) - too weak? |

**Quick Debug:**
```cpp
// Add to loop() in main.cpp if still debugging:
static uint32_t lastCheck = 0;
if (millis() - lastCheck > 10000) {  // Every 10 seconds
  Serial.printf("[DEBUG] WiFi: %s, RSSI: %d dBm, IP: %s\n",
    WiFiManager::isConnected() ? "Connected" : "Disconnected",
    WiFiManager::getRSSI(),
    WiFiManager::getIPAddress().c_str());
  lastCheck = millis();
}
```

### Firebase Connection Issues

**Problem:** "Waiting for WiFi connection..." appears, then no Firebase logs
```
[Firebase] Database URL: https://your-project.firebaseio.com
[Firebase] Waiting for WiFi connection...
```

| Symptom | Fix |
|---------|-----|
| WiFi says connected but Firebase says offline | Wait 5s after WiFi connects for Firebase to initialize |
| "HTTP 401: Unauthorized" | Check Web API Key in main.cpp is correct |
| "HTTP 400: Bad Request" | JSON structure invalid - check ArduinoJson version |
| Log sent but doesn't appear in Firebase | Check database Rules allow `.write: true` |

**Quick Test - Manual Log Send:**
```cpp
// Add temporary code to setup() after Firebase init:
delay(3000);  // Wait for WiFi
FirebaseLogger::logEvent(FirebaseLogger::EVENT_CAR_UNLOCKED, 
                        FirebaseLogger::METHOD_NFC_PHASE_0, 
                        "TEST");
```

### Rate Limiting

**Problem:** Multiple rapid unlocks show "Rate limiting" messages
```
[Firebase] ⚠ Rate limiting - log queued
```

This is normal. The logger enforces 100ms minimum between logs to prevent Firebase quota exhaustion. Queued logs are sent immediately when the rate limit expires.

To adjust rate limit, edit `iot/src/firebase_logger.cpp` line ~195:
```cpp
if (now - g_lastLogTime < 100) {  // ← Change 100 to your preferred milliseconds
```

## Production Readiness

### Before Deploying to Real Vehicles:

1. **Secure Firebase Rules**
   ```json
   {
     "rules": {
       "access_logs": {
         ".read": "auth.uid != null",
         ".write": "auth.uid != null && auth.uid == root.child('vehicle_owner').val()"
       }
     }
   }
   ```

2. **Add User Authentication**
   - Enable Firebase Auth in Console
   - Modify C++ code to include ID token in HTTP header

3. **Data Validation**
   - Validate timestamp format
   - Verify method enum values
   - Reject malformed entries

4. **Monitor & Alerting**
   - Set up Cloud Functions to alert on suspicious patterns
   - Implement rate limiting on database writes
   - Log to Cloud Logging for audit trail

5. **Privacy Compliance**
   - Encrypt sensitive data
   - Implement data retention policy
   - Obtain user consent for logging

## Advanced Configuration

### Adjust WiFi Reconnect Behavior
Edit `iot/src/main.cpp` lines 29-33:
```cpp
WiFiManager::WiFiConfig wifiConfig = {
  .ssid = WIFI_SSID,
  .password = WIFI_PASSWORD,
  .reconnectInterval = 5000,      // Wait 5s between reconnect attempts
  .connectionTimeout = 15000,     // Give up after 15s per attempt
  .maxRetries = 5                 // Reset after 5 failed attempts
};
```

Higher values = slower reconnection but less network churn (good for stable networks)
Lower values = faster reconnection but more network overhead (good for unstable 4G)

### Custom Log Metadata
To add extra data to logs:
```cpp
char metadata[64] = "gps_locked=1,speed=0";
FirebaseLogger::logEvent(FirebaseLogger::EVENT_CAR_UNLOCKED,
                         FirebaseLogger::METHOD_NFC_PHASE_0,
                         metadata);
```

This appears in Firebase as:
```json
{
  "event": "CAR_UNLOCKED",
  "method": "NFC_PHASE_0",
  "timestamp": 1716856440000,
  "data": "gps_locked=1,speed=0"
}
```

## Support

If stuck, provide these details:
1. Serial Monitor output (first 30 lines after upload)
2. Firebase API Key first 10 characters (rest redacted)
3. WiFi SSID name (if not sensitive)
4. Error message from Serial Monitor
