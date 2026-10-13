#!/bin/bash
# Auto Configuration per bus clock, as the original BIOS's table: for each CPU the POST shows its clock
# and the chipset fields (HB50 L2 wait states, HB51 DRAM wait states, IBC56 ISA clock, IBCA4 host:PCI
# clock) are what the original BIOS sets on the same CPU in 86Box. usage: busclock.sh (all) | <one line>
. "$(dirname "$0")/lib.sh"
if [ $# -eq 0 ]; then
  mkdir -p "$vm_dir"
  # 86Box CPU family, speed, multiplier; MHz; HB50&30 HB51&f0 IBC56&0f IBCA4&03 (original BIOS values, but for
  # the Am5x86 at 3 x 40 MHz: the original BIOS assumes 4x and takes the 33 MHz row, newBIOS the 40 MHz one)
  xargs -P 6 -L 1 "$0" > "$vm_dir/all.log" 2>&1 <<'LIST'
am486dx2 50000000 2 50 30 f0 00 01
am5x86 133333333 4 133 10 a0 01 01
am486dx2 80000000 2 80 00 a0 00 02
am486dx4 120000000 3 120 00 a0 00 02
am5x86 120000000 3 120 00 a0 00 02
i486dx 50000000 1 50 00 00 00 00
LIST
  vm_name=busclock
  [ "$(grep -c '^PASS' "$vm_dir/all.log")" = 6 ] || { cat "$vm_dir/all.log"; fail "see above"; }
  echo "PASS [busclock]"
  exit 0
fi

vm_dir=$vm_dir/$1-$2
vm_cpu_family=$1 vm_cpu_speed=$2 vm_cpu_multi=$3
vm_start "busclock $1 $2" "$rom"
vm_end_of_post 120
vm_expect "CPU Clock    : $4MHz"
expect_bits "$(vm_pci_byte 00:10.0 50)" 0x30 "0x$5" "L2 wait states (HB50)"
expect_bits "$(vm_pci_byte 00:10.0 51)" 0xf0 "0x$6" "DRAM wait states (HB51)"
expect_bits "$(vm_pci_byte 00:12.0 56)" 0x0f "0x$7" "ISA/KBC clock (IBC56)"
expect_bits "$(vm_pci_byte 00:12.0 a4)" 0x03 "0x$8" "host:PCI clock (IBCA4)"
vm_stop
