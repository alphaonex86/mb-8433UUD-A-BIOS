#!/bin/bash
# Build the PXE NBP used by the network boot test -> newBIOS/build/ipxe-chain.pxe (iPXE, netboot config
# from configs/ipxe, embedded chain.ipxe).
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
