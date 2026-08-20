#!/usr/bin/env python3
"""
Chuyen file anh (PNG/JPG) thanh header C chua mang RGB565 de ve len man TFT.

Dung:
    python scripts/img2header.py <anh.png> [--size 160] [--name logo]
                                [--out include/logo_rgb565.h]

Ket qua: mang `const uint16_t <name>_rgb565[] PROGMEM` kem <NAME>_W / <NAME>_H.
Vung trong suot cua PNG duoc tron voi nen den (mau nen radar).
"""

import argparse
import os
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="duong dan file anh nguon")
    ap.add_argument("--size", type=int, default=160,
                    help="canh dai nhat sau khi resize, pixel (mac dinh 160)")
    ap.add_argument("--name", default="logo", help="ten bien trong header")
    ap.add_argument("--out", default=None, help="file header xuat ra")
    ap.add_argument("--bg", default="000000",
                    help="mau nen hex de tron vung trong suot (mac dinh 000000)")
    args = ap.parse_args()

    out = args.out or os.path.join("include", f"{args.name}_rgb565.h")

    img = Image.open(args.image).convert("RGBA")
    img.thumbnail((args.size, args.size), Image.LANCZOS)

    bg = tuple(int(args.bg[i:i + 2], 16) for i in (0, 2, 4))
    flat = Image.new("RGB", img.size, bg)
    flat.paste(img, mask=img.split()[3])   # tron alpha len nen

    w, h = flat.size
    px = flat.load()

    words = []
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            words.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

    up = args.name.upper()
    lines = [
        "// Tu dong sinh boi scripts/img2header.py - khong sua tay",
        f"// Nguon: {os.path.basename(args.image)}  ->  {w}x{h} RGB565",
        "#pragma once",
        "#include <Arduino.h>",
        "",
        f"#define {up}_W {w}",
        f"#define {up}_H {h}",
        "",
        f"const uint16_t {args.name}_rgb565[] PROGMEM = {{",
    ]
    for i in range(0, len(words), 12):
        chunk = ", ".join(f"0x{v:04X}" for v in words[i:i + 12])
        lines.append("    " + chunk + ",")
    lines.append("};")

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print(f"Da ghi {out}: {w}x{h} = {len(words)} pixel, {len(words) * 2} byte flash")


if __name__ == "__main__":
    main()
