#pragma once

#include <stdint.h>

#include "can/CanFrame.h"

// The car's CAN bus, read-only.
//
// On a board with no transceiver every method here does nothing and begin()
// returns false, exactly like GpsReceiver.
class CanBus {
 public:
  // The ND's powertrain bus. A property of the car, not a preference.
  static constexpr uint32_t kBitrateKbps = 500;

  // Installs and starts the controller in listen-only mode. False when the
  // driver will not install -- a bus that is not there must not stop the rest
  // of the firmware.
  bool begin();

  // Pops one frame. False when the queue is empty.
  bool read(CanFrame& out);

  bool present() const { return present_; }
  uint32_t received() const { return received_; }

 private:
  bool present_ = false;
  uint32_t received_ = 0;
};
