#!/bin/bash
# Setup L2 SRAM TEST page: it finds the size and banks of any L2 the chipset takes, names the data chip that fails
# (86Box BOX86_L2_FAULT=bank:lane:mask: those bits of one chip read 0; ff: dead or missing) and finds a bad tag
# RAM bit (BOX86_L2_TAG_FAULT=mask), the dirty bit (80) included. With a faulty chip the POST crashes once the L2 is
# on, as on the real board: the next POST leaves it off (L2 init guard, or "the previous POST stopped with it on").
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
run() {  # L2 KB, banks, data fault, tag fault -> the page on screen
  BOX86_L2_FAULT=$3 BOX86_L2_TAG_FAULT=$4 vm_ram=$(ram_cfg 5 0 0 0 "$1" "$2") vm_start l2sram "$rom" ""
  local end=$((SECONDS + 60)) reset=
  # this POST at its prompt (the screen may still be the last VM's); a POST that hangs is reset once by hand
  until awk '/^RESET/ { p = 0 } /^B8$/ { p = 1 } END { exit !p }' "$vm/post.log" 2>/dev/null; do
    if [ $SECONDS -ge $end ]; then
      [ -z "$reset" ] || fail "$1K x $2 faults '$3' '$4': no POST prompt, even after a reset"
      reset=1 end=$((SECONDS + 60))
      vm_cmd reset
    fi
    sleep 0.5
  done
  vm_setup
  vm_tool 9 3                                       # HARDWARE TESTS: L2 SRAM TEST
  vm_wait_text "Found:|No L2 data SRAM" 60
}
chips() { printf "Found: %dK = %d bank%s x 4 chips of %dK x 8 = %d data chips \\\\+ 1 tag RAM" "$1" "$2" \
  "$([ "$2" = 2 ] && echo s || true)" $(($1 / $2 / 4)) $(($2 * 4)); }

for cfg in "256 1 7" "256 2 7" "64 1 7" "128 2 7" "512 2 6" "1024 2 5"; do
  set -- $cfg
  run "$1" "$2" "" ""
  vm_expect "$(chips "$1" "$2")"
  vm_expect "All $(($2 * 4)) data chips work"
  vm_expect "Tag RAM: OK \($3 tag bits, $(($1 * 64)) lines x 2\); dirty bit: OK"
  vm_stop
done
# a POST that stopped with the L2 on (as a bad L2 crashes, in B6 here): the next one leaves it off and says why
vm_start l2sram "$rom" ""
vm_end_of_post
vm_cmd out 70 3f                                    # CMOS 3Fh: the last SeaBIOS POST code
vm_cmd out 71 b6
vm_reset
vm_wait_text "The previous POST did not finish, its last POST code: B6" 60
vm_expect "Cache Memory +: 0K"
vm_setup
vm_menu 1
vm_wait_text "External Cache +: Enabled" 5          # the setting is kept: only this POST runs without it
vm_stop

export vm_post_crash_ok=1                           # the faulty chips below crash POSTs
run 256 2 1:2:10 ""                                 # the board's 2 x 4 chips: bit 4 of D16-D23 in bank 1
vm_expect "$(chips 256 2)"
vm_expect "Bank 0 +OK +OK +OK +OK"
vm_expect "Bank 1 +OK +OK +BAD 10 +OK"
vm_expect "FAIL: data chip bank 1, D16-D23: bits stuck"
vm_stop
run 256 2 0:0:ff ""                                 # chip D0-D7 of bank 0 dead or missing
vm_expect "Bank 0 +DEAD +OK +OK +OK"
vm_expect "FAIL: data chip bank 0, D0-D7: dead or missing"
vm_stop
run 256 2 "" 08                                     # tag RAM bit 3 reads 0
vm_expect "All 8 data chips work"
vm_expect "FAIL: tag RAM \(the 9th chip\): [0-9]+ of 32768 lines miss"
vm_expect "63 of 127 tag values kept: bit 3 stuck at 0"   # tag 3: 768K-1M, not testable
vm_stop
run 256 2 "" ff                                     # the tag RAM dead: it always reads 0
vm_expect "All 8 data chips work"
vm_expect "it reads 00 whatever is written: dead or a bad socket contact"
vm_stop
run 256 2 "" 80                                     # the dirty bit reads 0
vm_expect "Tag RAM: OK .*; dirty bit: FAIL"
vm_expect "FAIL: tag RAM bit 7 \(dirty\)"
vm_stop
run 512 2 0:3:ff 04                                 # two faults at once: chip D24-D31 of bank 0 and tag bit 2
vm_expect "$(chips 512 2)"
vm_expect "Bank 0 +OK +OK +OK +DEAD"
vm_expect "FAIL: data chip bank 0, D24-D31: dead or missing"
vm_expect "FAIL: tag RAM \(the 9th chip\): [0-9]+ of [0-9]+ lines miss"
vm_expect "tag values kept: bit 2 stuck at 0"
vm_stop
