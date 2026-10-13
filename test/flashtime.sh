#!/bin/bash
# Flash access budget (86Box counts, interpreter): on the board each byte of the BIOS flash is an 8-bit ISA cycle
# (~1 us, never cached before the shadow copy), which 86Box does not slow down; a loop left running from the flash
# was fast here and minutes long on the board (POST 38/D1). Estimated as 3 bytes per instruction fetched plus the
# bytes read, the POST must stay under 1 s of flash time, and nothing may run from the flash after it.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
rm -f "$vm_dir/flash.txt"
vm_dynarec=0 vm_ram="$(ram_cfg 0 0 3 3 256 2)" vm_start flashtime "$rom" ""
vm_wait_text "Press F1 to continue, DEL to enter SETUP" 120
vm_cmd flash "$vm/flash.txt"
sleep 3
vm_cmd flash "$vm/flash.txt"
for _ in $(seq 50); do [ "$(wc -l < "$vm/flash.txt" 2>/dev/null)" = 2 ] && break; sleep 0.1; done
read -r fetches bytes < "$vm/flash.txt"
[ -n "$fetches" ] || fail "no flash counters"
us=$((fetches * 3 + bytes))
echo "POST: $fetches instructions fetched from the flash, $bytes bytes read: about $((us / 1000)) ms on the board"
[ $us -le 1000000 ] || fail "flash time over 1 s"
read -r fetches bytes < <(tail -1 "$vm/flash.txt")
[ "$fetches" = 0 ] && [ "$bytes" = 0 ] || fail "after the POST: $fetches instructions and $bytes bytes from the flash"
vm_stop
