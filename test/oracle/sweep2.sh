#!/bin/bash
# Second oracle pass: every value of the multi-valued chipset items, Auto Configuration itself, and the
# IDE PIO modes with a disk on the primary master (Award programs the timings only for present drives).
here=$(dirname "$(readlink -f "$0")")
vm=$here/../vm
jobs=()
add() { jobs+=("$*"); }
add base-chip0 2 -1 0
for p in 2 3; do
  for i in 1 2 3 4 15 18 20 21 22 23; do add "chip$p-$i" 2 "$i" "$p" 1; done
done
for p in 0 1 2 3 4 5; do add "pio-$p" 4 8 "$p"; done
printf '%s\n' "${jobs[@]}" | DISK=$vm/../../vm/dos.img xargs -P 8 -L 1 bash -c \
  'timeout 300 '"$here"'/award.sh "$0" "$@" > '"$vm"'/oracle-$0.out 2>&1'
