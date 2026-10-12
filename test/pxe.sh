#!/bin/bash
# Boot order + NIC socket ROM: the operator's RTL8139C ROM (Etherboot 5.4.4 rtl8139.zrom, first 32 KiB of
# the chip) and a DOS disk. The default order boots DOS; with "1st boot device = Network" Etherboot does
# DHCP + TFTP of a PXE NBP (iPXE), which loads Linux 6.18 by TFTP.
. "$(dirname "$0")/lib.sh"
[ -f "$nb/build/ipxe-chain.pxe" ] || "$nb/test/ipxe/build.sh"
[ -f "$nb/build/bzImage" ] || "$nb/test/linux/build.sh"
dos_disk "$vm_dir/hdd.img"
mkdir -p "$vm_dir/tftp"
cp "$nb/build/ipxe-chain.pxe" "$vm_dir/tftp/boot.pxe"
cp "$nb/build/bzImage" "$vm_dir/tftp/bzImage"
export BOX86_SLIRP_TFTP=$vm_dir/tftp BOX86_SLIRP_BOOTFILE=boot.pxe
vm_start pxe "$rom" "$(disk_cfg "$vm_dir/hdd.img")
$(nic_cfg "$nb/test/roms/etherboot-5.4.4-rtl8139.rom")"

vm_wait_text "DOS-TEST-OK" 90
vm_cmd reset
vm_wait_text "Press DEL to enter SETUP" 40
vm_setup
vm_menu 1                                                  # BIOS FEATURES SETUP
vm_wait_text "1st Boot Device" 5
vm_key $K_DOWN $K_DOWN $K_PGDN $K_PGDN $K_PGDN             # 1st boot device: Floppy -> Network
vm_expect "1st Boot Device +: Network"
vm_save
vm_wait_text "Etherboot 5\.4\.4" 40

end=$((SECONDS + 180))
until grep -aq "LINUX-TEST-OK" "$vm/serial.log"; do
  [ $SECONDS -lt $end ] || { vm_screen; fail "Linux did not boot over the network"; }
  sleep 1
done
for re in "LINUX-TEST: 6\.18\." "BIOS-e820: \[mem 0x0000000000000000-0x000000000009[0-9a-f]{4}\] usable" \
          "BIOS-e820: \[mem 0x0000000000100000-0x0000000001f[89a-f][0-9a-f]{2}ff\] usable" "apm: BIOS version 1\.2" \
          "8139too .* eth0: .* IRQ [0-9]+" "sda: sda1"; do
  grep -aEq "$re" "$vm/serial.log" || fail "serial log lacks /$re/"
done
vm_stop
