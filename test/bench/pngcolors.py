#!/usr/bin/env python3
"""Counts of the colors (r,g,b) given as hex arguments inside a box of an 86Box screenshot (PNG, 8-bit RGB or RGBA).
usage: pngcolors.py FILE X0 Y0 X1 Y1 RRGGBB...   prints one count per color; the box is in 640x480 coordinates."""
import struct, sys, zlib

def pixels(path):
    data = open(path, 'rb').read()
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            bpp = {2: 3, 6: 4}[ctype]
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows

if __name__ == "__main__":
  w, h, bpp, rows = pixels(sys.argv[1])
  x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
  sx, sy = w / 640, h / 480
  wanted = [tuple(int(c[i:i + 2], 16) for i in (0, 2, 4)) for c in sys.argv[6:]]
  counts = [0] * len(wanted)
  for y in range(int(y0 * sy), int(y1 * sy)):
      for x in range(int(x0 * sx), int(x1 * sx)):
          px = tuple(rows[y][x * bpp:x * bpp + 3])
          for k, c in enumerate(wanted):
              if all(abs(px[i] - c[i]) < 40 for i in range(3)):
                  counts[k] += 1
  print(' '.join(map(str, counts)))
