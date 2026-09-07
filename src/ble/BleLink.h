#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ble/TelemetryRing.h"

// NimBLE peripheral: advertises, accepts one connection, and pumps batched
// telemetry out as notifications.
//
// Device-only. Never added to [env:native]'s build_src_filter.
class BleLink {
 public:
  // Randomly generated, fixed for the life of the product.
  static constexpr const char* kServiceUuid = "6f4a0001-3f2b-4d15-9c7e-1a2b3c4d5e6f";
  static constexpr const char* kLiveUuid    = "6f4a0002-3f2b-4d15-9c7e-1a2b3c4d5e6f";
  static constexpr const char* kStatusUuid  = "6f4a0003-3f2b-4d15-9c7e-1a2b3c4d5e6f";

  // Largest notification we will build. Sized for a 517-byte MTU less the
  // 3-byte ATT header, rounded down to a whole number of samples.
  static constexpr size_t kMaxNotifyBytes = 500;

  bool begin(const char* deviceName, TelemetryRing& ring);
  void end();

  // Call from loop(). Drains the ring into notifications.
  void tick(uint32_t nowMs);

  bool connected() const { return connected_; }
  uint16_t mtu() const { return mtu_; }
  uint32_t sentBytes() const { return sentBytes_; }

 private:
  TelemetryRing* ring_ = nullptr;
  bool connected_ = false;
  uint16_t mtu_ = 23;  // ATT default until the client negotiates up
  uint32_t sentBytes_ = 0;
};
