#pragma once

#include <stdint.h>

// A GPS fix in plain units, independent of both the receiver that produced it
// and the protocol that consumes it.
struct GpsFix {
  int32_t latE7 = 0;         // degrees * 10,000,000, signed
  int32_t lonE7 = 0;         // degrees * 10,000,000, signed
  float altitudeM = 0.0f;
  float speedKmh = 0.0f;
  float bearingDeg = 0.0f;
  float hdop = 0.0f;
  // RaceChrono's 2-bit quality: 0 none, 1 GPS, 2 differential.
  uint8_t fixQuality = 0;
  // The receiver's own fix type, kept for display: 0 none, 1 dead reckoning,
  // 2 = 2D, 3 = 3D, 4 GNSS+DR, 5 time only. RaceChrono never sees this.
  uint8_t fixType = 0;
  uint8_t satellites = 0;
  uint16_t year = 2000;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t seconds = 0;
  uint16_t millis = 0;
};
