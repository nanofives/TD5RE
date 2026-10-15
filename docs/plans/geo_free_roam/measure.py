"""F0a measurements for GEO FREE ROAM: La Plata casco urbano (read-only).

Reads re/assets/geo/la_plata_partido/PLACE.JSON and tiles/*.JSON, writes
REPORT.md, results.json and tiles_<S>m.csv next to this file.

Run:  python -I measure.py
Stdlib only. Nothing outside _archive/roam/F0a/ is written.
"""
import csv
import glob
import json
import math
import os
import re
import statistics
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
GEO = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\assets\geo\la_plata_partido"
TILES = os.path.join(GEO, "tiles")

# re/tools/geo_fetch.py:155-158  HIGHWAY_NONDRIVABLE (applied at :1259-1264,
# so these never reach roads[]; the `footway` family goes to FOOTWAYS.JSON).
HIGHWAY_NONDRIVABLE = {
    "footway", "path", "cycleway", "pedestrian", "steps", "track",
    "bridleway", "corridor", "platform", "construction", "proposed",
}
# td5_geo_route.c:134-143 k_class_cost: the classes the router prices.
ROUTER_CLASSES = {
    "motorway", "motorway_link", "trunk", "trunk_link", "primary", "primary_link",
    "secondary", "secondary_link", "tertiary", "tertiary_link", "unclassified",
    "residential", "living_street", "service", "road",
}
# td5_geo_roads.c:109-124 geo_roads_class: maps to an RC_* enum; anything else
# maps to RC_UNKNOWN but the way is still kept (drivable).

# Tile sizes in metres. 1 m = 430 world units (PLACE.JSON units_per_metre).
TILE_SIZES_M = (128, 256, 512)

# The casco box the orchestrator gave (lat, lon). Primary.
PRIMARY_LAT = (-34.955, -34.885)
PRIMARY_LON = (-57.995, -57.905)


def load_place():
    with open(os.path.join(GEO, "PLACE.JSON"), encoding="utf-8") as f:
        place = json.load(f)
    P = place["projection"]
    assert P["kind"] == "local_tangent_plane_wgs84", P["kind"]
    assert P["rotation_rad"] == 0.0 and P["offset_x"] == 0.0 and P["offset_z"] == 0.0
    return P


def make_to_world(P):
    lat0, lon0 = P["lat0"], P["lon0"]
    mlat, mlon, upm = P["m_per_deg_lat"], P["m_per_deg_lon"], P["units_per_metre"]

    def to_world(lat, lon):
        # geo_common.py:184-187 to_metres, :214-215 to_world with rotation 0.
        return ((lon - lon0) * mlon * upm, (lat - lat0) * mlat * upm)
    return to_world, upm


def load_roads(to_world, upm):
    ways = {}
    dup = 0
    len_mismatch = 0
    no_latlon = 0
    max_resid_m = 0.0
    resid_over_05m = 0
    files = sorted(glob.glob(os.path.join(TILES, "ROADS_*.JSON")))
    for path in files:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        for r in d.get("roads", []):
            wid = r.get("id")
            if wid in ways:
                dup += 1
                continue
            pts = r.get("points") or []
            ll = r.get("latlon")
            if not ll:
                no_latlon += 1
                ll = []
            if len(pts) != len(ll):
                len_mismatch += 1
            xz = [(float(p["x"]), float(p["z"])) for p in pts]
            lls = [(float(a[0]), float(a[1])) for a in ll]
            # Projection check: file points vs PLACE.JSON projection of latlon.
            for (x, z), (la, lo) in zip(xz, lls):
                wx, wz = to_world(la, lo)
                res = math.hypot(wx - x, wz - z) / upm
                max_resid_m = max(max_resid_m, res)
                if res > 0.5:
                    resid_over_05m += 1
            ways[wid] = {
                "id": wid,
                "file": os.path.basename(path),
                "name": r.get("name"),
                "class": r.get("class"),
                "oneway": bool(r.get("oneway")),
                "oneway_dir": r.get("oneway_dir"),
                "bridge": bool(r.get("bridge")),
                "bridge_kind": r.get("bridge_kind"),
                "tunnel": bool(r.get("tunnel")),
                "tunnel_kind": r.get("tunnel_kind"),
                "layer": r.get("layer") or 0,
                "junction": r.get("junction"),
                "xz": xz,
                "ll": lls,
                "closed": len(lls) > 3 and lls[0] == lls[-1],
            }
    meta = {
        "files": len(files), "ways": len(ways), "dup_ids_skipped": dup,
        "points_vs_latlon_len_mismatch": len_mismatch, "no_latlon": no_latlon,
        "projection_max_residual_m": max_resid_m,
        "projection_points_over_0.5m": resid_over_05m,
    }
    return ways, meta


def load_buildings():
    """Returns (records, meta). Each record: id, x, z (centroid), nverts, bytes."""
    recs = {}
    dup = 0
    files = sorted(glob.glob(os.path.join(TILES, "BUILDINGS_*.JSON")))
    total_file_bytes = 0
    for path in files:
        total_file_bytes += os.path.getsize(path)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        d = json.loads(text)
        objs = d["buildings"]
        blen = array_element_bytes(text, "buildings", len(objs))
        for obj, nb in zip(objs, blen):
            bid = obj.get("id")
            if bid in recs:
                dup += 1
                continue
            P = obj.get("points") or []
            if not P:
                continue
            pts = [(float(p["x"]), float(p["z"])) for p in P]
            uniq = pts[:-1] if len(pts) > 1 and pts[0] == pts[-1] else pts
            cx = sum(p[0] for p in uniq) / len(uniq)
            cz = sum(p[1] for p in uniq) / len(uniq)
            recs[bid] = {"id": bid, "x": cx, "z": cz, "nverts": len(pts), "bytes": nb,
                         "pts": pts}
    meta = {"files": len(files), "records": len(recs), "dup_ids_skipped": dup,
            "source_bytes_all_building_files": total_file_bytes}
    return list(recs.values()), meta


def array_element_bytes(text, key, expected_len):
    """UTF-8 byte length of each element of the top-level array `key` in `text`,
    taken from the raw text (so it is the real on-disk size, not a re-dump)."""
    m = re.search(r'"%s"\s*:\s*\[' % re.escape(key), text)
    if not m:
        raise ValueError("no array %r" % key)
    dec = json.JSONDecoder()
    i, n = m.end(), len(text)
    out = []
    while i < n:
        while i < n and text[i] in " \t\r\n,":
            i += 1
        if i >= n or text[i] == "]":
            break
        _obj, end = dec.raw_decode(text, i)
        out.append(len(text[i:end].encode("utf-8")))
        i = end
    if len(out) != expected_len:
        raise ValueError("byte scan found %d elements, json has %d" % (len(out), expected_len))
    return out


def load_signals():
    """`signals` is what the game reads (td5_geo_signals.c:275). `nodes` holds the
    other kinds (crossings, bus stops, calming) and is reported separately."""
    sig = []
    nodes_kind = Counter()
    nodes = []
    for path in sorted(glob.glob(os.path.join(TILES, "SIGNALS_*.JSON"))):
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        for s in d.get("signals", []):
            sig.append((float(s["x"]), float(s["z"])))
        for n_ in d.get("nodes", []):
            nodes_kind[n_.get("kind")] += 1
            nodes.append((float(n_["x"]), float(n_["z"])))
    return sig, nodes, nodes_kind


def inside(box, x, z):
    return box[0] <= x <= box[1] and box[2] <= z <= box[3]


def box_from_latlon(to_world, lat, lon):
    (x0, _), (x1, _) = to_world(lat[0], lon[0]), to_world(lat[0], lon[1])
    (_, z0), (_, z1) = to_world(lat[0], lon[0]), to_world(lat[1], lon[0])
    return (min(x0, x1), max(x0, x1), min(z0, z1), max(z0, z1))


def avenue_extents(ways):
    pat = re.compile(r"\b(av\.?|avenida)\s*(31|32|72|122)\b", re.I)
    ext = {}
    for w in ways.values():
        m = pat.search(w["name"] or "")
        if not m:
            continue
        key = "Av " + m.group(2)
        e = ext.setdefault(key, [999, -999, 999, -999, 0])
        for la, lo in w["ll"]:
            e[0] = min(e[0], la); e[1] = max(e[1], la)
            e[2] = min(e[2], lo); e[3] = max(e[3], lo)
        e[4] += 1
    return ext


def percentile_nr(sorted_vals, q):
    n = len(sorted_vals)
    return sorted_vals[min(n - 1, max(0, math.ceil(q * n) - 1))]


def dstats(vals):
    v = sorted(vals)
    if not v:
        return None
    return {"min": v[0], "median": statistics.median(v), "p95": percentile_nr(v, 0.95),
            "max": v[-1], "sum": sum(v), "n": len(v)}


def seglen_m(a, b, upm):
    return math.hypot(b[0] - a[0], b[1] - a[1]) / upm


def measure_box(name, box, ways, nodes_deg, node_xz, buildings, sigs, nodes_other, upm):
    """Summary numbers for one box (world-unit box)."""
    km_total = 0.0
    way_inside = {}           # wid -> inside km
    seg_in = 0
    for wid, w in ways.items():
        V = w["xz"]
        wkm = 0.0
        any_in = any(inside(box, x, z) for x, z in V)
        for i in range(len(V) - 1):
            (xa, za), (xb, zb) = V[i], V[i + 1]
            mx, mz = (xa + xb) / 2, (za + zb) / 2
            if inside(box, mx, mz):
                L = seglen_m((xa, za), (xb, zb), upm)
                wkm += L / 1000.0
                seg_in += 1
        if any_in:
            way_inside[wid] = wkm
            km_total += wkm
    ways_in = [ways[w] for w in way_inside]
    by_class = defaultdict(lambda: [0, 0.0, 0])  # ways, km, oneway
    for w in ways_in:
        c = w["class"] or "(none)"
        by_class[c][0] += 1
        by_class[c][1] += way_inside[w["id"]]
        by_class[c][2] += 1 if w["oneway"] else 0
    deg_hist = Counter()
    junc_in = 0
    for key, deg in nodes_deg.items():
        if deg < 2:
            continue
        x, z = node_xz[key]
        if not inside(box, x, z):
            continue
        junc_in += 1
        deg_hist["2" if deg == 2 else ("6+" if deg >= 6 else str(deg))] += 1
    rings = [w for w in ways_in if w["closed"]]
    roundabouts = [w for w in ways_in if (w["junction"] or "") == "roundabout"]
    bridges = [w for w in ways_in if w["bridge"]]
    tunnels = [w for w in ways_in if w["tunnel"]]
    layered = [w for w in ways_in if w["layer"] != 0]
    b_in = [b for b in buildings if inside(box, b["x"], b["z"])]
    s_in = [s for s in sigs if inside(box, s[0], s[1])]
    n_in = [s for s in nodes_other if inside(box, s[0], s[1])]
    return {
        "name": name,
        "box_world": box,
        "drivable_km": km_total,
        "segments_in_box": seg_in,
        "ways_touching_box": len(ways_in),
        "ways_oneway": sum(1 for w in ways_in if w["oneway"]),
        "class_table": {c: {"ways": v[0], "km": v[1], "oneway": v[2]}
                        for c, v in sorted(by_class.items(), key=lambda kv: -kv[1][1])},
        "junction_nodes_in_box": junc_in,
        "junction_degree_hist": dict(deg_hist),
        "closed_ways": len(rings),
        "roundabout_ways": len(roundabouts),
        "bridge_ways": len(bridges),
        "tunnel_ways": len(tunnels),
        "layer_nonzero_ways": len(layered),
        "bridge_kinds": dict(Counter(w["bridge_kind"] for w in bridges)),
        "tunnel_kinds": dict(Counter(w["tunnel_kind"] for w in tunnels)),
        "layer_values": dict(Counter(w["layer"] for w in layered)),
        "bridge_classes": dict(Counter(w["class"] for w in bridges)),
        "tunnel_classes": dict(Counter(w["class"] for w in tunnels)),
        "buildings": len(b_in),
        "building_vertices_raw": sum(b["nverts"] for b in b_in),
        "building_source_bytes": sum(b["bytes"] for b in b_in),
        "signals_traffic": len(s_in),
        "nodes_other_kinds": len(n_in),
    }


def tile_tables(box, ways, nodes_deg, node_xz, buildings, sigs, upm, S_m):
    """Per-tile metrics on a world-aligned square grid of S_m metres.
    Ownership: a segment/building/node/signal belongs to the tile holding its
    MIDPOINT / CENTROID / POSITION. Only features inside `box` count."""
    S = S_m * upm
    ix0 = math.floor(box[0] / S); ix1 = math.floor(box[1] / S)
    iz0 = math.floor(box[2] / S); iz1 = math.floor(box[3] / S)
    grid = [(ix, iz) for ix in range(ix0, ix1 + 1) for iz in range(iz0, iz1 + 1)]

    def tile_of(x, z):
        return (math.floor(x / S), math.floor(z / S))

    km = defaultdict(float)
    pieces = defaultdict(int)
    cross = defaultdict(int)
    cross_total = 0
    seg_total = 0
    for w in ways.values():
        V = w["xz"]
        prev_owner = None
        for i in range(len(V) - 1):
            (xa, za), (xb, zb) = V[i], V[i + 1]
            mx, mz = (xa + xb) / 2, (za + zb) / 2
            if not inside(box, mx, mz):
                prev_owner = None
                continue
            owner = tile_of(mx, mz)
            seg_total += 1
            if tile_of(xa, za) != tile_of(xb, zb):
                cross[owner] += 1
                cross_total += 1
            km[owner] += seglen_m((xa, za), (xb, zb), upm) / 1000.0
            if owner != prev_owner:
                pieces[owner] += 1
            prev_owner = owner
    junc = defaultdict(int)
    for key, deg in nodes_deg.items():
        if deg < 2:
            continue
        x, z = node_xz[key]
        if inside(box, x, z):
            junc[tile_of(x, z)] += 1
    bcount = defaultdict(int)
    bverts = defaultdict(int)
    for b in buildings:
        if inside(box, b["x"], b["z"]):
            t = tile_of(b["x"], b["z"])
            bcount[t] += 1
            bverts[t] += b["nverts"]
    scount = defaultdict(int)
    for x, z in sigs:
        if inside(box, x, z):
            scount[tile_of(x, z)] += 1

    rows = []
    for t in grid:
        rows.append({
            "tx": t[0], "tz": t[1],
            "km": km[t], "pieces": pieces[t], "junctions": junc[t],
            "buildings": bcount[t], "building_vertices": bverts[t],
            "signals": scount[t], "crossing_edges": cross[t],
        })
    stats = {}
    for metric in ("km", "pieces", "junctions", "buildings", "building_vertices",
                   "signals", "crossing_edges"):
        stats[metric] = dstats([r[metric] for r in rows])
    stats["_tiles_zero_km"] = sum(1 for r in rows if r["km"] == 0)
    return {"S_m": S_m, "tile_count": len(grid), "grid_x": ix1 - ix0 + 1,
            "grid_z": iz1 - iz0 + 1, "crossing_edges_total": cross_total,
            "segments_total": seg_total, "stats": stats, "rows": rows}


def fmt(v, nd=2):
    if isinstance(v, float):
        return f"{v:,.{nd}f}"
    return f"{v:,}"


def stat_row(label, s, nd=2):
    if not s:
        return f"| {label} | - | - | - | - | - | - |"
    return (f"| {label} | {fmt(s['min'], nd)} | {fmt(s['median'], nd)} | "
            f"{fmt(s['p95'], nd)} | {fmt(s['max'], nd)} | {fmt(s['sum'], nd)} | {s['n']} |")


def write_csv(path, rows):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        for r in rows:
            w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()})


def build_report(res, meta, ext, alt_latlon, boxes_out, tiles_out, P):
    L = []
    a = L.append
    pr = boxes_out[0]
    a("# F0a: La Plata casco urbano, road and building numbers")
    a("")
    a("Read-only measurement for GEO FREE ROAM (`docs/plans/GEO_FREE_ROAM.md`, phase F0).")
    a("Nothing in the repo was edited. Scratch and output live in `_archive/roam/F0a/`.")
    a("")
    a("## Command")
    a("")
    a("```")
    a("cd C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\_archive\\roam\\F0a")
    a("python -I measure.py")
    a("python -I explore.py   # avenue name-tag extents + top names (stdout only)")
    a("```")
    a("")
    a("Stdlib only. Inputs: `re/assets/geo/la_plata_partido/PLACE.JSON`, `tiles/ROADS_*`, `tiles/BUILDINGS_*`, `tiles/SIGNALS_*`.")
    a("")
    a("## Projection (lat/lon to world)")
    a("")
    a(f"- `PLACE.JSON` `projection`: kind `{P['kind']}`, lat0 {P['lat0']}, lon0 {P['lon0']}, "
      f"m/deg lat {P['m_per_deg_lat']:.3f}, m/deg lon {P['m_per_deg_lon']:.3f}, "
      f"units_per_metre {P['units_per_metre']}, rotation 0, offset (0, 0).")
    a("- Formula (`re/tools/geo_common.py:184-187` and `:214-216`, rotation 0, offset 0):")
    a("  `x = (lon - lon0) * m_per_deg_lon * 430`, `z = (lat - lat0) * m_per_deg_lat * 430`.")
    a("  North is +z.")
    a(f"- Checked against the file: every ROADS point is re-projected from its `latlon` and compared with its stored `x,z`. "
      f"Max residual {meta['roads']['projection_max_residual_m']:.6f} m over all roads points; "
      f"points off by more than 0.5 m: {meta['roads']['projection_points_over_0.5m']}.")
    a("")
    a("## Casco polygon")
    a("")
    a("Avenue name-tag extents (from `ROADS` `name` tags, every way that matches; `explore.py` prints the same):")
    a("")
    a("| Name tag | ways | lat min | lat max | lon min | lon max |")
    a("|---|---:|---:|---:|---:|---:|")
    for k in sorted(ext):
        e = ext[k]
        a(f"| {k} | {e[4]} | {e[0]:.6f} | {e[1]:.6f} | {e[2]:.6f} | {e[3]:.6f} |")
    a("")
    a(f"- **Primary box (the one the orchestrator gave):** lat {PRIMARY_LAT[0]} .. {PRIMARY_LAT[1]}, "
      f"lon {PRIMARY_LON[0]} .. {PRIMARY_LON[1]}. Size "
      f"{pr['box_world'][1]-pr['box_world'][0]:,.0f} x {pr['box_world'][3]-pr['box_world'][2]:,.0f} world units "
      f"= {(pr['box_world'][1]-pr['box_world'][0])/430/1000:.2f} x {(pr['box_world'][3]-pr['box_world'][2])/430/1000:.2f} km.")
    a(f"- **Alternative box (sensitivity):** union of the four avenue extents above, "
      f"lat {alt_latlon[0][0]:.6f} .. {alt_latlon[0][1]:.6f}, lon {alt_latlon[1][0]:.6f} .. {alt_latlon[1][1]:.6f}. "
      f"It is an upper bound: each avenue way runs past the ring, so this box is larger than the ring.")
    a("- **Size check:** the plan says the casco is about 4 x 4 km. The primary box is about 8.2 x 7.8 km. "
      "Either the plan's figure is a half-width or the box is too big. The numbers below use the primary box, "
      "and the alternative box is shown next to it. This needs a decision from Mariano.")
    a("- Ways are kept when any vertex is inside the box. Lengths and per-tile counts use segments whose "
      "midpoint is inside the box, so a way that crosses the edge is cut at its midpoints, not exactly.")
    a("")
    a("## Data loaded")
    a("")
    a(f"- ROADS: {meta['roads']['files']} tile files, {meta['roads']['ways']:,} unique ways "
      f"(duplicate ids skipped: {meta['roads']['dup_ids_skipped']}; points vs latlon length mismatches: "
      f"{meta['roads']['points_vs_latlon_len_mismatch']}; ways with no latlon: {meta['roads']['no_latlon']}).")
    a(f"- BUILDINGS: {meta['buildings']['files']} tile files, {meta['buildings']['records']:,} unique footprints "
      f"(duplicate ids skipped: {meta['buildings']['dup_ids_skipped']}). All BUILDINGS files together: "
      f"{meta['buildings']['source_bytes_all_building_files']/1e6:,.1f} MB on disk.")
    a(f"- SIGNALS: {meta['signals']['files']} tile files. `signals[]` (what the game reads) = {meta['signals']['signals_total']:,}; "
      f"`nodes[]` (other kinds) = {meta['signals']['nodes_total']:,}.")
    a("")
    a("## 1. Drivable ways")
    a("")
    a("Filter. A way is drivable when it is in `ROADS` `roads[]`. The writer never puts `HIGHWAY_NONDRIVABLE` "
      "values there, so the filter is the writer's, and a second check counts any leaked value.")
    a("- Writer: `re/tools/geo_fetch.py:155-158` (set), `:1259-1264` (skip), `:1319-1322` (append to `roads`).")
    a("- Reader: `td5mod/src/td5re/td5_geo_roads.c` keeps every `roads[]` entry with at least 2 points "
      "(`:530-580`). `geo_roads_class` (`:109-124`) maps classes to `RC_*`, and any other class becomes `RC_UNKNOWN` but is still kept.")
    a("- Router: `td5mod/src/td5re/td5_geo_route.c:134-143` prices the classes in `ROUTER_CLASSES`.")
    a(f"- Non-drivable values found in the ROADS files: {meta['roads']['nondrivable_leaked']}.")
    a("")
    a(f"Primary box: **{fmt(pr['drivable_km'], 1)} km** of drivable way (midpoint rule), "
      f"{pr['ways_touching_box']:,} ways with a vertex inside, of which {pr['ways_oneway']:,} one-way.")
    a(f"Alternative box: {fmt(boxes_out[1]['drivable_km'], 1)} km, {boxes_out[1]['ways_touching_box']:,} ways, "
      f"{boxes_out[1]['ways_oneway']:,} one-way.")
    a("")
    a("| Highway class (primary box) | ways | km | one-way ways |")
    a("|---|---:|---:|---:|")
    for c, v in pr["class_table"].items():
        a(f"| {c} | {v['ways']:,} | {fmt(v['km'], 2)} | {v['oneway']:,} |")
    a("")
    a("## 2. Junctions and rings")
    a("")
    a("A junction is an OSM node (exact 7-decimal `latlon`) that is a vertex of 2 or more distinct drivable ways. "
      "Degree = number of distinct ways. Only nodes inside the box count, but degrees use all ways in the file.")
    a("")
    a("| Degree | Primary box | Alternative box |")
    a("|---|---:|---:|")
    for k in ("2", "3", "4", "5", "6+"):
        a(f"| {k}{' (plain join, 2 ways)' if k == '2' else ''} | {pr['junction_degree_hist'].get(k, 0):,} | "
          f"{boxes_out[1]['junction_degree_hist'].get(k, 0):,} |")
    a(f"| **all shared nodes** | {pr['junction_nodes_in_box']:,} | {boxes_out[1]['junction_nodes_in_box']:,} |")
    a("")
    a(f"- Closed ways (first point = last point), primary box: {pr['closed_ways']}.")
    a(f"- Ways tagged `junction=roundabout`, primary box: {pr['roundabout_ways']}.")
    a("- Plaza or ring-road loops in OSM are closed ways, so the closed count is the plaza and ring candidate count.")
    a("")
    a("## 3. Tile sizes (world-aligned square grid, tiles overlapping the primary box)")
    a("")
    a("Ownership: a segment belongs to the tile of its midpoint, a junction to its node position, a building to its "
      "footprint centroid, a signal to its position. Only features inside the primary box are counted, and all grid "
      "tiles are listed (zeros included). `crossing_edges` = segments whose two endpoints are in different tiles, "
      "counted to the owner tile.")
    a("")
    for t in tiles_out:
        a(f"### {t['S_m']} m tiles")
        a("")
        a(f"- Tiles overlapping the box: **{t['tile_count']}** ({t['grid_x']} x {t['grid_z']}). "
          f"Tiles with zero drivable km: {t['stats']['_tiles_zero_km']}.")
        a(f"- Segments in box: {t['segments_total']:,}. Segments crossing a tile border: "
          f"**{t['crossing_edges_total']:,}** ({100.0*t['crossing_edges_total']/max(1,t['segments_total']):.2f} % of segments).")
        a("")
        a("| Per tile | min | median | p95 | max | sum | tiles |")
        a("|---|---:|---:|---:|---:|---:|---:|")
        a(stat_row("drivable km", t["stats"]["km"], 3))
        a(stat_row("way pieces (runs inside tile)", t["stats"]["pieces"], 0))
        a(stat_row("junction nodes", t["stats"]["junctions"], 0))
        a(stat_row("buildings (footprints)", t["stats"]["buildings"], 0))
        a(stat_row("building vertices (raw)", t["stats"]["building_vertices"], 0))
        a(stat_row("traffic signals", t["stats"]["signals"], 0))
        a(stat_row("crossing edges (owner)", t["stats"]["crossing_edges"], 0))
        a("")
        csv_name = f"tiles_{t['S_m']}m.csv"
        a(f"Per-tile rows: `{csv_name}`.")
        a("")
    a("## 4. Bridges, tunnels, layers inside the box")
    a("")
    a("| Item (primary box) | count |")
    a("|---|---:|")
    a(f"| ways with `bridge=yes` | {pr['bridge_ways']} |")
    a(f"| ways with `tunnel=yes` | {pr['tunnel_ways']} |")
    a(f"| ways with `layer != 0` | {pr['layer_nonzero_ways']} |")
    a("")
    a(f"- bridge kinds: `{pr['bridge_kinds']}`; classes: `{pr['bridge_classes']}`")
    a(f"- tunnel kinds: `{pr['tunnel_kinds']}`; classes: `{pr['tunnel_classes']}`")
    a(f"- layer values: `{pr['layer_values']}`")
    a("- Source: `geo_fetch.py:1330-1339` (`bridge`, `tunnel`, `layer` read from the way). "
      "Only `roads[]` is counted here, so a bridge that is not a drivable way (none expected) is not in this table.")
    a("")
    a("## 5. Building data volume (primary box)")
    a("")
    a(f"- Footprints inside the box: **{pr['buildings']:,}**.")
    a(f"- Vertices (raw `points`, closing point included): **{pr['building_vertices_raw']:,}**.")
    a(f"- Source JSON bytes of those records, measured on the raw file text (`array_element_bytes`): "
      f"**{pr['building_source_bytes']:,} bytes ({pr['building_source_bytes']/1e6:.2f} MB)**.")
    a(f"- Alternative box: {boxes_out[1]['buildings']:,} footprints, {boxes_out[1]['building_vertices_raw']:,} vertices, "
      f"{boxes_out[1]['building_source_bytes']/1e6:.2f} MB.")
    a("")
    a("## 6. Ambiguities and limits")
    a("")
    a("- **Casco size.** The plan says about 4 x 4 km. The box given is about 8.2 x 7.8 km (see the Casco section). "
      "Both boxes are reported.")
    a("- **Midpoint rule.** Km and crossing counts use segment midpoints, so a segment cut by a border is counted once, in the tile of its midpoint. "
      "A way that leaves the box keeps its inside segments only.")
    a("- **Degree-2 nodes.** A node shared by exactly two ways is often just a split in OSM, not a real junction. "
      "The 3/4/5/6+ rows are the real junctions. The degree-2 row is shown for completeness.")
    a("- **Signals.** Only `signals[]` is counted as traffic signals (the game reads that key, `td5_geo_signals.c:275`). "
      "`nodes[]` holds other kinds (crossings, bus stops, calming) and is reported as a count in the data section only.")
    a("- **Building vertices.** Raw count, including the closing point of each ring. Unique vertices are about one fewer per footprint.")
    a("- **Bytes.** Measured on the raw file, per element, with the same bytes the file holds (UTF-8). The records are not re-serialised.")
    a("- **Duplicates.** The loader keeps the first copy of any repeated id; the counts above show how many were skipped (0 expected).")
    a("- **Projection.** Verified against the file, not assumed. Residual is in the Projection section.")
    a("- **`bus_stop` in roads[].** One way with `class = bus_stop` is in the primary box. The writer's "
      "non-drivable set does not list it, so it is kept as a drivable road (class RC_UNKNOWN in the reader). "
      "It is 0.04 km, so the effect on the totals is nil, but the drivable filter for F1 should decide on it explicitly.")
    a("- **`tunnel=building_passage`.** 8 of the 10 tunnel ways are archways through buildings (`tunnel_kind`), not bores. "
      "Only `tunnel=yes` (2 ways) is a real tunnel. The F1 grade-separation rules should key on the value, not the boolean.")
    a("- **Plaza and ring loops.** Closed-way and roundabout counts are what the data gives. "
      "Whether a closed way is a plaza ring the game should treat as a loop is not decided here.")
    a("")
    a("## Raw results")
    a("")
    a("`results.json` holds every number above, in both boxes, for the orchestrator to re-read.")
    return "\n".join(L) + "\n"


def main():
    P = load_place()
    to_world, upm = make_to_world(P)
    ways, rmeta = load_roads(to_world, upm)
    # Non-drivable leak check (should be 0).
    leaked = Counter(w["class"] for w in ways.values() if w["class"] in HIGHWAY_NONDRIVABLE)
    rmeta["nondrivable_leaked"] = sum(leaked.values())
    buildings, bmeta = load_buildings()
    sigs, nodes_other, nodes_kind = load_signals()
    smeta = {"files": len(glob.glob(os.path.join(TILES, "SIGNALS_*.JSON"))),
             "signals_total": len(sigs), "nodes_total": len(nodes_other),
             "nodes_kinds": dict(nodes_kind)}
    meta = {"roads": rmeta, "buildings": bmeta, "signals": smeta}

    # Nodes (OSM node identity = exact latlon at 7 decimals).
    node_ways = defaultdict(set)
    node_xz = {}
    for wid, w in ways.items():
        for (la, lo), (x, z) in zip(w["ll"], w["xz"]):
            key = (round(la, 7), round(lo, 7))
            node_ways[key].add(wid)
            node_xz.setdefault(key, (x, z))
    nodes_deg = {k: len(v) for k, v in node_ways.items()}

    ext = avenue_extents(ways)
    alt_lat = (min(e[0] for e in ext.values()), max(e[1] for e in ext.values()))
    alt_lon = (min(e[2] for e in ext.values()), max(e[3] for e in ext.values()))
    box_p = box_from_latlon(to_world, PRIMARY_LAT, PRIMARY_LON)
    box_a = box_from_latlon(to_world, alt_lat, alt_lon)

    boxes = [
        measure_box("primary", box_p, ways, nodes_deg, node_xz, buildings, sigs, nodes_other, upm),
        measure_box("alternative", box_a, ways, nodes_deg, node_xz, buildings, sigs, nodes_other, upm),
    ]
    tiles = []
    for S_m in TILE_SIZES_M:
        t = tile_tables(box_p, ways, nodes_deg, node_xz, buildings, sigs, upm, S_m)
        tiles.append(t)
        write_csv(os.path.join(HERE, f"tiles_{S_m}m.csv"), t["rows"])
        del t["rows"]
    # tile_tables deleted rows for the JSON; the CSVs keep them.

    results = {"meta": meta, "avenue_extents": ext,
               "alt_latlon": [alt_lat, alt_lon],
               "primary_latlon": [PRIMARY_LAT, PRIMARY_LON],
               "boxes": boxes, "tiles": tiles, "projection": P}
    with open(os.path.join(HERE, "results.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, indent=1, default=str)
    with open(os.path.join(HERE, "REPORT.md"), "w", encoding="utf-8") as f:
        f.write(build_report(results, meta, ext, (alt_lat, alt_lon), boxes, tiles, P))
    print("ok: REPORT.md, results.json, tiles_*m.csv written to", HERE)
    print("primary km", round(boxes[0]["drivable_km"], 3), "ways", boxes[0]["ways_touching_box"])
    print("alt km", round(boxes[1]["drivable_km"], 3), "ways", boxes[1]["ways_touching_box"])


if __name__ == "__main__":
    main()
