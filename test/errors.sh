#!/bin/bash
# POST errors as the original BIOS: a cleared CMOS halts on "CMOS checksum error" until F1/DEL; drive A
# (1.44M by default) absent halts on "Floppy disk(s) fail (80)" unless Halt On excludes diskettes; LOAD BIOS
# DEFAULTS loads the fail-safe values and keeps the Standard CMOS page.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
vm_start errors "$rom" "[Floppy and CD-ROM drives]
fdd_01_type = none
fdd_02_type = none"

vm_wait_text "Press F1 to continue, DEL to enter SETUP" 60
vm_expect "CMOS checksum error - Defaults loaded"
vm_expect "Floppy disk\(s\) fail \(80\)"
vm_key $K_DEL                                     # the error prompt also enters setup
vm_wait_text "CMOS SETUP UTILITY" 10
menu_sel=0
vm_menu 0                                         # STANDARD CMOS SETUP
vm_wait_text "Date \(mm:dd:yy\)" 5
vm_expect "Drive A : 1.44M, 3.5 in."
vm_expect "Halt On : All Errors"
vm_save

vm_wait_text "Press F1 to continue, DEL to enter SETUP" 60
vm_expect "Floppy disk\(s\) fail \(80\)"
grep -q "CMOS checksum error" "$vm/screen.txt" && fail "CMOS checksum error after a save"
vm_key $K_DEL
vm_wait_text "CMOS SETUP UTILITY" 10
menu_sel=0
vm_menu 0
vm_wait_text "Date \(mm:dd:yy\)" 5
vm_key $K_RIGHT $K_RIGHT
for _ in $(seq 14); do vm_key $K_DOWN; done       # Halt On
vm_key $K_PGDN $K_PGDN $K_PGDN                    # All Errors -> All, But Diskette
vm_expect "Halt On : All, But Diskette"
vm_key $K_ESC
vm_menu 10                                        # LOAD BIOS DEFAULTS
vm_wait_text "Load BIOS Defaults \(Y/N\)" 5
vm_key $K_Y
vm_menu 1                                         # BIOS FEATURES SETUP
vm_wait_text "Quick Power On Self Test +: Disabled" 5
vm_key $K_ESC
vm_menu 5                                         # INTEGRATED PERIPHERALS
vm_wait_text "Onboard Serial Port 2 +: COM2/2F8" 5
vm_key $K_ESC
vm_menu 0                                         # Standard CMOS values kept
vm_wait_text "Halt On : All, But Diskette" 5
vm_expect "Drive A : 1.44M, 3.5 in."
vm_key $K_ESC
vm_save

vm_end_of_post 60                                 # diskette errors no longer halt
vm_expect "Floppy disk\(s\) fail \(80\)"
grep -q "Press F1 to continue" "$vm/screen.txt" && fail "halted although Halt On excludes diskettes"
vm_expect "Serial Port\(s\) +: 3F8 2F8"           # fail-safe COM2
vm_stop
