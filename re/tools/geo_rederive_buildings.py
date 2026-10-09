"""geo_rederive_buildings.py -- refresh ONE derived layer of a built route.

The in-game BUILD (td5_geo_route.c td5_geo_route_commit) writes <place>/_route/
from the immutable source cache: rasters re-gridded, every vector layer
re-projected into the route frame, DERIVED.OK last. It does not notice when a
source file changes afterwards -- DERIVED.OK carries a schema number, not a
hash -- so a place whose BUILDINGS.JSON was re-normalised (round 1012:
Overture footprints + Open Buildings heights) keeps racing the OLD buildings
until the user presses BUILD again.

This re-derives BUILDINGS.JSON alone, exactly the way gr_reproject_arr does:
every points[].x/z (and a top-level x/z) goes source frame -> lat/lon -> route
frame, every other field passes through. The two frames are rigid transforms
of one local tangent plane, so the result matches a fresh BUILD to float
rounding. Nothing else in _route/ is touched, and DERIVED.OK is left as is.

usage: python geo_rederive_buildings.py <place_dir> [--route-dir DIR]
           [--check]   compare against the current derived file, write nothing
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo_common import LocalProjection  # noqa: E402


def _proj(place: dict) -> LocalProjection:
    pr = place["projection"]
    p = LocalProjection(pr["lat0"], pr["lon0"], pr["units_per_metre"])
    p.set_rotation(pr.get("rotation_rad", 0.0))
    p.set_offset(pr.get("offset_x", 0.0), pr.get("offset_z", 0.0))
    return p


def rederive(place_dir: str, route_dir: str, check: bool = False) -> dict:
    with open(os.path.join(place_dir, "PLACE.JSON"), encoding="utf-8") as f:
        op = _proj(json.load(f))
    with open(os.path.join(route_dir, "PLACE.JSON"), encoding="utf-8") as f:
        np_ = _proj(json.load(f))
    with open(os.path.join(place_dir, "BUILDINGS.JSON"), encoding="utf-8") as f:
        doc = json.load(f)

    def move(x, z):
        la, lo = op.world_to_latlon(x, z)
        return np_.to_world(la, lo)

    for b in doc.get("buildings", []):
        for p in b.get("points") or []:
            p["x"], p["z"] = move(p["x"], p["z"])
        if isinstance(b.get("x"), (int, float)) and \
                isinstance(b.get("z"), (int, float)):
            b["x"], b["z"] = move(b["x"], b["z"])
    out = {"buildings": len(doc.get("buildings", []))}
    dst = os.path.join(route_dir, "BUILDINGS.JSON")
    if check:
        with open(dst, encoding="utf-8") as f:
            old = json.load(f)["buildings"]
        worst, n = 0.0, 0
        for a, b in zip(doc["buildings"], old):
            for p, q in zip(a.get("points") or [], b.get("points") or []):
                worst = max(worst, math.hypot(p["x"] - q["x"], p["z"] - q["z"]))
                n += 1
        out.update({"compared_points": n, "worst_units": worst,
                    "old_buildings": len(old)})
        return out
    tmp = dst + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, sort_keys=True, separators=(",", ":"))
        f.write("\n")
    os.replace(tmp, dst)
    out["bytes"] = os.path.getsize(dst)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("place_dir")
    ap.add_argument("--route-dir", help="default: <place_dir>/_route")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)
    rd = a.route_dir or os.path.join(a.place_dir, "_route")
    if not os.path.exists(os.path.join(rd, "DERIVED.OK")):
        print("no DERIVED.OK in %s: nothing to re-derive" % rd)
        return 1
    print(json.dumps(rederive(a.place_dir, rd, a.check)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
