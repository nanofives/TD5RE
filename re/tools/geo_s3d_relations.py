"""geo_s3d_relations.py -- the OSM BUILDING RELATIONS of a tiled geo place (roam JC).

WHY. geo_fetch.OVERPASS_QL asks for `way["building"]` and `way["building:part"]`
only ("NOT widened: relation[...]", geo_fetch.py). A building mapped as a
MULTIPOLYGON (an outline with courtyards = inner rings) or grouped by a
type=building relation (outline + its parts) is therefore not on disk at all.
MEASURED on the La Plata partido cache (verify/roam/s3d_census.py): 36 of the 78
building:part ways have no outline under them, among them the seven parts of the
Casa de Gobierno at place (2859879, 5683905): its outline and its NAME are not in
the tiles, so the building can only be identified from its surroundings. The
Palacio Municipal and the Teatro Argentino have no named footprint either.

WHAT. ONE Overpass request for the place bbox:

    relation["building"]          multipolygon outlines (outer + inner rings)
    relation["building:part"]     multipolygon parts
    relation["type"="building"]   outline + part grouping

`out body geom` inlines every member way's geometry, so no second pass. Rings are
assembled from the member ways (a ring split over several ways is joined end to
end; an open chain is reported, not guessed closed).

OUTPUT (inside <place>/, nothing else is touched)

    S3D_RELATIONS.JSON   {"relations": [...], "census": {...}}
        relations[]: {"id", "type" (multipolygon / building), "name", "class"
                      (building= or building:part= value), "part", "height_m",
                      "min_height_m", "roof_shape", "roof_height_m", "levels",
                      "colour", "roof_colour", "material", "roof_material",
                      "outer": [[{"x","z"}, ...], ...], "inner": [[...], ...],
                      "open_chains", "members": {"outline": [ids], "part": [ids]},
                      "tags": {every building / 3D tag verbatim}}
        place frame (x = east * upm, z = north * upm), the frame every tile
        layer is written in; the field names match BUILDINGS_*.JSON records so
        the C reader (td5_geob_parse_building) can take an outer ring as is.
    _cache/<sha>.json    the raw Overpass answer (geo_fetch cache key scheme):
                         a rerun is offline.

NETWORK DISCIPLINE (memory reference_geo_fetch_budget_dryrun_and_stale_baseline)
    --dry-run   urllib.request.urlopen is patched to record and raise: prints
                every request a real run would make (expected: exactly 1
                Overpass POST when uncached, 0 when cached).
    --online    required for any request not in the cache.

    python -I re/tools/geo_s3d_relations.py --dry-run
    python -I re/tools/geo_s3d_relations.py --online      # the one network call
    python -I re/tools/geo_s3d_relations.py               # rerun from the cache
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

REL_QL = """[out:json][timeout:180];
(
  relation["building"](%(bbox)s);
  relation["building:part"](%(bbox)s);
  relation["type"="building"](%(bbox)s);
);
out body geom;
"""

KEEP_TAGS = ("type", "building", "building:part", "name", "height", "min_height",
             "building:levels", "building:min_level", "roof:shape", "roof:height",
             "roof:levels", "roof:colour", "roof:material", "roof:orientation",
             "roof:direction", "roof:angle", "building:colour", "building:material",
             "amenity", "government", "office", "tourism", "historic", "heritage",
             "denomination", "religion", "layer", "wikidata", "wikipedia")

# A few names worth a line in the census (the civic landmarks Mariano asked about).
WATCH = ("Casa de Gobierno", "Municipalidad", "Palacio Municipal", "Teatro Argentino",
         "Legislatura", "Catedral", "Pasaje Dardo Rocha", "Palacio", "Museo")


def query_for(pj: dict) -> str:
    b = pj["bbox"]
    bbox = "%.6f,%.6f,%.6f,%.6f" % (b["south"], b["west"], b["north"], b["east"])
    return REL_QL % {"bbox": bbox}


def _join_rings(ways: list) -> tuple[list, int]:
    """Member way geometries ([[(lat, lon)]]) -> closed rings + the open chain count."""
    chains = [list(w) for w in ways if len(w) >= 2]
    rings, open_n = [], 0
    while chains:
        cur = chains.pop(0)
        grew = True
        while cur[0] != cur[-1] and grew:
            grew = False
            for i, c in enumerate(chains):
                if c[0] == cur[-1]:
                    cur += c[1:]
                elif c[-1] == cur[-1]:
                    cur += c[::-1][1:]
                elif c[-1] == cur[0]:
                    cur = c + cur[1:]
                elif c[0] == cur[0]:
                    cur = c[::-1] + cur[1:]
                else:
                    continue
                chains.pop(i)
                grew = True
                break
        if cur[0] == cur[-1] and len(cur) >= 4:
            rings.append(cur)
        else:
            open_n += 1
    return rings, open_n


def _ring_pts(proj, ring):
    out = []
    for lat, lon in ring:
        x, z = proj.to_world(lat, lon)
        out.append({"x": round(x, 2), "z": round(z, 2)})
    return out


def _num(v):
    try:
        return float(str(v).split(";")[0].strip().rstrip("m").strip())
    except (TypeError, ValueError):
        return None


def convert(osm: dict, proj) -> list:
    rels = []
    for el in osm.get("elements", []):
        if el.get("type") != "relation":
            continue
        t = el.get("tags", {})
        rtype = t.get("type")
        outer_w, inner_w = [], []
        members = {"outline": [], "part": []}
        for m in el.get("members", []):
            role = m.get("role") or ""
            if rtype == "building":
                if role in ("outline", "part"):
                    members[role].append([m.get("type"), m.get("ref")])
                continue
            if m.get("type") != "way" or not m.get("geometry"):
                continue
            geom = [(g["lat"], g["lon"]) for g in m["geometry"] if g]
            (inner_w if role == "inner" else outer_w).append(geom)
        outer, open_o = _join_rings(outer_w)
        inner, open_i = _join_rings(inner_w)
        cls = t.get("building") or t.get("building:part")
        rels.append({
            "id": el["id"], "type": rtype, "name": t.get("name"), "class": cls,
            "part": bool(t.get("building:part")),
            "height_m": _num(t.get("height")), "min_height_m": _num(t.get("min_height")),
            "roof_shape": t.get("roof:shape"), "roof_height_m": _num(t.get("roof:height")),
            "levels": t.get("building:levels"), "colour": t.get("building:colour"),
            "roof_colour": t.get("roof:colour"), "material": t.get("building:material"),
            "roof_material": t.get("roof:material"),
            "outer": [_ring_pts(proj, r) for r in outer],
            "inner": [_ring_pts(proj, r) for r in inner],
            "open_chains": open_o + open_i, "members": members,
            "tags": {k: t[k] for k in KEEP_TAGS if k in t},
        })
    rels.sort(key=lambda e: e["id"])
    return rels


def census(rels: list) -> dict:
    by_type: dict = {}
    for r in rels:
        by_type[r["type"] or "?"] = by_type.get(r["type"] or "?", 0) + 1
    watch = []
    for r in rels:
        n = r["name"] or ""
        if any(w.lower() in n.lower() for w in WATCH):
            c = r["outer"][0][0] if r["outer"] else None
            watch.append({"id": r["id"], "name": n, "type": r["type"], "class": r["class"],
                          "outer": len(r["outer"]), "inner": len(r["inner"]),
                          "height_m": r["height_m"], "roof_shape": r["roof_shape"],
                          "first_point": c, "members": {k: len(v) for k, v in r["members"].items()}})
    return {"relations": len(rels), "by_type": by_type,
            "with_name": sum(1 for r in rels if r["name"]),
            "parts": sum(1 for r in rels if r["part"]),
            "with_inner_rings": sum(1 for r in rels if r["inner"]),
            "inner_rings": sum(len(r["inner"]) for r in rels),
            "with_height": sum(1 for r in rels if r["height_m"]),
            "with_roof_shape": sum(1 for r in rels if r["roof_shape"]),
            "open_chains": sum(r["open_chains"] for r in rels),
            "watch": watch}


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
    ql = query_for(pj)
    osm = geo_rails.fetch(a.place, ql, a.online)
    if osm is None:
        print("NOT CACHED: %s\nrerun with --online (1 Overpass request)" % geo_rails.cache_path(a.place, ql))
        return 2
    if osm.get("remark"):
        print("OVERPASS REMARK: %s" % osm["remark"])
        if "runtime error" in osm["remark"]:
            return 3
    rels = convert(osm, proj)
    rep = census(rels)
    rep["elements_in_answer"] = len(osm.get("elements", []))
    write_json(os.path.join(a.place, "S3D_RELATIONS.JSON"), {"relations": rels, "census": rep})
    print(json.dumps(rep, indent=1, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
