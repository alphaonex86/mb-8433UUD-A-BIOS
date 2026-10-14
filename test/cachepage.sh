#!/bin/bash
# Setup CACHE TEST page: A runs the 16 L2 settings (WT/WB x timing 0-3 x 7+1/8 tag bits), all work on the emulated
# 256 KB L2; Enter runs one step by step with its log on screen; the POST's write-back L2 is on again afterwards.
# An L2 init that stopped at a step (CMOS 3Eh bits 7 and 6-4, as a hang or reset leaves them) shows that step.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
vm_start cachepage "$rom" ""
vm_wait_text "Press F1 to continue, DEL to enter SETUP" 60   # fresh CMOS
vm_setup
vm_save
vm_wait_text "Press DEL to enter SETUP" 60
vm_setup
vm_menu 10                                          # CACHE TEST
vm_wait_text "L2 found at POST: 256K, 1 bank\(s\)" 10
vm_key 1e                                           # A: run all
sleep 1
for _ in $(seq 120); do vm_screen; grep -q "testing" "$vm/screen.txt" || break; sleep 0.5; done
works=$(grep -c "^ *W[TB] .* works  " "$vm/screen.txt")
[ "$works" = 16 ] || fail "$works of 16 L2 settings work"
vm_key $K_ENTER                                     # the POST's setting, step by step
vm_wait_text "Step 2 write-through fill" 20
vm_wait_text "DRAM samples read through the L2: all equal" 20
vm_wait_text "line at 00000100: read 5aa5c23c 5aa5c23c, want 5aa5c23c: ok" 20
vm_wait_text "Done, press a key" 90
vm_expect "off    50h: 10 .* CR0 11 "               # L2 off after the speed test (timing kept), L1 on again
vm_key $K_ENTER
vm_key $K_ESC
vm_info 0                                           # CPU, CACHE AND MEMORY
vm_wait_text "L2 cache +: 256 KB, write-back, enabled" 5
vm_cmd out 70 3e                                    # an init that stopped in its write-through fill
vm_cmd out 71 a3
vm_reset
vm_wait_text "Press DEL to enter SETUP" 60
vm_setup
vm_info 0
vm_wait_text "L2 cache +: 256 KB, OFF: last init stopped at write-through fill" 5
vm_key $K_ESC $K_ESC
vm_menu 10
vm_wait_text "the last L2 init stopped at step 2: write-through fill" 10
vm_stop
