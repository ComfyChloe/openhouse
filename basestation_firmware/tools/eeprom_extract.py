#!/usr/bin/env python3
"""Extract the EEPROM snapshot written to flash sector 31 (0x1F000).
Formats: lhfw 0.3+/lhbs keep 4032 bytes at +0x40; lhfw 0.2 used +0x100 and
therefore only captured EEPROM 0..3839 (and its checksum is unreliable).
usage: eeprom_extract.py FLASH_READBACK.bin OUT_eeprom.bin"""
import struct, sys
if len(sys.argv) != 3:
    sys.exit(__doc__)
d = open(sys.argv[1], 'rb').read()
s = d[0x1F000:0x20000]
if s[:8] != b'LHEEDUMP':
    sys.exit("no LHEEDUMP header at 0x1F000")
length, csum, rc = struct.unpack_from('<3I', s, 8)
base = 0x40 if s[0x14:0x40] != b'\xff' * 0x2c or s[0x40:0x100] != b'\xff' * 0xc0 else 0x100
if base == 0x100:
    print("lhfw 0.2 format: only EEPROM 0..3839 captured")
data = s[base:base + length]
if csum == 0xFFFFFFFF:
    print(f"length {len(data)} (lhbs chunked format: no stored checksum)")
else:
    print(f"length {len(data)}, IAP status {rc}, checksum {'OK' if sum(data) & 0xffffffff == csum else 'BAD'}")
open(sys.argv[2], 'wb').write(data)
print(f"wrote {sys.argv[2]}")
