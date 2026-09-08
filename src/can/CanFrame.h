#pragma once

#include <stddef.h>
#include <stdint.h>

// One frame off the bus, in the form RaceChrono wants it.
struct CanFrame {
  // 4 bytes of id plus up to 8 of payload. Classic CAN caps the payload at 8;
  // the BLE profile allows 16, which only CAN FD would use.
  static constexpr size_t kMaxPacketBytes = 12;

  uint32_t id = 0;
  uint8_t dlc = 0;
  uint8_t data[8] = {};
};

// Builds the payload for characteristic 0x0001. Returns 4 + dlc; the packet is
// never padded, because the length is what tells the app how many data bytes
// the frame carried.
//
// The id is LITTLE-endian. This is the one field in RaceChrono's protocol that
// is -- its spec says so outright -- while every GPS field in this firmware is
// big-endian and the filter command that selects this id is big-endian too.
size_t encodeCanPacket(const CanFrame& frame, uint8_t out[CanFrame::kMaxPacketBytes]);
