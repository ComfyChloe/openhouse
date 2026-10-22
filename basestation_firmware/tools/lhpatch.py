#!/usr/bin/env python3
"""Apply byte patches to the stock lighthouse_tx firmware and fix the vector checksum.

Patch file format (one per line, '#' comments):
    <hex offset> <expected old bytes hex> <new bytes hex>
Every patch is verified against the expected old bytes before anything is written,
so a patch can never silently land on the wrong image.

usage: lhpatch.py STOCK.bin OUT.bin [patch.txt ...]
With no patch files this produces a byte-identical copy (pipeline self-test).
"""
import hashlib, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lpcsum


def parse(path):
    for n, line in enumerate(open(path), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        off, old, new = line.split()
        old, new = bytes.fromhex(old), bytes.fromhex(new)
        if len(old) != len(new):
            sys.exit(f"{path}:{n}: old/new length mismatch")
        yield path, n, int(off, 16), old, new

def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    img = bytearray(open(argv[1], 'rb').read())
    print(f"stock sha256 {hashlib.sha256(img).hexdigest()}  ({len(img)} bytes)")
    if not lpcsum.is_valid(img):
        sys.exit("input image has a bad vector checksum - refusing")
    patches = [p for f in argv[3:] for p in parse(f)]
    for path, n, off, old, new in patches:
        if off + len(new) > len(img):
            img.extend(b'\xff' * (off + len(new) - len(img)))
        cur = bytes(img[off:off + len(old)])
        if cur != old and cur != b'\xff' * len(old):
            sys.exit(f"{path}:{n}: @{off:#x} expected {old.hex()} found {cur.hex()}")
    for path, n, off, old, new in patches:
        img[off:off + len(new)] = new
        print(f"  {os.path.basename(path)}:{n} @{off:#07x} {len(new)} bytes")
    img = lpcsum.fix(bytes(img))
    open(argv[2], 'wb').write(img)
    print(f"wrote {argv[2]} sha256 {hashlib.sha256(img).hexdigest()}")

if __name__ == '__main__':
    main(sys.argv)
