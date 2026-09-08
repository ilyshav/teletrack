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

## 2. There is only one user button, and it is taken

`features.md` asks for "another button, not mode switcher". LilyGO's board support
for this board declares exactly one:

```
#define BUTTON_PIN   (0)
#define BUTTON_COUNT (1)
#define PMU_IRQ      (40)
```

GPIO0 is already the mode switch. The other button on the board is the **power
key, wired to the AXP2101**, which reports short and long presses as interrupts on
GPIO 40. That is the second button, and long-press-off / short-press-on is what
the key exists for.

## 3. Power down, not deep sleep

**`AXP2101::shutdown()` cuts every rail; only VRTC stays up.** Draw falls to the
PMU's quiescent current — tens of microamps, weeks on an 18650.

ESP32-S3 deep sleep is the obvious alternative and it is worse here. The MCU's RTC
domain draws about 10 µA, but the AXP2101 stays fully powered with DCDC1 up, so the
board keeps drawing the PMU's own current plus every rail not explicitly disabled.

**And deep sleep buys nothing back, because it does not resume either.** ESP32 deep
sleep restarts execution from `setup()`, exactly as a power-on does. Neither
mechanism preserves the running state, so the only real difference between them is
current, and shutdown wins outright.

### Expect a cold start on wake, and measure it

`shutdown()` documents itself as turning off all power channels, with **only VRTC**
staying up — and VRTC is the PMU's own RTC, not the backup rail. The receiver loses
power completely.

A u-blox M10 keeps its almanac, ephemeris, time and last position in battery-backed
RAM held up by its `V_BCKP` pin. What that pin is connected to on this board decides
everything:

| Fed by | Result on wake |
| --- | --- |
| a supercap or coin cell on the board | hot or warm start, seconds to a fix |
| a PMU rail that `shutdown()` cuts | cold start, tens of seconds to minutes |

**We do not know which this board is.** Phase 4 enabled the AXP2101's backup-cell
charger with a comment claiming it keeps the almanac alive; that claim came from the
general role of that rail, not from evidence about this board. LilyGO's own support
for the S3 Supreme never enables `XPOWERS_VBACKUP` at all — every reference to it is
in other board branches — and the comment has been corrected to say so.

So the design assumes a **cold start** and does not depend on anything better.

**The measurement that settles it** is in §7: sleep, wake, and time the first fix.
Under about ten seconds means the backup domain survived; thirty seconds or more
means it did not. Either way the answer belongs in `docs/hardware-notes.md`, because
it is a fact about the board that nothing in software can tell us.

If it turns out to be a cold start and that is too slow between sessions, the option
is to leave the GPS rail powered while sleeping — but that costs milliamps, not
microamps, and would defeat the phase. That would be a different design, not a tweak.

## 4. The sequence

```
src/board/Pmu.h/.cpp    enableSleepButton(), sleepRequested(), sleep()
src/main.cpp            acts on the request in loop()
```

`Pmu::begin()` additionally:

- `enableIRQ(XPOWERS_AXP2101_PKEY_LONG_IRQ | XPOWERS_AXP2101_PKEY_SHORT_IRQ)`
- `setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S)` — the hardware backstop
- `setPowerKeyPressOnTime(XPOWERS_POWERON_512MS)` — a deliberate press to wake, not
  a knock

`Pmu::tick()` already runs at 1 Hz for the battery. It also reads the IRQ status and
latches `sleepRequested_` when the long-press bit is set, then clears the IRQ.

**Polling at 1 Hz is enough and the GPIO 40 interrupt line is not used.** The button
is held for over a second by definition; a second of latency before the screen says
anything is acceptable, and an ISR that touches I2C is not.

`loop()` acts on the request:

1. Log `sleep: powering down`.
2. Stop the active radio — the same `stopCurrentMode()` a mode switch uses, so BLE
   or the AP comes down cleanly rather than being cut mid-connection.
3. Draw `SLEEPING` on the display and hold it briefly, so the button press is
   visibly acknowledged before everything goes dark.
4. `Pmu::sleep()` → `g_pmu.shutdown()`.

### The hardware backstop stays armed

`enableLongPressShutdown()` with `setLongPressPowerOFF()` is left on. The long-press
**interrupt** fires well before the 10 s **off-timer**, so firmware always wins the
race and does the orderly shutdown — but if it ever hangs, holding the button for
ten seconds still kills the board. A device that cannot be switched off without
pulling the cell is worse than one with an untidy shutdown path.

## 5. Waking

Nothing to implement. A short press re-powers the rails in hardware and the ESP32
boots normally. Because no firmware is involved, waking cannot fail the way sleeping
could.

Plugging in USB also powers the board on — VBUS insertion turns the PMU on. That is
the chip's behaviour and it is the right one: a device on a charger should be awake.

## 6. Sleeping while plugged into USB

`shutdown()` with VBUS present is likely to power straight back on, because the
AXP2101 treats VBUS insertion as a power-on event. Rather than pretend otherwise,
the request is **refused** when USB is present, and the log says why:

```
sleep: ignored, USB is connected
```

`DeviceStatus::batteryUsbPresent` already carries what that test needs, from Phase 4.

## 7. Testing

**No new host tests.** This is I2C register access, a latch, and a call into the PMU
that turns the board off — the same reason `Pmu`, `GpsReceiver` and the displays have
none. A test here would assert that a bool can be set.

The gate is:

- `pio run -e esp` and `pio run -e tbeam` both build.
- `pio test -e native` still reports 118.
- `Pmu.cpp` stays out of `[env:native]`'s `build_src_filter`.

**On hardware:** a long press logs, shows `SLEEPING`, and the board goes dark; a
short press brings it back; the mode button is unaffected; a long press on USB is
refused; and the GPS reaches a fix quickly after waking rather than cold-starting.

**Worth measuring once:** the current draw while off. The design claims tens of
microamps and that number has never been checked on this board.

## 8. Success criteria

- [ ] Long press on the power key powers the board down, after stopping the radio
      and showing `SLEEPING`.
- [ ] Short press brings it back, with no firmware involvement in the wake.
- [ ] A long press while USB is connected is refused and logged.
- [ ] The hardware 10 s off-timer still works if firmware is wedged.
- [ ] The mode button on GPIO0 behaves exactly as before.
- [ ] Time to first fix after a wake is measured and written into
      `docs/hardware-notes.md`, whichever way it comes out.
- [ ] DevKitC builds and behaves exactly as before; 118 host tests still pass.

## 9. Risks

**The 1 Hz poll adds up to a second of latency** between releasing the button and
the screen changing. Acceptable for a shutdown, and the alternative — an interrupt
handler that talks to an I2C device — is worse.

**`XPOWERS_AXP2101_PKEY_LONG_IRQ` fires at a threshold set by the chip, not by us.**
It is well under the 4 s minimum off-time, so firmware wins the race, but the exact
value is from the datasheet rather than measured here. If the board ever cuts power
before `SLEEPING` appears, the hardware timer won and the two are closer than
assumed.

**Nothing verifies the board actually stays off.** If `shutdown()` does not hold on
this hardware — a rail that re-triggers power-on, for instance — the symptom is a
boot loop on sleeping rather than a quiet board, and the fix is the sequence, not
the trigger.
