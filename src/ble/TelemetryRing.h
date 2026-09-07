#pragma once

#include <stddef.h>
#include <stdint.h>

// One ready-to-send RaceChrono GPS main-characteristic packet (UUID 0x0003).
// Opaque here: RaceChronoGps builds it, TelemetryRing only moves it. Packed so
// the on-wire layout is exactly kSize bytes and drain() can use a fixed
// stride. A RaceChrono packet carries its own time (see RaceChronoGps), so
// there is no separate seq/uptimeMs framing.
struct __attribute__((packed)) TelemetrySample {
  static constexpr size_t kSize = 20;

  uint8_t bytes[kSize];
};

// Fixed ring between the sample producer and the BLE notify pump.
//
// On overflow it overwrites the oldest sample and counts the loss. It never
// blocks and never allocates: a phone that cannot keep up must not be able to
// stall loop() and take the screen and button down with it.
class TelemetryRing {
 public:
  // 256 * 20 B = 5.1 kB, about 1.28 s of production at 200 Hz.
  static constexpr size_t kCapacity = 256;

  // Copies one already-encoded packet (TelemetrySample::kSize bytes) in.
  void push(const uint8_t* packet);

  // Copies as many whole samples as fit into out, returning bytes written.
  // Never writes a partial sample.
  size_t drain(uint8_t* out, size_t outSize);

  uint32_t dropped() const { return dropped_; }
  uint32_t produced() const { return produced_; }
  size_t pending() const { return count_; }
  bool empty() const { return count_ == 0; }

 private:
  TelemetrySample samples_[kCapacity] = {};
  size_t head_ = 0;   // oldest
  size_t count_ = 0;
  uint32_t dropped_ = 0;
  uint32_t produced_ = 0;
};
