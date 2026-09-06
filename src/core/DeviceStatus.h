#pragma once

#include <stdint.h>

// A snapshot of what the device is doing right now. Produced by the config
// module, consumed by the display and by GET /api/status. Plain data — copying
// it is how the status crosses a task boundary safely.
struct DeviceStatus {
  char ssid[33] = {};  // 32-character SSID limit + NUL
  char ip[16] = {};    // "255.255.255.255" + NUL
  uint8_t clients = 0;
  uint32_t uptimeMs = 0;
  uint32_t freeHeap = 0;
  bool apUp = false;
};
