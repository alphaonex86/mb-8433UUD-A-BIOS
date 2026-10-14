#!/bin/sh
# bios-update IMAGE: rewrite the MB-8433UUD-A's BIOS from Linux with flashrom (patches/buildroot/flashrom), as root.
# Its 1 Mbit socket leaves A17 of a 2 Mbit chip high: only the upper 128 KiB is reachable, the lower half reads as an
# alias of it. So only that region is erased, written and verified (the image holds the BIOS in both halves, what the
# chip reads). flashrom alone refuses (max decode 128 KiB) rather than erase the BIOS through the alias.
# IMAGE: a 128 KiB newBIOS ROM, or a 256 KiB one (its upper half is taken).
set -eu
[ $# = 1 ] && [ -f "$1" ] || { echo "usage: bios-update IMAGE (e.g. newbios-halt1.rom)" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
case $(wc -c < "$1") in
	131072) cp "$1" "$tmp/bios" ;;
	262144) dd if="$1" of="$tmp/bios" bs=1024 skip=128 count=128 2>/dev/null ;;
	*) echo "bios-update: $1 is neither 128 nor 256 KiB" >&2; exit 1 ;;
esac
cat "$tmp/bios" "$tmp/bios" > "$tmp/image"
printf '00020000:0003ffff bios\n' > "$tmp/layout"
flashrom -p internal --force --noverify-all -l "$tmp/layout" -i bios -w "$tmp/image"
