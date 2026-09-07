#pragma once

#include <stdint.h>

#include "radio/HoldDetector.h"

// GPIO39, wired to ground through a button, using the internal pull-up: the pin
// reads LOW while pressed, so no external resistor is needed. All timing lives
// in HoldDetector; this is only the pin read.
//
// Not GPIO19 or GPIO20: those are USB D-/D+ on the S3 (USBPHY_DM_NUM/DP_NUM),
// and this board uses native USB for serial and flashing. Not 26-37 either:
// flash and the octal PSRAM claim those. GPIO39 is MTCK, free unless an
// external JTAG probe is attached.
class ModeButton {
 public:
  static constexpr uint8_t kPin = 39;

  void begin();

  // Call from loop(). True exactly once per completed 3-second hold.
  bool tick(uint32_t nowMs);

  uint32_t heldMs(uint32_t nowMs) const { return detector_.heldMs(nowMs); }

 private:
  HoldDetector detector_;
};
