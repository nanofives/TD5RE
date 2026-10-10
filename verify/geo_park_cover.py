#!/usr/bin/env python3
"""geo_park_cover.py -- are the park lawns of a generated geo level actually SEEN?

Round 1014 E (parks / ground / trees on La Plata). Two measurements read straight
out of MODELS.DAT + MESHTAG.BIN, no game needed:

  top    for every park polygon in AREAS.JSON, sample the polygon on a grid and
         report which page is the TOP surface (highest triangle over the point,
         billboards excluded): lawn (65) / ground (5) / green (2) / other.
         This is what a straight-down camera sees.
  cover  for every lawn mesh (page 65), sample its triangles and report how many
         samples lie UNDER a skirt/terrain surface (hidden) and the lawn-minus-
         surface margin in raw units.
  trees  billboard meshes of kind `block` per entry (park trees), plus the
         busiest entry's mesh count against the 384-slot budget.

  python verify/geo_park_cover.py top   <level dir> [AREAS.JSON]
  python verify/geo_park_cover.py cover <level dir>
  python verify/geo_park_cover.py trees <level dir>

The level dir is re/assets/levels/level091 (or a copy holding MODELS.DAT and
MESHTAG.BIN). Default AREAS.JSON is re/assets/geo/la_plata/_route/AREAS.JSON.
"""
import collections
import json
import os
import struct
import sys

HDR, CMD, VTX = 0x38, 16, 44
GK = ["other", "skirt", "road", "branchroad", "tunnel", "gantry", "endwall",
      "deck", "water", "coast", "decal", "city", "block", "cross", "flora",
      "parktree", "terrain", "building", "prop", "rail", "branchside"]
LAWN = 65
PARK = {'park', 'grass', 'pitch', 'playground', 'common', 'village_green'}
CELL = 20000


def load(lvl):
    b = open(os.path.join(lvl, 'MODELS.DAT'), 'rb').read()
    tag = open(os.path.join(lvl, 'MESHTAG.BIN'), 'rb').read()
    _m, _v, _s, _n, stride = struct.unpack_from('<IIIII', tag, 0)
    tg = tag[20:]
    (n,) = struct.unpack_from('<I', b, 0)
    out = []
    for e in range(n):
        eo, _es = struct.unpack_from('<II', b, 4 + e * 8)
        (nm,) = struct.unpack_from('<I', b, eo)
        for s in range(nm):
            (mo,) = struct.unpack_from('<I', b, eo + 4 + s * 4)
            m = eo + mo
            _mg, flags, ncmd, nvtx = struct.unpack_from('<HHII', b, m)
            cmdoff, = struct.unpack_from('<I', b, m + 0x2C)
            vtxoff, = struct.unpack_from('<I', b, m + 0x30)
            cmds = []
            for c in range(ncmd):
                _d, page, _z, tri, quad, _z2 = struct.unpack_from(
                    '<HHIHHI', b, m + cmdoff + c * CMD)
                cmds.append((page, tri, quad))
            vs = [struct.unpack_from('<fff', b, m + vtxoff + v * VTX)
                  for v in range(nvtx)]
            k = tg[e * stride + s] if s < stride and e * stride + s < len(tg) else 255
            out.append(dict(e=e, s=s, flags=flags, cmds=cmds, v=vs,
                            kind=GK[k] if k < len(GK) else 'k%d' % k))
    return out


def tris(m):
    out, i = [], 0
    for page, t, q in m['cmds']:
        for _ in range(t):
            out.append((page, m['v'][i:i + 3]))
            i += 3
        for _ in range(q):
            v = m['v'][i:i + 4]
            i += 4
            out.append((page, [v[0], v[1], v[2]]))
            out.append((page, [v[0], v[2], v[3]]))
    return out


def bary(t, x, z):
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = t
    d = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2)
    if abs(d) < 1e-9:
        return None
    a = ((z1 - z2) * (x - x2) + (x2 - x1) * (z - z2)) / d
    b = ((z2 - z0) * (x - x2) + (x0 - x2) * (z - z2)) / d
    c = 1 - a - b
    if a < -1e-6 or b < -1e-6 or c < -1e-6:
        return None
    return a * y0 + b * y1 + c * y2


def grid_of(ms, pred):
    g = collections.defaultdict(list)
    for m in ms:
        if not pred(m):
            continue
        for page, t in tris(m):
            xs = [p[0] for p in t]
            zs = [p[2] for p in t]
            for gx in range(int(min(xs) // CELL), int(max(xs) // CELL) + 1):
                for gz in range(int(min(zs) // CELL), int(max(zs) // CELL) + 1):
                    g[(gx, gz)].append((page, t))
    return g


def top_at(g, x, z):
    best = None
    for page, t in g.get((int(x // CELL), int(z // CELL)), []):
        y = bary(t, x, z)
        if y is not None and (best is None or y > best[0]):
            best = (y, page)
    return best


def inside(pts, x, z):
    c = False
    n = len(pts)
    for i in range(n):
        a, b = pts[i], pts[(i + 1) % n]
        if (a['z'] > z) != (b['z'] > z) and \
                x < (b['x'] - a['x']) * (z - a['z']) / (b['z'] - a['z']) + a['x']:
            c = not c
    return c


def cmd_top(lvl, areas_path):
    areas = json.load(open(areas_path))['areas']
    ms = load(lvl)
    g = grid_of(ms, lambda m: not (m['flags'] & 0xff))
    step = 1800
    tot = collections.Counter()
    for a in areas:
        if a['kind'] not in PARK:
            continue
        pts = a['points']
        xs = [p['x'] for p in pts]
        zs = [p['z'] for p in pts]
        if (max(xs) - min(xs)) * (max(zs) - min(zs)) < 20000 * 20000:
            continue
        cnt = collections.Counter()
        n = 0
        x = min(xs)
        while x <= max(xs):
            z = min(zs)
            while z <= max(zs):
                if inside(pts, x, z):
                    r = top_at(g, x, z)
                    n += 1
                    cnt['none' if r is None else r[1]] += 1
                z += step
            x += step
        if not n or cnt.get('none', 0) == n:
            continue                       # not on the generated route
        lawn, gr = cnt.get(LAWN, 0), cnt.get(5, 0) + cnt.get(2, 0)
        print('%-34s id %-10d samples %5d  lawn(65) %3d%%  ground(5)+green(2) '
              '%3d%%  none %3d%%  other %3d%%'
              % (str(a.get('name') or a['kind'])[:34], a['id'], n,
                 100 * lawn // n, 100 * gr // n, 100 * cnt.get('none', 0) // n,
                 100 * (n - lawn - gr - cnt.get('none', 0)) // n))
        tot.update(cnt)
    n = sum(tot.values())
    if n:
        print('TOTAL samples %d  lawn %.1f%%  ground+green %.1f%%  none %.1f%%'
              % (n, 100.0 * tot.get(LAWN, 0) / n,
                 100.0 * (tot.get(5, 0) + tot.get(2, 0)) / n,
                 100.0 * tot.get('none', 0) / n))


def cmd_cover(lvl):
    ms = load(lvl)
    g = grid_of(ms, lambda m: m['kind'] in ('skirt', 'terrain'))
    for m in ms:
        if not any(c[0] == LAWN for c in m['cmds']):
            continue
        n = hidden = 0
        diffs = []
        for page, t in tris(m):
            if page != LAWN:
                continue
            (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = t
            N = 8
            for i in range(N + 1):
                for j in range(N + 1 - i):
                    a, b = i / N, j / N
                    c = 1 - a - b
                    x = a * x0 + b * x1 + c * x2
                    z = a * z0 + b * z1 + c * z2
                    y = a * y0 + b * y1 + c * y2
                    r = top_at(g, x, z)
                    if r is None:
                        continue
                    n += 1
                    diffs.append(y - r[0])
                    if r[0] > y:
                        hidden += 1
        if n:
            diffs.sort()
            print('lawn e%d s%d samples %d hidden %d (%.0f%%) lawn-minus-surface '
                  'median %d min %d' % (m['e'], m['s'], n, hidden,
                                        100.0 * hidden / n, diffs[len(diffs) // 2],
                                        diffs[0]))


def cmd_trees(lvl):
    ms = load(lvl)
    by = collections.Counter()
    per = collections.Counter()
    for m in ms:
        per[m['e']] += 1
        if m['kind'] == 'block' and m['flags'] == 1:
            by[m['e']] += 1
    print('park-tree billboards (block, tag 1): %d  by entry %s'
          % (sum(by.values()), sorted(by.items())))
    print('busiest entries (meshes): %s of 384' % per.most_common(3))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    what, lvl = sys.argv[1], sys.argv[2]
    if what == 'top':
        cmd_top(lvl, sys.argv[3] if len(sys.argv) > 3
                else 're/assets/geo/la_plata/_route/AREAS.JSON')
    elif what == 'cover':
        cmd_cover(lvl)
    elif what == 'trees':
        cmd_trees(lvl)
    else:
        sys.exit(__doc__)
