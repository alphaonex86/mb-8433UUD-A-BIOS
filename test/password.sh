#!/bin/bash
# Supervisor password: set in setup, asked when entering setup again (wrong one refused) and, with
# Security Option "System", before booting.
. "$(dirname "$0")/lib.sh"
type_word() { local k; for k in "$@"; do vm_key "$k"; done; vm_key $K_ENTER; }
ABC="1e 30 2e" XYZ="2d 15 2c"
vm_start password "$rom"
vm_end_of_post 60
vm_setup
vm_menu 6                                          # SUPERVISOR PASSWORD
vm_wait_text "Enter Password:" 5
type_word $ABC
vm_wait_text "Confirm Password:" 5
type_word $ABC
vm_wait_text "Password set" 5
vm_key $K_ENTER
vm_menu 1                                          # BIOS FEATURES: security option System
for _ in $(seq 12); do vm_key $K_DOWN; done
vm_key $K_PGDN
vm_expect "Security Option +: System"
vm_save

vm_wait_text "Enter Password:" 60                  # before boot
type_word $XYZ
vm_wait_text "Enter Password:" 5
type_word $ABC
vm_end_of_post 30
vm_key $K_DEL
vm_wait_text "Enter Password:" 10                  # entering setup
type_word $ABC
vm_wait_text "CMOS SETUP UTILITY" 10
vm_stop
