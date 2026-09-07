# Phase 2 — BLE telemetry link

**Date:** 2026-09-07
**Status:** Draft, awaiting review

## 1. Goal

Add a Bluetooth Low Energy link that streams telemetry to a phone, and a hardware
button that switches the device between BLE and the Phase 1 WiFi configuration
portal. The two radios are mutually exclusive.

Phase 2 streams **synthetic** telemetry at the real cadence and packet size. There is
no GPS or IMU yet, so there is nothing genuine to send. The point is to establish the
transport and **measure its actual throughput** before Phase 3 commits to a payload
format. The deliverable is a number, not a demo.

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
- **GPIO4 held for 3 seconds** toggles the mode. Active-low with an internal pull-up.
- **A write to the `mode` characteristic** requests `Wifi`.
- On a switch, the outgoing stack is fully torn down before the incoming one starts.

### Why the button is a 3-second hold

A short press is too easy to trigger by accident, and dropping the telemetry link
mid-session because something brushed a button is the failure this avoids. The event
fires **on reaching 3 s**, not on release, so the device reacts at the moment you feel
it should and holding longer does not toggle twice.

The screen counts the hold down while the button is held, so the timing is visible.

### Why a `mode` characteristic exists

Boot mode is `Ble` and the GPIO4 button **is not yet physically wired**. Without a
second route, a device would boot into BLE with no way to reach the configuration
portal — unconfigurable until someone solders a button on.

Writing `1` to the `mode` characteristic requests `Wifi`. That makes "WiFi on demand"
mean the button *or* any BLE client, removes the lockout entirely, and stays useful
after the button exists.

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

## 5. Throughput budget

The target is **~30 kB/s sustained**: GPS at 25 Hz plus IMU at 100–200 Hz.

The intended client is an **Android phone**, which negotiates a 517-byte ATT MTU and
short connection intervals. That puts 30 kB/s comfortably inside the practical
ceiling of roughly 40–100 kB/s.

Every one of these is required; the budget does not close without them:

- **NimBLE-Arduino 2.5.1** rather than the bundled Bluedroid `BLE` library — half the
  flash and about 100 KB less RAM.
- **ATT MTU negotiated to 517**, so a full batch fits in one notification.
- **LE 2M PHY** requested on connect.
- **Data Length Extension** enabled.
- **Connection interval 7.5–15 ms** requested.
- **Notifications, not indications** — indications require a per-packet
  acknowledgement and roughly halve throughput.
- **Batching.** One sample per notification would spend the whole budget on protocol
  overhead. Pack samples until the payload approaches the negotiated MTU, then send.

### Client platform limits, recorded so the numbers are read correctly

| Client | Max ATT MTU | Realistic sustained |
| --- | --- | --- |
| Android | 517 | 40–100 kB/s |
| iOS / macOS | 185 | 15–40 kB/s |

macOS and iOS share CoreBluetooth, which negotiates MTU itself and gives applications
no control over the connection interval. A measurement taken on the laptop is
therefore a **floor**, not the device's ceiling, and is expected to land at roughly
half the Android figure. Both numbers get recorded, labelled with the platform.

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

## 7. GATT layout

One service, three characteristics. UUIDs are 128-bit randoms fixed in
`BleLink.h`; the advertised device name is `teletrack`.

### `live` — notify

Batched samples, packed binary, sized to the negotiated MTU. Each sample:

| Field | Type | Meaning |
| --- | --- | --- |
| `seq` | `uint32` | monotonic, gap = loss |
| `uptimeMs` | `uint32` | device uptime at capture |
| `payload` | `uint8[N]` | Phase 2: filler. Phase 3: real telemetry |

Sample size is a compile-time constant chosen so a batch fills the MTU.

### `status` — read / notify

JSON, reusing the Phase 1 `ConfigApi` style:

```json
{ "mode": "ble", "uptimeMs": 134221, "freeHeap": 186432,
  "mtu": 517, "connIntervalMs": 15, "sent": 84213, "dropped": 12 }
```

### `mode` — write

A single byte: `0` requests `Ble`, `1` requests `Wifi`. Any other value is ignored.

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

## 9. Measuring throughput

### `tools/ble_throughput.py`

A Python script using **`bleak`**, run from the laptop. It:

1. Scans for a device advertising as `teletrack` and connects.
2. Reports the **negotiated MTU** — the single most important number for interpreting
   the result.
3. Subscribes to `live` and consumes notifications for a fixed window (default 30 s).
4. Reports sustained **kB/s**, notifications per second, samples per second, and
   **loss percentage derived from sequence gaps**.

It is checked into the repository so the measurement is repeatable rather than a
one-off reading someone remembers.

`bleak` is not currently installed; the script's header documents `pip install bleak`.

### Acceptance

- **Android, via nRF Connect or an equivalent:** the headline number. Target
  ≥ 30 kB/s sustained with < 1% loss.
- **macOS, via `tools/ble_throughput.py`:** the regression floor. Expected roughly
  half the Android figure. A large drop between runs signals a regression even though
  the absolute value is limited by the platform.

Both figures and the MTU each was measured at are recorded in the implementation plan
when the task runs.

## 10. Failure handling

| Failure | Behaviour |
| --- | --- |
| BLE stack fails to start | Log `ERR`, fall back to `Wifi` mode so the device stays reachable |
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
- Mode switch via the `mode` characteristic, since GPIO4 is unwired.

**Manual acceptance:** power on → `BLE ADV` on screen → connect a phone →
`BLE CONN` with a live rate → run the throughput measurement → write `1` to `mode` →
screen shows `AP UP` and the config page is reachable → write `0` or reboot → back to
BLE.

## 12. Success criteria

- [ ] Device boots into BLE mode and advertises as `teletrack` within 3 s.
- [ ] An Android client connects, negotiates a 517-byte MTU, and receives notifications.
- [ ] Sustained throughput ≥ 30 kB/s with < 1% sequence loss, measured and recorded.
- [ ] `tools/ble_throughput.py` produces a repeatable measurement from the laptop.
- [ ] Dropped samples are counted, exposed in `status`, and shown on screen.
- [ ] A `mode` write switches to WiFi and the Phase 1 portal works unchanged.
- [ ] GPIO4 held 3 s toggles the mode, once the button is wired.
- [ ] The screen always shows the current mode.
- [ ] Host test suite passes with no hardware attached.

## 13. Risks

**The 30 kB/s target depends on the client as much as the device.** An Android phone
that negotiates a 30 ms connection interval will roughly halve throughput regardless
of what the firmware does. This is why the acceptance criterion records the negotiated
MTU and interval alongside the rate — a number without them is not interpretable.

**GPIO4 is not wired.** Until it is, the `mode` characteristic is the only way out of
BLE mode. If BLE also fails to start, §10's fallback to `Wifi` is the remaining
safety net.

**NimBLE is a new dependency and a different API from the bundled library.** If it
does not behave, the fallback is Bluedroid at roughly double the memory cost — a
rewrite of `BleLink` but nothing else, because everything above it talks to
`TelemetryRing`.
