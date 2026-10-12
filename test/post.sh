#!/bin/bash
# POST codes on port 80h (fork POST card log): a boot without disk sends exactly the "Normal boot" codes
# of POSTCODES.md in order, setup and its memory test send theirs, the failed boot ends with BD.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
normal=$(sed -n '/^## Normal boot/,/^## Other/p' "$nb/POSTCODES.md" | grep -oE '^\| `[0-9A-F]{2}`' | grep -oE '[0-9A-F]{2}')
codes() { tr '\n' ' ' < "$vm/post.log" | sed 's/ $//'; }
vm_start post "$rom"
vm_wait_text "Press DEL to enter SETUP, ESC for boot" 60
upto_prompt=$(echo "$normal" | sed '/^B8$/q' | tr '\n' ' ' | sed 's/ $//')
[ "$(codes)" = "$upto_prompt" ] || fail "POST codes: $(codes) - expected: $upto_prompt"
vm_setup
[ "$(tail -1 "$vm/post.log")" = B9 ] || fail "no B9 in setup: $(codes)"
vm_menu 9                                         # MEMORY TEST
vm_wait_text "Testing  :" 10
[ "$(tail -1 "$vm/post.log")" = BA ] || fail "no BA in the memory test: $(codes)"
vm_key $K_ESC
vm_wait_text "CMOS SETUP UTILITY" 10
vm_key $K_ESC
vm_wait_text "Quit Without Saving" 5
vm_key $K_Y
vm_wait_text "No bootable device" 60
expected="$upto_prompt B9 BA B9 $(echo "$normal" | sed -n '/^B8$/,$p' | sed 1d | tr '\n' ' ')BD"
[ "$(codes)" = "$expected" ] || fail "POST codes: $(codes) - expected: $expected"
vm_stop
