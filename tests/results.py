#!/usr/bin/env python3
"""Prints what the test program (tests/make-testprog.py) left in the Lynx's
memory, from a dump of the RAM domain: one name=value a line.

usage: results.py RAM.bin [NAME]   (with a NAME, its value alone)"""
import sys

w = open(sys.argv[1], "rb").read()
if len(w) != 0x10000:
    sys.exit("not a dump of the Lynx's 64 KB of RAM")
v = {
    "frames": w[0x80] | w[0x81] << 8,  # frames counted by timer 2's flag
    "joy": w[0x82],          # the joystick on the last frame
    "joySeen": w[0x83],      # every key pressed so far
    "switchesSeen": w[0x84],  # every switch (Pause is 1)
    "line": w[0x86],         # the screen line drawn last
    "mapctl": w[0x87],       # what 0xFFF9 read at the start
    "linkIn": w[0x8A],       # the last byte that came in through ComLynx for it
    "linkCount": w[0x8B],    # how many came in (wrapping at 256)
}
if len(sys.argv) > 2:
    print(v[sys.argv[2]])
else:
    for k, x in v.items():
        print(f"{k}={x}")
