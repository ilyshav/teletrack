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
  // Declared as 16-bit, not as the equivalent 128-bit base-UUID strings.
  // Identical at the GATT level, but it changes what goes on the air: a 16-bit
  // service UUID is advertised as AD type 0x03 in 4 bytes, where the 128-bit
  // form is AD type 0x07 in 18 bytes. With 18 bytes plus 3 for flags there is
  // no room left in the 31-byte advertisement for the device name, so it gets
  // pushed into the scan response where a passive scanner never sees it.
  static constexpr uint16_t kServiceUuid16 = 0x1FF8;
  // The CAN half of the profile. RaceChrono configures a DIY device by
  // writing a filter command to 0x0002 on connect regardless of whether it
  // ends up using CAN, so a device missing these is one it cannot set up.
  static constexpr uint16_t kCanMainUuid16 = 0x0001;
  static constexpr uint16_t kCanFilterUuid16 = 0x0002;
  static constexpr uint16_t kGpsMainUuid16 = 0x0003;
  static constexpr uint16_t kGpsTimeUuid16 = 0x0004;

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

  // Sends one CAN frame on characteristic 0x0001. len is 4 + dlc.
  void publishCan(const uint8_t* packet, size_t len);

  // Pops one filter command written by the app. Call from loop() only: the
  // queue is filled on the BLE host task.
  bool takeFilterCommand(uint8_t* out, size_t& len);

  // Filter writes dropped because the queue was full. A dropped command is a
  // channel that never appears in the app, so this is reported, not just kept.
  uint32_t filterOverflows() const;

  bool connected() const { return connected_; }
  uint16_t mtu() const { return mtu_; }
  uint32_t sentBytes() const { return sentBytes_; }

 private:
  TelemetryRing* ring_ = nullptr;
  bool connected_ = false;
  uint16_t mtu_ = 23;  // ATT default until the client negotiates up
  uint32_t sentBytes_ = 0;
};
