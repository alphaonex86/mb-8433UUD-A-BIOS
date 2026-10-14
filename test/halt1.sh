#!/bin/bash
# HALT_ON=1 ROM (Halt On: No Errors, for a board without keyboard or with a dead CMOS battery): with a fresh
# CMOS the POST errors print a WARNING and DOS boots without any key.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
HALT_ON=1 "$nb/build.sh" > "$vm_dir/build.log" 2>&1 || { tail "$vm_dir/build.log"; fail "HALT_ON=1 build"; }
dos_disk "$vm_dir/hdd.img"
vm_start halt1 "$nb/out/newbios-halt1.rom" "$(disk_cfg "$vm_dir/hdd.img")"
vm_wait_text "WARNING: POST errors above, boot continues" 60
vm_expect "CMOS checksum error - Defaults loaded"
vm_wait_text "DOS-TEST-OK" 90
grep -q "Press F1 to continue" "$vm/screen.txt" && fail "the No Errors ROM waits for F1"
vm_stop

# The real board: a dead CMOS battery (RTC register D reads VRT 0 forever, garbage CMOS at power-on) and a bad L2
# data chip. A power-on loads the defaults (L2 on): that POST crashes, the next runs without the L2 once and boots;
# the setting stays on, so a warm reboot crashes once again. Each boot must end in DOS, saying why.
export vm_post_crash_ok=1 BOX86_RTC_DEAD=1 BOX86_L2_FAULT=1:2:10 BOX86_POSTTEXT=BB:$vm_dir/bb.txt
boot_past_crash() {  # [command starting the boot] -> DOS; a POST that hangs instead of resetting is reset once by hand
  local resets
  resets=$(cat "$vm/post.log" 2>/dev/null | grep -c '^RESET' || true)
  rm -f "$vm_dir/bb.txt"
  "$@"
  for _ in 1 2; do
    local end=$((SECONDS + 90))
    until [ -s "$vm_dir/bb.txt" ] || [ $SECONDS -ge $end ]; do sleep 0.5; done
    [ -s "$vm_dir/bb.txt" ] && break
    vm_cmd reset
  done
  [ -s "$vm_dir/bb.txt" ] || fail "no boot past the crashing L2 POST, log: $(tr '\n' ' ' < "$vm/post.log")"
  vm_wait_text "DOS-TEST-OK" 90
  grep -q "CMOS battery failed" "$vm_dir/bb.txt" || fail "no \"CMOS battery failed\" with VRT 0"
  grep -Eq "Cache Memory +: 0K" "$vm_dir/bb.txt" || fail "the boot after the crash has the L2 on"
  [ "$(awk '/^RESET/ { n++ } /^C1$/ { c = n } END { print n - c }' "$vm/post.log")" -le 1 ] \
    || fail "more than one POST after the L2 crash"
  [ $(( $(grep -c '^RESET' "$vm/post.log") - resets )) -le 2 ] || fail "more than one crash in one boot"
}
python3 -c "import random,sys; r=random.Random(7); sys.stdout.buffer.write(bytes(r.randrange(256) for _ in range(256)))" \
  > "$vm_dir/cmos.bin"
vm_nvr=$vm_dir/cmos.bin vm_ram=$(ram_cfg 3 3 0 0 256 2) boot_past_crash vm_start halt1 "$nb/out/newbios-halt1.rom" \
  "$(disk_cfg "$vm_dir/hdd.img")"
boot_past_crash vm_cmd combo 1d 38 53                # Ctrl+Alt+Del from DOS
vm_stop
