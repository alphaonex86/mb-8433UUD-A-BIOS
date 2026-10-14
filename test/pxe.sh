#!/bin/bash
# Boot order + network boot from the card: the board's 2 Mbit flash (86Box W49F002A, A17 high: the BIOS in its upper
# half), the operator's plain RTL8139C with Etherboot 5.4.4 in its socket ROM, and a DOS disk. The default order boots
# DOS; with "1st boot device = Network" Etherboot does DHCP and loads iPXE by TFTP, which loads Linux 6.18.
. "$(dirname "$0")/lib.sh"
[ -f "$nb/build/bzImage" ] || "$nb/test/linux/build.sh"
dos_disk "$vm_dir/hdd.img"
net_boot_dir $'#!ipxe\nchain tftp://${next-server}/bzImage\n'
cp "$nb/build/bzImage" "$vm_dir/tftp/bzImage"
export BOX86_FLASH=w49f002a
vm_start pxe "${rom%.rom}-2mbit.rom" "$(disk_cfg "$vm_dir/hdd.img")
$(nic_cfg "$etherboot")"

vm_end_of_post
vm_wait_text "DOS-TEST-OK" 90
vm_cmd reset
vm_wait_text "Press DEL to enter SETUP" 40
vm_setup
vm_menu 1                                                  # BIOS FEATURES SETUP
vm_wait_text "1st Boot Device" 5
vm_net_first
vm_save
vm_wait_text "Etherboot 5\.4\.4" 40

end=$((SECONDS + 180))
until grep -aq "LINUX-TEST-OK" "$vm/serial.log"; do
  [ $SECONDS -lt $end ] || { vm_screen; fail "Linux did not boot over the network"; }
  sleep 1
done
for re in "LINUX-TEST: 6\.18\." "BIOS-e820: \[mem 0x0000000000000000-0x000000000009[0-9a-f]{4}\] usable" \
          "BIOS-e820: \[mem 0x0000000000100000-0x0000000001[ef][0-9a-f]{5}\] usable" "apm: BIOS version 1\.2" \
          "8139too .* eth0: .* IRQ [0-9]+" "sda: sda1"; do
  grep -aEq "$re" "$vm/serial.log" || fail "serial log lacks /$re/"
done
vm_stop
