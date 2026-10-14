#!/bin/bash
# Benchmark pages of HARDWARE INFORMATION (VGA mode 12h, SpeedSys style), checked on 86Box screenshots by pixel
# colors and by their text (test/bench/gridocr.py: 8x8 font cells matched exactly): the memory chart has its read
# (yellow), write (light red) and move (light green) curves and values; the CPU page its clock, ALU bar and
# MIPS/MFLOPS; the disk benchmark of the IDE disk its seek times, speeds and linear read curve; the disk speed page
# a bar per disk (IDE and USB). After each page the setup is back in text mode.
. "$(dirname "$0")/lib.sh"
dos_disk "$vm_dir/hdd.img"
dos_disk "$vm_dir/usb.img"
export BOX86_USB_DISK=$vm_dir/usb.img
vm_start bench "$rom" "$(disk_cfg "$vm_dir/hdd.img")"
vm_end_of_post
colors() { "$nb/test/bench/pngcolors.py" "$@"; }   # file x0 y0 x1 y1 colors... -> counts
page() {  # HARDWARE INFORMATION index, seconds to wait -> screenshot of it
  vm_setup
  vm_info "$1"
  sleep "$2"
  vm_shot
}
back() {  # ESC to the HARDWARE INFORMATION menu (text mode again), then the main menu
  vm_key $K_ESC
  vm_wait_text "MEMORY SPEED" 30
  vm_key $K_ESC $K_ESC
  vm_wait_text "Quit Without Saving" 5
  vm_key $K_Y
  vm_wait_text "DOS-TEST-OK" 90
  vm_cmd reset
  vm_end_of_post
}
png=$(page 6 25)
"$nb/test/bench/gridocr.py" "$png" > "$vm/ocr.txt"
vm_ocr_expect "MEMORY SPEED"
vm_ocr_expect "Reading: [0-9]+ MB/s at 4K, [0-9]+ MB/s at 2M"
vm_ocr_expect "Moving: [0-9]+ MB/s"
read -r yellow red green < <(colors "$png" 57 41 615 391 ffff55 ff5555 55ff55)
echo "memory chart pixels: read $yellow, write $red, move $green"
# 86Box reads and writes at the same speed: the curves may overlap, the last drawn (write) on top
[ $((yellow + red)) -gt 200 ] && [ "$red" -gt 100 ] && [ "$green" -gt 200 ] || fail "memory chart curves missing"
back
png=$(page 7 10)
"$nb/test/bench/gridocr.py" "$png" > "$vm/ocr.txt"
vm_ocr_expect "133 MHz \(measured\)"
vm_ocr_expect "[0-9]+ MIPS"
vm_ocr_expect "[0-9]+\.[0-9] MFLOPS"
vm_ocr_expect "5x86-160"
read -r bar < <(colors "$png" 40 144 600 159 5555ff)
echo "CPU ALU bar pixels: $bar"
[ "$bar" -gt 500 ] || fail "CPU bar missing"
back
vm_setup
vm_info 8
sleep 3
vm_key $K_ENTER                                     # test the first disk
sleep 60
png=$(vm_shot)
"$nb/test/bench/gridocr.py" "$png" > "$vm/ocr.txt"
vm_ocr_expect "Maximal seek time +: [0-9]+\.[0-9]+ ms"
vm_ocr_expect "Linear read speed +: [0-9]+ KB/s"
read -r curve < <(colors "$png" 57 153 615 391 ffff55)
echo "disk linear read curve pixels: $curve"
[ "$curve" -gt 100 ] || fail "disk read curve missing"
back
png=$(page 9 60)
"$nb/test/bench/gridocr.py" "$png" > "$vm/ocr.txt"
vm_ocr_expect "[0-9]+ KB/s linear read"
vm_ocr_expect "PIO 4"
read -r bars < <(colors "$png" 40 72 600 87 5555ff)
read -r bars2 < <(colors "$png" 40 200 600 215 5555ff)
echo "disk speed bar pixels: $bars, $bars2"
[ "$bars" -gt 50 ] && [ "$bars2" -gt 50 ] || fail "a disk bar is missing (IDE and USB)"
vm_stop
