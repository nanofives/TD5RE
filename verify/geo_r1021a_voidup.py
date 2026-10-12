#!/usr/bin/env python3
"""geo_r1021a_voidup.py -- like geo_r1021b_void.py but counts a cell COVERED only by an UP-FACING triangle
(front face seen from above, the sign the road quads have), so a ground slab that is backface-culled from the
car, or a sloped wall, does not hide a hole. Round 1021 A (item 10 of the La Plata feedback).

  python verify/geo_r1021a_voidup.py <level dir> [--radius 12000] [--cell 300] [--min 8] [--png out.png]
"""
import importlib.util, json, os, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('r1015b_plot', os.path.join(HERE, 'geo_r1015b_plot.py'))
plot = importlib.util.module_from_spec(spec); spec.loader.exec_module(plot)

def ny(t):
    ax, ay, az = t[0]; bx, by, bz = t[1]; cx, cz = t[2][0], t[2][2]
    return (bz - az) * (cx - ax) - (bx - ax) * (cz - az)   # y component of (b-a)x(c-a), up to sign

def run(lvl, radius, cell, min_cells, png):
    nj = json.load(open(os.path.join(lvl, 'NETWORK.JSON')))
    sp = [(s[0], s[1], s[2]) for s in nj['spans']]
    xs = [s[1] for s in sp]; zs = [s[2] for s in sp]
    x0, z0 = min(xs) - radius - cell, min(zs) - radius - cell
    W = int((max(xs) + radius + cell - x0) / cell) + 2
    H = int((max(zs) + radius + cell - z0) / cell) + 2
    meshes = [m for m in plot.load_level(lvl) if not (m['flags'] & 0xff)]
    # the up sign: the majority sign over page-0 (road) triangles
    s = [0, 0]
    for m in meshes:
        for page, t in plot.tris(m):
            if page == 0:
                v = ny(t)
                if v > 1: s[1] += 1
                elif v < -1: s[0] += 1
    up = 1 if s[1] >= s[0] else -1
    cov = Image.new('L', (W, H), 0); d = ImageDraw.Draw(cov)
    for m in meshes:
        for page, t in plot.tris(m):
            if ny(t) * up > 1:
                d.polygon([((p[0] - x0) / cell, (p[2] - z0) / cell) for p in t], fill=255)
    cov = np.array(cov) > 0
    near = Image.new('L', (W, H), 0); dn = ImageDraw.Draw(near); r = radius / cell
    for (_i, x, z) in sp:
        cx, cz = (x - x0) / cell, (z - z0) / cell
        dn.ellipse([cx - r, cz - r, cx + r, cz + r], fill=255)
    empty = (np.array(near) > 0) & ~cov
    lab, n = ndimage.label(empty)
    sizes = ndimage.sum(empty, lab, range(1, n + 1)); objs = ndimage.find_objects(lab)
    comps = []
    for i in range(n):
        if sizes[i] < min_cells: continue
        sl = objs[i]
        wx = x0 + (sl[1].start + sl[1].stop) / 2.0 * cell; wz = z0 + (sl[0].start + sl[0].stop) / 2.0 * cell
        best = min(sp, key=lambda q: (q[1] - wx) ** 2 + (q[2] - wz) ** 2)
        comps.append(dict(cells=int(sizes[i]), m2=round(float(sizes[i]) * (cell / 430.0) ** 2, 1), x=round(wx), z=round(wz), span=best[0]))
    comps.sort(key=lambda c: -c['cells'])
    if png:
        img = np.zeros((H, W, 3), np.uint8); img[cov] = (70, 70, 70); img[empty] = (255, 0, 0)
        Image.fromarray(img).save(png)
    return up, comps

if __name__ == '__main__':
    argv = sys.argv[1:]; a = []; opt = {}
    i = 0
    while i < len(argv):
        if argv[i].startswith('--'): opt[argv[i][2:]] = argv[i + 1]; i += 2
        else: a.append(argv[i]); i += 1
    up, comps = run(a[0], float(opt.get('radius', 12000)), float(opt.get('cell', 300)), int(opt.get('min', 8)), opt.get('png'))
    tot = sum(c['m2'] for c in comps if 5 < c['span'] < 765)
    print('up sign %d; uncovered-from-above components >= %s cells: %d; excluding the route ends: %.0f m2' % (up, opt.get('min', 8), len(comps), tot))
    for c in comps[:20]:
        print('  span %-5d %7.0f m2 (%d,%d)' % (c['span'], c['m2'], c['x'], c['z']))
