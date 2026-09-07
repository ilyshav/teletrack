# RaceChrono BLE DIY device — vendored reference

Fetched 2026-09-07 from https://github.com/aollin/racechrono-ble-diy-device (master).

| File | What it is |
| --- | --- |
| `PROTOCOL.md` | The repo's README: the authoritative API specification |
| `canbus-gps-device-main.ino` | Reference implementation (Adafruit Bluefruit / nRF52) |
| `canbus-gps-device-README.md` | That example's own README |

Vendored so the packet encoder can be ported from the reference rather than
reconstructed from prose. **Port the bit manipulation from
`canbus-gps-device-main.ino` verbatim** — the fine/coarse switchover for altitude
and speed, the sync-bit increment rule, and the big-endian byte order are all
easy to get subtly wrong from the spec text alone, and a wrong encoding produces
plausible-looking but incorrect data in RaceChrono.

The reference targets a different BLE stack (Bluefruit, not NimBLE), so the
service and characteristic *setup* differs. The **payload construction does not**
and should be copied.
