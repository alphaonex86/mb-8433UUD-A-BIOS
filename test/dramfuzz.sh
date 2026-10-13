#!/bin/bash
# Native (no emulator) test of coreboot's UM8881 DRAM and L2 sizing (raminit.c, memmap.c, the romstage
# defaults) and of SeaBIOS's L2 enable at POST (setup_l2_cache in setup.c), compiled unchanged, against a
# C port of the 86Box fork's DRAM/L2 model plus a normal-mode L2 (test/native/um8881_model.c): every
# combination of the 10 SIMM types in the 4 banks times 81 L2 hardware variants (absent, real 64K-1M
# in 1 or 2 banks, random power-on contents, fake data SRAM and/or tag RAM; setup option, CMOS validity and
# unfinished-POST guard). Also checks the register fields
# against the oracle lines of test/ram.sh and the original BIOS's values measured on a real board.
# usage: [SEABIOS=tree] dramfuzz.sh   (default tree build/seabios; failing cases: test/vm/dramfuzz/failures.txt)
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
nb=$(dirname "$here")
cb=$nb/sources/coreboot_486/src
um=$cb/northbridge/umc/um8881
sb=${SEABIOS:-$nb/build/seabios}
out=$here/vm/dramfuzz
mkdir -p "$out"
rm -f "$out"/*.o
[ -f "$sb/out-rel/autoconf.h" ] || { echo "FAIL [dramfuzz]: no SeaBIOS build in $sb (run build.sh)"; exit 1; }

car=()
for opt in CONFIG_DCACHE_RAM_BASE CONFIG_DCACHE_RAM_SIZE; do
  value=$(sed -n "s/^$opt=//p" "$nb/configs/mb8433_author.config")
  [ -n "$value" ] || { echo "FAIL [dramfuzz]: no $opt in configs/mb8433_author.config"; exit 1; }
  car+=("-D$opt=$value")
done
sections=(-ffunction-sections -fdata-sections)   # only what is reached is linked (--gc-sections)
flags=(-m32 -O2 -fno-pie "${sections[@]}" -I"$here/native/include" -I"$um" -I"$cb" -I"$cb/mainboard/biostar/mb8433" "${car[@]}")
for src in "$um/raminit.c" "$um/memmap.c" "$um/reg_init.c" "$here/native/romstage_seam.c"; do
  gcc "${flags[@]}" -Wall -Wno-format -c "$src" -o "$out/$(basename "$src" .c).o"   # coreboot code; %lu = uintptr_t only there
done
for src in "$here/native/um8881_model.c" "$here/native/dramfuzz.c"; do
  gcc "${flags[@]}" -Wall -Wextra -Werror -c "$src" -o "$out/$(basename "$src" .c).o"
done
# setup.c as devfuzz.sh compiles it (test/native/seabios/seam.h), its asm l2_fill weakened for the model's
gcc -m32 -O1 -fno-pie "${sections[@]}" -ffreestanding -fno-builtin -fno-strict-aliasing -fno-delete-null-pointer-checks \
  -DMODE16=0 -DMODESEGMENT=0 -iquote "$sb/out-rel" -iquote "$sb/src" -I"$here/native/seabios" \
  -include "$here/native/seabios/seam.h" -w -c "$here/native/seabios_l2.c" -o "$out/seabios_l2.o"
objcopy --weaken-symbol=l2_fill --weaken-symbol=l2_line_works --weaken-symbol=l2_caches "$out/seabios_l2.o"
gcc -m32 -no-pie -Wl,--gc-sections "$out"/*.o -o "$out/dramfuzz"

# banks 0-3, L2 KB, L2 banks, memory KB, cache, 50h&0f 52h&7f 53h&3f 61h&30 ("-": not known)
{
  sed -n '/xargs.*<<.EOF.$/,/^EOF$/p' "$here/ram.sh" | sed '1d;$d'
  # Original BIOS on a real MB-8433UUD-A, one SIMM per socket (notes in reg_init.h); it also sets 61h
  # bit 5 (region U MA11) with region U empty, only bit 4 is compared
  cat <<'EOF'
6 0 0 0 256 1 32768 - - 05 02 10
0 6 0 0 256 1 32768 - - 05 12 10
0 0 6 0 256 1 32768 - - 0d 02 10
0 0 0 6 256 1 32768 - - 0d 12 10
3 0 0 0 256 1 16384 - - 04 00 00
0 3 0 0 256 1 16384 - - 04 10 00
0 0 3 0 256 1 16384 - - 0c 00 00
0 0 0 3 256 1 16384 - - 0c 10 00
EOF
} > "$out/oracle.txt"
[ "$(grep -c . "$out/oracle.txt")" = 18 ] || { echo "FAIL [dramfuzz]: oracle lines not found in ram.sh"; exit 1; }

if "$out/dramfuzz" "$out/failures.txt" < "$out/oracle.txt"; then
  echo "PASS [dramfuzz]"
else
  echo "FAIL [dramfuzz]: see above and $out/failures.txt"
  exit 1
fi
