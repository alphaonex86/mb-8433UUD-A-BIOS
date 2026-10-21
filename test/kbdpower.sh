#!/bin/bash
# Cold boot with a slow 8042 and keyboard (86Box BOX86_KBC_POWERON_MS / BOX86_KBD_POWERON_MS): the keyboard is
# found and works for power-on times real ones take (a keyboard error on the board's cold boot, fine after a reset);
# one far too slow keyboard must give the keyboard error (the model is live) without halting the boot (no keyboard
# could press F1), then type once awake (Ctrl+Alt+Del).
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
BOX86_KBC_POWERON_MS=0 BOX86_KBD_POWERON_MS=15000 vm_start kbdpower "$rom" ""
vm_wait_text "boot continues \(no keyboard\)" 90     # nobody could press F1: the boot goes on
vm_expect "Keyboard error"
sleep 12                                            # left enabled, as the original BIOS: once awake it types
vm_cad 60
vm_stop
