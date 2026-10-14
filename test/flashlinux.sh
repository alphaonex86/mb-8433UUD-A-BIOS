#!/bin/bash
# BIOS update from Linux on the board (86Box W49F002A, A17 high as in its 1 Mbit socket: only the upper half reached),
# booted by Etherboot in the card's socket ROM, then iPXE, the tools and images by TFTP:
#  - flashrom (UMC patch, tools/build_flashrom.sh) refuses a plain 256 KiB write: on the board one erased the BIOS
#    through the alias (max decode 128 KiB);
#  - tools/bios-update.sh refuses a file of the wrong size, then the chip read back as the image, and rewrites it:
#    the 96 KiB main block changed (b.rom), the image restored (a.rom), a free byte of the boot block changed (b2.rom).
# After a reset the new BIOS runs, the chip's upper half holds b2.rom and its unreachable lower half is untouched.
. "$(dirname "$0")/lib.sh"
test_linux
"$nb/tools/build_flashrom.sh" > /dev/null || fail "flashrom does not build"
net_boot_dir $'#!ipxe\nchain tftp://${next-server}/bzImage ip=dhcp console=ttyS0 flashtest\n'
t=$vm_dir/tftp
cp "$nb/build/flashrom" "$t/flashrom"
cp "$nb/tools/bios-update.sh" "$t/bios-update"
cp "$nb/build/bzImage" "$t/bzImage"
cp "$rom" "$t/a.rom"
cp "${rom%.rom}-2mbit.rom" "$t/a2.rom"
head -c 1000 "$rom" > "$t/bad.rom"
python3 - "$rom" "$t" <<'PY'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
b = bytearray(d)
b[0x1000:0x1400] = bytes(x ^ 0xff for x in b[0x1000:0x1400])   # in the 96 KiB main block (chip 0x21000)
open(sys.argv[2] + '/b.rom', 'wb').write(b)
free = next(i for i in range(0x1c000, 0x1fff0) if d[i:i + 64] == b'\xff' * 64) + 32
d[free] = 0                                                       # a free byte of the 16 KiB boot block
open(sys.argv[2] + '/b2.rom', 'wb').write(d)
PY
export BOX86_FLASH=w49f002a
vm_start flashlinux "${rom%.rom}-2mbit.rom" "$(nic_cfg "$etherboot")"
vm_end_of_post 60
vm_setup
vm_menu 1                                                  # BIOS FEATURES SETUP
vm_wait_text "1st Boot Device" 5
vm_net_first
vm_save
com_done() { grep -aq "$1" "$vm/serial.log"; }
end=$((SECONDS + 300))
until com_done "FLASHTEST-RC="; do [ $SECONDS -lt $end ] || { vm_screen; fail "Linux did not run the flash test"; }; sleep 1; done
com_done "FLASHTEST-FULL-RC=0" && fail "flashrom wrote a 256 KiB image on the 128 KiB-decoded board"
grep -aq "This flash chip is too big for this programmer" "$vm/serial.log" || fail "flashrom did not refuse the 2 Mbit write"
grep -aq "FLASHTEST-BAD-RC=1" "$vm/serial.log" || fail "a file of the wrong size was not refused"
grep -aq "FLASHTEST-RC=0" "$vm/serial.log" || { grep -a "flashrom\|chip\|rror\|FAIL" "$vm/serial.log"; fail "bios-update failed"; }
grep -a "^Found \|UMC\|VERIFIED" "$vm/serial.log" | tr -d '\r' | sort | uniq -c | sed 's/^/flashrom: /'
vm_reset
vm_wait_text "Press DEL to enter SETUP" 60
vm_cmd quit                                                # 86Box saves the chip when it powers off
for _ in $(seq 50); do kill -0 "$vm_pid" 2>/dev/null || break; sleep 0.1; done
cmp -s <(tail -c 131072 "$vm/nvr/84xxuuda.bin") "$t/b2.rom" || fail "the chip's upper half does not hold b2.rom"
cmp -s <(head -c 131072 "$vm/nvr/84xxuuda.bin") "$t/a.rom" || fail "the chip's unreachable lower half changed"
vm_stop
