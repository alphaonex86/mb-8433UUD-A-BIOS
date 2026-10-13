#!/bin/bash
# PCI RTL8169S (gigabit) without a socket ROM, no disk, the iPXE of a 2 Mbit flash's lower half: with "1st boot
# device = Network" iPXE does DHCP and runs the boot file (an iPXE script: the Linux 6.18 bzImage with ip=dhcp),
# whose r8169 driver brings the link up and gets its address by DHCP. KERNEL: another build to test.
. "$(dirname "$0")/lib.sh"
kernel=${KERNEL:-$nb/build/bzImage}
[ -f "$kernel" ] || "$nb/test/linux/build.sh"
mkdir -p "$vm_dir/tftp"
printf '#!ipxe\nchain tftp://${next-server}/bzImage ip=dhcp\n' > "$vm_dir/tftp/boot.ipxe"
cp "$kernel" "$vm_dir/tftp/bzImage"
export BOX86_SLIRP_TFTP=$vm_dir/tftp BOX86_SLIRP_BOOTFILE=boot.ipxe BOX86_FLASH=w49f002a
vm_start pxe8169 "${rom%.rom}-2mbit.rom" "$(printf '[Network]\nnet_01_card = rtl8169\nnet_01_net_type = slirp\nnet_01_link = 0\n
[Realtek RTL8169S #1]\nbios = 0\n')"

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
