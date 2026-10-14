#!/bin/bash
# A JEDEC flash chip newBIOS does not know (86Box SST39SF010, as a W49F002A or SST39SF010A on the board): the POST
# runs, its ID probe leaves the chip readable (the reboot after the setup's save fetches from it), no ESCD written.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
BOX86_FLASH=sst39sf010 vm_start otherflash "$rom" ""
vm_wait_text "Press F1 to continue, DEL to enter SETUP" 60   # fresh CMOS
vm_setup
vm_save
vm_wait_text "Press DEL to enter SETUP, ESC for boot menu" 60
vm_reset
vm_wait_text "Press DEL to enter SETUP, ESC for boot menu" 60
vm_cmd quit                                         # 86Box saves the flash when it powers off
for _ in $(seq 50); do kill -0 "$vm_pid" 2>/dev/null || break; sleep 0.1; done
cmp -s "$vm/nvr/84xxuuda.bin" "$rom" || fail "the flash changed"
vm_stop
