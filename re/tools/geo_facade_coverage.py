"""geo_facade_coverage.py -- how much of a street has a building beside it.

The measure behind round 1012's "facade within 25 m of only 3-9% of street
sides" (memory note 2026-10-08): stand on a road centreline every STEP metres,
cast a ray RAY metres square to the road on each side, and count a side as
FRONTED when the ray meets a building footprint. Reported for

  * every drivable way in ROADS.JSON, split calle / avenida by the OSM name
    (La Plata's two street kinds), and
  * the conditioned race route in ROUTE.JSON, when there is one,

so a building source A/B is a number rather than a screenshot. Any place dir or
derived _route/ dir works: everything is read in that dir's own frame.

usage: python geo_facade_coverage.py <dir> [<dir> ...] [--ray 25] [--step 10]
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

import shapely
from shapely.strtree import STRtree


def _load(d: str, name: str):
    p = os.path.join(d, name)
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def _upm(d: str) -> float:
    pl = _load(d, "PLACE.JSON") or {}
    return float((pl.get("projection") or {}).get("units_per_metre", 430.0))


def _kind(road: dict) -> str:
    n = (road.get("name") or "").lower()
    if n.startswith(("avenida", "diagonal", "boulevard", "bulevar")):
        return "avenida"
    if n.startswith("calle"):
        return "calle"
    return "other"


def _rays(pts, step: float, ray: float):
    """(left ray, right ray) LineStrings every `step` along a polyline."""
    out = []
    carry = 0.0
    for (x0, z0), (x1, z1) in zip(pts, pts[1:]):
        seg = math.hypot(x1 - x0, z1 - z0)
        if seg <= 1e-9:
            continue
        tx, tz = (x1 - x0) / seg, (z1 - z0) / seg
        nx, nz = -tz, tx
        s = carry
        while s < seg:
            px, pz = x0 + tx * s, z0 + tz * s
            out.append((shapely.LineString([(px, pz), (px + nx * ray,
                                                      pz + nz * ray)]),
                        shapely.LineString([(px, pz), (px - nx * ray,
                                                      pz - nz * ray)])))
            s += step
        carry = s - seg
    return out


def coverage(d: str, ray: float = 25.0, step: float = 10.0) -> dict:
    upm = _upm(d)
    bdoc = _load(d, "BUILDINGS.JSON") or {"buildings": []}
    polys = []
    for b in bdoc["buildings"]:
        pts = [(p["x"] / upm, p["z"] / upm) for p in b.get("points") or []]
        if len(pts) >= 3:
            g = shapely.Polygon(pts)
            if not g.is_valid:
                g = g.buffer(0)
            if not g.is_empty:
                polys.append(g)
    tree = STRtree(polys)

    def frac(rays):
        if not rays:
            return 0, 0
        flat = [r for pair in rays for r in pair]
        hit = tree.query(flat, predicate="intersects")
        got = set(int(i) for i in hit[0])
        return len(got), len(flat)

    res = {"dir": d, "buildings": len(polys), "ray_m": ray, "step_m": step}
    roads = (_load(d, "ROADS.JSON") or {}).get("roads", [])
    by = {}
    for r in roads:
        pts = [(p["x"] / upm, p["z"] / upm) for p in r.get("points") or []]
        k = _kind(r)
        by.setdefault(k, []).extend(_rays(pts, step, ray))
    tot_h = tot_n = 0
    for k, rays in sorted(by.items()):
        h, n = frac(rays)
        res["roads_" + k] = {"sides": n, "fronted": h,
                             "share": round(h / n, 4) if n else None}
        tot_h += h
        tot_n += n
    res["roads_all"] = {"sides": tot_n, "fronted": tot_h,
                        "share": round(tot_h / tot_n, 4) if tot_n else None}
    route = _load(d, "ROUTE.JSON")
    if route and route.get("points"):
        u = float(route.get("units_per_metre", upm))
        pts = [(p["x"] / u, p["z"] / u) for p in route["points"]]
        h, n = frac(_rays(pts, step, ray))
        res["route"] = {"sides": n, "fronted": h,
                        "share": round(h / n, 4) if n else None,
                        "length_km": route.get("length_km")}
    return res


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--ray", type=float, default=25.0)
    ap.add_argument("--step", type=float, default=10.0)
    a = ap.parse_args(argv)
    for d in a.dirs:
        print(json.dumps(coverage(d, a.ray, a.step)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
