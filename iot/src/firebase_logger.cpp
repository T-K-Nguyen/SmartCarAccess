// Firebase Logger Implementation
#include "firebase_logger.h"
#include "wifi_manager.h"
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>

namespace FirebaseLogger {

namespace {
  Config g_config = {};
  bool g_initialized = false;
  char g_lastError[128] = {0};
  uint32_t g_lastLogTime = 0;
  bool g_requestPending = false;
  
  struct PendingRequest {
    EventType eventType;
    AuthMethod authMethod;
    char additionalData[64];
  } g_pendingRequest = {};
  
  const char* eventTypeToString(EventType type) {
    switch (type) {
      case EVENT_CAR_UNLOCKED: return "CAR_UNLOCKED";
      case EVENT_CAR_LOCKED: return "CAR_LOCKED";
      case EVENT_KEY_SHARED: return "KEY_SHARED";
      case EVENT_KEY_REVOKED: return "KEY_REVOKED";
      default: return "UNKNOWN";
    }
  }
  
  const char* methodToString(AuthMethod method) {
    switch (method) {
      case METHOD_NFC_PHASE_0: return "NFC_PHASE_0";
      case METHOD_BLE_FAST: return "BLE_FAST";
      case METHOD_UWB_RANGING: return "UWB_RANGING";
      default: return "UNKNOWN";
    }
  }
  
  void setError(const char* error) {
    strncpy(g_lastError, error, sizeof(g_lastError) - 1);
    g_lastError[sizeof(g_lastError) - 1] = '\0';
    Serial.printf("[Firebase] ✗ Error: %s\n", g_lastError);
  }
  
  bool sendLogToDatabase(EventType eventType, AuthMethod authMethod, const char* additionalData) {
    if (!WiFiManager::isConnected()) {
      setError("WiFi not connected");
      return false;
    }
    
    // Create JSON payload
    JsonDocument doc;
    doc["event"] = eventTypeToString(eventType);
    doc["method"] = methodToString(authMethod);
    doc["timestamp"] = "{\"_placeholder\":true,\"type\":\"server_timestamp\"}";  // Firebase server timestamp
    
    if (additionalData && strlen(additionalData) > 0) {
      doc["data"] = additionalData;
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Build Firebase Realtime Database URL
    // Format: https://your-project.firebaseio.com/access_logs.json?key=YOUR_API_KEY
    String url = String(g_config.databaseUrl);
    if (!url.endsWith("/")) {
      url += "/";
    }
    url += "access_logs.json?auth=";
    url += g_config.apiKey;
    
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    Serial.printf("[Firebase] → POST %s\n", url.c_str());
    Serial.printf("[Firebase] Payload: %s\n", jsonString.c_str());
    
    int httpCode = http.POST(jsonString);
    String response = http.getString();
    http.end();
    
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      Serial.printf("[Firebase] ✓ Log sent successfully (HTTP %d)\n", httpCode);
      Serial.printf("[Firebase] Response: %s\n", response.c_str());
      return true;
    } else {
      String error = "HTTP " + String(httpCode) + ": " + response;
      setError(error.c_str());
      return false;
    }
  }
}

void begin(const Config& config) {
  g_config = config;
  g_initialized = true;
  g_requestPending = false;
  
  Serial.println("[Firebase] ╔═══════════════════════════════════════════╗");
  Serial.println("[Firebase] ║  Firebase Logger - Access Logs            ║");
  Serial.println("[Firebase] ╚═══════════════════════════════════════════╝");
  Serial.printf("[Firebase] Database URL: %s\n", g_config.databaseUrl);
  Serial.printf("[Firebase] API Key: %s...\n", String(g_config.apiKey).substring(0, 10).c_str());
  Serial.println("[Firebase] Waiting for WiFi connection...");
}

void tick() {
  // Retry pending request if WiFi comes back online
  if (g_requestPending && WiFiManager::isConnected()) {
    Serial.println("[Firebase] → Retrying pending request...");
    if (sendLogToDatabase(g_pendingRequest.eventType, g_pendingRequest.authMethod, g_pendingRequest.additionalData)) {
      g_requestPending = false;
    }
  }
}

bool isReady() {
  return g_initialized && WiFiManager::isConnected();
}

bool isConnected() {
  return WiFiManager::isConnected();
}

void logUnlock(AuthMethod method) {
  logEvent(EVENT_CAR_UNLOCKED, method);
}

void logLock() {
  logEvent(EVENT_CAR_LOCKED, METHOD_UNKNOWN);
}

void logKeyShared(const char* friendUserId) {
  logEvent(EVENT_KEY_SHARED, METHOD_UNKNOWN, friendUserId);
}

void logKeyRevoked(const char* friendUserId) {
  logEvent(EVENT_KEY_REVOKED, METHOD_UNKNOWN, friendUserId);
}

void logEvent(EventType eventType, AuthMethod method, const char* additionalData) {
  if (!g_initialized) {
    setError("Firebase not initialized");
    return;
  }
  
  if (!WiFiManager::isConnected()) {
    // Queue the request for retry
    g_pendingRequest.eventType = eventType;
    g_pendingRequest.authMethod = method;
    if (additionalData) {
      strncpy(g_pendingRequest.additionalData, additionalData, sizeof(g_pendingRequest.additionalData) - 1);
    }
    g_requestPending = true;
    setError("WiFi not connected, request queued");
    return;
  }
  
  // Rate limit: don't send logs too frequently (min 100ms between logs)
  uint32_t now = millis();
  if (now - g_lastLogTime < 100) {
    Serial.println("[Firebase] ⚠ Rate limiting - log queued");
    return;
  }
  g_lastLogTime = now;
  
  if (sendLogToDatabase(eventType, method, additionalData)) {
    g_requestPending = false;
  } else {
    // Queue for retry
    g_pendingRequest.eventType = eventType;
    g_pendingRequest.authMethod = method;
    if (additionalData) {
      strncpy(g_pendingRequest.additionalData, additionalData, sizeof(g_pendingRequest.additionalData) - 1);
    }
    g_requestPending = true;
  }
}

const char* getLastError() {
  return g_lastError;
}

void retryLastRequest() {
  if (g_requestPending) {
    tick();
  }
}

}  // namespace FirebaseLogger
