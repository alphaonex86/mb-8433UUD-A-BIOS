#!/bin/bash
# Makes usb.img.tar.xz, the board's USB stick for the tests usblilo and nokbd: the HDD project's image (LILO, Linux,
# swap; default ../HDD-build/output/images/usb.img) with FreeDOS 1.3 (sources/FD13-LiteUSB.zip, its installer
# packages left out) in place of MS-DOS 6.22 on partition 4: no proprietary software in git. A sparse tar: the 62 GB
# image holds ~150 MB of data. Tests unpack it with lib.sh usb_stick. usage: make_usb_img.sh [source image]
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
nb=$(dirname "$(dirname "$here")")
src=${1:-$nb/../HDD-build/output/images/usb.img}
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT

cp --sparse=always "$src" "$tmp/usb.img"
read -r start size < <(/sbin/sfdisk -d "$tmp/usb.img" | sed -n 's/.*usb.img4 : start= *\([0-9]*\), size= *\([0-9]*\),.*/\1 \2/p')
[ -n "${start:-}" ] || { echo "no partition 4 in $src" >&2; exit 1; }
fallocate --punch-hole --offset $((start * 512)) --length $((size * 512)) "$tmp/usb.img"   # MS-DOS gone, holes

unzip -p "$nb/sources/FD13-LiteUSB.zip" FD13LITE.img > "$tmp/fd.img"
fd=$((63 * 512))
mdeltree -i "$tmp/fd.img@@$fd" ::/PACKAGES
mdel -i "$tmp/fd.img@@$fd" ::/SETUP.BAT
printf '@echo off\r\nSET DOSDIR=\\FREEDOS\r\nSET PATH=%%DOSDIR%%\\BIN\r\necho DOS-TEST-OK\r\n' | mcopy -o -i "$tmp/fd.img@@$fd" - ::/FDAUTO.BAT
fs_sectors=$(python3 -I -c 'import struct,sys; b=open(sys.argv[1],"rb").read(64*512)[63*512:]; print(struct.unpack("<H",b[19:21])[0])' "$tmp/fd.img")
[ "$fs_sectors" -le "$size" ] || { echo "FreeDOS file system larger than partition 4" >&2; exit 1; }
dd if="$tmp/fd.img" of="$tmp/usb.img" bs=512 skip=63 seek="$start" count="$fs_sectors" conv=notrunc,sparse status=none
# Its boot sector: where the partition starts, the BIOS geometry of a large USB disk (1024/255/63)
python3 -I - "$tmp/usb.img" "$start" <<'PY'
import struct, sys
path, start = sys.argv[1], int(sys.argv[2])
with open(path, "r+b") as f:
    f.seek(start * 512 + 24)
    f.write(struct.pack("<HHI", 63, 255, start))
PY
tar -C "$tmp" -cSJf "$here/usb.img.tar.xz" usb.img
ls -l "$here/usb.img.tar.xz"
