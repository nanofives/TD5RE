"""[GEO LAND] Build an OFFLINE landmark fixture place, no network.

WHY THIS EXISTS. The real La Plata cache is thin where the landmark path is
interesting: 2047 footprints, but only 4 carry a roof:shape (3 mansard, 1
flat), ZERO carry building:part or min_height, and only 2 of the 10 tagged
landmarks come within 100 m of the route. So `roof:shape`, `building:part` and
the prefab-table fallback shipped exercised on two buildings. Re-fetching a
richer city needs the network, which this workstream does not have.

This writes a SYNTHETIC place whose road, terrain and route are La Plata's
(copied byte for byte, so the walk, the spans and the world are the real
thing) and whose BUILDINGS.JSON is a controlled test set: one building per
roof:shape the reader accepts, a multi-part stack with min_height, a tagged
landmark with no 3D tags at all (the prefab-table fallback), and the
degenerate rings the emitter has to refuse rather than crash on.

Each test building is planted beside a KNOWN span, spaced far enough apart
that one --StartSpanOffset framedump frames exactly one of them.

    python re/tools/geo_fixtures/land_fixture_build.py [--slug land_test]
                                                       [--src la_plata]
                                                       [--first-span 200]
                                                       [--step 40] [--keep-areas]

Then, from the worktree root:

    pwsh -NoProfile -Command "& ./verify/topo_gen.ps1 -Seed 20260901 \
        -Tag land -Port 37193 -Extra @{TD5RE_GEO_PLACE='land_test'}"

The output place lives under re/assets/geo/<slug>/ beside the real caches and
is disposable: this script is the committed artefact, not the place.
"""
import argparse
import json
import math
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
GEO = os.path.join(ROOT, "re", "assets", "geo")

# Files the fixture inherits unchanged: the world and the drive are REAL, so
# only the vector layer under test is synthetic.
COPY = ("ROUTE.JSON", "ROUTE_RAW.JSON", "HEIGHT.R16", "WATER.R8", "COVER.R8")

M = 430.0          # world units per metre; asserted against the cache below


def _rect(cx, cz, ux, uz, half_len, half_wid):
    """Axis-aligned-to-(ux,uz) rectangle ring, CCW, closed."""
    vx, vz = -uz, ux
    pts = []
    for sl, sw in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        pts.append({"x": cx + ux * half_len * sl + vx * half_wid * sw,
                    "z": cz + uz * half_len * sl + vz * half_wid * sw})
    pts.append(dict(pts[0]))
    return pts


def _ell(cx, cz, ux, uz, ra, rb, n=16):
    vx, vz = -uz, ux
    pts = []
    for i in range(n):
        a = 2.0 * math.pi * i / n
        pts.append({"x": cx + ux * ra * math.cos(a) + vx * rb * math.sin(a),
                    "z": cz + uz * ra * math.cos(a) + vz * rb * math.sin(a)})
    pts.append(dict(pts[0]))
    return pts


def _ell_ring(cx, cz, ux, uz, ra, rb, n=16):
    return _ell(cx, cz, ux, uz, ra, rb, n)


def _lshape(cx, cz, ux, uz, a, b):
    """CONCAVE L, so the pitched-roof convexity gate and the ear clipper both
    see a ring a centroid fan would get wrong."""
    vx, vz = -uz, ux

    def p(l, w):
        return {"x": cx + ux * l + vx * w, "z": cz + uz * l + vz * w}
    pts = [p(-a, -b), p(a, -b), p(a, 0.0), p(0.0, 0.0), p(0.0, b), p(-a, b)]
    pts.append(dict(pts[0]))
    return pts


def _collinear(cx, cz, ux, uz, a):
    """Degenerate: three points on a line, zero area."""
    pts = [{"x": cx - ux * a, "z": cz - uz * a},
           {"x": cx, "z": cz},
           {"x": cx + ux * a, "z": cz + uz * a}]
    pts.append(dict(pts[0]))
    return pts


def _bowtie(cx, cz, ux, uz, a, b):
    """Self-intersecting ring: the ear clipper must refuse, not loop."""
    vx, vz = -uz, ux

    def p(l, w):
        return {"x": cx + ux * l + vx * w, "z": cz + uz * l + vz * w}
    pts = [p(-a, -b), p(a, b), p(a, -b), p(-a, b)]
    pts.append(dict(pts[0]))
    return pts


# (label, roof_shape, height_m, min_height_m, part, landmark, height_src, shape)
# One row per thing under test. `shape` names the ring builder below.
CASES = [
    ("gabled",      "gabled",     12.0, None, False, True,  "osm_height", "rect"),
    ("hipped",      "hipped",     12.0, None, False, True,  "osm_height", "rect"),
    ("pyramidal",   "pyramidal",  14.0, None, False, True,  "osm_height", "square"),
    ("dome",        "dome",       18.0, None, False, True,  "osm_height", "round"),
    ("round",       "round",      11.0, None, False, True,  "osm_height", "round"),
    ("skillion",    "skillion",   10.0, None, False, True,  "osm_height", "rect"),
    ("half-hipped", "half-hipped", 12.0, None, False, True, "osm_height", "rect"),
    ("onion",       "onion",      16.0, None, False, True,  "osm_height", "round"),
    ("flat",        "flat",       15.0, None, False, True,  "osm_height", "rect"),
    ("mansard",     "mansard",    13.0, None, False, True,  "osm_height", "rect"),
    ("gambrel",     "gambrel",    13.0, None, False, True,  "osm_height", "rect"),
    ("concave-gabled", "gabled",  12.0, None, False, True,  "osm_height", "lshape"),
    # building:part stack -- a plinth, a mid block and a tower, each starting
    # where the one below it stops. OSM's min_height is the BASE of the part
    # and height is its TOP, both measured from the ground.
    ("part-base",   None,          8.0, None, True,  False, "osm_height", "rect"),
    ("part-mid",    None,         20.0,  8.0, True,  False, "osm_height", "rect_in"),
    ("part-top",    "pyramidal",  34.0, 20.0, True,  True,  "osm_height", "rect_in2"),
    # The prefab-table fallback: tagged a landmark, no 3D tag of any kind.
    ("fallback",    None,          0.0, None, False, True,  None,         "big"),
    ("fallback-sm", None,          0.0, None, False, True,  None,         "rect"),
    # Refusals, not renders.
    ("degenerate-collinear", None, 10.0, None, False, True, "osm_height", "line"),
    ("degenerate-bowtie",    "gabled", 10.0, None, False, True, "osm_height", "bowtie"),
    ("tiny",        "gabled",      6.0, None, False, True,  "osm_height", "tiny"),
]


def build_ring(shape, cx, cz, ux, uz):
    if shape == "rect":
        return _rect(cx, cz, ux, uz, 9.0 * M, 6.0 * M)
    if shape == "rect_in":
        return _rect(cx, cz, ux, uz, 7.0 * M, 4.5 * M)
    if shape == "rect_in2":
        return _rect(cx, cz, ux, uz, 5.0 * M, 3.0 * M)
    if shape == "square":
        return _rect(cx, cz, ux, uz, 7.0 * M, 7.0 * M)
    if shape == "round":
        return _ell_ring(cx, cz, ux, uz, 8.0 * M, 8.0 * M, 16)
    if shape == "lshape":
        return _lshape(cx, cz, ux, uz, 10.0 * M, 8.0 * M)
    if shape == "big":
        return _rect(cx, cz, ux, uz, 16.0 * M, 13.0 * M)
    if shape == "line":
        return _collinear(cx, cz, ux, uz, 6.0 * M)
    if shape == "bowtie":
        return _bowtie(cx, cz, ux, uz, 7.0 * M, 5.0 * M)
    if shape == "tiny":
        return _rect(cx, cz, ux, uz, 1.0 * M, 1.0 * M)
    raise SystemExit("unknown shape " + shape)


def polygon_area_m2(pts):
    a = 0.0
    for i in range(len(pts) - 1):
        a += pts[i]["x"] * pts[i + 1]["z"] - pts[i + 1]["x"] * pts[i]["z"]
    return abs(a) * 0.5 / (M * M)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slug", default="land_test")
    ap.add_argument("--src", default="la_plata")
    ap.add_argument("--first-span", type=int, default=200)
    ap.add_argument("--step", type=int, default=40)
    ap.add_argument("--offset-m", type=float, default=22.0,
                    help="lateral standoff of the footprint CENTRE, metres")
    ap.add_argument("--keep-areas", action="store_true",
                    help="keep the source plazas (default: none, so a landmark "
                         "capture has no plaza geometry in it)")
    a = ap.parse_args()

    src = os.path.join(GEO, a.src)
    dst = os.path.join(GEO, a.slug)
    if not os.path.isdir(src):
        raise SystemExit("no source place at " + src)
    os.makedirs(dst, exist_ok=True)
    for f in COPY:
        shutil.copyfile(os.path.join(src, f), os.path.join(dst, f))

    place = json.load(open(os.path.join(src, "PLACE.JSON")))
    upm = place["projection"]["units_per_metre"]
    if abs(upm - M) > 1e-6:
        raise SystemExit("source units_per_metre %r, fixture assumes %r"
                         % (upm, M))
    place["slug"] = a.slug
    place["name"] = "LAND FIXTURE (%s road, synthetic buildings)" % a.src
    place["fixture"] = ("re/tools/geo_fixtures/land_fixture_build.py -- "
                        "roof:shape / building:part / prefab-fallback test set")
    json.dump(place, open(os.path.join(dst, "PLACE.JSON"), "w"),
              indent=1, sort_keys=True)

    route = json.load(open(os.path.join(src, "ROUTE.JSON")))
    pts = route["points"]
    n = len(pts)

    out, rows = [], []
    for k, (label, roof, h_m, mh_m, part, lm, hsrc, shape) in enumerate(CASES):
        si = a.first_span + k * a.step
        if si + 1 >= n:
            raise SystemExit("route has %d nodes, case %d wants span %d"
                             % (n, k, si))
        x0, z0 = pts[si]["x"], pts[si]["z"]
        x1, z1 = pts[si + 1]["x"], pts[si + 1]["z"]
        tx, tz = x1 - x0, z1 - z0
        tl = math.hypot(tx, tz) or 1.0
        tx, tz = tx / tl, tz / tl
        side = 1.0 if (k % 2 == 0) else -1.0
        # left normal of the travel direction, matching tg_geo_outward
        lx, lz = tz * side, -tx * side
        # A building:part stack shares ONE site with the part below it.
        if label.startswith("part-") and label != "part-base":
            si = rows[-1][1]
            x0, z0 = rows[-1][2], rows[-1][3]
            lx, lz, tx, tz = rows[-1][4], rows[-1][5], rows[-1][6], rows[-1][7]
        off = a.offset_m * M
        cx = x0 + lx * off
        cz = z0 + lz * off
        ring = build_ring(shape, cx, cz, tx, tz)
        out.append({
            "id": 900000 + k,
            "name": "LAND %s" % label,
            "class": "yes",
            "part": bool(part),
            "area_m2": round(polygon_area_m2(ring), 1),
            "height_m": h_m if h_m > 0.0 else None,
            "height_src": hsrc,
            "roof_shape": roof,
            "roof_height_m": None,
            # OSM tag values arrive as STRINGS from Overpass and geo_fetch
            # stores them raw, so the fixture stores them raw too -- a reader
            # that only accepts a JSON number would silently read 0 here, and
            # that is exactly the kind of miss this fixture is for.
            "min_height_m": (None if mh_m is None else str(mh_m)),
            "colour": None,
            "material": None,
            "landmark": bool(lm),
            "points": ring,
        })
        rows.append((label, si, x0, z0, lx, lz, tx, tz))

    json.dump({"buildings": out,
               "storey_height_m": 3.0,
               "height_provenance": {"fixture": len(out)}},
              open(os.path.join(dst, "BUILDINGS.JSON"), "w"),
              indent=1, sort_keys=True)

    areas = {"areas": []}
    if a.keep_areas:
        areas = json.load(open(os.path.join(src, "AREAS.JSON")))
    json.dump(areas, open(os.path.join(dst, "AREAS.JSON"), "w"),
              indent=1, sort_keys=True)
    sig = os.path.join(src, "SIGNALS.JSON")
    if os.path.isfile(sig):
        json.dump({"signals": []},
                  open(os.path.join(dst, "SIGNALS.JSON"), "w"), indent=1)

    print("fixture place %s  (road + terrain from %s, %d nodes)"
          % (dst, a.src, n))
    print("%-22s %6s %10s %9s %6s %5s %s"
          % ("case", "span", "roof", "height_m", "min_h", "part", "landmark"))
    for i, r in enumerate(rows):
        c = CASES[i]
        print("%-22s %6d %10s %9s %6s %5s %s"
              % (c[0], r[1], c[1] or "-", c[2] or "-", c[3] or "-",
                 "yes" if c[4] else "-", "yes" if c[5] else "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
