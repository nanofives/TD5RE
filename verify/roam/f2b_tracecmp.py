#!/usr/bin/env python3
"""Compare two race-trace dirs (race_trace_<module>.csv) ignoring the render-frame
column (wall-clock dependent). Prints per module: rows, identical rows, first diff.

  python verify/roam/f2b_tracecmp.py <dirA> <dirB> [--modules pose,motion,rotation]
"""
import argparse, csv, os, sys


def load(path):
    with open(path, newline="") as f:
        r = list(csv.reader(f))
    if not r:
        return [], []
    hdr = r[0]
    drop = [i for i, h in enumerate(hdr) if h in ("frame", "frame_index", "wall_ms", "time_ms", "fps")]
    keep = [i for i in range(len(hdr)) if i not in drop]
    return [hdr[i] for i in keep], [tuple(row[i] for i in keep if i < len(row)) for row in r[1:]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--modules", default="")
    a = ap.parse_args()
    mods = [m for m in a.modules.split(",") if m]
    files = sorted(f for f in os.listdir(a.a) if f.startswith("race_trace_") and f.endswith(".csv"))
    if mods:
        files = [f for f in files if f[len("race_trace_"):-4] in mods]
    bad = 0
    for f in files:
        pb = os.path.join(a.b, f)
        if not os.path.exists(pb):
            print("%-28s missing in B" % f)
            bad += 1
            continue
        ha, ra = load(os.path.join(a.a, f))
        hb, rb = load(pb)
        n = min(len(ra), len(rb))
        same = sum(1 for i in range(n) if ra[i] == rb[i])
        first = next((i for i in range(n) if ra[i] != rb[i]), None)
        ok = (ha == hb and len(ra) == len(rb) and same == n)
        if not ok:
            bad += 1
        print("%-28s rows %d/%d identical %d %s" % (f, len(ra), len(rb), same, "OK" if ok else "DIFF"))
        if first is not None:
            cols = [ha[k] + ":" + ra[first][k] + "->" + rb[first][k]
                    for k in range(min(len(ra[first]), len(rb[first]))) if ra[first][k] != rb[first][k]]
            print("    first diff row %d (%s): %s" % (first + 1, ",".join(ra[first][:3]), " ".join(cols[:8])))
    print("RESULT", "IDENTICAL" if bad == 0 else "DIFFERENT (%d modules)" % bad)
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
