"""geo_selector.py -- the GEO TRACK map selector (Phase 4 of
docs/plans/GEO_TRACK_OSM_PLAN.md).

Serves a local page (re/tools/geo_selector/index.html, Leaflet over OSM tiles,
skinned to the TD5 frontend) and a small JSON API around the Phase 1 tools, so
a route is picked by clicking A and B and dragging waypoints instead of typing
coordinates into three scripts:

    python re/tools/geo_selector.py            # then open http://127.0.0.1:8765/

Run it from the repo root (the cache lives at re/assets/geo/, relative, same as
the game). It binds 127.0.0.1 ONLY: nothing outside this machine can reach it,
and no firewall rule is involved.

WHAT NEEDS THE NETWORK. Map tiles and place search are fetched by the BROWSER
(OSM tiles, Nominatim). FETCH AREA runs geo_fetch (Overpass + Terrarium, cached
on disk after the first time). Routing, conditioning and SEND TO GAME are local:
they read the cache and write files. So a place fetched once is editable offline
forever, map background aside.

API (all JSON):
    GET  /api/state                      places on disk + the game's selection
    POST /api/fetch  {name,lat,lon,radius}     geo_fetch a place (network)
    POST /api/route  {place, waypoints:[[lat,lon],...]}
         -> routed polyline + the conditioner's verdict, crossings as lat/lon
    POST /api/save   {place, waypoints}
         -> writes ROUTE_RAW.JSON + ROUTE.JSON, rebuilds the rasters in the
            route's frame (cached, no new downloads), selects it for the game

WHY THE SAVE REBUILDS THE RASTERS. The conditioner DECIDES the frame (rotation,
offset and origin at the route's own centroid) and the terrain must be in that
same frame or the road and the ground disagree with nothing raising an error
(geo_audit R8/R9). geo_fetch's --frame-from takes the rotation and offset but
keeps the place centre as the origin, so the save passes the ROUTE's origin
explicitly and widens the radius to still cover the original area.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo_common import (  # noqa: E402
    CACHE_ROOT,
    LocalProjection,
    TD5_TG_MAX_SPANS,
    place_dir,
    read_json,
    write_json,
)
import geo_condition  # noqa: E402
import geo_forks  # noqa: E402
import geo_route  # noqa: E402

PAGE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "geo_selector")
SELECTED_NAME = "SELECTED.TXT"

# geo_fetch pulls in numpy/PIL; import lazily so the selector (and its
# self-test) still runs where only the routing half is needed.
_fetch_mod = None
_fetch_lock = threading.Lock()   # one Overpass job at a time, it rate-limits


def _geo_fetch():
    global _fetch_mod
    if _fetch_mod is None:
        import geo_fetch as m  # noqa: WPS433
        _fetch_mod = m
    return _fetch_mod


def _haversine_m(a: tuple[float, float], b: tuple[float, float]) -> float:
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = (math.sin((la2 - la1) / 2) ** 2
         + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2)
    return 2 * 6371008.8 * math.asin(math.sqrt(h))


# ----------------------------------------------------------- layer report ---
#
# Plan section 9: "Raster vintage differs per layer (canopy nominally 2018-2020,
# WorldCover 2020/2021, IGN flights 2011-2016, OSM current). A tree felled in
# 2022 is still there, a street opened in 2024 has no terrain under it. Record
# each layer's vintage in PLACE.JSON so a surprise is explainable." -- and
# section 7 puts it on screen: "show the bbox, the span count against the 3000
# cap, and the per-layer data source and vintage."
#
# geo_fetch already writes all of it into PLACE.JSON "layers"; this flattens it
# into rows the page can render without knowing each layer's shape. A layer that
# is NOT WIRED says so, in those words, rather than being hidden: a missing
# canopy raster is why the plaza trees are procedural, and that is a fact the
# person picking a place should be able to read off the page.

LAYER_LABELS = [
    ("osm",    "Roads, buildings, areas"),
    ("height", "Elevation"),
    ("cover",  "Land cover"),
    ("canopy", "Tree canopy"),
]


def _layer_detail(key: str, d: dict) -> str:
    if key == "osm":
        c = d.get("counts") or {}
        bits = [("%d %s" % (c[k], k)) for k in
                ("roads", "buildings", "areas", "signals", "water") if c.get(k)]
        if c.get("clipped_from"):
            bits.append("clipped from %d" % c["clipped_from"])
        return ", ".join(bits)
    if key == "height":
        out = []
        if d.get("native_m"):
            out.append("%.0f m native" % d["native_m"])
        if d.get("ground_res_m"):
            out.append("%.1f m/px here" % d["ground_res_m"])
        if d.get("relief_m") is not None:
            out.append("%.1f m of relief" % d["relief_m"])
        return ", ".join(out)
    if key == "cover":
        p = d.get("painted") or {}
        if p:
            return ("%d built / %d water cells of %d"
                    % (p.get("built_cells", 0), p.get("water_cells", 0),
                       p.get("cover_cells", 0)))
    return ""


def layer_rows(place: dict) -> list[dict]:
    """PLACE.JSON "layers" -> ordered rows of (layer, source, vintage, licence).

    `wired` is False only when the source string SAYS it is not wired, so the
    page never claims a layer is live because a key happened to exist.
    """
    layers = place.get("layers") or {}
    rows = []
    for key, label in LAYER_LABELS:
        d = layers.get(key)
        if d is None:
            rows.append({"key": key, "label": label, "wired": False,
                         "source": "not present in PLACE.JSON",
                         "vintage": "", "licence": "", "detail": ""})
            continue
        src = str(d.get("source") or "")
        wired = not src.upper().startswith("NOT YET WIRED")
        if not wired:
            # Keep the reason, drop the banner: the page shows NOT WIRED itself.
            src = src.split("--", 1)[-1].strip() or src
        rows.append({
            "key": key,
            "label": label,
            "wired": wired,
            "source": src,
            "vintage": str(d.get("vintage") or ""),
            "licence": str(d.get("licence") or ""),
            "detail": _layer_detail(key, d) if wired else "",
        })
    return rows


# ------------------------------------------------------------------ state ---

def list_places(root: str = CACHE_ROOT) -> list[dict]:
    out = []
    if not os.path.isdir(root):
        return out
    for slug in sorted(os.listdir(root)):
        d = os.path.join(root, slug)
        if slug.startswith(("_", ".")) or not os.path.isdir(d):
            continue
        pj = os.path.join(d, "PLACE.JSON")
        if not os.path.isfile(pj):
            continue
        p = read_json(pj)
        route = None
        rr = os.path.join(d, "ROUTE_RAW.JSON")
        if os.path.isfile(rr):
            raw = read_json(rr)
            route = {"waypoints": raw.get("waypoints", []),
                     "length_km": raw.get("length_km")}
        out.append({
            "slug": slug,
            "name": p.get("name", slug),
            "centre": p.get("centre"),
            "radius_m": p.get("radius_m"),
            "bbox": p.get("bbox"),
            "attribution": p.get("attribution", []),
            "layers": layer_rows(p),
            "has_route": os.path.isfile(os.path.join(d, "ROUTE.JSON")),
            "route": route,
            "forks_saved": _read_forks_file(d),
        })
    return out


def read_selected(root: str = CACHE_ROOT) -> str:
    try:
        with open(os.path.join(root, SELECTED_NAME), encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def write_selected(slug: str, root: str = CACHE_ROOT) -> None:
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, SELECTED_NAME), "w", encoding="utf-8") as f:
        f.write(slug)


# ------------------------------------------------- "not enough road here" ---
#
# Plan section 9: "OSM data quality varies wildly. Rural coverage is sparse; a
# dense centre holds ten thousand ways in a 2 km box. Needs the bbox cap, a
# way-count cap, and a graceful 'not enough road here' path in the selector
# rather than a failed build."
#
# The bbox and way-count caps live in geo_fetch (they belong to the FETCH). What
# is here is the other half: every reason a place or a pair of clicks cannot
# become a track, stated as a sentence in the page BEFORE anything downstream
# has to cope with it. `ok` False disables SEND TO GAME, which is the only
# irreversible step.
#
# MEASURED on the two fixtures this ships with:
#   la_plata  -- 2291 drivable ways cached, 1650 inside the pinned routing bbox,
#                4907 graph nodes, one connected component holding all of them.
#   testgrid  -- the self-test's synthetic 12x12 block city: 26 ways, 338 nodes.
# So a floor of 8 ways / 40 nodes clears the smaller fixture by 3x and 8x, and
# still refuses the cases that actually occur: an ocean or desert tile (0 ways),
# a hamlet with one street through it, and a bbox whose roads were all clipped.
GRAPH_MIN_WAYS = 8
GRAPH_MIN_NODES = 40
# The largest connected component has to hold most of the graph, or A and B can
# sit on roads that never meet. Both fixtures are a single component (1.00).
GRAPH_MIN_MAIN_FRAC = 0.35
# How far a click may be from a drivable road before it is a mistake rather
# than a snap. 250 m is more than one La Plata block (110 m), so a click in the
# middle of a plaza still finds the street round it.
CLICK_MAX_SNAP_M = 250.0


def _components(g) -> tuple[int, int]:
    """(number of connected components, size of the largest). Iterative, so a
    16k-node graph cannot blow the recursion limit."""
    seen = [False] * len(g.pos)
    comps, biggest = 0, 0
    for s in range(len(g.pos)):
        if seen[s]:
            continue
        comps += 1
        n, stack = 0, [s]
        seen[s] = True
        while stack:
            u = stack.pop()
            n += 1
            for v, _w, _ri in g.adj[u]:
                if not seen[v]:
                    seen[v] = True
                    stack.append(v)
        if n > biggest:
            biggest = n
    return comps, biggest


def graph_verdict(place: dict, g) -> dict:
    """Is there enough drivable road here to build a track at all?"""
    nodes, ways = len(g.pos), len(g.roads)
    reasons, warnings = [], []
    comps, biggest = _components(g) if nodes else (0, 0)
    if ways < GRAPH_MIN_WAYS or nodes < GRAPH_MIN_NODES:
        reasons.append("not enough drivable road here: %d way(s), %d junction "
                       "point(s). Fetch a bigger radius, or pick somewhere with "
                       "streets." % (ways, nodes))
    elif nodes and biggest < nodes * GRAPH_MIN_MAIN_FRAC:
        reasons.append("the road graph here is in %d disconnected pieces and the "
                       "biggest holds only %d of %d points, so most pairs of "
                       "points cannot be driven between."
                       % (comps, biggest, nodes))
    elif comps > 1:
        warnings.append("%d disconnected road pieces; the main one holds %d of "
                        "%d points." % (comps, biggest, nodes))
    clipped = ((place.get("layers") or {}).get("osm") or {}) \
        .get("counts", {}).get("clipped_from")
    if clipped:
        warnings.append("this area was clipped at fetch time: %d of %d ways kept "
                        "(dense centre)." % (ways, clipped))
    return {"ok": not reasons, "reasons": reasons, "warnings": warnings,
            "ways": ways, "nodes": nodes, "components": comps,
            "main_component": biggest}


def click_verdict(g, proj, waypoints: list[list[float]]) -> list[str]:
    """Every waypoint has to land on, or near, a drivable road."""
    out = []
    for i, (la, lo) in enumerate(waypoints):
        x, z = proj.to_world(float(la), float(lo))
        n = g.nearest(x, z)
        if n < 0:
            out.append("there is no drivable road in this place at all")
            break
        d_m = math.dist(g.pos[n], (x, z)) / proj.units_per_metre
        if d_m > CLICK_MAX_SNAP_M:
            out.append("point %s is %.0f m from the nearest drivable road (limit "
                       "%.0f m). Click on a street."
                       % ("AB"[i] if i < 2 else str(i + 1), d_m, CLICK_MAX_SNAP_M))
    return out


# -------------------------------------------------------------- routing ---

_graph_cache: dict[str, tuple[float, tuple]] = {}


def _in_bbox(la: float, lo: float, bb: dict) -> bool:
    return bb["south"] <= la <= bb["north"] and bb["west"] <= lo <= bb["east"]


def _load_graph(slug: str, root: str):
    """The road graph is ~16k nodes; build it once per place and reuse it until
    ROADS.JSON changes (a save re-writes it in the new frame).

    ROUTING AREA IS PINNED. SEND TO GAME re-fetches a WIDER circle (the route's
    own origin plus the old radius, so the terrain covers everything). Routing
    over that wider graph found a different, shorter path for the same A/B
    (La Plata: 1492 -> 1200 spans), so touching a saved route silently changed
    it. The first save records the area the user actually routed in
    (PLACE.JSON "route_graph_bbox") and the graph is limited to roads with a
    point inside it from then on."""
    d = place_dir(slug, root)
    rp = os.path.join(d, "ROADS.JSON")
    mt = os.path.getmtime(rp)
    hit = _graph_cache.get(root + "|" + slug)
    if hit and hit[0] == mt:
        return hit[1]
    place = read_json(os.path.join(d, "PLACE.JSON"))
    roads = read_json(rp)["roads"]
    pr = place["projection"]
    proj = LocalProjection(pr["lat0"], pr["lon0"], pr["units_per_metre"])
    proj.set_rotation(pr.get("rotation_rad", 0.0))
    proj.set_offset(pr.get("offset_x", 0.0), pr.get("offset_z", 0.0))
    bb = place.get("route_graph_bbox")
    if bb:
        keep = []
        for r in roads:
            for pt in r.get("points") or []:
                la, lo = proj.world_to_latlon(pt["x"], pt["z"])
                if _in_bbox(la, lo, bb):
                    keep.append(r)
                    break
        roads = keep
    loaded = (place, geo_route.RoadGraph(roads), proj)
    _graph_cache[root + "|" + slug] = (mt, loaded)
    return loaded


# --------------------------------------------------------------- forks ---
#
# Phase 4's last selector item: "show detected fork candidates (median avenues,
# comparable alternative routes) as toggles the user confirms". Detection is
# geo_forks; what is here is the half that has to line up with the ENGINE.
#
# THE ONE HARD PART is that a fork is not just a span range. The engine splits
# the road at F into two carriageways (tg_fork_split_lanes) and refuses a fork
# whose span at F carries fewer than tg_fork_kind_min_lanes(kind) lanes -- 4 for
# an ISLAND -- and refuses it again if the lane count is not UNIFORM across
# F-TD5_TG_BRANCH_WIDEN-2 .. R+2 (td5_tg_branch.c:301, :314). A real median
# avenue reaches us as TWO one-way OSM ways of 2 lanes each, so the route's own
# lane count is 2 and every fork would be skipped with "2 lanes, needs 4".
#
# So confirming a fork WIDENS THE ROUTE to lanes(A)+lanes(B) over the fork's
# window, which is what the road physically is, and the widening is fed into
# condition_route as its `lanes` argument rather than patched into the result
# afterwards. That matters: the curvature floor is radius >= (width/2) *
# curve_safety, so the wider road has a TIGHTER turn limit, and enforcing it
# after the fact would store geometry smoothed for the narrow road and let a
# fork fold on a corner -- the failure tg_fork_region_max_curve exists to catch.
#
# Consequence the page has to live with, and does: toggling a fork re-conditions
# and moves the span count. That is honest -- it is a different road.
#
# TWO INDEX MAPPINGS, deliberately. The span range is read by ARCLENGTH (where
# the avenue is on the ground). The lane window is written by condition_route's
# own INDEX-FRACTION mapping (_lane_at), because that is what decides which
# stored node gets which lane count. They disagree whenever the raw vertices are
# unevenly spaced, so the achieved fork range is read BACK off the conditioned
# lanes array (_fit_fork) instead of being assumed. Nothing here trusts the two
# to agree.

FORKS_NAME = "FORKS.JSON"
# Span headroom the engine needs around a fork (TD5_TG_BRANCH_WIDEN 6, plus the
# +/-2 margin the walk and the placement loop both use, plus one for rounding).
FORK_WIDEN_PAD = 9
FORK_TAIL_PAD = 3
# The first fork must clear the grid (TD5_TG_GRID_SPAN 24) and the conditioner's
# lead-in (TG_LEAD_IN_NODES 40), and the last must leave the engine's own
# "must fit on the ring" margin (R + 24 < ring).
FORK_MIN_F = 48
FORK_RING_TAIL = 26
# Length floors, mirroring tg_fork_len_floored: an ISLAND keeps any length from
# 3 spans up, everything else is floored at TD5_TG_BRANCH_MIN_LEN.
FORK_MIN_LEN = {"ISLAND": 3, "AVENUE": 24, "WIDE": 24, "SLIP": 24, "MAJOR": 24}
FORK_DEFAULT_MIN_LEN = 24
# TD5_TG_BRANCH_MAX.
FORK_MAX_COUNT = 8
# Each fork appends 1+len spans after the ring, and the ring plus every corridor
# has to stay inside TD5_TG_MAX_SPANS.
FORK_TOTAL_SPAN_CAP = TD5_TG_MAX_SPANS


def _read_forks_file(d: str) -> list[dict]:
    p = os.path.join(d, FORKS_NAME)
    if not os.path.isfile(p):
        return []
    try:
        return list(read_json(p).get("forks", []))
    except Exception:
        return []


def _arc_fractions(pts: list[tuple[float, float]]) -> list[float]:
    """Cumulative arclength of a polyline, normalised to 0..1."""
    cum, tot = [0.0], 0.0
    for i in range(len(pts) - 1):
        tot += math.dist(pts[i], pts[i + 1])
        cum.append(tot)
    if tot <= 0.0:
        return [0.0] * len(pts)
    return [c / tot for c in cum]


def _span_of_frac(frac: float, cond: dict) -> int:
    """Arclength fraction along the ROUTE BODY -> conditioned span index.

    The conditioned node list is `lead` (lead_in_nodes+1 entries, node 0..40)
    followed by the resampled body, and the body's first point coincides with
    the lead's last, so fraction 0 is node lead_in_nodes and fraction 1 is the
    last span."""
    lead = int(cond.get("lead_in_nodes", 40))
    spans = int(cond.get("spans", 0))
    f = 0.0 if frac < 0.0 else (1.0 if frac > 1.0 else frac)
    return lead + int(round(f * (spans - lead)))


def _src_window(cond: dict, n_src: int, span_lo: int, span_hi: int
                ) -> tuple[int, int]:
    """Span range -> the RAW source-index range condition_route's _lane_at
    would map onto it (inclusive, clamped)."""
    lead = int(cond.get("lead_in_nodes", 40))
    n_body = int(cond.get("spans", 0)) - lead
    if n_body <= 1 or n_src <= 1:
        return 0, n_src - 1
    scale = (n_src - 1) / float(n_body - 1)
    lo = int(math.floor((span_lo - lead - 1) * scale))
    hi = int(math.ceil((span_hi - lead - 1) * scale))
    return max(0, min(lo, n_src - 1)), max(0, min(hi, n_src - 1))


def _fit_fork(cond: dict, want_lanes: int, span_lo: int, span_hi: int,
              taken: list[tuple[int, int]]) -> tuple[int, int] | None:
    """The fork range the CONDITIONED route can actually carry.

    Finds the maximal run of spans whose stored lane count is exactly
    `want_lanes`, overlapping [span_lo, span_hi] and not already claimed, then
    insets it by the engine's approach and rejoin margins. Returns (F, R) or
    None. Reading it back rather than assuming it is what makes the uniformity
    check in tg_fork_place pass by construction."""
    lanes = [int(p.get("lanes", 2)) for p in cond.get("points", [])]
    n = len(lanes)
    if n < 3:
        return None
    mid = max(0, min((span_lo + span_hi) // 2, n - 1))
    if lanes[mid] != want_lanes:
        # The target centre did not get widened (rounding between the two index
        # mappings): look for the nearest span in range that did.
        mid = next((i for i in range(max(0, span_lo), min(n, span_hi + 1))
                    if lanes[i] == want_lanes), -1)
        if mid < 0:
            return None
    a = mid
    while a > 0 and lanes[a - 1] == want_lanes:
        a -= 1
    b = mid
    while b + 1 < n and lanes[b + 1] == want_lanes:
        b += 1
    for ta, tb in taken:
        if a <= tb and ta <= b:        # overlap: start after what is claimed
            a = max(a, tb + FORK_WIDEN_PAD + 2)
    F = a + FORK_WIDEN_PAD
    R = b - FORK_TAIL_PAD
    return (F, R) if R > F + 1 else None


def fork_candidates(slug: str, waypoints: list[list[float]],
                    root: str = CACHE_ROOT) -> dict:
    """Detect candidates for the CURRENT A/B and place them on the map + spans."""
    res = route_and_condition(slug, waypoints, root)
    if not res.get("polyline"):
        return {"ok": False, "reasons": res.get("reasons", ["no route"]),
                "candidates": []}
    place, g, proj = _load_graph(slug, root)
    upm = place["projection"]["units_per_metre"]
    raw = res["_raw"]
    cond = res["_cond"]
    cands = geo_forks.detect(g, raw["route"], upm)
    fr = _arc_fractions(raw["route"]["points"])
    rev = cond.get("direction") == "reversed"
    out = []
    for c in cands:
        f0, f1 = fr[c["k0"]], fr[c["k1"]]
        if rev:
            f0, f1 = 1.0 - f1, 1.0 - f0
        s0, s1 = _span_of_frac(f0, cond), _span_of_frac(f1, cond)
        c = dict(c)
        c["span_from"], c["span_to"] = s0, s1
        c["latlon"] = [list(p) for p in
                       (raw["latlon"][c["k0"]:c["k1"] + 1])]
        if c.get("alt_world"):
            c["latlon_alt"] = [list(proj.world_to_latlon(x, z))
                               for x, z in c.pop("alt_world")]
        elif c.get("peer_latlon"):
            c["latlon_alt"] = c["peer_latlon"]
        c.pop("peer_latlon", None)
        c.pop("alt_world", None)
        out.append(c)
    return {"ok": True, "reasons": [], "candidates": out,
            "spans": cond.get("spans")}


def _ramp_one_lane_per_seam(lanes: list[int]) -> list[int]:
    """Raise the lane profile until no seam changes by more than ONE lane.

    MEASURED, and it is the reason this pass exists. Widening a fork window
    straight from 2 lanes to 4 puts a two-lane step at each end of the window.
    The strip emitter types a two-lane change as "add/drop BOTH sides" (span
    types 4 and 7), and the generator's own invariant -- checked by
    re/tools/tg_strip_audit.py -- is that a both-sides change must also move the
    lane BASE nibble by one, because a lane appears on each side and the lane
    numbering origin moves with it. The synthetic walk does that bookkeeping in
    its lane_vary block; the geo walk (tg_geo_walk) has no lane management at
    all and leaves every span on TD5_TG_HEIGHT_NIBBLE, so a both-sides change
    on the geo path is a genuine violation. The first attempt at this feature
    shipped it and the audit said so:
        lane-change seams on ring: 8 violations: 6
          seam  167: 2->4 type 4 base 8->8 BAD
          seam  409: 4->5 type 2 base 8->8 ok
    -- every two-lane seam BAD, and every ONE-lane seam fine, because a
    one-lane change is typed as a right-side add/drop (types 2 and 5) and a
    right-side change is defined to leave the base alone.
    So the profile is ramped one lane per seam instead. The cost is honest and
    visible: the approach to a divided avenue gains a lane about a dozen spans
    early, which is roughly the flare a real avenue has anyway.
    Only ever RAISES, so no fork window can be narrowed by this pass."""
    out = list(lanes)
    changed = True
    while changed:
        changed = False
        for k in range(len(out) - 1):
            if out[k] < out[k + 1] - 1:
                out[k] = out[k + 1] - 1
                changed = True
            elif out[k + 1] < out[k] - 1:
                out[k + 1] = out[k] - 1
                changed = True
    return out


def _widen_lanes_for(cands: list[dict], lanes: list[int], cond: dict,
                     fr: list[float], rev: bool) -> list[int]:
    """Lane list with every confirmed fork's window widened to its own count."""
    out = list(lanes)
    n_src = len(lanes)
    for c in cands:
        f0, f1 = fr[c["k0"]], fr[c["k1"]]
        if rev:
            f0, f1 = 1.0 - f1, 1.0 - f0
        s0, s1 = _span_of_frac(f0, cond), _span_of_frac(f1, cond)
        lo, hi = _src_window(cond, n_src, s0 - FORK_WIDEN_PAD - 2,
                             s1 + FORK_TAIL_PAD + 2)
        if rev:
            lo, hi = n_src - 1 - hi, n_src - 1 - lo
        for k in range(max(0, lo), min(n_src, hi + 1)):
            out[k] = int(c["lanes"])
    return _ramp_one_lane_per_seam(out)


def resolve_forks(slug: str, waypoints: list[list[float]], want_ids: list,
                  root: str = CACHE_ROOT) -> dict:
    """Condition the route WITH the confirmed forks and place them on spans.

    Returns the conditioned result plus `forks` (the entries FORKS.JSON gets)
    and `fork_reasons` for every candidate that could not be honoured."""
    base = route_and_condition(slug, waypoints, root)
    if not base.get("polyline") or not want_ids:
        base["forks"] = []
        base["fork_reasons"] = []
        return base
    place, g, proj = _load_graph(slug, root)
    upm = place["projection"]["units_per_metre"]
    raw, cond = base["_raw"], base["_cond"]
    all_c = geo_forks.detect(g, raw["route"], upm)
    want = [c for c in all_c if c["id"] in want_ids]
    unknown = [i for i in want_ids if not any(c["id"] == i for c in all_c)]

    fr = _arc_fractions(raw["route"]["points"])
    rev = cond.get("direction") == "reversed"
    lanes2 = _widen_lanes_for(want, raw["lanes"], cond, fr, rev)
    res = route_and_condition(slug, waypoints, root, lanes=lanes2)
    if not res.get("polyline"):
        return res
    cond2 = res["_cond"]
    rev2 = cond2.get("direction") == "reversed"
    spans = int(cond2.get("spans", 0))

    # IN SPAN ORDER. geo_forks works in raw route order, and a route the
    # conditioner chose to drive REVERSED comes out descending in spans -- which
    # would write FORKS.JSON back to front. The engine's stateless fork gates
    # (tg_span_in_fork_run, tg_fork_window_ahead) walk the table in order and
    # the overlap bookkeeping below assumes it too, so sort here, once.
    spanned = []
    for c in want:
        f0, f1 = fr[c["k0"]], fr[c["k1"]]
        if rev2:
            f0, f1 = 1.0 - f1, 1.0 - f0
        spanned.append((_span_of_frac(f0, cond2), _span_of_frac(f1, cond2), c))
    spanned.sort(key=lambda t: t[0])

    forks, reasons, taken, corridor = [], [], [], 0
    for s0, s1, c in spanned:
        fit = _fit_fork(cond2, int(c["lanes"]), s0, s1, taken)
        if fit is None:
            reasons.append("%s: the route could not carry a %d-lane window "
                           "there (spans %d-%d)" % (c["name"], c["lanes"], s0, s1))
            continue
        F, R = fit
        L = R - F - 1
        floor = FORK_MIN_LEN.get(c["kind"], FORK_DEFAULT_MIN_LEN)
        if F < FORK_MIN_F:
            reasons.append("%s: starts at span %d, inside the grid and lead-in "
                           "(first usable span %d)" % (c["name"], F, FORK_MIN_F))
            continue
        if R + FORK_RING_TAIL >= spans:
            reasons.append("%s: rejoins at span %d, too close to the finish "
                           "(ring is %d spans)" % (c["name"], R, spans))
            continue
        if L < floor:
            reasons.append("%s: only %d span(s) long, a %s fork needs %d"
                           % (c["name"], L, c["kind"], floor))
            continue
        if len(forks) >= FORK_MAX_COUNT:
            reasons.append("%s: the engine holds %d forks and they are taken"
                           % (c["name"], FORK_MAX_COUNT))
            continue
        if spans + corridor + 1 + L > FORK_TOTAL_SPAN_CAP:
            reasons.append("%s: %d more corridor spans would push the track past "
                           "the %d-span cap" % (c["name"], 1 + L,
                                                FORK_TOTAL_SPAN_CAP))
            continue
        corridor += 1 + L
        taken.append((F - FORK_WIDEN_PAD, R + FORK_TAIL_PAD))
        forks.append({
            "id": c["id"], "kind": c["kind"], "name": c["name"],
            "F": F, "len": L, "sep": c["sep"], "lanes": int(c["lanes"]),
            "length_m": c["length_m"], "source": c["source"],
            "detail": c["detail"],
        })
    for i in unknown:
        reasons.append("%s: no longer detected on this route" % i)
    res["forks"] = forks
    res["fork_reasons"] = reasons
    res["fork_corridor_spans"] = corridor
    return res


def save_forks(slug: str, waypoints: list[list[float]], want_ids: list,
               root: str = CACHE_ROOT) -> dict:
    """Write FORKS.JSON only (the SAVE FORKS action), leaving ROUTE.JSON alone.

    Refuses when the confirmed set would change the route, because ROUTE.JSON's
    lane counts and FORKS.JSON's span ranges are one artefact: a fork whose
    window is not widened in ROUTE.JSON is a fork the engine will skip."""
    res = resolve_forks(slug, waypoints, want_ids, root)
    if not res.get("ok"):
        return {"ok": False, "reasons": res.get("reasons") or ["route not usable"]}
    d = place_dir(slug, root)
    routed = read_json(os.path.join(d, "ROUTE.JSON")) if os.path.isfile(
        os.path.join(d, "ROUTE.JSON")) else None
    if routed is None:
        return {"ok": False, "reasons": ["send the route to the game first"]}
    if int(routed.get("spans", -1)) != int(res["_cond"]["spans"]):
        return {"ok": False, "reasons": [
            "these forks change the road width, so the route has %d spans "
            "instead of the saved %d -- use SEND TO GAME to store both"
            % (res["_cond"]["spans"], routed.get("spans"))]}
    _write_forks(d, slug, res)
    return {"ok": True, "forks": res["forks"], "reasons": res["fork_reasons"]}


def _write_forks(d: str, slug: str, res: dict) -> None:
    """FORKS.JSON, or its removal when nothing is confirmed.

    Removal matters: td5_geo_forks treats a missing file as "no geo forks" and
    the generator then falls back to its own synthetic fork placement, so
    leaving a stale file behind would silently keep old forks alive."""
    p = os.path.join(d, FORKS_NAME)
    if not res.get("forks"):
        if os.path.isfile(p):
            os.remove(p)
        return
    write_json(p, {
        "place": slug,
        "spans": res["_cond"]["spans"],
        "corridor_spans": res.get("fork_corridor_spans", 0),
        "forks": res["forks"],
    })


def route_and_condition(slug: str, waypoints: list[list[float]],
                        root: str = CACHE_ROOT,
                        lanes: list[int] | None = None) -> dict:
    if len(waypoints) < 2:
        return {"ok": False, "reasons": ["click A and B first"]}
    place, g, proj = _load_graph(slug, root)
    gv = graph_verdict(place, g)
    if not gv["ok"]:
        return {"ok": False, "reasons": gv["reasons"], "warnings": gv["warnings"],
                "graph": gv}
    clicks = click_verdict(g, proj, waypoints)
    if clicks:
        return {"ok": False, "reasons": clicks, "warnings": gv["warnings"],
                "graph": gv}
    wps = [proj.to_world(float(la), float(lo)) for la, lo in waypoints]
    r = g.route(wps)
    if not r.get("ok"):
        return {"ok": False,
                "reasons": [r.get("reason", "routing failed")
                            + " -- A and B are on roads that do not connect"],
                "warnings": gv["warnings"], "graph": gv}

    latlon = [proj.world_to_latlon(x, z) for x, z in r["points"]]
    cond = geo_condition.condition_route(latlon, lanes if lanes else r["lanes"])

    # Crossings are reported in the CONDITIONED frame; map them back to the map
    # so the page can put a marker where the user has to drag.
    marks = []
    if cond.get("projection") and cond.get("points"):
        pr = cond["projection"]
        cp = LocalProjection(pr["lat0"], pr["lon0"], pr["units_per_metre"])
        cp.set_rotation(pr.get("rotation_rad", 0.0))
        cp.set_offset(pr.get("offset_x", 0.0), pr.get("offset_z", 0.0))
        pts = cond["points"]
        for c in cond.get("crossings", []):
            a = pts[min(c.get("node_a", 0), len(pts) - 1)]
            la, lo = cp.world_to_latlon(a["x"], a["z"])
            marks.append({"lat": la, "lon": lo,
                          "span_gap": c.get("span_gap"),
                          "distance_m": (c.get("distance_units", 0.0)
                                         / pr["units_per_metre"])})
    upm = place["projection"]["units_per_metre"]
    return {
        "ok": bool(cond.get("ok")),
        "reasons": cond.get("reasons", []),
        "warnings": gv["warnings"] + cond.get("warnings", []),
        "graph": gv,
        "polyline": [[round(la, 7), round(lo, 7)] for la, lo in latlon],
        "street_names": r.get("street_names", []),
        "length_km": r["length_units"] / upm / 1000.0,
        "spans": cond.get("spans"),
        "span_cap": TD5_TG_MAX_SPANS,
        "direction": cond.get("direction"),
        "worst_turn_deg": (cond.get("curvature") or {}).get("worst_final_deg"),
        "monotone_pct": round(100.0 * (cond.get("final") or {}).get("monotone_frac", 0.0), 1),
        "crossings": marks,
        "_cond": cond,
        "_raw": {"latlon": latlon, "lanes": r["lanes"], "route": r},
    }


def save_route(slug: str, waypoints: list[list[float]], root: str = CACHE_ROOT,
               refetch: bool = True, forks: list | None = None) -> dict:
    res = (resolve_forks(slug, waypoints, list(forks), root) if forks
           else route_and_condition(slug, waypoints, root))
    if not res["ok"]:
        return {"ok": False, "reasons": res["reasons"] or ["route not usable"]}
    d = place_dir(slug, root)
    cond = res.pop("_cond")
    raw = res.pop("_raw")
    write_json(os.path.join(d, "ROUTE_RAW.JSON"), {
        "place": slug,
        "waypoints": waypoints,
        "length_km": res["length_km"],
        "street_names": res["street_names"],
        "points": [{"lat": round(la, 7), "lon": round(lo, 7), "lanes": ln}
                   for (la, lo), ln in zip(raw["latlon"], raw["lanes"])],
    })
    write_json(os.path.join(d, "ROUTE.JSON"), cond)
    # FORKS.JSON alongside ROUTE.JSON, ALWAYS -- including the delete when
    # nothing is confirmed. The two files are one artefact (see _write_forks).
    _write_forks(d, slug, {"forks": res.get("forks", []),
                           "fork_corridor_spans": res.get("fork_corridor_spans", 0),
                           "_cond": cond})

    if refetch:
        place = read_json(os.path.join(d, "PLACE.JSON"))
        # Pin the routing area to what the user routed in, the FIRST time only:
        # a later save must not widen it again (see _load_graph).
        graph_bbox = place.get("route_graph_bbox") or place.get("bbox")
        pr = cond["projection"]
        centre = (place["centre"]["lat"], place["centre"]["lon"])
        origin = (pr["lat0"], pr["lon0"])
        radius = float(place["radius_m"]) + _haversine_m(centre, origin) + 200.0
        with _fetch_lock:
            _geo_fetch().fetch_place(place["name"], origin[0], origin[1], radius,
                                     root, pr["units_per_metre"], None,
                                     rotation_rad=pr["rotation_rad"],
                                     offset_x=pr["offset_x"],
                                     offset_z=pr["offset_z"])
        if graph_bbox:
            pj = os.path.join(d, "PLACE.JSON")
            newp = read_json(pj)
            newp["route_graph_bbox"] = graph_bbox
            write_json(pj, newp)
        _graph_cache.pop(root + "|" + slug, None)

    write_selected(slug, root)
    return {"ok": True, "spans": cond["spans"], "length_km": res["length_km"],
            "selected": slug, "forks": res.get("forks", []),
            "fork_reasons": res.get("fork_reasons", [])}


def fetch_area(name: str, lat: float, lon: float, radius: float,
               root: str = CACHE_ROOT) -> dict:
    """geo_fetch one circle. The BBOX CAP is checked here and REFUSED rather
    than silently clamped: the old `min(radius, 6000)` turned a 6 km ask into a
    6 km fetch with no way for the user to know the number they picked was not
    the number they got, and 6 km is a 12 x 12 km box -- 144 km2, four times the
    area plan section 0 sized the cap for ("a 6.3 km route wants roughly a
    4 x 4 km box with margin")."""
    if not name.strip():
        return {"ok": False, "reasons": ["give the place a name"]}
    gf = _geo_fetch()
    bad = gf.area_cap_reasons(float(radius))
    if bad:
        return {"ok": False, "reasons": bad}
    with _fetch_lock:
        p = gf.fetch_place(name.strip(), float(lat), float(lon),
                           float(radius), root)
    return {"ok": True, "slug": p["slug"], "name": p["name"],
            "clipped": ((p.get("layers") or {}).get("osm") or {})
                       .get("counts", {}).get("clipped_from")}


# ------------------------------------------------------------------ http ---

def handle_api(method: str, path: str, body: dict, root: str = CACHE_ROOT) -> tuple[int, dict]:
    """Pure dispatch, so the API is testable without opening a socket."""
    try:
        if method == "GET" and path == "/api/state":
            return 200, {"places": list_places(root), "selected": read_selected(root)}
        if method == "POST" and path == "/api/route":
            forks = body.get("forks") or []
            r = (resolve_forks(body["place"], body.get("waypoints", []), forks, root)
                 if forks
                 else route_and_condition(body["place"], body.get("waypoints", []), root))
            r.pop("_cond", None)
            r.pop("_raw", None)
            return 200, r
        if method == "POST" and path == "/api/forks":
            return 200, fork_candidates(body["place"], body.get("waypoints", []), root)
        if method == "POST" and path == "/api/saveforks":
            return 200, save_forks(body["place"], body.get("waypoints", []),
                                   body.get("forks") or [], root)
        if method == "POST" and path == "/api/save":
            return 200, save_route(body["place"], body.get("waypoints", []), root,
                                   forks=body.get("forks") or None)
        if method == "POST" and path == "/api/fetch":
            return 200, fetch_area(body.get("name", ""), body["lat"], body["lon"],
                                   body.get("radius", 2200), root)
        if method == "POST" and path == "/api/select":
            write_selected(body.get("place", ""), root)
            return 200, {"ok": True, "selected": body.get("place", "")}
        return 404, {"ok": False, "reasons": ["no such endpoint"]}
    except Exception as e:  # surfaced in the page, never a silent hang
        return 500, {"ok": False, "reasons": ["%s: %s" % (type(e).__name__, e)]}


class _Handler(BaseHTTPRequestHandler):
    root = CACHE_ROOT

    def _send(self, code: int, data: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, code: int, obj: dict) -> None:
        self._send(code, json.dumps(obj).encode("utf-8"), "application/json")

    def do_GET(self):  # noqa: N802
        if self.path.startswith("/api/"):
            self._json(*handle_api("GET", self.path.split("?")[0], {}, self.root))
            return
        name = "index.html" if self.path in ("/", "/index.html") else None
        if not name:
            self._send(404, b"not found", "text/plain")
            return
        with open(os.path.join(PAGE_DIR, name), "rb") as f:
            self._send(200, f.read(), "text/html; charset=utf-8")

    def do_POST(self):  # noqa: N802
        n = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            self._json(400, {"ok": False, "reasons": ["bad JSON"]})
            return
        self._json(*handle_api("POST", self.path, body, self.root))

    def log_message(self, fmt, *args):  # quieter console
        sys.stderr.write("[selector] " + (fmt % args) + "\n")


# ------------------------------------------------------------- self-test ---

#: The synthetic grid the self-test routes on. One horizontal line is a real
#: DIVIDED AVENUE -- two one-way ways of the same name, 12 m apart, running
#: anti-parallel -- because that is the shape geo_forks has to recognise and
#: there is no other way to exercise the fork path with no network. The
#: verticals carry extra vertices at the carriageways' northings so the
#: junctions exist in the graph (RoadGraph recovers junctions from shared
#: geometry, and 12 m apart is not shared).
TEST_AVENUE_N = 0.0        # the grid line the avenue replaces
TEST_AVENUE_HALF_M = 6.0   # each carriageway this far off it


def _build_test_place(tmp: str, slug: str = "testgrid",
                      blocks: int = 12, block: float = 110.0) -> tuple:
    d = place_dir(slug, tmp)
    os.makedirs(d, exist_ok=True)
    lat0, lon0, upm = -34.92, -57.95, 430.0
    proj = LocalProjection(lat0, lon0, upm)
    half = blocks * block / 2.0
    av_ns = (TEST_AVENUE_N - TEST_AVENUE_HALF_M, TEST_AVENUE_N + TEST_AVENUE_HALF_M)

    def pt(e, no):
        la, lo = proj.to_degrees(e, no)
        x, z = proj.to_world(la, lo)
        return {"x": x, "z": z}, [la, lo]

    roads, rid = [], 1

    def add(name, cls, lanes, oneway, coords):
        nonlocal rid
        pts, lls = [], []
        for e, no in coords:
            p, ll = pt(e, no)
            pts.append(p)
            lls.append(ll)
        roads.append({"id": rid, "name": name, "class": cls, "lanes": lanes,
                      "oneway": oneway, "median": False,
                      "points": pts, "latlon": lls})
        rid += 1

    av_row = blocks // 2
    for k in range(blocks + 1):
        no = k * block - half
        if k != av_row:                       # ordinary two-way cross street
            add("Calle %d" % (100 + k), "residential", 2, False,
                [(s * block - half, no) for s in range(blocks + 1)])
    # The divided avenue, replacing row av_row: eastbound then westbound.
    add("Avenida Central", "primary", 2, True,
        [(s * block - half, av_ns[0]) for s in range(blocks + 1)])
    add("Avenida Central", "primary", 2, True,
        [(s * block - half, av_ns[1]) for s in reversed(range(blocks + 1))])
    for k in range(blocks + 1):
        e = k * block - half
        norths = sorted([s * block - half for s in range(blocks + 1)]
                        + list(av_ns))
        add("Diagonal %d" % (200 + k), "residential", 2, False,
            [(e, no) for no in norths])

    write_json(os.path.join(d, "ROADS.JSON"), {"roads": roads})
    write_json(os.path.join(d, "PLACE.JSON"), {
        "name": "Test Grid", "slug": slug, "centre": {"lat": lat0, "lon": lon0},
        "radius_m": 900, "projection": proj.describe(),
        "attribution": ["Map data (c) OpenStreetMap contributors, ODbL 1.0"],
        # Item 2's fixture: one wired layer per shape plus one that is NOT.
        "layers": {
            "osm": {"source": "OpenStreetMap via Overpass",
                    "licence": "ODbL 1.0 -- attribution required on screen",
                    "vintage": "live at fetch time",
                    "counts": {"roads": len(roads), "buildings": 0}},
            "height": {"source": "terrarium", "native_m": 30.0,
                       "ground_res_m": 7.8, "relief_m": 0.0,
                       "vintage": "SRTM-derived",
                       "licence": "ODbL / public-domain mix"},
            "cover": {"source": "OSM landuse/leisure (ESA WorldCover not yet wired)",
                      "painted": {"built_cells": 1, "water_cells": 0,
                                  "cover_cells": 4}},
            "canopy": {"source": "NOT YET WIRED -- Meta/WRI Global Canopy "
                                 "Height, 1 m, CC-BY 4.0"},
        }})
    return d, proj, roads


def _self_test() -> int:
    """Offline: synthetic 'places' in a temp dir, routed, conditioned, forked and
    saved (no re-fetch). Proves the whole API without network or sockets.

    Covers, besides the original route/condition/save path:
      * per-layer source + vintage, including an honest NOT WIRED (item 2);
      * the caps and every "not enough road here" refusal (item 3);
      * fork detection, confirmation, span placement and FORKS.JSON (item 1).
    """
    tmp = tempfile.mkdtemp(prefix="geo_selector_")
    slug = "testgrid"
    d, proj, roads = _build_test_place(tmp, slug)

    def at(e, no):
        return list(proj.to_degrees(e, no))

    fails = 0
    code, st = handle_api("GET", "/api/state", {}, tmp)
    ok = code == 200 and st["places"] and st["places"][0]["slug"] == slug
    print("state        :", "OK" if ok else "FAIL", len(st.get("places", [])), "place(s)")
    fails += not ok

    # SW corner -> the middle of the divided avenue -> SE corner. Chosen so the
    # route spends one unbroken run on the avenue, which is what the fork
    # checks below need, and so it converges (the old pre-avenue waypoints put
    # a 36.5 deg turn against a 32.3 deg limit once the avenue replaced a grid
    # line).
    wps = [at(-660, -660), at(0, -TEST_AVENUE_HALF_M), at(660, -660)]
    code, r = handle_api("POST", "/api/route", {"place": slug, "waypoints": wps}, tmp)
    ok = code == 200 and r.get("polyline") and r.get("spans")
    print("route        :", "OK" if ok else "FAIL",
          "%s spans, %.2f km, verdict %s, reasons %s"
          % (r.get("spans"), r.get("length_km", 0), r.get("ok"), r.get("reasons")))
    fails += not ok

    # Out and back down the same avenue: the conditioner must flag it.
    code, rb = handle_api("POST", "/api/route",
                          {"place": slug, "waypoints": [at(-660, 0), at(660, 0), at(-500, 0)]}, tmp)
    ok = code == 200 and not rb.get("ok") and rb.get("crossings")
    print("retrace flag :", "OK" if ok else "FAIL",
          "%d crossing marker(s), verdict %s" % (len(rb.get("crossings", [])), rb.get("ok")))
    fails += not ok

    if r.get("ok"):
        s = save_route(slug, wps, tmp, refetch=False)
        routed = read_json(os.path.join(d, "ROUTE.JSON"))
        ok = (s.get("ok") and read_selected(tmp) == slug
              and routed["points"][0]["x"] == 0 and routed["points"][0]["z"] == 0)
        print("save         :", "OK" if ok else "FAIL", s)
        fails += not ok
    else:
        print("save         : SKIPPED (route verdict not OK:", r.get("reasons"), ")")

    code, bad = handle_api("POST", "/api/nope", {}, tmp)
    ok = code == 404
    print("unknown path :", "OK" if ok else "FAIL")
    fails += not ok

    # ---- item 2: per-layer source + vintage ---------------------------------
    rows = {x["key"]: x for x in st["places"][0]["layers"]}
    ok = (len(rows) == 4
          and rows["osm"]["wired"] and rows["osm"]["vintage"]
          and rows["height"]["wired"] and rows["height"]["vintage"]
          and rows["cover"]["wired"]                 # OSM-painted, so WIRED
          and not rows["canopy"]["wired"]            # and this one says so
          and "NOT YET WIRED" not in rows["canopy"]["source"].upper()
          and ("%d roads" % len(roads)) in rows["osm"]["detail"])
    print("layers       :", "OK" if ok else "FAIL",
          "osm/%s height/%s cover/%s canopy/%s"
          % tuple("wired" if rows[k]["wired"] else "NOT WIRED"
                  for k in ("osm", "height", "cover", "canopy")))
    fails += not ok

    layers_missing = layer_rows({})
    ok = len(layers_missing) == 4 and not any(x["wired"] for x in layers_missing)
    print("layers empty :", "OK" if ok else "FAIL",
          "a place with no layers block reports 4 NOT WIRED rows")
    fails += not ok

    # ---- item 3: caps and every refusal -------------------------------------
    import geo_fetch as _gf            # numpy/PIL: only needed for these checks
    ok = (not _gf.area_cap_reasons(2200.0)
          and _gf.area_cap_reasons(300.0)
          and _gf.area_cap_reasons(6000.0))
    print("bbox cap     :", "OK" if ok else "FAIL",
          "2200 m accepted, 300 m and 6000 m refused (cap %.0f m)"
          % _gf.FETCH_MAX_RADIUS_M)
    fails += not ok

    dense = ([{"class": "service", "id": i} for i in range(5000)]
             + [{"class": "residential", "id": 10000 + i} for i in range(3000)]
             + [{"class": "primary", "id": 20000 + i} for i in range(400)])
    kept, rep = _gf.clip_roads(dense)
    ok = (len(kept) <= _gf.FETCH_MAX_DRIVABLE_WAYS
          and rep["clipped_from"] == 8400
          and sum(1 for r in kept if r["class"] == "primary") == 400)
    print("way cap      :", "OK" if ok else "FAIL",
          "%d -> %d ways, every primary kept, dropped %s"
          % (rep.get("clipped_from", 0), len(kept), rep.get("dropped_classes")))
    fails += not ok
    ok = clip_roads_identity = (_gf.clip_roads(dense[:100]) == (dense[:100], {}))
    print("way cap idle :", "OK" if ok else "FAIL",
          "under the cap the clip is the identity")
    fails += not ok

    # A place with almost no road: refused with a sentence, and SEND is off.
    thin = "testthin"
    dthin = place_dir(thin, tmp)
    os.makedirs(dthin, exist_ok=True)
    write_json(os.path.join(dthin, "ROADS.JSON"), {"roads": roads[:2]})
    write_json(os.path.join(dthin, "PLACE.JSON"),
               {"name": "Thin", "slug": thin, "centre": {"lat": -34.92, "lon": -57.95},
                "radius_m": 900, "projection": proj.describe()})
    code, rt = handle_api("POST", "/api/route",
                          {"place": thin, "waypoints": wps[:2]}, tmp)
    ok = (code == 200 and not rt["ok"] and rt["reasons"]
          and "not enough drivable road" in rt["reasons"][0])
    print("thin place   :", "OK" if ok else "FAIL", (rt.get("reasons") or [""])[0][:74])
    fails += not ok

    # A click nowhere near a road: named, with the distance, and refused.
    code, rf = handle_api("POST", "/api/route",
                          {"place": slug,
                           "waypoints": [at(-660, -660), at(9000, 9000)]}, tmp)
    ok = (code == 200 and not rf["ok"] and rf["reasons"]
          and "from the nearest drivable road" in rf["reasons"][0])
    print("far click    :", "OK" if ok else "FAIL", (rf.get("reasons") or [""])[0][:74])
    fails += not ok

    # Two road systems that never meet: the routing failure says so.
    split = "testsplit"
    dsp = place_dir(split, tmp)
    os.makedirs(dsp, exist_ok=True)
    # A SECOND WHOLE GRID far away, not a handful of roads: translating a
    # subset would give one component per parallel line (measured: 15), and the
    # case being tested is two road SYSTEMS that never meet.
    far = []
    for r0 in roads:
        r2 = dict(r0)
        r2["id"] = r0["id"] + 90000
        r2["points"] = [{"x": q["x"] + 5.0e6, "z": q["z"]} for q in r0["points"]]
        far.append(r2)
    write_json(os.path.join(dsp, "ROADS.JSON"), {"roads": roads + far})
    write_json(os.path.join(dsp, "PLACE.JSON"),
               {"name": "Split", "slug": split, "centre": {"lat": -34.92, "lon": -57.95},
                "radius_m": 900, "projection": proj.describe()})
    _place, gsp, _pr = _load_graph(split, tmp)
    gv = graph_verdict(read_json(os.path.join(dsp, "PLACE.JSON")), gsp)
    ok = gv["components"] == 2 and (gv["warnings"] or gv["reasons"])
    print("split graph  :", "OK" if ok else "FAIL",
          "%d component(s), biggest %d of %d"
          % (gv["components"], gv["main_component"], gv["nodes"]))
    fails += not ok

    # ---- item 1: fork candidates, confirmation, placement, FORKS.JSON -------
    code, fc = handle_api("POST", "/api/forks",
                          {"place": slug, "waypoints": wps}, tmp)
    med = [c for c in fc.get("candidates", []) if c["kind"] == "ISLAND"]
    ok = (code == 200 and fc["ok"] and med
          and med[0]["name"] == "Avenida Central"
          and 4.0 <= med[0]["gap_m"] <= 45.0
          and med[0]["lanes"] == 4)
    print("fork detect  :", "OK" if ok else "FAIL",
          "%d candidate(s)%s" % (len(fc.get("candidates", [])),
          (": %s spans %d-%d, %.0f m apart, cover %.2f"
           % (med[0]["name"], med[0]["span_from"], med[0]["span_to"],
              med[0]["gap_m"], med[0]["cover"])) if med else ""))
    fails += not ok

    if med:
        want = [med[0]["id"]]
        res = resolve_forks(slug, wps, want, tmp)
        got = res.get("forks") or []
        lanes_out = [int(p["lanes"]) for p in res["_cond"]["points"]]
        seams = [abs(lanes_out[i + 1] - lanes_out[i])
                 for i in range(len(lanes_out) - 1)
                 if lanes_out[i] != lanes_out[i + 1]]
        win = set()
        if got:
            f0 = got[0]
            win = set(lanes_out[f0["F"] - 8: f0["F"] + 1 + f0["len"] + 3])
        ok = (len(got) == 1 and got[0]["kind"] == "ISLAND"
              and got[0]["F"] >= FORK_MIN_F
              and got[0]["F"] + 1 + got[0]["len"] + FORK_RING_TAIL < res["spans"]
              and win == {got[0]["lanes"]}          # the engine's uniformity rule
              and (not seams or max(seams) == 1))   # and its lane-seam rule
        print("fork place   :", "OK" if ok else "FAIL",
              ("F=%d len=%d lanes=%d, window %s, %d seam(s) max step %d"
               % (got[0]["F"], got[0]["len"], got[0]["lanes"], sorted(win),
                  len(seams), max(seams) if seams else 0)) if got
              else "nothing placed: %s" % res.get("fork_reasons"))
        fails += not ok

        s = save_route(slug, wps, tmp, refetch=False, forks=want)
        fj = os.path.join(d, FORKS_NAME)
        saved = read_json(fj) if os.path.isfile(fj) else {}
        ok = (s["ok"] and len(saved.get("forks", [])) == 1
              and saved["spans"] == s["spans"]
              and saved["forks"][0]["kind"] == "ISLAND"
              # and the state endpoint hands the page back what to re-tick
              and [x["forks_saved"] for x in list_places(tmp)
                   if x["slug"] == slug] == [saved["forks"]])
        print("fork save    :", "OK" if ok else "FAIL",
              "%d fork(s), route %s spans" % (len(saved.get("forks", [])),
                                              saved.get("spans")))
        fails += not ok

        # Un-confirming has to DELETE the file: a stale one would keep forks
        # the user turned off alive in the generator.
        s = save_route(slug, wps, tmp, refetch=False, forks=None)
        ok = s["ok"] and not os.path.isfile(fj)
        print("fork clear   :", "OK" if ok else "FAIL",
              "FORKS.JSON removed when nothing is confirmed")
        fails += not ok

        # An id that is not on this route is reported, not silently ignored.
        res = resolve_forks(slug, wps, ["median:Nowhere:0-1"], tmp)
        ok = not res.get("forks") and any("no longer detected" in x
                                          for x in res.get("fork_reasons", []))
        print("fork unknown :", "OK" if ok else "FAIL",
              "%s" % (res.get("fork_reasons") or ["(silent)"])[0][:60])
        fails += not ok

    print("RESULT:", "OK" if not fails else "%d FAIL" % fails, "(temp dir %s)" % tmp)
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--root", default=CACHE_ROOT)
    ap.add_argument("--self-test", action="store_true",
                    help="offline API test on a synthetic grid; opens no socket")
    a = ap.parse_args(argv)
    if a.self_test:
        return _self_test()
    _Handler.root = a.root
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), _Handler)
    print("GEO TRACK selector on http://127.0.0.1:%d/  (cache %s)" % (a.port, a.root))
    print("Ctrl+C to stop.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
