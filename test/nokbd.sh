#!/bin/bash
# The board as the operator runs it: no AT keyboard (its 8042 then hangs on the first byte sent to the keyboard,
# IBF stays set), a USB keyboard and the USB stick image on the VT6212 card. Everything works from the USB
# keyboard: F1 at the fresh-CMOS error, DEL, each setup page and back, F10 save, the boot menu, LILO ("dos"
# typed), MS-DOS ("ver"), Ctrl+Alt+Del (the reset starts within 3 s: each 8042 wait took 500 ms, 5 s in all),
# then LILO's countdown runs out and Linux finds the disk on EHCI and the keyboard on its UHCI companion.
# With no keyboard at all, the POST errors do not halt the boot (nobody could press F1).
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
cp --sparse=always "$nb/../HDD-build/output/images/usb.img" "$vm_dir/usb.img"
export BOX86_USB_DISK=$vm_dir/usb.img BOX86_USB_KBD=1 BOX86_KBD_ABSENT=1
key() { vm_cmd usbkey "$@"; }
wait_gone() {  # regex, timeout: the screen no longer shows it
  local end=$((SECONDS + $2))
  while vm_screen; grep -Eq "$1" "$vm/screen.txt"; do [ $SECONDS -lt $end ] || fail "still on screen: /$1/"; sleep 0.3; done
}
main="SAVE & EXIT SETUP"

vm_start nokbd "$rom" ""
vm_wait_text "Press F1 to continue" 90
grep -q "Keyboard error" "$vm/screen.txt" && fail "keyboard error with a USB keyboard"
key 3a                                              # F1
vm_wait_text "Press DEL to enter SETUP" 20
key 4c                                              # DEL
vm_wait_text "$main" 20
sel=0
for entry in "0 STANDARD CMOS SETUP" "1 BIOS FEATURES SETUP" "2 CHIPSET FEATURES SETUP" "3 POWER MANAGEMENT SETUP" \
             "4 PNP/PCI CONFIGURATION" "5 INTEGRATED PERIPHERALS" "7 CHIPSET REGISTERS" "8 MEMORY" "9 BOARD TEST"; do
  read -r i title <<< "$entry"
  for ((; sel < i; sel++)); do key 51; done         # down
  key 28                                            # Enter
  wait_gone "$main" 10
  vm_expect "$title"
  key 29                                            # ESC
  vm_wait_text "$main" 10
done
key 43                                              # F10
vm_wait_text "SAVE to CMOS and EXIT" 5
key 1c                                              # Y
vm_wait_text "Press DEL to enter SETUP, ESC for boot menu" 60
key 29                                              # ESC: boot menu
vm_wait_text "Select boot device" 10
key 1e                                              # 1: the USB stick
vm_wait_text "boot:" 60
key 07 12 16 28                                     # d o s Enter
vm_wait_text 'C:\\>' 90
key 19 08 15 28                                     # v e r Enter
vm_wait_text "MS-DOS Version 6" 10
resets=$(grep -c '^RESET' "$vm/post.log")
t=$SECONDS
key 054c                                            # Ctrl+Alt+Del
until [ "$(grep -c '^RESET' "$vm/post.log")" -gt "$resets" ]; do
  [ $((SECONDS - t)) -le 3 ] || fail "Ctrl+Alt+Del: no reset within 3 s"
  sleep 0.1
done
vm_wait_text "boot:" 90                             # no key: LILO's default after its 3 s
vm_wait_text "Loading linux" 10 || fail "LILO's countdown does not run out"
end=$((SECONDS + 300))                              # Linux: the disk on EHCI, the keyboard on its UHCI companion
until grep -aq "hid-generic.*Keyboard" "$vm/serial.log"; do [ $SECONDS -lt $end ] || fail "Linux: no USB keyboard"; sleep 2; done
grep -aq "ehci-pci .*USB 2.0 started" "$vm/serial.log" || fail "Linux: EHCI did not start"
grep -aq " sda: sda1 sda2 sda3 sda4" "$vm/serial.log" || fail "Linux: no USB disk"
! grep -aq "nobody cared" "$vm/serial.log" || fail "Linux: an interrupt nobody handles"
vm_stop

# No keyboard at all (fresh CMOS, Halt On: All Errors): nobody could press F1, the errors show and the boot goes on
cp --sparse=always "$nb/../HDD-build/output/images/usb.img" "$vm_dir/usb.img"
unset BOX86_USB_KBD
vm_start nokbd "$rom" ""
vm_wait_text "boot continues \(no keyboard\)" 90
vm_expect "Keyboard error or no keyboard present"
vm_expect "CMOS checksum error"
vm_wait_text "Loading linux" 120 || fail "no boot without a keyboard"
vm_stop

