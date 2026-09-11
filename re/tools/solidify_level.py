#!/usr/bin/env python3
"""Give every landmark on a level thickness, so no surface is a boundary.

Why this and not the parametric route: fitting a lathe/extrusion per shell was
measured on level023 and only 42% of 575 prims fit. A landmark is enclosed only
if EVERY one of its shells closes, so a 42% per-prim fit yields almost no closed
landmarks. Thickening makes no shape assumption at all -- it turns any open
surface into a solid by construction and leaves the shipped art untouched on the
outside.

WATERTIGHTNESS, not backface exposure, is the metric here. Free-edge count is
topological: unlike exposure it cannot be improved by piling on geometry, which
is how the earlier passes flattered themselves. Exposure is reported as a
secondary, independent confirmation.

What this does NOT do: restore structure that was never authored. A building
missing its back wall becomes a closed slab, not a complete building -- the
opening stays an opening, it simply stops showing an interior. Use the symmetry
pass (fill_level.py) for missing volumes; the two are complementary.

    python re/tools/solidify_level.py --level 23 [--apply] [--thickness N]
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "track_studio"))
sys.path.insert(0, HERE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, default=23)
    ap.add_argument("--thickness", type=float, default=None,
                    help="world units; default scales with each landmark")
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    import td5_track_studio as st
    import td5_holefill as hf
    import autofill_landmarks as af

    rays = {"naz": 12, "nel": 2, "grid": 22}
    lms = st._landmarks(args.level)
    fills = {}
    closed = clean = 0
    f0 = f1 = 0
    ex_before = ex_after = 0.0
    worst = []

    for i, o in enumerate(lms):
        T = hf.topology(o["prims"])
        add, th = hf.solidify(T, thickness=args.thickness)
        T2 = hf.topology(list(o["prims"]) + af.as_prims(add))
        e0 = hf.backface_exposure(hf.triangles(T), **rays)["frac"]
        e1 = hf.backface_exposure(hf.triangles(T2), **rays)["frac"]
        free, nm = len(T2["free"]), T2["nonmanifold"]
        f0 += len(T["F"])
        f1 += len(T2["F"])
        ex_before += e0
        ex_after += e1
        if free == 0:
            closed += 1
        if free == 0 and nm == 0:
            clean += 1
        worst.append((e1, i, free, nm))
        fills["L%d.lm%02d" % (args.level, i)] = add
        print("  lm%02d t=%-4.0f free %4d -> %-3d  nonman %-3d  faces %4d -> %-4d  expo %.3f -> %.3f"
              % (i, th, len(T["free"]), free, nm, len(T["F"]), len(T2["F"]), e0, e1))

    n = len(lms)
    print("\n  closed (0 boundary edges) : %d of %d" % (closed, n))
    print("  also clean 2-manifold     : %d of %d" % (clean, n))
    print("  exposure  mean            : %.4f -> %.4f" % (ex_before / n, ex_after / n))
    print("  faces                     : %d -> %d  (%.1fx)" % (f0, f1, f1 / f0))
    worst.sort(reverse=True)
    print("  highest remaining exposure: " +
          ", ".join("lm%02d %.3f" % (w[1], w[0]) for w in worst[:4]))

    if args.apply:
        path = af._fills_path()
        doc = json.load(open(path, encoding="utf-8")) if os.path.isfile(path) else \
            {"_format": "td5_authored_fills", "_version": 1}
        doc.setdefault("fills", {})
        pref = "L%d.lm" % args.level
        for k in [k for k in doc["fills"] if k.startswith(pref) and k not in fills]:
            del doc["fills"][k]
        for k, v in fills.items():
            doc["fills"][k] = {
                "note": ("AUTO (solidify_level.py): the shell given thickness -- outer "
                         "faces untouched, an inward-offset reversed copy, and a quad "
                         "band stitching every boundary edge. Watertight by "
                         "construction (0 free edges), which is why the interior is "
                         "never visible. Does NOT restore unauthored structure."),
                "faces": v}
        json.dump(doc, open(path, "w", encoding="utf-8"), indent=1)
        print("\napplied %d landmark(s) -> %s" % (len(fills), path))
    else:
        print("\n(dry run -- pass --apply to write)")


if __name__ == "__main__":
    main()
