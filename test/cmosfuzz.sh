#!/bin/bash
# Native (no emulator) fuzz of the CMOS values coreboot reads (src/mainboard/biostar/mb8433/newbios.c):
# romstage chipset options, ramstage Auto timings, PCI IRQ presets through the PIRQ routing; compiled
# unchanged against CMOS, PCI and PIT seams, with array bounds, division and shifts trapping.
# usage: cmosfuzz.sh
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
nb=$(dirname "$here")
cb=$nb/sources/coreboot_486/src
out=$here/vm/cmosfuzz
mkdir -p "$out"

defs=()
for opt in PIRQ_ROUTE DEBUG_PIRQ GENERATE_PIRQ_TABLE MAX_PIRQ_LINKS IRQ_SLOT_COUNT; do
  value=$(sed -n "s/^CONFIG_$opt=//p" "$nb/configs/mb8433_author.config")
  case "$value" in y) value=1 ;; "") value=0 ;; esac
  defs+=("-DCONFIG_$opt=$value")
done
flags=(-m32 -O2 -pthread -fsanitize=bounds,integer-divide-by-zero,shift -fsanitize-trap=all -I"$here/native/include" -I"$cb/northbridge/umc/um8881"
  -I"$cb/arch/486/include" -I"$cb/mainboard/biostar/mb8433" "${defs[@]}")
for src in "$cb/arch/486/pirq_routing.c" "$cb/mainboard/biostar/mb8433/irq_tables.c" \
           "$cb/southbridge/umc/um8886/pirq.c"; do
  gcc "${flags[@]}" -include types.h -Wall -c "$src" -o "$out/$(basename "$src" .c).o"   # u8 before pirq_routing.h
done
gcc "${flags[@]}" -Wall -Wextra -Werror -Wno-unused-parameter -Wno-overflow -c "$here/native/cmosfuzz.c" \
  -o "$out/cmosfuzz.o"   # newbios.c inside: ~mask into a u8 parameter
gcc -m32 -pthread "$out"/*.o -o "$out/cmosfuzz"

if "$out/cmosfuzz"; then
  echo "PASS [cmosfuzz]"
else
  echo "FAIL [cmosfuzz]: see above"
  exit 1
fi
