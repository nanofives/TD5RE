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

CROSSINGS ARE NO LONGER UNCONDITIONALLY FATAL (2026-09-30, plan section 5
Option B). The engine's span walker now has a crossing-safe localiser -- it
prefers candidates near the span the car was already on, and uses the deck
height to tell a flyover from the road beneath it (td5_track.c, the "crossing-
safe span localisation" section, auto-armed for geo-route tracks). Three
outcomes now, and the tool still REPORTS every crossing in all three:

  * GRADE-SEPARATED -- accepted on its own merits, no flag needed. Either the
    two legs carry different OSM `layer` values (how OSM actually encodes a
    flyover) or the DEM puts them GRADE_SEPARATION_M apart vertically. A car
    cannot be on both, so nothing is ambiguous to begin with.
  * LEVEL, with --allow-crossings -- accepted because the caller is asserting
    the crossing-safe localiser is on. This is the loops and figure-eights
    case, and it is the one that needs the engine change to be real.
  * LEVEL, without the flag -- still NOT USABLE, unchanged.

WHAT THE DEM CAN AND CANNOT SEE. HEIGHT.R16 is TERRAIN, not road deck. A real
bridge sits above the terrain, and the DEM under both legs is then the same, so
the DEM detects only the case where the ground itself separates the two legs (a
cutting under a ridge). The authoritative signal for a built flyover is the OSM
`layer` tag; geo_route.py does not carry it yet, so `layers` is plumbed here
ready for it and is None in practice today. Read a "not grade separated"
verdict as "could not prove separation", never as "proven level".
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
    TD5_TG_GRID_SPAN,
    TD5_TG_LANE_WIDTH,
    TD5_TG_MAX_GRADE_X1000_DEFAULT,
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

# Vertical clearance, in metres, at which two legs of a crossing stop being
# ambiguous. Set from what the ENGINE needs, not from a highway standard: the
# crossing-safe localiser compares |deck Y - probe Y| between the two candidate
# quads, so the separation only has to exceed the spread a single car's probes
# see on one deck (ride height plus body roll plus one span of grade, well
# under a metre). 4 m is the usual real road-over-road clearance and leaves an
# order of magnitude of margin, so a crossing that clears it is never a close
# call. Lower it only with a measurement in hand.
GRADE_SEPARATION_M = 4.0

# ------------------------------------------------- built grade separation ---
# [OPTION B 2026-09-30] The numbers the GENERATOR builds a grade separation
# with. Mirrored from the C, same contract as geo_common's MIRRORING RULE: if
# the C moves, this tool plans a clearance the engine does not build.
#
# td5_trackgen_internal.h:3510 -- clear height under an underpass soffit. This
# is the clearance the NETWORK layer's underpass crossings already use, and the
# plan's Option B says to reuse it rather than invent a second number.
TD5_TG_UP_CLEAR = 2600.0
# td5_trackgen_internal.h:3701 -- the bridge deck's girder depth under the
# carriageway. The lower road has to clear the SOFFIT, not the deck surface.
TD5_TG_BRIDGE_UNDER = 480.0
# So the deck's carriageway sits this far above the lower carriageway.
XSEP_LIFT_UNITS = TD5_TG_UP_CLEAR + TD5_TG_BRIDGE_UNDER      # 3080
# td5_tg_road.c:236 -- no structure at or below this span, so a deck may not
# land there (the grid straight, the start line and the y[0] spawn anchor all
# live in that window).
XSEP_FIRST_SPAN = TD5_TG_GRID_SPAN + 25
# td5_tg_road.c:56 -- TD5RE_TG_BRIDGE_MAX, the deck-run span cap.
XSEP_BRIDGE_MAX_SPANS = 56
# Ramp grade the plan is BUDGETED at, as a fraction of the spec's own cap
# (TD5_TG_MAX_GRADE_X1000_DEFAULT / 1000 = 0.12). Half, because the ramp has to
# share the grade budget with the terrain the ramp is climbing over: the road
# profile's limiter caps the TOTAL grade, so a ramp planned at the full cap is
# clipped wherever the ground itself rises, and the clearance is lost silently.
# The engine re-derives the ramp from the caps it can actually see (it knows the
# per-biome cap and this tool does not), so this is the advisory minimum.
XSEP_RAMP_GRADE_FRACTION = 0.5


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


# --------------------------------------------------- grade separation ---

def height_sampler_from_raster(path: str, rotation_rad: float | None = None,
                               units_per_metre: float = UNITS_PER_METRE_DERIVED):
    """(x, z) -> terrain metres, from a HEIGHT.R16 written by geo_fetch.

    Returns None if the file cannot be used, so the caller degrades to "could
    not prove separation" instead of guessing. numpy is imported here and not
    at module scope: the self-test and the plain conditioning path must keep
    working on a bare Python, and only this optional check needs it.

    The rotation check is not a formality. The raster carries the frame it was
    built in exactly because a silent frame mismatch once put 896 of 1451 route
    nodes outside their own terrain with nothing raising an error (geo_raster's
    own header note). Sampling a DEM from a different frame here would produce
    a confident, wrong grade-separation verdict, so a mismatch refuses.
    """
    try:
        from geo_raster import Raster                      # noqa: PLC0415
    except Exception as e:                                 # numpy missing, etc.
        print("height: cannot load geo_raster (%s)" % e)
        return None
    try:
        r = Raster.read(path)
    except Exception as e:
        print("height: cannot read %s (%s)" % (path, e))
        return None
    if rotation_rad is not None and abs(_wrap(r.rotation_rad - rotation_rad)) > 1e-6:
        print("height: %s is in frame %.6f rad but this route is %.6f rad -- "
              "refusing to sample a DEM from a different frame"
              % (path, r.rotation_rad, rotation_rad))
        return None
    h, w = r.data.shape

    def sample(x: float, z: float):
        ix = int(round((x - r.origin_x) / r.cell))
        iz = int(round((z - r.origin_z) / r.cell))
        if ix < 0 or iz < 0 or ix >= w or iz >= h:
            return None
        if int(r.data[iz, ix]) == r.nodata_raw:
            return None
        return r.value(ix, iz) / units_per_metre       # world units -> metres

    return sample


def classify_crossings(groups: list[dict], nodes: list[tuple[float, float]],
                       layers: list[int] | None = None,
                       height_sampler=None,
                       clearance_m: float = GRADE_SEPARATION_M) -> list[dict]:
    """Mark each crossing site grade-separated or not, in place, and return it.

    Judged at the CLOSEST pair of the site (`worst_pair`), because that is the
    pair the engine's separation test would fail on: if the tightest point of
    the overlap is cleanly separated, every looser point in the same site is
    too.
    """
    for g in groups:
        a, b = g["worst_pair"]
        g["layer_a"] = g["layer_b"] = None
        g["layer_separated"] = False
        g["dem_delta_m"] = None
        g["dem_separated"] = False
        if layers and a < len(layers) and b < len(layers):
            g["layer_a"], g["layer_b"] = int(layers[a]), int(layers[b])
            g["layer_separated"] = g["layer_a"] != g["layer_b"]
        if height_sampler is not None:
            ha = height_sampler(*nodes[a])
            hb = height_sampler(*nodes[b])
            if ha is not None and hb is not None:
                g["dem_delta_m"] = abs(ha - hb)
                g["dem_separated"] = g["dem_delta_m"] >= clearance_m
        g["grade_separated"] = bool(g["layer_separated"] or g["dem_separated"])
    return groups


def plan_grade_separations(groups: list[dict], n_nodes: int,
                           span_length: float = TD5_TG_SPAN_LENGTH,
                           max_grade: float = TD5_TG_MAX_GRADE_X1000_DEFAULT / 1000.0,
                           lift_units: float = XSEP_LIFT_UNITS) -> list[dict]:
    """Decide, for every LEVEL crossing site, which leg the engine raises.

    [OPTION B 2026-09-30] The crossing-safe localiser makes a level crossing
    survivable; it does not make it a road layout. Two carriageways at the same
    height and the same XZ is still one piece of tarmac with two span indices on
    it, and a car that drifts a lane there is on the other leg with nothing
    physical having gone wrong. A real grade separation removes the ambiguity at
    the source -- one leg is several metres above the other -- and it is also
    what the localiser's HEIGHT key was built to read.

    WHY THE DECISION IS MADE HERE AND NOT IN THE ENGINE. The engine walks the
    route one chunk at a time with a 64-span revisable window (td5_tg_road.c,
    TG_ROAD_WINDOW), so at the first leg it cannot yet see the second. Deciding
    offline and shipping the answer in ROUTE.JSON makes the lift a KNOWN target
    before the walk starts, which is what lets the profile ramp into it instead
    of discovering it too late to climb.

    THE RULE, and it draws no random number (the standing byte-identity rule at
    td5_trackgen_internal.h:1290-1296 forbids one, and a layout that moved
    between two builds of the same route would defeat the determinism gate):

      1. A leg that cannot hold a ramp is not eligible. Ramp room is the span
         distance to the first legal structure span (XSEP_FIRST_SPAN) on one
         side and to the last node on the other, and a leg also may not ramp
         into the OTHER leg of its own crossing -- the point is to separate
         them, so lifting the second one too would put both in the air.
      2. Of the eligible legs, the one with MORE ramp room goes over. That is
         the leg whose ramps can be built at the gentlest grade, so it is the
         one most likely to actually reach the clearance.
      3. Ties go to the LATER leg (higher span index). Arbitrary but fixed, and
         it is the leg a driver meets second, so the track reads as "the road
         you drove earlier passes underneath".

    `ramp_spans` is ADVISORY. The engine re-derives it against the per-biome
    grade caps, which this tool cannot see (tg_road_cap_at); what it may not
    re-derive is WHICH leg goes over, because that is the decision that has to
    be stable across builds.
    """
    out: list[dict] = []
    if n_nodes < 2:
        return out
    last = n_nodes - 1
    budget = max_grade * XSEP_RAMP_GRADE_FRACTION * span_length
    ramp_min = int(math.ceil(lift_units / budget)) if budget > 0.0 else 0
    for idx, g in enumerate(groups):
        if g.get("grade_separated"):
            continue
        a_lo, a_hi = int(g["a_lo"]), int(g["a_hi"])
        b_lo, b_hi = int(g["b_lo"]), int(g["b_hi"])
        legs = {"a": (a_lo, a_hi), "b": (b_lo, b_hi)}
        # Ramp room per leg: the shorter of the two clear sides, with the other
        # leg of THIS crossing treated as a boundary (rule 1).
        room = {}
        for key, (lo, hi) in legs.items():
            other_lo, other_hi = legs["b" if key == "a" else "a"]
            back_stop = XSEP_FIRST_SPAN
            fwd_stop = last
            if other_hi < lo:
                back_stop = max(back_stop, other_hi + 1)
            if other_lo > hi:
                fwd_stop = min(fwd_stop, other_lo - 1)
            room[key] = min(lo - back_stop, fwd_stop - hi)
        # Rule 2, then rule 3 (b is always the later leg: find_crossings only
        # emits pairs with node_b > node_a).
        over = "b" if room["b"] >= room["a"] else "a"
        under = "a" if over == "b" else "b"
        o_lo, o_hi = legs[over]
        u_lo, u_hi = legs[under]
        ramp = min(ramp_min, max(0, room[over]))
        deck = o_hi - o_lo + 1
        ok = (room[over] > 0 and ramp > 0
              and deck + 2 * ramp <= XSEP_BRIDGE_MAX_SPANS * 2)
        out.append({
            "site": idx,
            "over_lo": o_lo, "over_hi": o_hi,
            "under_lo": u_lo, "under_hi": u_hi,
            "over_leg": over,
            "ramp_spans": ramp,
            "ramp_spans_wanted": ramp_min,
            "ramp_room_spans": room[over],
            "clearance_units": lift_units,
            "clearance_m": lift_units / UNITS_PER_METRE_DERIVED,
            "buildable": bool(ok),
            "why": ("more ramp room" if room[over] != room[under]
                    else "tie -> later leg"),
        })
        g["grade_separation_planned"] = bool(ok)
        g["grade_separation_over"] = over
    return out


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
                    allow_reverse: bool = True,
                    allow_crossings: bool = False,
                    layers: list[int] | None = None,
                    height_path: str | None = None) -> dict:
    """Raw (lat, lon) route -> a TD5-legal centerline plus a verdict.

    Returns a dict that is both the ROUTE.JSON payload and the report the
    selector renders. `ok` is False when the route cannot be used as drawn; the
    reasons are enumerated so the UI can point at each one.

    `allow_crossings` asserts the engine's crossing-safe localiser is armed, so
    a LEVEL self-crossing stops being fatal. `layers` (OSM `layer` per input
    point) and `height_path` (a HEIGHT.R16 for this place, in THIS route's
    frame) let a crossing prove its own grade separation and be accepted with
    no flag at all. See the module docstring for what each can and cannot see.
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
    if layers is None:
        layer_list_src = None
    else:
        layer_list_src = [int(v) for v in layers]
        if len(layer_list_src) != len(latlon):
            return {"ok": False, "reasons": ["layers length != route length"]}

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
        if layer_list_src is not None:
            layer_list_src = list(reversed(layer_list_src))

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

    # Same index mapping for the OSM layer, so a crossing can be judged at the
    # node indices the crossing report uses. The lead-in inherits the route's
    # first layer: it is synthetic ground-level road, and a crossing can never
    # land on it anyway (it is straight and the body starts at its end).
    if layer_list_src is None:
        layers_out = None
    else:
        def _layer_at(i: int, n: int) -> int:
            t = 0.0 if n <= 1 else i / float(n - 1)
            k = min(int(t * (len(layer_list_src) - 1) + 0.5), len(layer_list_src) - 1)
            return int(layer_list_src[k])
        layers_out = ([layer_list_src[0]] * len(lead)
                      + [_layer_at(i, len(body)) for i in range(len(body))])

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
    sampler = None
    if height_path and crossings:
        # Only opened when there is something to judge, and asserted against
        # THIS route's frame -- see height_sampler_from_raster.
        sampler = height_sampler_from_raster(height_path, theta, units_per_metre)
    classify_crossings(crossings, nodes, layers_out, sampler)
    level_crossings = [g for g in crossings if not g["grade_separated"]]
    # [OPTION B 2026-09-30] Plan the lift for the level sites, unconditionally:
    # the plan is a fact about the route's geometry, and computing it even when
    # the route is going to be REJECTED is what lets the selector say "this
    # crossing can be built as a flyover" instead of only "this crossing is
    # fatal". The engine only reads the list when the route is accepted.
    grade_seps = plan_grade_separations(crossings, len(nodes), span_length)

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
    if level_crossings and not allow_crossings:
        reasons.append("%d LEVEL self-crossing site(s): without the crossing-safe "
                       "localiser the span walker snaps to the wrong span. Re-run "
                       "with --allow-crossings if it is armed (it is, by default, "
                       "for geo-route tracks), or drag a waypoint to resolve them"
                       % len(level_crossings))
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

    # Reported in every outcome, including the accepted ones: an accepted
    # crossing is still a place the track does something unusual, and the
    # selector should mark it rather than quietly drop it.
    cross_notes: list[str] = []
    n_sep = len(crossings) - len(level_crossings)
    if n_sep:
        cross_notes.append("%d self-crossing site(s) are GRADE-SEPARATED and need "
                           "no flag" % n_sep)
    if level_crossings and allow_crossings:
        cross_notes.append("%d LEVEL self-crossing site(s) ACCEPTED on "
                           "--allow-crossings: this route needs the engine's "
                           "crossing-safe localiser (TD5RE_XSPAN) to be armed"
                           % len(level_crossings))
    if grade_seps:
        n_built = sum(1 for p in grade_seps if p["buildable"])
        cross_notes.append("%d of %d level site(s) get a BUILT grade separation "
                           "(%.1f m of clearance, deck on the leg with more ramp "
                           "room)" % (n_built, len(grade_seps),
                                      XSEP_LIFT_UNITS / units_per_metre))
        if n_built < len(grade_seps):
            cross_notes.append("%d level site(s) have no room for a ramp and stay "
                               "LEVEL: only the localiser separates them there"
                               % (len(grade_seps) - n_built))
    if crossings and height_path is None and layers_out is None:
        cross_notes.append("no DEM and no layer tags were supplied, so no crossing "
                           "could be PROVEN grade-separated -- pass --height to "
                           "check against the terrain")

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
        "warnings": [w for w in (reasons_note,) if w] + cross_notes,
        "crossings": crossings,
        "level_crossings": len(level_crossings),
        "allow_crossings": bool(allow_crossings),
        "grade_separation_m": GRADE_SEPARATION_M,
        # [OPTION B 2026-09-30] ADDITIVE, and it has to stay that way: td5_geo.c
        # validates ROUTE.JSON against a contract (node 0 at the origin, chord
        # spacing, node cap) and an older file simply has no key here, which the
        # loader reads as "no grade separation" -- the pre-Option-B behaviour.
        # Never make this key required.
        "grade_separations": grade_seps,
        "grade_separation_lift_units": XSEP_LIFT_UNITS,
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
        n_lvl = r.get("level_crossings", len(r["crossings"]))
        print("crossings  : %d site(s), %d grade-separated, %d level"
              % (len(r["crossings"]), len(r["crossings"]) - n_lvl, n_lvl))
        for g in r["crossings"][:10]:
            if g.get("grade_separated"):
                if g.get("layer_separated"):
                    how = "GRADE-SEPARATED (osm layer %s vs %s)" % (g["layer_a"],
                                                                    g["layer_b"])
                else:
                    how = "GRADE-SEPARATED (%.1f m of terrain)" % g["dem_delta_m"]
            elif g.get("dem_delta_m") is not None:
                how = "level (only %.1f m apart, needs %.1f)" % (
                    g["dem_delta_m"], r.get("grade_separation_m", GRADE_SEPARATION_M))
            else:
                how = "level (separation not checked)"
            print("   nodes %d..%d vs %d..%d : %d pairs, closest %.0f units "
                  "(needs %.0f) -- %s"
                  % (g["a_lo"], g["a_hi"], g["b_lo"], g["b_hi"],
                     g["pairs"], g["worst_distance_units"], g["need_units"], how))
    else:
        print("crossings  : none")
    for p in r.get("grade_separations", []):
        print("grade sep  : spans %d..%d go OVER spans %d..%d (%s), ramp %d span(s) "
              "each side of %d room, %.0f units = %.1f m of clearance -- %s"
              % (p["over_lo"], p["over_hi"], p["under_lo"], p["under_hi"],
                 p["why"], p["ramp_spans"], p["ramp_room_spans"],
                 p["clearance_units"], p["clearance_m"],
                 "BUILDABLE" if p["buildable"] else "NO ROOM, stays level"))
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
        elif kind == "figure8":
            # Gerono lemniscate e = A sin(u), n = B sin(u) cos(u), traversed
            # u in [-0.5, pi+0.5] so it passes through the origin at BOTH u=0
            # and u=pi, with tangents (A,B) and (-A,B) -- one genuine
            # transversal self-crossing at a 62 degree angle -- and does NOT
            # close, so the `loop` case's end-meets-start overlap is not what
            # is being tested here. This is the Option B fixture: the route a
            # city loop or a flyover produces, legal only with the crossing-
            # safe localiser.
            u = -0.5 + t * (math.pi + 1.0)
            e = 600.0 * math.sin(u)
            nn = 360.0 * math.sin(u) * math.cos(u)
        else:
            raise SystemExit("unknown synthetic route %r" % kind)
        out.append((lat0 + nn * mlat, lon0 + e * mlon))
    return out


def _self_test() -> int:
    cases = (
        ("straight", True,  {}, "a straight drive must pass"),
        ("gentle",   True,  {}, "a wide sweep must pass"),
        ("grid",     True,  {}, "a city staircase must pass"),
        ("hairpin_wide", True, {}, "doubling back 700 m away is LEGAL: no overlap "
                                   "at TD5 scale"),
        ("retrace",  False, {}, "retracing the SAME street must be REJECTED"),
        ("loop",     False, {}, "a closed loop must be REJECTED"),
        # --- Option B: the crossing-safe localiser changes the verdict, and
        # ONLY when it is asserted. Both directions are pinned so a future
        # change cannot quietly make crossings always-legal or always-fatal.
        ("figure8",  False, {}, "a self-crossing figure-eight is REJECTED by "
                                "default"),
        ("figure8",  True,  {"allow_crossings": True, "_gradesep_built": 1},
                            "...and ACCEPTED with --allow-crossings, with ONE "
                            "buildable grade separation planned for it"),
        # The crossing reason must DISAPPEAR here while the verdict stays
        # NOT USABLE: a retrace doubles back through 180 degrees at 6 m, which
        # fails the curvature floor no matter what the localiser can do. Pinned
        # this way round so --allow-crossings can never be mistaken for a
        # blanket override of the other gates.
        ("retrace",  False, {"allow_crossings": True, "_no_reason": "self-crossing"},
                            "--allow-crossings clears the CROSSING reason for a "
                            "retrace, but its 180 deg hairpin still fails the "
                            "curvature floor, which is a separate limit"),
        ("figure8",  True,  {"layers": "alternating", "_gradesep_built": 0},
                            "a figure-eight whose legs carry different OSM "
                            "layers is grade-separated, so no flag is needed -- "
                            "and nothing has to be BUILT either"),
    )
    bad = 0
    for kind, want_ok, kw, why in cases:
        print("\n=== %s -- %s" % (kind, why))
        pts = _synth_latlon(kind)
        kw = dict(kw)
        no_reason = kw.pop("_no_reason", None)
        want_gs = kw.pop("_gradesep_built", None)
        if kw.get("layers") == "alternating":
            # Layer 1 over the first half, layer 0 over the second: the two
            # legs of the lemniscate meet with different layers, which is how
            # OSM records a flyover.
            kw["layers"] = [1 if i < len(pts) // 2 else 0 for i in range(len(pts))]
        r = condition_route(pts, lanes=2, **kw)
        print_report(r)
        if bool(r["ok"]) != want_ok:
            print("*** SELF-TEST FAILURE: expected ok=%s, got ok=%s"
                  % (want_ok, r["ok"]))
            bad += 1
        elif no_reason and any(no_reason in why for why in r.get("reasons", [])):
            print("*** SELF-TEST FAILURE: no reason should mention %r, got %s"
                  % (no_reason, r["reasons"]))
            bad += 1
        elif want_gs is not None:
            # Pinned in BOTH directions, like the crossing verdict above: a
            # future change must not be able to quietly plan a flyover where
            # the legs are already separated, nor drop the plan where the
            # engine is the only thing that can separate them.
            got = sum(1 for p in r.get("grade_separations", []) if p["buildable"])
            if got != want_gs:
                print("*** SELF-TEST FAILURE: expected %d buildable grade "
                      "separation(s), got %d" % (want_gs, got))
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
    ap.add_argument("--allow-crossings", action="store_true",
                    help="accept LEVEL self-crossings (loops, figure-eights). "
                         "They are still reported. Only sound when the engine's "
                         "crossing-safe localiser is armed, which it is by "
                         "default for geo-route tracks (TD5RE_XSPAN)")
    ap.add_argument("--height",
                    help="HEIGHT.R16 for this place, to prove a crossing is "
                         "grade-separated by the terrain (needs numpy)")
    ap.add_argument("--synth", help="condition a built-in synthetic route "
                                    "instead of --in (e.g. figure8, loop, grid)")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)

    if a.self_test or (not a.inp and not a.synth):
        return _self_test()

    layers = None
    if a.synth:
        pts = _synth_latlon(a.synth)
        lanes = a.lanes
    else:
        raw = json.load(open(a.inp, encoding="utf-8"))
        if isinstance(raw, dict):
            pts = [(p["lat"], p["lon"]) for p in raw["points"]]
            lanes = [int(p.get("lanes", a.lanes)) for p in raw["points"]]
            if any("layer" in p for p in raw["points"]):
                layers = [int(p.get("layer", 0)) for p in raw["points"]]
        else:
            pts = [(float(p[0]), float(p[1])) for p in raw]
            lanes = a.lanes

    r = condition_route(pts, lanes=lanes,
                        units_per_metre=a.units_per_metre,
                        curve_safety_x100=a.curve_safety,
                        allow_reverse=not a.no_reverse,
                        allow_crossings=a.allow_crossings,
                        layers=layers,
                        height_path=a.height)
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
