#!/usr/bin/env python3
"""Text of an 86Box screenshot (or a box of it, 640x480 coordinates) by the local tesseract OCR: bright pixels (the
light colors of text) become black on white, scaled x3, then tesseract --psm 6. usage: ocr.py PNG [X0 Y0 X1 Y1]"""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngcolors import pixels

w, h, bpp, rows = pixels(sys.argv[1])
x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6]) if len(sys.argv) >= 6 else (0, 0, 640, 480)
sx, sy, k = w / 640, h / 480, 3
out = bytearray()
for y in range(int(y0 * sy), int(y1 * sy)):
    line = bytearray()
    for x in range(int(x0 * sx), int(x1 * sx)):
        r, g, b = rows[y][x * bpp:x * bpp + 3]
        line += (b'\x00' if max(r, g, b) > 0x90 and r + g > 0xc0 else b'\xff') * k   # bright, not the blue
    out += bytes(line) * k
with tempfile.NamedTemporaryFile(suffix='.pgm', delete=False) as f:
    f.write(b'P5 %d %d 255\n' % (len(out) // ((int(y1 * sy) - int(y0 * sy)) * k), (int(y1 * sy) - int(y0 * sy)) * k))
    f.write(out)
try:
    print(subprocess.run(['tesseract', f.name, '-', '--psm', '6'], capture_output=True, text=True, check=True).stdout)
finally:
    os.unlink(f.name)
