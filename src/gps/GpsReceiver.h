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
  // The MAX-M10S reaches 25 Hz with a single constellation; 10 Hz is the
  // ceiling only when several run concurrently. Asking for more than
  // kMaxConcurrentRateHz makes begin() disable the others rather than refuse
  // the rate.
  static constexpr uint8_t kMaxRateHz = 25;
  static constexpr uint8_t kMaxConcurrentRateHz = 10;

  // The rate begin() will actually use for a requested one. Callers compare
  // against this rather than against the raw setting: comparing against a
  // value that gets adjusted makes a restart guard fire forever.
  static uint8_t effectiveRate(uint8_t requested);

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
