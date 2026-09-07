#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/GpsFix.h"

// Finds UBX-NAV-PVT messages in a byte stream and decodes them into a GpsFix.
//
// Pure: no Arduino, no UART, no clock. Fed one byte at a time so it never
// needs a buffer of its own beyond the frame being assembled, and so a frame
// split across two UART reads costs nothing to handle.
//
// UBX is LITTLE-endian throughout. RaceChrono's payloads next door are
// big-endian; do not copy byte-order idioms between the two.
class UbxParser {
 public:
  static constexpr uint8_t kSync1 = 0xB5;
  static constexpr uint8_t kSync2 = 0x62;
  static constexpr uint8_t kClassNav = 0x01;
  static constexpr uint8_t kIdPvt = 0x07;
  static constexpr size_t kPvtLength = 92;

  // Feeds one byte. Returns true on the byte that completes a valid NAV-PVT
  // frame, at which point fix() and timeValid() hold the decoded result.
  bool feed(uint8_t byte);

  const GpsFix& fix() const { return fix_; }

  // True when the receiver reported both validDate and validTime. Until it
  // does, its clock is not to be trusted and nothing should be transmitted.
  bool timeValid() const { return timeValid_; }

  // Frames whose checksum did not match. Diagnostic only.
  uint32_t checksumErrors() const { return checksumErrors_; }

 private:
  enum class State : uint8_t {
    Sync1, Sync2, Class, Id, LenLo, LenHi, Payload, CkA, CkB
  };

  void decodePvt();

  State state_ = State::Sync1;
  uint8_t class_ = 0;
  uint8_t id_ = 0;
  uint16_t length_ = 0;
  uint16_t index_ = 0;
  // Only NAV-PVT is stored. Longer messages still run through the state
  // machine -- they must be consumed to stay in sync -- but their payload is
  // discarded rather than overflowing this.
  uint8_t payload_[kPvtLength] = {};
  uint8_t ckA_ = 0;
  uint8_t ckB_ = 0;
  uint8_t rxCkA_ = 0;
  // The byte before this one. A 0xB5,0x62 pair appearing anywhere past the
  // initial sync -- e.g. a fresh frame arriving right behind one that got
  // truncated -- means a new frame is starting, whatever state we were in.
  uint8_t prevByte_ = 0;
  GpsFix fix_;
  bool timeValid_ = false;
  uint32_t checksumErrors_ = 0;
};
