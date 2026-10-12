"""[R1024 F] Plot one window of the La Plata route: OSM roads, footways, route nodes and the drawn p0/p44 faces.
  python verify/r1024f_plot.py <level dir> <route dir> x0 z0 x1 z1 out.png [entries lo hi]"""
import json, struct, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon as MP
sys.path.insert(0, __file__.rsplit('/', 1)[0] if '/' in __file__ else '.')
import r1024f_footpaths as R

level, route = sys.argv[1], sys.argv[2]
x0, z0, x1, z1 = [float(v) for v in sys.argv[3:7]]
out = sys.argv[7]
lo, hi = (int(sys.argv[8]), int(sys.argv[9])) if len(sys.argv) > 9 else (0, 10 ** 6)
roads = json.load(open(route + '/ROADS.JSON', encoding='utf-8'))['roads']
fw = json.load(open(route + '/FOOTWAYS.JSON', encoding='utf-8'))['footways']
pts = json.load(open(route + '/ROUTE.JSON', encoding='utf-8'))['points']
tagb, ts = R.meshtag(level)
fig, ax = plt.subplots(figsize=(10, 10 * (z1 - z0) / (x1 - x0)))
colmap = {0: '#bbbbbb', 1: '#dddddd', 2: '#888888', 3: '#777777', 10: '#ffffff', 11: '#6fa8dc', 12: '#93c47d', 13: '#f6b26b', 18: '#ead1dc'}
for e, mi, vt, cmds in R.meshes(level + '/MODELS.DAT'):
    if e < lo or e > hi:
        continue
    kind = tagb[20 + e * ts + mi] if mi < ts else 0
    for page, tri, quad, vo in cmds:
        if page not in (0, 44, 65, 5):
            continue
        col = {65: '#b6d7a8', 5: '#f3e5c3'}.get(page, colmap.get(kind, '#cccccc'))
        if page == 0 and kind == 1:
            continue
        z = 2 if page in (0, 44) else 1
        for t in range(tri):
            v = vt[vo + t * 3: vo + t * 3 + 3][:, [0, 2]]
            ax.add_patch(MP(v, closed=True, fc=col, ec='none', alpha=0.8, zorder=z))
        for q in range(quad):
            v = vt[vo + tri * 3 + q * 4: vo + tri * 3 + q * 4 + 4][:, [0, 2]]
            ax.add_patch(MP(v[[0, 1, 2, 3]], closed=True, fc=col, ec='none', alpha=0.8, zorder=z))
for r in roads:
    p = r['points']
    xs = [a['x'] for a in p]; zs = [a['z'] for a in p]
    if max(xs) < x0 or min(xs) > x1 or max(zs) < z0 or min(zs) > z1:
        continue
    ax.plot(xs, zs, '-', color='k', lw=0.6, zorder=5)
    ax.text(xs[len(xs) // 2], zs[len(zs) // 2], r['name'] or '', fontsize=5, zorder=6)
for w in fw:
    p = w['points']
    xs = [a['x'] for a in p]; zs = [a['z'] for a in p]
    if max(xs) < x0 or min(xs) > x1 or max(zs) < z0 or min(zs) > z1:
        continue
    c = {'cycleway': 'm', 'footway': 'r', 'path': 'orange', 'pedestrian': 'brown', 'sidewalk': 'c', 'crossing': 'y', 'steps': 'g'}.get(w['kind'], 'gray')
    ax.plot(xs, zs, '-', color=c, lw=0.8, zorder=7)
    ax.text(xs[0], zs[0], str(w['id']), fontsize=4, color=c, zorder=8)
ax.plot([p['x'] for p in pts], [p['z'] for p in pts], '.-', color='b', ms=2, lw=0.5, zorder=9)
for i, p in enumerate(pts):
    if x0 < p['x'] < x1 and z0 < p['z'] < z1 and i % 2 == 0:
        ax.text(p['x'], p['z'], str(i), fontsize=5, color='b', zorder=9)
ax.set_xlim(x0, x1); ax.set_ylim(z0, z1); ax.set_aspect('equal')
fig.savefig(out, dpi=100)
print('wrote', out)
