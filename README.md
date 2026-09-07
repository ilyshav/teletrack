# teletrack

DIY trackday telemetry logger. ESP32-S3 with a u-blox M10N GPS module and a 320×240
ILI9341 display.

On the T-Beam Supreme, the firmware reads the board's onboard u-blox MAX-M10S over
UBX-NAV-PVT at the rate set in the web UI, and reports satellite count and fix state
on the OLED and to RaceChrono over BLE. The DevKitC has no receiver attached, so it
emits a synthetic fix instead, keeping BLE testable indoors.

## Status

**Phase 1 — configuration portal.** The device opens an open WiFi access point,
serves a captive-portal web UI for editing settings, persists them to NVS, and shows
a status header and scrolling log on the TFT. GPS and session logging are not built
yet.

Design: `docs/superpowers/specs/2026-09-06-phase1-config-portal-design.md`
Plan: `docs/superpowers/plans/2026-09-06-phase1-config-portal.md`

## Hardware notes

`docs/hardware-notes.md` collects the platform landmines this board has already cost
time on — boot loops from the partition table, build flags that silently do nothing,
the TFT SPI-port crash, which GPIOs are unusable, why reading the serial port yourself
returns nothing, and the BLE advertisement limits. Symptom first, since that is how you
arrive there.

## Building

| Command | What it does |
| --- | --- |
| `pio run -e esp` | Build the firmware |
| `pio run -e esp -t upload` | Build and flash |
| `pio device monitor` | Serial log at 115200 |
| `pio test -e native` | Host unit tests, no hardware needed |
| `pio test -e esp -f embedded/test_nvs` | On-device tests |

## Using it

Power the board, join the open `teletrack` WiFi network, and the configuration page
opens by itself. If it does not, browse to `http://192.168.4.1/`.

## Layout

```
src/core/     logging, formatting, the DeviceStatus struct — no feature knowledge
src/config/   the configuration module: settings, storage, AP, captive portal, HTTP
src/ui/       the display and its log buffer
data/ui/      the web UI source; gzipped into a C header at build time
```

`src/config/` is cut by feature, not by layer: WiFi, DNS, HTTP and persistence all
exist to configure the device, so they live together behind `ConfigPortal`. Only
`Settings`, `SettingsStore` and `ConfigPortal` are public — a
future GPS module includes `config/Settings.h` to read the sample rate without
pulling in the network stack.

Each module splits pure logic from I/O — `Settings`/`NvsStore`, `ConfigApi`/`WebUi`,
`LogRing`/`Display` — so the logic runs in host tests with no hardware attached.
`platformio.ini`'s `build_src_filter` under `[env:native]` is the enforcement
mechanism: a file listed there that fails to compile has an Arduino dependency it
should not have.
