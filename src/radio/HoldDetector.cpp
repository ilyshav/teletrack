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
        // A real new press. If a release was still awaiting classification --
        // loop() never sampled while the button was up, which a single 25 ms
        // display redraw is enough to cause -- classify it now. Without this
        // the click is silently dropped, three deliberate clicks register two,
        // and the gesture simply does nothing.
        ButtonEvent pending = ButtonEvent::None;
        if (releasing_) {
          // Dated when the release would have been confirmed, not now, so the
          // multi-click window measures real time rather than sampling luck.
          pending = classifyRelease(releaseStartMs_ + kDebounceMs);
        }
        pressed_ = true;
        fired_ = false;
        pressStartMs_ = nowMs;
        releasing_ = false;
        if (pending != ButtonEvent::None) {
          return pending;
        }
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
    return classifyRelease(nowMs);
  }
  return ButtonEvent::None;
}

// Reachable two ways: a sample taken while the button is up and the debounce
// window has passed, or the next press arriving after that window with the
// release never having been sampled at all. Both are real releases and both
// must count, which is why this is not inlined into one of them.
ButtonEvent HoldDetector::classifyRelease(uint32_t releasedAtMs) {
  releasing_ = false;
  fired_ = false;

  if (lastPressMs_ >= kClickMaxMs) {
    clicks_ = 0;  // that was a hold, not a click
    return ButtonEvent::None;
  }

  if (clicks_ == 0 || (releasedAtMs - firstClickMs_) > kMultiClickWindowMs) {
    clicks_ = 1;
    firstClickMs_ = releasedAtMs;
  } else {
    ++clicks_;
  }

  if (clicks_ >= kClicksForSleep) {
    clicks_ = 0;
    return ButtonEvent::TripleClick;
  }
  return ButtonEvent::None;
}

uint32_t HoldDetector::heldMs(uint32_t nowMs) const {
  if (!pressed_) {
    return 0;
  }
  return nowMs - pressStartMs_;
}
