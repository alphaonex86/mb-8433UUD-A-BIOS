#!/bin/bash
# Build the Linux 6.18 i486 test kernel with an embedded busybox initramfs -> newBIOS/build/bzImage.
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
nb=$(dirname "$(dirname "$here")")
off=$nb/sources
br=$off/buildroot_486/output
tarball=$(ls "$off"/linux-6.18.*.tar.xz | sort -V | tail -1)
src=$nb/build/$(basename "$tarball" .tar.xz)
[ -d "$src" ] || tar xJf "$tarball" -C "$nb/build"

cat > "$nb/build/initramfs.list" <<LIST
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /bin 0755 0 0
dir /lib 0755 0 0
file /init $here/init 0755 0 0
file /bin/busybox $br/target/bin/busybox 0755 0 0
slink /bin/sh busybox 0777 0 0
file /lib/ld-linux.so.2 $br/target/lib/ld-linux.so.2 0755 0 0
file /lib/libc.so.6 $br/target/lib/libc.so.6 0755 0 0
file /lib/libresolv.so.2 $br/target/lib/libresolv.so.2 0755 0 0
file /lib/libtirpc.so.3 $(readlink -f "$br/target/usr/lib/libtirpc.so.3") 0755 0 0
LIST

kmake() { make -C "$src" -s ARCH=i386 CROSS_COMPILE="$br/host/bin/i486-buildroot-linux-gnu-" "$@"; }
{ cat "$here/kernel.config"; echo "CONFIG_INITRAMFS_SOURCE=\"$nb/build/initramfs.list\""; } > "$nb/build/kernel.fragment"
KCONFIG_ALLCONFIG="$nb/build/kernel.fragment" kmake allnoconfig
for opt in M486 EARLY_PRINTK 8139TOO ATA_GENERIC APM VGA_CONSOLE SERIAL_8250_CONSOLE; do
  grep -q "^CONFIG_$opt=y" "$src/.config" || { echo "kernel config lacks CONFIG_$opt" >&2; exit 1; }
done
kmake -j"$(nproc)" bzImage > "$nb/build/linux.build.log" 2>&1 \
  || { tail -30 "$nb/build/linux.build.log"; echo "LINUX BUILD FAILED" >&2; exit 1; }
cp "$src/arch/x86/boot/bzImage" "$nb/build/bzImage"
ls -la "$nb/build/bzImage"
