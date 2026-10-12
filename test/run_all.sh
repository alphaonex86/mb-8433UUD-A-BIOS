#!/bin/bash
# Build everything the tests need (86Box fork, newBIOS ROM, iPXE NIC ROM, Linux test kernel) and run all
# tests in parallel, one 86Box per test. usage: run_all.sh [test...]   (default: all)
set -uo pipefail
here=$(dirname "$(readlink -f "$0")")
nb=$(dirname "$here")
"$nb/fetch_sources.sh" && "$nb/build_86box.sh" > /dev/null && "$nb/build.sh" > /dev/null && "$here/ipxe/build.sh" \
  && "$here/linux/build.sh" > /dev/null || { echo "BUILD FAILED" >&2; exit 1; }
[ $# -gt 0 ] || set -- setup dos scsi pxe pxebios pxe8169 chipset password bigdisk ram memtest post errors hddtype busclock isapnp
mkdir -p "$here/vm"
for t; do timeout 600 "$here/$t.sh" > "$here/vm/$t.out" 2>&1 & done
wait
fails=0
for t; do
  if grep -q "^PASS" "$here/vm/$t.out"; then echo "PASS $t"; else echo "FAIL $t (log: $here/vm/$t.out)"; fails=$((fails + 1)); fi
done
exit $fails
