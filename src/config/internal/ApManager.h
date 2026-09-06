#pragma once

#include <stdint.h>

// SoftAP lifecycle and client-count polling. Logs transitions; holds no HTTP or
// DNS knowledge.
class ApManager {
 public:
  static constexpr uint32_t kPollIntervalMs = 1000;

  bool begin(const char* ssid, uint8_t channel, uint8_t maxClients);

  // Call from loop(). Polls the station count once per kPollIntervalMs and logs
  // any change.
  void tick(uint32_t nowMs);

  bool up() const;
  uint8_t clients() const;
  const char* ssid() const;
  const char* ip() const;

 private:
  char ssid_[33] = {};
  char ip_[16] = {};
  bool up_ = false;
  uint8_t clients_ = 0;
  uint32_t lastPollMs_ = 0;
};
