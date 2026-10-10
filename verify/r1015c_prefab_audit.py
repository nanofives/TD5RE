#!/usr/bin/env python3
"""r1015c_prefab_audit.py -- ROUND 1015 C item 8: is a SHIPPED SET PIECE a closed shell?

Mariano: "remove TD5 (shipped set-piece) geometry that doesn't close out properly ...
audit EVERY prefab in the landmark/set-piece pools for open geometry (missing faces /
unclosed shells / backfaces visible)".

The renderer draws double-sided (D3D12 backend: CullMode NONE), so a back face is never
the problem; a MISSING face is. Measures, all from the generated table
td5_tg_prefab_data.h (the same data the generator stamps):

 0. WALL-LOOP CLOSURE (primary verdict, fast): see wall_closure().
 1. RAY PARITY (--parity only). Parallel rays from 80 exterior directions (5 elevations x 16 azimuths)
    through the whole piece, 0.4 m apart. Every face is double sided; hits closer than
    ~3 raw units merge (coplanar duplicates). A virtual FLOOR (the convex hull of the
    vertices standing on the lowest y) counts as a surface, because a building is closed
    by the ground it stands on. A closed solid is crossed an EVEN number of times by
    every line, so a ray with an ODD count went in (or out) through a face that does
    not exist: that ray is a see-through hole. `odd%` = odd rays / rays that hit anything.
 2. BOUNDARY EDGES. Faces welded at 3 raw units; an edge used by exactly ONE face that
    does not lie on the floor plane is a free edge of an unclosed shell. Reported as
    total metres of free edge (T-junction noise from subdivided walls inflates it, which
    is why parity is the primary number).

Usage:
  python verify/r1015c_prefab_audit.py [--png DIR] [--json OUT] [--only N]
"""
import argparse, json, math, os, re, sys
from collections import defaultdict
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "re", "tools"))
import tg_prefab_audit as PA                      # noqa: E402

U = 411.0                                           # raw units per metre (same as the audit)
WELD = 3.0
TALL_M = 6.0        # a free wall edge at least this tall is a BUILDING wall that stops in the air
MERGE_T = 3.0


def faces_tris(pf):
    tri = []
    fl = []
    for page, poly in PA.faces_of(pf):
        fl.append((page, poly))
        a = [np.array(v, float) for v in poly]
        tri.append((a[0], a[1], a[2]))
        if len(a) == 4:
            tri.append((a[0], a[2], a[3]))
    return fl, tri


def hull2d(pts):
    pts = sorted(set(map(tuple, pts)))
    if len(pts) <= 2:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lo = []
    for p in pts:
        while len(lo) >= 2 and cross(lo[-2], lo[-1], p) <= 0:
            lo.pop()
        lo.append(p)
    up = []
    for p in reversed(pts):
        while len(up) >= 2 and cross(up[-2], up[-1], p) <= 0:
            up.pop()
        up.append(p)
    return lo[:-1] + up[:-1]


def parity(tris, ymin, floor_hull):
    T = np.array(tris)                               # (n,3,3)
    if len(T) == 0:
        return 0, 0, []
    v0 = T[:, 0]
    e1 = T[:, 1] - T[:, 0]
    e2 = T[:, 2] - T[:, 0]
    allp = T.reshape(-1, 3)
    c = allp.mean(axis=0)
    rad = np.linalg.norm(allp - c, axis=1).max()
    step = 0.4 * U
    total = odd = 0
    where = []
    H = np.array(floor_hull) if len(floor_hull) >= 3 else None
    for el in (8, 25, 45, 65, 85):
        for az in range(0, 360, 22):
            a = math.radians(az)
            e = math.radians(el)
            d = np.array([math.cos(e) * math.sin(a), -math.sin(e), math.cos(e) * math.cos(a)])
            up = np.array([0, 1.0, 0]) if abs(d[1]) < 0.99 else np.array([1.0, 0, 0])
            u = np.cross(d, up)
            u /= np.linalg.norm(u)
            w = np.cross(d, u)
            g = np.arange(-rad, rad + step, step)
            gu, gw = np.meshgrid(g, g)
            O = c - d * (rad * 2.0) + gu.reshape(-1, 1) * u + gw.reshape(-1, 1) * w   # (R,3)
            pv = np.cross(d, e2)                      # (n,3)
            det = np.einsum('ij,ij->i', e1, pv)       # (n,)
            ok = np.abs(det) > 1e-9
            inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
            tv = O[:, None, :] - v0[None, :, :]       # (R,n,3)
            uu = np.einsum('rij,ij->ri', tv, pv) * inv
            qv = np.cross(tv, e1[None, :, :])
            vv = np.einsum('j,rij->ri', d, qv) * inv
            tt = np.einsum('ij,rij->ri', e2, qv) * inv
            hit = ok[None, :] & (uu >= -1e-6) & (vv >= -1e-6) & (uu + vv <= 1 + 1e-6) & (tt > 0)
            tf = (ymin - O[:, 1]) / d[1]
            fx = O[:, 0] + d[0] * tf
            fz = O[:, 2] + d[2] * tf
            fh = np.zeros(len(O), bool)
            if H is not None:
                inside = np.ones(len(O), bool)
                for i in range(len(H)):
                    p, q = H[i], H[(i + 1) % len(H)]
                    inside &= ((q[0] - p[0]) * (fz - p[1]) - (q[1] - p[1]) * (fx - p[0])) >= -1e-6
                fh = inside
            for r in range(len(O)):
                ts = list(tt[r][hit[r]])
                if fh[r]:
                    ts.append(tf[r])
                if not ts:
                    continue
                ts.sort()
                merged = [ts[0]]
                for t in ts[1:]:
                    if t - merged[-1] > MERGE_T:
                        merged.append(t)
                total += 1
                if len(merged) % 2 == 1:
                    odd += 1
                    if len(where) < 4000:
                        where.append(O[r] + d * merged[0])
    return total, odd, where


def boundary_edges(fl, ymin):
    inv = 1.0 / WELD

    def key(p):
        return (int(round(p[0] * inv)), int(round(p[1] * inv)), int(round(p[2] * inv)))
    cnt = defaultdict(int)
    pts = {}
    for page, poly in fl:
        n = len(poly)
        for i in range(n):
            a, b = key(poly[i]), key(poly[(i + 1) % n])
            if a == b:
                continue
            k = (a, b) if a < b else (b, a)
            cnt[k] += 1
            pts[k] = (poly[i], poly[(i + 1) % n])
    free = []
    for k, c in cnt.items():
        if c == 1:
            p, q = pts[k]
            if p[1] <= ymin + 20 and q[1] <= ymin + 20:
                continue                                  # on the floor
            free.append((p, q))
    ln = sum(math.dist(p, q) for p, q in free) / U
    return free, ln


def wall_closure(fl, ymin, hmin_m=3.0, tol=60.0):
    """WALL-LOOP CLOSURE. Every near-vertical face projects to a segment in XZ.
    Duplicate segments (one per storey) merge. An endpoint is DANGLING when no
    other segment touches it (endpoint or body within `tol` raw, ~15 cm): the
    wall just stops there, so the shell is open at that edge. A free-standing
    facade sheet has 2 dangling endpoints, a closed block has 0.
    Returns (n_segments, dangling_endpoints, dangling_height_m_sum, free_segments)."""
    segs = {}
    for page, poly in fl:
        xs = [v[0] for v in poly]
        zs = [v[2] for v in poly]
        ys = [v[1] for v in poly]
        h = max(ys) - min(ys)
        # near-vertical: XZ extent collapses to a line
        P = np.array([(v[0], v[2]) for v in poly])
        c = P - P.mean(axis=0)
        sv = np.linalg.svd(c, compute_uv=False)
        if len(sv) < 2 or sv[1] > 25.0:
            continue                                   # a roof / floor / slab
        if h < hmin_m * U:
            continue
        i0 = int(np.argmin(P[:, 0] * 1.0 + P[:, 1] * 1e-3))
        i1 = int(np.argmax(P[:, 0] * 1.0 + P[:, 1] * 1e-3))
        a, b = tuple(P[i0]), tuple(P[i1])
        if math.dist(a, b) < 30.0:
            continue
        k = (round(a[0] / 20), round(a[1] / 20), round(b[0] / 20), round(b[1] / 20))
        old = segs.get(k)
        segs[k] = (a, b, max(h, old[2]) if old else h, min(min(ys), old[3]) if old else min(ys))
    S = list(segs.values())

    def touches(pt, j):
        a, b = np.array(S[j][0]), np.array(S[j][1])
        d = b - a
        L2 = float(d @ d)
        t = 0.0 if L2 == 0 else max(0.0, min(1.0, float((np.array(pt) - a) @ d) / L2))
        return math.dist(pt, tuple(a + t * d)) <= tol
    dang = 0
    dh = 0.0
    free = []
    for i, (a, b, h, y0) in enumerate(S):
        for e in (a, b):
            if not any(j != i and touches(e, j) for j in range(len(S))):
                dang += 1
                dh += h / U
                free.append((e, y0, h))
    return len(S), dang, dh, free


def render(pf, fl, free, path, pct):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    fig = plt.figure(figsize=(14, 7))
    for k, (el, az) in enumerate(((22, 35), (22, 215), (70, 120))):
        ax = fig.add_subplot(1, 3, k + 1, projection="3d")
        polys = [[(p[0] / U, p[2] / U, p[1] / U) for p in poly] for page, poly in fl]
        cols = [plt.cm.tab20((page % 20) / 20.0) for page, poly in fl]
        ax.add_collection3d(Poly3DCollection(polys, facecolors=cols, edgecolors="#00000040",
                                             linewidths=0.3, alpha=0.9))
        for p, q in free:
            ax.plot([p[0] / U, q[0] / U], [p[2] / U, q[2] / U], [p[1] / U, q[1] / U], color="red", lw=2.0)
        xs = [v[0] / U for v in pf["verts"]]
        zs = [v[2] / U for v in pf["verts"]]
        ys = [v[1] / U for v in pf["verts"]]
        span = max(max(xs) - min(xs), max(zs) - min(zs), max(ys) - min(ys))
        cx, cz, cy = (max(xs) + min(xs)) / 2, (max(zs) + min(zs)) / 2, (max(ys) + min(ys)) / 2
        ax.set_xlim(cx - span / 2, cx + span / 2)
        ax.set_ylim(cz - span / 2, cz + span / 2)
        ax.set_zlim(cy - span / 2, cy + span / 2)
        ax.view_init(elev=el, azim=az)
        ax.set_title("%s dangling wall ends=%d (red)" % (pf["name"], pct), fontsize=8)
    fig.savefig(path, dpi=70)
    plt.close(fig)


CLOSE_HDR = os.path.join(ROOT, "td5mod", "src", "td5re", "td5_tg_prefab_close_data.h")


def load_close():
    """The J7 closing quads the generator ADDS to a prefab (TD5RE_TG_PREFAB_CLOSE, default on),
    so the audit judges the EFFECTIVE stamped geometry and not the raw table."""
    src = open(CLOSE_HDR, encoding="utf-8", errors="replace").read()
    quads = []
    for m in re.finditer(r"\{\s*(\d+)\s*,\s*\{([^}]*)\}\s*\}", src.split("k_pfclose_quads[]")[1].split("};")[0]):
        v = [float(t.rstrip("f")) for t in re.split(r"[,\s]+", m.group(2)) if t]
        quads.append((int(m.group(1)), [(v[k * 5], v[k * 5 + 1], v[k * 5 + 2]) for k in range(4)]))
    idx = [tuple(map(int, m.groups())) for m in re.finditer(
        r"\{\s*(\d+)\s*,\s*(\d+)\s*\},\s*/\*\s*L23\.lm\d+", src)]
    ex = [int(m.group(1)) for m in re.finditer(r"^\s*(\d),\s*/\*\s*L23\.lm\d+", src.split("k_pfclose_exclude")[1], re.M)]
    return quads, idx, ex


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", default="")
    ap.add_argument("--json", default="")
    ap.add_argument("--only", type=int, default=-1)
    ap.add_argument("--raw", action="store_true", help="judge the raw table, without the J7 closing quads")
    ap.add_argument("--parity", action="store_true", help="slow ray-parity measure (not discriminating: every shipped piece scores high)")
    ap.add_argument("--header", default=PA.DEFAULT_HEADER)
    a = ap.parse_args()
    pfs = PA.parse_header(a.header)
    cq, cidx, cex = load_close()
    rows = []
    print("%-10s %5s %5s %5s %8s %8s" % ("prefab", "faces", "wallS", "dangl", "dangl_h", "freeEdge") + " %8s %3s %5s %7s" % ("maxFreeH", "cq", "tallD", "tallDh"))
    for i, pf in enumerate(pfs):
        if a.only >= 0 and i != a.only:
            continue
        fl, tris = faces_tris(pf)
        nclose = 0
        if not a.raw and i < len(cidx) and cidx[i][1] > 0:
            for (pg, poly) in cq[cidx[i][0]:cidx[i][0] + cidx[i][1]]:
                fl.append((pg, poly))
                nclose += 1
        ys = [v[1] for v in pf["verts"]]
        ymin = min(ys)
        base = [(v[0], v[2]) for v in pf["verts"] if v[1] <= ymin + 20]
        hull = hull2d(base)
        if a.parity:
            total, odd, where = parity(tris, ymin, hull)
        else:
            total = odd = 0
        free, ln = boundary_edges(fl, ymin)
        nseg, dang, dh, wfree = wall_closure(fl, ymin)
        roof_m2 = 0.0
        pct = 100.0 * odd / max(1, total)
        maxh = max([h / U for (_e, _y0, h) in wfree], default=0.0)
        tall = sum(1 for (_e, _y0, h) in wfree if h / U >= TALL_M)
        tallh = sum(h / U for (_e, _y0, h) in wfree if h / U >= TALL_M)
        rows.append(dict(i=i, name=pf["name"], faces=len(fl), close_quads=nclose, wall_segs=nseg, dangling=dang,
                         max_free_edge_m=round(maxh, 1), tall_dangling=tall, tall_dangling_h_m=round(tallh, 1), excluded=(cex[i] if i < len(cex) else 0),
                         dangling_h_m=round(dh, 1), free_edge_m=round(ln, 1),
                         odd_pct=round(pct, 2), height_m=round(pf["height"] / U, 1),
                         fp_m=[round(pf["fx"] / U, 1), round(pf["fz"] / U, 1)]))
        print("%-10s %5d %5d %5d %8.1f %8.1f %8.1f %3d %5d %7.1f" % (pf["name"], len(fl), nseg, dang, dh, ln, maxh, nclose, tall, tallh), flush=True)
        if a.png:
            render(pf, fl, [((e[0], y0, e[1]), (e[0], y0 + h, e[1])) for (e, y0, h) in wfree],
                   os.path.join(a.png, "prefab_%02d.png" % i), dang)
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
