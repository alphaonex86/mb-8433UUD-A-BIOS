#!/usr/bin/env python3
"""Text of an 86Box screenshot of the benchmark pages (VGA mode 12h, the video BIOS's 8x8 font on its 80x60 cell
grid): each cell matched against that font, taken from the emulated Trident's ROM, exactly: no OCR guess. A cell
that is not a character (chart lines) prints as '?'. usage: gridocr.py PNG [ROW0 ROW1]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngcolors import pixels

ROM = os.path.expanduser(os.environ.get('BOX86_VGA_ROM', '~/.local/share/86Box/roms/video/tgui9440/BIOS.BIN'))


def font():
    rom = open(ROM, 'rb').read()
    a = rom.find(bytes([0x30, 0x78, 0xcc, 0xcc, 0xfc, 0xcc, 0xcc, 0x00]))   # 'A' of the IBM 8x8 font
    if a < 0:
        sys.exit('gridocr: no 8x8 font in ' + ROM)
    base = a - 0x41 * 8
    return {bytes(rom[base + c * 8:base + c * 8 + 8]): chr(c) for c in range(32, 127)}


def main():
    glyphs = font()
    w, h, bpp, rows = pixels(sys.argv[1])
    r0, r1 = (int(v) for v in sys.argv[2:4]) if len(sys.argv) >= 4 else (0, 60)
    sx, sy = w / 640, h / 480
    for r in range(r0, r1):
        line = ''
        for c in range(80):
            cell = bytearray(8)
            for y in range(8):
                py = int((r * 8 + y + 0.5) * sy)
                for x in range(8):
                    px = int((c * 8 + x + 0.5) * sx)
                    if sum(rows[py][px * bpp:px * bpp + 3]) > 0x30:
                        cell[y] |= 0x80 >> x
            line += ' ' if not any(cell) else glyphs.get(bytes(cell), '?')
        print(line.rstrip())


main()
