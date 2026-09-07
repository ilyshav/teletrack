#include "ui/OledDisplay.h"

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>
#include <string.h>

#include "board/BoardConfig.h"
#include "core/Format.h"
#include "core/Log.h"
#include "radio/HoldDetector.h"

namespace {

// Same fields the TFT header compares, so the panel repaints when anything
// visible changes and not otherwise.
bool headerDiffers(const DeviceStatus& a, const DeviceStatus& b) {
  // The name is not drawn here, so a rename does not dirty the header.
  return a.clients != b.clients || a.apUp != b.apUp || a.mode != b.mode ||
         a.bleConnected != b.bleConnected || a.dropped != b.dropped ||
         (a.holdMs / 100u) != (b.holdMs / 100u) ||
         (a.uptimeMs / 1000u) != (b.uptimeMs / 1000u);
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

  const uint32_t revision = Log::revision();
  const bool logChanged = revision != drawnRevision_;
  const bool headerChanged = !drawnOnce_ || headerDiffers(status, drawnStatus_);
  if (!logChanged && !headerChanged) {
    return;
  }
  if (logChanged) {
    Log::snapshot(console_);
    drawnRevision_ = revision;
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

  char left[kCols + 1];
  if (status.holdMs > 0) {
    snprintf(left, sizeof(left), "HOLD %lus",
             (unsigned long)((HoldDetector::kHoldMs - status.holdMs) / 1000u + 1u));
  } else if (status.mode == RadioMode::Wifi) {
    if (status.apUp) {
      snprintf(left, sizeof(left), "AP UP %u cli", (unsigned)status.clients);
    } else {
      snprintf(left, sizeof(left), "AP FAIL");
    }
  } else if (status.bleConnected) {
    if (status.dropped > 0) {
      snprintf(left, sizeof(left), "BLE CONN d%u", (unsigned)status.dropped);
    } else {
      snprintf(left, sizeof(left), "BLE CONN");
    }
  } else {
    snprintf(left, sizeof(left), "BLE ADV");
  }

  char stamp[9];
  Format::uptime(status.uptimeMs, stamp, sizeof(stamp));

  // One inverse-video row. It separates header from log without spending a
  // row on a rule, and the widest state string plus the stamp is 20 columns.
  u8g2_.drawBox(0, 0, 128, kHeaderRows * kRowHeight);
  u8g2_.setDrawColor(0);
  u8g2_.drawStr(1, kRowHeight - 1, left);
  u8g2_.drawStr(128 - 1 - u8g2_.getStrWidth(stamp), kRowHeight - 1, stamp);
  u8g2_.setDrawColor(1);

  // The newest kLogRows lines from a ring that holds more. LogRing::row(0) is
  // the oldest, so start kLogRows back from the end.
  for (size_t i = 0; i < kLogRows; ++i) {
    const size_t src = LogRing::kRows - kLogRows + i;
    // LogRing rows are the full serial line, "HH:MM:SS [INF] tag: msg". The
    // stamp and level cost 15 of the 21 columns here and say nothing the
    // serial log does not already carry, so skip past them to the message.
    const char* full = console_.row(src);
    const char* body = strstr(full, "] ");
    body = (body != nullptr) ? body + 2 : full;

    char line[kCols + 1];
    snprintf(line, sizeof(line), "%s", body);
    if (strlen(body) > kCols) {
      line[kCols - 1] = '~';  // same truncation mark LogRing itself uses
    }
    const int16_t y =
        static_cast<int16_t>((kHeaderRows + i + 1) * kRowHeight - 1);
    u8g2_.drawStr(1, y, line);
  }

  u8g2_.sendBuffer();
}
