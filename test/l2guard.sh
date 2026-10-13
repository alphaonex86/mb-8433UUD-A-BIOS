#!/bin/bash
# L2 enable guard with a valid CMOS: POSTs left unfinished (setup entered then reset, reset at the error
# prompt) must not turn the L2 off (on the real board they did); an L2 init that hung (CMOS 3Eh bit 7 still
# set) leaves it off once, with the reason on the CPU page, and the next POST enables it again.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
l2_page() {  # setup -> CPU page; expects its L2 line
  vm_setup
  vm_info 0                                         # CPU, CACHE AND MEMORY
  vm_wait_text "L2 cache +: $1" 5
}
vm_start l2guard "$rom" ""
vm_wait_text "Press F1 to continue, DEL to enter SETUP" 60   # fresh CMOS
vm_setup
vm_save                                             # valid settings from now on
for _ in 1 2 3; do                                  # unfinished POSTs: in the setup, then reset
  vm_wait_text "Press DEL to enter SETUP" 60
  l2_page "256 KB, write-back, enabled"
  vm_cmd reset
done
vm_wait_text "Press DEL to enter SETUP" 60
rm -f "$vm/cmos3e.txt"                              # "in" appends: only this read
vm_cmd out 70 3e                                    # an L2 init that hung: in-progress bit set
vm_cmd in 71 "$vm/cmos3e.txt"
for _ in $(seq 20); do [ -s "$vm/cmos3e.txt" ] && break; sleep 0.1; done
read -r _ cmos3e < "$vm/cmos3e.txt"
[ -n "$cmos3e" ] || fail "CMOS 3Eh not read"
vm_cmd out 70 3e
vm_cmd out 71 "$(printf '%02x' $((16#$cmos3e | 0x80)))"
vm_cmd reset
vm_wait_text "Press DEL to enter SETUP" 60
l2_page "256 KB, 1 bank\(s\), OFF: hang in the previous L2 init"
vm_cmd reset
vm_wait_text "Press DEL to enter SETUP" 60
l2_page "256 KB, write-back, enabled"
vm_stop
