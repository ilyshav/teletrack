#include "ui/OledDisplay.h"

#include <Arduino.h>
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
  return strcmp(a.ssid, b.ssid) != 0 || a.clients != b.clients ||
         a.apUp != b.apUp || a.mode != b.mode ||
         a.bleConnected != b.bleConnected || a.dropped != b.dropped ||
         (a.holdMs / 100u) != (b.holdMs / 100u) ||
         (a.uptimeMs / 1000u) != (b.uptimeMs / 1000u);
}

}  // namespace

bool OledDisplay::begin() {
  u8g2_.setBusClock(400000);
  u8g2_.setI2CAddress(0x3C << 1);  // u8g2 wants the address pre-shifted
  if (!u8g2_.begin()) {
    return false;
  }
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
  char right[kCols + 1];

  if (status.mode == RadioMode::Wifi) {
    snprintf(left, sizeof(left), "%s", status.ssid);
    snprintf(right, sizeof(right), "%s", status.apUp ? "AP UP" : "AP FAIL");
  } else {
    snprintf(left, sizeof(left), "%s", status.ssid);  // deviceName drives both
    snprintf(right, sizeof(right), "%s",
             status.bleConnected ? "BLE CONN" : "BLE ADV");
  }

  // Header rows in inverse video: it separates them from the log without
  // spending one of the eight rows on a rule.
  u8g2_.drawBox(0, 0, 128, kHeaderRows * kRowHeight);
  u8g2_.setDrawColor(0);
  u8g2_.drawStr(1, kRowHeight - 1, left);
  u8g2_.drawStr(128 - 1 - u8g2_.getStrWidth(right), kRowHeight - 1, right);

  char stamp[9];
  Format::uptime(status.uptimeMs, stamp, sizeof(stamp));
  char lower[kCols + 1];
  char lowerRight[kCols + 1];
  snprintf(lower, sizeof(lower), "up %s", stamp);
  if (status.holdMs > 0) {
    snprintf(lower, sizeof(lower), "HOLD %lus",
             (unsigned long)((HoldDetector::kHoldMs - status.holdMs) / 1000u + 1u));
  }
  if (status.mode == RadioMode::Wifi) {
    snprintf(lowerRight, sizeof(lowerRight), "%u cli", (unsigned)status.clients);
  } else if (status.bleConnected) {
    snprintf(lowerRight, sizeof(lowerRight), "drop %u", (unsigned)status.dropped);
  } else {
    lowerRight[0] = '\0';
  }
  u8g2_.drawStr(1, 2 * kRowHeight - 1, lower);
  if (lowerRight[0] != '\0') {
    u8g2_.drawStr(128 - 1 - u8g2_.getStrWidth(lowerRight), 2 * kRowHeight - 1,
                  lowerRight);
  }
  u8g2_.setDrawColor(1);

  // The newest kLogRows lines from a ring that holds more. LogRing::row(0) is
  // the oldest, so start kLogRows back from the end.
  for (size_t i = 0; i < kLogRows; ++i) {
    const size_t src = LogRing::kRows - kLogRows + i;
    char line[kCols + 1];
    snprintf(line, sizeof(line), "%s", console_.row(src));
    if (strlen(console_.row(src)) > kCols) {
      line[kCols - 1] = '~';  // same truncation mark LogRing itself uses
    }
    const int16_t y =
        static_cast<int16_t>((kHeaderRows + i + 1) * kRowHeight - 1);
    u8g2_.drawStr(1, y, line);
  }

  u8g2_.sendBuffer();
}
