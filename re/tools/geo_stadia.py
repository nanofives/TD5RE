"""geo_stadia.py -- everything OSM maps INSIDE the stadiums of a tiled geo place (roam PB).

WHY. The tile layers keep a stadium as what geo_fetch whitelists: the
leisure=stadium area (AREAS), building=stadium footprints (BUILDINGS) and the
S3D_RELATIONS.JSON outline. Round P PB asked whether OSM maps more than that
for the Estadio Unico (building:part tiers, the roof rings, the masts, seating
areas, entrances, steps). This layer answers it: ONE Overpass request, every
node / way / relation in a small box round each stadium, with full tags and
geometry, kept when it lies inside the stadium ring (+ MARGIN_M).

WHAT. Stadium rings are read from disk, not fetched: leisure=stadium ways of
tiles/AREAS_*.JSON and leisure=stadium relations of PARK_RELATIONS.JSON. One
query:

    ( nwr(<bbox stadium 1>); nwr(<bbox 2>); ... );  out body geom;

OUTPUT (inside <place>/, nothing else is touched)

    STADIA.JSON  {"stadia": [{"id", "osm" ("w"/"r"), "name", "area_m2",
                   "ring": [{"x","z"}], "elements": [{"type", "id", "tags",
                   "points": [{"x","z"}] (way / relation member ways: their
                   geometry; node: one point), "inner": bool}]}],
                  "census": {...}}
        place SOURCE frame (x = east * upm, z = north * upm), like every tile
        layer. The C side (td5_roam_stadium.c) does not need this file: it
        builds from the tile layers. It is the evidence of what OSM has, and the
        place a later round reads tier / roof parts from if they get mapped.
    _cache/<sha>.json   the raw Overpass answer (geo_fetch cache key scheme).

NETWORK DISCIPLINE (memory reference_geo_fetch_budget_dryrun_and_stale_baseline)
    --dry-run   urllib.request.urlopen is patched to record and raise: prints
                every request a real run would make (expected: exactly 1
                Overpass POST when uncached, 0 when cached).
    --online    required for any request not in the cache.

    python -I re/tools/geo_stadia.py --dry-run
    python -I re/tools/geo_stadia.py --online      # the one network call
    python -I re/tools/geo_stadia.py               # rerun from the cache
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import geo_rails  # noqa: E402  (endpoints, UA, cache key scheme, place loader)
from geo_common import CACHE_ROOT, write_json  # noqa: E402

DEFAULT_PLACE = os.path.join(CACHE_ROOT, "la_plata_partido")
MARGIN_M = 25.0      # box margin round each ring, and the "inside" tolerance
MIN_AREA_M2 = 5000.0  # a smaller leisure=stadium is a club ground, not a stadium

DETAIL_KEYS = ("building", "building:part", "roof:shape", "roof:height", "height", "min_height",
               "building:levels", "building:min_level", "leisure", "man_made", "entrance",
               "barrier", "sport", "surface", "seats", "capacity", "amenity", "highway", "name",
               "colour", "building:colour", "roof:colour", "material", "roof:material")


def _area_m2(pts, upm):
    a = 0.0
    for i in range(len(pts)):
        x0, z0 = pts[i]["x"], pts[i]["z"]
        x1, z1 = pts[(i + 1) % len(pts)]["x"], pts[(i + 1) % len(pts)]["z"]
        a += x0 * z1 - x1 * z0
    return abs(a) * 0.5 / (upm * upm)


def _pip(x, z, pts):
    ins = False
    n = len(pts)
    for i in range(n):
        ax, az = pts[i]["x"], pts[i]["z"]
        bx, bz = pts[i - 1]["x"], pts[i - 1]["z"]
        if (az > z) != (bz > z) and x < (bx - ax) * (z - az) / (bz - az) + ax:
            ins = not ins
    return ins


def _seg_d(px, pz, a, b):
    dx, dz = b["x"] - a["x"], b["z"] - a["z"]
    L = dx * dx + dz * dz
    t = 0.0 if L <= 0 else max(0.0, min(1.0, ((px - a["x"]) * dx + (pz - a["z"]) * dz) / L))
    return math.hypot(px - a["x"] - t * dx, pz - a["z"] - t * dz)


def _near_ring(x, z, ring, tol):
    if _pip(x, z, ring):
        return True
    return min(_seg_d(x, z, ring[i], ring[(i + 1) % len(ring)]) for i in range(len(ring))) <= tol


def stadium_rings(place: str, upm: float) -> list:
    """leisure=stadium rings on disk, sorted by (osm, id)."""
    out = {}
    for f in sorted(glob.glob(os.path.join(place, "tiles", "AREAS_*.JSON"))):
        with open(f, encoding="utf-8") as fh:
            for a in json.load(fh).get("areas", []):
                if a.get("leisure") == "stadium" and len(a.get("points", [])) >= 4:
                    out[("w", a["id"])] = {"id": a["id"], "osm": "w", "name": a.get("name"), "ring": a["points"]}
    pr = os.path.join(place, "PARK_RELATIONS.JSON")
    if os.path.exists(pr):
        with open(pr, encoding="utf-8") as fh:
            for r in json.load(fh).get("relations", []):
                if r.get("leisure") == "stadium" and r.get("outer"):
                    out[("r", r["id"])] = {"id": r["id"], "osm": "r", "name": r.get("name"), "ring": r["outer"][0]}
    rows = []
    for k in sorted(out):
        s = out[k]
        s["area_m2"] = round(_area_m2(s["ring"], upm), 1)
        if s["area_m2"] >= MIN_AREA_M2:
            rows.append(s)
    return rows


def query_for(pj: dict, rings: list) -> str:
    pr = pj["projection"]
    upm = pr["units_per_metre"]
    boxes = []
    for s in rings:
        xs = [p["x"] for p in s["ring"]]
        zs = [p["z"] for p in s["ring"]]
        m = MARGIN_M * upm
        lat = lambda z: pr["lat0"] + z / upm / pr["m_per_deg_lat"]   # noqa: E731
        lon = lambda x: pr["lon0"] + x / upm / pr["m_per_deg_lon"]   # noqa: E731
        boxes.append("  nwr(%.6f,%.6f,%.6f,%.6f);" % (lat(min(zs) - m), lon(min(xs) - m), lat(max(zs) + m), lon(max(xs) + m)))
    return "[out:json][timeout:120];\n(\n%s\n);\nout body geom;\n" % "\n".join(boxes)


def convert(osm: dict, proj, rings: list, upm: float):
    tol = MARGIN_M * upm
    for s in rings:
        s["elements"] = []
    for el in osm.get("elements", []):
        t = el.get("tags") or {}
        if not t:
            continue
        if el["type"] == "node":
            pts = [geo_rails._pt(proj, el["lat"], el["lon"])]
        elif el["type"] == "way":
            pts = [geo_rails._pt(proj, g["lat"], g["lon"]) for g in (el.get("geometry") or [])]
        else:
            pts = []
            for m in el.get("members", []):
                pts += [geo_rails._pt(proj, g["lat"], g["lon"]) for g in (m.get("geometry") or [])]
        if not pts:
            continue
        cx = sum(p["x"] for p in pts) / len(pts)
        cz = sum(p["z"] for p in pts) / len(pts)
        for s in rings:
            if _near_ring(cx, cz, s["ring"], tol):
                s["elements"].append({"type": el["type"], "id": el["id"],
                                      "tags": {k: t[k] for k in sorted(t)},
                                      "points": pts, "inner": _pip(cx, cz, s["ring"])})
    for s in rings:
        s["elements"].sort(key=lambda e: (e["type"], e["id"]))


def census(rings: list) -> dict:
    rep = {}
    for s in rings:
        c = {"elements": len(s["elements"]), "by_key": {}}
        for e in s["elements"]:
            for k in DETAIL_KEYS:
                if k in e["tags"]:
                    kv = "%s=%s" % (k, e["tags"][k]) if k not in ("name", "height", "seats", "capacity") else k
                    c["by_key"][kv] = c["by_key"].get(kv, 0) + 1
        c["by_key"] = dict(sorted(c["by_key"].items()))
        rep["%s%d %s" % (s["osm"], s["id"], s["name"])] = c
    return rep


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
    real_sleep = geo_rails._sleep
    urllib.request.urlopen = blocked
    geo_rails._sleep = lambda s: None          # noqa: E731 - the back-off, not a fetch
    try:
        pj, _, _ = geo_rails.load_place(place)
        rings = stadium_rings(place, pj["projection"]["units_per_metre"])
        ql = query_for(pj, rings)
        cp = geo_rails.cache_path(place, ql)
        cached = os.path.exists(cp) and os.path.getsize(cp) > 0
        try:
            geo_rails.fetch(place, ql, online=True)
        except Exception:                      # noqa: BLE001 - expected offline
            pass
    finally:
        urllib.request.urlopen = real
        geo_rails._sleep = real_sleep
    return {"place": place, "stadia": [(s["osm"], s["id"], s["name"], s["area_m2"]) for s in rings],
            "cached": cached, "cache_file": cp,
            "query_bytes": len(ql.encode("utf-8")), "query": ql,
            "requests_attempted_incl_retries_and_mirror": len(would),
            "distinct_requests": sorted(set(would)),
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
        print(json.dumps(dry_run(a.place), indent=1, ensure_ascii=False))
        return 0
    pj, _, proj = geo_rails.load_place(a.place)
    upm = pj["projection"]["units_per_metre"]
    rings = stadium_rings(a.place, upm)
    ql = query_for(pj, rings)
    osm = geo_rails.fetch(a.place, ql, a.online)
    if osm is None:
        print("NOT CACHED: %s\nrerun with --online (1 Overpass request)" % geo_rails.cache_path(a.place, ql))
        return 2
    if osm.get("remark"):
        print("OVERPASS REMARK: %s" % osm["remark"])
        if "runtime error" in osm["remark"]:
            return 3
    convert(osm, proj, rings, upm)
    rep = census(rings)
    write_json(os.path.join(a.place, "STADIA.JSON"), {"stadia": rings, "census": rep,
                                                       "elements_in_answer": len(osm.get("elements", []))})
    print(json.dumps(rep, indent=1, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
