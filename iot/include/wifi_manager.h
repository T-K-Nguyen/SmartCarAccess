// WiFi Manager with Auto-Reconnect for ESP32-S3
#pragma once
#include <WiFi.h>
#include <Arduino.h>

namespace WiFiManager {

// WiFi connection states
enum WiFiState {
  WIFI_IDLE,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  WIFI_DISCONNECTED,
  WIFI_FAILED
};

// Configuration structure
struct WiFiConfig {
  const char* ssid;
  const char* password;
  uint32_t reconnectInterval;  // ms between reconnect attempts
  uint32_t connectionTimeout;  // ms to wait for connection
  uint8_t maxRetries;
};

// Initialize WiFi manager
void begin(const WiFiConfig& config);

// Main loop function - call this periodically
void tick();

// Check if WiFi is connected
bool isConnected();

// Get current WiFi state
WiFiState getState();

// Get RSSI (signal strength)
int getRSSI();

// Get IP address as string
String getIPAddress();

// Manual reconnect attempt
void reconnect();

// Get retry count
uint8_t getRetryCount();

// Force disconnect and reset
void reset();

}  // namespace WiFiManager
