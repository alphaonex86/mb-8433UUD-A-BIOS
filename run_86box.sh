#!/bin/bash
# Try newBIOS interactively in the 86Box fork (Qt GUI): Biostar MB-8433UUD, Am5x86-133, one 32 MB SIMM and
# 256 KB L2 (other SIMMs: [UMC HB4(8881F)] in vm/86box.cfg, types in umc_hb4.c), TGUI9440,
# FreeDOS disk on IDE, RTL8139C with the operator's Etherboot ROM in its socket, SLiRP DHCP/TFTP serving
# boot.pxe (iPXE) -> bzImage (Linux 6.18 test kernel), POST card (codes in the status bar, POSTCODES.md).
# CMOS and disk persist in vm/ like on real hardware.
# usage: run_86box.sh [rom]   default out/newbios.rom, rebuilt if a source changed; BIOSORIGINAL.bin boots the original BIOS
set -euo pipefail
nb=$(dirname "$(readlink -f "$0")")
. "$nb/test/lib.sh"
vm=$nb/vm
rom=$(readlink -f "${1:-$nb/out/newbios.rom}")
GUI=1 "$nb/build_86box.sh" > /dev/null
sources=("$nb/build.sh" "$nb/cmos.h" "$nb/seabios.config" "$nb/configs/mb8433_author.config"
         "$nb/build/seabios/src" "$nb/build/seabios/Makefile" "$nb/sources/coreboot_486/src")
if [ $# -eq 0 ] && { [ ! -f "$nb/out/newbios.rom" ] ||
     [ -n "$(find "${sources[@]}" -newer "$nb/out/newbios.rom" -print -quit)" ]; }; then
  "$nb/build.sh" > /dev/null   # the ROM is missing or older than one of its sources
fi
[ -f "$nb/build/ipxe-chain.pxe" ] || "$nb/test/ipxe/build.sh"
[ -f "$nb/build/bzImage" ] || "$nb/test/linux/build.sh"

mkdir -p "$vm/roms/machines/84xxuuda" "$vm/roms/network/rtl8139c" "$vm/tftp"
cp "$rom" "$vm/roms/machines/84xxuuda/uud0520s.bin"   # -R paths are searched before the default ROM set
rm -f "$vm/nvr/84xxuuda.bin"   # flash chip saved by 86Box: start from the ROM (CMOS is 84xxuuda.nvr, kept)
cp "$nb/test/roms/etherboot-5.4.4-rtl8139.rom" "$vm/roms/network/rtl8139c/rtl8139c.rom"
cp "$nb/build/ipxe-chain.pxe" "$vm/tftp/boot.pxe"
cp "$nb/build/bzImage" "$vm/tftp/bzImage"
[ -f "$vm/dos.img" ] || dos_disk "$vm/dos.img"
[ -f "$vm/86box.cfg" ] || cat > "$vm/86box.cfg" <<EOF
[Machine]
machine = 84xxuuda
cpu_family = am5x86
cpu_speed = 133333333
cpu_multi = 4
cpu_use_dynarec = 1
fpu_type = internal
mem_size = 131072
time_sync = disabled

$(ram_cfg 5 0 0 0 256 1)

[Video]
gfxcard = tgui9440_pci

[Input devices]
keyboard_type = keyboard_at
mouse_type = none

$(disk_cfg "$vm/dos.img")

[Other peripherals]
postcard_enabled = 1

[Network]
net_01_card = rtl8139c+
net_01_net_type = slirp
net_01_link = 0

[Realtek RTL8139C+ #1]
bios = 1
EOF
grep -q '^postcard_enabled' "$vm/86box.cfg" || printf '\n[Other peripherals]\npostcard_enabled = 1\n' >> "$vm/86box.cfg"
grep -q '^\[UMC HB4' "$vm/86box.cfg" || {   # VM made before the DRAM model: production ROM needs it
  sed -i 's/^mem_size = .*/mem_size = 131072/' "$vm/86box.cfg"
  printf '\n%s\n' "$(ram_cfg 5 0 0 0 256 1)" >> "$vm/86box.cfg"
}
BOX86_SLIRP_TFTP=$vm/tftp BOX86_SLIRP_BOOTFILE=boot.pxe \
  exec "$nb/build/86box/out-qt/src/86Box" -P "$vm" -R "$vm/roms"
