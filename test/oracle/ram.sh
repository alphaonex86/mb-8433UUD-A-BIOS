#!/bin/bash
# Oracle for the RAM/L2 sizing: boot the original BIOS (BIOSORIGINAL.bin, a dump of the chip) and newBIOS on the same SIMM
# and L2 configuration of the fork's UM8881 model and print what each found, as a test/ram.sh line:
# banks 0-3, L2 KB and banks, memory KB, cache, 50h&0f 52h&7f 53h&3f 61h (MA11 bits of enabled regions).
# usage: ram.sh <bank0> <bank1> <bank2> <bank3> <L2 KB> <L2 banks>   (SIMM types: umc_hb4.c)
. "$(dirname "$0")/../lib.sh"
vm_dir=$nb/test/vm/oracle-ram-$(echo "$*" | tr ' ' _)
vm_ram=$(ram_cfg "$@")

fields() {
  local r52=$((16#$(vm_pci_byte 00:10.0 52) & 0x7f)) ma=0
  [ $((r52 & 0x07)) = 0 ] || ma=$((ma | 0x10))
  [ $((r52 & 0x70)) = 0 ] || ma=$((ma | 0x20))
  printf '%02x %02x %02x %02x' $((16#$(vm_pci_byte 00:10.0 50) & 0x0f)) $r52 \
    $((16#$(vm_pci_byte 00:10.0 53) & 0x3f)) $((16#$(vm_pci_byte 00:10.0 61) & ma))
}

vm_start oracle-ram-award "$nb/BIOSORIGINAL.bin"
vm_wait_text "Press F1" 120                          # fresh CMOS: checksum error
memory=$(grep -oE "Memory Test : *[0-9]+K" "$vm/screen.txt" | grep -oE "[0-9]+")
vm_key 3b
vm_wait_text "Cache Memory" 30
cache=$(grep -oE "Cache Memory *: *[0-9A-Za-z]+" "$vm/screen.txt" | awk '{print $NF}' | sed 's/None/none/')
award="$* $memory $cache $(fields)"
vm_kill

vm_start oracle-ram-newbios "$rom"
vm_end_of_post 180
memory=$(($(grep -oE "Extended Memory  : [0-9]+K" "$vm/screen.txt" | grep -oE "[0-9]+") + 1024))
cache=$(grep -oE "Cache Memory     : [0-9]+K" "$vm/screen.txt" | awk '{print $NF}' | sed 's/^0K$/none/')
newbios="$* $memory $cache $(fields)"
vm_kill

echo "award:   $award"
echo "newbios: $newbios"
[ "$award" = "$newbios" ] && echo SAME || echo DIFF
