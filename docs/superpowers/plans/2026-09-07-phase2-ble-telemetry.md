# Phase 2 BLE Telemetry Link — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stream telemetry over BLE at ~30 kB/s, and switch between BLE and the Phase 1 WiFi portal with a 3-second hold on a hardware button.

**Architecture:** Two mutually exclusive radio modes arbitrated by a pure `ModeController`. A `TelemetryRing` decouples sample production from BLE notification, dropping oldest on overflow so a slow client never stalls `loop()`. `BleLink` wraps NimBLE. Logic is host-tested; hardware wrappers are thin.

**Tech Stack:** PlatformIO, Arduino-ESP32 2.0.17, NimBLE-Arduino 1.4.3, existing Phase 1 modules, `bleak` for laptop-side measurement.

**Spec:** `docs/superpowers/specs/2026-09-07-phase2-ble-telemetry-design.md`

## Global Constraints

- PlatformIO is **not on `PATH`**: `export PATH="$HOME/.platformio/penv/bin:$PATH"` first, every shell.
- **Do not change `platformio.ini`'s hardware settings.** Specifically: no
  `board_build.partitions` line (it boot-loops this board), keep `build_unflags =
  -std=gnu++11`, keep `-DUSE_FSPI_PORT` (without it TFT init null-derefs). Adding
  `lib_deps` and `build_flags` entries this plan calls for is fine; touching anything
  else is not.
- **NimBLE-Arduino pinned to `^1.4.3`.** The 2.x line requires Arduino core 3.x; this
  project is on 2.0.17. Do not "upgrade" it.
- C++ standard `gnu++17`. Fixed-size buffers for our own data. No Arduino `String`,
  no `std::string`, no `std::vector` in code we write. Library-internal allocation is
  fine; do not write custom allocators.
- One module per file pair, header/implementation split, `#pragma once` in every header.
- All string building through `snprintf` with an explicit size.
- `[env:native]`'s `build_src_filter` is the purity boundary: a file listed there must
  compile with no Arduino dependency. Never add a file that includes `Arduino.h`,
  `NimBLEDevice.h`, `WiFi.h` or `TFT_eSPI.h`.
- **Validate untrusted input; trust our own callers.** No defensive guards against
  callers that do not exist. This is a proof of concept.
- Only `Display::tick()` touches SPI, and only from `loop()`.
- Mode button is **GPIO4**, active-low, `INPUT_PULLUP`, hold **3000 ms** to toggle.
- Commit after every task. Never commit `src/config/internal/ui_index.h` (generated).
- There is a `.superpowers/` directory at the repo root; it self-ignores. Never
  `git add` it and never use `git add -A`.

## Starting State

Phase 1 is complete and on `main`/`phase1-config-portal`: 52 host tests, 5 on-device
tests, WiFi portal and TFT verified working on hardware.

Existing API this plan builds on:

- `Log::begin(unsigned long baud)`, `Log::info/warn/error(tag, fmt, ...)` — three levels.
- `Log::snapshot(LogRing&)`, `Log::revision()`.
- `struct Settings { char deviceName[32]; uint8_t sampleHz; }`, `Settings::defaults()`, `validate()`.
- `class SettingsStore` — `load()`, `save()`. `NvsStore`, `MemoryStore`.
- `class ConfigPortal` — `explicit ConfigPortal(Settings&)`, `begin()`, `tick(uint32_t)`, `status()`. Owns `NvsStore`, `ApManager`, `CaptivePortal`, `AsyncWebServer`, `WebUi`.
- `struct DeviceStatus { char ssid[33]; char ip[16]; uint8_t clients; uint32_t uptimeMs; uint32_t freeHeap; bool apUp; }`.
- `class Display` — `begin()`, `tick(uint32_t nowMs, const DeviceStatus&)`. Not a log sink; reads `Log::revision()`/`Log::snapshot()`.
- `Format::uptime(uint32_t, char*, size_t)`, `Format::logLine(...)`.

`[env:native]` `build_src_filter` currently lists: `core/Format.cpp`,
`config/Settings.cpp`, `config/internal/MemoryStore.cpp`,
`config/internal/ConfigApi.cpp`, `config/internal/ConfigService.cpp`,
`core/LogRing.cpp`.

## File Structure

| Path | Responsibility |
| --- | --- |
| `src/radio/RadioMode.h` | `enum class RadioMode`, `ModeEvent`, `ModeController` — pure |
| `src/radio/RadioMode.cpp` | `ModeController` implementation — pure |
| `src/radio/ModeButton.h/.cpp` | GPIO debounce + hold timing; logic split out and host-tested |
| `src/ble/TelemetryRing.h/.cpp` | Fixed sample ring, batch extraction, drop counting — pure |
| `src/ble/BleLink.h/.cpp` | NimBLE server, advertising, notify pump — device only |
| `src/config/ConfigPortal.h/.cpp` | Gains `end()` |
| `src/config/internal/CaptivePortal.h/.cpp` | Regains `end()` |
| `src/ui/Display.h/.cpp` | Header renders per-mode |
| `src/main.cpp` | Owns the mode switch; wires everything |
| `tools/ble_throughput.py` | Laptop receiver and measurement |
| `test/native/test_mode_controller/` | Host tests |
| `test/native/test_mode_button/` | Host tests |
| `test/native/test_telemetry_ring/` | Host tests |

Tasks 1–3 are pure and host-tested, no hardware. Task 4 is a small device change.
Task 5 is the NimBLE spike — the riskiest step, isolated deliberately. Tasks 6–7 wire
it together. Task 8 is measurement.

---

### Task 1: `ModeController` — the mode state machine

Pure logic: which mode we are in, and what events change it. No Arduino, no radio.

**Files:**
- Create: `src/radio/RadioMode.h`, `src/radio/RadioMode.cpp`
- Modify: `platformio.ini` (`build_src_filter` in `[env:native]`)
- Test: `test/native/test_mode_controller/test_mode_controller.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class RadioMode : uint8_t { Ble, Wifi }`
  - `enum class ModeEvent : uint8_t { ButtonHeld }`
  - `class ModeController` — `RadioMode mode() const`, `bool handle(ModeEvent)`, `bool switchPending() const`, `void switchComplete()`
  - `ModeController::kBootMode == RadioMode::Ble`

**Design note the tests pin down:** `handle()` returns true when the event caused a
transition. A second event arriving while a switch is still pending is **ignored** —
the radio teardown/startup is not reentrant, and a button bounce or a double-hold must
not start a second switch mid-flight.

- [ ] **Step 1: Write the failing test**

Create `test/native/test_mode_controller/test_mode_controller.cpp`:

```cpp
#include <unity.h>

#include "radio/RadioMode.h"

void setUp() {}
void tearDown() {}

static void test_boots_into_ble() {
  ModeController c;
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Ble);
  TEST_ASSERT_FALSE(c.switchPending());
}

static void test_button_hold_toggles_to_wifi() {
  ModeController c;
  TEST_ASSERT_TRUE(c.handle(ModeEvent::ButtonHeld));
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Wifi);
  TEST_ASSERT_TRUE(c.switchPending());
}

static void test_button_hold_toggles_back_to_ble() {
  ModeController c;
  c.handle(ModeEvent::ButtonHeld);
  c.switchComplete();
  TEST_ASSERT_TRUE(c.handle(ModeEvent::ButtonHeld));
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Ble);
}

static void test_event_while_switch_pending_is_ignored() {
  ModeController c;
  TEST_ASSERT_TRUE(c.handle(ModeEvent::ButtonHeld));
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Wifi);

  // Radio teardown is not reentrant: a second hold mid-switch must not start
  // another one, and must not flip the mode back.
  TEST_ASSERT_FALSE(c.handle(ModeEvent::ButtonHeld));
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Wifi);
  TEST_ASSERT_TRUE(c.switchPending());
}

static void test_switch_complete_clears_pending_and_reenables_events() {
  ModeController c;
  c.handle(ModeEvent::ButtonHeld);
  c.switchComplete();
  TEST_ASSERT_FALSE(c.switchPending());
  TEST_ASSERT_TRUE(c.handle(ModeEvent::ButtonHeld));
}

static void test_switch_complete_without_pending_is_harmless() {
  ModeController c;
  c.switchComplete();
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Ble);
  TEST_ASSERT_FALSE(c.switchPending());
}

static void test_many_toggles_alternate() {
  ModeController c;
  for (int i = 0; i < 10; ++i) {
    c.handle(ModeEvent::ButtonHeld);
    c.switchComplete();
    const RadioMode expected = (i % 2 == 0) ? RadioMode::Wifi : RadioMode::Ble;
    TEST_ASSERT_TRUE(c.mode() == expected);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boots_into_ble);
  RUN_TEST(test_button_hold_toggles_to_wifi);
  RUN_TEST(test_button_hold_toggles_back_to_ble);
  RUN_TEST(test_event_while_switch_pending_is_ignored);
  RUN_TEST(test_switch_complete_clears_pending_and_reenables_events);
  RUN_TEST(test_switch_complete_without_pending_is_harmless);
  RUN_TEST(test_many_toggles_alternate);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
pio test -e native -f native/test_mode_controller
```

Expected: FAIL — `fatal error: radio/RadioMode.h: No such file or directory`.

- [ ] **Step 3: Write `src/radio/RadioMode.h`**

```cpp
#pragma once

#include <stdint.h>

// The two radios share one 2.4 GHz front end, so exactly one is up at a time.
enum class RadioMode : uint8_t {
  Ble,
  Wifi,
};

enum class ModeEvent : uint8_t {
  ButtonHeld,
};

// Decides which radio should be running. Pure: it owns no hardware and starts
// nothing. main.cpp reads switchPending() and does the actual teardown and
// startup, then reports back with switchComplete().
class ModeController {
 public:
  static constexpr RadioMode kBootMode = RadioMode::Ble;

  RadioMode mode() const { return mode_; }

  // True when the mode changed. A switch already in flight swallows the event:
  // radio teardown is not reentrant and a bounce must not start a second one.
  bool handle(ModeEvent event);

  bool switchPending() const { return pending_; }

  // Called once the caller has finished starting the new radio.
  void switchComplete() { pending_ = false; }

 private:
  RadioMode mode_ = kBootMode;
  bool pending_ = false;
};
```

- [ ] **Step 4: Write `src/radio/RadioMode.cpp`**

```cpp
#include "radio/RadioMode.h"

bool ModeController::handle(ModeEvent event) {
  if (pending_) {
    return false;
  }
  switch (event) {
    case ModeEvent::ButtonHeld:
      mode_ = (mode_ == RadioMode::Ble) ? RadioMode::Wifi : RadioMode::Ble;
      pending_ = true;
      return true;
  }
  return false;
}
```

- [ ] **Step 5: Add to the host build**

In `platformio.ini`, `[env:native]`, append to `build_src_filter`:

```ini
    +<radio/RadioMode.cpp>
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
pio test -e native -f native/test_mode_controller
```

Expected: PASS — `7 Tests 0 Failures 0 Ignored`.

- [ ] **Step 7: Commit**

```bash
git add platformio.ini src/radio/RadioMode.h src/radio/RadioMode.cpp test/native/test_mode_controller/test_mode_controller.cpp
git commit -m "Add radio mode state machine"
```

---

### Task 2: `HoldDetector` and `ModeButton`

The 3-second hold timing is pure and host-tested by feeding it synthetic timestamps
and pin states. `ModeButton` is the thin GPIO wrapper around it. This split is what
lets the button be verified before it is soldered on.

**Files:**
- Create: `src/radio/HoldDetector.h`, `src/radio/HoldDetector.cpp`
- Create: `src/radio/ModeButton.h`, `src/radio/ModeButton.cpp`
- Modify: `platformio.ini` (`build_src_filter`)
- Test: `test/native/test_mode_button/test_mode_button.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `class HoldDetector` — `bool update(bool pressed, uint32_t nowMs)`, `uint32_t heldMs(uint32_t nowMs) const`, `bool isHolding() const`
  - `HoldDetector::kHoldMs == 3000`, `HoldDetector::kDebounceMs == 30`
  - `class ModeButton` — `void begin()`, `bool tick(uint32_t nowMs)`, `uint32_t heldMs(uint32_t nowMs) const`
  - `ModeButton::kPin == 4`

**Behaviour the tests pin down:**

- `update()` returns true **exactly once**, at the moment the press reaches 3000 ms.
  Not on release, and not again while the button stays down.
- A release before 3000 ms fires nothing and resets the timer.
- Contact bounce shorter than `kDebounceMs` does not restart the hold.
- `heldMs()` reports progress so the screen can count down.

- [ ] **Step 1: Write the failing test**

Create `test/native/test_mode_button/test_mode_button.cpp`:

```cpp
#include <unity.h>

#include "radio/HoldDetector.h"

void setUp() {}
void tearDown() {}

static void test_idle_never_fires() {
  HoldDetector d;
  for (uint32_t t = 0; t < 10000; t += 100) {
    TEST_ASSERT_FALSE(d.update(false, t));
  }
}

static void test_fires_once_exactly_at_the_hold_threshold() {
  HoldDetector d;
  TEST_ASSERT_FALSE(d.update(true, 0));
  TEST_ASSERT_FALSE(d.update(true, 2999));
  TEST_ASSERT_TRUE(d.update(true, 3000));
}

static void test_does_not_fire_again_while_still_held() {
  HoldDetector d;
  d.update(true, 0);
  TEST_ASSERT_TRUE(d.update(true, 3000));
  for (uint32_t t = 3100; t < 12000; t += 100) {
    TEST_ASSERT_FALSE(d.update(true, t));
  }
}

static void test_release_before_threshold_fires_nothing() {
  HoldDetector d;
  d.update(true, 0);
  d.update(true, 2500);
  TEST_ASSERT_FALSE(d.update(false, 2600));
  TEST_ASSERT_FALSE(d.isHolding());
}

static void test_release_then_hold_again_fires() {
  HoldDetector d;
  d.update(true, 0);
  d.update(false, 1000);
  d.update(true, 2000);
  TEST_ASSERT_FALSE(d.update(true, 4999));
  TEST_ASSERT_TRUE(d.update(true, 5000));  // 3000 ms after the second press
}

static void test_bounce_shorter_than_debounce_does_not_restart_the_hold() {
  HoldDetector d;
  d.update(true, 0);
  // A 10 ms glitch to released, well under kDebounceMs.
  d.update(false, 1000);
  d.update(true, 1010);
  // Still measured from t=0, so it fires at 3000, not 4010.
  TEST_ASSERT_TRUE(d.update(true, 3000));
}

static void test_release_longer_than_debounce_does_restart_the_hold() {
  HoldDetector d;
  d.update(true, 0);
  d.update(false, 1000);
  d.update(false, 1100);  // 100 ms released, past kDebounceMs
  d.update(true, 1200);
  TEST_ASSERT_FALSE(d.update(true, 3000));   // would have fired on the old timer
  TEST_ASSERT_TRUE(d.update(true, 4200));    // 3000 ms after the restart
}

static void test_held_ms_reports_progress() {
  HoldDetector d;
  TEST_ASSERT_EQUAL_UINT32(0u, d.heldMs(0));
  d.update(true, 1000);
  TEST_ASSERT_EQUAL_UINT32(500u, d.heldMs(1500));
  TEST_ASSERT_TRUE(d.isHolding());
  d.update(false, 2000);
  TEST_ASSERT_EQUAL_UINT32(0u, d.heldMs(2500));
}

static void test_held_ms_is_zero_when_not_pressed() {
  HoldDetector d;
  TEST_ASSERT_EQUAL_UINT32(0u, d.heldMs(5000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_idle_never_fires);
  RUN_TEST(test_fires_once_exactly_at_the_hold_threshold);
  RUN_TEST(test_does_not_fire_again_while_still_held);
  RUN_TEST(test_release_before_threshold_fires_nothing);
  RUN_TEST(test_release_then_hold_again_fires);
  RUN_TEST(test_bounce_shorter_than_debounce_does_not_restart_the_hold);
  RUN_TEST(test_release_longer_than_debounce_does_restart_the_hold);
  RUN_TEST(test_held_ms_reports_progress);
  RUN_TEST(test_held_ms_is_zero_when_not_pressed);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
pio test -e native -f native/test_mode_button
```

Expected: FAIL — `fatal error: radio/HoldDetector.h: No such file or directory`.

- [ ] **Step 3: Write `src/radio/HoldDetector.h`**

```cpp
#pragma once

#include <stdint.h>

// Turns a stream of (pressed, now) samples into a single event when the button
// has been held long enough.
//
// Pure and clock-free: the caller supplies the timestamp, so the 3-second
// behaviour is host-testable without a button or a board.
class HoldDetector {
 public:
  static constexpr uint32_t kHoldMs = 3000;
  // A release shorter than this is contact bounce and does not reset the hold.
  static constexpr uint32_t kDebounceMs = 30;

  // Returns true exactly once, on the sample where the press reaches kHoldMs.
  bool update(bool pressed, uint32_t nowMs);

  // Milliseconds held so far, 0 when not pressed. Drives the screen countdown.
  uint32_t heldMs(uint32_t nowMs) const;

  bool isHolding() const { return pressed_; }

 private:
  bool pressed_ = false;
  bool fired_ = false;
  uint32_t pressStartMs_ = 0;
  // When a release began, used to tell bounce from a real release.
  bool releasing_ = false;
  uint32_t releaseStartMs_ = 0;
};
```

- [ ] **Step 4: Write `src/radio/HoldDetector.cpp`**

```cpp
#include "radio/HoldDetector.h"

bool HoldDetector::update(bool pressed, uint32_t nowMs) {
  if (pressed) {
    if (!pressed_) {
      // First contact, or contact restored after a bounce.
      pressed_ = true;
      fired_ = false;
      pressStartMs_ = nowMs;
    } else if (releasing_) {
      // Contact came back within the debounce window: the hold never broke.
      releasing_ = false;
    }

    if (!fired_ && (nowMs - pressStartMs_) >= kHoldMs) {
      fired_ = true;
      return true;
    }
    return false;
  }

  // Not pressed.
  if (pressed_) {
    if (!releasing_) {
      releasing_ = true;
      releaseStartMs_ = nowMs;
    } else if ((nowMs - releaseStartMs_) >= kDebounceMs) {
      // Released for longer than bounce: the hold is genuinely over.
      pressed_ = false;
      releasing_ = false;
      fired_ = false;
    }
  }
  return false;
}

uint32_t HoldDetector::heldMs(uint32_t nowMs) const {
  if (!pressed_) {
    return 0;
  }
  return nowMs - pressStartMs_;
}
```

- [ ] **Step 5: Write `src/radio/ModeButton.h`**

```cpp
#pragma once

#include <stdint.h>

#include "radio/HoldDetector.h"

// GPIO4, wired to ground through a button, using the internal pull-up: the pin
// reads LOW while pressed. All timing lives in HoldDetector; this is only the
// pin read.
class ModeButton {
 public:
  static constexpr uint8_t kPin = 4;

  void begin();

  // Call from loop(). True exactly once per completed 3-second hold.
  bool tick(uint32_t nowMs);

  uint32_t heldMs(uint32_t nowMs) const { return detector_.heldMs(nowMs); }

 private:
  HoldDetector detector_;
};
```

- [ ] **Step 6: Write `src/radio/ModeButton.cpp`**

```cpp
#include "radio/ModeButton.h"

#include <Arduino.h>

void ModeButton::begin() { pinMode(kPin, INPUT_PULLUP); }

bool ModeButton::tick(uint32_t nowMs) {
  const bool pressed = digitalRead(kPin) == LOW;  // pull-up: LOW means pressed
  return detector_.update(pressed, nowMs);
}
```

- [ ] **Step 7: Add `HoldDetector.cpp` to the host build**

`ModeButton.cpp` includes `Arduino.h` and must **not** be added. In
`platformio.ini`, `[env:native]`, append to `build_src_filter`:

```ini
    +<radio/HoldDetector.cpp>
```

- [ ] **Step 8: Run the test to verify it passes**

```bash
pio test -e native -f native/test_mode_button
```

Expected: PASS — `9 Tests 0 Failures 0 Ignored`.

- [ ] **Step 9: Confirm the device still builds**

```bash
pio run -e esp
```

Expected: `SUCCESS`. `ModeButton` is compiled but not yet called.

- [ ] **Step 10: Commit**

```bash
git add platformio.ini src/radio/HoldDetector.h src/radio/HoldDetector.cpp src/radio/ModeButton.h src/radio/ModeButton.cpp test/native/test_mode_button/test_mode_button.cpp
git commit -m "Add 3-second hold detection and the mode button"
```

---

### Task 3: `TelemetryRing`

The buffer between sample production and BLE notification. Fixed size, overwrites
oldest on overflow, counts what it dropped, and hands out batches sized to a byte
budget.

**Files:**
- Create: `src/ble/TelemetryRing.h`, `src/ble/TelemetryRing.cpp`
- Modify: `platformio.ini` (`build_src_filter`)
- Test: `test/native/test_telemetry_ring/test_telemetry_ring.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct TelemetrySample { uint32_t seq; uint32_t uptimeMs; uint8_t payload[TelemetrySample::kPayloadBytes]; }`
  - `TelemetrySample::kPayloadBytes == 12`, `TelemetrySample::kSize == 20`
  - `class TelemetryRing` — `void push(uint32_t uptimeMs, const uint8_t* payload)`, `size_t drain(uint8_t* out, size_t outSize)`, `uint32_t dropped() const`, `uint32_t produced() const`, `size_t pending() const`, `bool empty() const`
  - `TelemetryRing::kCapacity == 256`

**Why 20 bytes per sample:** at 200 Hz that is 4 kB/s per stream and 25 samples fit in
a 500-byte notification. `kPayloadBytes` is the room Phase 3 has for real GPS and IMU
fields; Phase 2 fills it with a pattern.

**Why capacity 256:** 5.1 kB of RAM, and at 200 Hz it absorbs 1.28 s of production
if the link stalls — long enough to ride out a hiccup, short enough that recovered
data is still recent.

**Behaviour the tests pin down:**

- `drain()` copies whole samples only, never a partial one, and never exceeds `outSize`.
- Overflow overwrites the oldest and increments `dropped()`; it never blocks or grows.
- `seq` is assigned by the ring, is monotonic, and keeps counting across overflow — a
  gap in `seq` at the receiver is exactly the count of dropped samples.

- [ ] **Step 1: Write the failing test**

Create `test/native/test_telemetry_ring/test_telemetry_ring.cpp`:

```cpp
#include <unity.h>
#include <string.h>

#include "ble/TelemetryRing.h"

void setUp() {}
void tearDown() {}

static void fillPayload(uint8_t* p, uint8_t value) {
  memset(p, value, TelemetrySample::kPayloadBytes);
}

static uint32_t seqAt(const uint8_t* buf, size_t index) {
  uint32_t seq = 0;
  memcpy(&seq, buf + index * TelemetrySample::kSize, sizeof(seq));
  return seq;
}

static void test_starts_empty() {
  TelemetryRing ring;
  TEST_ASSERT_TRUE(ring.empty());
  TEST_ASSERT_EQUAL_UINT(0u, (unsigned)ring.pending());
  TEST_ASSERT_EQUAL_UINT32(0u, ring.dropped());
  TEST_ASSERT_EQUAL_UINT32(0u, ring.produced());
}

static void test_drain_of_empty_ring_writes_nothing() {
  TelemetryRing ring;
  uint8_t out[256];
  TEST_ASSERT_EQUAL_UINT(0u, (unsigned)ring.drain(out, sizeof(out)));
}

static void test_push_then_drain_round_trips() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes];
  fillPayload(payload, 0xAB);
  ring.push(1234, payload);

  TEST_ASSERT_EQUAL_UINT(1u, (unsigned)ring.pending());

  uint8_t out[TelemetrySample::kSize];
  const size_t n = ring.drain(out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT((unsigned)TelemetrySample::kSize, (unsigned)n);
  TEST_ASSERT_EQUAL_UINT32(0u, seqAt(out, 0));

  uint32_t uptime = 0;
  memcpy(&uptime, out + sizeof(uint32_t), sizeof(uptime));
  TEST_ASSERT_EQUAL_UINT32(1234u, uptime);
  TEST_ASSERT_EQUAL_UINT8(0xAB, out[2 * sizeof(uint32_t)]);
  TEST_ASSERT_TRUE(ring.empty());
}

static void test_sequence_numbers_are_monotonic() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  for (int i = 0; i < 5; ++i) {
    ring.push((uint32_t)i, payload);
  }
  uint8_t out[5 * TelemetrySample::kSize];
  ring.drain(out, sizeof(out));
  for (uint32_t i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL_UINT32(i, seqAt(out, i));
  }
}

static void test_drain_copies_whole_samples_only() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  for (int i = 0; i < 10; ++i) {
    ring.push((uint32_t)i, payload);
  }
  // Room for 3 samples and a bit: must return exactly 3, never a partial one.
  uint8_t out[3 * TelemetrySample::kSize + 7];
  const size_t n = ring.drain(out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT((unsigned)(3 * TelemetrySample::kSize), (unsigned)n);
  TEST_ASSERT_EQUAL_UINT(7u, (unsigned)ring.pending());
}

static void test_drain_smaller_than_one_sample_returns_nothing() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  ring.push(0, payload);
  uint8_t out[TelemetrySample::kSize - 1];
  TEST_ASSERT_EQUAL_UINT(0u, (unsigned)ring.drain(out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT(1u, (unsigned)ring.pending());  // still there
}

static void test_overflow_drops_oldest_and_counts_it() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  const size_t over = 10;
  for (size_t i = 0; i < TelemetryRing::kCapacity + over; ++i) {
    ring.push((uint32_t)i, payload);
  }
  TEST_ASSERT_EQUAL_UINT32((uint32_t)over, ring.dropped());
  TEST_ASSERT_EQUAL_UINT((unsigned)TelemetryRing::kCapacity, (unsigned)ring.pending());
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(TelemetryRing::kCapacity + over), ring.produced());
}

static void test_sequence_gap_after_overflow_equals_drop_count() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  const size_t over = 10;
  for (size_t i = 0; i < TelemetryRing::kCapacity + over; ++i) {
    ring.push((uint32_t)i, payload);
  }
  // The oldest surviving sample must be seq == over: that is what lets the
  // receiver measure loss from sequence gaps alone.
  uint8_t out[TelemetrySample::kSize];
  ring.drain(out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)over, seqAt(out, 0));
}

static void test_drain_then_push_reuses_space_without_dropping() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  for (size_t i = 0; i < TelemetryRing::kCapacity; ++i) {
    ring.push((uint32_t)i, payload);
  }
  uint8_t out[64 * TelemetrySample::kSize];
  ring.drain(out, sizeof(out));  // free 64 slots
  for (size_t i = 0; i < 64; ++i) {
    ring.push(0, payload);
  }
  TEST_ASSERT_EQUAL_UINT32(0u, ring.dropped());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_empty);
  RUN_TEST(test_drain_of_empty_ring_writes_nothing);
  RUN_TEST(test_push_then_drain_round_trips);
  RUN_TEST(test_sequence_numbers_are_monotonic);
  RUN_TEST(test_drain_copies_whole_samples_only);
  RUN_TEST(test_drain_smaller_than_one_sample_returns_nothing);
  RUN_TEST(test_overflow_drops_oldest_and_counts_it);
  RUN_TEST(test_sequence_gap_after_overflow_equals_drop_count);
  RUN_TEST(test_drain_then_push_reuses_space_without_dropping);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
pio test -e native -f native/test_telemetry_ring
```

Expected: FAIL — `fatal error: ble/TelemetryRing.h: No such file or directory`.

- [ ] **Step 3: Write `src/ble/TelemetryRing.h`**

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

// One telemetry sample as it goes over the air. Packed so the on-wire layout is
// exactly kSize bytes and the receiver can parse it with a fixed stride.
struct __attribute__((packed)) TelemetrySample {
  // Room Phase 3 will use for real GPS and IMU fields. Phase 2 fills it with a
  // pattern so the transport can be measured before the payload exists.
  static constexpr size_t kPayloadBytes = 12;
  static constexpr size_t kSize = sizeof(uint32_t) * 2 + kPayloadBytes;  // 20

  uint32_t seq;
  uint32_t uptimeMs;
  uint8_t payload[kPayloadBytes];
};

// Fixed ring between the sample producer and the BLE notify pump.
//
// On overflow it overwrites the oldest sample and counts the loss. It never
// blocks and never allocates: a phone that cannot keep up must not be able to
// stall loop() and take the screen and button down with it.
class TelemetryRing {
 public:
  // 256 * 20 B = 5.1 kB, about 1.28 s of production at 200 Hz.
  static constexpr size_t kCapacity = 256;

  // seq is assigned here, so it counts every sample ever produced — including
  // the dropped ones. A gap at the receiver is exactly the loss.
  void push(uint32_t uptimeMs, const uint8_t* payload);

  // Copies as many whole samples as fit into out, returning bytes written.
  // Never writes a partial sample.
  size_t drain(uint8_t* out, size_t outSize);

  uint32_t dropped() const { return dropped_; }
  uint32_t produced() const { return produced_; }
  size_t pending() const { return count_; }
  bool empty() const { return count_ == 0; }

 private:
  TelemetrySample samples_[kCapacity] = {};
  size_t head_ = 0;   // oldest
  size_t count_ = 0;
  uint32_t nextSeq_ = 0;
  uint32_t dropped_ = 0;
  uint32_t produced_ = 0;
};
```

- [ ] **Step 4: Write `src/ble/TelemetryRing.cpp`**

```cpp
#include "ble/TelemetryRing.h"

#include <string.h>

void TelemetryRing::push(uint32_t uptimeMs, const uint8_t* payload) {
  size_t slot;
  if (count_ < kCapacity) {
    slot = (head_ + count_) % kCapacity;
    ++count_;
  } else {
    // Full: overwrite the oldest and advance the window.
    slot = head_;
    head_ = (head_ + 1) % kCapacity;
    ++dropped_;
  }

  samples_[slot].seq = nextSeq_++;
  samples_[slot].uptimeMs = uptimeMs;
  memcpy(samples_[slot].payload, payload, TelemetrySample::kPayloadBytes);
  ++produced_;
}

size_t TelemetryRing::drain(uint8_t* out, size_t outSize) {
  const size_t room = outSize / TelemetrySample::kSize;
  size_t take = room < count_ ? room : count_;

  size_t written = 0;
  for (size_t i = 0; i < take; ++i) {
    memcpy(out + written, &samples_[head_], TelemetrySample::kSize);
    head_ = (head_ + 1) % kCapacity;
    --count_;
    written += TelemetrySample::kSize;
  }
  return written;
}
```

- [ ] **Step 5: Add to the host build**

```ini
    +<ble/TelemetryRing.cpp>
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
pio test -e native -f native/test_telemetry_ring
```

Expected: PASS — `9 Tests 0 Failures 0 Ignored`.

- [ ] **Step 7: Run the whole host suite**

```bash
pio test -e native
```

Expected: 52 existing + 7 + 9 + 9 = **77 tests passing**, output pristine.

- [ ] **Step 8: Commit**

```bash
git add platformio.ini src/ble/TelemetryRing.h src/ble/TelemetryRing.cpp test/native/test_telemetry_ring/test_telemetry_ring.cpp
git commit -m "Add telemetry ring with drop accounting"
```

---

### Task 4: `ConfigPortal::end()`

Phase 1 never tore the AP down — it ran from boot to power-down. Mode switching needs
it gone, and gone completely, before the BLE stack comes up on the same radio.

**Files:**
- Modify: `src/config/internal/CaptivePortal.h`, `src/config/internal/CaptivePortal.cpp`
- Modify: `src/config/ConfigPortal.h`, `src/config/ConfigPortal.cpp`

**Interfaces:**
- Produces: `void CaptivePortal::end()`, `void ConfigPortal::end()`

**No test.** This is Arduino teardown calls with no logic to verify on the host. It is
exercised by the manual acceptance in Task 8; a host test here would assert nothing.

- [ ] **Step 1: Add `end()` to `src/config/internal/CaptivePortal.h`**

After `registerRoutes`:

```cpp
  // Stops answering DNS. Safe to call when never begun.
  void end();
```

- [ ] **Step 2: Add the definition to `src/config/internal/CaptivePortal.cpp`**

```cpp
void CaptivePortal::end() {
  if (up_) {
    dns_.stop();
    up_ = false;
  }
}
```

- [ ] **Step 3: Add `end()` to `src/config/ConfigPortal.h`**

After `tick`:

```cpp
  // Tears down HTTP, DNS and the access point. Safe to call when never begun,
  // and safe to call twice.
  void end();
```

- [ ] **Step 4: Add the definition to `src/config/ConfigPortal.cpp`**

Add `#include <WiFi.h>` at the top if it is not already there, then:

```cpp
void ConfigPortal::end() {
  server_.end();
  portal_.end();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  Log::info("ap", "stopped");
}
```

`WIFI_OFF` matters: leaving the WiFi driver initialised keeps it holding the radio,
which is the thing BLE is about to need.

- [ ] **Step 5: Verify it builds and nothing regressed**

```bash
pio run -e esp
pio test -e native
```

Expected: `SUCCESS`, and 77 host tests still passing.

- [ ] **Step 6: Commit**

```bash
git add src/config/internal/CaptivePortal.h src/config/internal/CaptivePortal.cpp src/config/ConfigPortal.h src/config/ConfigPortal.cpp
git commit -m "Add ConfigPortal teardown so the radio can be released"
```

---

### Task 5: NimBLE spike — prove the library and the link

**The riskiest task, isolated on purpose.** NimBLE 1.4.3 has never been compiled in
this project, and the version constraint against Arduino core 2.0.17 is a claim that
has not been tested. Find out here, in a task that adds one dependency and one
advertising server, rather than inside a larger change.

**Files:**
- Modify: `platformio.ini` (`lib_deps`, `build_flags`)
- Create: `src/ble/BleLink.h`, `src/ble/BleLink.cpp`

**Interfaces:**
- Consumes: `TelemetryRing` (Task 3), `Log`.
- Produces:
  - `class BleLink` — `bool begin(const char* deviceName, TelemetryRing& ring)`, `void end()`, `void tick(uint32_t nowMs)`, `bool connected() const`, `uint16_t mtu() const`, `uint32_t sentBytes() const`
  - `BleLink::kServiceUuid`, `kLiveUuid`, `kStatusUuid`

**If NimBLE 1.4.3 does not compile against this Arduino core, STOP and report it.**
Do not switch to the bundled Bluedroid `BLE` library, do not bump the platform, and do
not try NimBLE 2.x. Report the exact error — the fallback is a decision for the human,
because bumping the platform risks the TFT and WiFi configuration that was just
verified working on hardware.

- [ ] **Step 1: Add the dependency and BLE tuning flags**

In `platformio.ini`, `[env:esp]`, add to `lib_deps`:

```ini
    h2zero/NimBLE-Arduino@^1.4.3
```

and to `build_flags`:

```ini
    ; BLE throughput: without these the 30 kB/s budget does not close.
    ; Extended advertising off (we advertise legacy), roles trimmed to peripheral.
    -DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
    -DCONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517
    -DCONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED
    -DCONFIG_BT_NIMBLE_ROLE_OBSERVER_DISABLED
```

Change nothing else in that file.

- [ ] **Step 2: Write `src/ble/BleLink.h`**

```cpp
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
```

- [ ] **Step 3: Write `src/ble/BleLink.cpp`**

```cpp
#include "ble/BleLink.h"

#include <NimBLEDevice.h>

#include "core/Log.h"

namespace {

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_live = nullptr;
NimBLECharacteristic* g_status = nullptr;
bool g_connected = false;
uint16_t g_mtu = 23;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    g_connected = true;
    // Ask for the shortest interval the client will accept: throughput is
    // packets-per-interval, so the interval is the dominant term.
    server->updateConnParams(desc->conn_handle, 6, 12, 0, 200);
    Log::info("ble", "connected");
  }

  void onDisconnect(NimBLEServer* server) override {
    g_connected = false;
    g_mtu = 23;
    Log::info("ble", "disconnected, advertising again");
    NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t mtu, ble_gap_conn_desc* desc) override {
    g_mtu = mtu;
    Log::info("ble", "mtu %u", (unsigned)mtu);
  }
};

ServerCallbacks g_callbacks;

}  // namespace

bool BleLink::begin(const char* deviceName, TelemetryRing& ring) {
  ring_ = &ring;

  NimBLEDevice::init(deviceName);
  NimBLEDevice::setMTU(517);
  // 2M PHY doubles the symbol rate; without it the budget does not close.
  NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK);

  g_server = NimBLEDevice::createServer();
  if (g_server == nullptr) {
    Log::error("ble", "createServer failed");
    return false;
  }
  g_server->setCallbacks(&g_callbacks);

  NimBLEService* service = g_server->createService(kServiceUuid);
  g_live = service->createCharacteristic(kLiveUuid, NIMBLE_PROPERTY::NOTIFY);
  g_status = service->createCharacteristic(
      kStatusUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  advertising->start();

  Log::info("ble", "advertising as %s", deviceName);
  return true;
}

void BleLink::end() {
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);
  g_server = nullptr;
  g_live = nullptr;
  g_status = nullptr;
  g_connected = false;
  g_mtu = 23;
  connected_ = false;
  mtu_ = 23;
  Log::info("ble", "stopped");
}

void BleLink::tick(uint32_t nowMs) {
  connected_ = g_connected;
  mtu_ = g_mtu;

  if (!connected_ || ring_ == nullptr || g_live == nullptr) {
    return;
  }

  // Fill one notification to just under the negotiated MTU. Batching is what
  // keeps protocol overhead from eating the budget.
  const size_t budget = (mtu_ > 3) ? static_cast<size_t>(mtu_ - 3) : 20;
  const size_t limit = budget < kMaxNotifyBytes ? budget : kMaxNotifyBytes;

  uint8_t buffer[kMaxNotifyBytes];
  const size_t n = ring_->drain(buffer, limit);
  if (n == 0) {
    return;
  }

  g_live->setValue(buffer, n);
  g_live->notify();
  sentBytes_ += n;
}
```

- [ ] **Step 4: Verify it compiles**

```bash
pio run -e esp
```

Expected: `SUCCESS`. Record the flash and RAM figures — NimBLE is the largest thing
added in this phase.

If it fails: **stop and report the exact error.** Do not substitute a different
library or platform version.

- [ ] **Step 5: Confirm the host build is untouched**

```bash
pio test -e native
```

Expected: 77 passing. `BleLink.cpp` must **not** be in `build_src_filter` — it
includes `NimBLEDevice.h`.

- [ ] **Step 6: Commit**

```bash
git add platformio.ini src/ble/BleLink.h src/ble/BleLink.cpp
git commit -m "Add NimBLE peripheral with batched telemetry notifications"
```

---

### Task 6: Screen shows the mode

The screen is the only feedback in BLE mode, so it changes before the mode switch is
wired up — that way Task 7 has something to watch.

**Files:**
- Modify: `src/ui/Display.h`, `src/ui/Display.cpp`
- Modify: `src/core/DeviceStatus.h`

**Interfaces:**
- Consumes: `RadioMode` (Task 1).
- Produces: `DeviceStatus` gains `RadioMode mode`, `bool bleConnected`, `uint32_t kbPerSec`, `uint32_t dropped`, `uint32_t holdMs`.

**Rendering, by mode:**

| Condition | Top line | Second line |
| --- | --- | --- |
| `Wifi` | `<ssid>` … `<ip>  clients:<n>` | `AP UP` … `up HH:MM:SS` |
| `Ble`, not connected | `teletrack` … `BLE ADV` | `WAITING` … `up HH:MM:SS` |
| `Ble`, connected | `teletrack` … `BLE CONN` | `<n> kB/s drop:<n>` … `up HH:MM:SS` |
| button held | unchanged | `HOLD 3s… <n>` replaces the second line |

Keep the existing anti-flicker approach: **no `fillRect` in `drawHeader`**, opaque text
with `setTextPadding` so each field overwrites its own background. That was a real bug;
reintroducing a clear-then-draw brings the flicker back.

- [ ] **Step 1: Extend `src/core/DeviceStatus.h`**

Add the include and the fields:

```cpp
#include "radio/RadioMode.h"
```

```cpp
  RadioMode mode = RadioMode::Ble;
  bool bleConnected = false;
  uint32_t kbPerSec = 0;
  uint32_t dropped = 0;
  // Non-zero while the mode button is held; drives the countdown.
  uint32_t holdMs = 0;
```

- [ ] **Step 2: Update `headerDiffers` in `src/ui/Display.cpp`**

The header must repaint when any of the new fields change, or the mode indicator will
lag. Replace the function body:

```cpp
bool headerDiffers(const DeviceStatus& a, const DeviceStatus& b) {
  return strcmp(a.ssid, b.ssid) != 0 || strcmp(a.ip, b.ip) != 0 ||
         a.clients != b.clients || a.apUp != b.apUp ||
         a.mode != b.mode || a.bleConnected != b.bleConnected ||
         a.kbPerSec != b.kbPerSec || a.dropped != b.dropped ||
         (a.holdMs / 100u) != (b.holdMs / 100u) ||
         (a.uptimeMs / 1000u) != (b.uptimeMs / 1000u);
}
```

`holdMs` is compared at 100 ms resolution so the countdown animates without repainting
on every single frame.

- [ ] **Step 3: Rewrite `Display::drawHeader` in `src/ui/Display.cpp`**

```cpp
void Display::drawHeader(const DeviceStatus& status) {
  // No fillRect here on purpose. setTextPadding makes each drawString paint its
  // own background out to a fixed width, so the new value covers the old one in
  // the same operation and the bar is never momentarily blank.
  tft_.setTextFont(2);
  tft_.setTextSize(1);
  tft_.setTextColor(TFT_WHITE, TFT_NAVY);

  char topLeft[40];
  char topRight[40];
  char bottomLeft[40];

  if (status.mode == RadioMode::Wifi) {
    snprintf(topLeft, sizeof(topLeft), "%s", status.ssid);
    snprintf(topRight, sizeof(topRight), "%s  clients:%u", status.ip,
             (unsigned)status.clients);
    snprintf(bottomLeft, sizeof(bottomLeft), "%s", status.apUp ? "AP UP" : "AP FAIL");
  } else {
    snprintf(topLeft, sizeof(topLeft), "teletrack");
    snprintf(topRight, sizeof(topRight), "%s",
             status.bleConnected ? "BLE CONN" : "BLE ADV");
    if (status.bleConnected) {
      snprintf(bottomLeft, sizeof(bottomLeft), "%u kB/s drop:%u",
               (unsigned)status.kbPerSec, (unsigned)status.dropped);
    } else {
      snprintf(bottomLeft, sizeof(bottomLeft), "WAITING");
    }
  }

  // A hold in progress takes over the bottom-left field: it is the only
  // feedback that the button is doing anything.
  if (status.holdMs > 0) {
    snprintf(bottomLeft, sizeof(bottomLeft), "HOLD %lus",
             (unsigned long)((HoldDetector::kHoldMs - status.holdMs) / 1000u + 1u));
  }

  tft_.setTextDatum(TL_DATUM);
  tft_.setTextPadding(kHeaderLeftWidth);
  tft_.drawString(topLeft, 4, 2);
  tft_.drawString(bottomLeft, 4, 20);

  char stamp[9];
  Format::uptime(status.uptimeMs, stamp, sizeof(stamp));
  char bottomRight[40];
  snprintf(bottomRight, sizeof(bottomRight), "up %s", stamp);

  tft_.setTextDatum(TR_DATUM);
  tft_.setTextPadding(kHeaderRightWidth);
  tft_.drawString(topRight, tft_.width() - 4, 2);
  tft_.drawString(bottomRight, tft_.width() - 4, 20);

  tft_.setTextPadding(0);  // drawLog pads its own lines
  tft_.setTextDatum(TL_DATUM);
}
```

Add `#include "radio/HoldDetector.h"` to `Display.cpp` for `kHoldMs`.

- [ ] **Step 4: Verify**

```bash
pio run -e esp
pio test -e native
```

Expected: `SUCCESS` and 77 passing.

- [ ] **Step 5: Commit**

```bash
git add src/core/DeviceStatus.h src/ui/Display.h src/ui/Display.cpp
git commit -m "Show radio mode and BLE state on the screen"
```

---

### Task 7: Wire it together in `main.cpp`

`main.cpp` owns the switch: it reads the button, asks `ModeController` what should be
running, and does the teardown and startup.

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1–6.

**The one rule that matters:** the outgoing radio is fully down before the incoming one
starts. Both drive the same front end, and starting BLE while the WiFi driver still
holds the radio is the failure this ordering avoids.

- [ ] **Step 1: Write `src/main.cpp`**

```cpp
#ifndef PIO_UNIT_TESTING

#include <Arduino.h>

#include "ble/BleLink.h"
#include "ble/TelemetryRing.h"
#include "config/ConfigPortal.h"
#include "config/Settings.h"
#include "core/DeviceStatus.h"
#include "core/Log.h"
#include "radio/ModeButton.h"
#include "radio/RadioMode.h"
#include "ui/Display.h"

namespace {

// Synthetic telemetry until GPS and IMU exist: same cadence and packet size as
// the real thing, so the throughput measured here is the throughput Phase 3 gets.
constexpr uint32_t kSampleHz = 200;
constexpr uint32_t kSampleIntervalMs = 1000 / kSampleHz;  // 5 ms

// The one global. Everything else is reached through it by reference.
struct App {
  Settings settings = Settings::defaults();
  ConfigPortal portal{settings};
  TelemetryRing ring;
  BleLink ble;
  ModeButton button;
  ModeController modes;
  Display display;
};

App app;

uint32_t g_lastSampleMs = 0;
uint32_t g_rateWindowMs = 0;
uint32_t g_rateWindowBytes = 0;
uint32_t g_kbPerSec = 0;

void startCurrentMode() {
  if (app.modes.mode() == RadioMode::Wifi) {
    if (!app.portal.begin()) {
      Log::error("boot", "config portal failed to start");
    }
  } else {
    if (!app.ble.begin("teletrack", app.ring)) {
      // BLE is the boot mode and the button may not be wired, so falling back
      // to WiFi is the difference between a recoverable device and a brick.
      Log::error("ble", "failed to start, falling back to wifi");
      app.modes.handle(ModeEvent::ButtonHeld);
      app.portal.begin();
    }
  }
  app.modes.switchComplete();
}

void stopCurrentMode(RadioMode leaving) {
  if (leaving == RadioMode::Wifi) {
    app.portal.end();
  } else {
    app.ble.end();
  }
}

void produceSample(uint32_t nowMs) {
  if (nowMs - g_lastSampleMs < kSampleIntervalMs) {
    return;
  }
  g_lastSampleMs = nowMs;

  // Phase 3 replaces this with real GPS and IMU fields. The pattern is
  // deliberate: a receiver can spot corruption as well as loss.
  uint8_t payload[TelemetrySample::kPayloadBytes];
  for (size_t i = 0; i < sizeof(payload); ++i) {
    payload[i] = static_cast<uint8_t>(nowMs + i);
  }
  app.ring.push(nowMs, payload);
}

DeviceStatus buildStatus(uint32_t nowMs) {
  DeviceStatus s = app.portal.status();
  s.mode = app.modes.mode();
  s.bleConnected = app.ble.connected();
  s.kbPerSec = g_kbPerSec;
  s.dropped = app.ring.dropped();
  s.holdMs = app.button.heldMs(nowMs);
  s.uptimeMs = nowMs;
  return s;
}

void updateRate(uint32_t nowMs) {
  if (nowMs - g_rateWindowMs < 1000) {
    return;
  }
  const uint32_t sent = app.ble.sentBytes();
  g_kbPerSec = (sent - g_rateWindowBytes) / 1024;
  g_rateWindowBytes = sent;
  g_rateWindowMs = nowMs;
}

}  // namespace

void setup() {
  Log::begin(115200);
  if (!app.display.begin()) {
    Log::error("tft", "display init failed, serial only");
  }
  Log::info("boot", "teletrack");

  app.button.begin();
  startCurrentMode();
}

void loop() {
  const uint32_t now = millis();

  if (app.button.tick(now)) {
    const RadioMode leaving = app.modes.mode();
    if (app.modes.handle(ModeEvent::ButtonHeld)) {
      Log::info("mode", "switching");
      // Down before up: both radios share one front end.
      stopCurrentMode(leaving);
      startCurrentMode();
    }
  }

  if (app.modes.mode() == RadioMode::Wifi) {
    app.portal.tick(now);
  } else {
    produceSample(now);
    app.ble.tick(now);
  }

  updateRate(now);
  app.display.tick(now, buildStatus(now));
  delay(1);  // yield to the WiFi and BLE tasks
}

#endif  // PIO_UNIT_TESTING
```

- [ ] **Step 2: Verify**

```bash
pio run -e esp
pio test -e native
```

Expected: `SUCCESS` and 77 passing. Record the final flash and RAM figures.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "Wire mode switching, BLE and the screen together"
```

---

### Task 8: Laptop receiver and the throughput measurement

The deliverable of this phase is a measured number, and this is where it gets measured.

**Files:**
- Create: `tools/ble_throughput.py`
- Modify: `README.md`

- [ ] **Step 1: Write `tools/ble_throughput.py`**

```python
"""Measure teletrack's BLE telemetry throughput from a laptop.

    pip install bleak
    python3 tools/ble_throughput.py [--seconds 30]

Reports sustained kB/s, notification rate, and sample loss derived from
sequence-number gaps.

IMPORTANT: on macOS this measures a FLOOR, not the device's ceiling.
CoreBluetooth negotiates the ATT MTU itself (typically 185 bytes, not 517) and
gives applications no control over the connection interval, so expect roughly
half the figure an Android client gets. Use this for repeatable regression
checks; take the headline number from Android.
"""

import argparse
import asyncio
import struct
import time

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "teletrack"
LIVE_UUID = "6f4a0002-3f2b-4d15-9c7e-1a2b3c4d5e6f"

SAMPLE_SIZE = 20
SEQ_OFFSET = 0


class Stats:
    def __init__(self):
        self.notifications = 0
        self.bytes = 0
        self.samples = 0
        self.first_seq = None
        self.last_seq = None
        self.gaps = 0

    def feed(self, data: bytes) -> None:
        self.notifications += 1
        self.bytes += len(data)
        for offset in range(0, len(data) - SAMPLE_SIZE + 1, SAMPLE_SIZE):
            (seq,) = struct.unpack_from("<I", data, offset + SEQ_OFFSET)
            self.samples += 1
            if self.first_seq is None:
                self.first_seq = seq
            elif seq != self.last_seq + 1:
                # Sequence numbers are assigned by the device before the ring
                # can drop anything, so a gap is exactly the count lost.
                self.gaps += seq - self.last_seq - 1
            self.last_seq = seq

    def report(self, elapsed: float, mtu: int) -> None:
        expected = (self.last_seq - self.first_seq + 1) if self.first_seq is not None else 0
        loss = (100.0 * self.gaps / expected) if expected else 0.0
        print()
        print(f"  negotiated MTU     {mtu} bytes")
        print(f"  duration           {elapsed:.1f} s")
        print(f"  throughput         {self.bytes / elapsed / 1024:.1f} kB/s")
        print(f"  notifications      {self.notifications / elapsed:.0f}/s")
        print(f"  samples received   {self.samples} ({self.samples / elapsed:.0f}/s)")
        print(f"  samples lost       {self.gaps} ({loss:.2f}%)")
        print()
        if mtu < 200:
            print("  NOTE: MTU under 200 means this host capped it (macOS/iOS do).")
            print("        This is a floor. Measure on Android for the real number.")


async def main(seconds: int) -> None:
    print(f"scanning for {DEVICE_NAME!r}...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
    if device is None:
        raise SystemExit(f"no device advertising as {DEVICE_NAME!r} found")

    print(f"connecting to {device.address}...")
    stats = Stats()
    async with BleakClient(device) as client:
        mtu = getattr(client, "mtu_size", 0)
        print(f"connected, MTU {mtu}, collecting for {seconds}s")

        await client.start_notify(LIVE_UUID, lambda _, data: stats.feed(data))
        started = time.monotonic()
        await asyncio.sleep(seconds)
        elapsed = time.monotonic() - started
        await client.stop_notify(LIVE_UUID)

    stats.report(elapsed, mtu)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=30)
    asyncio.run(main(parser.parse_args().seconds))
```

- [ ] **Step 2: Install `bleak`**

```bash
python3 -m pip install bleak
```

- [ ] **Step 3: Flash and measure from the laptop**

```bash
pio run -e esp -t upload
python3 tools/ble_throughput.py --seconds 30
```

Record the reported MTU, kB/s and loss percentage. On macOS expect an MTU near 185
and roughly half the Android figure — that is the platform, not a fault.

- [ ] **Step 4: Measure from Android — the headline number**

Use nRF Connect (or any BLE client that requests a large MTU): connect to
`teletrack`, subscribe to the `live` characteristic, and observe the throughput it
reports.

Record the negotiated MTU alongside the rate. **A rate without its MTU is not
interpretable** — the acceptance target of ≥ 30 kB/s assumes a 517-byte MTU.

- [ ] **Step 5: Manual acceptance**

1. Power on → screen shows `BLE ADV` / `WAITING`.
2. Connect a phone → `BLE CONN` with a live kB/s and a drop count.
3. Disconnect → back to `BLE ADV`, device keeps running.
4. Hold GPIO4 for 3 s → screen counts down, then `AP UP`; the Phase 1 config page is
   reachable at `192.168.4.1`.
5. Hold GPIO4 again → back to `BLE ADV`, and a phone can reconnect.
6. Power-cycle → boots to `BLE ADV`.

- [ ] **Step 6: Record the numbers in this plan**

Append the measured figures to this file under a `## Measured results` heading:
platform, negotiated MTU, sustained kB/s, loss percentage, date. A number nobody
wrote down is a number nobody can regress against.

- [ ] **Step 7: Update `README.md`**

Add a Phase 2 line to the Status section, the mode-button behaviour, and the
throughput tool under Building:

```markdown
| `python3 tools/ble_throughput.py` | Measure BLE throughput (needs `pip install bleak`) |
```

- [ ] **Step 8: Commit**

```bash
git add tools/ble_throughput.py README.md docs/superpowers/plans/2026-09-07-phase2-ble-telemetry.md
git commit -m "Add BLE throughput measurement tool and record results"
```

---

## Deviations from the spec, and why

1. **`HoldDetector` split out of `ModeButton`** (Task 2). The spec names only
   `ModeButton`. Splitting the timing into a pure class is what lets the 3-second
   behaviour be host-tested while GPIO4 is unwired.
2. **NimBLE pinned to 1.4.3, not 2.5.1.** The 2.x line needs Arduino core 3.x; this
   project is on 2.0.17. The spec was corrected before this plan was written.
3. **No host test for `ConfigPortal::end()`** (Task 4). It is Arduino teardown calls
   with no logic; a host test would assert nothing. Covered by manual acceptance.
4. **BLE-start failure falls back to WiFi** (Task 7). The spec's §10 calls for this;
   it matters more than usual because the button may not be wired, so it is the
   difference between a recoverable device and a reflash.
