#pragma once

#include <stdint.h>

// What the button did. Nothing else about the pin escapes this module.
enum class ButtonEvent : uint8_t {
  None,
  Hold,         // held for kHoldMs, reported while still down
  TripleClick,  // three short presses inside kMultiClickWindowMs
};

// Turns a stream of (pressed, now) samples into button events.
//
// Pure and clock-free: the caller supplies the timestamp, so every threshold
// here is host-testable without a button or a board.
//
// Both gestures live on one pin because it is the only pin that can wake the
// chip -- see the sleep-mode design. They stay distinguishable because a hold
// is not a click: a press longer than kClickMaxMs resets the click sequence
// rather than extending it.
class HoldDetector {
 public:
  static constexpr uint32_t kHoldMs = 3000;
  // A release shorter than this is contact bounce and does not reset the hold.
  static constexpr uint32_t kDebounceMs = 30;
  // A press shorter than this counts as a click rather than a hold.
  static constexpr uint32_t kClickMaxMs = 500;
  // All the clicks of a sequence must land within this of the first.
  static constexpr uint32_t kMultiClickWindowMs = 1200;
  static constexpr uint8_t kClicksForSleep = 3;

  ButtonEvent update(bool pressed, uint32_t nowMs);

  // Milliseconds held so far, 0 when not pressed. Drives the screen countdown,
  // and keeps counting past kHoldMs.
  uint32_t heldMs(uint32_t nowMs) const;

  bool isHolding() const { return pressed_; }

 private:
  // Turns the release that just ended into a click, or discards it if the
  // press was too long to be one. Called from both places a release can be
  // recognised -- see the comment at its definition.
  ButtonEvent classifyRelease(uint32_t releasedAtMs);

  bool pressed_ = false;
  bool fired_ = false;
  uint32_t pressStartMs_ = 0;
  // When a release began, used to tell bounce from a real release.
  bool releasing_ = false;
  uint32_t releaseStartMs_ = 0;
  // How long the press that is currently being released lasted. Classifying it
  // has to wait until the release survives the debounce window.
  uint32_t lastPressMs_ = 0;
  uint8_t clicks_ = 0;
  uint32_t firstClickMs_ = 0;
};
