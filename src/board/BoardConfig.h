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
// A boot scan of 17/18 found the display and the BME280 but no AXP2101, so
// the PMU is on a second bus. These are the candidate pins; the boot scan
// reports what is actually there.
inline constexpr uint8_t kPmuSda = 42;
inline constexpr uint8_t kPmuScl = 41;

// u-blox MAX-M10S UART, from LilyGO's own board support for this board.
// Named from the ESP32's point of view: kGpsRxPin receives the module's TX.
inline constexpr uint8_t kGpsRxPin = 9;
inline constexpr uint8_t kGpsTxPin = 8;
// GPIO 7 is GPS_EN and GPIO 6 is PPS. LilyGO's own code drives neither, so
// neither is defined here. If the receiver is silent at every baud with
// ALDO4 confirmed on, GPIO 7 is the first thing to try.

// SN65HVD230 transceiver. Unclaimed by LilyGO's map for this board, not
// strapping pins, not USB, not flash. NOT 17 and 18 -- those are this board's
// I2C bus and the display lives on them. Named from the ESP32's point of view:
// kCanTxPin drives the transceiver's TX input, which is never asserted --
// the controller runs listen-only and the peripheral merely requires a pin.
inline constexpr uint8_t kCanTxPin = 15;
inline constexpr uint8_t kCanRxPin = 16;

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
