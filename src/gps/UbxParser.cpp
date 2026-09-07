#include "gps/UbxParser.h"

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
  // Task 3 fills this in.
}
