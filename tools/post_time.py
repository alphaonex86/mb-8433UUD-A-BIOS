#!/usr/bin/env python3
"""POST timing of the real board: its BIOS sends each POST code on COM1 (115200 8N1, post_serial.h) as one line; this
prints each with the time since the first one and since the previous one. Power the board on after starting it.
Other lines (an OS console after the boot) print as they come. usage: post_time.py [/dev/ttyUSB0]"""
import os, sys, termios, time

dev = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
fd = os.open(dev, os.O_RDONLY | os.O_NOCTTY)
attr = termios.tcgetattr(fd)
attr[0] = termios.IGNBRK                                        # iflag: raw
attr[1] = 0                                                     # oflag
attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL         # cflag: 8N1, no modem lines
attr[3] = 0                                                     # lflag: no echo, no line editing
attr[4] = attr[5] = termios.B115200
attr[6][termios.VMIN], attr[6][termios.VTIME] = 1, 0
termios.tcsetattr(fd, termios.TCSAFLUSH, attr)
print(f'{dev}: waiting for POST codes (power the board on), Ctrl+C ends', flush=True)

first = prev = None
line = b''
try:
    while True:
        line += os.read(fd, 256)
        while b'\n' in line:
            text, line = line.split(b'\n', 1)
            text = text.rstrip(b'\r').decode('ascii', 'replace')
            now = time.monotonic()
            if first is None:
                first = prev = now
            print(f'{(now - first) * 1000:9.1f} ms  +{(now - prev) * 1000:8.1f}  {text}', flush=True)
            prev = now
except KeyboardInterrupt:
    pass
