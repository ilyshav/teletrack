#include "can/CanStats.h"

void CanStats::recordFrame(uint32_t id) {
  ++frames_;

  if (id > kMaxStandardId) {
    ++extendedFrames_;
    return;
  }
  // Id 0 is a real CAN id, so membership is a bit rather than a non-zero
  // value, and 0x7FF has to land on the last bit of the last word.
  const uint32_t word = id / 32u;
  const uint32_t bit = 1u << (id % 32u);
  if ((seen_[word] & bit) == 0u) {
    seen_[word] |= bit;
    ++idsSeen_;
  }
}

void CanStats::recordLost(uint32_t count) { lost_ += count; }

bool CanStats::tick(uint32_t nowMs) {
  // Unsigned subtraction, so the comparison is correct across the millis()
  // wrap every 49 days. Comparing nowMs against a deadline instead would stall
  // the window for the rest of the epoch.
  const uint32_t elapsed = nowMs - windowStartMs_;
  if (elapsed < kWindowMs) {
    return false;
  }

  // Normalised to a true per-second rate. The window is only rolled when tick()
  // is called, and loop() can be held up -- a full OLED repaint is ~25 ms --
  // so reporting the raw count of a long window would overstate the rate.
  // 64-bit so the multiply cannot overflow whatever the window length.
  published_.framesPerSec =
      static_cast<uint32_t>(static_cast<uint64_t>(frames_) * 1000u / elapsed);
  published_.lostPerSec =
      static_cast<uint32_t>(static_cast<uint64_t>(lost_) * 1000u / elapsed);

  // Cumulative, so they are published every window but never reset.
  published_.idsSeen = idsSeen_;
  published_.extendedFrames = extendedFrames_;

  frames_ = 0;
  lost_ = 0;
  windowStartMs_ = nowMs;
  return true;
}
