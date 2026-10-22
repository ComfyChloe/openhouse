#!/usr/bin/env python3
"""Decode Lighthouse 1.0 parameter partitions (stock cparam.c format) from an EEPROM image.

EEPROM layout: partitions of {u32 'CPRM', u32 version, u32 crc32(payload)} + payload.
The payload is the concatenation, in parameter-table order, of every parameter whose
flags bit0 is clear (bit0 = runtime-only).  The table is read from the stock firmware:
32-byte records {type, ram_addr, name*, default*, size|flags<<8, ?, description*, enum*}.

usage: params_decode.py EEPROM.bin [STOCK_FW.bin] [--diff]
  --diff  only print values that differ from the firmware defaults
"""
import struct, sys, zlib

FW_DEFAULT = 'lighthouse_tx_htc_2_0-436-2016-09-20.bin'
TABLE_START, TABLE_COUNT = 0xECF0, 185   # ends at 0x10410 (all-zero record)


def cstr(fw, a):
    if not 0 < a < len(fw):
        return ''
    return fw[a:fw.find(b'\0', a)].decode('latin1')


def load_table(fw):
    params = []
    for i in range(TABLE_COUNT):
        typ, ram, name, dflt, w4, w5, desc, enum = struct.unpack_from('<8I', fw, TABLE_START + 32 * i)
        params.append(dict(name=cstr(fw, name), type=typ, size=w4 & 0xff, flags=(w4 >> 8) & 0xff,
                           default=cstr(fw, dflt), desc=cstr(fw, desc), enum=enum, ram=ram))
    return params


def enum_names(fw, addr):
    """Enum tables are null-terminated arrays of string pointers."""
    names = []
    while addr and (a := struct.unpack_from('<I', fw, addr + 4 * len(names))[0]):
        names.append(cstr(fw, a))
    return names


def decode_value(fw, p, raw):
    t, n = p['type'], p['size']
    if t == 1:
        return 'true' if raw[0] else 'false'
    if t == 8 and n == 4:
        return f"{struct.unpack('<f', raw)[0]:.6g}"
    if t == 10:
        return repr(raw.split(b'\0')[0].decode('latin1'))
    if t == 7 and n == 4:
        return str(struct.unpack('<i', raw)[0])
    if t == 5 and n == 1:
        return str(struct.unpack('<b', raw)[0])
    if n in (1, 2, 4):
        v = int.from_bytes(raw, 'little')
        if t == 13 and p['enum']:
            # enum tables are arrays of 20-byte records whose first word is a name pointer
            names = enum_names(fw, p['enum'])
            return names[v] if v < len(names) else f"{v} (out of range)"
        if p['name'].endswith('label') and n == 1:
            return chr(v)
        return str(v)
    return raw.hex()


def same_value(val, p):
    d = p['default']
    if p['name'].endswith('label'):
        return d.isdigit() and val == chr(int(d))
    try:
        return float(val) == float(d)
    except ValueError:
        return val.strip("'") == d


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    diff_only = '--diff' in argv
    if not args:
        sys.exit(__doc__)
    ee = open(args[0], 'rb').read()
    fw = open(args[1] if len(args) > 1 else FW_DEFAULT, 'rb').read()
    params = load_table(fw)
    stored = [p for p in params if not p['flags'] & 1]
    plen = sum(p['size'] for p in stored)
    print(f"parameter table: {len(params)} entries, {len(stored)} stored, payload {plen} bytes")

    off = 0
    while (off := ee.find(b'CPRM', off)) >= 0:
        ver, crc = struct.unpack_from('<2I', ee, off + 4)
        payload = ee[off + 12: off + 12 + plen]
        ok = zlib.crc32(payload) == crc
        print(f"\n=== partition @0x{off:03X}  version {ver}  crc {crc:#010x} {'OK' if ok else 'BAD'} ===")
        pos = 0
        for p in stored:
            raw = payload[pos:pos + p['size']]
            pos += p['size']
            if p['type'] == 12:          # reserved storage slot
                continue
            val = decode_value(fw, p, raw)
            same = same_value(val, p)
            if diff_only and same:
                continue
            mark = '' if same else f"   (default {p['default']})"
            print(f"  {p['name']:28s} = {val}{mark}")
        off += 4


if __name__ == '__main__':
    main(sys.argv)
