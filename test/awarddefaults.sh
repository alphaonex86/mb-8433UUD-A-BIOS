#!/bin/bash
# tools/award_defaults.py on the board's original Award BIOS (BIOSORIGINAL.bin, not published): its -lh5- encoder
# and decoder round-trip the system BIOS module, and the image with External Cache off and boot from C boots in
# 86Box (2 Mbit W49F002A flash, as on the board) with those defaults on a cleared CMOS and the L2 off (50h bit 7).
here=$(dirname "$(readlink -f "$0")")
nb=$(dirname "$here")
out=$here/vm/awarddefaults
mkdir -p "$out"
"$nb/tools/award_defaults.py" "$nb/BIOSORIGINAL.bin" "$out/award.bin" --l2-off --boot-c --2mbit \
  || { echo "FAIL [awarddefaults]: patcher"; exit 1; }
BOX86_FLASH=w49f002a AWARD_ROM=$out/award.bin "$here/oracle/award.sh" awarddefaults 1 -1 0 > "$out/oracle.log" 2>&1 \
  || { cat "$out/oracle.log"; echo "FAIL [awarddefaults]: the patched BIOS did not boot"; exit 1; }
vm=$here/vm/oracle-awarddefaults
grep -Eq "External Cache +: Disabled" "$vm/before.txt" || { echo "FAIL [awarddefaults]: External Cache default"; exit 1; }
grep -Eq "Boot Sequence +: C,A" "$vm/before.txt" || { echo "FAIL [awarddefaults]: Boot Sequence default"; exit 1; }
grep -q "^00:10.0 50: [0-7]" "$vm/pci.txt" || { echo "FAIL [awarddefaults]: L2 enabled (50h bit 7)"; exit 1; }
echo "PASS [awarddefaults]"
