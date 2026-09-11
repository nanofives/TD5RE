#!/usr/bin/env python3
"""Locate and rank the actual see-through gaps in a landmark, so they can be
authored one at a time.

Every automatic pass tried in this work failed for the same reason: the data
carries no signal saying what shape the missing surface had, so anything
synthesised covers the hole without belonging there, and every coverage metric
happily rewards it. What did produce correct geometry was looking at a specific
gap and authoring specific faces for it.

This finds the gaps. It shoots the see-through rays, keeps the ones that pass
clean through the silhouette, takes the midpoint of each one's passage through
the model, and clusters those midpoints. Each cluster is one hole, reported with
its position, size, and the real faces around it -- their pages and their
world->uv maps -- which is the context needed to author a patch that continues
the neighbouring art.

    python re/tools/find_gaps.py --level 23 --idx 0 [--top 12]
"""
import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "track_studio"))
sys.path.insert(0, HERE)

import numpy as np


def gap_points(tris, naz=24, nel=3, grid=56, elev=(0.12, 0.38, 0.62)):
    """Midpoints, inside the model's AABB, of rays that pass clean through."""
    import td5_holefill as hf
    A, B, C = tris[:, 0], tris[:, 1], tris[:, 2]
    e1, e2 = B - A, C - A
    pts = tris.reshape(-1, 3)
    lo, hi = pts.min(0), pts.max(0)
    cen = (lo + hi) / 2.0
    rad = float(np.linalg.norm(hi - lo)) / 2.0
    out = []
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
            hull = hf._hull2d(P2)
            if len(hull) < 3:
                continue
            gx = np.linspace(P2[:, 0].min(), P2[:, 0].max(), grid)
            gy = np.linspace(P2[:, 1].min(), P2[:, 1].max(), grid)
            G = np.stack(np.meshgrid(gx, gy), -1).reshape(-1, 2)
            G = G[hf._inside(hull, G)]
            if not len(G):
                continue
            tgt = cen + G[:, :1] * right + G[:, 1:2] * up
            D = tgt - O
            D /= np.linalg.norm(D, axis=1, keepdims=True)
            _, hit = hf._cast(O, D, A, e1, e2, e2)
            for d in D[~hit]:
                # midpoint of the segment crossing the AABB
                with np.errstate(divide='ignore', invalid='ignore'):
                    t1 = (lo - O) / d
                    t2 = (hi - O) / d
                tmin = np.nanmax(np.minimum(t1, t2))
                tmax = np.nanmin(np.maximum(t1, t2))
                if tmax > tmin > 0:
                    out.append(O + d * (0.5 * (tmin + tmax)))
    return np.array(out) if out else np.zeros((0, 3))


def cluster(P, tol):
    """Greedy distance clustering; returns list of index arrays, largest first."""
    if not len(P):
        return []
    left = np.ones(len(P), bool)
    groups = []
    while left.any():
        i = int(np.argmax(left))
        seed = P[i]
        for _ in range(6):
            d = np.linalg.norm(P - seed, axis=1)
            m = left & (d <= tol)
            if not m.any():
                break
            seed = P[m].mean(0)
        d = np.linalg.norm(P - seed, axis=1)
        m = left & (d <= tol)
        if not m.any():
            left[i] = False
            continue
        groups.append(np.where(m)[0])
        left &= ~m
    groups.sort(key=len, reverse=True)
    return groups


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, default=23)
    ap.add_argument("--idx", type=int, default=0)
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    import td5_track_studio as st
    import td5_holefill as hf
    import td5_geomlib as gl

    o = st._landmarks(args.level)[args.idx]
    prims = list(o["prims"]) + gl.load_authored_fills(
        "L%d.lm%02d" % (args.level, args.idx))
    T = hf.topology(prims)
    tris = hf.triangles(T)
    frac = hf.silhouette_holes(tris)["frac"]
    P = gap_points(tris)
    span = float(np.max(tris.reshape(-1, 3).max(0) - tris.reshape(-1, 3).min(0)))
    groups = cluster(P, tol=0.09 * span)

    F_cen = np.array([np.array(f["pos"], float).mean(0) for f in T["F"]])
    print("L%d.lm%02d  see-through %.3f   %d gap samples   %d clusters"
          % (args.level, args.idx, frac, len(P), len(groups)))
    print(" %3s %6s %9s %-26s %-22s %s"
          % ("#", "rays", "share", "centre (x,y,z)", "size (dx,dy,dz)", "pages nearby"))
    doc = []
    for gi, g in enumerate(groups[:args.top]):
        Q = P[g]
        c = Q.mean(0)
        ext = Q.max(0) - Q.min(0)
        d = np.linalg.norm(F_cen - c, axis=1)
        near = np.argsort(d)[:8]
        pages = []
        for i in near:
            pg = int(T["F"][i]["page"])
            if pg not in pages:
                pages.append(pg)
        print(" %3d %6d %8.1f%% (%7.0f,%7.0f,%7.0f) (%6.0f,%6.0f,%6.0f) %s"
              % (gi, len(g), 100.0 * len(g) / max(1, len(P)),
                 c[0], c[1], c[2], ext[0], ext[1], ext[2], pages[:5]))
        doc.append({"gap": gi, "rays": int(len(g)),
                    "centre": [round(float(v), 1) for v in c],
                    "extent": [round(float(v), 1) for v in ext],
                    "pages_nearby": pages[:5],
                    "nearest_faces": [{"prim": T["F"][i]["prim"],
                                       "page": int(T["F"][i]["page"]),
                                       "centre": [round(float(v), 1) for v in F_cen[i]]}
                                      for i in near[:4]]})
    if args.out:
        json.dump({"id": "L%d.lm%02d" % (args.level, args.idx),
                   "see_through": round(frac, 4), "gaps": doc},
                  open(args.out, "w", encoding="utf-8"), indent=1)
        print("\nwrote %s" % args.out)


if __name__ == "__main__":
    main()
