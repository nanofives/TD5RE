#!/usr/bin/env python3
"""tg_mesh_render.py -- orthographic render of ONE mesh out of a MODELS.DAT.

Written for the J7 "buildings with no sides" round. The control socket has no
camera-position command (td5_control.c: start_race, spectate, framedump,
inject_key ... but nothing that places an eye), so an in-game framedump cannot
be aimed at a particular landmark. This renders the shipped bytes instead,
which is the stronger evidence anyway: it shows the geometry that is actually
in the file, with nothing between it and the picture.

Flat-shaded z-buffer, no textures, no lighting model beyond a face-normal
cosine, because the question is "is there a face here at all".

Usage:
    python re/tools/tg_mesh_render.py LEVELDIR --mesh N --out OUT.png
                                      [--yaw DEG] [--pitch DEG] [--size 640]
    python re/tools/tg_mesh_render.py LEVELDIR --find-pages 562,571
"""
from __future__ import annotations

import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mesh_tool as mt                                   # noqa: E402
from PIL import Image, ImageDraw                         # noqa: E402

from tg_building_audit import newell                     # noqa: E402


def faces_of(mesh):
    verts, cur = mesh["vertices"], 0
    for c in mesh["commands"]:
        for _ in range(c["tri"]):
            if cur + 3 > len(verts):
                return
            yield [verts[cur + k]["pos"] for k in range(3)]
            cur += 3
        for _ in range(c["quad"]):
            if cur + 4 > len(verts):
                return
            yield [verts[cur + k]["pos"] for k in range(4)]
            cur += 4


def render(mesh, yaw, pitch, size, out, title):
    faces = list(faces_of(mesh))
    if not faces:
        raise SystemExit("mesh has no faces")

    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))

    def view(p):
        x, y, z = p
        # yaw about Y, then pitch about the rotated X; depth grows away.
        xr, zr = x * cy - z * sy, x * sy + z * cy
        yr, zr2 = y * cp - zr * sp, y * sp + zr * cp
        return xr, yr, zr2

    pts = [view(v) for f in faces for v in f]
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    pad = 0.08
    w = max(max(xs) - min(xs), max(ys) - min(ys)) or 1.0
    w *= (1.0 + 2 * pad)
    ox = (min(xs) + max(xs)) * 0.5
    oy = (min(ys) + max(ys)) * 0.5
    s = size / w

    def proj(p):
        vx, vy, vz = view(p)
        return ((vx - ox) * s + size * 0.5, size * 0.5 - (vy - oy) * s, vz)

    # Painter's algorithm on face depth. Good enough for a convex-ish shell and
    # it keeps this file dependency-free beyond PIL.
    order = []
    for f in faces:
        pr = [proj(v) for v in f]
        order.append((sum(p[2] for p in pr) / len(pr), pr, f))
    order.sort(key=lambda t: -t[0])

    img = Image.new("RGB", (size, size), (24, 26, 30))
    d = ImageDraw.Draw(img)
    for _z, pr, f in order:
        nrm, area = newell(f)
        if area <= 0:
            continue
        shade = 0.35 + 0.65 * abs(nrm[1] * 0.35 + nrm[0] * 0.5 + nrm[2] * 0.5)
        shade = max(0.0, min(1.0, shade))
        col = (int(70 + 150 * shade), int(78 + 150 * shade), int(90 + 140 * shade))
        d.polygon([(p[0], p[1]) for p in pr], fill=col, outline=(30, 32, 36))
    d.text((8, 8), title, fill=(235, 235, 235))
    img.save(out)
    print("wrote %s (%d faces)" % (out, len(faces)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("leveldir")
    ap.add_argument("--mesh", type=int, default=None)
    ap.add_argument("--find-pages", default=None)
    ap.add_argument("--yaw", type=float, default=0.0)
    ap.add_argument("--pitch", type=float, default=20.0)
    ap.add_argument("--size", type=int, default=640)
    ap.add_argument("--out", default="mesh.png")
    ap.add_argument("--title", default=None)
    args = ap.parse_args()

    with open(os.path.join(args.leveldir, "MODELS.DAT"), "rb") as f:
        model = mt.decode(f.read(), "models")

    if args.find_pages:
        want = {int(t) for t in args.find_pages.split(",") if t.strip()}
        for mi, mesh in enumerate(model["meshes"]):
            pages = {c["texture_page_id"] for c in mesh["commands"]}
            if want & pages:
                print("mesh %-6d faces=%-4d pages=%s"
                      % (mi, sum(c["tri"] + c["quad"] for c in mesh["commands"]),
                         sorted(pages)))
        return

    mesh = model["meshes"][args.mesh]
    render(mesh, args.yaw, args.pitch, args.size, args.out,
           args.title or ("mesh %d" % args.mesh))


if __name__ == "__main__":
    main()
