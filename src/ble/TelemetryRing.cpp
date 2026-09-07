#include "ble/TelemetryRing.h"

#include <string.h>

void TelemetryRing::push(const uint8_t* packet) {
  size_t slot;
  if (count_ < kCapacity) {
    slot = (head_ + count_) % kCapacity;
    ++count_;
  } else {
    // Full: overwrite the oldest and advance the window.
    slot = head_;
    head_ = (head_ + 1) % kCapacity;
    ++dropped_;
  }

  memcpy(samples_[slot].bytes, packet, TelemetrySample::kSize);
  ++produced_;
}

size_t TelemetryRing::drain(uint8_t* out, size_t outSize) {
  const size_t room = outSize / TelemetrySample::kSize;
  size_t take = room < count_ ? room : count_;

  size_t written = 0;
  for (size_t i = 0; i < take; ++i) {
    memcpy(out + written, &samples_[head_], TelemetrySample::kSize);
    head_ = (head_ + 1) % kCapacity;
    --count_;
    written += TelemetrySample::kSize;
  }
  return written;
}
