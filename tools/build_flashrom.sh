#!/bin/bash
# flashrom 1.7.0 (the HDD image's Buildroot version) with patches/buildroot/flashrom (the UMC UM8886BF chipset
# enable, also applied by that Buildroot), internal programmer only, for i486 Linux with the test kernel's Buildroot
# toolchain -> build/flashrom. libpci (pciutils 3.10.0) is linked in statically, without zlib, udev or DNS: only
# glibc is needed beside it.
set -euo pipefail
nb=$(dirname "$(dirname "$(readlink -f "$0")")")
br=$nb/sources/buildroot_486/output
tc=$br/host/bin/i486-buildroot-linux-gnu-
fr=$nb/build/flashrom-1.7.0
pci=$nb/build/pciutils-3.10.0
log=$nb/build/flashrom.build.log
rm -rf "$fr" "$pci"
tar xJf "$nb/sources/buildroot_486/dl/pciutils/pciutils-3.10.0.tar.xz" -C "$nb/build"
tar xzf "$nb/sources/flashrom-1.7.0.tar.gz" -C "$nb/build"
for p in "$nb"/patches/buildroot/flashrom/*.patch; do
  if [ -e "$p" ]; then patch -s -d "$fr" -p1 < "$p"; fi
done
mkdir -p "$fr/pc"
printf 'Name: libpci\nDescription: libpci\nVersion: 3.10.0\nCflags: -I%s\nLibs: %s/lib/libpci.a\n' "$pci" "$pci" \
  > "$fr/pc/libpci.pc"
cat > "$fr/cross.txt" <<EOF
[binaries]
c = '${tc}gcc'
ar = '${tc}ar'
strip = '${tc}strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'i486'
endian = 'little'
EOF
{
  make -C "$pci" -j"$(nproc)" CC="${tc}gcc" HOST=i486-linux ZLIB=no DNS=no HWDB=no LIBKMOD=no SHARED=no lib/libpci.a
  PKG_CONFIG_LIBDIR=$fr/pc meson setup "$fr/out" "$fr" --cross-file "$fr/cross.txt" -Ddefault_library=static \
    -Dprogrammer=internal -Ddefault_programmer_name=internal -Dclassic_cli=enabled -Dtests=disabled \
    -Dich_descriptors_tool=disabled -Dbash_completion=disabled -Drpmc=disabled -Duse_internal_dmi=true \
    -Dman-pages=disabled -Ddocumentation=disabled -Dwerror=false
  ninja -C "$fr/out" flashrom
} > "$log" 2>&1 || { tail -20 "$log"; echo "FLASHROM BUILD FAILED (log: $log)" >&2; exit 1; }
cp "$fr/out/flashrom" "$nb/build/flashrom"
echo "$nb/build/flashrom"
