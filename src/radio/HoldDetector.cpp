#include "radio/HoldDetector.h"

ButtonEvent HoldDetector::update(bool pressed, uint32_t nowMs) {
  if (pressed) {
    if (!pressed_) {
      if (releasing_ && (nowMs - releaseStartMs_) < kDebounceMs) {
        // Bounce: contact came back inside the debounce window, so the press
        // never really ended. Do not restart the timer, and do not let it
        // count as a click -- three chattering contacts must not read as a
        // deliberate triple click.
        pressed_ = true;
        releasing_ = false;
      } else {
        pressed_ = true;
        fired_ = false;
        pressStartMs_ = nowMs;
        releasing_ = false;
      }
    }

    if (!fired_ && (nowMs - pressStartMs_) >= kHoldMs) {
      fired_ = true;
      // A hold ends any click sequence in progress: the two gestures are
      // alternatives, not stages of one another.
      clicks_ = 0;
      return ButtonEvent::Hold;
    }
    return ButtonEvent::None;
  }

  // Not pressed.
  if (pressed_) {
    pressed_ = false;
    releasing_ = true;
    releaseStartMs_ = nowMs;
    lastPressMs_ = nowMs - pressStartMs_;
    return ButtonEvent::None;
  }

  if (releasing_ && (nowMs - releaseStartMs_) >= kDebounceMs) {
    // The release outlasted the debounce window, so it was real and the press
    // it ended can finally be classified.
    releasing_ = false;
    fired_ = false;

    if (lastPressMs_ >= kClickMaxMs) {
      clicks_ = 0;  // that was a hold, not a click
      return ButtonEvent::None;
    }

    if (clicks_ == 0 || (nowMs - firstClickMs_) > kMultiClickWindowMs) {
      clicks_ = 1;
      firstClickMs_ = nowMs;
    } else {
      ++clicks_;
    }

    if (clicks_ >= kClicksForSleep) {
      clicks_ = 0;
      return ButtonEvent::TripleClick;
    }
  }
  return ButtonEvent::None;
}

uint32_t HoldDetector::heldMs(uint32_t nowMs) const {
  if (!pressed_) {
    return 0;
  }
  return nowMs - pressStartMs_;
}
