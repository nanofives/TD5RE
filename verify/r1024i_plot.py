#!/usr/bin/env python3
"""[R1024 I] Plan-view plot around chosen building meshes of a generated geo level.

  python verify/r1024i_plot.py LEVELDIR out.png e,s [e,s ...] [--win 9000] [--cols 3]

Surfaces (horizontal quads): carriageway p0/p101 grey/orange, pavement p44 yellow, other ground pale green.
The target mesh: roof footprint red fill, wall bottom edges red; other buildings blue outline."""
import sys, math, argparse, os
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
import r1023c_bld_intrude as R

ap = argparse.ArgumentParser()
ap.add_argument("level"); ap.add_argument("out"); ap.add_argument("meshes", nargs="+")
ap.add_argument("--win", type=float, default=9000.0)
ap.add_argument("--cols", type=int, default=3)
ap.add_argument("--px", type=int, default=640)
a = ap.parse_args()
b, meshes = A.load_level(a.level)
want = [tuple(int(t) for t in s.split(",")) for s in a.meshes]
bymk = {(m["e"], m["s"]): m for m in meshes}
tiles = []
for key in want:
    tm = bymk[key]
    cx, cz = tm["c"][0], tm["c"][2]
    W = a.win; S = a.px / (2 * W)
    img = Image.new("RGB", (a.px, a.px), (30, 30, 30)); d = ImageDraw.Draw(img, "RGBA")
    P = lambda x, z: ((x - cx + W) * S, (z - cz + W) * S)
    for m in meshes:
        if abs(m["c"][0] - cx) > W + m["r"] or abs(m["c"][2] - cz) > W + m["r"]:
            continue
        if R.is_billboard(b, m):
            continue
        for typ, page, vs in A.faces(b, m):
            if len(vs) != 4:
                continue
            h = R.horizontal(vs)
            if not h:
                continue
            q = h[0]
            if page in (0,):
                col = (150, 150, 150, 255) if m["kind"] != "skirt" else (60, 80, 60, 255)
            elif page == 101:
                col = (230, 140, 40, 255)
            elif page == 44:
                col = (240, 230, 90, 255)
            elif m["kind"] in ("terrain", "skirt"):
                col = (40, 90, 45, 255)
            else:
                continue
            d.polygon([P(*p) for p in q], fill=col)
    for m in meshes:
        if m["kind"] not in R.BLD_KINDS or R.is_billboard(b, m):
            continue
        if abs(m["c"][0] - cx) > W + m["r"] or abs(m["c"][2] - cz) > W + m["r"]:
            continue
        tgt = (m["e"], m["s"]) == key
        base = min(v[1] for v in A.verts(b, m))
        for typ, page, vs in A.faces(b, m):
            if len(vs) not in (3, 4):
                continue
            ys = [v[1] for v in vs]; pts = [P(v[0], v[2]) for v in vs]
            if min(ys) >= base + 1500 and max(ys) - min(ys) < 400:
                d.polygon(pts, fill=(255, 40, 40, 150) if tgt else (60, 120, 255, 110))
            elif max(ys) - min(ys) >= 1000:
                low = sorted(vs, key=lambda v: v[1])[:2]
                d.line([P(low[0][0], low[0][2]), P(low[1][0], low[1][2])], fill=(255, 0, 0, 255) if tgt else (80, 160, 255, 255), width=2 if tgt else 1)
    d.text((4, 4), "e%d s%d %s %s" % (key[0], key[1], tm["kind"], A.classify(tm)), fill=(255, 255, 255))
    d.text((4, 16), "c=%d,%d win=%d" % (cx, cz, W), fill=(255, 255, 255))
    tiles.append(img)
cols = min(a.cols, len(tiles)); rows = (len(tiles) + cols - 1) // cols
sheet = Image.new("RGB", (cols * a.px, rows * a.px))
for i, t in enumerate(tiles):
    sheet.paste(t, ((i % cols) * a.px, (i // cols) * a.px))
sheet.save(a.out)
print(a.out, sheet.size)
