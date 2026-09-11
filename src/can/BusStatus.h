#pragma once

#include <stdint.h>

// What the status bar has to say about the CAN bus.
//
// Ported from the telelog bus monitor, where it drove an RGB LED. The question
// it answers is the same one here: is the wiring right?
enum class BusStatus : uint8_t {
  // The controller is not running -- the driver would not install or start, or
  // there is no transceiver on this board. Nothing is being read at all.
  Starting,
  // No frames in the last window: the bus is silent or the wiring is wrong.
  // This is the state to rule out first, and the one that says to swap CAN RX
  // and TX before suspecting anything subtler. A swap gives silence, which in
  // listen-only mode is indistinguishable from a quiet bus.
  Silent,
  // Frames are arriving, but some are being lost -- to a full RX queue while
  // the screen redraws, or to errors on the wire. The CAN screen's separate
  // rx/bus error counters tell those two apart.
  Lossy,
  // Frames are arriving and none are being lost. The wiring is right.
  Healthy,
};

// Decides the status from one window of counters -- the same one-second window
// the CAN screen reports.
//
// lostInWindow counts every frame the bus sent that we did not get: queue
// overruns and wire errors alike. A window with losses but no frames is NOT
// silent -- something is transmitting, and saying "check your wiring" then
// sends you to the wrong place entirely.
BusStatus busStatus(bool controllerRunning, uint32_t framesInWindow,
                    uint32_t lostInWindow);
