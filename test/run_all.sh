#!/bin/bash
# Build everything the tests need (86Box fork, newBIOS ROMs with iPXE, Linux test kernel) and run all
# tests in parallel, one 86Box per test. usage: run_all.sh [test...]   (default: all)
set -uo pipefail
here=$(dirname "$(readlink -f "$0")")
nb=$(dirname "$here")
"$nb/fetch_sources.sh" && "$nb/build_86box.sh" > /dev/null && "$nb/build.sh" > /dev/null \
  && "$here/linux/build.sh" > /dev/null || { echo "BUILD FAILED" >&2; exit 1; }
[ $# -gt 0 ] || set -- setup dos pxe nicrom chipset password bigdisk ram memtest post errors hddtype busclock isapnp pnpbios fmtlint floppy deadcmos halt1 l2guard bootmenu cachetest kbdpower flashtime cachepage otherflash warmboot flashlinux usbkbd usbboot usbfault bench awarddefaults l2sram dramfuzz cmoslayout cmosfuzz devfuzz
mkdir -p "$here/vm"
for t; do (timeout 600 "$here/$t.sh" > "$here/vm/$t.out" 2>&1; echo $? > "$here/vm/$t.rc") & done
wait
fails=0
for t; do   # the exit status: a test with several VMs may print PASS for one, then fail
  if [ "$(cat "$here/vm/$t.rc")" = 0 ]; then echo "PASS $t"; else echo "FAIL $t (log: $here/vm/$t.out)"; fails=$((fails + 1)); fi
done
exit $fails
