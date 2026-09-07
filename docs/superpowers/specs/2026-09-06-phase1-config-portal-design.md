# Phase 1 — Configuration Portal

**Date:** 2026-09-06
**Status:** Approved, ready for implementation planning

## 1. Goal

The device boots, opens an open WiFi access point, and serves a web UI that lets a
connected phone or laptop read and change device settings. Settings persist across
reboots. The TFT shows a status header and a scrolling log so the device is usable
and debuggable without a serial cable.

Phase 1 proves the whole configuration chain end to end. The settings themselves are
deliberately placeholders — real settings arrive in later phases alongside the
features that need them.

### Out of scope

- GPS: no driver, no fix handling, no NMEA/UBX parsing.
- Logging to storage: no SD card, no filesystem, no session files.
- WiFi station mode, OTA updates, authentication.
- Any UI on the screen beyond the status header and log console.
- Turning the AP off. It runs from boot until power-down.

## 2. Hardware

- ESP32-S3-DevKitC-1, 16 MB flash, PSRAM (`qio_opi`).
- ILI9341 320×240 TFT over SPI, pins already defined in `platformio.ini`
  (MOSI 11, SCLK 12, CS 10, DC 8, RST 9, BL 21).
- USB CDC serial on boot for logs.

## 3. Module structure

Modules are cut by feature, not by technical layer. WiFi, HTTP, and the settings
themselves all exist to serve one purpose — configuring the device — so they form
one module with one public surface.

```
src/main.cpp                  wiring + loop() pump, nothing else

src/core/                     shared primitives, no feature knowledge
  Log.h/.cpp                  Log::info(tag, fmt, ...) — fans out to sinks
  LogSink.h                   abstract sink interface
  SerialSink.h/.cpp           LogSink → USB CDC
  Format.h/.cpp               formatUptime(), formatLine()          pure
  DeviceStatus.h              plain struct, produced by config, consumed by ui

src/config/                   the configuration module
  Settings.h/.cpp             PUBLIC — struct, defaults, validate()  pure
  SettingsStore.h             PUBLIC — abstract load/save
  ConfigPortal.h/.cpp         PUBLIC — facade: begin(), tick(), status()
  internal/
    ApManager.h/.cpp          SoftAP lifecycle, client-count polling
    CaptivePortal.h/.cpp      DNSServer wildcard + OS-probe routes
    WebUi.h/.cpp              route registration, HTTP plumbing only
    ConfigApi.h/.cpp          Settings ⇄ JSON, error shaping         pure
    NvsStore.h/.cpp           Preferences-backed SettingsStore
    MemoryStore.h/.cpp        in-RAM SettingsStore — test double
    ui_index.h                generated, gitignored

src/ui/
  LogRing.h/.cpp              fixed ring of log lines                pure
  Display.h/.cpp              : public LogSink — owns TFT_eSPI

data/ui/index.html            the web UI source, one file
tools/embed_ui.py             pre-build script: gzip → C header
```

`main.cpp` knows exactly three names: `Settings`, `ConfigPortal`, `Display`.

### Why `Settings` is public but the rest is internal

In Phase 2 the GPS module needs to read a sample rate. If `Settings` were private
behind `ConfigPortal`, GPS would have to depend on the entire AP-and-HTTP stack to
read one integer. So the module exposes two things — the settings *data*, and the
portal that *edits* it. How editing works is private.

Code outside `src/config/` includes `config/Settings.h` and `config/ConfigPortal.h`.
Nothing else. Tests are part of the module and may include from `internal/`.

### Testability boundary

Each pair splits logic from I/O so the logic can be tested on the host:

| Pure (host-tested)      | I/O shell (not unit-tested)   |
| ----------------------- | ----------------------------- |
| `Settings::validate()`  | `NvsStore`                    |
| `ConfigApi`             | `WebUi`                       |
| `LogRing`               | `Display`                     |
| `Format`                | `SerialSink`                  |

When a shell class grows logic worth testing, that logic moves into the pure half.

## 4. C++ conventions

- Plain classes, one per file, header/implementation split.
- `gnu++17`, `-fno-exceptions -fno-rtti`.
- Fixed-size `char` buffers. No Arduino `String`, no `std::string`, no heap
  allocation after `setup()` returns.
- Dependencies injected by constructor reference — this is what lets `MemoryStore`
  substitute for `NvsStore` in tests.
- No global variables except one `App` struct instantiated in `main.cpp`.
- `snprintf`/`vsnprintf` with explicit sizes for all string building.

## 5. Settings model

| Field           | Type      | Default     | Valid                              |
| --------------- | --------- | ----------- | ---------------------------------- |
| `deviceName`    | `char[32]`| `teletrack` | 1–31 chars, `[A-Za-z0-9_-]` only   |
| `sampleHz`      | `uint8_t` | `10`        | one of 1, 5, 10, 25                |

Stored in NVS namespace `teletrack`, one key per field. A missing key yields the
default.

`Settings::validate()` returns a result carrying a per-field error message. It is a
pure function over the struct — no NVS, no JSON, no HTTP.

### Save semantics

All-or-nothing. `ConfigApi` validates every field of an incoming payload before
anything is written. If any field fails, nothing is persisted and the in-memory
`Settings` is untouched. This avoids a half-applied config where `deviceName` saved
but `sampleHz` did not.

A `POST` payload omitting a field leaves that field at its current value. Only
supplied fields are updated — but they are still validated as a set.

## 6. HTTP API

Served by `ESPAsyncWebServer` on port 80.

### `GET /`

The gzipped single-page UI from PROGMEM.
Headers: `Content-Type: text/html`, `Content-Encoding: gzip`, `Cache-Control: no-store`.

### `GET /api/config`

```json
{ "deviceName": "teletrack", "sampleHz": 10 }
```

### `POST /api/config`

Request body is JSON, capped at 1024 bytes; a larger body is rejected with `413`
without being buffered. Malformed JSON returns `400`.

Success — `200`:

```json
{ "ok": true }
```

Validation failure — `400`, nothing written:

```json
{
  "ok": false,
  "errors": {
    "sampleHz": "must be 1, 5, 10 or 25",
    "deviceName": "1-31 characters, letters digits _ - only"
  }
}
```

Persistence failure — `500`, in-memory `Settings` rolled back to its prior value:

```json
{ "ok": false, "errors": { "_": "could not write to storage" } }
```

### `GET /api/status`

```json
{
  "ssid": "teletrack",
  "ip": "192.168.4.1",
  "clients": 1,
  "uptimeMs": 134221,
  "freeHeap": 186432
}
```

The UI polls this every 2 s while its tab is visible, and stops polling when hidden.

## 7. Access point and captive portal

- Open network, no password. SSID `teletrack`, channel 1, max 4 clients.
- IP `192.168.4.1`, netmask `255.255.255.0`, DHCP from the SoftAP default pool.
- SSID is a compile-time constant in Phase 1, deliberately **not** wired to
  `deviceName`. A rename that takes effect immediately could drop the very client
  making the change, and there is no second channel to recover through.

`DNSServer` on port 53 answers every query with `192.168.4.1`. Explicit routes are
registered for the OS connectivity probes:

```
/generate_204            /gen_204                 (Android)
/hotspot-detect.html     /library/test/success.html   (iOS, macOS)
/ncsi.txt                /connecttest.txt         (Windows)
/redirect
```

All of these, and the catch-all `onNotFound`, return `302` to
`http://192.168.4.1/`. Returning a redirect rather than a `204` is what makes the OS
classify the network as needing sign-in and raise the captive-portal sheet.

## 8. Concurrency

`ESPAsyncWebServer` invokes handlers on the **AsyncTCP task**, not from `loop()`.
A log call inside an HTTP handler therefore runs on a different task than the one
driving the TFT over SPI. Drawing from a handler would race with `loop()` on the SPI
bus and on TFT_eSPI's internal state.

The rule: **no module draws. Only `Display::tick()` draws, and only from `loop()`.**

- `Display::write()` — the `LogSink` method, callable from any task — formats the
  line and appends it to `LogRing` inside a short `portMUX_TYPE` critical section,
  then returns. It never touches SPI.
- `Display::tick()` snapshots changed rows into a local buffer under the same
  critical section, releases it, then draws. Drawing never happens with the lock
  held.
- `Settings` is read and written only from the AsyncTCP task (HTTP handlers) and
  read at boot. `loop()` does not touch it in Phase 1.

```cpp
void loop() {
  app.portal.tick();    // DNS pump + client-count poll
  app.display.tick();   // redraw if dirty, rate-limited
}
```

`ConfigPortal::tick()` calls `dns.processNextRequest()` on every pass and polls
`WiFi.softAPgetStationNum()` at 1 Hz, emitting a log line when the count changes.

## 9. Screen

Landscape, rotation 1, 320×240.

**Header** — top 40 px, TFT_eSPI font 2. Two lines:

```
teletrack              192.168.4.1   clients:1
AP UP                  up 00:02:14
```

Redrawn only when the rendered `DeviceStatus` differs from the previously drawn one,
so the uptime ticks once per second and nothing else repaints.

**Log** — remaining 200 px, GLCD font (font 1) at size 1. That font is 6×8 px and
monospace, which the proportional font 2 is not; a log console needs fixed columns.
At a 10 px line pitch this gives **53 columns × 20 rows**.

(This supersedes the 40×15 figure used while sketching the layout — that assumed
font 2 was monospace, which it isn't. Both dimensions are named constants in
`Display.h` and can be retuned without touching `LogRing`.)

`LogRing` holds 20 lines × 56 chars, overwriting the oldest. Lines longer than 53
columns are truncated with a trailing `~`.

The ring exposes a single monotonic `revision()` counter, bumped on every append.
`tick()` redraws the whole log region when the revision differs from the last drawn
one. Per-row change tracking would buy nothing here: the console scrolls, so a single
new line shifts the content of every row. A full-region redraw is ~1060 characters at
6×8 px ≈ 50 k pixels ≈ 20 ms over 40 MHz SPI, comfortably inside the 100 ms frame
budget. Text is drawn with an opaque background colour so each glyph overwrites the
previous one in place — no clear-then-draw, so no flicker.

Redraws are rate-limited to 10 Hz. A burst of log lines coalesces into a single
frame rather than queueing 30 full-region repaints.

Line format:

```
MM:SS.d [LVL] tag: message
00:42.3 [INF] http: GET /api/config
01:03.7 [WRN] http: POST /api/config 400
```

Levels are `DBG`, `INF`, `WRN`, `ERR`. `SerialSink` prints every level; `Display`
drops `DBG` so the screen stays readable.

## 10. Logging

```cpp
Log::info("ap", "started ch%d open", channel);
```

`Log` holds a fixed array of up to 4 `LogSink*`, registered during `setup()` and
never removed. Each call formats once into a shared fixed buffer under a critical
section and hands the result to every sink. `Log` has no knowledge of TFT, serial,
or HTTP.

`Display` and `SerialSink` are both `LogSink` implementations. Adding an SD-card log
sink in a later phase requires no change to any calling module.

## 11. Build

`tools/embed_ui.py` is a PlatformIO pre-build script. It gzips `data/ui/index.html`
into `src/config/internal/ui_index.h` as a `const uint8_t[]` in PROGMEM plus a
length constant, and regenerates only when the source is newer than the output. The
generated header is gitignored — the HTML is the source of truth.

`platformio.ini` gains:

- `esp32async/ESPAsyncWebServer@3.12.0` and `esp32async/AsyncTCP@3.5.0`. This library
  has a messy fork history; the maintained fork is the `esp32async` one, confirmed
  against the PlatformIO registry on 2026-09-06.
- No `board_build.partitions`: setting it to `default_16MB.csv` boot-loops the board
  (verified on hardware). The stock table's ~1.25 MB app partition is sufficient.
- `extra_scripts = pre:tools/embed_ui.py`.
- `build_unflags`/`build_flags` for `-fno-exceptions -fno-rtti -std=gnu++17`.
- A second environment, `[env:native]`, running host tests with no Arduino
  framework.

Unused libraries currently in `lib_deps` (`LiquidCrystal`, `ArduinoHttpClient`,
`TJpg_Decoder`, `Adafruit NeoPixel`) are removed. `ArduinoJson` and `TFT_eSPI` stay.

## 12. Failure handling

| Failure                        | Behavior                                                                                                       |
| ------------------------------ | -------------------------------------------------------------------------------------------------------------- |
| NVS unavailable or corrupt     | Run from RAM defaults and log one `ERR` line. Saves then fail with `500`. Not worth a reporting path in a proof of concept — it does not happen on a working board. |
| NVS write fails on save        | `500`, in-memory `Settings` rolled back, `ERR` logged.                                                           |
| Malformed or oversized JSON    | `400` / `413`, nothing written.                                                                                  |
| Any field fails validation     | `400` with per-field messages, nothing written.                                                                  |
| SoftAP fails to start          | `ERR` on screen and serial, header shows `AP FAIL`. Device keeps running so the log is readable.                 |
| TFT init fails                 | Logging continues to serial. `Display::tick()` becomes a no-op. The portal is unaffected.                        |

No failure path reboots the device. A device that reboots on error loses the log
that explains why.

## 13. Testing

### Host tests — `[env:native]`, no hardware

- **Settings**: defaults; every validation boundary (`deviceName` at 0, 1, 31, 32
  chars, and with an illegal character; `sampleHz` at each valid value and several
  invalid ones).
- **ConfigApi**: serialize round-trip; parse of a valid payload; parse of malformed
  JSON; partial payload leaves omitted fields untouched; a payload with one bad
  field writes nothing; error-shape matches the documented JSON.
- **LogRing**: append below capacity; wrap-around past capacity preserves the newest
  N in order; over-long line truncation; `revision()` advances on every append and
  only on append.
- **Format**: `formatUptime` at 0, sub-second, minute rollover, and hour rollover;
  `formatLine` truncation and level rendering.

`MemoryStore` backs every store-dependent test. No test in this environment links
Arduino, WiFi, or TFT_eSPI.

### On-device test

NVS round-trip: write non-default settings, reboot, read back and confirm they match.

### Manual acceptance

1. Flash and power on with no serial cable attached.
2. Read SSID and IP off the screen; watch boot log lines appear.
3. Connect a phone to `teletrack`. The captive portal sheet opens on its own.
4. Screen logs the client connection and the HTTP requests.
5. Change `sampleHz` to 25 and save. UI confirms; screen logs the save.
6. Submit an invalid `deviceName`. UI shows a field error; screen logs a `WRN`;
   reloading the page shows the old value still in place.
7. Power-cycle. `sampleHz` is still 25.

## 14. Success criteria

- [ ] Device opens an open AP named `teletrack` at `192.168.4.1` within 3 s of boot.
- [ ] A connecting phone raises the captive-portal sheet without manual navigation.
- [ ] The UI reads current settings and writes valid changes.
- [ ] Invalid input is rejected per-field with nothing persisted.
- [ ] Settings survive a power cycle.
- [ ] The screen shows live SSID, IP, client count, uptime, and a scrolling log.
- [ ] Log lines from HTTP handlers reach the screen without corrupting the display.
- [ ] Host test suite passes with no hardware attached.
- [ ] Nothing outside `src/config/` includes anything from `src/config/internal/`.

## 15. Risks

**`ESPAsyncWebServer` fork churn.** Multiple competing forks exist with divergent
APIs. Mitigation: pin an exact version of the `esp32async` fork and verify the
build compiles before the implementation plan is considered final.

**Captive-portal detection varies by OS and version.** Android in particular has
changed probe behavior across releases. Mitigation: the fallback is always usable —
the IP is on screen and typing it works even if the sheet never appears.

**SPI contention between TFT and WiFi.** Mitigated by the single-drawing-task rule
in §8, but it is the most likely source of a subtle Phase 1 bug. If the display
corrupts under load, that rule is the first thing to re-verify.
