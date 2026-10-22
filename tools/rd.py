#!/usr/bin/env python3
"""Read constants from driver_lighthouse.so by Ghidra address (image base 0x100000).
usage: rd.py [d|f|q|i|s] ADDR [count]"""
import sys, struct, os
SO = os.path.join(os.path.dirname(__file__), '..', 'driver_lighthouse.so')
d = open(SO, 'rb').read()
# map VA->file offset using program headers
import struct as S
phoff = S.unpack_from('<Q', d, 0x20)[0]; phn = S.unpack_from('<H', d, 0x38)[0]
segs = []
for i in range(phn):
    t, fl, off, va, pa, fsz, msz, al = S.unpack_from('<IIQQQQQQ', d, phoff + i * 56)
    if t == 1: segs.append((va, off, fsz))
def fo(va):
    for v, o, sz in segs:
        if v <= va < v + sz: return va - v + o
    raise ValueError(hex(va))
kind = sys.argv[1]; a = int(sys.argv[2], 16) - 0x100000; n = int(sys.argv[3]) if len(sys.argv) > 3 else 1
fmt = {'d': '<d', 'f': '<f', 'q': '<Q', 'i': '<i'}
if kind == 's':
    o = fo(a); print(d[o:d.index(b'\0', o)].decode(errors='replace'))
else:
    sz = S.calcsize(fmt[kind])
    for i in range(n):
        v = S.unpack_from(fmt[kind], d, fo(a + i * sz))[0]
        print(hex(a + 0x100000 + i * sz), repr(v) if kind in 'df' else hex(v))
