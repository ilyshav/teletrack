#include "can/CanFilter.h"

void CanFilter::clear() {
  count_ = 0;
  droppedUnknown_ = 0;
}

CanFilter::Entry* CanFilter::find(uint32_t canId) {
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].id == canId) {
      return &entries_[i];
    }
  }
  return nullptr;
}

CanFilter::Entry* CanFilter::add(uint32_t canId, uint16_t intervalMs) {
  if (count_ >= kMaxIds) {
    return nullptr;
  }
  Entry& e = entries_[count_++];
  e.id = canId;
  e.intervalMs = intervalMs;
  e.lastNotifiedMs = 0;
  e.notified = false;
  return &e;
}

bool CanFilter::applyCommand(const uint8_t* data, size_t len) {
  if (data == nullptr || len < 1) {
    return false;
  }

  switch (data[0]) {
    case kCmdDenyAll:
      if (len != 1) {
        return false;
      }
      clear();
      allowAll_ = false;
      defaultIntervalMs_ = 0;
      return true;

    case kCmdAllowAll:
      if (len != 3) {
        return false;
      }
      clear();
      // Big-endian, like every field the app writes to us.
      defaultIntervalMs_ = static_cast<uint16_t>((data[1] << 8) | data[2]);
      allowAll_ = true;
      return true;

    case kCmdAllowOne: {
      if (len != 7) {
        return false;
      }
      const uint16_t intervalMs = static_cast<uint16_t>((data[1] << 8) | data[2]);
      const uint32_t canId = (static_cast<uint32_t>(data[3]) << 24) |
                             (static_cast<uint32_t>(data[4]) << 16) |
                             (static_cast<uint32_t>(data[5]) << 8) |
                             static_cast<uint32_t>(data[6]);
      Entry* e = find(canId);
      if (e == nullptr) {
        e = add(canId, intervalMs);
        return e != nullptr;
      }
      // Repeating an id changes its rate rather than adding it twice.
      e->intervalMs = intervalMs;
      return true;
    }

    default:
      return false;
  }
}

bool CanFilter::shouldNotify(uint32_t canId, uint32_t nowMs) {
  Entry* e = find(canId);
  if (e == nullptr) {
    if (!allowAll_) {
      return false;
    }
    e = add(canId, defaultIntervalMs_);
    if (e == nullptr) {
      // Allow-all is a discovery mode and 64 ids is more than a car has, so
      // this means something unexpected is on the bus. Counted, not silent.
      ++droppedUnknown_;
      return false;
    }
  }

  // The first frame for an id goes out immediately: waiting one interval would
  // delay a slow channel's first sample for no reason. Unsigned subtraction
  // carries across a millis() wrap.
  if (e->notified && e->intervalMs != 0 &&
      (nowMs - e->lastNotifiedMs) < e->intervalMs) {
    return false;
  }

  e->notified = true;
  e->lastNotifiedMs = nowMs;
  return true;
}
