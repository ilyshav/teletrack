#pragma once

#include <stdint.h>

#include "core/DeviceStatus.h"

// What main.cpp holds. Two implementations exist: TftDisplay for the
// ILI9341 on the DevKitC, OledDisplay for the SH1106 on the T-Beam. They
// share no code -- colour SPI against monochrome I2C, 53x20 against 21x8 --
// so this is an interface rather than a base class with behaviour.
class Display {
 public:
  virtual ~Display() = default;

  virtual bool begin() = 0;

  // Call from loop() only. The sole place that drives the display bus.
  virtual void tick(uint32_t nowMs, const DeviceStatus& status) = 0;
};
