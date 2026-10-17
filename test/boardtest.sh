#!/bin/bash
# Setup BOARD TEST page: on the emulated board (IDE disk and CD-ROM, floppy, WD8013 RAM enabled at D0000, an 8 KiB
# ISA ROM at D8000, TGUI9440 with 2 MB) every test passes, the I/O scan finds the WD8013, and the board still boots DOS;
# with the fork's faults (IRQ 4 and 8 lines broken, IDE data line DD3 stuck at 0, no refresh request, a ROM with a bad
# checksum, a video memory bit stuck at 0) each one is named. MEMORY TEST's bit fade test (F) finds the fork's stuck
# bit (bit 3 of 2AA400h reads 0) only with the all-ones pattern; the RTC is advanced past its waits. HDD SURFACE SCAN
# lists the fork's bad sectors and stops on ESC. usage: boardtest.sh (all) | boardtest.sh good|faults|fade|scan
. "$(dirname "$0")/lib.sh"
if [ $# -eq 0 ]; then
  mkdir -p "$vm_dir"
  vm_name=boardtest pids=()
  for t in good faults fade scan; do "$0" $t > "$vm_dir/$t.log" 2>&1 & pids+=($!); done
  for i in 0 1 2 3; do
    t=$(echo good faults fade scan | cut -d' ' -f$((i + 1)))
    wait "${pids[$i]}" || { cat "$vm_dir/$t.log"; fail "$t"; }
  done
  echo "PASS [boardtest]"
  exit 0
fi
vm_dir=$vm_dir/$1
mkdir -p "$vm_dir"
K_R=13 K_F=21 K_I=17

isa_rom() {  # file, checksum good (1) or not (0): an 8 KiB option ROM whose init returns at once
  python3 -c 'import sys; r = bytearray(8192); r[0:4] = b"\x55\xaa\x10\xcb"; r[-1] = (-sum(r) - 1 + int(sys.argv[1])) & 0xff
sys.stdout.buffer.write(r)' "$2" > "$1"
}

board() {  # extra config lines: the BOARD TEST page on screen, after the POST of a fresh CMOS
  vm_start "boardtest $1" "$rom" "$(disk_cfg "$vm_dir/hdd.img")
$2"
  vm_end_of_post
  vm_setup
  vm_tool 9 0                                       # HARDWARE TESTS: BOARD TEST
  vm_wait_text "Keyboard ctrl" 60
}

if [ "$1" = fade ]; then
  vm_ram=$(ram_cfg 1 0 0 0 128 1)$'\nstuck_addr = 2794496\nstuck_bit = 3'   # 2AA400h
  vm_start "boardtest fade" "$rom"
  vm_end_of_post 60
  vm_setup
  vm_tool 9 1                                       # MEMORY TEST
  vm_wait_text "F : Bit fade test" 5
  vm_key $K_F
  vm_wait_text "Test 9 \[Bit fade test, 2 patterns, 5 min each\]" 30
  vm_wait_text "Pattern  : 00000000   Waiting : [45]:[0-9]{2}, the RAM left alone" 60
  vm_cmd rtcadd 300                                 # all zeros: the stuck bit holds them
  vm_wait_text "Pattern  : ffffffff   Waiting : [45]:[0-9]{2}" 60
  vm_expect "Errors   : 0 "
  vm_cmd rtcadd 300
  vm_wait_text " 9   002aa400  ffffffff  fffffff7  00000008  1" 60
  vm_wait_text "Pass     : 1" 60
  vm_expect "Errors   : 1 "                          # that dword only
  vm_tool_end
  vm_stop
  exit 0
fi

if [ "$1" = scan ]; then
  dos_disk "$vm_dir/hdd.img"
  BOX86_IDE_BAD_LBA=1000,65519 vm_start "boardtest scan" "$rom" "$(disk_cfg "$vm_dir/hdd.img")"
  vm_end_of_post
  vm_setup
  vm_tool 9 4                                       # HDD SURFACE SCAN
  vm_wait_text "Primary Master +: 86B_HD[0-9]*, 31MB" 10
  vm_key $K_ENTER
  vm_wait_text "Done: 2 bad sectors" 120
  vm_expect "Sector   : 65520 of 65520"
  vm_expect "sector 1000: uncorrectable"
  vm_expect "sector 65519: uncorrectable"               # the last one
  vm_key $K_ENTER $K_ENTER $K_ESC                       # again, stopped at once
  vm_wait_text "Stopped \(ESC\) at sector [0-9]+" 30
  vm_key $K_ENTER
  vm_tool_end
  vm_key $K_ESC $K_Y                                    # the disk still boots
  vm_wait_text "DOS-TEST-OK" 90
  vm_stop
  exit 0
fi

dos_disk "$vm_dir/hdd.img"
nasm -f bin -o "$vm_dir/floppy.img" "$nb/test/floppy/boot.asm"
isa_rom "$vm_dir/isarom.bin" "$([ "$1" = good ] && echo 1 || echo 0)"
vm_other="isarom0_type = isarom"
cfg="[Floppy and CD-ROM drives]
fdd_01_type = 35_2hd
fdd_01_fn = $vm_dir/floppy.img
cdrom_01_parameters = 0, atapi
cdrom_01_ide_channel = 1:0

[Generic ISA ROM Board #1]
bios_fn = $vm_dir/isarom.bin
bios_addr = d8000
bios_size = 8192

[Network]
net_01_card = wd8013ebt
net_01_net_type = slirp
net_01_link = 0"

if [ "$1" = faults ]; then
  BOX86_IRQ_FAULT=0110 BOX86_IDE_DATA_FAULT=0008 BOX86_REFRESH_DEAD=1 BOX86_VRAM_FAULT=10010:08 board faults "$cfg"
  vm_expect "IRQ lines    : 0 1 3 6 13 14 15 OK, PIC masks OK"
  vm_expect "IRQ 2 \(Cascade \(IRQ 9\)\): never reaches the PIC"
  vm_expect "IRQ 4 \(Serial port\): never reaches the PIC"
  vm_expect "IRQ 8 \(Real-time clock\): never reaches the PIC"
  vm_expect "PIT timers   : ch1 refresh request every 0\.0 us, ch2 10\.0 ms \(10\.0\): BAD"
  vm_expect "no refresh request: the DRAM is not refreshed \(PIT channel 1\)"
  vm_expect "IDE Pri\. Master: data lines wrong, 16-bit: DD3; 32-bit: DD3"
  vm_expect "ISA ROM      : d8000-d9fff \(8K\): checksum BAD"
  vm_expect "Video memory : bits 00000008 wrong at 64K \+ 10"
  vm_stop
  exit 0
fi

board good "$cfg"
vm_expect "IRQ lines    : 0 1 2 3 4 6 8 13 14 15 OK, PIC masks OK"
vm_expect "not tested \(no device here drives them\): 5 7 9 10 11 12 *$"
vm_expect "PIT timers   : ch1 refresh request every 15\.[0-9] us, ch2 10\.0 ms \(10\.0\): OK"
vm_expect "RTC crystal  : [+-]?[0-9] ppm against the 14\.318 MHz clock, 0\.[0-9] s/day [a-z]+: OK"   # under 10 ppm
vm_expect "DMA 8237     : address, count and page registers of channels 0-3, 5-7: OK"
vm_expect "Floppy DMA   : F to read drive A"
vm_expect "IDE Pri\. Master: cable data lines OK, 16-bit and 32-bit I/O"
vm_expect "IDE Sec\. Master: CD-ROM, not tested \(no buffer commands\)"
vm_expect "PCI bus      : no parity or abort error on [0-9]+ devices"
vm_expect "Video memory : 2048K, the (linear frame buffer at [0-9a-f]+|window of VESA mode [0-9a-f]+h): OK"
vm_expect "ISA I/O      : UART 3f8 OK, UART 2e8 OK, CMOS RAM 112 bytes OK"
vm_expect "ISA ROM      : d8000-d9fff \(8K\): checksum OK"
vm_expect "Keyboard ctrl: line test now: OK, POST tests: OK"
grep -q "ISA RAM" "$vm/screen.txt" && fail "ISA RAM found before the WD8013 enabled it"
vm_cmd out 280 68                                   # WD8013 MSR: its 16 KiB RAM on at D0000
vm_key $K_F                                         # again, with the floppy read
vm_wait_text "Floppy DMA   : 2 reads of A: sector 1 by DMA 2: equal, boot signature 55AA" 60
vm_wait_text "Keyboard ctrl" 30
vm_expect "ISA RAM      : d0000-d3fff \(16K\) written and read: OK"
vm_expect "IRQ lines    : 0 1 2 3 4 6 8 13 14 15 OK"      # a second run: every device left as it was
vm_key $K_I                                         # with the I/O scan: the WD8013's ports
vm_wait_text "ISA I/O scan : 280-299 29c-29f *$" 60
vm_tool_end
vm_key $K_ESC $K_Y                                  # quit without saving: the boot goes on, from the hard disk
vm_wait_text "DOS-TEST-OK" 90
vm_reset                                            # the CMOS RAM test restored every byte
vm_wait_text "Press DEL to enter SETUP" 60
grep -q "CMOS checksum error" "$vm/screen.txt" && fail "CMOS checksum error after the board test"
vm_stop
