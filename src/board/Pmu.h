#pragma once

#include <stdint.h>

#include "board/BoardConfig.h"

// What the AXP2101 reports about the cell. On a board with no PMU every
// field stays at its default, which reads as "no battery, no USB".
struct BatteryState {
  bool present = false;      // a cell is fitted
  bool usbPresent = false;   // VBUS is up
  bool charging = false;
  bool full = false;         // the charger reports done
  uint8_t percent = 0;       // 0 when unknown
  uint16_t milliVolts = 0;   // 0 when unknown
};

// The T-Beam's AXP2101 switches the rails the display and GPS sit on, so it
// has to be brought up before either. On a board without a PMU every method
// here does nothing and returns success -- the caller does not branch.
class Pmu {
 public:
  // How often the battery registers are re-read. Charge level moves over
  // minutes; loop() runs at roughly 1 kHz. Reading every pass would put five
  // I2C transactions on the hot path for a number that cannot have changed.
  static constexpr uint32_t kRefreshIntervalMs = 1000;

  // Returns false only when a PMU is expected and did not respond.
  bool begin();

  // Call from loop(). Re-reads the battery at most once per kRefreshIntervalMs.
  void tick(uint32_t nowMs);

  // The most recent reading. Cheap: no I2C, just the cache.
  BatteryState battery() const { return battery_; }

  bool present() const { return present_; }

 private:
  bool present_ = false;
  BatteryState battery_;
  uint32_t lastReadMs_ = 0;
  bool readOnce_ = false;
};
