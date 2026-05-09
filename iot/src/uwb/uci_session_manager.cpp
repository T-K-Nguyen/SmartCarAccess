#include "uwb/uci_session_manager.h"
#include "uwb/uci_door_unlock.h"

namespace UwbUci {

namespace {

constexpr uint8_t kGidSession = 0x01;
constexpr uint8_t kGidRanging = 0x02;

constexpr uint8_t kOidSessionInit = 0x00;
constexpr uint8_t kOidSessionDeinit = 0x01;
constexpr uint8_t kOidSessionSetAppConfig = 0x03;

constexpr uint8_t kOidRangingStart = 0x00;
constexpr uint8_t kOidRangingStop = 0x01;

constexpr uint8_t kAppDeviceType = 0x00;
constexpr uint8_t kAppRangingRoundUsage = 0x01;
constexpr uint8_t kAppStsConfig = 0x02;
constexpr uint8_t kAppMultiNodeMode = 0x03;
constexpr uint8_t kAppChannelNumber = 0x04;
constexpr uint8_t kAppDeviceMacAddress = 0x06;
constexpr uint8_t kAppDstMacAddress = 0x07;
constexpr uint8_t kAppSlotDuration = 0x08;
constexpr uint8_t kAppRangingInterval = 0x09;
constexpr uint8_t kAppAoaResultReq = 0x0D;
constexpr uint8_t kAppDeviceRole = 0x11;
constexpr uint8_t kAppRframeConfig = 0x12;
constexpr uint8_t kAppRssiReporting = 0x13;
constexpr uint8_t kAppPreambleCodeIndex = 0x14;
constexpr uint8_t kAppSfdId = 0x15;
constexpr uint8_t kAppScheduleMode = 0x22;
constexpr uint8_t kAppSlotsPerRr = 0x1B;
constexpr uint8_t kAppHoppingMode = 0x2C;
constexpr uint8_t kAppResultReportConfig = 0x2E;
constexpr uint8_t kAppVendorId = 0x27;
constexpr uint8_t kAppStaticStsIv = 0x28;

constexpr uint8_t kSessionTypeRanging = 0x00;
constexpr uint8_t kStatusOk = 0x00;
constexpr double kNearFieldSaturationReuseThresholdM = 0.5;
constexpr double kAntennaOffsetM = 0.24;

}  // namespace

UciSessionManager::UciSessionManager(UciUartLink& link)
    : link_(link),
      waitingResponse_(false),
      expectedGid_(0),
      expectedOid_(0),
      responseReady_(false),
      responseStatus_(0xFF),
      responsePayload_(),
      activeSessionId_(0),
      activeSessionIdValid_(false),
      sessionActive_(false),
      activeCfg_(),
      rangingNotifCount_(0) {
  link_.setPacketCallback([this](const UciPacket& packet) { onPacket(packet); });
  lstm_ai_.begin();
}

void UciSessionManager::poll() {
  link_.poll();
}

uint32_t UciSessionManager::rangingNotificationCount() const {
  return rangingNotifCount_;
}

bool UciSessionManager::runOnce(const UciRunConfig& cfg) {
  if (sessionActive_) {
    Serial.println("[UCI] Existing session active, stopping it before restart");
    if (!stopActiveSession()) {
      Serial.println("[UCI] Failed to stop previous session");
      return false;
    }
  }

  rangingNotifCount_ = 0;
  activeSessionId_ = 0;
  activeSessionIdValid_ = false;
  first_reading_ = true;
  Serial.println("[UCI] ==== Run sequence begin ====");

  const bool initOk = commandSessionInit(cfg);
  if (!initOk) {
    Serial.println("[UCI] session_init failed");
    return false;
  }

  const bool cfgOk = commandSessionSetAppConfig(cfg);
  if (!cfgOk) {
    Serial.println("[UCI] session_set_app_config failed");
    commandSessionDeinit(cfg);
    return false;
  }

  const bool startOk = commandRangingStart(cfg);
  if (!startOk) {
    Serial.println("[UCI] ranging_start failed");
    commandSessionDeinit(cfg);
    return false;
  }

  activeCfg_ = cfg;
  sessionActive_ = true;
  Serial.printf("[UCI] ==== Session active. notif_count=%lu ====" "\n", static_cast<unsigned long>(rangingNotifCount_));
  return true;
}

bool UciSessionManager::stopActiveSession() {
  if (!sessionActive_) {
    Serial.println("[UCI] No active session to stop");
    return true;
  }

  Serial.printf("[UCI] ==== Stop sequence begin. notif_count=%lu ====" "\n",
                static_cast<unsigned long>(rangingNotifCount_));

  bool ok = true;
  if (!commandRangingStop(activeCfg_)) {
    Serial.println("[UCI] ranging_stop failed");
    ok = false;
  }
  if (!commandSessionDeinit(activeCfg_)) {
    Serial.println("[UCI] session_deinit failed");
    ok = false;
  }

  activeSessionId_ = 0;
  activeSessionIdValid_ = false;
  sessionActive_ = false;
  first_reading_ = true;
  Serial.printf("[UCI] ==== Stop sequence done. result=%s ====" "\n", ok ? "SUCCESS" : "FAIL");
  return ok;
}

bool UciSessionManager::isSessionActive() const {
  return sessionActive_;
}

void UciSessionManager::onPacket(const UciPacket& packet) {
  if (packet.mt == Mt::Response) {
    if (waitingResponse_ && packet.gid == expectedGid_ && packet.oid == expectedOid_) {
      responsePayload_ = packet.payload;
      responseStatus_ = packet.payload.empty() ? 0xFF : packet.payload[0];
      responseReady_ = true;
    }
    return;
  }

  if (packet.mt == Mt::Notification && packet.gid == kGidRanging && packet.oid == kOidRangingStart) {
    rangingNotifCount_++;
    Serial.printf("[UCI] RangingData notification #%lu payload_len=%u\n",
                  static_cast<unsigned long>(rangingNotifCount_),
                  static_cast<unsigned>(packet.payload.size()));
    // Dump payload hex for debugging distance parsing
    // Serial.print("[UCI] Ranging payload hex:");
    // for (size_t i = 0; i < packet.payload.size(); ++i) {
    //   Serial.printf(" %02X", packet.payload[i]);
    // }
    // Serial.println();
    
    // Parse measurement per FiRa/CCC UCI spec:
    // - payload[24] = number of measurements (n)
    // - for first measurement: status at payload[27], distance at payload[29:30] (uint16 little-endian, cm)
    if (packet.payload.size() >= 31) {
      const uint8_t num_meas = packet.payload[24];
      Serial.printf("[UCI] num_measurements=%u\n", static_cast<unsigned>(num_meas));
      if (num_meas > 0 && packet.payload.size() >= 31) {
        const uint8_t status = packet.payload[27];
        // Handle saturation / too-close before attempting to decode distance
        if (status == 0x1B && !first_reading_) {
          const double lastFilteredDistance = UwbDoorUnlock::getLastFilteredDistance();
          if (lastFilteredDistance > -1.0 && lastFilteredDistance < kNearFieldSaturationReuseThresholdM) {
            Serial.printf("[UCI] Saturation Error 0x1B detected. Reusing last filtered distance: %.2fm\n",
                          lastFilteredDistance);
            UwbDoorUnlock::handleRangingDistance(lastFilteredDistance);
            return;
          }
        }

        if (status == 0x00) {
          const size_t dist_off = 29;
          // Use unsigned decode then reinterpret as signed to avoid underflow issues
          uint16_t raw_cm_u = static_cast<uint16_t>(packet.payload[dist_off]) |
                              (static_cast<uint16_t>(packet.payload[dist_off + 1]) << 8);
          int16_t dist_cm = static_cast<int16_t>(raw_cm_u);

          double rawDistanceMeters = static_cast<double>(dist_cm) / 100.0;

          // Apply antenna offset (may produce slight negative values before sanity check)
          rawDistanceMeters += kAntennaOffsetM;
          
          



          // ======================================================================================================================
          // [GIẢ LẬP RELAY ATTACK] - CHỈ MỞ UNCOMMENT KHI THU THẬP LABEL 2
          // Bản chất Relay Attack làm trễ sóng, tạo ra các đỉnh nhiễu (Spike) về ToF.
          // Đoạn code này cứ mỗi 50 frame (~5 giây) sẽ tạo ra một đợt tấn công kéo dài 
          // 10 frame (~1 giây) bằng cách cộng dồn khoảng cách ảo.
          // =================================================================
          // static int attack_frame_counter = 0;
          // attack_frame_counter++;
          
          // if (attack_frame_counter % 50 > 40) {
          //     // Cộng thêm 8.0m khoảng cách ảo (độ trễ Relay)
          //     // Kết hợp hàm random() để tạo độ nhiễu rung lắc (Jitter) của trạm lặp
          //     rawDistanceMeters += 8.0 + (random(-20, 20) / 100.0); 
          //     Serial.println("[ATTACK] Đang bơm nhiễu Relay Attack (+8.0m) !!!");
          // }
          // ====================================================================================================================





          // Sanity check: allow slight negative due to offset, but drop extreme values
          if (rawDistanceMeters < -1.0 || rawDistanceMeters > 30.0) {
            Serial.printf("[UCI] Dropped out-of-bounds reading: %.2fm\n", rawDistanceMeters);
            return;
          }

          double filteredDistanceMeters = rawDistanceMeters;
          if (first_reading_) {
            uwbFilter_ = Kalman(0.05, 0.2, 1.0, rawDistanceMeters);
            first_reading_ = false;
          } else {
            filteredDistanceMeters = uwbFilter_.getFilteredValue(rawDistanceMeters);
          }

          const double residual = rawDistanceMeters - filteredDistanceMeters;
          Serial.printf("[UCI] Valid Distance: Raw=%.2fm, Filtered=%.2fm, Res=%.2fm (status=0x%02X)\n",
                        rawDistanceMeters, filteredDistanceMeters, residual, status);
          Serial.printf("[LSTM_DATA],%lu,%.2f,%.2f,%.2f\n",
                        static_cast<unsigned long>(millis()),
                        rawDistanceMeters,
                        filteredDistanceMeters,
                        residual);
          
          // Run LSTM inference to detect relay attacks
          float p_walk = 0.0f, p_loiter = 0.0f, p_attack = 0.0f;
          bool ai_ready = lstm_ai_.predict(static_cast<float>(filteredDistanceMeters),
                                            static_cast<float>(residual),
                                            p_walk, p_loiter, p_attack);
          
          if (ai_ready) {
            Serial.printf("[AI] Walk: %.2f | Loiter: %.2f | Attack: %.2f\n",
                          p_walk, p_loiter, p_attack);
            UwbDoorUnlock::handleRangingWithAI(filteredDistanceMeters, p_walk, p_loiter, p_attack);
          } else {
            Serial.printf("[AI] Window warm-up: %d/15 frames\n", static_cast<int>(lstm_ai_.getFrameCount()));
            // During warm-up, still process distance but don't make lock decisions
            UwbDoorUnlock::handleRangingDistance(filteredDistanceMeters);
          }
        } else if (status != 0x1B) {
          Serial.printf("[UCI] Ignoring measurement. Status error: 0x%02X\n", status);
        }
      }
    } else {
      Serial.println("[UCI] Payload too small for measurement parsing");
    }
  }
}

bool UciSessionManager::sendCommandWithRetry(
    uint8_t gid,
    uint8_t oid,
    const std::vector<uint8_t>& payload,
    uint32_t timeoutMs,
    uint8_t retries,
    std::vector<uint8_t>* outPayload,
    uint8_t* outStatus) {
  for (uint8_t attempt = 0; attempt <= retries; ++attempt) {
    waitingResponse_ = true;
    expectedGid_ = gid;
    expectedOid_ = oid;
    responseReady_ = false;
    responseStatus_ = 0xFF;
    responsePayload_.clear();

    // Log outgoing UCI command for debugging
    Serial.printf("[UCI] Sending cmd gid=0x%02X oid=0x%02X payload_len=%u attempt=%u\n", gid, oid, (unsigned)payload.size(), static_cast<unsigned>(attempt + 1));
    Serial.print("[UCI] Outgoing payload:");
    for (size_t i = 0; i < payload.size(); ++i) Serial.printf(" %02X", payload[i]);
    Serial.println();
    const bool sendOk = link_.sendPacket(Mt::Command, gid, oid, payload, 0);
    if (!sendOk) {
      waitingResponse_ = false;
      Serial.printf("[UCI] send failed gid=0x%02X oid=0x%02X attempt=%u\n", gid, oid, static_cast<unsigned>(attempt + 1));
      continue;
    }

    const uint32_t startMs = millis();
    while ((millis() - startMs) < timeoutMs) {
      poll();
      if (responseReady_) {
        waitingResponse_ = false;
        if (outPayload != nullptr) {
          *outPayload = responsePayload_;
        }
        if (outStatus != nullptr) {
          *outStatus = responseStatus_;
        }
        if (responseStatus_ == kStatusOk) {
          return true;
        }
        Serial.printf("[UCI] rsp bad status gid=0x%02X oid=0x%02X status=0x%02X attempt=%u\n",
                      gid,
                      oid,
                      responseStatus_,
                      static_cast<unsigned>(attempt + 1));
        break;
      }
      delay(2);
    }

    waitingResponse_ = false;
    if (!responseReady_) {
      Serial.printf("[UCI] rsp timeout gid=0x%02X oid=0x%02X attempt=%u\n", gid, oid, static_cast<unsigned>(attempt + 1));
    }
  }

  return false;
}

bool UciSessionManager::commandSessionInit(const UciRunConfig& cfg) {
  std::vector<uint8_t> payload;
  appendLe(payload, cfg.sessionId, 4);
  payload.push_back(kSessionTypeRanging);

  std::vector<uint8_t> rsp;
  uint8_t status = 0xFF;
  const bool ok = sendCommandWithRetry(kGidSession, kOidSessionInit, payload, 1200, 2, &rsp, &status);
  if (!ok) {
    return false;
  }

  if (rsp.size() >= 5) {
    activeSessionId_ = static_cast<uint32_t>(rsp[1]) |
                       (static_cast<uint32_t>(rsp[2]) << 8) |
                       (static_cast<uint32_t>(rsp[3]) << 16) |
                       (static_cast<uint32_t>(rsp[4]) << 24);
    activeSessionIdValid_ = true;
    Serial.printf("[UCI] session_init returned session_handle=%lu\n",
                  static_cast<unsigned long>(activeSessionId_));
  } else {
    activeSessionId_ = cfg.sessionId;
    activeSessionIdValid_ = true;
    Serial.printf("[UCI] session_init uses session_id=%lu (FiRa 1.x style)\n",
                  static_cast<unsigned long>(activeSessionId_));
  }

  return true;
}

bool UciSessionManager::commandSessionSetAppConfig(const UciRunConfig& cfg) {
  std::vector<uint8_t> payload;
  appendLe(payload, effectiveSessionId(), 4);

  std::vector<uint8_t> tlvs;
  uint8_t v1[8];

  v1[0] = cfg.controlee ? 0x00 : 0x01;
  appendTlv(tlvs, kAppDeviceType, v1, 1);

  v1[0] = cfg.controlee ? 0x00 : 0x01;
  appendTlv(tlvs, kAppDeviceRole, v1, 1);

  v1[0] = 0x00;
  appendTlv(tlvs, kAppMultiNodeMode, v1, 1);

  v1[0] = 0x02;
  appendTlv(tlvs, kAppRangingRoundUsage, v1, 1);

  v1[0] = cfg.channel;
  appendTlv(tlvs, kAppChannelNumber, v1, 1);

  v1[0] = cfg.scheduleMode;
  appendTlv(tlvs, kAppScheduleMode, v1, 1);

  appendLe(tlvs, kAppDeviceMacAddress, 1);
  appendLe(tlvs, 2, 1);
  appendLe(tlvs, cfg.localMac, 2);

  appendLe(tlvs, kAppDstMacAddress, 1);
  appendLe(tlvs, 2, 1);
  appendLe(tlvs, cfg.destMac, 2);

  appendLe(tlvs, kAppSlotDuration, 1);
  appendLe(tlvs, 2, 1);
  appendLe(tlvs, cfg.slotDuration, 2);

  appendLe(tlvs, kAppRangingInterval, 1);
  appendLe(tlvs, 4, 1);
  appendLe(tlvs, cfg.rangingDuration, 4);

  v1[0] = cfg.rframeConfig;
  appendTlv(tlvs, kAppRframeConfig, v1, 1);

  v1[0] = 1;
  appendTlv(tlvs, kAppRssiReporting, v1, 1);

  v1[0] = cfg.preambleIdx;
  appendTlv(tlvs, kAppPreambleCodeIndex, v1, 1);

  v1[0] = cfg.sfd;
  appendTlv(tlvs, kAppSfdId, v1, 1);

  v1[0] = cfg.slotsPerRr;
  appendTlv(tlvs, kAppSlotsPerRr, v1, 1);

  v1[0] = cfg.hoppingMode;
  appendTlv(tlvs, kAppHoppingMode, v1, 1);

  v1[0] = cfg.stsConfig;
  appendTlv(tlvs, kAppStsConfig, v1, 1);

  v1[0] = cfg.aoaReport;
  appendTlv(tlvs, kAppAoaResultReq, v1, 1);

  v1[0] = cfg.resultReportConfig;
  appendTlv(tlvs, kAppResultReportConfig, v1, 1);

  appendLe(tlvs, kAppVendorId, 1);
  appendLe(tlvs, 2, 1);
  appendLe(tlvs, cfg.vendorId, 2);

  appendLe(tlvs, kAppStaticStsIv, 1);
  appendLe(tlvs, 6, 1);
  tlvs.insert(tlvs.end(), cfg.staticStsIv, cfg.staticStsIv + 6);

  size_t count = 0;
  for (size_t i = 0; i + 1 < tlvs.size();) {
    const uint8_t len = tlvs[i + 1];
    i += static_cast<size_t>(2 + len);
    count++;
  }

  payload.push_back(static_cast<uint8_t>(count & 0xFF));
  payload.insert(payload.end(), tlvs.begin(), tlvs.end());

  uint8_t status = 0xFF;
  std::vector<uint8_t> rsp;
  return sendCommandWithRetry(kGidSession, kOidSessionSetAppConfig, payload, 2000, 2, &rsp, &status);
}

bool UciSessionManager::commandRangingStart(const UciRunConfig& cfg) {
  std::vector<uint8_t> payload;
  appendLe(payload, effectiveSessionId(), 4);
  uint8_t status = 0xFF;
  return sendCommandWithRetry(kGidRanging, kOidRangingStart, payload, 1200, 2, nullptr, &status);
}

bool UciSessionManager::commandRangingStop(const UciRunConfig& cfg) {
  std::vector<uint8_t> payload;
  appendLe(payload, effectiveSessionId(), 4);
  uint8_t status = 0xFF;
  return sendCommandWithRetry(kGidRanging, kOidRangingStop, payload, 1200, 1, nullptr, &status);
}

bool UciSessionManager::commandSessionDeinit(const UciRunConfig& cfg) {
  std::vector<uint8_t> payload;
  appendLe(payload, effectiveSessionId(), 4);
  uint8_t status = 0xFF;
  return sendCommandWithRetry(kGidSession, kOidSessionDeinit, payload, 1200, 1, nullptr, &status);
}

uint32_t UciSessionManager::effectiveSessionId() const {
  return activeSessionIdValid_ ? activeSessionId_ : 0;
}

bool UciSessionManager::waitForAtLeastOneRangingNotification(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    poll();
    if (rangingNotifCount_ > 0) {
      return true;
    }
    delay(5);
  }
  return false;
}

void UciSessionManager::appendLe(std::vector<uint8_t>& dst, uint64_t v, uint8_t len) {
  for (uint8_t i = 0; i < len; ++i) {
    dst.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void UciSessionManager::appendTlv(std::vector<uint8_t>& dst, uint8_t tag, const uint8_t* value, uint8_t len) {
  dst.push_back(tag);
  dst.push_back(len);
  for (uint8_t i = 0; i < len; ++i) {
    dst.push_back(value[i]);
  }
}

}  // namespace UwbUci
