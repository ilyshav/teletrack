#include "radio/HoldDetector.h"

bool HoldDetector::update(bool pressed, uint32_t nowMs) {
  if (pressed) {
    if (!pressed_) {
      // Check if this is a bounce (contact restored within debounce window)
      if (releasing_ && (nowMs - releaseStartMs_) < kDebounceMs) {
        // Bounce: contact came back within the debounce window.
        // The hold never broke, so don't restart the timer.
        pressed_ = true;
        releasing_ = false;
      } else {
        // New press: first contact or genuine new press after real release.
        pressed_ = true;
        fired_ = false;
        pressStartMs_ = nowMs;
        releasing_ = false;
      }
    }

    if (!fired_ && (nowMs - pressStartMs_) >= kHoldMs) {
      fired_ = true;
      return true;
    }
    return false;
  }

  // Not pressed.
  if (pressed_) {
    // Immediately mark as released, but track timing to detect bounces.
    pressed_ = false;
    releasing_ = true;
    releaseStartMs_ = nowMs;
  } else if (releasing_ && (nowMs - releaseStartMs_) >= kDebounceMs) {
    // Released for longer than bounce: clear the release tracking.
    releasing_ = false;
    fired_ = false;
  }
  return false;
}

uint32_t HoldDetector::heldMs(uint32_t nowMs) const {
  if (!pressed_) {
    return 0;
  }
  return nowMs - pressStartMs_;
}
