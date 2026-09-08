#pragma once

#include <stdint.h>

#include "core/GpsFix.h"
#include "radio/RadioMode.h"

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
  RadioMode mode = RadioMode::Ble;
  bool bleConnected = false;
  uint32_t kbPerSec = 0;
  uint32_t dropped = 0;
  // Non-zero while the mode button is held; drives the countdown.
  uint32_t holdMs = 0;
  // GPS. gpsPresent is false when no receiver answered at boot, which the
  // display shows as "NO GPS" rather than a satellite count of zero.
  bool gpsPresent = false;
  bool gpsTimeValid = false;
  GpsFix gpsFix;
  // Battery, from the AXP2101. batteryPresent false with batteryUsbPresent
  // true is a board running on USB with no cell fitted -- which is not the
  // same as a flat one, and 0% cannot express the difference.
  bool batteryPresent = false;
  bool batteryUsbPresent = false;
  bool batteryCharging = false;
  bool batteryFull = false;
  uint8_t batteryPercent = 0;
  uint16_t batteryMilliVolts = 0;
};
