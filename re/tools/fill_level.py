#!/usr/bin/env python3
"""Run BOTH fill passes over a level and keep, per landmark, whichever result the
exposure metric actually prefers.

The two passes answer different defects and were measured separately, which is
not the same as measuring them together: a rotational fill adds whole volumes,
and those volumes can close an existing rim or open a new one. So the caps are
recomputed on the SYMMETRY-AUGMENTED topology rather than reused from the base.

Four candidates per landmark -- nothing, symmetry only, caps only, symmetry+caps
-- and the winner must beat the shipped geometry or the landmark is left alone.
Cap orientation is likewise chosen by measurement: on L23.lm08 the geometric
"face away from the centroid" rule was backwards and drove exposure 0.459 ->
0.821, while the measured choice gave 0.178 from the very same triangles.

    python re/tools/fill_level.py --level 23 [--apply]
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "track_studio"))
sys.path.insert(0, HERE)

RAYS = {"naz": 16, "nel": 2, "grid": 28}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, default=23)
    ap.add_argument("--min-score", type=float, default=0.40)
    ap.add_argument("--min-part", type=float, default=0.35)
    ap.add_argument("--max-occ", type=float, default=0.35)
    # A pass must EARN its geometry. Eight level023 landmarks "improved" by
    # <=0.005 (lm05 0.504->0.504 for 9 faces), which is cost without benefit on
    # a 1999-era renderer. Require a real margin before writing anything.
    ap.add_argument("--min-gain", type=float, default=0.01)
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    import td5_track_studio as st
    import td5_holefill as hf
    import autofill_landmarks as af

    def expo(prims, extra=None):
        T = hf.topology(list(prims) + (af.as_prims(extra) if extra else []))
        return hf.backface_exposure(hf.triangles(T), **RAYS)["frac"], T

    lms = st._landmarks(args.level)
    fills, rows = {}, []
    for i, o in enumerate(lms):
        base, T0 = expo(o["prims"])
        cands = [("shipped", base, [])]

        sym = []
        for u in ("prim", "comp"):
            r, f = af.process(args.level, i, args.min_score, args.min_part,
                              args.max_occ, RAYS, unit=u)
            if f:
                e, _ = expo(o["prims"], f)
                cands.append(("sym[%s]" % u, e, f))
                if not sym or e < sym[0]:
                    sym = (e, f)

        for lab, fl in (("auto", None), ("out", False), ("in", True)):
            c = hf.cap_planar_rims(T0, flip_all=fl)
            if c:
                e, _ = expo(o["prims"], c)
                cands.append(("cap[%s]" % lab, e, c))

        # caps RECOMPUTED on the symmetry-augmented surface
        if sym:
            _, Ts = expo(o["prims"], sym[1])
            for lab, fl in (("auto", None), ("out", False), ("in", True)):
                c = hf.cap_planar_rims(Ts, flip_all=fl)
                if c:
                    both = list(sym[1]) + list(c)
                    e, _ = expo(o["prims"], both)
                    cands.append(("sym+cap[%s]" % lab, e, both))

        name, best, faces = min(cands, key=lambda c: c[1])
        rows.append((i, base, best, name, len(faces)))
        if faces and best < base - args.min_gain:
            fills["L%d.lm%02d" % (args.level, i)] = faces
            print("  lm%02d %-16s %.3f -> %.3f   %3d faces" % (i, name, base, best, len(faces)))

    imp = [r for r in rows if r[3] != "shipped" and r[2] < r[1] - args.min_gain]
    allb = sum(r[1] for r in rows) / len(rows)
    alla = sum((r[2] if (r[3] != "shipped" and r[2] < r[1] - args.min_gain)
            else r[1]) for r in rows) / len(rows)
    print("\nlandmarks improved : %d of %d" % (len(imp), len(rows)))
    if imp:
        b = sum(r[1] for r in imp) / len(imp)
        a = sum(r[2] for r in imp) / len(imp)
        print("mean exposure on improved : %.3f -> %.3f  (%.0f%% better)"
              % (b, a, 100 * (b - a) / b))
    print("mean exposure LEVEL-WIDE  : %.3f -> %.3f  (%.0f%% better)"
          % (allb, alla, 100 * (allb - alla) / allb))

    if args.apply and fills:
        path = af._fills_path()
        doc = json.load(open(path, encoding="utf-8")) if os.path.isfile(path) else \
            {"_format": "td5_authored_fills", "_version": 1}
        doc.setdefault("fills", {})
        # Drop this level's previously written entries first. Merging alone is a
        # trap: tightening a guard makes a later run emit FEWER landmarks, and the
        # ones it now rejects would otherwise sit in the file forever, still being
        # loaded by the studio. This pass owns its level outright.
        pref = "L%d.lm" % args.level
        for k in [k for k in doc["fills"] if k.startswith(pref) and k not in fills]:
            del doc["fills"][k]
        for k, v in fills.items():
            doc["fills"][k] = {
                "note": ("AUTO (fill_level.py): rotational symmetry fill and/or planar "
                         "rim capping, gated on symmetry score, shell participation, "
                         "destination occupancy and footprint containment. Kept only "
                         "because measured backface exposure improved; cap winding "
                         "chosen by measurement, not by a centroid rule."),
                "faces": v}
        json.dump(doc, open(path, "w", encoding="utf-8"), indent=1)
        print("\napplied %d landmark(s) -> %s" % (len(fills), path))
    elif fills:
        print("\n(dry run -- pass --apply to write)")


if __name__ == "__main__":
    main()
