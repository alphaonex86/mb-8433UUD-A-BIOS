#!/bin/bash
# Boot from a USB disk (86Box UHCI, bulk-only mass storage; no IDE disk): FreeDOS boots from it. Then Ctrl+Alt+Del
# while the USB controller runs the schedule DOS's BIOS calls left (the operator's GRUB2-from-USB case: a warm
# boot does not reset the card): the POST finishes and boots from USB again, twice.
. "$(dirname "$0")/lib.sh"
dos_disk "$vm_dir/usb.img"
export BOX86_USB_DISK=$vm_dir/usb.img
vm_start usbboot "$rom" ""
vm_end_of_post
vm_wait_text "DOS-TEST-OK" 120
for n in 1 2; do
  vm_cmd combo 1d 38 53                             # Ctrl+Alt+Del
  vm_wait_text "Press DEL to enter SETUP" 60 || fail "warm boot $n from USB: no POST prompt"
  vm_wait_text "DOS-TEST-OK" 120
done
vm_stop
