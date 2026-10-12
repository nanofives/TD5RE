#!/usr/bin/env python3
"""r1023e_street_gap.py -- does a built side street reach the main carriageway? (round 1023 E, item 12)

For every connected ARM of page-101 (R4_CROSS) surface in MODELS.DAT, the plan distance from the arm's
nearest vertex to the main road carriageway (page 0 triangles of kind road, sampled every ~300 units).
An arm that meets the road has a vertex on the carriageway edge (gap ~ 0). Arms whose near end stops at
the pavement outer edge (gap ~ pavement + kerb, 1300..1700) or further are "short".

  python verify/r1023e_street_gap.py <level dir> [--list] [--json out.json] [--short 500] [--near 12000]

Arms farther than --near from the road are not side streets that reach the route (a loop, an
avenue's far carriageway) and are reported separately as `far`.
"""
import importlib.util, json, math, os, sys
import numpy as np
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('r1015b_plot', os.path.join(HERE, 'geo_r1015b_plot.py'))
plot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plot)


def sample_tri(t, step):
    (ax, _ay, az), (bx, _by, bz), (cx, _cy, cz) = t
    L = max(math.hypot(bx - ax, bz - az), math.hypot(cx - bx, cz - bz), math.hypot(ax - cx, az - cz))
    n = max(1, int(L / step))
    out = []
    for i in range(n + 1):
        for j in range(n + 1 - i):
            u, v = i / n, j / n
            out.append((ax + (bx - ax) * u + (cx - ax) * v, az + (bz - az) * u + (cz - az) * v))
    return out


def arms_of(quads):
    """union-find over quads sharing a vertex position (rounded to 50 units)"""
    par = list(range(len(quads)))

    def find(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    seen = {}
    for qi, q in enumerate(quads):
        for p in q:
            k = (round(p[0] / 50), round(p[2] / 50))
            if k in seen:
                par[find(qi)] = find(seen[k])
            else:
                seen[k] = qi
    groups = {}
    for qi in range(len(quads)):
        groups.setdefault(find(qi), []).append(quads[qi])
    return list(groups.values())


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    lvl = a[0]
    short = 500.0
    near = 12000.0
    for i, x in enumerate(sys.argv):
        if x == '--short':
            short = float(sys.argv[i + 1])
        if x == '--near':
            near = float(sys.argv[i + 1])
    road_pts = []
    street = []                      # (e, s, kind, [quad vertex lists])
    for m in plot.load_level(lvl):
        if m['flags'] & 0xff:
            continue
        if m['kind'] in ('road', 'branchroad', 'deck'):
            for page, t in plot.tris(m):
                if page == 0:
                    road_pts.extend(sample_tri(t, 400.0))
        qs = []
        i = 0
        for page, t, q in m['cmds']:
            i += t * 3
            for _ in range(q):
                if page == 101:
                    qs.append(m['v'][i:i + 4])
                i += 4
        if qs:
            street.append((m['e'], m['s'], m['kind'], qs))
    tree = cKDTree(np.array(road_pts))
    rows = []
    for e, s, kind, qs in street:
        for arm in arms_of(qs):
            pts = np.array([(p[0], p[2]) for q in arm for p in q])
            d, _ = tree.query(pts)
            rows.append(dict(e=e, s=s, kind=kind, quads=len(arm), gap=round(float(d.min())),
                             cx=round(float(pts[:, 0].mean())), cz=round(float(pts[:, 1].mean()))))
    meets = [r for r in rows if r['gap'] <= short]
    shortr = [r for r in rows if short < r['gap'] <= near]
    far = [r for r in rows if r['gap'] > near]
    print('page-101 arms: %d  meet the road (gap<=%d): %d   SHORT (%d<gap<=%d): %d   far (>%d): %d'
          % (len(rows), short, len(meets), short, near, len(shortr), near, len(far)))
    hist = {}
    for r in shortr:
        b = (r['gap'] // 500) * 500
        hist[b] = hist.get(b, 0) + 1
    print('  short-arm gap histogram (units, 430/m):', sorted(hist.items()))
    for k in sorted({r['kind'] for r in rows}):
        sub = [r for r in rows if r['kind'] == k]
        print('  kind %-6s arms %3d  meet %3d  short %3d  far %3d' % (
            k, len(sub), sum(1 for r in sub if r['gap'] <= short),
            sum(1 for r in sub if short < r['gap'] <= near), sum(1 for r in sub if r['gap'] > near)))
    if '--list' in sys.argv:
        for r in sorted(rows, key=lambda r: -r['gap']):
            print(r)
    for i, x in enumerate(sys.argv):
        if x == '--json':
            json.dump(rows, open(sys.argv[i + 1], 'w'), indent=1)


if __name__ == '__main__':
    main()
