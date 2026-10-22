#!/usr/bin/env bash
# Flash / verify an image through the LPC11U3x ROM USB mass-storage bootloader.
#
#   tools/flash.sh IMAGE.bin          back up current flash, write IMAGE in place
#   tools/flash.sh --verify IMAGE.bin compare current flash with IMAGE
#
# Enter ISP: power off, hold the mode button, plug micro-USB (+ power).
# A drive labelled "CRP DISABLD" with firmware.bin (= whole flash) appears.
#
# IMPORTANT: use a FRESH ISP session for every write (power-cycle into ISP
# again), and never cp/drag-drop. Replacing the file makes the ROM's emulated
# FAT map the data to the wrong flash offsets (observed: image shifted by
# 1 KB with the first 3 KB missing -> no vector table -> board looks dead).
# This script overwrites the existing file in place with O_DIRECT so the
# sectors go out in order and unbuffered.
set -euo pipefail

verify=0
if [[ ${1:-} == --verify ]]; then verify=1; shift; fi
img=${1:?usage: flash.sh [--verify] IMAGE.bin}
here=$(cd "$(dirname "$0")" && pwd)

python3 "$here/lpcsum.py" check "$img"
size=$(stat -c %s "$img")
(( size <= 131072 )) || { echo "image larger than 128 KB"; exit 1; }

dev=$(blkid -L "CRP DISABLD" 2>/dev/null || true)
mnt=$(findmnt -n -o TARGET -S LABEL="CRP DISABLD" 2>/dev/null | head -n1 || true)
if [[ -z "$mnt" ]]; then
    [[ -n "$dev" ]] || { echo "No 'CRP DISABLD' drive found - is the board in ISP mode?"; exit 1; }
    udisksctl mount -b "$dev" >/dev/null
    mnt=$(findmnt -n -o TARGET -S LABEL="CRP DISABLD" | head -n1)
fi
target="$mnt/firmware.bin"
[[ -f "$target" ]] || { echo "$target missing"; exit 1; }

current=$(mktemp)
trap 'rm -f "$current"' EXIT
dd if="$target" of="$current" bs=512 iflag=direct status=none

if (( verify )); then
    if cmp -n "$size" "$img" "$current"; then
        echo "VERIFY OK: flash matches $img ($size bytes)"
    else
        echo "VERIFY FAILED: flash differs from $img"; exit 2
    fi
    exit 0
fi

mkdir -p "$here/../backups"
bk="$here/../backups/flash-$(date +%Y%m%d-%H%M%S).bin"
cp "$current" "$bk"
echo "backed up current flash -> $bk"

# pad to whole 512-byte sectors so every O_DIRECT write is a full sector
padded=$(mktemp)
trap 'rm -f "$current" "$padded"' EXIT
python3 - "$img" "$padded" <<'E'
import sys
d = open(sys.argv[1], 'rb').read()
open(sys.argv[2], 'wb').write(d + b'\xff' * (-len(d) % 512))
E
dd if="$padded" of="$target" bs=512 conv=notrunc,nocreat oflag=direct status=none
sync
echo "wrote $size bytes to $target"
udisksctl unmount -b "${dev:-$(findmnt -n -o SOURCE -T "$mnt")}" >/dev/null 2>&1 || umount "$mnt" || true
cat <<EOF
done. Now: unplug USB/power, re-enter ISP (hold button + plug in) and run
  tools/flash.sh --verify $img
to confirm, then power-cycle without the button to boot it.
EOF
