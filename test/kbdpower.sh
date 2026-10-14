#!/bin/bash
# Cold boot with a slow 8042 and keyboard (86Box BOX86_KBC_POWERON_MS / BOX86_KBD_POWERON_MS): the keyboard is
# found and works for power-on times real ones take (a keyboard error on the board's cold boot, fine after a reset);
# one far too slow keyboard must give the keyboard error (the model is live), then type once awake.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
boot() {  # 8042 ms, keyboard ms -> fresh-CMOS POST up to its error prompt
  BOX86_KBC_POWERON_MS=$1 BOX86_KBD_POWERON_MS=$2 vm_start kbdpower "$rom" ""
  vm_wait_text "Press F1 to continue, DEL to enter SETUP" 90
}
for delays in "0 1000" "0 2500" "500 1500" "1000 2500"; do
  boot $delays
  ! grep -q "Keyboard error" "$vm/screen.txt" || fail "8042/keyboard ready after ${delays/ //} ms: keyboard error"
  vm_key $K_F1
  vm_wait_text "Press DEL to enter SETUP, ESC for boot menu" 20
  vm_key $K_ESC
  vm_wait_text "Select boot device" 10
  vm_stop
done
boot 0 15000
grep -q "Keyboard error" "$vm/screen.txt" || fail "keyboard silent for 15 s: no keyboard error"
for _ in $(seq 30); do   # left enabled, as the original BIOS: it types once awake
  vm_key $K_F1
  vm_screen
  grep -q "Press DEL to enter SETUP, ESC for boot menu" "$vm/screen.txt" && break
  sleep 1
done
vm_wait_text "Press DEL to enter SETUP, ESC for boot menu" 1
vm_stop
