"""F0a follow-up: real casco polygon from the ring avenues, sections 1-5 clipped to it,
and 256 m tiles on a north-up grid vs a grid rotated to the casco axes.

Reuses the loaders of measure.py (loaded by path, because `python -I` does not put
this directory on sys.path). Writes casco_poly.json and appends a section to REPORT.md
(idempotent: an earlier appended section is replaced).

Run:  python -I measure_poly.py
"""
import csv
import importlib.util
import json
import math
import os
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("measure_base", os.path.join(HERE, "measure.py"))
m = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(m)

MARK = "<!-- casco-poly:start -->"
RING = [("Av 31", "Av 32"), ("Av 32", "Av 122"), ("Av 122", "Av 72"), ("Av 72", "Av 31")]
TILE_M = 256


# ---------------------------------------------------------------- geometry ---
def pt_seg(p, a, b):
    """Closest point on segment ab to p -> (dist, qx, qz)."""
    ax, az = a; bx, bz = b; px, pz = p
    dx, dz = bx - ax, bz - az
    L2 = dx * dx + dz * dz
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (pz - az) * dz) / L2))
    qx, qz = ax + t * dx, az + t * dz
    return math.hypot(px - qx, pz - qz), qx, qz


def orient(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def seg_seg(a, b, c, d):
    """(distance, px, pz): a point midway between the closest pair of seg ab and seg cd."""
    o1, o2 = orient(a, b, c), orient(a, b, d)
    o3, o4 = orient(c, d, a), orient(c, d, b)
    if (o1 * o2 < 0) and (o3 * o4 < 0):
        t = o1 / (o1 - o2)
        return 0.0, a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])
    best = None
    for (p, s0, s1, other) in ((a, c, d, None), (b, c, d, None), (c, a, b, None), (d, a, b, None)):
        dist, qx, qz = pt_seg(p, s0, s1)
        if best is None or dist < best[0]:
            best = (dist, p, (qx, qz))
    dist, p, q = best
    return dist, (p[0] + q[0]) / 2, (p[1] + q[1]) / 2


def seg_bbox_gap(a, b, c, d):
    ax0, ax1 = min(a[0], b[0]), max(a[0], b[0])
    az0, az1 = min(a[1], b[1]), max(a[1], b[1])
    cx0, cx1 = min(c[0], d[0]), max(c[0], d[0])
    cz0, cz1 = min(c[1], d[1]), max(c[1], d[1])
    gx = max(0.0, max(ax0, cx0) - min(ax1, cx1))
    gz = max(0.0, max(az0, cz0) - min(az1, cz1))
    return math.hypot(gx, gz)


def pip(poly, x, z):
    """Ray-casting point in polygon (poly = list of (x, z))."""
    inside = False
    n = len(poly)
    j = n - 1
    for i in range(n):
        xi, zi = poly[i]; xj, zj = poly[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi) + xi:
            inside = not inside
        j = i
    return inside


def poly_area(poly):
    s = 0.0
    for i in range(len(poly)):
        x1, z1 = poly[i]; x2, z2 = poly[(i + 1) % len(poly)]
        s += x1 * z2 - x2 * z1
    return abs(s) / 2


def sat_box_poly(box, poly):
    """Does axis-aligned box (u0,u1,v0,v1) overlap convex polygon poly (same frame)?"""
    axes = [(1.0, 0.0), (0.0, 1.0)]
    for i in range(len(poly)):
        x1, z1 = poly[i]; x2, z2 = poly[(i + 1) % len(poly)]
        ex, ez = x2 - x1, z2 - z1
        axes.append((-ez, ex))
    corners = [(box[0], box[2]), (box[1], box[2]), (box[1], box[3]), (box[0], box[3])]
    for ax, az in axes:
        pb = [c[0] * ax + c[1] * az for c in corners]
        pp = [c[0] * ax + c[1] * az for c in poly]
        if max(pb) < min(pp) or max(pp) < min(pb):
            return False
    return True


def make_frame(theta):
    c, s = math.cos(theta), math.sin(theta)

    def frame(x, z):
        return (x * c + z * s, -x * s + z * c)
    return frame


def direction_deg_axial(segs_world):
    """Axial mean direction (mod pi) of segments, length-weighted, returned in radians."""
    s2 = c2 = 0.0
    for (a, b) in segs_world:
        dx, dz = b[0] - a[0], b[1] - a[1]
        L = math.hypot(dx, dz)
        if L == 0:
            continue
        ang = math.atan2(dz, dx)
        s2 += L * math.sin(2 * ang)
        c2 += L * math.cos(2 * ang)
    return 0.5 * math.atan2(s2, c2)


# ----------------------------------------------------------------- polygon ---
def avenue_segments(ways, name_key):
    pat = name_key
    segs = []
    wids = []
    for wid, w in ways.items():
        if (w["name"] or "").lower() and pat(w["name"]):
            wids.append(wid)
            V = w["xz"]
            for i in range(len(V) - 1):
                segs.append((wid, V[i], V[i + 1]))
    return segs, wids


def name_matcher(num):
    import re
    rx = re.compile(r"\b(av\.?|avenida)\s*%s\b" % num, re.I)
    return lambda nm: bool(rx.search(nm))


def find_corner(segA, segB, max_m=60.0):
    """Closest approach between two avenues. Returns sorted candidates (dist, x, z, wa, wb)."""
    cands = []
    best_any = None
    for (wa, a, b) in segA:
        for (wb, c, d) in segB:
            dist, px, pz = seg_seg(a, b, c, d) if seg_bbox_gap(a, b, c, d) <= 2000 * 430 else (None, 0, 0)
            if dist is None:
                continue
            if best_any is None or dist < best_any[0]:
                best_any = (dist, px, pz, wa, wb)
            if dist < max_m * 430:
                cands.append((dist, px, pz, wa, wb))
    if not cands and best_any is not None:
        print("  closest approach %.1f m at (%.6f, %.6f) ways %s / %s"
              % (best_any[0] / 430, *world_to_latlon(P_GLOBAL, best_any[1], best_any[2]),
                 best_any[3], best_any[4]))
    cands.sort()
    return cands


def cluster(cands, gap_m=150.0):
    out = []
    for c in cands:
        for o in out:
            if math.hypot(c[1] - o["x"], c[2] - o["z"]) / m_upm < gap_m:
                break
        else:
            out.append({"x": c[1], "z": c[2], "dist_m": c[0] / m_upm, "ways": (c[3], c[4])})
    return out


m_upm = None
P_GLOBAL = None


def world_to_latlon(P, x, z):
    upm = P["units_per_metre"]
    return (P["lat0"] + z / (upm * P["m_per_deg_lat"]),
            P["lon0"] + x / (upm * P["m_per_deg_lon"]))


# ------------------------------------------------------------------ measure ---
def measure_poly(poly, ways, nodes_deg, node_xz, buildings, sigs, nodes_other, upm):
    km_total = 0.0
    way_in = {}
    seg_in = 0
    for wid, w in ways.items():
        V = w["xz"]
        wkm = 0.0
        any_in = False
        for (x, z) in V:
            if pip(poly, x, z):
                any_in = True
                break
        for i in range(len(V) - 1):
            (xa, za), (xb, zb) = V[i], V[i + 1]
            mx, mz = (xa + xb) / 2, (za + zb) / 2
            if pip(poly, mx, mz):
                seg_in += 1
                wkm += m.seglen_m((xa, za), (xb, zb), upm) / 1000.0
                any_in = True
        if any_in:
            way_in[wid] = wkm
            km_total += wkm
    ways_in = [ways[w] for w in way_in]
    by_class = defaultdict(lambda: [0, 0.0, 0])
    for w in ways_in:
        c = w["class"] or "(none)"
        by_class[c][0] += 1
        by_class[c][1] += way_in[w["id"]]
        by_class[c][2] += 1 if w["oneway"] else 0
    deg_hist = Counter()
    junc_in = 0
    for key, deg in nodes_deg.items():
        if deg < 2:
            continue
        x, z = node_xz[key]
        if not pip(poly, x, z):
            continue
        junc_in += 1
        deg_hist["2" if deg == 2 else ("6+" if deg >= 6 else str(deg))] += 1
    b_in = [b for b in buildings if pip(poly, b["x"], b["z"])]
    s_in = [s for s in sigs if pip(poly, s[0], s[1])]
    n_in = [s for s in nodes_other if pip(poly, s[0], s[1])]
    rings = [w for w in ways_in if w["closed"]]
    roundabouts = [w for w in ways_in if (w["junction"] or "") == "roundabout"]
    bridges = [w for w in ways_in if w["bridge"]]
    tunnels = [w for w in ways_in if w["tunnel"]]
    layered = [w for w in ways_in if w["layer"] != 0]
    return {
        "drivable_km": km_total,
        "segments_in_poly": seg_in,
        "ways_touching_poly": len(ways_in),
        "ways_oneway": sum(1 for w in ways_in if w["oneway"]),
        "class_table": {c: {"ways": v[0], "km": v[1], "oneway": v[2]}
                        for c, v in sorted(by_class.items(), key=lambda kv: -kv[1][1])},
        "junction_nodes_in_poly": junc_in,
        "junction_degree_hist": dict(deg_hist),
        "closed_ways": len(rings),
        "roundabout_ways": len(roundabouts),
        "bridge_ways": len(bridges),
        "tunnel_ways": len(tunnels),
        "layer_nonzero_ways": len(layered),
        "bridge_kinds": dict(Counter(w["bridge_kind"] for w in bridges)),
        "tunnel_kinds": dict(Counter(w["tunnel_kind"] for w in tunnels)),
        "layer_values": dict(Counter(w["layer"] for w in layered)),
        "buildings": len(b_in),
        "building_vertices_raw": sum(b["nverts"] for b in b_in),
        "building_source_bytes": sum(b["bytes"] for b in b_in),
        "signals_traffic": len(s_in),
        "nodes_other_kinds": len(n_in),
    }


def tile_table(frame, poly, ways, nodes_deg, node_xz, buildings, sigs, upm, S_m, csv_path):
    """Tiles on the grid defined by `frame` (north-up = identity), S_m metres square.
    Only tiles that intersect the polygon are listed. Features counted only inside poly."""
    S = S_m * upm
    fpoly = [frame(x, z) for (x, z) in poly]
    us = [p[0] for p in fpoly]; vs = [p[1] for p in fpoly]
    ix0, ix1 = math.floor(min(us) / S), math.floor(max(us) / S)
    iz0, iz1 = math.floor(min(vs) / S), math.floor(max(vs) / S)
    grid = []
    for ix in range(ix0, ix1 + 1):
        for iz in range(iz0, iz1 + 1):
            box = (ix * S, (ix + 1) * S, iz * S, (iz + 1) * S)
            if sat_box_poly(box, fpoly):
                grid.append((ix, iz))
    gset = set(grid)

    def tile_of(x, z):
        u, v = frame(x, z)
        return (math.floor(u / S), math.floor(v / S))

    km = defaultdict(float); pieces = defaultdict(int); cross = defaultdict(int)
    cross_total = 0; seg_total = 0
    for w in ways.values():
        V = w["xz"]
        prev = None
        for i in range(len(V) - 1):
            (xa, za), (xb, zb) = V[i], V[i + 1]
            mx, mz = (xa + xb) / 2, (za + zb) / 2
            if not pip(poly, mx, mz):
                prev = None
                continue
            owner = tile_of(mx, mz)
            seg_total += 1
            if tile_of(xa, za) != tile_of(xb, zb):
                cross[owner] += 1; cross_total += 1
            km[owner] += m.seglen_m((xa, za), (xb, zb), upm) / 1000.0
            if owner != prev:
                pieces[owner] += 1
            prev = owner
    junc = defaultdict(int)
    for key, deg in nodes_deg.items():
        if deg < 2:
            continue
        x, z = node_xz[key]
        if pip(poly, x, z):
            junc[tile_of(x, z)] += 1
    bc = defaultdict(int); bv = defaultdict(int)
    for b in buildings:
        if pip(poly, b["x"], b["z"]):
            t = tile_of(b["x"], b["z"]); bc[t] += 1; bv[t] += b["nverts"]
    sc = defaultdict(int)
    for x, z in sigs:
        if pip(poly, x, z):
            sc[tile_of(x, z)] += 1
    rows = []
    for t in grid:
        rows.append({"ix": t[0], "iz": t[1], "km": km[t], "pieces": pieces[t],
                     "junctions": junc[t], "buildings": bc[t], "building_vertices": bv[t],
                     "signals": sc[t], "crossing_edges": cross[t]})
    stats = {k: m.dstats([r[k] for r in rows]) for k in
             ("km", "pieces", "junctions", "buildings", "building_vertices", "signals", "crossing_edges")}
    stats["_zero_km"] = sum(1 for r in rows if r["km"] == 0)
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w_ = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w_.writeheader()
        for r in rows:
            w_.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()})
    return {"tiles": len(grid), "cross_total": cross_total, "seg_total": seg_total,
            "stats": stats, "csv": os.path.basename(csv_path)}


# --------------------------------------------------------------------- main ---
def main():
    global m_upm, P_GLOBAL
    P = m.load_place()
    to_world, upm = m.make_to_world(P)
    m_upm = upm
    P_GLOBAL = P
    ways, rmeta = m.load_roads(to_world, upm)
    buildings, bmeta = m.load_buildings()
    sigs, nodes_other, _nk = m.load_signals()
    node_ways = defaultdict(set); node_xz = {}
    for wid, w in ways.items():
        for (la, lo), (x, z) in zip(w["ll"], w["xz"]):
            key = (round(la, 7), round(lo, 7))
            node_ways[key].add(wid)
            node_xz.setdefault(key, (x, z))
    nodes_deg = {k: len(v) for k, v in node_ways.items()}

    # --- corners ---
    seg = {}
    dirs = {}
    for nm in ("31", "32", "72", "122"):
        segs, wids = avenue_segments(ways, name_matcher(nm))
        seg["Av " + nm] = segs
        dirs["Av " + nm] = direction_deg_axial([(a, b) for _, a, b in segs])
    corners = []
    corner_info = []
    for a, b in RING:
        cands = find_corner(seg[a], seg[b], max_m=500.0)
        cl = cluster(cands)
        if not cl:
            raise SystemExit("no corner found for %s x %s" % (a, b))
        best = cl[0]
        corners.append((best["x"], best["z"]))
        corner_info.append({"pair": a + " x " + b, "best_dist_m": round(best["dist_m"], 3),
                            "clusters_within_60m": [
                                {"dist_m": round(c["dist_m"], 2),
                                 "latlon": world_to_latlon(P, c["x"], c["z"]),
                                 "ways": c["ways"]} for c in cl[:4]]})
    # Angles between avenues, to confirm the ring is a rectangle.
    ang = {k: math.degrees(v) % 180 for k, v in dirs.items()}

    def dang(p, q):
        d = abs(ang[p] - ang[q]) % 180
        return min(d, 180 - d)
    geom = {
        "axial_bearing_deg (mod 180, atan2(dz,dx), 0 = east, 90 = north)": ang,
        "angle 31 vs 122 (deg)": dang("Av 31", "Av 122"),
        "angle 32 vs 72 (deg)": dang("Av 32", "Av 72"),
        "angle 31 vs 32 (deg)": dang("Av 31", "Av 32"),
        "angle 32 vs 122 (deg)": dang("Av 32", "Av 122"),
    }

    sides_m = []
    for i in range(4):
        (x1, z1), (x2, z2) = corners[i], corners[(i + 1) % 4]
        sides_m.append(math.hypot(x2 - x1, z2 - z1) / upm)
    area_m2 = poly_area(corners) / (upm * upm)
    # Rotation: the axis of Av 31 mod 90 deg, mapped into (-45, 45].
    th31 = dirs["Av 31"]
    theta = (th31 + math.pi / 4) % (math.pi / 2) - math.pi / 4
    frame_rot = make_frame(theta)
    frame_north = make_frame(0.0)

    poly = corners
    base = measure_poly(poly, ways, nodes_deg, node_xz, buildings, sigs, nodes_other, upm)
    t_north = tile_table(frame_north, poly, ways, nodes_deg, node_xz, buildings, sigs, upm,
                         TILE_M, os.path.join(HERE, "tiles_256m_poly_northup.csv"))
    t_rot = tile_table(frame_rot, poly, ways, nodes_deg, node_xz, buildings, sigs, upm,
                       TILE_M, os.path.join(HERE, "tiles_256m_poly_rotated.csv"))

    corners_ll = [world_to_latlon(P, x, z) for (x, z) in corners]
    out = {
        "corners": [{"ring": RING[i][0] + " x " + RING[i][1], "lat": corners_ll[i][0],
                     "lon": corners_ll[i][1], "x": corners[i][0], "z": corners[i][1]}
                    for i in range(4)],
        "corner_search": corner_info,
        "geometry_check": geom,
        "sides_m": sides_m,
        "area_km2": area_m2 / 1e6,
        "rotation_rad": theta,
        "rotation_deg": math.degrees(theta),
        "poly_measure": base,
        "tiles_256_northup": t_north,
        "tiles_256_rotated": t_rot,
        "meta": {"roads": rmeta, "buildings": bmeta},
    }
    with open(os.path.join(HERE, "casco_poly.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1, default=str)

    write_section(out)
    print("corners:", [(round(c["lat"], 6), round(c["lon"], 6)) for c in out["corners"]])
    print("sides m:", [round(s, 1) for s in sides_m], "area km2:", round(out["area_km2"], 3))
    print("rotation deg:", round(out["rotation_deg"], 3))
    print("poly km:", round(base["drivable_km"], 2), "ways:", base["ways_touching_poly"])


def write_section(o):
    L = []
    a = L.append
    a(MARK)
    a("## Casco polygon (rotated, from the ring avenues)")
    a("")
    a("Run: `python -I measure_poly.py` (writes `casco_poly.json`, `tiles_256m_poly_*.csv`, and this section).")
    a("The earlier north-up sections above stay as they were. This section replaces them for the casco.")
    a("")
    a("### Corners")
    a("")
    a("Each corner is the closest approach between two named avenue ways (`Avenida N` / `Av N` tags, both"
      " ways of each pair). The corner is the midpoint of the closest pair. Corners are in ring order.")
    a("")
    a("| Corner (ring pair) | lat | lon | world x | world z |")
    a("|---|---:|---:|---:|---:|")
    for c in o["corners"]:
        a(f"| {c['ring']} | {c['lat']:.6f} | {c['lon']:.6f} | {c['x']:,.0f} | {c['z']:,.0f} |")
    a("")
    a("Closest-approach distance per pair (m), and other candidate clusters within 500 m:")
    a("")
    for ci in o["corner_search"]:
        extra = ", ".join(f"{c['dist_m']:.1f} m at ({c['latlon'][0]:.6f}, {c['latlon'][1]:.6f})"
                          for c in ci["clusters_within_60m"][1:])
        a(f"- {ci['pair']}: best {ci['best_dist_m']} m" + (f"; other clusters: {extra}" if extra else "; no other cluster within 500 m"))
    a("")
    a("### Geometry check (avenue axes, axial mean of segment directions)")
    a("")
    g = o["geometry_check"]
    for k, v in g.items():
        if isinstance(v, dict):
            a("- " + k + ": " + ", ".join(f"{kk} {vv:.1f}" for kk, vv in v.items()))
        else:
            a(f"- {k}: {v:.2f}")
    a("")
    a("### Side lengths and area")
    a("")
    s = o["sides_m"]
    a(f"- Sides (m): " + ", ".join(f"{RING[i][0]}-{RING[i][1]} {s[i]:,.1f}" for i in range(4)))
    a(f"- Area: {o['area_km2']:.3f} km²")
    a("- Shoelace area on the world frame, divided by 430²; side lengths from world distances / 430.")
    a("")
    a("### Measurements clipped to the polygon (sections 1-5)")
    a("")
    p = o["poly_measure"]
    a(f"- Drivable km (midpoint rule): **{p['drivable_km']:,.2f} km**; ways touching the polygon: "
      f"{p['ways_touching_poly']:,}; one-way: {p['ways_oneway']:,}.")
    a("")
    a("| Highway class | ways | km | one-way |")
    a("|---|---:|---:|---:|")
    for c, v in p["class_table"].items():
        a(f"| {c} | {v['ways']:,} | {v['km']:,.2f} | {v['oneway']:,} |")
    a("")
    a("| Junction degree | nodes |")
    a("|---|---:|")
    for k in ("2", "3", "4", "5", "6+"):
        a(f"| {k} | {p['junction_degree_hist'].get(k, 0):,} |")
    a(f"| all shared | {p['junction_nodes_in_poly']:,} |")
    a("")
    a(f"- Closed ways: {p['closed_ways']}; roundabout ways: {p['roundabout_ways']}.")
    a(f"- Bridges: {p['bridge_ways']} (kinds {p['bridge_kinds']}); tunnels: {p['tunnel_ways']} "
      f"(kinds {p['tunnel_kinds']}); layer != 0: {p['layer_nonzero_ways']} (values {p['layer_values']}).")
    a(f"- Buildings: {p['buildings']:,} footprints, {p['building_vertices_raw']:,} raw vertices, "
      f"{p['building_source_bytes']:,} source bytes ({p['building_source_bytes']/1e6:.2f} MB).")
    a(f"- Traffic signals (`signals[]`): {p['signals_traffic']:,}; other nodes (`nodes[]`): {p['nodes_other_kinds']:,}.")
    a("")
    a("### 256 m tiles: north-up vs rotated to the casco axes")
    a("")
    a(f"Rotation used for the rotated grid: **{o['rotation_deg']:.2f}°** "
      f"({o['rotation_rad']:.4f} rad), the axis of Av 31 folded into (-45°, 45°]. "
      f"For comparison the strip route uses rotation_rad -0.8513 (-48.8°), which is a different frame and is not used here.")
    a("Tiles are those that intersect the polygon. Features count only inside the polygon, owned by the tile of their midpoint, centroid or position.")
    a("")
    a("| Per tile (256 m) | north-up min | north-up median | north-up p95 | north-up max | rotated min | rotated median | rotated p95 | rotated max |")
    a("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    tn, tr = o["tiles_256_northup"], o["tiles_256_rotated"]
    for lab, key, nd in (("drivable km", "km", 3), ("way pieces", "pieces", 0), ("junction nodes", "junctions", 0),
                         ("buildings", "buildings", 0), ("building vertices", "building_vertices", 0),
                         ("traffic signals", "signals", 0), ("crossing edges", "crossing_edges", 0)):
        sn, sr = tn["stats"][key], tr["stats"][key]
        a(f"| {lab} | {sn['min']:.{nd}f} | {sn['median']:.{nd}f} | {sn['p95']:.{nd}f} | {sn['max']:.{nd}f} | "
          f"{sr['min']:.{nd}f} | {sr['median']:.{nd}f} | {sr['p95']:.{nd}f} | {sr['max']:.{nd}f} |")
    a("")
    a("| Grid | tiles intersecting polygon | tiles with 0 km | border-crossing segments | segments in polygon |")
    a("|---|---:|---:|---:|---:|")
    a(f"| 256 m north-up | {tn['tiles']} | {tn['stats']['_zero_km']} | {tn['cross_total']:,} | {tn['seg_total']:,} |")
    a(f"| 256 m rotated {o['rotation_deg']:.2f}° | {tr['tiles']} | {tr['stats']['_zero_km']} | {tr['cross_total']:,} | {tr['seg_total']:,} |")
    a("")
    a(f"Per-tile rows: `{tn['csv']}`, `{tr['csv']}`.")
    a("")
    a("- **Av 32 x Av 122 has no shared node.** The closest approach is 364 m, so that corner is the midpoint of a 364 m gap, and its true position is uncertain by roughly 180 m. The other three corners are shared-node intersections (0.0 m). The Av 122 ways near that corner should be checked in the data before F1 uses this polygon.")
    a("Notes: the polygon is convex, so a tile intersects it by a separating-axis test on the tile's own frame. "
      "The rotated grid's tiles are axis-aligned to the casco, so tile count is close to area / tile area with a boundary fringe.")
    a("")
    text = "\n".join(L) + "\n"
    rp = os.path.join(HERE, "REPORT.md")
    with open(rp, encoding="utf-8") as f:
        cur = f.read()
    if MARK in cur:
        cur = cur[:cur.index(MARK)].rstrip() + "\n\n"
    with open(rp, "w", encoding="utf-8") as f:
        f.write(cur.rstrip() + "\n\n" + text)


if __name__ == "__main__":
    main()
