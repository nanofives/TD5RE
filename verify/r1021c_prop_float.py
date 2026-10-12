#!/usr/bin/env python3
"""[R1021 C item 14] Which PROP meshes (MESHTAG kind prop) float above the ground they stand on?
  python verify/r1021c_prop_float.py LEVELDIR [--min 1.0] [--mesh e,s ...]
Ground = the highest horizontal-ish face (terrain, skirt, road, branch road, pavement/city, block,
cross, decal) under the prop's lowest vertex that is not above it by more than 1.5 m.
Floating = prop bottom minus that ground, metres.  Reads MODELS.DAT + MESHTAG.BIN only."""
import argparse, collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
UPM = 430.0
def main():
    ap = argparse.ArgumentParser(); ap.add_argument("level"); ap.add_argument("--min", type=float, default=1.0)
    ap.add_argument("--mesh", nargs="*", default=[]); ap.add_argument("--list", type=int, default=0)
    a = ap.parse_args()
    b, meshes = A.load_level(a.level)
    ground = A.Grid(8.0 * UPM)
    for m in meshes:
        if m["kind"] in ("terrain", "skirt", "road", "branchroad", "city", "block", "cross", "decal", "deck", "branchside"):
            if A.classify(m) in ("real", "proc", "prefab"):
                continue
            for typ, page, vs in A.faces(b, m):
                if abs(A.poly_area([(v[0], v[2]) for v in vs])) < 1.0:
                    continue
                ys = [v[1] for v in vs]
                if max(ys) - min(ys) > 300.0 and m["kind"] in ("city", "block", "cross"):
                    continue                     # kerb faces / walls
                tris = [vs[0:3]] if len(vs) == 3 else [vs[0:3], [vs[0], vs[2], vs[3]]]
                for T in tris:
                    ground.add(tuple(T), [q[0] for q in T], [q[2] for q in T])
    def gy(px, pz, ref):
        best = None
        for T in ground.near(px, px, pz, pz):
            if A.point_in_tri(px, pz, (T[0][0], T[0][2]), (T[1][0], T[1][2]), (T[2][0], T[2][2])):
                y = A.tri_y(px, pz, T[0], T[1], T[2])
                if y is not None and y <= ref + 1.5 * UPM and (best is None or y > best):
                    best = y
        return best
    rows = []; tot = 0
    for m in meshes:
        if m["kind"] != "prop":
            continue
        tot += 1
        vs = A.verts(b, m)
        ymin = min(v[1] for v in vs)
        low = [v for v in vs if abs(v[1] - ymin) < 60.0]
        g = [gy(v[0], v[2], ymin) for v in low]
        g = [x for x in g if x is not None]
        if not g:
            continue
        fl = (ymin - max(g)) / UPM
        rows.append((fl, m["e"], m["s"], m["cmds"][0][0], round(sum(v[0] for v in vs) / len(vs)), round(sum(v[2] for v in vs) / len(vs))))
    fl = [r for r in rows if r[0] > a.min]
    print("%d prop meshes, %d measured, %d float more than %.1f m above ground; by page: %s"
          % (tot, len(rows), len(fl), a.min, dict(collections.Counter(r[3] for r in fl))))
    for r in sorted(fl, reverse=True)[:a.list]:
        print("  e%d s%d p%d float %.1f m at (%d,%d)" % (r[1], r[2], r[3], r[0], r[4], r[5]))
    for e_s in a.mesh:
        e, s = map(int, e_s.split(","))
        for r in rows:
            if (r[1], r[2]) == (e, s): print("  mesh e%d s%d p%d: floats %.2f m" % (e, s, r[3], r[0]))
main()
