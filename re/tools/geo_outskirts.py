"""geo_outskirts.py -- add the OSM OUTSKIRTS layer to a TILED geo place (roam LH).

Power poles / towers / lines, barrier=guard_rail ways and road gates for the
partido outside the casco. The tiled place was never asked for `power` or
`barrier`, so none of it is on disk. Same discipline as geo_rails.py: one
Overpass call (or --chunks N*N bbox cells, one POST each), budget proven
offline with --dry-run, raw answers cached under <place>/_cache/, stdlib only
(`python -I` works).

OUTPUT (inside <place>/, nothing else touched except tiles/INDEX.JSON)

    tiles/OUTSKIRTS_<ix>_<iz>.JSON
        {"lines": [...], "poles": [...], "guards": [...], "gates": [...]}
        each entry owned by the tile of its first vertex / its point, the
        same owner rule and compact sort_keys JSON as geo_rails.write_tiles.
    tiles/INDEX.JSON   gains layers.OUTSKIRTS = {"file": "OUTSKIRTS.JSON",
        "key": "lines", "key2": "poles", "keys": ["lines","poles","guards","gates"],
        "tiles": rows}, rows = [ix, iz, n, minx, minz, maxx, maxz].
        geo_region.build_index drops this layer: rerun (offline, from cache).

ENTRY SHAPE (place frame x = east * upm, z = north * upm; the reflection to
x = -east happens in C, td5_geo_frame.h)

    lines[]:  {"id","kind" (line|minor_line|cable),"points":[{x,z}],"latlon",
               "voltage","circuits","cables","name","operator","underground",
               "pole_ids": [node id per vertex when it is a pole/tower else 0]}
              underground / substation-bound cables are dropped.
    poles[]:  {"id","kind" (pole|tower|portal),"x","z","latlon","material",
               "structure","design","height","voltage","ref","tags"}
    guards[]: {"id","points","latlon","highway_near": road class or null,
               "tags"}   (barrier=guard_rail)
    gates[]:  {"id","kind","x","z","latlon","access","road","class","d",
               "tags"}   ONLY gates within GATE_ROAD_M of a ROADS way and not
               access=private/no on a service/track road: yard gates are
               dropped here (the round rule), "dropped_gates" is in the census.

    python -I re/tools/geo_outskirts.py --dry-run
    python -I re/tools/geo_outskirts.py --online [--chunks 2]
    python -I re/tools/geo_outskirts.py            # rerun from the cache
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo_common import CACHE_ROOT, LocalProjection, write_json  # noqa: E402

OVERPASS_ENDPOINTS = (
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
)
UA = "TD5RE-geo-track/0.1 (+https://github.com/; source port research tool)"
RETRIES = 3

DEFAULT_PLACE = os.path.join(CACHE_ROOT, "la_plata_partido")
BBOX_MARGIN_DEG = 0.01
GATE_ROAD_M = 6.0              # a gate counts as "on a road" within this of a way
POLE_KINDS = ("pole", "tower", "portal")
LINE_KINDS = ("line", "minor_line", "cable")
GATE_KINDS = ("gate", "lift_gate", "toll_booth", "swing_gate", "sliding_gate")
PRIVATE_ROADS = ("service", "track", "path", "footway", "pedestrian")

# No regex-on-key filters (slow on Overpass); the value alternations are on a
# handful of literals, which is cheap.
QL = """[out:json][timeout:240];
(
  node["power"~"^(%(poles)s)$"](%(bbox)s);
  way["power"~"^(%(lines)s)$"](%(bbox)s);
  way["barrier"="guard_rail"](%(bbox)s);
  node["barrier"~"^(%(gates)s)$"](%(bbox)s);
);
out body geom;
"""

KEEP_POLE = ("power", "material", "structure", "design", "height", "voltage",
             "ref", "operator", "line_attachment", "line_management")
KEEP_LINE = ("power", "voltage", "circuits", "cables", "name", "operator",
             "location", "line", "wires", "ref")
KEEP_GUARD = ("barrier", "highway", "side", "material", "height", "access")
KEEP_GATE = ("barrier", "access", "vehicle", "motor_vehicle", "name", "highway",
             "toll", "opening_hours", "locked")


def _cache_path(root: str, key: str, ext: str) -> str:
    h = hashlib.sha256(key.encode("utf-8")).hexdigest()[:20]
    return os.path.join(root, "_cache", "%s%s" % (h, ext))


def _sleep(s: float) -> None:
    time.sleep(s)


def _cached_post(url, root, data, key, timeout=240):
    path = _cache_path(root, key, ".json")
    if os.path.exists(path) and os.path.getsize(path) > 0:
        with open(path, "rb") as f:
            return f.read()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    last = None
    for attempt in range(RETRIES):
        try:
            req = urllib.request.Request(url, data=data, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                body = r.read()
            with open(path, "wb") as f:
                f.write(body)
            return body
        except (urllib.error.URLError, urllib.error.HTTPError, OSError) as exc:
            last = exc
            if attempt < RETRIES - 1:
                _sleep(4.0 * (attempt + 1))
    raise RuntimeError("fetch failed after %d tries: %s (%s)" % (RETRIES, url, last))


def load_place(place: str):
    with open(os.path.join(place, "PLACE.JSON"), encoding="utf-8") as f:
        pj = json.load(f)
    with open(os.path.join(place, "tiles", "INDEX.JSON"), encoding="utf-8") as f:
        idx = json.load(f)
    pr = pj["projection"]
    if abs(pr.get("rotation_rad", 0.0)) > 1e-12 or pr.get("offset_x", 0.0) or pr.get("offset_z", 0.0):
        raise SystemExit("geo_outskirts: %s is not an unrotated tiled frame" % place)
    proj = LocalProjection(pr["lat0"], pr["lon0"], pr["units_per_metre"])
    if (abs(proj._m_per_deg_lat - pr["m_per_deg_lat"]) > 1e-6
            or abs(proj._m_per_deg_lon - pr["m_per_deg_lon"]) > 1e-6):
        raise SystemExit("geo_outskirts: projection mismatch")
    return pj, idx, proj


def queries_for(pj: dict, chunks: int) -> list:
    b = pj["bbox"]
    s, w = b["south"] - BBOX_MARGIN_DEG, b["west"] - BBOX_MARGIN_DEG
    n, e = b["north"] + BBOX_MARGIN_DEG, b["east"] + BBOX_MARGIN_DEG
    out = []
    for i in range(chunks):
        for j in range(chunks):
            bb = "%.6f,%.6f,%.6f,%.6f" % (
                s + (n - s) * i / chunks, w + (e - w) * j / chunks,
                s + (n - s) * (i + 1) / chunks, w + (e - w) * (j + 1) / chunks)
            out.append(QL % {"bbox": bb, "poles": "|".join(POLE_KINDS),
                             "lines": "|".join(LINE_KINDS),
                             "gates": "|".join(GATE_KINDS)})
    return out


def cache_path(place: str, ql: str) -> str:
    return _cache_path(place, "overpass\x00" + ql, ".json")


def fetch(place: str, ql: str, online: bool):
    p = cache_path(place, ql)
    if not (os.path.exists(p) and os.path.getsize(p) > 0) and not online:
        return None
    last = None
    for ep in OVERPASS_ENDPOINTS:
        try:
            return json.loads(_cached_post(ep, place, ql.encode("utf-8"), "overpass\x00" + ql))
        except Exception as exc:            # noqa: BLE001 - try the mirror
            last = exc
    raise RuntimeError("every Overpass endpoint failed: %s" % last)


def _pt(proj, lat, lon):
    x, z = proj.to_world(lat, lon)
    return {"x": round(x, 2), "z": round(z, 2)}


def _keep(t, keys):
    return {k: t[k] for k in keys if k in t}


def load_roads(place: str):
    """Grid of ROADS segments (metre cells) for the gate / guardrail joins."""
    with open(os.path.join(place, "PLACE.JSON"), encoding="utf-8") as f:
        upm = json.load(f)["projection"]["units_per_metre"]
    cell = 40.0 * upm
    grid: dict = {}
    tdir = os.path.join(place, "tiles")
    for fn in sorted(os.listdir(tdir)):
        if not (fn.startswith("ROADS_") and fn.endswith(".JSON")):
            continue
        with open(os.path.join(tdir, fn), encoding="utf-8") as f:
            for r in json.load(f).get("roads", []):
                p = r.get("points") or []
                for k in range(len(p) - 1):
                    a, b = p[k], p[k + 1]
                    for gx in range(int(math.floor(min(a["x"], b["x"]) / cell)),
                                    int(math.floor(max(a["x"], b["x"]) / cell)) + 1):
                        for gz in range(int(math.floor(min(a["z"], b["z"]) / cell)),
                                        int(math.floor(max(a["z"], b["z"]) / cell)) + 1):
                            grid.setdefault((gx, gz), []).append((a, b, r))
    return grid, cell, upm


def nearest_road(grid, cell, upm, x, z):
    """(distance in metres, road) of the nearest ROADS segment, or (None, None)."""
    gx, gz = int(math.floor(x / cell)), int(math.floor(z / cell))
    best, br = 1e300, None
    for dx in (-1, 0, 1):
        for dz in (-1, 0, 1):
            for a, b, r in grid.get((gx + dx, gz + dz), ()):
                vx, vz = b["x"] - a["x"], b["z"] - a["z"]
                L2 = vx * vx + vz * vz
                t = 0.0 if L2 <= 0 else max(0.0, min(1.0, ((x - a["x"]) * vx + (z - a["z"]) * vz) / L2))
                d = math.hypot(x - (a["x"] + vx * t), z - (a["z"] + vz * t))
                if d < best:
                    best, br = d, r
    return (best / upm, br) if br is not None else (None, None)


def convert(osms: list, proj, roads) -> dict:
    grid, cell, upm = roads
    poles, lines, guards, gates = {}, {}, {}, {}
    dropped = {"gates_off_road": 0, "gates_private_yard": 0, "lines_underground": 0}
    for osm in osms:
        for el in osm.get("elements", []):
            t = el.get("tags", {})
            if el.get("type") == "node":
                if t.get("power") in POLE_KINDS:
                    p = _pt(proj, el["lat"], el["lon"])
                    poles[el["id"]] = {"id": el["id"], "kind": t["power"], "x": p["x"], "z": p["z"],
                                       "latlon": [el["lat"], el["lon"]],
                                       "material": t.get("material"), "structure": t.get("structure"),
                                       "design": t.get("design"), "height": t.get("height"),
                                       "voltage": t.get("voltage"), "ref": t.get("ref"),
                                       "tags": _keep(t, KEEP_POLE)}
                elif t.get("barrier") in GATE_KINDS:
                    p = _pt(proj, el["lat"], el["lon"])
                    d, r = nearest_road(grid, cell, upm, p["x"], p["z"])
                    if d is None or d > GATE_ROAD_M:
                        dropped["gates_off_road"] += 1
                        continue
                    cls = r.get("class")
                    if (cls in PRIVATE_ROADS and t.get("access") in ("private", "no", "customers")) \
                            or (cls in PRIVATE_ROADS and t.get("barrier") != "toll_booth"
                                and t.get("access") is None and t.get("name") is None):
                        dropped["gates_private_yard"] += 1
                        continue
                    gates[el["id"]] = {"id": el["id"], "kind": t["barrier"], "x": p["x"], "z": p["z"],
                                       "latlon": [el["lat"], el["lon"]], "access": t.get("access"),
                                       "road": r.get("id"), "class": cls, "d": round(d, 2),
                                       "tags": _keep(t, KEEP_GATE)}
            elif el.get("type") == "way":
                geom = el.get("geometry") or []
                if len(geom) < 2:
                    continue
                pts = [_pt(proj, g["lat"], g["lon"]) for g in geom]
                ll = [[g["lat"], g["lon"]] for g in geom]
                if t.get("power") in LINE_KINDS:
                    if t.get("location") in ("underground", "underwater") or t.get("power") == "cable" and t.get("location") != "overhead":
                        dropped["lines_underground"] += 1
                        continue
                    lines[el["id"]] = {"id": el["id"], "kind": t["power"], "points": pts, "latlon": ll,
                                       "voltage": t.get("voltage"), "circuits": t.get("circuits"),
                                       "cables": t.get("cables"), "name": t.get("name"),
                                       "operator": t.get("operator"),
                                       "node_ids": el.get("nodes") or []}
                elif t.get("barrier") == "guard_rail":
                    m = pts[len(pts) // 2]
                    d, r = nearest_road(grid, cell, upm, m["x"], m["z"])
                    guards[el["id"]] = {"id": el["id"], "points": pts, "latlon": ll,
                                        "highway_near": r.get("class") if r is not None and d is not None and d < 25 else None,
                                        "tags": _keep(t, KEEP_GUARD)}
    for ln in lines.values():                  # per-vertex pole id (0 = none)
        ln["pole_ids"] = [n if n in poles else 0 for n in ln.pop("node_ids")]
    s = lambda d: sorted(d.values(), key=lambda e: e["id"])   # noqa: E731
    return {"lines": s(lines), "poles": s(poles), "guards": s(guards), "gates": s(gates),
            "_dropped": dropped}


KEYS = ("lines", "poles", "guards", "gates")


def split_by_tile(entries: list, idx: dict, bbox: dict) -> list:
    """Overpass returns WHOLE ways (some run 300 km), the tile layer owns an entry by its first
    vertex: cut every way into per-tile runs (each run keeps the next vertex, so the pieces join),
    and drop runs that start outside the place bbox. A piece keeps the way id and gains "part"."""
    size, x0, z0 = float(idx["size"]), float(idx["x0"]), float(idx["z0"])
    tile = lambda p: (int(math.floor((p["x"] - x0) / size)), int(math.floor((p["z"] - z0) / size)))  # noqa: E731
    out = []
    for e in entries:
        pts, n = e["points"], len(e["points"])
        i, part = 0, 0
        while i < n - 1:
            t = tile(pts[i])
            j = i + 1
            while j < n - 1 and tile(pts[j]) == t:
                j += 1
            ll = e["latlon"][i]
            if bbox["south"] <= ll[0] <= bbox["north"] and bbox["west"] <= ll[1] <= bbox["east"]:
                q = dict(e)
                q["points"], q["latlon"], q["part"] = pts[i:j + 1], e["latlon"][i:j + 1], part
                if "pole_ids" in e:
                    q["pole_ids"] = e["pole_ids"][i:j + 1]
                out.append(q)
            part += 1
            i = j
    return out


def _anchor(e):
    pts = e.get("points")
    if pts:
        xs = [p["x"] for p in pts]
        zs = [p["z"] for p in pts]
        return pts[0]["x"], pts[0]["z"], min(xs), min(zs), max(xs), max(zs)
    return e["x"], e["z"], e["x"], e["z"], e["x"], e["z"]


def write_tiles(place: str, idx: dict, doc: dict) -> list:
    tdir = os.path.join(place, "tiles")
    size, x0, z0 = float(idx["size"]), float(idx["x0"]), float(idx["z0"])
    for fn in os.listdir(tdir):
        if fn.startswith("OUTSKIRTS_") and fn.endswith(".JSON"):
            os.remove(os.path.join(tdir, fn))
    by: dict = {}
    for key in KEYS:
        for e in doc[key]:
            ax, az, mnx, mnz, mxx, mxz = _anchor(e)
            t = (int(math.floor((ax - x0) / size)), int(math.floor((az - z0) / size)))
            slot = by.setdefault(t, {"doc": {k: [] for k in KEYS}, "ext": [1e300, 1e300, -1e300, -1e300]})
            slot["doc"][key].append(e)
            ex = slot["ext"]
            ex[0], ex[1] = min(ex[0], mnx), min(ex[1], mnz)
            ex[2], ex[3] = max(ex[2], mxx), max(ex[3], mxz)
    rows = []
    for (ix, iz) in sorted(by):
        slot = by[(ix, iz)]
        path = os.path.join(tdir, "OUTSKIRTS_%d_%d.JSON" % (ix, iz))
        with open(path + ".tmp", "w", encoding="utf-8", newline="\n") as f:
            json.dump(slot["doc"], f, sort_keys=True, separators=(",", ":"))
            f.write("\n")
        os.replace(path + ".tmp", path)
        rows.append([ix, iz, sum(len(slot["doc"][k]) for k in KEYS)] + [round(v, 2) for v in slot["ext"]])
    return rows


def census(doc: dict, upm: float) -> dict:
    def km(es):
        L = 0.0
        for e in es:
            p = e["points"]
            L += sum(math.hypot(b["x"] - a["x"], b["z"] - a["z"]) for a, b in zip(p, p[1:]))
        return round(L / upm / 1000.0, 1)
    pk: dict = {}
    for e in doc["poles"]:
        k = "%s/%s" % (e["kind"], e["material"] or "?")
        pk[k] = pk.get(k, 0) + 1
    lk: dict = {}
    for e in doc["lines"]:
        lk[e["kind"]] = lk.get(e["kind"], 0) + 1
    gk: dict = {}
    for e in doc["gates"]:
        gk["%s/%s" % (e["kind"], e["class"])] = gk.get("%s/%s" % (e["kind"], e["class"]), 0) + 1
    return {"poles_by_kind_material": pk, "lines_by_kind": lk, "line_km": km(doc["lines"]),
            "line_pieces": len(doc["lines"]),
            "pieces_with_all_poles": sum(1 for e in doc["lines"] if all(e["pole_ids"])),
            "guard_rails": len(doc["guards"]), "guard_km": km(doc["guards"]),
            "guards_near_a_road": sum(1 for e in doc["guards"] if e["highway_near"]),
            "gates_kept_by_kind_class": gk, "dropped": doc["_dropped"]}


def dry_run(place: str, chunks: int) -> dict:
    would: list = []

    def blocked(req, *a, **k):
        would.append("%s [POST %d B]" % (req.full_url, len(req.data or b"")))
        raise RuntimeError("dry run: WOULD FETCH " + req.full_url)

    real = urllib.request.urlopen
    urllib.request.urlopen = blocked
    global _sleep
    real_sleep, _sleep = _sleep, (lambda s: None)
    try:
        pj, _, _ = load_place(place)
        qls = queries_for(pj, chunks)
        uncached = [q for q in qls if not os.path.exists(cache_path(place, q))]
        for q in uncached:
            try:
                fetch(place, q, online=True)
            except Exception:               # noqa: BLE001 - expected offline
                pass
    finally:
        urllib.request.urlopen = real
        _sleep = real_sleep
    return {"place": place, "chunks": chunks, "queries": len(qls), "uncached": len(uncached),
            "real_run_requests_if_first_endpoint_answers": len(uncached),
            "attempted_incl_retries_and_mirror": len(would),
            "query_bytes_each": len(qls[0].encode("utf-8")), "query_first": qls[0]}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--place", default=DEFAULT_PLACE)
    ap.add_argument("--online", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--chunks", type=int, default=1, help="split the bbox into N*N requests")
    a = ap.parse_args(argv)
    if a.dry_run:
        print(json.dumps(dry_run(a.place, a.chunks), indent=1))
        return 0
    pj, idx, proj = load_place(a.place)
    osms = []
    for q in queries_for(pj, a.chunks):
        o = fetch(a.place, q, a.online)
        if o is None:
            print("NOT CACHED: %s\nrerun with --online" % cache_path(a.place, q))
            return 2
        if o.get("remark"):
            print("OVERPASS REMARK: %s" % o["remark"])
            if "runtime error" in o["remark"]:
                return 3
        osms.append(o)
    doc = convert(osms, proj, load_roads(a.place))
    for k in ("lines", "guards"):
        doc[k] = split_by_tile(doc[k], idx, pj["bbox"])
    rows = write_tiles(a.place, idx, doc)
    idx["layers"]["OUTSKIRTS"] = {"file": "OUTSKIRTS.JSON", "key": "lines", "key2": "poles",
                                  "keys": list(KEYS), "tiles": rows}
    write_json(os.path.join(a.place, "tiles", "INDEX.JSON"), idx)
    rep = census(doc, float(pj["projection"]["units_per_metre"]))
    rep["tiles"] = len(rows)
    print(json.dumps(rep, indent=1, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
