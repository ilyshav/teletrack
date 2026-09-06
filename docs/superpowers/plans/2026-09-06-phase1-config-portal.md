# Phase 1 Configuration Portal — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship an ESP32-S3 firmware that opens an open WiFi AP with a captive-portal web UI for editing device settings, persists them to NVS, and shows a status header plus scrolling log on the TFT.

**Architecture:** One feature module, `src/config/`, owns everything about configuring the device — WiFi AP, DNS hijack, HTTP server, settings, persistence — behind a `ConfigPortal` facade. Only `Settings` and `SettingsStore` are public so later feature modules can read settings without pulling in the network stack. `src/core/` holds logging and formatting primitives, `src/ui/` holds the display. Every module splits pure logic from I/O so the logic runs in host tests with no hardware attached.

**Tech Stack:** PlatformIO 6.2, Arduino-ESP32, ESPAsyncWebServer 3.12.0 + AsyncTCP 3.5.0 (`esp32async` fork), ArduinoJson 7.4.3, TFT_eSPI 2.5.43, Unity for tests.

**Spec:** `docs/superpowers/specs/2026-09-06-phase1-config-portal-design.md`

## Global Constraints

- PlatformIO is **not on `PATH`**. Every command in this plan assumes you first ran
  `export PATH="$HOME/.platformio/penv/bin:$PATH"` in your shell. Do it once per session.
- C++ standard `gnu++17`. Build with `-fno-exceptions -fno-rtti`.
- Fixed-size `char` buffers for our own data. No Arduino `String`, no `std::string`,
  no `std::vector` in code we write. Library-internal allocation is fine — ArduinoJson 7
  allocates its document pool from the heap and that is accepted. Do not write custom
  allocators or pools to avoid it.
- One module per file pair, header/implementation split, `#pragma once` in every
  header. A small value type may share its producer's header when it has no
  independent use — `ValidationResult` lives in `Settings.h` because
  `Settings::validate()` is its only producer.
- No global variables except a single `App` struct in `main.cpp`.
- All string building goes through `snprintf`/`vsnprintf` with an explicit size.
- Nothing outside `src/config/` may include from `src/config/internal/`. Tests are part of
  the module and may.
- Only `Display::tick()` draws to the TFT, and only from `loop()`. No other code path
  touches SPI. This is the rule that prevents the AsyncTCP task from racing `loop()`.
- Settings field names on the wire are exactly `schemaVersion`, `deviceName`, `sampleHz`.
- Validation messages are exactly `"1-31 characters, letters digits _ - only"` and
  `"must be 1, 5, 10 or 25"`. Storage failure message is exactly `"could not write to storage"`.
- Commit after every task. Never commit `src/config/internal/ui_index.h` — it is generated.

## File Structure

| Path | Responsibility |
| --- | --- |
| `platformio.ini` | Two environments: `esp` (device) and `native` (host tests) |
| `tools/embed_ui.py` | Pre-build: gzip `data/ui/index.html` into a C header |
| `data/ui/index.html` | The entire web UI — one file, no external assets |
| `src/main.cpp` | Wiring and `loop()` pump only |
| `src/core/LogLevel.h` | `LogLevel` enum + `logLevelName()`, header-only |
| `src/core/Format.h/.cpp` | Uptime and log-line formatting — pure |
| `src/core/Log.h/.cpp` | Serial + on-screen logging; owns the console ring and its lock |
| `src/core/DeviceStatus.h` | Plain status struct, produced by config, consumed by ui |
| `src/config/Settings.h/.cpp` | PUBLIC — settings struct, defaults, validation — pure |
| `src/config/SettingsStore.h` | PUBLIC — abstract load/save/available |
| `src/config/ConfigPortal.h/.cpp` | PUBLIC — facade: `begin()`, `tick()`, `status()` |
| `src/config/internal/MemoryStore.h/.cpp` | In-RAM store — the test double |
| `src/config/internal/NvsStore.h/.cpp` | Preferences-backed store |
| `src/config/internal/ConfigApi.h/.cpp` | Settings ⇄ JSON, error shaping — pure |
| `src/config/internal/ApManager.h/.cpp` | SoftAP lifecycle, client-count polling |
| `src/config/internal/CaptivePortal.h/.cpp` | DNSServer wildcard + OS-probe routes |
| `src/config/internal/WebUi.h/.cpp` | HTTP route registration only |
| `src/config/internal/ui_index.h` | Generated, gitignored |
| `src/core/LogRing.h/.cpp` | Fixed ring of log lines — pure |
| `src/ui/Display.h/.cpp` | Draws the header and console; owns TFT_eSPI |
| `test/native/test_*/` | Host tests, no Arduino, no hardware |
| `test/embedded/test_*/` | On-device tests |

Tasks 1–6 and 8 build and test entirely on the host. Task 7 is the first task that needs
the board. Task 9 is the first flashable milestone (AP and captive portal), Task 10 the
first working web UI, Task 11 the first working screen.

---

### Task 1: Build foundation and the `Format` module

Reworks `platformio.ini` into two environments and proves the host test loop works by
delivering the first pure module.

**Files:**
- Modify: `platformio.ini` (whole file)
- Modify: `.gitignore`
- Create: `src/core/LogLevel.h`
- Create: `src/core/Format.h`, `src/core/Format.cpp`
- Test: `test/native/test_format/test_format.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class LogLevel : uint8_t { Debug, Info, Warn, Error }`
  - `const char* logLevelName(LogLevel)` → `"DBG" | "INF" | "WRN" | "ERR"`
  - `void Format::uptimeShort(uint32_t ms, char* out, size_t outSize)` → `"MM:SS.d"`
  - `void Format::uptimeLong(uint32_t ms, char* out, size_t outSize)` → `"HH:MM:SS"`
  - `void Format::logLine(uint32_t ms, LogLevel, const char* tag, const char* msg, char* out, size_t outSize)`

- [ ] **Step 1: Replace `platformio.ini`**

The existing file lists four libraries this project never uses (`LiquidCrystal`,
`ArduinoHttpClient`, `TJpg_Decoder`, `Adafruit NeoPixel`). They go. The TFT pin and driver
flags are kept **exactly** as they are — they describe the user's wiring and are not ours
to change.

```ini
; PlatformIO Project Configuration File
; https://docs.platformio.org/page/projectconf.html

[platformio]
default_envs = esp

[common]
build_flags =
    -std=gnu++17
    -fno-exceptions
    -fno-rtti
    -I src

[env:esp]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
board_build.flash_size = 16MB
board_build.partitions = default_16MB.csv
board_build.arduino.memory_type = qio_opi
build_flags =
    ${common.build_flags}
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DUSER_SETUP_LOADED
    -DILI9341_DRIVER
    -DTFT_WIDTH=240
    -DTFT_HEIGHT=320
    -DTFT_MOSI=11
    -DTFT_SCLK=12
    -DTFT_CS=10
    -DTFT_DC=8
    -DTFT_RST=9
    -DTFT_BL=21
    -DSPI_FREQUENCY=40000000
    -DTFT_SPI_PORT=3
    -DTFT_SPI_OVERLAP
    -DLOAD_GLCD
    -DLOAD_FONT2
    -DLOAD_FONT4
    -DSMOOTH_FONT
lib_deps =
    bblanchon/ArduinoJson@^7.4.3
    bodmer/TFT_eSPI@^2.5.43
    esp32async/AsyncTCP@^3.5.0
    esp32async/ESPAsyncWebServer@^3.12.0
monitor_speed = 115200
monitor_filters = esp32_exception_decoder
test_filter = embedded/*
build_type = debug

[env:native]
platform = native
build_flags =
    ${common.build_flags}
    -DUNIT_TEST
lib_deps =
    bblanchon/ArduinoJson@^7.4.3
test_build_src = yes
test_filter = native/*
build_src_filter =
    -<*>
    +<core/Format.cpp>
```

`extra_scripts` is deliberately absent here — the UI generator is added in Task 10, once
`data/ui/index.html` exists. Wiring it up now would fail every build with
`embed_ui: missing data/ui/index.html`.

`build_src_filter` in `[env:native]` is an allow-list: it names every source file that is
pure enough to compile without Arduino. **Each later task adds exactly one line to it.**
That list is the enforcement mechanism for the purity boundary — if a file you add there
fails to compile on the host, it has an Arduino dependency it should not have.

- [ ] **Step 2: Add the generated header to `.gitignore`**

Append to `.gitignore`:

```
src/config/internal/ui_index.h
```

- [ ] **Step 3: Write the failing test**

Create `test/native/test_format/test_format.cpp`:

```cpp
#include <unity.h>
#include <string.h>

#include "core/Format.h"
#include "core/LogLevel.h"

void setUp() {}
void tearDown() {}

static void test_level_names() {
  TEST_ASSERT_EQUAL_STRING("DBG", logLevelName(LogLevel::Debug));
  TEST_ASSERT_EQUAL_STRING("INF", logLevelName(LogLevel::Info));
  TEST_ASSERT_EQUAL_STRING("WRN", logLevelName(LogLevel::Warn));
  TEST_ASSERT_EQUAL_STRING("ERR", logLevelName(LogLevel::Error));
}

static void test_uptime_short_zero() {
  char buf[16];
  Format::uptimeShort(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00.0", buf);
}

static void test_uptime_short_subsecond() {
  char buf[16];
  Format::uptimeShort(342, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00.3", buf);
}

static void test_uptime_short_minute_rollover() {
  char buf[16];
  // 1 min 3.7 s
  Format::uptimeShort(63700, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("01:03.7", buf);
}

static void test_uptime_short_wraps_at_100_minutes() {
  char buf[16];
  // 100 minutes exactly wraps back to zero: the short form is a fixed-width
  // relative marker, not an absolute clock. uptimeLong() carries absolute time.
  Format::uptimeShort(100u * 60u * 1000u, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00.0", buf);
}

static void test_uptime_long_hours() {
  char buf[16];
  // 2 h 3 m 4 s
  Format::uptimeLong((2u * 3600u + 3u * 60u + 4u) * 1000u, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("02:03:04", buf);
}

static void test_uptime_long_saturates_at_99_hours() {
  char buf[16];
  Format::uptimeLong(4294967295u, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("99:59:59", buf);
}

static void test_log_line_shape() {
  char buf[64];
  Format::logLine(42300, LogLevel::Info, "http", "GET /api/config", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:42.3 [INF] http: GET /api/config", buf);
}

static void test_log_line_truncates_with_tilde() {
  char buf[20];  // far too small
  Format::logLine(42300, LogLevel::Warn, "http", "a very long message indeed",
                  buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT(19u, (unsigned)strlen(buf));
  TEST_ASSERT_EQUAL_CHAR('~', buf[18]);
}

static void test_log_line_tolerates_null_tag_and_message() {
  char buf[64];
  Format::logLine(0, LogLevel::Error, nullptr, nullptr, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00.0 [ERR] ?: ", buf);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_level_names);
  RUN_TEST(test_uptime_short_zero);
  RUN_TEST(test_uptime_short_subsecond);
  RUN_TEST(test_uptime_short_minute_rollover);
  RUN_TEST(test_uptime_short_wraps_at_100_minutes);
  RUN_TEST(test_uptime_long_hours);
  RUN_TEST(test_uptime_long_saturates_at_99_hours);
  RUN_TEST(test_log_line_shape);
  RUN_TEST(test_log_line_truncates_with_tilde);
  RUN_TEST(test_log_line_tolerates_null_tag_and_message);
  return UNITY_END();
}
```

- [ ] **Step 4: Run the test to verify it fails**

```bash
pio test -e native -f native/test_format
```

Expected: FAIL — `fatal error: core/Format.h: No such file or directory`.

- [ ] **Step 5: Write `src/core/LogLevel.h`**

```cpp
#pragma once

#include <stdint.h>

enum class LogLevel : uint8_t {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
};

inline const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "DBG";
    case LogLevel::Info:  return "INF";
    case LogLevel::Warn:  return "WRN";
    case LogLevel::Error: return "ERR";
  }
  return "???";
}
```

- [ ] **Step 6: Write `src/core/Format.h`**

Note what this header does *not* include: no `Arduino.h`. That is what lets it compile in
`[env:native]`.

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/LogLevel.h"

namespace Format {

// "MM:SS.d". Minutes wrap modulo 100 so the field stays a fixed 7 characters.
// This is a relative marker for the log console; use uptimeLong() for absolute time.
// Needs outSize >= 8.
void uptimeShort(uint32_t ms, char* out, size_t outSize);

// "HH:MM:SS", hours saturating at 99. Needs outSize >= 9.
void uptimeLong(uint32_t ms, char* out, size_t outSize);

// "MM:SS.d [LVL] tag: msg", truncated to outSize-1 characters with a trailing '~'
// when it does not fit. A null tag renders as "?", a null msg as empty.
// Needs outSize >= 2.
void logLine(uint32_t ms, LogLevel level, const char* tag, const char* msg,
             char* out, size_t outSize);

}  // namespace Format
```

- [ ] **Step 7: Write `src/core/Format.cpp`**

```cpp
#include "core/Format.h"

#include <stdio.h>

namespace Format {

void uptimeShort(uint32_t ms, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  const unsigned deci = static_cast<unsigned>((ms / 100u) % 10u);
  const unsigned secs = static_cast<unsigned>((ms / 1000u) % 60u);
  const unsigned mins = static_cast<unsigned>((ms / 60000u) % 100u);
  snprintf(out, outSize, "%02u:%02u.%u", mins, secs, deci);
}

void uptimeLong(uint32_t ms, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  const uint32_t total = ms / 1000u;
  unsigned hours = static_cast<unsigned>(total / 3600u);
  unsigned mins = static_cast<unsigned>((total / 60u) % 60u);
  unsigned secs = static_cast<unsigned>(total % 60u);
  if (hours > 99u) {
    hours = 99u;
    mins = 59u;
    secs = 59u;
  }
  snprintf(out, outSize, "%02u:%02u:%02u", hours, mins, secs);
}

void logLine(uint32_t ms, LogLevel level, const char* tag, const char* msg,
             char* out, size_t outSize) {
  if (out == nullptr || outSize < 2) {
    if (out != nullptr && outSize == 1) {
      out[0] = '\0';
    }
    return;
  }
  char stamp[9];
  uptimeShort(ms, stamp, sizeof(stamp));

  const int written = snprintf(out, outSize, "%s [%s] %s: %s", stamp,
                               logLevelName(level), tag != nullptr ? tag : "?",
                               msg != nullptr ? msg : "");
  if (written < 0) {
    out[0] = '\0';
    return;
  }
  if (static_cast<size_t>(written) >= outSize) {
    out[outSize - 2] = '~';
  }
}

}  // namespace Format
```

- [ ] **Step 8: Run the test to verify it passes**

```bash
pio test -e native -f native/test_format
```

Expected: PASS — `10 Tests 0 Failures 0 Ignored`.

- [ ] **Step 9: Verify the device environment still compiles**

This pulls the ESP32 toolchain on first run and can take several minutes.

```bash
pio run -e esp
```

Expected: `SUCCESS`. If it fails on `-DTFT_SPI_OVERLAP`, that flag is an ESP8266-only
option inherited from the original config — drop it and re-run, and note the change.

- [ ] **Step 10: Commit**

```bash
git add platformio.ini .gitignore src/core/LogLevel.h src/core/Format.h src/core/Format.cpp test/native/test_format/test_format.cpp
git commit -m "Add host test environment and core Format module"
```

---

### Task 2: `Settings` and validation

The public data type of the config module. Pure — no NVS, no JSON, no HTTP.

**Files:**
- Create: `src/config/Settings.h`, `src/config/Settings.cpp`
- Modify: `platformio.ini` (`build_src_filter` in `[env:native]`)
- Test: `test/native/test_settings/test_settings.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct Settings { uint8_t schemaVersion; char deviceName[32]; uint8_t sampleHz; }`
  - `static Settings Settings::defaults()`
  - `ValidationResult Settings::validate() const`
  - `struct ValidationResult` with `count`, `errors[4]` of `{const char* field; const char* message;}`,
    `bool ok() const`, `void add(const char*, const char*)`, `const char* messageFor(const char*) const`
  - `SettingsError::kDeviceName`, `SettingsError::kSampleHz`, `SettingsError::kStorage`

- [ ] **Step 1: Write the failing test**

Create `test/native/test_settings/test_settings.cpp`:

```cpp
#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "config/Settings.h"

void setUp() {}
void tearDown() {}

static Settings withName(const char* name) {
  Settings s = Settings::defaults();
  memset(s.deviceName, 0, sizeof(s.deviceName));
  snprintf(s.deviceName, sizeof(s.deviceName), "%s", name);
  return s;
}

static void test_defaults() {
  const Settings s = Settings::defaults();
  TEST_ASSERT_EQUAL_UINT8(1, s.schemaVersion);
  TEST_ASSERT_EQUAL_STRING("teletrack", s.deviceName);
  TEST_ASSERT_EQUAL_UINT8(10, s.sampleHz);
  TEST_ASSERT_TRUE(s.validate().ok());
}

static void test_device_name_empty_is_invalid() {
  const ValidationResult v = withName("").validate();
  TEST_ASSERT_FALSE(v.ok());
  TEST_ASSERT_EQUAL_STRING(SettingsError::kDeviceName, v.messageFor("deviceName"));
}

static void test_device_name_single_char_is_valid() {
  TEST_ASSERT_TRUE(withName("a").validate().ok());
}

static void test_device_name_31_chars_is_valid() {
  char name[32];
  memset(name, 'a', 31);
  name[31] = '\0';
  TEST_ASSERT_TRUE(withName(name).validate().ok());
}

static void test_device_name_without_terminator_is_invalid() {
  Settings s = Settings::defaults();
  memset(s.deviceName, 'a', sizeof(s.deviceName));  // no NUL anywhere
  const ValidationResult v = s.validate();
  TEST_ASSERT_FALSE(v.ok());
  TEST_ASSERT_EQUAL_STRING(SettingsError::kDeviceName, v.messageFor("deviceName"));
}

static void test_device_name_rejects_illegal_characters() {
  TEST_ASSERT_FALSE(withName("has space").validate().ok());
  TEST_ASSERT_FALSE(withName("has.dot").validate().ok());
  TEST_ASSERT_FALSE(withName("has/slash").validate().ok());
}

static void test_device_name_accepts_legal_characters() {
  TEST_ASSERT_TRUE(withName("Track-Day_01").validate().ok());
}

static void test_sample_hz_accepts_only_the_four_rates() {
  const uint8_t valid[] = {1, 5, 10, 25};
  for (uint8_t rate : valid) {
    Settings s = Settings::defaults();
    s.sampleHz = rate;
    TEST_ASSERT_TRUE(s.validate().ok());
  }
  const uint8_t invalid[] = {0, 2, 9, 11, 24, 26, 255};
  for (uint8_t rate : invalid) {
    Settings s = Settings::defaults();
    s.sampleHz = rate;
    const ValidationResult v = s.validate();
    TEST_ASSERT_FALSE(v.ok());
    TEST_ASSERT_EQUAL_STRING(SettingsError::kSampleHz, v.messageFor("sampleHz"));
  }
}

static void test_reports_multiple_errors_together() {
  Settings s = withName("bad name");
  s.sampleHz = 3;
  const ValidationResult v = s.validate();
  TEST_ASSERT_EQUAL_UINT(2u, (unsigned)v.count);
  TEST_ASSERT_EQUAL_STRING(SettingsError::kDeviceName, v.messageFor("deviceName"));
  TEST_ASSERT_EQUAL_STRING(SettingsError::kSampleHz, v.messageFor("sampleHz"));
}

static void test_message_for_unknown_field_is_null() {
  TEST_ASSERT_NULL(Settings::defaults().validate().messageFor("nope"));
}

static void test_add_replaces_rather_than_duplicates() {
  ValidationResult v;
  v.add("a", "first");
  v.add("a", "second");
  TEST_ASSERT_EQUAL_UINT(1u, (unsigned)v.count);
  TEST_ASSERT_EQUAL_STRING("second", v.messageFor("a"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_defaults);
  RUN_TEST(test_device_name_empty_is_invalid);
  RUN_TEST(test_device_name_single_char_is_valid);
  RUN_TEST(test_device_name_31_chars_is_valid);
  RUN_TEST(test_device_name_without_terminator_is_invalid);
  RUN_TEST(test_device_name_rejects_illegal_characters);
  RUN_TEST(test_device_name_accepts_legal_characters);
  RUN_TEST(test_sample_hz_accepts_only_the_four_rates);
  RUN_TEST(test_reports_multiple_errors_together);
  RUN_TEST(test_message_for_unknown_field_is_null);
  RUN_TEST(test_add_replaces_rather_than_duplicates);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
pio test -e native -f native/test_settings
```

Expected: FAIL — `fatal error: config/Settings.h: No such file or directory`.

- [ ] **Step 3: Write `src/config/Settings.h`**

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

// The exact strings sent to the client. Single source of truth — the web UI,
// ConfigApi and the tests all reference these, never literals.
namespace SettingsError {
inline constexpr const char* kDeviceName = "1-31 characters, letters digits _ - only";
inline constexpr const char* kSampleHz = "must be 1, 5, 10 or 25";
inline constexpr const char* kStorage = "could not write to storage";
}  // namespace SettingsError

struct ValidationResult {
  static constexpr size_t kMaxErrors = 4;

  struct Error {
    const char* field;
    const char* message;
  };

  Error errors[kMaxErrors] = {};
  size_t count = 0;

  bool ok() const { return count == 0; }

  // Adds an error, replacing any existing error for the same field. Silently
  // drops the error if kMaxErrors is already reached — there are only two
  // fields, so that cannot happen in practice.
  void add(const char* field, const char* message);

  // Returns nullptr when the field has no error.
  const char* messageFor(const char* field) const;
};

struct Settings {
  static constexpr uint8_t kSchemaVersion = 1;
  static constexpr size_t kDeviceNameSize = 32;  // 31 usable characters + NUL

  uint8_t schemaVersion = kSchemaVersion;
  char deviceName[kDeviceNameSize] = {};
  uint8_t sampleHz = 10;

  static Settings defaults();

  // Pure. Never touches storage, JSON or the network.
  ValidationResult validate() const;

  static bool isValidSampleHz(uint8_t hz);
};
```

- [ ] **Step 4: Write `src/config/Settings.cpp`**

```cpp
#include "config/Settings.h"

#include <stdio.h>
#include <string.h>

void ValidationResult::add(const char* field, const char* message) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(errors[i].field, field) == 0) {
      errors[i].message = message;
      return;
    }
  }
  if (count >= kMaxErrors) {
    return;
  }
  errors[count].field = field;
  errors[count].message = message;
  ++count;
}

const char* ValidationResult::messageFor(const char* field) const {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(errors[i].field, field) == 0) {
      return errors[i].message;
    }
  }
  return nullptr;
}

Settings Settings::defaults() {
  Settings s;
  s.schemaVersion = kSchemaVersion;
  memset(s.deviceName, 0, sizeof(s.deviceName));
  snprintf(s.deviceName, sizeof(s.deviceName), "%s", "teletrack");
  s.sampleHz = 10;
  return s;
}

bool Settings::isValidSampleHz(uint8_t hz) {
  return hz == 1 || hz == 5 || hz == 10 || hz == 25;
}

static bool isLegalNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

ValidationResult Settings::validate() const {
  ValidationResult result;

  const size_t len = strnlen(deviceName, kDeviceNameSize);
  bool nameOk = len >= 1 && len <= kDeviceNameSize - 1;
  if (nameOk) {
    for (size_t i = 0; i < len; ++i) {
      if (!isLegalNameChar(deviceName[i])) {
        nameOk = false;
        break;
      }
    }
  }
  if (!nameOk) {
    result.add("deviceName", SettingsError::kDeviceName);
  }

  if (!isValidSampleHz(sampleHz)) {
    result.add("sampleHz", SettingsError::kSampleHz);
  }

  return result;
}
```

- [ ] **Step 5: Add `Settings.cpp` to the host build**

In `platformio.ini`, `[env:native]`, extend `build_src_filter`:

```ini
build_src_filter =
    -<*>
    +<core/Format.cpp>
    +<config/Settings.cpp>
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
pio test -e native -f native/test_settings
```

Expected: PASS — `11 Tests 0 Failures 0 Ignored`.

- [ ] **Step 7: Commit**

```bash
git add platformio.ini src/config/Settings.h src/config/Settings.cpp test/native/test_settings/test_settings.cpp
git commit -m "Add Settings struct with defaults and validation"
```

---

### Task 3: `SettingsStore` interface and `MemoryStore` test double

The seam that lets everything above it be tested without NVS.

**Files:**
- Create: `src/config/SettingsStore.h`
- Create: `src/config/internal/MemoryStore.h`, `src/config/internal/MemoryStore.cpp`
- Modify: `platformio.ini` (`build_src_filter`)
- Test: `test/native/test_memory_store/test_memory_store.cpp`

**Interfaces:**
- Consumes: `Settings` from Task 2.
- Produces:
  - `class SettingsStore` — `virtual bool load(Settings&)`, `virtual bool save(const Settings&)`, `virtual bool available() const`
  - `class MemoryStore : public SettingsStore` — plus `setAvailable(bool)`, `failNextSave()`, `hasStored() const`, `stored() const`

- [ ] **Step 1: Write the failing test**

Create `test/native/test_memory_store/test_memory_store.cpp`:

```cpp
#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "config/internal/MemoryStore.h"

void setUp() {}
void tearDown() {}

static void test_load_on_empty_store_returns_false_and_leaves_out_untouched() {
  MemoryStore store;
  Settings out = Settings::defaults();
  out.sampleHz = 25;

  TEST_ASSERT_FALSE(store.load(out));
  TEST_ASSERT_EQUAL_UINT8(25, out.sampleHz);  // untouched
}

static void test_save_then_load_round_trips() {
  MemoryStore store;
  Settings in = Settings::defaults();
  in.sampleHz = 25;
  snprintf(in.deviceName, sizeof(in.deviceName), "%s", "bike-one");

  TEST_ASSERT_TRUE(store.save(in));

  Settings out;
  TEST_ASSERT_TRUE(store.load(out));
  TEST_ASSERT_EQUAL_UINT8(25, out.sampleHz);
  TEST_ASSERT_EQUAL_STRING("bike-one", out.deviceName);
  TEST_ASSERT_EQUAL_UINT8(Settings::kSchemaVersion, out.schemaVersion);
}

static void test_fail_next_save_fails_once_and_stores_nothing() {
  MemoryStore store;
  Settings in = Settings::defaults();
  in.sampleHz = 25;

  store.failNextSave();
  TEST_ASSERT_FALSE(store.save(in));
  TEST_ASSERT_FALSE(store.hasStored());

  TEST_ASSERT_TRUE(store.save(in));  // the next one succeeds
  TEST_ASSERT_TRUE(store.hasStored());
}

static void test_unavailable_store_refuses_both_operations() {
  MemoryStore store;
  store.setAvailable(false);

  Settings s = Settings::defaults();
  TEST_ASSERT_FALSE(store.available());
  TEST_ASSERT_FALSE(store.save(s));
  TEST_ASSERT_FALSE(store.load(s));
}

static void test_usable_through_the_base_interface() {
  MemoryStore concrete;
  SettingsStore& store = concrete;

  Settings in = Settings::defaults();
  in.sampleHz = 5;
  TEST_ASSERT_TRUE(store.save(in));

  Settings out;
  TEST_ASSERT_TRUE(store.load(out));
  TEST_ASSERT_EQUAL_UINT8(5, out.sampleHz);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_load_on_empty_store_returns_false_and_leaves_out_untouched);
  RUN_TEST(test_save_then_load_round_trips);
  RUN_TEST(test_fail_next_save_fails_once_and_stores_nothing);
  RUN_TEST(test_unavailable_store_refuses_both_operations);
  RUN_TEST(test_usable_through_the_base_interface);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
pio test -e native -f native/test_memory_store
```

Expected: FAIL — `fatal error: config/internal/MemoryStore.h: No such file or directory`.

- [ ] **Step 3: Write `src/config/SettingsStore.h`**

```cpp
#pragma once

#include "config/Settings.h"

// Abstract persistence for Settings. The only reason this interface exists is so
// that everything above it can be tested on the host against MemoryStore.
class SettingsStore {
 public:
  virtual ~SettingsStore() = default;

  // Reads persisted settings into `out`. Returns false when nothing valid could
  // be read; `out` is left untouched in that case, so the caller keeps its
  // defaults.
  virtual bool load(Settings& out) = 0;

  // Persists `s`. Returns false when the write failed.
  virtual bool save(const Settings& s) = 0;

  // False when the backing store is unusable. The device still runs, from RAM
  // defaults, and reports persistDegraded to the client.
  virtual bool available() const = 0;
};
```

- [ ] **Step 4: Write `src/config/internal/MemoryStore.h`**

```cpp
#pragma once

#include "config/SettingsStore.h"

// In-RAM SettingsStore. This is the test double: it is the only implementation
// that host tests link against, and its failure switches let tests drive the
// degraded and write-failure paths deterministically.
class MemoryStore : public SettingsStore {
 public:
  bool load(Settings& out) override;
  bool save(const Settings& s) override;
  bool available() const override;

  void setAvailable(bool value);
  void failNextSave();
  bool hasStored() const;
  const Settings& stored() const;

 private:
  Settings stored_ = Settings::defaults();
  bool hasStored_ = false;
  bool available_ = true;
  bool failNextSave_ = false;
};
```

- [ ] **Step 5: Write `src/config/internal/MemoryStore.cpp`**

```cpp
#include "config/internal/MemoryStore.h"

bool MemoryStore::load(Settings& out) {
  if (!available_ || !hasStored_) {
    return false;
  }
  out = stored_;
  return true;
}

bool MemoryStore::save(const Settings& s) {
  if (!available_) {
    return false;
  }
  if (failNextSave_) {
    failNextSave_ = false;
    return false;
  }
  stored_ = s;
  hasStored_ = true;
  return true;
}

bool MemoryStore::available() const { return available_; }

void MemoryStore::setAvailable(bool value) { available_ = value; }

void MemoryStore::failNextSave() { failNextSave_ = true; }

bool MemoryStore::hasStored() const { return hasStored_; }

const Settings& MemoryStore::stored() const { return stored_; }
```

- [ ] **Step 6: Add `MemoryStore.cpp` to the host build**

In `platformio.ini`, `[env:native]`:

```ini
build_src_filter =
    -<*>
    +<core/Format.cpp>
    +<config/Settings.cpp>
    +<config/internal/MemoryStore.cpp>
```

- [ ] **Step 7: Run the test to verify it passes**

```bash
pio test -e native -f native/test_memory_store
```

Expected: PASS — `5 Tests 0 Failures 0 Ignored`.

- [ ] **Step 8: Commit**

```bash
git add platformio.ini src/config/SettingsStore.h src/config/internal/MemoryStore.h src/config/internal/MemoryStore.cpp test/native/test_memory_store/test_memory_store.cpp
git commit -m "Add SettingsStore interface and in-memory implementation"
```

---

### Task 4: `ConfigApi` — Settings ⇄ JSON

All the wire-format logic, with zero HTTP in it. This is what makes the API testable
without a network stack.

**Files:**
- Create: `src/config/internal/ConfigApi.h`, `src/config/internal/ConfigApi.cpp`
- Modify: `platformio.ini` (`build_src_filter`)
- Test: `test/native/test_config_api/test_config_api.cpp`

**Interfaces:**
- Consumes: `Settings`, `ValidationResult`, `SettingsError` from Task 2.
- Produces:
  - `enum class ConfigApi::ParseStatus : uint8_t { Ok, BadJson, Invalid }`
  - `struct ConfigApi::ParseResult { ParseStatus status; ValidationResult validation; }`
  - `size_t ConfigApi::toJson(const Settings&, char* out, size_t outSize)`
  - `ConfigApi::ParseResult ConfigApi::applyJson(const char* body, size_t len, Settings& inOut)`
  - `size_t ConfigApi::okToJson(char* out, size_t outSize)`
  - `size_t ConfigApi::errorsToJson(const ValidationResult&, char* out, size_t outSize)`
  - `size_t ConfigApi::storageErrorToJson(char* out, size_t outSize)`
  - `ConfigApi::kMaxBodyBytes == 1024`, `ConfigApi::kJsonBufferSize == 512`

The `*ToJson` functions return the number of characters written. Callers pass a
`kJsonBufferSize` buffer, which is far larger than any document produced here.

- [ ] **Step 1: Write the failing test**

Create `test/native/test_config_api/test_config_api.cpp`:

```cpp
#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "config/internal/ConfigApi.h"

void setUp() {}
void tearDown() {}

static void test_to_json_shape() {
  char buf[ConfigApi::kJsonBufferSize];
  const size_t n = ConfigApi::toJson(Settings::defaults(), buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"schemaVersion\":1,\"deviceName\":\"teletrack\",\"sampleHz\":10}", buf);
}

static void test_to_json_refuses_a_buffer_that_is_too_small() {
  char buf[8];
  TEST_ASSERT_EQUAL_UINT(0u, (unsigned)ConfigApi::toJson(Settings::defaults(), buf, sizeof(buf)));
}

static void test_ok_json() {
  char buf[32];
  const size_t n = ConfigApi::okToJson(buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", buf);
}

static void test_storage_error_json() {
  char buf[ConfigApi::kJsonBufferSize];
  ConfigApi::storageErrorToJson(buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"_\":\"could not write to storage\"}}", buf);
}

static void test_errors_json_shape() {
  ValidationResult v;
  v.add("sampleHz", SettingsError::kSampleHz);
  char buf[ConfigApi::kJsonBufferSize];
  ConfigApi::errorsToJson(v, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"sampleHz\":\"must be 1, 5, 10 or 25\"}}", buf);
}

static void test_apply_valid_payload_updates_settings() {
  Settings s = Settings::defaults();
  const char* body = "{\"deviceName\":\"bike-one\",\"sampleHz\":25}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Ok);
  TEST_ASSERT_EQUAL_STRING("bike-one", s.deviceName);
  TEST_ASSERT_EQUAL_UINT8(25, s.sampleHz);
}

static void test_apply_partial_payload_leaves_other_fields_alone() {
  Settings s = Settings::defaults();
  const char* body = "{\"sampleHz\":5}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Ok);
  TEST_ASSERT_EQUAL_UINT8(5, s.sampleHz);
  TEST_ASSERT_EQUAL_STRING("teletrack", s.deviceName);
}

static void test_apply_ignores_schema_version_from_the_client() {
  Settings s = Settings::defaults();
  const char* body = "{\"schemaVersion\":99,\"sampleHz\":5}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Ok);
  TEST_ASSERT_EQUAL_UINT8(Settings::kSchemaVersion, s.schemaVersion);
}

static void test_apply_malformed_json_is_bad_json() {
  Settings s = Settings::defaults();
  const char* body = "{not json";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::BadJson);
  TEST_ASSERT_EQUAL_UINT8(10, s.sampleHz);  // untouched
}

static void test_apply_non_object_json_is_bad_json() {
  Settings s = Settings::defaults();
  const char* body = "[1,2,3]";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);
  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::BadJson);
}

static void test_apply_writes_nothing_when_any_field_is_invalid() {
  Settings s = Settings::defaults();
  // deviceName is fine, sampleHz is not. Neither may be applied.
  const char* body = "{\"deviceName\":\"bike-one\",\"sampleHz\":7}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Invalid);
  TEST_ASSERT_EQUAL_STRING(SettingsError::kSampleHz, r.validation.messageFor("sampleHz"));
  TEST_ASSERT_EQUAL_STRING("teletrack", s.deviceName);  // rolled back
  TEST_ASSERT_EQUAL_UINT8(10, s.sampleHz);
}

static void test_apply_rejects_an_oversized_device_name_rather_than_truncating() {
  Settings s = Settings::defaults();
  // 40 characters — must NOT be silently clipped to 31.
  const char* body =
      "{\"deviceName\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Invalid);
  TEST_ASSERT_EQUAL_STRING(SettingsError::kDeviceName, r.validation.messageFor("deviceName"));
  TEST_ASSERT_EQUAL_STRING("teletrack", s.deviceName);
}

static void test_apply_rejects_wrong_field_types() {
  Settings s = Settings::defaults();
  const char* body = "{\"deviceName\":42,\"sampleHz\":\"ten\"}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Invalid);
  TEST_ASSERT_EQUAL_STRING(SettingsError::kDeviceName, r.validation.messageFor("deviceName"));
  TEST_ASSERT_EQUAL_STRING(SettingsError::kSampleHz, r.validation.messageFor("sampleHz"));
}

static void test_apply_empty_object_is_a_no_op_success() {
  Settings s = Settings::defaults();
  const char* body = "{}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Ok);
  TEST_ASSERT_EQUAL_UINT8(10, s.sampleHz);
}

static void test_round_trip_through_json() {
  Settings original = Settings::defaults();
  original.sampleHz = 25;
  snprintf(original.deviceName, sizeof(original.deviceName), "%s", "round-trip");

  char buf[ConfigApi::kJsonBufferSize];
  const size_t n = ConfigApi::toJson(original, buf, sizeof(buf));

  Settings restored = Settings::defaults();
  const ConfigApi::ParseResult r = ConfigApi::applyJson(buf, n, restored);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Ok);
  TEST_ASSERT_EQUAL_UINT8(25, restored.sampleHz);
  TEST_ASSERT_EQUAL_STRING("round-trip", restored.deviceName);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_to_json_shape);
  RUN_TEST(test_to_json_refuses_a_buffer_that_is_too_small);
  RUN_TEST(test_ok_json);
  RUN_TEST(test_storage_error_json);
  RUN_TEST(test_errors_json_shape);
  RUN_TEST(test_apply_valid_payload_updates_settings);
  RUN_TEST(test_apply_partial_payload_leaves_other_fields_alone);
  RUN_TEST(test_apply_ignores_schema_version_from_the_client);
  RUN_TEST(test_apply_malformed_json_is_bad_json);
  RUN_TEST(test_apply_non_object_json_is_bad_json);
  RUN_TEST(test_apply_writes_nothing_when_any_field_is_invalid);
  RUN_TEST(test_apply_rejects_an_oversized_device_name_rather_than_truncating);
  RUN_TEST(test_apply_rejects_wrong_field_types);
  RUN_TEST(test_apply_empty_object_is_a_no_op_success);
  RUN_TEST(test_round_trip_through_json);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
pio test -e native -f native/test_config_api
```

Expected: FAIL — `fatal error: config/internal/ConfigApi.h: No such file or directory`.

- [ ] **Step 3: Write `src/config/internal/ConfigApi.h`**

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config/Settings.h"

// Wire format for the settings API. Pure: no HTTP, no storage. Every *ToJson
// function returns the number of characters written, or 0 when the output
// buffer is too small — it never emits a truncated document.
namespace ConfigApi {

// Largest POST body accepted. Anything longer is rejected before buffering.
inline constexpr size_t kMaxBodyBytes = 1024;
// Large enough for any document this module produces.
inline constexpr size_t kJsonBufferSize = 512;

enum class ParseStatus : uint8_t {
  Ok,       // body parsed, validated, and applied to inOut
  BadJson,  // not parseable, or not a JSON object
  Invalid,  // parsed but failed validation; inOut untouched
};

struct ParseResult {
  ParseStatus status = ParseStatus::BadJson;
  ValidationResult validation;
};

size_t toJson(const Settings& s, char* out, size_t outSize);

// Applies the fields present in `body` on top of `inOut`. `inOut` is modified
// only when the returned status is Ok — a payload with one bad field changes
// nothing. `schemaVersion` in the body is ignored.
ParseResult applyJson(const char* body, size_t len, Settings& inOut);

size_t okToJson(char* out, size_t outSize);
size_t errorsToJson(const ValidationResult& v, char* out, size_t outSize);
size_t storageErrorToJson(char* out, size_t outSize);

}  // namespace ConfigApi
```

- [ ] **Step 4: Write `src/config/internal/ConfigApi.cpp`**

```cpp
#include "config/internal/ConfigApi.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

namespace ConfigApi {
namespace {

size_t serializeIfItFits(const JsonDocument& doc, char* out, size_t outSize) {
  if (out == nullptr || measureJson(doc) + 1 > outSize) {
    return 0;
  }
  return serializeJson(doc, out, outSize);
}

}  // namespace

size_t toJson(const Settings& s, char* out, size_t outSize) {
  JsonDocument doc;
  doc["schemaVersion"] = s.schemaVersion;
  doc["deviceName"] = s.deviceName;
  doc["sampleHz"] = s.sampleHz;
  return serializeIfItFits(doc, out, outSize);
}

size_t okToJson(char* out, size_t outSize) {
  if (out == nullptr) {
    return 0;
  }
  const int n = snprintf(out, outSize, "{\"ok\":true}");
  if (n < 0 || static_cast<size_t>(n) >= outSize) {
    return 0;
  }
  return static_cast<size_t>(n);
}

size_t errorsToJson(const ValidationResult& v, char* out, size_t outSize) {
  JsonDocument doc;
  doc["ok"] = false;
  JsonObject errors = doc["errors"].to<JsonObject>();
  for (size_t i = 0; i < v.count; ++i) {
    errors[v.errors[i].field] = v.errors[i].message;
  }
  return serializeIfItFits(doc, out, outSize);
}

size_t storageErrorToJson(char* out, size_t outSize) {
  ValidationResult v;
  v.add("_", SettingsError::kStorage);
  return errorsToJson(v, out, outSize);
}

ParseResult applyJson(const char* body, size_t len, Settings& inOut) {
  ParseResult result;
  result.status = ParseStatus::BadJson;

  if (body == nullptr || len == 0 || len > kMaxBodyBytes) {
    return result;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) {
    return result;
  }
  if (!doc.is<JsonObject>()) {
    return result;
  }
  JsonObject obj = doc.as<JsonObject>();

  // Build a candidate. inOut is only overwritten if everything checks out.
  Settings candidate = inOut;
  candidate.schemaVersion = Settings::kSchemaVersion;  // never client-supplied

  ValidationResult typeErrors;

  if (!obj["deviceName"].isNull()) {
    const char* name = obj["deviceName"].is<const char*>()
                           ? obj["deviceName"].as<const char*>()
                           : nullptr;
    // An over-long name is rejected, never silently clipped to the buffer size.
    if (name == nullptr || strlen(name) >= Settings::kDeviceNameSize) {
      typeErrors.add("deviceName", SettingsError::kDeviceName);
    } else {
      memset(candidate.deviceName, 0, sizeof(candidate.deviceName));
      memcpy(candidate.deviceName, name, strlen(name));
    }
  }

  if (!obj["sampleHz"].isNull()) {
    if (!obj["sampleHz"].is<unsigned int>()) {
      typeErrors.add("sampleHz", SettingsError::kSampleHz);
    } else {
      const unsigned int hz = obj["sampleHz"].as<unsigned int>();
      if (hz > 255u || !Settings::isValidSampleHz(static_cast<uint8_t>(hz))) {
        typeErrors.add("sampleHz", SettingsError::kSampleHz);
      } else {
        candidate.sampleHz = static_cast<uint8_t>(hz);
      }
    }
  }

  if (!typeErrors.ok()) {
    result.status = ParseStatus::Invalid;
    result.validation = typeErrors;
    return result;
  }

  const ValidationResult validation = candidate.validate();
  if (!validation.ok()) {
    result.status = ParseStatus::Invalid;
    result.validation = validation;
    return result;
  }

  inOut = candidate;
  result.status = ParseStatus::Ok;
  return result;
}

}  // namespace ConfigApi
```

- [ ] **Step 5: Add `ConfigApi.cpp` to the host build**

```ini
build_src_filter =
    -<*>
    +<core/Format.cpp>
    +<config/Settings.cpp>
    +<config/internal/MemoryStore.cpp>
    +<config/internal/ConfigApi.cpp>
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
pio test -e native -f native/test_config_api
```

Expected: PASS — `15 Tests 0 Failures 0 Ignored`.

- [ ] **Step 7: Commit**

```bash
git add platformio.ini src/config/internal/ConfigApi.h src/config/internal/ConfigApi.cpp test/native/test_config_api/test_config_api.cpp
git commit -m "Add ConfigApi JSON serialisation and validation-aware parsing"
```

---

### Task 5: `LogRing` — the console buffer

Holds the last N log lines. Deliberately **not** thread-safe: `Display` owns the lock,
`LogRing` owns the data. That split is what keeps it host-testable.

**Files:**
- Create: `src/core/LogRing.h`, `src/core/LogRing.cpp`
- Modify: `platformio.ini` (`build_src_filter`)
- Test: `test/native/test_log_ring/test_log_ring.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `LogRing::kRows == 20`, `LogRing::kCols == 53`, `LogRing::kLineSize == 54`
  - `void LogRing::append(const char* line)`
  - `const char* LogRing::row(size_t index) const` — index 0 is the oldest visible row
  - `uint32_t LogRing::revision() const`

- [ ] **Step 1: Write the failing test**

Create `test/native/test_log_ring/test_log_ring.cpp`:

```cpp
#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "core/LogRing.h"

void setUp() {}
void tearDown() {}

static void test_fresh_ring_is_all_empty_rows() {
  LogRing ring;
  for (size_t i = 0; i < LogRing::kRows; ++i) {
    TEST_ASSERT_EQUAL_STRING("", ring.row(i));
  }
  TEST_ASSERT_EQUAL_UINT32(0u, ring.revision());
}

static void test_appends_fill_from_the_top() {
  LogRing ring;
  ring.append("one");
  ring.append("two");
  ring.append("three");

  TEST_ASSERT_EQUAL_STRING("one", ring.row(0));
  TEST_ASSERT_EQUAL_STRING("two", ring.row(1));
  TEST_ASSERT_EQUAL_STRING("three", ring.row(2));
  TEST_ASSERT_EQUAL_STRING("", ring.row(3));
}

static void test_exactly_full_keeps_every_line() {
  LogRing ring;
  char line[16];
  for (size_t i = 0; i < LogRing::kRows; ++i) {
    snprintf(line, sizeof(line), "line%u", (unsigned)i);
    ring.append(line);
  }
  TEST_ASSERT_EQUAL_STRING("line0", ring.row(0));
  snprintf(line, sizeof(line), "line%u", (unsigned)(LogRing::kRows - 1));
  TEST_ASSERT_EQUAL_STRING(line, ring.row(LogRing::kRows - 1));
}

static void test_wrap_around_drops_the_oldest_and_keeps_order() {
  LogRing ring;
  char line[16];
  // One more than capacity: line0 falls off the top.
  for (size_t i = 0; i <= LogRing::kRows; ++i) {
    snprintf(line, sizeof(line), "line%u", (unsigned)i);
    ring.append(line);
  }
  TEST_ASSERT_EQUAL_STRING("line1", ring.row(0));
  snprintf(line, sizeof(line), "line%u", (unsigned)LogRing::kRows);
  TEST_ASSERT_EQUAL_STRING(line, ring.row(LogRing::kRows - 1));

  // And the rows in between are still in order.
  for (size_t i = 0; i < LogRing::kRows; ++i) {
    snprintf(line, sizeof(line), "line%u", (unsigned)(i + 1));
    TEST_ASSERT_EQUAL_STRING(line, ring.row(i));
  }
}

static void test_wrap_around_many_times() {
  LogRing ring;
  char line[16];
  for (size_t i = 0; i < LogRing::kRows * 7 + 3; ++i) {
    snprintf(line, sizeof(line), "line%u", (unsigned)i);
    ring.append(line);
  }
  const size_t newest = LogRing::kRows * 7 + 2;
  snprintf(line, sizeof(line), "line%u", (unsigned)newest);
  TEST_ASSERT_EQUAL_STRING(line, ring.row(LogRing::kRows - 1));
  snprintf(line, sizeof(line), "line%u", (unsigned)(newest - LogRing::kRows + 1));
  TEST_ASSERT_EQUAL_STRING(line, ring.row(0));
}

static void test_line_of_exactly_kcols_is_not_truncated() {
  LogRing ring;
  char line[LogRing::kLineSize];
  memset(line, 'x', LogRing::kCols);
  line[LogRing::kCols] = '\0';

  ring.append(line);
  TEST_ASSERT_EQUAL_UINT((unsigned)LogRing::kCols, (unsigned)strlen(ring.row(0)));
  TEST_ASSERT_EQUAL_CHAR('x', ring.row(0)[LogRing::kCols - 1]);
}

static void test_over_long_line_is_truncated_with_a_tilde() {
  LogRing ring;
  char line[LogRing::kCols + 20];
  memset(line, 'x', sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  ring.append(line);
  TEST_ASSERT_EQUAL_UINT((unsigned)LogRing::kCols, (unsigned)strlen(ring.row(0)));
  TEST_ASSERT_EQUAL_CHAR('~', ring.row(0)[LogRing::kCols - 1]);
}

static void test_null_line_appends_an_empty_row() {
  LogRing ring;
  ring.append(nullptr);
  TEST_ASSERT_EQUAL_STRING("", ring.row(0));
  TEST_ASSERT_EQUAL_UINT32(1u, ring.revision());
}

static void test_revision_advances_only_on_append() {
  LogRing ring;
  TEST_ASSERT_EQUAL_UINT32(0u, ring.revision());
  ring.append("a");
  TEST_ASSERT_EQUAL_UINT32(1u, ring.revision());
  ring.append("b");
  TEST_ASSERT_EQUAL_UINT32(2u, ring.revision());

  (void)ring.row(0);
  (void)ring.row(LogRing::kRows - 1);
  TEST_ASSERT_EQUAL_UINT32(2u, ring.revision());
}

static void test_row_out_of_range_is_empty() {
  LogRing ring;
  ring.append("a");
  TEST_ASSERT_EQUAL_STRING("", ring.row(LogRing::kRows));
  TEST_ASSERT_EQUAL_STRING("", ring.row(9999));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_ring_is_all_empty_rows);
  RUN_TEST(test_appends_fill_from_the_top);
  RUN_TEST(test_exactly_full_keeps_every_line);
  RUN_TEST(test_wrap_around_drops_the_oldest_and_keeps_order);
  RUN_TEST(test_wrap_around_many_times);
  RUN_TEST(test_line_of_exactly_kcols_is_not_truncated);
  RUN_TEST(test_over_long_line_is_truncated_with_a_tilde);
  RUN_TEST(test_null_line_appends_an_empty_row);
  RUN_TEST(test_revision_advances_only_on_append);
  RUN_TEST(test_row_out_of_range_is_empty);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
pio test -e native -f native/test_log_ring
```

Expected: FAIL — `fatal error: core/LogRing.h: No such file or directory`.

- [ ] **Step 3: Write `src/core/LogRing.h`**

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

// Fixed-size scrolling console buffer for the on-screen log.
//
// Not thread-safe on purpose: Display owns the critical section, LogRing owns
// the data. Keeping the locking out of here is what lets it be tested on the
// host with no FreeRTOS.
class LogRing {
 public:
  static constexpr size_t kRows = 20;
  static constexpr size_t kCols = 53;
  static constexpr size_t kLineSize = kCols + 1;

  // Appends a line, dropping the oldest when full. Lines longer than kCols are
  // truncated with a trailing '~'. A null line appends an empty row.
  void append(const char* line);

  // Index 0 is the oldest visible row, kRows-1 the newest. Rows never written,
  // and out-of-range indices, read as "".
  const char* row(size_t index) const;

  // Monotonic; bumped by append(). Display redraws when this differs from
  // the revision it last drew.
  uint32_t revision() const;

 private:
  char lines_[kRows][kLineSize] = {};
  size_t count_ = 0;  // rows written so far, saturating at kRows
  size_t head_ = 0;   // index into lines_ of the oldest row, once full
  uint32_t revision_ = 0;
};
```

- [ ] **Step 4: Write `src/core/LogRing.cpp`**

```cpp
#include "core/LogRing.h"

#include <string.h>

void LogRing::append(const char* line) {
  const char* src = (line != nullptr) ? line : "";

  size_t slot;
  if (count_ < kRows) {
    slot = count_;
    ++count_;
  } else {
    slot = head_;
    head_ = (head_ + 1) % kRows;
  }

  const size_t len = strlen(src);
  if (len > kCols) {
    memcpy(lines_[slot], src, kCols - 1);
    lines_[slot][kCols - 1] = '~';
    lines_[slot][kCols] = '\0';
  } else {
    memcpy(lines_[slot], src, len);
    lines_[slot][len] = '\0';
  }

  ++revision_;
}

const char* LogRing::row(size_t index) const {
  if (index >= kRows) {
    return "";
  }
  if (count_ < kRows) {
    return (index < count_) ? lines_[index] : "";
  }
  return lines_[(head_ + index) % kRows];
}

uint32_t LogRing::revision() const { return revision_; }
```

- [ ] **Step 5: Add `LogRing.cpp` to the host build**

```ini
build_src_filter =
    -<*>
    +<core/Format.cpp>
    +<config/Settings.cpp>
    +<config/internal/MemoryStore.cpp>
    +<config/internal/ConfigApi.cpp>
    +<core/LogRing.cpp>
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
pio test -e native -f native/test_log_ring
```

Expected: PASS — `10 Tests 0 Failures 0 Ignored`.

- [ ] **Step 7: Commit**

```bash
git add platformio.ini src/core/LogRing.h src/core/LogRing.cpp test/native/test_log_ring/test_log_ring.cpp
git commit -m "Add LogRing console buffer"
```

---

### Task 6: `Log` — serial and on-screen logging

One module owns logging: it formats a line once, prints it to serial, and appends it
to the on-screen console buffer.

**Files:**
- Create: `src/core/Log.h`, `src/core/Log.cpp`

**Interfaces:**
- Consumes: `Format`, `LogLevel` (Task 1), `LogRing` (Task 5).
- Produces:
  - `void Log::begin(unsigned long baud)`
  - `void Log::info/warn/error(const char* tag, const char* fmt, ...)`
  - `void Log::snapshot(LogRing& dest)` — copies the console under the lock
  - `uint32_t Log::revision()`
  - `Log::kMaxMessageBytes == 160`

**Why there is no test for this task.** `Log` is `Serial`, `millis()` and a FreeRTOS
mutex — all Arduino, none of it host-buildable. Its only logic is "format once, write
to two places", and the half that has logic, `LogRing`, is already host-tested in Task 5.
A host test here would need three fakes to assert nothing. Do **not** add
`core/Log.cpp` to `[env:native]`'s `build_src_filter`.

**Concurrency.** `Log::info` is called from the AsyncTCP task (HTTP handlers) and from
`loop()`. The format buffer is a stack local, so concurrent callers never share it; only
the ring append takes the mutex. `Display` copies the ring out under that same mutex and
draws from its own copy, so drawing never holds the lock.

- [ ] **Step 1: Write `src/core/Log.h`**

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/LogLevel.h"
#include "core/LogRing.h"

// Logging for the whole firmware. Formats a line once, prints it to serial,
// and appends it to the on-screen console.
//
// Callable from any task: HTTP handlers run on the AsyncTCP task, everything
// else on loop().
namespace Log {

inline constexpr size_t kMaxMessageBytes = 160;

void begin(unsigned long baud);

void info(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void warn(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void error(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Copies the console into `dest` under the lock. Call from loop() only.
void snapshot(LogRing& dest);

// Bumped on every line. Display redraws when it differs from what it drew.
uint32_t revision();

}  // namespace Log
```

- [ ] **Step 2: Write `src/core/Log.cpp`**

```cpp
#include "core/Log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "core/Format.h"

namespace Log {
namespace {

LogRing g_ring;
SemaphoreHandle_t g_mutex = nullptr;

void emit(LogLevel level, const char* tag, const char* fmt, va_list args) {
  // Stack-local: concurrent callers never share these buffers, so the only
  // thing needing a lock is the ring append below.
  char message[kMaxMessageBytes];
  vsnprintf(message, sizeof(message), fmt, args);

  // Wider than the screen on purpose: serial gets the full line, and
  // LogRing::append truncates its own copy to the console width.
  char line[240];
  // TODO(phase2): millis() is uptime and resets every boot. Replace with
  // GPS-derived wall clock once the M10N module exists. See "Follow-ups".
  Format::logLine(millis(), level, tag, message, line, sizeof(line));

  Serial.println(line);

  if (g_mutex != nullptr && xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    g_ring.append(line);
    xSemaphoreGive(g_mutex);
  }
}

}  // namespace

void begin(unsigned long baud) {
  g_mutex = xSemaphoreCreateMutex();
  Serial.begin(baud);
}

void info(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  emit(LogLevel::Info, tag, fmt, args);
  va_end(args);
}

void warn(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  emit(LogLevel::Warn, tag, fmt, args);
  va_end(args);
}

void error(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  emit(LogLevel::Error, tag, fmt, args);
  va_end(args);
}

void snapshot(LogRing& dest) {
  if (g_mutex != nullptr && xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    dest = g_ring;
    xSemaphoreGive(g_mutex);
  }
}

// A 32-bit aligned read is atomic on this core, so this needs no lock. A caller
// racing an append just redraws on the next frame.
uint32_t revision() { return g_ring.revision(); }

}  // namespace Log
```

- [ ] **Step 3: Verify it compiles**

```bash
pio run -e esp
```

Expected: `SUCCESS`.

- [ ] **Step 4: Confirm the host suite is unaffected**

```bash
pio test -e native
```

Expected: still passing, with no new suite. `core/Log.cpp` must not have leaked into
`build_src_filter`.

- [ ] **Step 5: Commit**

```bash
git add src/core/Log.h src/core/Log.cpp
git commit -m "Add serial and on-screen logging"
```

---

### Task 7: `NvsStore` — persistence on the device

The first task with code that only runs on hardware, so it comes with an on-device test
rather than a host test.

**Files:**
- Create: `src/config/internal/NvsStore.h`, `src/config/internal/NvsStore.cpp`
- Modify: `src/main.cpp` (replace the template with a guarded stub)
- Modify: `platformio.ini` (`test_build_src` in `[env:esp]`)
- Test: `test/embedded/test_nvs/test_nvs.cpp`

**Interfaces:**
- Consumes: `SettingsStore`, `Settings` from Tasks 2–3.
- Produces:
  - `class NvsStore : public SettingsStore` with `bool begin()`, `void end()`
  - `NvsStore::kNamespace == "teletrack"`

Keys are `schema`, `name`, `hz` — NVS keys are capped at 15 characters, so the field
names are not reused verbatim.

- [ ] **Step 1: Guard `src/main.cpp` so it does not collide with test binaries**

PlatformIO defines `PIO_UNIT_TESTING` when building a test. Without this guard, the
`setup()`/`loop()` in `main.cpp` and the ones in the test file are duplicate symbols.
Replace the whole file:

```cpp
#ifndef PIO_UNIT_TESTING

#include <Arduino.h>

void setup() {}

void loop() {}

#endif  // PIO_UNIT_TESTING
```

- [ ] **Step 2: Let the device test environment compile `src/`**

In `platformio.ini`, `[env:esp]`, add:

```ini
test_build_src = yes
```

- [ ] **Step 3: Write the failing test**

Create `test/embedded/test_nvs/test_nvs.cpp`:

```cpp
#include <Arduino.h>
#include <unity.h>

#include "config/internal/NvsStore.h"

void setUp() {}
void tearDown() {}

static void test_begin_succeeds() {
  NvsStore store;
  TEST_ASSERT_TRUE(store.begin());
  TEST_ASSERT_TRUE(store.available());
  store.end();
}

static void test_operations_fail_before_begin() {
  NvsStore store;
  Settings s = Settings::defaults();
  TEST_ASSERT_FALSE(store.available());
  TEST_ASSERT_FALSE(store.save(s));
  TEST_ASSERT_FALSE(store.load(s));
}

static void test_save_then_load_round_trips() {
  NvsStore store;
  TEST_ASSERT_TRUE(store.begin());

  Settings in = Settings::defaults();
  in.sampleHz = 25;
  snprintf(in.deviceName, sizeof(in.deviceName), "%s", "nvs-round-trip");
  TEST_ASSERT_TRUE(store.save(in));

  Settings out;
  TEST_ASSERT_TRUE(store.load(out));
  TEST_ASSERT_EQUAL_UINT8(25, out.sampleHz);
  TEST_ASSERT_EQUAL_STRING("nvs-round-trip", out.deviceName);
  TEST_ASSERT_EQUAL_UINT8(Settings::kSchemaVersion, out.schemaVersion);
  store.end();
}

static void test_load_survives_a_close_and_reopen() {
  {
    NvsStore store;
    TEST_ASSERT_TRUE(store.begin());
    Settings in = Settings::defaults();
    in.sampleHz = 5;
    snprintf(in.deviceName, sizeof(in.deviceName), "%s", "persisted");
    TEST_ASSERT_TRUE(store.save(in));
    store.end();
  }
  {
    NvsStore store;
    TEST_ASSERT_TRUE(store.begin());
    Settings out;
    TEST_ASSERT_TRUE(store.load(out));
    TEST_ASSERT_EQUAL_UINT8(5, out.sampleHz);
    TEST_ASSERT_EQUAL_STRING("persisted", out.deviceName);
    store.end();
  }
}

static void test_corrupt_stored_value_is_rejected() {
  NvsStore store;
  TEST_ASSERT_TRUE(store.begin());

  Settings good = Settings::defaults();
  TEST_ASSERT_TRUE(store.save(good));

  // Write a sample rate that is not one of the four allowed values, behind the
  // store's back, the way a firmware downgrade or a flash bit-flip would.
  Preferences raw;
  TEST_ASSERT_TRUE(raw.begin(NvsStore::kNamespace, false));
  raw.putUChar("hz", 77);
  raw.end();

  Settings out = Settings::defaults();
  out.sampleHz = 10;
  TEST_ASSERT_FALSE(store.load(out));   // refuses to hand back invalid settings
  TEST_ASSERT_EQUAL_UINT8(10, out.sampleHz);  // and leaves the caller's defaults
  store.end();
}

void setup() {
  delay(2000);  // let USB CDC enumerate before the first output
  UNITY_BEGIN();
  RUN_TEST(test_begin_succeeds);
  RUN_TEST(test_operations_fail_before_begin);
  RUN_TEST(test_save_then_load_round_trips);
  RUN_TEST(test_load_survives_a_close_and_reopen);
  RUN_TEST(test_corrupt_stored_value_is_rejected);
  UNITY_END();
}

void loop() {}
```

- [ ] **Step 4: Run the test to verify it fails**

With the board connected over USB:

```bash
pio test -e esp -f embedded/test_nvs
```

Expected: FAIL — `fatal error: config/internal/NvsStore.h: No such file or directory`.

- [ ] **Step 5: Write `src/config/internal/NvsStore.h`**

```cpp
#pragma once

#include <Preferences.h>

#include "config/SettingsStore.h"

// SettingsStore backed by ESP32 NVS via Preferences.
//
// NVS keys are limited to 15 characters and are not the wire field names.
class NvsStore : public SettingsStore {
 public:
  static constexpr const char* kNamespace = "teletrack";
  static constexpr const char* kKeySchema = "schema";
  static constexpr const char* kKeyDeviceName = "name";
  static constexpr const char* kKeySampleHz = "hz";

  // Opens the namespace. Returns false when NVS is unusable — the caller then
  // runs from RAM defaults and reports persistDegraded.
  bool begin();
  void end();

  bool load(Settings& out) override;
  bool save(const Settings& s) override;
  bool available() const override;

 private:
  Preferences prefs_;
  bool available_ = false;
};
```

- [ ] **Step 6: Write `src/config/internal/NvsStore.cpp`**

```cpp
#include "config/internal/NvsStore.h"

bool NvsStore::begin() {
  available_ = prefs_.begin(kNamespace, /*readOnly=*/false);
  return available_;
}

void NvsStore::end() {
  if (available_) {
    prefs_.end();
    available_ = false;
  }
}

bool NvsStore::available() const { return available_; }

bool NvsStore::load(Settings& out) {
  if (!available_) {
    return false;
  }
  // Nothing has ever been written: let the caller keep its defaults.
  if (!prefs_.isKey(kKeySampleHz) && !prefs_.isKey(kKeyDeviceName)) {
    return false;
  }

  Settings loaded = Settings::defaults();
  loaded.schemaVersion = prefs_.getUChar(kKeySchema, Settings::kSchemaVersion);
  if (prefs_.isKey(kKeyDeviceName)) {
    prefs_.getString(kKeyDeviceName, loaded.deviceName, sizeof(loaded.deviceName));
  }
  loaded.sampleHz = prefs_.getUChar(kKeySampleHz, loaded.sampleHz);

  // Refuse to hand back settings that would not have been accepted over HTTP.
  // A downgrade or a flash bit-flip lands here.
  if (!loaded.validate().ok()) {
    return false;
  }

  out = loaded;
  return true;
}

bool NvsStore::save(const Settings& s) {
  if (!available_) {
    return false;
  }
  if (prefs_.putUChar(kKeySchema, Settings::kSchemaVersion) == 0) {
    return false;
  }
  if (prefs_.putString(kKeyDeviceName, s.deviceName) == 0) {
    return false;
  }
  if (prefs_.putUChar(kKeySampleHz, s.sampleHz) == 0) {
    return false;
  }
  return true;
}
```

- [ ] **Step 7: Run the test to verify it passes**

```bash
pio test -e esp -f embedded/test_nvs
```

Expected: PASS — `5 Tests 0 Failures 0 Ignored`.

- [ ] **Step 8: Confirm the host suite is unaffected**

```bash
pio test -e native
```

Expected: all five host suites still pass. `NvsStore` must not have leaked into the
native build.

- [ ] **Step 9: Commit**

```bash
git add platformio.ini src/main.cpp src/config/internal/NvsStore.h src/config/internal/NvsStore.cpp test/embedded/test_nvs/test_nvs.cpp
git commit -m "Add NVS-backed settings store with on-device tests"
```

---

### Task 8: `ConfigService` — save orchestration

The decision logic behind `POST /api/config`: parse, validate, persist, roll back, and
choose the HTTP status. Pulling it out of `WebUi` is what makes the rollback and
degraded-mode paths testable — they are the two behaviours most likely to be wrong and
least likely to be noticed by hand.

**Files:**
- Modify: `src/config/Settings.h` (two new error constants)
- Create: `src/config/internal/ConfigService.h`, `src/config/internal/ConfigService.cpp`
- Modify: `platformio.ini` (`build_src_filter`)
- Test: `test/native/test_config_service/test_config_service.cpp`

**Interfaces:**
- Consumes: `ConfigApi` (Task 4), `SettingsStore`/`MemoryStore` (Task 3), `Settings` (Task 2).
- Produces:
  - `struct ConfigService::SaveOutcome { uint16_t httpStatus; size_t bodyLength; }`
  - `SaveOutcome ConfigService::save(const char* requestBody, size_t len, Settings& settings, SettingsStore& store, char* out, size_t outSize)`
  - `SettingsError::kBadJson`, `SettingsError::kTooLarge`

**Spec note:** §6 of the spec defines the `400`/`413`/`500` responses but not the message
text for a malformed or oversized body. This task fixes those strings as
`"malformed JSON body"` and `"body exceeds 1024 bytes"`, both keyed under `"_"` the way
the storage error is.

**Degraded mode:** when `store.available()` is false, a save applies to RAM and returns
`200`. Spec §12 says the device "stays fully usable; changes just don't survive reboot" —
returning `500` there would contradict that. The client learns about it from
`persistDegraded` in `/api/status`, not from a failed save.

- [ ] **Step 1: Write the failing test**

Create `test/native/test_config_service/test_config_service.cpp`:

```cpp
#include <unity.h>
#include <string.h>

#include "config/internal/ConfigService.h"
#include "config/internal/MemoryStore.h"

void setUp() {}
void tearDown() {}

static void test_valid_save_returns_200_and_persists() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{\"deviceName\":\"bike-one\",\"sampleHz\":25}";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(200, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", body);
  TEST_ASSERT_EQUAL_UINT8(25, settings.sampleHz);
  TEST_ASSERT_TRUE(store.hasStored());
  TEST_ASSERT_EQUAL_STRING("bike-one", store.stored().deviceName);
}

static void test_invalid_field_returns_400_and_persists_nothing() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{\"deviceName\":\"bike-one\",\"sampleHz\":7}";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(400, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"sampleHz\":\"must be 1, 5, 10 or 25\"}}", body);
  TEST_ASSERT_EQUAL_STRING("teletrack", settings.deviceName);
  TEST_ASSERT_EQUAL_UINT8(10, settings.sampleHz);
  TEST_ASSERT_FALSE(store.hasStored());
}

static void test_malformed_json_returns_400() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{not json";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(400, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"_\":\"malformed JSON body\"}}", body);
  TEST_ASSERT_FALSE(store.hasStored());
}

static void test_oversized_body_returns_413_without_parsing() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  char body[ConfigApi::kJsonBufferSize];

  const ConfigService::SaveOutcome out = ConfigService::save(
      "{}", ConfigApi::kMaxBodyBytes + 1, settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(413, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"_\":\"body exceeds 1024 bytes\"}}", body);
  TEST_ASSERT_FALSE(store.hasStored());
}

static void test_storage_write_failure_returns_500_and_rolls_back() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  store.failNextSave();
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{\"deviceName\":\"bike-one\",\"sampleHz\":25}";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(500, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"_\":\"could not write to storage\"}}", body);
  // The in-memory settings must not keep a change that was never persisted.
  TEST_ASSERT_EQUAL_STRING("teletrack", settings.deviceName);
  TEST_ASSERT_EQUAL_UINT8(10, settings.sampleHz);
}

static void test_degraded_store_still_applies_to_ram_and_returns_200() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  store.setAvailable(false);
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{\"sampleHz\":5}";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(200, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", body);
  TEST_ASSERT_EQUAL_UINT8(5, settings.sampleHz);  // live now
  TEST_ASSERT_FALSE(store.hasStored());           // but not persisted
}

static void test_body_length_matches_what_was_written() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{\"sampleHz\":5}";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT((unsigned)strlen(body), (unsigned)out.bodyLength);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_save_returns_200_and_persists);
  RUN_TEST(test_invalid_field_returns_400_and_persists_nothing);
  RUN_TEST(test_malformed_json_returns_400);
  RUN_TEST(test_oversized_body_returns_413_without_parsing);
  RUN_TEST(test_storage_write_failure_returns_500_and_rolls_back);
  RUN_TEST(test_degraded_store_still_applies_to_ram_and_returns_200);
  RUN_TEST(test_body_length_matches_what_was_written);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
pio test -e native -f native/test_config_service
```

Expected: FAIL — `fatal error: config/internal/ConfigService.h: No such file or directory`.

- [ ] **Step 3: Add the two new error constants to `src/config/Settings.h`**

Inside `namespace SettingsError`, after `kStorage`:

```cpp
inline constexpr const char* kBadJson = "malformed JSON body";
inline constexpr const char* kTooLarge = "body exceeds 1024 bytes";
```

- [ ] **Step 4: Write `src/config/internal/ConfigService.h`**

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config/Settings.h"
#include "config/SettingsStore.h"
#include "config/internal/ConfigApi.h"

// Decision logic for POST /api/config. Pure orchestration: it takes a request
// body and a store, and returns the HTTP status plus the response body. No
// network types appear here, which is what makes the rollback and degraded
// paths testable on the host.
namespace ConfigService {

struct SaveOutcome {
  uint16_t httpStatus = 500;
  size_t bodyLength = 0;
};

// On success `settings` holds the new values. On any failure `settings` is
// exactly what it was on entry.
SaveOutcome save(const char* requestBody, size_t len, Settings& settings,
                 SettingsStore& store, char* out, size_t outSize);

}  // namespace ConfigService
```

- [ ] **Step 5: Write `src/config/internal/ConfigService.cpp`**

```cpp
#include "config/internal/ConfigService.h"

namespace ConfigService {
namespace {

SaveOutcome singleError(const char* message, char* out, size_t outSize,
                        uint16_t status) {
  ValidationResult v;
  v.add("_", message);
  SaveOutcome outcome;
  outcome.httpStatus = status;
  outcome.bodyLength = ConfigApi::errorsToJson(v, out, outSize);
  return outcome;
}

}  // namespace

SaveOutcome save(const char* requestBody, size_t len, Settings& settings,
                 SettingsStore& store, char* out, size_t outSize) {
  if (len > ConfigApi::kMaxBodyBytes) {
    return singleError(SettingsError::kTooLarge, out, outSize, 413);
  }

  const Settings previous = settings;
  const ConfigApi::ParseResult parsed =
      ConfigApi::applyJson(requestBody, len, settings);

  if (parsed.status == ConfigApi::ParseStatus::BadJson) {
    return singleError(SettingsError::kBadJson, out, outSize, 400);
  }
  if (parsed.status == ConfigApi::ParseStatus::Invalid) {
    SaveOutcome outcome;
    outcome.httpStatus = 400;
    outcome.bodyLength = ConfigApi::errorsToJson(parsed.validation, out, outSize);
    return outcome;
  }

  // Degraded storage is not a client error: the change takes effect now and the
  // client learns it will not survive a reboot from persistDegraded in
  // /api/status. See spec §12.
  if (store.available() && !store.save(settings)) {
    settings = previous;
    SaveOutcome outcome;
    outcome.httpStatus = 500;
    outcome.bodyLength = ConfigApi::storageErrorToJson(out, outSize);
    return outcome;
  }

  SaveOutcome outcome;
  outcome.httpStatus = 200;
  outcome.bodyLength = ConfigApi::okToJson(out, outSize);
  return outcome;
}

}  // namespace ConfigService
```

- [ ] **Step 6: Add `ConfigService.cpp` to the host build**

```ini
build_src_filter =
    -<*>
    +<core/Format.cpp>
    +<core/Log.cpp>
    +<config/Settings.cpp>
    +<config/internal/MemoryStore.cpp>
    +<config/internal/ConfigApi.cpp>
    +<config/internal/ConfigService.cpp>
    +<core/LogRing.cpp>
```

- [ ] **Step 7: Run the test to verify it passes**

```bash
pio test -e native -f native/test_config_service
```

Expected: PASS — `7 Tests 0 Failures 0 Ignored`.

- [ ] **Step 8: Commit**

```bash
git add platformio.ini src/config/Settings.h src/config/internal/ConfigService.h src/config/internal/ConfigService.cpp test/native/test_config_service/test_config_service.cpp
git commit -m "Add ConfigService save orchestration with rollback and degraded handling"
```

---

### Task 9: AP, captive portal, and the `ConfigPortal` facade

**First flashable milestone.** After this task the device opens a network, a phone
connecting to it gets the "Sign in to network" sheet, and the serial log narrates it.
There is no web UI yet — every URL redirects.

**Files:**
- Create: `src/core/DeviceStatus.h`
- Create: `src/config/internal/ApManager.h`, `src/config/internal/ApManager.cpp`
- Create: `src/config/internal/CaptivePortal.h`, `src/config/internal/CaptivePortal.cpp`
- Create: `src/config/ConfigPortal.h`, `src/config/ConfigPortal.cpp`
- Modify: `src/config/internal/ConfigApi.h`, `src/config/internal/ConfigApi.cpp` (add `statusToJson`)
- Modify: `src/main.cpp`
- Test: `test/native/test_config_api/test_config_api.cpp` (add two cases)

**Interfaces:**
- Consumes: `Settings`, `SettingsStore`, `NvsStore`, `Log`.
- Produces:
  - `struct DeviceStatus { char ssid[33]; char ip[16]; uint8_t clients; uint32_t uptimeMs; uint32_t freeHeap; bool apUp; bool persistDegraded; }`
  - `class ApManager` — `bool begin(const char* ssid, uint8_t channel, uint8_t maxClients)`, `void tick(uint32_t nowMs)`, `bool up() const`, `uint8_t clients() const`, `const char* ssid() const`, `const char* ip() const`
  - `class CaptivePortal` — `bool begin(const IPAddress&)`, `void tick()`, `void registerRoutes(AsyncWebServer&)`
  - `class ConfigPortal` — `explicit ConfigPortal(Settings&)`, `bool begin()`, `void tick(uint32_t nowMs)`, `DeviceStatus status() const`, constants `kSsid`, `kChannel`, `kMaxClients`, `kHttpPort`
  - `size_t ConfigApi::statusToJson(const DeviceStatus&, char* out, size_t outSize)`

**`ConfigPortal` owns its `NvsStore`.** `main.cpp` must not include from `internal/`, so
the concrete store is constructed inside the facade rather than injected. Host tests do
not construct a `ConfigPortal` at all — they test `ConfigService` against `MemoryStore`,
which is where the store-dependent logic lives.

- [ ] **Step 1: Add the failing `statusToJson` tests**

Append these two tests to `test/native/test_config_api/test_config_api.cpp`, and add
their `RUN_TEST` lines to `main()`:

```cpp
static void test_status_json_shape() {
  DeviceStatus status;
  snprintf(status.ssid, sizeof(status.ssid), "%s", "teletrack");
  snprintf(status.ip, sizeof(status.ip), "%s", "192.168.4.1");
  status.clients = 1;
  status.uptimeMs = 134221;
  status.freeHeap = 186432;
  status.apUp = true;
  status.persistDegraded = false;

  char buf[ConfigApi::kJsonBufferSize];
  ConfigApi::statusToJson(status, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"ssid\":\"teletrack\",\"ip\":\"192.168.4.1\",\"clients\":1,"
      "\"uptimeMs\":134221,\"freeHeap\":186432,\"apUp\":true,"
      "\"persistDegraded\":false}",
      buf);
}

static void test_status_json_reports_degraded_storage() {
  DeviceStatus status;
  status.persistDegraded = true;

  char buf[ConfigApi::kJsonBufferSize];
  ConfigApi::statusToJson(status, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"persistDegraded\":true"));
}
```

Add `#include "core/DeviceStatus.h"` to the top of that test file.

- [ ] **Step 2: Run the test to verify it fails**

```bash
pio test -e native -f native/test_config_api
```

Expected: FAIL — `fatal error: core/DeviceStatus.h: No such file or directory`.

- [ ] **Step 3: Write `src/core/DeviceStatus.h`**

```cpp
#pragma once

#include <stdint.h>

// A snapshot of what the device is doing right now. Produced by the config
// module, consumed by the display and by GET /api/status. Plain data — copying
// it is how the status crosses a task boundary safely.
struct DeviceStatus {
  char ssid[33] = {};  // 32-character SSID limit + NUL
  char ip[16] = {};    // "255.255.255.255" + NUL
  uint8_t clients = 0;
  uint32_t uptimeMs = 0;
  uint32_t freeHeap = 0;
  bool apUp = false;
  bool persistDegraded = false;
};
```

- [ ] **Step 4: Add `statusToJson` to `ConfigApi`**

In `src/config/internal/ConfigApi.h`, add the include and the declaration:

```cpp
#include "core/DeviceStatus.h"
```

```cpp
size_t statusToJson(const DeviceStatus& status, char* out, size_t outSize);
```

In `src/config/internal/ConfigApi.cpp`, add the definition:

```cpp
size_t statusToJson(const DeviceStatus& status, char* out, size_t outSize) {
  JsonDocument doc;
  doc["ssid"] = status.ssid;
  doc["ip"] = status.ip;
  doc["clients"] = status.clients;
  doc["uptimeMs"] = status.uptimeMs;
  doc["freeHeap"] = status.freeHeap;
  doc["apUp"] = status.apUp;
  doc["persistDegraded"] = status.persistDegraded;
  return serializeIfItFits(doc, out, outSize);
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
pio test -e native -f native/test_config_api
```

Expected: PASS — `17 Tests 0 Failures 0 Ignored`.

- [ ] **Step 6: Write `src/config/internal/ApManager.h`**

```cpp
#pragma once

#include <stdint.h>

// SoftAP lifecycle and client-count polling. Logs transitions; holds no HTTP or
// DNS knowledge.
class ApManager {
 public:
  static constexpr uint32_t kPollIntervalMs = 1000;

  bool begin(const char* ssid, uint8_t channel, uint8_t maxClients);

  // Call from loop(). Polls the station count once per kPollIntervalMs and logs
  // any change.
  void tick(uint32_t nowMs);

  bool up() const;
  uint8_t clients() const;
  const char* ssid() const;
  const char* ip() const;

 private:
  char ssid_[33] = {};
  char ip_[16] = {};
  bool up_ = false;
  uint8_t clients_ = 0;
  uint32_t lastPollMs_ = 0;
};
```

- [ ] **Step 7: Write `src/config/internal/ApManager.cpp`**

```cpp
#include "config/internal/ApManager.h"

#include <WiFi.h>
#include <stdio.h>

#include "core/Log.h"

bool ApManager::begin(const char* ssid, uint8_t channel, uint8_t maxClients) {
  snprintf(ssid_, sizeof(ssid_), "%s", ssid != nullptr ? ssid : "teletrack");

  WiFi.mode(WIFI_AP);

  const IPAddress ip(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  if (!WiFi.softAPConfig(ip, gateway, subnet)) {
    Log::error("ap", "softAPConfig failed");
    up_ = false;
    return false;
  }

  // A null password is what makes this an open network.
  if (!WiFi.softAP(ssid_, nullptr, channel, /*ssid_hidden=*/0, maxClients)) {
    Log::error("ap", "softAP failed");
    up_ = false;
    return false;
  }

  // Built by hand rather than via IPAddress::toString(), which returns an
  // Arduino String.
  const IPAddress actual = WiFi.softAPIP();
  snprintf(ip_, sizeof(ip_), "%u.%u.%u.%u", actual[0], actual[1], actual[2], actual[3]);

  up_ = true;
  Log::info("ap", "up ssid=%s ch%u open", ssid_, (unsigned)channel);
  Log::info("ap", "ip %s", ip_);
  return true;
}

void ApManager::tick(uint32_t nowMs) {
  if (!up_) {
    return;
  }
  if (nowMs - lastPollMs_ < kPollIntervalMs) {
    return;
  }
  lastPollMs_ = nowMs;

  const uint8_t current = static_cast<uint8_t>(WiFi.softAPgetStationNum());
  if (current != clients_) {
    Log::info("ap", "clients %u -> %u", (unsigned)clients_, (unsigned)current);
    clients_ = current;
  }
}

bool ApManager::up() const { return up_; }
uint8_t ApManager::clients() const { return clients_; }
const char* ApManager::ssid() const { return ssid_; }
const char* ApManager::ip() const { return ip_; }
```

- [ ] **Step 8: Write `src/config/internal/CaptivePortal.h`**

```cpp
#pragma once

#include <DNSServer.h>
#include <IPAddress.h>

class AsyncWebServer;

// DNS hijack plus the OS connectivity-probe routes that make a phone raise its
// "Sign in to network" sheet.
class CaptivePortal {
 public:
  static constexpr uint16_t kDnsPort = 53;

  bool begin(const IPAddress& ip);

  // Call from loop(). Answers one pending DNS query.
  void tick();

  // Registers the probe routes and the catch-all. Call before server.begin().
  void registerRoutes(AsyncWebServer& server);

 private:
  DNSServer dns_;
  bool up_ = false;
};
```

- [ ] **Step 9: Write `src/config/internal/CaptivePortal.cpp`**

```cpp
#include "config/internal/CaptivePortal.h"

#include <ESPAsyncWebServer.h>

#include "core/Log.h"

namespace {

constexpr const char* kPortalUrl = "http://192.168.4.1/";

// Each OS probes a different URL to decide whether a network has internet.
// Answering all of them with a redirect — never a 204 — is what classifies the
// network as needing sign-in and pops the portal.
const char* const kProbePaths[] = {
    "/generate_204",             // Android
    "/gen_204",                  // Android
    "/hotspot-detect.html",      // iOS, macOS
    "/library/test/success.html", // iOS, macOS
    "/ncsi.txt",                 // Windows
    "/connecttest.txt",          // Windows
    "/redirect",                 // Windows
};

}  // namespace

bool CaptivePortal::begin(const IPAddress& ip) {
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  up_ = dns_.start(kDnsPort, "*", ip);
  if (up_) {
    Log::info("dns", "captive portal up on port %u", (unsigned)kDnsPort);
  } else {
    Log::error("dns", "failed to start on port %u", (unsigned)kDnsPort);
  }
  return up_;
}

void CaptivePortal::tick() {
  if (up_) {
    dns_.processNextRequest();
  }
}

void CaptivePortal::registerRoutes(AsyncWebServer& server) {
  for (const char* path : kProbePaths) {
    server.on(path, HTTP_GET, [](AsyncWebServerRequest* request) {
      request->redirect(kPortalUrl);
    });
  }
  server.onNotFound([](AsyncWebServerRequest* request) {
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
      return;
    }
    request->redirect(kPortalUrl);
  });
}
```

- [ ] **Step 10: Write `src/config/ConfigPortal.h`**

```cpp
#pragma once

#include <ESPAsyncWebServer.h>
#include <stdint.h>

#include "config/Settings.h"
#include "config/SettingsStore.h"
#include "config/internal/ApManager.h"
#include "config/internal/CaptivePortal.h"
#include "config/internal/NvsStore.h"

// The configuration module's whole public surface, besides Settings.
//
// Owns the store, the AP, the DNS hijack and the HTTP server. main.cpp only
// ever calls begin(), tick() and status().
class ConfigPortal {
 public:
  static constexpr const char* kSsid = "teletrack";
  static constexpr uint8_t kChannel = 1;
  static constexpr uint8_t kMaxClients = 4;
  static constexpr uint16_t kHttpPort = 80;

  explicit ConfigPortal(Settings& settings);

  // Loads persisted settings, starts the AP, DNS and HTTP server. Returns false
  // only when the AP itself could not start — a storage failure is survivable
  // and reported through status().persistDegraded.
  bool begin();

  // Call from loop().
  void tick(uint32_t nowMs);

  DeviceStatus status() const;

 private:
  Settings& settings_;
  NvsStore store_;
  ApManager ap_;
  CaptivePortal portal_;
  AsyncWebServer server_;
};
```

- [ ] **Step 11: Write `src/config/ConfigPortal.cpp`**

`WebUi` is added to this file in Task 10; for now the portal serves nothing but
redirects.

```cpp
#include "config/ConfigPortal.h"

#include <Arduino.h>

#include "core/Log.h"

ConfigPortal::ConfigPortal(Settings& settings)
    : settings_(settings), server_(kHttpPort) {}

bool ConfigPortal::begin() {
  if (!store_.begin()) {
    Log::error("nvs", "storage unavailable, changes will not persist");
  } else if (store_.load(settings_)) {
    Log::info("cfg", "loaded name=%s hz=%u", settings_.deviceName,
              (unsigned)settings_.sampleHz);
  } else {
    Log::info("cfg", "no stored settings, using defaults");
  }

  if (!ap_.begin(kSsid, kChannel, kMaxClients)) {
    return false;
  }

  portal_.begin(IPAddress(192, 168, 4, 1));
  portal_.registerRoutes(server_);
  server_.begin();
  Log::info("http", "listening on port %u", (unsigned)kHttpPort);
  return true;
}

void ConfigPortal::tick(uint32_t nowMs) {
  portal_.tick();
  ap_.tick(nowMs);
}

DeviceStatus ConfigPortal::status() const {
  DeviceStatus s;
  snprintf(s.ssid, sizeof(s.ssid), "%s", ap_.ssid());
  snprintf(s.ip, sizeof(s.ip), "%s", ap_.ip());
  s.clients = ap_.clients();
  s.uptimeMs = millis();
  s.freeHeap = ESP.getFreeHeap();
  s.apUp = ap_.up();
  s.persistDegraded = !store_.available();
  return s;
}
```

- [ ] **Step 12: Write `src/main.cpp`**

```cpp
#ifndef PIO_UNIT_TESTING

#include <Arduino.h>

#include "config/ConfigPortal.h"
#include "config/Settings.h"
#include "core/Log.h"

namespace {

// The one global. Everything else is reached through it by reference.
struct App {
  Settings settings = Settings::defaults();
  ConfigPortal portal{settings};
};

App app;

}  // namespace

void setup() {
  Log::begin(115200);
  Log::info("boot", "teletrack phase 1");

  if (!app.portal.begin()) {
    Log::error("boot", "config portal failed to start");
  }
}

void loop() {
  const uint32_t now = millis();
  app.portal.tick(now);
  delay(1);  // yield to the WiFi and AsyncTCP tasks
}

#endif  // PIO_UNIT_TESTING
```

- [ ] **Step 13: Build and flash**

```bash
pio run -e esp -t upload
pio device monitor
```

Expected serial output within a few seconds of boot:

```
00:00:00 [INF] boot: teletrack phase 1
00:00:00 [INF] cfg: no stored settings, using defaults
00:00:00 [INF] ap: up ssid=teletrack ch1 open
00:00:00 [INF] ap: ip 192.168.4.1
00:00:00 [INF] dns: captive portal up on port 53
00:00:00 [INF] http: listening on port 80
```

- [ ] **Step 14: Verify the captive portal on a real device**

Connect a phone to the open `teletrack` network. Expected:
1. The "Sign in to network" sheet appears on its own.
2. The page it shows is a redirect loop to `http://192.168.4.1/`, which 404s — there
   is no UI yet. That is correct for this task.
3. The serial log prints `[INF] ap: clients 0 -> 1` within a second of connecting.

- [ ] **Step 15: Commit**

```bash
git add src/core/DeviceStatus.h src/config/ConfigPortal.h src/config/ConfigPortal.cpp src/config/internal/ApManager.h src/config/internal/ApManager.cpp src/config/internal/CaptivePortal.h src/config/internal/CaptivePortal.cpp src/config/internal/ConfigApi.h src/config/internal/ConfigApi.cpp src/main.cpp test/native/test_config_api/test_config_api.cpp
git commit -m "Add SoftAP, captive portal and ConfigPortal facade"
```

---

### Task 10: Web UI asset pipeline and `WebUi`

**Second flashable milestone.** After this task the captive portal serves a real page and
settings can be read and written from a phone.

**Files:**
- Create: `tools/embed_ui.py`
- Create: `data/ui/index.html`
- Create: `src/config/internal/WebUi.h`, `src/config/internal/WebUi.cpp`
- Modify: `src/config/ConfigPortal.h`, `src/config/ConfigPortal.cpp`

**Interfaces:**
- Consumes: `ConfigApi`, `ConfigService`, `Settings`, `SettingsStore`, `ConfigPortal`.
- Produces:
  - `kUiIndexGz` (`const uint8_t[]`) and `kUiIndexGzLength` (`size_t`) in `ui_index.h`
  - `class WebUi` — `WebUi(Settings&, SettingsStore&, const ConfigPortal&)`, `void registerRoutes(AsyncWebServer&)`

**Scoped exception to the "no Arduino `String`" rule:** ESPAsyncWebServer takes
`const String&` for content types and redirect targets. Passing a string literal there
creates a short-lived `String` we cannot avoid. The rule still holds for everything we
own — no `String` members, no `String` return values, no `String` built from settings data.

- [ ] **Step 1: Write `tools/embed_ui.py`**

```python
"""PlatformIO pre-build hook: gzip data/ui/index.html into a C header.

The HTML is the source of truth. The header is generated and gitignored, so the
firmware is always built from the page currently on disk.
"""

Import("env")  # noqa: F821  - injected by PlatformIO

import gzip
import os

SOURCE = "data/ui/index.html"
TARGET = "src/config/internal/ui_index.h"
BYTES_PER_LINE = 16


def build_header(source_path, target_path):
    with open(source_path, "rb") as handle:
        raw = handle.read()

    # mtime=0 keeps the output byte-identical for identical input, so an
    # unchanged page never triggers a needless rebuild downstream.
    compressed = gzip.compress(raw, compresslevel=9, mtime=0)

    lines = [
        "// Generated by tools/embed_ui.py from data/ui/index.html.",
        "// Do not edit this file; edit the HTML instead.",
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "inline constexpr size_t kUiIndexGzLength = %d;" % len(compressed),
        "",
        "inline constexpr uint8_t kUiIndexGz[] = {",
    ]
    for offset in range(0, len(compressed), BYTES_PER_LINE):
        chunk = compressed[offset:offset + BYTES_PER_LINE]
        lines.append("    " + " ".join("0x%02x," % byte for byte in chunk))
    lines.append("};")
    lines.append("")

    directory = os.path.dirname(target_path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(target_path, "w") as handle:
        handle.write("\n".join(lines))

    print("embed_ui: %s -> %s (%d -> %d bytes)"
          % (SOURCE, TARGET, len(raw), len(compressed)))


def main():
    project_dir = env["PROJECT_DIR"]  # noqa: F821
    source_path = os.path.join(project_dir, SOURCE)
    target_path = os.path.join(project_dir, TARGET)

    if not os.path.isfile(source_path):
        raise Exception("embed_ui: missing %s" % source_path)

    if (os.path.isfile(target_path)
            and os.path.getmtime(target_path) >= os.path.getmtime(source_path)):
        return

    build_header(source_path, target_path)


main()
```

- [ ] **Step 2: Write `data/ui/index.html`**

One file, no external assets — the CSP of a captive portal sheet is hostile and there is
no internet on the far side anyway.

```html
<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>teletrack</title>
<style>
  :root {
    --bg: #f6f6f4; --fg: #16181d; --muted: #6b7280; --card: #ffffff;
    --line: #e3e3e0; --accent: #1f6feb; --bad: #b42318; --warn: #7a5b00;
    --warnbg: #fff6d6;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #14161a; --fg: #e8eaed; --muted: #9aa1ab; --card: #1c1f24;
      --line: #2b2f36; --accent: #539bf5; --bad: #ff6b5e; --warn: #ffd579;
      --warnbg: #3a2f10;
    }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 16px; background: var(--bg); color: var(--fg);
    font: 15px/1.45 system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
  }
  main { max-width: 460px; margin: 0 auto; }
  h1 { font-size: 20px; margin: 0 0 2px; letter-spacing: -0.01em; }
  .sub { color: var(--muted); font-size: 13px; margin: 0 0 16px; }
  .card {
    background: var(--card); border: 1px solid var(--line); border-radius: 10px;
    padding: 14px; margin-bottom: 14px;
  }
  .stat { display: flex; justify-content: space-between; padding: 3px 0; font-size: 14px; }
  .stat span:first-child { color: var(--muted); }
  .stat span:last-child { font-variant-numeric: tabular-nums; }
  label { display: block; font-size: 13px; color: var(--muted); margin: 12px 0 4px; }
  label:first-of-type { margin-top: 0; }
  input, select {
    width: 100%; padding: 9px 10px; font: inherit; color: var(--fg);
    background: var(--bg); border: 1px solid var(--line); border-radius: 7px;
  }
  input:focus, select:focus { outline: 2px solid var(--accent); outline-offset: -1px; }
  .err { color: var(--bad); font-size: 12px; min-height: 15px; margin-top: 3px; }
  button {
    width: 100%; margin-top: 16px; padding: 11px; font: inherit; font-weight: 600;
    color: #fff; background: var(--accent); border: 0; border-radius: 7px;
  }
  button:disabled { opacity: 0.55; }
  #banner {
    display: none; background: var(--warnbg); color: var(--warn);
    border: 1px solid currentColor; border-radius: 8px; padding: 9px 11px;
    font-size: 13px; margin-bottom: 14px;
  }
  #toast { min-height: 18px; margin-top: 9px; font-size: 13px; text-align: center; }
</style>

<main>
  <h1>teletrack</h1>
  <p class="sub">Phase 1 &middot; device configuration</p>

  <div id="banner">Storage is unavailable. Changes take effect now but will be
    lost on reboot.</div>

  <div class="card">
    <div class="stat"><span>Network</span><span id="s-ssid">&mdash;</span></div>
    <div class="stat"><span>Address</span><span id="s-ip">&mdash;</span></div>
    <div class="stat"><span>Clients</span><span id="s-clients">&mdash;</span></div>
    <div class="stat"><span>Uptime</span><span id="s-uptime">&mdash;</span></div>
    <div class="stat"><span>Free heap</span><span id="s-heap">&mdash;</span></div>
  </div>

  <form class="card" id="form">
    <label for="deviceName">Device name</label>
    <input id="deviceName" name="deviceName" autocomplete="off"
           autocapitalize="off" spellcheck="false">
    <div class="err" id="e-deviceName"></div>

    <label for="sampleHz">Sample rate</label>
    <select id="sampleHz" name="sampleHz">
      <option value="1">1 Hz</option>
      <option value="5">5 Hz</option>
      <option value="10">10 Hz</option>
      <option value="25">25 Hz</option>
    </select>
    <div class="err" id="e-sampleHz"></div>

    <button type="submit" id="save">Save</button>
    <div id="toast"></div>
  </form>
</main>

<script>
  var $ = function (id) { return document.getElementById(id); };

  function clearErrors() {
    $('e-deviceName').textContent = '';
    $('e-sampleHz').textContent = '';
  }

  function showErrors(errors) {
    clearErrors();
    var generic = [];
    Object.keys(errors || {}).forEach(function (field) {
      var node = $('e-' + field);
      if (node) { node.textContent = errors[field]; }
      else { generic.push(errors[field]); }
    });
    return generic.join(' ');
  }

  function formatUptime(ms) {
    var total = Math.floor(ms / 1000);
    var h = Math.floor(total / 3600);
    var m = Math.floor(total / 60) % 60;
    var s = total % 60;
    var pad = function (n) { return (n < 10 ? '0' : '') + n; };
    return pad(h) + ':' + pad(m) + ':' + pad(s);
  }

  function loadConfig() {
    return fetch('/api/config').then(function (r) { return r.json(); })
      .then(function (cfg) {
        $('deviceName').value = cfg.deviceName;
        $('sampleHz').value = String(cfg.sampleHz);
      });
  }

  function loadStatus() {
    return fetch('/api/status').then(function (r) { return r.json(); })
      .then(function (st) {
        $('s-ssid').textContent = st.ssid || '—';
        $('s-ip').textContent = st.ip || '—';
        $('s-clients').textContent = st.clients;
        $('s-uptime').textContent = formatUptime(st.uptimeMs);
        $('s-heap').textContent = Math.round(st.freeHeap / 1024) + ' KB';
        $('banner').style.display = st.persistDegraded ? 'block' : 'none';
      })
      .catch(function () { /* device rebooting or out of range; try again later */ });
  }

  $('form').addEventListener('submit', function (event) {
    event.preventDefault();
    clearErrors();
    $('save').disabled = true;
    $('toast').textContent = '';

    var payload = {
      deviceName: $('deviceName').value,
      sampleHz: Number($('sampleHz').value)
    };

    fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    })
      .then(function (r) { return r.json().then(function (b) { return [r.ok, b]; }); })
      .then(function (pair) {
        var ok = pair[0], body = pair[1];
        if (ok && body.ok) {
          $('toast').textContent = 'Saved.';
          return;
        }
        var generic = showErrors(body.errors);
        $('toast').textContent = generic || 'Check the highlighted fields.';
      })
      .catch(function () { $('toast').textContent = 'Could not reach the device.'; })
      .then(function () { $('save').disabled = false; });
  });

  // Poll status only while the tab is visible — no point draining the device
  // while the phone is in a pocket.
  var timer = null;
  function startPolling() {
    if (timer !== null) { return; }
    loadStatus();
    timer = setInterval(loadStatus, 2000);
  }
  function stopPolling() {
    if (timer === null) { return; }
    clearInterval(timer);
    timer = null;
  }
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) { stopPolling(); } else { startPolling(); }
  });

  loadConfig();
  startPolling();
</script>
```

- [ ] **Step 3: Hook the generator into the build and verify it runs**

In `platformio.ini`, `[env:esp]`, add a line below `board_build.arduino.memory_type`:

```ini
extra_scripts = pre:tools/embed_ui.py
```

Then:

```bash
pio run -e esp
head -12 src/config/internal/ui_index.h
```

Expected: a build log line like `embed_ui: data/ui/index.html -> src/config/internal/ui_index.h (5xxx -> 2xxx bytes)`,
and a header starting with the generated-file comment and `inline constexpr size_t kUiIndexGzLength`.

- [ ] **Step 4: Write `src/config/internal/WebUi.h`**

```cpp
#pragma once

#include <ESPAsyncWebServer.h>
#include <stddef.h>

#include "config/Settings.h"
#include "config/SettingsStore.h"
#include "config/internal/ConfigApi.h"

// Forward-declared, not included: ConfigPortal.h includes this header for its
// WebUi member, so including it back would be circular. WebUi.cpp includes it.
class ConfigPortal;

// HTTP plumbing. Every decision it makes is delegated: ConfigApi for
// serialisation, ConfigService for saves. If logic starts accumulating here,
// it belongs in one of those instead.
class WebUi {
 public:
  WebUi(Settings& settings, SettingsStore& store, const ConfigPortal& portal);

  // Call before AsyncWebServer::begin().
  void registerRoutes(AsyncWebServer& server);

 private:
  void collectBody(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                   size_t index, size_t total);
  void handleSave(AsyncWebServerRequest* request);

  Settings& settings_;
  SettingsStore& store_;
  const ConfigPortal& portal_;

  // POST body accumulation. One buffer is enough: the AsyncTCP task delivers
  // request bodies one at a time, and owner_ guards against a completion
  // handler reading a body that belonged to a different request.
  AsyncWebServerRequest* owner_ = nullptr;
  char body_[ConfigApi::kMaxBodyBytes + 1] = {};
  size_t bodyLen_ = 0;
  bool bodyOverflow_ = false;
};
```

- [ ] **Step 5: Write `src/config/internal/WebUi.cpp`**

```cpp
#include "config/internal/WebUi.h"

#include <string.h>

#include "config/ConfigPortal.h"
#include "config/internal/ConfigService.h"
#include "config/internal/ui_index.h"
#include "core/Log.h"

namespace {

void sendJson(AsyncWebServerRequest* request, uint16_t status, const char* body,
              size_t length) {
  AsyncWebServerResponse* response = request->beginResponse(
      status, "application/json", reinterpret_cast<const uint8_t*>(body), length);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

}  // namespace

WebUi::WebUi(Settings& settings, SettingsStore& store, const ConfigPortal& portal)
    : settings_(settings), store_(store), portal_(portal) {}

void WebUi::registerRoutes(AsyncWebServer& server) {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response =
        request->beginResponse(200, "text/html", kUiIndexGz, kUiIndexGzLength);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    Log::info("http", "GET /");
  });

  server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
    char body[ConfigApi::kJsonBufferSize];
    const size_t n = ConfigApi::toJson(settings_, body, sizeof(body));
    sendJson(request, 200, body, n);
    Log::info("http", "GET /api/config");
  });

  server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
    const DeviceStatus snapshot = portal_.status();
    char body[ConfigApi::kJsonBufferSize];
    const size_t n = ConfigApi::statusToJson(snapshot, body, sizeof(body));
    sendJson(request, 200, body, n);
  });

  server.on(
      "/api/config", HTTP_POST,
      [this](AsyncWebServerRequest* request) { handleSave(request); },
      nullptr,
      [this](AsyncWebServerRequest* request, uint8_t* data, size_t len,
             size_t index, size_t total) {
        collectBody(request, data, len, index, total);
      });
}

void WebUi::collectBody(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                        size_t index, size_t total) {
  if (index == 0) {
    owner_ = request;
    bodyLen_ = 0;
    bodyOverflow_ = total > ConfigApi::kMaxBodyBytes;
  }
  if (bodyOverflow_) {
    return;  // do not buffer what we are going to reject
  }
  if (bodyLen_ + len > ConfigApi::kMaxBodyBytes) {
    bodyOverflow_ = true;
    bodyLen_ = 0;
    return;
  }
  memcpy(body_ + bodyLen_, data, len);
  bodyLen_ += len;
  body_[bodyLen_] = '\0';
}

void WebUi::handleSave(AsyncWebServerRequest* request) {
  // A POST that never reached collectBody (no body at all) must not be judged
  // on whatever the previous request left behind.
  if (owner_ != request) {
    bodyLen_ = 0;
    bodyOverflow_ = false;
  }
  owner_ = nullptr;

  // An oversized body is signalled to ConfigService by a length past the cap,
  // which is exactly the condition its 413 branch tests for.
  const size_t effectiveLen =
      bodyOverflow_ ? ConfigApi::kMaxBodyBytes + 1 : bodyLen_;

  char response[ConfigApi::kJsonBufferSize];
  const ConfigService::SaveOutcome outcome = ConfigService::save(
      body_, effectiveLen, settings_, store_, response, sizeof(response));

  if (outcome.httpStatus == 200) {
    Log::info("cfg", "saved name=%s hz=%u", settings_.deviceName,
              (unsigned)settings_.sampleHz);
  } else {
    Log::warn("http", "POST /api/config %u", (unsigned)outcome.httpStatus);
  }

  sendJson(request, outcome.httpStatus, response, outcome.bodyLength);

  bodyLen_ = 0;
  bodyOverflow_ = false;
}
```

- [ ] **Step 6: Wire `WebUi` into `ConfigPortal`**

In `src/config/ConfigPortal.h`, add the include and the member:

```cpp
#include "config/internal/WebUi.h"
```

```cpp
  WebUi web_;
```

Declare `web_` as the **last** member. Member initialisers run in declaration order, so
`store_` is fully constructed before `web_` captures a reference to it.

In `src/config/ConfigPortal.cpp`, extend the constructor initialiser list:

```cpp
ConfigPortal::ConfigPortal(Settings& settings)
    : settings_(settings), server_(kHttpPort), web_(settings, store_, *this) {}
```

and register the UI routes **before** the captive-portal catch-all, so the real pages win:

```cpp
  web_.registerRoutes(server_);
  portal_.registerRoutes(server_);
  server_.begin();
```

- [ ] **Step 7: Build, flash, and verify over HTTP**

```bash
pio run -e esp -t upload
pio device monitor
```

Then, from a machine joined to the `teletrack` network:

```bash
curl -s http://192.168.4.1/api/config
curl -s -X POST http://192.168.4.1/api/config -H 'Content-Type: application/json' -d '{"sampleHz":25}'
curl -s -X POST http://192.168.4.1/api/config -H 'Content-Type: application/json' -d '{"sampleHz":7}'
curl -s http://192.168.4.1/api/status
```

Expected, in order:

```
{"schemaVersion":1,"deviceName":"teletrack","sampleHz":10}
{"ok":true}
{"ok":false,"errors":{"sampleHz":"must be 1, 5, 10 or 25"}}
{"ssid":"teletrack","ip":"192.168.4.1","clients":1,"uptimeMs":...,"freeHeap":...,"apUp":true,"persistDegraded":false}
```

- [ ] **Step 8: Verify persistence**

Power-cycle the board, then:

```bash
curl -s http://192.168.4.1/api/config
```

Expected: `"sampleHz":25` — the value set before the reboot.

- [ ] **Step 9: Verify the page in a browser**

Open `http://192.168.4.1/` on a phone joined to the network. The status card should tick
the uptime every two seconds, the form should be pre-filled, saving a valid change should
show "Saved.", and submitting a device name containing a space should show
`1-31 characters, letters digits _ - only` beneath the field.

- [ ] **Step 10: Commit**

```bash
git add platformio.ini tools/embed_ui.py data/ui/index.html src/config/internal/WebUi.h src/config/internal/WebUi.cpp src/config/ConfigPortal.h src/config/ConfigPortal.cpp
git commit -m "Serve embedded web UI and settings API from the captive portal"
```

---

### Task 11: `Display` — status header and log console

**Third flashable milestone.** The device becomes usable without a serial cable.

**Files:**
- Create: `src/ui/Display.h`, `src/ui/Display.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `Log`, `LogRing`, `Format`, `DeviceStatus`.
- Produces:
  - `class Display` — `bool begin()`, `void tick(uint32_t nowMs, const DeviceStatus&)`
  - constants `kMinRedrawIntervalMs == 100`, `kHeaderHeight == 40`, `kLogLinePitch == 10`, `kRotation == 1`

**Geometry:** rotation 1 gives 320×240. The header takes the top 40 px in font 2. The log
uses the GLCD font (font 1, 6×8 px, monospace) at a 10 px pitch — 20 rows from y=40 to
y=240, and 53 columns from x=2 to x=320. Those two numbers are `LogRing::kRows` and
`LogRing::kCols`; changing the geometry means changing them together.

**Locking:** `Display` holds no lock of its own. `Log` owns the console ring and its
mutex; `Display::tick()` calls `Log::snapshot()` to copy the ring into its own `LogRing`
and then draws from that copy, so the lock is never held while SPI is busy.

- [ ] **Step 1: Write `src/ui/Display.h`**

```cpp
#pragma once

#include <TFT_eSPI.h>

#include "core/DeviceStatus.h"
#include "core/LogRing.h"

// The only thing in the firmware that touches the TFT. tick() is called from
// loop() and is the sole place that drives SPI, which is what keeps the
// AsyncTCP task from racing the display.
class Display {
 public:
  static constexpr uint32_t kMinRedrawIntervalMs = 100;  // 10 Hz ceiling
  static constexpr int16_t kHeaderHeight = 40;
  static constexpr int16_t kLogLinePitch = 10;
  static constexpr uint8_t kRotation = 1;  // landscape, 320x240

  bool begin();

  // Call from loop() only.
  void tick(uint32_t nowMs, const DeviceStatus& status);

 private:
  void drawHeader(const DeviceStatus& status);
  void drawLog();

  TFT_eSPI tft_;
  LogRing console_;  // this frame's copy, refreshed from Log::snapshot()
  bool ready_ = false;
  uint32_t lastDrawMs_ = 0;
  uint32_t drawnRevision_ = 0;
  bool headerDrawn_ = false;
  DeviceStatus drawnStatus_;
};
```

- [ ] **Step 2: Write `src/ui/Display.cpp`**

```cpp
#include "ui/Display.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "core/Format.h"
#include "core/Log.h"

namespace {

// The header is worth repainting only when something on it actually changed.
// Uptime is compared at second resolution, which is the only field that moves
// on its own.
bool headerDiffers(const DeviceStatus& a, const DeviceStatus& b) {
  return strcmp(a.ssid, b.ssid) != 0 || strcmp(a.ip, b.ip) != 0 ||
         a.clients != b.clients || a.apUp != b.apUp ||
         a.persistDegraded != b.persistDegraded ||
         (a.uptimeMs / 1000u) != (b.uptimeMs / 1000u);
}

}  // namespace

bool Display::begin() {
  tft_.init();
  tft_.setRotation(kRotation);
  tft_.fillScreen(TFT_BLACK);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  ready_ = true;
  return true;
}

void Display::tick(uint32_t nowMs, const DeviceStatus& status) {
  if (!ready_) {
    return;
  }
  if (nowMs - lastDrawMs_ < kMinRedrawIntervalMs) {
    return;
  }
  lastDrawMs_ = nowMs;

  if (!headerDrawn_ || headerDiffers(status, drawnStatus_)) {
    drawHeader(status);
    drawnStatus_ = status;
    headerDrawn_ = true;
  }

  const uint32_t revision = Log::revision();
  if (revision != drawnRevision_) {
    Log::snapshot(console_);
    drawLog();
    drawnRevision_ = revision;
  }
}

void Display::drawHeader(const DeviceStatus& status) {
  tft_.fillRect(0, 0, tft_.width(), kHeaderHeight, TFT_NAVY);
  tft_.setTextFont(2);
  tft_.setTextSize(1);
  tft_.setTextColor(TFT_WHITE, TFT_NAVY);

  tft_.setTextDatum(TL_DATUM);
  tft_.drawString(status.ssid, 4, 2);
  tft_.drawString(status.apUp ? "AP UP" : "AP FAIL", 4, 20);

  char right[48];
  snprintf(right, sizeof(right), "%s  clients:%u", status.ip,
           (unsigned)status.clients);
  tft_.setTextDatum(TR_DATUM);
  tft_.drawString(right, tft_.width() - 4, 2);

  char stamp[9];
  Format::uptimeLong(status.uptimeMs, stamp, sizeof(stamp));
  char lower[48];
  snprintf(lower, sizeof(lower), "up %s%s", stamp,
           status.persistDegraded ? "   NO STORAGE" : "");
  tft_.drawString(lower, tft_.width() - 4, 20);

  tft_.setTextDatum(TL_DATUM);
}

void Display::drawLog() {
  // console_ is this frame's private copy, taken by tick(). Drawing takes
  // ~20 ms and must never block a logging task for that long.
  tft_.setTextFont(1);  // GLCD 6x8, monospace
  tft_.setTextSize(1);
  tft_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft_.setTextDatum(TL_DATUM);

  for (size_t i = 0; i < LogRing::kRows; ++i) {
    // Padded to the full column count so a shorter line erases the longer one
    // that was there before. Opaque text background means no clear-then-draw,
    // so no flicker.
    char padded[LogRing::kLineSize];
    snprintf(padded, sizeof(padded), "%-*s", static_cast<int>(LogRing::kCols),
             console_.row(i));
    tft_.drawString(padded, 2, kHeaderHeight + static_cast<int16_t>(i) * kLogLinePitch);
  }
}
```

- [ ] **Step 3: Wire `Display` into `src/main.cpp`**

`Log::begin()` comes first so the display-init failure has somewhere to go; every line
logged after it lands on both serial and the console ring, and the first `tick()` draws
whatever accumulated.

```cpp
#ifndef PIO_UNIT_TESTING

#include <Arduino.h>

#include "config/ConfigPortal.h"
#include "config/Settings.h"
#include "core/Log.h"
#include "ui/Display.h"

namespace {

// The one global. Everything else is reached through it by reference.
struct App {
  Settings settings = Settings::defaults();
  Display display;
  ConfigPortal portal{settings};
};

App app;

}  // namespace

void setup() {
  Log::begin(115200);
  if (!app.display.begin()) {
    Log::error("tft", "display init failed, serial only");
  }

  Log::info("boot", "teletrack phase 1");

  if (!app.portal.begin()) {
    Log::error("boot", "config portal failed to start");
  }
}

void loop() {
  const uint32_t now = millis();
  app.portal.tick(now);
  app.display.tick(now, app.portal.status());
  delay(1);  // yield to the WiFi and AsyncTCP tasks
}

#endif  // PIO_UNIT_TESTING
```

- [ ] **Step 4: Build, flash, and check the screen**

```bash
pio run -e esp -t upload
```

Expected on the TFT within a few seconds of boot:
- A navy header with `teletrack` top-left, `192.168.4.1  clients:0` top-right,
  `AP UP` below the SSID and `up 00:00:07` below the address, the uptime ticking once
  per second.
- Six or so grey log lines below it, oldest at the top, ending with
  `http: listening on port 80`.

If the screen stays black, drop `-DTFT_SPI_OVERLAP` from `platformio.ini` — it is an
ESP8266-only option inherited from the original config — and re-flash.

- [ ] **Step 5: Check that HTTP traffic reaches the screen without corrupting it**

Join the network and hold down reload on `http://192.168.4.1/` for ten seconds, so
requests arrive faster than the 10 Hz redraw.

Expected: log lines scroll, the header keeps ticking, and no row is left garbled or
half-drawn. Corruption here means something other than `Display::tick()` reached SPI —
re-check that nothing else in the call path draws.

- [ ] **Step 6: Commit**

```bash
git add src/ui/Display.h src/ui/Display.cpp src/main.cpp
git commit -m "Add TFT status header and scrolling log console"
```

---

### Task 12: Acceptance run and project README

Closes out the phase: the full suite, the manual checklist from the spec, and a README
that tells a future reader how to build and what the module boundaries mean.

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Run the full host suite**

```bash
pio test -e native
```

Expected: six suites pass — `test_config_api`, `test_config_service`, `test_format`,
`test_log_ring`, `test_memory_store`, `test_settings`.

- [ ] **Step 2: Run the on-device suite**

```bash
pio test -e esp -f embedded/test_nvs
```

Expected: `5 Tests 0 Failures 0 Ignored`.

- [ ] **Step 3: Verify the module boundary holds**

```bash
grep -rn "config/internal" src --include=*.cpp --include=*.h | grep -v "^src/config/internal/" | grep -v "^src/config/ConfigPortal"
```

Expected: no output. `ConfigPortal` is the only file outside `internal/` allowed to
include from it. If `src/main.cpp` appears here, the facade has been bypassed.

- [ ] **Step 4: Run the manual acceptance checklist**

Flash a release build, disconnect USB, and power the board from a battery or charger.

1. Power on with no serial cable. The screen shows the SSID and `192.168.4.1` within
   three seconds, and boot log lines appear beneath the header.
2. Connect a phone to the open `teletrack` network. The captive-portal sheet opens on
   its own, without typing an address.
3. The screen logs `ap: clients 0 -> 1` and then the HTTP requests.
4. Change the sample rate to 25 Hz and save. The page shows "Saved." and the screen logs
   `cfg: saved name=teletrack hz=25`.
5. Enter a device name containing a space and save. The field shows
   `1-31 characters, letters digits _ - only`, the screen logs a `[WRN]` line, and
   reloading the page shows the previous name still in place.
6. Power-cycle. The sample rate is still 25 Hz.

Record any step that fails; do not proceed to Step 5 until all six pass.

- [ ] **Step 5: Write `README.md`**

```markdown
# teletrack

DIY trackday telemetry logger. ESP32-S3 with a u-blox M10N GPS module and a 320×240
ILI9341 display.

## Status

**Phase 1 — configuration portal.** The device opens an open WiFi access point,
serves a captive-portal web UI for editing settings, persists them to NVS, and shows
a status header and scrolling log on the TFT. GPS and session logging are not built
yet.

Design: `docs/superpowers/specs/2026-09-06-phase1-config-portal-design.md`
Plan: `docs/superpowers/plans/2026-09-06-phase1-config-portal.md`

## Building

PlatformIO is not on `PATH` by default:

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
```

| Command | What it does |
| --- | --- |
| `pio run -e esp` | Build the firmware |
| `pio run -e esp -t upload` | Build and flash |
| `pio device monitor` | Serial log at 115200 |
| `pio test -e native` | Host unit tests, no hardware needed |
| `pio test -e esp -f embedded/test_nvs` | On-device tests |

## Using it

Power the board, join the open `teletrack` WiFi network, and the configuration page
opens by itself. If it does not, browse to `http://192.168.4.1/`.

## Layout

```
src/core/     logging, formatting, the DeviceStatus struct — no feature knowledge
src/config/   the configuration module: settings, storage, AP, captive portal, HTTP
src/ui/       the display and its log buffer
data/ui/      the web UI source; gzipped into a C header at build time
```

`src/config/` is cut by feature, not by layer: WiFi, DNS, HTTP and persistence all
exist to configure the device, so they live together behind `ConfigPortal`. Only
`Settings`, `SettingsStore` and `ConfigPortal` are public — a
future GPS module includes `config/Settings.h` to read the sample rate without
pulling in the network stack.

Each module splits pure logic from I/O — `Settings`/`NvsStore`, `ConfigApi`/`WebUi`,
`LogRing`/`Display` — so the logic runs in host tests with no hardware attached.
`platformio.ini`'s `build_src_filter` under `[env:native]` is the enforcement
mechanism: a file listed there that fails to compile has an Arduino dependency it
should not have.
```

- [ ] **Step 6: Commit**

```bash
git add README.md
git commit -m "Document Phase 1 build, usage and module layout"
```

---

## Deviations from the spec, and why

1. **`ConfigService` is a new module** not named in the spec. §6 and §12 define save
   semantics — validation ordering, rollback on write failure, degraded-mode behaviour —
   that would otherwise live inside `WebUi` where nothing can test them. Task 8.
2. **`ConfigPortal` owns its `NvsStore`** rather than receiving a `SettingsStore`.
   The spec forbids anything outside `src/config/` from including `internal/`, and
   `main.cpp` would have to in order to inject the concrete store. The store-dependent
   logic is tested through `ConfigService` against `MemoryStore` instead. Task 9.
3. **Two error strings the spec does not define** — `"malformed JSON body"` and
   `"body exceeds 1024 bytes"` — for the `400`/`413` bodies. Task 8.
4. **`apUp` added to `/api/status`.** The spec's status example omits it, but the
   header renders `AP UP` / `AP FAIL` and the UI has no other way to know. Task 9.
5. **`-fno-exceptions -fno-rtti` are applied to both environments** via `[common]`.
   They are already the default for Arduino-ESP32 builds, so on the device they are
   redundant; on the host they are what the spec asked for.

## Follow-ups for later phases

**Log timestamps should come from GPS, not uptime.** Phase 1 stamps every log line and
the screen header with `millis()`-derived uptime, because there is no other clock on the
board — the ESP32-S3 has no battery-backed RTC here, so every boot restarts at
`00:00:00`. That is fine for watching a boot sequence and useless for correlating a log
line with a lap, a session, or anything a second device recorded.

The u-blox M10N carries the fix: it reports UTC in `UBX-NAV-PVT` (and in the `GxRMC` /
`GxZDA` NMEA sentences). Time resolves from the satellite signal *before* a full position
fix, and `UBX-NAV-PVT` exposes `valid.validDate`, `valid.validTime` and
`valid.fullyResolved` so the code can tell when the value is trustworthy rather than
guessing.

When the GPS module lands:

- Set the system clock (`settimeofday`) once `validTime && fullyResolved` goes true, and
  log that transition — it is the moment timestamps stop being comparable across reboots.
- Switch `Format::logLine` and the screen header to wall-clock UTC, falling back to
  uptime while the clock is unset. Both places already take a `uint32_t ms`, so this is a
  change of source, not of shape.
- Do not chase sub-second accuracy without wiring the module's PPS pin. UART delivery
  jitter puts message-derived time in the tens-of-milliseconds range, which is plenty for
  log correlation but not for anything claiming to time a lap.

There is a `TODO(phase2)` marker on `Format::logLine` pointing here.

## Simplification pass (2026-09-06)

This is a proof of concept, not a shipping product. The following were cut as unearned
abstraction, in two rounds after Task 4:

- **`StatusProvider`** — a pure-virtual interface with exactly one implementation
  (`ConfigPortal`) and exactly one consumer (`WebUi`). `WebUi` now holds
  `const ConfigPortal&` and forward-declares it to avoid the circular include.
  One fewer file, one fewer indirection, identical behaviour.
- **`LogRing::clear()`** — no caller anywhere in the plan.
- **`CaptivePortal::end()`** — no caller; the AP runs from boot to power-down.
- **The no-heap-allocation constraint** — ArduinoJson 7 removed `StaticJsonDocument`,
  and writing a custom pool allocator to route around that costs far more complexity
  than the fragmentation it avoids on a device serving a config page.
- **`LogSink` and `SerialSink`** — an interface and an implementation to fan a log line
  out to two destinations that are both known at compile time. `Log` now writes to
  serial and to the console ring directly, and owns the ring's mutex. Three files became
  one, and `Display` stopped being a log sink that also draws.
- **`Format::uptimeShort` and deciseconds** — two time formats where one does. Log lines
  now carry `HH:MM:SS`, the same stamp as the header, which also removes the
  wraps-at-100-minutes wart the short form had.
- **`LogLevel::Debug`** — nothing in the firmware logs at debug level.
- **Defensive guards for callers that do not exist** — null and buffer-size checks in
  `Format`, buffer-too-small returns in `ConfigApi`, the dedup scan in
  `ValidationResult::add`. The rule kept instead: validate untrusted network input,
  trust our own callers.

Deliberately kept, and why:

- **`SettingsStore` + `MemoryStore`** — `NvsStore` cannot compile on the host, so
  without this seam the save/rollback/degraded paths have no host tests at all.
- **`ConfigService`** — holds the save semantics (validate, persist, roll back,
  choose a status code). Inside `WebUi` none of it would be testable.
- **The console lock** — the AsyncTCP task and `loop()` genuinely race on the log
  buffer. It now lives in `Log` beside the ring it protects, and `Display` draws from a
  private copy. This is correctness, not gilding.
