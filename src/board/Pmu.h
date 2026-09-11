#pragma once

#include <stdint.h>

#include "board/BoardConfig.h"

// The T-Beam's AXP2101 switches the rails the display and GPS sit on, so it
// has to be brought up before either. On a board without a PMU every method
// here does nothing and returns success -- the caller does not branch.
class Pmu {
 public:
  // Returns false only when a PMU is expected and did not respond.
  bool begin();

  bool present() const { return present_; }

 private:
  bool present_ = false;
};
