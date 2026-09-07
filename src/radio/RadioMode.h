#pragma once

#include <stdint.h>

// The two radios share one 2.4 GHz front end, so exactly one is up at a time.
enum class RadioMode : uint8_t {
  Ble,
  Wifi,
};

enum class ModeEvent : uint8_t {
  ButtonHeld,
};

// Decides which radio should be running. Pure: it owns no hardware and starts
// nothing. main.cpp reads switchPending() and does the actual teardown and
// startup, then reports back with switchComplete().
class ModeController {
 public:
  static constexpr RadioMode kBootMode = RadioMode::Ble;

  RadioMode mode() const { return mode_; }

  // True when the mode changed. A switch already in flight swallows the event:
  // radio teardown is not reentrant and a bounce must not start a second one.
  bool handle(ModeEvent event);

  bool switchPending() const { return pending_; }

  // Called once the caller has finished starting the new radio.
  void switchComplete() { pending_ = false; }

 private:
  RadioMode mode_ = kBootMode;
  bool pending_ = false;
};
