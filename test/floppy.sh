#!/bin/bash
# Boot from drive A (1.44M, the default first boot device): the boot sector runs and reads sector 2 with
# INT 13h; no hard disk.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
nasm -f bin -o "$vm_dir/floppy.img" "$nb/test/floppy/boot.asm"
vm_start floppy "$rom" "[Floppy and CD-ROM drives]
fdd_01_type = 35_2hd
fdd_01_fn = $vm_dir/floppy.img"
vm_wait_text "Press F1 to continue" 60                  # fresh CMOS
vm_key $K_F1
vm_wait_text "FLOPPY-BOOT FLOPPY-READ-OK" 60
vm_stop
