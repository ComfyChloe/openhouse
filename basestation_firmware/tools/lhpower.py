#!/usr/bin/env python3
"""Lighthouse 1.0 (HTC Vive) base station power management over BLE - the part
SteamVR does not do on Linux.

    lhpower.py scan
    lhpower.py sleep|wake ADDRESS BASE_ID_HEX
    lhpower.py sleep|wake all            (every 'HTC BS' found; IDs from SteamVR's DB)

BASE_ID is the 8-hex-digit base station ID (= OOTX serial, e.g. A101B835 for
SteamVR's LHB-A101B835). The command is a 20-byte write to characteristic
0000cb01 (service 0000cb00): 12 <00 wake | 02 sleep> <u16 BE> <ID u32 LE> 00*12.
Observed on hardware: the base station's Broadcom BLE chip then types
"param set sys.standby 0|1" into the main MCU's UART console once per second.

Never touches the Broadcom OTA firmware-update service (9e5d1e47-...).
"""
import asyncio, json, os, struct, sys
from bleak import BleakClient, BleakScanner

CHAR = "0000cb01-0000-1000-8000-00805f9b34fb"
DB = os.path.expanduser("~/.local/share/Steam/config/lighthouse/lighthousedb.json")


def command(action, base_id):
    if action == "wake":
        head = bytes([0x12, 0x00, 0x00, 0x00])
    else:
        head = bytes([0x12, 0x02, 0x00, 0x01])
    return head + struct.pack("<I", base_id) + bytes(12)


async def scan(seconds=8):
    found = await BleakScanner.discover(timeout=seconds)
    return [(d.address, d.name) for d in found if d.name and d.name.startswith("HTC BS")]


def known_ids():
    """Base IDs SteamVR has seen, to match the 'HTC BS xxyyzz' name suffix."""
    try:
        j = json.load(open(DB))
        return [b["config"]["serialNumber"] for b in j.get("base_stations", [])]
    except Exception:
        return []


def id_for_name(name, ids):
    tail = name.split()[-1][-4:].upper()      # name ends in the ID's low 16 bits
    for i in ids:
        if f"{i:08X}".endswith(tail):
            return i
    return None


async def send(addr, action, base_id):
    async with BleakClient(addr, timeout=20) as c:
        await c.write_gatt_char(CHAR, command(action, base_id), response=True)
        print(f"{addr}: {action} sent to base {base_id:08X}")


async def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[1] == "scan":
        ids = known_ids()
        for addr, name in await scan():
            i = id_for_name(name, ids)
            print(f"{addr}  {name}  id {f'{i:08X}' if i else '? (not in SteamVR DB)'}")
        return
    action = argv[1]
    if action not in ("sleep", "wake"):
        sys.exit(__doc__)
    if len(argv) >= 3 and argv[2] == "all":
        ids = known_ids()
        for addr, name in await scan():
            i = id_for_name(name, ids)
            if i is None:
                print(f"{addr} {name}: unknown ID, skipped (pass it explicitly)")
                continue
            await send(addr, action, i)
        return
    if len(argv) < 4:
        sys.exit(__doc__)
    await send(argv[2], action, int(argv[3], 16))


if __name__ == "__main__":
    asyncio.run(main(sys.argv))
