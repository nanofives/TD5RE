"""geo_r1014a_coverage.py -- what the divided avenues of a generated geo track are made of,
span by span, read back from MODELS.DAT + MESHTAG.BIN (round 1014 A).

  python verify/geo_r1014a_coverage.py <level dir> [<place>]
  python verify/geo_r1014a_coverage.py re/assets/levels/level091 la_plata

Per ring span inside an AVENUES.JSON run it reports which of these the mesh set has:

  scenery  a `branch-road` page-0 mesh in a RING entry: the opposite carriageway as
           scenery, i.e. a road surface with NO span record (not driveable)
  island   a `road` [GREEN, kerb] mesh: the median island, kerb to kerb
  walk     a `branch-road` [SIDEWALK, kerb] mesh: the far footway
  wedge    a mesh of any of those kinds standing past the road edge by less than 1 lane

and totals: avenue spans, spans carrying a scenery carriageway ("looks driveable, is
not"), spans of the median with no island and no opening fill, spans with no footway.
Meshes are assigned to a span by their centre's distance along the route, so the table is
the same for any build of the same route. Reads files only (no game).
"""
import json
import math
import os
import struct
import sys

LVL = sys.argv[1]
PLACE = sys.argv[2] if len(sys.argv) > 2 else 'la_plata'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
route = json.load(open(os.path.join(ROOT, 're', 'assets', 'geo', PLACE, '_route', 'ROUTE.JSON')))
av = json.load(open(os.path.join(ROOT, 're', 'assets', 'geo', PLACE, '_route', 'AVENUES.JSON')))
P = [(p['x'], p['z']) for p in route['points']]
RING = route['spans']
SPAN = route['span_length']
b = open(os.path.join(LVL, 'MODELS.DAT'), 'rb').read()
t = open(os.path.join(LVL, 'MESHTAG.BIN'), 'rb').read()
stride = struct.unpack_from('<5I', t, 0)[4]
names = ("other skirt road branch-road tunnel gantry end-wall deck water coast decal city block "
         "cross flora park-tree terrain building prop rail branch-side").split()

# the along-route coordinate of a point: nearest node + the fraction along its segment
def along(x, z):
    best, bi = None, 0
    for i in range(0, len(P) - 1):
        ax, az = P[i]
        bx, bz = P[i + 1]
        dx, dz = bx - ax, bz - az
        l2 = dx * dx + dz * dz
        if l2 <= 0:
            continue
        f = ((x - ax) * dx + (z - az) * dz) / l2
        f = max(0.0, min(1.0, f))
        d = (x - ax - f * dx) ** 2 + (z - az - f * dz) ** 2
        if best is None or d < best:
            best, bi = d, i + f
    return bi, math.sqrt(best)

nent = struct.unpack_from('<I', b, 0)[0]
ring_entries = RING // 4          # entries wholly inside the ring (the next one mixes in corridor meshes)
spans = {}          # span -> {kind: n}
for e in range(min(nent, ring_entries)):
    eo, es = struct.unpack_from('<II', b, 4 + e * 8)
    nm = struct.unpack_from('<I', b, eo)[0]
    for s in range(nm):
        mo = struct.unpack_from('<I', b, eo + 4 + s * 4)[0]
        m = eo + mo
        magic, flags, ncmd, nvtx = struct.unpack_from('<HHII', b, m)
        if nvtx > 24:
            continue
        k = t[20 + e * stride + s]
        kn = names[k] if k < len(names) else str(k)
        if kn not in ('road', 'branch-road'):
            continue
        cmdoff = struct.unpack_from('<I', b, m + 0x2C)[0]
        pages = [struct.unpack_from('<HHIHHI', b, m + cmdoff + c * 16)[1] for c in range(ncmd)]
        rad, cx, cy, cz = struct.unpack_from('<ffff', b, m + 12)
        a, dist = along(cx, cz)
        sp = int(a)
        if sp >= RING:
            continue
        if kn == 'branch-road' and pages == [0]:
            cls = 'scenery'
        elif kn == 'road' and len(pages) == 2 and pages[0] == 2:
            cls = 'island'
        elif kn == 'branch-road' and len(pages) == 2 and pages[0] == 44:
            cls = 'walk'
        elif kn == 'road' and pages == [0] and rad < 1000:
            cls = 'fill'
        else:
            continue
        spans.setdefault(sp, {}).setdefault(cls, 0)
        spans[sp][cls] += 1

tot = 0
rows = []
for run in av['avenues']:
    for sp in range(run['s0'], run['s1'] + 1):
        tot += 1
        c = spans.get(sp, {})
        rows.append((sp, c.get('scenery', 0), c.get('island', 0), c.get('walk', 0), c.get('fill', 0), run['name']))

n_scen = sum(1 for r in rows if r[1] > 0)
n_noisl = sum(1 for r in rows if r[2] == 0 and r[4] == 0)
n_nowalk = sum(1 for r in rows if r[3] == 0)
print('%s: %d avenue spans in %d run(s)' % (LVL, tot, len(av['avenues'])))
print('  spans with a SCENERY carriageway (road surface, no span record): %d (%.0f%%)'
      % (n_scen, 100.0 * n_scen / max(1, tot)))
print('  spans with no island and no opening fill: %d' % n_noisl)
print('  spans with no far footway: %d' % n_nowalk)


def runs(pred):
    out, cur = [], None
    for r in rows:
        if pred(r):
            if cur and cur[1] == r[0] - 1:
                cur[1] = r[0]
            else:
                cur = [r[0], r[0]]
                out.append(cur)
    return out


print('  scenery-carriageway runs:', ' '.join('%d..%d' % (a, b2) for a, b2 in runs(lambda r: r[1] > 0)))
print('  no-island runs          :', ' '.join('%d..%d' % (a, b2) for a, b2 in runs(lambda r: r[2] == 0 and r[4] == 0)))
print('  no-footway runs         :', ' '.join('%d..%d' % (a, b2) for a, b2 in runs(lambda r: r[3] == 0)))
