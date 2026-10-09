#!/usr/bin/env python3
"""geo_bld_heights.py -- ROUND 1014 C item 21: are the SOURCE heights (Overture
height / num_floors, OSM height / levels, Open Buildings raster) what the game
actually builds?  Joins every real-footprint mesh of a generated level
(MODELS.DAT, kind city, roof page 68) to its record in _route/BUILDINGS.JSON by
roof centroid, and compares the built wall height (top - bottom, minus the
0.7 m base sink) against the record's height_m (- min_height_m).

  python verify/geo_bld_heights.py LEVELDIR _route/BUILDINGS.JSON
"""
import json, math, sys, collections, statistics as st
sys.path.insert(0, __file__.rsplit("verify", 1)[0] + "verify")
import geo_bld_audit as A

UPM = 430.0
lvl, bj = sys.argv[1], sys.argv[2]
b, ms = A.load_level(lvl)
recs = json.load(open(bj, encoding="utf-8"))["buildings"]
grid = collections.defaultdict(list)
for r in recs:
    pts = r.get("points") or []
    if not pts:
        continue
    cx = sum(p["x"] for p in pts) / len(pts); cz = sum(p["z"] for p in pts) / len(pts)
    grid[(int(cx // 6000), int(cz // 6000))].append((cx, cz, r))
rows = []
for m in ms:
    if A.classify(m) != "real":
        continue
    fs = list(A.faces(b, m))
    roofs = [vs for t, p, vs in fs if p == 68 and t == "t"]
    if not roofs:
        continue
    xs = [v[0] for vs in roofs for v in vs]; zs = [v[2] for vs in roofs for v in vs]
    cx = (min(xs) + max(xs)) / 2; cz = (min(zs) + max(zs)) / 2
    ys = [v[1] for _t, _p, vs in fs for v in vs]
    built = (max(ys) - min(ys)) / UPM
    best = None
    for gx in range(int(cx // 6000) - 1, int(cx // 6000) + 2):
        for gz in range(int(cz // 6000) - 1, int(cz // 6000) + 2):
            for (rx, rz, r) in grid.get((gx, gz), ()):
                d = math.hypot(rx - cx, rz - cz)
                if best is None or d < best[0]:
                    best = (d, r)
    if not best or best[0] > 4000:
        continue
    r = best[1]
    want = (r.get("height_m") or 0.0) - (r.get("min_height_m") or 0.0)
    rows.append((r.get("height_src"), r.get("footprint_src"), want, built, m["e"], m["s"]))
print("joined %d real-footprint meshes to their source record" % len(rows))
bysrc = collections.defaultdict(list)
for src, fsrc, want, built, e, s in rows:
    bysrc[src].append((want, built))
for src, v in sorted(bysrc.items(), key=lambda t: -len(t[1])):
    err = [abs(bt - 0.7 - w) for w, bt in v]
    ws = sorted(w for w, _ in v); bs = sorted(bt - 0.7 for _, bt in v)
    print("  %-24s n=%4d  source p50 %.1f p90 %.1f max %.1f m | built p50 %.1f p90 %.1f max %.1f m | |built-source| mean %.2f max %.2f"
          % (src, len(v), ws[len(ws)//2], ws[len(ws)*9//10], ws[-1], bs[len(bs)//2], bs[len(bs)*9//10], bs[-1],
             st.mean(err), max(err)))
bad = [(abs(bt - 0.7 - w), e, s, src, w, bt) for src, f, w, bt, e, s in rows if abs(bt - 0.7 - w) > 0.5]
print("  more than 0.5 m off the source: %d of %d" % (len(bad), len(rows)))
for x in sorted(bad, reverse=True)[:6]:
    print("    e%d s%d %s source %.1f built %.1f" % (x[1], x[2], x[3], x[4], x[5] - 0.7))
