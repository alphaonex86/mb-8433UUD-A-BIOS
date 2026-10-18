#!/bin/bash
# Dead CMOS battery: the CMOS holds garbage at power-on (RTC registers, shutdown code 0Fh, the mb-8433UUD
# block, the boot-failure counter). POST must not hang or take the CPU-only reset path: it reaches the
# setup prompt (Halt On: All Errors), F1 continues and DOS boots, for several random CMOS images.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
dos_disk "$vm_dir/hdd.img"
for seed in 1 2 3; do
  python3 -c "import random,sys; r=random.Random($seed); b=bytearray(r.randrange(256) for _ in range(256)); b[0x0f]=(0x05,0x0a,0xff)[$seed-1]; sys.stdout.buffer.write(b)" > "$vm_dir/cmos.bin"
  vm_nvr=$vm_dir/cmos.bin vm_start deadcmos "$rom" "$(disk_cfg "$vm_dir/hdd.img")"
  vm_wait_text "Press F1 to continue" 90
  vm_key $K_F1
  vm_wait_text "DOS-TEST-OK" 90
  vm_stop
done
