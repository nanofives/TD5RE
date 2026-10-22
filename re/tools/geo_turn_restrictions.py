"""geo_turn_restrictions.py -- the OSM TURN RESTRICTIONS of a tiled geo place (roam LG).

WHY. geo_fetch.OVERPASS_QL fetches ways and nodes only, so no `type=restriction`
relation is on disk (round K audit: 274 in the casco, 487 in the place bbox,
"not queried"). The roam planner, the AI racers (they follow its route) and the
ambient traffic pick turns from the street graph alone, so they take turns the
real signs forbid (a left off an avenue, a U-turn round a median).

WHAT. ONE Overpass request for the place bbox:

    relation["type"="restriction"]

`out body geom` inlines every member's geometry (via nodes: lat/lon; from / to /
via ways: their points), so no second pass and nothing else to join.

OUTPUT (inside <place>/, nothing else is touched)

    RESTRICTIONS.JSON   {"restrictions": [...], "census": {...}}
        restrictions[]: {"id", "restriction" (the value as tagged),
                         "kind" (no_left_turn / no_right_turn / no_straight_on /
                                 no_u_turn / no_entry / no_exit / only_left_turn /
                                 only_right_turn / only_straight_on / only_u_turn,
                                 null when the value is none of those),
                         "only" (bool), "motorcar" (bool: binds a car; false when
                         `except` names motorcar / motor_vehicle / vehicle, or the
                         only restriction is a mode-specific key such as
                         restriction:bus), "conditional" (the :conditional value or
                         null; a relation with ONLY a conditional value is kept with
                         motorcar false), "except",
                         "from": [{"id", "pts": [{"x","z"}, ...]}],
                         "to":   [{"id", "pts": [...]}],
                         "via_nodes": [{"id", "x", "z", "latlon": [lat, lon]}],
                         "via_ways":  [{"id", "pts": [...]}],
                         "name", "tags": {the restriction keys verbatim}}
        place frame (geo_rails proj.to_world, the frame of the ROADS tile points),
        sorted by id. The C reader (td5_roam_restrict.c) matches from / to by OSM
        way id at the street-graph node nearest the via node.
    _cache/<sha>.json   the raw Overpass answer (geo_fetch cache key scheme): a
                        rerun is offline.

NETWORK DISCIPLINE (memory reference_geo_fetch_budget_dryrun_and_stale_baseline)
    --dry-run   urllib.request.urlopen is patched to record and raise: prints
                every request a real run would make (expected: exactly 1
                Overpass POST when uncached, 0 when cached).
    --online    required for any request not in the cache.

    python -I re/tools/geo_turn_restrictions.py --dry-run
    python -I re/tools/geo_turn_restrictions.py --online      # the one network call
    python -I re/tools/geo_turn_restrictions.py               # rerun from the cache
"""
from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import geo_rails  # noqa: E402  (endpoints, UA, cache key scheme, place loader)
from geo_common import CACHE_ROOT, write_json  # noqa: E402

DEFAULT_PLACE = os.path.join(CACHE_ROOT, "la_plata_partido")

# No regex on keys (slow on Overpass): one exact-value clause, the clause the
# round K census counted (osm_counts.csv relation_restriction).
REST_QL = """[out:json][timeout:180];
relation["type"="restriction"](%(bbox)s);
out body geom;
"""

KINDS = ("no_left_turn", "no_right_turn", "no_straight_on", "no_u_turn", "no_entry",
         "no_exit", "only_left_turn", "only_right_turn", "only_straight_on", "only_u_turn")

# Modes whose exemption means a car is NOT bound.
CAR_MODES = ("motorcar", "motor_vehicle", "vehicle")

KEEP_TAGS = ("type", "restriction", "restriction:motorcar", "restriction:motor_vehicle",
             "restriction:vehicle", "restriction:conditional", "restriction:bus",
             "restriction:hgv", "restriction:bicycle", "except", "day_on", "day_off",
             "hour_on", "hour_off", "name", "note")


def query_for(pj: dict) -> str:
    b = pj["bbox"]
    bbox = "%.6f,%.6f,%.6f,%.6f" % (b["south"], b["west"], b["north"], b["east"])
    return REST_QL % {"bbox": bbox}


def _pts(proj, geom) -> list:
    out = []
    for g in geom or []:
        if not g:
            continue
        x, z = proj.to_world(g["lat"], g["lon"])
        out.append({"x": round(x, 2), "z": round(z, 2)})
    return out


def _value(t: dict) -> tuple[str | None, bool, str | None]:
    """(restriction value, binds a car, conditional value)."""
    cond = t.get("restriction:conditional")
    for k in ("restriction:motorcar", "restriction:motor_vehicle", "restriction:vehicle"):
        if t.get(k):
            return t[k].strip(), True, cond
    v = t.get("restriction")
    if v:
        exc = [m.strip() for m in (t.get("except") or "").split(";") if m.strip()]
        timed = any(t.get(k) for k in ("day_on", "day_off", "hour_on", "hour_off"))
        return v.strip(), not any(m in CAR_MODES for m in exc) and not timed, cond
    for k in ("restriction:bus", "restriction:hgv", "restriction:bicycle"):
        if t.get(k):
            return t[k].strip(), False, cond
    if cond:
        return cond.split("@")[0].strip(), False, cond
    return None, False, cond


def convert(osm: dict, proj) -> list:
    out = []
    for el in osm.get("elements", []):
        if el.get("type") != "relation":
            continue
        t = el.get("tags", {})
        val, car, cond = _value(t)
        rec = {"id": el["id"], "restriction": val,
               "kind": val if val in KINDS else None,
               "only": bool(val and val.startswith("only_")),
               "motorcar": bool(car and val in KINDS),
               "conditional": cond, "except": t.get("except"),
               "from": [], "to": [], "via_nodes": [], "via_ways": [],
               "name": t.get("name"),
               "tags": {k: t[k] for k in KEEP_TAGS if k in t}}
        for m in el.get("members", []):
            role, mt = m.get("role") or "", m.get("type")
            if role in ("from", "to") and mt == "way":
                rec[role].append({"id": m.get("ref"), "pts": _pts(proj, m.get("geometry"))})
            elif role == "via" and mt == "node" and m.get("lat") is not None:
                x, z = proj.to_world(m["lat"], m["lon"])
                rec["via_nodes"].append({"id": m.get("ref"), "x": round(x, 2), "z": round(z, 2),
                                         "latlon": [m["lat"], m["lon"]]})
            elif role == "via" and mt == "way":
                rec["via_ways"].append({"id": m.get("ref"), "pts": _pts(proj, m.get("geometry"))})
        out.append(rec)
    out.sort(key=lambda r: r["id"])
    return out


def census(rs: list) -> dict:
    by_kind: dict = {}
    for r in rs:
        k = r["kind"] or ("(%s)" % r["restriction"])
        by_kind[k] = by_kind.get(k, 0) + 1
    usable = [r for r in rs if r["motorcar"] and r["from"] and r["to"]
              and (r["via_nodes"] or r["via_ways"])]
    return {"relations": len(rs), "by_kind": by_kind,
            "binds_a_car": sum(1 for r in rs if r["motorcar"]),
            "not_for_cars": sum(1 for r in rs if not r["motorcar"]),
            "with_except": sum(1 for r in rs if r["except"]),
            "conditional": sum(1 for r in rs if r["conditional"]),
            "via_node": sum(1 for r in rs if r["via_nodes"] and not r["via_ways"]),
            "via_way": sum(1 for r in rs if r["via_ways"]),
            "no_via": sum(1 for r in rs if not r["via_nodes"] and not r["via_ways"]),
            "multi_from_or_to": sum(1 for r in rs if len(r["from"]) > 1 or len(r["to"]) > 1),
            "missing_from_or_to": sum(1 for r in rs if not r["from"] or not r["to"]),
            "usable_for_cars": len(usable),
            "usable_via_node": sum(1 for r in usable if r["via_nodes"] and not r["via_ways"]),
            "usable_via_way": sum(1 for r in usable if r["via_ways"])}


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
    rs = convert(osm, proj)
    rep = census(rs)
    rep["elements_in_answer"] = len(osm.get("elements", []))
    write_json(os.path.join(a.place, "RESTRICTIONS.JSON"), {"restrictions": rs, "census": rep})
    print(json.dumps(rep, indent=1, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
