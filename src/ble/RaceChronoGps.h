#pragma once

#include <stdint.h>

#include "core/GpsFix.h"

// Encodes GpsFix into RaceChrono's BLE DIY GPS characteristics (UUIDs 0x0003
// and 0x0004 of service 0x1FF8; see docs/reference/racechrono/PROTOCOL.md).
//
// Pure: no BLE, no hardware, no floating clock. The bit manipulation is
// ported from the reference implementation's gpsLoop()
// (docs/reference/racechrono/canbus-gps-device-main.ino), not reconstructed
// from the spec prose -- the fine/coarse switchover for altitude and speed,
// the sync-bit rule, and the byte order are all easy to get subtly wrong
// from prose alone.
class RaceChronoGps {
 public:
  // The 21-bit value (widened to uint32_t; top 11 bits always 0) whose change
  // drives the sync bit counter: (year - 2000) * 8928 + (month - 1) * 744 +
  // (day - 1) * 24 + hour.
  static uint32_t dateAndHour(const GpsFix& fix);

  // Fills the 20-byte GPS main characteristic (UUID 0x0003) payload.
  static void encodeMain(const GpsFix& fix, uint8_t syncBits, uint8_t out[20]);

  // Fills the 3-byte GPS time characteristic (UUID 0x0004) payload.
  static void encodeTime(const GpsFix& fix, uint8_t syncBits, uint8_t out[3]);

  // Tracks the sync bit counter across successive fixes: returns the current
  // 3-bit sync value, incrementing it only when dateAndHour(fix) differs from
  // the previous call. Ported from the reference's gpsPreviousDateAndHour /
  // gpsSyncBits globals, including their starting point of 0: a fix at
  // year 2000/month 1/day 1/hour 0 (dateAndHour == 0) is indistinguishable
  // from "no previous fix yet" and so does not bump the counter either.
  uint8_t updateSyncBits(const GpsFix& fix);

 private:
  uint32_t previousDateAndHour_ = 0;
  uint8_t syncBits_ = 0;
};
