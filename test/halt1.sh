#!/bin/bash
# HALT_ON=1 ROM (Halt On: No Errors, for a board without keyboard or with a dead CMOS battery): with a fresh
# CMOS the POST errors print a WARNING and DOS boots without any key.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
HALT_ON=1 "$nb/build.sh" > "$vm_dir/build.log" 2>&1 || { tail "$vm_dir/build.log"; fail "HALT_ON=1 build"; }
dos_disk "$vm_dir/hdd.img"
vm_start halt1 "$nb/out/newbios-halt1.rom" "$(disk_cfg "$vm_dir/hdd.img")"
vm_wait_text "WARNING: POST errors above, boot continues" 60
vm_expect "CMOS checksum error - Defaults loaded"
vm_wait_text "DOS-TEST-OK" 90
grep -q "Press F1 to continue" "$vm/screen.txt" && fail "the No Errors ROM waits for F1"
vm_stop
