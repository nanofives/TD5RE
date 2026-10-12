#!/usr/bin/env python3
"""geo_r1021b_void.py -- offline JUNCTION VOID census of a generated geo level (round 1021 B).

Rasterises every world triangle of MODELS.DAT (flags & 0xff == 0, any page, any kind) top-down at
CELL units, keeps the cells within RADIUS of a race span (NETWORK.JSON `spans`) and counts the
connected components of UNCOVERED cells (no triangle above them at all = sky/void seen from the
car) that are at least MIN_CELLS big. The component list is the "junction void" ledger: where two
streets converge on the route and nothing is drawn between them.

  python verify/geo_r1021b_void.py <level dir> [--radius 30000] [--cell 500] [--min 40]
         [--json out.json] [--png out.png]
"""
import importlib.util, json, os, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('r1015b_plot', os.path.join(HERE, 'geo_r1015b_plot.py'))
plot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plot)


def run(lvl, radius=30000.0, cell=500.0, min_cells=40, png=None):
    nj = json.load(open(os.path.join(lvl, 'NETWORK.JSON')))
    sp = [(s[0], s[1], s[2]) for s in nj['spans']]
    xs = [s[1] for s in sp]; zs = [s[2] for s in sp]
    x0, z0 = min(xs) - radius - cell, min(zs) - radius - cell
    W = int((max(xs) + radius + cell - x0) / cell) + 2
    H = int((max(zs) + radius + cell - z0) / cell) + 2
    cov = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(cov)
    for m in plot.load_level(lvl):
        if m['flags'] & 0xff:
            continue
        for page, t in plot.tris(m):
            d.polygon([((p[0] - x0) / cell, (p[2] - z0) / cell) for p in t], fill=255)
    cov = np.array(cov) > 0
    near = Image.new('L', (W, H), 0)
    dn = ImageDraw.Draw(near)
    r = radius / cell
    for (_i, x, z) in sp:
        cx, cz = (x - x0) / cell, (z - z0) / cell
        dn.ellipse([cx - r, cz - r, cx + r, cz + r], fill=255)
    near = np.array(near) > 0
    empty = near & ~cov
    lab, n = ndimage.label(empty)
    sizes = ndimage.sum(empty, lab, range(1, n + 1))
    comps = []
    objs = ndimage.find_objects(lab)
    for i in range(n):
        if sizes[i] < min_cells:
            continue
        sl = objs[i]
        cz_, cx_ = (sl[0].start + sl[0].stop) / 2.0, (sl[1].start + sl[1].stop) / 2.0
        wx, wz = x0 + cx_ * cell, z0 + cz_ * cell
        # nearest race span
        best = min(sp, key=lambda s: (s[1] - wx) ** 2 + (s[2] - wz) ** 2)
        comps.append(dict(cells=int(sizes[i]), m2=round(float(sizes[i]) * (cell / 430.0) ** 2, 1),
                          x=round(wx), z=round(wz), span=best[0],
                          dist=round(((best[1] - wx) ** 2 + (best[2] - wz) ** 2) ** 0.5)))
    comps.sort(key=lambda c: -c['cells'])
    if png:
        img = np.zeros((H, W, 3), np.uint8)
        img[cov] = (70, 70, 70)
        img[empty] = (255, 0, 0)
        Image.fromarray(img).save(png)
    return dict(components=len(comps), void_cells=int(sum(c['cells'] for c in comps)),
                void_m2=round(sum(c['m2'] for c in comps), 1), worst=comps[:40], all=comps)


if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    opt = {}
    argv = sys.argv[1:]
    for i, x in enumerate(argv):
        if x in ('--radius', '--cell', '--min', '--json', '--png'):
            opt[x[2:]] = argv[i + 1]
            if argv[i + 1] in a:
                a.remove(argv[i + 1])
    res = run(a[0], float(opt.get('radius', 30000)), float(opt.get('cell', 500)),
              int(opt.get('min', 40)), opt.get('png'))
    print('void components >= %s cells: %d, total %d cells = %.0f m2' %
          (opt.get('min', 40), res['components'], res['void_cells'], res['void_m2']))
    for c in res['worst'][:15]:
        print('  span %-5d %6.0f m2 at (%d,%d) %d units from the route' % (c['span'], c['m2'], c['x'], c['z'], c['dist']))
    if 'json' in opt:
        json.dump(res, open(opt['json'], 'w'), indent=1)
