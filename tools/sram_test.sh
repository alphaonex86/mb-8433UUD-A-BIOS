#!/bin/bash
# sram_test.sh: test one 32Kx8 SRAM of the L2 cache (EM51256C-15P and other 62256-pinout chips, DIP-28) in the
# T48/TL866 with minipro. Each pattern is written and verified by minipro in the same session (the socket loses
# power between two runs, so a separate read-back would see garbage). Patterns: all 0, all 1, 55/AA checkerboards,
# address low byte, address high byte (stuck or swapped address lines), pseudo-random.
# The programmer is slow: a hard fault (dead cell, stuck bit or address line) shows, not a chip too slow at 15 ns.
#   tools/sram_test.sh            DEV=62256 by default (minipro -q t48 -L 62256)
# Exit 0: all patterns verified; 1: a pattern did not verify (bad chip); 2: minipro could not run the test.
set -u
dev=${DEV:-62256}
tmp=$(mktemp -d)
trap 'rm -r -- "$tmp"' EXIT
python3 - "$tmp" <<'EOF'
import random, sys
d = sys.argv[1]
n = 0x8000
pats = {
    "zeros": bytes(n), "ones": b"\xff" * n, "55aa": bytes(0x55 if i & 1 else 0xaa for i in range(n)),
    "aa55": bytes(0xaa if i & 1 else 0x55 for i in range(n)), "addr-low": bytes(i & 0xff for i in range(n)),
    "addr-high": bytes((i >> 8) ^ 0x5a for i in range(n)),
}
random.seed(486)
pats["random"] = bytes(random.getrandbits(8) for _ in range(n))
for name, data in pats.items():
    open(f"{d}/{name}.bin", "wb").write(data)
EOF
fails=0
for p in zeros ones 55aa aa55 addr-low addr-high random; do
  minipro -p "$dev" -w "$tmp/$p.bin" > "$tmp/$p.log" 2>&1
  rc=$?
  if grep -qi "verification failed" "$tmp/$p.log"; then
    echo "pattern $p: FAILED"
    grep -i "verification failed" "$tmp/$p.log" | head -3
    fails=$((fails + 1))
  elif [ $rc != 0 ]; then
    cat "$tmp/$p.log"
    echo "minipro could not test the chip (exit $rc, log above): no verdict on the SRAM"
    exit 2
  else
    echo "pattern $p: OK"
  fi
done
same=$(grep -hi "verification failed" "$tmp"/*.log | sed 's/.*Device=//' | sort -u | wc -l)
if [ $fails = 7 ] && [ "$same" = 1 ]; then   # every pattern read back the same byte: nothing was written
  echo "NO VERDICT: the chip kept its power-up content, no write took effect. Check its orientation; if a known-good"
  echo "62256 does the same, this programmer/minipro cannot write SRAM (T48 support is incomplete in minipro)"
  exit 2
fi
[ $fails = 0 ] && echo "SRAM OK (no hard fault)" || echo "SRAM BAD: $fails of 7 patterns failed"
[ $fails = 0 ]
