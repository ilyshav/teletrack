#include "ble/RaceChronoGps.h"

#include <math.h>

namespace {

// Reference clamps every rounded quantity to >= 0 before masking (max(0,
// round(...))) so a stray negative (e.g. altitude below -500 m) does not wrap
// through two's complement into the sign/coarse bit. Ported as-is.
long roundedFloorZero(float v) {
  long r = lroundf(v);
  return r < 0 ? 0 : r;
}

}  // namespace

uint32_t RaceChronoGps::dateAndHour(const GpsFix& fix) {
  return static_cast<uint32_t>((fix.year - 2000) * 8928) +
         static_cast<uint32_t>((fix.month - 1) * 744) +
         static_cast<uint32_t>((fix.day - 1) * 24) + fix.hour;
}

void RaceChronoGps::encodeMain(const GpsFix& fix, uint8_t syncBits, uint8_t out[20]) {
  // Ported verbatim from gpsLoop() in
  // docs/reference/racechrono/canbus-gps-device-main.ino, substituting
  // GpsFix fields for the Adafruit_GPS object.
  const int32_t timeSinceHourStart =
      static_cast<int32_t>(fix.minute) * 30000 + static_cast<int32_t>(fix.seconds) * 500 +
      static_cast<int32_t>(fix.millis) / 2;

  const int32_t latitude = fix.latE7;
  const int32_t longitude = fix.lonE7;

  // Hand over to the coarse equations only once the fine ones would overflow
  // the 15 bits they are masked to: (0x7FFF / 10) - 500 = 2776.7 m, and
  // 0x7FFF / 100 = 327.67 km/h. The comparison is strictly-greater-than,
  // matching the reference exactly: at the boundary value itself the fine
  // equation still fits in 15 bits, so switching there too would be wrong.
  const int32_t altitude = fix.altitudeM > 2776.7f
                                ? (roundedFloorZero(fix.altitudeM + 500.0f) & 0x7FFF) | 0x8000
                                : roundedFloorZero((fix.altitudeM + 500.0f) * 10.0f) & 0x7FFF;
  const int32_t speed = fix.speedKmh > 327.67f
                             ? (roundedFloorZero(fix.speedKmh * 10.0f) & 0x7FFF) | 0x8000
                             : roundedFloorZero(fix.speedKmh * 100.0f) & 0x7FFF;
  const int32_t bearing = roundedFloorZero(fix.bearingDeg * 100.0f);

  const uint8_t fixQuality = fix.fixQuality < 0x3 ? fix.fixQuality : 0x3;
  const uint8_t satellites = fix.satellites < 0x3F ? fix.satellites : 0x3F;

  out[0] = static_cast<uint8_t>(((syncBits & 0x7) << 5) | ((timeSinceHourStart >> 16) & 0x1F));
  out[1] = static_cast<uint8_t>(timeSinceHourStart >> 8);
  out[2] = static_cast<uint8_t>(timeSinceHourStart);
  out[3] = static_cast<uint8_t>((fixQuality << 6) | satellites);
  out[4] = static_cast<uint8_t>(latitude >> 24);
  out[5] = static_cast<uint8_t>(latitude >> 16);
  out[6] = static_cast<uint8_t>(latitude >> 8);
  out[7] = static_cast<uint8_t>(latitude >> 0);
  out[8] = static_cast<uint8_t>(longitude >> 24);
  out[9] = static_cast<uint8_t>(longitude >> 16);
  out[10] = static_cast<uint8_t>(longitude >> 8);
  out[11] = static_cast<uint8_t>(longitude >> 0);
  out[12] = static_cast<uint8_t>(altitude >> 8);
  out[13] = static_cast<uint8_t>(altitude);
  out[14] = static_cast<uint8_t>(speed >> 8);
  out[15] = static_cast<uint8_t>(speed);
  out[16] = static_cast<uint8_t>(bearing >> 8);
  out[17] = static_cast<uint8_t>(bearing);
  // 0x00..0xFE is 0.0..25.4; 0xFF is the documented invalid value. A u-blox
  // receiver with no fix reports pDOP 99.99, which would wrap to 232 and read
  // as a confident 23.2. Say "unknown" rather than something plausible.
  const long dop = lroundf(fix.hdop * 10.0f);
  out[18] = (dop < 0 || dop > 0xFE) ? 0xFF : static_cast<uint8_t>(dop);
  out[19] = 0xFF;  // VDOP: unimplemented, per the reference, which always
                    // sends the documented invalid value here.
}

void RaceChronoGps::encodeTime(const GpsFix& fix, uint8_t syncBits, uint8_t out[3]) {
  const uint32_t dh = dateAndHour(fix);
  out[0] = static_cast<uint8_t>(((syncBits & 0x7) << 5) | ((dh >> 16) & 0x1F));
  out[1] = static_cast<uint8_t>(dh >> 8);
  out[2] = static_cast<uint8_t>(dh);
}

uint8_t RaceChronoGps::updateSyncBits(const GpsFix& fix) {
  // Ported from gpsLoop(): `if (gpsPreviousDateAndHour != dateAndHour) {
  // gpsPreviousDateAndHour = dateAndHour; gpsSyncBits++; }`.
  const uint32_t dh = dateAndHour(fix);
  if (dh != previousDateAndHour_) {
    previousDateAndHour_ = dh;
    syncBits_ = static_cast<uint8_t>((syncBits_ + 1) & 0x7);
  }
  return syncBits_;
}
