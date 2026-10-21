#!/bin/bash
# LILO's 3 s boot prompt from a USB disk without an AT keyboard: the board's 8042 then hangs on the first byte sent
# to the keyboard (IBF stays set). The countdown must still run out and Linux start (the board froze at "02s"):
# with a USB keyboard, and with no keyboard at all (HALT_ON=1 ROM: Halt On: No Errors).
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
usb_stick "$vm_dir/usb.img"
export BOX86_USB_DISK=$vm_dir/usb.img BOX86_KBD_ABSENT=1
lilo_runs_out() {
  vm_wait_text "boot:" 120
  vm_wait_text "Loading linux" 10 || fail "LILO's countdown does not run out ($1)"
  vm_stop
}
BOX86_USB_KBD=1 vm_start usblilo "$rom" ""
vm_wait_text "Press F1 to continue" 90
vm_cmd usbkey 3a                                    # F1 (fresh CMOS)
lilo_runs_out "USB keyboard"
vm_start usblilo "$nb/out/newbios-halt1.rom" ""
lilo_runs_out "no keyboard"
