#pragma once

#include <U8g2lib.h>

#include "board/BoardConfig.h"
#include "core/DeviceStatus.h"
#include "core/LogRing.h"
#include "ui/Display.h"

// SH1106 128x64 over I2C. At the 6x8 font that is 21 columns by 8 rows: one
// header row in inverse video, then the seven most recent log lines truncated
// to fit. On 21 columns every row spent on chrome is a row of log lost, so the
// header carries only what changes -- state, its one relevant count, uptime.
class OledDisplay : public Display {
 public:
  static constexpr uint32_t kMinRedrawIntervalMs = 100;  // 10 Hz ceiling
  static constexpr size_t kCols = 21;
  static constexpr size_t kHeaderRows = 1;
  static constexpr size_t kLogRows = 7;
  static constexpr int16_t kRowHeight = 8;

  // 0x3C is the SSD1306/SH1106 default, but on T-Beam Supreme V3 the QMC6310N
  // magnetometer occupies it and the panel moves to 0x3D.
  static constexpr uint8_t kAddrPrimary = 0x3C;
  static constexpr uint8_t kAddrAlternate = 0x3D;

  bool begin() override;
  void tick(uint32_t nowMs, const DeviceStatus& status) override;

 private:
  void draw(const DeviceStatus& status);

  // The pins are passed to the constructor, not left to Wire. U8g2's hardware
  // I2C begin() calls Wire.begin() with no arguments, which on the S3 resets
  // the bus to default pins -- so a display that answered a scan on 17/18 goes
  // silent, begin() still returns true, and the panel stays dark.
  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2_{U8G2_R0, U8X8_PIN_NONE,
                                           BoardConfig::kI2cScl,
                                           BoardConfig::kI2cSda};
  LogRing console_;  // this frame's copy, refreshed from Log::snapshot()
  bool ready_ = false;
  uint32_t lastDrawMs_ = 0;
  uint32_t drawnRevision_ = 0;
  DeviceStatus drawnStatus_;
  bool drawnOnce_ = false;
};
