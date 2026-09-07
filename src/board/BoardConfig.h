#pragma once

#include <stdint.h>

// Every constant that differs between the two supported boards, in one place.
// Selected by a build flag: -DBOARD_DEVKITC or -DBOARD_TBEAM.
//
// Values here come from vendor documentation and are confirmed on hardware at
// first flash. Each is one line to change.
namespace BoardConfig {

#if defined(BOARD_TBEAM)

inline constexpr const char* kBoardName = "T-Beam Supreme";
// Onboard User/Program button, the left of the three. Active low with an
// existing pull-up. Also a strapping pin: held low AT RESET the board enters
// download mode, which is how it gets flashed. Pressed while running it is
// just a button.
inline constexpr uint8_t kModeButtonPin = 0;
// Display, PMU and sensors share I2C bus 0.
inline constexpr uint8_t kI2cSda = 17;
inline constexpr uint8_t kI2cScl = 18;

#elif defined(BOARD_DEVKITC)

inline constexpr const char* kBoardName = "ESP32-S3-DevKitC-1";
// Hand-wired button to ground. Not 19 or 20 -- those are USB D-/D+ on the S3.
inline constexpr uint8_t kModeButtonPin = 39;
// No kI2cSda/kI2cScl here on purpose: this board has no I2C peripherals, so
// code referencing them should fail to compile rather than get a plausible
// default. Pmu.cpp branches on the board macro, so it never reaches them.

#else
#error "No board selected: define BOARD_DEVKITC or BOARD_TBEAM in build_flags"
#endif

}  // namespace BoardConfig
