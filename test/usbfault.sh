#!/bin/bash
# USB faults injected by the 86Box VT6212 model (BOX86_USB_FAULT), with a FreeDOS USB disk and no IDE disk: the POST
# always finishes and the boot fails with a message instead of hanging.
#  kbd-stall   the USB keyboard stalls its requests, no PS/2 keyboard: keyboard error, the boot goes on (no F1 prompt:
#              no keyboard could answer it) and DOS starts
#  disk-nak    the disk never sends data       disk-stall  it stalls the data of READ
#  disk-csw    its CSWs have a bad signature   disk-gone   unplugged 3 s after its first READ (DOS boot), then
#              Ctrl+Alt+Del: the POST finishes without it
. "$(dirname "$0")/lib.sh"
dos_disk "$vm_dir/usb.img"
start() {  # fault
  export BOX86_USB_FAULT=$1 BOX86_USB_KBD=1 BOX86_USB_DISK=$vm_dir/usb.img
  vm_start "usbfault $1" "$rom" ""
}
export BOX86_KBD_POWERON_MS=600000                 # no PS/2 keyboard
start kbd-stall
vm_wait_text "boot continues \\(no keyboard\\)" 120
vm_expect "Keyboard error or no keyboard present"
vm_wait_text "DOS-TEST-OK" 120
vm_kill
unset BOX86_KBD_POWERON_MS
for f in disk-nak disk-stall disk-csw; do
  start $f
  vm_end_of_post 180
  vm_wait_text "No bootable device|Boot failed" 180 || fail "$f: no boot failure message"
  ! grep -q "DOS-TEST-OK" "$vm/screen.txt" || fail "$f: DOS booted from a failing disk"
  echo "usbfault $f: POST finished, boot failed with a message"
  vm_kill
done
start disk-gone
vm_end_of_post 120
sleep 10                                            # the disk is gone 3 s after the first READ
vm_cad 120
vm_wait_text "No bootable device|Boot failed" 180
echo "usbfault disk-gone: POST finished without the unplugged disk"
vm_stop
