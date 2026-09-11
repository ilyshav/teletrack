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

## T-Beam Supreme power rails

From LilyGO's own board support (Xinyuan-LilyGO/LilyGo-LoRa-Series,
`examples/.../LoRaBoards.cpp`), not from guesswork:

| Rail | Feeds |
| --- | --- |
| ALDO1 | sensors (magnetometer, BME280) |
| ALDO2 | SD card |
| ALDO3 | LoRa |
| **ALDO4** | **GPS** |
| VBACKUP | the AXP2101's coin-cell domain — **does not reach the GNSS `V_BCKP`** |
| DCDC1 | ESP32 VDD — protected, never disable |

**The display is on none of them.** An earlier version of `Pmu.cpp` labelled
ALDO2 "display" and ALDO3 "GPS"; both were wrong, and hours went into power
theories for a dark panel whose actual fault was an I2C address collision.

### Every GPS start is a cold start, about 35 s

**Symptom:** the receiver takes 30–35 s to fix after a power cycle, outdoors with
a clear sky and a good antenna, even when the previous fix was a minute earlier.
NAV-PVT reports valid time only after about 20 s.

**Cause:** the board's coin cell does not hold the MAX-M10S's `V_BCKP`. The
20-second figure is the proof. A receiver whose backup domain has power keeps its
own RTC running and reports valid time within a second or two of boot, long
before it has a fix; recovering time at 20 s means it was decoded from the
satellite downlink, so the backup domain was dead and the navigation database was
empty.

The cell is an SII MS412FE, about 1 mAh. That is sized for the PCF8563 RTC on
Wire1 — roughly 250 nA, so over a year — and not for a GNSS backup domain at
~15 µA, which would flatten it in under three days. `Pmu::begin()` used to enable
the AXP2101's button-battery charger on the chance its rail reached the receiver.
It does not, and the call is gone.

**There is nothing to fix in firmware.** u-blox's own cold-start figure is ~28 s
and the ephemeris download is the floor: the navigation message carries it in
subframes that repeat every 30 s, so no antenna or setting moves it. A *working*
backup cell would not have helped much either — it buys a hot start only while
the ephemeris is under about two hours old, and past that a warm start costs the
same ~30 s as a cold one. Anything faster needs AssistNow data injected over
UART, not a battery.

### GPS pins

`GPS_RX_PIN 9`, `GPS_TX_PIN 8` (ESP32 side), `GPS_EN_PIN 7`, `GPS_PPS_PIN 6`.
LilyGO's code never drives GPS_EN or PPS, so neither does ours.

### 25 Hz needs a single constellation

The MAX-M10S datasheet's headline rate is 25 Hz, but that is **GPS-only**. With
several constellations running concurrently the ceiling is 10 Hz, and the receiver
will not accept a faster rate while they are enabled.

`GpsReceiver::begin()` therefore writes the constellation set before the rate:
above 10 Hz it enables GPS and disables Galileo, BeiDou, GLONASS, SBAS and QZSS;
at or below 10 Hz it switches them all back on. Writing the set explicitly in both
directions matters — otherwise dropping the rate after a 25 Hz run would leave the
receiver quietly GPS-only.

Fewer constellations means fewer satellites in view and worse accuracy where sky
view is poor. That is the trade 25 Hz buys, and the boot log says when it is made.

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

### LoRa is off from boot

The AXP2101 enables every rail by itself — a boot log reads `ALDO1=1 ALDO2=1
ALDO3=1 ALDO4=1 BLDO1=1 BLDO2=1` before the firmware touches anything — so ALDO3
had been powering a LoRa radio this project never uses since the board was first
flashed.

## CAN bus

Passive reading of the car's bus, forwarded to RaceChrono. A Waveshare
SN65HVD230 module (chip marked `VP230`) on the T-Beam:

```
module 3.3V   -> 3V3        module CAN RX -> GPIO 16
module GND    -> GND        module CAN TX -> GPIO 15
module CANH   -> OBD-II pin 6
module CANL   -> OBD-II pin 14
```

**Not GPIO 17 or 18** — those are this board's I2C bus, and the OLED is on them.

**Listen-only, always.** `TWAI_MODE_LISTEN_ONLY` means the controller never
transmits and never acknowledges. In any other mode a CAN controller acks every
frame it receives — it actively drives the bus — and on a car's live powertrain
network that turns a firmware bug into a vehicle behaviour bug.

**The module's 120 Ω terminator must be removed** — the SMD resistor marked
`121`, between the transceiver and the CANH/CANL headers. A bus is terminated at
both ends and the car has both; a third makes 40 Ω where the transceivers expect
60. Leave the pads **open**: the resistor sits across the pair, not in line with
it, and bridging shorts the bus.

**If no frames arrive, swap RX and TX first.** These modules are inconsistent
about whose perspective the labels take, and in listen-only mode a swap gives
silence, which is indistinguishable from a quiet bus.

### Byte order

Three, in one firmware:

| Where | Order |
| --- | --- |
| CAN id on characteristic 0x0001 | **little**-endian |
| Filter commands on 0x0002 | big-endian |
| Every GPS field | big-endian |

RaceChrono's spec calls the first one out explicitly — *"unlike other values in
this API"*. Getting it wrong yields ids that look plausible and match nothing.

### Mazda MX-5 ND

Main bus on OBD-II pins 6 and 14 at 500 kbps. Community-decoded ids:

| Id | Carries |
| --- | --- |
| `0x202` | accelerator position, speed, engine RPM |
| `0x78` | brake position |
| `0x86` | steering angle |

Fuel level, coolant temperature, clutch and gear are **not** known. Source:
`timurrrr/RaceChronoDiyBleDevice`, `can_db/mazda_mx5_nd.md`.

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

### RaceChrono connects, then drops about a second later, over and over

**Symptom:** `ble: connected` / `ble: disconnected, advertising again` cycling
roughly once a second. RaceChrono never stays on long enough to show a lock, so
it reports no satellites regardless of what the GPS is doing. Seen on **both**
boards, which is what rules out anything board-specific.

**Cause:** the peripheral asked for connection parameters no phone will accept.
`updateConnParams(handle, 6, 12, 0, 200)` requests a 7.5 ms minimum interval and
a 15 ms maximum. Apple's Accessory Design Guidelines require **interval min >=
15 ms** and **interval max >= interval min + 15 ms**; this violates both, and it
is sent on every connect. Android rejects out-of-range requests too.

**Fix:** `updateConnParams(handle, 12, 24, 0, 400)` -- 15 ms to 30 ms, 4 s
supervision timeout. 15 ms still carries 66 notifications a second against the
25 the device sends at its fastest.

The MTU-517 and 2M-PHY calls went at the same time. All three were sized for a
30 kB/s target that died when the consumer became RaceChrono, which takes one
fix per notification -- 500 B/s at 25 Hz. The reference implementation does none
of them, and each one is a deviation that can fail against a central we do not
control.

### Log lines arrive truncated, and BLE drops around the same time

**Symptom:** serial shows partial lines -- `00:00:35 [INF] b`, `00:00:40 [INF]`
with nothing after, a line starting mid-timestamp -- alongside a connect/drop
cycle.

**Cause:** `USBCDC::tx_timeout_ms` defaults to **250 ms**. When the host is not
draining the buffer, `Serial.write` blocks that long and then returns short,
which is what cuts a line in half. Whichever task called the logger wears the
stall -- and `NimBLEServerCallbacks::onConnect`/`onDisconnect` run on the
**NimBLE host task**, so a log line there stalls connection handling by a
quarter second at exactly the moment a central is discovering services.

**Fix, two parts:**
- `Serial.setTxTimeoutMs(0)` in `Log::begin()`. A log line is never worth
  stalling a task for; a write that will not fit now drops instead of waiting.
- The BLE callbacks set flags and `BleLink::tick()` does the logging, on
  `loop()`. Nothing on the host task touches serial.

**Also removed:** the explicit `startAdvertising()` in `onDisconnect`.
`NimBLEServer::m_advertiseOnDisconnect` defaults to true, so the stack restarts
advertising itself as soon as the callback returns; calling it again just fails
with `EALREADY`.

### RaceChrono connects and drops every two seconds, with no data in between

**Symptom:** a metronomic `connected` / `disconnected, advertising again` cycle
about two seconds apart, on both boards, with the GPS reporting 0 satellites.

**Cause:** the firmware was withholding every packet until the receiver reported
`validDate && validTime`. Indoors the receiver never resolves time, so nothing was
ever sent. RaceChrono subscribes, waits, receives nothing, and drops the link —
which is reasonable behaviour on its part.

**Fix:** send the fix regardless. A packet with `fixQuality = 0` says "still
acquiring" and keeps the client connected while the receiver works; silence says
nothing and reads as a dead device. This is what the reference implementation
does, and departing from it was the mistake.

**Related:** HDOP is one byte holding `dop * 10`, so 0.0 to 25.4, with 0xFF meaning
invalid. A u-blox receiver with no fix reports pDOP 99.99, which encoded naively
wraps to 232 and reads as a confident 23.2. It is clamped to 0xFF now. This never
showed up while the synthetic fix hardcoded `hdop = 1.0`.

### RaceChrono drops the link but nRF Connect stays connected

That asymmetry is the whole diagnosis: a generic central holds the connection
happily, so the BLE stack is fine and RaceChrono is hanging up on purpose.

**Cause:** the device exposed only the GPS half of the profile. The reference
implementation declares four characteristics on service 0x1FF8 --
`0x0001` CAN main (READ|NOTIFY), `0x0002` CAN filter (WRITE), `0x0003` GPS main,
`0x0004` GPS time. RaceChrono configures a DIY device by **writing a filter
command to 0x0002 on connect**, and against a device where that characteristic
does not exist the write fails and the app gives up.

The spec text says a device may implement whichever features it wants, which is
what made omitting them look safe. It is not, for the connect handshake.

**Fix:** declare both CAN characteristics. `0x0001` is never notified -- there is
no CAN bus on this device -- and the `0x0002` write handler accepts and ignores
every command, since deny-all, allow-all and allow-one-PID all mean the same
thing with no bus. What matters is that the write succeeds.

The handler logs the command byte it received, so the boot log now says whether
RaceChrono talks to that characteristic at all.

### `NimBLEDevice::deinit()` crashes the host task with PC=0

**Symptom:** `Guru Meditation Error: Core 0 panic'ed (InstrFetchProhibited)`,
`PC : 0x00000000`, backtrace one frame deep in
`NimBLEDevice::host_task at NimBLEDevice.cpp:837`. Seen on a mode switch, and
again on entering sleep — anything that called `BleLink::end()`.

**Cause:** line 837 is `nimble_port_run()`, the loop the NimBLE host task lives
in. `deinit()` calls `nimble_port_stop()` and then frees the port out from under
that task, which then dispatches an event whose handler is null.

**Fix: do not deinitialise.** `end()` stops advertising and disconnects the peer,
and leaves the stack running. That is all a mode switch or a sleep needs — the
radio is silent and the front end is free — and `begin()` checks
`NimBLEDevice::getInitialized()` so a later restart reuses the service and
characteristics that are still there rather than building them again.

The reference implementation never deinitialises either. Calling it was our
invention, and it had two separate ways to crash the board.

### The BLE host task is on core 0, loop() is on core 1

NimBLE pins its host task with `CONFIG_BT_NIMBLE_PINNED_TO_CORE`, which is 0,
while Arduino's `loop()` runs on `ARDUINO_RUNNING_CORE`, which is 1. Anything
shared between a BLE callback and `loop()` therefore crosses cores.

`volatile` is not enough for that. It constrains how the compiler treats one
object and promises nothing about the order two cores observe two different
writes. The CAN filter queue publishes a payload and then an index; without
release/acquire the consumer can see the new index and read a slot that is not
written yet -- applying a wrong id or interval rather than dropping a command,
which is the failure nothing counts.

The queue uses `std::atomic<uint8_t>` with a release store on the producer's
index and an acquire load on the consumer's. Two other things already live on
this boundary and are safe for a different reason: the log flags are single
booleans where a stale read costs one frame of latency, and the connection state
is a single bool.

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
