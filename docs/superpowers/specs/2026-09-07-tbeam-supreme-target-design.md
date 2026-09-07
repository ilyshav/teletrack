# Second target — LILYGO T-Beam Supreme

**Date:** 2026-09-07
**Status:** Draft, awaiting review

## 1. Goal

Build the same firmware for a second board, the LILYGO T-Beam Supreme, without
regressing the ESP32-S3-DevKitC-1 that currently works.

Everything below the UI layer is already board-agnostic — radio modes, BLE, the
RaceChrono encoder, the configuration portal, logging. What differs is the display, the
power path and a handful of pins.

### Out of scope

- **The T-Beam's built-in u-blox MAX-M10S.** It is right there and tempting, but real
  GPS is separate work with its own failure modes — NMEA/UBX parsing, fix acquisition,
  time validity. Mixing it in means a failure could be the port *or* the GPS with no way
  to tell which. Phase 3 does GPS, on both boards.
- LoRa, the IMU, the RTC, the BME280, the SD slot. Present on the board, unused here.
- Any change to DevKitC behaviour. This branch must not regress a working board.

## 2. The two boards

| | ESP32-S3-DevKitC-1 | T-Beam Supreme |
| --- | --- | --- |
| MCU | ESP32-S3 | ESP32-S3 — same family |
| Flash | 16 MB | 8 MB |
| PSRAM | 8 MB **octal** — `qio_opi` | 8 MB **quad** — `qio_qspi` |
| Display | ILI9341 320×240 colour, SPI | SH1106 128×64 mono, I²C (SDA 17, SCL 18) |
| Mode button | GPIO39, wired by hand | **GPIO0**, onboard User/Program button |
| Power | direct | AXP2101 PMU switches the peripheral rails |
| GPS | none | u-blox MAX-M10S (unused here) |

### Three things that will bite

**PSRAM is quad, not octal.** `qio_opi` on this board is the same class of error as the
partition-table mismatch documented in `docs/hardware-notes.md`: a silent boot loop with
no output. Getting `memory_type` right is a prerequisite, not a detail.

**The AXP2101 powers the display.** Display and GPS sit on PMU-switched rails. Without
initialising the PMU over I²C first, the screen is simply unpowered — which presents as
"the display driver is broken" and sends you debugging the wrong thing entirely.

**SH1106 is not SSD1306.** The controller has 132 columns of RAM against a 128-pixel
panel, so the wrong driver renders everything shifted two pixels sideways. That looks
like a wiring fault. U8g2 handles the offset.

## 3. Display becomes an interface

```
src/ui/Display.h        abstract — begin(), tick(nowMs, const DeviceStatus&)
src/ui/TftDisplay.*     ILI9341 320×240 — today's Display, renamed
src/ui/OledDisplay.*    SH1106 128×64 — new
```

This is the one abstraction the branch adds, and it is earned: there are genuinely two
implementations, and they share almost no code. Colour versus monochrome, SPI versus
I²C, 53×20 versus 21×8.

`main.cpp` holds a `Display&`, constructed from whichever concrete type the board flag
selects. Nothing else changes.

### `LogRing` is unchanged

It stays 53 columns × 20 rows and remains the **superset**: the TFT draws all 20 rows at
full width, the OLED draws the last 6 truncated to 21 characters. No shared-buffer
resizing, no coupling between the two displays, no host tests to rewrite.

## 4. OLED layout

128×64 at the 6×8 font is **21 columns × 8 rows**. Two header rows and six log rows fill
it exactly, so there is no room for a separator row — the header is drawn in inverse
video instead, which divides the areas more clearly and costs nothing.

```
+---------------------+
|teletrack    BLE CONN|  inverse video
|up 00:12:34   drop 0 |  inverse video
|mode: switching      |
|ble: advertising as ~|
|ap: up ssid=teletra~ |
|http: listening on ~ |
|cfg: no stored sett~ |
|boot: teletrack      |
+---------------------+
```

**The timestamp and level are dropped on this display.** `HH:MM:SS [INF] ` spends 15 of
21 columns before the message starts. The OLED shows `tag: message` truncated with a
trailing `~`, the same convention `LogRing` already uses. Serial keeps the full line,
timestamp and level included.

Header content follows the TFT's, compressed:

| Mode | Row 0 | Row 1 |
| --- | --- | --- |
| WiFi | `<ssid>` … `AP UP` | `up HH:MM:SS` … `<n> cli` |
| BLE, advertising | `<name>` … `BLE ADV` | `up HH:MM:SS` |
| BLE, connected | `<name>` … `BLE CONN` | `up HH:MM:SS` … `drop <n>` |
| button held | unchanged | `HOLD 3s… <n>` |

## 5. Board configuration

One header holds every board-specific fact:

```
src/board/BoardConfig.h
```

Display type, I²C pins, mode-button GPIO, whether a PMU is present. Selected by a build
flag. A pin that differs between boards appears **once**, here — not scattered through
the modules.

```ini
[env:esp]                          [env:tbeam]
board_build.arduino.memory_type    board_build.arduino.memory_type
  = qio_opi                          = qio_qspi
board_build.flash_size = 16MB      board_build.flash_size = 8MB
board_upload.flash_size = 16MB     (stock 8MB partition table)
board_build.partitions
  = default_16MB.csv
-DBOARD_DEVKITC                    -DBOARD_TBEAM
```

The DevKitC environment is **not touched**. Its settings were expensive to establish and
each carries a comment explaining why; the new environment is additive.

## 6. Power management

```
src/board/Pmu.h/.cpp    AXP2101 on the T-Beam; a no-op elsewhere
```

`Pmu::begin()` runs in `setup()` **before** `Display::begin()`, and enables the rails the
display and GPS sit on. On the DevKitC every method compiles to nothing.

Keeping it as a real (if empty) module rather than an `#ifdef` in `main.cpp` means the
ordering constraint — power before display — is visible in one place instead of implied.

## 7. Libraries

| | For | Environment |
| --- | --- | --- |
| `olikraus/U8g2` | SH1106, handles the 132-column offset | `tbeam` only |
| `lewisxhe/XPowersLib` | AXP2101 | `tbeam` only |

Both are additive. The DevKitC build links neither.

## 8. Mode button

`GPIO0`, the onboard **User/Program** button — the left one. Active-low with an existing
pull-up, so `HoldDetector` and `ModeButton` work unchanged and nothing needs soldering.

The Power button is wired to the AXP2101 and read over I²C, and long-press already means
power-off; overloading it would fight the PMU. Reset is hardware-only.

GPIO0 is a strapping pin: held low **at reset** the board enters download mode. That is
how it gets flashed, so it is expected rather than a hazard — but it means never holding
the mode button while power-cycling.

## 9. Testing

**No new host tests.** Both displays are I/O shells with no logic, exactly like the
existing `Display`. A test here would assert nothing. The logic they draw —
`LogRing`, `Format`, `ModeController`, `HoldDetector`, the RaceChrono encoder — is
already covered by the 95 host tests, and those must continue to pass unchanged.

**The gate is:**

- `pio run -e esp` and `pio run -e tbeam` both succeed.
- `pio test -e native` still reports 95.
- Neither `OledDisplay.cpp`, `TftDisplay.cpp` nor `Pmu.cpp` enters `[env:native]`'s
  `build_src_filter` — all three include Arduino or a driver library.

**On hardware, T-Beam:** boots without a loop, OLED shows header and log, BLE advertises
and RaceChrono connects, the User button toggles modes on a 3-second hold, the WiFi
portal serves its page.

**On hardware, DevKitC — the regression check that matters most:** flash the branch and
confirm the TFT, BLE, RaceChrono, mode switch and portal all behave exactly as they did
before. A second target is not worth breaking the board that works.

## 10. Success criteria

- [ ] Both environments build from one source tree.
- [ ] T-Beam boots — correct `qio_qspi`, no boot loop.
- [ ] AXP2101 brings up the display rail before the display initialises.
- [ ] OLED shows a two-row header and six log rows, undistorted (no SH1106 offset).
- [ ] BLE works on the T-Beam and RaceChrono connects.
- [ ] The onboard User button toggles modes on a 3-second hold.
- [ ] DevKitC behaviour is unchanged, verified on hardware.
- [ ] 95 host tests still pass.

## 11. Risks

**Board-specific settings are guesses until flashed.** The PSRAM mode, the I²C pins and
the button GPIO come from vendor documentation, not from this board on this desk. Each is
a one-line change in `BoardConfig.h` or `platformio.ini`, and the first flash is where
they get confirmed.

**The AXP2101 rail names are the least certain part.** Which LDO or DC rail feeds the
display varies across T-Beam revisions. If the screen stays dark on a board that is
otherwise booting, this is the first thing to check — not the display driver.

**Regressing the DevKitC is the real risk**, and it is why the display refactor is a
rename plus an interface rather than a rewrite. `TftDisplay` should be today's code with
a new name and a base class, nothing more.
