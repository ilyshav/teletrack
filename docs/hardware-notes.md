# Hardware and platform notes

Things that cost real debugging time on this board, with the symptom first — that is
how you will arrive here. Every entry was verified on the actual hardware.

Board: ESP32-S3-DevKitC-1, N16R8 (16 MB flash, 8 MB **octal** PSRAM), native USB,
ILI9341 TFT over SPI.

---

## Boot loops and flash

### Board boot-loops ~40×/second, no app output, dark screen

**Symptom:** `rst:0x3 (RTC_SW_SYS_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)` repeating, second
stage bootloader loads, then resets. Serial shows the ROM banner over and over.

**Cause:** `board_build.partitions = default_16MB.csv` **without** declaring the flash
size on the upload side. The board manifest sets `upload.flash_size = 8MB`, and that is
what esptool stamps into the bootloader header. `board_build.flash_size` does **not**
change it. The 16 MB table places spiffs at `0xc90000` and coredump at `0xff0000`, both
past the 8 MB the bootloader believes exists, so the partition table fails validation on
every boot.

**Fix:** declare it on both sides.

```ini
board_build.flash_size = 16MB
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
board_build.partitions = default_16MB.csv
```

Without the `board_upload.*` lines, PlatformIO silently uses `default_8MB.csv` — a
3.19 MB app partition, with half the chip unaddressable. With them, 6.55 MB.

### Diagnosing this class of problem

Read the ROM banner's `boot:` field. `boot:0x8` is a normal flash boot; `boot:0x0
(DOWNLOAD(USB/UART0))` means GPIO0 was low at reset and the chip is sitting in the ROM
loader running nothing at all. A dark display and silent serial mean "check the boot
mode" before "the firmware crashed".

---

## Build flags

### `-std=gnu++17` silently has no effect

The Arduino core appends its own `-std=gnu++11` **after** our flags, so it wins.
Symptom: `warning: inline variables are only available with -std=c++17`, and C++17
features compile as GNU extensions or not at all.

**Fix:** `build_unflags = -std=gnu++11` in `[env:esp]`.

### `-fno-exceptions` / `-fno-rtti` warn on every C file

`build_flags` applies to C as well as C++, so every `.c` in the framework emits
`cc1: warning: command line option '-fno-rtti' is valid for C++/ObjC++ but not for C`.

**Fix:** do not set them. The Arduino ESP32 core already builds with both.

---

## Display

### `TFT_eSPI::init()` crashes with `StoreProhibited` at address `0x10`

**Symptom:** `Guru Meditation Error: Core 1 panic'ed (StoreProhibited)`, backtrace
through `TFT_eSPI::begin_tft_write()` → `TFT_eSPI::init()`. `EXCVADDR: 0x00000010`.

**Cause:** TFT_eSPI defaults `SPI_PORT` to `FSPI` on the S3, which constructs a
`SPIClass` on a bus whose `begin()` fails, leaving a null bus pointer.
[Bodmer/TFT_eSPI#3743](https://github.com/Bodmer/TFT_eSPI/issues/3743).

**Fix:** `-DUSE_FSPI_PORT` in `build_flags`. That selects `SPI_PORT 2`, which is
`SPI3_HOST`, without patching the library — the usual fix edits
`Processors/TFT_eSPI_ESP32_S3.h` in place, which is lost on any clean or reinstall.

### Two TFT flags that do nothing

- **`-DTFT_SPI_PORT=3`** only fills a diagnostic struct (`TFT_eSPI.cpp:6007`). It does
  not select the bus. It is not the fix for the above.
- **`-DTFT_SPI_OVERLAP`** is ESP8266-only. On ESP32 it does not enable overlap mode; it
  makes `init()` skip `spi.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, -1)` and call bare
  `spi.begin()`, so the library uses default pins rather than the configured ones.

### Header flickers once a second

**Cause:** clearing a region with `fillRect` and then drawing text into it. The gap
between the two is visible, and anything containing a clock repaints at 1 Hz.

**Fix:** paint the background **once** at init, then use `setTextPadding(width)` so each
`drawString` fills its own fixed-width background as part of drawing. New text covers old
in a single operation.

---

## GPIO

| Pins | Claimed by |
| --- | --- |
| 0, 3, 45, 46 | strapping — avoid |
| **19, 20** | **USB D− / D+** — native USB uses these for serial *and* flashing |
| 26–37 | SPI flash **and octal PSRAM** (an N16R8; a quad-PSRAM part frees 33–37) |
| 8, 9, 10, 11, 12, 21 | this project's TFT |
| 39–42 | external JTAG (MTCK/MTDO/MTDI/MTMS) — free unless a probe is attached |

A button on GPIO20 shorts USB D+ to ground on every press: the link drops and flashing
fails while it is held. The mode button ended up on **GPIO39**.

Buttons need no external resistor — `pinMode(pin, INPUT_PULLUP)` and wire to ground.

---

## Serial

### Reading the port yourself returns zero bytes

The ESP32-S3's USB-Serial/JTAG peripheral emulates the classic auto-reset from
**DTR/RTS**, and pyserial asserts both on open — which holds the chip in reset. Every
read then returns nothing and it looks like the firmware is dead.

**Fix:** set `dtr = False` and `rts = False` on the port object **before** `open()`.

`pio device monitor` refuses to run with piped stdin, so it cannot be used from a
scripted shell. Run it interactively instead.

### Something else is holding the port

A stale `pio run -t upload -t monitor` silently consumes all output. Check with
`lsof /dev/cu.usbmodem*` before concluding anything about the device.

---

## BLE

### The device never appears in the OS Bluetooth settings pane

It never will. A BLE peripheral with a custom GATT service and no pairing does not
appear in macOS, iOS or Android Bluetooth settings — that list is for pairable devices
offering standard profiles. Use a BLE scanner (nRF Connect, LightBlue) or
`tools/ble_throughput.py`.

This device also **cannot** be paired: the ESP32-S3 has no Bluetooth Classic radio at
all. `BluetoothSerial` declares `architectures=esp32` only, and the S3's `soc_caps.h`
defines `SOC_BT_SUPPORTED` without `SOC_BT_CLASSIC_SUPPORTED`. Any app flow that asks
you to pair first is a dead end for this hardware.

### `NimBLEDevice::deinit()` aborts with "free() target pointer is outside heap areas"

**Cause:** `NimBLEServer::setCallbacks()` defaults to `deleteCallbacks = true`, so the
server's destructor calls `delete` on the pointer it was given. Handing it the address
of a static object aborts on teardown.

**Fix:** `setCallbacks(&callbacks, false)`.

Only reachable through `BleLink::end()`, so nothing catches it until a mode switch
actually tears BLE down.

### A scanning app shows an empty list while nRF Connect sees the device

**Cause:** the advertisement was full. A 128-bit service UUID costs 18 of the 31 bytes,
plus 3 for flags, leaving 10 — and `"teletrack"` needs 11. NimBLE silently moves the
name to the **scan response**, which only an *active* scanner requests. nRF Connect
scans actively and sees everything; a passive or hardware-filtered scan sees only the
primary advertisement, with no name and the UUID in AD type `0x07` rather than the
`0x03` a 16-bit filter matches.

**Fix:** declare the UUIDs 16-bit — 4 bytes instead of 18 — and call
`advertising->setName()` so the name is in the primary advertisement.

```cpp
service = server->createService(NimBLEUUID((uint16_t)0x1FF8));
advertising->addServiceUUID(NimBLEUUID((uint16_t)0x1FF8));
advertising->setName(deviceName);
```

16-bit and base-UUID 128-bit are identical at the GATT level, so clients still resolve
the service the same way. Only the bytes on the air change.

### NimBLE version

Pin **`h2zero/NimBLE-Arduino@^1.4.3`**. The 2.x line requires Arduino core 3.x / ESP-IDF
5.x; this project is on core 2.0.17 via `espressif32@7.0.1`. `NimBLEDevice::setDefaultPhy`
does not exist in 1.4.x — call `ble_gap_set_prefered_default_le_phy()` instead.

---

## RaceChrono

Adding a DIY device: **Settings → Add other device → Add other device → RaceChrono DIY →
Bluetooth LE → Search for devices in range.** The first list is devices already
configured, not a scan result. Requires RaceChrono Pro.

The device name is arbitrary — RaceChrono does not filter on it. An empty scan list is
an advertisement problem, not a naming one; see above.

Protocol and a working reference implementation are vendored at
`docs/reference/racechrono/`. **Port the packet encoder from the reference rather than
reconstructing it from the spec text** — the fine/coarse switchover for altitude and
speed, the sync-bit rule and the big-endian byte order are all easy to get subtly wrong,
and a wrong encoding produces plausible but incorrect data.
