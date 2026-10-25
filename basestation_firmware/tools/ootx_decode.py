#!/usr/bin/env python3
"""Decode an OOTX frame (hex bytes as printed by the lhbs 'ootx' command) using
the framing rules from openhouse ai/driver/disambiguator_tdm.md, independent of lhbs's
encoder: >=17 zeros, '1', then 16-bit words each followed by a '1' stuffing bit;
u16 length, payload, pad to even, crc32 (zlib)."""
import struct, sys, zlib

def bits_of(data, nbits):
    return [(data[i >> 3] >> (7 - (i & 7))) & 1 for i in range(nbits)]

def decode(bits):
    z = 0
    for i, b in enumerate(bits):
        if b == 0: z += 1; continue
        if z >= 17: start = i + 1; break
        z = 0
    else:
        raise ValueError("no preamble")
    words, i = [], start
    while i + 17 <= len(bits):
        w = bits[i:i + 16]
        if bits[i + 16] != 1: raise ValueError(f"bad stuffing bit at {i + 16}")
        words.append(bytes(int(''.join(map(str, w[k:k + 8])), 2) for k in (0, 8)))
        i += 17
    raw = b''.join(words)
    n = raw[0] | raw[1] << 8
    payload = raw[2:2 + n]
    padded = n + (n & 1)
    crc = struct.unpack_from('<I', raw, 2 + padded)[0]
    return n, payload, crc == zlib.crc32(payload)

def half(b, o): return struct.unpack_from('<e', b, o)[0]

if __name__ == '__main__':
    hexs = sys.argv[1:] if len(sys.argv) > 1 else sys.stdin.read().split()
    data = bytes(int(h, 16) for h in hexs)
    n, p, ok = decode(bits_of(data, 358))
    print(f"length {n}  crc {'OK' if ok else 'BAD'}")
    v = struct.unpack_from('<H', p, 0)[0]
    print(f"version {v & 0x3f}  firmware {v >> 6}  serial {struct.unpack_from('<I', p, 2)[0]} "
          f"(0x{struct.unpack_from('<I', p, 2)[0]:08X})  hw {p[15]}  mode {p[31]}  faults {p[32]}")
    for a in (0, 1):
        print(f"axis {a}: phase {half(p, 6 + 2*a):.6f} tilt {half(p, 10 + 2*a):.6f} "
              f"curve {half(p, 16 + 2*a):.6f} gibphase {half(p, 23 + 2*a):.6f} gibmag {half(p, 27 + 2*a):.6f}")
    print("accel", list(struct.unpack_from('<3b', p, 20)), " lock events", p[14])
