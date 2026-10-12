#!/usr/bin/env python3
"""[R1021 C item 11] How many procedural frontage walls and real footprints stand within BAND metres of a
plaza outline (AREAS.JSON kinds the generator treats as a plaza), or inside it?
  python verify/r1021c_plaza_band.py LEVELDIR [--route DIR] [--band 20] [--mesh e,s ...]
A procedural wall counts when the midpoint of any wall base edge is within BAND m of (or inside) a plaza
polygon; a real footprint when its centroid is.  Reads MODELS.DAT + AREAS.JSON only."""
import argparse, collections, json, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
UPM = 430.0
PLAZA_KINDS = {"park", "common", "garden", "village_green", "recreation_ground", "grass", "meadow",
               "grassland", "pitch", "playground", "nature_reserve", "picnic_site", "cemetery",
               "track", "stadium", "sports_centre"}
def pip(poly, x, z):
    ins = False; j = len(poly) - 1
    for i in range(len(poly)):
        xi, zi = poly[i]; xj, zj = poly[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi) + xi: ins = not ins
        j = i
    return ins
def dpoly(poly, x, z):
    best = 1e30
    for i in range(len(poly)):
        x0, z0 = poly[i]; x1, z1 = poly[(i + 1) % len(poly)]
        dx, dz = x1 - x0, z1 - z0; L = dx * dx + dz * dz
        t = 0 if L == 0 else max(0, min(1, ((x - x0) * dx + (z - z0) * dz) / L))
        best = min(best, math.hypot(x - x0 - t * dx, z - z0 - t * dz))
    return best
def main():
    ap = argparse.ArgumentParser(); ap.add_argument("level")
    ap.add_argument("--route", default="re/assets/geo/la_plata_partido/_route"); ap.add_argument("--band", type=float, default=20.0)
    ap.add_argument("--mesh", nargs="*", default=[])
    a = ap.parse_args()
    areas = json.load(open(os.path.join(a.route, "AREAS.JSON"), encoding="utf-8"))["areas"]
    plz = []
    for ar in areas:
        if ar.get("kind") in PLAZA_KINDS and ar.get("points") and len(ar["points"]) >= 3:
            pts = [(p["x"], p["z"]) for p in ar["points"]]
            xs = [p[0] for p in pts]; zs = [p[1] for p in pts]
            plz.append((pts, min(xs), max(xs), min(zs), max(zs)))
    b, meshes = A.load_level(a.level)
    bandu = a.band * UPM
    def nearest(x, z):
        best = 1e30
        for pts, x0, x1, z0, z1 in plz:
            if x < x0 - bandu * 3 or x > x1 + bandu * 3 or z < z0 - bandu * 3 or z > z1 + bandu * 3: continue
            if pip(pts, x, z): return 0.0
            best = min(best, dpoly(pts, x, z))
        return best
    c = collections.Counter(); hit = {}
    for m in meshes:
        cl = A.classify(m)
        if cl == "real":
            vs = A.verts(b, m)
            cx = sum(v[0] for v in vs) / len(vs); cz = sum(v[2] for v in vs) / len(vs)
            d = nearest(cx, cz) / UPM
            c["real"] += 1
            if d <= 0: c["real_inside"] += 1
            if d <= a.band: c["real_within_band"] += 1; hit[(m["e"], m["s"])] = d
        elif cl in ("proc", "prefab"):
            n_in = 0; n_band = 0
            for typ, page, vs in A.faces(b, m):
                if len(vs) != 4: continue
                ys = [v[1] for v in vs]
                if max(ys) - min(ys) < 200.0: continue
                lo = sorted(vs, key=lambda v: v[1])[:2]
                mx = (lo[0][0] + lo[1][0]) / 2; mz = (lo[0][2] + lo[1][2]) / 2
                d = nearest(mx, mz) / UPM
                if d <= 0: n_in += 1
                if d <= a.band: n_band += 1
            c[cl] += 1
            if n_in: c[cl + "_inside"] += 1
            if n_band: c[cl + "_within_band"] += 1; hit[(m["e"], m["s"])] = a.band
    print("plaza polygons %d; band %.0f m" % (len(plz), a.band))
    for k in ("proc", "proc_inside", "proc_within_band", "prefab", "prefab_within_band", "real", "real_inside", "real_within_band"):
        print("  %-20s %d" % (k, c[k]))
    for e_s in a.mesh:
        e, s = map(int, e_s.split(","))
        print("  mesh e%d s%d: %s" % (e, s, ("within band" if (e, s) in hit else "outside band")))
main()
