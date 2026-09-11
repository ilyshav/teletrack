#pragma once

#include <stddef.h>
#include <stdint.h>

// Which CAN ids to forward and how often, as RaceChrono asked for them.
//
// Pure: no CAN driver, no BLE, no clock of its own. The caller supplies the
// timestamp, so every rule here is host-testable without a car.
class CanFilter {
 public:
  // A car broadcasts tens of distinct ids and RaceChrono asks for a handful.
  // The reference implementation uses a 1024-slot hash map with heap
  // allocation; 64 entries scanned linearly is smaller, allocates nothing, and
  // 64 comparisons against a frame every 500 us is nothing on this core.
  static constexpr size_t kMaxIds = 64;

  static constexpr uint8_t kCmdDenyAll = 0;
  static constexpr uint8_t kCmdAllowAll = 1;
  static constexpr uint8_t kCmdAllowOne = 2;

  // Applies one command from characteristic 0x0002. False when the payload is
  // not a shape this understands -- an unknown command, or the wrong length
  // for its type. A malformed command is refused rather than half-applied.
  //
  // Fields inside are BIG-endian, unlike the id encodeCanPacket() writes.
  bool applyCommand(const uint8_t* data, size_t len);

  // True when a frame with this id should go out now. Records the time when it
  // says yes, so a caller cannot forget to and quietly disable the throttle.
  bool shouldNotify(uint32_t canId, uint32_t nowMs);

  bool allowAll() const { return allowAll_; }
  size_t trackedIds() const { return count_; }
  // Ids seen in allow-all mode after the table filled. Diagnostic.
  uint32_t droppedUnknown() const { return droppedUnknown_; }

 private:
  struct Entry {
    uint32_t id = 0;
    uint32_t lastNotifiedMs = 0;
    uint16_t intervalMs = 0;
    bool notified = false;  // lastNotifiedMs means something
  };

  Entry* find(uint32_t canId);
  Entry* add(uint32_t canId, uint16_t intervalMs);
  void clear();

  Entry entries_[kMaxIds];
  size_t count_ = 0;
  uint16_t defaultIntervalMs_ = 0;
  bool allowAll_ = false;
  uint32_t droppedUnknown_ = 0;
};
