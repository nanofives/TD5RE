"""geo_lane_paint.py -- the OSM BUS-LANE and BIKE-LANE tags of a tiled geo place (roam PA).

WHY. The ROADS tiles dropped `busway`, `bus:lanes`, `psv:lanes`, `lanes:bus`, `oneway:bus` and the
`cycleway*` family (round K audit: casco 38 + 89 ways). Roam paints no lane at all, so a bus lane and
a bike lane look like any other asphalt.

WHAT. ONE Overpass request for the place bbox, tags only (no geometry, nothing to join but the way id,
which every ROADS way carries as "id"):

    way["highway"]["cycleway"], ["cycleway:left"], ["cycleway:right"], ["cycleway:both"],
    ["busway"], ["busway:left"], ["busway:right"], ["busway:both"],
    ["lanes:bus"], ["bus:lanes"], ["psv:lanes"], ["lanes:psv"], ["oneway:bus"]

OUTPUT (inside <place>/, nothing else is touched)

    LANES.JSON   {"ways": {"<osm way id>": {"bus": {...}, "cycle": {...}, "tags": {...},
                  "pts": [{"x","z"}, ...] (the ROADS tile polyline, source frame)}}, "census": {...}}
        bus:   {"left": kind|null, "right": kind|null, "left_n", "right_n": reserved lanes from that kerb, "contraflow": bool}
        cycle: {"left": kind|null, "right": kind|null}
        kind (bus):   "lane"   a lane of the carriageway kept for buses (busway=lane, a `designated`
                               slot of bus:lanes / psv:lanes, lanes:bus=1 read on the kerb side)
                      "track"  a physically separate busway (busway=track)
        kind (cycle): "lane"   painted lane (cycleway=lane, :lane=exclusive|advisory)
                      "track"  separated from traffic (cycleway=track, opposite_track, :segregated)
                      "shared" shared lane / share_busway / shoulder (a sharrow, not a lane)
        `left` / `right` are relative to the way's direction of travel, as OSM does. A tag with no side
        (cycleway=lane) means BOTH sides. cycleway=opposite_lane / opposite_track on a one-way way is a
        contraflow lane on its LEFT (right-hand traffic). Every kept raw tag is repeated in "tags".
    _cache/<sha>.json   the raw Overpass answer (geo_fetch cache key scheme): a rerun is offline.

NETWORK DISCIPLINE (memory reference_geo_fetch_budget_dryrun_and_stale_baseline)
    --dry-run   urllib.request.urlopen is patched to record and raise: prints every request a real
                run would make (expected: exactly 1 Overpass POST when uncached, 0 when cached).
    --online    required for any request not in the cache.

    python -I re/tools/geo_lane_paint.py --dry-run
    python -I re/tools/geo_lane_paint.py --online      # the one network call
    python -I re/tools/geo_lane_paint.py               # rerun from the cache
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

# No regex on keys (slow on Overpass): one exact-key clause each, unioned.
LANE_KEYS = ("cycleway", "cycleway:left", "cycleway:right", "cycleway:both",
             "busway", "busway:left", "busway:right", "busway:both",
             "lanes:bus", "bus:lanes", "psv:lanes", "lanes:psv", "oneway:bus")

LANE_QL = ("[out:json][timeout:180];\n(\n"
           + "".join('  way["highway"]["%s"](%%(bbox)s);\n' % k for k in LANE_KEYS)
           + ");\nout tags;\n")

KEEP_PREFIX = ("cycleway", "busway", "bus:lanes", "psv:lanes", "lanes:bus", "lanes:psv", "oneway:bus",
               "bicycle:lanes", "lanes", "oneway", "highway", "name", "surface:cycleway")

BUS_SLOTS = ("designated", "yes", "lane")      # a bus:lanes slot value: lane allowed for buses (yes) / reserved (designated)


def query_for(pj: dict) -> str:
    b = pj["bbox"]
    bbox = "%.6f,%.6f,%.6f,%.6f" % (b["south"], b["west"], b["north"], b["east"])
    return LANE_QL % {"bbox": bbox}


def _cyc_kind(v: str | None, seg: str | None = None) -> str | None:
    v = (v or "").strip()
    if v in ("lane", "opposite_lane"):
        return "lane"
    if v in ("track", "opposite_track"):
        return "track"
    if v in ("shared_lane", "share_busway", "shoulder", "opposite", "shared"):
        return "shared"
    return None


def _cycle(t: dict) -> dict:
    out = {"left": None, "right": None}
    base = t.get("cycleway")
    both = t.get("cycleway:both")
    for side in ("left", "right"):
        v = t.get("cycleway:" + side) or both or base
        k = _cyc_kind(v)
        sub = t.get("cycleway:%s:lane" % side)
        if k == "lane" and sub not in (None, "exclusive", "advisory"):
            sub = None
        if t.get("cycleway:%s:segregated" % side) == "yes" and k == "lane":
            k = "track"
        out[side] = k
    if (base or "").startswith("opposite") and not t.get("cycleway:left") and not t.get("cycleway:right") and not both:
        # one-way street, lane against the traffic: the left kerb for right-hand traffic
        out["right"] = None
        out["left"] = _cyc_kind(base)
    return out


def _bus(t: dict) -> dict:
    out = {"left": None, "right": None, "contraflow": t.get("oneway:bus") == "no"}
    both = t.get("busway:both") or t.get("busway")
    for side in ("left", "right"):
        v = (t.get("busway:" + side) or both or "").strip()
        if v == "lane" or v == "opposite_lane":
            out[side] = "lane"
        elif v in ("track", "opposite_track"):
            out[side] = "track"
    if v := (t.get("busway") or ""):
        if v.startswith("opposite") and not t.get("busway:left") and not t.get("busway:right"):
            out["right"] = None
            out["left"] = "track" if v == "opposite_track" else "lane"
            out["contraflow"] = True
    out["left_n"] = 1 if out["left"] else 0
    out["right_n"] = 1 if out["right"] else 0
    slots_seen = False
    for key in ("bus:lanes", "psv:lanes"):
        raw = t.get(key)
        if not raw or "|" not in raw and raw.strip() not in BUS_SLOTS:
            continue
        slots = [x.strip() for x in raw.split("|")]       # one slot per lane, left to right
        # `designated` reserves the lane for buses; `yes` only lets them use a general lane
        nr = 0
        for x in reversed(slots):
            if x != "designated":
                break
            nr += 1
        nl = 0
        for x in slots:
            if x != "designated":
                break
            nl += 1
        if nr == len(slots):          # every lane reserved: not a bus LANE any more, leave it
            nr = nl = 0
        if nr and out["right"] is None:
            out["right"], out["right_n"] = "lane", nr
        if nl and out["left"] is None:
            out["left"], out["left_n"] = "lane", nl
        slots_seen = True
    if not slots_seen and not out["right"] and not out["left"]:
        n = (t.get("lanes:bus") or t.get("lanes:psv") or "").strip()
        if n.isdigit() and int(n) > 0:   # a count with no slot list: the kerb lanes, right side
            out["right"], out["right_n"] = "lane", min(int(n), 2)
    return out


def road_points(place: str, ids: set) -> dict:
    """way id -> [{"x","z"}, ...] (source frame, as in the ROADS tiles). The C painter needs the way's own
    direction: OSM left/right are relative to it and a ribbon row may run either way."""
    import glob
    out: dict = {}
    for f in sorted(glob.glob(os.path.join(place, "tiles", "ROADS_*.JSON"))):
        with open(f, encoding="utf-8") as fh:
            doc = json.load(fh)
        for r in doc.get("roads", []):
            k = str(r.get("id"))
            if k in ids and k not in out and r.get("points"):
                out[k] = [{"x": p["x"], "z": p["z"]} for p in r["points"]]
    return out


def convert(osm: dict, pts: dict | None = None) -> dict:
    ways: dict = {}
    for el in osm.get("elements", []):
        if el.get("type") != "way":
            continue
        t = el.get("tags", {})
        b, c = _bus(t), _cycle(t)
        if not (b["left"] or b["right"] or c["left"] or c["right"]):
            continue
        ways[str(el["id"])] = {
            "bus": b, "cycle": c,
            "tags": {k: v for k, v in t.items() if k.startswith(KEEP_PREFIX)}}
    if pts is not None:
        for k, w in ways.items():
            w["pts"] = pts.get(k, [])
    return ways


def census(ways: dict, osm: dict) -> dict:
    n = lambda f: sum(1 for w in ways.values() if f(w))  # noqa: E731
    return {"elements_in_answer": len(osm.get("elements", [])), "ways_kept": len(ways),
            "bus_lane": n(lambda w: "lane" in (w["bus"]["left"], w["bus"]["right"])),
            "bus_track": n(lambda w: "track" in (w["bus"]["left"], w["bus"]["right"])),
            "bus_contraflow": n(lambda w: w["bus"]["contraflow"] and (w["bus"]["left"] or w["bus"]["right"])),
            "cycle_lane": n(lambda w: "lane" in (w["cycle"]["left"], w["cycle"]["right"])),
            "cycle_track": n(lambda w: "track" in (w["cycle"]["left"], w["cycle"]["right"])),
            "cycle_shared": n(lambda w: "shared" in (w["cycle"]["left"], w["cycle"]["right"])),
            "dropped_no_usable_value": len([e for e in osm.get("elements", []) if e.get("type") == "way"]) - len(ways)}


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
    pj, _, _ = geo_rails.load_place(a.place)
    ql = query_for(pj)
    osm = geo_rails.fetch(a.place, ql, a.online)
    if osm is None:
        print("NOT CACHED: %s\nrerun with --online (1 Overpass request)" % geo_rails.cache_path(a.place, ql))
        return 2
    if osm.get("remark"):
        print("OVERPASS REMARK: %s" % osm["remark"])
        if "runtime error" in osm["remark"]:
            return 3
    ways = convert(osm)
    pts = road_points(a.place, set(ways))
    for k, w in ways.items():
        w["pts"] = pts.get(k, [])
    rep = census(ways, osm)
    rep["ways_with_geometry"] = sum(1 for w in ways.values() if w["pts"])
    write_json(os.path.join(a.place, "LANES.JSON"), {"ways": ways, "census": rep})
    print(json.dumps(rep, indent=1, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
