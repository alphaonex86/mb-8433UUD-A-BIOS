# Test helpers: run a ROM on the emulated Biostar MB-8433UUD (86Box fork, machine 84xxuuda) headless,
# drive it through the fork's test control FIFO and check the text screen / COM1.
# Source it, then: vm_start <name> <rom> [extra 86box.cfg lines]; vm_wait_text ...; vm_stop
set -euo pipefail
nb=$(dirname "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")")
emu=${EMU:-$nb/build/86box/out/src/86Box}   # EMU: another 86Box build
rom=${ROM:-$nb/out/newbios.rom}   # ROM=out/newbios-debug.rom for full COM1 logs
vm_dir=$nb/test/vm/$(basename "$0" .sh)
[ -x "$emu" ] || "$nb/build_86box.sh" > /dev/null

fail() { echo "FAIL [$vm_name]: $*" >&2; [ -z "${vm:-}" ] || { echo "--- screen:"; cat "$vm/screen.txt" 2>/dev/null; } >&2; exit 1; }

ram_cfg() {  # SIMM types of banks 0-3 (0 empty, see umc_hb4.c), L2 KB, L2 banks -> 86Box config lines
  printf '[UMC HB4(8881F)]\nbank0 = %s\nbank1 = %s\nbank2 = %s\nbank3 = %s\nl2_cache = %s\nl2_banks = %s\n' "$@"
}

vm_start() {  # name, rom, extra config lines; RAM: $vm_ram (default 32 MB in bank 0, 256 KB L2),
              # CPU: $vm_cpu_family/speed/multi (default Am5x86-133)
  vm_name=$1
  vm=$vm_dir
  rm -rf "$vm/nvr" "$vm/post.log"   # fresh CMOS and flash, POST codes of this boot only
  mkdir -p "$vm/roms/machines/84xxuuda"
  cp "$2" "$vm/roms/machines/84xxuuda/uud0520s.bin"   # -R paths are searched before the default ROM set
  cat > "$vm/86box.cfg" <<EOF
[Machine]
machine = 84xxuuda
cpu_family = ${vm_cpu_family:-am5x86}
cpu_speed = ${vm_cpu_speed:-133333333}
cpu_multi = ${vm_cpu_multi:-4}
cpu_use_dynarec = 1
fpu_type = internal
mem_size = 131072
time_sync = disabled

${vm_ram:-$(ram_cfg 5 0 0 0 256 1)}

[Video]
gfxcard = tgui9440_pci

[Input devices]
keyboard_type = keyboard_at
mouse_type = none

[Ports (COM & LPT)]
serial1_device = pipe

[Other peripherals]
postcard_enabled = 1

[Named Pipe (COM) #1]
path = $vm/com1
${3:-}
EOF
  [ -p "$vm/ctl" ] || mkfifo "$vm/ctl"
  [ -p "$vm/com1.out" ] || mkfifo "$vm/com1.out"
  exec {vm_ctl}<>"$vm/ctl"   # read-write open never blocks; commands wait in the pipe for 86Box
  cat 0<>"$vm/com1.out" > "$vm/serial.log" & vm_serial=$!   # read-write: no EOF when 86Box reopens COM1
  BOX86_TESTCTL=$vm/ctl BOX86_POSTLOG=$vm/post.log SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$emu" -N -X both -P "$vm" -R "$vm/roms" > "$vm/86box.log" 2>&1 & vm_pid=$!
  trap vm_kill EXIT
}

vm_cmd() { echo "$*" >&"$vm_ctl"; }

vm_screen() {  # refresh $vm/screen.txt from the emulated text screen
  rm -f "$vm/screen.txt"
  vm_cmd text "$vm/screen.txt"
  for _ in $(seq 50); do [ -f "$vm/screen.txt" ] && return 0; sleep 0.1; done
  kill -0 "$vm_pid" 2>/dev/null || fail "86Box exited (see $vm/86box.log)"
  fail "no screen dump"
}

vm_wait_text() {  # extended regex, timeout seconds
  local end=$((SECONDS + $2))
  while [ $SECONDS -lt $end ]; do
    vm_screen
    grep -Eq "$1" "$vm/screen.txt" && return 0
    sleep 0.2
  done
  fail "timeout waiting for /$1/"
}

vm_expect() { vm_screen; grep -Eq "$1" "$vm/screen.txt" || fail "screen lacks /$1/"; }

vm_key() { for k; do vm_cmd key "$k"; sleep 0.2; done; }   # set 1 scan codes (hex), typed one by one

vm_kill() {  # 86Box ignores SIGTERM
  kill -KILL "$vm_pid" "$vm_serial" 2>/dev/null || true
  wait "$vm_pid" "$vm_serial" 2>/dev/null || true
}

vm_stop() {
  vm_cmd quit
  for _ in $(seq 50); do kill -0 "$vm_pid" 2>/dev/null || break; sleep 0.1; done
  vm_kill
  trap - EXIT
  echo "PASS [$vm_name]"
}

dos_disk() {  # path, [extra file] -> FreeDOS 1.3 disk whose FDAUTO.BAT runs ver, mem, the extra file
              # (copied to C:\) and prints DOS-TEST-OK
  local part=$((63 * 512))   # FAT16 partition at LBA 63, CHS 65/16/63
  mkdir -p "$(dirname "$1")"
  unzip -p "$nb/sources/FD13-LiteUSB.zip" FD13LITE.img > "$1"
  [ -z "${2:-}" ] || mcopy -o -i "$1@@$part" "$2" "::/$(basename "$2")"
  printf '@echo off\r\nSET DOSDIR=\\FREEDOS\r\nSET PATH=%%DOSDIR%%\\BIN\r\nver\r\nmem\r\n%s\r\necho DOS-TEST-OK\r\n' \
    "$(basename "${2:-rem}" .com)" | mcopy -o -i "$1@@$part" - ::/FDAUTO.BAT
}

scsi_dos_disk() {  # path -> minimal FreeDOS disk partitioned for the 64 heads/32 sectors translation of
                   # SCSI BIOSes (31 cylinders), AUTOEXEC.BAT prints SCSI-DOS-OK
  local fd=$1.fd13 mt=$1.mtoolsrc part=$((63 * 512))
  mkdir -p "$(dirname "$1")"
  mkdir -p "$(dirname "$1")"
  unzip -p "$nb/sources/FD13-LiteUSB.zip" FD13LITE.img > "$fd"
  printf 'drive z: file="%s" partition=1\nmtools_skip_check=1\n' "$1" > "$mt"
  rm -f "$1"
  truncate -s $((65520 * 512)) "$1"
  dd if="$fd" of="$1" bs=446 count=1 conv=notrunc status=none                # FreeDOS MBR code
  dd if="$fd" of="$1.boot" bs=512 skip=63 count=1 status=none                 # FreeDOS FAT16 boot sector
  MTOOLSRC=$mt mpartition -I -c -t 31 -h 64 -s 32 z:
  MTOOLSRC=$mt mpartition -a z:
  MTOOLSRC=$mt mformat -h 64 -s 32 -t 31 -H 32 -B "$1.boot" z:
  for f in KERNEL.SYS COMMAND.COM; do
    mcopy -i "$fd@@$part" "::/$f" - | MTOOLSRC=$mt mcopy - "z:/$f"
  done
  printf '@echo off\r\nver\r\necho SCSI-DOS-OK\r\n' | MTOOLSRC=$mt mcopy - z:/AUTOEXEC.BAT
  rm -f "$fd" "$1.boot" "$mt"
}

disk_cfg() {  # path -> 86Box config lines for that image as IDE primary master
  printf '[Storage controllers]\nhdc = internal\n\n[Hard disks]\nhdd_01_parameters = 63, 16, 65, 0, ide\n'
  printf 'hdd_01_fn = %s\nhdd_01_ide_channel = 0:0\n' "$1"
}

nic_cfg() {  # boot ROM file -> 86Box config lines for an RTL8139 with that ROM in its socket, on SLiRP
  mkdir -p "$vm_dir/roms/network/rtl8139c"
  cp "$1" "$vm_dir/roms/network/rtl8139c/rtl8139c.rom"
  printf '[Network]\nnet_01_card = rtl8139c+\nnet_01_net_type = slirp\nnet_01_link = 0\n\n'
  printf '[Realtek RTL8139C+ #1]\nbios = 1\n'
}

vm_setup() {  # DEL at the prompt -> main menu of the setup utility
  vm_key $K_DEL
  vm_wait_text "CMOS SETUP UTILITY" 10
  menu_sel=0
}

vm_end_of_post() {  # [timeout] -> the setup prompt; F1 past the POST error halt (e.g. cleared CMOS)
  vm_wait_text "Press F1 to continue|Press DEL to enter SETUP, ESC for boot" "${1:-60}"
  grep -q "Press F1 to continue" "$vm/screen.txt" || return 0
  vm_key $K_F1
  vm_wait_text "Press DEL to enter SETUP, ESC for boot" 10
}

vm_menu() {  # main menu entry index (0-13, column by column) -> opens it; the menu keeps its cursor
  local i
  for ((i = 0; i < ($1 - menu_sel + 14) % 14; i++)); do vm_key $K_DOWN; done
  menu_sel=$1
  vm_key $K_ENTER
}

vm_save() {  # F10 + Y: save the setup and reboot
  vm_key $K_F10
  vm_wait_text "SAVE to CMOS and EXIT" 5
  vm_key $K_Y
}

vm_pci_byte() {  # bus:dev.fn register -> its value (hex) from a fresh config space dump
  rm -f "$vm/pci.txt"
  vm_cmd pci "$vm/pci.txt"
  for _ in $(seq 50); do [ -f "$vm/pci.txt" ] && break; sleep 0.1; done
  awk -v d="$1" -v r=$((16#$2)) '$1 == d && strtonum("0x" substr($2, 1, 2)) == r - r % 16 {print $(3 + r % 16)}' "$vm/pci.txt"
}

vm_cr0() {  # CPU register CR0 now (hex)
  rm -f "$vm/cr0.txt"
  vm_cmd cr0 "$vm/cr0.txt"
  for _ in $(seq 50); do [ -s "$vm/cr0.txt" ] && break; sleep 0.1; done
  cat "$vm/cr0.txt"
}

vm_sio() {  # UM8663BF register -> value (hex)
  rm -f "$vm/sio.txt"
  vm_cmd out 108 aa
  vm_cmd out 108 "$1"
  vm_cmd in 109 "$vm/sio.txt"
  vm_cmd out 108 55
  for _ in $(seq 50); do [ -s "$vm/sio.txt" ] && break; sleep 0.1; done
  awk '{print $2}' "$vm/sio.txt"
}

expect_bits() {  # value mask expected name
  [ $((16#$1 & $2)) -eq $(($3)) ] || fail "$4: register $1 & $2 != $3"
}

# Scan codes
K_F1=3b K_ESC=01 K_UP=48 K_DOWN=50 K_LEFT=4b K_RIGHT=4d K_DEL=53 K_F9=43 K_F10=44 K_ENTER=1c
K_PGUP=49 K_PGDN=51 K_Y=15
