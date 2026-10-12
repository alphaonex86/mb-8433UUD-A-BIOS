#!/bin/bash
# SIMM and L2 cache configurations on the fork's UM8881 DRAM/L2 model: coreboot sizes the banks and the
# cache, the POST summary shows them and the memory test passes. The expected sizes and register fields (50h, 52h,
# 53h, 61h of the enabled regions) are what the original BIOS gets on the same configuration, except
# bank 1 alone, where the original BIOS does not start. usage: ram.sh (all) | ram.sh <one config line>
. "$(dirname "$0")/lib.sh"
if [ $# -eq 0 ]; then
  mkdir -p "$vm_dir"
  # banks 0-3 (SIMM types of umc_hb4.c), L2 KB and banks, memory KB, cache, 50h&0f 52h&7f 53h&3f 61h&30
  xargs -P 3 -L 1 "$0" > "$vm_dir/all.log" 2>&1 <<'EOF'
1 0 0 0 128 1 4096 128K 02 02 00 00
2 2 0 0 256 1 16384 256K 03 04 03 00
4 4 0 0 256 2 32768 256K 0b 05 01 10
3 0 3 0 512 2 32768 512K 0c 44 00 00
1 0 2 0 128 1 12288 128K 02 2b 02 00
9 0 0 0 512 1 131072 512K 04 07 02 10
5 5 5 5 0 1 131072 none 00 66 0f 00
8 0 0 0 1024 1 65536 1024K 05 06 00 10
0 1 0 0 256 1 4096 256K 03 02 10 00
EOF
  vm_name=ram
  [ "$(grep -c '^PASS' "$vm_dir/all.log")" = 9 ] || { cat "$vm_dir/all.log"; fail "see above"; }
  echo "PASS [ram]"
  exit 0
fi

tag=$(echo "${@:1:6}" | tr ' ' _)
vm_dir=$vm_dir/$tag
vm_ram=$(ram_cfg "${@:1:6}")
vm_start "ram $tag" "$rom"
vm_wait_text "Press DEL to enter SETUP, ESC for boot" 120   # summary box, kept until boot
cache=$8; [ "$cache" = none ] && cache=0K
grep -q "Extended Memory  : $(($7 - 1024))K" "$vm/screen.txt" || fail "no 'Extended Memory  : $(($7 - 1024))K'"
grep -q "Cache Memory     : $cache" "$vm/screen.txt" || fail "no 'Cache Memory     : $cache'"
grep -q "Memory Test FAIL" "$vm/screen.txt" && fail "memory test failed"
expect_bits "$(vm_pci_byte 00:10.0 50)" 0x0f "0x$9" "L2 size/banks"
expect_bits "$(vm_pci_byte 00:10.0 52)" 0x7f "0x${10}" "DRAM regions"
expect_bits "$(vm_pci_byte 00:10.0 53)" 0x3f "0x${11}" "DRAM banks"
expect_bits "$(vm_pci_byte 00:10.0 61)" 0x30 "0x${12}" "MA11"
vm_stop
