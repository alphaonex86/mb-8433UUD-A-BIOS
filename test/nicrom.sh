#!/bin/bash
# The RTL8139C's socket ROM (Etherboot) reaches the boot menu only when the card's 93C46 EEPROM selects a boot ROM
# size (CONFIG0 BS2-0, byte 14h): the operator's card had 000, so F8 offered no network boot on the real board
# (86Box BOX86_RTL8139_EEPROM: its EEPROM dump); the same card with 64 KiB (tools/rtl8139-bootrom 64) offers it.
# Then tools/rtl8139-bootrom in Linux (booted by that Etherboot) writes 000, 64 KiB, 000: after a reset, no network boot.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
dos_disk "$vm_dir/hdd.img"
card=$nb/test/roms/rtl8139-operator-eeprom.txt   # the operator's card (not published)
grep -q "^.*0a:4d10" "$card" || fail "$card: not the operator's EEPROM (word 0Ah 4D10h, no boot ROM)"
sed 's/0a:4d10/0a:4d14/' "$card" > "$vm_dir/eeprom-64k.txt"

boot_menu() {  # EEPROM dump -> the F8 boot menu on screen
  BOX86_RTL8139_EEPROM=$1 vm_start nicrom "$rom" "$(disk_cfg "$vm_dir/hdd.img")
$(nic_cfg "$etherboot")"
  vm_end_of_post 60
  vm_setup
  vm_key 42                                         # F8
  vm_wait_text "Floppy \[drive A\]" 10
}
boot_menu "$card"
grep -q "etherboot" "$vm/screen.txt" && fail "network boot offered with the card's boot ROM off"
vm_stop
boot_menu "$vm_dir/eeprom-64k.txt"
vm_expect "rtl8139\.zrom 5\.4\.4 \(GPL\) etherboot\.org"
vm_stop

test_linux
net_boot_dir $'#!ipxe\nchain tftp://${next-server}/bzImage ip=dhcp console=ttyS0 romtest\n'
cp "$nb/build/bzImage" "$vm_dir/tftp/bzImage"
"$nb/sources/buildroot_486/output/host/bin/i486-buildroot-linux-gnu-gcc" -static -Os -Wall -Werror \
  -o "$vm_dir/tftp/rtl8139-bootrom" "$nb/tools/rtl8139-bootrom.c" || fail "rtl8139-bootrom does not build"
BOX86_RTL8139_EEPROM=$vm_dir/eeprom-64k.txt vm_start nicrom "$rom" "$(nic_cfg "$etherboot")"
vm_end_of_post 60
vm_setup
vm_menu 1                                                  # BIOS FEATURES SETUP
vm_wait_text "1st Boot Device" 5
vm_net_first
vm_save
end=$((SECONDS + 300))
until grep -aq "ROMTEST-RC=" "$vm/serial.log"; do [ $SECONDS -lt $end ] || fail "Linux did not run rtl8139-bootrom"; sleep 1; done
log=$(tr -d '\r' < "$vm/serial.log")
grep -q "ROMTEST-RC=0" <<< "$log" || { grep -A12 "EEPROM:" <<< "$log"; fail "rtl8139-bootrom failed"; }
grep -q "boot ROM: 64 KiB (CONFIG0 14h)" <<< "$log" || fail "rtl8139-bootrom did not read 64 KiB"
for w in "4d14 -> 4d10" "4d10 -> 4d14" "4d14 -> 4d10"; do
  grep -q "EEPROM word 0ah: $w" <<< "$log" || fail "rtl8139-bootrom did not write $w"
done
vm_reset
vm_setup
vm_key 42                                                  # F8
vm_wait_text "Floppy \[drive A\]" 10
grep -q "etherboot" "$vm/screen.txt" && fail "network boot still offered after rtl8139-bootrom 0"
vm_stop
