#pragma once

#include <stdint.h>

// Turns a stream of (pressed, now) samples into a single event when the button
// has been held long enough.
//
// Pure and clock-free: the caller supplies the timestamp, so the 3-second
// behaviour is host-testable without a button or a board.
class HoldDetector {
 public:
  static constexpr uint32_t kHoldMs = 3000;
  // A release shorter than this is contact bounce and does not reset the hold.
  static constexpr uint32_t kDebounceMs = 30;

  // Returns true exactly once, on the sample where the press reaches kHoldMs.
  bool update(bool pressed, uint32_t nowMs);

  // Milliseconds held so far, 0 when not pressed. Drives the screen countdown.
  uint32_t heldMs(uint32_t nowMs) const;

  bool isHolding() const { return pressed_; }

 private:
  bool pressed_ = false;
  bool fired_ = false;
  uint32_t pressStartMs_ = 0;
  // When a release began, used to tell bounce from a real release.
  bool releasing_ = false;
  uint32_t releaseStartMs_ = 0;
};
