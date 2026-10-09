#!/usr/bin/env python3
"""Builds the animated 3D HOME banner client/cia/banner.cgfx (the logo as a stereo 3D scene: the
"3D" floats out and the triangle pulses every 2 s) and its sound client/cia/banner.wav, which
`make -C client cia` packs into the CIA.

Writes a glTF scene to client/build/banner3d/ and converts it with pycgfx
(https://github.com/skyfloogle/pycgfx, commit 1f78850), which has no licence, so it's fetched,
never committed. The HOME-safety fixes after conversion are adapted from ClouDS Music FA's
tools/banner/convert_banner_cgfx.py (MIT, Copyright (c) 2026 ClouDS Music contributors).

    git clone https://github.com/skyfloogle/pycgfx ~/.cache/tube3d-banner/pycgfx
    git -C ~/.cache/tube3d-banner/pycgfx checkout 1f78850
    pip install --user gltflib pillow
    python3 tools/make_banner3d.py [--pycgfx DIR]
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import os
import struct
import subprocess
import sys
from io import BytesIO
from typing import Any

from make_logo import CYAN, CYAN_DK, CYAN_TOP, RED, WHITE, font, gradient_text, text_box
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "client", "cia", "banner.cgfx")
BUILD = os.path.join(ROOT, "client", "build", "banner3d")
LIMIT = 0x80000  # decompressed CGFX size limit for application banners (3dbrew CBMD)

# HOME's banner camera sits at (0, 1, 44.8) looking down -Z, 30 degree FOV: ~40 x 24 units at z = 0
SQ = 5.6  # the play square's side, in scene units
SLAB = 1.2  # its thickness
CENTRE_Y = 1.6  # a little above the camera's centre, since HOME draws below the banner
POP = 4.0  # how far "3D" floats out
LOOP = 2.0  # seconds
KEYS = 61  # keyframes per loop, first == last
LAYERS = 4  # dark layers behind the "3D" face
POP_Z = SLAB / 2 + 0.2  # "3D"'s resting depth

FLOAT, UNSIGNED_SHORT, ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 5126, 5123, 34962, 34963

PixelBox = tuple[int, int, int, int]
UnitBox = tuple[float, float, float, float]  # x0, y_top, x1, y_bottom (y up)
Primitive = tuple[list, list, list | None, list[int], int]  # positions, normals, uvs, indices, material


def layout() -> tuple[UnitBox, UnitBox, UnitBox, dict[str, Image.Image]]:
    """The square, "tube" and "3D" boxes in scene units, and the textures, laid out as make_logo.py."""
    height_px = 400
    size = (height_px * 5, height_px + height_px // 3)
    draw = ImageDraw.Draw(Image.new("L", size, 0))
    square_y = height_px // 12
    square = height_px - 2 * square_y
    tube_font = font(int(height_px * 0.74), 850)
    tube_w, tube_h, tube_left, tube_top = text_box(draw, "tube", tube_font)
    tube_pos = (square + int(height_px * 0.2) - tube_left, square_y + (square - tube_h) / 2 - tube_top)
    pop_font = font(int(height_px * 0.86), 900)
    _, pop_h, _, pop_top = text_box(draw, "3D", pop_font)
    pop_pos = (tube_pos[0] + tube_w + tube_left + int(height_px * 0.04), square_y + (square - pop_h) / 2 - pop_top)
    stroke = int(height_px * 0.035)  # dark outline so "tube" reads on light themes

    def ink_box(text: str, pos: tuple[float, float], f: ImageFont.FreeTypeFont, stroke: int = 0) -> PixelBox:
        mask = Image.new("L", size, 0)
        ImageDraw.Draw(mask).text(pos, text, font=f, fill=255, stroke_width=stroke, stroke_fill=255)
        return mask.getbbox() or (0, 0, 1, 1)

    tube_box = ink_box("tube", tube_pos, tube_font, stroke)
    pop_box = ink_box("3D", pop_pos, pop_font)
    scale = SQ / square
    cx, cy = pop_box[2] / 2, square_y + square / 2

    def units(b: PixelBox) -> UnitBox:
        return (
            (b[0] - cx) * scale,
            CENTRE_Y - (b[1] - cy) * scale,
            (b[2] - cx) * scale,
            CENTRE_Y - (b[3] - cy) * scale,
        )

    def cut(box: PixelBox, kind: str) -> Image.Image:
        img = Image.new("RGBA", size, (0, 0, 0, 0))
        if kind == "tube":
            ImageDraw.Draw(img).text(
                tube_pos, "tube", font=tube_font, fill=(*WHITE, 255), stroke_width=stroke, stroke_fill=(40, 44, 52, 255)
            )
        elif kind == "face":
            gradient_text(img, *pop_pos, "3D", pop_font, CYAN_TOP, CYAN)
        else:
            ImageDraw.Draw(img).text(pop_pos, "3D", font=pop_font, fill=(*CYAN_DK, 255))
        part = img.crop(box)
        return part.resize((256, 128 if part.size[0] > part.size[1] * 1.4 else 256), Image.Resampling.LANCZOS)

    textures = {"tube": cut(tube_box, "tube"), "face": cut(pop_box, "face"), "back": cut(pop_box, "back")}
    return units((0, square_y, square, square_y + square)), units(tube_box), units(pop_box), textures


class Gltf:
    def __init__(self) -> None:
        self.doc: dict = {
            "asset": {"version": "2.0", "generator": "Tube3D tools/make_banner3d.py"},
            "scene": 0,
            "scenes": [{"nodes": []}],
            **{k: [] for k in ("nodes", "meshes", "materials", "textures", "images")},
            "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}],  # linear, clamp
            **{k: [] for k in ("accessors", "bufferViews", "buffers", "animations")},
        }
        self.bin = bytearray()

    def add(self, key: str, item: dict) -> int:
        self.doc[key].append(item)
        return len(self.doc[key]) - 1

    def accessor(self, values: list, kind: str, target: int | None = None, minmax: bool = False, fmt: str = "f") -> int:
        flat = [c for v in values for c in (v if kind != "SCALAR" else (v,))]
        self.bin += bytes(-len(self.bin) % 4)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(flat) * struct.calcsize(fmt)}
        if target:
            view["target"] = target
        self.bin += struct.pack(f"<{len(flat)}{fmt}", *flat)
        acc = {
            "bufferView": self.add("bufferViews", view),
            "componentType": FLOAT if fmt == "f" else UNSIGNED_SHORT,
            "count": len(values),
            "type": kind,
        }
        if minmax:
            columns = list(zip(*values, strict=True)) if kind != "SCALAR" else [values]
            acc["min"] = [min(c) for c in columns]
            acc["max"] = [max(c) for c in columns]
        return self.add("accessors", acc)

    def material(self, name: str, color: tuple[float, ...] | None = None, image: Image.Image | None = None) -> int:
        pbr: dict = {"metallicFactor": 0.0, "roughnessFactor": 1.0}
        mat: dict = {"name": name, "pbrMetallicRoughness": pbr, "doubleSided": True}
        if color:
            pbr["baseColorFactor"] = list(color)
        if image is not None:
            png = BytesIO()
            image.save(png, "PNG")
            source = self.add("images", {"uri": "data:image/png;base64," + base64.b64encode(png.getvalue()).decode()})
            pbr["baseColorTexture"] = {"index": self.add("textures", {"sampler": 0, "source": source})}
            mat["alphaMode"] = "MASK"  # hard-edged cut-out: BLEND materials crash HOME
            mat["alphaCutoff"] = 0.5
        return self.add("materials", mat)

    def mesh(self, name: str, prims: list[Primitive]) -> int:
        out = []
        for pos, nrm, uv, idx, mat in prims:
            attrs = {
                "POSITION": self.accessor(pos, "VEC3", ARRAY_BUFFER, minmax=True),
                "NORMAL": self.accessor(nrm, "VEC3", ARRAY_BUFFER),
            }
            if uv is not None:
                attrs["TEXCOORD_0"] = self.accessor(uv, "VEC2", ARRAY_BUFFER)
            indices = self.accessor(idx, "SCALAR", ELEMENT_ARRAY_BUFFER, fmt="H")
            out.append({"attributes": attrs, "indices": indices, "material": mat, "mode": 4})  # triangles
        return self.add("meshes", {"name": name, "primitives": out})

    def node(
        self, name: str, mesh: int | None = None, children: list[int] | None = None, translation: Any = None
    ) -> int:
        node: dict = {"name": name}
        if mesh is not None:
            node["mesh"] = mesh
        if children:
            node["children"] = children
        if translation:
            node["translation"] = list(translation)
        return self.add("nodes", node)

    def save(self, path: str) -> None:
        uri = "data:application/octet-stream;base64," + base64.b64encode(self.bin).decode()
        self.doc["buffers"] = [{"byteLength": len(self.bin), "uri": uri}]
        for key in ("textures", "images", "animations"):
            if not self.doc[key]:
                del self.doc[key]
        with open(path, "w") as f:
            json.dump(self.doc, f, indent=1)


def rounded_rect(w: float, h: float, r: float, seg: int = 8) -> list[tuple[float, float]]:
    """Outline of a w x h rectangle centred on (0, 0) with corner radius r, counter-clockwise."""
    corners = ((w / 2 - r, h / 2 - r), (-w / 2 + r, h / 2 - r), (-w / 2 + r, -h / 2 + r), (w / 2 - r, -h / 2 + r))
    return [
        (cx + r * math.cos(a), cy + r * math.sin(a))
        for k, (cx, cy) in enumerate(corners)
        for a in (math.radians(90 * k + 90 * i / seg) for i in range(seg + 1))
    ]


def prism(outline: list[tuple[float, float]], z0: float, z1: float, material: int) -> Primitive:
    """A closed extrusion of a convex counter-clockwise outline from z0 (back) to z1 (front)."""
    pos: list = []
    nrm: list = []
    idx: list[int] = []
    n = len(outline)
    cx = sum(p[0] for p in outline) / n
    cy = sum(p[1] for p in outline) / n
    for z, nz in ((z1, 1.0), (z0, -1.0)):  # caps (fans)
        c = len(pos)
        pos += [(cx, cy, z)] + [(x, y, z) for x, y in outline]
        nrm += [(0.0, 0.0, nz)] * (n + 1)
        for i in range(n):
            a, b = c + 1 + i, c + 1 + (i + 1) % n
            idx += [c, a, b] if nz > 0 else [c, b, a]
    for i in range(n):  # sides
        (x0, y0), (x1, y1) = outline[i], outline[(i + 1) % n]
        length = math.hypot(x1 - x0, y1 - y0) or 1.0
        first = len(pos)
        pos += [(x0, y0, z0), (x1, y1, z0), (x1, y1, z1), (x0, y0, z1)]
        nrm += [((y1 - y0) / length, -(x1 - x0) / length, 0.0)] * 4
        idx += [first, first + 1, first + 2, first, first + 2, first + 3]
    return pos, nrm, None, idx, material


def quad(x0: float, y0: float, x1: float, y1: float, z: float, material: int) -> Primitive:
    """A camera-facing textured rectangle, image top-left at (x0, y0); y0 > y1."""
    pos = [(x0, y1, z), (x1, y1, z), (x1, y0, z), (x0, y0, z)]
    uv = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    return pos, [(0.0, 0.0, 1.0)] * 4, uv, [0, 1, 2, 0, 2, 3], material


def centre(box: UnitBox) -> tuple[float, float]:
    return (box[0] + box[2]) / 2, (box[1] + box[3]) / 2


def build_scene(path: str) -> None:
    square, tube, pop_box, tex = layout()
    g = Gltf()
    red = g.material("Red", (*(v / 255 for v in RED), 1.0))
    white = g.material("White", (*(v / 255 for v in WHITE), 1.0))
    m_tube = g.material("Tube", image=tex["tube"])
    m_face = g.material("Face3D", image=tex["face"])
    m_back = g.material("Back3D", image=tex["back"])

    scx, scy = centre(square)
    play = g.mesh("Play", [prism(rounded_rect(SQ, SQ, SQ * 0.28), -SLAB / 2, SLAB / 2, red)])
    s, nudge = SQ * 0.22, SQ * 0.03  # nudged right, as in the 2D logo
    tri_outline = [(-s * 0.8 + nudge, -s), (s + nudge, 0.0), (-s * 0.8 + nudge, s)]
    tri = g.mesh("Tri", [prism(tri_outline, SLAB / 2, SLAB / 2 + 0.35, white)])
    pcx, pcy = centre(pop_box)
    px0, py0, px1, py1 = pop_box[0] - pcx, pop_box[1] - pcy, pop_box[2] - pcx, pop_box[3] - pcy
    pop_prims = [
        quad(px0 + i * 0.12, py0 - i * 0.15, px1 + i * 0.12, py1 - i * 0.15, -i * 0.4, m_back)
        for i in range(LAYERS, 0, -1)
    ]
    pop = g.mesh("Pop3D", [*pop_prims, quad(px0, py0, px1, py1, 0.0, m_face)])
    # the billboarded label; position baked into the vertices since CGFX billboards around the bone origin
    label = g.mesh("name", [quad(*tube, 0.3, m_tube)])

    n_play = g.node("Play", play, translation=(scx, scy, 0.0))
    n_tri = g.node("Tri", tri, translation=(scx, scy, 0.0))
    n_pop = g.node("Pop3D", pop, translation=(pcx, pcy, POP_Z))
    world = g.node("world", children=[n_play, n_tri, n_pop])
    g.doc["scenes"][0]["nodes"] = [g.node("COMMON", children=[world, g.node("name", label)])]

    # rigid linear tracks, first pose == last so the loop is seamless
    times = [LOOP * i / (KEYS - 1) for i in range(KEYS)]
    t_in = g.accessor(times, "SCALAR", minmax=True)
    samplers: list[dict] = []
    channels: list[dict] = []
    ease = [(1 - math.cos(2 * math.pi * t / LOOP)) / 2 for t in times]  # 0 -> 1 -> 0
    pulse = [(1 - math.cos(4 * math.pi * t / LOOP)) / 2 for t in times]  # twice per loop
    no_rotation = [(0.0, 0.0, 0.0, 1.0)] * KEYS
    for node, target, values, kind in (
        (n_pop, "translation", [(pcx, pcy, POP_Z + POP * e) for e in ease], "VEC3"),
        (n_pop, "rotation", no_rotation, "VEC4"),
        (n_pop, "scale", [(1.0 + 0.06 * e,) * 3 for e in ease], "VEC3"),
        (n_tri, "translation", [(scx, scy, 0.0)] * KEYS, "VEC3"),
        (n_tri, "rotation", no_rotation, "VEC4"),
        (n_tri, "scale", [(1.0 + 0.12 * p, 1.0 + 0.12 * p, 1.0) for p in pulse], "VEC3"),
    ):
        samplers.append({"input": t_in, "output": g.accessor(values, kind), "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": target}})
    g.doc["animations"] = [{"name": "Tube3D_Loop", "samplers": samplers, "channels": channels}]
    g.save(path)


def fix_model(model: Any) -> None:
    """Billboards the label, binds meshes to nodes, refuses skinning, makes materials matte."""
    from cgfx.mtob import ColorFloat, FragmentLightingFlags
    from cgfx.primitives import VertexAttributeUsage
    from cgfx.sobj import BillboardMode

    label_bone = model.skeleton.bones["name"]
    if label_bone is None:
        sys.exit("no 'name' bone")
    label_bone.billboard_mode = BillboardMode.YAxial
    for mesh in model.meshes.data.contents:
        node = next(
            (n for n in ("name", "Play", "Tri", "Pop3D") if mesh.name == n or mesh.name.startswith(n + "_")), None
        )
        if node is None:
            sys.exit(f"mesh {mesh.name} has no node")
        mesh.mesh_node_name = node  # pycgfx leaves it empty; some banners crashed HOME without it
    for shape in model.shapes.data.contents:
        for primitive_set in shape.primitive_sets.data.contents:
            if primitive_set.skinning_mode != 0:
                sys.exit(f"{shape.name}: skinning mode {primitive_set.skinning_mode} (froze HOME on hardware)")
        for attr in shape.vertex_attributes.data.contents:
            for usage in attr.vertex_streams.data.contents if hasattr(attr, "vertex_streams") else (attr,):
                if usage.usage in (VertexAttributeUsage.BoneIndex, VertexAttributeUsage.BoneWeight):
                    sys.exit(f"{shape.name}: skin attributes")
    for name in model.materials:  # pycgfx's white specular washes the logo out under HOME's light
        mat = model.materials[name]
        mat.material_color.constant[0] = ColorFloat(0, 0, 0, 1)
        stage = mat.fragment_shader.texture_combiners[2]
        stage.src_rgb = 0xFFF
        stage.combine_rgb = 0
        mat.fragment_shader.fragment_lighting.flags = FragmentLightingFlags(0)
        mat.fragment_shader.fragment_lighting_table.distribution_0_sampler = None


def convert(gltf_path: str, pycgfx_root: str) -> int:
    if not os.path.isfile(os.path.join(pycgfx_root, "main.py")):
        sys.exit(f"pycgfx not found at {pycgfx_root} (see this script's docstring)")
    sys.path.insert(0, pycgfx_root)
    import gltflib
    import main as pycgfx
    from cgfx.canm import CANMBoneTransform, PrimitiveType

    cgfx = pycgfx.convert_gltf(gltflib.GLTF.load(gltf_path, load_file_resources=True))  # type: ignore[misc]
    model = cgfx.data.models["COMMON"]
    if model is None or getattr(model, "skeleton", None) is None:
        sys.exit("no COMMON model with a skeleton")
    fix_model(model)
    anim = cgfx.data.skeletal_animations["COMMON"]
    if anim is None:
        sys.exit("no COMMON skeletal animation")
    # only the two moving nodes, each with an ordinary rigid Transform
    members = {m: anim.member_animations_data[m] for m in anim.member_animations_data}
    if set(members) != {"Tri", "Pop3D"}:
        sys.exit("animation members: " + ", ".join(sorted(members)))
    for m, member in members.items():
        if not isinstance(member, CANMBoneTransform) or member.primitive_type != PrimitiveType.Transform:
            sys.exit(f"{m} is not an ordinary Transform (5)")
    data = pycgfx.write(cgfx)
    if len(data) > LIMIT:
        sys.exit(f"banner.cgfx is {len(data)} bytes, over the {LIMIT} limit")
    with open(OUT, "wb") as f:
        f.write(data)
    return len(data)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pycgfx", default=os.path.expanduser("~/.cache/tube3d-banner/pycgfx"))
    args = parser.parse_args()
    os.makedirs(BUILD, exist_ok=True)
    gltf_path = os.path.join(BUILD, "banner3d.gltf")
    build_scene(gltf_path)
    size = convert(gltf_path, args.pycgfx)
    print(f"wrote {os.path.normpath(OUT)} ({size} bytes, limit {LIMIT}); scene {os.path.normpath(gltf_path)}")
    # two soft notes, 1.2 s (HOME allows 3 s)
    wav = os.path.join(os.path.dirname(OUT), "banner.wav")
    graph = (
        "sine=f=659.25:d=0.35,volume=0.35,afade=t=out:st=0.2:d=0.15[a];"
        "sine=f=987.77:d=0.85,volume=0.35,afade=t=out:st=0.3:d=0.55[b];"
        "[a][b]concat=n=2:v=0:a=1,aformat=sample_fmts=s16:sample_rates=32728:channel_layouts=stereo"
    )
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-filter_complex", graph, wav], check=True)
    print(f"wrote {os.path.normpath(wav)}")


if __name__ == "__main__":
    main()
