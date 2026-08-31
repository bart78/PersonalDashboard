#!/usr/bin/env python3
"""Generate the buslog PWA icons: dark rounded square with a simple bus glyph.

Pure stdlib (zlib + struct), no PIL. Renders at any size by scaling 512-space
coordinates. Run:  python3 make_icons.py
"""
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons")
os.makedirs(OUT, exist_ok=True)

BG = (11, 14, 20)       # #0b0e14 app background
BODY = (226, 232, 240)  # light body
TOPBAND = (148, 163, 184)
ACCENT = (56, 189, 248)  # #38bdf8
WHEEL = (30, 36, 50)


def pixel(S, x, y):
    u = S / 512.0
    s = lambda v: v * u  # noqa: E731
    def in_rect(x0, y0, x1, y1):
        return x0 <= x <= x1 and y0 <= y <= y1
    def in_rrect(x0, y0, x1, y1, r):
        if not in_rect(x0, y0, x1, y1):
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    def in_circle(cx, cy, r):
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r

    if in_rrect(s(64), s(72), s(448), s(396), s(46)):
        color = BODY
        if in_rect(s(96), s(96), s(416), s(240)):
            color = TOPBAND
        if in_rect(s(96), s(96), s(416), s(150)):
            color = ACCENT
        if in_rect(s(100), s(256), s(240), s(330)) or in_rect(s(272), s(256), s(412), s(330)):
            color = ACCENT  # doors
        return color
    if in_circle(s(158), s(388), s(46)) or in_circle(s(354), s(388), s(46)):
        return WHEEL
    return BG


def render(S):
    return [[c for x in range(S) for c in pixel(S, x + 0.5, y + 0.5)] for y in range(S)]


def write_png(path, pixels, size):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(row) for row in pixels)
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)  # 8-bit RGB
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    for size in (192, 512):
        path = os.path.join(OUT, f"icon-{size}.png")
        write_png(path, render(size), size)
        print(f"wrote {path}")
