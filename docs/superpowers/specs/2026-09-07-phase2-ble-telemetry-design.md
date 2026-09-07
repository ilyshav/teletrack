# Phase 2 — BLE telemetry link

**Date:** 2026-09-07
**Status:** Draft, awaiting review

## 1. Goal

Add a Bluetooth Low Energy link that streams telemetry to a phone, and a hardware
button that switches the device between BLE and the Phase 1 WiFi configuration
portal. The two radios are mutually exclusive.

The telemetry consumer is **RaceChrono**, so the device implements RaceChrono's
published BLE DIY device profile rather than a protocol of our own. That decision fixes
the service UUID, the characteristic UUIDs and the exact byte layout of every payload —
none of it is ours to choose.

Phase 2 streams **synthetic** GPS fixes in RaceChrono's format at a realistic rate.
There is no GPS module yet, so there is nothing genuine to send; the point is to prove
RaceChrono connects, recognises the device and plots the data before Phase 3 puts a real
receiver behind it.

### Out of scope

- GPS, IMU, and any real sensor data.
- Session recording and bulk transfer.
- BLE pairing, bonding, encryption, or authentication.
- A phone application. Verification uses a laptop script and an off-the-shelf BLE tool.

## 2. Hard constraint: no Bluetooth Classic

The ESP32-S3 has **no BR/EDR radio**. Verified two ways: the Arduino
`BluetoothSerial` library declares `architectures=esp32` only, and the S3's
`soc_caps.h` defines `SOC_BT_SUPPORTED` without `SOC_BT_CLASSIC_SUPPORTED`.

There is therefore no SPP, no RFCOMM, and no "pair it and open a serial terminal".
Everything is BLE GATT.

## 3. Radio modes

Both radios share one 2.4 GHz front end. Rather than run them concurrently and accept
degraded timing on both, the device is in exactly one mode at a time.

| Mode | Radio | Reachable by |
| --- | --- | --- |
| `Ble` | BLE advertising / connected | a BLE client |
| `Wifi` | SoftAP + captive portal | any browser (Phase 1) |

- **Boot mode is `Ble`.** The device advertises immediately on power-up.
- **GPIO39 held for 3 seconds** toggles the mode. Active-low with an internal pull-up.
- On a switch, the outgoing stack is fully torn down before the incoming one starts.

### Why the button is a 3-second hold

A short press is too easy to trigger by accident, and dropping the telemetry link
mid-session because something brushed a button is the failure this avoids. The event
fires **on reaching 3 s**, not on release, so the device reacts at the moment you feel
it should and holding longer does not toggle twice.

The screen counts the hold down while the button is held, so the timing is visible.

### Prerequisite: GPIO39 must be wired

Boot mode is `Ble` and the button is the only way out of it. Until GPIO39 has a button
on it, a flashed device advertises over BLE and its configuration portal cannot be
reached at all.

Wiring the button is therefore a prerequisite for using this firmware, not a
follow-up. The recovery if it is not wired is a reflash.

## 4. Module structure

```
src/radio/RadioMode.h           enum RadioMode + ModeController        pure
src/radio/ModeButton.h/.cpp     GPIO debounce and hold timing          I/O shell
src/ble/BleLink.h/.cpp          NimBLE server, advertising, notify     I/O shell
src/ble/TelemetryRing.h/.cpp    fixed sample ring with drop counting   pure
src/config/ConfigPortal.h/.cpp  gains end() to tear the AP down
src/ui/Display.h/.cpp           header gains mode, rate and drop count
tools/ble_throughput.py         laptop receiver and measurement script
```

`ModeController` and `TelemetryRing` hold all the logic and are host-tested.
`ModeButton` and `BleLink` are thin shells over hardware. This is the same split the
Phase 1 modules use, and `[env:native]`'s `build_src_filter` enforces it: a file
listed there must compile with no Arduino dependency.

### `ConfigPortal::end()`

Phase 1 deliberately had no teardown — the AP ran from boot to power-down, so
`CaptivePortal::end()` was cut during simplification as having no caller. Mode
switching gives it one. `ConfigPortal::end()` stops the HTTP server, stops the DNS
server, and calls `WiFi.softAPdisconnect(true)`.

This is YAGNI working correctly: cut when unused, restored when a real caller appears.

## 5. Data rate

**Far lower than an earlier draft of this spec assumed.** RaceChrono's GPS
characteristic carries **one 20-byte fix per notification**. At 25 Hz that is
**500 B/s** — under 1% of what BLE can carry.

The earlier draft targeted ~30 kB/s and mandated a 517-byte MTU, LE 2M PHY, Data
Length Extension, a 7.5 ms connection interval and multi-sample batching. All of that
was sized for a custom protocol carrying raw IMU data. Against RaceChrono's profile it
is unnecessary, and the batching would be actively wrong — the format is one fix per
packet.

What remains:

- **NimBLE-Arduino 1.4.3** rather than the bundled Bluedroid `BLE` library — half the
  flash and about 100 KB less RAM. Version 1.4.3 specifically: this project is on
  `espressif32@7.0.1`, which resolves to Arduino core 2.0.17 (ESP-IDF 4.4), and the
  NimBLE 2.x line requires Arduino core 3.x / IDF 5.x. Upgrading the platform to reach
  NimBLE 2.x is not worth it — the current pin is what the working TFT and WiFi
  configuration was verified against.
- **Notifications, not indications.**
- The default 23-byte ATT MTU is already enough for a 20-byte payload. Negotiating
  higher is harmless but earns nothing, so it is not required.

IMU data has no place in this API. RaceChrono's DIY profile carries GPS, CAN-bus and
monitor values; the established way to carry IMU is as synthetic CAN packets on the
CAN-bus characteristic. That is out of scope here and belongs with the real sensors.

## 6. Backpressure and dropped samples

`TelemetryRing` is a fixed-capacity ring of samples with a producer (the telemetry
source) and a consumer (the BLE notify pump).

**On overflow it overwrites the oldest sample and increments a drop counter.** It
never blocks and never grows. If the phone cannot keep up, or the link stalls, the
device keeps running and loses old data rather than stalling `loop()` behind a
backed-up notify queue.

The drop count is exposed in the `status` characteristic and shown on screen. Silent
data loss during a session is worse than a visible number.

Every sample carries a **sequence number**. The receiver detects exactly which samples
were lost rather than inferring loss from a byte count — that is what makes the
measurement trustworthy.

## 7. GATT layout — RaceChrono BLE DIY profile

**None of this is ours to choose.** The service, the characteristic UUIDs and every
byte of every payload are defined by RaceChrono. The specification and a working
reference implementation are vendored at `docs/reference/racechrono/`.

| | UUID | Properties |
| --- | --- | --- |
| Service | `00001ff8-0000-1000-8000-00805f9b34fb` (0x1FF8) | — |
| GPS main | `0x0003` | READ, NOTIFY — 20 bytes |
| GPS time | `0x0004` | READ, NOTIFY — 3 bytes |

CAN-bus (`0x0001`, `0x0002`) and monitor (`0x0005`, `0x0006`) are **out of scope for
Phase 2**. They are where IMU data will eventually go, as synthetic CAN packets.

### Encoding

All multi-byte values are **big-endian** — the opposite of the native layout, and the
single easiest thing to get wrong here. (The CAN packet ID is little-endian, but we do
not implement CAN.)

**GPS main, 20 bytes:**

| Bytes | Field |
| --- | --- |
| 0–2 | sync bits (3) + time from hour start (21) = `minute*30000 + seconds*500 + ms/2` |
| 3 | fix quality (2 bits) + locked satellites (6 bits, invalid `0x3F`) |
| 4–7 | latitude, degrees × 10⁷, signed two's complement, invalid `0x7FFFFFFF` |
| 8–11 | longitude, same encoding |
| 12–13 | altitude — fine: `((m+500)*10) & 0x7FFF`; coarse: `((m+500) & 0x7FFF) \| 0x8000` |
| 14–15 | speed — fine: `(km/h*100) & 0x7FFF`; coarse: `((km/h*10) & 0x7FFF) \| 0x8000` |
| 16–17 | bearing, degrees × 100, invalid `0xFFFF` |
| 18 | HDOP × 10, invalid `0xFF` |
| 19 | VDOP × 10, invalid `0xFF` |

**GPS time, 3 bytes:** sync bits (3) + `(year-2000)*8928 + (month-1)*744 + (day-1)*24 + hour` (21 bits).

**Sync bits** are a 3-bit counter that increments **whenever the GPS-time value
changes**, and must be identical in both characteristics. RaceChrono compares them and
waits if they disagree, so getting this wrong stalls the client rather than corrupting
a value.

**Fine vs coarse switchover:** the reference hands over as soon as the fine encoding
overflows its 15 bits — above 2776.7 m and above 327.67 km/h. Switching later leaves
values that wrap and decode as plausible but wrong numbers.

**Port the encoder from `docs/reference/racechrono/canbus-gps-device-main.ino`
verbatim.** Do not reconstruct it from the table above; the table is for reading, the
reference is for building.

### Phase 2 payload

No GPS receiver exists yet, so Phase 2 sends a **synthetic fix that moves**: a slow
circle at walking pace with a valid time, a fix quality of 1 and a plausible satellite
count. A frozen point would prove the encoding parses but not that RaceChrono tracks
updates. Fields we cannot know send their documented invalid value rather than zero —
zero is a real coordinate.

## 7a. Device name

`Settings::deviceName` drives **both** the AP SSID and the BLE advertised name, and
takes effect **immediately** on save: the active radio is torn down and restarted with
the new name.

Saving a new name from the web page therefore **drops the browser** — the AP it was
connected through has gone. That is the accepted cost of immediate application, and the
page says so next to the field. The restart happens on the loop tick *after* the HTTP
response has been sent, so the client sees its `{"ok":true}` before the connection goes.

Until now `deviceName` was stored and validated but never used by anything, which made
it a setting that silently did nothing.

## 8. Screen

The header gains the current mode, replacing nothing — the SSID and IP fields are
meaningless in BLE mode and are replaced by BLE state.

| Mode | Header shows |
| --- | --- |
| `Ble`, advertising | `BLE ADV` |
| `Ble`, connected | `BLE CONN`, live kB/s, dropped count |
| `Wifi` | `AP UP`, SSID, IP, client count (Phase 1 behaviour) |

While the mode button is held, the header shows a countdown to the 3-second mark.

In BLE mode there is no web UI, so the screen is the only feedback the device gives.

## 9. Verifying the link

The deliverable is no longer a throughput number — at 500 B/s there is nothing to
measure. It is **RaceChrono accepting the device and plotting a moving position.**

### Primary: RaceChrono on Android

Add the device as a "RaceChrono DIY" BLE device. It must appear in RaceChrono's own
scan (not the OS Bluetooth pane — a BLE peripheral with a custom service never appears
there), connect, and show a position that moves along the synthetic path with a
plausible speed and a valid time.

This is the acceptance test. Anything else is a proxy for it.

### Secondary: `tools/ble_throughput.py`

A `bleak` script run from the laptop, kept for regression rather than performance. It
subscribes to GPS main, **decodes the 20-byte packet with the same field layout
RaceChrono uses**, and prints the decoded latitude, longitude, speed, bearing, fix
quality and sync bits along with the notification rate.

That decode is the point: it catches a byte-order or bit-packing error immediately and
locally, where the symptom is a wrong number on a terminal rather than a silently wrong
track in an app. Two independent decoders agreeing is decent evidence the encoding is
right.

`bleak` is not a project dependency; the script's header documents `pip install bleak`,
and on a PEP 668 system it needs a virtualenv.

## 10. Failure handling

| Failure | Behaviour |
| --- | --- |
| BLE stack fails to start | Log `ERR`. No fallback — it has never happened, and the button is the recovery |
| Client disconnects | Return to advertising; ring keeps filling and overwriting |
| Notify queue full | Drop oldest, increment counter, never block |
| Mode switch while connected | Disconnect cleanly, tear down, start the other stack |
| `ConfigPortal::end()` fails | Log `ERR` and start BLE anyway; a half-torn-down AP is better than a device stuck in neither mode |

No failure path reboots the device.

## 11. Testing

**Host tests** (`pio test -e native`, no hardware):

- `ModeController`: every transition; boot state; button event; BLE-requested switch;
  a switch requested while already switching; that a switch is idempotent.
- `TelemetryRing`: fill below capacity; wraparound; drop counter accuracy under
  overflow; batch extraction respecting a byte budget; sequence numbers monotonic
  across a wrap.
- `ModeButton` hold timing is host-tested by feeding it synthetic timestamps and pin
  states, so the 3-second logic is verified without the button existing.

**On-device:**

- BLE advertises and accepts a connection.
- Throughput measured per §9.
- Mode switch via GPIO39, once the button is wired. Until then this is untestable
  on hardware, which is why the hold logic is host-tested against synthetic
  timestamps.

**Manual acceptance:** power on → `BLE ADV` on screen → connect a phone →
`BLE CONN` with a live rate → run the throughput measurement → hold GPIO39 for 3 s →
screen shows `AP UP` and the config page is reachable → hold again or reboot → back to
BLE.

## 12. Success criteria

- [ ] Device boots into BLE mode and advertises as `teletrack` within 3 s.
- [ ] RaceChrono discovers the device in its own scan and connects to it.
- [ ] RaceChrono shows a position that moves, with a plausible speed and a valid time.
- [ ] `tools/ble_throughput.py` decodes the packet and its values match what RaceChrono shows.
- [ ] Dropped samples are counted and shown on screen.
- [ ] Changing `deviceName` renames both the AP SSID and the BLE name on save.
- [ ] Holding GPIO39 for 3 s switches to WiFi and the Phase 1 portal works unchanged.
- [ ] GPIO39 held 3 s toggles the mode, once the button is wired.
- [ ] The screen always shows the current mode.
- [ ] Host test suite passes with no hardware attached.

## 13. Risks

**The 30 kB/s target depends on the client as much as the device.** An Android phone
that negotiates a 30 ms connection interval will roughly halve throughput regardless
of what the firmware does. This is why the acceptance criterion records the negotiated
MTU and interval alongside the rate — a number without them is not interpretable.

**GPIO39 is not wired.** Until it is, there is no way out of BLE mode and the
configuration portal is unreachable — the device must be reflashed to recover. There
is no software safety net by design. Wire the button before relying on this firmware.

**NimBLE is a new dependency and a different API from the bundled library.** If it
does not behave, the fallback is Bluedroid at roughly double the memory cost — a
rewrite of `BleLink` but nothing else, because everything above it talks to
`TelemetryRing`.
