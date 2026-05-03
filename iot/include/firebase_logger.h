// Firebase Logger - Send access logs to Firebase Realtime Database
#pragma once
#include <Arduino.h>

namespace FirebaseLogger {

// Configuration structure
struct Config {
  const char* databaseUrl;    // e.g., "https://your-project.firebaseio.com"
  const char* apiKey;         // Web API Key from Firebase Console
};

// Log event types
enum EventType {
  EVENT_UNKNOWN,
  EVENT_CAR_UNLOCKED,
  EVENT_CAR_LOCKED,
  EVENT_KEY_SHARED,
  EVENT_KEY_REVOKED
};

// Method used for authentication
enum AuthMethod {
  METHOD_UNKNOWN,
  METHOD_NFC_PHASE_0,
  METHOD_BLE_FAST,
  METHOD_UWB_RANGING
};

// Initialize Firebase logger
void begin(const Config& config);

// Check if Firebase is ready
bool isReady();

// Main loop function - call this periodically
void tick();

// Send car unlocked log asynchronously
void logUnlock(AuthMethod method = METHOD_UNKNOWN);

// Send car locked log
void logLock();

// Send key shared event
void logKeyShared(const char* friendUserId);

// Send key revoked event
void logKeyRevoked(const char* friendUserId);

// Generic log function
void logEvent(EventType eventType, AuthMethod method, const char* additionalData = nullptr);

// Get connection status
bool isConnected();

// Get last error message
const char* getLastError();

// Force retry if previous request failed
void retryLastRequest();

}  // namespace FirebaseLogger
