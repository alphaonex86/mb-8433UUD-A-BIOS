#!/bin/bash
# USB keyboard only (86Box UHCI, VIA 1106:3038 like the operator's card; low-speed HID boot keyboard; the PS/2
# keyboard never answers): no keyboard error at POST, F1 and DEL typed on it work, the setup's USB page lists it,
# Ctrl+Alt+Del on it reboots.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
export BOX86_USB_KBD=1 BOX86_KBD_POWERON_MS=600000
vm_start usbkbd "$rom" ""
vm_wait_text "Press F1 to continue, DEL to enter SETUP" 60   # fresh CMOS
vm_expect "CMOS checksum error"
grep -q "Keyboard error" "$vm/screen.txt" && fail "keyboard error with a USB keyboard"
vm_cmd usbkey 3a                                    # F1
vm_wait_text "Press DEL to enter SETUP, ESC for boot menu" 20
vm_cmd reset
vm_wait_text "Press DEL to enter SETUP" 60          # the CMOS is valid now: no error prompt
vm_cmd usbkey 4c                                    # DEL
vm_wait_text "CMOS SETUP UTILITY" 20
vm_cmd usbkey 51 51 51 51 51 51 28                  # down x6: HARDWARE INFORMATION, Enter
vm_wait_text "HARD DISK SPEED" 10
vm_cmd usbkey 51 51 28                              # USB DEVICES
vm_wait_text "UHCI 00:..\.0, 2 ports" 10
vm_expect "port 1: 1234:0001 keyboard, low speed"
vm_cmd usbkey 29 29                                 # ESC ESC
vm_wait_text "CMOS SETUP UTILITY" 10
vm_cmd usbkey 054c                                  # Ctrl+Alt+Del
vm_wait_text "Press DEL to enter SETUP" 60
vm_stop
