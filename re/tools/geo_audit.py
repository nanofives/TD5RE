"""geo_audit.py -- acceptance gate for a GEO TRACK place and route.

Same contract as the generator's existing audits (re/tools/tg_network_audit.py,
tg_strip_audit.py): print every violation, exit 1 if any. This is what makes a
geo build trustworthy BEFORE any C code exists, and it is deliberately stricter
than the engine, because a failure here costs a message and a failure in the
engine costs a debugging session.

Checks, each traced to the constraint it defends:

  ROUTE
   R1  span count within TD5_TG_MAX_SPANS -- s_struct[]/s_rn[] are indexed by
       node with no bounds check (td5_tg_road.c:123-132), so overrunning corrupts
       memory rather than failing
   R2  long enough to hold a grid, a race and a run-off
   R3  node 0 exactly at the origin, and the first TG_LEAD_IN_NODES nodes
       straight along +X (td5_tg_road.c:733-741)
   R4  uniform span spacing -- TG_Node has no arclength field, so uneven spacing
       is mis-measured in silence
   R5  turn radius within the curve_safety floor (td5_tg_road.c:638-641)
   R6  no self-crossing beyond the adjacent-skip window (tg_too_close,
       td5_trackgen.c:1760)
   R7  lane counts in range
   R8  route inside the cached DEM box, or the terrain under it is undefined

  PLACE
   P1  the four rasters share one grid exactly
   P2  HEIGHT is not degenerate (some relief, no all-nodata)
   P3  attribution present -- OSM is ODbL and the credit is not optional
   P4  building height provenance recorded for every building
   P5  the exaggerated gradient stays inside TG_ROAD_GRADE_ABSMAX
"""
from __future__ import annotations

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo_common import (  # noqa: E402
    CACHE_ROOT,
    ELEVATION_EXAGGERATION,
    TD5_TG_LANE_WIDTH,
    TD5_TG_MAX_LANES,
    TD5_TG_MAX_SPANS,
    TD5_TG_SPAN_LENGTH,
    TG_LEAD_IN_NODES,
    TG_ROAD_GRADE_ABSMAX,
    adjacent_skip,
    max_turn_per_span,
    place_dir,
    read_json,
    too_close_need,
)
from geo_raster import Raster  # noqa: E402


class Audit:
    def __init__(self) -> None:
        self.fail: list[str] = []
        self.warn: list[str] = []
        self.ok: list[str] = []

    def check(self, tag: str, cond: bool, msg: str, warn_only: bool = False) -> bool:
        if cond:
            self.ok.append("%s %s" % (tag, msg))
        elif warn_only:
            self.warn.append("%s %s" % (tag, msg))
        else:
            self.fail.append("%s %s" % (tag, msg))
        return cond

    def report(self, verbose: bool) -> int:
        if verbose:
            for line in self.ok:
                print("  ok   %s" % line)
        for line in self.warn:
            print("  WARN %s" % line)
        for line in self.fail:
            print("  FAIL %s" % line)
        print("\n%d passed, %d warning(s), %d FAILURE(S)"
              % (len(self.ok), len(self.warn), len(self.fail)))
        return 1 if self.fail else 0


def audit_route(route: dict, a: Audit, height: Raster | None = None) -> None:
    pts = [(p["x"], p["z"]) for p in route["points"]]
    lanes = [p.get("lanes", 2) for p in route["points"]]
    step = route.get("span_length", TD5_TG_SPAN_LENGTH)
    lane_w = route.get("lane_width", TD5_TG_LANE_WIDTH)
    safety = route.get("curve_safety_x100", 180)
    n = len(pts)
    spans = n - 1

    a.check("R1", spans <= TD5_TG_MAX_SPANS,
            "%d spans (cap %d)" % (spans, TD5_TG_MAX_SPANS))
    a.check("R2", spans >= TG_LEAD_IN_NODES + 150,
            "%d spans, needs >= %d for grid+race+runoff"
            % (spans, TG_LEAD_IN_NODES + 150))

    origin_ok = abs(pts[0][0]) < 1e-6 and abs(pts[0][1]) < 1e-6
    lead_ok = all(abs(pts[i][1]) < 1e-6 and pts[i][0] > pts[i - 1][0]
                  for i in range(1, min(TG_LEAD_IN_NODES + 1, n)))
    a.check("R3", origin_ok and lead_ok,
            "node 0 at origin and %d-node lead-in straight along +X "
            "(origin=%s lead=%s)" % (TG_LEAD_IN_NODES, origin_ok, lead_ok))

    segs = [math.dist(pts[i], pts[i + 1]) for i in range(spans)]
    worst_dev = max(abs(s - step) for s in segs) if segs else 0.0
    a.check("R4", worst_dev < 1.0,
            "span spacing uniform to %.3f units (of %.0f)" % (worst_dev, step))

    over = 0
    worst_turn = 0.0
    for i in range(1, spans):
        h0 = math.atan2(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1])
        h1 = math.atan2(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
        d = abs((h1 - h0 + math.pi) % (2 * math.pi) - math.pi)
        worst_turn = max(worst_turn, d)
        if d > max_turn_per_span(lanes[i] * lane_w, step, safety) + 1e-6:
            over += 1
    a.check("R5", over == 0,
            "turn radius floor: %d node(s) over, worst turn %.1f deg"
            % (over, math.degrees(worst_turn)))

    skip = adjacent_skip(lane_w, step, safety)
    hits = 0
    worst_pair = None
    for i in range(n):
        xi, zi = pts[i]
        wi = lanes[i] * lane_w
        for j in range(i + skip, n):
            need = too_close_need(wi, lanes[j] * lane_w, lane_w)
            d2 = (pts[j][0] - xi) ** 2 + (pts[j][1] - zi) ** 2
            if d2 < need * need:
                hits += 1
                if worst_pair is None or d2 < worst_pair[0]:
                    worst_pair = (d2, i, j)
    msg = "no self-crossing beyond skip=%d" % skip
    if worst_pair:
        msg = ("%d overlapping pair(s) beyond skip=%d, closest nodes %d/%d at "
               "%.0f units" % (hits, skip, worst_pair[1], worst_pair[2],
                               math.sqrt(worst_pair[0])))
    a.check("R6", hits == 0, msg)

    a.check("R7", all(1 <= l <= TD5_TG_MAX_LANES for l in lanes),
            "lane counts in 1..%d (min %d, max %d)"
            % (TD5_TG_MAX_LANES, min(lanes), max(lanes)))

    if height is not None:
        # R9 before R8: an offset FRAME makes the box test meaningless, because
        # the route and the grid would be describing different planes. This is the
        # check that was missing when the first La Plata run put half the route
        # outside its own terrain.
        r_rot = route.get("rotation_rad")
        if r_rot is None:
            r_rot = (route.get("projection") or {}).get("rotation_rad")
        a.check("R9", r_rot is not None
                and abs(float(r_rot) - height.rotation_rad) < 1e-9,
                "route frame matches the raster frame (route %.6f vs raster "
                "%.6f rad)" % (float(r_rot) if r_rot is not None else float("nan"),
                               height.rotation_rad))

        h, w = height.data.shape
        x0, z0 = height.origin_x, height.origin_z
        x1 = x0 + (w - 1) * height.cell
        z1 = z0 + (h - 1) * height.cell
        # The lead-in is synthetic and sits at the origin by construction, so it
        # legitimately falls outside the fetched box; only the real body matters.
        body = pts[TG_LEAD_IN_NODES:]
        inside = sum(1 for x, z in body if x0 <= x <= x1 and z0 <= z <= z1)
        a.check("R8", inside == len(body),
                "route body inside the DEM box (%d of %d nodes)"
                % (inside, len(body)),
                warn_only=True)


def audit_place(slug: str, root: str, a: Audit) -> Raster | None:
    d = place_dir(slug, root)
    need = ("PLACE.JSON", "HEIGHT.R16", "COVER.R8", "WATER.R8",
            "ROADS.JSON", "BUILDINGS.JSON", "AREAS.JSON", "SIGNALS.JSON")
    missing = [f for f in need if not os.path.exists(os.path.join(d, f))]
    if not a.check("P0", not missing, "cache complete (missing: %s)"
                   % (", ".join(missing) or "none")):
        return None

    place = read_json(os.path.join(d, "PLACE.JSON"))
    height = Raster.read(os.path.join(d, "HEIGHT.R16"))
    cover = Raster.read(os.path.join(d, "COVER.R8"))
    water = Raster.read(os.path.join(d, "WATER.R8"))

    same = (height.data.shape == cover.data.shape == water.data.shape
            and (height.origin_x, height.origin_z, height.cell)
            == (cover.origin_x, cover.origin_z, cover.cell)
            == (water.origin_x, water.origin_z, water.cell))
    a.check("P1", same, "HEIGHT/COVER/WATER share one grid (%dx%d, cell %.0f)"
            % (height.data.shape[1], height.data.shape[0], height.cell))

    hs = height.stats()
    upm = place["projection"]["units_per_metre"]
    relief_m = (hs["max"] - hs["min"]) / upm
    a.check("P2", hs["valid"] > 0 and relief_m > 0.5,
            "HEIGHT has %d valid cells and %.1f m of relief"
            % (hs["valid"], relief_m))

    attr = place.get("attribution") or []
    a.check("P3", any("OpenStreetMap" in s for s in attr),
            "attribution records OpenStreetMap (ODbL)")

    b = read_json(os.path.join(d, "BUILDINGS.JSON"))
    bl = b.get("buildings", [])
    unflagged = [x for x in bl if x.get("height_m") and not x.get("height_src")]
    a.check("P4", not unflagged,
            "every building's height provenance recorded (%d building(s), "
            "%d unflagged)" % (len(bl), len(unflagged)))
    est = b.get("height_provenance", {}).get("estimated", 0)
    if bl:
        a.check("P4b", est / len(bl) < 0.98,
                "%.0f%% of building heights are estimated rather than measured"
                % (100.0 * est / len(bl)), warn_only=True)

    # P5: worst exaggerated gradient the terrain can ask of the road. The road
    # profile caps it (td5_tg_road.c:152-204), so this is a heads-up about how
    # much clipping the cap will do, not a defect.
    d16 = height.data.astype(float) * height.scale + height.bias
    gx = abs(d16[:, 1:] - d16[:, :-1]).max() if d16.shape[1] > 1 else 0.0
    gz = abs(d16[1:, :] - d16[:-1, :]).max() if d16.shape[0] > 1 else 0.0
    worst_grade = ELEVATION_EXAGGERATION * max(gx, gz) / height.cell
    a.check("P5", worst_grade <= TG_ROAD_GRADE_ABSMAX,
            "worst exaggerated cell gradient %.3f vs absolute cap %.2f "
            "(the road profile will clip above this)"
            % (worst_grade, TG_ROAD_GRADE_ABSMAX), warn_only=True)
    return height


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--place", help="cache slug to audit, e.g. la_plata")
    ap.add_argument("--route", help="a conditioned ROUTE.JSON to audit")
    ap.add_argument("--root", default=CACHE_ROOT)
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="also print the checks that passed")
    a = ap.parse_args(argv)
    if not a.place and not a.route:
        ap.error("give --place and/or --route")

    au = Audit()
    height = None
    if a.place:
        print("PLACE %s" % a.place)
        height = audit_place(a.place, a.root, au)
    if a.route:
        print("ROUTE %s" % a.route)
        audit_route(read_json(a.route), au, height)
    return au.report(a.verbose)


if __name__ == "__main__":
    sys.exit(main())
