#!/bin/bash
# The CMOS saved by an older ROM: while NB_SIG_VALUE stays, a new ROM keeps those settings, so changed defaults never
# reach a board (the real board kept the old Floppy-first boot order). 1) The setting list and Defaults match the
# fingerprint recorded for this NB_SIG_VALUE. 2) The previous layout's CMOS (Floppy first) loads the defaults
# (Hard Disk first); the same CMOS with this layout's signature keeps Floppy first.
. "$(dirname "$0")/lib.sh"
known="a7 4d46a367e61ad8a7"
prev_sig=0xa6
sig=$(sed -nE 's/^#define NB_SIG_VALUE +0x([0-9a-f]+).*/\1/p' "$nb/cmos.h")
sum=$({ sed -n '/^enum nb_setting/,/^};/p' "$nb/cmos.h"; sed -n '/^static const u8 Defaults/,/^};/p' "$nb/build/seabios/src/setup.c"; } \
  | sed 's|/\*.*\*/||; s|//.*||' | tr -d ' \t\n' | sha256sum | cut -c1-16)
[ "$sig $sum" = "$known" ] || {
  echo "FAIL [cmoslayout]: settings or Defaults changed: bump NB_SIG_VALUE in cmos.h, then set known=\"<sig> $sum\" here"
  exit 1
}

mkdir -p "$vm_dir"
cmos() {  # signature -> $vm_dir/cmos.bin: valid RTC, boot order Floppy, Hard Disk, CD-ROM, Network, valid sum
  python3 - "$1" "$vm_dir/cmos.bin" <<'EOF'
import sys
b = bytearray(256)
b[0x0a], b[0x0b], b[0x0d] = 0x26, 0x02, 0x80
b[0x07], b[0x08], b[0x09], b[0x32] = 0x01, 0x01, 0x26, 0x20
for var, v in ((9, 1), (10, 2), (11, 3), (12, 4)):   # NB_BOOT1..4, a nibble each from NB_CMOS_BASE
    b[0x40 + var // 2] |= v << (var % 2 * 4)
b[0x7b] = int(sys.argv[1], 0)
s = sum(b[0x40:0x7c])
b[0x7c], b[0x7d] = s >> 8, s & 0xff
open(sys.argv[2], 'wb').write(b)
EOF
}

boot_order() {  # signature, expected warning or "", expected 1st boot device
  cmos "$1"
  vm_nvr=$vm_dir/cmos.bin vm_start cmoslayout "$rom"
  vm_end_of_post 60
  if [ -n "$2" ]; then vm_expect "$2"
  else vm_screen; ! grep -q "Defaults loaded" "$vm/screen.txt" || fail "defaults loaded from a valid CMOS"; fi
  vm_setup
  vm_menu 1
  vm_wait_text "Boot Up NumLock Status" 5
  vm_expect "1st Boot Device +: $3"
  vm_stop
}
boot_order $prev_sig "CMOS checksum error - Defaults loaded" "Hard Disk"
boot_order 0x$sig "" "Floppy"
