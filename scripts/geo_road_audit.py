#!/usr/bin/env python3
"""geo_road_audit.py -- how much of the REAL road network inside the visible
corridor of a GEO track actually gets emitted as geometry.

geo_mouth_audit.py (round 1008b) asks a narrower question: for every real OSM
JUNCTION on the route, is there a mouth? That measures the junction table and
nothing else. Mariano's round-1009 note 6 ("not all roads near the track are
being rendered") is about the ROADS, not the junctions: a street can have a
correct mouth and still be a 49 m stub, and a street one block over -- plainly
visible across the gardens -- is never emitted at all because it never touches
the route.

So this audit measures LENGTH, in the corridor the camera can see:

  corridor      every real way clipped to |lateral| <= --corr of the route
                centre line (default 43000 units = 100 m, about one La Plata
                block; the far clip is 195000 = 453 m, so this is the
                conservative read)
  route itself  ways the conditioner routed along, by the same 6/10 test the
                engine uses (TG_GEO_ALONG_NUM/DEN) -- they are the carriageway,
                not a side street, and are excluded from both sides of the ratio
  emitted       NETWORK.JSON street/avenue/continuation edge polylines, clipped
                to the same corridor

and reports emitted/real as a percentage, plus the per-way table of what is
missing and the reason the engine would have given.

  python scripts/geo_road_audit.py --geo re/assets/geo/la_plata/_route \
         [--network re/assets/levels/level091/NETWORK.JSON] [--corr 43000]
         [--top 40] [--json out.json]

Exit code is always 0: this is a measurement, not a gate.
"""
import argparse, json, math, os

UNITS_PER_M = 430.0
SPAN_LENGTH = 1500.0
# TG_GEO_* in td5_tg_network.c, kept in sync by name so a knob change here is
# an explicit edit rather than a silent drift.
ALONG_NUM, ALONG_DEN = 6, 10
SAMPLE = 3000.0


def load_route(geo):
    r = json.load(open(os.path.join(geo, "ROUTE.JSON")))
    pts = r["points"]
    lw = float(r.get("lane_width", 1500))
    X = [float(p["x"]) for p in pts]
    Z = [float(p["z"]) for p in pts]
    W = [float(p.get("lanes", 2)) * lw for p in pts]
    n = len(pts)
    TX, TZ = [0.0] * n, [0.0] * n
    for i in range(n):
        a, b = max(i - 1, 0), min(i + 1, n - 1)
        dx, dz = X[b] - X[a], Z[b] - Z[a]
        ln = math.hypot(dx, dz) or 1.0
        TX[i], TZ[i] = dx / ln, dz / ln
    return X, Z, W, TX, TZ


class Route:
    """Nearest-node lookup with a uniform grid, so a 2300-way sweep against a
    1400-node route is not 3.2 M python distance calls per sample."""

    def __init__(self, geo):
        self.X, self.Z, self.W, self.TX, self.TZ = load_route(geo)
        self.n = len(self.X)
        self.cell = 20000.0
        self.g = {}
        for i in range(self.n):
            self.g.setdefault((int(self.X[i] // self.cell),
                               int(self.Z[i] // self.cell)), []).append(i)

    def nearest(self, x, z):
        cx, cz = int(x // self.cell), int(z // self.cell)
        best, bd = -1, 1e300
        rad = 1
        while True:
            for ix in range(cx - rad, cx + rad + 1):
                for iz in range(cz - rad, cz + rad + 1):
                    for i in self.g.get((ix, iz), ()):
                        d = (x - self.X[i]) ** 2 + (z - self.Z[i]) ** 2
                        if d < bd:
                            bd, best = d, i
            if best >= 0 or rad > 24:
                break
            rad += 1
        return best, math.sqrt(bd) if best >= 0 else 1e300

    def lat(self, i, x, z):
        return (x - self.X[i]) * self.TZ[i] - (z - self.Z[i]) * self.TX[i]


def iter_samples(pts, step=SAMPLE):
    """Walk a polyline at a fixed step, yielding (x, z, ux, uz)."""
    for k in range(len(pts) - 1):
        ax, az = pts[k]
        bx, bz = pts[k + 1]
        ln = math.hypot(bx - ax, bz - az)
        if ln < 1.0:
            continue
        ux, uz = (bx - ax) / ln, (bz - az) / ln
        t = 0.0
        while True:
            if t > ln:
                t = ln
            yield ax + ux * t, az + uz * t, ux, uz
            if t >= ln:
                break
            t += step


def corridor_length(pts, rt, corr):
    """Metres of this polyline inside the corridor, and the min |lateral|."""
    inside = 0.0
    best = 1e300
    prev = None
    for x, z, _ux, _uz in iter_samples(pts):
        ni, _d = rt.nearest(x, z)
        lat = abs(rt.lat(ni, x, z)) if ni >= 0 else 1e300
        lim = rt.W[ni] * 0.5 + corr if ni >= 0 else 0.0
        here = lat <= lim
        if here:
            best = min(best, lat)
            if prev is not None:
                inside += math.hypot(x - prev[0], z - prev[1])
        prev = (x, z) if here else None
    return inside / UNITS_PER_M, best


def is_route_way(pts, rt, width, skew_max_deg):
    """The engine's own 6/10 along test (tg_geo_road_hits)."""
    sin_lim = math.sin(math.radians(skew_max_deg))
    nsamp = nalong = 0
    for x, z, ux, uz in iter_samples(pts):
        ni, _d = rt.nearest(x, z)
        if ni < 0:
            continue
        sn = abs(ux * rt.TZ[ni] - uz * rt.TX[ni])
        lat = abs(rt.lat(ni, x, z))
        nsamp += 1
        if sn < sin_lim and lat < rt.W[ni] * 0.5 + width * 0.5 + 1500.0:
            nalong += 1
    return nsamp > 0 and nalong * ALONG_DEN >= nsamp * ALONG_NUM


def touches(pts, rt):
    """Does the way reach the carriageway? (a mouth is possible at all)
    Returns (touches, min_skew_deg_at_closest)."""
    best = 1e300
    skew = None
    for x, z, ux, uz in iter_samples(pts, step=SAMPLE * 0.5):
        ni, _d = rt.nearest(x, z)
        if ni < 0:
            continue
        lat = abs(rt.lat(ni, x, z))
        lim = rt.W[ni] * 0.5 + SPAN_LENGTH
        if lat - lim < best:
            best = lat - lim
            # skew from the OUTWARD NORMAL, which is what tg_geo_arm_push caps
            sn = abs(ux * rt.TZ[ni] - uz * rt.TX[ni])
            cs = abs(ux * rt.TX[ni] + uz * rt.TZ[ni])
            skew = math.degrees(math.atan2(cs, sn))
    return best <= 0.0, skew


def emitted_length(network, rt, corr):
    """Metres of emitted street polyline inside the corridor, by kind."""
    if not network or not os.path.exists(network):
        return {}, 0.0
    d = json.load(open(network))
    per = {}
    total = 0.0
    for e in d.get("edges") or []:
        kind = e.get("kind") or "?"
        poly = [(float(p[0]), float(p[1])) for p in (e.get("poly") or [])]
        if len(poly) < 2:
            continue
        ln, _lat = corridor_length(poly, rt, corr)
        per[kind] = per.get(kind, 0.0) + ln
        total += ln
    return per, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--geo", required=True, help="derived frame dir (_route)")
    ap.add_argument("--network")
    ap.add_argument("--corr", type=float, default=43000.0,
                    help="corridor half-width in world units (default 100 m)")
    ap.add_argument("--skew-max", type=float, default=65.0,
                    help="TD5RE_GEO_NET_SKEW_MAX_DEG the build used")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--json")
    a = ap.parse_args()

    rt = Route(a.geo)
    roads = json.load(open(os.path.join(a.geo, "ROADS.JSON")))["roads"]

    rows = []
    for rd in roads:
        pts = [(float(p["x"]), float(p["z"])) for p in (rd.get("points") or [])]
        if len(pts) < 2:
            continue
        ln_m, minlat = corridor_length(pts, rt, a.corr)
        if ln_m <= 0.0:
            continue
        width = float(rd.get("lanes", 2)) * 1500.0
        onroute = is_route_way(pts, rt, width, a.skew_max)
        tch, skew = touches(pts, rt)
        rows.append({"id": rd.get("id"), "name": rd.get("name") or "?",
                     "klass": rd.get("klass", 0), "lanes": rd.get("lanes", 0),
                     "len_m": round(ln_m, 1), "min_lat_m": round(minlat / UNITS_PER_M, 1),
                     "route": bool(onroute), "touches": bool(tch),
                     "skew_deg": round(skew, 1) if skew is not None else None})

    side = [r for r in rows if not r["route"]]
    real_m = sum(r["len_m"] for r in side)
    touch_m = sum(r["len_m"] for r in side if r["touches"])
    notouch = [r for r in side if not r["touches"]]
    notouch_m = sum(r["len_m"] for r in notouch)
    skewed = [r for r in side if r["touches"] and r["skew_deg"] is not None
              and r["skew_deg"] > a.skew_max]

    print("corridor: |lat| <= half-width + %.0f units (%.0f m)"
          % (a.corr, a.corr / UNITS_PER_M))
    print("real ways with geometry in the corridor: %d  (%d are the route itself)"
          % (len(rows), len(rows) - len(side)))
    print("real SIDE-road centreline in the corridor: %.0f m" % real_m)
    print("  on ways that REACH the carriageway (a mouth is possible): %.0f m  (%d way(s))"
          % (touch_m, len(side) - len(notouch)))
    print("  on ways that never touch it (no mechanism emits these): %.0f m  (%d way(s))"
          % (notouch_m, len(notouch)))
    print("  of the touching ways, refused by the %.0f deg skew cap: %d"
          % (a.skew_max, len(skewed)))

    per, emit_m = emitted_length(a.network, rt, a.corr)
    if a.network:
        print("emitted street geometry in the corridor: %.0f m  (%s)"
              % (emit_m, ", ".join("%s %.0f m" % (k, v) for k, v in sorted(per.items()))))
        if real_m > 0:
            print("COVERAGE: %.1f%% of the real side-road network in the corridor"
                  % (100.0 * emit_m / real_m))

    print("-- longest unreachable ways (no junction with the route) --")
    for r in sorted(notouch, key=lambda q: -q["len_m"])[:a.top]:
        print("  %7.0f m  lat %6.1f m  %s" % (r["len_m"], r["min_lat_m"], r["name"]))
    if skewed:
        print("-- touching ways over the skew cap --")
        for r in sorted(skewed, key=lambda q: -q["len_m"])[:a.top]:
            print("  %7.0f m  skew %5.1f deg  %s" % (r["len_m"], r["skew_deg"], r["name"]))

    if a.json:
        json.dump({"corr": a.corr, "rows": rows, "real_m": real_m,
                   "touch_m": touch_m, "notouch_m": notouch_m,
                   "emitted_m": emit_m, "emitted_per_kind": per},
                  open(a.json, "w"), indent=1)
        print("wrote %s" % a.json)


if __name__ == "__main__":
    main()
