"""geo_rails.py -- add the OSM RAILWAY layer to a TILED geo place (roam ID).

The tiled place (geo_region.py) was fetched with geo_fetch.OVERPASS_QL, which
asks for highway / landuse / building / ... but never `railway`, so La Plata's
Roca line, the Provincial line and the Tolosa / Gambier / Ensenada yards are not
on disk anywhere. Adding `railway` to OVERPASS_QL would change the Overpass cache
key of every block (a full re-fetch of 43 blocks); this tool instead makes ONE
railway-only Overpass call for the place bbox and writes one more tile layer
beside the existing five.

OUTPUT (all inside <place>/, nothing else is touched except INDEX.JSON)

    tiles/RAILS_<ix>_<iz>.JSON   {"rails": [...], "nodes": [...]}
                                 entries anchored (first vertex / the node) in
                                 tile (ix, iz) of the place's tile lattice, the
                                 same owner rule and the same compact sort_keys
                                 JSON geo_region.write_tile_docs uses.
    tiles/INDEX.JSON             gains layers.RAILS = {"file": "RAILS.JSON",
                                 "key": "rails", "key2": "nodes", "tiles": rows}
                                 rows = [ix, iz, n, minx, minz, maxx, maxz] as
                                 geo_region.build_index writes them. The other
                                 layers' rows are copied through unchanged.
    _cache/<sha>.json            the raw Overpass answer (geo_fetch cache key
                                 scheme), so a rerun is offline.

    geo_region.build_index rewrites INDEX.JSON from its own TILED_LAYERS table
    and so DROPS layers.RAILS: after any geo_region rebuild of the place, rerun
    this tool (from the cache, no network) to put the layer back.

ENTRY SHAPE (place frame: x = east * units_per_metre, z = north * upm, the same
frame every other tile layer is written in; the game's reflection to x = -east
happens in C, td5_geo_frame.h)

    rails[]: {"id", "kind" (the railway= value, or "yard" for landuse=railway),
              "points": [{"x","z"}], "latlon": [[lat,lon]], "closed", "area",
              "name", "usage", "service", "gauge", "tracks", "electrified",
              "bridge", "tunnel", "layer", "cutting", "embankment",
              "xing": [vertex index of every level/foot crossing node],
              "tags": {every railway-relevant tag, verbatim}}
    nodes[]: {"id", "kind" (railway= value), "x", "z", "latlon", "name",
              "tags": {...}}  -- level_crossing, crossing, station, halt, stop,
              buffer_stop, tram_stop, platform nodes.

NETWORK DISCIPLINE (memory reference_geo_fetch_budget_dryrun_and_stale_baseline)
    --dry-run   urllib.request.urlopen is patched to record and raise, so the
                budget is PROVED offline: prints every request the real run
                would make (expected: exactly 1 Overpass POST when uncached, 0
                when cached) and the bbox / query bytes.
    --online    required for any request not in the cache. Without it the run
                converts from the cache or stops with "NOT CACHED".

    python -I re/tools/geo_rails.py --dry-run
    python -I re/tools/geo_rails.py --online          # the one network call
    python -I re/tools/geo_rails.py                   # rerun from the cache
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

# Stdlib only, so `python -I` runs it (numpy lives in the user site, which -I
# hides, and geo_fetch imports numpy). The endpoints, User-Agent and the cache
# key / path scheme are geo_fetch's (OVERPASS_ENDPOINTS, UA, _cache_path,
# _cached_get), copied so a cache file written by either tool serves both.
OVERPASS_ENDPOINTS = (
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
)
UA = "TD5RE-geo-track/0.1 (+https://github.com/; source port research tool)"
RETRIES = 3


def _cache_path(root: str, key: str, ext: str) -> str:
    h = hashlib.sha256(key.encode("utf-8")).hexdigest()[:20]
    return os.path.join(root, "_cache", "%s%s" % (h, ext))


def _sleep(s: float) -> None:
    time.sleep(s)


def _cached_post(url: str, root: str, data: bytes, key: str,
                 timeout: int = 180) -> bytes:
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
                _sleep(4.0 * (attempt + 1))     # Overpass 429/504 back-off
    raise RuntimeError("fetch failed after %d tries: %s (%s)" % (RETRIES, url, last))

DEFAULT_PLACE = os.path.join(CACHE_ROOT, "la_plata_partido")
BBOX_MARGIN_DEG = 0.01        # ~1 km past the place bbox: a line leaving the
                              # partido keeps its last segment inside the frame

# Nodes worth keeping. Signals, switches, milestones and derails are thousands
# of points nobody draws; these are the ones roam geometry or a label uses.
NODE_KINDS = ("level_crossing", "crossing", "station", "halt", "stop",
              "tram_stop", "buffer_stop", "platform", "tram_level_crossing")
XING_KINDS = ("level_crossing", "crossing", "tram_level_crossing")

RAILS_QL = """[out:json][timeout:180];
(
  way["railway"](%(bbox)s);
  way["landuse"="railway"](%(bbox)s);
  node["railway"~"^(%(nodes)s)$"](%(bbox)s);
);
out body geom;
"""

# Tags copied into "tags" verbatim (railway-relevant only; the rest of a way's
# tags are dropped like every other layer drops what nothing reads).
KEEP_TAGS = ("railway", "landuse", "usage", "service", "gauge", "tracks",
             "electrified", "voltage", "frequency", "bridge", "tunnel",
             "layer", "cutting", "embankment", "name", "operator", "ref",
             "railway:preferred_direction", "railway:track_ref", "maxspeed",
             "crossing", "crossing:barrier", "crossing:light", "crossing:bell",
             "crossing:saltire", "public_transport", "train", "level",
             "disused", "abandoned", "historic", "area", "surface", "covered")


def _bool_tag(v) -> bool:
    return v is not None and str(v).lower() not in ("no", "0", "false")


def _int_tag(v, dflt: int) -> int:
    try:
        return int(str(v).split(";")[0].strip())
    except (TypeError, ValueError):
        return dflt


def load_place(place: str):
    with open(os.path.join(place, "PLACE.JSON"), encoding="utf-8") as f:
        pj = json.load(f)
    with open(os.path.join(place, "tiles", "INDEX.JSON"), encoding="utf-8") as f:
        idx = json.load(f)
    pr = pj["projection"]
    if abs(pr.get("rotation_rad", 0.0)) > 1e-12 or pr.get("offset_x", 0.0) or pr.get("offset_z", 0.0):
        raise SystemExit("geo_rails: %s is not an unrotated tiled frame" % place)
    proj = LocalProjection(pr["lat0"], pr["lon0"], pr["units_per_metre"])
    # The projection is rebuilt from lat0 / lon0; prove it is the one the
    # other tiles were written with before writing a single coordinate.
    if (abs(proj._m_per_deg_lat - pr["m_per_deg_lat"]) > 1e-6
            or abs(proj._m_per_deg_lon - pr["m_per_deg_lon"]) > 1e-6):
        raise SystemExit("geo_rails: projection mismatch (%r vs %r)" % (
            (proj._m_per_deg_lat, proj._m_per_deg_lon),
            (pr["m_per_deg_lat"], pr["m_per_deg_lon"])))
    return pj, idx, proj


def query_for(pj: dict) -> str:
    b = pj["bbox"]
    bbox = "%.6f,%.6f,%.6f,%.6f" % (b["south"] - BBOX_MARGIN_DEG,
                                    b["west"] - BBOX_MARGIN_DEG,
                                    b["north"] + BBOX_MARGIN_DEG,
                                    b["east"] + BBOX_MARGIN_DEG)
    return RAILS_QL % {"bbox": bbox, "nodes": "|".join(NODE_KINDS)}


def cache_path(place: str, ql: str) -> str:
    return _cache_path(place, "overpass\x00" + ql, ".json")


def fetch(place: str, ql: str, online: bool) -> dict | None:
    """The Overpass answer, from the cache or (online) the network."""
    p = cache_path(place, ql)
    if not (os.path.exists(p) and os.path.getsize(p) > 0) and not online:
        return None
    last = None
    for ep in OVERPASS_ENDPOINTS:
        try:
            raw = _cached_post(ep, place, ql.encode("utf-8"), "overpass\x00" + ql)
            return json.loads(raw)
        except Exception as exc:               # noqa: BLE001 - try the mirror
            last = exc
    raise RuntimeError("every Overpass endpoint failed: %s" % last)


def _pt(proj, lat, lon):
    x, z = proj.to_world(lat, lon)
    return {"x": round(x, 2), "z": round(z, 2)}


def convert(osm: dict, proj) -> dict:
    """{"rails": [...], "nodes": [...]} in the place frame, sorted by id."""
    xnode = {}
    nodes = []
    for el in osm.get("elements", []):
        if el.get("type") != "node":
            continue
        t = el.get("tags", {})
        kind = t.get("railway")
        if kind not in NODE_KINDS:
            continue
        if kind in XING_KINDS:
            xnode[el["id"]] = kind
        p = _pt(proj, el["lat"], el["lon"])
        nodes.append({"id": el["id"], "kind": kind, "x": p["x"], "z": p["z"],
                      "latlon": [el["lat"], el["lon"]], "name": t.get("name"),
                      "tags": {k: t[k] for k in KEEP_TAGS if k in t}})
    rails = []
    for el in osm.get("elements", []):
        if el.get("type") != "way":
            continue
        geom = el.get("geometry") or []
        if len(geom) < 2:
            continue
        t = el.get("tags", {})
        kind = t.get("railway") or ("yard" if t.get("landuse") == "railway" else None)
        if not kind:
            continue
        ids = el.get("nodes") or []
        closed = len(ids) > 2 and ids[0] == ids[-1]
        rails.append({
            "id": el["id"], "kind": kind,
            "points": [_pt(proj, g["lat"], g["lon"]) for g in geom],
            "latlon": [[g["lat"], g["lon"]] for g in geom],
            "closed": closed,
            "area": kind in ("yard", "platform", "station") or _bool_tag(t.get("area")) and closed,
            "name": t.get("name"), "usage": t.get("usage"),
            "service": t.get("service"), "gauge": t.get("gauge"),
            "tracks": _int_tag(t.get("tracks"), 1),
            "electrified": t.get("electrified"),
            "bridge": _bool_tag(t.get("bridge")),
            "tunnel": _bool_tag(t.get("tunnel")),
            "layer": _int_tag(t.get("layer"), 0),
            "cutting": _bool_tag(t.get("cutting")),
            "embankment": _bool_tag(t.get("embankment")),
            "xing": [i for i, nid in enumerate(ids) if nid in xnode],
            "tags": {k: t[k] for k in KEEP_TAGS if k in t},
        })
    rails.sort(key=lambda e: e["id"])
    nodes.sort(key=lambda e: e["id"])
    return {"rails": rails, "nodes": nodes}


XING_CELL = 50.0 * 430.0       # road-segment grid cell for the crossing search


def _seg_hit(ax, az, bx, bz, cx, cz, dx, dz):
    """(t, u) where segment ab meets segment cd, or None."""
    rx, rz, sx, sz = bx - ax, bz - az, dx - cx, dz - cz
    den = rx * sz - rz * sx
    if abs(den) < 1e-9:
        return None
    t = ((cx - ax) * sz - (cz - az) * sx) / den
    u = ((cx - ax) * rz - (cz - az) * rx) / den
    if 0.0 <= t <= 1.0 and 0.0 <= u <= 1.0:
        return t, u
    return None


def road_crossings(place: str, doc: dict) -> dict:
    """Every rail way gets "road_x": where a ROADS way crosses it, in the place frame.

    kind: "grade"     both at layer 0, neither a bridge / tunnel: a LEVEL CROSSING
          "rail_over" the rail is the bridge: the road passes under the deck
          "road_over" the road is the bridge: the rail passes under it
          "tunnel"    either one is in a tunnel
    Each row: {"x","z","seg","t","road","class","kind"} (seg = the rail's segment index, t in [0,1]
    along it), sorted by (seg, t). Computed here, offline and once, so the game reads a fact
    of the data instead of finding crossings tile by tile (a crossing outside a tile's 3x3
    would otherwise be invisible to it, and a bridge ramp would step at the seam)."""
    tiles_dir = os.path.join(place, "tiles")
    grid: dict = {}
    roads = []
    for fn in sorted(os.listdir(tiles_dir)):
        if not (fn.startswith("ROADS_") and fn.endswith(".JSON")):
            continue
        with open(os.path.join(tiles_dir, fn), encoding="utf-8") as f:
            for r in json.load(f).get("roads", []):
                p = r.get("points") or []
                if len(p) < 2:
                    continue
                ri = len(roads)
                roads.append(r)
                for k in range(len(p) - 1):
                    a, b = p[k], p[k + 1]
                    for gx in range(int(math.floor(min(a["x"], b["x"]) / XING_CELL)),
                                    int(math.floor(max(a["x"], b["x"]) / XING_CELL)) + 1):
                        for gz in range(int(math.floor(min(a["z"], b["z"]) / XING_CELL)),
                                        int(math.floor(max(a["z"], b["z"]) / XING_CELL)) + 1):
                            grid.setdefault((gx, gz), []).append((ri, k))
    counts: dict = {}
    for e in doc["rails"]:
        rows = []
        seen = set()
        p = e["points"]
        if e["area"]:
            e["road_x"] = rows
            continue
        for k in range(len(p) - 1):
            a, b = p[k], p[k + 1]
            cand = set()
            for gx in range(int(math.floor(min(a["x"], b["x"]) / XING_CELL)),
                            int(math.floor(max(a["x"], b["x"]) / XING_CELL)) + 1):
                for gz in range(int(math.floor(min(a["z"], b["z"]) / XING_CELL)),
                                int(math.floor(max(a["z"], b["z"]) / XING_CELL)) + 1):
                    cand.update(grid.get((gx, gz), ()))
            for ri, j in sorted(cand):
                r = roads[ri]
                q0, q1 = r["points"][j], r["points"][j + 1]
                h = _seg_hit(a["x"], a["z"], b["x"], b["z"], q0["x"], q0["z"], q1["x"], q1["z"])
                if h is None:
                    continue
                t = h[0]
                x = a["x"] + (b["x"] - a["x"]) * t
                z = a["z"] + (b["z"] - a["z"]) * t
                key = (r["id"], round(x), round(z))
                if key in seen:                    # a road vertex on the rail: one hit, not two
                    continue
                seen.add(key)
                rl, ql = e["layer"], _int_tag(r.get("layer"), 0)
                rb, qb = e["bridge"], bool(r.get("bridge"))
                if e["tunnel"] or r.get("tunnel"):
                    kind = "tunnel"
                elif rb and (not qb or rl > ql):
                    kind = "rail_over"
                elif qb and (not rb or ql > rl):
                    kind = "road_over"
                elif rl != ql:
                    kind = "rail_over" if rl > ql else "road_over"
                else:
                    kind = "grade"
                rows.append({"x": round(x, 2), "z": round(z, 2), "seg": k, "t": round(t, 6),
                             "road": r["id"], "class": r.get("class"), "kind": kind})
                counts[kind] = counts.get(kind, 0) + 1
        rows.sort(key=lambda w: (w["seg"], w["t"], w["road"]))
        e["road_x"] = rows
    return counts


def _anchor_bbox(e: dict):
    pts = e.get("points")
    if pts:
        xs = [p["x"] for p in pts]
        zs = [p["z"] for p in pts]
        return pts[0]["x"], pts[0]["z"], min(xs), min(zs), max(xs), max(zs)
    return e["x"], e["z"], e["x"], e["z"], e["x"], e["z"]


def write_tiles(place: str, idx: dict, doc: dict) -> list:
    """RAILS_<ix>_<iz>.JSON per anchor tile; returns the INDEX rows."""
    tiles_dir = os.path.join(place, "tiles")
    size, x0, z0 = float(idx["size"]), float(idx["x0"]), float(idx["z0"])
    for fn in os.listdir(tiles_dir):           # a rerun replaces the layer whole
        if fn.startswith("RAILS_") and fn.endswith(".JSON"):
            os.remove(os.path.join(tiles_dir, fn))
    by_tile: dict = {}
    for key in ("rails", "nodes"):
        for e in doc[key]:
            ax, az, mnx, mnz, mxx, mxz = _anchor_bbox(e)
            t = (int(math.floor((ax - x0) / size)), int(math.floor((az - z0) / size)))
            slot = by_tile.setdefault(t, {"doc": {"rails": [], "nodes": []},
                                          "ext": [1e300, 1e300, -1e300, -1e300]})
            slot["doc"][key].append(e)
            ex = slot["ext"]
            ex[0], ex[1] = min(ex[0], mnx), min(ex[1], mnz)
            ex[2], ex[3] = max(ex[2], mxx), max(ex[3], mxz)
    rows = []
    for (ix, iz) in sorted(by_tile):
        slot = by_tile[(ix, iz)]
        path = os.path.join(tiles_dir, "RAILS_%d_%d.JSON" % (ix, iz))
        with open(path + ".tmp", "w", encoding="utf-8", newline="\n") as f:
            json.dump(slot["doc"], f, sort_keys=True, separators=(",", ":"))
            f.write("\n")
        os.replace(path + ".tmp", path)
        n = len(slot["doc"]["rails"]) + len(slot["doc"]["nodes"])
        rows.append([ix, iz, n] + [round(v, 2) for v in slot["ext"]])
    return rows


def census(doc: dict) -> dict:
    kinds: dict = {}
    km: dict = {}
    for e in doc["rails"]:
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
        if not e["area"]:
            L = 0.0
            p = e["points"]
            for a, b in zip(p, p[1:]):
                L += math.hypot(b["x"] - a["x"], b["z"] - a["z"])
            km[e["kind"]] = km.get(e["kind"], 0.0) + L
    nk: dict = {}
    for e in doc["nodes"]:
        nk[e["kind"]] = nk.get(e["kind"], 0) + 1
    return {"ways_by_kind": kinds, "nodes_by_kind": nk,
            "units_by_kind": {k: round(v) for k, v in km.items()},
            "bridges": sum(1 for e in doc["rails"] if e["bridge"]),
            "tunnels": sum(1 for e in doc["rails"] if e["tunnel"]),
            "ways_with_xing": sum(1 for e in doc["rails"] if e["xing"])}


def dry_run(place: str) -> dict:
    """Prove the budget offline: every urlopen is recorded and refused."""
    import urllib.request
    would: list = []

    class NetBlocked(RuntimeError):
        pass

    def blocked(req, *a, **k):
        url = getattr(req, "full_url", req)
        body = getattr(req, "data", None)
        would.append("%s [POST %d B]" % (url, len(body)) if body else str(url))
        raise NetBlocked("dry run: WOULD FETCH " + str(url))

    real = urllib.request.urlopen
    urllib.request.urlopen = blocked
    global _sleep
    real_sleep = _sleep
    _sleep = lambda s: None                   # noqa: E731 - the back-off, not a fetch
    try:
        pj, idx, _ = load_place(place)
        ql = query_for(pj)
        cp = cache_path(place, ql)
        cached = os.path.exists(cp) and os.path.getsize(cp) > 0
        try:
            fetch(place, ql, online=True)
        except Exception:                      # noqa: BLE001 - expected offline
            pass
    finally:
        urllib.request.urlopen = real
        _sleep = real_sleep
    distinct = sorted(set(would))
    return {"place": place, "cached": cached, "cache_file": cp,
            "query_bytes": len(ql.encode("utf-8")), "query": ql,
            "requests_attempted_incl_retries_and_mirror": len(would),
            "distinct_requests": distinct,
            "real_run_requests_if_first_endpoint_answers": 0 if cached else 1}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--place", default=DEFAULT_PLACE)
    ap.add_argument("--online", action="store_true",
                    help="allow the one Overpass request when it is not cached")
    ap.add_argument("--dry-run", action="store_true",
                    help="prove the fetch budget offline (urlopen patched)")
    a = ap.parse_args(argv)
    if a.dry_run:
        print(json.dumps(dry_run(a.place), indent=1))
        return 0
    pj, idx, proj = load_place(a.place)
    ql = query_for(pj)
    osm = fetch(a.place, ql, a.online)
    if osm is None:
        print("NOT CACHED: %s\nrerun with --online (1 Overpass request)" %
              cache_path(a.place, ql))
        return 2
    if osm.get("remark"):
        print("OVERPASS REMARK: %s" % osm["remark"])
        if "runtime error" in osm["remark"]:
            return 3
    doc = convert(osm, proj)
    xings = road_crossings(a.place, doc)
    rows = write_tiles(a.place, idx, doc)
    idx["layers"]["RAILS"] = {"file": "RAILS.JSON", "key": "rails",
                              "key2": "nodes", "tiles": rows}
    write_json(os.path.join(a.place, "tiles", "INDEX.JSON"), idx)
    rep = census(doc)
    rep["tiles"] = len(rows)
    rep["road_crossings_by_kind"] = xings
    rep["elements_in_answer"] = len(osm.get("elements", []))
    print(json.dumps(rep, indent=1, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
