#!/bin/bash
# Boot order + network boot from the flash: a 2 Mbit flash (86Box W49F002A) whose lower half holds iPXE, the
# operator's card (a plain RTL8139C, no C+ mode: iPXE's legacy mode, PCI ID 10ec:8039, no socket ROM) and a DOS
# disk. The default order boots DOS; with "1st boot device = Network" iPXE does DHCP and runs the DHCP boot file
# (an iPXE script: the kernel by TFTP), Linux 6.18.
. "$(dirname "$0")/lib.sh"
[ -f "$nb/build/bzImage" ] || "$nb/test/linux/build.sh"
dos_disk "$vm_dir/hdd.img"
mkdir -p "$vm_dir/tftp"
cp "$nb/test/ipxe/chain.ipxe" "$vm_dir/tftp/boot.ipxe"
cp "$nb/build/bzImage" "$vm_dir/tftp/bzImage"
export BOX86_SLIRP_TFTP=$vm_dir/tftp BOX86_SLIRP_BOOTFILE=boot.ipxe BOX86_FLASH=w49f002a
vm_start pxe "${rom%.rom}-2mbit.rom" "$(disk_cfg "$vm_dir/hdd.img")
$(printf '[Network]\nnet_01_card = rtl8139c+\nnet_01_net_type = slirp\nnet_01_link = 0\n
[Realtek RTL8139C+ #1]\nbios = 0\ndevice_id = 8039\ncplus = 0\n')"

vm_end_of_post
vm_wait_text "DOS-TEST-OK" 90
vm_cmd reset
vm_wait_text "Press DEL to enter SETUP" 40
vm_setup
vm_menu 1                                                  # BIOS FEATURES SETUP
vm_wait_text "1st Boot Device" 5
vm_key $K_DOWN $K_DOWN $K_PGDN $K_PGDN $K_PGDN             # 1st boot device: Floppy -> Network
vm_expect "1st Boot Device +: Network"
vm_save
vm_wait_text "iPXE .* Open Source Network Boot Firmware" 40
vm_wait_text "net0: .* using rtl8039 on" 30

end=$((SECONDS + 180))
until grep -aq "LINUX-TEST-OK" "$vm/serial.log"; do
  [ $SECONDS -lt $end ] || { vm_screen; fail "Linux did not boot over the network"; }
  sleep 1
done
# (the top of RAM below CBMEM, less what iPXE hides for itself)
for re in "LINUX-TEST: 6\.18\." "BIOS-e820: \[mem 0x0000000000000000-0x000000000009[0-9a-f]{4}\] usable" \
          "BIOS-e820: \[mem 0x0000000000100000-0x0000000001[ef][0-9a-f]{5}\] usable" "apm: BIOS version 1\.2" \
          "sda: sda1"; do                        # (8139too does not know 10ec:8039: no eth0 here)
  grep -aEq "$re" "$vm/serial.log" || fail "serial log lacks /$re/"
done
vm_stop
