#!/usr/bin/env python3
"""r1023e_cross_uv.py -- which cross-street (page 101) meshes carry the lane paint ACROSS the street
instead of down it (round 1023 E, item 12).

Page 101 (R4_CROSS, tg_emit_texture_page_r4_cross) paints a dashed centre line along the page's X
axis (== u) at its Y centre (== v). It therefore only reads as a centre line when u runs ALONG the
street and v ACROSS it. A mesh whose u runs across the street gets a "rung" every span.

For every page-101 mesh of MODELS.DAT this reads the quads in file order, takes the mesh travel
direction T = last quad centre - first quad centre, and the plan gradient of u (and v) over the
mesh. rotated = |grad u . T| < |grad v . T|, i.e. u does not follow the travel direction. A single
quad (no T) uses its own edges: the street runs along the edge whose u changes (ok) or whose v
changes (rotated) only when that edge is the LONGER one; otherwise it is reported "ambiguous".

  python verify/r1023e_cross_uv.py <level dir> [--json out.json] [--list]
"""
import importlib.util, json, math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('r1015b_plot', os.path.join(HERE, 'geo_r1015b_plot.py'))
plot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plot)

HDR, CMD, VTX = 0x38, 16, 44


def meshes(lvl):
    b = open(os.path.join(lvl, 'MODELS.DAT'), 'rb').read()
    tag = open(os.path.join(lvl, 'MESHTAG.BIN'), 'rb').read()
    _m, _v, _s, _n, stride = struct.unpack_from('<IIIII', tag, 0)
    tg = tag[20:]
    (n,) = struct.unpack_from('<I', b, 0)
    for e in range(n):
        eo, _es = struct.unpack_from('<II', b, 4 + e * 8)
        if eo <= 0 or eo >= len(b):
            continue
        (nm,) = struct.unpack_from('<I', b, eo)
        for s in range(nm):
            (mo,) = struct.unpack_from('<I', b, eo + 4 + s * 4)
            m = eo + mo
            _mg, flags, ncmd, nvtx = struct.unpack_from('<HHII', b, m)
            if flags & 0xff:
                continue
            cmdoff, = struct.unpack_from('<I', b, m + 0x2C)
            vtxoff, = struct.unpack_from('<I', b, m + 0x30)
            vs = []                                           # (x, z, u, v) per vertex
            for v in range(nvtx):
                f = struct.unpack_from('<fff fff I ff', b, m + vtxoff + v * VTX)
                vs.append((f[0], f[2], f[7], f[8]))
            quads = []
            i = 0
            for c in range(ncmd):
                _d, page, _z, tri, quad, _z2 = struct.unpack_from('<HHIHHI', b, m + cmdoff + c * CMD)
                i += tri * 3
                for _ in range(quad):
                    if page == 101:
                        quads.append(vs[i:i + 4])
                    i += 4
            k = tg[e * stride + s] if s < stride and e * stride + s < len(tg) else 255
            kind = plot.GK[k] if k < len(plot.GK) else 'k%d' % k
            if quads:
                yield e, s, kind, quads


def grad(quads):
    """least-squares plan gradient of u and v over all vertices of the quads"""
    pts = [p for q in quads for p in q]
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    mz = sum(p[1] for p in pts) / n
    mu = sum(p[2] for p in pts) / n
    mv = sum(p[3] for p in pts) / n
    sxx = sum((p[0] - mx) ** 2 for p in pts)
    szz = sum((p[1] - mz) ** 2 for p in pts)
    sxz = sum((p[0] - mx) * (p[1] - mz) for p in pts)
    det = sxx * szz - sxz * sxz
    if abs(det) < 1e-6:
        return None
    def g(idx, mean):
        sxy = sum((p[0] - mx) * (p[idx] - mean) for p in pts)
        szy = sum((p[1] - mz) * (p[idx] - mean) for p in pts)
        return ((sxy * szz - szy * sxz) / det, (szy * sxx - sxy * sxz) / det)
    return g(2, mu), g(3, mv)


def classify(quads):
    if len(quads) >= 2:
        def cen(q):
            return (sum(p[0] for p in q) / 4, sum(p[1] for p in q) / 4)
        a, b = cen(quads[0]), cen(quads[-1])
        T = (b[0] - a[0], b[1] - a[1])
        tl = math.hypot(*T)
        if tl > 1.0:
            T = (T[0] / tl, T[1] / tl)
            gr = grad(quads)
            if gr:
                (gu, gv) = gr
                du = abs(gu[0] * T[0] + gu[1] * T[1])
                dv = abs(gv[0] * T[0] + gv[1] * T[1])
                if du + dv > 2e-4:        # a coherent ribbon; otherwise the quads are scattered
                    return ('ok' if du >= dv else 'rotated'), du, dv
        # scattered quads (several mouths in one mesh): vote per quad on its longer edge
        votes = [classify([q])[0] for q in quads]
        r = votes.count('rotated')
        return ('rotated' if r * 2 > len(votes) else 'ok'), 0.0, 0.0
    # single quad (or degenerate): edge q0->q1 and q1->q2
    q = quads[0]
    e1 = (q[1][0] - q[0][0], q[1][1] - q[0][1]); e2 = (q[2][0] - q[1][0], q[2][1] - q[1][1])
    l1, l2 = math.hypot(*e1), math.hypot(*e2)
    du1 = abs(q[1][2] - q[0][2]); dv1 = abs(q[1][3] - q[0][3])
    du2 = abs(q[2][2] - q[1][2]); dv2 = abs(q[2][3] - q[1][3])
    longer = 1 if l1 >= l2 else 2
    if longer == 1:
        return ('ok' if du1 > dv1 else 'rotated'), du1, dv1
    return ('ok' if du2 > dv2 else 'rotated'), du2, dv2


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    lvl = a[0]
    rows = []
    for e, s, kind, quads in meshes(lvl):
        verdict, du, dv = classify(quads)
        rows.append(dict(e=e, s=s, kind=kind, nq=len(quads), verdict=verdict, du=round(du, 4), dv=round(dv, 4)))
    tot = {}
    for r in rows:
        k = (r['kind'], r['verdict'])
        t = tot.setdefault(k, [0, 0])
        t[0] += 1
        t[1] += r['nq']
    print('page-101 meshes: %d, quads: %d' % (len(rows), sum(r['nq'] for r in rows)))
    for (kind, v), (nm, nq) in sorted(tot.items()):
        print('  %-10s %-8s meshes %4d  quads %5d' % (kind, v, nm, nq))
    rot = [r for r in rows if r['verdict'] == 'rotated']
    print('ROTATED meshes %d / %d, quads %d / %d' % (len(rot), len(rows), sum(r['nq'] for r in rot),
                                                      sum(r['nq'] for r in rows)))
    if '--list' in sys.argv:
        for r in rows:
            print(r)
    for i, x in enumerate(sys.argv):
        if x == '--json':
            json.dump(rows, open(sys.argv[i + 1], 'w'), indent=1)


if __name__ == '__main__':
    main()
