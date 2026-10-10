"""geo_r1015e_cover.py -- a top-down COVERAGE MAP of a generated level's ground and road meshes
(round 1015 E): magenta = no quad of any ground/road/pavement kind covers that spot, i.e. a slot
to the sky.

  python verify/geo_r1015e_cover.py <level dir> <cx> <cz> <half> <out.png>
  python verify/geo_r1015e_cover.py re/assets/levels/level091 272000 -207000 25000 cov.png

x, z and half are world units (430 per metre). Colours: green skirt, brown far-band terrain, grey
road, yellow branch road (a fork corridor / wedge / scenery carriageway), blue pavement, purple
city pads, white decals. Reads MODELS.DAT + MESHTAG.BIN only (GenOnly harness runs leave them).
"""
import math
import os
import struct
import sys

from PIL import Image, ImageDraw

lvl, cx, cz, half, out = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
b = open(os.path.join(lvl, 'MODELS.DAT'), 'rb').read()
t = open(os.path.join(lvl, 'MESHTAG.BIN'), 'rb').read()
stride = struct.unpack_from('<5I', t, 0)[4]
names = ("other skirt road branch-road tunnel gantry end-wall deck water coast decal city block "
         "cross flora park-tree terrain building prop rail branch-side").split()
col = {'skirt': (40, 110, 40), 'terrain': (110, 80, 40), 'road': (90, 90, 90),
       'branch-road': (200, 200, 60), 'other': (60, 60, 200), 'branch-side': (60, 60, 200),
       'decal': (255, 255, 255), 'city': (150, 80, 150), 'block': (150, 80, 150)}
order = ['terrain', 'skirt', 'city', 'block', 'other', 'branch-side', 'road', 'branch-road', 'decal']
N = 1000
S = N / (2 * half)
quads = {k: [] for k in order}
nent = struct.unpack_from('<I', b, 0)[0]
for e in range(nent):
    eo, _es = struct.unpack_from('<II', b, 4 + e * 8)
    nm = struct.unpack_from('<I', b, eo)[0]
    for s in range(nm):
        mo = struct.unpack_from('<I', b, eo + 4 + s * 4)[0]
        m = eo + mo
        _magic, _flags, _ncmd, nvtx = struct.unpack_from('<HHII', b, m)
        k = t[20 + e * stride + s]
        kn = names[k] if k < len(names) else str(k)
        if kn not in quads:
            continue
        voff = struct.unpack_from('<I', b, m + 0x30)[0]
        v = [struct.unpack_from('<fff', b, m + voff + i * 44) for i in range(nvtx)]
        for q in range(0, nvtx - 3, 4):
            pts = [(v[q + j][0], v[q + j][2]) for j in range(4)]
            xs = [p[0] for p in pts]
            zs = [p[1] for p in pts]
            if max(xs) < cx - half or min(xs) > cx + half or max(zs) < cz - half or min(zs) > cz + half:
                continue
            quads[kn].append([((p[0] - cx) * S + N / 2, N / 2 - (p[1] - cz) * S) for p in pts])
im = Image.new('RGB', (N, N), (255, 0, 255))
d = ImageDraw.Draw(im)
for kn in order:
    for poly in quads[kn]:
        d.polygon(poly, fill=col[kn])
im.save(out)
n_pix = sum(1 for p in im.getdata() if p == (255, 0, 255))
print('%s: %.1f%% of %.0f x %.0f m uncovered' % (out, 100.0 * n_pix / (N * N), 2 * half / 430.0, 2 * half / 430.0))
