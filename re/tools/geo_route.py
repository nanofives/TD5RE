"""geo_route.py -- the drivable road graph, and A-to-B routing with waypoints.

This is the routing half of Phase 1. The selector's interaction model is the one
Mariano chose: click A and B, then drag the line to insert waypoints that
re-route, like the Google Maps route editor. That needs three things from here:

  * a graph built from ROADS.JSON that only contains roads a car can drive;
  * a shortest-path that prefers bigger roads, because a racing line down a
    service alley is technically shortest and practically useless;
  * waypoint routing, which is just the concatenation of consecutive legs -- and
    is exactly what makes a self-crossing fixable with one drag (see
    geo_condition's crossing report).

WHY VERTEX-LEVEL AND NOT JUNCTION-LEVEL. Overpass was queried with `out geom`,
which inlines each way's coordinates but drops node ids, so junctions have to be
recovered from shared geometry rather than read off. Ways that connect in OSM
share the exact same node, and the projection is deterministic, so identical
lat/lon produce identical world coordinates and an exact (rounded) key finds
every junction. Building the graph at vertex level rather than contracting to
junctions costs more nodes -- about 16k for a 2.2 km radius of La Plata -- which
A* does not notice, and it hands back a dense polyline that needs no
re-interpolation before conditioning.
"""
from __future__ import annotations

import argparse
import heapq
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo_common import (  # noqa: E402
    CACHE_ROOT,
    LocalProjection,
    UNITS_PER_METRE_DERIVED,
    place_dir,
    read_json,
    write_json,
)

# Cost multiplier per highway class. Below 1.0 means "prefer this road".
# A racing line wants arterials, so trunk/primary are cheap and service roads
# and living streets are expensive without being forbidden -- they are still
# needed to reach a start point that sits on one.
CLASS_COST = {
    "motorway": 0.80, "motorway_link": 1.00,
    "trunk": 0.85, "trunk_link": 1.00,
    "primary": 0.90, "primary_link": 1.05,
    "secondary": 1.00, "secondary_link": 1.10,
    "tertiary": 1.15, "tertiary_link": 1.25,
    "unclassified": 1.40, "residential": 1.50, "living_street": 2.20,
    "service": 2.60, "road": 1.50,
}
DEFAULT_CLASS_COST = 1.5

# Coordinate rounding for junction detection, in world units. 10 units is 2.3 cm
# -- far tighter than any real road, so this only ever merges genuinely identical
# OSM nodes and never fuses two distinct ones.
SNAP = 10.0


class RoadGraph:
    def __init__(self, roads: list[dict], respect_oneway: bool = False):
        self.pos: list[tuple[float, float]] = []
        self.adj: list[list[tuple[int, float, int]]] = []   # (to, cost, road_idx)
        self.roads = roads
        self._key: dict[tuple[int, int], int] = {}
        self.respect_oneway = respect_oneway
        self._build(roads)

    def _node(self, x: float, z: float) -> int:
        k = (int(round(x / SNAP)), int(round(z / SNAP)))
        i = self._key.get(k)
        if i is None:
            i = len(self.pos)
            self._key[k] = i
            self.pos.append((x, z))
            self.adj.append([])
        return i

    def _build(self, roads: list[dict]) -> None:
        for ri, r in enumerate(roads):
            cost_mul = CLASS_COST.get(r.get("class"), DEFAULT_CLASS_COST)
            pts = r.get("points") or []
            prev = None
            for p in pts:
                n = self._node(p["x"], p["z"])
                if prev is not None and prev != n:
                    d = math.dist(self.pos[prev], self.pos[n]) * cost_mul
                    self.adj[prev].append((n, d, ri))
                    # A oneway restriction is real, but for a point-to-point
                    # RACE line it mostly just makes routes fail in a grid city
                    # where every second street is one-way. Off by default, and
                    # the selector can turn it on when authenticity matters more
                    # than finding a route at all.
                    if not self.respect_oneway or not r.get("oneway"):
                        self.adj[n].append((prev, d, ri))
                prev = n

    # -- queries ----------------------------------------------------------
    def nearest(self, x: float, z: float) -> int:
        best, bd = -1, float("inf")
        for i, (px, pz) in enumerate(self.pos):
            d = (px - x) ** 2 + (pz - z) ** 2
            if d < bd:
                bd, best = d, i
        return best

    def path(self, a: int, b: int) -> list[int] | None:
        """A* on straight-line distance, which is admissible because every edge
        cost is at least its length (the cheapest multiplier is 0.80, so the
        heuristic is scaled by that to stay a lower bound)."""
        if a == b:
            return [a]
        h_scale = min(CLASS_COST.values())
        goal = self.pos[b]

        def h(i: int) -> float:
            return math.dist(self.pos[i], goal) * h_scale

        dist = {a: 0.0}
        prev: dict[int, int] = {}
        pq = [(h(a), a)]
        seen = set()
        while pq:
            _, u = heapq.heappop(pq)
            if u in seen:
                continue
            seen.add(u)
            if u == b:
                out = [b]
                while out[-1] != a:
                    out.append(prev[out[-1]])
                out.reverse()
                return out
            du = dist[u]
            for v, w, _ri in self.adj[u]:
                nd = du + w
                if nd < dist.get(v, float("inf")):
                    dist[v] = nd
                    prev[v] = u
                    heapq.heappush(pq, (nd + h(v), v))
        return None

    def edge_road(self, u: int, v: int) -> int:
        for to, _w, ri in self.adj[u]:
            if to == v:
                return ri
        return -1

    def route(self, waypoints_world: list[tuple[float, float]]
              ) -> dict:
        """Route through every waypoint in order. A and B are the ends.

        Each leg is an independent A*, which is what lets the selector insert a
        waypoint and re-route only the two legs it touches.
        """
        if len(waypoints_world) < 2:
            raise ValueError("need at least A and B")
        nodes = [self.nearest(x, z) for x, z in waypoints_world]
        legs: list[list[int]] = []
        for i in range(len(nodes) - 1):
            p = self.path(nodes[i], nodes[i + 1])
            if p is None:
                return {"ok": False,
                        "reason": "no drivable path for leg %d of %d"
                                  % (i + 1, len(nodes) - 1)}
            legs.append(p)

        seq: list[int] = []
        for i, leg in enumerate(legs):
            seq.extend(leg if i == 0 else leg[1:])

        pts, lanes, road_ids = [], [], []
        for i, n in enumerate(seq):
            pts.append(self.pos[n])
            ri = self.edge_road(seq[i - 1], n) if i else self.edge_road(n, seq[1])
            r = self.roads[ri] if 0 <= ri < len(self.roads) else {}
            lanes.append(int(r.get("lanes") or 2))
            road_ids.append(r.get("id"))

        length = sum(math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1))
        names: list[str] = []
        for ri in road_ids:
            r = next((x for x in self.roads if x.get("id") == ri), None)
            nm = r.get("name") if r else None
            if nm and (not names or names[-1] != nm):
                names.append(nm)
        return {"ok": True, "points": pts, "lanes": lanes,
                "length_units": length, "street_names": names,
                "legs": [len(x) for x in legs]}


def load_place(slug: str, root: str = CACHE_ROOT) -> tuple[dict, RoadGraph, LocalProjection]:
    d = place_dir(slug, root)
    place = read_json(os.path.join(d, "PLACE.JSON"))
    roads = read_json(os.path.join(d, "ROADS.JSON"))["roads"]
    pr = place["projection"]
    proj = LocalProjection(pr["lat0"], pr["lon0"], pr["units_per_metre"])
    proj.set_rotation(pr.get("rotation_rad", 0.0))
    return place, RoadGraph(roads), proj


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--place", required=True, help="cache slug, e.g. la_plata")
    ap.add_argument("--root", default=CACHE_ROOT)
    ap.add_argument("--from", dest="a", required=True, metavar="LAT,LON")
    ap.add_argument("--to", dest="b", required=True, metavar="LAT,LON")
    ap.add_argument("--via", action="append", default=[], metavar="LAT,LON",
                    help="repeatable waypoint, in order")
    ap.add_argument("--out", help="write the raw route JSON here")
    a = ap.parse_args(argv)

    place, g, proj = load_place(a.place, a.root)
    print("graph: %d nodes, %d roads" % (len(g.pos), len(g.roads)))

    def parse(s: str) -> tuple[float, float]:
        la, lo = (float(v) for v in s.replace(" ", "").split(","))
        return proj.to_world(la, lo)

    wps = [parse(a.a)] + [parse(v) for v in a.via] + [parse(a.b)]
    r = g.route(wps)
    if not r["ok"]:
        print("FAILED: %s" % r["reason"])
        return 1

    upm = place["projection"]["units_per_metre"]
    print("route: %d points, %.2f km, %d legs"
          % (len(r["points"]), r["length_units"] / upm / 1000.0, len(r["legs"])))
    print("streets: %s" % " -> ".join(r["street_names"][:14]))
    if len(r["street_names"]) > 14:
        print("         (+%d more)" % (len(r["street_names"]) - 14))

    if a.out:
        latlon = [proj.world_to_latlon(x, z) for x, z in r["points"]]
        write_json(a.out, {
            "place": place["slug"],
            "length_km": r["length_units"] / upm / 1000.0,
            "street_names": r["street_names"],
            "points": [{"lat": round(la, 7), "lon": round(lo, 7), "lanes": ln}
                       for (la, lo), ln in zip(latlon, r["lanes"])],
        })
        print("wrote %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
