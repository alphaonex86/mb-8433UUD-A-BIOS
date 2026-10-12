#!/bin/bash
# Build the built-in network boot ROM: Etherboot 5.4.4 with the rtl8139 and r8169 drivers in one PCI ROM
# image, from the pinned source + patches/etherboot.patch, with the coreboot cross toolchain. Outputs, added
# to the CBFS by build.sh: build/etherboot/pxe.rom.lzma (LZMA with the header SeaBIOS reads: properties,
# 64-bit size) and pxe-pci-ids.bin (the PCI IDs of the drivers, u32 device << 16 | vendor each).
set -euo pipefail
nb=$(dirname "$(dirname "$(readlink -f "$0")")")
src=$nb/build/etherboot-5.4.4/src
out=$nb/build/etherboot
rom=rtl8139--r8169
x=$nb/sources/coreboot_486/util/crossgcc/xgcc/bin/i386-elf-
if [ ! -d "$src" ]; then
  tar xjf "$nb/sources/etherboot-5.4.4.tar.bz2" -C "$nb/build"
  patch -d "$src/.." -p1 -s < "$nb/patches/etherboot.patch"
fi
mkdir -p "$out"
make -C "$src" ARCH=i386 CC="${x}gcc -fgnu89-inline -fcommon" CPP="${x}gcc -E" AS="${x}as" LD="${x}ld" \
  SIZE="${x}size" AR="${x}ar" RANLIB="${x}ranlib" OBJCOPY="${x}objcopy" HOST_CC=gcc "bin/$rom.zrom" \
  > "$out/build.log" 2>&1 || { tail -20 "$out/build.log"; echo "ETHERBOOT BUILD FAILED" >&2; exit 1; }
python3 - "$src" "$rom" "$out" ${rom//--/ } <<'PY'
import lzma, re, struct, sys
src, rom, out, *drivers = sys.argv[1:]
data = open(f'{src}/bin/{rom}.zrom', 'rb').read()
packed = lzma.compress(data, format=lzma.FORMAT_ALONE, preset=9 | lzma.PRESET_EXTREME)
open(out + '/pxe.rom.lzma', 'wb').write(packed[:5] + struct.pack('<Q', len(data)) + packed[13:])
ids = [int(dev, 16) << 16 | int(ven, 16) for d in drivers
       for ven, dev in re.findall(r'PCI_ROM\(\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+)', open(f'{src}/drivers/net/{d}.c').read())]
if not ids:
    sys.exit('no PCI IDs found')
open(out + '/pxe-pci-ids.bin', 'wb').write(struct.pack('<%dI' % len(ids), *ids))
PY
