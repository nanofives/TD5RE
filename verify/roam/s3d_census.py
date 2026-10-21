#!/usr/bin/env python3
"""roam JC: offline census of the OSM Simple 3D Buildings tags in a tiled place.

Reads re/assets/geo/<place>/tiles/BUILDINGS_*.JSON (source frame, x = east, 430 units/m) and prints:
  - building:part count, outlines that hold parts (centroid of a part inside the outline), orphan parts
  - roof:shape histogram (parts and outlines), with how many rings are concave / have collinear vertices
  - min_height / building:min_level / roof:height / colour / material coverage
  - the parts and roof-tagged footprints near a list of named landmarks (--near "x,z,R")
Usage: python verify/roam/s3d_census.py [--place la_plata_partido] [--near 2859879,5683905,120]
"""
import argparse, glob, json, math, os, sys
from collections import Counter

def ring(b):
    p = [(q["x"], q["z"]) for q in b["points"]]
    if len(p) > 1 and p[0] == p[-1]:
        p = p[:-1]
    return p

def cross(o, a, b):
    return (a[0] - o[0]) * (b[1] - a[1]) - (a[1] - o[1]) * (b[0] - a[0])

def concave(p, tol_deg=1.0):
    """1 = a real reflex corner (collinear corners within tol_deg ignored)."""
    n = len(p); pos = neg = 0; col = 0
    for i in range(n):
        a, b, c = p[i], p[(i + 1) % n], p[(i + 2) % n]
        l1 = math.hypot(b[0] - a[0], b[1] - a[1]); l2 = math.hypot(c[0] - b[0], c[1] - b[1])
        if l1 < 1 or l2 < 1: col += 1; continue
        s = cross(a, b, c) / (l1 * l2)
        if abs(s) < math.sin(math.radians(tol_deg)): col += 1; continue
        if s > 0: pos = 1
        else: neg = 1
    return (pos and neg), col

def inside(p, x, z):
    c = False; n = len(p)
    for i in range(n):
        a, b = p[i], p[(i + 1) % n]
        if (a[1] > z) != (b[1] > z):
            if x < a[0] + (z - a[1]) * (b[0] - a[0]) / (b[1] - a[1]): c = not c
    return c

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--place", default="la_plata_partido")
    ap.add_argument("--near", action="append", default=[])
    a = ap.parse_args()
    root = os.path.join(os.path.dirname(__file__), "..", "..", "re", "assets", "geo", a.place, "tiles")
    seen = {}
    for f in sorted(glob.glob(os.path.join(root, "BUILDINGS_*.JSON"))):
        for b in json.load(open(f, encoding="utf-8"))["buildings"]:
            if b["id"] not in seen: seen[b["id"]] = b
    B = list(seen.values())
    parts = [b for b in B if b.get("part")]
    s3d = [b for b in B if b.get("part") or b.get("roof_shape")]
    print("footprints %d (unique ids), building:part %d, roof:shape %d, either %d" %
          (len(B), len(parts), sum(1 for b in B if b.get("roof_shape")), len(s3d)))
    # outlines holding parts: bbox prefilter on OSM outlines near any part
    osm_out = [b for b in B if not b.get("part") and b.get("footprint_src") == "osm"]
    pc = [(sum(x for x, _ in ring(p)) / len(ring(p)), sum(z for _, z in ring(p)) / len(ring(p)), p) for p in parts]
    holders = Counter(); orphan = 0
    for x, z, p in pc:
        host = None
        for o in osm_out:
            r = ring(o)
            xs = [q[0] for q in r]; zs = [q[1] for q in r]
            if min(xs) <= x <= max(xs) and min(zs) <= z <= max(zs) and inside(r, x, z):
                host = o; break
        if host is None: orphan += 1
        else: holders[host["id"]] += 1
    print("outlines holding >=1 part: %d (parts inside them %d), orphan parts (no outline in the cache) %d" %
          (len(holders), sum(holders.values()), orphan))
    for oid, k in holders.most_common():
        o = seen[oid]
        print("   outline %d %r class=%s h=%s roof=%s holds %d part(s)" % (oid, o.get("name"), o.get("class"),
              o.get("height_m"), o.get("roof_shape"), k))
    sh = Counter(); shc = Counter(); colc = 0
    for b in s3d:
        r = ring(b); cc, col = concave(r)
        s = b.get("roof_shape") or "(none)"
        sh[s] += 1
        if cc: shc[s] += 1
        if col: colc += 1
    print("roof:shape over parts+roof-tagged: " + ", ".join("%s %d (concave %d)" % (k, v, shc[k]) for k, v in sh.most_common()))
    print("rings with a collinear (<1 deg) corner: %d of %d" % (colc, len(s3d)))
    def cov(key):
        return sum(1 for b in s3d if b.get(key) not in (None, "", 0))
    for key in ("height_m", "min_height_m", "roof_height_m", "levels", "colour", "roof_colour", "material",
                "roof_material"):
        print("   %-14s %d of %d" % (key, cov(key), len(s3d)))
    tags = Counter()
    for b in s3d:
        for k in (b.get("tags") or {}): tags[k] += 1
    print("raw tags on parts+roof-tagged: " + ", ".join("%s %d" % kv for kv in tags.most_common(30)))
    cls = Counter((b.get("class") or "?") for b in parts)
    print("building:part values: " + ", ".join("%s %d" % kv for kv in cls.most_common()))
    for nr in a.near:
        x, z, R = (float(v) for v in nr.split(","))
        print("near (%g,%g) R %g m:" % (x, z, R))
        for b in s3d:
            r = ring(b); cx = sum(q[0] for q in r) / len(r); cz = sum(q[1] for q in r) / len(r)
            d = math.hypot(cx - x, cz - z) / 430.0
            if d <= R:
                cc, col = concave(r)
                print("   %4.0f m id %d part=%s class=%s roof=%s h=%s rh=%s n=%d concave=%d collinear=%d %s" % (
                    d, b["id"], bool(b.get("part")), b.get("class"), b.get("roof_shape"), b.get("height_m"),
                    b.get("roof_height_m"), len(r), cc, col, b.get("name") or ""))

if __name__ == "__main__":
    main()
