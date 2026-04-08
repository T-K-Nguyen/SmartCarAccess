#pragma once

// Rollout flags for staged PKE deployment.
// These are compile-time defaults that can be overridden in platformio.ini.

#ifndef PKE_ROLLOUT_BACKGROUND_MODE
#define PKE_ROLLOUT_BACKGROUND_MODE 0
#endif

#ifndef PKE_ROLLOUT_FAST_TRANSACTION
#define PKE_ROLLOUT_FAST_TRANSACTION 0
#endif

#ifndef PKE_ROLLOUT_BONDING_ENFORCE
#define PKE_ROLLOUT_BONDING_ENFORCE 0
#endif

#ifndef PKE_ROLLOUT_RSSI_MONITOR_ONLY
#define PKE_ROLLOUT_RSSI_MONITOR_ONLY 1
#endif

#ifndef PKE_ROLLOUT_RSSI_THRESHOLD_DBM
#define PKE_ROLLOUT_RSSI_THRESHOLD_DBM -70
#endif

namespace BLERollout {
struct Flags {
  bool backgroundMode;
  bool fastTransaction;
  bool bondingEnforce;
  bool rssiMonitorOnly;
  int rssiThresholdDbm;
};

inline Flags flags() {
  Flags f;
  f.backgroundMode = (PKE_ROLLOUT_BACKGROUND_MODE != 0);
  f.fastTransaction = (PKE_ROLLOUT_FAST_TRANSACTION != 0);
  f.bondingEnforce = (PKE_ROLLOUT_BONDING_ENFORCE != 0);
  f.rssiMonitorOnly = (PKE_ROLLOUT_RSSI_MONITOR_ONLY != 0);
  f.rssiThresholdDbm = PKE_ROLLOUT_RSSI_THRESHOLD_DBM;
  return f;
}
}  // namespace BLERollout
