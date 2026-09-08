# Sleep Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A triple click on the mode button puts the T-Beam Supreme into deep sleep at roughly 60 µA with the GPS holding its almanac, and a press wakes it to a fix in seconds.

**Architecture:** `HoldDetector` grows click counting and returns an event instead of a bool; the GPS goes into u-blox software backup rather than losing power; every unused rail is switched off, LoRa permanently.

**Tech Stack:** PlatformIO, Arduino-ESP32 2.0.17, C++17, XPowersLib (AXP2101), U8g2 (SH1106), UBX, Unity host tests.

**Spec:** `docs/superpowers/specs/2026-09-08-sleep-mode-design.md`

## Global Constraints

- **T-Beam Supreme only.** The DevKitC's mode button is **GPIO39, which is not RTC-capable** on the ESP32-S3 (RTC GPIOs stop at 21), so it cannot wake from deep sleep. A triple click there must log that sleep is unsupported and do nothing — never sleep a board that cannot wake.
- **The 3-second hold keeps its exact current behaviour:** fires while the button is still down, at the instant the hold reaches `kHoldMs`, with the same on-screen countdown. Triple click is purely additive.
- **A warm or hot GPS start after sleep is the requirement the phase exists for.** It is delivered by `UBX-RXM-PMREQ` software backup with ALDO4 **left powered** — not by cutting the GPS rail and hoping `V_BCKP` is wired to a backup supply.
- **UBX-RXM-PMREQ:** class `0x02`, id `0x41`, 16-byte payload, little-endian: version `0x00`, 3 reserved, duration `0` (indefinite), flags `0x06` (backup | force), 3 reserved, wakeupSources `0x08` (UART RX). **Do not wait for an ACK** — the receiver may go down before sending one.
- **A receiver in software backup is silent and wakes on UART activity.** The baud probe must **send** filler bytes before listening, or the GPS reads as absent at every baud on every boot after the first sleep.
- **LoRa (ALDO3) is disabled at boot, unconditionally**, on every start. The AXP2101 enables every rail by itself, so it has been running since the board was first flashed.
- **ALDO4 stays enabled during sleep.** Every other unused rail goes down.
- Verified XPowersLib names: `disableALDO1`…`disableALDO4`, `disableBLDO1`, `disableBLDO2`, `disableDC1`…`disableDC5`. U8g2: `setPowerSave(uint8_t)`.
- Comments explain *why*, not *what*. POC discipline: no defensive validation for callers that do not exist.
- **Commit messages must never contain a `Co-Authored-By` trailer or any AI attribution.**

## File Structure

| File | Responsibility |
| --- | --- |
| `src/radio/HoldDetector.{h,cpp}` | `ButtonEvent`, click counting, debounce. Pure, **host-tested**. |
| `test/native/test_mode_button/test_mode_button.cpp` | Rewritten for the event contract; new triple-click cases. |
| `src/radio/ModeButton.h` | Returns `ButtonEvent` instead of `bool`. |
| `src/board/Pmu.{h,cpp}` | LoRa off at boot; `prepareForSleep()`. |
| `src/gps/GpsReceiver.{h,cpp}` | `sleep()` sending RXM-PMREQ; wake bytes in the probe. |
| `src/ui/Display.h`, `OledDisplay.{h,cpp}` | `sleep()` — show `SLEEPING`, then blank the panel. |
| `src/main.cpp` | Acts on the events; owns the sleep sequence. |

---

### Task 1: `HoldDetector` reports events, and counts clicks

**Files:**
- Modify: `src/radio/HoldDetector.h`, `src/radio/HoldDetector.cpp`
- Modify: `src/radio/ModeButton.h`, `src/main.cpp`
- Test: `test/native/test_mode_button/test_mode_button.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class ButtonEvent : uint8_t { None, Hold, TripleClick };` in `src/radio/HoldDetector.h`; `ButtonEvent HoldDetector::update(bool, uint32_t)`; `ButtonEvent ModeButton::tick(uint32_t)`. Constants `kClickMaxMs = 500`, `kMultiClickWindowMs = 1200`, `kClicksForSleep = 3`.

- [ ] **Step 1: Rewrite the tests**

Replace the whole of `test/native/test_mode_button/test_mode_button.cpp` with:

```cpp
#include <unity.h>

#include "radio/HoldDetector.h"

void setUp() {}
void tearDown() {}

namespace {

// Feeds a press of `pressMs`, then a release of `releaseMs`, one sample per
// millisecond, and returns the last non-None event seen. One sample per ms is
// far finer than loop() manages, which is the point: the detector must not
// depend on sample spacing.
ButtonEvent feed(HoldDetector& d, uint32_t& now, uint32_t pressMs,
                 uint32_t releaseMs) {
  ButtonEvent seen = ButtonEvent::None;
  for (uint32_t i = 0; i < pressMs; ++i) {
    const ButtonEvent e = d.update(true, now++);
    if (e != ButtonEvent::None) {
      seen = e;
    }
  }
  for (uint32_t i = 0; i < releaseMs; ++i) {
    const ButtonEvent e = d.update(false, now++);
    if (e != ButtonEvent::None) {
      seen = e;
    }
  }
  return seen;
}

}  // namespace

static void test_idle_never_fires() {
  HoldDetector d;
  for (uint32_t t = 0; t < 10000; t += 10) {
    TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(false, t));
  }
}

static void test_hold_fires_once_at_the_threshold_while_still_down() {
  HoldDetector d;
  TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(true, 0));
  TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(true, HoldDetector::kHoldMs - 1));
  TEST_ASSERT_EQUAL(ButtonEvent::Hold, d.update(true, HoldDetector::kHoldMs));
  // Still held: must not repeat.
  TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(true, HoldDetector::kHoldMs + 5000));
}

static void test_release_before_the_threshold_fires_no_hold() {
  HoldDetector d;
  d.update(true, 0);
  d.update(true, 2999);
  TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(false, 3000));
}

static void test_release_then_hold_again_fires_again() {
  HoldDetector d;
  d.update(true, 0);
  TEST_ASSERT_EQUAL(ButtonEvent::Hold, d.update(true, 3000));
  for (uint32_t t = 3001; t < 3200; ++t) {
    d.update(false, t);
  }
  d.update(true, 4000);
  TEST_ASSERT_EQUAL(ButtonEvent::Hold, d.update(true, 7000));
}

static void test_bounce_shorter_than_debounce_does_not_restart_the_hold() {
  HoldDetector d;
  d.update(true, 0);
  d.update(false, 1000);              // contact drops
  d.update(true, 1000 + HoldDetector::kDebounceMs - 1);  // and returns
  // The hold never broke, so it still completes 3000 ms after the first press.
  TEST_ASSERT_EQUAL(ButtonEvent::Hold, d.update(true, 3000));
}

static void test_three_clicks_fire_triple_click() {
  HoldDetector d;
  uint32_t now = 0;
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
  TEST_ASSERT_EQUAL(ButtonEvent::TripleClick, feed(d, now, 60, 100));
}

static void test_two_clicks_alone_fire_nothing() {
  HoldDetector d;
  uint32_t now = 0;
  feed(d, now, 60, 100);
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
}

static void test_a_fourth_click_does_not_fire_again() {
  HoldDetector d;
  uint32_t now = 0;
  feed(d, now, 60, 100);
  feed(d, now, 60, 100);
  TEST_ASSERT_EQUAL(ButtonEvent::TripleClick, feed(d, now, 60, 100));
  // The sequence is consumed; a lone click afterwards starts a new one.
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
}

static void test_clicks_spread_past_the_window_do_not_accumulate() {
  HoldDetector d;
  uint32_t now = 0;
  feed(d, now, 60, 100);  // click one
  // Click two, then a gap longer than the window. Note the gap has to come
  // BEFORE the third click: put it after and the third click completes the
  // sequence normally and the test passes for the wrong reason.
  feed(d, now, 60, HoldDetector::kMultiClickWindowMs + 200);
  // So this click lands outside the window the first one opened, and starts a
  // fresh sequence rather than completing the stale one.
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
}

static void test_a_long_press_is_not_a_click() {
  HoldDetector d;
  uint32_t now = 0;
  feed(d, now, 60, 100);
  feed(d, now, 60, 100);
  // Two clicks, then a press longer than kClickMaxMs. It must not complete the
  // sequence -- it resets it -- so the next click cannot fire TripleClick.
  feed(d, now, HoldDetector::kClickMaxMs + 100, 100);
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
}

static void test_a_hold_still_works_after_two_clicks() {
  HoldDetector d;
  uint32_t now = 0;
  feed(d, now, 60, 100);
  feed(d, now, 60, 100);
  ButtonEvent seen = ButtonEvent::None;
  for (uint32_t i = 0; i < 3100; ++i) {
    const ButtonEvent e = d.update(true, now++);
    if (e != ButtonEvent::None) {
      seen = e;
    }
  }
  TEST_ASSERT_EQUAL(ButtonEvent::Hold, seen);
}

static void test_bouncing_contacts_do_not_manufacture_a_triple_click() {
  // The failure this guards against: a worn switch chattering three times
  // inside the window and putting the board to sleep on its own.
  HoldDetector d;
  uint32_t now = 0;
  d.update(true, now++);
  for (int bounce = 0; bounce < 6; ++bounce) {
    for (uint32_t i = 0; i < HoldDetector::kDebounceMs - 5; ++i) {
      TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(false, now++));
    }
    for (uint32_t i = 0; i < 5; ++i) {
      TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(true, now++));
    }
  }
}

static void test_held_ms_reports_progress_and_zero_when_idle() {
  HoldDetector d;
  TEST_ASSERT_EQUAL_UINT32(0, d.heldMs(0));
  d.update(true, 1000);
  TEST_ASSERT_EQUAL_UINT32(500, d.heldMs(1500));
  d.update(false, 2000);
  TEST_ASSERT_EQUAL_UINT32(0, d.heldMs(2500));
}

static void test_held_ms_keeps_counting_past_the_hold_threshold() {
  // The header reads this to decide what to show, and it must not wrap or
  // saturate at kHoldMs.
  HoldDetector d;
  d.update(true, 0);
  d.update(true, 3000);
  TEST_ASSERT_EQUAL_UINT32(9000, d.heldMs(9000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_idle_never_fires);
  RUN_TEST(test_hold_fires_once_at_the_threshold_while_still_down);
  RUN_TEST(test_release_before_the_threshold_fires_no_hold);
  RUN_TEST(test_release_then_hold_again_fires_again);
  RUN_TEST(test_bounce_shorter_than_debounce_does_not_restart_the_hold);
  RUN_TEST(test_three_clicks_fire_triple_click);
  RUN_TEST(test_two_clicks_alone_fire_nothing);
  RUN_TEST(test_a_fourth_click_does_not_fire_again);
  RUN_TEST(test_clicks_spread_past_the_window_do_not_accumulate);
  RUN_TEST(test_a_long_press_is_not_a_click);
  RUN_TEST(test_a_hold_still_works_after_two_clicks);
  RUN_TEST(test_bouncing_contacts_do_not_manufacture_a_triple_click);
  RUN_TEST(test_held_ms_reports_progress_and_zero_when_idle);
  RUN_TEST(test_held_ms_keeps_counting_past_the_hold_threshold);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `export PATH="$HOME/.platformio/penv/bin:$PATH" && pio test -e native -f native/test_mode_button`
Expected: a compile error — `'ButtonEvent' was not declared in this scope`.

- [ ] **Step 3: Rewrite `src/radio/HoldDetector.h`**

```cpp
#pragma once

#include <stdint.h>

// What the button did. Nothing else about the pin escapes this module.
enum class ButtonEvent : uint8_t {
  None,
  Hold,         // held for kHoldMs, reported while still down
  TripleClick,  // three short presses inside kMultiClickWindowMs
};

// Turns a stream of (pressed, now) samples into button events.
//
// Pure and clock-free: the caller supplies the timestamp, so every threshold
// here is host-testable without a button or a board.
//
// Both gestures live on one pin because it is the only pin that can wake the
// chip -- see the sleep-mode design. They stay distinguishable because a hold
// is not a click: a press longer than kClickMaxMs resets the click sequence
// rather than extending it.
class HoldDetector {
 public:
  static constexpr uint32_t kHoldMs = 3000;
  // A release shorter than this is contact bounce and does not reset the hold.
  static constexpr uint32_t kDebounceMs = 30;
  // A press shorter than this counts as a click rather than a hold.
  static constexpr uint32_t kClickMaxMs = 500;
  // All the clicks of a sequence must land within this of the first.
  static constexpr uint32_t kMultiClickWindowMs = 1200;
  static constexpr uint8_t kClicksForSleep = 3;

  ButtonEvent update(bool pressed, uint32_t nowMs);

  // Milliseconds held so far, 0 when not pressed. Drives the screen countdown,
  // and keeps counting past kHoldMs.
  uint32_t heldMs(uint32_t nowMs) const;

  bool isHolding() const { return pressed_; }

 private:
  bool pressed_ = false;
  bool fired_ = false;
  uint32_t pressStartMs_ = 0;
  // When a release began, used to tell bounce from a real release.
  bool releasing_ = false;
  uint32_t releaseStartMs_ = 0;
  // How long the press that is currently being released lasted. Classifying it
  // has to wait until the release survives the debounce window.
  uint32_t lastPressMs_ = 0;
  uint8_t clicks_ = 0;
  uint32_t firstClickMs_ = 0;
};
```

- [ ] **Step 4: Rewrite `src/radio/HoldDetector.cpp`**

```cpp
#include "radio/HoldDetector.h"

ButtonEvent HoldDetector::update(bool pressed, uint32_t nowMs) {
  if (pressed) {
    if (!pressed_) {
      if (releasing_ && (nowMs - releaseStartMs_) < kDebounceMs) {
        // Bounce: contact came back inside the debounce window, so the press
        // never really ended. Do not restart the timer, and do not let it
        // count as a click -- three chattering contacts must not read as a
        // deliberate triple click.
        pressed_ = true;
        releasing_ = false;
      } else {
        pressed_ = true;
        fired_ = false;
        pressStartMs_ = nowMs;
        releasing_ = false;
      }
    }

    if (!fired_ && (nowMs - pressStartMs_) >= kHoldMs) {
      fired_ = true;
      // A hold ends any click sequence in progress: the two gestures are
      // alternatives, not stages of one another.
      clicks_ = 0;
      return ButtonEvent::Hold;
    }
    return ButtonEvent::None;
  }

  // Not pressed.
  if (pressed_) {
    pressed_ = false;
    releasing_ = true;
    releaseStartMs_ = nowMs;
    lastPressMs_ = nowMs - pressStartMs_;
    return ButtonEvent::None;
  }

  if (releasing_ && (nowMs - releaseStartMs_) >= kDebounceMs) {
    // The release outlasted the debounce window, so it was real and the press
    // it ended can finally be classified.
    releasing_ = false;
    fired_ = false;

    if (lastPressMs_ >= kClickMaxMs) {
      clicks_ = 0;  // that was a hold, not a click
      return ButtonEvent::None;
    }

    if (clicks_ == 0 || (nowMs - firstClickMs_) > kMultiClickWindowMs) {
      clicks_ = 1;
      firstClickMs_ = nowMs;
    } else {
      ++clicks_;
    }

    if (clicks_ >= kClicksForSleep) {
      clicks_ = 0;
      return ButtonEvent::TripleClick;
    }
  }
  return ButtonEvent::None;
}

uint32_t HoldDetector::heldMs(uint32_t nowMs) const {
  if (!pressed_) {
    return 0;
  }
  return nowMs - pressStartMs_;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `pio test -e native -f native/test_mode_button`
Expected: `14 test cases: 14 succeeded`

- [ ] **Step 6: Return the event from `ModeButton`**

In `src/radio/ModeButton.h`, replace the `tick` declaration and its comment:

```cpp
  // Call from loop(). Reports what the button did on this sample.
  ButtonEvent tick(uint32_t nowMs);
```

In `src/radio/ModeButton.cpp`, change the signature only — the body already forwards straight to the detector:

```cpp
ButtonEvent ModeButton::tick(uint32_t nowMs) {
  const bool pressed = digitalRead(kPin) == LOW;  // pull-up: LOW means pressed
  return detector_.update(pressed, nowMs);
}
```

`ModeButton.h` already includes `radio/HoldDetector.h`, so `ButtonEvent` is in scope without a new include.

- [ ] **Step 7: Handle the events in `src/main.cpp`**

Replace the button block in `loop()`:

```cpp
  if (app.button.tick(now)) {
    const RadioMode leaving = app.modes.mode();
    if (app.modes.handle(ModeEvent::ButtonHeld)) {
      Log::info("mode", "switching");
      // Down before up: both radios share one front end.
      stopCurrentMode(leaving);
      startCurrentMode();
    }
  }
```

with:

```cpp
  switch (app.button.tick(now)) {
    case ButtonEvent::Hold: {
      const RadioMode leaving = app.modes.mode();
      if (app.modes.handle(ModeEvent::ButtonHeld)) {
        Log::info("mode", "switching");
        // Down before up: both radios share one front end.
        stopCurrentMode(leaving);
        startCurrentMode();
      }
      break;
    }
    case ButtonEvent::TripleClick:
      // Task 4 turns this into an actual sleep.
      Log::info("sleep", "requested");
      break;
    case ButtonEvent::None:
      break;
  }
```

- [ ] **Step 8: Build both environments and run the whole suite**

Run: `pio run -e esp && pio run -e tbeam && pio test -e native`
Expected: `SUCCESS` for both, and `123 test cases: 123 succeeded` (118 before, minus the 9 old button tests, plus the 14 new ones).

- [ ] **Step 9: Commit**

```bash
git add src/radio/HoldDetector.h src/radio/HoldDetector.cpp src/radio/ModeButton.h src/radio/ModeButton.cpp src/main.cpp test/native/test_mode_button/test_mode_button.cpp
git commit -m "Report button events, and count triple clicks

The button has to carry two gestures because GPIO0 is the only pin that
can wake the chip from deep sleep. They stay apart by construction: a
press longer than kClickMaxMs is a hold, and it resets the click count
rather than extending it.

The 3-second hold is untouched -- it still fires while the button is
down, at the same instant, with the same countdown. Triple click is
purely additive, which a longer hold could not have been: it would have
passed three seconds on the way and switched the radio first.

Clicks are counted only once a release outlasts the debounce window, so
a chattering contact cannot manufacture a triple click and put the board
to sleep on its own. That case has its own test."
```

---

### Task 2: LoRa off permanently, and the rails that sleep

**Files:**
- Modify: `src/board/Pmu.h`, `src/board/Pmu.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `void Pmu::prepareForSleep()` — disables every unused rail, leaving ALDO4 and DCDC1 alone. A no-op on a board with no PMU.

- [ ] **Step 1: Declare `prepareForSleep` in `src/board/Pmu.h`**

Add after the existing `void tick(uint32_t nowMs);`:

```cpp
  // Switches off every rail that is not needed while asleep. ALDO4 is left ON
  // deliberately: the GPS holds its own almanac in software backup and needs
  // its supply to do it. DCDC1 is the ESP32's own and is never touched.
  void prepareForSleep();
```

- [ ] **Step 2: Turn LoRa off at boot in `src/board/Pmu.cpp`**

In `Pmu::begin()`, immediately after the `enableALDO4()` call and before the backup-charger lines, add:

```cpp
  // LoRa lives on ALDO3 and this project does not use it. The AXP2101 brings
  // every rail up by itself -- the boot log showed ALDO1 through BLDO2 all
  // enabled before this function ever ran -- so the radio has been powered
  // since the board was first flashed. Off, on every boot, not just in sleep.
  g_pmu.disableALDO3();
```

- [ ] **Step 3: Implement `prepareForSleep` in the T-Beam branch**

Add after `Pmu::tick()`, still inside `#if defined(BOARD_TBEAM)`:

```cpp
void Pmu::prepareForSleep() {
  if (!present_) {
    return;
  }
  // ALDO3 is already off from begin(). ALDO4 stays on: cutting it is what
  // would cost a warm GPS start, which is the whole point of the phase.
  g_pmu.disableALDO1();  // sensors
  g_pmu.disableALDO2();  // SD card
  g_pmu.disableBLDO1();
  g_pmu.disableBLDO2();
  g_pmu.disableDC3();  // M.2 interface
  g_pmu.disableDC4();
  g_pmu.disableDC5();
  Log::info("sleep", "rails down, GPS rail held");
}
```

- [ ] **Step 4: Add the no-op to the `#else` branch**

```cpp
void Pmu::prepareForSleep() {}  // no PMU on this board
```

- [ ] **Step 5: Build both environments**

Run: `export PATH="$HOME/.platformio/penv/bin:$PATH" && pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both.

Every method above was verified present in the installed XPowersLib: `disableALDO1` (`XPowersAXP2101.tpp:1761`), `disableALDO2` (`:1803`), `disableALDO3` (`:1845`), `disableBLDO1` (`:1929`), `disableBLDO2` (`:1975`), `disableDC3` (`:1529`), `disableDC4` (`:1618`), `disableDC5` (`:1684`). Do not invent a name if one fails to resolve — report it.

- [ ] **Step 6: Run the host tests**

Run: `pio test -e native`
Expected: `123 test cases: 123 succeeded`, unchanged — nothing here is host-compiled.

- [ ] **Step 7: Commit**

```bash
git add src/board/Pmu.h src/board/Pmu.cpp
git commit -m "Switch LoRa off at boot, and add the sleep rail sequence

The rail log shows every output enabled at power-up, so ALDO3 has been
powering a LoRa radio this project does not use since the board was
first flashed. Disabling it at boot is a continuous saving, not only a
sleep one.

prepareForSleep() drops the sensors, SD card and M.2 rails. ALDO4 stays
up on purpose: the receiver keeps its almanac in software backup and
needs its supply to hold it, which is what makes a warm start after
sleep a guarantee rather than a hope about how V_BCKP is wired."
```

---

### Task 3: Put the GPS into software backup, and wake it again

**Files:**
- Modify: `src/gps/GpsReceiver.h`, `src/gps/GpsReceiver.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `void GpsReceiver::sleep()` — sends `UBX-RXM-PMREQ` and returns without waiting for an ACK. A no-op when no receiver is present or on a board without one.

- [ ] **Step 1: Declare `sleep()` in `src/gps/GpsReceiver.h`**

Add after `bool tick();`:

```cpp
  // Puts the receiver into u-blox software backup: it keeps its ephemeris,
  // almanac, time and last position in its own memory at about 15 uA, while
  // its supply stays on. This is what makes the first fix after a wake take
  // seconds rather than tens of seconds, and it does not depend on how this
  // board wires the receiver's V_BCKP pin.
  void sleep();
```

- [ ] **Step 2: Implement it in the T-Beam branch of `src/gps/GpsReceiver.cpp`**

Add after `GpsReceiver::tick()`, inside `#if defined(BOARD_TBEAM)`:

```cpp
void GpsReceiver::sleep() {
  if (!present_) {
    return;
  }
  // UBX-RXM-PMREQ, taken from the reference implementation's
  // powerOffWithInterrupt() rather than reconstructed from the protocol
  // tables. Little-endian throughout, like all of UBX.
  const uint8_t payload[16] = {
      0x00, 0x00, 0x00, 0x00,  // version 0, then three reserved bytes
      0x00, 0x00, 0x00, 0x00,  // duration 0: indefinite, until woken
      0x06, 0x00, 0x00, 0x00,  // flags: backup | force
      0x08, 0x00, 0x00, 0x00,  // wakeupSources: UART RX
  };
  sendUbx(0x02, 0x41, payload, sizeof(payload));
  // No ACK is waited for. The receiver may go down before it sends one, and
  // waiting would only delay the sleep.
  Log::info("gps", "receiver in software backup");
}
```

- [ ] **Step 3: Add the no-op to the `#else` branch**

```cpp
void GpsReceiver::sleep() {}  // no receiver on this board
```

- [ ] **Step 4: Wake the receiver before probing, in the same file**

In the anonymous namespace, replace `probe()`:

```cpp
bool probe(uint32_t baud) {
  Serial1.begin(baud, SERIAL_8N1, BoardConfig::kGpsRxPin, BoardConfig::kGpsTxPin);

  // A receiver left in software backup is silent and wakes on UART activity,
  // so listening alone would report it absent at every baud -- on this boot
  // and every boot after, since it would stay in backup. Rattle the line
  // first. The bytes are meaningless; only the edges matter.
  const uint8_t filler[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  Serial1.write(filler, sizeof(filler));
  Serial1.flush();
  delay(20);  // the M10 needs a moment to come out of backup

  const uint32_t deadline = millis() + kProbeMs;
  uint8_t prev = 0;
  while (millis() < deadline) {
    while (Serial1.available() > 0) {
      const uint8_t b = static_cast<uint8_t>(Serial1.read());
      if (b == '$' || (prev == UbxParser::kSync1 && b == UbxParser::kSync2)) {
        return true;
      }
      prev = b;
    }
    delay(5);
  }
  Serial1.end();
  return false;
}
```

- [ ] **Step 5: Build both environments**

Run: `export PATH="$HOME/.platformio/penv/bin:$PATH" && pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both.

- [ ] **Step 6: Run the host tests**

Run: `pio test -e native`
Expected: `123 test cases: 123 succeeded`, unchanged.

- [ ] **Step 7: Commit**

```bash
git add src/gps/GpsReceiver.h src/gps/GpsReceiver.cpp
git commit -m "Put the receiver into software backup instead of cutting its power

UBX-RXM-PMREQ leaves the M10 holding its own ephemeris, almanac and time
at about 15 uA while ALDO4 stays powered. That makes a warm start after
sleep a property of the receiver rather than a hope about whether this
board routes V_BCKP to a backup supply -- something no amount of reading
the software could settle.

The probe now sends before it listens. A receiver in backup wakes on
UART activity and is silent until it does, so listening alone would have
reported it absent at every baud, on every boot after the first sleep,
recoverable only by a power cycle."
```

---

### Task 4: The sleep sequence

**Files:**
- Modify: `src/ui/Display.h`, `src/ui/OledDisplay.h`, `src/ui/OledDisplay.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `ButtonEvent::TripleClick` (Task 1), `Pmu::prepareForSleep()` (Task 2), `GpsReceiver::sleep()` (Task 3).
- Produces: nothing further.

- [ ] **Step 1: Add `sleep()` to the display interface**

In `src/ui/Display.h`, add inside the class, after `tick`:

```cpp
  // Acknowledge on screen, then power the panel down. Defaulted: only the
  // board that can sleep needs to do anything here.
  virtual void sleep() {}
```

- [ ] **Step 2: Override it in `src/ui/OledDisplay.h`**

Add after `void tick(uint32_t nowMs, const DeviceStatus& status) override;`:

```cpp
  void sleep() override;
```

- [ ] **Step 3: Implement it in `src/ui/OledDisplay.cpp`**

Add at the end of the file:

```cpp
void OledDisplay::sleep() {
  if (!ready_) {
    return;
  }
  // Acknowledge the gesture before everything goes dark, or a triple click
  // looks identical to a crash.
  u8g2_.clearBuffer();
  u8g2_.drawStr(1, kRowHeight - 1, "SLEEPING");
  u8g2_.sendBuffer();
  delay(600);  // long enough to read; the board is about to stop anyway
  u8g2_.setPowerSave(1);
}
```

- [ ] **Step 4: Add the sleep sequence to `src/main.cpp`**

Add `#include <esp_sleep.h>` with the other includes, and this function in the anonymous namespace, after `stopCurrentMode`:

```cpp
#if defined(BOARD_TBEAM)

void enterSleep() {
  Log::info("sleep", "going down");
  // The radio comes down the same way a mode switch brings it down, so a
  // connected client sees a clean disconnect rather than a dead link.
  stopCurrentMode(app.modes.mode());
  app.gpsRx.sleep();
  app.display.sleep();
  app.pmu.prepareForSleep();

  // GPIO0 going low. It is a strapping pin, but a deep-sleep wake is not a
  // power-on reset: the ROM takes its fast path through the wake stub and
  // never re-reads the boot-mode straps, so waking on it cannot drop the
  // board into download mode.
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BoardConfig::kModeButtonPin), 0);
  esp_deep_sleep_start();  // does not return; a wake restarts setup()
}

#else

void enterSleep() {
  // This board's mode button is GPIO39, and the ESP32-S3's RTC GPIOs stop at
  // 21, so nothing here could wake it again. Refusing is the only safe answer.
  Log::warn("sleep", "not supported on this board");
}

#endif
```

- [ ] **Step 5: Call it from the button handler**

Replace the placeholder added in Task 1:

```cpp
    case ButtonEvent::TripleClick:
      // Task 4 turns this into an actual sleep.
      Log::info("sleep", "requested");
      break;
```

with:

```cpp
    case ButtonEvent::TripleClick:
      enterSleep();
      break;
```

- [ ] **Step 6: Build both environments**

Run: `export PATH="$HOME/.platformio/penv/bin:$PATH" && pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both.

- [ ] **Step 7: Run the host tests**

Run: `pio test -e native`
Expected: `123 test cases: 123 succeeded`.

- [ ] **Step 8: Record the behaviour in `docs/hardware-notes.md`**

Add this section immediately before the `## Serial` heading:

```markdown
## Sleep

A triple click on the mode button sleeps the T-Beam; any press wakes it.

**Why both gestures are on one button.** Keeping the GPS's memory alive means
the ESP32 deep-sleeps rather than powering off, waking from deep sleep needs an
RTC GPIO, and the ESP32-S3's stop at GPIO21 — `SOC_RTCIO_PIN_COUNT` is 22. The
AXP2101's power key reaches the CPU only on GPIO 40, so it cannot wake it. GPIO0
is the only candidate.

**The DevKitC cannot sleep at all.** Its mode button is GPIO39, past the RTC
range, so a triple click there logs and does nothing. Sleeping a board that
cannot wake would need a power cycle to recover.

**Waking on GPIO0 does not enter download mode.** The strapping is read at a
power-on reset; a deep-sleep wake takes the ROM's fast path through the wake
stub and never re-reads it.

**The GPS is put to sleep, not powered off.** `UBX-RXM-PMREQ` (class 0x02, id
0x41) places the M10 in software backup: it holds ephemeris, almanac and time in
its own memory at about 15 µA while ALDO4 stays powered. Cutting ALDO4 instead
would depend on whether this board routes `V_BCKP` to a backup supply, which is
unknown.

Two consequences, both of which otherwise present as a dead GPS:

- **A receiver in backup is silent** and wakes on UART activity, so the baud
  probe sends filler bytes before listening. Without that it reads as absent at
  every baud, on every boot after the first sleep.
- **Software backup keeps navigation data but not the RAM-layer configuration**,
  so the receiver returns at its default baud emitting NMEA. The probe and
  reconfiguration that already run on every boot cover this.

**LoRa is off from boot.** The AXP2101 enables every rail by itself, so ALDO3
had been powering a radio this project never uses.
```

- [ ] **Step 9: Commit**

```bash
git add src/ui/Display.h src/ui/OledDisplay.h src/ui/OledDisplay.cpp src/main.cpp docs/hardware-notes.md
git commit -m "Sleep on a triple click

Stops the radio the same way a mode switch does, puts the receiver into
software backup, acknowledges on screen, drops every rail except the
GPS's, and deep-sleeps waiting for GPIO0.

The DevKitC refuses: its mode button is GPIO39 and the S3's RTC GPIOs
stop at 21, so nothing could wake it again. Sleeping a board that cannot
wake would need a power cycle to recover, which is worse than not
sleeping at all."
```

---

## Hardware acceptance

Not a task — no code comes out of it — but the branch is not done until this passes.

- [ ] `pio run -e esp -t upload` on the DevKitC: TFT, BLE, mode switch and portal all behave as before, and a triple click logs `sleep: not supported on this board` without sleeping.
- [ ] `pio run -e tbeam -t upload` on the T-Beam. The rail log shows ALDO3 off.
- [ ] A 3-second hold still switches radio, with the same countdown and the same feel.
- [ ] A triple click logs, shows `SLEEPING`, and the panel goes dark.
- [ ] A press wakes it, and it boots normally rather than into download mode.
- [ ] **The GPS reaches a fix within seconds of waking, not tens of seconds.** This is the requirement the phase exists to meet.
- [ ] Charging still works while asleep on USB.
- [ ] Measure the sleep current and the time to first fix after a wake, and write both into `docs/hardware-notes.md`. Every power figure in the design is a datasheet estimate.
