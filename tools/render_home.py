#!/usr/bin/env python3
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 272, 792
SS = 2

SANS = ("Helvetica.ttc", "HelveticaNeue.ttc")

def font(names, size, bold=False):
    for n in names:
        for p in [f"/System/Library/Fonts/{n}", f"/System/Library/Fonts/Supplemental/{n}"]:
            if os.path.exists(p):
                try:
                    return ImageFont.truetype(p, size, index=1 if bold else 0)
                except Exception:
                    return ImageFont.truetype(p, size)
    return ImageFont.load_default()

def tracked(d, xy, text, fnt, fill, tracking):
    x, y = xy
    for ch in text:
        d.text((x, y), ch, font=fnt, fill=fill)
        x += d.textlength(ch, font=fnt) + tracking

img = Image.new("1", (W * SS, H * SS), 1)
d = ImageDraw.Draw(img)

tiny = font(SANS, 9 * SS)
word = font(SANS, 46 * SS, bold=True)
label = font(SANS, 17 * SS, bold=True)
num = font(SANS, 10 * SS)
rem_line = font(SANS, 10 * SS)

def P(v):
    return v * SS

tracked(d, (P(12), P(4)), "PERSONAL DASHBOARD  ED.001", tiny, 0, P(1))
d.text((P(12), P(20)), "bart78@", font=word, fill=0)
d.rectangle([P(12), P(70), P(260), P(71)], fill=0)

CARD_NAMES = ["WEATHER", "NAV", "CALENDAR", "NEWS",
              "STOCKS", "BOOKS", "CARD", "TODO"]
ROWS = [120, 276, 432, 588]
CELL_H = 140

for i, name in enumerate(CARD_NAMES):
    x = 8 if i % 2 == 0 else 142
    y = ROWS[i // 2]
    d.rectangle([P(x), P(y), P(x + 122), P(y + CELL_H)], outline=0)
    tracked(d, (P(x + 8), P(y + 10)), "%02d" % (i + 1), num, 0, P(1))
    tracked(d, (P(x + 8), P(y + 36)), name, label, 0, P(1))

d.rectangle([P(12), P(776), P(260), P(777)], fill=0)

img = img.resize((W, H), Image.LANCZOS).convert("1")
d1 = ImageDraw.Draw(img)
for i, name in enumerate(CARD_NAMES):
    x = 8 if i % 2 == 0 else 142
    y = ROWS[i // 2]
    d1.rectangle([x, y, x + 122, y + CELL_H], outline=0)
d1.rectangle([12, 70, 260, 71], fill=0)
d1.rectangle([12, 776, 260, 777], fill=0)
data = img.tobytes()
with open("/Users/bartjarochowski/Dev/AntigravityProjects/Crow/m3project/src/home_base.h", "w") as f:
    f.write("// home_base 272x792 1bpp (%d bytes)\n" % len(data))
    f.write("const unsigned char HOME_BASE[%d] = {\n" % len(data))
    for i in range(0, len(data), 16):
        f.write("  " + ", ".join(f"0x{b:02x}" for b in data[i:i+16]) + ",\n")
    f.write("};\n")
img.save("/tmp/home_base_v5.png")
print("home_base v5:", len(data), "bytes")