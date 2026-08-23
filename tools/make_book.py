#!/usr/bin/env python3
import os
import re
import sys
import html
from PIL import Image, ImageDraw, ImageFont

W, H = 272, 792
SS = 2
MARGIN_X = 16
MARGIN_TOP = 18
MARGIN_BOT = 34

SERIF = ("Georgia.ttf", "Times New Roman.ttf")

def font(names, size):
    for n in names:
        for p in [f"/System/Library/Fonts/{n}", f"/System/Library/Fonts/Supplemental/{n}"]:
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()

def extract_text(path):
    raw = open(path, encoding="utf-8", errors="replace").read()
    body = re.sub(r"<(script|style)[^>]*>.*?</\1>", " ", raw, flags=re.S | re.I)
    body = re.sub(r"<[^>]+>", "\n", body)
    body = html.unescape(body)
    paras = []
    for p in re.split(r"\n\s*\n", body):
        t = re.sub(r"\s+", " ", p).strip()
        if t:
            paras.append(t)
    return paras

def wrap_line(d, text, fnt, max_w):
    words = text.split()
    if not words:
        return "", ""
    lines = []
    cur = words[0]
    for w in words[1:]:
        if d.textlength(cur + " " + w, font=fnt) <= max_w:
            cur += " " + w
        else:
            lines.append(cur)
            cur = w
    lines.append(cur)
    return lines[0], " ".join(lines[1:])

def render_book(paras, out_dir, title):
    os.makedirs(out_dir, exist_ok=True)
    body_f = font(SERIF, 15 * SS)
    head_f = font(SERIF, 12 * SS)
    page_f = font(SERIF, 9 * SS)
    md = ImageDraw.Draw(Image.new("1", (2, 2), 1))
    d = md

    pages = []
    cur_lines = []
    for para in paras:
        rest = para
        if cur_lines:
            cur_lines.append("")
        while rest:
            line, rest = wrap_line(d, rest, body_f, (W - 2 * MARGIN_X) * SS)
            cur_lines.append(line)
            if len(cur_lines) >= 44:
                pages.append(cur_lines)
                cur_lines = []

    if cur_lines:
        pages.append(cur_lines)

    for i, lines in enumerate(pages):
        img = Image.new("1", (W * SS, H * SS), 1)
        d = ImageDraw.Draw(img)
        y = MARGIN_TOP * SS
        d.text((MARGIN_X * SS, y), title.upper(), font=head_f, fill=0)
        y += 16 * SS
        d.rectangle([MARGIN_X * SS, y, (W - MARGIN_X) * SS, y + 1], fill=0)
        y += 10 * SS
        for line in lines:
            if line:
                d.text((MARGIN_X * SS, y), line, font=body_f, fill=0)
            y += 20 * SS
        pg = "%d" % (i + 1)
        d.text(((W - MARGIN_X) * SS - d.textlength(pg, font=page_f), (H - MARGIN_BOT + 12) * SS), pg, font=page_f, fill=0)

        small = img.resize((W, H), Image.LANCZOS).convert("1")
        name = "page_%03d" % (i + 1)
        small.save(os.path.join(out_dir, name + ".png"))
        with open(os.path.join(out_dir, name + ".bin"), "wb") as f:
            f.write(small.tobytes())

    return len(pages)

if __name__ == "__main__":
    book = sys.argv[1]
    out = sys.argv[2]
    title = os.path.basename(book).replace(".html", "").replace("_", " ").replace("ON ", "on ").title()
    paras = extract_text(book)
    n = render_book(paras, out, title)
    print(f"{n} pages -> {out}")