#!/bin/bash
# CMOS setup utility: defaults on a cleared CMOS, main menu and pages, edit date/floppy/boot sequence,
# save + reboot, values persist; hardware information pages; a disk on the secondary IDE channel.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
truncate -s $((20 * 16 * 63 * 512)) "$vm_dir/sec.img"
vm_start setup "$rom" "[Storage controllers]
hdc = internal

[Hard disks]
hdd_01_parameters = 63, 16, 20, 0, ide
hdd_01_fn = $vm_dir/sec.img
hdd_01_ide_channel = 1:0"

vm_end_of_post 60
vm_expect "CMOS checksum error - Defaults loaded"
vm_expect "Sec. Master Disk : 9MB"
vm_expect "CPU Type     : Am5x86"
vm_setup
for m in "STANDARD CMOS SETUP" "BIOS FEATURES SETUP" "CHIPSET FEATURES SETUP" "POWER MANAGEMENT SETUP" \
         "PNP/PCI CONFIGURATION" "INTEGRATED PERIPHERALS" "HARDWARE INFORMATION" "SUPERVISOR PASSWORD" \
         "IDE HDD AUTO DETECTION" "MEMORY TEST" "LOAD SETUP DEFAULTS" "SAVE & EXIT SETUP" "EXIT WITHOUT SAVING"; do
  vm_expect "$m"
done

vm_menu 0                                         # STANDARD CMOS SETUP
vm_wait_text "Date \(mm:dd:yy\)" 5
vm_expect "Secondary Master : Auto +9M +20 +16 +63"
year=$(sed -nE 's/.*Date \(mm:dd:yy\) : [0-9]+:[0-9]+:([0-9]{4}).*/\1/p' "$vm/screen.txt")
vm_key $K_RIGHT $K_RIGHT $K_PGDN                  # year + 1
for _ in $(seq 12); do vm_key $K_DOWN; done       # Drive A
vm_expect "Drive A : 1.44M, 3.5 in."               # default, as the original BIOS
vm_key $K_PGDN                                    # 1.44M -> 2.88M
vm_expect "Date \(mm:dd:yy\) : [0-9]+:[0-9]+:$((year + 1))"
vm_expect "Drive A : 2.88M, 3.5 in."
vm_key $K_ESC
vm_menu 1                                         # BIOS FEATURES SETUP
vm_wait_text "Boot Up NumLock Status" 5
vm_key $K_DOWN $K_DOWN $K_PGDN $K_PGDN $K_PGDN    # 1st boot device: Floppy -> Network (swaps)
vm_expect "1st Boot Device +: Network"
vm_expect "2nd Boot Device +: Floppy"
vm_expect "3rd Boot Device +: Hard Disk"
vm_expect "4th Boot Device +: CD-ROM"
vm_save

vm_end_of_post 60
grep -q "CMOS checksum error" "$vm/screen.txt" && fail "settings lost after reboot"
vm_expect "Diskette Drive A : 2.88M"
vm_setup
vm_menu 0
vm_wait_text "Date \(mm:dd:yy\) : [0-9]+:[0-9]+:$((year + 1))" 5
vm_expect "Drive A : 2.88M, 3.5 in."
vm_key $K_ESC
vm_menu 1
vm_wait_text "1st Boot Device +: Network" 5
vm_expect "4th Boot Device +: CD-ROM"
vm_key $K_ESC
vm_info 0                                         # HARDWARE INFORMATION: a menu of the pages
vm_wait_text "CPU +: Am5x86" 5
vm_expect "Core clock +: 1[0-9]{2} MHz"
vm_key $K_ESC $K_DOWN $K_ENTER
vm_wait_text "1060:8881 +UMC +Host bridge" 5
vm_key $K_ESC $K_DOWN $K_ENTER
vm_wait_text "USB DEVICES" 5
vm_expect "^ +none"                                # no USB controller in 86Box
vm_key $K_ESC $K_DOWN $K_ENTER
vm_wait_text "IRQ 15 : IDE secondary" 5
vm_key $K_ESC $K_DOWN $K_ENTER
vm_wait_text "Secondary Master : 86B_HD" 5
vm_expect "PIO mode [0-4] \(drive max [0-4]\), block (2|4|8|16|32|64|128) "   # IDE HDD Block Mode
vm_key $K_ESC $K_DOWN $K_ENTER
vm_wait_text "UM8881F +00:10.0" 5
vm_key $K_ESC $K_DOWN $K_ENTER                    # MEMORY SPEED: graphics (test/bench.sh checks it)
sleep 20
vm_key $K_ESC
vm_wait_text "HARD DISK SPEED" 20                 # back in text mode, in the HARDWARE INFORMATION menu
vm_key $K_ESC $K_ESC
vm_wait_text "Quit Without Saving" 5
vm_key $K_Y                                       # quit without saving: boot continues
vm_wait_text "No bootable device|Boot failed" 60
vm_stop
