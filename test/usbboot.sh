#!/bin/bash
# Boot from a USB disk (86Box UHCI, bulk-only mass storage; no IDE disk): FreeDOS boots from it. Then Ctrl+Alt+Del
# while the USB controller runs the schedule DOS's BIOS calls left (the operator's GRUB2-from-USB case: a warm
# boot does not reset the card): the POST finishes and boots from USB again, twice. With an IDE disk too, the
# default order (Hard Disk, USB, ...) boots the IDE disk, and "1st Boot Device: USB" the USB disk.
. "$(dirname "$0")/lib.sh"
dos_disk "$vm_dir/usb.img"
export BOX86_USB_DISK=$vm_dir/usb.img
vm_start usbboot "$rom" ""
vm_end_of_post
vm_wait_text "DOS-TEST-OK" 120
for n in 1 2; do
  vm_cad
  vm_wait_text "DOS-TEST-OK" 120
done
vm_stop

marker() { printf '@echo off\r\necho BOOTED-%s\r\n' "$1" > "$vm_dir/$2.bat"; dos_disk "$vm_dir/$2.img" "$vm_dir/$2.bat"; }
marker IDE ide
marker USB usb
vm_start usbboot "$rom" "$(disk_cfg "$vm_dir/ide.img")"
vm_end_of_post
vm_wait_text "BOOTED-IDE" 120
vm_reset
vm_end_of_post 60
vm_setup
vm_menu 1                                           # BIOS FEATURES SETUP
vm_wait_text "1st Boot Device" 5
vm_key $K_DOWN $K_DOWN $K_PGUP $K_PGUP              # Hard Disk -> Floppy -> USB
vm_expect "1st Boot Device +: USB"
vm_save
vm_wait_text "BOOTED-USB" 120
vm_stop
