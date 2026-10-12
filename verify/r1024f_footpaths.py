"""[R1024 F] Offline audit of OSM footway/cycleway quads in a generated MODELS.DAT.

  python verify/r1024f_footpaths.py <level dir> <route dir> [--list] [--entry N ...]

Finds every page-44 (SIDEWALK) quad whose centre line lies on a FOOTWAYS.JSON polyline of a kind the
footpath emitters draw (footway / path / pedestrian / cycleway), then reports

  * BURIED : quad top is at or under the park lawn (page 65) triangle that covers its centre
  * LONE   : a footpath-emitter quad (single-quad p44 mesh) with no road / pavement / lawn surface
             within LONE_GAP units of its footprint -- a thin strip standing alone in the ground

Mesh format: see verify/r12geom_dumpmodels.py.
"""
import json, math, struct, sys
import numpy as np

HDR, CMD, VTX = 0x38, 16, 44
PAGE_SIDEWALK, PAGE_LAWN, PAGE_ROAD = 44, 65, 0
KINDS = ('footway', 'path', 'pedestrian', 'cycleway')
LONE_GAP = 700.0          # ~1.6 m: nearer than this to any other surface and it is not alone


def meshes(path):
    b = open(path, 'rb').read()
    (nent,) = struct.unpack_from('<I', b, 0)
    for e in range(nent):
        eoff, esize = struct.unpack_from('<II', b, 4 + e * 8)
        if esize == 0:
            continue
        (nm,) = struct.unpack_from('<I', b, eoff)
        for i in range(nm):
            (mo,) = struct.unpack_from('<I', b, eoff + 4 + i * 4)
            m = eoff + mo
            magic, flags, ncmd, nvtx = struct.unpack_from('<HHII', b, m)
            cmdoff, = struct.unpack_from('<I', b, m + 0x2C)
            vtxoff, = struct.unpack_from('<I', b, m + 0x30)
            vt = np.frombuffer(b, dtype='<f4', count=nvtx * 11, offset=m + vtxoff).reshape(nvtx, 11)[:, :3]
            cmds = []
            vo = 0
            for c in range(ncmd):
                _d, page, _z, tri, quad, _z2 = struct.unpack_from('<HHIHHI', b, m + cmdoff + c * CMD)
                cmds.append((page, tri, quad, vo))
                vo += tri * 3 + quad * 4
            yield e, i, vt, cmds


def load_footways(route):
    fw = json.load(open(route + '/FOOTWAYS.JSON', encoding='utf-8'))['footways']
    segs = []
    for wi, w in enumerate(fw):
        if w['kind'] not in KINDS:
            continue
        P = w['points']
        for k in range(len(P) - 1):
            segs.append((P[k]['x'], P[k]['z'], P[k + 1]['x'], P[k + 1]['z'], w['id'], k))
    return np.array([s[:4] for s in segs], dtype=float), [(s[4], s[5]) for s in segs]


def seg_dist(px, pz, S):
    ax, az, bx, bz = S[:, 0], S[:, 1], S[:, 2], S[:, 3]
    dx, dz = bx - ax, bz - az
    l2 = np.maximum(dx * dx + dz * dz, 1e-9)
    t = np.clip(((px - ax) * dx + (pz - az) * dz) / l2, 0, 1)
    return np.hypot(px - (ax + t * dx), pz - (az + t * dz))


SURF_KINDS = {2, 3, 4, 7, 10, 11, 12, 13}   # road, branch-road, tunnel, deck, decal, city, block, cross


def meshtag(level):
    b = open(level + '/MESHTAG.BIN', 'rb').read()
    _m, _v, _seed, nent, stride = struct.unpack_from('<5I', b, 0)
    return b, stride


def main():
    level, route = sys.argv[1], sys.argv[2]
    tagb, tstride = meshtag(level)
    lst = '--list' in sys.argv
    S, ids = load_footways(route)
    print('footway segments (kinds %s): %d' % (','.join(KINDS), len(S)))
    quads = []      # footpath-style quads
    lawn = []       # (ax az ay bx bz by cx cz cy)
    surf = []       # (e, mi, vo, q, polygon pts) of road (p0) and pavement (p44) faces, for the LONE test
    for e, mi, vt, cmds in meshes(level + '/MODELS.DAT'):
        for page, tri, quad, vo in cmds:
            if page == PAGE_LAWN:
                for t in range(tri):
                    v = vt[vo + t * 3: vo + t * 3 + 3]
                    lawn.append(v.reshape(-1))
            if page == PAGE_SIDEWALK and quad > 0:
                for q in range(quad):
                    v = vt[vo + tri * 3 + q * 4: vo + tri * 3 + q * 4 + 4]
                    quads.append((e, mi, len(cmds), tri, quad, v.copy(), vo, q))
            kind = tagb[20 + e * tstride + mi] if mi < tstride else 0
            if page in (PAGE_SIDEWALK, PAGE_ROAD) and kind in SURF_KINDS:
                for t in range(tri):
                    v = vt[vo + t * 3: vo + t * 3 + 3]
                    surf.append((e, mi, vo, -1 - t, v[:, [0, 2]].copy()))
                for q in range(quad):
                    v = vt[vo + tri * 3 + q * 4: vo + tri * 3 + q * 4 + 4]
                    surf.append((e, mi, vo, q, v[:, [0, 2]].copy()))
    lawn = np.array(lawn) if lawn else np.zeros((0, 9))
    print('lawn triangles (p65): %d, sidewalk quads (p44): %d' % (len(lawn), len(quads)))

    foot = []
    for (e, mi, ncmd, tri, quad, v, vo, qi) in quads:
        # two pairings: edges (0,1)/(2,3) or (0,3)/(1,2) -- the footpath emitter uses the first, the
        # plaza real-path pieces the second; the narrow side is the width
        w01 = math.hypot(*(v[1] - v[0])[[0, 2]])
        w03 = math.hypot(*(v[3] - v[0])[[0, 2]])
        if w01 <= w03:
            A = (v[0] + v[1]) / 2; B = (v[3] + v[2]) / 2; wd = w01
        else:
            A = (v[0] + v[3]) / 2; B = (v[1] + v[2]) / 2; wd = w03
        # A and B carry the mid-edge HEIGHT too (index 1)
        dA = seg_dist(A[0], A[2], S); dB = seg_dist(B[0], B[2], S)
        if dA.min() < 60 and dB.min() < 60 and 300 < wd < 4000:
            k = int(np.argmin(dA + dB))
            foot.append(dict(e=e, mesh=mi, vo=vo, qi=qi, n=quad, A=A, B=B, wd=wd, v=v, way=ids[k], tri=tri))
    print('footpath-style quads matched to a footway polyline: %d' % len(foot))

    def lawn_y(x, z):
        """highest lawn-triangle height at (x,z) whose triangle contains it (None when uncovered)"""
        if not len(lawn):
            return None
        ax, az, ay = lawn[:, 0], lawn[:, 2], lawn[:, 1]
        bx, bz, by = lawn[:, 3], lawn[:, 5], lawn[:, 4]
        cx, cz, cy = lawn[:, 6], lawn[:, 8], lawn[:, 7]
        d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
        d = np.where(np.abs(d) < 1e-6, 1e-6, d)
        u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d
        w = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d
        m = (u >= -1e-4) & (w >= -1e-4) & (1 - u - w >= -1e-4)
        if not m.any():
            return None
        y = u[m] * ay[m] + w[m] * by[m] + (1 - u - w)[m] * cy[m]
        return float(y.max())

    buried = []
    lone = []
    on_lawn = 0
    for f in foot:
        v = f['v']
        cxz = v.mean(0)
        ly = lawn_y(cxz[0], cxz[2])
        # sample 3 points along the centre line, the quad is "buried" if the lawn is at/over it at any
        pts = [(f['A'] * (1 - t) + f['B'] * t) for t in (0.0, 0.25, 0.5, 0.75, 1.0)]
        worst = None
        for ti, p in enumerate(pts):
            yy = lawn_y(p[0], p[2])
            if yy is None:
                continue
            ytop = float(p[1])
            d = yy - ytop
            if worst is None or d > worst:
                worst = d
        f['lawn_d'] = worst
        if worst is not None:
            on_lawn += 1
            if worst >= 0.0:
                buried.append(f)
        if f['n'] == 1 and f['tri'] == 0:
            f['single'] = True
    print('footpath-style quads over a lawn triangle: %d ; BURIED (lawn top >= path top): %d' % (on_lawn, len(buried)))
    for th in (20, 50, 100, 200):
        bq = [f for f in buried if f['lawn_d'] >= th]
        print('  lawn top at least %3d raw ABOVE the path: %d quads, %d distinct (way,seg) pieces' % (th, len(bq), len({f['way'] for f in bq})))
    ways = sorted({f['way'][0] for f in buried})
    print('  buried quads by way id:', {w: sum(1 for f in buried if f['way'][0] == w) for w in ways})
    print('  buried "pieces" (distinct way,seg): %d' % len({f['way'] for f in buried}))

    # LONE: a footpath-emitter strip (single-quad p44 mesh) whose nearest road / pavement face is farther
    # than LONE_GAP: it stands alone in the ground. Other footpath strips do not count as company, but
    # lawn faces do (a path on a lawn is not alone).
    from shapely.geometry import Polygon
    from shapely.strtree import STRtree
    mine = {(f['e'], f['mesh'], f['vo'], f['qi']) for f in foot}
    polys, keep = [], []
    for (e, mi, vo, q, pts) in surf:
        if (e, mi, vo, q) in mine:
            continue
        pg = Polygon(pts)
        if not pg.is_valid or pg.area <= 0:
            pg = pg.buffer(0)
            if pg.is_empty:
                continue
        polys.append(pg)
    for tri in lawn:
        polys.append(Polygon([(tri[0], tri[2]), (tri[3], tri[5]), (tri[6], tri[8])]))
    tree = STRtree(polys)
    singles = [f for f in foot if f['n'] == 1 and f['tri'] == 0]
    print('single-quad footpath-emitter meshes: %d' % len(singles))
    lone = []
    for f in singles:
        pg = Polygon(f['v'][:, [0, 2]])
        if not pg.is_valid:
            pg = pg.buffer(0)
        i = tree.nearest(pg)
        d = pg.distance(polys[i]) if i is not None else 1e9
        f['gap'] = d
        if d > LONE_GAP:
            lone.append(f)
    print('LONE single strips (nearest road/pavement/lawn face farther than %.0f units): %d' % (LONE_GAP, len(lone)))
    gaps = sorted(f['gap'] for f in singles)
    print('  gap distribution (units): ' + ' '.join('%.0f' % g for g in gaps[::max(1, len(gaps) // 20)]))
    if lst:
        for f in lone:
            print('  LONE e%d m%d way %s seg %d gap %.0f centre %.0f,%.0f len %.0f wd %.0f' % (
                f['e'], f['mesh'], f['way'][0], f['way'][1], f['gap'], (f['A'][0] + f['B'][0]) / 2,
                (f['A'][2] + f['B'][2]) / 2, math.hypot(*(f['B'] - f['A'])[[0, 2]]), f['wd']))
        for f in sorted(buried, key=lambda q: -q['lawn_d'])[:60]:
            print('  BURIED e%d m%d way %s seg %d lawn-path %+.0f' % (f['e'], f['mesh'], f['way'][0], f['way'][1], f['lawn_d']))
    six = {1016640879, 1016640880, 1016640881, 1319964678, 1319964679, 1319964683}
    sq = [f for f in foot if f['way'][0] in six]
    print('QUADS of the six OSM ways of R1024 F1 (cycleways 879/880/881/678/679 + footway 683): %d  %s' % (
        len(sq), sorted('e%d m%d w%d s%d' % (f['e'], f['mesh'], f['way'][0] % 1000, f['way'][1]) for f in sq)))
    print('summary: footpath_quads=%d buried=%d lone=%d' % (len(foot), len(buried), len(lone)))


if __name__ == '__main__':
    main()
