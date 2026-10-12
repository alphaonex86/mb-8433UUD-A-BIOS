#!/usr/bin/env python3
"""Summarize the Award oracle: for each changed setup item, the PCI config bytes and super I/O registers
that differ from the page baseline. usage: diff.py [vm dir] > award_map.txt"""
import glob, os, re, sys

vm = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'vm')

def regs(d):
    out = {}
    for line in open(os.path.join(d, 'pci.txt')):
        dev, off, *vals = line.split()
        for i, v in enumerate(vals):
            out[(dev, int(off.rstrip(':'), 16) + i)] = int(v, 16)
    for n, line in enumerate(open(os.path.join(d, 'sio.txt'))):
        v = int(line.split()[1], 16)
        out[('sio', 0xc0 + n)] = v & 0x1f if n == 0 else v   # C0 bits 7-5 read random in 86Box
    return out

def key(d):
    m = re.search(r'-(\d+)$', d)
    return (re.sub(r'-\d+$', '', d), int(m.group(1)) if m else -1)

for d in sorted(glob.glob(os.path.join(vm, 'oracle-*-*/')), key=key):
    d = d.rstrip('/')
    name = os.path.basename(d)[len('oracle-'):]
    page = re.sub(r'\d+$', '', name.split('-')[0])          # chip2-4: 2 presses on chip item 4
    page = {'pio': 'pci', 'base': 'base'}.get(page, page)
    if page == 'base' or name == 'pio-0' or not os.path.exists(os.path.join(d, 'pci.txt')):
        if page != 'base':
            print(f'== {name}: NO DATA')
        continue
    base, cur = regs(os.path.join(vm, f'oracle-base-{page}')), regs(d)
    if name.startswith('pio-'):                             # PIO runs have a disk: compare with Auto
        base = regs(os.path.join(vm, 'oracle-pio-0'))
    item = ' | '.join(re.sub(r'\s+', ' ', l[1:].strip()) for l in open(os.path.join(d, 'item.txt')))
    changes = [f'{k[0]} {k[1]:02x}: {base.get(k, -1):02x}->{v:02x}' for k, v in sorted(cur.items())
               if base.get(k) != v]
    print(f'== {name}: {item}')
    for c in changes:
        print('   ', c)
