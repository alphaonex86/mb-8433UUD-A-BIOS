#!/bin/bash
# iPXE PXE program for the network boot tests: the board boots the network with Etherboot in its RTL8139's socket ROM,
# which loads this by TFTP; it does DHCP again and runs the test's boot.ipxe from the TFTP server (test/ipxe/chain.ipxe,
# embedded). The realtek driver (RTL8139, 8139C+, 8169) plus the 10ec:8039 clone (patches/ipxe.patch), config
# configs/ipxe. Output: build/ipxe-chain.pxe. The patch is re-exported after each build.
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
env -u DEBUG make -C "$src/src" -s -j"$(nproc)" CROSS="$nb/sources/coreboot_486/util/crossgcc/xgcc/bin/i386-elf-" \
  bin/realtek.pxe EMBED="$nb/test/ipxe/chain.ipxe" > "$log" 2>&1 \
  || { tail -20 "$log"; echo "IPXE BUILD FAILED (log: $log)" >&2; exit 1; }
cp "$src/src/bin/realtek.pxe" "$nb/build/ipxe-chain.pxe"
git -C "$src" diff --full-index -- src/drivers > "$nb/patches/ipxe.patch"
