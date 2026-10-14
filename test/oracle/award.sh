#!/bin/bash
# Oracle for the chipset mapping: boot the board's original Award BIOS (BIOSORIGINAL.bin, a dump of the chip) in the 86Box
# fork, change one setup item, save, reboot and dump what it programmed (PCI config space of bus 0 and the
# UM8663BF super I/O registers C0h/C1h).
# usage: award.sh <name> <main menu index> <item index> <PgDn presses> [Auto Configuration off]
#   main menu index 1 = BIOS FEATURES, 2 = CHIPSET FEATURES, 4 = PCI/GREEN; item -1 = no change
# Output: vm/oracle-<name>/{pci.txt,sio.txt,item.txt}
. "$(dirname "$0")/../lib.sh"
name=$1 menu=$2 item=$3 presses=$4 auto_off=${5:-}
vm_dir=$nb/test/vm/oracle-$name
rom=${AWARD_ROM:-$nb/BIOSORIGINAL.bin}   # AWARD_ROM: another image of it (e.g. tools/award_defaults.py output)
vm_start "oracle-$name" "$rom" "$([ -z "${DISK:-}" ] || disk_cfg "$DISK")"   # DISK=image: IDE primary master
vm_wait_text "DEL to enter SETUP" 60
vm_key $K_DEL
for _ in $(seq 60); do                          # F4 skips the IDE detection of the POST
  vm_screen
  grep -q "CMOS SETUP UTILITY" "$vm/screen.txt" && break
  grep -q "Press F4" "$vm/screen.txt" && vm_key 3e
  grep -q "Press F1 to continue, DEL" "$vm/screen.txt" && vm_key $K_DEL
  sleep 1
done
vm_expect "CMOS SETUP UTILITY"
for ((i = 0; i < menu; i++)); do vm_key $K_DOWN; done
vm_key $K_ENTER
sleep 1
[ -z "$auto_off" ] || vm_key 51                 # Auto Configuration: Disabled
for ((i = 0; i < item; i++)); do vm_key $K_DOWN; done
vm_screen; cp "$vm/screen.txt" "$vm/before.txt"
for ((i = 0; i < presses; i++)); do vm_key 51; done
vm_screen; diff "$vm/before.txt" "$vm/screen.txt" | grep '^>' > "$vm_dir/item.txt" || true
vm_key $K_ESC
sleep 1
vm_key $K_F10
sleep 1
vm_key 15 $K_ENTER                               # Y
for _ in $(seq 90); do
  vm_screen
  grep -Eq "DISK BOOT FAILURE|NO ROM BASIC|INSERT SYSTEM|DOS-TEST-OK" "$vm/screen.txt" && break
  grep -q "Press F4" "$vm/screen.txt" && vm_key 3e
  grep -q "Press F1 to continue" "$vm/screen.txt" && vm_key 3b
  sleep 1
done
vm_expect "DISK BOOT FAILURE|NO ROM BASIC|INSERT SYSTEM|DOS-TEST-OK"
sleep 2
vm_cmd pci "$vm_dir/pci.txt"
rm -f "$vm_dir/sio.txt"
for r in c0 c1; do vm_cmd out 108 aa; vm_cmd out 108 $r; vm_cmd in 109 "$vm_dir/sio.txt"; done
vm_cmd out 108 55
sleep 1
vm_stop
