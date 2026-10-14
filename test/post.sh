#!/bin/bash
# POST codes on port 80h (fork POST card log): a boot without disk sends exactly the "Normal boot" codes
# of POSTCODES.md in order, setup and its memory test send theirs, the failed boot ends with BD.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
normal=$(sed -n '/^## Normal boot/,/^## Other/p' "$nb/POSTCODES.md" | grep -oE '^\| `[0-9A-F]{2}`' | grep -oE '[0-9A-F]{2}')
codes() { grep -E '^[0-9A-F]{2}$' "$vm/post.log" | uniq | tr '\n' ' ' | sed 's/ $//'; }
vm_start post "$rom"
vm_end_of_post 60
upto_prompt=$(echo "$normal" | sed '/^B8$/q' | tr '\n' ' ' | sed 's/ $//')
[ "$(codes)" = "$upto_prompt" ] || fail "POST codes: $(codes) - expected: $upto_prompt"
vm_setup
[ "$(codes | sed 's/.* //')" = B9 ] || fail "no B9 in setup: $(codes)"
vm_menu 9                                         # MEMORY TEST
vm_wait_text "Testing  :" 10
[ "$(codes | sed 's/.* //')" = BA ] || fail "no BA in the memory test: $(codes)"
vm_key $K_ESC
vm_wait_text "CMOS SETUP UTILITY" 10
vm_key $K_ESC
vm_wait_text "Quit Without Saving" 5
vm_key $K_Y
vm_wait_text "No bootable device" 60
expected="$upto_prompt B9 BA B9 $(echo "$normal" | sed -n '/^B8$/,$p' | sed 1d | tr '\n' ' ')BD"
[ "$(codes)" = "$expected" ] || fail "POST codes: $(codes) - expected: $expected"
# each WBINVD empties the whole L2 on this chipset: a POST memory test that flushed per block with the L2 on
# took minutes on the real board
n=$(grep -c "WBINVD with the L2 on" "$vm/86box.log" || true)
[ "$n" -le 100 ] || fail "$n WBINVD with the L2 on (the setup memory test page uses a few)"
vm_stop
