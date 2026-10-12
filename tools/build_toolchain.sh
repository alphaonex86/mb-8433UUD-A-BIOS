#!/bin/bash
# Build coreboot's i386 cross gcc and iasl once (idempotent). Host gcc>=14 needs C17 + permissive,
# texinfo 7 rejects GCC's docs.
set -euo pipefail
cg=$(dirname "$(readlink -f "$0")")/../sources/coreboot_486/util/crossgcc
if [ ! -x "$cg/xgcc/bin/i386-elf-gcc" ]; then
  (cd "$cg" && gcc_cv_prog_makeinfo_modern=no CFLAGS="-std=gnu17 -fpermissive" \
    ./buildgcc -y -p i386-elf -j "$(nproc)" -d "$PWD/xgcc")
fi
if [ ! -x "$cg/xgcc/bin/iasl" ]; then
  (cd "$cg" && CFLAGS="-std=gnu17 -fpermissive" ./buildgcc -y -P iasl -j "$(nproc)" -d "$PWD/xgcc")
fi
