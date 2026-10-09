#!/usr/bin/env python3
"""Pre-renders Noto Sans (OFL 1.1, tools/fonts/OFL-NotoSans.txt) at every UI pixel size into
client/data/ui_font.bin for client/source/uifont.c, so the 3DS draws glyphs 1:1 instead of
GPU-scaling its soft system font.

Format (LE): "T3F1", u16 tex_w, tex_h, n_sizes, n_glyphs; per size u8 px, pad, u16 first, count;
per glyph u16 codepoint, x, y, u8 w, h, s8 left, top (from the baseline pen), u8 advance, pad;
then the A8 atlas in the GPU's tiled layout (8x8 tiles, Morton order inside).

  python3 tools/make_font.py [font.ttf] [out.bin] [atlas.png]
"""

from __future__ import annotations

import struct
import sys

from PIL import Image, ImageFont

TEX_W = 1024
SMALL_SIZES = list(range(10, 23))
BIG_SIZES = [29, 33, 36, 43]
EXTRA = [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC, 0x2122, 0x266A]
SMALL_SET = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + EXTRA
BIG_SET = list(range(0x20, 0x7F))

# (codepoint, bitmap or None, left, top, advance)
Glyph = tuple[int, Image.Image | None, int, int, int]


def morton(x: int, y: int) -> int:
    """Index of (x, y) inside an 8x8 tile: x in the even bits, y in the odd bits."""
    index = 0
    for bit in range(3):
        index |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
    return index


def render_all(path: str) -> tuple[list[tuple[int, int, int]], list[Glyph]]:
    glyphs: list[Glyph] = []
    sizes = []
    for px in SMALL_SIZES + BIG_SIZES:
        font = ImageFont.truetype(path, px)
        font.set_variation_by_axes([600 if px <= 13 else 500])  # thin strokes read badly when small
        first = len(glyphs)
        for codepoint in SMALL_SET if px in SMALL_SIZES else BIG_SET:
            mask, (left, top) = font.getmask2(chr(codepoint), mode="L", anchor="ls")
            bitmap = Image.frombytes("L", mask.size, bytes(mask)) if mask.size[0] and mask.size[1] else None
            glyphs.append((codepoint, bitmap, left, top, round(font.getlength(chr(codepoint)))))
        sizes.append((px, first, len(glyphs) - first))
    return sizes, glyphs


def main() -> None:
    out_path = sys.argv[2] if len(sys.argv) > 2 else "client/data/ui_font.bin"
    sizes, glyphs = render_all(
        sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf"
    )

    # shelf packing, tallest first, 1 px apart
    positions = {}
    x = y = shelf_h = 0
    for i in sorted(range(len(glyphs)), key=lambda i: -(b.size[1] if (b := glyphs[i][1]) else 0)):
        bitmap = glyphs[i][1]
        if bitmap is None:
            positions[i] = (0, 0)
            continue
        w, h = bitmap.size
        if x + w + 1 > TEX_W:
            x, y, shelf_h = 0, y + shelf_h + 1, 0
        positions[i] = (x, y)
        x += w + 1
        shelf_h = max(shelf_h, h)
    tex_h = 8
    while tex_h < y + shelf_h + 1:
        tex_h *= 2
    if tex_h > 1024:
        sys.exit(f"atlas too tall: {y + shelf_h + 1} px")

    atlas = Image.new("L", (TEX_W, tex_h), 0)
    blob = bytearray(b"T3F1") + struct.pack("<HHHH", TEX_W, tex_h, len(sizes), len(glyphs))
    for px, first, count in sizes:
        blob += struct.pack("<BBHH", px, 0, first, count)
    for i, (codepoint, bitmap, left, top, advance) in enumerate(glyphs):
        x, y = positions[i]
        w, h = bitmap.size if bitmap else (0, 0)
        if bitmap:
            atlas.paste(bitmap, (x, y))
        if not (-128 <= left < 128 and -128 <= top < 128 and w < 256 and h < 256 and advance < 256):
            sys.exit(f"glyph U+{codepoint:04X} out of range")
        blob += struct.pack("<HHHBBbbBB", codepoint, x, y, w, h, left, top, advance, 0)

    linear = atlas.tobytes()
    tiled = bytearray(len(linear))
    for y in range(tex_h):
        for x in range(TEX_W):
            tiled[((y >> 3) * (TEX_W // 8) + (x >> 3)) * 64 + morton(x & 7, y & 7)] = linear[y * TEX_W + x]
    blob += tiled

    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"{out_path}: {len(sizes)} sizes, {len(glyphs)} glyphs, atlas {TEX_W}x{tex_h}, {len(blob) // 1024} KB")
    if len(sys.argv) > 3:
        atlas.save(sys.argv[3])


if __name__ == "__main__":
    main()
