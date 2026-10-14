#!/bin/bash
# The ROMs for the board's flash: out/release/ holds them only when both were built from the current sources and every
# test passed (test/run_all.sh, which also rebuilds out/newbios.rom). A failing run leaves out/release/ empty.
# *-2mbit.rom: for a 2 Mbit chip (W49F002A...): the BIOS in both halves (the socket keeps A17 high).
# nic-rtl8139-ipxe-64k.rom: iPXE for the RTL8139's boot ROM socket (tools/build_ipxe.sh).
set -euo pipefail
nb=$(dirname "$(readlink -f "$0")")
rel=$nb/out/release
mkdir -p "$rel"
roms=(newbios.rom newbios-halt1.rom newbios-2mbit.rom newbios-halt1-2mbit.rom)
rm -f "${roms[@]/#/$rel/}" "$rel/nic-rtl8139-ipxe-64k.rom" "$rel/SHA256SUMS" "$rel/newbios-mb-8433UUD-A.zip"
HALT_ON=1 "$nb/build.sh"
"$nb/tools/build_ipxe.sh"
"$nb/test/run_all.sh" || { echo "RELEASE REFUSED: tests failed (above)" >&2; exit 1; }
cp "${roms[@]/#/$nb/out/}" "$nb/build/nic-rtl8139-ipxe-64k.rom" "$rel/"
(cd "$rel" && sha256sum "${roms[@]}" nic-rtl8139-ipxe-64k.rom > SHA256SUMS)
# The archive to publish: the BIOS for the board's original chip, its POST codes and dist/README.txt, nothing else
zip=$rel/newbios-mb-8433UUD-A.zip
stage=$(mktemp -d)
trap 'rm -r "$stage"' EXIT
cp "$rel/newbios.rom" "$stage/BIOS.bin"
cp "$nb/POSTCODES.md" "$stage/POSTCODES.txt"
cp "$nb/dist/README.txt" "$stage/"
(cd "$stage" && zip -q -X -D "$zip" BIOS.bin POSTCODES.txt README.txt)
ls -l "$rel"
