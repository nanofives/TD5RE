#!/usr/bin/env python3
"""roam F9b: compare two roam_rebase_trace.csv files (TD5RE_ROAM_REBASE_TRACE=1), e.g. rebase off vs on.

  python -I verify/roam/f9b_rebase_cmp.py <a.csv> [<b.csv>]

One file: continuity check. Every tick's PLACE-frame step (place_x/z, exact: world/256 + origin) must
be what the velocity says (|step - v/256| small); a rebase tick that teleports the car shows up as a
step far larger than its velocity. Prints the rebases, the worst step on a rebase tick and elsewhere.
Two files: tick-by-tick place-frame difference (max, and the first tick where it exceeds 1 unit).
"""
import csv, math, sys


def load(p):
    with open(p, newline="") as f:
        return [{k: float(v) for k, v in r.items()} for r in csv.DictReader(f)]


def continuity(rows, name):
    worst_rb = worst = 0.0
    nrb = 0
    span = None
    for a, b in zip(rows, rows[1:]):
        if b["tick"] != a["tick"] + 1:
            continue
        sx, sz = b["place_x"] - a["place_x"], b["place_z"] - a["place_z"]
        vx, vz = a["vx"] / 256.0, a["vz"] / 256.0       # units per tick
        dev = math.hypot(sx - vx, sz - vz) / 430.0        # metres off what the velocity says
        if b["rebased"]:
            nrb += 1
            worst_rb = max(worst_rb, dev)
        else:
            worst = max(worst, dev)
    mx = max(max(abs(r["world_x_fp"]), abs(r["world_z_fp"])) for r in rows) / 256.0
    first, last = rows[0], rows[-1]
    dist = math.hypot(last["place_x"] - first["place_x"], last["place_z"] - first["place_z"]) / 430.0
    print("%s: %d ticks, %d rebase tick(s), worst step deviation on a rebase tick %.3f m, elsewhere %.3f m, "
          "max |world| %.0f units (%.0f m), start->end %.0f m" % (name, len(rows), nrb, worst_rb, worst, mx, mx / 430.0, dist))
    return worst_rb


def compare(ra, rb):
    b = {int(r["tick"]): r for r in rb}
    mx, first = 0.0, None
    n = 0
    for r in ra:
        o = b.get(int(r["tick"]))
        if not o:
            continue
        n += 1
        d = math.hypot(r["place_x"] - o["place_x"], r["place_z"] - o["place_z"])
        if d > mx:
            mx = d
        if first is None and d > 1.0:
            first = int(r["tick"])
    print("common ticks %d: max place-frame difference %.4f units (%.4f m); first tick > 1 unit: %s"
          % (n, mx, mx / 430.0, first))


def main():
    a = load(sys.argv[1])
    continuity(a, sys.argv[1])
    if len(sys.argv) > 2:
        b = load(sys.argv[2])
        continuity(b, sys.argv[2])
        compare(a, b)


if __name__ == "__main__":
    main()
