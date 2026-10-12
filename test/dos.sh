#!/bin/bash
# DOS: FreeDOS 1.3 boots from the IDE disk (INT 13h/10h/16h/1Ah) and loads HIMEMX (XMS via INT 15h);
# the shared RAM of a WD8013 ISA network card at D0000 is reachable. POST screen: board name, release
# date, drive detection lines.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
nasm -f bin -o "$vm_dir/shmem.com" "$nb/test/dos/shmem.asm"
dos_disk "$vm_dir/hdd.img" "$vm_dir/shmem.com"
vm_start dos "$rom" "$(disk_cfg "$vm_dir/hdd.img")

[Network]
net_01_card = wd8013ebt
net_01_net_type = slirp
net_01_link = 0"
vm_wait_text "Detecting HDD Primary Master +\.\.\. 86B_HD" 30   # POST screen: board, release date, drives
vm_expect "^Biostar MB-8433UUD-A"
vm_expect "^[0-3][0-9]/[01][0-9]/20[0-9]{2} "
vm_expect "Detecting HDD Primary Slave +\.\.\. None"
vm_expect "^rel-[^ ]*-[0-9]{8}_[0-9]{6} "             # SeaBIOS version and build time, nothing after
vm_end_of_post
vm_wait_text "DOS-TEST-OK" 90
expect_bits "$(vm_cr0)" 0x60000000 0 "L1 cache enabled in DOS (CR0.CD, NW)"
# Auto Configuration: the original BIOS's values on this 33 MHz bus (test/oracle baseline)
expect_bits "$(vm_pci_byte 00:10.0 50)" 0x30 0x10 "L2 3-1-1-1 (HB50)"
expect_bits "$(vm_pci_byte 00:10.0 51)" 0xf0 0xa0 "DRAM read/write 1 WS (HB51)"
expect_bits "$(vm_pci_byte 00:10.0 53)" 0x40 0x00 "no burst copy-back (HB53)"
expect_bits "$(vm_pci_byte 00:10.0 61)" 0xc0 0x00 "no EDO (HB61)"
for r in 43 44; do                                # PCI INTA#-INTD# never on IRQ 14/15 (IDE)
  v=$((16#$(vm_pci_byte 00:12.0 $r)))
  [ $((v >> 4)) -lt 14 ] && [ $((v & 15)) -lt 14 ] || fail "PCI interrupt on IRQ 14/15 (IBC$r=$v)"
done
vm_expect "FreeDOS"
vm_expect "Extended \(XMS\) +31,[45][0-9]{2}K"   # 32 MB minus HMA and CBMEM/SeaBIOS at the top
vm_expect "Conventional +63[0-9]K"
vm_expect "SHMEM-OK"
vm_stop
