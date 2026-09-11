#!/usr/bin/env python3
"""Alpha-shape envelope reconstruction for landmark set pieces.

Why this and not the earlier passes. Measured on L23.lm00, 36% of the silhouette
is see-through, and neither rotational copies (0.358) nor thickening (0.355) nor
same-surface edge bridging (0.293) closes it. Edge bridging can only span a gap
where two FACING EDGES OF THE SAME SURFACE exist; the remaining gaps are regions
where a surface was never authored at all, so there is no partner edge to find.

An alpha shape is closed BY CONSTRUCTION: take the Delaunay tetrahedralisation of
the vertices, keep the tetrahedra whose circumradius is below alpha, and the
boundary of what remains is a watertight surface that hugs the geometry where it
is dense and spans it where it is sparse. The shipped faces are kept exactly as
they are; reconstructed faces are emitted ONLY where nothing already covers that
area, and each takes its page and UVs from the nearest real face.

alpha is the one real knob -- too small leaves holes, too large swallows detail
into a blob -- so it is swept per landmark against the see-through metric, with
a render as the veto. Three times in this work a metric happily rewarded
geometry that was visibly wrong; the number is necessary, never sufficient.
"""
import math

import numpy as np


def _circumradius(P):
    """Circumradius of each tetrahedron in an Mx4x3 array."""
    a = P[:, 0]
    b = P[:, 1] - a
    c = P[:, 2] - a
    d = P[:, 3] - a
    bb = (b * b).sum(1)
    cc = (c * c).sum(1)
    dd = (d * d).sum(1)
    cr = np.cross(c, d)
    denom = 2.0 * np.einsum('ij,ij->i', b, cr)
    with np.errstate(divide='ignore', invalid='ignore'):
        num = (bb[:, None] * cr
               + cc[:, None] * np.cross(d, b)
               + dd[:, None] * np.cross(b, c))
        ctr = num / denom[:, None]
        R = np.linalg.norm(ctr, axis=1)
    R[~np.isfinite(R)] = np.inf
    return R


def alpha_faces(V, alpha):
    """Boundary triangles of the alpha complex over points V (Nx3).

    Returns (tris, ok) where tris is a list of vertex-index triples wound so the
    normal points OUT of the solid, and ok is False when the point set is too
    degenerate to tetrahedralise.
    """
    from scipy.spatial import Delaunay
    from collections import defaultdict
    if len(V) < 4:
        return [], False
    try:
        D = Delaunay(V)
    except Exception:
        return [], False
    simp = D.simplices
    if not len(simp):
        return [], False
    R = _circumradius(V[simp])
    keep = simp[R <= alpha]
    if not len(keep):
        return [], False

    count = defaultdict(list)
    for t in keep:
        for drop in range(4):
            face = tuple(sorted(int(t[k]) for k in range(4) if k != drop))
            count[face].append((t, int(t[drop])))
    out = []
    for face, uses in count.items():
        if len(uses) != 1:
            continue
        _t, opp = uses[0]
        i, j, k = face
        n = np.cross(V[j] - V[i], V[k] - V[i])
        # point away from the tetra's fourth vertex, i.e. out of the solid
        if float(np.dot(n, V[opp] - V[i])) > 0:
            i, k = k, i
        out.append((i, j, k))
    return out, True


def _covered(tri_pts, F_cen, F_nrm, F_span, tol_dist=90.0, tol_dot=0.80):
    """Is this reconstructed triangle already covered by shipped art?"""
    c = tri_pts.mean(0)
    n = np.cross(tri_pts[1] - tri_pts[0], tri_pts[2] - tri_pts[0])
    L = np.linalg.norm(n)
    if L < 1e-9:
        return True
    n = n / L
    d = np.linalg.norm(F_cen - c, axis=1)
    near = d < np.maximum(F_span, tol_dist)
    if not near.any():
        return False
    par = np.abs(F_nrm[near] @ n) > tol_dot
    if not par.any():
        return False
    off = np.abs(np.einsum('ij,j->i', F_cen[near][par] - c, n))
    return bool((off < tol_dist).any())


def envelope(T, alpha_frac=0.12, keep_roles=("wall",), cover_tol=90.0):
    """Reconstruct the missing envelope of a landmark.

    Ground/plaza faces are excluded from the point set by default: they are flat
    slabs lying away from the building, and including them drags the alpha shape
    out into a tent over the plaza.
    """
    import td5_geomlib as gl

    F = [f for f in T["F"] if (not keep_roles or f.get("role") in keep_roles)]
    if len(F) < 4:
        return [], 0.0
    pts = np.array([p for f in F for p in f["pos"]], float)
    # de-duplicate to a lattice so Delaunay is not fed coincident points
    key = np.round(pts).astype(np.int64)
    _, idx = np.unique(key, axis=0, return_index=True)
    V = pts[np.sort(idx)]
    span = float(np.max(V.max(0) - V.min(0)))
    alpha = alpha_frac * span

    tris, ok = alpha_faces(V, alpha)
    if not ok:
        return [], alpha

    F_cen = np.array([np.array(f["pos"], float).mean(0) for f in F])
    F_nrm = []
    F_span = []
    for f in F:
        P = np.array(f["pos"], float)
        n = np.cross(P[1] - P[0], P[2] - P[0])
        L = np.linalg.norm(n)
        F_nrm.append(n / L if L > 1e-9 else np.zeros(3))
        F_span.append(float(np.max(np.linalg.norm(P - P.mean(0), axis=1))))
    F_nrm = np.array(F_nrm)
    F_span = np.array(F_span)

    affs = [gl._affine_uv([(f["pos"][i], f["uv"][i]) for i in range(len(f["pos"]))])
            for f in F]

    out = []
    for (i, j, k) in tris:
        P = V[[i, j, k]]
        if _covered(P, F_cen, F_nrm, F_span, cover_tol):
            continue
        c = P.mean(0)
        src = int(np.argmin(np.linalg.norm(F_cen - c, axis=1)))
        aff = affs[src]
        if aff is None:
            continue
        uvs = [gl._uv_at(aff, w) for w in P]
        # the art must stay on its page; a reconstructed panel that tiles wildly
        # is the same failure mode that produced facade-textured roof lids
        if any(not (-1.05 <= q <= 2.05) for uv in uvs for q in uv):
            uvs = [(0.5, 0.5)] * 3
        out.append({"page": int(F[src]["page"]), "role": "wall",
                    "v": [{"pos": [round(float(w[0]), 1), round(float(w[1]), 1),
                                   round(float(w[2]), 1)],
                           "uv": [round(float(u[0]), 5), round(float(u[1]), 5)],
                           "light": int(F[src]["light"][0])}
                          for w, u in zip(P, uvs)]})
    return out, alpha
