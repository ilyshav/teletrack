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

  // Frames the driver holds for us, so a stalled reader is a delay rather than
  // a loss. 128 frames is 64 ms of a 2000 frames-a-second bus.
  //
  // This was 32 -- 16 ms -- on the reasoning that loop() turns at roughly 1 kHz
  // so a pass sees a couple of frames. That ignored the display: the panel is a
  // full-buffer 128x64 at 400 kHz, which is 1024 bytes and about 25 ms on the
  // wire, once a second. Every repaint could therefore overflow the queue, and
  // nothing counted it. See takeLost().
  static constexpr uint32_t kRxQueueLen = 128;

  // Is a powered transceiver actually driving the RX pin?
  //
  // With no bus attached there are no frames either way, so silence proves
  // nothing about the wiring. This does: an idle CAN bus is recessive, and a
  // powered transceiver holds its R output HIGH. Pulling the pin down and
  // finding it still high means something is actively driving it -- which on
  // that pin can only be the transceiver.
  //
  // False means unpowered, wired to the wrong pin, or RX and TX swapped: the
  // module's D pin is an input, so a swap leaves this pin floating with nothing
  // to fight the pull-down. That is the failure docs/hardware-notes.md says to
  // suspect first, and in listen-only mode it is otherwise indistinguishable
  // from a quiet bus.
  //
  // Passive, and call it BEFORE begin() -- the TWAI driver claims the pin.
  static bool transceiverPresent();

  // Installs and starts the controller in listen-only mode. False when the
  // driver will not install -- a bus that is not there must not stop the rest
  // of the firmware.
  bool begin();

  // Pops one frame. False when the queue is empty.
  bool read(CanFrame& out);

  // What the controller itself thinks is happening. Without this, "RaceChrono
  // is showing nothing" cannot be told apart from a silent bus, a filter that
  // matches nothing, or frames lost to a full queue.
  struct Diagnostics {
    const char* state = "?";
    uint32_t rxErrors = 0;   // receive error counter
    uint32_t busErrors = 0;  // bit/stuff/form/CRC errors seen on the wire
    uint32_t missed = 0;     // frames lost to a full RX queue, cumulative
    uint32_t arbLost = 0;
  };
  Diagnostics diagnostics() const;

  // Frames the controller lost to a full RX queue or a FIFO overrun since the
  // last call. These never reached the reader, so only the driver's own
  // counters know about them.
  uint16_t takeLost();

  bool present() const { return present_; }
  uint32_t received() const { return received_; }

 private:
  bool present_ = false;
  uint32_t received_ = 0;
  uint32_t lostSeen_ = 0;
};
