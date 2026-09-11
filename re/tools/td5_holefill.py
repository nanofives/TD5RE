#!/usr/bin/env python3
"""Hole-fill agent for segmented landmarks: read the geometry TOPOLOGICALLY,
find the boundaries, and close the ones that expose a building's interior.

Why this exists: the shipped set pieces are open single-sided quad shells -- no
backs, no caps, no floors -- so from any angle the original never used you see
straight through one wall into the inside of the far one. `fill_landmark_walls`
bridges FACING FREE-EDGE PAIRS, which only ever closes a slot between two
parallel rims; it cannot close a rim that is the open side of a whole volume.

The model here is a welded surface with a GLOBALLY CONSISTENT winding, from
which holes are derived rather than guessed, plus an objective measure of the
actual complaint -- can you see the interior -- so a fill is judged by
measurement rather than by eye.

    topology(prims)        -> welded verts, faces, edge uses, consistent winding
    boundary_loops(T)      -> the rims, chained and classified
    backface_exposure(...) -> THE METRIC: fraction of camera rays hitting a back
"""
import math
from collections import defaultdict

import numpy as np

WELD = 1.0          # world units; the corpus sits on a ~1-unit lattice


# ---------------------------------------------------------------- topology
def _faces_of(prims, roles=None):
    """Flatten prims to polygons, keeping the page/light/uv each one carries."""
    out = []
    for pi, p in enumerate(prims):
        if roles and p.get("role") not in roles:
            continue
        vs = p["mesh"]["vertices"]
        cur = 0
        for c in p["mesh"]["commands"]:
            tri, quad, page = int(c["tri"]), int(c["quad"]), c["texture_page_id"]
            groups = [[cur + t * 3 + k for k in range(3)] for t in range(tri)]
            qb = cur + tri * 3
            groups += [[qb + q * 4 + k for k in range(4)] for q in range(quad)]
            for g in groups:
                out.append({"prim": pi, "role": p.get("role"), "page": page,
                            "pos": [tuple(vs[k]["pos"]) for k in g],
                            "uv": [tuple(vs[k]["tex"]) for k in g],
                            "light": [int(vs[k]["light"]) & 0xFFFFFFFF for k in g]})
            cur += tri * 3 + quad * 4
    return out


def _newell(P):
    """Unit normal of a polygon, robust to non-planarity."""
    n = np.zeros(3)
    for i in range(len(P)):
        a, b = P[i], P[(i + 1) % len(P)]
        n[0] += (a[1] - b[1]) * (a[2] + b[2])
        n[1] += (a[2] - b[2]) * (a[0] + b[0])
        n[2] += (a[0] - b[0]) * (a[1] + b[1])
    L = np.linalg.norm(n)
    return n / L if L > 1e-9 else n


def _reverse(f):
    f["pos"] = f["pos"][::-1]
    f["uv"] = f["uv"][::-1]
    f["light"] = f["light"][::-1]
    f["vi"] = f["vi"][::-1]


def topology(prims, roles=None):
    """Weld, index, and give every face a winding consistent with its neighbours.

    The corpus is manifold (measured on L23.lm00: 0 edges used by >2 faces), so a
    BFS that flips any neighbour disagreeing across a shared edge terminates with
    ONE consistent orientation per connected component. The remaining global sign
    is settled per component by an AREA-WEIGHTED vote that its normals point away
    from the component centroid. Unlike orienting each face independently against
    the centroid, a vote cannot scatter individual normals on a concave shell --
    that per-face heuristic is what made an earlier gap census unreliable.
    """
    F = _faces_of(prims, roles)
    key = lambda p: (round(p[0] / WELD), round(p[1] / WELD), round(p[2] / WELD))
    vid, verts = {}, []
    for f in F:
        f["vi"] = []
        for p in f["pos"]:
            k = key(p)
            if k not in vid:
                vid[k] = len(verts)
                verts.append(p)
            f["vi"].append(vid[k])

    undirected = defaultdict(list)
    for fi, f in enumerate(F):
        n = len(f["vi"])
        for i in range(n):
            a, b = f["vi"][i], f["vi"][(i + 1) % n]
            if a != b:
                undirected[frozenset((a, b))].append(fi)

    adj = defaultdict(list)
    for e, fs in undirected.items():
        if len(fs) == 2:
            adj[fs[0]].append((fs[1], e))
            adj[fs[1]].append((fs[0], e))

    def dir_of(fi, a, b, flip):
        v = F[fi]["vi"]
        n = len(v)
        for i in range(n):
            x, y = v[i], v[(i + 1) % n]
            if flip[fi]:
                x, y = y, x
            if (x == a and y == b) or (x == b and y == a):
                return (x, y)
        return None

    flip = [False] * len(F)
    seen = [False] * len(F)
    comp = [-1] * len(F)
    ncomp = 0
    for s in range(len(F)):
        if seen[s]:
            continue
        seen[s] = True
        comp[s] = ncomp
        stack = [s]
        while stack:
            cur = stack.pop()
            for (nb, e) in adj[cur]:
                if seen[nb]:
                    continue
                a, b = tuple(e)
                d1 = dir_of(cur, a, b, flip)
                d2 = dir_of(nb, a, b, flip)
                # neighbours agree when they traverse the shared edge OPPOSITELY
                if d1 is not None and d1 == d2:
                    flip[nb] = True
                seen[nb] = True
                comp[nb] = ncomp
                stack.append(nb)
        ncomp += 1

    for fi, f in enumerate(F):
        if flip[fi]:
            _reverse(f)
        f["comp"] = comp[fi]

    V = np.array(verts, float)
    for c in range(ncomp):
        idx = [i for i, f in enumerate(F) if f["comp"] == c]
        pts = np.array([p for i in idx for p in F[i]["pos"]], float)
        C = pts.mean(0)
        vote = 0.0
        for i in idx:
            P = np.array(F[i]["pos"], float)
            vote += float(np.dot(_newell(P), P.mean(0) - C))
        if vote < 0:
            for i in idx:
                _reverse(F[i])

    free = [e for e, fs in undirected.items() if len(fs) == 1]
    return {"V": V, "F": F, "edges": undirected, "free": free, "ncomp": ncomp,
            "nonmanifold": sum(1 for fs in undirected.values() if len(fs) > 2)}


# ------------------------------------------------------------ boundary rims
def boundary_loops(T):
    """Chain free edges into rims and describe each one.

    A rim is reported with its planarity residual because that decides how it can
    be closed: a rim that lies in a plane can be triangulated directly, while one
    that wanders (L23.lm00 has a closed rim spanning 9320 units vertically) is the
    open side of a volume and needs the volume reconstructed, not a cap.
    """
    adj = defaultdict(list)
    for e in T["free"]:
        a, b = tuple(e)
        adj[a].append(b)
        adj[b].append(a)
    V = T["V"]
    seen, loops = set(), []
    for s in adj:
        if s in seen:
            continue
        comp, stack = [], [s]
        while stack:
            n = stack.pop()
            if n in seen:
                continue
            seen.add(n)
            comp.append(n)
            stack.extend(m for m in adj[n] if m not in seen)
        P = V[comp]
        C = P.mean(0)
        # planarity: smallest singular value of the centred point cloud
        sv = np.linalg.svd(P - C, compute_uv=False)
        resid = float(sv[2] / (sv[0] + 1e-9))
        loops.append({"verts": comp, "n": len(comp),
                      "closed": all(len(adj[n]) == 2 for n in comp),
                      "centre": C, "extent": (P.max(0) - P.min(0)),
                      "planar_resid": resid,
                      "normal": np.linalg.svd(P - C)[2][2]})
    loops.sort(key=lambda l: -l["n"])
    return loops


# ---------------------------------------------------------------- THE METRIC
def triangles(T, extra=()):
    """Outward-oriented triangle soup (fan), for the metric."""
    tris = []
    for f in list(T["F"]) + list(extra):
        P = f["pos"]
        for i in range(1, len(P) - 1):
            tris.append((P[0], P[i], P[i + 1]))
    return np.array(tris, float)


def backface_exposure(tris, naz=24, nel=2, grid=40, seed=0, elev=(0.10, 0.45)):
    """Fraction of camera rays whose NEAREST hit is a BACK face.

    This is the complaint made numeric: a back face nearest to you means you are
    looking at the inside of the building. Rays come from a ring of viewpoints
    around the model plus a raised ring, aimed at a jittered grid over its bbox,
    so the number is comparable between two versions of the same landmark.
    0 means the interior is never visible from outside. Deterministic, no GPU.
    """
    A, B, C = tris[:, 0], tris[:, 1], tris[:, 2]
    e1, e2 = B - A, C - A
    N = np.cross(e1, e2)
    Ln = np.linalg.norm(N, axis=1, keepdims=True)
    Nn = N / np.where(Ln > 1e-9, Ln, 1.0)
    pts = tris.reshape(-1, 3)
    lo, hi = pts.min(0), pts.max(0)
    cen = (lo + hi) / 2.0
    rad = float(np.linalg.norm(hi - lo)) / 2.0
    rng = np.random.default_rng(seed)

    tot = back = 0
    for ei in range(nel):
        ey = cen[1] + elev[ei] * (hi[1] - lo[1]) * 2.0
        for k in range(naz):
            a = 2 * math.pi * k / naz
            O = np.array([cen[0] + 1.9 * rad * math.cos(a), ey,
                          cen[2] + 1.9 * rad * math.sin(a)])
            tgt = lo + rng.random((grid * grid, 3)) * (hi - lo)
            D = tgt - O
            D /= np.linalg.norm(D, axis=1, keepdims=True)
            idx, hit = _cast(O, D, A, e1, e2, e2)
            if not hit.any():
                continue
            d = np.einsum('ij,ij->i', Nn[idx[hit]], D[hit])
            tot += int(hit.sum())
            back += int((d > 0).sum())
    return {"rays": tot, "backface": back, "frac": (back / tot) if tot else 0.0}


def _cast(O, D, A, e1, e2, _unused, chunk=2048):
    """Nearest-hit Moller-Trumbore, vectorised over rays in chunks.

    Shapes are the trap here: tv and qv do NOT depend on the ray, so they stay
    (T,3) and are contracted against the ray directions explicitly rather than
    broadcast -- einsum will not silently expand a leading 1 into R.
    """
    nR = len(D)
    best_t = np.full(nR, np.inf)
    best_i = np.zeros(nR, int)
    tv = O - A                                   # (T,3)
    qv = np.cross(tv, e1)                        # (T,3)
    t_num = np.einsum('tj,tj->t', qv, e2)        # (T,)
    for s in range(0, nR, chunk):
        d = D[s:s + chunk]                       # (r,3)
        pv = np.cross(d[:, None, :], e2[None, :, :])      # (r,T,3)
        det = np.einsum('rtj,tj->rt', pv, e1)
        ok = np.abs(det) > 1e-9
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        u = np.einsum('rtj,tj->rt', pv, tv) * inv
        v = np.einsum('tj,rj->rt', qv, d) * inv
        tt = t_num[None, :] * inv
        good = (ok & (u >= -1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6)
                & (tt > 1e-3))
        tt = np.where(good, tt, np.inf)
        j = np.argmin(tt, axis=1)
        best_t[s:s + chunk] = tt[np.arange(len(d)), j]
        best_i[s:s + chunk] = j
    return best_i, np.isfinite(best_t)


# ----------------------------------------------------------- symmetry: the
# detector that turns "what is missing" from a judgement call into a measurement
def principal_axis(T, prims=None):
    """Vertical axis of the building: the XZ centroid of its TALLEST shell.

    Tallest rather than the whole cloud, because plazas, walls and ground slabs
    drag a global centroid off the tower that the symmetry actually turns about.
    """
    best, axis = -1.0, None
    for c in range(T["ncomp"]):
        P = np.array([p for f in T["F"] if f["comp"] == c for p in f["pos"]], float)
        if len(P) < 6:
            continue
        h = P[:, 1].max() - P[:, 1].min()
        if h > best:
            best, axis = h, P.mean(0)
    if axis is None:
        P = np.array([p for f in T["F"] for p in f["pos"]], float)
        axis = P.mean(0)
    return np.array([axis[0], axis[2]], float)


def _rot(P, axis, ang):
    c, s = math.cos(ang), math.sin(ang)
    x, z = P[:, 0] - axis[0], P[:, 2] - axis[1]
    out = P.copy()
    out[:, 0] = axis[0] + c * x - s * z
    out[:, 2] = axis[1] + s * x + c * z
    return out


def _face_centroids(T):
    return np.array([np.array(f["pos"], float).mean(0) for f in T["F"]])


def symmetry_orders(T, axis, kmax=12, tol=90.0):
    """How well does the model map onto itself under a 1/k turn, for each k?

    Matching is done on FACE CENTROIDS keyed by texture page: a face may only
    match a face using the same page, so a coincidence of position across
    different art does not inflate the score.
    """
    C = _face_centroids(T)
    pages = np.array([f["page"] for f in T["F"]])
    out = []
    for k in range(2, kmax + 1):
        hit = 0
        R = _rot(C, axis, 2 * math.pi / k)
        for i in range(len(C)):
            d = np.linalg.norm(C - R[i], axis=1)
            d = np.where(pages == pages[i], d, np.inf)
            if d.min() <= tol:
                hit += 1
        out.append({"k": k, "match": hit / len(C)})
    return sorted(out, key=lambda r: -r["match"])


def missing_copies(T, axis, k, tol=90.0, min_faces=3):
    """Faces whose rotational COPY is absent -- i.e. the holes, with a source.

    For every face and every 1/k turn, look for an existing same-page face at the
    rotated position. A face with no counterpart is evidence that a whole piece
    was never authored there. Results are grouped by (shell, turn) so the output
    is a short list of PROPOSALS -- "copy shell S by r turns" -- each of which is
    in-format and texture-correct by construction, because it reuses real faces.
    """
    C = _face_centroids(T)
    pages = np.array([f["page"] for f in T["F"]])
    gaps = defaultdict(list)
    for r in range(1, k):
        R = _rot(C, axis, 2 * math.pi * r / k)
        for i in range(len(C)):
            d = np.linalg.norm(C - R[i], axis=1)
            d = np.where(pages == pages[i], d, np.inf)
            if d.min() > tol:
                gaps[(T["F"][i]["comp"], r)].append(i)
    props = []
    for (comp, r), fis in gaps.items():
        if len(fis) < min_faces:
            continue
        P = np.array([p for i in fis for p in T["F"][i]["pos"]], float)
        Q = _rot(P, axis, 2 * math.pi * r / k)
        props.append({"shell": comp, "turns": r, "k": k,
                      "deg": round(360.0 * r / k, 1), "faces": len(fis),
                      "pages": sorted({int(pages[i]) for i in fis}),
                      "src_centre": P.mean(0), "dst_centre": Q.mean(0),
                      "dst_y": (float(Q[:, 1].min()), float(Q[:, 1].max())),
                      "face_idx": fis})
    props.sort(key=lambda p: -p["faces"])
    return props


def _match_score(C, pages, axis, k, tol):
    """Fraction of faces that land on a same-page face under a 1/k turn."""
    R = _rot(C, axis, 2 * math.pi / k)
    D = np.linalg.norm(C[None, :, :] - R[:, None, :], axis=2)
    D = np.where(pages[None, :] == pages[:, None], D, np.inf)
    return float((D.min(axis=1) <= tol).mean())


def find_axis_and_order(T, ks=(2, 3, 4, 5, 6, 8), tol=300.0, span=1600.0):
    """SEARCH for the symmetry axis and order instead of assuming them.

    Two lessons are baked in. The seed is the centroid of the shell reaching the
    HIGHEST point, not the one with the largest vertical extent -- on L23.lm00 the
    tallest-extent shell is a merged base/plaza blob 3370 units off the tower. And
    the tolerance defaults to 300, because the siblings are only APPROXIMATELY
    symmetric: their measured radii spread ~270 units, so a tight tolerance
    rejects true counterparts even with a perfect axis.
    """
    best = (-1.0, None, None)
    tallest, seed = -1e30, None
    for c in range(T["ncomp"]):
        P = np.array([p for f in T["F"] if f["comp"] == c for p in f["pos"]], float)
        if len(P) >= 6 and P[:, 1].max() > tallest:
            tallest, seed = P[:, 1].max(), P.mean(0)
    C = _face_centroids(T)
    pages = np.array([f["page"] for f in T["F"]])
    cur = np.array([seed[0], seed[2]], float)
    for step in (span / 7.0, span / 28.0, span / 110.0):
        grid = [cur + np.array([dx, dz])
                for dx in np.arange(-3, 4) * step
                for dz in np.arange(-3, 4) * step]
        for ax in grid:
            for k in ks:
                s = _match_score(C, pages, ax, k, tol)
                if s > best[0]:
                    best = (s, ax, k)
        cur = best[1]
    return {"score": best[0], "axis": best[1], "k": best[2], "tol": tol}


# ------------------------------------------------------------- planar capping
# The level-wide sweep found 33 of 171 rims planar (L23.lm00 has none, which is
# why capping looked useless from that landmark alone). A planar rim IS a hole in
# a surface -- a missing roof, floor or recess back -- and can be closed exactly.
def rim_cycle(T, loop):
    """Order a rim's vertices into a single closed walk, or None."""
    if not loop["closed"]:
        return None
    adj = defaultdict(list)
    want = set(loop["verts"])
    for e in T["free"]:
        a, b = tuple(e)
        if a in want and b in want:
            adj[a].append(b)
            adj[b].append(a)
    if any(len(v) != 2 for v in adj.values()) or len(adj) != len(want):
        return None
    start = next(iter(adj))
    order, prev, cur = [start], None, start
    while True:
        nxt = [n for n in adj[cur] if n != prev]
        if not nxt:
            return None
        prev, cur = cur, nxt[0]
        if cur == start:
            break
        order.append(cur)
        if len(order) > len(want):
            return None
    return order if len(order) == len(want) else None


def _earclip(P2):
    """Ear-clip a simple polygon given as Nx2, returning index triples."""
    n = len(P2)
    idx = list(range(n))
    area = 0.0
    for i in range(n):
        x1, y1 = P2[i]
        x2, y2 = P2[(i + 1) % n]
        area += x1 * y2 - x2 * y1
    if area < 0:
        idx.reverse()
    out, guard = [], 0
    while len(idx) > 2 and guard < 4 * n:
        guard += 1
        for j in range(len(idx)):
            a, b, c = idx[j - 1], idx[j], idx[(j + 1) % len(idx)]
            A, B, C = P2[a], P2[b], P2[c]
            cross = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0])
            if cross <= 0:
                continue
            bad = False
            for m in idx:
                if m in (a, b, c):
                    continue
                Pm = P2[m]
                d1 = (B[0]-A[0])*(Pm[1]-A[1]) - (B[1]-A[1])*(Pm[0]-A[0])
                d2 = (C[0]-B[0])*(Pm[1]-B[1]) - (C[1]-B[1])*(Pm[0]-B[0])
                d3 = (A[0]-C[0])*(Pm[1]-C[1]) - (A[1]-C[1])*(Pm[0]-C[0])
                if d1 >= 0 and d2 >= 0 and d3 >= 0:
                    bad = True
                    break
            if bad:
                continue
            out.append((a, b, c))
            idx.pop(j)
            break
        else:
            break
    return out


def cap_planar_rims(T, max_resid=0.02, min_verts=3, max_verts=64,
                    flip_all=None):
    """Close planar rims with triangles that inherit the neighbour's page and UVs.

    UVs come from the OWNING face's affine world->uv map rather than being
    invented, the same rule fill_landmark_walls learned the hard way: invented
    UVs put V=0 (the TOP of the page) on the wrong vertex and every patch came
    out mirrored.
    """
    import td5_geomlib as gl
    V = T["V"]
    owner = {}
    for e, fs in T["edges"].items():
        if len(fs) == 1:
            owner[e] = fs[0]
    out = []
    for loop in boundary_loops(T):
        if loop["planar_resid"] > max_resid:
            continue
        cyc = rim_cycle(T, loop)
        if not cyc or not (min_verts <= len(cyc) <= max_verts):
            continue
        src = None
        for i in range(len(cyc)):
            e = frozenset((cyc[i], cyc[(i + 1) % len(cyc)]))
            if e in owner:
                src = T["F"][owner[e]]
                break
        if src is None:
            continue
        aff = gl._affine_uv([(src["pos"][i], src["uv"][i])
                             for i in range(len(src["pos"]))])
        if aff is None:
            continue
        P = V[cyc]
        C = P.mean(0)
        u, s, vt = np.linalg.svd(P - C)
        n = vt[2]
        e1, e2 = vt[0], vt[1]
        P2 = np.stack([(P - C) @ e1, (P - C) @ e2], axis=1)
        tris = _earclip(P2)
        if not tris:
            continue
        # A cap must look like the art it borrows. Measured on L23.lm08, the
        # unguarded pass emitted lids up to 4x the largest real face carrying UVs
        # from -4.05 to 7.79 -- the neighbour's facade tiled a dozen times across
        # a roof -- while every shipped face keeps its UVs inside [0,1]. Exposure
        # LOVED it (0.459 -> 0.178) because a big plane hides a hole no matter
        # what it looks like, which is exactly how this metric is gamed. Reject a
        # cap whose UVs leave the page or whose area dwarfs the real geometry.
        uvs = [gl._uv_at(aff, V[vi]) for vi in cyc]
        if any(not (-0.25 <= c <= 1.25) for uv in uvs for c in uv):
            continue
        area = 0.5 * abs(np.cross(P2[1] - P2[0], P2[2] - P2[0])) if len(P2) > 2 else 0.0
        big = max(0.5 * float(np.linalg.norm(np.cross(
            np.array(f["pos"][1]) - np.array(f["pos"][0]),
            np.array(f["pos"][2]) - np.array(f["pos"][0]))))
            for f in T["F"])
        if area > 1.5 * big:
            continue
        # Orientation is NOT reliably derivable from the model centroid: a rim on
        # a concave or interior surface points the other way, and on L23.lm08 that
        # heuristic drove exposure from 0.459 to 0.821 by facing 56 caps inward.
        # flip_all lets the caller settle it by MEASUREMENT instead of geometry.
        if flip_all is None:
            M = np.array([p for f in T["F"] for p in f["pos"]], float).mean(0)
            flip = np.dot(n, C - M) < 0
        else:
            flip = bool(flip_all)
        for (a, b, c) in tris:
            tri = [cyc[a], cyc[c], cyc[b]] if flip else [cyc[a], cyc[b], cyc[c]]
            vs = []
            for vi in tri:
                w = V[vi]
                uv = gl._uv_at(aff, w)
                vs.append({"pos": [round(float(w[0]), 1), round(float(w[1]), 1),
                                   round(float(w[2]), 1)],
                           "uv": [round(float(uv[0]), 5), round(float(uv[1]), 5)],
                           "light": int(src["light"][0])})
            out.append({"page": int(src["page"]), "role": "wall", "v": vs})
    return out


# ----------------------------------------------------------------- solidify
# Watertightness cannot come from fitting primitives: measured on level023 only
# 42% of 575 prims are lathe/prism-like, and a landmark is closed only if EVERY
# shell closes, so the parametric route yields almost no closed landmarks.
# Thickening is assumption-free -- it turns any open surface into a solid by
# construction, and it keeps the shipped art untouched on the outside.
def solidify(T, thickness=None, unit="prim"):
    """Close every shell by giving it thickness.

    outer = the original faces, untouched.
    inner = the same faces offset along -normal and reversed.
    rim   = a quad band joining outer to inner along every boundary edge.

    Every edge then has exactly two uses, so the result is watertight and the
    free-edge count -- which, unlike backface exposure, cannot be improved by
    piling on geometry -- goes to zero.
    """
    V = T["V"]
    # area-weighted vertex normals over the WELDED indexing, so the inner surface
    # mirrors the outer connectivity exactly (offsetting per face instead would
    # split the inner shell apart and close nothing).
    N = np.zeros_like(V)
    for f in T["F"]:
        P = np.array(f["pos"], float)
        n = _newell(P)
        a = 0.5 * float(np.linalg.norm(np.cross(P[1] - P[0], P[2] - P[0])))
        for vi in f["vi"]:
            N[vi] += n * a
    L = np.linalg.norm(N, axis=1, keepdims=True)
    N = N / np.where(L > 1e-9, L, 1.0)

    if thickness is None:
        ext = V.max(0) - V.min(0)
        thickness = max(15.0, 0.004 * float(np.max(ext)))
    Vi = V - N * thickness

    out = []

    def mk(idxs, uvs, page, light):
        return {"page": int(page), "role": "wall",
                "v": [{"pos": [round(float(p[0]), 1), round(float(p[1]), 1),
                               round(float(p[2]), 1)],
                       "uv": [round(float(u[0]), 5), round(float(u[1]), 5)],
                       "light": int(light)}
                      for p, u in zip(idxs, uvs)]}

    for f in T["F"]:
        pts = [Vi[vi] for vi in f["vi"]][::-1]
        uvs = list(f["uv"])[::-1]
        out.append(mk(pts, uvs, f["page"], f["light"][0]))

    owner = {e: fs[0] for e, fs in T["edges"].items() if len(fs) == 1}
    for e, fi in owner.items():
        a, b = tuple(e)
        f = T["F"][fi]
        try:
            ia, ib = f["vi"].index(a), f["vi"].index(b)
        except ValueError:
            continue
        ua, ub = f["uv"][ia], f["uv"][ib]
        # outer a -> outer b -> inner b -> inner a
        out.append(mk([V[a], V[b], Vi[b], Vi[a]], [ua, ub, ub, ua],
                      f["page"], f["light"][0]))
    return out, float(thickness)


# ------------------------------------------------------- THE CORRECTED METRIC
def _hull2d(P):
    """Monotone-chain convex hull of Nx2 points."""
    pts = sorted(map(tuple, P))
    if len(pts) < 3:
        return np.array(pts)
    def half(ps):
        out = []
        for p in ps:
            while len(out) >= 2:
                (x1, y1), (x2, y2) = out[-2], out[-1]
                if (x2-x1)*(p[1]-y1) - (y2-y1)*(p[0]-x1) <= 0:
                    out.pop()
                else:
                    break
            out.append(p)
        return out
    return np.array(half(pts)[:-1] + half(pts[::-1])[:-1])


def _inside(hull, Q):
    """Point-in-convex-polygon for many points at once (hull is CCW)."""
    ins = np.ones(len(Q), bool)
    n = len(hull)
    for i in range(n):
        a, b = hull[i], hull[(i + 1) % n]
        ins &= ((b[0]-a[0])*(Q[:,1]-a[1]) - (b[1]-a[1])*(Q[:,0]-a[0])) >= -1e-9
    return ins


def silhouette_holes(tris, naz=16, nel=2, grid=48, elev=(0.15, 0.5)):
    """Fraction of the model's OWN SILHOUETTE you can see straight through.

    This is the metric `backface_exposure` should have been. That one counted
    only rays that HIT something, so a ray passing clean through a gap was never
    counted at all -- a building reduced to disconnected slabs scored a perfect
    0.000 while being full of daylight. Here every ray aimed INSIDE the model's
    projected outline is counted, and a ray that hits nothing is a hole.

    Not a perfect ground truth: a genuine courtyard or archway also lets a ray
    through, so a real building never reaches 0. Use it to COMPARE versions of
    the same landmark, and read it alongside free-edge count.
    """
    A, B, C = tris[:, 0], tris[:, 1], tris[:, 2]
    e1, e2 = B - A, C - A
    pts = tris.reshape(-1, 3)
    lo, hi = pts.min(0), pts.max(0)
    cen = (lo + hi) / 2.0
    rad = float(np.linalg.norm(hi - lo)) / 2.0

    seen = miss = 0
    for ei in range(nel):
        ey = cen[1] + elev[ei] * (hi[1] - lo[1]) * 2.0
        for k in range(naz):
            a = 2 * math.pi * k / naz
            O = np.array([cen[0] + 2.2 * rad * math.cos(a), ey,
                          cen[2] + 2.2 * rad * math.sin(a)])
            fwd = cen - O
            fwd /= np.linalg.norm(fwd)
            right = np.cross(fwd, [0.0, 1.0, 0.0])
            right /= np.linalg.norm(right)
            up = np.cross(right, fwd)
            rel = pts - cen
            P2 = np.stack([rel @ right, rel @ up], axis=1)
            hull = _hull2d(P2)
            if len(hull) < 3:
                continue
            gx = np.linspace(P2[:, 0].min(), P2[:, 0].max(), grid)
            gy = np.linspace(P2[:, 1].min(), P2[:, 1].max(), grid)
            G = np.stack(np.meshgrid(gx, gy), -1).reshape(-1, 2)
            G = G[_inside(hull, G)]
            if not len(G):
                continue
            tgt = cen + G[:, :1] * right + G[:, 1:2] * up
            D = tgt - O
            D /= np.linalg.norm(D, axis=1, keepdims=True)
            _, hit = _cast(O, D, A, e1, e2, e2)
            seen += len(G)
            miss += int((~hit).sum())
    return {"rays": seen, "through": miss, "frac": (miss / seen) if seen else 0.0}


# ------------------------------------------------------------- GAP BRIDGING
# What the earlier passes missed. The set pieces are not one broken surface but
# MANY disconnected fragments with daylight between them: measured on L23.lm00,
# 36% of the silhouette is see-through, and neither rotational copies nor
# thickening changed it (0.360 -> 0.355). The gaps live BETWEEN pieces, so they
# are closed by spanning free edge to free edge, taking page and UVs from the
# real face that owns the edge.
def _edge_context(T):
    """Every boundary edge with the context needed to span it."""
    import td5_geomlib as gl
    V = T["V"]
    out = []
    for e, fs in T["edges"].items():
        if len(fs) != 1:
            continue
        f = T["F"][fs[0]]
        a, b = tuple(e)
        try:
            ia, ib = f["vi"].index(a), f["vi"].index(b)
        except ValueError:
            continue
        P = np.array(f["pos"], float)
        n = _newell(P)
        mid = (V[a] + V[b]) / 2.0
        d = V[b] - V[a]
        L = float(np.linalg.norm(d))
        if L < 1.0:
            continue
        out.append({"a": a, "b": b, "ia": ia, "ib": ib, "f": fs[0],
                    "mid": mid, "dir": d / L, "len": L, "n": n,
                    "cen": P.mean(0), "page": int(f["page"]),
                    "role": f.get("role"),
                    "light": int(f["light"][0]),
                    "aff": gl._affine_uv([(f["pos"][i], f["uv"][i])
                                          for i in range(len(f["pos"]))])})
    return out


def bridge_gaps(T, max_gap=None, rounds=8, min_len=40.0,
                par_min=0.20, ratio_min=0.15, nrm_max=0.95, gap_frac=0.60):
    """Span gaps between fragments with quads whose corners are REAL vertices.

    Pairs two boundary edges when they are close, near-parallel and facing each
    other, then emits the quad joining them. UVs come from the owning face's
    affine world->uv map so the new panel continues the neighbour's art instead
    of inventing a mapping. Runs a few rounds because each new face changes which
    edges are still free.
    """
    import td5_geomlib as gl
    V = T["V"]
    pts = np.array([p for f in T["F"] for p in f["pos"]], float)
    span = float(np.max(pts.max(0) - pts.min(0)))
    if max_gap is None:
        max_gap = gap_frac * span

    # which page pairs genuinely share a prim in the shipped art
    co_pages = set()
    from collections import defaultdict as _dd
    _by = _dd(set)
    for f in T["F"]:
        _by[f["prim"]].add(int(f["page"]))
    for ps in _by.values():
        for x in ps:
            for y in ps:
                co_pages.add((x, y))

    out = []
    cur = T
    for _ in range(rounds):
        E = _edge_context(cur)
        if len(E) < 2:
            break
        M = np.array([e["mid"] for e in E])
        used = set()
        cands = []
        for i in range(len(E)):
            d = np.linalg.norm(M - M[i], axis=1)
            for j in np.where((d > 1.0) & (d <= max_gap))[0]:
                if j <= i:
                    continue
                e1, e2 = E[i], E[j]
                if e1["f"] == e2["f"]:
                    continue
                # A bridge may only join edges that plausibly belong to the SAME
                # surface. Without this, relaxing the pairing rules sprayed grass
                # and foliage panels across St Basil's towers -- and the
                # see-through metric APPROVED it (0.360 -> 0.197), because a big
                # panel blocks rays wherever it is. Same role, and pages that
                # actually co-occur on a real prim.
                if e1["role"] != e2["role"]:
                    continue
                if e1["page"] != e2["page"] and                         (e1["page"], e2["page"]) not in co_pages:
                    continue
                par = abs(float(np.dot(e1["dir"], e2["dir"])))
                if par < par_min:
                    continue
                if min(e1["len"], e2["len"]) / max(e1["len"], e2["len"]) < ratio_min:
                    continue
                # the span must run roughly ALONG each owner face, not out of it
                sep = M[j] - M[i]
                sl = float(np.linalg.norm(sep))
                sep = sep / sl
                if abs(float(np.dot(sep, e1["n"]))) > nrm_max:
                    continue
                if abs(float(np.dot(sep, e2["n"]))) > nrm_max:
                    continue
                cands.append((sl / max(1e-6, par), i, j))
        cands.sort()
        added = 0
        for _sc, i, j in cands:
            if i in used or j in used:
                continue
            e1, e2 = E[i], E[j]
            if e1["aff"] is None:
                continue
            A1, B1 = V[e1["a"]], V[e1["b"]]
            if (np.linalg.norm(V[e2["a"]] - A1) + np.linalg.norm(V[e2["b"]] - B1) <=
                    np.linalg.norm(V[e2["b"]] - A1) + np.linalg.norm(V[e2["a"]] - B1)):
                A2, B2 = V[e2["a"]], V[e2["b"]]
            else:
                A2, B2 = V[e2["b"]], V[e2["a"]]
            quad = [A1, B1, B2, A2]
            if min(np.linalg.norm(A2 - A1), np.linalg.norm(B2 - B1)) < min_len:
                continue
            uvs = [gl._uv_at(e1["aff"], w) for w in quad]
            if any(not (-1.05 <= c <= 2.05) for uv in uvs for c in uv):
                continue
            out.append({"page": e1["page"], "role": "wall",
                        "v": [{"pos": [round(float(w[0]), 1), round(float(w[1]), 1),
                                       round(float(w[2]), 1)],
                               "uv": [round(float(u[0]), 5), round(float(u[1]), 5)],
                               "light": e1["light"]}
                              for w, u in zip(quad, uvs)]})
            used.add(i); used.add(j); added += 1
        if not added:
            break
        from autofill_landmarks import as_prims
        cur = topology([{"role": "wall", "pages": [0], "nface": 0,
                         "mesh": {"vertices": [], "commands": []}}] * 0
                       or list(_prims_of(T)) + as_prims(out))
    return out


def _prims_of(T):
    """Rebuild prim-shaped records from a topology (for re-running a round)."""
    from collections import defaultdict
    by = defaultdict(list)
    for f in T["F"]:
        by[(f["prim"], int(f["page"]))].append(f)
    out = []
    for (pi, page), fs in by.items():
        tris = [f for f in fs if len(f["pos"]) == 3]
        quads = [f for f in fs if len(f["pos"]) == 4]
        vs = []
        for f in tris + quads:
            for i in range(len(f["pos"])):
                vs.append({"pos": list(f["pos"][i]), "tex": list(f["uv"][i]),
                           "light": int(f["light"][i])})
        out.append({"role": fs[0].get("role") or "wall", "pages": [page],
                    "nface": len(fs),
                    "mesh": {"vertices": vs,
                             "commands": [{"texture_page_id": page,
                                           "tri": len(tris), "quad": len(quads)}]}})
    return out
