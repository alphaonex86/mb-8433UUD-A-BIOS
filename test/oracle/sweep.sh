#!/bin/bash
# Run award.sh for every item of the given Award setup pages, 8 VMs in parallel, then print for each
# changed item the PCI config bytes and super I/O registers that differ from the page baseline.
# usage: sweep.sh   -> vm/oracle-*/ and test/oracle/award_map.txt
here=$(dirname "$(readlink -f "$0")")
vm=$here/../vm
jobs=()
add() { jobs+=("$*"); }
add base-bios 1 -1 0
add base-chip 2 -1 0 1
add base-pci 4 -1 0
for i in $(seq 0 21); do add "bios-$i" 1 "$i" 1; done
for i in $(seq 1 23); do add "chip-$i" 2 "$i" 1 1; done
for i in $(seq 0 22); do add "pci-$i" 4 "$i" 1; done
printf '%s\n' "${jobs[@]}" | xargs -P 8 -L 1 bash -c 'timeout 300 '"$here"'/award.sh "$0" "$@" > '"$vm"'/oracle-$0.out 2>&1'
for d in "$vm"/oracle-bios-* "$vm"/oracle-chip-* "$vm"/oracle-pci-*; do
  n=$(basename "$d"); page=${n#oracle-}; page=${page%-*}
  echo "== $n: $(sed 's/^> *//; s/  */ /g' "$d/item.txt" 2>/dev/null | head -2 | tr '\n' '|')"
  diff <(cut -c1-60 "$vm/oracle-base-$page/pci.txt") <(cut -c1-60 "$d/pci.txt") | grep '^[<>]'
  sio() { gawk '{v = strtonum("0x" $2); if (NR == 1) v = and(v, 0x1f); printf "sio C%d %02x\n", NR - 1, v}' "$1"; }  # C0 bits 7-5 read random
  diff <(sio "$vm/oracle-base-$page/sio.txt") <(sio "$d/sio.txt") | grep '^[<>]'
done > "$here/award_map.txt"
grep -c "^==" "$here/award_map.txt"
