#!/usr/bin/env python3
"""[R1021 C item 11] How many generated buildings (procedural frontage walls, real extrusions, prefabs)
stand on PLAZA LAWN (kind block, page 65 R3_BLOCK, the lawn every plaza/park emitter lays)?
  python verify/r1021c_lawn_under.py LEVELDIR [--mesh e,s ...]
A wall counts when >= 3 of 5 sample points on its base edge are inside a lawn triangle; a real
footprint counts when its centroid or >= half of its hull points are.  Reads MODELS.DAT only."""
import argparse, collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("level"); ap.add_argument("--mesh", nargs="*", default=[])
    a = ap.parse_args()
    b, meshes = A.load_level(a.level)
    cell = 20000.0
    grid = collections.defaultdict(list)
    for m in meshes:
        if m["kind"] != "block":
            continue
        for typ, page, vs in A.faces(b, m):
            if page != 65:
                continue
            tris = [vs[0:3]] if len(vs) == 3 else [vs[0:3], [vs[0], vs[2], vs[3]]]
            for T in tris:
                p = [(v[0], v[2]) for v in T]
                xs = [q[0] for q in p]; zs = [q[1] for q in p]
                for cx in range(int(min(xs) // cell), int(max(xs) // cell) + 1):
                    for cz in range(int(min(zs) // cell), int(max(zs) // cell) + 1):
                        grid[(cx, cz)].append(p)
    def on_lawn(x, z):
        for p in grid.get((int(x // cell), int(z // cell)), []):
            if A.point_in_tri(x, z, p[0], p[1], p[2]):
                return True
        return False
    res = collections.defaultdict(list)
    for m in meshes:
        cl = A.classify(m)
        if not cl:
            continue
        vs = A.verts(b, m)
        if cl == "real":
            pl = sorted(set((v[0], v[2]) for v in vs))
            cx = sum(p[0] for p in pl) / len(pl); cz = sum(p[1] for p in pl) / len(pl)
            pts = pl + [(cx, cz)]
            n = sum(1 for p in pts if on_lawn(*p))
            if on_lawn(cx, cz) or n * 2 >= len(pts):
                res[cl].append((m["e"], m["s"]))
        else:
            hit = 0
            for typ, page, fv in A.faces(b, m):
                if len(fv) != 4: continue
                ys = [v[1] for v in fv]
                if max(ys) - min(ys) < 200.0: continue
                base = sorted(fv, key=lambda v: v[1])[:2]
                nin = 0
                for k in range(1, 6):
                    t = k / 6.0
                    nin += on_lawn(base[0][0] + (base[1][0] - base[0][0]) * t, base[0][2] + (base[1][2] - base[0][2]) * t)
                hit += nin >= 3
            if hit:
                res[cl].append((m["e"], m["s"], hit))
    for k, v in res.items():
        print("%-7s meshes standing on plaza lawn: %d" % (k, len(v)))
    for e_s in a.mesh:
        e, s = map(int, e_s.split(","))
        for k, v in res.items():
            for r in v:
                if (r[0], r[1]) == (e, s): print("  mesh e%d s%d (%s) stands on lawn %s" % (e, s, k, r))
main()
