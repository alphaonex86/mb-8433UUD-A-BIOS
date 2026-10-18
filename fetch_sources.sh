#!/bin/bash
# Download what mb-8433UUD is built and tested from, at the tested versions, into sources/ (idempotent:
# existing entries, e.g. symlinks to local copies, are kept). Tarballs are checked against their SHA-256.
set -euo pipefail
src=$(dirname "$(readlink -f "$0")")/sources
mkdir -p "$src"

git_at() {  # url, commit, directory
  [ -e "$3" ] && return 0
  rm -rf "$3.part"
  { git init -q "$3.part" && git -C "$3.part" fetch -q --depth 1 "$1" "$2" \
      && git -C "$3.part" checkout -q FETCH_HEAD && mv "$3.part" "$3" && echo "got $(basename "$3")"; } \
    || { rm -rf "$3.part"; echo "FAILED: $1 $2" >&2; exit 1; }
}
file_sha() {  # url, sha256, file
  [ -e "$3" ] && return 0
  { curl -sfL --retry 3 -o "$3.part" "$1" && echo "$2  $3.part" | sha256sum -c --quiet \
      && mv "$3.part" "$3" && echo "got $(basename "$3")"; } \
    || { rm -f "$3.part"; echo "FAILED: $1" >&2; exit 1; }
}

# Firmware: the 486 coreboot and SeaBIOS trees of their author (github.com/pc2005cz), patched by build.sh.
git_at https://github.com/pc2005cz/coreboot_486.git ff1be301ad1bbb35d1644243dd63d4b2c8fd8a8d "$src/coreboot_486"
git_at https://github.com/pc2005cz/seabios_486.git 7d5405a094acca0cb80bb7c1fb94425ce181031b "$src/seabios_486"
[ "${1:-}" = firmware ] && exit 0

# Network boot tests: iPXE as the PXE program Etherboot loads (tools/build_ipxe.sh).
git_at https://github.com/ipxe/ipxe.git 6ba671acd922ee046b257c5119b8a0f64d275473 "$src/ipxe"

# Tests: 86Box 6.0 (patched into the test emulator), FreeDOS 1.3, Linux 6.18 (last i486 LTS).
file_sha https://github.com/86Box/86Box/archive/refs/tags/v6.0.tar.gz \
  9c3c52b8587dac6adbfeab566d9543d37fec554ef00c147e39deebc418fab07c "$src/86Box-6.0.tar.gz"
file_sha https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-LiteUSB.zip \
  64a934585087ccd91a18c55e20ee01f5f6762be712eeaa5f456be543778f9f7e "$src/FD13-LiteUSB.zip"
file_sha https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.55.tar.xz \
  f410638061a165c12f42ab871d2f3fcd525515359b5faeee80969cff84524df9 "$src/linux-6.18.55.tar.xz"
# BIOS update from Linux: flashrom (the HDD image's Buildroot version), patched by patches/buildroot/flashrom.
file_sha https://github.com/flashrom/flashrom/archive/v1.7.0/flashrom-1.7.0.tar.gz \
  c5a8892158bd749aadfee6e114eec268130cdd8c2027fd1ddf44273755b07b59 "$src/flashrom-1.7.0.tar.gz"
