#pragma once

#include <stddef.h>
#include <stdint.h>

// One telemetry sample as it goes over the air. Packed so the on-wire layout is
// exactly kSize bytes and the receiver can parse it with a fixed stride.
struct __attribute__((packed)) TelemetrySample {
  // Room Phase 3 will use for real GPS and IMU fields. Phase 2 fills it with a
  // pattern so the transport can be measured before the payload exists.
  static constexpr size_t kPayloadBytes = 12;
  static constexpr size_t kSize = sizeof(uint32_t) * 2 + kPayloadBytes;  // 20

  uint32_t seq;
  uint32_t uptimeMs;
  uint8_t payload[kPayloadBytes];
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

  // seq is assigned here, so it counts every sample ever produced — including
  // the dropped ones. A gap at the receiver is exactly the loss.
  void push(uint32_t uptimeMs, const uint8_t* payload);

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
  uint32_t nextSeq_ = 0;
  uint32_t dropped_ = 0;
  uint32_t produced_ = 0;
};
