"""geo_condition.py -- turn a real-world route into a TD5-legal centerline.

This is Option A of docs/plans/GEO_TRACK_OSM_PLAN.md section 5, and it is the
crux of the whole GEO TRACK feature.

THE PROBLEM. The generator's walk is non-trapping because it keeps every span
within +-80 degrees of the +X axis, so the axis coordinate strictly increases and
two nodes far apart in span index provably cannot coincide -- self-intersection
is geometrically IMPOSSIBLE, not merely rejected (td5_trackgen_internal.h:
1332-1373). A real A-to-B city drive is monotone in no axis, so that guarantee is
unconditionally lost, and the only remaining guard (tg_too_close,
td5_trackgen.c:1760) is described in its own comment as "a pure backstop that
never fires". When the road really does overlap, the engine localises a car to a
span by proximity, snaps to the wrong span, and spawn/progress/ground-probe all
break -- recorded symptom: car placed at span 1791 of 1800 on the start line,
airborne.

Note also what the same comment rules out: pure rejection sampling was tried
FIRST and traps (1800 spans requested, 300 delivered). So "keep drawing until it
does not cross" is not available to us either. The route has to be CONDITIONED.

WHAT THIS DOES, in order:
  1. pick the best of {forward, reversed} and rotate so the start tangent lands
     on +X -- mandatory, because the fixed straight lead-in runs along +X;
  2. resample to exactly TD5_TG_SPAN_LENGTH, since no per-node arclength field
     exists in TG_Node and uneven spacing is silently mis-measured;
  3. enforce the curvature floor, which is also what makes the engine's
     adjacent-skip exemption sound;
  4. prepend the 40-node lead-in and pin node 0 at the origin;
  5. test every non-adjacent span pair with the engine's own separation test and
     REPORT crossings rather than trying to invent a fix -- resolving one is a
     single waypoint drag in the selector, which is why the route mode and this
     tool were chosen to fit each other.

It never silently repairs a crossing. A crossing is a fact about the route the
user drew, and hiding it would trade a visible map marker for an invisible
in-game failure.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo_common import (  # noqa: E402
    ELEVATION_EXAGGERATION,
    LocalProjection,
    TD5_TG_CURVE_SAFETY_X100_DEFAULT,
    TD5_TG_LANE_WIDTH,
    TD5_TG_MAX_SPANS,
    TD5_TG_SPAN_LENGTH,
    TG_LEAD_IN_NODES,
    UNITS_PER_METRE_DERIVED,
    adjacent_skip,
    max_turn_per_span,
    min_turn_radius,
    too_close_need,
    write_json,
)

# The heading budget the engine's non-trapping proof rests on
# (td5_trackgen_internal.h:1370, TD5_TG_HEADING_LIMIT = 1.396 rad ~ 80 deg).
# We cannot enforce it on a real route, but we MEASURE against it: a route that
# stays inside it inherits the proof outright and cannot self-cross at all.
HEADING_LIMIT = 1.396


# ----------------------------------------------------------------- helpers ---

def _polyline_length(pts: list[tuple[float, float]]) -> float:
    return sum(math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1))


def _resample_uniform(pts: list[tuple[float, float]], step: float
                      ) -> list[tuple[float, float]]:
    """Resample so consecutive points are exactly `step` apart IN A STRAIGHT LINE.

    CHORD, not arc. The engine's walk integrates
    `x += sin(h)*span_len; z += cos(h)*span_len` (td5_tg_road.c:663-664), so its
    nodes are exactly one span apart in Euclidean distance. Arc-length resampling
    instead puts them one span apart ALONG THE PATH, and at a corner the chord
    falls short -- measured 34.6 units of shortfall at a 31.6 degree turn on the
    La Plata route, which geo_audit R4 failed on.

    Invariant 4 of the plan is why this has to be exact: capg
    (td5_tg_road.c:130), the lookahead (:512-513), the curvature term
    /(span_length^2) (:562), tg_adjacent_skip and the grade statistics (:1465)
    all assume a uniform span, and TG_Node (td5_trackgen_internal.h:1300-1316)
    carries no arclength field, so a wrong spacing is mis-measured in silence
    rather than rejected.

    Method: walk the polyline and place each next point where the circle of
    radius `step` about the previous point first crosses it.
    """
    if len(pts) < 2:
        return list(pts)
    out = [pts[0]]
    seg_i = 0            # segment currently being consumed
    t = 0.0              # position along that segment, 0..seg_len
    while seg_i < len(pts) - 1:
        cx, cz = out[-1]
        placed = False
        j = seg_i
        tt = t
        while j < len(pts) - 1:
            ax, az = pts[j]
            bx, bz = pts[j + 1]
            dx, dz = bx - ax, bz - az
            seg = math.hypot(dx, dz)
            if seg <= 1e-12:
                j += 1
                tt = 0.0
                continue
            ux, uz = dx / seg, dz / seg
            # |(a + u*s) - c| = step, solved for s >= tt.
            fx, fz = ax - cx, az - cz
            b = fx * ux + fz * uz
            c0 = fx * fx + fz * fz - step * step
            disc = b * b - c0
            if disc >= 0.0:
                root = math.sqrt(disc)
                for s in (-b + root, -b - root):
                    if tt - 1e-9 <= s <= seg + 1e-9:
                        out.append((ax + ux * s, az + uz * s))
                        seg_i, t = j, max(s, 0.0)
                        placed = True
                        break
            if placed:
                break
            j += 1
            tt = 0.0
        if not placed:
            break
    return out


def _headings(pts: list[tuple[float, float]]) -> list[float]:
    """Per-span heading in the engine's convention: atan2(dx, dz).

    td5_tg_road.c:663-664 integrates x += sin(h)*len, z += cos(h)*len, so the
    heading of a span is atan2(dx, dz) and TD5_TG_AXIS_HEADING (PI/2) is +X.
    """
    return [math.atan2(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
            for i in range(len(pts) - 1)]


def _wrap(a: float) -> float:
    while a > math.pi:
        a -= 2.0 * math.pi
    while a < -math.pi:
        a += 2.0 * math.pi
    return a


def _turns(pts: list[tuple[float, float]]) -> list[float]:
    """Signed heading change at each interior vertex."""
    h = _headings(pts)
    return [_wrap(h[i + 1] - h[i]) for i in range(len(h) - 1)]


def _principal_axis(pts: list[tuple[float, float]]) -> float:
    """Angle of the polyline's principal axis, by PCA on the vertices.

    Reported rather than applied: the start tangent has to be +X for the lead-in
    join, so this is a DIAGNOSTIC that tells the selector whether the route's
    overall direction agrees with where it starts. A large disagreement is why a
    route curls back on itself, which is the crossing risk.
    """
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    mz = sum(p[1] for p in pts) / n
    sxx = syy = sxy = 0.0
    for x, z in pts:
        dx, dz = x - mx, z - mz
        sxx += dx * dx
        syy += dz * dz
        sxy += dx * dz
    # Principal eigenvector angle of the 2x2 covariance, in the same
    # atan2(dx, dz) convention as _headings.
    theta = 0.5 * math.atan2(2.0 * sxy, sxx - syy)
    return math.atan2(math.cos(theta), math.sin(theta))


def _rotate(pts: list[tuple[float, float]], theta: float
            ) -> list[tuple[float, float]]:
    c, s = math.cos(theta), math.sin(theta)
    return [(x * c - z * s, x * s + z * c) for x, z in pts]


# ------------------------------------------------------- curvature control ---

def enforce_curvature(pts: list[tuple[float, float]],
                      width: float | list[float],
                      curve_safety_x100: int = TD5_TG_CURVE_SAFETY_X100_DEFAULT,
                      step: float = TD5_TG_SPAN_LENGTH,
                      max_iter: int = 200) -> tuple[list, dict]:
    """Smooth until every turn respects the radius floor, then re-resample.

    The floor is radius >= (width/2) * curve_safety (td5_tg_road.c:638-641);
    below it the row quads fold (td5_trackgen.c:2254-2258). Enforcing it is also
    what licenses the engine's adjacent-skip exemption, which assumes the floor
    holds -- see adjacent_skip() in geo_common.

    Method: Laplacian smoothing applied ONLY at offending vertices (so straight
    and legal stretches keep the real geometry, which is the whole point of the
    feature), re-resampling each pass to keep the spacing uniform. Endpoints are
    pinned: the start must stay tangent to the lead-in, and the finish is where
    the user put it.
    """
    # The limit is LOCAL, not one value for the whole route. Using the widest
    # road's limit everywhere over-smooths the narrow ones: a 2-lane street
    # tolerates 32.3 deg per span while a 4-lane avenue tolerates 16.0, so a
    # global max would round off exactly the tight residential corners that make
    # a real city recognisable. `width` may therefore be a per-node list, sampled
    # by normalised position so it survives the re-resampling below.
    if isinstance(width, (int, float)):
        widths_src = [float(width)]
    else:
        widths_src = [float(x) for x in width] or [float(TD5_TG_LANE_WIDTH * 2)]

    def lim_at(i: int, n: int) -> float:
        t = 0.0 if n <= 1 else i / float(n - 1)
        k = min(int(t * (len(widths_src) - 1) + 0.5), len(widths_src) - 1)
        return max_turn_per_span(widths_src[k], step, curve_safety_x100)

    lim = max_turn_per_span(max(widths_src), step, curve_safety_x100)
    lim_loose = max_turn_per_span(min(widths_src), step, curve_safety_x100)
    cur = list(pts)
    worst_before = 0.0
    it = 0
    for it in range(1, max_iter + 1):
        tn = _turns(cur)
        if not tn:
            break
        n = len(cur)
        worst = max(abs(t) for t in tn)
        if it == 1:
            worst_before = worst
        if all(abs(t) <= lim_at(i + 1, n) + 1e-6 for i, t in enumerate(tn)):
            break
        # tn[i] is the turn at vertex i+1.
        nxt = list(cur)
        for i, t in enumerate(tn):
            if abs(t) <= lim_at(i + 1, n):
                continue
            v = i + 1
            # Pull the offending vertex toward the midpoint of its neighbours.
            # 0.5 converges fast without oscillating; the re-resample below
            # repairs the spacing it disturbs.
            ax, az = cur[v - 1]
            bx, bz = cur[v + 1]
            px, pz = cur[v]
            nxt[v] = (px + 0.5 * ((ax + bx) * 0.5 - px),
                      pz + 0.5 * ((az + bz) * 0.5 - pz))
        nxt[0] = cur[0]
        nxt[-1] = cur[-1]
        cur = _resample_uniform(nxt, step)

    tn = _turns(cur)
    n = len(cur)
    worst_after = max((abs(t) for t in tn), default=0.0)
    over = [i + 1 for i, t in enumerate(tn) if abs(t) > lim_at(i + 1, n) + 1e-6]
    return cur, {
        "limit_deg": math.degrees(lim),            # tightest (widest road)
        "limit_loose_deg": math.degrees(lim_loose),  # loosest (narrowest road)
        "worst_before_deg": math.degrees(worst_before),
        "worst_after_deg": math.degrees(worst_after),
        "iterations": it,
        "converged": not over,
        "over_nodes": len(over),
        "min_radius_units": min_turn_radius(max(widths_src), curve_safety_x100),
    }


# ------------------------------------------------------- crossing detection ---

def find_crossings(pts: list[tuple[float, float]], widths: list[float],
                   lane_width: float = TD5_TG_LANE_WIDTH,
                   skip: int | None = None) -> list[dict]:
    """Every non-adjacent node pair closer than the engine's `need` distance.

    Exactly tg_too_close's test (td5_trackgen.c:1760-1772):
        need = (w_i + w_j) * 0.5 + lane_width * 0.25,  overlap if d^2 < need^2
    with pairs within `skip` along the road exempt, as tg_adjacent_skip
    prescribes. Run offline, where failing costs nothing, instead of in the walk
    where it was only ever a backstop.

    O(n^2) is 4.5M pairs at the 3000-span cap, which is a second or two in
    Python and runs once per route, so a spatial index would be complexity for
    no user-visible gain. If that stops being true, grid-bucket by need_max.
    """
    n = len(pts)
    if skip is None:
        skip = adjacent_skip(lane_width)
    out: list[dict] = []
    for i in range(n):
        xi, zi = pts[i]
        wi = widths[i]
        for j in range(i + skip, n):
            need = too_close_need(wi, widths[j], lane_width)
            dx = pts[j][0] - xi
            dz = pts[j][1] - zi
            d2 = dx * dx + dz * dz
            if d2 < need * need:
                out.append({
                    "node_a": i,
                    "node_b": j,
                    "distance_units": math.sqrt(d2),
                    "need_units": need,
                    "span_gap": j - i,
                })
    return out


def merge_crossings(cr: list[dict], span_tol: int = 8) -> list[dict]:
    """Collapse a crossing region into one reportable site.

    A single road-over-road overlap produces a whole block of offending pairs.
    The selector wants one marker per place to drag, not hundreds.
    """
    if not cr:
        return []
    groups: list[dict] = []
    for c in sorted(cr, key=lambda d: (d["node_a"], d["node_b"])):
        for g in groups:
            if (abs(c["node_a"] - g["a_last"]) <= span_tol
                    and abs(c["node_b"] - g["b_last"]) <= span_tol):
                g["pairs"] += 1
                g["a_last"], g["b_last"] = c["node_a"], c["node_b"]
                g["a_lo"] = min(g["a_lo"], c["node_a"])
                g["a_hi"] = max(g["a_hi"], c["node_a"])
                g["b_lo"] = min(g["b_lo"], c["node_b"])
                g["b_hi"] = max(g["b_hi"], c["node_b"])
                if c["distance_units"] < g["worst_distance_units"]:
                    g["worst_distance_units"] = c["distance_units"]
                    g["worst_pair"] = (c["node_a"], c["node_b"])
                break
        else:
            groups.append({
                "a_first": c["node_a"], "a_last": c["node_a"],
                "b_first": c["node_b"], "b_last": c["node_b"],
                "a_lo": c["node_a"], "a_hi": c["node_a"],
                "b_lo": c["node_b"], "b_hi": c["node_b"],
                "pairs": 1,
                "worst_distance_units": c["distance_units"],
                "need_units": c["need_units"],
                "worst_pair": (c["node_a"], c["node_b"]),
            })
    return groups


# ------------------------------------------------------------- the pipeline ---

def heading_byte_report(pts: list[tuple[float, float]]) -> dict:
    """How faithfully the AI route table can store this route's headings.

    ROUTES.DAT byte 1 is an ABSOLUTE 12-bit heading, not a deflection, and
    tg_emit_routes (td5_trackgen.c:2598-2640) encodes it as

        h12 = round(atan2(tx, tz) * 4096 / 2pi) & 0xFFF
        hb  = round(h12 * 256 / 4140)          # 4140 = 0x102C
        clamp hb to 4..253

    because `byte < 4` is a junction-zone sentinel rather than a heading. The
    engine recovers heading = (byte * 0x102C) >> 8 (td5_ai.c:1280, :305), so the
    clamp costs angular accuracy instead of emitting a sentinel -- which is the
    safe direction, but it is still a loss the AI steers by.

    The generator never meets this: its +-80 deg budget about +X keeps every
    heading far from the dead zone. A real route that wanders through +Z does
    meet it, and then a handful of spans carry a heading up to ~5.7 deg wrong.
    Reported, not rejected: 5.7 deg on a few spans is a quality note, and the
    plan's fidelity-vs-playability decision is exactly this kind of trade.
    """
    lo_deg = 4.0 * 4140.0 / 256.0 * 360.0 / 4096.0      # heading of hb == 4
    worst = 0.0
    n_lo = n_hi = 0
    for h in _headings(pts):
        h12 = int(round(h * 4096.0 / (2.0 * math.pi))) & 0xFFF
        hb = int(round(h12 * 256.0 / 4140.0))
        if hb < 4:
            n_lo += 1
            err = abs(lo_deg - h12 * 360.0 / 4096.0)
            worst = max(worst, err)
        elif hb > 253:
            n_hi += 1
            err = abs(h12 * 360.0 / 4096.0 - 253.0 * 4140.0 / 256.0 * 360.0 / 4096.0)
            worst = max(worst, err)
    return {
        "clamped_low": n_lo,
        "clamped_high": n_hi,
        "clamped_total": n_lo + n_hi,
        "worst_heading_error_deg": worst,
        "dead_zone_note": "headings within ~%.1f deg of +Z cannot be represented"
                          % lo_deg,
    }


def _score(pts: list[tuple[float, float]]) -> dict:
    """How close this orientation comes to inheriting the engine's proof."""
    h = _headings(pts)
    if not h:
        return {"max_dev_deg": 0.0, "over_budget": 0, "monotone_frac": 1.0,
                "inherits_proof": True}
    devs = [abs(_wrap(a - math.pi / 2.0)) for a in h]
    over = sum(1 for d in devs if d > HEADING_LIMIT)
    fwd = sum(1 for i in range(len(pts) - 1) if pts[i + 1][0] > pts[i][0])
    return {
        "max_dev_deg": math.degrees(max(devs)),
        "mean_dev_deg": math.degrees(sum(devs) / len(devs)),
        "over_budget": over,
        "monotone_frac": fwd / float(len(pts) - 1),
        "inherits_proof": over == 0,
    }


def condition_route(latlon: list[tuple[float, float]],
                    lanes: list[int] | int = 2,
                    units_per_metre: float = UNITS_PER_METRE_DERIVED,
                    curve_safety_x100: int = TD5_TG_CURVE_SAFETY_X100_DEFAULT,
                    span_length: float = TD5_TG_SPAN_LENGTH,
                    lane_width: float = TD5_TG_LANE_WIDTH,
                    allow_reverse: bool = True) -> dict:
    """Raw (lat, lon) route -> a TD5-legal centerline plus a verdict.

    Returns a dict that is both the ROUTE.JSON payload and the report the
    selector renders. `ok` is False when the route cannot be used as drawn; the
    reasons are enumerated so the UI can point at each one.
    """
    if len(latlon) < 2:
        return {"ok": False, "reasons": ["route has fewer than 2 points"]}

    lat0 = sum(p[0] for p in latlon) / len(latlon)
    lon0 = sum(p[1] for p in latlon) / len(latlon)
    proj = LocalProjection(lat0, lon0, units_per_metre)
    metric = [proj.to_world(la, lo) for la, lo in latlon]   # rotation identity

    if isinstance(lanes, int):
        lane_list_src = [lanes] * len(latlon)
    else:
        lane_list_src = list(lanes)
        if len(lane_list_src) != len(latlon):
            return {"ok": False, "reasons": ["lanes length != route length"]}
    width_max = max(lane_list_src) * lane_width

    # -- 1. orientation ---------------------------------------------------
    # The lead-in is a fixed straight along +X (td5_tg_road.c:733-741), so the
    # route's START TANGENT must be +X. That uses up the rotational freedom, and
    # the only remaining choice is which end is the start -- free for a
    # point-to-point race, and it can decide whether the route self-crosses.
    candidates = [("forward", metric)]
    if allow_reverse:
        candidates.append(("reversed", list(reversed(metric))))

    scored = []
    for name, seq in candidates:
        h0 = _headings(seq)[0]
        # SIGN: _rotate(pts, theta) maps a heading phi to phi - theta. With
        # x = r sin(phi), z = r cos(phi), the transform (x*c - z*s, x*s + z*c)
        # gives (r sin(phi-theta), r cos(phi-theta)). So carrying h0 onto the
        # +X axis (pi/2) needs theta = h0 - pi/2, NOT pi/2 - h0.
        #
        # This was pi/2 - h0 and silently wrong: R3 only checks the SYNTHETIC
        # lead-in, which is +X by construction, so nothing tested the body's
        # first heading. The whole orientation score was measured in a frame
        # rotated by twice the start deviation.
        theta = h0 - math.pi / 2.0
        rot = _rotate(seq, theta)
        sc = _score(rot)
        scored.append((name, theta, rot, sc))
    # Prefer an orientation that inherits the proof outright; then fewest spans
    # over budget; then the smallest worst-case deviation.
    scored.sort(key=lambda t: (not t[3]["inherits_proof"],
                               t[3]["over_budget"], t[3]["max_dev_deg"]))
    direction, theta, pts, orient = scored[0]
    proj.set_rotation(theta)
    if direction == "reversed":
        lane_list_src = list(reversed(lane_list_src))

    pca_deg = math.degrees(_wrap(_principal_axis(pts) - math.pi / 2.0))

    # -- 2. uniform spacing ----------------------------------------------
    pts = _resample_uniform(pts, span_length)

    # -- 3. curvature floor ----------------------------------------------
    pts, curv = enforce_curvature(pts, [n * lane_width for n in lane_list_src],
                                  curve_safety_x100, span_length)

    # -- 3b. RE-ALIGN the start tangent ----------------------------------
    # Step 1 put the start tangent on +X, but smoothing moves interior points and
    # the re-resample shifts every sample, so the first segment no longer points
    # exactly along +X. Left uncorrected that puts a kink at the lead-in join --
    # observed as a 97.2 degree turn at node 40 on the La Plata route, caught by
    # geo_audit's R5 and invisible in the conditioner's own convergence check.
    h0 = _headings(pts)[0]
    fix = h0 - math.pi / 2.0
    if abs(_wrap(fix)) > 1e-9:
        pts = _rotate(pts, fix)
        theta += fix
        proj.set_rotation(theta)

    # -- 4. lead-in + origin ---------------------------------------------
    # Node 0 at (0,0) then TG_LEAD_IN_NODES straight along +X, and the real route
    # continues from there. Translating the route so its first point lands at the
    # lead-in's end keeps the join tangential, because step 1 already put the
    # start tangent on +X.
    lead = [(i * span_length, 0.0) for i in range(TG_LEAD_IN_NODES + 1)]
    ox, oz = pts[0]
    join_x, join_z = lead[-1]
    off_x, off_z = join_x - ox, join_z - oz
    body = [(x + off_x, z + off_z) for x, z in pts[1:]]
    nodes = lead + body
    # The cache MUST be built with this translation as well as the rotation.
    # geo_audit R8 caught the omission: with only the rotation shared, 896 of
    # 1451 route nodes fell outside their own terrain.
    proj.set_offset(off_x, off_z)

    # ONE lanes array from here on. The first version derived the curvature
    # limit from one index mapping and stored the lanes with another, so the
    # conditioner reported "converged" while geo_audit found a node over the
    # limit -- they were checking different widths for the same node.
    def _lane_at(i: int, n: int) -> int:
        t = 0.0 if n <= 1 else i / float(n - 1)
        k = min(int(t * (len(lane_list_src) - 1) + 0.5), len(lane_list_src) - 1)
        return int(lane_list_src[k])

    lanes_out = ([lane_list_src[0]] * len(lead)
                 + [_lane_at(i, len(body)) for i in range(len(body))])
    widths = [n * lane_width for n in lanes_out]

    # Final verification against the widths actually stored, which is the same
    # test geo_audit R5 runs. Reported rather than silently re-smoothed: a turn
    # that survives here is a fact about the route, not a solver failure.
    curv["over_nodes_final"] = 0
    curv["worst_final_deg"] = 0.0
    for i in range(1, len(nodes) - 1):
        h_a = math.atan2(nodes[i][0] - nodes[i - 1][0],
                         nodes[i][1] - nodes[i - 1][1])
        h_b = math.atan2(nodes[i + 1][0] - nodes[i][0],
                         nodes[i + 1][1] - nodes[i][1])
        dd = abs(_wrap(h_b - h_a))
        curv["worst_final_deg"] = max(curv["worst_final_deg"], math.degrees(dd))
        if dd > max_turn_per_span(widths[i], span_length, curve_safety_x100) + 1e-6:
            curv["over_nodes_final"] += 1
    curv["converged"] = curv["over_nodes_final"] == 0

    # -- 5. crossings + cap ----------------------------------------------
    skip = adjacent_skip(lane_width, span_length, curve_safety_x100)
    crossings = merge_crossings(find_crossings(nodes, widths, lane_width, skip))

    spans = len(nodes) - 1
    reasons: list[str] = []
    if spans > TD5_TG_MAX_SPANS:
        reasons.append(
            "route is %d spans, over the %d cap (s_struct[]/s_rn[] are indexed "
            "by node with no bounds check, so this would corrupt memory rather "
            "than fail)" % (spans, TD5_TG_MAX_SPANS))
    # A track must hold a grid, a race and a run-off. RUN-OFF defaults to 100
    # spans (TD5RE_AUTOTRACK_RUNOFF, td5_fe_race.c:7286) and the grid is at
    # TD5_TG_GRID_SPAN, so anything near the lead-in length is not a track.
    if spans < TG_LEAD_IN_NODES + 100 + 50:
        reasons.append("route is %d spans, too short to hold a grid, a race and "
                       "a run-off" % spans)
    if crossings:
        reasons.append("%d self-crossing site(s): the span walker would snap to "
                       "the wrong span" % len(crossings))
    if not curv["converged"]:
        reasons.append("curvature smoothing did not converge (worst turn %.1f "
                       "deg vs limit %.1f)"
                       % (curv["worst_after_deg"], curv["limit_deg"]))

    final = _score(nodes)
    heading = heading_byte_report(nodes)
    if heading["clamped_total"]:
        reasons_note = ("%d span(s) point within the route-byte dead zone near "
                        "+Z; their stored heading is up to %.1f deg wrong for the "
                        "AI (geometry is unaffected)"
                        % (heading["clamped_total"],
                           heading["worst_heading_error_deg"]))
    else:
        reasons_note = None
    return {
        "ok": not reasons,
        "reasons": reasons,
        "direction": direction,
        "spans": spans,
        "nodes": len(nodes),
        "length_km": spans * span_length / units_per_metre / 1000.0,
        "span_length": span_length,
        "lane_width": lane_width,
        "curve_safety_x100": curve_safety_x100,
        "units_per_metre": units_per_metre,
        "elevation_exaggeration": ELEVATION_EXAGGERATION,
        "adjacent_skip": skip,
        "lead_in_nodes": TG_LEAD_IN_NODES,
        "projection": proj.describe(),
        # The frame the points are expressed in. geo_fetch MUST build HEIGHT/
        # COVER/WATER with this rotation or the terrain will not correspond to
        # the road -- see the rotation field in geo_raster.
        "rotation_rad": proj.describe()["rotation_rad"],
        "offset_x": off_x,
        "offset_z": off_z,
        "orientation": orient,
        "final": final,
        "principal_axis_dev_deg": pca_deg,
        "curvature": curv,
        "heading_bytes": heading,
        "warnings": [w for w in (reasons_note,) if w],
        "crossings": crossings,
        "points": [{"x": round(x, 3), "z": round(z, 3), "lanes": l}
                   for (x, z), l in zip(nodes, lanes_out)],
    }


# ------------------------------------------------------------------- report ---

def print_report(r: dict) -> None:
    if not r.get("spans"):
        print("FAILED: %s" % "; ".join(r.get("reasons", ["unknown"])))
        return
    o, f, c = r["orientation"], r["final"], r["curvature"]
    print("route      : %d spans (%d nodes), %.2f km, %s"
          % (r["spans"], r["nodes"], r["length_km"], r["direction"]))
    print("orientation: max dev %.1f deg, %d span(s) over the %.0f deg budget, "
          "%.0f%% axis-monotone"
          % (o["max_dev_deg"], o["over_budget"], math.degrees(HEADING_LIMIT),
             100.0 * o["monotone_frac"]))
    if o["inherits_proof"]:
        print("             -> inside the heading budget, so it INHERITS the "
              "engine's no-crossing proof outright")
    print("principal  : %+.1f deg off the start tangent (large = the route "
          "curls away from where it starts)" % r["principal_axis_dev_deg"])
    print("curvature  : worst turn %.1f -> %.1f deg (limit %.1f tight .. %.1f "
          "loose), %d iter, %s"
          % (c["worst_before_deg"], c["worst_after_deg"], c["limit_deg"],
             c.get("limit_loose_deg", c["limit_deg"]), c["iterations"],
             "converged" if c["converged"] else
             "NOT converged (%d node(s) over)" % c.get("over_nodes_final",
                                                       c.get("over_nodes", 0))))
    if c.get("worst_final_deg"):
        print("             final worst turn %.1f deg against the STORED lane "
              "widths" % c["worst_final_deg"])
    hb = r.get("heading_bytes") or {}
    if hb.get("clamped_total"):
        print("route byte : %d span(s) in the +Z dead zone, worst heading error "
              "%.1f deg (AI routing only)"
              % (hb["clamped_total"], hb["worst_heading_error_deg"]))
    for w in r.get("warnings", []):
        print("warning    : %s" % w)
    print("final      : max dev %.1f deg, %.0f%% monotone"
          % (f["max_dev_deg"], 100.0 * f["monotone_frac"]))
    if r["crossings"]:
        print("crossings  : %d site(s)" % len(r["crossings"]))
        for g in r["crossings"][:10]:
            print("   nodes %d..%d vs %d..%d : %d pairs, closest %.0f units "
                  "(needs %.0f)"
                  % (g["a_lo"], g["a_hi"], g["b_lo"], g["b_hi"],
                     g["pairs"], g["worst_distance_units"], g["need_units"]))
    else:
        print("crossings  : none")
    print("VERDICT    : %s" % ("OK" if r["ok"] else "NOT USABLE"))
    for why in r["reasons"]:
        print("   - %s" % why)


# --------------------------------------------------------------- self-test ---
# Runs with no network and no game, so the crux is verifiable from the first
# commit. Each case targets one invariant from the plan's section 5.

def _synth_latlon(kind: str, n: int = 900) -> list[tuple[float, float]]:
    """Synthetic routes around La Plata, in degrees, for the self-test."""
    lat0, lon0 = -34.9215, -57.9545
    mlat, mlon = 1.0 / 110_574.0, 1.0 / (111_320.0 * math.cos(math.radians(lat0)))
    out = []
    for i in range(n):
        t = i / float(n - 1)
        if kind == "straight":                  # 6 km due east
            e, nn = t * 6000.0, 0.0
        elif kind == "gentle":                  # 6 km with a wide sweep
            e, nn = t * 6000.0, 400.0 * math.sin(t * math.pi)
        elif kind == "grid":                    # staircase, like a city route
            e, nn = t * 5000.0, 250.0 * math.floor(t * 8) / 8.0 * 8
        elif kind == "hairpin_wide":
            # Doubles back 180 deg, but the return leg is 700 m away. At 430
            # units/m that is 301 000 units, against a `need` of 3375 for a
            # 2-lane road -- 100 road-widths apart, so there is no overlap and
            # this route is LEGAL despite being nowhere near axis-monotone.
            if t < 0.5:
                e, nn = t * 2.0 * 3000.0, 0.0
            else:
                e, nn = (1.0 - t) * 2.0 * 3000.0, 700.0
        elif kind == "retrace":
            # Out and back along the SAME street, 6 m apart -- 2580 units, under
            # the 3375 need. This is the failure that actually happens.
            if t < 0.5:
                e, nn = t * 2.0 * 3000.0, 0.0
            else:
                e, nn = (1.0 - t) * 2.0 * 3000.0, 6.0
        elif kind == "loop":                    # closed loop: must be caught
            a = t * 2.0 * math.pi
            e, nn = 1400.0 * math.sin(a), 1400.0 * (1.0 - math.cos(a))
        else:
            raise SystemExit("unknown synthetic route %r" % kind)
        out.append((lat0 + nn * mlat, lon0 + e * mlon))
    return out


def _self_test() -> int:
    cases = (
        ("straight", True,  "a straight drive must pass"),
        ("gentle",   True,  "a wide sweep must pass"),
        ("grid",     True,  "a city staircase must pass"),
        ("hairpin_wide", True,  "doubling back 700 m away is LEGAL: no overlap "
                                "at TD5 scale"),
        ("retrace",  False, "retracing the SAME street must be REJECTED"),
        ("loop",     False, "a closed loop must be REJECTED"),
    )
    bad = 0
    for kind, want_ok, why in cases:
        print("\n=== %s -- %s" % (kind, why))
        r = condition_route(_synth_latlon(kind), lanes=2)
        print_report(r)
        if bool(r["ok"]) != want_ok:
            print("*** SELF-TEST FAILURE: expected ok=%s, got ok=%s"
                  % (want_ok, r["ok"]))
            bad += 1
    print("\n%s" % ("all %d cases behaved as specified" % len(cases) if not bad
                    else "%d of %d cases WRONG" % (bad, len(cases))))
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--in", dest="inp",
                    help="raw route JSON: [[lat,lon],...] or {points:[{lat,lon,lanes}]}")
    ap.add_argument("--out", help="write ROUTE.JSON here")
    ap.add_argument("--raw-out", help="write ROUTE_RAW.JSON here")
    ap.add_argument("--lanes", type=int, default=2)
    ap.add_argument("--units-per-metre", type=float,
                    default=UNITS_PER_METRE_DERIVED)
    ap.add_argument("--curve-safety", type=int,
                    default=TD5_TG_CURVE_SAFETY_X100_DEFAULT)
    ap.add_argument("--no-reverse", action="store_true",
                    help="do not consider driving the route the other way")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)

    if a.self_test or not a.inp:
        return _self_test()

    raw = json.load(open(a.inp, encoding="utf-8"))
    if isinstance(raw, dict):
        pts = [(p["lat"], p["lon"]) for p in raw["points"]]
        lanes = [int(p.get("lanes", a.lanes)) for p in raw["points"]]
    else:
        pts = [(float(p[0]), float(p[1])) for p in raw]
        lanes = a.lanes

    r = condition_route(pts, lanes=lanes,
                        units_per_metre=a.units_per_metre,
                        curve_safety_x100=a.curve_safety,
                        allow_reverse=not a.no_reverse)
    print_report(r)
    if a.raw_out:
        write_json(a.raw_out, {"points": [{"lat": la, "lon": lo}
                                          for la, lo in pts]})
    if a.out:
        write_json(a.out, r)
        print("\nwrote %s" % a.out)
    return 0 if r["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
