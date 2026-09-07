#pragma once

#include <TFT_eSPI.h>

#include "core/DeviceStatus.h"
#include "core/LogRing.h"
#include "ui/Display.h"

// The only thing in the firmware that touches the TFT. tick() is called from
// loop() and is the sole place that drives SPI, which is what keeps the
// AsyncTCP task from racing the display.
class TftDisplay : public Display {
 public:
  static constexpr uint32_t kMinRedrawIntervalMs = 100;  // 10 Hz ceiling
  static constexpr int16_t kHeaderHeight = 40;
  static constexpr int16_t kLogLinePitch = 10;
  static constexpr uint8_t kRotation = 1;  // landscape, 320x240

  bool begin() override;

  // Call from loop() only.
  void tick(uint32_t nowMs, const DeviceStatus& status) override;

 private:
  void drawHeader(const DeviceStatus& status);
  void drawLog();

  TFT_eSPI tft_;
  LogRing console_;  // this frame's copy, refreshed from Log::snapshot()
  bool ready_ = false;
  uint32_t lastDrawMs_ = 0;
  uint32_t drawnRevision_ = 0;
  bool headerDrawn_ = false;
  DeviceStatus drawnStatus_;
};
