#!/bin/bash
# Native (no emulator) fault-injection test of newBIOS's device and CMOS handling: the SeaBIOS files of the
# ROM (IDE, floppy, PCI, option ROMs, block layer, RTC/clock, shadow, USB host controllers, hubs, HID, mass
# storage, PnP BIOS and ESCD, and setup.c) compiled unchanged for the host under ASan/UBSan, with port I/O, MMIO, timers and
# real-mode calls linked to device models (test/native/seabios) that inject faults: absent, floating, stuck
# or lying IDE drives, floppy controllers, PCI devices and option ROMs, ISA ROMs and PnP cards, USB
# controllers and devices, corrupt CMOS, flash chips and ESCDs. Each case runs in its own process; it must not crash, hang
# (virtual time budget, CPU loop) or misbehave (checks per area).
# usage: [SEABIOS=tree] devfuzz.sh   (default tree build/seabios; failing cases: test/vm/devfuzz/failures.txt;
#        rerun one with a stack trace: test/vm/devfuzz/devfuzz -v area index; one area: devfuzz -a area)
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
nb=$(dirname "$here")
sb=${SEABIOS:-$nb/build/seabios}
src=$here/native/seabios
out=$here/vm/devfuzz
mkdir -p "$out"

[ -f "$sb/out-rel/autoconf.h" ] && [ -f "$sb/out-rel/rom.o" ] \
  || { echo "FAIL [devfuzz]: no SeaBIOS build in $sb (run build.sh)"; exit 1; }
rom_max=$(nm "$sb/out-rel/rom.o" | awk '$3 == "final_readonly_start" {print "0x" $1}')
[ -n "$rom_max" ] || { echo "FAIL [devfuzz]: final_readonly_start not in $sb/out-rel/rom.o"; exit 1; }

# shift-base: GCC defines shifting into the sign bit, SeaBIOS relies on it
common=(-m32 -O1 -g -ffunction-sections -fdata-sections -fno-omit-frame-pointer -fsanitize=address,undefined
        -fno-sanitize=null,alignment,pointer-overflow,shift-base -fno-sanitize-recover=all -I"$src")
seabios=("${common[@]}" -ffreestanding -fno-builtin -fno-strict-aliasing -fno-delete-null-pointer-checks
         -DMODE16=0 -DMODESEGMENT=0 -DFUZZ_ROM_MAX="$rom_max" -DPNP_FLASH_BASE=0x60000000u -DFLASH_LOW_BASE=0x61000000u -iquote "$sb/out-rel" -iquote "$sb/src"
         -include "$src/seam.h")
objs=() pids=()
compile() {  # object, source, flags...
  objs+=("$out/$1.o")
  gcc "${@:3}" -c "$2" -o "$out/$1.o" & pids+=($!)
}
for f in hw/ata.c hw/floppy.c hw/pci.c hw/pcidevice.c optionroms.c block.c hw/rtc.c fw/shadow.c string.c clock.c \
         hw/blockcmd.c hw/usb.c hw/usb-hub.c hw/usb-hid.c hw/usb-msc.c hw/usb-uhci.c hw/usb-ohci.c hw/usb-ehci.c \
         pnpbios.c; do
  compile "sb_$(basename "$f" .c)" "$sb/src/$f" "${seabios[@]}" -w
done
compile output_seam "$src/output_seam.c" "${seabios[@]}" -w
for f in seams setup_seam roms fuzz_ata fuzz_fdc fuzz_pcirom fuzz_isa fuzz_usb fuzz_usbhc fuzz_cmos fuzz_pnp fuzz_memtest; do
  compile "$f" "$src/$f.c" "${seabios[@]}" -Wall -Wno-unused-parameter -Werror
done
for f in runner models; do
  compile "$f" "$src/$f.c" "${common[@]}" -Wall -Wextra -Wno-unused-parameter -Werror
done
for pid in "${pids[@]}"; do
  wait "$pid" || { echo "FAIL [devfuzz]: compile error"; exit 1; }
done
# string.c also has its own memcpy/memset/strlen...: keep libc's (ASan checks them)
objcopy -L memcpy -L memset -L memmove -L memcmp -L strlen -L strcmp -L strchr "$out/sb_string.o"
# setup.c's L2 asm (CACHE TEST, L2 SRAM TEST pages): the seams' versions (seams.c), no DRAM scan on the host
objcopy --weaken-symbol=l2_fill --weaken-symbol=l2_fill_off --weaken-symbol=l2_line_works --weaken-symbol=l2_caches \
  --weaken-symbol=l2s_probe --weaken-symbol=l2s_pass --weaken-symbol=l2s_tags --weaken-symbol=l2s_dirty \
  "$out/setup_seam.o"
# SeaBIOS's snprintf and znprintf for the SeaBIOS objects only: the harness's own messages use glibc's (%.*s)
for o in "$out"/sb_*.o "$out/setup_seam.o" "$out/output_seam.o"; do
  objcopy --redefine-sym vsnprintf=sb_vsnprintf --redefine-sym snprintf=sb_snprintf --redefine-sym znprintf=sb_znprintf "$o"
done
# output.c's screen, debug and warning output: the seams' versions record them
objcopy -L printf -L panic -L __dprintf -L __warn_timeout -L __warn_noalloc -L __warn_internalerror -L __set_invalid \
  -L __set_unimplemented -L __set_code_invalid -L __set_code_unimplemented "$out/output_seam.o"
gcc -m32 -fsanitize=address,undefined -Wl,--gc-sections "${objs[@]}" -o "$out/devfuzz"

if "$out/devfuzz" -o "$out/failures.txt"; then
  echo "PASS [devfuzz]"
else
  echo "FAIL [devfuzz]: see above and $out/failures.txt"
  exit 1
fi
