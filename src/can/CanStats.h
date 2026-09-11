#pragma once

#include <stdint.h>

// What one window of the bus looked like.
struct CanSnapshot {
  uint32_t framesPerSec = 0;
  uint32_t lostPerSec = 0;
  // Distinct 11-bit ids since boot, not since the last window. Discovery is
  // cumulative: the count answers "what is on this bus", which does not stop
  // being true when the window rolls.
  uint16_t idsSeen = 0;
  // Frames carrying a 29-bit id, since boot. Counted apart because they cannot
  // index the set, so without this "ids seen" could silently disagree with the
  // frame rate. The MX-5's powertrain bus is 11-bit throughout, so anything
  // here means a diagnostic tool is active or this is the wrong bus.
  uint32_t extendedFrames = 0;
};

// Accumulates what arrives off the bus and publishes it once a second.
//
// Pure and clock-free, like HoldDetector: the caller supplies the timestamp, so
// every threshold here is host-testable with no bus and no board.
//
// Rates are published rather than live because the CAN screen is read by a
// human -- a counter updating at 2 kHz is unreadable, and a partial window
// reads low for the first second after every switch to the screen.
class CanStats {
 public:
  static constexpr uint32_t kWindowMs = 1000;
  // The largest id expressible in 11 bits, and so the last index of the set.
  static constexpr uint32_t kMaxStandardId = 0x7FF;

  // Called for every frame handed over by CanBus.
  void recordFrame(uint32_t id);

  // Frames the controller lost before the reader ever saw them, from
  // CanBus::takeLost(). Accumulated into the current window like arrivals.
  void recordLost(uint32_t count);

  // Rolls the window when kWindowMs has elapsed. True when a new snapshot was
  // published, so the caller can repaint only then.
  bool tick(uint32_t nowMs);

  const CanSnapshot& snapshot() const { return published_; }

 private:
  // One bit per 11-bit id: 2048 bits in 64 words, 256 bytes. A bitset rather
  // than the full IdTable the telelog monitor carries -- the screen needs the
  // count, not the per-id rates, and that is what telelog is for.
  static constexpr uint32_t kSetWords = (kMaxStandardId + 1) / 32u;

  CanSnapshot published_;
  uint32_t seen_[kSetWords] = {};
  uint16_t idsSeen_ = 0;
  uint32_t extendedFrames_ = 0;
  uint32_t frames_ = 0;
  uint32_t lost_ = 0;
  // Zero-initialised deliberately: the first tick() past kWindowMs publishes an
  // empty window and seats the real start, which costs one second of display at
  // boot and avoids a "have we started yet" flag.
  uint32_t windowStartMs_ = 0;
};
