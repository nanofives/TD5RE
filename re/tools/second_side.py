#!/usr/bin/env python3
"""Give a NAMED prim its missing second side, and nothing else.

The shipped set pieces contain single-sided ribbons: L23.lm00 prim 0 is a
Kremlin wall segment of 5 quads, every normal pointing -Z, 12 of its 16 edges
open. Seen from the cathedral side it is not a thin wall, it is absent -- the
face is culled and you look straight through. Prim 3, the raised deck, has the
same problem underneath.

This adds the opposite face plus a rim band, so the ribbon becomes a thin solid:
a wall you can walk round, a deck with an underside. The new faces reuse the
prim's own page and UVs, so the back of a wall is the same masonry as its front.

Deliberately per-prim and driven by a pick. An earlier pass applied the same
operation to every landmark at once and reported success from a metric, while
the actual gaps -- daylight BETWEEN disconnected pieces -- were untouched. This
one only does what it is pointed at.

    python re/tools/second_side.py --idx 0 --prims 0,3 [--thickness 250] [--apply]
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "track_studio"))
sys.path.insert(0, HERE)

import numpy as np


def second_side(T, prims, thickness):
    """Reversed offset copy of the named prims, plus a rim band closing the edge."""
    import td5_holefill as hf
    want = set(prims)
    sel = [i for i, f in enumerate(T["F"]) if f["prim"] in want]
    if not sel:
        return [], {}
    V = T["V"]

    # vertex normals over the SELECTED faces only, on the welded indexing, so the
    # back surface mirrors the front's connectivity instead of splitting apart
    N = np.zeros_like(V)
    for i in sel:
        f = T["F"][i]
        P = np.array(f["pos"], float)
        n = hf._newell(P)
        a = 0.5 * float(np.linalg.norm(np.cross(P[1] - P[0], P[2] - P[0])))
        for vi in f["vi"]:
            N[vi] += n * a
    L = np.linalg.norm(N, axis=1, keepdims=True)
    N = N / np.where(L > 1e-9, L, 1.0)
    Vi = V - N * thickness          # -normal = into the solid, i.e. behind the face

    def mk(pts, uvs, page, light):
        return {"page": int(page), "role": "wall",
                "v": [{"pos": [round(float(p[0]), 1), round(float(p[1]), 1),
                               round(float(p[2]), 1)],
                       "uv": [round(float(u[0]), 5), round(float(u[1]), 5)],
                       "light": int(light)}
                      for p, u in zip(pts, uvs)]}

    out = []
    for i in sel:
        f = T["F"][i]
        out.append(mk([Vi[vi] for vi in f["vi"]][::-1], list(f["uv"])[::-1],
                      f["page"], f["light"][0]))

    # rim: close the edge of the ribbon where it has no neighbour in the SELECTION
    from collections import defaultdict
    use = defaultdict(list)
    for i in sel:
        vi = T["F"][i]["vi"]
        for k in range(len(vi)):
            use[frozenset((vi[k], vi[(k + 1) % len(vi)]))].append(i)
    rim = 0
    for e, fs in use.items():
        if len(fs) != 1:
            continue
        f = T["F"][fs[0]]
        a, b = tuple(e)
        try:
            ia, ib = f["vi"].index(a), f["vi"].index(b)
        except ValueError:
            continue
        out.append(mk([V[a], V[b], Vi[b], Vi[a]],
                      [f["uv"][ia], f["uv"][ib], f["uv"][ib], f["uv"][ia]],
                      f["page"], f["light"][0]))
        rim += 1
    return out, {"front_faces": len(sel), "back_faces": len(sel), "rim_quads": rim,
                 "thickness": thickness}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, default=23)
    ap.add_argument("--idx", type=int, default=0)
    ap.add_argument("--prims", required=True, help="comma-separated prim indices")
    ap.add_argument("--thickness", type=float, default=250.0)
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    import td5_track_studio as st
    import td5_holefill as hf
    import td5_geomlib as gl
    import autofill_landmarks as af

    lm_id = "L%d.lm%02d" % (args.level, args.idx)
    o = st._landmarks(args.level)[args.idx]
    T = hf.topology(o["prims"])           # shipped geometry only
    prims = [int(x) for x in args.prims.split(",") if x.strip() != ""]
    add, info = second_side(T, prims, args.thickness)
    if not add:
        sys.exit("no faces found for prims %s" % prims)

    for p in prims:
        F = [f for f in T["F"] if f["prim"] == p]
        P = np.array([q for f in F for q in f["pos"]], float)
        nrm = np.array([hf._newell(np.array(f["pos"], float)) for f in F])
        print("  prim %-3d %2d faces  pages %s  bbox y %.0f..%.0f  mean normal (%.2f,%.2f,%.2f)"
              % (p, len(F), sorted({int(f["page"]) for f in F}),
                 P[:, 1].min(), P[:, 1].max(), *nrm.mean(0)))
    print("\n  emitting %d faces  (%d back + %d rim)  thickness %.0f"
          % (len(add), info["back_faces"], info["rim_quads"], info["thickness"]))

    keep = gl.load_authored_fills(lm_id)
    T2 = hf.topology(list(o["prims"]) + keep + af.as_prims(add))
    T1 = hf.topology(list(o["prims"]) + keep)
    print("  landmark free edges %d -> %d" % (len(T1["free"]), len(T2["free"])))

    if args.apply:
        path = af._fills_path()
        doc = json.load(open(path, encoding="utf-8"))
        ent = doc.setdefault("fills", {}).setdefault(lm_id, {"note": "", "faces": []})
        ent["faces"] = list(ent.get("faces", [])) + add
        ent["note"] = ((ent.get("note", "") or "").split(" | SECOND-SIDE")[0]
                       + " | SECOND-SIDE: prims %s given their opposite face plus a rim "
                         "band (thickness %.0f) so the single-sided ribbons read as "
                         "solids from both directions." % (prims, args.thickness))
        json.dump(doc, open(path, "w", encoding="utf-8"), indent=1)
        print("\n  applied -> %s  (%s now has %d authored faces)"
              % (path, lm_id, len(ent["faces"])))
    else:
        print("\n  (dry run -- pass --apply to write)")


if __name__ == "__main__":
    main()
