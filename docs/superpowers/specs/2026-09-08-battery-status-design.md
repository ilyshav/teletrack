# Phase 4 — Battery charging and status

**Date:** 2026-09-08
**Status:** Draft, awaiting review

## 1. Goal

Charge the 18650 properly, and show its state of charge on the screen and in the
web UI. T-Beam Supreme only — the DevKitC has no PMU and no battery.

### Out of scope

- **Sleep mode.** That is Phase 5, and it needs a second button. This phase
  deliberately leaves the OLED's one spare row free for it.
- **Low-battery behaviour** — no auto-shutdown, no warning threshold, no logging
  of charge history. Nothing has asked for them.
- **Making the currents configurable.** They are two constants; the web UI does
  not need a knob for a value that is set once for a known cell.

## 2. There is currently no way to read the battery

`Pmu::begin()` sets the charge voltage and current and enables cell charging.
That is all it does. It never calls `enableBattDetection()`, and it enables no ADC
channel — so `isBatteryConnect()` reads false, and both `getBattVoltage()` and
`getBatteryPercent()` short-circuit on that and return 0 and -1.

The charging that happens today is the AXP2101 doing it in hardware. The firmware
cannot see any of it.

## 3. Charging

Three things are wrong or missing today.

**The input limit is never set.** `setVbusCurrentLimit()` caps what the board draws
from USB in total — charging *plus* running. Whatever it powers up as is what we
get. Set to **1500 mA**, which leaves room for a full 1 A of charging alongside the
150-250 mA the board draws with GPS, display and a radio running.

**The charge current is too low for the cell.** 500 mA into an 18650 is about
0.17C. The standard rate is 0.5C. Set to **1000 mA** — the chip's maximum, 0.5C for
a 2000 mAh cell and 0.33C for a 3000 mAh one, so it is safe across any 18650 likely
to be fitted. With the input limit above, a full charge takes 3-4 hours while
running rather than most of a day.

If the supply cannot deliver it, the AXP2101's input undervoltage loop backs off by
itself and charging simply slows. Nothing needs to detect this.

**The charge profile ends at defaults.** Precharge and termination currents come
from whatever the chip powers up with. Both are set explicitly, for the same reason
the 4.2 V target already is: an inherited value is not a chosen one.

The target voltage stays **4.2 V**. The part also accepts 4.35 V and 4.4 V, which
suit high-voltage cells and would overcharge a standard 18650.

`setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG)` hands the onboard LED to the
charger, so it reports charge state as a hardware function — true even if the
firmware hangs.

## 4. Reading it

```
src/board/Pmu.h    BatteryState, Pmu::battery(), Pmu::tick(uint32_t)
```

```cpp
struct BatteryState {
  bool present = false;      // a cell is fitted
  bool usbPresent = false;   // VBUS is up
  bool charging = false;
  bool full = false;         // charger reports done
  uint8_t percent = 0;       // 0 when unknown
  uint16_t milliVolts = 0;   // 0 when unknown
};
```

`battery()` returns a cached copy. `tick(nowMs)` refreshes it **once a second**.

The cache is the point. These are I2C register reads on Wire1, and `loop()` runs at
roughly 1 kHz; polling the PMU every pass would put five register reads on the hot
path for a value that moves over minutes. One refresh per second is far more often
than a percentage changes, and it keeps the I2C bus free for the display.

`begin()` also enables what makes those reads work: `enableBattDetection()`,
`enableBattVoltageMeasure()`, `enableVbusVoltageMeasure()`.

## 5. The header

Battery on the left, mode on the right. Uptime goes: on a track day the charge
level matters more than how long the board has been powered, and serial keeps full
timestamps, which is where uptime was actually readable.

```
+---------------------+
|87%+ 4.05V        BLE|   <- header, inverse video
|SATS 09       3D FIX |
|LAT  52.37134        |
|LON   4.89521        |
|ALT   12m DOP 0.9    |
|SPD  48.2 km/h       |
|                     |   <- stays free for Phase 5
+---------------------+
```

One character between the percentage and the voltage carries the charge state:

| Left side | Meaning |
| --- | --- |
| `87%+ 4.05V` | charging |
| `87%- 4.05V` | running on the cell |
| `100%= 4.20V` | charged, charger reports done |
| `USB` | no cell fitted, running on USB |

`USB` is not a corner case: a T-Beam on the bench with no cell is how most of this
gets tested, and `0%- 0.00V` would be a lie.

### The right side is the radio, and nothing else

`BLE` or `AP`. Not whether a client is connected, not how many, not how many
packets were dropped. Which radio is running is a fact about the device; the rest
is a fact about someone else's phone, and it is read with a browser open, not at a
glance across a paddock.

All of it stays in `DeviceStatus` and is served by `/api/status`.

**The one exception is the mode-button countdown**, which replaces the radio name
while the button is held:

```
|87%+ 4.05V    HOLD 3s|
```

That is not a connection detail — it is the only feedback that a three-second hold
is registering at all. Without it the button feels broken.

Widths: the longest left side is `100%= 4.20V` at 11 columns, the longest right is
`HOLD 3s` at 7. Worst case 11 + 7 plus separation is **20 of 21**, and in normal
running it is 11 + 3.## 6. Elsewhere

`DeviceStatus` carries the same fields, so `GET /api/status` and the web UI get
them without further work — it is the same struct the display reads.

The DevKitC is untouched. Its `Pmu` is already a no-op stub; it gains a `battery()`
returning a default-constructed `BatteryState` (`present` and `usbPresent` false)
and a `tick()` that does nothing. `TftDisplay` is not modified.

## 7. Testing

**No new host tests.** `Pmu` is I2C register access and the header change is a
format string — the same reason `TftDisplay`, `OledDisplay` and `GpsReceiver` have
none. A test here would assert that `snprintf` works.

The gate is:

- `pio run -e esp` and `pio run -e tbeam` both build.
- `pio test -e native` still reports 116.
- `Pmu.cpp` stays out of `[env:native]`'s `build_src_filter`.

**On hardware:** the percentage tracks a real charge and discharge; `+` appears on
USB and `-` on the cell; pulling the cell shows `USB`; the onboard charge LED
agrees with the header.

## 8. Success criteria

- [ ] Battery detection and the voltage/VBUS ADC channels are enabled.
- [ ] VBUS limit 1500 mA, charge current 1000 mA, precharge and termination set
      explicitly, target still 4.2 V.
- [ ] `Pmu::battery()` returns a cache refreshed at 1 Hz, not a live I2C read.
- [ ] Header shows percentage, charge state and voltage in all four cases of §5,
      with the radio on the right and nothing exceeding 21 columns.
- [ ] `/api/status` carries the same fields.
- [ ] The OLED's spare row is still spare.
- [ ] DevKitC builds and behaves exactly as before; 116 host tests still pass.

## 9. Risks

**The percentage comes from the AXP2101's own fuel gauge**, which is a
voltage-based estimate, not coulomb counting. It will jump under load and settle
when idle. That is the chip's behaviour, not a bug to chase — and it is why voltage
is carried alongside the percentage in `BatteryState` even though the header shows
only the percentage.

**1500 mA of input limit assumes a supply that can provide it.** If it cannot --
an old USB 2.0 hub, say -- the 5 V rail droops under the load. The AXP2101 has an
input undervoltage loop that reduces what it draws when that happens, so it
self-corrects; the symptom is charging that is slow or stops and starts, and at
worst the ESP32's own brownout detector resetting the board while it is plugged
into that port. Unplugging clears it.

Nothing is damaged and the cell is never at risk: the PMU sits between USB and the
battery and is the thing backing off. The failure mode is slow charging, not heat.

If it does misbehave on a particular port,
`XPOWERS_AXP2101_VBUS_CUR_LIM_500MA` is the single line to change.
