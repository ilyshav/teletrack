#include "can/CanFrame.h"

size_t encodeCanPacket(const CanFrame& frame, uint8_t out[CanFrame::kMaxPacketBytes]) {
  out[0] = static_cast<uint8_t>(frame.id);
  out[1] = static_cast<uint8_t>(frame.id >> 8);
  out[2] = static_cast<uint8_t>(frame.id >> 16);
  out[3] = static_cast<uint8_t>(frame.id >> 24);

  const uint8_t dlc = frame.dlc > 8 ? 8 : frame.dlc;
  for (uint8_t i = 0; i < dlc; ++i) {
    out[4 + i] = frame.data[i];
  }
  return 4u + dlc;
}
