#!/bin/bash
# Static check: SeaBIOS's printf (src/output.c bvprintf) has no '-' '+' ' ' '#' flags, no '*' width or
# precision, pads numbers only and knows d u p x X c s % and "%.s". devfuzz runs that engine but only on the
# paths its cases reach; this checks every format (the USB page showed "%*sS f6075:..." on the board).
nb=$(dirname "$(dirname "$(readlink -f "$0")")")
python3 - "$nb/build/seabios/src" <<'PY'
import os, re, sys
bad = []
lit = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
spec = re.compile(r'%([-+ #*]*)(0?)(\d*)(\.?\*?)(l{0,2}|z)([a-zA-Z%])?')
for root, _, files in os.walk(sys.argv[1]):
    for f in files:
        if not f.endswith(('.c', '.h')):
            continue
        path = os.path.join(root, f)
        call = False                    # inside a printf-family call (it may span lines)
        for no, line in enumerate(open(path, errors='replace'), 1):
            if re.search(r'\b(\w*printf|panic|warn\w*)\s*\(', line):
                call = True
            if not call:
                continue
            if ';' in line:
                call = False
            for s in lit.findall(line):
                for m in spec.finditer(s):
                    flags, zero, width, dot, length, conv = m.groups()
                    if conv == '%' and not (flags or zero or width or dot):
                        continue
                    ok = not flags and conv and (conv in 'dupxXc' or (conv == 's' and dot in ('', '.')))
                    ok = ok and not (width and conv in 'sc') and dot in ('', '.') and (dot != '.' or conv == 's')
                    if not ok:
                        bad.append(f'{path}:{no}: "{m.group(0)}" in "{s}"')
for b in bad:
    print(b)
sys.exit(1 if bad else 0)
PY
[ $? = 0 ] && echo "PASS [fmtlint]" || { echo "FAIL [fmtlint]: formats SeaBIOS's printf cannot print (above)"; exit 1; }
