#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ble/TelemetryRing.h"

// NimBLE peripheral implementing RaceChrono's published BLE DIY device
// profile (GPS feature only): advertises service 0x1FF8, accepts one
// connection, and notifies the GPS main and time characteristics.
// See docs/reference/racechrono/PROTOCOL.md.
//
// Device-only. Never added to [env:native]'s build_src_filter.
class BleLink {
 public:
  // RaceChrono's published UUIDs -- not ours to choose. 16-bit assigned
  // numbers expanded against the Bluetooth base UUID.
  static constexpr const char* kServiceUuid = "00001ff8-0000-1000-8000-00805f9b34fb";
  static constexpr const char* kGpsMainUuid = "00000003-0000-1000-8000-00805f9b34fb";
  static constexpr const char* kGpsTimeUuid = "00000004-0000-1000-8000-00805f9b34fb";

  bool begin(const char* deviceName, TelemetryRing& ring);
  void end();

  // Call from loop(). Drains the ring and notifies one GPS main-characteristic
  // packet at a time -- RaceChrono expects each notification to be one
  // complete, self-describing 20-byte packet, not a concatenated batch.
  void tick(uint32_t nowMs);

  // Publishes the GPS time characteristic (UUID 0x0004). Unlike the ring-fed
  // main characteristic, this value changes only when the producer's
  // dateAndHour changes (about once an hour of driving), so it needs no
  // buffering: a missed notify just means the app reads the current value
  // instead (the characteristic is READ + NOTIFY).
  void publishTime(const uint8_t bytes[3]);

  bool connected() const { return connected_; }
  uint16_t mtu() const { return mtu_; }
  uint32_t sentBytes() const { return sentBytes_; }

 private:
  TelemetryRing* ring_ = nullptr;
  bool connected_ = false;
  uint16_t mtu_ = 23;  // ATT default until the client negotiates up
  uint32_t sentBytes_ = 0;
};
