#!/bin/bash
# Build newBIOS for the Biostar MB-8433UUD-A: coreboot_486 (UMC chipset init) + SeaBIOS (BIOS services,
# option ROMs, CMOS setup) + Etherboot (built-in network boot for RTL8139/RTL8169 cards without a boot ROM).
# Output: out/newbios.rom (flash, also used by the 86Box tests).
# Sources: fetch_sources.sh (pinned coreboot_486 and seabios_486 trees, Etherboot tarball, in sources/).
# usage: [DEBUG=1] build.sh    DEBUG=1 also builds out/newbios-debug.rom (logs on COM1).
# The patches in patches/ are the source of truth: a missing work tree is recreated from them, and they
# are re-exported after each build so they always match the built ROM.
set -euo pipefail
nb=$(dirname "$(readlink -f "$0")")
cb=$nb/sources/coreboot_486
sb=$nb/build/seabios
out=$nb/out
mkdir -p "$out"

"$nb/fetch_sources.sh" firmware
"$nb/tools/build_toolchain.sh"
"$nb/tools/build_etherboot.sh"
xgcc=$cb/util/crossgcc/xgcc/bin

if [ ! -d "$sb/.git" ]; then
  git clone -q "$nb/sources/seabios_486" "$sb"
  git -C "$sb" apply "$nb/patches/seabios.patch"
fi
if git -C "$cb" diff --quiet; then
  git -C "$cb" apply "$nb/patches/coreboot.patch"
fi
# CMOS layout shared by the setup utility (SeaBIOS) and the chipset options (coreboot).
for dst in "$sb/src/newbios_cmos.h" "$cb/src/mainboard/biostar/mb8433/newbios_cmos.h"; do
  cmp -s "$nb/cmos.h" "$dst" || cp "$nb/cmos.h" "$dst"
done

seabios() {  # name, extra SeaBIOS config -> $sb/out-<name>/bios.bin.elf
  local cfg=$sb/.config-$1 log=$out/seabios-$1.build.log
  { cat "$cb/src/mainboard/biostar/mb8433/config_seabios" "$nb/seabios.config"; echo "$2"; } > "$cfg"
  local mk=(make -C "$sb" -s CROSS_PREFIX="$xgcc/i386-elf-" OUT="out-$1/" KCONFIG_CONFIG="$cfg")
  "${mk[@]}" olddefconfig > "$log" 2>&1
  "${mk[@]}" -j"$(nproc)" >> "$log" 2>&1 \
    || { tail -30 "$log"; echo "SEABIOS BUILD FAILED (log: $log)" >&2; exit 1; }
}

# Same host-tool/warning workarounds as build_variants.sh (gcc>=15 host, author's %u for size_t).
coreboot() {  # tag, SeaBIOS name, extra coreboot config, [CBFS files to add: name=path ...]
  local tag=$1 cfg=$out/$1.config obj=build-$1 log=$out/$1.build.log
  { cat "$nb/configs/mb8433_author.config"; cat <<EOF
# CONFIG_DEBUG_HW_BREAKPOINTS is not set
# CONFIG_DEBUG_NULL_DEREF_BREAKPOINTS is not set
# CONFIG_DEBUG_CBFS is not set
# CONFIG_DEBUG_PIRQ is not set
# CONFIG_DEFAULT_CONSOLE_LOGLEVEL_8 is not set
CONFIG_DEFAULT_CONSOLE_LOGLEVEL_4=y
# CONFIG_PAYLOAD_SEABIOS is not set
CONFIG_PAYLOAD_ELF=y
CONFIG_PAYLOAD_FILE="$sb/out-$2/bios.bin.elf"
CONFIG_COMPRESSED_PAYLOAD_LZMA=y
CONFIG_POST_DEVICE=y
CONFIG_POST_IO=y
# CONFIG_DECOMPRESS_OFAST is not set
$3
EOF
  } > "$cfg"
  local mk=(make -C "$cb" -s DOTCONFIG="$cfg" obj="$obj" HOSTCFLAGS="-g -Wno-error" WERROR= CONFIG_WARNINGS_ARE_ERRORS=)
  "${mk[@]}" olddefconfig > "$log" 2>&1
  "${mk[@]}" -j"$(nproc)" >> "$log" 2>&1 \
    || { tail -30 "$log"; echo "BUILD FAILED: $tag (log: $log)" >&2; exit 1; }
  local rom=$cb/$obj/coreboot.rom
  cp "$rom" "$out/$tag.rom"
  for f in "${@:4}"; do
    "$cb/$obj/cbfstool" "$out/$tag.rom" add -t raw -n "${f%%=*}" -f "${f#*=}" > /dev/null \
      || { echo "BUILD FAILED: $tag, no room for ${f%%=*}" >&2; exit 1; }
  done
  "$cb/$obj/cbfstool" "$out/$tag.rom" print > "$out/$tag.cbfs.txt"
  echo "$out/$tag.rom: $(awk '/empty/ {s += $4} END {print s}' "$out/$tag.cbfs.txt") bytes free"
}

seabios rel ""
coreboot newbios rel "# CONFIG_CONSOLE_SERIAL is not set
# CONFIG_BOOTBLOCK_CONSOLE is not set
# CONFIG_POSTCAR_CONSOLE is not set" \
  "pxe.rom.lzma=$nb/build/etherboot/pxe.rom.lzma" "etc/pxe-pci-ids=$nb/build/etherboot/pxe-pci-ids.bin"
# no coreboot log in the flash ROM (the debug ROM has it, but no room for the built-in network boot ROM)
if [ -n "${DEBUG:-}" ]; then
  seabios debug "CONFIG_DEBUG_LEVEL=1
CONFIG_DEBUG_SERIAL=y
CONFIG_DEBUG_SERIAL_PORT=0x3f8"   # with the coreboot logs, SeaBIOS level 1 fits
  coreboot newbios-debug debug "# CONFIG_DEFAULT_CONSOLE_LOGLEVEL_4 is not set
CONFIG_DEFAULT_CONSOLE_LOGLEVEL_7=y
# CONFIG_BOOTBLOCK_CONSOLE is not set
# CONFIG_POSTCAR_CONSOLE is not set"   # romstage and ramstage logs; POST codes cover the other stages
fi

git -C "$sb" add -N src
git -C "$sb" diff --full-index > "$nb/patches/seabios.patch"
git -C "$cb" add -N src/mainboard/biostar/mb8433 src/include/cpu/486/newbios_post.h
git -C "$cb" diff --full-index > "$nb/patches/coreboot.patch"
