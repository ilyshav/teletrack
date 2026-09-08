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

## 2. One button, two gestures that cannot be confused

`features.md` asks for "another button, not mode switcher". LilyGO's board support
declares exactly one user button:

```
#define BUTTON_PIN   (0)
#define BUTTON_COUNT (1)
#define PMU_IRQ      (40)
```

The other button on the board is the power key, wired to the AXP2101 and reported
over I2C on GPIO 40. It works as a *sleep* trigger — but it cannot **wake** the chip,
and that decides the design.

### Why the power key cannot wake it

Keeping the GPS's backup RAM alive means the PMU must stay powered, which means the
ESP32 must sleep rather than be switched off. Waking from deep sleep needs an RTC
GPIO, and on the ESP32-S3 those stop at GPIO21 — `SOC_RTCIO_PIN_COUNT` is 22 and the
last channel defined is `RTCIO_GPIO21_CHANNEL`. The PMU's interrupt line is GPIO 40.

So the only button that can wake this board is **GPIO0, the mode button**.

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

## 4. What sleep actually does

```
src/board/Pmu.h/.cpp    Pmu::prepareForSleep()
src/main.cpp            drives the sequence
```

1. Log `sleep: going down`.
2. Stop the active radio through the existing `stopCurrentMode()`, so BLE or the AP
   comes down cleanly rather than being cut mid-connection.
3. Draw `SLEEPING` and hold it briefly, then blank the panel with U8g2's
   `setPowerSave(1)`.
4. `Pmu::prepareForSleep()` disables ALDO1, ALDO2, ALDO3, ALDO4, BLDO1, BLDO2,
   DCDC3, DCDC4 and DCDC5 — the sensors, SD card, LoRa, GPS main and the M.2
   interface — and **leaves the backup rail enabled**.
5. `esp_sleep_enable_ext0_wakeup((gpio_num_t)0, 0)` — wake on GPIO0 going low.
6. `esp_deep_sleep_start()`.

### What stays powered, and why

**The backup rail.** This is the whole reason for sleeping rather than shutting
down. The receiver keeps its almanac, ephemeris and time, so the first fix after a
wake takes seconds instead of tens of seconds. On this board that routing is
**unverified** — see §6.

**DCDC1**, the ESP32's own supply, which the vendor marks protected. The chip is in
deep sleep at roughly 10 µA rather than unpowered.

**The charger.** The PMU stays up, so a board left on USB keeps charging while it
sleeps. That is the right behaviour and needs no code — it is simply what does not
get disabled.

Estimated total draw is around 150 µA, which is over two years on a 3000 mAh cell.
The figure is an estimate from datasheet quiescent currents and has not been
measured on this board.

## 5. Waking

`esp_deep_sleep_start()` does not return. A wake restarts execution from `setup()`,
exactly like a power-on, so there is no state to save or restore and no resume path
to design. The radio comes up in `kBootMode` as it always does.

**GPIO0 is a strapping pin, and waking on it is safe.** Held low at a *power-on*
reset the chip enters download mode, which is how the board is flashed. A deep-sleep
wake is not a power-on reset: the ROM takes its fast path through the wake stub and
never re-evaluates the boot-mode strapping. Holding the button to wake cannot drop
the board into download mode.

A short press does nothing while awake — only a three-second hold does anything — so
"short press wakes it" collides with no existing gesture.

## 6. The GPS backup rail is unverified on this board

`Pmu::begin()` enables the AXP2101's backup-cell charger, and a comment there used
to claim it keeps the GNSS almanac alive. That claim came from the general role of
that rail, not from evidence about this board, and it has been corrected.

LilyGO's own support for the S3 Supreme never enables `XPOWERS_VBACKUP` — every
reference to it is in other board branches, one of them noting the rail costs about
100 µA. Whether it reaches this receiver's `V_BCKP` pin is a layout question that no
amount of reading the software will answer.

Two things settle it, both cheap:

- **`getButtonBatteryVoltage()` in the boot log.** If that rail reads nothing, there
  is nothing on it and the premise is wrong.
- **Time the first fix after a wake.** Under about ten seconds means the backup
  domain survived; thirty or more means it did not.

If it turns out the rail does not reach the receiver, sleep still works and still
saves the battery — it just wakes to a cold start, and the design should then be
simplified back to `shutdown()`, which is both simpler and lower.

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
download mode on wake; charging continues while asleep; and the boot log reports the
backup-rail voltage.

**Worth measuring once:** sleep current, and time to first fix after a wake. Both
claims in this document are estimates.

## 8. Success criteria

- [ ] A 3-second hold switches radio exactly as before this phase, with the same
      countdown and the same firing instant.
- [ ] A triple click sleeps: radio stopped, panel dark, rails down.
- [ ] Bouncing contacts never produce a triple click on their own.
- [ ] A press wakes the board, and it does not enter download mode.
- [ ] The backup rail is still enabled while asleep.
- [ ] Charging continues while asleep on USB.
- [ ] The boot log reports the backup-rail voltage.
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

**The 150 µA estimate is unmeasured**, and so is everything about the backup rail.
If the rail turns out not to reach the receiver, the correct response is to simplify
back to `shutdown()` rather than keep a more complex sleep that buys nothing.

**Nothing verifies the board actually stays asleep.** If a rail we disable feeds
something that pulls GPIO0, the board would wake immediately and the symptom is a
sleep that does not stick.
