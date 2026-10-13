#!/bin/bash
# tools/flashbios from Linux on the board (86Box W49F002A, 2 Mbit): booted by the flash's iPXE, Linux fetches the
# tool and images by TFTP and rewrites the chip: a file that is not an image is refused, then the lower half erased
# (b.rom), the image restored (a.rom), one free byte of the boot block changed (b2.rom). After a reset the new
# BIOS runs and the chip holds b2.rom.
. "$(dirname "$0")/lib.sh"
[ -f "$nb/build/bzImage" ] || "$nb/test/linux/build.sh"
a=${rom%.rom}-2mbit.rom
t=$vm_dir/tftp
mkdir -p "$t"
"$nb/sources/buildroot_486/output/host/bin/i486-buildroot-linux-gnu-gcc" -O2 -static -Wall -Werror -o "$t/flashbios" \
  "$nb/tools/flashbios.c" || fail "flashbios does not build"
cp "$a" "$t/a.rom"
head -c 1000 "$a" > "$t/bad.rom"
{ head -c 131072 /dev/zero | tr '\0' '\377'; tail -c 131072 "$a"; } > "$t/b.rom"
cp "$a" "$t/b2.rom"
ff=$(python3 -c "import sys; d=open(sys.argv[1],'rb').read(); print(next(i for i in range(0x3c000, 0x3fff0) if d[i:i+64] == b'\xff'*64) + 32)" "$a")
printf '\x00' | dd of="$t/b2.rom" bs=1 seek="$ff" conv=notrunc status=none   # a free byte of the boot block
cp "$nb/build/bzImage" "$t/bzImage"
printf '#!ipxe\nchain tftp://${next-server}/bzImage ip=dhcp console=ttyS0 flashtest\n' > "$t/boot.ipxe"
export BOX86_FLASH=w49f002a BOX86_SLIRP_TFTP=$t BOX86_SLIRP_BOOTFILE=boot.ipxe
vm_start flashlinux "$a" "$(printf '[Network]\nnet_01_card = rtl8169\nnet_01_net_type = slirp\nnet_01_link = 0\n
[Realtek RTL8169S #1]\nbios = 0\n')"
vm_end_of_post 60
vm_setup
vm_menu 1                                                  # BIOS FEATURES SETUP
vm_wait_text "1st Boot Device" 5
vm_key $K_DOWN $K_DOWN $K_PGDN $K_PGDN $K_PGDN             # 1st boot device: Floppy -> Network
vm_save
com_done() { grep -aq "$1" "$vm/serial.log"; }
end=$((SECONDS + 300))
until com_done "FLASHTEST-RC="; do [ $SECONDS -lt $end ] || { vm_screen; fail "Linux did not run the flash test"; }; sleep 1; done
grep -aq "FLASHTEST-BAD-RC=1" "$vm/serial.log" || fail "a file that is not an image was not refused"
grep -aq "FLASHTEST-RC=0" "$vm/serial.log" || { grep -a "flashbios\|block\|chip" "$vm/serial.log"; fail "flashbios failed"; }
grep -a "^chip \|rewriting\|^done" "$vm/serial.log" | tr -d '\r' | sed 's/^/flashbios: /'
vm_cmd reset
vm_wait_text "Press DEL to enter SETUP" 60
vm_cmd quit                                                # 86Box saves the chip when it powers off
for _ in $(seq 50); do kill -0 "$vm_pid" 2>/dev/null || break; sleep 0.1; done
cmp -s "$vm/nvr/84xxuuda.bin" "$t/b2.rom" || fail "the chip does not hold b2.rom"
vm_stop
