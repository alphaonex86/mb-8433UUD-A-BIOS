#!/bin/bash
# Build the PXE NBP used by the network boot test -> newBIOS/build/ipxe-chain.pxe (iPXE, netboot config
# from configs/ipxe, embedded chain.ipxe), and the RTL8169 boot ROM -> newBIOS/build/ipxe-rtl8169.rom.
set -euo pipefail
nb=$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")
src=$nb/build/ipxe
[ -d "$src/.git" ] || git clone -q "$nb/sources/ipxe" "$src"
cp "$nb/configs/ipxe/general.h" "$nb/configs/ipxe/usb.h" "$src/src/config/local/"
"$nb/tools/build_toolchain.sh"
ipxe() { make -C "$src/src" -s -j"$(nproc)" CROSS="$nb/sources/coreboot_486/util/crossgcc/xgcc/bin/i386-elf-" "$@" \
  >> "$nb/build/ipxe.build.log" 2>&1 || { tail -20 "$nb/build/ipxe.build.log"; echo "IPXE BUILD FAILED" >&2; exit 1; }; }
: > "$nb/build/ipxe.build.log"
# PXE NBP for PXE-capable socket ROMs (e.g. Etherboot 5.4): DHCP again, then the Linux bzImage by TFTP.
ipxe bin/10ec8139.pxe EMBED="$(dirname "$(readlink -f "$0")")/chain.ipxe"
cp "$src/src/bin/10ec8139.pxe" "$nb/build/ipxe-chain.pxe"
# Boot ROM for an RTL8169's socket: DHCP, then the Linux bzImage by TFTP with ip=dhcp (r8169 DHCPs again).
printf '#!ipxe\ndhcp\nchain tftp://${next-server}/bzImage ip=dhcp\n' > "$nb/build/ipxe-rtl8169.ipxe"
ipxe bin/10ec8169.rom EMBED="$nb/build/ipxe-rtl8169.ipxe"
cp "$src/src/bin/10ec8169.rom" "$nb/build/ipxe-rtl8169.rom"
