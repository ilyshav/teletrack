#include "ui/OledDisplay.h"

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>

#include "board/BoardConfig.h"
#include "core/Log.h"
#include "radio/HoldDetector.h"

namespace {

bool headerDiffers(const DeviceStatus& a, const DeviceStatus& b) {
  // Only what is actually drawn. Uptime, the client count and the drop count
  // are not in the header, so comparing them would repaint for nothing; a field
  // that IS drawn and omitted here freezes on a stale value.
  return a.mode != b.mode || (a.holdMs / 100u) != (b.holdMs / 100u) ||
         a.gpsPresent != b.gpsPresent || a.gpsTimeValid != b.gpsTimeValid ||
         a.gpsFix.satellites != b.gpsFix.satellites ||
         a.gpsFix.fixType != b.gpsFix.fixType ||
         a.gpsFix.fixQuality != b.gpsFix.fixQuality ||
         a.gpsFix.latE7 != b.gpsFix.latE7 || a.gpsFix.lonE7 != b.gpsFix.lonE7 ||
         a.gpsFix.altitudeM != b.gpsFix.altitudeM ||
         a.gpsFix.speedKmh != b.gpsFix.speedKmh ||
         a.gpsFix.hdop != b.gpsFix.hdop ||
         a.gpsFix.seconds != b.gpsFix.seconds;
}

}  // namespace

bool OledDisplay::begin() {
  // On T-Beam Supreme V3 the QMC6310N magnetometer sits at 0x3C, the panel's
  // usual address, and the panel moves to 0x3D. Both then ACK, so an address
  // probe alone cannot tell them apart -- but if BOTH answer, 0x3C is the
  // magnetometer. Writing frames to it succeeds silently and lights nothing.
  // See meshcore-dev/MeshCore#2609.
  Wire.begin(BoardConfig::kI2cSda, BoardConfig::kI2cScl);
  auto responds = [](uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
  };
  const bool primary = responds(kAddrPrimary);
  const bool alternate = responds(kAddrAlternate);
  const uint8_t addr = (primary && alternate) ? kAddrAlternate
                       : primary             ? kAddrPrimary
                                             : kAddrAlternate;
  // The boot scan already brought Wire up on these pins, and u8g2's begin()
  // initialises it again. Re-initialising the ESP32 I2C driver underneath
  // itself wedges the bus -- begin() never returns, so nothing after this
  // point in setup() runs. Release it and let u8g2 own the bus.
  Wire.end();

  u8g2_.setBusClock(400000);
  u8g2_.setI2CAddress(addr << 1);  // u8g2 wants the address pre-shifted
  if (!u8g2_.begin()) {
    return false;
  }
  Log::info("display", "SH1106 at 0x%02X", addr);
  u8g2_.setFont(u8g2_font_5x8_tr);  // 6x8 cell, 21 columns across 128 px
  u8g2_.clearBuffer();
  u8g2_.sendBuffer();
  ready_ = true;
  return true;
}

void OledDisplay::tick(uint32_t nowMs, const DeviceStatus& status) {
  if (!ready_) {
    return;
  }
  if (nowMs - lastDrawMs_ < kMinRedrawIntervalMs) {
    return;
  }
  lastDrawMs_ = nowMs;

  if (drawnOnce_ && !headerDiffers(status, drawnStatus_)) {
    return;
  }

  draw(status);
  drawnStatus_ = status;
  drawnOnce_ = true;
}

void OledDisplay::draw(const DeviceStatus& status) {
  // A monochrome panel has one framebuffer and no partial-update trick worth
  // having at this size, so the whole thing is rebuilt and sent each frame.
  // 1 KB over I2C at 400 kHz is about 25 ms, inside the 100 ms budget.
  u8g2_.clearBuffer();

  // Which radio is running, and nothing about who is connected to it. The
  // client and drop counts are in /api/status, which is where they are read.
  char right[kCols + 1];
  // Only while the countdown is actually running. heldMs() keeps counting for
  // as long as the button is down -- fired_ stops the switch repeating, not
  // the timer -- so past kHoldMs the switch has already happened and the new
  // radio is the useful thing to show. Bounding it here also keeps the
  // unsigned subtraction below from wrapping to ~4.29 billion and printing
  // "HOLD 4294966s", which is 13 columns on a 21-column panel.
  if (status.holdMs > 0 && status.holdMs < HoldDetector::kHoldMs) {
    // The only feedback that a three-second hold is registering at all.
    // Ceiling divide so it counts down 3, 2, 1 rather than 4, 3, 2.
    snprintf(right, sizeof(right), "HOLD %lus",
             static_cast<unsigned long>(
                 (HoldDetector::kHoldMs - status.holdMs + 999u) / 1000u));
  } else {
    snprintf(right, sizeof(right), "%s",
             status.mode == RadioMode::Wifi ? "AP" : "BLE");
  }

  // One inverse-video row, right-aligned. The left of the bar is free now that
  // the battery readout is gone.
  u8g2_.drawBox(0, 0, 128, kHeaderRows * kRowHeight);
  u8g2_.setDrawColor(0);
  u8g2_.drawStr(128 - 1 - u8g2_.getStrWidth(right), kRowHeight - 1, right);
  u8g2_.setDrawColor(1);

  // Row 1: satellite count and fix state. The count is the number that
  // answers "is this thing working yet", so it is always on the left.
  char line[kCols + 1];
  const GpsFix& fix = status.gpsFix;

  char sats[12];
  if (!status.gpsPresent) {
    snprintf(sats, sizeof(sats), "SATS --");
  } else {
    snprintf(sats, sizeof(sats), "SATS %02u", static_cast<unsigned>(fix.satellites));
  }

  const char* state = !status.gpsPresent ? "NO GPS"
                      : fix.fixType == 3 ? "3D FIX"
                      : fix.fixType == 2 ? "2D FIX"
                                         : "NO FIX";

  // Status row n, counting from 0 immediately below the header.
  auto row = [this](size_t n) {
    return static_cast<int16_t>((kHeaderRows + n + 1) * kRowHeight - 1);
  };

  u8g2_.drawStr(1, row(0), sats);
  u8g2_.drawStr(128 - 1 - u8g2_.getStrWidth(state), row(0), state);

  // Formatted with integer arithmetic throughout. Nothing else in this
  // firmware printf()s a float, and whether %f works at all depends on which
  // newlib variant the core was built with -- not a thing to discover on a
  // screen at a track day.
  if (status.gpsPresent && fix.fixQuality > 0) {
    const char* latSign = fix.latE7 < 0 ? "-" : "";
    const uint32_t latAbs = static_cast<uint32_t>(fix.latE7 < 0 ? -fix.latE7 : fix.latE7);
    snprintf(line, sizeof(line), "LAT %s%lu.%05lu", latSign,
             static_cast<unsigned long>(latAbs / 10000000UL),
             static_cast<unsigned long>((latAbs % 10000000UL) / 100UL));
    u8g2_.drawStr(1, row(1), line);

    const char* lonSign = fix.lonE7 < 0 ? "-" : "";
    const uint32_t lonAbs = static_cast<uint32_t>(fix.lonE7 < 0 ? -fix.lonE7 : fix.lonE7);
    snprintf(line, sizeof(line), "LON %s%lu.%05lu", lonSign,
             static_cast<unsigned long>(lonAbs / 10000000UL),
             static_cast<unsigned long>((lonAbs % 10000000UL) / 100UL));
    u8g2_.drawStr(1, row(2), line);

    const unsigned dop = static_cast<unsigned>(fix.hdop * 10.0f + 0.5f);
    snprintf(line, sizeof(line), "ALT %dm DOP %u.%u", static_cast<int>(fix.altitudeM),
             dop / 10u, dop % 10u);
    u8g2_.drawStr(1, row(3), line);

    const unsigned speed = static_cast<unsigned>(fix.speedKmh * 10.0f + 0.5f);
    snprintf(line, sizeof(line), "SPD %u.%u km/h", speed / 10u, speed % 10u);
    u8g2_.drawStr(1, row(4), line);
  } else if (status.gpsPresent) {
    u8g2_.drawStr(1, row(1), "ACQUIRING");
  }

  // UTC last, and only once the receiver says its clock is trustworthy --
  // that flag is also what gates transmission to RaceChrono.
  if (status.gpsTimeValid) {
    snprintf(line, sizeof(line), "UTC %02u:%02u:%02u", static_cast<unsigned>(fix.hour),
             static_cast<unsigned>(fix.minute), static_cast<unsigned>(fix.seconds));
    u8g2_.drawStr(1, row(5), line);
  }

  u8g2_.sendBuffer();
}
