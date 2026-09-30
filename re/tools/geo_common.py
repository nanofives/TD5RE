"""geo_common.py -- shared constants, projection and cache contract for GEO TRACK.

GEO TRACK builds an auto-track from a real place on Earth. See
docs/plans/GEO_TRACK_OSM_PLAN.md. This module is the single place that knows

  * the generator's own constants, MIRRORED from the C headers (section below),
  * how lat/lon becomes world units,
  * where a place's cache lives and what files it holds.

MIRRORING RULE. Every constant below carries the C file:line it was read from.
If the C changes, this file is wrong and the conditioner will certify routes the
engine then breaks on -- which is the one failure mode this whole tool exists to
prevent. `python geo_common.py --check` re-reads the headers and diffs them
against these values; run it whenever td5_trackgen*.h or td5_tg_road.c move.
"""
from __future__ import annotations

import json
import math
import os
import re
import sys

# --------------------------------------------------------------- generator ---
# Mirrored from the C. Do not "tidy" these -- they are a contract, not taste.

# td5_trackgen.h:27 -- one lane's width in world units.
TD5_TG_LANE_WIDTH = 1500
# td5_trackgen.h:29 -- down-track distance between consecutive spans.
TD5_TG_SPAN_LENGTH = 1500
# td5_tg_world.h:62 -- one world cell is exactly one span.
TG_WORLD_CELL = 1500.0
# td5_trackgen_internal.h:629 -- hard span ceiling. s_struct[] (td5_tg_road.c:63)
# and s_rn[] (:108) are indexed by NODE with no bounds check in
# tg_road_node_fill (:123-132), so exceeding this corrupts memory rather than
# failing cleanly. The conditioner enforces it; the engine does not.
TD5_TG_MAX_SPANS = 3000
# td5_trackgen_internal.h:85 -- grid (start-line) span.
TD5_TG_GRID_SPAN = 24
# td5_tg_road.c:733-741 -- the fixed straight lead-in is GRID_SPAN + 16 nodes
# along TD5_TG_AXIS_HEADING, with node 0 pinned at (0, 0).
TG_LEAD_IN_NODES = TD5_TG_GRID_SPAN + 16
# td5_trackgen_internal.h:1383 -- the global axis the walk wanders about. +X,
# deliberately, because route byte[1] encodes an absolute 12-bit heading.
# td5_tg_road.c:663-664 integrates as x += sin(h)*len, z += cos(h)*len, so
# heading == PI/2 advances +X with Z held constant.
TD5_TG_AXIS_HEADING = math.pi / 2.0
# td5_trackgen_internal.h:1352 -- DUAL_LANE caps lanes at 12, which bounds the
# worst-case width used by the adjacent-skip derivation.
TD5_TG_MAX_LANES = 12
# td5_trackgen_internal.h:108 -- the walker's cross-span lane-index shift.
TD5_TG_HEIGHT_NIBBLE = 8
# Spec defaults, td5_tg_pages.c:3470 and the AT_Row table td5_fe_race.c:7286.
TD5_TG_CURVE_SAFETY_X100_DEFAULT = 180   # min turn radius / half-width, x100
TD5_TG_MAX_GRADE_X1000_DEFAULT = 120     # steepest |dY/d(arc)|, x1000
# td5_tg_road.c:46 -- absolute grade ceiling for any biome.
TG_ROAD_GRADE_ABSMAX = 0.20

# Derived from the four independent constants in the plan's section 2. This is
# the ONE number Phase 0 must confirm by measurement rather than derivation;
# every distance in the pipeline scales off it.
UNITS_PER_METRE_DERIVED = 430.0

# Elevation exaggeration, per the plan's section 3: scale the real gradient,
# THEN let the existing cap clip it.
ELEVATION_EXAGGERATION = 1.5


def too_close_need(width_a: float, width_b: float,
                   lane_width: float = TD5_TG_LANE_WIDTH) -> float:
    """The exact separation the engine demands between two centerlines.

    Mirror of tg_too_close, td5_trackgen.c:1760-1772:
        need = (w_i + w) * 0.5 + lane_width * 0.25
    and overlap is dist^2 < need^2.
    """
    return (width_a + width_b) * 0.5 + lane_width * 0.25


def adjacent_skip(lane_width: float = TD5_TG_LANE_WIDTH,
                  span_length: float = TD5_TG_SPAN_LENGTH,
                  curve_safety_x100: int = TD5_TG_CURVE_SAFETY_X100_DEFAULT) -> int:
    """Nodes this close along the road are EXEMPT from the overlap test.

    Mirror of tg_adjacent_skip, td5_trackgen.c:1819-1863. The window is NOT
    derived from the heading budget -- that was the 2026-08-27 root cause, which
    produced 351 and exempted precisely the band where real overlaps live. It
    comes from the CURVATURE SAFETY floor: the worst case for two nodes n spans
    apart is a single arc at exactly the minimum radius, where their separation
    is the chord c(n) = 2r*sin(n*span_len/(2r)). So n is the smallest value with
    c(n) >= need_max:

        n >= (2r / span_len) * asin(need_max / 2r),   2r = curve_safety * w_max

    plus 2 spans of margin, because width RAMPS across a dual-lane taper so the
    closed form is a bound rather than an identity.

    WHY THE CONDITIONER CARES. This exemption is only SOUND if the curvature
    floor actually holds. The engine may assume that; a real OSM polyline may
    not. So geo_condition enforces the floor first and only then trusts this
    window -- exactly the engine's own reasoning, with its premise checked.
    """
    w_max = TD5_TG_MAX_LANES * lane_width
    need_max = w_max + lane_width * 0.25
    safety = curve_safety_x100 / 100.0
    two_r = safety * w_max
    step = span_length if span_length > 1.0 else 1.0
    if two_r > need_max:
        skip = math.ceil((two_r / step) * math.asin(need_max / two_r)) + 2
    else:
        skip = math.ceil(need_max / step) + 2
    return max(1, int(skip))


def min_turn_radius(width: float,
                    curve_safety_x100: int = TD5_TG_CURVE_SAFETY_X100_DEFAULT) -> float:
    """radius >= (width/2) * curve_safety -- td5_tg_road.c:638-641.

    Below this the row quads fold (td5_trackgen.c:2254-2258).
    """
    return (width * 0.5) * (curve_safety_x100 / 100.0)


def max_turn_per_span(width: float, span_length: float = TD5_TG_SPAN_LENGTH,
                      curve_safety_x100: int = TD5_TG_CURVE_SAFETY_X100_DEFAULT) -> float:
    """Largest legal heading change per span, radians.

    Three points at spacing L on an arc of radius r turn by theta with
    r = L / (2 sin(theta/2)), so theta_max = 2 asin(L / 2 r_min). At the default
    safety and 2 lanes this is ~0.563 rad (32.2 deg), so a 90-degree city corner
    needs at least 3 spans -- which is why the floor is permissive for real urban
    geometry rather than an obstacle (plan section 5, invariant 3).
    """
    r = min_turn_radius(width, curve_safety_x100)
    ratio = span_length / (2.0 * r)
    if ratio >= 1.0:
        return math.pi
    return 2.0 * math.asin(ratio)


# -------------------------------------------------------------- projection ---
# WGS84. Over a 4 km box a local tangent plane about the centre is sub-metre,
# and using the true radii of curvature rather than a mean sphere costs four
# lines, so there is no reason to approximate.
WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563
WGS84_E2 = WGS84_F * (2.0 - WGS84_F)


class LocalProjection:
    """Lat/lon <-> world units about a fixed centre.

    Two stages, kept separate on purpose:
      1. geodetic -> local metric ENU (east, north) about (lat0, lon0);
      2. metric -> world units, and a rotation that puts the route's principal
         axis on +X (the generator's TD5_TG_AXIS_HEADING).

    The rotation is set later by the conditioner, once it has the polyline to
    run PCA on, so a projection can be built before the route is known.
    """

    def __init__(self, lat0: float, lon0: float,
                 units_per_metre: float = UNITS_PER_METRE_DERIVED):
        self.lat0 = float(lat0)
        self.lon0 = float(lon0)
        self.units_per_metre = float(units_per_metre)
        phi = math.radians(self.lat0)
        s = math.sin(phi)
        w = 1.0 - WGS84_E2 * s * s
        # Meridional and prime-vertical radii of curvature at the centre.
        self._m_per_deg_lat = math.radians(1.0) * WGS84_A * (1.0 - WGS84_E2) / (w ** 1.5)
        self._m_per_deg_lon = math.radians(1.0) * WGS84_A * math.cos(phi) / math.sqrt(w)
        # Set by set_rotation()/set_offset(); identity until then.
        self._cos_t = 1.0
        self._sin_t = 0.0
        # The conditioner TRANSLATES the route so node 0 lands at (0, 0), which
        # td5_tg_road.c:733 requires. The place cache has to carry the same
        # translation or the terrain sits somewhere else entirely -- the second
        # half of the frame bug the rotation field documents.
        self._off_x = 0.0
        self._off_z = 0.0

    # -- stage 1 ----------------------------------------------------------
    def to_metres(self, lat: float, lon: float) -> tuple[float, float]:
        """(east, north) metres from the centre."""
        return ((lon - self.lon0) * self._m_per_deg_lon,
                (lat - self.lat0) * self._m_per_deg_lat)

    def to_degrees(self, east: float, north: float) -> tuple[float, float]:
        """Inverse of to_metres, for drawing a conditioned route back on a map."""
        return (self.lat0 + north / self._m_per_deg_lat,
                self.lon0 + east / self._m_per_deg_lon)

    # -- stage 2 ----------------------------------------------------------
    def set_rotation(self, theta: float) -> None:
        """Rotate the metric frame by theta before scaling to world units.

        The conditioner passes the angle that carries the route's principal axis
        onto +X. See the PCA step in geo_condition; this is what makes a real
        A-to-B drive as close to axis-monotone as its own shape allows, which is
        what the engine's non-trapping proof needs (plan section 5).
        """
        self._cos_t = math.cos(theta)
        self._sin_t = math.sin(theta)

    def set_offset(self, dx: float, dz: float) -> None:
        """World-unit translation applied after rotation and scaling."""
        self._off_x = float(dx)
        self._off_z = float(dz)

    def to_world(self, lat: float, lon: float) -> tuple[float, float]:
        """(x, z) world units. x is the generator's down-track axis."""
        e, n = self.to_metres(lat, lon)
        x = (e * self._cos_t - n * self._sin_t) * self.units_per_metre
        z = (e * self._sin_t + n * self._cos_t) * self.units_per_metre
        return (x + self._off_x, z + self._off_z)

    def world_to_latlon(self, x: float, z: float) -> tuple[float, float]:
        xm = (x - self._off_x) / self.units_per_metre
        zm = (z - self._off_z) / self.units_per_metre
        e = xm * self._cos_t + zm * self._sin_t
        n = -xm * self._sin_t + zm * self._cos_t
        return self.to_degrees(e, n)

    def describe(self) -> dict:
        return {
            "kind": "local_tangent_plane_wgs84",
            "lat0": self.lat0,
            "lon0": self.lon0,
            "units_per_metre": self.units_per_metre,
            "m_per_deg_lat": self._m_per_deg_lat,
            "m_per_deg_lon": self._m_per_deg_lon,
            "rotation_rad": math.atan2(self._sin_t, self._cos_t),
            "offset_x": self._off_x,
            "offset_z": self._off_z,
        }


# ------------------------------------------------------------------ cache ---
# The on-disk contract of the plan's section 4. The C side reads these and
# knows nothing about HTTP, Overpass, tiles or projections.

CACHE_ROOT = os.path.join("re", "assets", "geo")
DEM_OVERRIDE_DIR = os.path.join(CACHE_ROOT, "_dem_override")

CONTRACT_FILES = (
    "PLACE.JSON",       # name, centre, bbox, projection, per-layer provenance
    "HEIGHT.R16",       # DEM on the world cell grid
    "CANOPY.R8",        # canopy height (Meta/WRI 1 m)
    "COVER.R8",         # land-cover class (ESA WorldCover 10 m)
    "WATER.R8",         # water mask, OSM hydrography x COVER
    "ROADS.JSON",       # road graph in world units
    "BUILDINGS.JSON",   # footprints + height (measured or estimated, flagged)
    "AREAS.JSON",       # plaza / park polygons with internal paths
    "SIGNALS.JSON",     # highway=traffic_signals positions
    "FORKS.JSON",       # detected fork candidates + user confirmations
    "ROUTE.JSON",       # the CONDITIONED drive
    "ROUTE_RAW.JSON",   # the route as drawn, for re-conditioning
)


def slugify(name: str) -> str:
    s = re.sub(r"[^a-z0-9]+", "_", name.strip().lower())
    return s.strip("_") or "place"


def place_dir(slug: str, root: str = CACHE_ROOT) -> str:
    return os.path.join(root, slugify(slug))


def write_json(path: str, obj) -> None:
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


def read_json(path: str):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ------------------------------------------------------ mirror self-check ---
# The mirroring rule at the top of this file, made executable.

_CHECKS = (
    # (repo-relative header, regex with one capture, expected python value)
    ("td5mod/src/td5re/td5_trackgen.h",
     r"#define\s+TD5_TG_LANE_WIDTH\s+(\d+)", TD5_TG_LANE_WIDTH),
    ("td5mod/src/td5re/td5_trackgen.h",
     r"#define\s+TD5_TG_SPAN_LENGTH\s+(\d+)", TD5_TG_SPAN_LENGTH),
    ("td5mod/src/td5re/td5_tg_world.h",
     r"#define\s+TG_WORLD_CELL\s+([\d.]+)", TG_WORLD_CELL),
    ("td5mod/src/td5re/td5_trackgen_internal.h",
     r"#define\s+TD5_TG_MAX_SPANS\s+(\d+)", TD5_TG_MAX_SPANS),
    ("td5mod/src/td5re/td5_trackgen_internal.h",
     r"#define\s+TD5_TG_GRID_SPAN\s+(\d+)", TD5_TG_GRID_SPAN),
    ("td5mod/src/td5re/td5_trackgen_internal.h",
     r"#define\s+TD5_TG_MAX_LANES\s+(\d+)", TD5_TG_MAX_LANES),
    ("td5mod/src/td5re/td5_trackgen_internal.h",
     r"#define\s+TD5_TG_HEIGHT_NIBBLE\s+(\d+)", TD5_TG_HEIGHT_NIBBLE),
    ("td5mod/src/td5re/td5_tg_road.c",
     r"#define\s+TG_ROAD_GRADE_ABSMAX\s+([\d.]+)", TG_ROAD_GRADE_ABSMAX),
)


def check_mirror(repo_root: str = ".") -> int:
    """Diff the mirrored constants against the C. Returns the failure count."""
    bad = 0
    for rel, pattern, expect in _CHECKS:
        path = os.path.join(repo_root, rel)
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError as exc:
            print("MISS %s (%s)" % (rel, exc))
            bad += 1
            continue
        m = re.search(pattern, text)
        if not m:
            print("MISS %s :: %s not found" % (rel, pattern))
            bad += 1
            continue
        got = float(m.group(1))
        if abs(got - float(expect)) > 1e-9:
            print("DIFF %s :: C has %g, geo_common has %g" % (rel, got, expect))
            bad += 1
        else:
            print("ok   %s = %g" % (pattern.split(r"\s+")[1], got))
    return bad


def _main(argv) -> int:
    if len(argv) > 1 and argv[1] == "--check":
        root = argv[2] if len(argv) > 2 else "."
        bad = check_mirror(root)
        print("\n%d mismatch(es)" % bad)
        return 1 if bad else 0

    # Default: print the derived quantities, which is what the plan quotes.
    upm = UNITS_PER_METRE_DERIVED
    print("units per metre (derived) : %.1f" % upm)
    print("span length               : %d units = %.2f m"
          % (TD5_TG_SPAN_LENGTH, TD5_TG_SPAN_LENGTH / upm))
    print("world cell                : %.0f units = %.2f m"
          % (TG_WORLD_CELL, TG_WORLD_CELL / upm))
    print("max route (3000 spans)    : %.2f km"
          % (TD5_TG_MAX_SPANS * TD5_TG_SPAN_LENGTH / upm / 1000.0))
    print("default route (1800)      : %.2f km"
          % (1800 * TD5_TG_SPAN_LENGTH / upm / 1000.0))
    print("lead-in                   : %d nodes" % TG_LEAD_IN_NODES)
    for lanes in (2, 4, 12):
        w = lanes * TD5_TG_LANE_WIDTH
        print("lanes=%-2d width=%-6d r_min=%-8.0f (%.1f m)  max turn/span=%.1f deg"
              % (lanes, w, min_turn_radius(w), min_turn_radius(w) / upm,
                 math.degrees(max_turn_per_span(w))))
    print("adjacent skip (default)   : %d spans" % adjacent_skip())
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
