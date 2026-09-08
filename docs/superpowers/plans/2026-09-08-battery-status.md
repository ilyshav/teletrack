# Battery Charging and Status Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Charge the T-Beam Supreme's 18650 properly and show its state of charge on the OLED header and in the web API.

**Architecture:** `Pmu` gains the charging configuration it never had and a `BatteryState` refreshed from the AXP2101 once a second; `DeviceStatus` carries that state to the display and `/api/status`; the OLED header is rebuilt as battery-left, radio-right.

**Tech Stack:** PlatformIO, Arduino-ESP32 2.0.17, C++17, XPowersLib (AXP2101), U8g2 (SH1106), ArduinoJson 7, Unity host tests.

**Spec:** `docs/superpowers/specs/2026-09-08-battery-status-design.md`

## Global Constraints

- **T-Beam Supreme only.** The ESP32-S3-DevKitC-1 has no PMU. Its `Pmu` is a no-op stub and `TftDisplay` must not be touched — it is the board already verified working.
- **Charging values, exact:** VBUS input limit **1500 mA**, constant current **1000 mA**, precharge **100 mA**, termination **100 mA**, target voltage **4.2 V** (unchanged), charge LED **driven by the charger**.
- **The AXP2101 cannot measure current.** It has no current ADC. Everything named `*Curr*` in XPowersLib for this part is a configured limit read back, not a measurement. Do not attempt to display current.
- **`battery()` returns a cache.** `tick(nowMs)` refreshes it at 1 Hz. `loop()` runs at roughly 1 kHz and the PMU is on I2C; per-iteration reads are not acceptable.
- **Header is 21 columns.** Battery left, radio right. Longest left `100%= 4.20V` (11), longest right `HOLD 3s` (7).
- **The right side is the radio alone** — `BLE` or `AP`. No connection state, no client count, no drop count. Those stay in `DeviceStatus` and `/api/status`.
- **The OLED's spare status row stays spare.** Phase 5 (sleep mode) needs it.
- Comments explain *why*, not *what*. POC discipline: no defensive validation for callers that do not exist.
- **Commit messages must never contain a `Co-Authored-By` trailer or any AI attribution.**

## File Structure

| File | Responsibility |
| --- | --- |
| `src/board/Pmu.h` | Adds `struct BatteryState`, `Pmu::battery()`, `Pmu::tick(uint32_t)`. |
| `src/board/Pmu.cpp` | Charging configuration; the 1 Hz AXP2101 read. Board-guarded, no host tests. |
| `src/core/DeviceStatus.h` | Carries the battery fields to display and API. |
| `src/config/internal/ConfigApi.cpp` | Serialises them into `/api/status`. **Host-tested.** |
| `test/native/test_config_api/test_config_api.cpp` | Asserts the new fields serialise. |
| `src/ui/OledDisplay.cpp` | Header rebuilt: battery left, radio right. |
| `src/main.cpp` | Calls `pmu.tick()`; fills the battery fields in `buildStatus`. |

---

### Task 1: Charging configuration and a readable battery

**Files:**
- Modify: `src/board/Pmu.h`, `src/board/Pmu.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct BatteryState { bool present; bool usbPresent; bool charging; bool full; uint8_t percent; uint16_t milliVolts; };` in `src/board/Pmu.h`, all members defaulted as shown in Step 1.
  - `BatteryState Pmu::battery() const` — returns the cache.
  - `void Pmu::tick(uint32_t nowMs)` — refreshes it at 1 Hz.

- [ ] **Step 1: Add `BatteryState` and the two methods to `src/board/Pmu.h`**

Replace the whole class body, keeping the file's existing `#pragma once` and `#include "board/BoardConfig.h"`, and adding `#include <stdint.h>`:

```cpp
// What the AXP2101 reports about the cell. On a board with no PMU every
// field stays at its default, which reads as "no battery, no USB".
struct BatteryState {
  bool present = false;      // a cell is fitted
  bool usbPresent = false;   // VBUS is up
  bool charging = false;
  bool full = false;         // the charger reports done
  uint8_t percent = 0;       // 0 when unknown
  uint16_t milliVolts = 0;   // 0 when unknown
};

// The T-Beam's AXP2101 switches the rails the display and GPS sit on, so it
// has to be brought up before either. On a board without a PMU every method
// here does nothing and returns success -- the caller does not branch.
class Pmu {
 public:
  // How often the battery registers are re-read. Charge level moves over
  // minutes; loop() runs at roughly 1 kHz. Reading every pass would put five
  // I2C transactions on the hot path for a number that cannot have changed.
  static constexpr uint32_t kRefreshIntervalMs = 1000;

  // Returns false only when a PMU is expected and did not respond.
  bool begin();

  // Call from loop(). Re-reads the battery at most once per kRefreshIntervalMs.
  void tick(uint32_t nowMs);

  // The most recent reading. Cheap: no I2C, just the cache.
  BatteryState battery() const { return battery_; }

  bool present() const { return present_; }

 private:
  bool present_ = false;
  BatteryState battery_;
  uint32_t lastReadMs_ = 0;
  bool readOnce_ = false;
};
```

- [ ] **Step 2: Configure charging properly in `src/board/Pmu.cpp`**

Replace this block:

```cpp
  // Charging is a hardware function of the AXP2101 and works without any of
  // this, but the current and termination voltage would then come from
  // whatever the chip powers up with. 4.2 V is correct for a standard 18650;
  // the part can also be told 4.35 or 4.4 V, which suits some high-voltage
  // cells and would overcharge a normal one. Set it rather than inherit it.
  g_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  g_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
  g_pmu.enableCellbatteryCharge();

  present_ = true;
  Log::info("pmu", "AXP2101 up, charging 500mA to 4.2V");
  return true;
```

with:

```cpp
  // Nothing below can be read back without these. isBatteryConnect() gates
  // both getBattVoltage() and getBatteryPercent(), and it reads false until
  // battery detection is on -- which is why the firmware could not see the
  // cell at all before now.
  g_pmu.enableBattDetection();
  g_pmu.enableBattVoltageMeasure();
  g_pmu.enableVbusVoltageMeasure();

  // How much the board may draw from USB in total: charging plus running.
  // Never set before, so it was whatever the chip powers up with, and the
  // 150-250 mA this board draws came out of the same unknown budget as the
  // charge current.
  g_pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);

  // 4.2 V is correct for a standard 18650; the part also accepts 4.35 and
  // 4.4 V, which suit high-voltage cells and would overcharge a normal one.
  g_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  // 1000 mA is the part's maximum and 0.5C even for a small 2000 mAh cell,
  // 0.33C for a 3000 mAh one -- safe across any 18650 likely to be fitted.
  // The previous 500 mA was about 0.17C and took most of a day from empty.
  g_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_1000MA);
  // Precharge revives a deeply discharged cell gently; termination decides
  // when the charger stops. Both were inherited defaults, and an inherited
  // value is not a chosen one.
  g_pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_100MA);
  g_pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_100MA);
  g_pmu.enableCellbatteryCharge();

  // Hands the onboard LED to the charger, so it reports charge state as a
  // hardware function -- true even if this firmware hangs.
  g_pmu.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);

  present_ = true;
  Log::info("pmu", "AXP2101 up, charging 1000mA to 4.2V, 1500mA from USB");
  return true;
```

- [ ] **Step 3: Add `tick()` to the T-Beam branch of `src/board/Pmu.cpp`**

Add immediately after `Pmu::begin()`'s closing brace, still inside `#if defined(BOARD_TBEAM)`:

```cpp
void Pmu::tick(uint32_t nowMs) {
  if (!present_) {
    return;
  }
  // readOnce_ makes the first call read immediately rather than waiting a
  // second: at boot lastReadMs_ and nowMs are both near zero.
  if (readOnce_ && nowMs - lastReadMs_ < kRefreshIntervalMs) {
    return;
  }
  lastReadMs_ = nowMs;
  readOnce_ = true;

  BatteryState state;
  state.present = g_pmu.isBatteryConnect();
  state.usbPresent = g_pmu.isVbusIn();
  if (state.present) {
    const int percent = g_pmu.getBatteryPercent();
    state.percent = percent < 0 ? 0 : static_cast<uint8_t>(percent);
    state.milliVolts = g_pmu.getBattVoltage();
    // getChargerStatus() distinguishes done from merely not charging, which
    // isCharging() cannot. Trickle, pre, constant-current and constant-
    // voltage are all "charging" as far as anyone looking at the screen is
    // concerned.
    const uint8_t status = g_pmu.getChargerStatus();
    state.full = status == XPOWERS_AXP2101_CHG_DONE_STATE;
    state.charging = status == XPOWERS_AXP2101_CHG_TRI_STATE ||
                     status == XPOWERS_AXP2101_CHG_PRE_STATE ||
                     status == XPOWERS_AXP2101_CHG_CC_STATE ||
                     status == XPOWERS_AXP2101_CHG_CV_STATE;
  }
  battery_ = state;
}
```

- [ ] **Step 4: Add the no-op `tick()` to the `#else` branch**

In the `#else` branch of `src/board/Pmu.cpp`, after the existing stub `begin()`:

```cpp
void Pmu::tick(uint32_t nowMs) {
  (void)nowMs;  // no PMU on this board; battery_ stays default-constructed
}
```

- [ ] **Step 5: Build both environments**

Run: `export PATH="$HOME/.platformio/penv/bin:$PATH" && pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both.

If a method name does not exist, do **not** invent one. All of these were verified present in the installed library: `enableBattDetection` (`XPowersAXP2101.tpp:2300`), `enableBattVoltageMeasure` (`:2290`), `enableVbusVoltageMeasure` (`:2247`), `setVbusCurrentLimit` (`:500`), `setPrechargeCurr` (`:2390`), `setChargerTerminationCurr` (`:2434`), `setChargingLedMode` (`:2343`), `isBatteryConnect` (`:262`), `isVbusIn` (`:307`), `getBattVoltage` (`:2310`), `getBatteryPercent` (`:2318`), `getChargerStatus` (`:312`).

- [ ] **Step 6: Confirm the host tests are unaffected**

Run: `pio test -e native`
Expected: `116 test cases: 116 succeeded`. `Pmu.cpp` is not in `[env:native]`'s `build_src_filter` and must not be added — it includes Arduino and XPowersLib.

- [ ] **Step 7: Commit**

```bash
git add src/board/Pmu.h src/board/Pmu.cpp
git commit -m "Charge the cell properly and let the firmware read it

The firmware could not see the battery at all: begin() never enabled
battery detection, and isBatteryConnect() gates both getBattVoltage()
and getBatteryPercent(), so both returned nothing.

Charging was also sharing an unknown budget. The USB input limit was
never set, so the 150-250 mA the board draws came out of the same
unmeasured allowance as the charge current. 1500 mA in, 1000 mA to the
cell -- 0.5C for a 2000 mAh 18650 and 0.33C for a 3000 mAh one, against
the 0.17C it was doing before.

Precharge and termination are set rather than inherited, and the charge
LED is handed to the charger so it tells the truth even if this
firmware hangs.

battery() returns a cache that tick() refreshes once a second. loop()
runs at roughly 1 kHz and these are I2C reads of a number that moves
over minutes."
```

---

### Task 2: Carry the battery into `DeviceStatus` and `/api/status`

**Files:**
- Modify: `src/core/DeviceStatus.h`
- Modify: `src/config/internal/ConfigApi.cpp:36-45`
- Test: `test/native/test_config_api/test_config_api.cpp`

**Interfaces:**
- Consumes: `BatteryState` semantics from Task 1 (the fields carry the same meanings).
- Produces: `DeviceStatus::batteryPresent`, `batteryUsbPresent`, `batteryCharging`, `batteryFull` (all `bool`), `batteryPercent` (`uint8_t`), `batteryMilliVolts` (`uint16_t`). JSON keys `batteryPresent`, `batteryUsbPresent`, `batteryCharging`, `batteryFull`, `batteryPercent`, `batteryMilliVolts`.

- [ ] **Step 1: Write the failing tests**

`test_config_api.cpp` asserts the **complete** JSON string, not individual fields,
so adding six keys breaks the existing `test_status_json_shape`. Update it rather
than working around it — that whole-document assertion is what catches a renamed or
reordered field, and it is worth keeping.

Note the namespace: it is `ConfigApi::statusToJson`, and the buffer is
`ConfigApi::kJsonBufferSize` (512, against a 235-byte document — no overflow).

Replace the expected string in the existing `test_status_json_shape` with:

```cpp
  TEST_ASSERT_EQUAL_STRING(
      "{\"ssid\":\"teletrack\",\"ip\":\"192.168.4.1\","
      "\"clients\":1,\"uptimeMs\":134221,\"freeHeap\":186432,"
      "\"apUp\":true,\"batteryPresent\":false,"
      "\"batteryUsbPresent\":false,\"batteryCharging\":false,"
      "\"batteryFull\":false,\"batteryPercent\":0,"
      "\"batteryMilliVolts\":0}",
      buf);
```

Then add two tests before `int main`:

```cpp
static void test_status_json_reports_a_charging_battery() {
  DeviceStatus status;
  status.batteryPresent = true;
  status.batteryUsbPresent = true;
  status.batteryCharging = true;
  status.batteryPercent = 87;
  status.batteryMilliVolts = 4052;

  char buf[ConfigApi::kJsonBufferSize];
  ConfigApi::statusToJson(status, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"ssid\":\"\",\"ip\":\"\",\"clients\":0,\"uptimeMs\":0,"
      "\"freeHeap\":0,\"apUp\":false,\"batteryPresent\":true,"
      "\"batteryUsbPresent\":true,\"batteryCharging\":true,"
      "\"batteryFull\":false,\"batteryPercent\":87,"
      "\"batteryMilliVolts\":4052}",
      buf);
}

static void test_status_json_tells_no_cell_apart_from_a_flat_one() {
  // A board on USB with no cell fitted. Both cases report 0%, so
  // batteryPresent is the only thing that distinguishes them.
  DeviceStatus status;
  status.batteryUsbPresent = true;

  char buf[ConfigApi::kJsonBufferSize];
  ConfigApi::statusToJson(status, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"ssid\":\"\",\"ip\":\"\",\"clients\":0,\"uptimeMs\":0,"
      "\"freeHeap\":0,\"apUp\":false,\"batteryPresent\":false,"
      "\"batteryUsbPresent\":true,\"batteryCharging\":false,"
      "\"batteryFull\":false,\"batteryPercent\":0,"
      "\"batteryMilliVolts\":0}",
      buf);
}
```

Register both in `main`, after the existing `RUN_TEST(test_status_json_shape);`:

```cpp
  RUN_TEST(test_status_json_reports_a_charging_battery);
  RUN_TEST(test_status_json_tells_no_cell_apart_from_a_flat_one);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `export PATH="$HOME/.platformio/penv/bin:$PATH" && pio test -e native -f native/test_config_api`
Expected: a compile error — `'struct DeviceStatus' has no member named 'batteryPresent'`.
The updated `test_status_json_shape` would also fail on its expected string, but the
compile error comes first.

- [ ] **Step 3: Add the fields to `src/core/DeviceStatus.h`**

Add at the end of the struct, after the existing `gpsFix` member:

```cpp
  // Battery, from the AXP2101. batteryPresent false with batteryUsbPresent
  // true is a board running on USB with no cell fitted -- which is not the
  // same as a flat one, and 0% cannot express the difference.
  bool batteryPresent = false;
  bool batteryUsbPresent = false;
  bool batteryCharging = false;
  bool batteryFull = false;
  uint8_t batteryPercent = 0;
  uint16_t batteryMilliVolts = 0;
```

- [ ] **Step 4: Serialise them in `src/config/internal/ConfigApi.cpp`**

Add to `statusToJson`, immediately before `return serializeJson(doc, out, outSize);`:

```cpp
  doc["batteryPresent"] = status.batteryPresent;
  doc["batteryUsbPresent"] = status.batteryUsbPresent;
  doc["batteryCharging"] = status.batteryCharging;
  doc["batteryFull"] = status.batteryFull;
  doc["batteryPercent"] = status.batteryPercent;
  doc["batteryMilliVolts"] = status.batteryMilliVolts;
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `pio test -e native -f native/test_config_api`
Expected: every test in the file passes, including the two new ones.

- [ ] **Step 6: Run the whole suite and both builds**

Run: `pio test -e native && pio run -e esp && pio run -e tbeam`
Expected: `118 test cases: 118 succeeded`, and `SUCCESS` for both environments.

- [ ] **Step 7: Commit**

```bash
git add src/core/DeviceStatus.h src/config/internal/ConfigApi.cpp test/native/test_config_api/test_config_api.cpp
git commit -m "Carry battery state into DeviceStatus and /api/status

The same struct the display reads, so the web UI gets it for nothing.

batteryPresent is separate from batteryPercent on purpose: a board on
USB with no cell fitted and a board with a flat one are both 0%, and a
consumer has to be able to tell them apart."
```

---

### Task 3: Rebuild the header and wire it up

**Files:**
- Modify: `src/ui/OledDisplay.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `DeviceStatus` battery fields (Task 2); `Pmu::tick(uint32_t)` and `Pmu::battery()` returning `BatteryState` (Task 1).
- Produces: nothing further.

- [ ] **Step 1: Replace the header composition in `src/ui/OledDisplay.cpp`**

Replace everything from `char left[kCols + 1];` down to and including the `u8g2_.setDrawColor(1);` that closes the header block, with:

```cpp
  // Battery on the left. The state character sits between the percentage and
  // the voltage so the two numbers stay adjacent and readable at a glance.
  char left[kCols + 1];
  if (!status.batteryPresent) {
    // No cell fitted. "0%- 0.00V" would be a lie, and a bench T-Beam running
    // on USB alone is how most of this gets tested.
    snprintf(left, sizeof(left), "USB");
  } else {
    const char state = status.batteryFull      ? '='
                       : status.batteryCharging ? '+'
                                                : '-';
    snprintf(left, sizeof(left), "%u%%%c %u.%02uV",
             static_cast<unsigned>(status.batteryPercent), state,
             static_cast<unsigned>(status.batteryMilliVolts / 1000u),
             static_cast<unsigned>((status.batteryMilliVolts % 1000u) / 10u));
  }

  // Which radio is running, and nothing about who is connected to it. The
  // client and drop counts are in /api/status, which is where they are read.
  char right[kCols + 1];
  if (status.holdMs > 0) {
    // Not a connection detail: the only feedback that a three-second hold is
    // registering at all. Without it the button feels broken.
    snprintf(right, sizeof(right), "HOLD %lus",
             static_cast<unsigned long>(
                 (HoldDetector::kHoldMs - status.holdMs) / 1000u + 1u));
  } else {
    snprintf(right, sizeof(right), "%s",
             status.mode == RadioMode::Wifi ? "AP" : "BLE");
  }

  // One inverse-video row. Widest case is "100%= 4.20V" (11) against
  // "HOLD 3s" (7), which is 20 of 21 columns with a separator.
  u8g2_.drawBox(0, 0, 128, kHeaderRows * kRowHeight);
  u8g2_.setDrawColor(0);
  u8g2_.drawStr(1, kRowHeight - 1, left);
  u8g2_.drawStr(128 - 1 - u8g2_.getStrWidth(right), kRowHeight - 1, right);
  u8g2_.setDrawColor(1);
```

- [ ] **Step 2: Update the repaint gate in the same file**

Replace `headerDiffers` in the anonymous namespace with:

```cpp
bool headerDiffers(const DeviceStatus& a, const DeviceStatus& b) {
  // Only what is actually drawn. Uptime, the client count and the drop count
  // left the header, so comparing them would repaint for nothing; the battery
  // fields arrived, and omitting one freezes it on a stale value.
  return a.mode != b.mode || (a.holdMs / 100u) != (b.holdMs / 100u) ||
         a.batteryPresent != b.batteryPresent ||
         a.batteryCharging != b.batteryCharging ||
         a.batteryFull != b.batteryFull ||
         a.batteryPercent != b.batteryPercent ||
         a.batteryMilliVolts != b.batteryMilliVolts ||
         a.gpsPresent != b.gpsPresent || a.gpsTimeValid != b.gpsTimeValid ||
         a.gpsFix.satellites != b.gpsFix.satellites ||
         a.gpsFix.fixType != b.gpsFix.fixType ||
         a.gpsFix.fixQuality != b.gpsFix.fixQuality ||
         a.gpsFix.latE7 != b.gpsFix.latE7 || a.gpsFix.lonE7 != b.gpsFix.lonE7 ||
         a.gpsFix.altitudeM != b.gpsFix.altitudeM ||
         a.gpsFix.speedKmh != b.gpsFix.speedKmh ||
         a.gpsFix.hdop != b.gpsFix.hdop ||
         a.gpsFix.seconds != b.gpsFix.seconds;
}
```

`Format::uptime` is no longer called from this file. Remove `#include "core/Format.h"` from `OledDisplay.cpp` — but leave `src/core/Format.{h,cpp}` alone, because `TftDisplay.cpp` still uses it.

- [ ] **Step 3: Drive the PMU and fill the fields in `src/main.cpp`**

In `loop()`, add immediately after `const uint32_t now = millis();`:

```cpp
  // Re-reads the battery at most once a second; cheap on every other pass.
  app.pmu.tick(now);
```

In `buildStatus`, add before `return s;`:

```cpp
  const BatteryState battery = app.pmu.battery();
  s.batteryPresent = battery.present;
  s.batteryUsbPresent = battery.usbPresent;
  s.batteryCharging = battery.charging;
  s.batteryFull = battery.full;
  s.batteryPercent = battery.percent;
  s.batteryMilliVolts = battery.milliVolts;
```

- [ ] **Step 4: Build both environments**

Run: `export PATH="$HOME/.platformio/penv/bin:$PATH" && pio run -e esp && pio run -e tbeam`
Expected: `SUCCESS` for both. The DevKitC compiles `Pmu`'s stub, so its battery fields stay false and its TFT — which does not read them — is unchanged.

- [ ] **Step 5: Run the host tests**

Run: `pio test -e native`
Expected: `118 test cases: 118 succeeded`.

- [ ] **Step 6: Check the column arithmetic by hand**

Not a command — read the code and confirm. For each case, count the characters `snprintf` can produce at its widest:

| Left | Columns |
| --- | --- |
| `USB` | 3 |
| `0%- 3.20V` | 9 |
| `87%+ 4.05V` | 10 |
| `100%= 4.20V` | 11 |

| Right | Columns |
| --- | --- |
| `AP` | 2 |
| `BLE` | 3 |
| `HOLD 3s` | 7 |

Worst pairing is 11 + 7 = 18 plus separation, inside 21. The two strings are drawn to opposite edges, so they cannot overlap unless their combined width exceeds 21.

- [ ] **Step 7: Commit**

```bash
git add src/ui/OledDisplay.cpp src/main.cpp
git commit -m "Show battery and radio in the header

Charge level, state and voltage on the left; which radio is running on
the right. Uptime leaves the screen -- serial keeps timestamps, which is
where it was readable anyway -- and so do the client and drop counts,
which are read with a browser open rather than at a glance.

USB rather than 0% when no cell is fitted: a bench board on USB and a
flat battery are not the same thing, and one of them is how this gets
tested.

The mode-button countdown still takes the right slot while the button is
held. That is not connection detail; it is the only sign a three-second
hold is registering."
```

---

## Hardware acceptance

Not a task — no code comes out of it — but the branch is not done until this passes.

- [ ] `pio run -e esp -t upload` on the DevKitC: TFT, log, BLE, mode switch and portal all behave exactly as before.
- [ ] `pio run -e tbeam -t upload` on the T-Beam. Serial shows `pmu: AXP2101 up, charging 1000mA to 4.2V, 1500mA from USB`.
- [ ] On USB with the cell fitted, the header shows a percentage and `+`, and the onboard charge LED agrees.
- [ ] Unplug USB: the character becomes `-` and the percentage falls over time.
- [ ] Remove the cell and run on USB: the header reads `USB`.
- [ ] Leave it charging to completion: the character becomes `=` and the LED agrees.
- [ ] `GET /api/status` carries all six battery fields.
- [ ] The status screen's last row is still empty.
