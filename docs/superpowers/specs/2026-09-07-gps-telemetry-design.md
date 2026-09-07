# Real GPS telemetry — u-blox MAX-M10S on the T-Beam Supreme

**Date:** 2026-09-07
**Status:** Draft, awaiting review

## 1. Goal

Replace the synthetic fix with the T-Beam Supreme's onboard u-blox MAX-M10S, feed
real position, speed and **satellite lock state** to RaceChrono, and put GPS status
on the OLED in place of the log.

RaceChrono currently reports "no satellite lock" against the synthetic fix even
though that fix claims `fixQuality = 1` and 8 satellites. The encoder has been
checked byte-for-byte against the reference and matches. The most likely remaining
cause is the synthetic clock: it starts at a hardcoded 2026-09-07 12:00 UTC and
advances on `millis()`, so it disagrees with the phone's clock by hours. A real
receiver reports real UTC, which is why this work is expected to resolve it. **This
is a hypothesis, not a diagnosis** — §9 says how we will know.

### Out of scope

- **GPS on the ESP32-S3-DevKitC-1.** No module is attached. It keeps the synthetic
  fix so BLE and RaceChrono stay testable indoors, and its TFT keeps today's log
  view. No change to that board.
- **PPS, LoRa, IMU, SD card, magnetometer, RTC.** Present, unused.
- **The BLE mode-switch crash** (`InstrFetchProhibited` in `NimBLEDevice::host_task`
  during `deinit`). Real and unfixed, tracked separately.
- **UBX-NAV-DOP.** See §5 on HDOP.

## 2. Hardware facts

Every value below comes from LilyGO's own board support, not from guesswork —
`examples/.../utilities.h` and `LoRaBoards.cpp` in Xinyuan-LilyGO/LilyGo-LoRa-Series.

| | Value |
| --- | --- |
| GPS UART RX (ESP32 side) | GPIO **9** |
| GPS UART TX (ESP32 side) | GPIO **8** |
| GPS enable | GPIO 7 — vendor code never drives it; we do not either |
| GPS PPS | GPIO 6 — unused |
| **GPS power rail** | **ALDO4 @ 3300 mV** |
| GNSS RTC backup | **VBACKUP @ 3300 mV** — without it every start is a cold start |

### The rail comments in `Pmu.cpp` are wrong and get corrected

Today `Pmu::begin()` sets ALDO2 labelled "display" and ALDO3 labelled "GPS". On the
Supreme:

```
ALDO4   = GPS          <- we never touch it today
ALDO3   = LoRa
ALDO1   = sensors
ALDO2   = SD card
VBACKUP = GNSS RTC backup
DCDC1   = ESP32 VDD, protected, never disable
```

Neither of the rails we set has anything to do with the display — consistent with
the earlier finding that the dark panel was an I²C address problem, never a power
one. This spec enables **ALDO4 and VBACKUP** and fixes the two labels.

## 3. Module structure

```
src/core/GpsFix.h          GpsFix moves here from ble/RaceChronoGps.h
src/gps/UbxParser.{h,cpp}  pure: bytes in, GpsFix out. Host-tested.
src/gps/GpsReceiver.{h,cpp} Arduino shell: UART, power, configuration.
```

The split follows the pattern already in the codebase. `UbxParser` has no Arduino
dependency and joins `[env:native]`'s `build_src_filter`, so the framing, checksum
and field decoding get real host tests — the same treatment `RaceChronoGps` gets,
and for the same reason: it is fiddly, and wrong answers look plausible.
`GpsReceiver` is an I/O shell with no logic and gets no tests, like the displays.

`GpsFix` moves to `src/core/` because `DeviceStatus` now carries one, and
`core/` must not depend on `ble/`.

## 4. Talking to the M10

**UBX-NAV-PVT, not NMEA.** One 92-byte message carries the entire fix atomically:
position, height, ground speed, heading, pDOP, `fixType`, `numSV`, and UTC with
explicit validity flags. NMEA would need GGA + RMC + GSA stitched together, and
those can disagree mid-update. Since the clock is the prime suspect in §1, a
receiver that states outright whether its time is valid is worth the parser.

**All UBX fields are little-endian.** RaceChrono's payloads are big-endian. The two
encodings sit in adjacent modules; this is the single easiest thing to get wrong.

### Frame format

```
B5 62 | class | id | len_lo len_hi | payload[len] | CK_A CK_B
```

Checksum is 8-bit Fletcher over class, id, both length bytes and the payload:

```
CK_A = 0; CK_B = 0;
for each byte b:  CK_A += b;  CK_B += CK_A;    // uint8_t, wrapping
```

### UBX-NAV-PVT (class 0x01, id 0x07, len 92) — fields we read

| Offset | Type | Field | Use |
| --- | --- | --- | --- |
| 4 | U2 | year | `GpsFix::year` |
| 6 | U1 | month | `GpsFix::month` |
| 7 | U1 | day | `GpsFix::day` |
| 8 | U1 | hour | `GpsFix::hour` |
| 9 | U1 | min | `GpsFix::minute` |
| 10 | U1 | sec | `GpsFix::seconds` |
| 11 | X1 | valid | bit0 validDate, bit1 validTime, bit2 fullyResolved |
| 16 | I4 | nano | nanoseconds, signed; → `millis` (see below) |
| 20 | U1 | fixType | 0 none, 1 DR, 2 = 2D, 3 = 3D, 4 GNSS+DR, 5 time only |
| 21 | X1 | flags | bit0 gnssFixOK, bit1 diffSoln |
| 23 | U1 | numSV | `GpsFix::satellites` |
| 24 | I4 | lon | 1e-7 deg → `lonE7` directly |
| 28 | I4 | lat | 1e-7 deg → `latE7` directly |
| 36 | I4 | hMSL | mm above mean sea level → `altitudeM` = /1000 |
| 60 | I4 | gSpeed | mm/s → `speedKmh` = × 0.0036 |
| 64 | I4 | headMot | 1e-5 deg → `bearingDeg` = /100000 |
| 76 | U2 | pDOP | ×0.01 → `hdop` (see §5) |

`nano` is signed and can be negative (the second boundary is the reported second,
with `nano` an offset either side of it). `millis` is `nano / 1000000` clamped to
`[0, 999]`; a negative `nano` yields 0.

### Configuration at boot

Sent as **UBX-CFG-VALSET (0x06 0x8A)** to the **RAM layer only** (`layers = 0x01`).
RAM means the module reverts to its own defaults on power cycle, so we reconfigure
every boot — no flash wear, and nothing about the module is left permanently
altered. Key IDs are from SparkFun's `u-blox_config_keys.h`:

| Key | ID | Type | Value |
| --- | --- | --- | --- |
| CFG-UART1-BAUDRATE | 0x40520001 | U4 | 115200 |
| CFG-MSGOUT-UBX_NAV_PVT_UART1 | 0x20910007 | U1 | 1 |
| CFG-MSGOUT-NMEA_ID_GGA_UART1 | 0x209100bb | U1 | 0 |
| CFG-MSGOUT-NMEA_ID_GLL_UART1 | 0x209100ca | U1 | 0 |
| CFG-MSGOUT-NMEA_ID_GSA_UART1 | 0x209100c0 | U1 | 0 |
| CFG-MSGOUT-NMEA_ID_GSV_UART1 | 0x209100c5 | U1 | 0 |
| CFG-MSGOUT-NMEA_ID_RMC_UART1 | 0x209100ac | U1 | 0 |
| CFG-MSGOUT-NMEA_ID_VTG_UART1 | 0x209100b1 | U1 | 0 |
| CFG-RATE-MEAS | 0x30210001 | U2 | 1000 / rate |
| CFG-RATE-NAV | 0x30210002 | U2 | 1 |

**Baud must be raised.** NAV-PVT is 100 bytes on the wire; at the default 10 Hz that
is 1000 B/s, and 9600 baud 8N1 carries 960 B/s. It does not fit. 115200 carries
11520 B/s, enough for any rate the setting allows.

**We do not parse ACKs.** Success is proven by NAV-PVT frames arriving, which is
stronger evidence than an ACK and much less code. The receiver logs
`gps: NAV-PVT at <n> Hz` on the first decoded frame.

### Baud detection

The module's current baud is genuinely unknown: LilyGO's header says 9600, u-blox
M10 modules default to 38400, and LilyGO ship a recovery sketch that sweeps
`{9600, 19200, 38400, ...}` precisely because it varies. We also set 115200 in RAM
ourselves, which survives a warm reset but not a power cycle.

So `GpsReceiver::begin()` tries **115200, then 38400, then 9600**, opening each for
250 ms and looking for either a UBX sync pair (`B5 62`) or an NMEA `$`. The first
that produces bytes wins and is logged. If none do, it logs
`gps: no response at any baud` and the rest of the firmware carries on — a dead
receiver must not take BLE down with it.

This is fifteen lines that removes the single most likely "the GPS is dead" dead
end, on a project that has already lost days to exactly this class of ambiguity.

## 5. Mapping NAV-PVT onto RaceChrono

`fixType` → RaceChrono's 2-bit fix quality:

| NAV-PVT | RaceChrono | Meaning |
| --- | --- | --- |
| 0, 1, 5 | 0 | no usable fix |
| 2, 3, 4 with `diffSoln` | 2 | differential |
| 2, 3, 4 | 1 | GPS fix |

`gnssFixOK` (flags bit 0) must also be set; if it is clear the quality is 0
regardless of `fixType`.

**HDOP is reported as pDOP.** NAV-PVT carries position DOP, not horizontal DOP.
`pDOP >= hDOP` always, so this is pessimistic rather than misleading, and RaceChrono
uses the value for quality weighting only. Getting true HDOP means enabling
UBX-NAV-DOP as a second message and correlating the two — not worth it here. The
substitution is commented at the assignment.

**Satellites are always reported**, fix or no fix. Before a lock this is exactly
what tells you the receiver is alive and working, and it is what the screen shows.

### We withhold packets until the receiver's clock is valid

Unlike the reference — which transmits whatever its parser last produced — nothing
is sent to RaceChrono until `validDate && validTime` are both set. A GPS timestamp
is the axis every sample is placed on, and feeding RaceChrono a wrong one is the
leading hypothesis for the bug in §1. The receiver's time goes valid well before it
has a position fix, so this costs nothing in practice. Until then the display shows
acquisition progress and BLE stays quiet.

## 6. Sample rate follows the setting

`Settings::sampleHz` is already in the web UI and validated to `{1, 5, 10, 25}`, but
nothing reads it — the BLE path uses a hardcoded `constexpr kSampleHz = 5`. That
constant is deleted. The configured rate becomes `CFG-RATE-MEAS = 1000 / sampleHz`,
and the firmware sends one RaceChrono packet per NAV-PVT frame, so the receiver's
own rate is the sample rate. No timer, no resampling.

**25 Hz is clamped to 10 Hz** and logged. The MAX-M10S supports 25 Hz only with a
single constellation; at the multi-GNSS default its ceiling is 10 Hz. Silently
running at the wrong rate would be worse than saying so.

A rate change takes effect on the next radio restart, the same path a device rename
already uses.

The existing rule that nothing is produced unless a client is connected stays: a
fix that nobody was listening for is not worth buffering, and the ring must never
be able to replay a minute of history at a client the moment it connects.

## 7. The OLED becomes a status screen

The log rows go. Serial keeps every line, with timestamp and level, which is where
log history is actually readable.

**With a fix:**

```
+---------------------+
|BLE CONN     00:12:34|  inverse header, unchanged
|SATS 09       3D FIX |
| 52.371340   4.895210|
|ALT   12m   PDOP 0.9 |
|SPD   48.2 km/h      |
|UTC 14:23:11         |
|                     |
+---------------------+
```

**Before a fix** — the case that matters, because it is the one you stare at:

```
+---------------------+
|BLE ADV      00:00:47|
|SATS 03      NO FIX  |
|ACQUIRING            |
|                     |
|                     |
|UTC 14:23:11         |
|                     |
+---------------------+
```

**With no receiver responding:**

```
|SATS --      NO GPS  |
```

Satellite count is on the second row, left, in every state: it is the number that
answers "is this thing working yet". `DeviceStatus` grows a `GpsFix lastFix`, a
`bool gpsFixValid` and a `bool gpsPresent`; the display reads only those.

The DevKitC's TFT is not touched.

## 8. Testing

**Host tests for `UbxParser`** — the only part with logic worth asserting:

- A correct NAV-PVT frame decodes every field in the §4 table to known values.
- A frame with one flipped payload byte is rejected by the checksum.
- Framing resyncs after leading garbage, and after a truncated frame.
- A frame split across three `feed()` calls decodes identically to one call.
- `fixType`/`gnssFixOK`/`diffSoln` produce each of the three quality values in §5.
- `valid` with `validTime` clear yields a fix marked time-invalid.
- Negative `nano` yields `millis == 0`; `nano = 999_999_999` yields 999.

**The existing 95 host tests keep passing**, and `pio run -e esp` keeps building —
the DevKitC path must not regress.

**On hardware:** cold start reaches a 3D fix outdoors; the screen shows the
satellite count climbing before lock; RaceChrono shows a satellite lock and records
a session with plausible speed and position.

## 9. How we will know whether this fixed §1

If RaceChrono locks: the synthetic clock was the cause, and it is gone.

If it still reports no lock **with a real 3D fix, valid UTC and 9 satellites on
screen**, then the payload reaches RaceChrono intact and the fault is in the BLE
layer, not the data. At that point the next step is not another guess: it is a
laptop-side BLE client that subscribes to 0x0003 and 0x0004 and decodes what is
actually on the wire. That tool was cut during Phase 2 simplification, and its
absence is why this bug has been guesswork so far.

## 10. Success criteria

- [ ] `UbxParser` decodes NAV-PVT, host-tested, no Arduino dependency.
- [ ] ALDO4 and VBACKUP enabled; the two wrong rail comments corrected.
- [ ] Baud detected and logged; a dead receiver does not stop BLE.
- [ ] Satellite count and fix state on the OLED, in all three states of §7.
- [ ] RaceChrono receives real fixes, and no packet is sent before UTC is valid.
- [ ] `Settings::sampleHz` drives the receiver's rate; 25 clamps to 10 and logs.
- [ ] `kSampleHz` and `buildSyntheticFix` are gone from the T-Beam path.
- [ ] DevKitC still builds and behaves exactly as before.
- [ ] 95 existing host tests still pass.

## 11. Risks

**The pin and rail values are vendor-documented but unverified on this desk.** They
were read from LilyGO's board support for this exact board, which is much better
than the guesses that preceded the display work, but the first flash is still where
they get confirmed. Each is one line in `BoardConfig.h`.

**GPIO 7 (`GPS_EN_PIN`) is left alone** because LilyGO's own code never drives it.
If the receiver is silent at every baud with ALDO4 confirmed on, this is the first
thing to try — before suspecting the parser.

**Indoors there is no fix.** Development and acceptance need a window or a walk
outside. The satellite count appearing at all is the first milestone and does
happen indoors near a window; a 3D fix generally does not.
