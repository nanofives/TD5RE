"""geo_pois.py -- the SHOP / AMENITY / OFFICE points of a tiled geo place (roam LB).

WHY. geo_fetch.OVERPASS_QL fetches ways only (building / highway / landuse ...),
so a shop, cafe, pharmacy or bank mapped as a NODE is not on disk anywhere.
Round K measured (_archive/roam/K/osm_counts.csv) in the casco 1,275 shop nodes,
1,034 amenity nodes and 159 office nodes, against 333 shop + 215 amenity tagged
building footprints. Free roam draws every facade the same; this layer lets the
ground floor of a footprint with a real business on it get a shopfront.

WHAT. ONE Overpass request for the place bbox (exact key / key=value filters,
no regex on a key, which Overpass evaluates slowly):

    node["shop"]  node["amenity"]  node["office"]  node["healthcare"]
    way["shop"]   way["amenity"="fuel"]

`out body geom` gives node coordinates and way geometry in one pass. Points are
classified offline (CATEGORIES below, unknown values dropped and counted), then
JOINED to a footprint of the BUILDINGS tile layer: the footprint that contains
the point, else the nearest footprint edge within JOIN_MAX_M (a shop node is
often drawn on the pavement in front of its door). A way POI (shop area, fuel
forecourt) joins every footprint whose centroid falls inside it.

OUTPUT (inside <place>/, nothing else is touched)

    POIS.JSON   {"pois": [...], "buildings": [...], "fuel": [...], "census": {...}}
        pois[]:      {"id", "osm" ("n"/"w"), "x", "z", "cat", "value", "name",
                      "brand", "bld" (footprint id or null), "edge" (index of the
                      footprint edge nearest to the point, the storefront side,
                      or -1), "dist_m" (0 inside), "tags": {...}}
        buildings[]: {"id" (BUILDINGS record id), "cat" (dominant category),
                      "n" (POIs joined), "edge", "name", "cats": {cat: n},
                      "x", "z" (the dominant POI's point; absent for an area POI)}
                      sorted by id, so C can bsearch it.
        fuel[]:      {"id", "osm", "x", "z" (centroid), "name", "brand",
                      "ring": [{"x","z"}] or null (node = no forecourt outline),
                      "area_m2"}
        place frame (x = east * upm, z = north * upm), the frame every tile
        layer is written in. Footprint ids are the BUILDINGS records' "id"
        (OSM way id, negative synthetic for Overture footprints): rerun this
        tool from the cache after any geo_region rebuild of the place.
    _cache/<sha>.json   the raw Overpass answer (geo_fetch cache key scheme).

NETWORK DISCIPLINE (memory reference_geo_fetch_budget_dryrun_and_stale_baseline)
    --dry-run   urllib.request.urlopen is patched to record and raise: prints
                every request a real run would make (expected: exactly 1
                Overpass POST when uncached, 0 when cached).
    --online    required for any request not in the cache.

    python -I re/tools/geo_pois.py --dry-run
    python -I re/tools/geo_pois.py --online      # the one network call
    python -I re/tools/geo_pois.py               # rerun from the cache (join + census)
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import geo_rails  # noqa: E402  (endpoints, UA, cache key scheme, place loader)
from geo_common import CACHE_ROOT, write_json  # noqa: E402

DEFAULT_PLACE = os.path.join(CACHE_ROOT, "la_plata_partido")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

POI_QL = """[out:json][timeout:180];
(
  node["shop"](%(bbox)s);
  node["amenity"](%(bbox)s);
  node["office"](%(bbox)s);
  node["healthcare"](%(bbox)s);
  way["shop"](%(bbox)s);
  way["amenity"="fuel"](%(bbox)s);
);
out body geom;
"""

JOIN_MAX_M = 15.0          # a point this far outside every footprint stays unjoined
GRID_M = 40.0              # footprint bucket size for the join

# Category of a POI, first match wins. The C side picks the shopfront colour /
# sign by category; PRIORITY decides the dominant category of a footprint with
# several POIs on it.
FOOD_AMENITY = ("restaurant", "fast_food", "cafe", "bar", "pub", "ice_cream",
                "biergarten", "food_court")
FOOD_SHOP = ("bakery", "pastry", "confectionery", "coffee", "deli", "butcher",
             "greengrocer", "cheese", "chocolate", "beverages", "alcohol", "wine",
             "seafood", "frozen_food", "dairy", "spices", "tea", "pasta")
KIOSK_SHOP = ("kiosk", "newsagent", "convenience", "tobacco", "lottery")
HEALTH_AMENITY = ("clinic", "doctors", "dentist", "veterinary")
SKIP_SHOP = ("vacant", "no", "none", "disused")
PRIORITY = ("fuel", "pharmacy", "bank", "food", "kiosk", "health", "shop", "office")

KEEP_TAGS = ("shop", "amenity", "office", "healthcare", "name", "brand", "operator",
             "cuisine", "level", "addr:street", "addr:housenumber", "opening_hours",
             "fuel:diesel", "compressed_air", "building")


def category(t: dict):
    """(category, value) or (None, reason)."""
    am = t.get("amenity")
    sh = t.get("shop")
    if am == "fuel":
        return "fuel", am
    if am == "pharmacy" or t.get("healthcare") == "pharmacy":
        return "pharmacy", "pharmacy"
    if am in ("bank", "bureau_de_change"):
        return "bank", am
    if am in FOOD_AMENITY:
        return "food", am
    if sh and sh not in SKIP_SHOP:
        if sh in FOOD_SHOP:
            return "food", sh
        if sh in KIOSK_SHOP:
            return "kiosk", sh
        if sh == "chemist":
            return "pharmacy", sh
        return "shop", sh
    if am in HEALTH_AMENITY or (t.get("healthcare") and not am):
        return "health", am or t.get("healthcare")
    if t.get("office") and t.get("office") not in ("no", "vacant"):
        return "office", t.get("office")
    if am:
        return None, "amenity=" + am
    return None, "other"


def query_for(pj: dict) -> str:
    b = pj["bbox"]
    bbox = "%.6f,%.6f,%.6f,%.6f" % (b["south"], b["west"], b["north"], b["east"])
    return POI_QL % {"bbox": bbox}


# ---------------------------------------------------------------- geometry

def _pip(x, z, pts):
    ins = False
    n = len(pts)
    j = n - 1
    for i in range(n):
        xi, zi = pts[i]
        xj, zj = pts[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi) + xi:
            ins = not ins
        j = i
    return ins


def _seg_d2(px, pz, ax, az, bx, bz):
    dx, dz = bx - ax, bz - az
    L = dx * dx + dz * dz
    t = 0.0 if L <= 0 else max(0.0, min(1.0, ((px - ax) * dx + (pz - az) * dz) / L))
    qx, qz = ax + t * dx - px, az + t * dz - pz
    return qx * qx + qz * qz


def _nearest_edge(x, z, pts):
    best, bi = None, -1
    n = len(pts)
    for i in range(n):
        a, b = pts[i], pts[(i + 1) % n]
        if a == b:
            continue
        d = _seg_d2(x, z, a[0], a[1], b[0], b[1])
        if best is None or d < best:
            best, bi = d, i
    return bi, (math.sqrt(best) if best is not None else 1e30)


def _ring(points):
    pts = [(p["x"], p["z"]) for p in points]
    if len(pts) > 1 and pts[0] == pts[-1]:
        pts = pts[:-1]
    return pts


def _centroid(pts):
    sx = sum(p[0] for p in pts) / len(pts)
    sz = sum(p[1] for p in pts) / len(pts)
    return sx, sz


def _area(pts):
    a = 0.0
    for i in range(len(pts)):
        x0, z0 = pts[i]
        x1, z1 = pts[(i + 1) % len(pts)]
        a += x0 * z1 - x1 * z0
    return abs(a) * 0.5


class Footprints:
    """BUILDINGS tile layer, loaded tile by tile on demand, bucketed on GRID_M."""

    def __init__(self, place: str, idx: dict, upm: float):
        lay = idx["layers"]["BUILDINGS"]
        self.place = place
        self.rows = lay["tiles"]
        self.base = os.path.splitext(lay["file"])[0]
        self.key = lay["key"]
        self.cell = GRID_M * upm
        self.upm = upm
        self.loaded = set()
        self.grid: dict = {}
        self.blds: list = []

    def _load_for(self, x, z, r):
        for row in self.rows:
            ix, iz, n, minx, minz, maxx, maxz = row[:7]
            if (ix, iz) in self.loaded:
                continue
            if x + r < minx or x - r > maxx or z + r < minz or z - r > maxz:
                continue
            self.loaded.add((ix, iz))
            p = os.path.join(self.place, "tiles", "%s_%d_%d.JSON" % (self.base, ix, iz))
            with open(p, encoding="utf-8") as f:
                doc = json.load(f)
            for b in doc.get(self.key, []):
                if b.get("part"):
                    continue          # building:part, the outline carries the shop
                pts = _ring(b.get("points") or [])
                if len(pts) < 3:
                    continue
                k = len(self.blds)
                xs = [p[0] for p in pts]
                zs = [p[1] for p in pts]
                bb = (min(xs), min(zs), max(xs), max(zs))
                self.blds.append((b["id"], pts, bb, b))
                for gx in range(int(math.floor(bb[0] / self.cell)), int(math.floor(bb[2] / self.cell)) + 1):
                    for gz in range(int(math.floor(bb[1] / self.cell)), int(math.floor(bb[3] / self.cell)) + 1):
                        self.grid.setdefault((gx, gz), []).append(k)

    def near(self, x, z, r):
        self._load_for(x, z, r)
        seen = set()
        for gx in range(int(math.floor((x - r) / self.cell)), int(math.floor((x + r) / self.cell)) + 1):
            for gz in range(int(math.floor((z - r) / self.cell)), int(math.floor((z + r) / self.cell)) + 1):
                for k in self.grid.get((gx, gz), ()):
                    if k not in seen:
                        seen.add(k)
                        yield self.blds[k]

    def join_point(self, x, z):
        """(bld id, edge, dist_m) or (None, -1, None)."""
        r = JOIN_MAX_M * self.upm
        best = None
        for bid, pts, bb, _ in self.near(x, z, r):
            if bb[0] <= x <= bb[2] and bb[1] <= z <= bb[3] and _pip(x, z, pts):
                e, _d = _nearest_edge(x, z, pts)
                # Smallest containing footprint wins (a shop inside a block-sized hull).
                a = _area(pts)
                if best is None or best[3] > 0 or a < best[4]:
                    best = (bid, e, 0.0, 0.0, a)
                continue
            if best is not None and best[3] == 0:
                continue
            e, d = _nearest_edge(x, z, pts)
            if d <= r and (best is None or d < best[3]):
                best = (bid, e, d / self.upm, d, 0.0)
        if best is None:
            return None, -1, None
        return best[0], best[1], round(best[2], 2)

    def inside_ring(self, ring):
        xs = [p[0] for p in ring]
        zs = [p[1] for p in ring]
        cx, cz = (min(xs) + max(xs)) * 0.5, (min(zs) + max(zs)) * 0.5
        r = max(max(xs) - min(xs), max(zs) - min(zs))
        out = []
        for bid, pts, bb, _ in self.near(cx, cz, r):
            c = _centroid(pts)
            if _pip(c[0], c[1], ring):
                out.append(bid)
        return sorted(set(out))


# ---------------------------------------------------------------- convert

def _in_casco(lat, lon, poly):
    if not poly:
        return False
    return _pip(lon, lat, [(c["lon"], c["lat"]) for c in poly])


def convert(osm: dict, proj, fp: Footprints, casco):
    pois, fuel, dropped = [], [], {}
    for el in osm.get("elements", []):
        t = el.get("tags", {})
        cat, val = category(t)
        if el["type"] == "node":
            lat, lon = el["lat"], el["lon"]
            ring = None
        elif el["type"] == "way" and el.get("geometry"):
            geom = [(g["lat"], g["lon"]) for g in el["geometry"] if g]
            lat = sum(g[0] for g in geom) / len(geom)
            lon = sum(g[1] for g in geom) / len(geom)
            ring = [proj.to_world(a, b) for a, b in geom]
            if len(ring) > 1 and ring[0] == ring[-1]:
                ring = ring[:-1]
        else:
            continue
        if cat is None:
            dropped[val] = dropped.get(val, 0) + 1
            continue
        x, z = proj.to_world(lat, lon)
        rec = {"id": el["id"], "osm": el["type"][0], "x": round(x, 2), "z": round(z, 2),
               "cat": cat, "value": val, "name": t.get("name"), "brand": t.get("brand"),
               "casco": _in_casco(lat, lon, casco),
               "tags": {k: t[k] for k in KEEP_TAGS if k in t}}
        if ring is not None and len(ring) >= 3 and t.get("building") is None:
            # An AREA (shop site, fuel forecourt): every footprint centred inside.
            rec["blds"] = fp.inside_ring(ring)
            rec["bld"], rec["edge"], rec["dist_m"] = (rec["blds"][0] if rec["blds"] else None), -1, None
        else:
            rec["bld"], rec["edge"], rec["dist_m"] = fp.join_point(x, z)
        pois.append(rec)
        if cat == "fuel":
            fuel.append({"id": el["id"], "osm": el["type"][0], "x": rec["x"], "z": rec["z"],
                         "name": t.get("name"), "brand": t.get("brand"), "casco": rec["casco"],
                         "ring": ([{"x": round(a, 2), "z": round(b, 2)} for a, b in ring]
                                  if ring is not None and len(ring) >= 3 else None),
                         "area_m2": (round(_area(ring) / (proj.units_per_metre ** 2), 1)
                                     if ring is not None and len(ring) >= 3 else None),
                         "bld": rec["bld"]})
    pois.sort(key=lambda r: (r["osm"], r["id"]))
    fuel.sort(key=lambda r: (r["osm"], r["id"]))

    # Footprint -> dominant category. The on-disk footprint tags (shop= /
    # amenity= on the building itself) are NOT folded in here: C reads those
    # from the BUILDINGS record directly, so this file only adds what is new.
    per: dict = {}
    for p in pois:
        ids = p.get("blds") or ([p["bld"]] if p["bld"] is not None else [])
        for bid in ids:
            e = per.setdefault(bid, {"id": bid, "cats": {}, "n": 0, "best": None})
            e["n"] += 1
            e["cats"][p["cat"]] = e["cats"].get(p["cat"], 0) + 1
            rank = PRIORITY.index(p["cat"])
            if e["best"] is None or rank < e["best"][0] or (rank == e["best"][0] and p["edge"] >= 0 and e["best"][1]["edge"] < 0):
                e["best"] = (rank, p)
    blds = []
    for bid in sorted(per):
        e = per[bid]
        p = e["best"][1]
        # x / z: the dominant POI's point (place frame), so the reader picks the
        # storefront wall nearest to it after its own ring decimation / winding.
        # An AREA POI (shop site) has no door point: x / z omitted.
        rec = {"id": bid, "cat": p["cat"], "n": e["n"],
               "edge": p["edge"] if p.get("bld") == bid else -1,
               "name": p["name"], "cats": e["cats"]}
        if p.get("bld") == bid and not p.get("blds"):
            rec["x"], rec["z"] = p["x"], p["z"]
        blds.append(rec)
    return pois, blds, fuel, dropped


def census(pois, blds, fuel, dropped, fp: Footprints) -> dict:
    def by(key, rows):
        out: dict = {}
        for r in rows:
            out[r[key]] = out.get(r[key], 0) + 1
        return dict(sorted(out.items()))
    cas = [p for p in pois if p["casco"]]
    ondisk_tagged = sum(1 for b in fp.blds if b[3].get("shop") or b[3].get("amenity"))
    return {
        "pois": len(pois), "pois_casco": len(cas),
        "by_cat": by("cat", pois), "by_cat_casco": by("cat", cas),
        "by_osm_type": by("osm", pois),
        "joined": sum(1 for p in pois if p["bld"] is not None),
        "joined_casco": sum(1 for p in cas if p["bld"] is not None),
        "joined_inside": sum(1 for p in pois if p["dist_m"] == 0.0),
        "joined_near": sum(1 for p in pois if p["dist_m"] not in (None, 0.0)),
        "unjoined": sum(1 for p in pois if p["bld"] is None),
        "unjoined_casco": sum(1 for p in cas if p["bld"] is None),
        "footprints_with_poi": len(blds),
        "footprints_with_poi_by_cat": by("cat", blds),
        "fuel_sites": len(fuel), "fuel_with_forecourt_ring": sum(1 for f in fuel if f["ring"]),
        "dropped_by_value": dict(sorted(dropped.items(), key=lambda kv: -kv[1])[:40]),
        "dropped_total": sum(dropped.values()),
        "building_tiles_loaded": len(fp.loaded),
        "loaded_footprints_already_tagged_shop_or_amenity": ondisk_tagged,
        "join_max_m": JOIN_MAX_M,
    }


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
        ql = query_for(pj)
        cp = geo_rails.cache_path(place, ql)
        cached = os.path.exists(cp) and os.path.getsize(cp) > 0
        try:
            geo_rails.fetch(place, ql, online=True)
        except Exception:                      # noqa: BLE001 - expected offline
            pass
    finally:
        urllib.request.urlopen = real
        geo_rails._sleep = real_sleep
    return {"place": place, "cached": cached, "cache_file": cp,
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
    ap.add_argument("--casco", default=os.path.join(REPO, "_archive", "roam", "F0a", "casco_poly.json"),
                    help="casco corner polygon for the casco census columns (optional)")
    a = ap.parse_args(argv)
    if a.dry_run:
        print(json.dumps(dry_run(a.place), indent=1))
        return 0
    pj, idx, proj = geo_rails.load_place(a.place)
    ql = query_for(pj)
    osm = geo_rails.fetch(a.place, ql, a.online)
    if osm is None:
        print("NOT CACHED: %s\nrerun with --online (1 Overpass request)" % geo_rails.cache_path(a.place, ql))
        return 2
    if osm.get("remark"):
        print("OVERPASS REMARK: %s" % osm["remark"])
        if "runtime error" in osm["remark"]:
            return 3
    casco = None
    if os.path.exists(a.casco):
        with open(a.casco, encoding="utf-8") as f:
            casco = json.load(f)["corners"]
    fp = Footprints(a.place, idx, pj["projection"]["units_per_metre"])
    pois, blds, fuel, dropped = convert(osm, proj, fp, casco)
    rep = census(pois, blds, fuel, dropped, fp)
    rep["elements_in_answer"] = len(osm.get("elements", []))
    rep["casco_polygon"] = a.casco if casco else None
    write_json(os.path.join(a.place, "POIS.JSON"),
               {"pois": pois, "buildings": blds, "fuel": fuel, "census": rep})
    print(json.dumps(rep, indent=1, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
