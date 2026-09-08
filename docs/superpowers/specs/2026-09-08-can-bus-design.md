# Phase 6 — CAN bus

**Date:** 2026-09-08
**Status:** Draft, awaiting review

## 1. Goal

Read the car's CAN bus and forward the frames RaceChrono asks for, so throttle,
brake, steering and RPM sit alongside GPS in the same session.

T-Beam Supreme only — the transceiver wires to that board. The
ESP32-S3-DevKitC-1 compiles a stub, as it does for the GPS and the PMU.

### Out of scope

- **OBD-II polling.** Requesting values by sending query frames is a different
  protocol and a different RaceChrono device type. This device never transmits.
- **Decoding.** No PIDs, no equations, no units in the firmware. RaceChrono is
  told which IDs to forward and decodes them itself.
- **CAN FD.** The ND's powertrain bus is classic CAN. The BLE profile allows a
  16-byte payload but classic CAN caps at 8, and nothing here needs more.
- **Logging frames to storage.** Same answer as every other phase: the phone
  records.

## 2. The car, and why this is known to work

A 2023 MX-5 (ND). The main bus is on **OBD-II pins 6 (CAN-H) and 14 (CAN-L)** at
**500 kbps**, and the community has already reverse-engineered the interesting IDs:

| ID | Carries |
| --- | --- |
| `0x202` | accelerator position, speed, engine RPM |
| `0x78` | brake position |
| `0x86` | steering angle |

Documented against a 2019 ND2 and reported to hold across ND model years. Fuel
level, coolant temperature, clutch and gear are **not** known — nobody has found
them yet, so do not promise them.

Source: `timurrrr/RaceChronoDiyBleDevice`, `can_db/mazda_mx5_nd.md`.

The car broadcasts these continuously. Nothing has to be asked for.

## 3. Hardware

Waveshare SN65HVD230 board (chip marked `VP230`).

```
module 3.3V    -> T-Beam 3V3
module GND     -> T-Beam GND
module CAN RX  -> GPIO 16      BoardConfig::kCanRxPin
module CAN TX  -> GPIO 15      BoardConfig::kCanTxPin
module CANH    -> OBD-II pin 6
module CANL    -> OBD-II pin 14
```

GPIO 15 and 16 are unclaimed in LilyGO's pin map for this board, are not
strapping pins, and are not USB or flash. They live in `BoardConfig.h` beside the
GPS pins, so moving them once the board is in hand is a one-line change.

**The module's 120 Ω terminator must be removed** — the SMD resistor marked
`121` between the transceiver and the CANH/CANL headers. The car's bus is already
terminated at both ends; a third makes 40 Ω where the transceivers expect 60. The
pads are left **open**, not bridged: the resistor sits across the pair, not in
line with it, and bridging would short the bus.

**If no frames arrive, swap RX and TX first.** These modules are inconsistent
about whose perspective the labels take, and in listen-only mode a swap produces
silence, which is indistinguishable from a quiet bus.

## 4. Listen-only, and why it is not negotiable

`TWAI_MODE_LISTEN_ONLY`.

A CAN controller in normal mode **acknowledges every frame it receives** — it
actively drives the bus — and emits error frames when it disagrees with what it
sees. On a car's live powertrain bus that turns a firmware bug into a vehicle
behaviour bug.

Listen-only makes it electrically impossible: the controller never drives the
line. Since this device only ever reads, and RaceChrono only ever reads, there is
no case where any other mode is wanted. The TX pin is still assigned because the
peripheral requires one; it is never driven.

## 5. The protocol

Both characteristics already exist — they were declared when RaceChrono's connect
handshake turned out to write to `0x0002` regardless of whether a device has CAN.

### Filter, written by RaceChrono to `0x0002`

| Command | Length | Payload |
| --- | --- | --- |
| `0` deny all | 1 | — |
| `1` allow all | 3 | 2-byte notify interval |
| `2` allow one PID | 7 | 2-byte interval, 4-byte PID |

Command 2 repeats, once per ID. **These fields are big-endian.**

### Frames, notified on `0x0001`

| Bytes | Content |
| --- | --- |
| 0-3 | 32-bit CAN ID, **little-endian** |
| 4-n | frame payload, 1-8 bytes for classic CAN |

The notification length is 4 + DLC, not padded.

### Three byte orders in one firmware

The CAN ID goes out **little-endian**. The filter arrives **big-endian**. Every
GPS field is **big-endian**. The protocol says so explicitly — *"this value is a
little-endian integer, unlike other values in this API"* — and getting it wrong
produces IDs that look plausible and match nothing. Each conversion is commented
at the point it happens.

## 6. The filter table

```
src/can/CanFilter.h/.cpp    pure: the table, the rules, the rate limiting
```

The reference implements this as a hash map with 1024 slots and heap allocation.
This uses **a fixed array of 64 entries with a linear scan**. A car broadcasts
tens of distinct IDs, RaceChrono asks for a handful, and 64 comparisons against a
frame arriving every 500 µs is nothing on a 240 MHz core. No allocation, no
buckets, and the whole thing fits in a page of code.

```cpp
class CanFilter {
 public:
  static constexpr size_t kMaxIds = 64;

  void denyAll();
  void allowAll(uint16_t intervalMs);
  void allowId(uint32_t canId, uint16_t intervalMs);

  // True when this frame should go out now. Records the time when it says yes,
  // so the caller cannot forget to.
  bool shouldNotify(uint32_t canId, uint32_t nowMs);
};
```

**The interval is per ID, not global.** That is the whole point of it: RaceChrono
asks for RPM at 50 ms and steering at 100 ms, and each is limited on its own
clock. A single global throttle would starve whichever ID happened to lose.

In allow-all mode, entries are created for IDs as they are first seen. When the
table is full, further new IDs are dropped and it says so once. Allow-all is a
discovery mode for finding what a car broadcasts; 64 is more than an MX-5 has.

## 7. Where it runs

```
src/can/CanBus.h/.cpp    Arduino shell: TWAI driver, drains the queue
```

`CanBus::tick()` runs from `loop()` and drains the whole RX queue each pass,
forwarding what the filter allows. No ring buffer: frames go straight out as
notifications, exactly as the reference does.

**Buffering telemetry has already gone wrong once on this project** — a ring
filled while nothing was connected and then flooded a minute of stale fixes at
RaceChrono the moment it appeared. A CAN frame is worth even less once it is old.
Frames are forwarded or dropped, never stored.

Nothing is forwarded unless a client is connected, the same rule the GPS follows.

The RX queue is sized at 32 frames. `loop()` turns at roughly 1 kHz and a busy
500 kbps bus delivers on the order of 1000-2000 frames a second, so a pass sees a
couple of frames in the ordinary case and the queue absorbs the jitter. Overflow
is a dropped frame, which is what the filter would have done to most of them.

## 8. The transceiver will cost the sleep budget

The SN65HVD230 draws **around 10 mA while active**. Phase 5 got the whole board
down to an estimated 55-75 µA asleep. Left powered, this transceiver alone is two
orders of magnitude more than that, and turns a sleep measured in months into one
measured in days.

The chip has a standby mode on its `Rs` pin, but **this module does not break
`Rs` out** — the header is 3.3V, GND, CAN RX, CAN TX only — so it is presumably
tied for high-speed operation and cannot be told to sleep.

This phase does **not** solve that. It records it, because the number is easy to
measure once the module is wired and impossible to guess reliably from a
datasheet:

- **Measure the sleep current with the transceiver connected.** That single
  reading decides whether anything needs doing.
- If it matters, the options are powering the module from a spare GPIO used as a
  switch — 10 mA is within a pin's budget — or from a PMU rail, and cutting it in
  `prepareForSleep()`.

Deciding that now, from datasheet numbers, would be guessing at a problem that
one measurement settles.

## 9. Testing

**`CanFilter` gets real host tests** — it is pure, it holds all the rules, and
every one of them is a place to be subtly wrong:

- Deny-all forwards nothing, including IDs previously allowed.
- Allow-all forwards an unknown ID, and throttles it to the default interval.
- An explicitly allowed ID uses its own interval, not the default.
- Two IDs with different intervals are limited independently — the busy one does
  not starve the slow one, and vice versa.
- The first frame for an ID goes out immediately rather than waiting an interval.
- An interval of 0 means no throttling.
- The table filling stops accepting new IDs without disturbing the ones it has.
- `millis()` wrapping does not stall an ID forever.

**No host tests for `CanBus`** — it is the TWAI driver and a notification call,
the same I/O shell as `GpsReceiver` and the displays.

The gate is:

- `pio run -e esp` and `pio run -e tbeam` both build.
- `pio test -e native` passes with the new `CanFilter` tests.
- `CanBus.cpp` stays out of `[env:native]`'s `build_src_filter`.

**On hardware:** with the car running, the log shows frames arriving; RaceChrono
writes a filter and the log reports the command; the channels defined from
`0x202`, `0x78` and `0x86` show plausible RPM, throttle, brake and steering.

## 10. Success criteria

- [ ] The controller runs in listen-only mode and never transmits.
- [ ] RaceChrono's filter commands are parsed — deny, allow-all, allow-one — and
      the log names which was received.
- [ ] Frames are forwarded with the ID **little-endian** and a length of 4 + DLC.
- [ ] Per-ID intervals are honoured independently.
- [ ] Nothing is forwarded with no client connected.
- [ ] GPS, BLE and the display keep working at their current rates.
- [ ] Sleep current with the transceiver attached is measured and written into
      `docs/hardware-notes.md`, whatever it turns out to be.
- [ ] DevKitC builds and behaves exactly as before.

## 11. Risks

**The bus may be gatewayed.** Some manufacturers filter the OBD-II connector so
only diagnostic traffic appears unless requested. The ND is documented as
exposing the powertrain bus directly, and the community's IDs were read this way,
but that is someone else's 2019 car. If no frames arrive with the wiring
confirmed and RX/TX swapped both ways, this is the reason, and no amount of
firmware fixes it.

**RaceChrono can ask for more than BLE can carry.** Allow-all with a short
interval on a busy bus will exceed what a 15-30 ms connection interval can move.
Notifications queue and then fail. The per-ID interval is the mechanism that
prevents this and it is under the app's control, not ours — worth knowing when
"some frames are missing" turns out to mean "you asked for all of them".

**The car is not a test bench.** Every hardware fault in this project so far has
been found by flashing and looking. Here the device is plugged into a moving car's
powertrain bus. Listen-only is what makes that acceptable, and it is the one
constraint in this document not to compromise on for convenience.
