#!/usr/bin/env python3
"""Histogram HID report IDs on hidraw nodes. usage: hidhist.py SECONDS /dev/hidrawN..."""
import os, sys, select, time, collections
secs = float(sys.argv[1]); fds = {}
for p in sys.argv[2:]:
    try: fds[os.open(p, os.O_RDONLY | os.O_NONBLOCK)] = p
    except OSError as e: print(p, e)
hist = {p: collections.Counter() for p in fds.values()}; lens = {p: {} for p in fds.values()}
end = time.time() + secs
while time.time() < end:
    r, _, _ = select.select(list(fds), [], [], 0.1)
    for fd in r:
        try: d = os.read(fd, 64)
        except OSError: continue
        if d: hist[fds[fd]][d[0]] += 1; lens[fds[fd]][d[0]] = len(d)
for p, h in hist.items():
    print(p, ' '.join('0x%02x:%d(len%d)' % (k, v, lens[p][k]) for k, v in sorted(h.items())) or '(nothing)')
