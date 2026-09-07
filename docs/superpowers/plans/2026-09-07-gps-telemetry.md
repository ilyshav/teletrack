# Real GPS Telemetry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the synthetic GPS fix on the T-Beam Supreme with its onboard u-blox MAX-M10S, feeding real position and satellite lock state to RaceChrono and showing GPS status on the OLED.

**Architecture:** A pure, host-tested `UbxParser` turns a byte stream into a `GpsFix`; a thin Arduino `GpsReceiver` owns the UART, finds the receiver's baud and configures it. The OLED's log rows become a status screen. The ESP32-S3-DevKitC-1 keeps the synthetic fix and its TFT log untouched.

**Tech Stack:** PlatformIO, Arduino-ESP32 2.0.17 (`espressif32@7.0.1`), C++17, Unity host tests, XPowersLib (AXP2101), U8g2 (SH1106).

**Spec:** `docs/superpowers/specs/2026-09-07-gps-telemetry-design.md`

## Global Constraints

- **All UBX fields are little-endian.** RaceChrono payloads in `src/ble/RaceChronoGps.cpp` are big-endian. Do not copy byte-order idioms between the two.
- **UBX frame:** `B5 62 | class | id | len_lo len_hi | payload | CK_A CK_B`. Checksum is 8-bit Fletcher over class, id, both length bytes and payload: `CK_A += b; CK_B += CK_A;` in wrapping `uint8_t`.
- **UBX-NAV-PVT is class `0x01`, id `0x07`, payload length exactly `92`.**
- **UBX-CFG-VALSET is class `0x06`, id `0x8A`.** Payload prefix is `version=0x00, layers=0x01 (RAM only), reserved 0x00 0x00`, then key/value pairs, key as little-endian `uint32_t`.
- **Config key IDs (verbatim, from SparkFun `u-blox_config_keys.h`):** `CFG-UART1-BAUDRATE 0x40520001` (U4), `CFG-MSGOUT-UBX_NAV_PVT_UART1 0x20910007` (U1), `CFG-RATE-MEAS 0x30210001` (U2), `CFG-RATE-NAV 0x30210002` (U2), NMEA off — GGA `0x209100bb`, GLL `0x209100ca`, GSA `0x209100c0`, GSV `0x209100c5`, RMC `0x209100ac`, VTG `0x209100b1` (all U1).
- **T-Beam pins (vendor-confirmed):** GPS UART RX `9`, TX `8`. GPIO 7 (`GPS_EN`) and GPIO 6 (`PPS`) are deliberately not driven.
- **GPS power is ALDO4 @ 3300 mV.** GNSS RTC backup is VBACKUP @ 3300 mV.
- **Target baud is 115200.** Detection order is `115200, 38400, 9600`, 250 ms each.
- **Max nav rate is 10 Hz.** `Settings::sampleHz` allows `{1, 5, 10, 25}`; 25 clamps to 10 and logs.
- **Nothing is sent to RaceChrono unless `validDate && validTime` are both set** and a client is connected.
- **Never regress the DevKitC.** `pio run -e esp` must keep building and its behaviour must not change.
- **Commit messages must not contain any AI attribution or Co-Authored-By trailer.**
- Keep it simple. This is a POC. No memory pools, no abstractions with one implementation, no validation for failures we have not seen.

## File Structure

| File | Responsibility |
| --- | --- |
| `src/core/GpsFix.h` | **New.** `GpsFix` moves here from `ble/RaceChronoGps.h` so `core/` need not depend on `ble/`. Gains `fixType`. |
| `src/gps/UbxParser.{h,cpp}` | **New.** Pure: byte-feed framing, checksum, NAV-PVT decode. Host-tested. |
| `src/gps/GpsReceiver.{h,cpp}` | **New.** Arduino shell: UART, baud detection, CFG-VALSET, pumping bytes. No tests. |
| `src/board/BoardConfig.h` | Add T-Beam GPS pin constants. |
| `src/board/Pmu.cpp` | Enable ALDO4 + VBACKUP; correct two wrong rail comments. |
| `src/core/DeviceStatus.h` | Add `gpsPresent`, `gpsTimeValid`, `gpsFix`. |
| `src/ui/OledDisplay.{h,cpp}` | Log rows become a GPS status screen. |
| `src/main.cpp` | Wire the receiver in; real fix on T-Beam, synthetic on DevKitC; rate from settings. |
| `platformio.ini` | Add `gps/UbxParser.cpp` to `[env:native]`'s `build_src_filter`. |
| `test/native/test_ubx_parser/test_ubx_parser.cpp` | **New.** Framing, checksum and decode tests. |
| `docs/hardware-notes.md` | Record the rail map and the baud finding. |

---

### Task 1: Move `GpsFix` into `core/` and add `fixType`

`GpsFix` currently lives in `src/ble/RaceChronoGps.h`. `DeviceStatus` (in `core/`) is about to carry one, and `core/` must not depend on `ble/`. It also needs the receiver's raw fix type so the display can say "2D FIX" versus "3D FIX" — `fixQuality` alone cannot distinguish them.

**Files:**
- Create: `src/core/GpsFix.h`
- Modify: `src/ble/RaceChronoGps.h`

**Interfaces:**
- Consumes: nothing.
- Produces: `struct GpsFix` in `src/core/GpsFix.h`, with all existing fields plus `uint8_t fixType = 0;`.

- [ ] **Step 1: Create `src/core/GpsFix.h`**

```cpp
#pragma once

#include <stdint.h>

// A GPS fix in plain units, independent of both the receiver that produced it
// and the protocol that consumes it.
struct GpsFix {
  int32_t latE7 = 0;         // degrees * 10,000,000, signed
  int32_t lonE7 = 0;         // degrees * 10,000,000, signed
  float altitudeM = 0.0f;
  float speedKmh = 0.0f;
  float bearingDeg = 0.0f;
  float hdop = 0.0f;
  // RaceChrono's 2-bit quality: 0 none, 1 GPS, 2 differential.
  uint8_t fixQuality = 0;
  // The receiver's own fix type, kept for display: 0 none, 1 dead reckoning,
  // 2 = 2D, 3 = 3D, 4 GNSS+DR, 5 time only. RaceChrono never sees this.
  uint8_t fixType = 0;
  uint8_t satellites = 0;
  uint16_t year = 2000;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t seconds = 0;
  uint16_t millis = 0;
};
```

- [ ] **Step 2: Replace the struct in `src/ble/RaceChronoGps.h` with an include**

Delete the whole `struct GpsFix { ... };` block and its leading comment, and add the include next to the existing ones so the file starts:

```cpp
#pragma once

#include <stdint.h>

#include "core/GpsFix.h"

// Encodes GpsFix into RaceChrono's BLE DIY GPS characteristics (UUIDs 0x0003
```

Leave the rest of the file, including the whole `class RaceChronoGps`, exactly as it is.

- [ ] **Step 3: Run the host tests to prove the move changed nothing**

Run: `pio test -e native`
Expected: `95 test cases: 95 succeeded`

- [ ] **Step 4: Build both device environments**

Run: `pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both.

- [ ] **Step 5: Commit**

```bash
git add src/core/GpsFix.h src/ble/RaceChronoGps.h
git commit -m "Move GpsFix into core and add the receiver's fix type

DeviceStatus is about to carry a fix, and core must not depend on ble.
fixType is the receiver's own classification, kept so the display can
tell a 2D fix from a 3D one -- RaceChrono's 2-bit quality cannot."
```

---

### Task 2: `UbxParser` framing and checksum

The parser is fed one byte at a time from a UART. It must find frames in a stream that also carries other UBX messages, survive garbage, and reject corrupted frames. Decoding NAV-PVT fields is Task 3 — this task only gets to "a valid NAV-PVT frame arrived".

**Files:**
- Create: `src/gps/UbxParser.h`, `src/gps/UbxParser.cpp`
- Create: `test/native/test_ubx_parser/test_ubx_parser.cpp`
- Modify: `platformio.ini` (`[env:native]` `build_src_filter`)

**Interfaces:**
- Consumes: `GpsFix` from `src/core/GpsFix.h` (Task 1).
- Produces:
  - `class UbxParser` with `bool feed(uint8_t byte)`, `const GpsFix& fix() const`, `bool timeValid() const`, `uint32_t checksumErrors() const`.
  - Public constants `UbxParser::kClassNav = 0x01`, `kIdPvt = 0x07`, `kPvtLength = 92`.
  - `feed()` returns `true` exactly once per accepted NAV-PVT frame, on the byte that completes it.

- [ ] **Step 1: Add the parser to the native build**

In `platformio.ini`, in `[env:native]`, append one line to `build_src_filter`, after `+<ble/RaceChronoGps.cpp>`:

```ini
    +<gps/UbxParser.cpp>
```

- [ ] **Step 2: Write the failing test**

Create `test/native/test_ubx_parser/test_ubx_parser.cpp`:

```cpp
#include <string.h>
#include <unity.h>

#include "gps/UbxParser.h"

void setUp() {}
void tearDown() {}

namespace {

void putU2(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU4(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

void putI4(uint8_t* p, int32_t v) { putU4(p, static_cast<uint32_t>(v)); }

// The NAV-PVT fields the parser reads, with a plausible default for each.
struct PvtFields {
  uint16_t year = 2026;
  uint8_t month = 9;
  uint8_t day = 7;
  uint8_t hour = 14;
  uint8_t min = 23;
  uint8_t sec = 45;
  uint8_t valid = 0x03;        // validDate | validTime
  int32_t nano = 500000000;    // 500 ms
  uint8_t fixType = 3;         // 3D
  uint8_t flags = 0x01;        // gnssFixOK
  uint8_t numSV = 9;
  int32_t lon = 48952100;      // 4.89521 deg
  int32_t lat = 523713400;     // 52.37134 deg
  int32_t hMSL = 12000;        // mm -> 12 m
  int32_t gSpeed = 13389;      // mm/s -> 48.2004 km/h
  int32_t headMot = 12345000;  // 1e-5 deg -> 123.45
  uint16_t pDOP = 90;          // 0.9
};

void buildPvt(uint8_t out[92], const PvtFields& f) {
  memset(out, 0, 92);
  putU4(out + 0, 123456);  // iTOW, never read
  putU2(out + 4, f.year);
  out[6] = f.month;
  out[7] = f.day;
  out[8] = f.hour;
  out[9] = f.min;
  out[10] = f.sec;
  out[11] = f.valid;
  putI4(out + 16, f.nano);
  out[20] = f.fixType;
  out[21] = f.flags;
  out[23] = f.numSV;
  putI4(out + 24, f.lon);
  putI4(out + 28, f.lat);
  putI4(out + 36, f.hMSL);
  putI4(out + 60, f.gSpeed);
  putI4(out + 64, f.headMot);
  putU2(out + 76, f.pDOP);
}

struct Frame {
  uint8_t bytes[256] = {};
  size_t len = 0;
};

// Wraps a payload in sync bytes, header and a correct Fletcher checksum.
Frame makeFrame(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t plen) {
  Frame f;
  f.bytes[0] = 0xB5;
  f.bytes[1] = 0x62;
  f.bytes[2] = cls;
  f.bytes[3] = id;
  f.bytes[4] = static_cast<uint8_t>(plen);
  f.bytes[5] = static_cast<uint8_t>(plen >> 8);
  for (uint16_t i = 0; i < plen; ++i) {
    f.bytes[6 + i] = payload[i];
  }
  uint8_t a = 0;
  uint8_t b = 0;
  for (size_t i = 2; i < 6u + plen; ++i) {
    a += f.bytes[i];
    b += a;
  }
  f.bytes[6 + plen] = a;
  f.bytes[7 + plen] = b;
  f.len = 8u + plen;
  return f;
}

Frame makePvtFrame(const PvtFields& f = PvtFields{}) {
  uint8_t payload[92];
  buildPvt(payload, f);
  return makeFrame(UbxParser::kClassNav, UbxParser::kIdPvt, payload, 92);
}

// Feeds every byte, returning how many frames the parser accepted.
int feedAll(UbxParser& p, const uint8_t* bytes, size_t len) {
  int accepted = 0;
  for (size_t i = 0; i < len; ++i) {
    if (p.feed(bytes[i])) {
      ++accepted;
    }
  }
  return accepted;
}

}  // namespace

static void test_a_valid_frame_is_accepted_on_its_last_byte() {
  UbxParser parser;
  const Frame f = makePvtFrame();
  for (size_t i = 0; i + 1 < f.len; ++i) {
    TEST_ASSERT_FALSE(parser.feed(f.bytes[i]));
  }
  TEST_ASSERT_TRUE(parser.feed(f.bytes[f.len - 1]));
  TEST_ASSERT_EQUAL_UINT32(0, parser.checksumErrors());
}

static void test_a_flipped_payload_byte_is_rejected() {
  UbxParser parser;
  Frame f = makePvtFrame();
  f.bytes[30] ^= 0xFF;  // inside the payload, so the checksum no longer matches
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT32(1, parser.checksumErrors());
}

static void test_leading_garbage_is_skipped() {
  UbxParser parser;
  const uint8_t junk[] = {0x00, 0xFF, 0x24, 0x47, 0xB5, 0x01, 0x7E};
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, junk, sizeof(junk)));

  const Frame f = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
}

static void test_a_truncated_frame_does_not_block_the_next_one() {
  UbxParser parser;
  const Frame truncated = makePvtFrame();
  // Half a frame, then a whole one. The parser must recover.
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, truncated.bytes, 40));

  const Frame good = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, good.bytes, good.len));
}

static void test_doubled_sync_byte_still_syncs() {
  UbxParser parser;
  // 0xB5 0xB5 0x62 ... -- the second 0xB5 must restart the sync, not consume
  // the 0x62 that follows.
  const Frame f = makePvtFrame();
  uint8_t stream[300];
  stream[0] = 0xB5;
  memcpy(stream + 1, f.bytes, f.len);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, stream, f.len + 1));
}

static void test_another_ubx_message_is_consumed_but_not_reported() {
  UbxParser parser;
  const uint8_t ackPayload[2] = {0x06, 0x8A};
  const Frame ack = makeFrame(0x05, 0x01, ackPayload, 2);  // UBX-ACK-ACK
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, ack.bytes, ack.len));

  const Frame pvt = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, pvt.bytes, pvt.len));
}

static void test_a_message_longer_than_navpvt_does_not_overflow() {
  UbxParser parser;
  uint8_t big[200];
  memset(big, 0xAB, sizeof(big));
  const Frame f = makeFrame(0x01, 0x35, big, sizeof(big));  // UBX-NAV-SAT
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, f.bytes, f.len));

  const Frame pvt = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, pvt.bytes, pvt.len));
}

static void test_a_navpvt_of_the_wrong_length_is_rejected() {
  UbxParser parser;
  uint8_t shortPayload[40];
  memset(shortPayload, 0, sizeof(shortPayload));
  const Frame f = makeFrame(UbxParser::kClassNav, UbxParser::kIdPvt,
                            shortPayload, sizeof(shortPayload));
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, f.bytes, f.len));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_valid_frame_is_accepted_on_its_last_byte);
  RUN_TEST(test_a_flipped_payload_byte_is_rejected);
  RUN_TEST(test_leading_garbage_is_skipped);
  RUN_TEST(test_a_truncated_frame_does_not_block_the_next_one);
  RUN_TEST(test_doubled_sync_byte_still_syncs);
  RUN_TEST(test_another_ubx_message_is_consumed_but_not_reported);
  RUN_TEST(test_a_message_longer_than_navpvt_does_not_overflow);
  RUN_TEST(test_a_navpvt_of_the_wrong_length_is_rejected);
  return UNITY_END();
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `pio test -e native -f native/test_ubx_parser`
Expected: build failure — `fatal error: gps/UbxParser.h: No such file or directory`.

- [ ] **Step 4: Write `src/gps/UbxParser.h`**

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/GpsFix.h"

// Finds UBX-NAV-PVT messages in a byte stream and decodes them into a GpsFix.
//
// Pure: no Arduino, no UART, no clock. Fed one byte at a time so it never
// needs a buffer of its own beyond the frame being assembled, and so a frame
// split across two UART reads costs nothing to handle.
//
// UBX is LITTLE-endian throughout. RaceChrono's payloads next door are
// big-endian; do not copy byte-order idioms between the two.
class UbxParser {
 public:
  static constexpr uint8_t kSync1 = 0xB5;
  static constexpr uint8_t kSync2 = 0x62;
  static constexpr uint8_t kClassNav = 0x01;
  static constexpr uint8_t kIdPvt = 0x07;
  static constexpr size_t kPvtLength = 92;

  // Feeds one byte. Returns true on the byte that completes a valid NAV-PVT
  // frame, at which point fix() and timeValid() hold the decoded result.
  bool feed(uint8_t byte);

  const GpsFix& fix() const { return fix_; }

  // True when the receiver reported both validDate and validTime. Until it
  // does, its clock is not to be trusted and nothing should be transmitted.
  bool timeValid() const { return timeValid_; }

  // Frames whose checksum did not match. Diagnostic only.
  uint32_t checksumErrors() const { return checksumErrors_; }

 private:
  enum class State : uint8_t {
    Sync1, Sync2, Class, Id, LenLo, LenHi, Payload, CkA, CkB
  };

  void decodePvt();

  State state_ = State::Sync1;
  uint8_t class_ = 0;
  uint8_t id_ = 0;
  uint16_t length_ = 0;
  uint16_t index_ = 0;
  // Only NAV-PVT is stored. Longer messages still run through the state
  // machine -- they must be consumed to stay in sync -- but their payload is
  // discarded rather than overflowing this.
  uint8_t payload_[kPvtLength] = {};
  uint8_t ckA_ = 0;
  uint8_t ckB_ = 0;
  uint8_t rxCkA_ = 0;
  GpsFix fix_;
  bool timeValid_ = false;
  uint32_t checksumErrors_ = 0;
};
```

- [ ] **Step 5: Write `src/gps/UbxParser.cpp` with framing only**

`decodePvt()` is a stub in this task; Task 3 fills it in.

```cpp
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
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `pio test -e native -f native/test_ubx_parser`
Expected: `10 test cases: 10 succeeded`

- [ ] **Step 7: Run the whole suite**

Run: `pio test -e native`
Expected: `105 test cases: 105 succeeded`

- [ ] **Step 8: Commit**

```bash
git add platformio.ini src/gps/UbxParser.h src/gps/UbxParser.cpp test/native/test_ubx_parser/test_ubx_parser.cpp
git commit -m "Add UBX frame parsing with checksum verification

Byte-at-a-time so a frame split across UART reads costs nothing, and so
the parser needs no buffer beyond the frame being assembled. Messages
longer than NAV-PVT are consumed to stay in sync but not stored."
```

---

### Task 3: Decode NAV-PVT into a `GpsFix`

**Files:**
- Modify: `src/gps/UbxParser.cpp` (`decodePvt`)
- Modify: `test/native/test_ubx_parser/test_ubx_parser.cpp`

**Interfaces:**
- Consumes: `UbxParser` framing (Task 2), `GpsFix` (Task 1).
- Produces: after `feed()` returns true, `fix()` holds every field in the table below and `timeValid()` reflects the receiver's flags.

| Offset | Type | Field | Maps to |
| --- | --- | --- | --- |
| 4 | U2 | year | `year` |
| 6 | U1 | month | `month` |
| 7 | U1 | day | `day` |
| 8 | U1 | hour | `hour` |
| 9 | U1 | min | `minute` |
| 10 | U1 | sec | `seconds` |
| 11 | X1 | valid | bit0 validDate, bit1 validTime |
| 16 | I4 | nano | `millis` = clamp(nano / 1e6, 0, 999) |
| 20 | U1 | fixType | `fixType`, and `fixQuality` via the rule below |
| 21 | X1 | flags | bit0 gnssFixOK, bit1 diffSoln |
| 23 | U1 | numSV | `satellites` |
| 24 | I4 | lon | `lonE7` |
| 28 | I4 | lat | `latE7` |
| 36 | I4 | hMSL | `altitudeM` = hMSL / 1000 |
| 60 | I4 | gSpeed | `speedKmh` = gSpeed * 0.0036 |
| 64 | I4 | headMot | `bearingDeg` = headMot / 100000 |
| 76 | U2 | pDOP | `hdop` = pDOP / 100 |

Fix quality: `gnssFixOK` clear, or `fixType` not in `{2, 3, 4}` gives `0`. Otherwise `2` when `diffSoln` is set, else `1`.

- [ ] **Step 1: Write the failing tests**

Add these to `test/native/test_ubx_parser/test_ubx_parser.cpp`, before `int main`:

```cpp
static void test_every_field_decodes() {
  UbxParser parser;
  const Frame f = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));

  const GpsFix& fix = parser.fix();
  TEST_ASSERT_EQUAL_UINT16(2026, fix.year);
  TEST_ASSERT_EQUAL_UINT8(9, fix.month);
  TEST_ASSERT_EQUAL_UINT8(7, fix.day);
  TEST_ASSERT_EQUAL_UINT8(14, fix.hour);
  TEST_ASSERT_EQUAL_UINT8(23, fix.minute);
  TEST_ASSERT_EQUAL_UINT8(45, fix.seconds);
  TEST_ASSERT_EQUAL_UINT16(500, fix.millis);
  TEST_ASSERT_EQUAL_UINT8(3, fix.fixType);
  TEST_ASSERT_EQUAL_UINT8(1, fix.fixQuality);
  TEST_ASSERT_EQUAL_UINT8(9, fix.satellites);
  TEST_ASSERT_EQUAL_INT32(523713400, fix.latE7);
  TEST_ASSERT_EQUAL_INT32(48952100, fix.lonE7);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, fix.altitudeM);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 48.2f, fix.speedKmh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 123.45f, fix.bearingDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, fix.hdop);
  TEST_ASSERT_TRUE(parser.timeValid());
}

static void test_negative_latitude_and_longitude_are_signed() {
  PvtFields fields;
  fields.lat = -523713400;
  fields.lon = -48952100;
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_INT32(-523713400, parser.fix().latE7);
  TEST_ASSERT_EQUAL_INT32(-48952100, parser.fix().lonE7);
}

static void test_no_fix_reports_quality_zero_but_keeps_satellites() {
  PvtFields fields;
  fields.fixType = 0;
  fields.flags = 0x00;
  fields.numSV = 4;
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT8(0, parser.fix().fixQuality);
  // The count is what tells you the receiver is alive while you wait.
  TEST_ASSERT_EQUAL_UINT8(4, parser.fix().satellites);
}

static void test_two_d_fix_is_quality_one() {
  PvtFields fields;
  fields.fixType = 2;
  fields.flags = 0x01;
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT8(1, parser.fix().fixQuality);
  TEST_ASSERT_EQUAL_UINT8(2, parser.fix().fixType);
}

static void test_differential_fix_is_quality_two() {
  PvtFields fields;
  fields.fixType = 3;
  fields.flags = 0x03;  // gnssFixOK | diffSoln
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT8(2, parser.fix().fixQuality);
}

static void test_gnss_fix_ok_clear_overrides_a_good_fix_type() {
  PvtFields fields;
  fields.fixType = 3;
  fields.flags = 0x00;  // gnssFixOK clear
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT8(0, parser.fix().fixQuality);
}

static void test_time_valid_requires_both_date_and_time_flags() {
  UbxParser parser;

  PvtFields dateOnly;
  dateOnly.valid = 0x01;
  const Frame a = makePvtFrame(dateOnly);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, a.bytes, a.len));
  TEST_ASSERT_FALSE(parser.timeValid());

  PvtFields timeOnly;
  timeOnly.valid = 0x02;
  const Frame b = makePvtFrame(timeOnly);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, b.bytes, b.len));
  TEST_ASSERT_FALSE(parser.timeValid());

  PvtFields both;
  both.valid = 0x03;
  const Frame c = makePvtFrame(both);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, c.bytes, c.len));
  TEST_ASSERT_TRUE(parser.timeValid());
}

static void test_negative_nano_yields_zero_millis() {
  PvtFields fields;
  fields.nano = -250000000;  // the fix is 250 ms before the reported second
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT16(0, parser.fix().millis);
}

static void test_maximum_nano_clamps_to_999_millis() {
  PvtFields fields;
  fields.nano = 999999999;
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT16(999, parser.fix().millis);
}
```

And register them in `main`, after `RUN_TEST(test_a_navpvt_of_the_wrong_length_is_rejected);`:

```cpp
  RUN_TEST(test_every_field_decodes);
  RUN_TEST(test_negative_latitude_and_longitude_are_signed);
  RUN_TEST(test_no_fix_reports_quality_zero_but_keeps_satellites);
  RUN_TEST(test_two_d_fix_is_quality_one);
  RUN_TEST(test_differential_fix_is_quality_two);
  RUN_TEST(test_gnss_fix_ok_clear_overrides_a_good_fix_type);
  RUN_TEST(test_time_valid_requires_both_date_and_time_flags);
  RUN_TEST(test_negative_nano_yields_zero_millis);
  RUN_TEST(test_maximum_nano_clamps_to_999_millis);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pio test -e native -f native/test_ubx_parser`
Expected: the nine new tests FAIL (`decodePvt` is still empty, so every field reads as its default), the eight framing tests still pass.

- [ ] **Step 3: Implement `decodePvt`**

Replace the stub in `src/gps/UbxParser.cpp`, and add the anonymous namespace above `UbxParser::feed`:

```cpp
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
```

```cpp
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pio test -e native -f native/test_ubx_parser`
Expected: `19 test cases: 19 succeeded`

- [ ] **Step 5: Run the whole suite**

Run: `pio test -e native`
Expected: `114 test cases: 114 succeeded`

- [ ] **Step 6: Commit**

```bash
git add src/gps/UbxParser.cpp test/native/test_ubx_parser/test_ubx_parser.cpp
git commit -m "Decode UBX-NAV-PVT into a GpsFix

One message carries the whole fix, so there is no risk of stitching
together two sentences that disagree. Fix quality needs gnssFixOK as
well as a usable fixType; HDOP is substituted with pDOP, which NAV-PVT
does carry and which is pessimistic rather than wrong."
```

---

### Task 4: GPS pins and power rails

The GPS sits on ALDO4, which the firmware has never enabled. The two rail comments currently in `Pmu.cpp` are both wrong for this board.

**Files:**
- Modify: `src/board/BoardConfig.h`
- Modify: `src/board/Pmu.cpp`
- Modify: `docs/hardware-notes.md`

**Interfaces:**
- Consumes: nothing.
- Produces: `BoardConfig::kGpsRxPin` (`uint8_t`, 9) and `BoardConfig::kGpsTxPin` (`uint8_t`, 8), defined only under `BOARD_TBEAM`.

- [ ] **Step 1: Add the GPS pins to `src/board/BoardConfig.h`**

In the `#if defined(BOARD_TBEAM)` branch, after the `kPmuScl` definition:

```cpp
// u-blox MAX-M10S UART, from LilyGO's own board support for this board.
// Named from the ESP32's point of view: kGpsRxPin receives the module's TX.
inline constexpr uint8_t kGpsRxPin = 9;
inline constexpr uint8_t kGpsTxPin = 8;
// GPIO 7 is GPS_EN and GPIO 6 is PPS. LilyGO's own code drives neither, so
// neither is defined here. If the receiver is silent at every baud with
// ALDO4 confirmed on, GPIO 7 is the first thing to try.
```

Leave the `BOARD_DEVKITC` branch alone — that board has no receiver, and code reaching for these pins there should fail to compile.

- [ ] **Step 2: Enable ALDO4 and VBACKUP in `src/board/Pmu.cpp`**

Replace this block:

```cpp
  // The board powers these up on its own -- every rail read as already enabled
  // before this ran -- so this sets the voltages rather than rescuing a dark
  // panel. A dark screen here is an I2C address problem, not a power one.
  g_pmu.setALDO2Voltage(3300);  // display
  g_pmu.enableALDO2();
  g_pmu.setALDO3Voltage(3300);  // GPS -- unused in this branch, powered anyway
  g_pmu.enableALDO3();
```

with:

```cpp
  // Rail map for the T-Beam S3 Supreme, from LilyGO's own board support:
  //   ALDO1 sensors   ALDO2 SD card   ALDO3 LoRa   ALDO4 GPS
  //   VBACKUP GNSS RTC   DCDC1 ESP32 VDD (protected, never disable)
  // The earlier labels here said ALDO2 was the display and ALDO3 the GPS.
  // Both were wrong. The display is on none of them, which is why the dark
  // panel turned out to be an I2C address problem rather than a power one.
  g_pmu.setALDO4Voltage(3300);
  g_pmu.enableALDO4();
  // Keeps the GNSS RTC and its almanac alive across power cycles. Without it
  // every start is a cold start: minutes to first fix instead of seconds.
  g_pmu.setButtonBatteryChargeVoltage(3300);
  g_pmu.enableButtonBatteryCharge();
```

- [ ] **Step 3: Build the T-Beam environment**

Run: `pio run -e tbeam`
Expected: `SUCCESS`. Both method names are confirmed present in the installed XPowersLib (`XPowersAXP2101.tpp:550` and `:567`); the AXP2101 calls VBACKUP the "button battery".

- [ ] **Step 4: Build both environments**

Run: `pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both. The DevKitC path compiles `Pmu.cpp`'s `#else` branch and is unaffected.

- [ ] **Step 5: Record the rail map in `docs/hardware-notes.md`**

Add this section immediately before the `## Serial` heading:

```markdown
## T-Beam Supreme power rails

From LilyGO's own board support (Xinyuan-LilyGO/LilyGo-LoRa-Series,
`examples/.../LoRaBoards.cpp`), not from guesswork:

| Rail | Feeds |
| --- | --- |
| ALDO1 | sensors (magnetometer, BME280) |
| ALDO2 | SD card |
| ALDO3 | LoRa |
| **ALDO4** | **GPS** |
| VBACKUP | GNSS RTC backup — without it every start is a cold start |
| DCDC1 | ESP32 VDD — protected, never disable |

**The display is on none of them.** An earlier version of `Pmu.cpp` labelled
ALDO2 "display" and ALDO3 "GPS"; both were wrong, and hours went into power
theories for a dark panel whose actual fault was an I2C address collision.

### GPS pins

`GPS_RX_PIN 9`, `GPS_TX_PIN 8` (ESP32 side), `GPS_EN_PIN 7`, `GPS_PPS_PIN 6`.
LilyGO's code never drives GPS_EN or PPS, so neither does ours.
```

- [ ] **Step 6: Commit**

```bash
git add src/board/BoardConfig.h src/board/Pmu.cpp docs/hardware-notes.md
git commit -m "Power the GPS on ALDO4 and correct the rail map

GPS is on ALDO4, which the firmware has never enabled. The comments here
claimed ALDO2 was the display and ALDO3 the GPS; both were wrong, and the
display is on neither -- which is why the dark panel turned out to be an
I2C address collision rather than a power fault.

VBACKUP keeps the GNSS RTC alive across power cycles, turning every start
from a cold start into a warm one."
```

---

### Task 5: `GpsReceiver` — UART, baud detection and configuration

**Files:**
- Create: `src/gps/GpsReceiver.h`, `src/gps/GpsReceiver.cpp`
- Modify: `docs/hardware-notes.md`

**Interfaces:**
- Consumes: `UbxParser` (Tasks 2-3), `BoardConfig::kGpsRxPin` / `kGpsTxPin` (Task 4).
- Produces: `class GpsReceiver` with:
  - `bool begin(uint8_t rateHz)` — detects baud, configures the module, returns false when nothing answers.
  - `bool tick()` — pumps available bytes, returns true when a new NAV-PVT arrived.
  - `const GpsFix& fix() const`, `bool timeValid() const`, `bool present() const`, `uint8_t rateHz() const`.
  - `static constexpr uint8_t kMaxRateHz = 10;`

- [ ] **Step 1: Write `src/gps/GpsReceiver.h`**

```cpp
#pragma once

#include <stdint.h>

#include "core/GpsFix.h"
#include "gps/UbxParser.h"

// The u-blox MAX-M10S on the T-Beam Supreme. Owns Serial1, finds the baud the
// module is currently using, configures it to emit UBX-NAV-PVT, and pumps the
// bytes into UbxParser.
//
// On boards with no receiver this compiles to a stub whose begin() returns
// false, exactly like Pmu.
class GpsReceiver {
 public:
  // The MAX-M10S tops out at 10 Hz with more than one constellation enabled.
  static constexpr uint8_t kMaxRateHz = 10;

  // Opens the UART, finds the module, and configures it for rateHz. Returns
  // false when nothing answers at any baud -- a dead receiver must not stop
  // the rest of the firmware.
  bool begin(uint8_t rateHz);

  // Call from loop(). Returns true when a new NAV-PVT was decoded.
  bool tick();

  const GpsFix& fix() const { return parser_.fix(); }
  bool timeValid() const { return parser_.timeValid(); }
  bool present() const { return present_; }
  uint8_t rateHz() const { return rateHz_; }

 private:
  UbxParser parser_;
  bool present_ = false;
  uint8_t rateHz_ = 0;
  uint32_t frames_ = 0;
};
```

- [ ] **Step 2: Write `src/gps/GpsReceiver.cpp`**

```cpp
#include "gps/GpsReceiver.h"

#include "core/Log.h"

#if defined(BOARD_TBEAM)

#include <Arduino.h>

#include "board/BoardConfig.h"

namespace {

// The module's current baud is genuinely unknown: LilyGO's board support says
// 9600, u-blox M10 parts default to 38400, and LilyGO ship a recovery sketch
// that sweeps bauds precisely because it varies. We also set 115200 ourselves
// in the RAM layer, which survives a warm reset but not a power cycle. So all
// three are plausible on any given boot.
constexpr uint32_t kBaudCandidates[] = {115200, 38400, 9600};
constexpr uint32_t kTargetBaud = 115200;
constexpr uint32_t kProbeMs = 250;

constexpr uint8_t kClassCfg = 0x06;
constexpr uint8_t kIdValset = 0x8A;

// Config keys, from SparkFun's u-blox_config_keys.h.
constexpr uint32_t kKeyUart1Baud = 0x40520001;    // U4
constexpr uint32_t kKeyNavPvtUart1 = 0x20910007;  // U1
constexpr uint32_t kKeyRateMeas = 0x30210001;     // U2
constexpr uint32_t kKeyRateNav = 0x30210002;      // U2
constexpr uint32_t kKeyNmeaOff[] = {
    0x209100bb,  // GGA
    0x209100ca,  // GLL
    0x209100c0,  // GSA
    0x209100c5,  // GSV
    0x209100ac,  // RMC
    0x209100b1,  // VTG
};

// Wraps a payload in sync bytes, header and Fletcher checksum, and writes it.
void sendUbx(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  const uint8_t header[6] = {0xB5, 0x62, cls, id,
                             static_cast<uint8_t>(len),
                             static_cast<uint8_t>(len >> 8)};
  uint8_t ckA = 0;
  uint8_t ckB = 0;
  for (size_t i = 2; i < 6; ++i) {
    ckA += header[i];
    ckB += ckA;
  }
  for (uint16_t i = 0; i < len; ++i) {
    ckA += payload[i];
    ckB += ckA;
  }
  const uint8_t checksum[2] = {ckA, ckB};

  Serial1.write(header, sizeof(header));
  Serial1.write(payload, len);
  Serial1.write(checksum, sizeof(checksum));
  Serial1.flush();
}

// CFG-VALSET header: version 0, RAM layer only, two reserved bytes. RAM means
// the module reverts to its own defaults on a power cycle, so we reconfigure
// every boot -- no flash wear, and nothing left permanently altered.
size_t beginValset(uint8_t* p) {
  p[0] = 0x00;
  p[1] = 0x01;
  p[2] = 0x00;
  p[3] = 0x00;
  return 4;
}

size_t addKey(uint8_t* p, size_t n, uint32_t key) {
  p[n++] = static_cast<uint8_t>(key);
  p[n++] = static_cast<uint8_t>(key >> 8);
  p[n++] = static_cast<uint8_t>(key >> 16);
  p[n++] = static_cast<uint8_t>(key >> 24);
  return n;
}

size_t addU1(uint8_t* p, size_t n, uint32_t key, uint8_t value) {
  n = addKey(p, n, key);
  p[n++] = value;
  return n;
}

size_t addU2(uint8_t* p, size_t n, uint32_t key, uint16_t value) {
  n = addKey(p, n, key);
  p[n++] = static_cast<uint8_t>(value);
  p[n++] = static_cast<uint8_t>(value >> 8);
  return n;
}

size_t addU4(uint8_t* p, size_t n, uint32_t key, uint32_t value) {
  n = addKey(p, n, key);
  p[n++] = static_cast<uint8_t>(value);
  p[n++] = static_cast<uint8_t>(value >> 8);
  p[n++] = static_cast<uint8_t>(value >> 16);
  p[n++] = static_cast<uint8_t>(value >> 24);
  return n;
}

// Opens Serial1 at baud and reports whether anything arrives within kProbeMs.
bool probe(uint32_t baud) {
  Serial1.begin(baud, SERIAL_8N1, BoardConfig::kGpsRxPin, BoardConfig::kGpsTxPin);
  const uint32_t deadline = millis() + kProbeMs;
  while (millis() < deadline) {
    if (Serial1.available() > 0) {
      return true;
    }
    delay(5);
  }
  Serial1.end();
  return false;
}

}  // namespace

bool GpsReceiver::begin(uint8_t rateHz) {
  if (rateHz == 0) {
    rateHz = 1;
  }
  if (rateHz > kMaxRateHz) {
    Log::warn("gps", "%u Hz is above the MAX-M10S limit, using %u Hz",
              static_cast<unsigned>(rateHz), static_cast<unsigned>(kMaxRateHz));
    rateHz = kMaxRateHz;
  }

  uint32_t found = 0;
  for (uint32_t baud : kBaudCandidates) {
    if (probe(baud)) {
      found = baud;
      break;
    }
  }
  if (found == 0) {
    Log::error("gps", "no response at any baud");
    present_ = false;
    return false;
  }
  Log::info("gps", "receiver answering at %lu baud", static_cast<unsigned long>(found));

  // Raise the baud first. NAV-PVT is 100 bytes on the wire, so 10 Hz needs
  // 1000 B/s and 9600 baud 8N1 carries only 960 -- it does not fit.
  if (found != kTargetBaud) {
    uint8_t p[16];
    size_t n = beginValset(p);
    n = addU4(p, n, kKeyUart1Baud, kTargetBaud);
    sendUbx(kClassCfg, kIdValset, p, static_cast<uint16_t>(n));
    delay(100);  // let the module finish the reply at the old rate
    Serial1.updateBaudRate(kTargetBaud);
    delay(100);
  }

  // One VALSET per concern, so a key the module rejects cannot take the
  // others down with it.
  {
    uint8_t p[64];
    size_t n = beginValset(p);
    for (uint32_t key : kKeyNmeaOff) {
      n = addU1(p, n, key, 0);
    }
    sendUbx(kClassCfg, kIdValset, p, static_cast<uint16_t>(n));
  }
  {
    uint8_t p[16];
    size_t n = beginValset(p);
    n = addU1(p, n, kKeyNavPvtUart1, 1);
    sendUbx(kClassCfg, kIdValset, p, static_cast<uint16_t>(n));
  }
  {
    uint8_t p[24];
    size_t n = beginValset(p);
    n = addU2(p, n, kKeyRateMeas, static_cast<uint16_t>(1000u / rateHz));
    n = addU2(p, n, kKeyRateNav, 1);
    sendUbx(kClassCfg, kIdValset, p, static_cast<uint16_t>(n));
  }

  rateHz_ = rateHz;
  present_ = true;
  // Deliberately no ACK parsing: NAV-PVT frames actually arriving is stronger
  // evidence than an ACK, and much less code. See the log line in tick().
  Log::info("gps", "configured for UBX-NAV-PVT at %u Hz",
            static_cast<unsigned>(rateHz));
  return true;
}

bool GpsReceiver::tick() {
  if (!present_) {
    return false;
  }
  bool decoded = false;
  // Bounded so one call cannot monopolise loop() if the buffer has backed up.
  for (int i = 0; i < 512 && Serial1.available() > 0; ++i) {
    if (parser_.feed(static_cast<uint8_t>(Serial1.read()))) {
      decoded = true;
      if (frames_++ == 0) {
        Log::info("gps", "first NAV-PVT decoded, %u satellites",
                  static_cast<unsigned>(parser_.fix().satellites));
      }
    }
  }
  return decoded;
}

#else

bool GpsReceiver::begin(uint8_t rateHz) {
  (void)rateHz;
  present_ = false;
  return false;  // no receiver on this board
}

bool GpsReceiver::tick() { return false; }

#endif
```

- [ ] **Step 3: Build both environments**

Run: `pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both.

- [ ] **Step 4: Confirm the parser stayed out of the native build's way**

Run: `pio test -e native`
Expected: `114 test cases: 114 succeeded`. `GpsReceiver.cpp` is not in `[env:native]`'s `build_src_filter` and must not be added — it includes Arduino.

- [ ] **Step 5: Record the baud finding in `docs/hardware-notes.md`**

Add this immediately after the `### GPS pins` block from Task 4:

```markdown
### The GPS baud rate is not knowable in advance

LilyGO's board support defines `GPS_BAUD_RATE 9600`, u-blox M10 modules leave
the factory at 38400, and LilyGO ship a recovery sketch that sweeps
`{9600, 19200, 38400, 57600, 115200, ...}` precisely because it varies in the
field. Our own firmware sets 115200 in the RAM layer, which survives a warm
reset but not a power cycle.

`GpsReceiver::begin()` therefore probes 115200, then 38400, then 9600, for
250 ms each, and logs which answered. A silent receiver logs
`gps: no response at any baud` and everything else carries on.

Configuration goes to the **RAM layer only** (`CFG-VALSET layers = 0x01`), so
the module is never permanently altered and a power cycle returns it to its
own defaults.
```

- [ ] **Step 6: Commit**

```bash
git add src/gps/GpsReceiver.h src/gps/GpsReceiver.cpp docs/hardware-notes.md
git commit -m "Add the MAX-M10S receiver driver

Probes 115200/38400/9600 because the module's baud genuinely varies --
LilyGO's own recovery sketch sweeps bauds for the same reason. Raises it
to 115200 because NAV-PVT at 10 Hz needs 1000 B/s and 9600 carries 960.

Configuration goes to the RAM layer only, so a power cycle leaves the
module exactly as it was found. Success is proven by frames arriving
rather than by parsing ACKs."
```

---

### Task 6: GPS status on the OLED

The log rows become a status screen. Serial keeps the full log.

**Files:**
- Modify: `src/core/DeviceStatus.h`
- Modify: `src/ui/OledDisplay.h`, `src/ui/OledDisplay.cpp`

**Interfaces:**
- Consumes: `GpsFix` (Task 1).
- Produces: `DeviceStatus::gpsPresent` (`bool`), `DeviceStatus::gpsTimeValid` (`bool`), `DeviceStatus::gpsFix` (`GpsFix`), all populated by Task 7.

- [ ] **Step 1: Add the GPS fields to `src/core/DeviceStatus.h`**

Add the include after `#include <stdint.h>`:

```cpp
#include "core/GpsFix.h"
```

and these fields at the end of the struct, after `holdMs`:

```cpp
  // GPS. gpsPresent is false when no receiver answered at boot, which the
  // display shows as "NO GPS" rather than a satellite count of zero.
  bool gpsPresent = false;
  bool gpsTimeValid = false;
  GpsFix gpsFix;
```

- [ ] **Step 2: Drop the log constants from `src/ui/OledDisplay.h`**

Update the class comment:

```cpp
// SH1106 128x64 over I2C. At the 6x8 font that is 21 columns by 8 rows: one
// header row in inverse video, then rows of GPS status. The scrolling
// log that used to live here is gone -- on 21 columns it showed six useful
// characters per line, and serial carries the full log with timestamps.
```

```cpp
  static constexpr size_t kHeaderRows = 1;
```

Delete `kLogRows` outright rather than renaming it — the status rows are addressed individually, so a count constant would have no reader.

Then remove the now-unused console member and its include:

- Delete `#include "core/LogRing.h"`
- Delete `LogRing console_;`
- Delete `uint32_t drawnRevision_ = 0;`

In `OledDisplay.cpp`, `<string.h>` becomes unused once the log loop goes -- nothing left calls `strlen` or `strcmp` -- so remove that include too. `core/Log.h`, `core/Format.h` and `radio/HoldDetector.h` are all still used and stay.

- [ ] **Step 3: Rewrite the status area in `src/ui/OledDisplay.cpp`**

Replace the whole `for (size_t i = 0; i < kLogRows; ++i) { ... }` loop, up to but not including `u8g2_.sendBuffer();`, with:

```cpp
  // Row 1: satellite count and fix state. The count is the number that
  // answers "is this thing working yet", so it is always on the left.
  char line[kCols + 1];
  const GpsFix& fix = status.gpsFix;

  char sats[12];
  if (!status.gpsPresent) {
    snprintf(sats, sizeof(sats), "SATS --");
  } else {
    snprintf(sats, sizeof(sats), "SATS %02u", static_cast<unsigned>(fix.satellites));
  }

  const char* state = !status.gpsPresent ? "NO GPS"
                      : fix.fixType == 3 ? "3D FIX"
                      : fix.fixType == 2 ? "2D FIX"
                                         : "NO FIX";

  // Status row n, counting from 0 immediately below the header.
  auto row = [this](size_t n) {
    return static_cast<int16_t>((kHeaderRows + n + 1) * kRowHeight - 1);
  };

  u8g2_.drawStr(1, row(0), sats);
  u8g2_.drawStr(128 - 1 - u8g2_.getStrWidth(state), row(0), state);

  // Formatted with integer arithmetic throughout. Nothing else in this
  // firmware printf()s a float, and whether %f works at all depends on which
  // newlib variant the core was built with -- not a thing to discover on a
  // screen at a track day.
  if (status.gpsPresent && fix.fixQuality > 0) {
    const char* latSign = fix.latE7 < 0 ? "-" : "";
    const uint32_t latAbs = static_cast<uint32_t>(fix.latE7 < 0 ? -fix.latE7 : fix.latE7);
    snprintf(line, sizeof(line), "LAT %s%lu.%05lu", latSign,
             static_cast<unsigned long>(latAbs / 10000000UL),
             static_cast<unsigned long>((latAbs % 10000000UL) / 100UL));
    u8g2_.drawStr(1, row(1), line);

    const char* lonSign = fix.lonE7 < 0 ? "-" : "";
    const uint32_t lonAbs = static_cast<uint32_t>(fix.lonE7 < 0 ? -fix.lonE7 : fix.lonE7);
    snprintf(line, sizeof(line), "LON %s%lu.%05lu", lonSign,
             static_cast<unsigned long>(lonAbs / 10000000UL),
             static_cast<unsigned long>((lonAbs % 10000000UL) / 100UL));
    u8g2_.drawStr(1, row(2), line);

    const unsigned dop = static_cast<unsigned>(fix.hdop * 10.0f + 0.5f);
    snprintf(line, sizeof(line), "ALT %dm DOP %u.%u", static_cast<int>(fix.altitudeM),
             dop / 10u, dop % 10u);
    u8g2_.drawStr(1, row(3), line);

    const unsigned speed = static_cast<unsigned>(fix.speedKmh * 10.0f + 0.5f);
    snprintf(line, sizeof(line), "SPD %u.%u km/h", speed / 10u, speed % 10u);
    u8g2_.drawStr(1, row(4), line);
  } else if (status.gpsPresent) {
    u8g2_.drawStr(1, row(1), "ACQUIRING");
  }

  // UTC last, and only once the receiver says its clock is trustworthy --
  // that flag is also what gates transmission to RaceChrono.
  if (status.gpsTimeValid) {
    snprintf(line, sizeof(line), "UTC %02u:%02u:%02u", static_cast<unsigned>(fix.hour),
             static_cast<unsigned>(fix.minute), static_cast<unsigned>(fix.seconds));
    u8g2_.drawStr(1, row(5), line);
  }
```

- [ ] **Step 4: Repaint when the fix changes, not when the log does**

In `OledDisplay::tick`, replace the log-revision block:

```cpp
  const uint32_t revision = Log::revision();
  const bool logChanged = revision != drawnRevision_;
  const bool headerChanged = !drawnOnce_ || headerDiffers(status, drawnStatus_);
  if (!logChanged && !headerChanged) {
    return;
  }
  if (logChanged) {
    Log::snapshot(console_);
    drawnRevision_ = revision;
  }
```

with:

```cpp
  if (drawnOnce_ && !headerDiffers(status, drawnStatus_)) {
    return;
  }
```

and extend `headerDiffers` in the anonymous namespace to cover the status area:

```cpp
bool headerDiffers(const DeviceStatus& a, const DeviceStatus& b) {
  // The name is not drawn here, so a rename does not dirty the header.
  return a.clients != b.clients || a.apUp != b.apUp || a.mode != b.mode ||
         a.bleConnected != b.bleConnected || a.dropped != b.dropped ||
         (a.holdMs / 100u) != (b.holdMs / 100u) ||
         (a.uptimeMs / 1000u) != (b.uptimeMs / 1000u) ||
         a.gpsPresent != b.gpsPresent || a.gpsTimeValid != b.gpsTimeValid ||
         a.gpsFix.satellites != b.gpsFix.satellites ||
         a.gpsFix.fixType != b.gpsFix.fixType ||
         a.gpsFix.latE7 != b.gpsFix.latE7 || a.gpsFix.lonE7 != b.gpsFix.lonE7 ||
         a.gpsFix.seconds != b.gpsFix.seconds;
}
```

Then remove the now-unused `#include "core/Log.h"` only if nothing else in the file uses `Log::` — `begin()` still logs, so keep it.

- [ ] **Step 5: Build both environments**

Run: `pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both. The TFT is untouched, so the DevKitC still shows its log.

- [ ] **Step 6: Run the host tests**

Run: `pio test -e native`
Expected: `114 test cases: 114 succeeded`

- [ ] **Step 7: Commit**

```bash
git add src/core/DeviceStatus.h src/ui/OledDisplay.h src/ui/OledDisplay.cpp
git commit -m "Replace the OLED log with a GPS status screen

Twenty-one columns left six characters of message per log line once the
tag was drawn; serial carries the full log and is where it is readable.
The satellite count is always on the left of the first status row --
it is the number that answers whether the receiver is working yet.

The TFT keeps its log view: the DevKitC has no receiver to report on."
```

---

### Task 7: Load saved settings at boot, whatever the radio mode

**This is a pre-existing bug and Task 8 depends on it.** `ConfigPortal::begin()` is the only caller of `NvsStore::load`, and `ModeController::kBootMode` is `RadioMode::Ble` — so on an ordinary boot the portal never starts and nothing ever reads NVS. `app.settings` keeps its defaults until the user switches to WiFi. The saved device name is ignored on a BLE-mode boot, and the saved `sampleHz` would never reach the receiver.

**Files:**
- Modify: `src/config/ConfigPortal.h`, `src/config/ConfigPortal.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `void ConfigPortal::loadSettings()` — reads NVS into the `Settings&` the portal was constructed with. Safe to call before any radio starts, and idempotent.

- [ ] **Step 1: Declare `loadSettings` in `src/config/ConfigPortal.h`**

Add above the existing `bool begin();`:

```cpp
  // Reads saved settings from NVS. Called from setup() before any radio
  // starts: the device boots into BLE mode, where begin() never runs, so
  // loading inside begin() meant a BLE-mode boot silently used defaults.
  void loadSettings();
```

- [ ] **Step 2: Move the load out of `begin()` in `src/config/ConfigPortal.cpp`**

Replace the opening of `begin()`:

```cpp
bool ConfigPortal::begin() {
  if (!store_.begin()) {
    Log::error("nvs", "storage unavailable, changes will not persist");
  } else if (store_.load(settings_)) {
    Log::info("cfg", "loaded name=%s hz=%u", settings_.deviceName,
              (unsigned)settings_.sampleHz);
  } else {
    Log::info("cfg", "no stored settings, using defaults");
  }

  if (!ap_.begin(settings_.deviceName, kChannel, kMaxClients)) {
```

with:

```cpp
void ConfigPortal::loadSettings() {
  if (!store_.begin()) {
    Log::error("nvs", "storage unavailable, changes will not persist");
    return;
  }
  if (store_.load(settings_)) {
    Log::info("cfg", "loaded name=%s hz=%u", settings_.deviceName,
              (unsigned)settings_.sampleHz);
  } else {
    Log::info("cfg", "no stored settings, using defaults");
  }
}

bool ConfigPortal::begin() {
  if (!ap_.begin(settings_.deviceName, kChannel, kMaxClients)) {
```

`NvsStore::begin()` is safe to call more than once, and the web handler already holds its own reference to `store_`, so nothing else moves.

- [ ] **Step 3: Call it from `setup()` in `src/main.cpp`**

Add immediately before `app.button.begin();`:

```cpp
  // Before any radio starts: the boot mode is BLE, so the portal -- which
  // used to be the only thing that read NVS -- may never run at all.
  app.portal.loadSettings();
```

- [ ] **Step 4: Build both environments**

Run: `pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both.

- [ ] **Step 5: Run the host tests**

Run: `pio test -e native`
Expected: `114 test cases: 114 succeeded`

- [ ] **Step 6: Verify on hardware that the saved name survives a reboot**

Flash the T-Beam, open the WiFi portal, set the device name to something distinctive, save, and power-cycle. The BLE advertisement after reboot must carry the new name. Before this change it reverted to the default.

- [ ] **Step 7: Commit**

```bash
git add src/config/ConfigPortal.h src/config/ConfigPortal.cpp src/main.cpp
git commit -m "Load saved settings at boot rather than inside the portal

The only caller of NvsStore::load was ConfigPortal::begin(), and the boot
mode is BLE -- so on an ordinary boot nothing ever read NVS and the saved
device name and sample rate were silently ignored until the user switched
to WiFi. setup() now loads them before any radio starts."
```

---

### Task 8: Wire the receiver into `main.cpp`

**Files:**
- Modify: `src/main.cpp`
- Modify: `README.md`

**Interfaces:**
- Consumes: `GpsReceiver` (Task 5), `DeviceStatus` GPS fields (Task 6), `Settings::sampleHz`.
- Produces: nothing further.

- [ ] **Step 1: Add the receiver to `App` and include it**

Add to the includes, after `#include "core/Log.h"`:

```cpp
#include "gps/GpsReceiver.h"
```

Add to `struct App`, after `RaceChronoGps gps;`:

```cpp
  GpsReceiver gpsRx;
```

- [ ] **Step 2: Delete the synthetic fix on the T-Beam and publish real fixes**

Wrap the synthetic constants and `buildSyntheticFix` in `#if !defined(BOARD_TBEAM)`. The block to guard starts at the comment `// Synthetic GPS until Phase 3 wires up a real receiver:` and ends at the closing brace of `buildSyntheticFix`, including `kSampleHz`, `kSampleIntervalMs`, `kCircleCenterLatDeg`, `kCircleCenterLonDeg`, `kCircleRadiusM`, `kWalkSpeedKmh`, `kWalkSpeedMps`, `kAngularSpeedRadPerS`, `kEarthRadiusM`, `kStartYear`, `kStartMonth`, `kStartDay`, `kStartHour`.

Then replace `produceSample` entirely with:

```cpp
// Sends one RaceChrono packet per fix. Nothing is buffered ahead of a client:
// the reference implementation notifies the fix it just parsed and keeps no
// history (gpsLoop() in docs/reference/racechrono/canbus-gps-device-main.ino),
// and producing while disconnected once filled the ring with a minute of stale
// fixes that flooded out at 25x real time the moment RaceChrono connected.
void publishFix(const GpsFix& fix) {
  if (!app.ble.connected()) {
    return;
  }

  const uint8_t syncBits = app.gps.updateSyncBits(fix);

  uint8_t mainPacket[TelemetrySample::kSize];
  RaceChronoGps::encodeMain(fix, syncBits, mainPacket);
  app.ring.push(mainPacket);

  uint8_t timePacket[3];
  RaceChronoGps::encodeTime(fix, syncBits, timePacket);
  app.ble.publishTime(timePacket);
}

#if defined(BOARD_TBEAM)

// The receiver's own rate is the sample rate: one packet per NAV-PVT, no
// timer and no resampling. Nothing goes out until the receiver says its clock
// is valid -- a wrong timestamp is the axis every sample would be placed on.
void produceSample(uint32_t nowMs, bool newFix) {
  (void)nowMs;
  if (!newFix || !app.gpsRx.timeValid()) {
    return;
  }
  publishFix(app.gpsRx.fix());
}

#else

void produceSample(uint32_t nowMs, bool newFix) {
  (void)newFix;
  if (!app.ble.connected()) {
    return;
  }
  if (nowMs - g_lastSampleMs < kSampleIntervalMs) {
    return;
  }
  g_lastSampleMs = nowMs;
  publishFix(buildSyntheticFix(nowMs));
}

#endif
```

`produceSample` takes the new-fix flag rather than calling `tick()` itself, because the receiver must be pumped in **both** radio modes: the status screen shows satellites while the WiFi portal is up, and a receiver drained only in BLE mode would freeze the screen and overflow its UART buffer. The next step wires that up.

- [ ] **Step 3: Pump the receiver every loop, in both modes**

In `loop()`, replace the mode branch:

```cpp
  if (app.modes.mode() == RadioMode::Wifi) {
    app.portal.tick(now);
  } else {
    produceSample(now);
    app.ble.tick(now);
  }
```

with:

```cpp
  // Drained in both modes: the status screen shows satellites while the
  // portal is up, and an undrained UART buffer would overflow either way.
  const bool newFix = app.gpsRx.tick();

  if (app.modes.mode() == RadioMode::Wifi) {
    app.portal.tick(now);
  } else {
    produceSample(now, newFix);
    app.ble.tick(now);
  }
```

- [ ] **Step 4: Report GPS state in `buildStatus`**

Add to `buildStatus`, before `return s;`:

```cpp
  s.gpsPresent = app.gpsRx.present();
  s.gpsTimeValid = app.gpsRx.timeValid();
  s.gpsFix = app.gpsRx.fix();
```

On the DevKitC `present()` is always false, so the TFT — which does not read these — is unaffected.

- [ ] **Step 5: Start the receiver in `setup()`**

Add after `app.button.begin();`:

```cpp
  // Rate comes from the saved setting, which Task 7's loadSettings() call
  // just above has already read. On the DevKitC this returns false and the
  // synthetic fix takes over; on the T-Beam a false means no receiver
  // answered, and BLE carries on without it.
  app.gpsRx.begin(app.settings.sampleHz);
```

This must come **after** `app.portal.loadSettings()` from Task 7, or the rate is the compiled-in default rather than the saved one.

- [ ] **Step 6: Restart the receiver when the rate changes**

In `startCurrentMode()`, after `app.modes.switchComplete();`, add:

```cpp
  // A rate change arrives through the same restart path as a rename.
  if (app.gpsRx.present() && app.gpsRx.rateHz() != app.settings.sampleHz) {
    app.gpsRx.begin(app.settings.sampleHz);
  }
```

- [ ] **Step 7: Build both environments**

Run: `pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both. If the DevKitC build fails on an unused `g_lastSampleMs`, guard that global with the same `#if !defined(BOARD_TBEAM)`.

- [ ] **Step 8: Run the host tests**

Run: `pio test -e native`
Expected: `114 test cases: 114 succeeded`

- [ ] **Step 9: Update `README.md`**

In the section that describes what the firmware does, add two or three sentences in the file's existing tone covering: the T-Beam Supreme reads its onboard u-blox **MAX-M10S** over UBX-NAV-PVT at the rate set in the web UI; satellite count and fix state appear on its OLED and are reported to RaceChrono; the DevKitC has no receiver attached and emits a synthetic fix so BLE stays testable indoors.

- [ ] **Step 10: Commit**

```bash
git add src/main.cpp README.md
git commit -m "Feed RaceChrono from the real receiver on the T-Beam

One packet per NAV-PVT, so the receiver's own rate is the sample rate --
no timer, no resampling. Nothing is transmitted until the receiver
reports validDate and validTime: a wrong timestamp is the axis every
sample would be placed on, and is the leading suspect for RaceChrono
refusing to lock onto the synthetic fix.

The DevKitC keeps the synthetic fix -- no module is attached to it."
```

---

## Hardware acceptance

Not a task — no code comes out of it — but the branch is not done until this passes.

- [ ] `pio run -e esp -t upload` on the DevKitC: TFT, log, BLE, RaceChrono, mode switch and portal all behave exactly as before.
- [ ] `pio run -e tbeam -t upload` on the T-Beam. Serial shows `gps: receiver answering at <baud> baud` and `gps: configured for UBX-NAV-PVT at <n> Hz`.
- [ ] Near a window, the OLED shows `SATS` climbing above zero with `NO FIX`.
- [ ] Outdoors, it reaches `3D FIX`, and latitude, longitude, altitude and speed are plausible.
- [ ] `UTC` appears only once the receiver's clock is valid.
- [ ] RaceChrono reports a satellite lock and records a session.
- [ ] If it still reports no lock with a good fix on screen, stop and write the laptop-side BLE decoder described in §9 of the spec. Do not guess again.
