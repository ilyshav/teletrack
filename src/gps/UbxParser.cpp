#include "gps/UbxParser.h"

namespace {

// UBX is little-endian, unlike the big-endian RaceChrono payloads next door.
uint16_t readU2(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readU4(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readI4(const uint8_t* p) { return static_cast<int32_t>(readU4(p)); }

}  // namespace

bool UbxParser::feed(uint8_t byte) {
  switch (state_) {
    case State::Sync1:
      if (byte == kSync1) {
        state_ = State::Sync2;
      }
      return false;

    case State::Sync2:
      // A repeated 0xB5 restarts the sync rather than being swallowed, so
      // "B5 B5 62 ..." still frames correctly.
      state_ = (byte == kSync2)   ? State::Class
               : (byte == kSync1) ? State::Sync2
                                  : State::Sync1;
      return false;

    case State::Class:
      class_ = byte;
      ckA_ = byte;  // Fletcher starts at zero, so the first add is the value
      ckB_ = byte;
      state_ = State::Id;
      return false;

    case State::Id:
      id_ = byte;
      ckA_ += byte;
      ckB_ += ckA_;
      state_ = State::LenLo;
      return false;

    case State::LenLo:
      length_ = byte;
      ckA_ += byte;
      ckB_ += ckA_;
      state_ = State::LenHi;
      return false;

    case State::LenHi:
      length_ = static_cast<uint16_t>(length_ | (static_cast<uint16_t>(byte) << 8));
      ckA_ += byte;
      ckB_ += ckA_;
      index_ = 0;
      // A length is only ever trusted this far. Resyncing inside a truncated
      // frame lands on arbitrary payload bytes, and a length read from those
      // can be up to 0xFFFF -- 65 kB, nearly six seconds of silence at 115200
      // baud. Anything past the largest message this receiver is configured to
      // emit is desync, not a message: drop back to hunting for sync.
      if (length_ > kMaxPayload) {
        state_ = State::Sync1;
        return false;
      }
      state_ = (length_ == 0) ? State::CkA : State::Payload;
      return false;

    case State::Payload:
      if (index_ < kPvtLength) {
        payload_[index_] = byte;
      }
      ckA_ += byte;
      ckB_ += ckA_;
      if (++index_ >= length_) {
        state_ = State::CkA;
      }
      return false;

    case State::CkA:
      rxCkA_ = byte;
      state_ = State::CkB;
      return false;

    case State::CkB:
      state_ = State::Sync1;
      if (rxCkA_ != ckA_ || byte != ckB_) {
        ++checksumErrors_;
        return false;
      }
      if (class_ != kClassNav || id_ != kIdPvt || length_ != kPvtLength) {
        return false;  // consumed and in sync, just not ours
      }
      decodePvt();
      return true;
  }
  return false;
}

void UbxParser::decodePvt() {
  const uint8_t* p = payload_;

  fix_.year = readU2(p + 4);
  fix_.month = p[6];
  fix_.day = p[7];
  fix_.hour = p[8];
  fix_.minute = p[9];
  fix_.seconds = p[10];

  const uint8_t valid = p[11];
  timeValid_ = (valid & 0x01) != 0 && (valid & 0x02) != 0;

  // nano is signed: the true instant can fall either side of the reported
  // second. A negative offset belongs to the previous second, which this
  // encoding cannot express, so it reads as .000.
  const int32_t nano = readI4(p + 16);
  const int32_t millis = nano > 0 ? nano / 1000000 : 0;
  fix_.millis = static_cast<uint16_t>(millis > 999 ? 999 : millis);

  const uint8_t fixType = p[20];
  const uint8_t flags = p[21];
  const bool gnssFixOk = (flags & 0x01) != 0;
  const bool diffSoln = (flags & 0x02) != 0;
  const bool usable = gnssFixOk && (fixType == 2 || fixType == 3 || fixType == 4);

  fix_.fixType = fixType;
  fix_.fixQuality = usable ? (diffSoln ? 2 : 1) : 0;
  fix_.satellites = p[23];

  fix_.lonE7 = readI4(p + 24);
  fix_.latE7 = readI4(p + 28);
  fix_.altitudeM = static_cast<float>(readI4(p + 36)) / 1000.0f;   // mm
  fix_.speedKmh = static_cast<float>(readI4(p + 60)) * 0.0036f;    // mm/s
  fix_.bearingDeg = static_cast<float>(readI4(p + 64)) / 100000.0f;
  // NAV-PVT carries position DOP, not horizontal. pDOP >= hDOP always, so
  // this is pessimistic rather than misleading, and RaceChrono uses the
  // value only for quality weighting. Getting true HDOP means enabling
  // UBX-NAV-DOP as a second message and correlating the two.
  fix_.hdop = static_cast<float>(readU2(p + 76)) / 100.0f;
}
