"""[R1015 C items 14+15] Which generated meshes stand inside a plaza polygon or on a landmark cluster?
  python verify/r1015c_plaza_cover.py LEVEL [--json out.json] [--mesh e,s ...]
Plaza = AREAS.JSON kinds the generator treats as plaza (park/common/garden/village_green/recreation/
grass/pitch/playground/...) -- the same set as td5_geob_area_is_plaza. A mesh counts as INSIDE when its
plan centroid is inside the polygon OR >= half of its distinct plan vertices are.
Landmark clusters: BUILDINGS.JSON footprints with landmark+worship tags plus the building:parts inside them;
the mesh is OVERLAPPING when its plan centroid is inside the convex hull of a cluster pushed out by --apron m.
Reported per mesh kind (building, cross, city-real, prefab, block, prop, ...).
"""
import argparse, collections, json, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
UPM = 430.0
PLAZA_KINDS = {"park", "common", "garden", "village_green", "recreation_ground", "grass", "meadow",
               "grassland", "pitch", "playground", "nature_reserve", "picnic_site", "cemetery",
               "track", "stadium", "sports_centre"}

def pip(poly, x, z):
    inside = False
    n = len(poly)
    j = n - 1
    for i in range(n):
        xi, zi = poly[i]; xj, zj = poly[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi) + xi:
            inside = not inside
        j = i
    return inside

def hull(pts):
    pts = sorted(set(pts))
    if len(pts) < 3: return pts
    def cr(o, a, b): return (a[0]-o[0])*(b[1]-o[1]) - (a[1]-o[1])*(b[0]-o[0])
    lo = []
    for p in pts:
        while len(lo) >= 2 and cr(lo[-2], lo[-1], p) <= 0: lo.pop()
        lo.append(p)
    up = []
    for p in reversed(pts):
        while len(up) >= 2 and cr(up[-2], up[-1], p) <= 0: up.pop()
        up.append(p)
    return lo[:-1] + up[:-1]

def dist_to_poly(h, x, z):
    """0 inside a convex CCW hull, else distance to its boundary."""
    n = len(h); inside = True; best = 1e30
    for i in range(n):
        p, q = h[i], h[(i + 1) % n]
        dx, dz = q[0] - p[0], q[1] - p[1]
        L2 = dx * dx + dz * dz
        t = max(0, min(1, ((x - p[0]) * dx + (z - p[1]) * dz) / L2)) if L2 else 0
        best = min(best, math.hypot(x - (p[0] + t * dx), z - (p[1] + t * dz)))
        if dx * (z - p[1]) - dz * (x - p[0]) < 0: inside = False
    return 0.0 if inside else best

def mesh_class(m):
    c = A.classify(m)
    if c: return c
    return m["kind"]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level"); ap.add_argument("--json")
    ap.add_argument("--route", default="re/assets/geo/la_plata/_route")
    ap.add_argument("--apron", type=float, default=12.0, help="metres pushed out of the cluster hull")
    ap.add_argument("--forecourt", type=float, default=0.0, help="second, wider ring (m) reported separately")
    ap.add_argument("--mesh", nargs="*", default=[])
    ap.add_argument("--major", action="store_true", help="only clusters that own building:parts (the cathedral)")
    a = ap.parse_args()
    areas = json.load(open(os.path.join(a.route, "AREAS.JSON"), encoding="utf-8"))["areas"]
    plz = []
    for ar in areas:
        if (ar.get("kind") in PLAZA_KINDS) and ar.get("points"):
            pts = [(p["x"], p["z"]) for p in ar["points"]]
            xs = [p[0] for p in pts]; zs = [p[1] for p in pts]
            plz.append((ar.get("name") or ar.get("kind"), ar["id"], pts, min(xs), max(xs), min(zs), max(zs)))
    blds = json.load(open(os.path.join(a.route, "BUILDINGS.JSON"), encoding="utf-8"))["buildings"]
    anchors = [b for b in blds if b.get("landmark") and not b.get("part") and
               (str(b.get("religion") or "") or "cathedral" in json.dumps(b.get("tags") or {}).lower()
                or b.get("amenity") == "place_of_worship")]
    clusters = []
    for an in anchors:
        ring = [(p["x"], p["z"]) for p in an["points"]]
        if len(ring) < 3: continue
        parts = []
        for b in blds:
            if b is an or not b.get("points"): continue
            cx = sum(p["x"] for p in b["points"]) / len(b["points"]); cz = sum(p["z"] for p in b["points"]) / len(b["points"])
            if b.get("part") and pip(ring, cx, cz):
                parts += [(p["x"], p["z"]) for p in b["points"]]
        hh = hull(ring + parts)
        clusters.append((an.get("name") or an["id"], an["id"], hh, ring, len(parts) > 0))
    print("plaza polygons: %d   worship landmark clusters: %d (%s)" % (len(plz), len(clusters), ", ".join(str(c[0]) for c in clusters)))
    b, meshes = A.load_level(a.level)
    stat = collections.defaultdict(lambda: collections.Counter())
    hits = collections.defaultdict(list)
    for m in meshes:
        cls = mesh_class(m)
        if cls in ("road", "branchroad", "terrain", "skirt", "decal", "water", "coast", "deck", "rail", "other",
                   "flora", "parktree", "tunnel", "gantry", "endwall"):
            continue
        vs = A.verts(b, m)
        pl = list(set((round(v[0]), round(v[2])) for v in vs))
        cx = sum(p[0] for p in pl) / len(pl); cz = sum(p[1] for p in pl) / len(pl)
        stat[cls]["total"] += 1
        # plaza
        inn = None
        for (nm, pid, pts, x0, x1, z0, z1) in plz:
            if cx < x0 - 50 or cx > x1 + 50 or cz < z0 - 50 or cz > z1 + 50: continue
            nin = sum(1 for p in pl if pip(pts, p[0], p[1]))
            if pip(pts, cx, cz) or nin * 2 >= len(pl):
                inn = nm; break
        if inn:
            stat[cls]["in_plaza"] += 1; hits[cls].append(("plaza:%s" % inn, m["e"], m["s"], round(cx), round(cz)))
        # landmark
        for (nm, pid, hh, ring, haspart) in clusters:
            if a.major and not haspart: continue
            if pip(ring, cx, cz): break          # the cluster's own mass (outline extrusion / parts)
            d = min(dist_to_poly(hh, p[0], p[1]) for p in pl + [(cx, cz)]) / UPM
            if d <= a.apron:
                stat[cls]["on_landmark_apron"] += 1; hits[cls].append(("lm:%s d=%.1fm" % (nm, d), m["e"], m["s"], round(cx), round(cz)))
                break
            if a.forecourt and d <= a.forecourt:
                stat[cls]["in_forecourt"] += 1
                hits[cls].append(("lmfc:%s d=%.1fm" % (nm, d), m["e"], m["s"], round(cx), round(cz)))
                break
    print("%-10s %7s %9s %9s %9s" % ("class", "meshes", "in_plaza", "lm_apron", "forecourt"))
    for k, v in sorted(stat.items()):
        print("%-10s %7d %9d %9d %9d" % (k, v["total"], v["in_plaza"], v["on_landmark_apron"], v["in_forecourt"]))
    for e_s in a.mesh:
        e, s = map(int, e_s.split(","))
        for k, lst in hits.items():
            for h in lst:
                if (h[1], h[2]) == (e, s): print("  mesh e%d s%d (%s): %s" % (e, s, k, h[0]))
    if a.json:
        json.dump({k: lst for k, lst in hits.items()}, open(a.json, "w"), indent=0)
main()
