#!/bin/bash
# Oracle for test/busclock.sh: chipset fields of the Auto Configuration after POST on one CPU, under the
# original BIOS (BIOSORIGINAL.bin) and mb-8433UUD. usage: busclock.sh <86Box CPU family> <speed> <multiplier>
. "$(dirname "$0")/../lib.sh"
vm_cpu_family=$1 vm_cpu_speed=$2 vm_cpu_multi=$3
fields() {
  printf '%02x %02x %02x %02x' $((16#$(vm_pci_byte 00:10.0 50) & 0x30)) $((16#$(vm_pci_byte 00:10.0 51) & 0xf0)) \
    $((16#$(vm_pci_byte 00:12.0 56) & 0x0f)) $((16#$(vm_pci_byte 00:12.0 a4) & 0x03))
}
vm_dir=$nb/test/vm/oracle-busclock-$1-$2
vm_start oracle-busclock-award "$nb/BIOSORIGINAL.bin"
vm_wait_text "Press F1" 120
vm_key $K_F1
vm_wait_text "Updating ESCD|Cache Memory" 60
award=$(fields)
vm_kill
vm_start oracle-busclock-newbios "$rom"
vm_end_of_post 120
newbios=$(fields)
vm_kill
echo "award:   $award"
echo "newbios: $newbios"
[ "$award" = "$newbios" ] && echo SAME || echo DIFF
