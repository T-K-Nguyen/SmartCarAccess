#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>

namespace UwbUci {

enum class Mt : uint8_t {
  Data = 0,
  Command = 1,
  Response = 2,
  Notification = 3,
};

struct UciPacket {
  Mt mt;
  uint8_t gid;
  uint8_t oid;
  uint8_t pbf;
  std::vector<uint8_t> payload;
};

class UciUartLink {
 public:
  using PacketCallback = std::function<void(const UciPacket&)>;

  UciUartLink();

  bool begin(HardwareSerial& serial, int rxPin, int txPin, uint32_t baud);
  void poll();

  bool sendPacket(Mt mt, uint8_t gid, uint8_t oid, const std::vector<uint8_t>& payload, uint8_t pbf = 0);

  void setPacketCallback(PacketCallback cb);
  bool isReady() const;

 private:
  bool tryParseOnePacket();

  HardwareSerial* serial_;
  PacketCallback callback_;
  std::vector<uint8_t> rxBuffer_;
  bool ready_;
};

}  // namespace UwbUci
