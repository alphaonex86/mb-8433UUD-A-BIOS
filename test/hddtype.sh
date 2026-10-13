#!/bin/bash
# Hard disk types as the original BIOS: User geometry typed in Standard CMOS becomes the INT 13h geometry
# (DOS still boots, translation shown as TYPE); a fixed type with no drive halts on "Hard disk(s) fail".
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
dos_disk "$vm_dir/hdd.img"
vm_start hddtype "$rom" "$(disk_cfg "$vm_dir/hdd.img")"
vm_wait_text "Press F1 to continue, DEL to enter SETUP" 60
vm_key $K_DEL
vm_wait_text "CMOS SETUP UTILITY" 10
menu_sel=0
vm_menu 0                                         # STANDARD CMOS SETUP
vm_wait_text "Date \(mm:dd:yy\)" 5
vm_key $K_RIGHT $K_RIGHT $K_DOWN $K_DOWN $K_DOWN $K_DOWN   # Primary Master TYPE
vm_key $K_PGUP                                    # Auto -> User
vm_key $K_RIGHT 07 06 $K_RIGHT 02 07 $K_RIGHT 07 04   # CYLS 65, HEAD 16, SECTOR 63 (digit keys)
vm_expect "Primary Master   : User +31M +65 +16 +63"
for _ in $(seq 6); do vm_key $K_DOWN; done        # Secondary Slave TYPE
vm_key $K_PGDN $K_PGDN $K_PGDN                    # Auto -> None -> 1 -> 2
vm_expect "Secondary Slave  : 2 +20M +615 +4 +17"
vm_save

vm_wait_text "Press F1 to continue, DEL to enter SETUP" 60
vm_expect "Hard disk\(s\) fail \(80\)"
vm_key $K_DEL
vm_wait_text "CMOS SETUP UTILITY" 10
menu_sel=0
vm_info 4                                         # HARDWARE INFORMATION: IDE DRIVES
vm_wait_text "Primary Master   : 86B_HD" 5
vm_expect "CHS 65/16/63, TYPE, PIO mode"
vm_key $K_ESC $K_ESC $K_ESC
vm_wait_text "Quit Without Saving" 5
vm_key $K_Y
vm_wait_text "DOS-TEST-OK" 90                     # FreeDOS through the User geometry
vm_stop
