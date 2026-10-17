#!/bin/bash
# BOARD TEST's video memory test on every video card 86Box offers for this board (BOX86_LIST_VIDEO): through a VESA
# BIOS window the memory size configured (or what that BIOS reports, or what its largest mode's window reaches: noted),
# without one the 256K of the VGA planes; a card that is no VGA untested; no error, no hang. Monochrome cards are skipped (the test reads the text screen at B8000). Up to one 86Box per core.
# usage: vgamem.sh [all] [card...]   (default: each card with its default memory size; all: with every size it
#        offers; cards: those only, by internal name)
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir/list/roms/machines/84xxuuda"
cp "$rom" "$vm_dir/list/roms/machines/84xxuuda/uud0520s.bin"
printf '[Machine]\nmachine = 84xxuuda\n' > "$vm_dir/list/86box.cfg"
BOX86_LIST_VIDEO=$vm_dir/cards.txt SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 60 "$emu" -N -P "$vm_dir/list" \
  -R "$vm_dir/list/roms" > "$vm_dir/list/86box.log" 2>&1
[ -s "$vm_dir/cards.txt" ] || { echo "FAIL [vgamem]: no card list from 86Box"; exit 1; }

one() {  # card, config section, memory value ("": none), its size in KB ("": unknown) -> a "card value|KB|line" result
  local name=$1${3:+-$3}
  (
    vm_dir=$vm_dir/$name
    vm_gfx=$1 vm_start "vgamem $name" "$rom" "${3:+[$2]
memory = $3}"
    vm_end_of_post 180
    vm_setup
    vm_tool 9 0                                     # HARDWARE TESTS: BOARD TEST
    vm_wait_text "Keyboard ctrl" 300
    echo "$1 $3|$4|$(grep -o 'Video memory : .*[^ ]' "$vm/screen.txt")" > "$run/$name"
    vm_stop
  ) > "$vm_dir/$name.log" 2>&1 || echo "$1 $3|$4|no result: $(grep -m1 FAIL "$vm_dir/$name.log")" > "$run/$name"
}

run=$vm_dir/run                                     # this run's results, one file per card and size
rm -rf "$run"
mkdir -p "$run"
# cards that newBIOS does not run (the original BIOS does): CGA-class without a video BIOS (SeaBIOS has no CGA INT 10h),
# and two whose video BIOS leaves the screen blank under SeaBIOS
unsupported=" cga cga_pravetz compaq_cga compaq_cga_2 plantronics quadcolor ogc nga genius sigma400 v6355d wy700 im1024
pgc incolor et2000 oti037c "
blank=" superega-32 "                                 # no screen under the original BIOS either
all= && [ "${1:-}" = all ] && all=1 && shift
only=" $* "
jobs=$(nproc)
while IFS='|' read -r card bus kind section mem; do
  case " $(echo $unsupported) " in *" $card "*) continue;; esac
  [ "$kind" = color ] || continue
  [ $# = 0 ] || [ "${only/ $card /}" != "$only" ] || continue
  sizes=${mem#*,}
  [ -n "$mem" ] && [ -z "$all" ] && sizes=$(echo "$mem" | tr ',' '\n' | grep "^${mem%%,*}=")
  for s in $(echo "${sizes:-none}" | tr ', ' '\n_'); do
    while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do wait -n; done
    if [ "$s" = none ]; then
      one "$card" "$section" "" "" &
    else
      case "$blank" in *" $card-${s%%=*} "*) continue;; esac
      desc=${s#*=}
      kb=$([ "${desc%_MB}" != "$desc" ] && echo $((${desc%_MB} * 1024)) || echo "${desc%_KB}")
      one "$card" "$section" "${s%%=*}" "$kb" &
    fi
  done
done < "$vm_dir/cards.txt"
wait
cat "$run"/* | sort > "$vm_dir/results.txt"
bad=0
while IFS='|' read -r what kb line; do
  case "$line" in
    "Video memory : ${kb}K, the window of VESA mode "*": OK"|"Video memory : ${kb}K, the linear frame buffer at "*": OK") ;;
    "Video memory : 256K, the VGA planes (no VESA BIOS): OK") ;;
    "Video memory : not a VGA (no mode 12h), not tested") ;;
    "Video memory : "*"K of ${kb}K, all the window of VESA mode "*" reaches: OK") echo "note $what: ${line#*: }";;
    "Video memory : "*"K, the window of VESA mode "*": OK"|"Video memory : "*"K of "*"K, all the window of VESA mode "*\
    |"Video memory : "*"K, the linear frame buffer at "*": OK")
      [[ $line =~ :\ ([0-9]+)K(\ of\ ([0-9]+)K)? ]]  # the card's VESA BIOS reports another size than configured
      size=${BASH_REMATCH[3]:-${BASH_REMATCH[1]}}
      [ -z "$kb" ] || echo "note $what: its VESA BIOS reports ${size}K, ${kb}K configured";;
    *) echo "FAIL $what: $line"; bad=1;;
  esac
done < "$vm_dir/results.txt"
echo "$(wc -l < "$vm_dir/results.txt") card/size runs: $(grep -c 'linear frame' "$vm_dir/results.txt") VESA linear," \
  "$(grep -c 'VESA mode' "$vm_dir/results.txt") VESA window," \
  "$(grep -c 'VGA planes' "$vm_dir/results.txt") VGA planes only, $(grep -c 'not a VGA' "$vm_dir/results.txt") not VGA"
[ $bad = 0 ] && echo "PASS [vgamem]" || { echo "FAIL [vgamem] (results: $vm_dir/results.txt)"; exit 1; }
