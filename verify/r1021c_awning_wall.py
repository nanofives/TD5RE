#!/usr/bin/env python3
"""[R1021 C item 14] The p505 prop Mariano flagged is an AWNING (TG_INFRA_CANOPY, page R9_INFRA 497 + 8): a
canopy hangs from a shop front, so one with no wall behind it floats in the air.  For every canopy on the
level, the plan distance from its centre to the nearest building wall (procedural frontage / real footprint /
set piece) and how many have none within --reach metres.
  python verify/r1021c_awning_wall.py LEVELDIR [--reach 4.0] [--mesh e,s ...]"""
import argparse, collections, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
UPM = 430.0
def main():
    ap = argparse.ArgumentParser(); ap.add_argument("level"); ap.add_argument("--reach", type=float, default=4.0)
    ap.add_argument("--mesh", nargs="*", default=[]); ap.add_argument("--list", type=int, default=0)
    a = ap.parse_args()
    b, meshes = A.load_level(a.level)
    g = A.Grid(10.0 * UPM)
    for m in meshes:
        if A.classify(m) in ("real", "proc", "prefab") or (m["kind"] == "city" and len(m["cmds"]) == 1 and 273 <= m["cmds"][0][0] <= 392):
            for typ, page, vs in A.faces(b, m):
                ys = [v[1] for v in vs]
                if max(ys) - min(ys) < 200.0:
                    continue                     # roof / floor
                lo = sorted(vs, key=lambda v: v[1])[:2]
                for k in range(0, 6):
                    t = k / 5.0
                    p = (lo[0][0] + (lo[1][0] - lo[0][0]) * t, lo[0][2] + (lo[1][2] - lo[0][2]) * t)
                    g.add(p, [p[0]], [p[1]])
    rows = []
    for m in meshes:
        if m["kind"] != "prop" or m["cmds"][0][0] != 505:
            continue
        vs = A.verts(b, m)
        cx = sum(v[0] for v in vs) / len(vs); cz = sum(v[2] for v in vs) / len(vs)
        best = 1e30
        for p in g.near(cx - 12 * UPM, cx + 12 * UPM, cz - 12 * UPM, cz + 12 * UPM):
            best = min(best, math.hypot(p[0] - cx, p[1] - cz))
        rows.append((best / UPM, m["e"], m["s"], round(cx), round(cz)))
    far = [r for r in rows if r[0] > a.reach]
    print("%d awning (p505) meshes, %d with no building wall within %.1f m" % (len(rows), len(far), a.reach))
    for r in sorted(far, reverse=True)[:a.list]:
        print("  e%d s%d at (%d,%d): nearest wall %.1f m" % (r[1], r[2], r[3], r[4], r[0]))
    for e_s in a.mesh:
        e, s = map(int, e_s.split(","))
        for r in rows:
            if (r[1], r[2]) == (e, s): print("  mesh e%d s%d: nearest wall %.1f m" % (e, s, r[0]))
main()
