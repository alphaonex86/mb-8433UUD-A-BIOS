#!/bin/bash
# The ROMs for the board's flash: out/release/ holds them only when both were built from the current sources and every
# test passed (test/run_all.sh, which also rebuilds out/newbios.rom). A failing run leaves out/release/ empty.
# *-2mbit.rom: for a 2 Mbit chip (W49F002A...): iPXE in the lower 128 KiB, the BIOS in the upper.
set -euo pipefail
nb=$(dirname "$(readlink -f "$0")")
rel=$nb/out/release
mkdir -p "$rel"
rm -f "$rel/newbios.rom" "$rel/newbios-halt1.rom" "$rel/newbios-2mbit.rom" "$rel/newbios-halt1-2mbit.rom" "$rel/SHA256SUMS"
HALT_ON=1 "$nb/build.sh"
"$nb/test/run_all.sh" || { echo "RELEASE REFUSED: tests failed (above)" >&2; exit 1; }
cp "$nb/out/newbios.rom" "$nb/out/newbios-halt1.rom" "$nb/out/newbios-2mbit.rom" "$nb/out/newbios-halt1-2mbit.rom" "$rel/"
(cd "$rel" && sha256sum newbios.rom newbios-halt1.rom newbios-2mbit.rom newbios-halt1-2mbit.rom > SHA256SUMS)
ls -l "$rel"
