#pragma once

#include <stdint.h>

#include "radio/HoldDetector.h"

// GPIO4, wired to ground through a button, using the internal pull-up: the pin
// reads LOW while pressed. All timing lives in HoldDetector; this is only the
// pin read.
class ModeButton {
 public:
  static constexpr uint8_t kPin = 4;

  void begin();

  // Call from loop(). True exactly once per completed 3-second hold.
  bool tick(uint32_t nowMs);

  uint32_t heldMs(uint32_t nowMs) const { return detector_.heldMs(nowMs); }

 private:
  HoldDetector detector_;
};
