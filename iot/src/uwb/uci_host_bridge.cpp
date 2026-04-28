#include "uwb/uci_host_bridge.h"

#include <Arduino.h>

namespace UwbUciHost {

namespace {

UwbUci::UciSessionManager* g_manager = nullptr;
bool g_hasCachedConfig = false;
bool g_pendingStart = false;
bool g_pendingStop = false;
bool g_busy = false;
UwbUci::UciRunConfig g_pendingCfg;

}  // namespace

void init(UwbUci::UciSessionManager* manager) {
  g_manager = manager;
  g_hasCachedConfig = false;
  g_pendingStart = false;
  g_pendingStop = false;
  g_busy = false;
}

bool submitBleOob(const uint8_t* payload, size_t len, const char** err) {
  if (err) *err = nullptr;
  if (!g_manager) {
    if (err) *err = "uci_not_ready";
    return false;
  }
  if (g_busy) {
    if (err) *err = "uci_busy";
    return false;
  }

  UwbUci::UciOobPayloadV1 oob;
  const char* parseErr = nullptr;
  if (!UwbUci::parseOobPayloadV1(payload, len, &oob, &parseErr)) {
    if (err) *err = parseErr;
    return false;
  }

  UwbUci::mapOobToRunConfig(oob, &g_pendingCfg);
  g_hasCachedConfig = true;
  g_pendingStart = false;

  Serial.printf("[UCI][BLE-OOB] cached sid=%lu local=0x%04X dest=0x%04X ch=%u\n",
                static_cast<unsigned long>(g_pendingCfg.sessionId),
                g_pendingCfg.localMac,
                g_pendingCfg.destMac,
                g_pendingCfg.channel);
  return true;
}

bool requestStart(const char** err) {
  if (err) *err = nullptr;
  if (!g_manager) {
    if (err) *err = "uci_not_ready";
    return false;
  }
  if (g_busy) {
    if (err) *err = "uci_busy";
    return false;
  }
  if (!g_hasCachedConfig) {
    if (err) *err = "no_cached_oob";
    return false;
  }
  if (g_pendingStart) {
    if (err) *err = "start_already_pending";
    return false;
  }

  g_pendingStart = true;
  Serial.printf("[UCI][BLE-OOB] start requested sid=%lu local=0x%04X dest=0x%04X\n",
                static_cast<unsigned long>(g_pendingCfg.sessionId),
                g_pendingCfg.localMac,
                g_pendingCfg.destMac);
  return true;
}

bool requestStop(const char** err) {
  if (err) *err = nullptr;
  if (!g_manager) {
    if (err) *err = "uci_not_ready";
    return false;
  }
  if (g_busy) {
    if (err) *err = "uci_busy";
    return false;
  }
  if (!g_manager->isSessionActive()) {
    if (err) *err = "no_active_session";
    return false;
  }

  if (g_pendingStop) {
    if (err) *err = "stop_already_pending";
    return false;
  }

  g_pendingStop = true;
  Serial.println("[UCI][BLE-OOB] stop requested");
  return true;
}

bool hasCachedConfig() {
  return g_hasCachedConfig;
}

bool isBusy() {
  return g_busy;
}

bool hasPending() {
  return g_pendingStart || g_pendingStop;
}

void tick() {
  if (!g_manager || g_busy) {
    return;
  }

  if (!g_pendingStart && !g_pendingStop) {
    return;
  }

  g_busy = true;
  if (g_pendingStop) {
    g_pendingStop = false;
    Serial.println("[UCI][BLE-OOB] stop executing");
    const bool ok = g_manager->stopActiveSession();
    Serial.printf("[UCI][BLE-OOB] stop result=%s\n", ok ? "SUCCESS" : "FAIL");
  } else if (g_pendingStart) {
    g_pendingStart = false;
    Serial.printf("[UCI][BLE-OOB] start sid=%lu local=0x%04X dest=0x%04X\n",
                  static_cast<unsigned long>(g_pendingCfg.sessionId),
                  g_pendingCfg.localMac,
                  g_pendingCfg.destMac);
    const bool ok = g_manager->runOnce(g_pendingCfg);
    Serial.printf("[UCI][BLE-OOB] result=%s\n", ok ? "SUCCESS" : "FAIL");
  }

  g_busy = false;
}

}  // namespace UwbUciHost
