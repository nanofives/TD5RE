"""geo_plaza_parks.py -- PARK RELATIONS + PLAZA FURNITURE of a tiled geo place (roam LD).

WHY. geo_fetch.OVERPASS_QL asks for ways only, so two things are not on disk:

  1. a green mapped as a MULTIPOLYGON relation (an outline split over several
     ways, or a park with holes = inner rings). Round K counted 25 such
     landuse / leisure / natural / amenity relations in the casco, 79 in the
     bbox (_archive/roam/K/osm_counts.csv row relation_landuse_leisure_natural).
     AREAS.JSON never saw them, so the lawn, the plaza trees, the plaza veto
     and the infill treat them as ordinary blocks.
  2. the plaza furniture: benches, fountains, memorials / monuments / statues,
     artworks, playgrounds, bandstands (casco 89 / 17 / 13 / 13 / 43, K
     osm_counts.csv), plus flagpoles and obelisks (the "mastil" of an Argentine
     plaza, not counted by K).

WHAT. ONE Overpass request for the place bbox, `out body geom` (member and way
geometry inlined, so no second pass). Rings are joined with the same code as
geo_s3d_relations._join_rings (an open chain is reported, never guessed closed).

OUTPUT (inside <place>/, nothing else is touched)

    PARK_RELATIONS.JSON  {"relations": [...], "census": {...}}
        relations[]: every field of an AREAS.JSON record (geo_fetch.convert_osm
            areas branch: id, name, kind, leisure, landuse, natural, sport,
            surface, amenity, tourism, historic, man_made, barrier, access,
            layer, closed, tags) with "points" = the LARGEST outer ring, plus
            "relation": true, "outer": [every outer ring], "inner": [every
            inner ring], "open_chains", "area_class" (green / water / build /
            other, see GREEN_KINDS) and "keep" (would geo_fetch's whitelist
            keep this kind as an area). Rings in the place frame
            (x = east * upm, z = north * upm), like every tile layer.
    PLAZA_FURNITURE.JSON {"items": [...], "census": {...}}
        items[]: {"id", "osm" (node / way / relation), "kind" (bench /
            fountain / memorial / monument / statue / artwork / playground /
            bandstand / picnic_table / fitness_station / flagpole / obelisk),
            "sub" (memorial= / artwork_type= / fountain= value), "name",
            "x", "z" (node point, or the centroid of the way / outer ring),
            "points" (the closed ring of an area feature, else []),
            "height_m" (height= tag when present), "landmark" (named, or a
            monument / statue / obelisk / named fountain), "tags" (KEEP_TAGS)}
    _cache/<sha>.json    the raw Overpass answer (geo_rails cache key scheme):
                         a rerun is offline.

NETWORK DISCIPLINE (memory reference_geo_fetch_budget_dryrun_and_stale_baseline)
    --dry-run   urllib.request.urlopen is patched to record and raise: prints
                every request a real run would make (expected: exactly 1
                Overpass POST when uncached, 0 when cached).
    --online    required for any request not in the cache.

    python -I re/tools/geo_plaza_parks.py --dry-run
    python -I re/tools/geo_plaza_parks.py --online      # the one network call
    python -I re/tools/geo_plaza_parks.py               # rerun from the cache
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import geo_rails  # noqa: E402  (endpoints, UA, cache key scheme, place loader)
import geo_s3d_relations  # noqa: E402  (ring join)
from geo_common import CACHE_ROOT, write_json  # noqa: E402


def _geo_fetch_consts() -> dict:
    """AREA_KEEP_* / AREA_TAG_KEYS read from geo_fetch.py's SOURCE (one source of
    truth, and no numpy import: `python -I` has no site-packages)."""
    import ast
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "geo_fetch.py")
    with open(src, encoding="utf-8") as f:
        tree = ast.parse(f.read())
    want = ("AREA_KEEP_LEISURE", "AREA_KEEP_LANDUSE", "AREA_KEEP_NATURAL",
            "AREA_KEEP_AMENITY", "AREA_TAG_KEYS")
    out = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1                 and getattr(node.targets[0], "id", None) in want:
            v = node.value
            if isinstance(v, ast.Call):            # frozenset((...))
                v = v.args[0]
            out[node.targets[0].id] = ast.literal_eval(v)
    missing = [w for w in want if w not in out]
    if missing:
        raise SystemExit("geo_plaza_parks: geo_fetch.py lost %s" % missing)
    return out


_C = _geo_fetch_consts()
AREA_KEEP_LEISURE = frozenset(_C["AREA_KEEP_LEISURE"])
AREA_KEEP_LANDUSE = frozenset(_C["AREA_KEEP_LANDUSE"])
AREA_KEEP_NATURAL = frozenset(_C["AREA_KEEP_NATURAL"])
AREA_KEEP_AMENITY = frozenset(_C["AREA_KEEP_AMENITY"])
AREA_TAG_KEYS = tuple(_C["AREA_TAG_KEYS"])


def _keep_tags(t: dict, keys) -> dict:
    """geo_fetch._keep_tags: the subset of `t` in `keys` with a non-empty value, raw strings."""
    return {k: t[k] for k in keys if t.get(k) not in (None, "")}

DEFAULT_PLACE = os.path.join(CACHE_ROOT, "la_plata_partido")
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# Explicit keys, no regex-on-key (slow on Overpass). Value regexes are cheap.
QL = """[out:json][timeout:180];
(
  relation["type"="multipolygon"]["leisure"](%(bbox)s);
  relation["type"="multipolygon"]["landuse"](%(bbox)s);
  relation["type"="multipolygon"]["natural"](%(bbox)s);
  relation["type"="multipolygon"]["amenity"](%(bbox)s);
  relation["type"="multipolygon"]["place"="square"](%(bbox)s);
  node["amenity"="bench"](%(bbox)s);
  nwr["amenity"="fountain"](%(bbox)s);
  nwr["historic"~"^(memorial|monument|statue)$"](%(bbox)s);
  nwr["tourism"="artwork"](%(bbox)s);
  nwr["leisure"~"^(playground|bandstand|picnic_table|fitness_station)$"](%(bbox)s);
  nwr["man_made"~"^(flagpole|obelisk)$"](%(bbox)s);
);
out body geom;
"""

# What the scenery does with a relation's kind (same vocabulary as AREAS kind).
GREEN_KINDS = frozenset((
    "park", "garden", "playground", "common", "recreation_ground", "dog_park",
    "village_green", "grass", "meadow", "grassland", "forest", "wood", "scrub",
    "heath", "nature_reserve", "cemetery", "golf_course", "pitch", "picnic_site",
    "allotments", "orchard", "plant_nursery", "square",
))
WATER_KINDS = frozenset(("water", "wetland", "marsh", "swamp", "reservoir", "basin"))

FURN_TAGS = ("amenity", "historic", "memorial", "tourism", "artwork_type", "leisure",
             "man_made", "fountain", "name", "name:es", "subject", "subject:wikidata",
             "height", "material", "inscription", "start_date", "wikidata",
             "wikipedia", "drinking_water", "backrest", "covered", "access")

# Landmarks worth a census line (Mariano's list in _archive/roam/L/BRIEF.md LD).
WATCH = ("Dardo Rocha", "San Mart", "Moreno", "Garibaldi", "Italia", "Rocha",
         "Nereida", "Fundacional", "Belgrano", "Rivadavia", "Mitre", "Sarmiento",
         "Bosque", "Fuente", "Monumento", "Obelisco", "Malvinas", "Eva", "Per")


def query_for(pj: dict) -> str:
    b = pj["bbox"]
    bbox = "%.6f,%.6f,%.6f,%.6f" % (b["south"], b["west"], b["north"], b["east"])
    return QL % {"bbox": bbox}


def _num(v):
    try:
        return float(str(v).split(";")[0].strip().rstrip("m").strip())
    except (TypeError, ValueError):
        return None


def _ring_area(ring) -> float:
    a = 0.0
    for i in range(len(ring) - 1):
        a += ring[i]["x"] * ring[i + 1]["z"] - ring[i + 1]["x"] * ring[i]["z"]
    return abs(a) * 0.5


def _centroid(pts) -> tuple[float, float]:
    """Area centroid of a closed ring, the vertex mean for an open chain."""
    if len(pts) >= 4 and pts[0] == pts[-1]:
        a = cx = cz = 0.0
        for i in range(len(pts) - 1):
            c = pts[i]["x"] * pts[i + 1]["z"] - pts[i + 1]["x"] * pts[i]["z"]
            a += c
            cx += (pts[i]["x"] + pts[i + 1]["x"]) * c
            cz += (pts[i]["z"] + pts[i + 1]["z"]) * c
        if abs(a) > 1e-9:
            return round(cx / (3.0 * a), 2), round(cz / (3.0 * a), 2)
    n = max(1, len(pts))
    return (round(sum(p["x"] for p in pts) / n, 2), round(sum(p["z"] for p in pts) / n, 2))


def _area_class(kind) -> str:
    if kind in GREEN_KINDS:
        return "green"
    if kind in WATER_KINDS:
        return "water"
    if kind in ("residential", "commercial", "retail", "industrial", "construction",
                "education", "institutional", "religious", "military", "school",
                "university", "college", "hospital", "prison", "parking", "farmyard",
                "brownfield", "landfill", "quarry"):
        return "build"
    return "other"


def convert_relation(el: dict, proj, upm: float) -> dict:
    t = el.get("tags", {})
    outer_w, inner_w = [], []
    for m in el.get("members", []):
        if m.get("type") != "way" or not m.get("geometry"):
            continue
        geom = [(g["lat"], g["lon"]) for g in m["geometry"] if g]
        (inner_w if (m.get("role") or "") == "inner" else outer_w).append(geom)
    outer, open_o = geo_s3d_relations._join_rings(outer_w)
    inner, open_i = geo_s3d_relations._join_rings(inner_w)
    outer_p = [geo_s3d_relations._ring_pts(proj, r) for r in outer]
    inner_p = [geo_s3d_relations._ring_pts(proj, r) for r in inner]
    outer_p.sort(key=_ring_area, reverse=True)
    leisure, landuse, natural = t.get("leisure"), t.get("landuse"), t.get("natural")
    amenity = t.get("amenity")
    kind = (leisure or landuse or natural
            or (amenity if amenity in AREA_KEEP_AMENITY else None)
            or ("square" if t.get("place") == "square" else None))
    keep = (leisure in AREA_KEEP_LEISURE or landuse in AREA_KEEP_LANDUSE
            or natural in AREA_KEEP_NATURAL or amenity in AREA_KEEP_AMENITY
            or t.get("place") == "square")
    return {
        "id": el["id"], "relation": True, "name": t.get("name"), "kind": kind,
        "leisure": leisure, "landuse": landuse, "natural": natural,
        "sport": t.get("sport"), "surface": t.get("surface"), "amenity": amenity,
        "tourism": t.get("tourism"), "historic": t.get("historic"),
        "man_made": t.get("man_made"), "barrier": t.get("barrier"),
        "access": t.get("access"), "layer": geo_rails._int_tag(t.get("layer"), 0),
        "closed": bool(outer_p),
        "points": outer_p[0] if outer_p else [],
        "outer": outer_p, "inner": inner_p, "open_chains": open_o + open_i,
        "area_class": _area_class(kind), "keep": bool(keep),
        "area_m2": round((sum(_ring_area(r) for r in outer_p)
                          - sum(_ring_area(r) for r in inner_p)) / (upm * upm), 1),
        "tags": _keep_tags(t, AREA_TAG_KEYS),
    }


def _furn_kind(t: dict):
    """(kind, sub) of a furniture element, or (None, None)."""
    h = t.get("historic")
    if h in ("memorial", "monument", "statue"):
        sub = t.get("memorial") or t.get("artwork_type")
        if h == "memorial" and sub == "statue":
            return "statue", sub
        return h, sub
    mm = t.get("man_made")
    if mm in ("obelisk", "flagpole"):
        return mm, None
    if t.get("amenity") == "fountain":
        return "fountain", t.get("fountain")
    if t.get("tourism") == "artwork":
        sub = t.get("artwork_type")
        return ("statue" if sub in ("statue", "bust") else "artwork"), sub
    le = t.get("leisure")
    if le in ("playground", "bandstand", "picnic_table", "fitness_station"):
        return le, None
    if t.get("amenity") == "bench":
        return "bench", None
    return None, None


def convert_item(el: dict, proj) -> dict | None:
    t = el.get("tags", {})
    kind, sub = _furn_kind(t)
    if kind is None:
        return None
    pts = []
    if el["type"] == "node":
        x, z = proj.to_world(el["lat"], el["lon"])
        x, z = round(x, 2), round(z, 2)
    else:
        if el["type"] == "way":
            ways = [[(g["lat"], g["lon"]) for g in el.get("geometry", []) if g]]
        else:
            ways = [[(g["lat"], g["lon"]) for g in m.get("geometry", []) if g]
                    for m in el.get("members", []) if m.get("type") == "way"
                    and (m.get("role") or "outer") != "inner" and m.get("geometry")]
        rings, _ = geo_s3d_relations._join_rings(ways)
        if rings:
            pts = max((geo_s3d_relations._ring_pts(proj, r) for r in rings), key=_ring_area)
            x, z = _centroid(pts)
        else:
            chain = [p for w in ways for p in w]
            if not chain:
                return None
            x, z = _centroid(geo_s3d_relations._ring_pts(proj, chain))
    name = t.get("name")
    landmark = bool(name) or kind in ("monument", "statue", "obelisk")
    return {"id": el["id"], "osm": el["type"], "kind": kind, "sub": sub, "name": name,
            "x": x, "z": z, "points": pts, "height_m": _num(t.get("height")),
            "landmark": landmark, "tags": _keep_tags(t, FURN_TAGS)}


def _casco_poly():
    p = os.path.join(REPO, "_archive", "roam", "F0a", "casco_poly.json")
    if not os.path.exists(p):           # worktree: the archive lives in the main tree
        p = os.path.join(REPO, "..", "..", "..", "_archive", "roam", "F0a", "casco_poly.json")
    try:
        with open(p, encoding="utf-8") as f:
            return [(c["x"], c["z"]) for c in json.load(f)["corners"]]
    except OSError:
        return None


def _pip(poly, x, z) -> bool:
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, zi = poly[i]
        xj, zj = poly[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi) + xi:
            inside = not inside
        j = i
    return inside


def census(rels: list, items: list) -> tuple[dict, dict]:
    casco = _casco_poly()

    def in_casco_pts(pts) -> bool | None:
        if casco is None:
            return None
        return any(_pip(casco, p["x"], p["z"]) for p in pts)

    rc: dict = {"relations": len(rels), "casco_poly": casco is not None,
                "by_class": {}, "by_kind": {}, "casco_by_class": {},
                "kept_by_geo_fetch_whitelist": sum(1 for r in rels if r["keep"]),
                "with_inner_rings": sum(1 for r in rels if r["inner"]),
                "inner_rings": sum(len(r["inner"]) for r in rels),
                "outer_rings": sum(len(r["outer"]) for r in rels),
                "no_closed_outer": sum(1 for r in rels if not r["outer"]),
                "open_chains": sum(r["open_chains"] for r in rels), "list": []}
    for r in rels:
        rc["by_class"][r["area_class"]] = rc["by_class"].get(r["area_class"], 0) + 1
        rc["by_kind"][str(r["kind"])] = rc["by_kind"].get(str(r["kind"]), 0) + 1
        ic = in_casco_pts([p for ring in r["outer"] for p in ring])
        if ic:
            rc["casco_by_class"][r["area_class"]] = rc["casco_by_class"].get(r["area_class"], 0) + 1
        rc["list"].append({"id": r["id"], "name": r["name"], "kind": r["kind"],
                           "class": r["area_class"], "keep": r["keep"], "casco": ic,
                           "outer": len(r["outer"]), "inner": len(r["inner"]),
                           "open_chains": r["open_chains"], "area_m2": r["area_m2"]})
    ic_: dict = {"items": len(items), "casco_poly": casco is not None, "by_kind": {},
                 "casco_by_kind": {}, "by_osm": {}, "with_height": 0, "landmarks": 0,
                 "watch": []}
    for it in items:
        ic_["by_kind"][it["kind"]] = ic_["by_kind"].get(it["kind"], 0) + 1
        ic_["by_osm"][it["osm"]] = ic_["by_osm"].get(it["osm"], 0) + 1
        ic_["with_height"] += it["height_m"] is not None
        ic_["landmarks"] += it["landmark"]
        inc = casco is not None and _pip(casco, it["x"], it["z"])
        if inc:
            ic_["casco_by_kind"][it["kind"]] = ic_["casco_by_kind"].get(it["kind"], 0) + 1
        if it["landmark"] and it["kind"] not in ("bench", "picnic_table"):
            n = it["name"] or ""
            ic_["watch"].append({"id": it["id"], "osm": it["osm"], "kind": it["kind"],
                                 "sub": it["sub"], "name": n, "casco": inc,
                                 "watched": any(w.lower() in n.lower() for w in WATCH),
                                 "height_m": it["height_m"], "x": it["x"], "z": it["z"]})
    return rc, ic_


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
    a = ap.parse_args(argv)
    if a.dry_run:
        print(json.dumps(dry_run(a.place), indent=1))
        return 0
    pj, _, proj = geo_rails.load_place(a.place)
    upm = float(pj["projection"]["units_per_metre"])
    ql = query_for(pj)
    osm = geo_rails.fetch(a.place, ql, a.online)
    if osm is None:
        print("NOT CACHED: %s\nrerun with --online (1 Overpass request)" % geo_rails.cache_path(a.place, ql))
        return 2
    if osm.get("remark"):
        print("OVERPASS REMARK: %s" % osm["remark"])
        if "runtime error" in osm["remark"]:
            return 3
    rels, items = [], []
    for el in osm.get("elements", []):
        t = el.get("tags", {})
        if (el.get("type") == "relation" and t.get("type") == "multipolygon"
                and (t.get("leisure") or t.get("landuse") or t.get("natural")
                     or t.get("amenity") or t.get("place") == "square")
                and _furn_kind(t)[0] is None):
            rels.append(convert_relation(el, proj, upm))
            continue
        it = convert_item(el, proj)
        if it is not None:
            items.append(it)
    rels.sort(key=lambda e: e["id"])
    items.sort(key=lambda e: (e["osm"], e["id"]))
    rc, ic = census(rels, items)
    rc["elements_in_answer"] = len(osm.get("elements", []))
    write_json(os.path.join(a.place, "PARK_RELATIONS.JSON"), {"relations": rels, "census": rc})
    write_json(os.path.join(a.place, "PLAZA_FURNITURE.JSON"), {"items": items, "census": ic})
    print(json.dumps({"park_relations": rc, "plaza_furniture": ic}, indent=1,
                     sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
