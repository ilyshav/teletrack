#pragma once

#include <stdint.h>

#include "can/BusStatus.h"
#include "core/GpsFix.h"
#include "radio/RadioMode.h"

// Which page the display is showing. A triple click on the mode button moves
// between them; the header is drawn on both.
enum class Screen : uint8_t {
  Gps,  // satellites, position, altitude, speed, UTC
  Can,  // bus health and the controller's own counters
};

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
  // Which page is on show.
  Screen screen = Screen::Gps;
  // CAN. canPresent is false when the controller never installed; canTransceiver
  // is the passive pin test from boot, and it is the only thing that separates a
  // genuinely quiet bus from wiring that was never right -- in listen-only mode
  // those look identical from the frame count alone.
  bool canPresent = false;
  bool canTransceiver = false;
  BusStatus canHealth = BusStatus::Starting;
  // Points at a string literal in flash, so copying this struct across a task
  // boundary copies a pointer that stays valid.
  const char* canState = "?";
  uint32_t canFramesPerSec = 0;
  uint32_t canLostPerSec = 0;
  uint32_t canRxErrors = 0;
  uint32_t canBusErrors = 0;
  uint16_t canIdsSeen = 0;
  uint32_t canExtendedFrames = 0;
};
