#pragma once

#include <stdint.h>

#include "core/GpsFix.h"
#include "gps/UbxParser.h"

// The u-blox MAX-M10S on the T-Beam Supreme. Owns Serial1, finds the baud the
// module is currently using, configures it to emit UBX-NAV-PVT, and pumps the
// bytes into UbxParser.
//
// On boards with no receiver this compiles to a stub whose begin() returns
// false, exactly like Pmu.
class GpsReceiver {
 public:
  // The MAX-M10S tops out at 10 Hz with more than one constellation enabled.
  static constexpr uint8_t kMaxRateHz = 10;

  // Opens the UART, finds the module, and configures it for rateHz. Returns
  // false when nothing answers at any baud -- a dead receiver must not stop
  // the rest of the firmware.
  bool begin(uint8_t rateHz);

  // Call from loop(). Returns true when a new NAV-PVT was decoded.
  bool tick();

  const GpsFix& fix() const { return parser_.fix(); }
  bool timeValid() const { return parser_.timeValid(); }
  bool present() const { return present_; }
  uint8_t rateHz() const { return rateHz_; }

 private:
  UbxParser parser_;
  bool present_ = false;
  uint8_t rateHz_ = 0;
  uint32_t frames_ = 0;
};
