#!/bin/bash
# Built-in network boot ROM (Etherboot rtl8139 + r8169 in the flash): an RTL8139C+ without cable and an
# RTL8169S, both without a boot ROM in their socket. The boot menu has one network entry; Etherboot tries
# the RTL8139 first, gets no DHCP answer, goes on with the RTL8169: PXE NBP (iPXE) -> Linux 6.18.
. "$(dirname "$0")/lib.sh"
[ -f "$nb/build/ipxe-chain.pxe" ] || "$nb/test/ipxe/build.sh"
[ -f "$nb/build/bzImage" ] || "$nb/test/linux/build.sh"
mkdir -p "$vm_dir/tftp"
cp "$nb/build/ipxe-chain.pxe" "$vm_dir/tftp/boot.pxe"
cp "$nb/build/bzImage" "$vm_dir/tftp/bzImage"
export BOX86_SLIRP_TFTP=$vm_dir/tftp BOX86_SLIRP_BOOTFILE=boot.pxe
vm_start pxebios "$rom" "[Network]
net_01_card = rtl8139c+
net_01_net_type = slirp
net_01_link = 2
net_02_card = rtl8169
net_02_net_type = slirp
net_02_link = 0"

vm_end_of_post 60
vm_key $K_ESC
vm_wait_text "Select boot device" 10
sleep 1
vm_screen
[ "$(grep -c "etherboot\.org" "$vm/screen.txt")" = 1 ] || fail "not exactly one network boot entry"
n=$(sed -nE 's/^([1-9])\. rtl8139--r8169\.zrom 5\.4\.4 .*/\1/p' "$vm/screen.txt")
[ -n "$n" ] || fail "no built-in network boot entry"
vm_key "$(printf '%02x' $((n + 1)))"                       # scan code of digit n
vm_wait_text "Etherboot 5\.4\.4" 20
vm_wait_text "UNDI device is PCI 00:05\.0" 120             # the NBP runs over Etherboot on the RTL8169

end=$((SECONDS + 240))
until grep -aq "LINUX-TEST-OK" "$vm/serial.log"; do
  [ $SECONDS -lt $end ] || { vm_screen; fail "Linux did not boot over the network"; }
  sleep 1
done
for re in "LINUX-TEST: 6\.18\." "8139too .* eth0: " "r8169 .* eth1: RTL8169s"; do
  grep -aEq "$re" "$vm/serial.log" || fail "serial log lacks /$re/"
done
vm_stop
