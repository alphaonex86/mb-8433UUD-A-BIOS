#!/bin/bash
# Build mb-8433UUD for the Biostar MB-8433UUD-A: coreboot_486 (UMC chipset init) + SeaBIOS (BIOS services, option ROMs,
# CMOS setup). Network boot is Etherboot in the RTL8139's socket ROM, not in this flash.
# Output: out/newbios.rom (1 Mbit flash, also used by the 86Box tests) and out/newbios-2mbit.rom (a 2 Mbit flash such
# as a W49F002A: the board's 1 Mbit socket keeps A17 high, only the upper half is reached; the BIOS is in both halves).
# Sources: fetch_sources.sh (pinned coreboot_486 and seabios_486 trees, in sources/).
# usage: [DEBUG=1] [HALT_ON=n] build.sh    DEBUG=1 also builds out/newbios-debug.rom (SeaBIOS's log on COM1);
#   HALT_ON=1 builds out/newbios-halt1.rom whose Halt On default is No Errors (no keyboard, dead CMOS battery).
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
for dst in "$sb/src/post_serial.h" "$cb/src/mainboard/biostar/mb8433/post_serial.h"; do   # POST codes on COM1
  cmp -s "$nb/post_serial.h" "$dst" || cp "$nb/post_serial.h" "$dst"
done

seabios() {  # name, extra SeaBIOS config -> $sb/out-<name>/bios.bcj.elf (x86 BCJ filtered)
  local cfg=$sb/.config-$1 log=$out/seabios-$1.build.log
  { cat "$cb/src/mainboard/biostar/mb8433/config_seabios" "$nb/seabios.config"; echo "$2"; } > "$cfg"
  local mk=(make -C "$sb" -s CROSS_PREFIX="$xgcc/i386-elf-" OUT="out-$1/" KCONFIG_CONFIG="$cfg")
  "${mk[@]}" olddefconfig > "$log" 2>&1
  "${mk[@]}" -j"$(nproc)" >> "$log" 2>&1 \
    || { tail -30 "$log"; echo "SEABIOS BUILD FAILED (log: $log)" >&2; exit 1; }
  "$nb/tools/bcj_x86.py" "$sb/out-$1/bios.bin.elf" "$sb/out-$1/bios.bcj.elf"   # undone by coreboot's loader
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
CONFIG_PAYLOAD_FILE="$sb/out-$2/bios.bcj.elf"
CONFIG_COMPRESSED_PAYLOAD_LZMA=y
CONFIG_PAYLOAD_BCJ_X86=y
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
  local rom=$out/$tag.rom cbfs=("$cb/$obj/cbfstool" "$out/$tag.rom") f
  cp "$cb/$obj/coreboot.rom" "$rom"
  # ESCD in the 4 KiB parameter block at flash 0x1c000 of the Intel 28F001BX-T: a raw file whose data fills
  # it (ESCD_OFF in SeaBIOS pnpbios.c). The payload and the first extra file fill the flash below it, postcar
  # the 8 KiB above it, the other files what is left. cmos_layout.bin goes: nothing reads it (no option table).
  head -c 4096 /dev/zero | tr '\0' '\377' > "$cb/$obj/escd.bin"
  for f in fallback/postcar fallback/payload cmos_layout.bin; do "${cbfs[@]}" remove -n "$f"; done
  "${cbfs[@]}" add -t raw -n escd -f "$cb/$obj/escd.bin" -b 0x1c000 \
    && "${cbfs[@]}" add-payload -n fallback/payload -f "$sb/out-$2/bios.bcj.elf" -c lzma > /dev/null \
    || { echo "BUILD FAILED: $tag, no room for the ESCD and the payload" >&2; exit 1; }
  for f in "${@:4:1}" postcar "${@:5}"; do
    if [ "$f" = postcar ]; then
      "${cbfs[@]}" add-stage -n fallback/postcar -f "$cb/$obj/cbfs/fallback/postcar.elf" -c none > /dev/null
    else
      "${cbfs[@]}" add -t raw -n "${f%%=*}" -f "${f#*=}" > /dev/null
    fi || { echo "BUILD FAILED: $tag, no room for ${f%%=*}" >&2; exit 1; }
  done
  "${cbfs[@]}" print > "$out/$tag.cbfs.txt"
  [ "$(awk '$1 == "escd" {print $2}' "$out/$tag.cbfs.txt")" = 0x1bfe0 ] \
    || { echo "BUILD FAILED: $tag, the ESCD data is not at flash 0x1c000" >&2; exit 1; }   # 32-byte header
  echo "$out/$tag.rom: $(awk '/empty/ {s += $4} END {print s}' "$out/$tag.cbfs.txt") bytes free"
  cat "$rom" "$rom" > "$out/$tag-2mbit.rom"
}

halt=${HALT_ON:-0} rom=newbios
[ "$halt" = 0 ] || rom=newbios-halt$halt
# no coreboot log: its console alone takes 7 KB (13 KB with its messages), more than the flash has left; its POST codes
# on COM1 time each step, and the debug ROM's SeaBIOS logs the chipset values coreboot set
nolog="# CONFIG_CONSOLE_SERIAL is not set
# CONFIG_BOOTBLOCK_CONSOLE is not set
# CONFIG_POSTCAR_CONSOLE is not set"
seabios rel "CONFIG_NEWBIOS_HALT_ON=$halt"
coreboot "$rom" rel "$nolog"
if [ -n "${DEBUG:-}" ]; then   # SeaBIOS's level 1 log: its key places (device setup, option ROMs, boot, warnings)
  seabios debug "CONFIG_DEBUG_LEVEL=1
CONFIG_DEBUG_SERIAL=y
CONFIG_DEBUG_SERIAL_PORT=0x3f8"
  coreboot newbios-debug debug "$nolog"
fi

git -C "$sb" add -N src
git -C "$sb" diff --full-index > "$nb/patches/seabios.patch"
git -C "$cb" add -N src/mainboard/biostar/mb8433 src/include/cpu/486/newbios_post.h
git -C "$cb" diff --full-index > "$nb/patches/coreboot.patch"
