"""geo_peer_direction.py -- does the real OSM way beside the route flow WITH or
AGAINST the race direction? (round 1013 F2: the wrong-way question for a
driveable avenue fork.)

  python verify/geo_peer_direction.py re/assets/geo/la_plata/_route "Diagonal 73"

For every segment of ROUTE_RAW.JSON (the routed polyline, lat/lon) it lists the
same-name ways within 4..45 m, on which side of travel they lie, and whether their
OSM one-way direction (`oneway_dir`, +1 = along the way's own vertex order) runs
WITH (< 60 deg) or AGAINST (> 120 deg) the route bearing. OSM left/right here is
the MAP's: the generator's `off` sign is the opposite (see
reference_geo_frame_handedness_osm_vs_engine), so a peer on the map's left is a
negative `off`.

MEASURED on Mariano's route (Diagonal 73): the carriageway beside the route is on the
map's LEFT at ~10.7 m and flows AGAINST the route -- the ordinary opposite
carriageway of a divided avenue in right-hand traffic. Racing it as a fork is
therefore driving against the OSM one-way flow.
"""
import json
import math
import sys


def main():
    route_dir = sys.argv[1] if len(sys.argv) > 1 else "re/assets/geo/la_plata/_route"
    names = sys.argv[2:] or ["Diagonal 73"]
    rr = json.load(open(route_dir + "/ROUTE_RAW.JSON"))
    roads = json.load(open(route_dir + "/ROADS.JSON"))["roads"]
    proj = json.load(open(route_dir + "/ROUTE.JSON"))["projection"]
    lat0, lon0 = proj["lat0"], proj["lon0"]
    mlat, mlon = proj["m_per_deg_lat"], proj["m_per_deg_lon"]

    def xy(p):
        return ((p[1] - lon0) * mlon, (p[0] - lat0) * mlat)

    R = [xy((q["lat"], q["lon"])) for q in rr["points"]]

    def brg(a, b):
        return math.degrees(math.atan2(b[0] - a[0], b[1] - a[1])) % 360

    def dd(a, b):
        d = abs(a - b) % 360
        return d if d <= 180 else 360 - d

    def near(poly, p):
        best = (1e18, None, None)
        for i in range(len(poly) - 1):
            a, b = poly[i], poly[i + 1]
            dx, dy = b[0] - a[0], b[1] - a[1]
            L = dx * dx + dy * dy
            t = 0 if L == 0 else max(0, min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / L))
            c = (a[0] + t * dx, a[1] + t * dy)
            d = math.hypot(p[0] - c[0], p[1] - c[1])
            if d < best[0]:
                best = (d, brg(a, b), c)
        return best

    named = {}
    for r in roads:
        if r.get("name") and r.get("latlon") and len(r["latlon"]) >= 2:
            named.setdefault(r["name"], []).append(r)

    for nm in names:
        print("==", nm, len(named.get(nm, [])), "ways")
        tally = {"WITH": 0, "AGAINST": 0, "x": 0}
        for k in range(len(R) - 1):
            rb = brg(R[k], R[k + 1])
            mid = ((R[k][0] + R[k + 1][0]) / 2, (R[k][1] + R[k + 1][1]) / 2)
            lx, ly = -math.cos(math.radians(rb)), math.sin(math.radians(rb))   # map-left of travel
            out = []
            for r in named.get(nm, []):
                poly = [xy(p) for p in r["latlon"]]
                d, pb, c = near(poly, mid)
                if d < 4 or d > 45:
                    continue
                flow = pb if r.get("oneway_dir", 1) == 1 else (pb + 180) % 360
                side = "L" if ((c[0] - mid[0]) * lx + (c[1] - mid[1]) * ly) > 0 else "R"
                rel = "WITH" if dd(flow, rb) < 60 else ("AGAINST" if dd(flow, rb) > 120 else "x")
                out.append((round(d, 1), side, rel, r["id"]))
                tally[rel] += 1
            if out:
                print(k, round(rb), out)
        print("tally of (segment, peer) pairs:", tally)


if __name__ == "__main__":
    main()
