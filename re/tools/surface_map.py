#!/usr/bin/env python3
"""surface_map.py — top-down render of a STRIP.DAT colored ROAD (gray) vs GRASS (green).

Two grass sources:
  - whole-span grass: TD6 surface_attribute(+1) index >= 16 (the wide run-off "fans")
  - per-lane shoulders: the outer `shoulder` lanes on EACH side of every road span
    (road is the center; grass runs along both edges the whole lap).

Renders PER-LANE so the paved-road width is visible. Tune the shoulder count with
arg 3 (lanes per side; default = lane_count//4). Use this to confirm the road
width visually before baking the shoulder grass into the converter.

Usage: python re/tools/surface_map.py <STRIP.DAT> <out.png> [shoulder_lanes_per_side|-1=auto]
"""
import struct, sys
from PIL import Image, ImageDraw

path = sys.argv[1] if len(sys.argv) > 1 else "re/assets/levels/level007/STRIP.DAT"
out  = sys.argv[2] if len(sys.argv) > 2 else "re/analysis/td6_surface_map.png"
shoulder_arg = int(sys.argv[3]) if len(sys.argv) > 3 else -1   # -1 = auto (lane_count//4)
d = open(path, "rb").read()
span_off, ring, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", d, 0)

def span(i):
    o = span_off + i*24
    return dict(t=d[o], surf=d[o+1], lmask=d[o+2], lanes=d[o+3] & 0xF,
                lv=struct.unpack_from("<H", d, o+4)[0],
                rv=struct.unpack_from("<H", d, o+6)[0],
                ox=struct.unpack_from("<i", d, o+12)[0],
                oz=struct.unpack_from("<i", d, o+20)[0])
def vtx(i):
    x, _, z = struct.unpack_from("<hhh", d, vtx_off + i*6); return x, z
def wpos(s, vi):
    x, z = vtx(vi); return (s['ox'] + x, s['oz'] + z)

xs, zs = [], []
for i in range(ring):
    s = span(i)
    for k in range(s['lanes']+1):
        for base in (s['lv'], s['rv']):
            vi = base + k
            if 0 <= vi < vtx_cnt:
                x, z = wpos(s, vi); xs.append(x); zs.append(z)
minx, maxx, minz, maxz = min(xs), max(xs), min(zs), max(zs)
W = H = 1100; mgn = 20
def px(x, z):
    fx = (x-minx)/(maxx-minx+1); fz = (z-minz)/(maxz-minz+1)
    return (mgn + fx*(W-2*mgn), H - (mgn + fz*(H-2*mgn)))

img = Image.new("RGB", (W, H), (15, 15, 25)); dr = ImageDraw.Draw(img)
n_grass_span = n_shoulder = 0
for i in range(ring):
    s = span(i); L = s['lanes']
    if s['t'] in (9, 10) or L < 1: continue
    sh = (L // 4) if shoulder_arg < 0 else shoulder_arg
    if L <= 2: sh = 0
    whole_grass = (s['lmask'] == 0xFF)   # remap set 0xFF for TD6 whole-span grass (the fans)
    if whole_grass: n_grass_span += 1
    for k in range(L):
        try:
            quad = [wpos(s, s['lv']+k), wpos(s, s['lv']+k+1),
                    wpos(s, s['rv']+k+1), wpos(s, s['rv']+k)]
        except Exception:
            continue
        is_shoulder = (k < sh) or (k >= L - sh)
        if whole_grass:
            col = (40, 170, 40)
        elif is_shoulder:
            col = (40, 200, 40); n_shoulder += 1
        else:
            col = (150, 150, 155)   # paved road
        dr.polygon([px(x, z) for (x, z) in quad], fill=col, outline=(0,0,0))
img.save(out)
print(f"wrote {out}: {ring} spans, shoulder/side={'auto(L//4)' if shoulder_arg<0 else shoulder_arg}")
print(f"  whole-grass spans={n_grass_span}, per-lane shoulder cells flagged={n_shoulder}")
