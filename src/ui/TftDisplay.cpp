#include "ui/TftDisplay.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "core/Format.h"
#include "core/Log.h"
#include "radio/HoldDetector.h"

namespace {

// The header is worth repainting only when something on it actually changed.
// Uptime is compared at second resolution, which is the only field that moves
// on its own. holdMs is compared at 100 ms resolution so the countdown
// animates without repainting on every single frame.
bool headerDiffers(const DeviceStatus& a, const DeviceStatus& b) {
  return strcmp(a.ssid, b.ssid) != 0 || strcmp(a.ip, b.ip) != 0 ||
         a.clients != b.clients || a.apUp != b.apUp ||
         a.mode != b.mode || a.bleConnected != b.bleConnected ||
         a.kbPerSec != b.kbPerSec || a.dropped != b.dropped ||
         (a.holdMs / 100u) != (b.holdMs / 100u) ||
         (a.uptimeMs / 1000u) != (b.uptimeMs / 1000u);
}

// Header fields are drawn with a fixed background width so new text overwrites
// the old in a single pass. Left column is anchored at x=4 growing right, the
// right column at x=width-4 growing left; together they span the bar without
// overlapping.
constexpr int16_t kHeaderLeftWidth = 150;
constexpr int16_t kHeaderRightWidth = 166;

}  // namespace

bool TftDisplay::begin() {
  tft_.init();
  tft_.setRotation(kRotation);
  tft_.fillScreen(TFT_BLACK);
  // Painted once. drawHeader() must never clear the bar itself — clearing and
  // then drawing is what made the header blink once a second as the uptime
  // ticked.
  tft_.fillRect(0, 0, tft_.width(), kHeaderHeight, TFT_NAVY);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  ready_ = true;
  return true;
}

void TftDisplay::tick(uint32_t nowMs, const DeviceStatus& status) {
  if (!ready_) {
    return;
  }
  if (nowMs - lastDrawMs_ < kMinRedrawIntervalMs) {
    return;
  }
  lastDrawMs_ = nowMs;

  if (!headerDrawn_ || headerDiffers(status, drawnStatus_)) {
    drawHeader(status);
    drawnStatus_ = status;
    headerDrawn_ = true;
  }

  const uint32_t revision = Log::revision();
  if (revision != drawnRevision_) {
    Log::snapshot(console_);
    drawLog();
    drawnRevision_ = revision;
  }
}

void TftDisplay::drawHeader(const DeviceStatus& status) {
  // No fillRect here on purpose. setTextPadding makes each drawString paint its
  // own background out to a fixed width, so the new value covers the old one in
  // the same operation and the bar is never momentarily blank.
  tft_.setTextFont(2);
  tft_.setTextSize(1);
  tft_.setTextColor(TFT_WHITE, TFT_NAVY);

  char topLeft[40];
  char topRight[40];
  char bottomLeft[40];

  if (status.mode == RadioMode::Wifi) {
    snprintf(topLeft, sizeof(topLeft), "%s", status.ssid);
    snprintf(topRight, sizeof(topRight), "%s  clients:%u", status.ip,
             (unsigned)status.clients);
    snprintf(bottomLeft, sizeof(bottomLeft), "%s", status.apUp ? "AP UP" : "AP FAIL");
  } else {
    snprintf(topLeft, sizeof(topLeft), "teletrack");
    snprintf(topRight, sizeof(topRight), "%s",
             status.bleConnected ? "BLE CONN" : "BLE ADV");
    if (status.bleConnected) {
      snprintf(bottomLeft, sizeof(bottomLeft), "%u kB/s drop:%u",
               (unsigned)status.kbPerSec, (unsigned)status.dropped);
    } else {
      snprintf(bottomLeft, sizeof(bottomLeft), "WAITING");
    }
  }

  // A hold in progress takes over the bottom-left field: it is the only
  // feedback that the button is doing anything.
  if (status.holdMs > 0) {
    snprintf(bottomLeft, sizeof(bottomLeft), "HOLD %lus",
             (unsigned long)((HoldDetector::kHoldMs - status.holdMs) / 1000u + 1u));
  }

  tft_.setTextDatum(TL_DATUM);
  tft_.setTextPadding(kHeaderLeftWidth);
  tft_.drawString(topLeft, 4, 2);
  tft_.drawString(bottomLeft, 4, 20);

  char stamp[9];
  Format::uptime(status.uptimeMs, stamp, sizeof(stamp));
  char bottomRight[40];
  snprintf(bottomRight, sizeof(bottomRight), "up %s", stamp);

  tft_.setTextDatum(TR_DATUM);
  tft_.setTextPadding(kHeaderRightWidth);
  tft_.drawString(topRight, tft_.width() - 4, 2);
  tft_.drawString(bottomRight, tft_.width() - 4, 20);

  tft_.setTextPadding(0);  // drawLog pads its own lines
  tft_.setTextDatum(TL_DATUM);
}

void TftDisplay::drawLog() {
  // console_ is this frame's private copy, taken by tick(). Drawing takes
  // ~20 ms and must never block a logging task for that long.
  tft_.setTextFont(1);  // GLCD 6x8, monospace
  tft_.setTextSize(1);
  tft_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft_.setTextDatum(TL_DATUM);

  for (size_t i = 0; i < LogRing::kRows; ++i) {
    // Padded to the full column count so a shorter line erases the longer one
    // that was there before. Opaque text background means no clear-then-draw,
    // so no flicker.
    char padded[LogRing::kLineSize];
    snprintf(padded, sizeof(padded), "%-*s", static_cast<int>(LogRing::kCols),
             console_.row(i));
    tft_.drawString(padded, 2, kHeaderHeight + static_cast<int16_t>(i) * kLogLinePitch);
  }
}
