#!/usr/bin/env python3
"""Decode what the device actually puts on the wire for RaceChrono.

RaceChrono is a black box: it either shows a lock or it does not, and it never
says why. This subscribes to the same two characteristics it does and prints
every notification decoded, so a disagreement between what we believe we are
sending and what is actually sent becomes visible.

    pip install bleak
    python3 tools/racechrono_probe.py            # finds a device advertising 0x1FF8
    python3 tools/racechrono_probe.py teletrack  # or match on name

Protocol: docs/reference/racechrono/PROTOCOL.md. All fields big-endian.
"""
import asyncio
import struct
import sys

from bleak import BleakClient, BleakScanner

SERVICE = "00001ff8-0000-1000-8000-00805f9b34fb"
GPS_MAIN = "00000003-0000-1000-8000-00805f9b34fb"
GPS_TIME = "00000004-0000-1000-8000-00805f9b34fb"

state = {"main_sync": None, "time_sync": None, "count": 0}


def decode_main(d: bytes) -> str:
    if len(d) != 20:
        return f"WRONG LENGTH {len(d)}, expected 20: {d.hex()}"
    sync = d[0] >> 5
    t = ((d[0] & 0x1F) << 16) | (d[1] << 8) | d[2]
    minute, rem = divmod(t, 30000)
    second, rem = divmod(rem, 500)
    millis = rem * 2

    quality, sats = d[3] >> 6, d[3] & 0x3F
    lat, lon = struct.unpack(">ii", d[4:12])
    alt_raw, spd_raw, bearing = struct.unpack(">HHH", d[12:18])
    alt = (alt_raw & 0x7FFF) - 500 if alt_raw & 0x8000 else (alt_raw & 0x7FFF) / 10 - 500
    spd = (spd_raw & 0x7FFF) / 10 if spd_raw & 0x8000 else (spd_raw & 0x7FFF) / 100
    hdop = "invalid" if d[18] == 0xFF else f"{d[18] / 10:.1f}"

    state["main_sync"] = sync
    flags = []
    if quality == 0:
        flags.append("!! quality 0 = NO FIX, RaceChrono will not lock")
    if sats == 0x3F:
        flags.append("!! satellites = 0x3F, the invalid marker")
    elif sats == 0:
        flags.append("!! 0 satellites")
    if lat == 0x7FFFFFFF or lon == 0x7FFFFFFF:
        flags.append("!! position is the invalid marker")
    if minute > 59 or second > 59:
        flags.append(f"!! impossible time {minute}:{second}")

    return (
        f"MAIN sync={sync} {minute:02d}:{second:02d}.{millis:03d} "
        f"quality={quality} sats={sats} "
        f"lat={lat / 1e7:.6f} lon={lon / 1e7:.6f} "
        f"alt={alt:.1f}m spd={spd:.2f}km/h brg={bearing / 100:.1f} hdop={hdop}"
        + ("\n     " + "\n     ".join(flags) if flags else "")
    )


def decode_time(d: bytes) -> str:
    if len(d) != 3:
        return f"WRONG LENGTH {len(d)}, expected 3: {d.hex()}"
    sync = d[0] >> 5
    dh = ((d[0] & 0x1F) << 16) | (d[1] << 8) | d[2]
    year, rem = divmod(dh, 8928)
    month, rem = divmod(rem, 744)
    day, hour = divmod(rem, 24)
    state["time_sync"] = sync

    flags = []
    if dh == 0:
        flags.append("!! date/hour is 0 = year 2000, the receiver has no time")
    if state["main_sync"] is not None and state["main_sync"] != sync:
        flags.append(
            f"!! sync mismatch: main={state['main_sync']} time={sync}. "
            "RaceChrono waits for these to agree before using either."
        )
    return (
        f"TIME sync={sync} {2000 + year:04d}-{month + 1:02d}-{day + 1:02d} {hour:02d}:00"
        + ("\n     " + "\n     ".join(flags) if flags else "")
    )


async def main() -> int:
    want = sys.argv[1] if len(sys.argv) > 1 else None
    print("scanning...")
    found = None
    for d, adv in (await BleakScanner.discover(timeout=8.0, return_adv=True)).values():
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        if SERVICE in uuids or (want and want.lower() in (d.name or "").lower()):
            found = d
            print(f"found {d.name or '(no name)'} [{d.address}] uuids={uuids}")
            break
    if not found:
        print("no device advertising 0x1FF8. Is it in BLE mode? Is something else connected?")
        return 1

    async with BleakClient(found) as client:
        chars = {c.uuid.lower() for s in client.services for c in s.characteristics}
        for name, uuid in (("GPS main 0x0003", GPS_MAIN), ("GPS time 0x0004", GPS_TIME)):
            print(f"{name}: {'present' if uuid in chars else 'MISSING'}")

        def on_main(_, data):
            state["count"] += 1
            print(decode_main(bytes(data)))

        def on_time(_, data):
            print(decode_time(bytes(data)))

        await client.start_notify(GPS_MAIN, on_main)
        await client.start_notify(GPS_TIME, on_time)
        print("subscribed. ctrl-c to stop.\n")
        try:
            while True:
                await asyncio.sleep(5)
                if state["count"] == 0:
                    print("... no GPS notifications yet. The device sends nothing "
                          "until the receiver reports a valid date AND time.")
        except asyncio.CancelledError:
            pass
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print(f"\n{state['count']} main notifications decoded")
