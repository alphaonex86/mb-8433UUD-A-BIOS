#!/bin/bash
# PCI RTL8169S (gigabit) with an iPXE ROM in its boot ROM socket, no disk: with "1st boot device = Network"
# iPXE does DHCP + TFTP of the Linux 6.18 bzImage with ip=dhcp, whose r8169 driver brings the link up and
# gets its address by DHCP. NIC_ROM / KERNEL: other builds to test.
. "$(dirname "$0")/lib.sh"
nic_rom=${NIC_ROM:-$nb/build/ipxe-rtl8169.rom} kernel=${KERNEL:-$nb/build/bzImage}
[ -f "$nic_rom" ] || "$nb/test/ipxe/build.sh"
[ -f "$kernel" ] || "$nb/test/linux/build.sh"
mkdir -p "$vm_dir/tftp" "$vm_dir/roms/network/rtl8169"
cp "$nic_rom" "$vm_dir/roms/network/rtl8169/rtl8169.rom"
cp "$kernel" "$vm_dir/tftp/bzImage"
export BOX86_SLIRP_TFTP=$vm_dir/tftp BOX86_SLIRP_BOOTFILE=bzImage
vm_start pxe8169 "$rom" "$(printf '[Network]\nnet_01_card = rtl8169\nnet_01_net_type = slirp\nnet_01_link = 0\n
[Realtek RTL8169S #1]\nbios = 1\n')"

vm_end_of_post 60
vm_setup
vm_menu 1                                                  # BIOS FEATURES SETUP
vm_wait_text "1st Boot Device" 5
vm_key $K_DOWN $K_DOWN $K_PGDN $K_PGDN $K_PGDN             # 1st boot device: Floppy -> Network
vm_expect "1st Boot Device +: Network"
vm_save
vm_wait_text "iPXE .* Open Source Network Boot Firmware" 40

end=$((SECONDS + 180))
until grep -aq "LINUX-TEST-OK" "$vm/serial.log"; do
  [ $SECONDS -lt $end ] || { vm_screen; fail "Linux did not boot over the network"; }
  sleep 1
done
for re in "LINUX-TEST: 6\.18\." "r8169 .* eth0: RTL8169s, ([0-9a-f]{2}:){5}[0-9a-f]{2}, XID 008, IRQ [0-9]+" \
          "r8169 .* eth0: Link is Up - 1Gbps/Full" "IP-Config: Got DHCP answer from 10\.0\.2\.2, my address is 10\.0\.2\.15"; do
  grep -aEq "$re" "$vm/serial.log" || fail "serial log lacks /$re/"
done
vm_stop
