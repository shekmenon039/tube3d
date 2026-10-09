#!/usr/bin/env python3
"""Draws the Tube3D logo (at 4x, scaled down) into client/data/logo.bin for client/source/logo.c,
and the 48x48 HOME icon client/icon.png. logo.bin holds two layers at two heights (16 px header,
44 px hero): "base" (red play square + "tube") and "pop" (the extruded cyan "3D", shown in front).

Format (LE): "T3L1", u16 tex_w, tex_h, n_images, pad; per image u8 size (0 header, 1 hero),
layer (0 base, 1 pop), u16 x, y, w, h (in the texture), s16 dx, dy (from the logo's top-left),
u16 logo_w, logo_h; then the RGBA8 texture tiled like the GPU wants (8x8 tiles, Morton order
inside), each pixel as A, B, G, R.

  python3 tools/make_logo.py
"""

from __future__ import annotations

import os
import struct

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FONT = "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf"  # Noto Sans, OFL (tools/fonts/)

RGB = tuple[int, int, int]
RED: RGB = (255, 40, 52)
CYAN: RGB = (0, 210, 211)
CYAN_TOP: RGB = (170, 255, 250)
CYAN_DK: RGB = (0, 110, 120)
WHITE: RGB = (255, 255, 255)
SS = 4  # supersampling
TEX_W = 256


def font(px: int, weight: int) -> ImageFont.FreeTypeFont:
    f = ImageFont.truetype(FONT, px)
    f.set_variation_by_axes([weight])
    return f


def text_box(draw: ImageDraw.ImageDraw, text: str, f: ImageFont.FreeTypeFont) -> tuple[int, int, int, int]:
    """The text's ink (w, h, left, top) when drawn at (0, 0)."""
    left, top, right, bottom = (int(v) for v in draw.textbbox((0, 0), text, font=f))
    return right - left, bottom - top, left, top


def gradient_text(
    img: Image.Image, x: float, y: float, text: str, f: ImageFont.FreeTypeFont, top: RGB, bottom: RGB
) -> None:
    """Draws text filled with a vertical gradient from top to bottom colour."""
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).text((x, y), text, font=f, fill=255)
    ink = mask.getbbox()
    if not ink:
        return
    gradient = Image.new("RGBA", img.size)
    draw = ImageDraw.Draw(gradient)
    for row in range(ink[1], ink[3] + 1):
        t = (row - ink[1]) / max(1, ink[3] - ink[1])
        draw.line(
            [(0, row), (img.size[0], row)], fill=(*(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)), 255)
        )
    img.paste(gradient, (0, 0), mask)


def draw_square(draw: ImageDraw.ImageDraw, x: int, y: int, side: int, radius: float, tri: float) -> None:
    """The red rounded play square with its white triangle (half height tri * side)."""
    draw.rounded_rectangle([x, y, x + side, y + side], radius=int(side * radius), fill=(*RED, 255))
    cx, cy, half_h = x + side * 0.53, y + side * 0.5, side * tri
    left = cx - half_h * 0.8
    draw.polygon([(left, cy - half_h), (left, cy + half_h), (cx + half_h, cy)], fill=(*WHITE, 255))


def draw_3d(img: Image.Image, x: float, y: float, f: ImageFont.FreeTypeFont, steps: int, dx: float, dy: float) -> None:
    """ "3D" with a dark extrusion stepping back by (dx, dy), and a gradient face."""
    draw = ImageDraw.Draw(img)
    for i in range(steps, 0, -1):
        color = tuple(int(CYAN_DK[j] * (1 - 0.5 * i / steps)) for j in range(3))
        draw.text((x + i * dx, y + i * dy), "3D", font=f, fill=(*color, 255))
    gradient_text(img, x, y, "3D", f, CYAN_TOP, CYAN)


def layers(h: int) -> list[Image.Image]:
    """The base and pop layers of a logo h pixels tall, at screen size."""
    big_h = h * SS
    base = Image.new("RGBA", (int(big_h * 4.6), big_h + big_h // 3), (0, 0, 0, 0))
    pop = Image.new("RGBA", base.size, (0, 0, 0, 0))
    square_y = big_h // 12
    square = big_h - 2 * square_y
    draw = ImageDraw.Draw(base)
    draw_square(draw, 0, square_y, square, 0.28, 0.22)
    tube_font = font(int(big_h * 0.74), 850)
    tube_w, tube_h, tube_left, tube_top = text_box(draw, "tube", tube_font)
    tube_x = square + int(big_h * 0.2) - tube_left
    draw.text((tube_x, square_y + (square - tube_h) / 2 - tube_top), "tube", font=tube_font, fill=(*WHITE, 255))
    font_3d = font(int(big_h * 0.86), 900)
    _, h_3d, _, top_3d = text_box(draw, "3D", font_3d)
    x_3d = tube_x + tube_w + tube_left + int(big_h * 0.04)
    draw_3d(pop, x_3d, square_y + (square - h_3d) / 2 - top_3d, font_3d, max(2, int(big_h * 0.16)), 0.8, 1)
    return [img.resize((img.size[0] // SS, img.size[1] // SS), Image.Resampling.LANCZOS) for img in (base, pop)]


def icon48() -> Image.Image:
    """Play square over "3D" on the app's dark; fully opaque, as HOME shows clear pixels black."""
    big = 48 * SS
    img = Image.new("RGBA", (big, big), (16, 18, 26, 255))
    draw = ImageDraw.Draw(img)
    square = int(big * 0.5)
    draw_square(draw, (big - square) // 2, int(big * 0.08), square, 0.26, 0.24)
    font_3d = font(int(big * 0.34), 900)
    w_3d, h_3d, left_3d, top_3d = text_box(draw, "3D", font_3d)
    draw_3d(img, (big - w_3d) / 2 - left_3d, big * 0.93 - h_3d - top_3d, font_3d, max(2, int(big * 0.035)), 0.6, 0.8)
    return img.resize((48, 48), Image.Resampling.LANCZOS)


def morton(x: int, y: int) -> int:
    """Index of (x, y) inside an 8x8 tile: x in the even bits, y in the odd bits."""
    index = 0
    for bit in range(3):
        index |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
    return index


def write_bin(path: str) -> None:
    # (size, layer, image, dx, dy, logo_w, logo_h), each layer cropped to its ink
    images = []
    for size, h in enumerate((16, 44)):
        small = layers(h)
        whole = Image.alpha_composite(small[0], small[1]).getbbox() or (0, 0, 1, 1)
        for layer, img in enumerate(small):
            if ink := img.getbbox():
                images.append(
                    (
                        size,
                        layer,
                        img.crop(ink),
                        ink[0] - whole[0],
                        ink[1] - whole[1],
                        whole[2] - whole[0],
                        whole[3] - whole[1],
                    )
                )

    # shelf packing, tallest first, 1 px apart
    x = y = shelf_h = 0
    positions = {}
    for i in sorted(range(len(images)), key=lambda i: -images[i][2].size[1]):
        w, h = images[i][2].size
        if x + w > TEX_W:
            x, y, shelf_h = 0, y + shelf_h + 1, 0
        positions[i] = (x, y)
        x += w + 1
        shelf_h = max(shelf_h, h)
    tex_h = 8
    while tex_h < y + shelf_h:
        tex_h *= 2
    atlas = Image.new("RGBA", (TEX_W, tex_h), (0, 0, 0, 0))

    out = bytearray(b"T3L1" + struct.pack("<HHHH", TEX_W, tex_h, len(images), 0))
    for i, (size, layer, part, dx, dy, logo_w, logo_h) in enumerate(images):
        atlas.paste(part, positions[i])
        out += struct.pack("<BBHHHHhhHH", size, layer, *positions[i], *part.size, dx, dy, logo_w, logo_h)
    linear = atlas.tobytes()
    tiled = bytearray(len(linear))
    for y in range(tex_h):
        for x in range(TEX_W):
            r, g, b, a = linear[(y * TEX_W + x) * 4 : (y * TEX_W + x) * 4 + 4]
            dst = (((y >> 3) * (TEX_W >> 3) + (x >> 3)) * 64 + morton(x & 7, y & 7)) * 4
            tiled[dst : dst + 4] = bytes((a, b, g, r))
    out += tiled
    with open(path, "wb") as f:
        f.write(out)
    print(f"wrote {os.path.normpath(path)}: {len(images)} images, {TEX_W}x{tex_h} RGBA8, {len(out)} bytes")


if __name__ == "__main__":
    write_bin(os.path.join(ROOT, "client", "data", "logo.bin"))
    icon48().save(os.path.join(ROOT, "client", "icon.png"))
    print("wrote client/icon.png")
