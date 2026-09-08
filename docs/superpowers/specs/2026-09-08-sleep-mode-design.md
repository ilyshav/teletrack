# Phase 5 — Sleep between sessions

**Date:** 2026-09-08
**Status:** Draft, awaiting review

## 1. Goal

Hold the power button and the device powers down; press it again and it comes
back. The point is a board that survives in a bag between track sessions instead
of flattening its 18650 overnight.

T-Beam Supreme only. The ESP32-S3-DevKitC-1 has no PMU and no power button.

### Out of scope

- **Timed wake.** Nothing needs the device to wake by itself.
- **Resuming state.** See §3: neither available mechanism resumes, so there is
  nothing to design here.
- **Low-battery auto-shutdown.** The AXP2101 has a threshold for it, but nothing
  has asked for it and it is a separate decision about when to give up.
- **Sleeping the GPS while the rest runs.** That is a power optimisation for the
  running state, not a sleep mode.

## 2. Everything is on the mode button, and it has to be

`features.md` asks for sleep on "another button, not mode switcher", and for waking
on that same button. Neither is possible here, so both gestures live on GPIO0.

LilyGO's board support declares exactly one user button — `BUTTON_PIN (0)`,
`BUTTON_COUNT (1)` — and GPIO0 is already the mode switch. The board's other button
is the power key, wired to the AXP2101 and reported over I2C on GPIO 40.

The power key can *trigger* a sleep. It cannot *wake* the chip. Keeping the GPS
backup RAM alive means the PMU stays powered and the ESP32 deep-sleeps rather than
switching off; waking from deep sleep needs an RTC GPIO; and the S3's stop at
GPIO21 — `SOC_RTCIO_PIN_COUNT` is 22 and the last channel defined is
`RTCIO_GPIO21_CHANNEL`. GPIO 40 is not among them.

Using it for sleep and GPIO0 for wake was considered and rejected: two buttons for
one feature, where the one you press to sleep is not the one that brings it back.
§3 is how one button carries both gestures without them colliding.

## 3. Triple click to sleep

| Gesture | Action |
| --- | --- |
| hold 3 s | switch radio — **unchanged** |
| triple click | sleep |
| any press while asleep | wake |

A triple click cannot be confused with a hold, so **the radio switch keeps its exact
current behaviour**: it fires at the instant the hold reaches three seconds, while
the button is still down, with the same countdown on screen. Nothing about the
existing gesture changes.

That matters more than it sounds. The obvious alternative — sleep on a longer hold —
would have forced both actions to be decided on release, because a six-second hold
passes three seconds on its way and would switch the radio first. Triple click
avoids that entirely.

### What counts as a click

`HoldDetector` already owns this pin's debounce, so it grows the click counting
rather than a second module duplicating it. Two constants:

```
kClickMaxMs         =  500   a press shorter than this is a click, not a hold
kMultiClickWindowMs = 1200   all three clicks must land inside this window
```

`update()` returns an event rather than a bool:

```cpp
enum class ButtonEvent : uint8_t { None, Hold, TripleClick };
ButtonEvent update(bool pressed, uint32_t nowMs);
```

`Hold` still fires while the button is down at `kHoldMs`. `TripleClick` fires on the
release of the third click. A press longer than `kClickMaxMs` is not a click and
resets the count, so a hold never contributes to one.

`HoldDetector` is pure and already host-tested, so unlike the rest of this phase the
thresholds and counting get real assertions.

### No new screen feedback is needed

The existing `HOLD 3s` countdown covers the hold. A triple click is over in under a
second — there is nothing useful to show mid-gesture, and the acknowledgement is the
`SLEEPING` message in §4. The spare status row stays spare.

## 4. LoRa is switched off permanently, not just while asleep

The boot log from this board shows every rail already enabled at power-up:

```
pmu: before: ALDO1=1 ALDO2=1 ALDO3=1 ALDO4=1 BLDO1=1 BLDO2=1
```

The firmware never enables ALDO3, but the AXP2101 powers it anyway, so **the LoRa
radio has been running since the board was first flashed.** This project does not
use LoRa and never will.

`Pmu::begin()` therefore disables ALDO3 unconditionally, at boot, on every start.
That is a continuous saving while running, not only during sleep, and it is the
single largest one available for a peripheral that does nothing.

The SD card (ALDO2), the sensors (ALDO1) and the M.2 interface (DCDC3/4/5) are
equally unused, but each has an interaction worth checking before switching it off
— the magnetometer on ALDO1 is what `OledDisplay::begin()` probes at 0x3C to decide
the panel's address, for instance. They are left alone here and are a separate,
easily reversible change.

## 5. What sleep actually does

```
src/gps/GpsReceiver.h/.cpp   GpsReceiver::sleep(), GpsReceiver::wake()
src/board/Pmu.h/.cpp         Pmu::prepareForSleep()
src/main.cpp                 drives the sequence
```

1. Log `sleep: going down`.
2. Stop the active radio through the existing `stopCurrentMode()`, so BLE or the AP
   comes down cleanly rather than being cut mid-connection.
3. **Put the GPS into software backup** — see §6.
4. Draw `SLEEPING`, hold it briefly, then blank the panel with U8g2's
   `setPowerSave(1)`.
5. `Pmu::prepareForSleep()` disables ALDO1, ALDO2, BLDO1, BLDO2, DCDC3, DCDC4 and
   DCDC5 — sensors, SD card and the M.2 interface. **ALDO3 is already off from
   boot, and ALDO4 stays on**, because the receiver needs its supply to hold its
   own memory.
6. `esp_sleep_enable_ext0_wakeup((gpio_num_t)0, 0)` — wake on GPIO0 going low.
7. `esp_deep_sleep_start()`.

| Consumer | Asleep |
| --- | --- |
| ESP32-S3, deep sleep | ~10 µA |
| MAX-M10S, software backup | ~15 µA |
| AXP2101 quiescent | ~30-50 µA |
| **Total** | **~55-75 µA** |

That is years on a 3000 mAh cell, and the figures are datasheet quiescent currents,
not measurements.

## 6. Guaranteeing a warm or hot start

**The receiver is put to sleep, not powered down.** `UBX-RXM-PMREQ` — class `0x02`,
id `0x41`, 16-byte payload — places the M10 in software backup, where it keeps its
ephemeris, almanac, time and last position in its own memory while drawing about
15 µA from a supply that stays on.

```
[0]       0x00         message version
[1..3]    0x00         reserved
[4..7]    0x00000000   duration, 0 means indefinite
[8]       0x06         flags: backup | force
[9..11]   0x00         reserved
[12..15]  0x00000008   wakeupSources: UART RX
```

Byte order is little-endian, as everywhere in UBX. The payload is taken from the
reference implementation's `powerOffWithInterrupt()`, not reconstructed from the
protocol tables.

**This is why ALDO4 stays powered, and it is what makes the guarantee hold.** The
earlier design cut the GPS rail and hoped the board routed `V_BCKP` to a backup
supply — an assumption about layout that nothing in software could confirm. Software
backup removes the assumption: the receiver holds its own state, whatever `V_BCKP` is
wired to.

### Waking it again

Two details that will otherwise present as a dead GPS.

**The receiver must be spoken to before it will answer.** It wakes on UART RX
activity, so `GpsReceiver::begin()` sends a short burst of filler bytes at each
candidate baud *before* listening. The existing probe only listens, and a receiver in
backup is silent — it would be reported as absent at every baud.

**Its configuration is gone.** Software backup preserves navigation data but not the
RAM-layer settings, so the receiver comes back at its default baud emitting NMEA.
That is already handled: a deep-sleep wake restarts from `setup()`, and
`GpsReceiver::begin()` re-probes the baud and re-sends the whole configuration on
every boot. Nothing extra is needed, but the reason it works is worth knowing.

**Do not wait for an ACK.** The reference notes the receiver may not acknowledge
`RXM-PMREQ` before it goes down; `sleep()` sends and returns.

### The backup rail is no longer load-bearing

`Pmu::begin()` still enables the AXP2101's backup-cell charger, and it is still
unverified whether that rail reaches this receiver. It no longer matters: if it does,
it is harmless redundancy; if it does not, software backup covers the case anyway.
Nothing in this design depends on it.

## 7. Testing

**`HoldDetector` gets real host tests** — it is pure, it already has them, and its
contract is changing from a bool to an event:

- Holding for `kHoldMs` still yields `Hold`, at the same instant as today, while the
  button is still down.
- Three clicks inside `kMultiClickWindowMs` yield `TripleClick` on the third release.
- Two clicks then silence yield nothing, and the count expires with the window.
- A fourth click does not fire a second `TripleClick` without a fresh sequence.
- A press longer than `kClickMaxMs` is not a click: it resets the count, so a hold
  never contributes to a triple click.
- A hold immediately after two clicks still yields `Hold`.
- Contact bounce inside the debounce window does not register as extra clicks —
  this is the case that would make the gesture fire by itself, and it is the one
  most worth testing.
- The boundaries at exactly `kClickMaxMs` and exactly `kMultiClickWindowMs` fall on
  the documented side.

**No host tests for the rest.** Disabling rails, `esp_deep_sleep_start()` and the
U8g2 blank are I/O with no logic, the same reason `Pmu`, `GpsReceiver` and the
displays have none.

The gate is:

- `pio run -e esp` and `pio run -e tbeam` both build.
- `pio test -e native` passes, with the rewritten `HoldDetector` tests.
- `Pmu.cpp` stays out of `[env:native]`'s `build_src_filter`.

**On hardware:** a 3-second hold still switches radio exactly as before; a triple
click sleeps; the panel goes dark; a press wakes it; the board does not enter
download mode on wake; charging continues while asleep; the rail log shows ALDO3 off
from the first boot; and — the point of the phase — **the GPS reaches a fix within
seconds of waking rather than tens of seconds.**

**Worth measuring once:** sleep current, and time to first fix after a wake. Both
claims in this document are estimates.

## 8. Success criteria

- [ ] A 3-second hold switches radio exactly as before this phase, with the same
      countdown and the same firing instant.
- [ ] A triple click sleeps: radio stopped, panel dark, rails down.
- [ ] Bouncing contacts never produce a triple click on their own.
- [ ] A press wakes the board, and it does not enter download mode.
- [ ] **LoRa (ALDO3) is off from boot, verified in the rail log, on every start.**
- [ ] **The GPS gets a warm or hot start after a wake** — a fix in seconds, not the
      tens of seconds a cold start needs. This is the requirement the phase exists
      to meet; if it fails, the design has failed.
- [ ] The receiver answers after a wake at all, which requires the filler bytes.
- [ ] ALDO4 stays powered while asleep; every other unused rail is down.
- [ ] Charging continues while asleep on USB.
- [ ] Sleep current and time-to-first-fix after a wake are measured once and written
      into `docs/hardware-notes.md`.
- [ ] DevKitC builds and behaves exactly as before; host tests pass.

## 9. Risks

**Triple click is undiscoverable without being told.** Unlike a hold, there is no
countdown to hint at it. That is accepted: it is a deliberate, rarely-used gesture,
and making it hard to trigger by accident is worth more than making it obvious.

**A bouncy button could in principle self-trigger.** Three contact closures inside
1.2 seconds is exactly what a failing switch produces. The existing debounce is what
stands between that and the board sleeping on its own, which is why the bounce case
is called out explicitly in the tests rather than left to chance.

**`kMultiClickWindowMs` at 1200 ms is a guess.** Too tight and the gesture feels
unreliable; too loose and three deliberate separate presses merge into one. It is one
constant and the first hardware session is where it gets confirmed.

**The current figures are datasheet quiescent values, not measurements.** ~55-75 µA
is an estimate built from three of them.

**Software backup is the whole guarantee, and it has one failure mode worth naming:
a receiver that will not wake.** If the filler bytes are too few, sent at the wrong
baud, or the M10 needs `EXTINT` rather than UART RX on this board, the symptom is a
GPS that is simply absent after the first sleep — and every subsequent boot, since
it stays in backup. Recovery is a power cycle, which is not obvious to anyone who has
not read this. If that happens, the fallback is to cut ALDO4 during sleep after all
and accept a cold start.

**Nothing verifies the board actually stays asleep.** If a rail we disable feeds
something that pulls GPIO0, the board would wake immediately and the symptom is a
sleep that does not stick.
