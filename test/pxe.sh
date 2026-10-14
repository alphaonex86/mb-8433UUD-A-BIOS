#!/bin/bash
# Boot order + network boot from the card: the board's 2 Mbit flash (86Box W49F002A, A17 high: the BIOS in its upper
# half), the operator's plain RTL8139C with Etherboot 5.4.4 in its socket ROM, and a DOS disk. The default order boots
# DOS; with "1st boot device = Network" Etherboot does DHCP and loads iPXE by TFTP, which loads Linux 6.18. The same
# with iPXE itself as the socket ROM (64 KiB, build/nic-rtl8139-ipxe-64k.rom) on the operator's card (its EEPROM dump
# set to 64 KiB, its MAC): it loads pxelinux.0 (the host's syslinux) from the TFTP root, which boots the kernel
# /mb-8433UUD/linux as the card's pxelinux.cfg/01-<MAC> says, as on the operator's /tftproot (no default file).
. "$(dirname "$0")/lib.sh"
test_linux
dos_disk "$vm_dir/hdd.img"
net_boot_dir $'#!ipxe\nchain tftp://${next-server}/bzImage\n'
cp "$nb/build/bzImage" "$vm_dir/tftp/bzImage"
export BOX86_FLASH=w49f002a
netboot() {  # socket ROM, regex of its banner, [regex of the serial log] -> DOS by default; network first: Linux
  vm_start pxe "${rom%.rom}-2mbit.rom" "$(disk_cfg "$vm_dir/hdd.img")
$(nic_cfg "$1")"

  vm_end_of_post
  vm_wait_text "DOS-TEST-OK" 90
  vm_reset
  vm_wait_text "Press DEL to enter SETUP" 40
  vm_setup
  vm_menu 1                                                  # BIOS FEATURES SETUP
  vm_wait_text "1st Boot Device" 5
  vm_net_first
  vm_save
  vm_wait_text "$2" 40

  end=$((SECONDS + 180))
  until grep -aq "LINUX-TEST-OK" "$vm/serial.log"; do
    [ $SECONDS -lt $end ] || { vm_screen; fail "Linux did not boot over the network"; }
    sleep 1
  done
  for re in ${3:+"$3"} "LINUX-TEST: 6\.18\." "BIOS-e820: \[mem 0x0000000000000000-0x000000000009[0-9a-f]{4}\] usable" \
            "BIOS-e820: \[mem 0x0000000000100000-0x0000000001[ef][0-9a-f]{5}\] usable" "apm: BIOS version 1\.2" \
            "8139too .* eth0: .* IRQ [0-9]+" "sda: sda1"; do
    grep -aEq "$re" "$vm/serial.log" || fail "serial log lacks /$re/"
  done
  vm_stop
}
netboot "$etherboot" "Etherboot 5\.4\.4"
sl=/usr/share/syslinux
t=$vm_dir/tftp
for f in pxelinux.0 ldlinux.c32; do cp "$sl/$f" "$t/" || fail "no $sl/$f (syslinux)"; done
mkdir -p "$t/pxelinux.cfg" "$t/mb-8433UUD"
mv "$t/bzImage" "$t/mb-8433UUD/linux"
printf 'DEFAULT linux\n\nPROMPT 0\nTIMEOUT 1\n\nLABEL linux\n    KERNEL /mb-8433UUD/linux\n    APPEND console=ttyS0,115200 from-pxelinux-cfg\n' \
  > "$t/pxelinux.cfg/01-00-e0-7d-9a-a8-3e"   # as on the operator's /tftproot
card=$nb/test/roms/rtl8139-operator-eeprom.txt   # the operator's card (not published)
sed 's/0a:4d10/0a:4d14/' "$card" > "$vm_dir/eeprom-64k.txt" || fail "no $card"
export BOX86_RTL8139_EEPROM=$vm_dir/eeprom-64k.txt
netboot "$ipxe_rom" "iPXE initialising devices" "Kernel command line: .*from-pxelinux-cfg"
