# T-Beam Supreme Second Target — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the same firmware for a LILYGO T-Beam Supreme as well as the ESP32-S3-DevKitC-1, from one source tree, without regressing the board that already works.

**Architecture:** `Display` becomes an abstract interface with two implementations — `TftDisplay` (today's ILI9341 code, renamed) and `OledDisplay` (SH1106 over I²C). A `BoardConfig.h` holds every differing pin behind a build flag, and a `Pmu` module brings up the T-Beam's AXP2101 power rails before the display initialises. Everything below `ui/` is already board-agnostic and is not touched.

**Tech Stack:** PlatformIO, Arduino-ESP32 2.0.17, U8g2 (SH1106), XPowersLib (AXP2101), plus the existing NimBLE, TFT_eSPI, ESPAsyncWebServer and ArduinoJson.

**Spec:** `docs/superpowers/specs/2026-09-07-tbeam-supreme-target-design.md`

## Global Constraints

- PlatformIO is **not on `PATH`**: `export PATH="$HOME/.platformio/penv/bin:$PATH"` first, every shell.
- **Do not modify `[env:esp]`.** Every line in it was expensive to establish and carries a comment saying why: no `board_build.partitions` change, keep `board_upload.flash_size = 16MB`, `board_build.arduino.memory_type = qio_opi`, `build_unflags = -std=gnu++11`, `-DUSE_FSPI_PORT`. The new environment is **additive**.
- **Regressing the DevKitC is the failure mode this plan is shaped to avoid.** `TftDisplay` is today's code with a new name and a base class — not a rewrite, not a tidy-up, not an opportunity to improve anything.
- C++ standard `gnu++17`. Fixed-size buffers for our own data. No Arduino `String`, no `std::string`, no `std::vector` in code we write.
- One module per file pair, header/implementation split, `#pragma once` in every header.
- All string building through `snprintf` with an explicit size.
- `[env:native]`'s `build_src_filter` is the purity boundary. **Never** add `TftDisplay.cpp`, `OledDisplay.cpp` or `Pmu.cpp` — all three include Arduino or a driver library.
- **Validate untrusted input; trust our own callers.** No defensive guards against callers that do not exist. This is a proof of concept.
- Only the display's `tick()` touches its bus, and only from `loop()`.
- `pio test -e native` must report **95** throughout. This branch adds no host tests and must break none.
- Commit after every task. Never `git add -A`; `.superpowers/` self-ignores.

## Board facts

From vendor documentation, **not yet confirmed on hardware**. Each is one line to change.

| | DevKitC-1 | T-Beam Supreme |
| --- | --- | --- |
| `memory_type` | `qio_opi` | **`qio_qspi`** — quad, not octal |
| Flash | 16 MB | 8 MB, stock partition table |
| Display | ILI9341 320×240 SPI | SH1106 128×64 I²C, SDA 17 / SCL 18 |
| Mode button | GPIO39 | **GPIO0** — onboard User/Program button |
| PMU | none | AXP2101, switches the display rail |

## File Structure

| Path | Responsibility |
| --- | --- |
| `src/ui/Display.h` | Abstract interface: `begin()`, `tick()` |
| `src/ui/TftDisplay.h/.cpp` | ILI9341 — today's `Display`, renamed |
| `src/ui/OledDisplay.h/.cpp` | SH1106 128×64 |
| `src/board/BoardConfig.h` | Every board-differing constant, behind the build flag |
| `src/board/Pmu.h/.cpp` | AXP2101 on the T-Beam; compiles to nothing elsewhere |
| `src/main.cpp` | Holds a `Display&`; constructs the board's concrete type |
| `platformio.ini` | Gains `[env:tbeam]` |

Task 1 is a pure refactor that must leave the DevKitC binary behaving identically.
Tasks 2–4 add the new board's pieces. Task 5 wires it. Task 6 is hardware acceptance,
including the regression check on the DevKitC.

---

### Task 1: Extract the `Display` interface

A rename and a base class. **No behaviour changes, no improvements, no tidying.** The
DevKitC must behave exactly as it does today, and the smaller this diff is the easier
that is to believe.

**Files:**
- Create: `src/ui/Display.h` (new abstract interface)
- Create: `src/ui/TftDisplay.h`, `src/ui/TftDisplay.cpp` (from today's `Display.*`)
- Delete: `src/ui/Display.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `DeviceStatus`, `LogRing`, `Log`.
- Produces:
  - `class Display` — `virtual ~Display()`, `virtual bool begin() = 0`, `virtual void tick(uint32_t nowMs, const DeviceStatus&) = 0`
  - `class TftDisplay : public Display` — the existing implementation, unchanged

- [ ] **Step 1: Write `src/ui/Display.h`**

Replace the current contents entirely:

```cpp
#pragma once

#include <stdint.h>

#include "core/DeviceStatus.h"

// What main.cpp holds. Two implementations exist: TftDisplay for the
// ILI9341 on the DevKitC, OledDisplay for the SH1106 on the T-Beam. They
// share no code -- colour SPI against monochrome I2C, 53x20 against 21x8 --
// so this is an interface rather than a base class with behaviour.
class Display {
 public:
  virtual ~Display() = default;

  virtual bool begin() = 0;

  // Call from loop() only. The sole place that drives the display bus.
  virtual void tick(uint32_t nowMs, const DeviceStatus& status) = 0;
};
```

- [ ] **Step 2: Create `src/ui/TftDisplay.h` from the old `Display.h`**

Take the old header verbatim, then make exactly these changes:

- `#include "ui/Display.h"` added
- `class Display {` becomes `class TftDisplay : public Display {`
- `bool begin();` becomes `bool begin() override;`
- `void tick(uint32_t nowMs, const DeviceStatus& status);` becomes `... override;`

Everything else — the constants, the private members, the comments — stays byte for
byte. The old header's `#include <TFT_eSPI.h>` and `#include "core/LogRing.h"` remain.

- [ ] **Step 3: Create `src/ui/TftDisplay.cpp` from `src/ui/Display.cpp`**

```bash
git mv src/ui/Display.cpp src/ui/TftDisplay.cpp
```

Then in the moved file, change only:

- `#include "ui/Display.h"` becomes `#include "ui/TftDisplay.h"`
- every `Display::` becomes `TftDisplay::`

Change nothing else. In particular **do not** touch `drawHeader` — it deliberately
contains no `fillRect`, because clearing and then drawing made the header blink once a
second. That was a reported bug.

- [ ] **Step 4: Point `main.cpp` at the concrete type**

Change the include from `"ui/Display.h"` to `"ui/TftDisplay.h"`, and the member
declaration in `struct App` from `Display display;` to `TftDisplay display;`.

Nothing else in `main.cpp` changes; `app.display.begin()` and `app.display.tick(...)`
still compile.

- [ ] **Step 5: Verify nothing moved**

```bash
pio run -e esp
pio test -e native
```

Expected: `SUCCESS`, and **95** host tests. Record the Flash and RAM figures — they
should be within a few bytes of the previous build, because nothing but names changed.

- [ ] **Step 6: Commit**

```bash
git add src/ui/Display.h src/ui/TftDisplay.h src/ui/TftDisplay.cpp src/main.cpp
git commit -m "Extract a Display interface, rename the TFT implementation"
```

---

### Task 2: `BoardConfig.h` and the `tbeam` environment

Every board-differing constant in one header, and a second PlatformIO environment that
selects it. No new behaviour yet — this task ends with both environments building the
same firmware.

**Files:**
- Create: `src/board/BoardConfig.h`
- Modify: `platformio.ini`
- Modify: `src/radio/ModeButton.h` (take the pin from `BoardConfig`)

**Interfaces:**
- Produces: `BoardConfig::kModeButtonPin`, `kHasPmu`, `kI2cSda`, `kI2cScl`, `kBoardName`

- [ ] **Step 1: Write `src/board/BoardConfig.h`**

```cpp
#pragma once

#include <stdint.h>

// Every constant that differs between the two supported boards, in one place.
// Selected by a build flag: -DBOARD_DEVKITC or -DBOARD_TBEAM.
//
// Values here come from vendor documentation and are confirmed on hardware at
// first flash. Each is one line to change.
namespace BoardConfig {

#if defined(BOARD_TBEAM)

inline constexpr const char* kBoardName = "T-Beam Supreme";
// Onboard User/Program button, the left of the three. Active low with an
// existing pull-up. Also a strapping pin: held low AT RESET the board enters
// download mode, which is how it gets flashed. Pressed while running it is
// just a button.
inline constexpr uint8_t kModeButtonPin = 0;
// Display, PMU and sensors share I2C bus 0.
inline constexpr uint8_t kI2cSda = 17;
inline constexpr uint8_t kI2cScl = 18;

#elif defined(BOARD_DEVKITC)

inline constexpr const char* kBoardName = "ESP32-S3-DevKitC-1";
// Hand-wired button to ground. Not 19 or 20 -- those are USB D-/D+ on the S3.
inline constexpr uint8_t kModeButtonPin = 39;
// No kI2cSda/kI2cScl here on purpose: this board has no I2C peripherals, so
// code referencing them should fail to compile rather than get a plausible
// default. Pmu.cpp branches on the board macro, so it never reaches them.

#else
#error "No board selected: define BOARD_DEVKITC or BOARD_TBEAM in build_flags"
#endif

}  // namespace BoardConfig
```

The `#error` is deliberate. A missing board flag should fail loudly at compile time, not
silently pick a default and produce a binary for the wrong hardware.

- [ ] **Step 2: Take the button pin from `BoardConfig`**

In `src/radio/ModeButton.h`, replace the hardcoded constant:

```cpp
#include "board/BoardConfig.h"
```

```cpp
  static constexpr uint8_t kPin = BoardConfig::kModeButtonPin;
```

Keep the existing comment about GPIO19/20 being USB and 26–37 being flash and PSRAM —
it is still the reason the DevKitC pin is what it is.

- [ ] **Step 3: Add `-DBOARD_DEVKITC` to `[env:esp]`**

Append it to that environment's existing `build_flags`. **This is the only change to
`[env:esp]` in the entire plan.** Do not reformat or reorder anything else in it.

- [ ] **Step 4: Add the `[env:tbeam]` environment**

Append to `platformio.ini`, after `[env:esp]` and before `[env:native]`:

```ini
[env:tbeam]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
; The T-Beam Supreme is an ESP32-S3 with 8MB flash and 8MB QUAD PSRAM.
; qio_opi here would boot-loop the board exactly as a mismatched partition
; table does -- see docs/hardware-notes.md.
board_build.flash_size = 8MB
board_build.arduino.memory_type = qio_qspi
; No board_build.partitions: the stock 8MB table is correct for this board.
build_unflags = -std=gnu++11
build_flags =
    ${common.build_flags}
    -DBOARD_TBEAM
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
lib_deps =
    bblanchon/ArduinoJson@^7.4.3
    esp32async/AsyncTCP@^3.5.0
    esp32async/ESPAsyncWebServer@^3.12.0
    h2zero/NimBLE-Arduino@^1.4.3
    olikraus/U8g2@^2.35.30
    lewisxhe/XPowersLib@^0.2.7
monitor_speed = 115200
monitor_filters = esp32_exception_decoder
build_type = debug
```

Note what is **absent**: no TFT_eSPI, and none of the ILI9341 pin defines. This board has
no SPI TFT, and pulling that library in would compile a driver for hardware that is not
there.

- [ ] **Step 5: Exclude the wrong display from each build**

Both display implementations are in `src/`, so both compile in both environments unless
filtered. Add to `[env:esp]`:

```ini
build_src_filter = +<*> -<ui/OledDisplay.cpp>
```

and to `[env:tbeam]`:

```ini
build_src_filter = +<*> -<ui/TftDisplay.cpp>
```

Without this the DevKitC build would need U8g2 and the T-Beam build would need TFT_eSPI,
each for a display it does not have.

- [ ] **Step 6: Verify both environments build**

```bash
pio run -e esp
pio run -e tbeam
pio test -e native
```

Expected: both `SUCCESS`, and 95 host tests. The `tbeam` build will fail to link if
anything still references `TftDisplay` unconditionally — that is Task 5's job, so at this
point it is expected to fail only if `main.cpp` was changed early. If `tbeam` fails here
for that reason, note it and continue; Task 5 resolves it.

- [ ] **Step 7: Commit**

```bash
git add src/board/BoardConfig.h src/radio/ModeButton.h platformio.ini
git commit -m "Add board configuration and the T-Beam build environment"
```

---

### Task 3: `Pmu` — bring up the AXP2101 rails

On the T-Beam the display and GPS sit on rails the AXP2101 switches. This must run
before the display initialises, or the screen is unpowered and the symptom looks like a
broken display driver.

**Files:**
- Create: `src/board/Pmu.h`, `src/board/Pmu.cpp`

**Interfaces:**
- Produces: `class Pmu` — `bool begin()`, `bool present() const`

**No test.** It is I²C register writes against a chip that is either there or not; a host
test would assert nothing. Verified on hardware in Task 6.

- [ ] **Step 1: Write `src/board/Pmu.h`**

```cpp
#pragma once

#include "board/BoardConfig.h"

// The T-Beam's AXP2101 switches the rails the display and GPS sit on, so it
// has to be brought up before either. On a board without a PMU every method
// here does nothing and returns success -- the caller does not branch.
class Pmu {
 public:
  // Returns false only when a PMU is expected and did not respond.
  bool begin();

  bool present() const { return present_; }

 private:
  bool present_ = false;
};
```

- [ ] **Step 2: Write `src/board/Pmu.cpp`**

```cpp
#include "board/Pmu.h"

#include "core/Log.h"

#if defined(BOARD_TBEAM)

#include <Wire.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

namespace {
XPowersPMU g_pmu;
}  // namespace

bool Pmu::begin() {
  Wire.begin(BoardConfig::kI2cSda, BoardConfig::kI2cScl);

  if (!g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, BoardConfig::kI2cSda,
                   BoardConfig::kI2cScl)) {
    Log::error("pmu", "AXP2101 not responding");
    present_ = false;
    return false;
  }

  // Which rail feeds what varies across T-Beam revisions. If the display stays
  // dark on a board that is otherwise booting, this block is the first thing to
  // check -- not the display driver.
  g_pmu.setALDO2Voltage(3300);  // display
  g_pmu.enableALDO2();
  g_pmu.setALDO3Voltage(3300);  // GPS -- unused in this branch, powered anyway
  g_pmu.enableALDO3();

  present_ = true;
  Log::info("pmu", "AXP2101 up, display rail on");
  return true;
}

#else

bool Pmu::begin() {
  present_ = false;
  return true;  // no PMU on this board; nothing to do and nothing failed
}

#endif
```

- [ ] **Step 3: Verify both environments still build**

```bash
pio run -e esp
pio run -e tbeam
pio test -e native
```

Expected: both `SUCCESS`, 95 host tests. Nothing calls `Pmu` yet.

If XPowersLib fails to compile, **report it with the exact error and stop.** Do not
substitute another library or bump the platform — the platform pin is what the working
DevKitC configuration was verified against.

- [ ] **Step 4: Commit**

```bash
git add src/board/Pmu.h src/board/Pmu.cpp
git commit -m "Add AXP2101 power management for the T-Beam"
```

---

### Task 4: `OledDisplay` — SH1106 128×64

**Files:**
- Create: `src/ui/OledDisplay.h`, `src/ui/OledDisplay.cpp`

**Interfaces:**
- Consumes: `Display`, `DeviceStatus`, `LogRing`, `Log`, `Format`, `BoardConfig`.
- Produces: `class OledDisplay : public Display` — `begin() override`, `tick(...) override`, constants `kCols == 21`, `kLogRows == 6`, `kHeaderRows == 2`

**Layout.** 128×64 at the 6×8 font is 21 columns × 8 rows. Two header rows and six log
rows fill it exactly, so there is no separator row — the header is drawn in inverse video
instead.

**`LogRing` is not resized.** It stays 53×20 and remains the superset: this display reads
the **last 6** rows and truncates each to 21 characters. The TFT continues to draw all 20
at full width.

**The timestamp and level are dropped here.** `HH:MM:SS [INF] ` costs 15 of 21 columns
before the message starts. Serial keeps the full line.

**Use `U8G2_SH1106_128X64_NONAME_F_HW_I2C`.** SH1106, not SSD1306: the controller has 132
columns of RAM behind a 128-pixel panel, and the wrong driver renders everything shifted
two pixels sideways — which looks like a wiring fault, not a software one.

- [ ] **Step 1: Write `src/ui/OledDisplay.h`**

```cpp
#pragma once

#include <U8g2lib.h>

#include "core/DeviceStatus.h"
#include "core/LogRing.h"
#include "ui/Display.h"

// SH1106 128x64 over I2C. At the 6x8 font that is 21 columns by 8 rows: two
// header rows drawn in inverse video, then the six most recent log lines
// truncated to fit.
class OledDisplay : public Display {
 public:
  static constexpr uint32_t kMinRedrawIntervalMs = 100;  // 10 Hz ceiling
  static constexpr size_t kCols = 21;
  static constexpr size_t kHeaderRows = 2;
  static constexpr size_t kLogRows = 6;
  static constexpr int16_t kRowHeight = 8;

  bool begin() override;
  void tick(uint32_t nowMs, const DeviceStatus& status) override;

 private:
  void draw(const DeviceStatus& status);

  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2_{U8G2_R0, U8X8_PIN_NONE};
  LogRing console_;  // this frame's copy, refreshed from Log::snapshot()
  bool ready_ = false;
  uint32_t lastDrawMs_ = 0;
  uint32_t drawnRevision_ = 0;
  DeviceStatus drawnStatus_;
  bool drawnOnce_ = false;
};
```

- [ ] **Step 2: Write `src/ui/OledDisplay.cpp`**

```cpp
#include "ui/OledDisplay.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "board/BoardConfig.h"
#include "core/Format.h"
#include "core/Log.h"
#include "radio/HoldDetector.h"

namespace {

// Same fields the TFT header compares, so the panel repaints when anything
// visible changes and not otherwise.
bool headerDiffers(const DeviceStatus& a, const DeviceStatus& b) {
  return strcmp(a.ssid, b.ssid) != 0 || a.clients != b.clients ||
         a.apUp != b.apUp || a.mode != b.mode ||
         a.bleConnected != b.bleConnected || a.dropped != b.dropped ||
         (a.holdMs / 100u) != (b.holdMs / 100u) ||
         (a.uptimeMs / 1000u) != (b.uptimeMs / 1000u);
}

}  // namespace

bool OledDisplay::begin() {
  u8g2_.setBusClock(400000);
  u8g2_.setI2CAddress(0x3C << 1);  // u8g2 wants the address pre-shifted
  if (!u8g2_.begin()) {
    return false;
  }
  u8g2_.setFont(u8g2_font_5x8_tr);  // 6x8 cell, 21 columns across 128 px
  u8g2_.clearBuffer();
  u8g2_.sendBuffer();
  ready_ = true;
  return true;
}

void OledDisplay::tick(uint32_t nowMs, const DeviceStatus& status) {
  if (!ready_) {
    return;
  }
  if (nowMs - lastDrawMs_ < kMinRedrawIntervalMs) {
    return;
  }
  lastDrawMs_ = nowMs;

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

  draw(status);
  drawnStatus_ = status;
  drawnOnce_ = true;
}

void OledDisplay::draw(const DeviceStatus& status) {
  // A monochrome panel has one framebuffer and no partial-update trick worth
  // having at this size, so the whole thing is rebuilt and sent each frame.
  // 1 KB over I2C at 400 kHz is about 25 ms, inside the 100 ms budget.
  u8g2_.clearBuffer();

  char left[kCols + 1];
  char right[kCols + 1];

  if (status.mode == RadioMode::Wifi) {
    snprintf(left, sizeof(left), "%s", status.ssid);
    snprintf(right, sizeof(right), "%s", status.apUp ? "AP UP" : "AP FAIL");
  } else {
    snprintf(left, sizeof(left), "%s", status.ssid);  // deviceName drives both
    snprintf(right, sizeof(right), "%s",
             status.bleConnected ? "BLE CONN" : "BLE ADV");
  }

  // Header rows in inverse video: it separates them from the log without
  // spending one of the eight rows on a rule.
  u8g2_.drawBox(0, 0, 128, kHeaderRows * kRowHeight);
  u8g2_.setDrawColor(0);
  u8g2_.drawStr(1, kRowHeight - 1, left);
  u8g2_.drawStr(128 - 1 - u8g2_.getStrWidth(right), kRowHeight - 1, right);

  char stamp[9];
  Format::uptime(status.uptimeMs, stamp, sizeof(stamp));
  char lower[kCols + 1];
  char lowerRight[kCols + 1];
  snprintf(lower, sizeof(lower), "up %s", stamp);
  if (status.holdMs > 0) {
    snprintf(lower, sizeof(lower), "HOLD %lus",
             (unsigned long)((HoldDetector::kHoldMs - status.holdMs) / 1000u + 1u));
  }
  if (status.mode == RadioMode::Wifi) {
    snprintf(lowerRight, sizeof(lowerRight), "%u cli", (unsigned)status.clients);
  } else if (status.bleConnected) {
    snprintf(lowerRight, sizeof(lowerRight), "drop %u", (unsigned)status.dropped);
  } else {
    lowerRight[0] = '\0';
  }
  u8g2_.drawStr(1, 2 * kRowHeight - 1, lower);
  if (lowerRight[0] != '\0') {
    u8g2_.drawStr(128 - 1 - u8g2_.getStrWidth(lowerRight), 2 * kRowHeight - 1,
                  lowerRight);
  }
  u8g2_.setDrawColor(1);

  // The newest kLogRows lines from a ring that holds more. LogRing::row(0) is
  // the oldest, so start kLogRows back from the end.
  for (size_t i = 0; i < kLogRows; ++i) {
    const size_t src = LogRing::kRows - kLogRows + i;
    char line[kCols + 1];
    snprintf(line, sizeof(line), "%s", console_.row(src));
    if (strlen(console_.row(src)) > kCols) {
      line[kCols - 1] = '~';  // same truncation mark LogRing itself uses
    }
    const int16_t y =
        static_cast<int16_t>((kHeaderRows + i + 1) * kRowHeight - 1);
    u8g2_.drawStr(1, y, line);
  }

  u8g2_.sendBuffer();
}
```

- [ ] **Step 3: Verify both environments build**

```bash
pio run -e esp
pio run -e tbeam
pio test -e native
```

Expected: both `SUCCESS`, 95 host tests. `OledDisplay.cpp` is excluded from `[env:esp]`
by the `build_src_filter` from Task 2, and must **not** appear in `[env:native]`'s.

If U8g2's font or constructor names differ from those above, adapt them and **say what
you changed in your report**. The layout — 21 columns, two inverse header rows, six log
rows — is the specification and must not change.

- [ ] **Step 4: Commit**

```bash
git add src/ui/OledDisplay.h src/ui/OledDisplay.cpp
git commit -m "Add the SH1106 OLED display for the T-Beam"
```

---

### Task 5: Wire the board into `main.cpp`

**Files:**
- Modify: `src/main.cpp`

**The ordering that matters:** `Pmu::begin()` runs **before** `Display::begin()`. On the
T-Beam the display sits on a rail the PMU switches, so initialising the display first
talks to a chip that has no power. The symptom is a dark screen and a driver that looks
broken.

- [ ] **Step 1: Select the display by board**

Replace the display include and the `App` member. The concrete type is chosen at compile
time; everything after that goes through the interface.

```cpp
#include "board/BoardConfig.h"
#include "board/Pmu.h"
#include "ui/Display.h"

#if defined(BOARD_TBEAM)
#include "ui/OledDisplay.h"
using BoardDisplay = OledDisplay;
#else
#include "ui/TftDisplay.h"
using BoardDisplay = TftDisplay;
#endif
```

In `struct App`, add the PMU and change the display's type:

```cpp
struct App {
  Settings settings = Settings::defaults();
  ConfigPortal portal{settings};
  TelemetryRing ring;
  BleLink ble;
  ModeButton button;
  ModeController modes;
  Pmu pmu;
  BoardDisplay display;
};
```

- [ ] **Step 2: Bring the PMU up first in `setup()`**

```cpp
void setup() {
  Log::begin(115200);
  Log::info("boot", "teletrack on %s", BoardConfig::kBoardName);

  // Before the display: on the T-Beam the panel sits on a rail this switches.
  if (!app.pmu.begin()) {
    Log::error("pmu", "power management failed to start");
  }

  if (!app.display.begin()) {
    Log::error("display", "init failed, serial only");
  }

  app.button.begin();
  startCurrentMode();
}
```

The board name in the boot line is worth having: it is the first thing on serial and it
tells you immediately which binary is running.

- [ ] **Step 3: Verify both environments build**

```bash
pio run -e esp
pio run -e tbeam
pio test -e native
```

Expected: both `SUCCESS`, 95 host tests. Record RAM and Flash for both — this is the
first build where each board's real dependency set is linked.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Select the display by board and bring the PMU up first"
```

---

### Task 6: Hardware acceptance

Two boards to check, and the DevKitC one matters most: a second target is not worth
breaking the board that already works.

**Files:**
- Modify: `README.md`
- Modify: `docs/hardware-notes.md`

- [ ] **Step 1: Flash the DevKitC and confirm nothing regressed**

```bash
pio run -e esp -t upload -t monitor
```

This is the regression check. Confirm, exactly as before:

1. Boot line names the board, TFT header and scrolling log appear.
2. `BLE ADV` on screen; RaceChrono connects and plots a moving position.
3. Holding GPIO39 for 3 s switches to `AP UP`; `192.168.4.1` serves the page.
4. Holding again returns to BLE.

Any difference here is a regression from Task 1 and must be fixed before going further.

- [ ] **Step 2: Flash the T-Beam**

```bash
pio run -e tbeam -t upload -t monitor
```

Expected on serial:

```
00:00:00 [INF] boot: teletrack on T-Beam Supreme
00:00:00 [INF] pmu: AXP2101 up, display rail on
00:00:00 [INF] ble: advertising as teletrack
```

- [ ] **Step 3: Work through the board-specific failures in order**

These are the predicted failure modes, most likely first. Each is a one-line change.

| Symptom | Cause | Fix |
| --- | --- | --- |
| Boot loop, no output | PSRAM mode wrong | `memory_type` — try `qio_qspi`, then `opi_opi` |
| `pmu: AXP2101 not responding` | I²C pins wrong | `BoardConfig::kI2cSda` / `kI2cScl` |
| Boots, screen dark | wrong PMU rail | the `enableALDO*` calls in `Pmu.cpp` |
| Screen on, image shifted 2 px | SSD1306 driver on an SH1106 | the U8g2 constructor |
| Screen fine, button does nothing | wrong GPIO | `BoardConfig::kModeButtonPin` |

- [ ] **Step 4: Confirm the T-Beam behaves**

1. OLED shows a two-row inverse header and six log rows, undistorted.
2. BLE advertises; RaceChrono connects and plots a moving position.
3. Holding the **User/Program** button (left of the three) for 3 s switches to `AP UP`;
   the config page is reachable.
4. Holding again returns to BLE.

- [ ] **Step 5: Record what the hardware actually said**

Append a T-Beam section to `docs/hardware-notes.md` with the values that turned out to be
right — PSRAM mode, I²C pins, PMU rails, button GPIO — and anything that surprised you.
The existing notes are written symptom-first; match that.

Correct any value in `BoardConfig.h` or `platformio.ini` that differed from the vendor
documentation, and say so in the commit message. These were explicitly flagged as
unconfirmed guesses.

- [ ] **Step 6: Update `README.md`**

Add the second target to the build table:

```markdown
| `pio run -e esp` | Build for the ESP32-S3-DevKitC-1 |
| `pio run -e tbeam` | Build for the LILYGO T-Beam Supreme |
```

and a line under Status naming both supported boards.

- [ ] **Step 7: Commit**

```bash
git add README.md docs/hardware-notes.md src/board/BoardConfig.h platformio.ini
git commit -m "Confirm the T-Beam target on hardware"
```

---

## Deviations from the spec, and why

1. **`build_src_filter` per environment** (Task 2). The spec describes selecting the
   display by build flag but does not say how the unused implementation is kept out of
   the build. Without a filter, `[env:esp]` would need U8g2 and `[env:tbeam]` would need
   TFT_eSPI, each for hardware it does not have.
2. **`#error` when no board flag is defined** (Task 2). Not in the spec. A missing flag
   should fail at compile time rather than silently default and produce a binary for the
   wrong board.
3. **The board name is logged at boot** (Task 5). Not in the spec, one line: with two
   binaries in play, the first line on serial should say which one is running.
