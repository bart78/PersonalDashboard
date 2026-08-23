#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path

from PIL import Image

DEFAULT_W = 272
DEFAULT_H = 792

DITHER_MODES = ["floyd", "bayer", "halftone", "threshold"]


def bayer_matrix(size):
    if size == 2:
        return [[0, 2], [3, 1]]
    half = bayer_matrix(size // 2)
    return [
        [4 * v + off for v in row]
        for off, row in zip((0, 3), (half, half))
    ] + [
        [4 * v + off for v in row]
        for off, row in zip((2, 1), (half, half))
    ]


def ordered_dither(img, size=4):
    mat = bayer_matrix(size)
    scale = 256 / (size * size)
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            v = px[x, y]
            if not isinstance(v, tuple):
                v = (v, v, v)
            gray = v[0]
            threshold = mat[y % size][x % size] * scale
            out = 0 if gray + threshold < 128 else 255
            px[x, y] = (out, out, out)
    return img


def halftone_dither(img, cell=4):
    px = img.load()
    out = Image.new("1", img.size)
    opx = out.load()
    for cy in range(0, img.height, cell):
        for cx in range(0, img.width, cell):
            total = 0
            count = 0
            for y in range(cy, min(cy + cell, img.height)):
                for x in range(cx, min(cx + cell, img.width)):
                    v = px[x, y]
                    if not isinstance(v, tuple):
                        v = (v, v, v)
                    total += v[0]
                    count += 1
            avg = total / count
            cover = (255 - avg) / 255
            n = round(cell * cell * cover)
            for i in range(n):
                y = cy + i // cell
                x = cx + i % cell
                if y < img.height and x < img.width:
                    opx[x, y] = 0
    return out


def to_hlsb(img):
    w, h = img.size
    data = bytearray((w * h + 7) // 8)
    px = img.load()
    for y in range(h):
        row_start = y * w
        for x in range(w):
            v = px[x, y]
            if not isinstance(v, tuple):
                v = (v, v, v)
            if v[0] < 128:
                data[(row_start + x) // 8] |= 0x80 >> (x % 8)
    return bytes(data)


def to_c_array(data, width, height, name):
    lines = [f"const unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    lines.append(f"// {width}x{height}, 1bpp MONO_HLSB, {len(data)} bytes")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(
        description="Convert an image for the CrowPanel 5.79\" e-paper (272x792). "
        "Outputs 1bpp MONO_HLSB raw bytes + optional C array and preview."
    )
    ap.add_argument("input", type=Path, help="source image (PNG/JPG/...)")
    ap.add_argument("-o", "--out", type=Path, default=None, help="output .bin path (default: <input>.bin)")
    ap.add_argument("-W", "--width", type=int, default=DEFAULT_W, help=f"target width (default {DEFAULT_W})")
    ap.add_argument("-H", "--height", type=int, default=DEFAULT_H, help=f"target height (default {DEFAULT_H})")
    ap.add_argument("-r", "--rotate", type=int, choices=[0, 90, 180, 270], default=0,
                    help="rotate source before fitting (default 0)")
    ap.add_argument("-f", "--fit", choices=["cover", "contain"], default="cover",
                    help="cover = crop to fill (default); contain = fit whole image")
    ap.add_argument("-d", "--dither", choices=DITHER_MODES, default="floyd",
                    help="1-bit conversion method (default floyd)")
    ap.add_argument("-b", "--bayer-size", type=int, choices=[2, 4, 8], default=4, help="bayer matrix size")
    ap.add_argument("-c", "--cell", type=int, default=4, help="halftone cell size (default 4)")
    ap.add_argument("-i", "--invert", action="store_true", help="invert black/white")
    ap.add_argument("--header", action="store_true", help="also emit a C array .h file")
    ap.add_argument("--header-name", default="epd_image", help="C array variable name")
    ap.add_argument("-p", "--preview", action="store_true", help="write a preview PNG of the result")
    args = ap.parse_args()

    if not args.input.exists():
        raise SystemExit(f"input not found: {args.input}")

    img = Image.open(args.input).convert("L")
    if args.rotate:
        img = img.rotate(args.rotate, expand=True)

    target = (args.width, args.height)
    if args.fit == "cover":
        scale = max(args.width / img.width, args.height / img.height)
        img = img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)
        left = (img.width - args.width) // 2
        top = (img.height - args.height) // 2
        img = img.crop((left, top, left + args.width, top + args.height))
    else:
        img = img.resize((args.width, args.height), Image.LANCZOS)

    if args.invert:
        img = img.point(lambda v: 255 - v)

    if args.dither == "floyd":
        img = img.convert("1", dither=Image.FLOYDSTEINBERG)
    elif args.dither == "bayer":
        ordered_dither(img.convert("RGB"))
        img = img.convert("1")
    elif args.dither == "halftone":
        img = halftone_dither(img, args.cell)
    else:
        img = img.convert("1")

    data = to_hlsb(img)
    out = args.out or args.input.with_suffix(".bin")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)

    report = [f"wrote {out} ({len(data)} bytes, {args.width}x{args.height} 1bpp, dither={args.dither})"]
    if args.header:
        hpath = out.with_suffix(".h")
        hpath.write_text(to_c_array(data, args.width, args.height, args.header_name))
        report.append(f"wrote {hpath}")
    if args.preview:
        png = Image.frombytes("1", img.size, img.tobytes())
        ppath = out.with_suffix(".preview.png")
        png.save(ppath)
        report.append(f"wrote {ppath}")
    print("\n".join(report))


if __name__ == "__main__":
    main()