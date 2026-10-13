#!/usr/bin/env python3
"""x86 BCJ filter (the xz/liblzma algorithm, start offset 0): relative CALL/JMP targets become absolute,
which LZMA compresses better. Coreboot undoes it after decompressing (src/lib/bcj_x86.c).
usage: bcj_x86.py in.elf out.elf    filters the file bytes of every PT_LOAD segment"""
import struct
import sys

ALLOWED = (True, True, True, False, True, False, False, False)
BITNUM = (0, 1, 2, 2, 3, 3, 3, 3)


def msbyte(b):
    return b in (0x00, 0xff)


def bcj_x86(buf, encode):
    buf = bytearray(buf)
    prev_mask, prev_pos, i = 0, -1, 0
    while i < len(buf) - 4:
        if buf[i] & 0xfe != 0xe8:
            i += 1
            continue
        dist = i - prev_pos
        if dist > 3:
            prev_mask = 0
        else:
            prev_mask = (prev_mask << (dist - 1)) & 7
            if prev_mask and (not ALLOWED[prev_mask] or msbyte(buf[i + 4 - BITNUM[prev_mask]])):
                prev_pos, prev_mask, i = i, (prev_mask << 1) | 1, i + 1
                continue
        prev_pos = i
        if not msbyte(buf[i + 4]):
            prev_mask = (prev_mask << 1) | 1
            i += 1
            continue
        src = struct.unpack_from('<I', buf, i + 1)[0]
        while True:
            dest = (src + i + 5 if encode else src - (i + 5)) & 0xffffffff
            if not prev_mask:
                break
            j = BITNUM[prev_mask] * 8
            if not msbyte((dest >> (24 - j)) & 0xff):
                break
            src = dest ^ ((1 << (32 - j)) - 1)
        dest &= 0x01ffffff
        dest = (dest | (-(dest & 0x01000000))) & 0xffffffff
        struct.pack_into('<I', buf, i + 1, dest)
        i += 5
    return bytes(buf)


def filter_elf(data):
    data = bytearray(data)
    phoff, = struct.unpack_from('<I', data, 28)
    phentsize, phnum = struct.unpack_from('<HH', data, 42)
    for n in range(phnum):
        ptype, offset, _, _, filesz = struct.unpack_from('<5I', data, phoff + n * phentsize)
        if ptype == 1 and filesz:
            data[offset:offset + filesz] = bcj_x86(data[offset:offset + filesz], True)
    return bytes(data)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        elf = f.read()
    if elf[:4] != b'\x7fELF' or elf[4] != 1:
        sys.exit('bcj_x86.py: not a 32-bit ELF: ' + sys.argv[1])
    with open(sys.argv[2], 'wb') as f:
        f.write(filter_elf(elf))
