#include "gps/UbxParser.h"

bool UbxParser::feed(uint8_t byte) {
  const uint8_t prev = prevByte_;
  prevByte_ = byte;

  // Mid-frame states have no timeout to fall back on, so this is the only
  // signal that the frame in progress is dead: a real sync pair can't occur
  // inside a well-formed one, so seeing one means whatever came before was
  // truncated or corrupt. Restart as if sync had just been recognized.
  if (state_ != State::Sync1 && state_ != State::Sync2 && prev == kSync1 &&
      byte == kSync2) {
    state_ = State::Class;
    ckA_ = 0;
    ckB_ = 0;
    index_ = 0;
    return false;
  }

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
