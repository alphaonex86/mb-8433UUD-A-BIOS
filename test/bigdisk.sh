#!/bin/bash
# 1 TB IDE disk (LBA48, as an SSD on an IDE adapter): size and LBA translation in setup, and the BIOS
# reads a sector beyond the 128 GiB LBA28 limit through INT 13h extensions, then writes and reads back
# 64 sectors there in block mode (READ/WRITE MULTIPLE EXT; test/bigdisk/*.asm), checked in the image.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
img=$vm_dir/big.img
rm -f "$img"
truncate -s $((121601 * 255 * 63 * 512)) "$img"   # sparse, 1 TB
nasm -f bin -o "$vm_dir/mbr.bin" "$nb/test/bigdisk/mbr.asm"
nasm -f bin -o "$vm_dir/far.bin" "$nb/test/bigdisk/far.asm"
dd if="$vm_dir/mbr.bin" of="$img" conv=notrunc status=none
dd if="$vm_dir/far.bin" of="$img" bs=512 seek=$((0x30000000)) conv=notrunc status=none
vm_start bigdisk "$rom" "[Storage controllers]
hdc = internal

[Hard disks]
hdd_01_parameters = 63, 255, 121601, 0, ide
hdd_01_fn = $img
hdd_01_ide_channel = 0:0"
vm_end_of_post 60
vm_expect "Pri. Master Disk : 9[0-9]{2}GB"
vm_setup
vm_tool 7 4                                        # HARDWARE INFORMATION: IDE DRIVES
vm_wait_text "Primary Master +: 86B_HD" 5
vm_expect "953867MB, CHS 16383/16/63, LBA, PIO mode"
vm_key $K_ESC $K_ESC $K_ESC
vm_wait_text "Quit Without Saving" 5
vm_key $K_Y
vm_wait_text "LBA48-OK|LBA48-FAIL" 30
vm_expect "LBA48-OK"
vm_stop
python3 -c "import struct,sys; f=open(sys.argv[1],'rb'); f.seek(0x30000001*512)
sys.exit(f.read(32768) != struct.pack('<16384H', *range(16384)))" "$img" || fail "image lacks the written sectors"
