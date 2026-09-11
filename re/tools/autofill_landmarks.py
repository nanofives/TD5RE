#!/usr/bin/env python3
"""Author rotational fills for the landmarks whose symmetry is strong enough to
trust, and prove the result with the exposure metric.

The sweep over level023 showed the symmetry detector earns confidence on only
about a quarter of set pieces: median self-match 0.23, and the LARGEST proposal
counts come from the LEAST symmetric buildings (lm18 scores 0.099 and still
offers 24). Proposal volume is therefore an anti-signal, and the pass gates on
score before it emits anything.

Gates, in order:
  1. landmark symmetry score >= --min-score       (else skip the landmark)
  2. shell is not ground and not on-axis          (a plaza is not radial; an
                                                   on-axis tower rotates onto
                                                   itself)
  3. shell participation >= --min-part            (it has real siblings)
  4. destination is EMPTY                          (occupancy test -- the
                                                   per-face non-match test
                                                   alone happily proposes
                                                   filling an occupied slot)

Every emitted face is a real face of a real shell, rotated: in-format, and
texture-correct by construction. Exposure is measured before and after; a
landmark whose exposure does not improve is rolled back.

    python re/tools/autofill_landmarks.py --level 23 [--apply] [--min-score 0.40]
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


def _fills_path():
    return os.path.join(HERE, "..", "assets", "library", "authored_fills.json")


UNIT = "prim"   # propose on the ORIGINAL authoring units, not welded shells


def shell_faces(T, c):
    return [f for f in T["F"] if f[UNIT] == c]


def occupancy(T, c, axis, ang, tol=400.0):
    """Is the rotated destination already occupied?

    Fraction of the shell's rotated face centroids that land near an existing
    face belonging to a DIFFERENT shell. High means the slot is taken -- usually
    by a sibling whose art differs, which is exactly the case a same-page match
    test misses.
    """
    import td5_holefill as hf
    src = shell_faces(T, c)
    if not src:
        return 1.0
    C = np.array([np.array(f["pos"], float).mean(0) for f in src])
    R = hf._rot(C, axis, ang)
    other = np.array([np.array(f["pos"], float).mean(0)
                      for f in T["F"] if f[UNIT] != c])
    if not len(other):
        return 0.0
    d = np.linalg.norm(other[None, :, :] - R[:, None, :], axis=2).min(axis=1)
    return float((d <= tol).mean())


def rotate_shell(T, c, axis, ang):
    """Emit the whole shell rotated -- a complete volume, not just the faces
    whose counterpart is missing. A partial copy would reproduce the very holes
    we are trying to close."""
    import td5_holefill as hf
    out = []
    for f in shell_faces(T, c):
        P = hf._rot(np.array(f["pos"], float), axis, ang)
        out.append({"page": int(f["page"]), "role": f.get("role") or "wall",
                    "v": [{"pos": [round(float(P[i][0]), 1),
                                   round(float(P[i][1]), 1),
                                   round(float(P[i][2]), 1)],
                           "uv": [round(float(f["uv"][i][0]), 5),
                                  round(float(f["uv"][i][1]), 5)],
                           "light": int(f["light"][i])}
                          for i in range(len(f["pos"]))]})
    return out


def as_prims(faces):
    """Authored faces -> synthetic prims, tris before quads, one per page."""
    from collections import defaultdict
    tris, quads = defaultdict(list), defaultdict(list)
    for f in faces:
        vs = [{"pos": [float(x) for x in v["pos"]],
               "tex": [float(x) for x in v["uv"]],
               "light": int(v["light"]) & 0xFFFFFFFF} for v in f["v"]]
        (tris if len(vs) == 3 else quads)[int(f["page"])].extend(vs)
    out = []
    for page in set(tris) | set(quads):
        nt, nq = len(tris[page]) // 3, len(quads[page]) // 4
        if not (nt or nq):
            continue
        out.append({"role": "wall", "pages": [page], "nface": nt + nq,
                    "mesh": {"vertices": tris[page] + quads[page],
                             "commands": [{"texture_page_id": page,
                                           "tri": nt, "quad": nq}]}})
    return out


def process(level, idx, min_score, min_part, max_occ, rays, unit="prim"):
    global UNIT
    UNIT = unit
    import td5_track_studio as st
    import td5_holefill as hf
    o = st._landmarks(level)[idx]
    T = hf.topology(o["prims"])
    sym = hf.find_axis_and_order(T)
    axis, k, score = sym["axis"], sym["k"], sym["score"]
    res = {"idx": idx, "k": k, "score": round(score, 3), "emitted": 0,
           "accepted": [], "rejected": {}, "skipped": None}
    if score < min_score:
        res["skipped"] = "score %.3f < %.2f" % (score, min_score)
        return res, []

    from dump_landmark_dossier import shell_table
    sh = {s["shell"]: s for s in shell_table(T, axis, k, sym["tol"], unit=UNIT)}
    P = np.array([p for f in T["F"] for p in f["pos"]], float)
    model_r = float(max(np.ptp(P[:, 0]), np.ptp(P[:, 2]))) / 2.0
    lo_xz = (float(P[:, 0].min()), float(P[:, 2].min()))
    hi_xz = (float(P[:, 0].max()), float(P[:, 2].max()))

    rej = {}
    faces = []
    for c, s in sh.items():
        gf = s["roles"].get("ground", 0)
        if gf and gf / max(1, s["faces"]) > 0.2:
            rej[c] = "ground"
            continue
        if s["radius_from_axis"] < 0.12 * model_r:
            rej[c] = "on-axis"
            continue
        pg = s.get("participation_geo", s["participation"])
        if pg < min_part:
            rej[c] = "participation %.2f" % pg
            continue
        for r in range(1, k):
            ang = 2 * math.pi * r / k
            occ = occupancy(T, c, axis, ang)
            if occ > max_occ:
                rej[(c, r)] = "occupied %.2f" % occ
                continue
            add = rotate_shell(T, c, axis, ang)
            # FOOTPRINT CONTAINMENT. A true 1/k rotation of a k-fold symmetric
            # building maps its own footprint onto itself, so a copy that spills
            # outside the model's XZ bbox is evidence the shell is not actually
            # part of the radial group -- typically a MERGED component holding
            # both building and plaza, which no shell-level score can separate.
            # Without this the pass scattered three rotated copies of a
            # plaza/wall blob around the cathedral and still scored well, because
            # piling on geometry lowers backface exposure whether or not it is
            # right.
            Q = np.array([v["pos"] for f in add for v in f["v"]], float)
            pad = 0.02 * model_r
            if (Q[:, 0].min() < lo_xz[0] - pad or Q[:, 0].max() > hi_xz[0] + pad or
                    Q[:, 2].min() < lo_xz[1] - pad or Q[:, 2].max() > hi_xz[1] + pad):
                rej[(c, r)] = "outside footprint"
                continue
            faces.extend(add)
            res["accepted"].append({"shell": c, "turns": r,
                                    "deg": round(360.0 * r / k, 1),
                                    "faces": len(add), "occ": round(occ, 2),
                                    "part": s.get("participation_geo", s["participation"])})
    res["rejected"] = {str(x): y for x, y in rej.items()}
    res["emitted"] = len(faces)
    if not faces:
        return res, []

    before = hf.backface_exposure(hf.triangles(T), **rays)
    T2 = hf.topology(list(o["prims"]) + as_prims(faces))
    after = hf.backface_exposure(hf.triangles(T2), **rays)
    res["exposure_before"] = round(before["frac"], 4)
    res["exposure_after"] = round(after["frac"], 4)
    res["improved"] = after["frac"] < before["frac"] - 1e-4
    if not res["improved"]:
        res["rolled_back"] = True
        return res, []
    return res, faces


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, default=23)
    ap.add_argument("--min-score", type=float, default=0.40)
    ap.add_argument("--min-part", type=float, default=0.35)
    ap.add_argument("--max-occ", type=float, default=0.35)
    ap.add_argument("--only", type=int, default=None)
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    import td5_track_studio as st
    n = len(st._landmarks(args.level))
    rays = {"naz": 16, "nel": 2, "grid": 28}
    todo = [args.only] if args.only is not None else range(n)

    results, fills = [], {}
    for i in todo:
        # Welded shells and original prims are COMPLEMENTARY: per-prim recovers a
        # chapel welded into its plaza, per-shell keeps a multi-prim volume whole.
        # Measured on level023 each wins on different landmarks, so run both and
        # let the referee -- exposure -- choose. Guessing one granularity loses
        # real fills either way.
        best = None
        for u in ("prim", "comp"):
            rr, ff = process(args.level, i, args.min_score, args.min_part,
                             args.max_occ, rays, unit=u)
            rr["unit"] = u
            if ff and (best is None or rr["exposure_after"] < best[0]["exposure_after"]):
                best = (rr, ff)
            if best is None:
                best = (rr, ff)
        r, faces = best
        results.append(r)
        if faces:
            fills["L%d.lm%02d" % (args.level, i)] = faces
        tag = ("[%s] " % r.get("unit", "?")) + (r["skipped"] or
               ("%d faces, exposure %.3f -> %.3f %s"
                % (r["emitted"], r.get("exposure_before", 0),
                   r.get("exposure_after", 0),
                   "OK" if r.get("improved") else "ROLLED BACK"))
               if r["emitted"] else "no proposal survived the gates")
        print("  lm%02d k=%d score %.3f : %s" % (i, r["k"], r["score"], tag))

    good = [r for r in results if r.get("improved")]
    print("\nlandmarks filled %d / %d considered" % (len(good), len(results)))
    if good:
        b = sum(r["exposure_before"] for r in good) / len(good)
        a = sum(r["exposure_after"] for r in good) / len(good)
        print("mean exposure on filled landmarks: %.3f -> %.3f  (%.0f%% better)"
              % (b, a, 100 * (b - a) / b))

    if args.apply and fills:
        path = _fills_path()
        doc = json.load(open(path, encoding="utf-8")) if os.path.isfile(path) else \
            {"_format": "td5_authored_fills", "_version": 1}
        doc.setdefault("fills", {})
        for kk, v in fills.items():
            doc["fills"][kk] = {
                "note": ("AUTO: rotational fill by td5_holefill symmetry detection, "
                         "gated on symmetry score/participation/occupancy and kept "
                         "only because measured backface exposure improved."),
                "faces": v}
        json.dump(doc, open(path, "w", encoding="utf-8"), indent=1)
        print("applied -> %s" % path)
    elif fills:
        print("(dry run -- pass --apply to write authored_fills.json)")


if __name__ == "__main__":
    main()
