#!/bin/bash
# Try mb-8433UUD interactively in the 86Box fork (Qt GUI): Biostar MB-8433UUD, Am5x86-133, one 32 MB SIMM and
# 256 KB L2 (other SIMMs: [UMC HB4(8881F)] in vm/86box.cfg, types in umc_hb4.c), TGUI9440,
# FreeDOS disk on IDE, a 2 Mbit flash (W49F002A) with iPXE in its lower half for the NICs: the operator's
# RTL8139C (10ec:8039, no C+ mode) and an RTL8169S, no socket ROMs; SLiRP DHCP/TFTP serving boot.ipxe (iPXE
# script) -> bzImage (Linux 6.18 test kernel), POST card (codes in the status bar, POSTCODES.md),
# floppy and hard disk sounds (samples from the 86Box ROM set).
# CMOS and disk persist in vm/ like on real hardware.
# usage: run_86box.sh [rom]   default out/newbios-2mbit.rom, rebuilt if a source changed; a 128 KiB ROM (e.g.
#        BIOSORIGINAL.bin, the original BIOS) runs on the board's 1 Mbit Intel chip
set -euo pipefail
nb=$(dirname "$(readlink -f "$0")")
. "$nb/test/lib.sh"
vm=$nb/vm
rom=$(readlink -f "${1:-$nb/out/newbios-2mbit.rom}")
GUI=1 "$nb/build_86box.sh" > /dev/null
sources=("$nb/build.sh" "$nb/cmos.h" "$nb/seabios.config" "$nb/configs/mb8433_author.config"
         "$nb/build/seabios/src" "$nb/build/seabios/Makefile" "$nb/sources/coreboot_486/src")
if [ $# -eq 0 ] && { [ ! -f "$rom" ] || [ -n "$(find "${sources[@]}" -newer "$rom" -print -quit)" ]; }; then
  "$nb/build.sh" > /dev/null   # the ROM is missing or older than one of its sources
fi
[ -f "$nb/build/bzImage" ] || "$nb/test/linux/build.sh"

mkdir -p "$vm/roms/machines/84xxuuda" "$vm/tftp"
cp "$rom" "$vm/roms/machines/84xxuuda/uud0520s.bin"   # -R paths are searched before the default ROM set
rm -f "$vm/nvr/84xxuuda.bin"   # flash chip saved by 86Box: start from the ROM (CMOS is 84xxuuda.nvr, kept)
cp "$nb/test/ipxe/chain.ipxe" "$vm/tftp/boot.ipxe"
flash=intel_bxt
[ "$(stat -c %s "$rom")" = 262144 ] && flash=w49f002a
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

[Network]
net_01_card = rtl8139c+
net_01_net_type = slirp
net_01_link = 0
net_02_card = rtl8169
net_02_net_type = slirp
net_02_link = 0
EOF
cfg_add() {  # section, key, value: set the key in the VM config unless it is already there
  awk -v s="[$1]" -v k="$2" '/^\[/ {sec = ($0 == s)} sec && $1 == k {f = 1} END {exit !f}' "$vm/86box.cfg" && return
  if grep -qxF "[$1]" "$vm/86box.cfg"; then
    awk -v s="[$1]" -v l="$2 = $3" '{print} $0 == s {print l}' "$vm/86box.cfg" > "$vm/86box.cfg.new"
    mv "$vm/86box.cfg.new" "$vm/86box.cfg"
  else
    printf '\n[%s]\n%s = %s\n' "$1" "$2" "$3" >> "$vm/86box.cfg"
  fi
}
cfg_set() {  # section, key, value: set the key in the VM config
  sed -i "/^\[$1\]\$/,/^\$/{/^$2 = /d}" "$vm/86box.cfg"
  cfg_add "$@"
}
grep -q '^net_02_card' "$vm/86box.cfg" ||   # VM made before the RTL8169: second card
  sed -i 's/^net_01_link = .*/&\nnet_02_card = rtl8169\nnet_02_net_type = slirp\nnet_02_link = 0/' "$vm/86box.cfg"
sed -i '/^\[Realtek RTL8169S #1\]$/,/^$/d' "$vm/86box.cfg"   # NIC sections are numbered by slot: #2
cfg_set "Realtek RTL8139C+ #1" bios 0   # no socket ROMs: iPXE from the flash
cfg_set "Realtek RTL8169S #2" bios 0
cfg_set "Realtek RTL8139C+ #1" device_id 8039
cfg_set "Realtek RTL8139C+ #1" cplus 0
cfg_add "Other peripherals" postcard_enabled 1
sed -i '/^fdd_0[12]_audio = panasonic_ju4755_40t$/d' "$vm/86box.cfg"   # sounds of the former 5.25" drives
cfg_add "Floppy and CD-ROM drives" fdd_01_type 35_2hd   # drive A as the BIOS default: 1.44M, 3.5 in.
cfg_add "Floppy and CD-ROM drives" fdd_02_type none
cfg_add "Floppy and CD-ROM drives" fdd_01_audio alps_df354h148f_80t
cfg_add "Hard disks" hdd_01_audio IBM_H3171_3600RPM
mkdir -p "$vm/assets"   # 86Box looks for assets/sounds/... in the VM directory too
ln -sfn "$HOME/.local/share/86Box/roms/sound" "$vm/assets/sounds"
grep -q '^\[UMC HB4' "$vm/86box.cfg" || {   # VM made before the DRAM model: production ROM needs it
  sed -i 's/^mem_size = .*/mem_size = 131072/' "$vm/86box.cfg"
  printf '\n%s\n' "$(ram_cfg 5 0 0 0 256 1)" >> "$vm/86box.cfg"
}
BOX86_SLIRP_TFTP=$vm/tftp BOX86_SLIRP_BOOTFILE=boot.ipxe BOX86_FLASH=$flash \
  exec "$nb/build/86box/out-qt/src/86Box" -P "$vm" -R "$vm/roms"
