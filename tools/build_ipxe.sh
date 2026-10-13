#!/bin/bash
# iPXE boot ROM for the lower 128 KiB of a 2 Mbit flash (out/newbios*-2mbit.rom; SeaBIOS runs it for the Realtek
# NICs instead of their socket ROMs): the realtek driver (RTL8139 in legacy mode, 8139C+, 8169) plus the
# operator's 10ec:8039 clone (patches/ipxe.patch), config configs/ipxe. It does DHCP and boots the DHCP boot
# file: a Linux bzImage or an iPXE script giving the kernel's path and arguments.
# Output: build/ipxe.rom (at most 128 KiB). The patch is re-exported after each build.
set -euo pipefail
nb=$(dirname "$(dirname "$(readlink -f "$0")")")
src=$nb/build/ipxe
log=$nb/build/ipxe.build.log
if [ ! -d "$src/.git" ]; then
  git clone -q "$nb/sources/ipxe" "$src"
  git -C "$src" apply "$nb/patches/ipxe.patch"
fi
cp "$nb/configs/ipxe/general.h" "$nb/configs/ipxe/usb.h" "$src/src/config/local/"
"$nb/tools/build_toolchain.sh"
make -C "$src/src" -s -j"$(nproc)" CROSS="$nb/sources/coreboot_486/util/crossgcc/xgcc/bin/i386-elf-" bin/realtek.rom \
  > "$log" 2>&1 || { tail -20 "$log"; echo "IPXE BUILD FAILED (log: $log)" >&2; exit 1; }
size=$(stat -c %s "$src/src/bin/realtek.rom")
[ "$size" -le 131072 ] || { echo "IPXE ROM $size bytes: over the 128 KiB lower half of the flash" >&2; exit 1; }
cp "$src/src/bin/realtek.rom" "$nb/build/ipxe.rom"
git -C "$src" diff --full-index -- src/drivers > "$nb/patches/ipxe.patch"
