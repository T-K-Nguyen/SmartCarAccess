// WiFi Manager Implementation
#include "wifi_manager.h"
#include <time.h>

namespace WiFiManager {

namespace {
  WiFiConfig g_config = {};
  WiFiState g_state = WIFI_IDLE;
  uint32_t g_lastAttempt = 0;
  uint8_t g_retryCount = 0;
  uint32_t g_connectionStartTime = 0;
  
  const char* stateToString(WiFiState state) {
    switch (state) {
      case WIFI_IDLE: return "IDLE";
      case WIFI_CONNECTING: return "CONNECTING";
      case WIFI_CONNECTED: return "CONNECTED";
      case WIFI_DISCONNECTED: return "DISCONNECTED";
      case WIFI_FAILED: return "FAILED";
      default: return "UNKNOWN";
    }
  }
  
  void printWiFiStatus() {
    Serial.printf("[WiFi] State: %s, RSSI: %d dBm, Retries: %d/%d\n",
                  stateToString(g_state), WiFi.RSSI(), g_retryCount, g_config.maxRetries);
  }
  
  void attemptConnection() {
    // If idle, attempt connection immediately (first time)
    if (g_state == WIFI_IDLE) {
      Serial.printf("[WiFi] → Connecting to '%s' (attempt 1/%d)...\n", 
                    g_config.ssid, g_config.maxRetries);
      WiFi.mode(WIFI_STA);
      WiFi.setAutoConnect(false);
      WiFi.setAutoReconnect(false);
      
      // Use non-blocking connect with listen interval to reduce watchdog issues
      WiFi.begin(g_config.ssid, g_config.password);
      
      g_state = WIFI_CONNECTING;
      g_connectionStartTime = millis();
      g_lastAttempt = millis();
      return;
    }
    
    if (g_state == WIFI_CONNECTING) {
      // Check if connection is timing out
      if (millis() - g_connectionStartTime > g_config.connectionTimeout) {
        Serial.println("[WiFi] ✗ Connection timeout");
        g_state = WIFI_FAILED;
        g_retryCount++;
        g_lastAttempt = millis();
      }
      return;
    }
    
    // Check if enough time has passed since last attempt
    if (millis() - g_lastAttempt < g_config.reconnectInterval) {
      return;
    }
    
    // Check max retries
    if (g_retryCount >= g_config.maxRetries) {
      Serial.printf("[WiFi] ✗ Max retries (%d) exceeded, waiting before retry\n", g_config.maxRetries);
      g_state = WIFI_FAILED;
      g_retryCount = 0;
      g_lastAttempt = millis();
      return;
    }
    
    // Start new connection attempt
    Serial.printf("[WiFi] → Connecting to '%s' (attempt %d/%d)...\n", 
                  g_config.ssid, g_retryCount + 1, g_config.maxRetries);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_config.ssid, g_config.password);
    
    g_state = WIFI_CONNECTING;
    g_connectionStartTime = millis();
    g_lastAttempt = millis();
  }
}

void begin(const WiFiConfig& config) {
  g_config = config;
  g_state = WIFI_IDLE;
  g_retryCount = 0;
  g_lastAttempt = millis();
  
  Serial.println("[WiFi] ╔═══════════════════════════════════════════╗");
  Serial.println("[WiFi] ║  WiFi Manager with Auto-Reconnect        ║");
  Serial.println("[WiFi] ╚═══════════════════════════════════════════╝");
  Serial.printf("[WiFi] SSID: %s\n", g_config.ssid);
  Serial.printf("[WiFi] Reconnect interval: %ld ms\n", g_config.reconnectInterval);
  Serial.printf("[WiFi] Connection timeout: %ld ms\n", g_config.connectionTimeout);
  Serial.printf("[WiFi] Max retries: %d\n", g_config.maxRetries);
  
  // Set WiFi power save mode off for more stable connection
  WiFi.setSleep(false);
  
  // Attempt first connection
  attemptConnection();
}

void tick() {
  static unsigned long lastDebugPrint = 0;
  
  WiFiState currentWiFiStatus = (WiFi.status() == WL_CONNECTED) ? WIFI_CONNECTED : WIFI_DISCONNECTED;
  
  // Debug: Print status every 5 seconds
  if (millis() - lastDebugPrint > 5000) {
    Serial.printf("[WiFi DEBUG] State: %s, WL_Status: %d\n", stateToString(g_state), WiFi.status());
    lastDebugPrint = millis();
  }
  
  // Handle state transitions
  if (g_state == WIFI_CONNECTING && currentWiFiStatus == WIFI_CONNECTED) {
    g_state = WIFI_CONNECTED;
    g_retryCount = 0;
    Serial.printf("[WiFi] ✓ Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
    return;
  }
  
  if (g_state == WIFI_CONNECTED && currentWiFiStatus == WIFI_DISCONNECTED) {
    g_state = WIFI_DISCONNECTED;
    Serial.println("[WiFi] ✗ Connection lost, attempting reconnect...");
    g_lastAttempt = millis() - g_config.reconnectInterval;  // Attempt immediately
    return;
  }
  
  if (g_state == WIFI_FAILED) {
    attemptConnection();
    return;
  }
  
  if (g_state == WIFI_DISCONNECTED) {
    attemptConnection();
    return;
  }
  
  if (g_state == WIFI_CONNECTING) {
    attemptConnection();
  }
}

bool isConnected() {
  return g_state == WIFI_CONNECTED && WiFi.status() == WL_CONNECTED;
}

WiFiState getState() {
  return g_state;
}

int getRSSI() {
  if (isConnected()) {
    return WiFi.RSSI();
  }
  return -100;  // Return weak signal if not connected
}

String getIPAddress() {
  if (isConnected()) {
    return WiFi.localIP().toString();
  }
  return "0.0.0.0";
}

void reconnect() {
  g_retryCount = 0;
  g_lastAttempt = millis() - g_config.reconnectInterval;
  g_state = WIFI_DISCONNECTED;
}

uint8_t getRetryCount() {
  return g_retryCount;
}

void reset() {
  WiFi.disconnect(true);  // true = turn off WiFi radio
  g_state = WIFI_IDLE;
  g_retryCount = 0;
  g_lastAttempt = millis();
  Serial.println("[WiFi] WiFi reset");
}

}  // namespace WiFiManager
