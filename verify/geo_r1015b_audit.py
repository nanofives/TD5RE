#!/usr/bin/env python3
"""geo_r1015b_audit.py -- offline pavement / street-surface audit of a generated geo level
(round 1015 B). Reads MODELS.DAT + MESHTAG.BIN only, no game needed.

Two counts (both must go DOWN with a fix, never up):

  overlap  PAVEMENT (page 44 SIDEWALK) lying INSIDE a carriageway: a sample point of a pavement
           triangle that is >= --inset units inside a carriageway triangle in plan view
           (itself and 4 neighbours at +-inset all inside) with the pavement within
           [-lo, +hi] of the road surface height. A pavement beside a road touches its edge;
           it never has the road under its interior. Carriageway = road/branch-road page 0 and
           the cross-street page (R4_CROSS = 101). Reported per pavement mesh (e,s).

  under    a STREET surface (cross-street page 101, corridor page 0 on branch-road, optionally
           main road) whose sample points have a GROUND / lawn / PAVEMENT triangle ABOVE them
           (the street renders below the tiles texture). A sample counts when the covering
           surface is more than --dy units above the street at that point.

  python verify/geo_r1015b_audit.py <level dir> [--top 25] [--json out.json] [--inset 200]
"""
import collections
import importlib.util
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('r1015b_plot', os.path.join(HERE, 'geo_r1015b_plot.py'))
plot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plot)

CELL = 6000.0
PAGE_ROAD = 0
PAGE_SIDEWALK = 44
PAGE_CROSS = 101
GROUND_PAGES = {5, 2, 65}
ROAD_KINDS = {'road', 'branchroad'}
STREET_KINDS = {'cross', 'city'}


def mesh_tris(m):
    """[(page, ((x,y,z)*3))] with the mesh's (entry, slot)."""
    return plot.tris(m)


class Grid:
    def __init__(self):
        self.cells = collections.defaultdict(list)
        self.tri = []

    def add(self, t, tag):
        xs = [p[0] for p in t]
        zs = [p[2] for p in t]
        i = len(self.tri)
        self.tri.append((t, tag))
        for gx in range(int(min(xs) // CELL), int(max(xs) // CELL) + 1):
            for gz in range(int(min(zs) // CELL), int(max(zs) // CELL) + 1):
                self.cells[(gx, gz)].append(i)

    def at(self, x, z):
        """Every (y, tag) of a triangle containing (x, z)."""
        out = []
        for i in self.cells.get((int(x // CELL), int(z // CELL)), ()):
            t, tag = self.tri[i]
            y = bary(t, x, z)
            if y is not None:
                out.append((y, tag))
        return out


def bary(t, x, z, strict=True):
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = t
    d = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2)
    if abs(d) < 1e-9:
        return None
    a = ((z1 - z2) * (x - x2) + (x2 - x1) * (z - z2)) / d
    b = ((z2 - z0) * (x - x2) + (x0 - x2) * (z - z2)) / d
    c = 1 - a - b
    if strict and (a < -1e-6 or b < -1e-6 or c < -1e-6):
        return None
    return a * y0 + b * y1 + c * y2


def samples(t):
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = t
    cx, cz = (x0 + x1 + x2) / 3.0, (z0 + z1 + z2) / 3.0
    out = [(cx, cz)]
    for (x, z) in ((x0, z0), (x1, z1), (x2, z2)):
        out.append((cx + (x - cx) * 0.6, cz + (z - cz) * 0.6))
    return out


def audit(lvl, inset=200.0, lo=300.0, hi=1500.0, dy=40.0, top=25, main_under=False):
    ms = plot.load_level(lvl)
    road, ground, street, pave, mainroad = Grid(), Grid(), [], [], []
    nroad = 0
    nisland = 0
    for m in ms:
        if m['flags'] & 0xff:
            continue
        # R15 cross-street median: a 3-quad raised island down the middle of a wide street, by
        # design (tg_city_emit_crossstreet). It is a pavement ON a carriageway on purpose, so it is
        # reported separately and not counted as an overlap.
        island = (m['kind'] == 'city' and len(m['v']) == 12 and m['cmds'] == [(PAGE_SIDEWALK, 0, 3)]
                  and abs(((m['v'][0][0] - m['v'][1][0]) ** 2 + (m['v'][0][2] - m['v'][1][2]) ** 2) ** 0.5 - 520.0) < 40.0)
        if island:
            nisland += 1
        for page, t in mesh_tris(m):
            tag = (m['e'], m['s'], m['kind'], page)
            if page == PAGE_CROSS and m['kind'] in STREET_KINDS:
                road.add(t, tag); street.append((t, tag)); nroad += 1
            elif page == PAGE_ROAD and m['kind'] in ROAD_KINDS:
                road.add(t, tag); nroad += 1
                if m['kind'] == 'branchroad':
                    street.append((t, tag))      # fork corridor surface: same defect class
                else:
                    mainroad.append((t, tag))
            if page == PAGE_SIDEWALK and not island:
                pave.append((t, tag))
            if page in GROUND_PAGES or page == PAGE_SIDEWALK:
                ground.add(t, tag)
    res = {}

    # ---- overlap: pavement samples inside a carriageway -------------------------------
    per_mesh = collections.Counter()
    per_pair = collections.Counter()
    nsamp = 0
    first = {}
    for t, tag in pave:
        e, s, kind, page = tag
        for (x, z) in samples(t):
            nsamp += 1
            hits = road.at(x, z)
            if not hits:
                continue
            ok = True
            for (ox, oz) in ((inset, 0), (-inset, 0), (0, inset), (0, -inset)):
                if not road.at(x + ox, z + oz):
                    ok = False
                    break
            if not ok:
                continue
            py = bary(t, x, z, False)
            if py is None:
                continue
            for (ry, rtag) in hits:
                d = py - ry
                if -lo <= d <= hi:
                    per_mesh[(e, s)] += 1
                    per_pair[(kind, rtag[2] + ':' + str(rtag[3]))] += 1
                    first.setdefault((e, s), (x, py, z, kind, rtag, d))
                    break
    res['overlap'] = dict(
        median_islands=nisland,
        pavement_samples=nsamp,
        overlap_samples=sum(per_mesh.values()),
        meshes=len(per_mesh),
        by_pair={'%s over %s' % k: v for k, v in per_pair.most_common()},
        worst=[dict(e=k[0], s=k[1], samples=v, at=[round(c) for c in first[k][:3]],
                    pave_kind=first[k][3], road=first[k][4][2] + ':' + str(first[k][4][3]),
                    dy=round(first[k][5]))
               for k, v in per_mesh.most_common(top)])

    # ---- under: street surface with ground/pavement above it -------------------------
    def under(group, name):
        per, firsts, n = collections.Counter(), {}, 0
        meshes = set()
        kmesh, khit = collections.Counter(), collections.Counter()
        seen = set()
        for t, tag in group:
            meshes.add(tag[:2])
            if tag[:2] not in seen:
                seen.add(tag[:2]); kmesh[tag[2]] += 1
            for (x, z) in samples(t):
                n += 1
                sy = bary(t, x, z, False)
                if sy is None:
                    continue
                top_y, top_tag = None, None
                for (gy, gtag) in ground.at(x, z):
                    if gtag[:2] == tag[:2]:
                        continue
                    if top_y is None or gy > top_y:
                        top_y, top_tag = gy, gtag
                if top_y is not None and top_y - sy > dy:
                    if tag[:2] not in per:
                        khit[tag[2]] += 1
                    per[tag[:2]] += 1
                    firsts.setdefault(tag[:2], (x, sy, z, top_tag, top_y - sy))
        return dict(
            samples=n, meshes=len(meshes), meshes_hit=len(per), hit_samples=sum(per.values()),
            by_kind={k: '%d of %d meshes' % (khit[k], kmesh[k]) for k in kmesh},
            worst=[dict(e=k[0], s=k[1], samples=v, at=[round(c) for c in firsts[k][:3]],
                        cover=firsts[k][3][2] + ':' + str(firsts[k][3][3]),
                        above=round(firsts[k][4]))
                   for k, v in per.most_common(top)])

    res['under_street'] = under(street, 'street')
    if main_under:
        res['under_main'] = under(mainroad, 'main')
    return res


def show(res, label=''):
    o = res['overlap']
    print('== %s overlap: %d / %d pavement samples inside a carriageway, %d pavement meshes (%d median islands skipped, by design)'
          % (label, o['overlap_samples'], o['pavement_samples'], o['meshes'], o['median_islands']))
    for k, v in o['by_pair'].items():
        print('     %-34s %5d' % (k, v))
    for w in o['worst']:
        print('   e%-4d s%-4d %-4d samples at %s  %s over %s  dy=%d'
              % (w['e'], w['s'], w['samples'], w['at'], w['pave_kind'], w['road'], w['dy']))
    for key in ('under_street', 'under_main'):
        if key in res:
            u = res[key]
            print('== %s %s: %d of %d surface meshes have ground/pavement above them (%d / %d samples)'
                  % (label, key, u['meshes_hit'], u['meshes'], u['hit_samples'], u['samples']))
            print('     by kind: %s' % u['by_kind'])
            for w in u['worst']:
                print('   e%-4d s%-4d %-4d samples at %s  covered by %s  by %d'
                      % (w['e'], w['s'], w['samples'], w['at'], w['cover'], w['above']))


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opt = {}
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a in ('--top', '--json', '--inset', '--lo', '--hi', '--dy', '--label'):
            opt[a[2:]] = argv[i + 1]
            if argv[i + 1] in args:
                args.remove(argv[i + 1])
    res = audit(args[0], inset=float(opt.get('inset', 200)), lo=float(opt.get('lo', 300)),
                hi=float(opt.get('hi', 1500)), dy=float(opt.get('dy', 40)),
                top=int(opt.get('top', 25)), main_under='--main' in argv)
    show(res, opt.get('label', os.path.basename(os.path.normpath(args[0]))))
    if 'json' in opt:
        json.dump(res, open(opt['json'], 'w'), indent=1)
