#include "ui/Display.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "core/Format.h"
#include "core/Log.h"

namespace {

// The header is worth repainting only when something on it actually changed.
// Uptime is compared at second resolution, which is the only field that moves
// on its own.
bool headerDiffers(const DeviceStatus& a, const DeviceStatus& b) {
  return strcmp(a.ssid, b.ssid) != 0 || strcmp(a.ip, b.ip) != 0 ||
         a.clients != b.clients || a.apUp != b.apUp ||
         (a.uptimeMs / 1000u) != (b.uptimeMs / 1000u);
}

}  // namespace

bool Display::begin() {
  tft_.init();
  tft_.setRotation(kRotation);
  tft_.fillScreen(TFT_BLACK);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  ready_ = true;
  return true;
}

void Display::tick(uint32_t nowMs, const DeviceStatus& status) {
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

void Display::drawHeader(const DeviceStatus& status) {
  tft_.fillRect(0, 0, tft_.width(), kHeaderHeight, TFT_NAVY);
  tft_.setTextFont(2);
  tft_.setTextSize(1);
  tft_.setTextColor(TFT_WHITE, TFT_NAVY);

  tft_.setTextDatum(TL_DATUM);
  tft_.drawString(status.ssid, 4, 2);
  tft_.drawString(status.apUp ? "AP UP" : "AP FAIL", 4, 20);

  char right[48];
  snprintf(right, sizeof(right), "%s  clients:%u", status.ip,
           (unsigned)status.clients);
  tft_.setTextDatum(TR_DATUM);
  tft_.drawString(right, tft_.width() - 4, 2);

  char stamp[9];
  Format::uptime(status.uptimeMs, stamp, sizeof(stamp));
  char lower[48];
  snprintf(lower, sizeof(lower), "up %s", stamp);
  tft_.drawString(lower, tft_.width() - 4, 20);

  tft_.setTextDatum(TL_DATUM);
}

void Display::drawLog() {
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
