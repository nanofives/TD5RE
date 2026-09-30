"""geo_forks.py -- FORK CANDIDATES along a chosen route (GEO TRACK Phase 4,
docs/plans/GEO_TRACK_OSM_PLAN.md section 7 "show detected fork candidates
(median avenues, comparable alternative routes) as toggles the user confirms").

Two kinds, because the generator already has exactly two shapes that fit:

  ISLAND -- a MEDIAN AVENUE. OSM splits a dual carriageway into two one-way
    ways that carry the SAME name and run anti-parallel a few metres apart.
    The route drives one of them; the other one is the fork's corridor. This is
    the only kind that is a FACT about the map rather than a choice: the second
    carriageway exists whether or not the user confirms it.

  BRANCH -- a COMPARABLE ALTERNATIVE ROUTE. Ban the chosen leg's own edges and
    ask the router again: if a second path to the same waypoint exists and is
    not much longer, that is a real alternative a driver could take, and the
    engine's long diverging branch (TG_FORK_WIDE) is the shape for it.

WHAT THIS MODULE DOES NOT DO. It works in RAW ROUTE VERTEX space -- the
sequence RoadGraph.route() hands back -- and knows nothing about spans. The
raw range is what geo_selector needs to widen the lanes BEFORE conditioning
(a median avenue is genuinely lanes(A)+lanes(B) wide, and the curvature floor
has to be enforced for that width, not for one carriageway's); the SPAN range
is read back off the conditioned result afterwards. Keeping the two apart is
what stops the two from chasing each other: nothing here depends on the
conditioner's output, so there is no fixed point to solve.

WHY A MEDIAN IS AN ISLAND AND NOT AN AVENUE. TG_FORK_AVENUE and TG_FORK_ISLAND
differ only in their plan-table length and separation; both read as a divided
avenue (sep <= TD5_TG_AVENUE_SEP_MAX, td5_trackgen_internal.h:1702) and both
get the central divider. ISLAND is the one whose length floor is 3 spans rather
than TD5_TG_BRANCH_MIN_LEN (24), so a real median of any length survives
tg_fork_len_floored unchanged -- which is the property that lets the span range
in FORKS.JSON mean what it says.

Thresholds are measured on the La Plata cache; see THRESHOLDS below for each
number and why it is where it is. Offline and deterministic: no network, no
RNG, no I/O beyond what the caller passes in.
"""
from __future__ import annotations

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo_common import LocalProjection  # noqa: E402,F401  (typing/doc only)

# ----------------------------------------------------------- THRESHOLDS ---
#
# MEASURED on re/assets/geo/la_plata (2291 drivable ways, 1650 inside the pinned
# routing bbox, 1893 of the 2291 tagged oneway -- La Plata's grid is one-way
# almost everywhere, so "oneway" alone is worthless as a median signal and the
# geometry below is what does the work).
#
# MEDIAN_MIN_M / MEDIAN_MAX_M -- lateral gap between the two carriageways.
#   Plan section 5 measured a 10 m median at 4300 world units, "no, marginally"
#   against the engine's own overlap test, so 4 m is the floor below which the
#   two carriageways are the same road drawn twice rather than a divided one.
#   45 m is above La Plata's widest boulevard (Avenida 7 / 13 / 44, ~30 m
#   between carriageway centrelines) and below one city block (110 m), so a
#   PARALLEL STREET one block over can never be mistaken for a median.
MEDIAN_MIN_M = 4.0
MEDIAN_MAX_M = 45.0
# Bearing agreement. A median carriageway runs anti-parallel (the two directions
# of one avenue); 35 deg of slack covers the divergence at junctions and the
# bend of a diagonal without admitting a crossing street.
MEDIAN_ANTIPARALLEL_TOL_DEG = 35.0
# A same-name way that runs PARALLEL (not anti-parallel) at median distance is
# a service road or a bus lane, not the other carriageway. Accepted too, but
# only when the way is itself one-way, so it is still a separate carriageway.
MEDIAN_PARALLEL_TOL_DEG = 25.0
# How much of the run has to find a partner, counted as a UNION over every
# same-name way (not per-way). Per-way was tried first and measured wrong: OSM
# splits the opposing carriageway of Calle 54 into ten ways, so the best single
# partner covered 18% of a 1055 m divided run that is divided end to end. The
# union scores it 0.76.
#   MEASURED on the reference La Plata route, union coverage per run:
#     0.00 Diagonal 101, 0.00 Diagonal 102, 0.00 Calle 6   -- undivided, correct
#     0.44 Calle 54 (20-28)   -- opposing carriageway mapped for part of it only
#     0.55 Avenida 53 (44-54) | 0.62 plaza ring | 0.70 Avenida 52
#     0.76 Calle 54 (66-98)   | 0.83 Avenida 53 (32-37) | 1.00 Avenida 53 (102-109)
#   0.50 sits in the 0.44 -> 0.55 gap, so every stretch that is divided on the
#   ground is taken and the one partial case is refused.
MEDIAN_MIN_COVER = 0.50
# A median shorter than this is a junction artefact. 120 m is one La Plata
# block (110 m) plus its intersection.
MEDIAN_MIN_LEN_M = 120.0

# ALTERNATIVE ROUTE. "Comparable" is the word in the plan; 1.35 is the ratio at
# which the alternative is still something a driver would take. Below 200 m it
# is a block-long detour around a median, which the ISLAND kind already covers
# and which would only duplicate it.
ALT_MAX_RATIO = 1.35
ALT_MIN_LEN_M = 200.0
# An alternative that rejoins within this many raw vertices of where it left is
# not a branch, it is noise in the graph.
ALT_MIN_VERTICES = 4
# CEILING on a branch, and it is not cosmetic. The engine APPENDS a corridor of
# 1+len spans after the ring, so a branch that shadows a whole leg doubles the
# track. Measured: the unbanned re-route of the La Plata reference leg returns a
# 5482 m path against 5124 m chosen -- comparable by every other test, and
# useless as a fork, because its 0-118 vertex range covers the entire route and
# (by the overlap rule in detect) would evict all five real medians. A branch is
# a DETOUR, so cap it at a quarter of the route and at 1200 m.
ALT_MAX_FRAC_OF_ROUTE = 0.25
ALT_MAX_LEN_M = 1200.0

# Never hand back more than the engine can hold (TD5_TG_BRANCH_MAX = 8), and
# never more than the page can sensibly show.
MAX_CANDIDATES = 8


def _bearing(ax: float, az: float, bx: float, bz: float) -> float:
    return math.atan2(bx - ax, bz - az)


def _angdiff_deg(a: float, b: float) -> float:
    d = (a - b + math.pi) % (2.0 * math.pi) - math.pi
    return abs(math.degrees(d))


def _seg_point_dist(px: float, pz: float,
                    ax: float, az: float, bx: float, bz: float) -> tuple[float, float]:
    """Distance from P to segment AB, and the segment's bearing."""
    dx, dz = bx - ax, bz - az
    L2 = dx * dx + dz * dz
    if L2 <= 1e-12:
        return math.hypot(px - ax, pz - az), 0.0
    t = ((px - ax) * dx + (pz - az) * dz) / L2
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    cx, cz = ax + dx * t, az + dz * t
    return math.hypot(px - cx, pz - cz), _bearing(ax, az, bx, bz)


def _nearest_on_road(road: dict, px: float, pz: float) -> tuple[float, float]:
    """Closest approach of a road polyline to P: (distance, local bearing)."""
    pts = road.get("points") or []
    best, bb = float("inf"), 0.0
    for i in range(len(pts) - 1):
        d, br = _seg_point_dist(px, pz, pts[i]["x"], pts[i]["z"],
                                pts[i + 1]["x"], pts[i + 1]["z"])
        if d < best:
            best, bb = d, br
    return best, bb


# -------------------------------------------------------- median avenues ---

def _runs_by_road(road_ids: list, min_len: int = 2) -> list[tuple[int, int, object]]:
    """Maximal runs of consecutive route vertices carried by one OSM way."""
    out, i, n = [], 0, len(road_ids)
    while i < n:
        j = i
        while j + 1 < n and road_ids[j + 1] == road_ids[i]:
            j += 1
        if j - i + 1 >= min_len and road_ids[i] is not None:
            out.append((i, j, road_ids[i]))
        i = j + 1
    return out


def _merge_runs(runs: list[tuple[int, int, object]], roads_by_id: dict,
                gap: int = 2) -> list[tuple[int, int, object]]:
    """Join runs of the SAME NAME separated by a short gap. OSM splits one
    avenue into many ways at every junction, so an avenue reaches the route as
    a dozen short runs that are one road to a driver."""
    out: list[list] = []
    for a, b, rid in runs:
        nm = (roads_by_id.get(rid) or {}).get("name")
        if out and nm and out[-1][3] == nm and a - out[-1][1] <= gap:
            out[-1][1] = b
            continue
        out.append([a, b, rid, nm])
    return [(a, b, rid) for a, b, rid, _nm in out]


def detect_medians(roads: list[dict], pts_world: list[tuple[float, float]],
                   road_ids: list, upm: float) -> list[dict]:
    """Median-avenue (dual carriageway) candidates along the route."""
    roads_by_id = {r.get("id"): r for r in roads}
    by_name: dict[str, list[dict]] = {}
    for r in roads:
        nm = r.get("name")
        if nm:
            by_name.setdefault(nm, []).append(r)

    out = []
    for k0, k1, rid in _merge_runs(_runs_by_road(road_ids), roads_by_id):
        road = roads_by_id.get(rid)
        if not road:
            continue
        name = road.get("name")
        run_m = sum(math.dist(pts_world[i], pts_world[i + 1])
                    for i in range(k0, k1)) / upm
        if run_m < MEDIAN_MIN_LEN_M:
            continue

        tagged = bool(road.get("median"))
        peers = [r for r in by_name.get(name or "", []) if r.get("id") != rid]
        # An untagged median needs a partner carriageway; a TAGGED one (OSM
        # dual_carriageway / divider) is believed on its own, because the
        # mapper asserted it and the second carriageway may simply not be
        # split out in the data.
        hits, total, dsum = 0, 0, 0.0
        used: dict = {}
        step = max(1, (k1 - k0) // 12)
        for i in range(k0, k1 + 1, step):
            px, pz = pts_world[i]
            total += 1
            j = min(i + 1, len(pts_world) - 1)
            rbr = _bearing(px, pz, pts_world[j][0], pts_world[j][1])
            near = None
            for peer in peers:
                if not peer.get("oneway") and not tagged:
                    continue
                d, pbr = _nearest_on_road(peer, px, pz)
                d_m = d / upm
                if not (MEDIAN_MIN_M <= d_m <= MEDIAN_MAX_M):
                    continue
                anti = _angdiff_deg(rbr + math.pi, pbr)
                para = _angdiff_deg(rbr, pbr)
                if anti <= MEDIAN_ANTIPARALLEL_TOL_DEG or (
                        peer.get("oneway") and para <= MEDIAN_PARALLEL_TOL_DEG):
                    if near is None or d_m < near[0]:
                        near = (d_m, peer)
            if near is not None:
                hits += 1
                dsum += near[0]
                used[near[1].get("id")] = used.get(near[1].get("id"), 0) + 1

        cover = hits / float(total) if total else 0.0
        if cover < MEDIAN_MIN_COVER and not tagged:
            continue
        gap_m = dsum / hits if hits else 0.0
        # The carriageway the run pairs with most often is the one to show on
        # the map and to read the opposing lane count off.
        peer = None
        if used:
            pid = max(used.items(), key=lambda t: t[1])[0]
            peer = next((r for r in peers if r.get("id") == pid), None)
        lanes_a = int(road.get("lanes") or 2)
        lanes_b = int((peer or {}).get("lanes") or lanes_a)
        out.append({
            "id": "median:%s:%d-%d" % (name or rid, k0, k1),
            "kind": "ISLAND",
            "name": name or "unnamed",
            "class": road.get("class"),
            "k0": k0, "k1": k1,
            "length_m": round(run_m, 1),
            "lanes": min(8, max(4, lanes_a + lanes_b)),
            "sep": 0.16,
            "gap_m": round(gap_m, 1),
            "cover": round(cover, 2),
            "source": "OSM dual_carriageway tag" if (tagged and peer is None)
                      else "paired one-way ways, same name (%d of them)" % len(used),
            "detail": ("%s: %.0f m of divided avenue, carriageways %.0f m apart"
                       % (name or "unnamed", run_m, gap_m)) if peer else
                      ("%s: %.0f m tagged as a divided carriageway"
                       % (name or "unnamed", run_m)),
            "peer_latlon": [[p[0], p[1]] for p in (peer or {}).get("latlon", [])],
        })
    return out


# ---------------------------------------------------- alternative routes ---

def detect_alternatives(graph, seq: list[int], legs: list[list[int]],
                        upm: float) -> list[dict]:
    """A second, comparable path for a leg of the route.

    Method: ban the leg's own edges and re-route. That is the cheapest honest
    definition of "another way to get there" -- it cannot return the chosen
    path, and anything it does return is drivable by construction.
    """
    out = []
    # Where each leg's vertex 0 sits in the concatenated route. RoadGraph.route
    # joins legs with leg[1:], so leg li starts where leg li-1 ended.
    offs, acc = [], 0
    for leg in legs:
        offs.append(acc)
        acc += len(leg) - 1

    for li, leg in enumerate(legs):
        off = offs[li]
        if len(leg) < ALT_MIN_VERTICES:
            continue
        banned = {(min(leg[i], leg[i + 1]), max(leg[i], leg[i + 1]))
                  for i in range(len(leg) - 1)}
        alt = graph.path(leg[0], leg[-1], banned)
        if not alt:
            continue

        def plen(p):
            return sum(math.dist(graph.pos[p[i]], graph.pos[p[i + 1]])
                       for i in range(len(p) - 1))

        l_base, l_alt = plen(leg), plen(alt)
        if l_base <= 0.0 or l_alt > l_base * ALT_MAX_RATIO:
            continue
        if l_alt / upm < ALT_MIN_LEN_M:
            continue
        # Trim the shared prefix/suffix: the branch is only the part that is
        # genuinely somewhere else.
        p = 0
        while p < min(len(leg), len(alt)) and leg[p] == alt[p]:
            p += 1
        s = 0
        while (s < min(len(leg), len(alt)) - p
               and leg[-1 - s] == alt[-1 - s]):
            s += 1
        k0 = off + max(p - 1, 0)
        k1 = off + len(leg) - 1 - s
        if k1 - k0 < ALT_MIN_VERTICES:
            continue
        # The DIVERGING part's length on the chosen route, which is what the
        # engine has to append as a corridor.
        span_m = sum(math.dist(graph.pos[leg[i]], graph.pos[leg[i + 1]])
                     for i in range(max(p - 1, 0),
                                    min(len(leg) - 1 - s, len(leg) - 1))) / upm
        route_m = l_base / upm
        if span_m > ALT_MAX_LEN_M or span_m > route_m * ALT_MAX_FRAC_OF_ROUTE:
            continue
        lanes = max(int(graph.roads[graph.edge_road(leg[i], leg[i + 1])].get("lanes") or 2)
                    for i in range(max(p - 1, 0), len(leg) - 1 - s)
                    if 0 <= graph.edge_road(leg[i], leg[i + 1]) < len(graph.roads))
        out.append({
            "id": "alt:%d:%d-%d" % (li, k0, k1),
            "kind": "BRANCH",
            "name": "alternative for leg %d" % (li + 1),
            "class": None,
            "k0": k0, "k1": k1,
            "length_m": round(span_m, 1),
            "lanes": min(8, max(4, lanes + 2)),
            "sep": 1.00,
            "gap_m": 0.0,
            "cover": round(l_alt / l_base, 2),
            "source": "second drivable path, leg edges banned",
            "detail": ("leg %d: an alternative %.0f m against %.0f m chosen "
                       "(%.0f%% of it)" % (li + 1, l_alt / upm, l_base / upm,
                                           100.0 * l_alt / l_base)),
            "alt_world": [graph.pos[i] for i in alt],
        })
    return out


def detect(graph, route: dict, upm: float,
           max_candidates: int = MAX_CANDIDATES) -> list[dict]:
    """All fork candidates for one routed polyline, longest first."""
    if not route.get("ok"):
        return []
    cands = detect_medians(graph.roads, route["points"], route["road_ids"], upm)
    cands += detect_alternatives(graph, route["nodes"], route["leg_nodes"], upm)
    # ISLANDs first, then longest, then drop anything overlapping an
    # already-kept range: two forks sharing spans would fight over the same
    # carriageway, and a median is a FACT about the map while an alternative
    # route is one of several choices, so the fact wins the ground.
    cands.sort(key=lambda c: (c["kind"] != "ISLAND", -c["length_m"]))
    kept: list[dict] = []
    for c in cands:
        if any(not (c["k1"] < k["k0"] or c["k0"] > k["k1"]) for k in kept):
            continue
        kept.append(c)
        if len(kept) >= max_candidates:
            break
    kept.sort(key=lambda c: c["k0"])
    return kept
