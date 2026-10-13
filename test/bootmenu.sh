#!/bin/bash
# Boot menu: ESC at the "Press DEL to enter SETUP, ESC for boot menu" prompt, and F8 in the setup's main
# menu (leaves without saving).
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
vm_start bootmenu "$rom" ""
vm_wait_text "Press F1 to continue, DEL to enter SETUP" 60   # fresh CMOS
vm_key $K_F1
vm_wait_text "Press DEL to enter SETUP, ESC for boot menu" 20
vm_key $K_ESC
vm_wait_text "Select boot device" 10
vm_cmd reset
vm_wait_text "Press DEL to enter SETUP" 60                # CMOS now valid: no error prompt
vm_setup
vm_expect "F8 : Boot Menu"
vm_key 42                                           # F8
vm_wait_text "Select boot device" 10
vm_stop
