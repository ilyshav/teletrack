# CAN sniffer — design

**Date:** 2026-09-08
**Status:** Draft, awaiting review
**This describes a SEPARATE project.** It lives here only until that repository
exists; move it there when it does.

## 1. Goal

Capture every frame on the car's CAN bus to an SD card, so the MX-5 ND's bus can
be reverse-engineered: press the clutch, watch which byte of which ID moves.

The point is the frames nobody has decoded yet. The community database has
throttle, speed, RPM, brake and steering; it is missing fuel level, coolant
temperature, clutch position and selected gear. Finding those is what this is for.

### Why a separate project, not another environment in `teletrack`

The overlap turned out to be a TWAI init and a read loop. Different board, no
PMU, no OLED, no BLE, no GPS — none of `teletrack`'s board knowledge applies, and
an environment sharing none of its modules is two programs in one folder rather
than reuse.

The one thing that should flow back is **data**: any ID decoded here belongs in
`teletrack`'s CAN spec, beside the bus it describes.

### Out of scope

- **Decoding.** This records; SavvyCAN analyses. See §6.
- **Filtering.** The whole value is capturing what you did not know to ask for.
- **Live streaming.** No BLE, no WiFi. A 500 kbps bus outruns both, which is the
  reason this exists rather than extending `teletrack`'s laptop probe tool.
- **Transmitting.** Never. See §4.

## 2. Hardware

A plain ESP32 devboard, an SN65HVD230 module, an SD card.

```
CAN TX   -> GPIO 21          CAN RX  -> GPIO 22
SD SCK   -> GPIO 18          SD MISO -> GPIO 19
SD MOSI  -> GPIO 23          SD CS   -> GPIO 5
CANH -> OBD-II pin 6         CANL -> OBD-II pin 14
```

500 kbps, the ND's powertrain rate.

**The SN65HVD230's 120 Ω terminator must be removed** — the SMD resistor marked
`121`, pads left open, not bridged. The car's bus is terminated at both ends
already. This is the same module and the same modification as `teletrack`; if
both devices are ever plugged in at once, both need it done.

**If no frames arrive, swap CAN RX and TX first.** These modules are inconsistent
about whose perspective the labels take, and in listen-only mode a swap gives
silence, which looks exactly like a quiet bus.

## 3. The architecture, and the problem it exists to solve

**SD cards stall.** Throughput is not the issue — 2000 frames a second at 20 bytes
is 40 kB/s and any card manages that. But cards pause for **100 ms or more**
during internal block erase, and at 2000 frames a second that is 200 frames lost
per stall.

So:

```
core 0                    core 1
TWAI read  ──▶ ring buffer (64 kB) ──▶ SD write, 8 kB blocks
```

- **Capture and writing run on different cores**, with a FreeRTOS queue between
  them. On one core the ring only delays the loss; on two, the writer stalls
  while the reader keeps draining the controller.
- **The ring holds 64 kB**, about 3300 records — roughly 1.6 seconds at 2000
  frames a second, which rides out any stall a card is likely to produce.
- **Writes go out in 8 kB blocks**, never per frame. A per-frame write spends
  more time in FAT bookkeeping than in data.
- The TWAI RX queue is set to 128 frames, so a scheduling hiccup on the reader
  does not lose frames either.

**Dropped frames are counted and recorded, never hidden.** A capture that lost
frames silently is worse than one that says so: you would spend the evening
hunting a signal that was never written down.

## 4. Listen-only, always

`TWAI_MODE_LISTEN_ONLY`.

A CAN controller in normal mode acknowledges every frame it receives and emits
error frames when it disagrees with the bus — it is an active participant. This
device will be plugged into a running car more often than anything else in this
project, usually while somebody prods at the controls. Listen-only makes driving
the bus electrically impossible.

There is no mode switch and no override. Nothing here ever needs to transmit.

## 5. The record

Fixed 20 bytes, little-endian, no framing:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | U4 | timestamp, microseconds since boot |
| 4 | U4 | CAN ID |
| 8 | U1 | DLC, 0-8 |
| 9 | U1 | flags: bit0 extended ID, bit1 RTR |
| 10 | U1[8] | data |
| 18 | U2 | frames dropped since the previous record |

Fixed width means the file is an array: seek to any record, no parsing. 20 bytes
at 2000 frames a second is 40 kB/s, about 144 MB an hour — a session is
comfortably within any card.

**Microseconds, not milliseconds.** Frames arrive within the same millisecond
routinely, and the interval between two frames is often the thing that identifies
them. `micros()` wraps every 71 minutes; a capture session is minutes, and the
converter in §6 flags a wrap rather than pretending it cannot happen.

**The dropped counter is in every record**, not a summary at the end, so a loss is
attributable to a moment rather than to the file.

## 6. Files, and getting them into SavvyCAN

One file per session, `CAN0001.BIN` upward. There is no clock, so the index comes
from scanning the card at startup and taking the next free number.

**Flush at least once a second.** A capture ends when the car is switched off or
the plug is pulled — never with a clean shutdown. FAT will not recover a file
whose directory entry was never updated, so an unflushed session is not a
truncated capture, it is no capture at all.

**Analysis belongs in [SavvyCAN](https://savvycan.com/), not here.** It is the
established tool for this exact job: overlay two captures, diff them, watch one
byte while you press the clutch. A converter script turns the binary into the
generic CSV it imports:

```
Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8
```

Writing that converter is an afternoon. Writing an analysis UI is not, and the
result would be worse than what already exists.

## 7. Knowing it is working, without a screen

There is no display. Serial reports once a second:

```
can: 1847 fps, 0 dropped, file CAN0003.BIN, 12.4 MB
```

That single line answers every question worth asking in the car: is the bus
alive, is anything being lost, is it still writing, and how far in. **A frame rate
of zero is the signal that the wiring is wrong** — and it is what tells you to try
swapping RX and TX.

## 8. Testing

**Host tests for the parts with rules**, which are the record packing and the
ring buffer:

- A frame packs to exactly 20 bytes with the documented field offsets.
- An extended ID and an RTR frame set the right flag bits.
- A DLC below 8 leaves the unused data bytes zero rather than stale.
- The ring returns records in the order they went in.
- A full ring drops the newest and counts it, rather than overwriting the oldest —
  losing the newest frame is recoverable, corrupting the sequence is not.
- The drop counter is carried into the next record written and then cleared.
- A `micros()` wrap does not reorder records or produce a negative interval.

**No host tests for the TWAI driver or the SD writer** — I/O shells.

**On hardware:** frames appear with the engine running; the rate is plausible for
a 500 kbps bus, in the hundreds to low thousands; nothing is dropped at idle;
pulling power mid-capture leaves a readable file containing everything up to the
last flush.

## 9. Success criteria

- [ ] Listen-only. The device never transmits.
- [ ] Frames captured to SD at bus rate with no drops at idle.
- [ ] Dropped frames, if any, counted and recorded against the moment they
      happened.
- [ ] Pulling the power leaves a file readable up to the last flush.
- [ ] The converter produces CSV that SavvyCAN imports without editing.
- [ ] Serial shows frame rate, drops and file position once a second.
- [ ] A byte visibly changes in SavvyCAN when a control is operated — the whole
      point, and the only end-to-end proof.

## 10. Risks

**The OBD-II connector may be gatewayed.** Some manufacturers expose only
diagnostic traffic there unless it is requested. The ND is documented as putting
the powertrain bus on pins 6 and 14, and the community's IDs were read that way,
but from a 2019 car rather than a 2023 one. If nothing arrives with the wiring
confirmed and RX/TX tried both ways, no firmware fixes it — the next step is a
tap elsewhere on the bus.

**Card speed varies wildly.** A slow or worn card stalls longer than the ring can
absorb. The dropped counter is what makes that visible instead of mysterious; the
fix is a better card, not more buffering.

**Capturing everything means capturing everything.** A busy bus at 2000 frames a
second fills a gigabyte in seven hours. That is fine for a session and wrong for
a device left plugged in — which this should not be, since it is listening to a
car's powertrain bus with no reason to be there when nobody is looking at it.
