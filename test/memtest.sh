#!/bin/bash
# Setup MEMORY TEST (memtest86+ 4.20 tests 0-8) on one 4 MB SIMM: a full pass without errors on good RAM;
# with the fork's faulty bit (bit 3 of 2AA400h reads 0) POST reports the block and the test the address,
# the expected and read values and the bit. usage: memtest.sh (both) | memtest.sh good|stuck
. "$(dirname "$0")/lib.sh"
if [ $# -eq 0 ]; then
  mkdir -p "$vm_dir"
  vm_name=memtest
  "$0" good > "$vm_dir/good.log" 2>&1 & good=$!
  "$0" stuck > "$vm_dir/stuck.log" 2>&1 || { cat "$vm_dir/stuck.log"; fail "stuck bit"; }
  wait $good || { cat "$vm_dir/good.log"; fail "good RAM"; }
  echo "PASS [memtest]"
  exit 0
fi

vm_dir=$vm_dir/$1
vm_ram=$(ram_cfg 1 0 0 0 128 1)
[ "$1" = good ] || vm_ram+=$'\nstuck_addr = 2794496\nstuck_bit = 3'   # 2AA400h
vm_start "memtest $1" "$rom"
vm_end_of_post 60
[ "$1" = good ] || vm_expect "Memory test fail at 2aa000"
vm_setup
vm_menu 9                                         # MEMORY TEST
vm_wait_text "Memory   : 4096K" 5
vm_expect "Testing  : 1024K - [0-9]+K"
if [ "$1" = good ]; then
  vm_wait_text "Pass complete, no errors" 600
  vm_expect "Errors   : 0 "
else
  vm_wait_text " 2   002aa400  ffffffff  fffffff7  00000008  [0-9]+" 120
  vm_expect "Err-Bits : 00000008"
fi
vm_key $K_ESC
vm_wait_text "CMOS SETUP UTILITY" 10
vm_stop
