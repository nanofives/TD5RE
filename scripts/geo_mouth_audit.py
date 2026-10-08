#!/usr/bin/env python3
"""geo_mouth_audit.py -- diff a GEO track's NETWORK.JSON mouths against the real
OSM junctions that sit on the race route.

A real junction is a POINT SHARED BY TWO OR MORE OSM ways in ROADS.JSON. OSM
node ids are not in the cache, but two ways that meet at a node carry the exact
same projected (x,z), so an exact coordinate match recovers the node. A junction
counts as "on the route" when its nearest route node is within
(route half width + --lat) units laterally -- i.e. it is a junction a driver on
the route passes, not one a block away.

For each such junction the audit asks whether NETWORK.JSON opened a mouth within
--tol spans of it, and for each built mouth whether any real junction is that
close. The two failure modes have different names:

  MISSED    real junction, no mouth    -> a street Mariano cannot see
  INVENTED  mouth, no real junction    -> a street the engine made up

Exit code is 0 always: this is a measurement, not a gate. Compare two runs.

  python scripts/geo_mouth_audit.py re/assets/levels/level091/NETWORK.JSON \
         --geo re/assets/geo/la_plata [--tol 6] [--lat 3000] [--json out.json]
"""
import argparse, json, math, os, sys


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
        a = i - 1 if i > 0 else i
        b = i + 1 if i < n - 1 else i
        dx, dz = X[b] - X[a], Z[b] - Z[a]
        ln = math.hypot(dx, dz)
        if ln < 1e-6:
            dx, dz, ln = 0.0, 1.0, 1.0
        TX[i], TZ[i] = dx / ln, dz / ln
    return X, Z, W, TX, TZ


def nearest(X, Z, x, z, nspans):
    bd, best = 1e300, -1
    for j in range(nspans + 1):
        d = (x - X[j]) ** 2 + (z - Z[j]) ** 2
        if d < bd:
            bd, best = d, j
    return best


def junctions(geo):
    """{(xi,zi): [way, ...]} for every point shared by >= 2 distinct ways."""
    roads = json.load(open(os.path.join(geo, "ROADS.JSON")))["roads"]
    seen = {}
    for rd in roads:
        ids = set()
        for p in rd.get("points") or []:
            key = (round(float(p["x"])), round(float(p["z"])))
            if key in ids:            # a way may revisit its own node
                continue
            ids.add(key)
            seen.setdefault(key, []).append(rd)
    return {k: v for k, v in seen.items() if len(v) >= 2}


def mouths(net):
    d = json.load(open(net))
    out = []
    for e in d.get("edges") or []:
        m = e.get("mouth") or {}
        si = m.get("si", -1)
        if si is None or si < 0:
            continue
        out.append({"si": si, "left": m.get("left", 0), "lo": m.get("lo", -1),
                    "hi": m.get("hi", -1), "kind": e.get("kind"), "id": e.get("id")})
    out.sort(key=lambda q: (q["si"], q["left"]))
    return out, d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("network")
    ap.add_argument("--geo", required=True)
    ap.add_argument("--tol", type=int, default=6)
    ap.add_argument("--lat", type=float, default=3000.0)
    ap.add_argument("--json")
    a = ap.parse_args()

    X, Z, W, TX, TZ = load_route(a.geo)
    net = json.load(open(a.network))
    nspans = min(int(net.get("ring", len(X) - 1)), len(X) - 1)
    ms, _ = mouths(a.network)

    jn = []
    for (x, z), ways in sorted(junctions(a.geo).items()):
        ni = nearest(X, Z, x, z, nspans)
        lat = (x - X[ni]) * TZ[ni] - (z - Z[ni]) * TX[ni]
        if abs(lat) > W[ni] * 0.5 + a.lat:
            continue
        if ni < 1 or ni >= nspans:
            continue
        names = sorted({(w.get("name") or "?") for w in ways})
        jn.append({"si": ni, "lat": round(lat), "x": x, "z": z, "names": names,
                   "ways": [w["id"] for w in ways]})
    jn.sort(key=lambda q: q["si"])

    mspans = sorted({m["si"] for m in ms})
    hit, miss = [], []
    for j in jn:
        near = [s for s in mspans if abs(s - j["si"]) <= a.tol]
        (hit if near else miss).append(j)
    # An underpass is a way that passes OVER or UNDER the route, so it shares no
    # OSM node with it by construction and can never match a junction here. It
    # is also not a mouth a driver can turn into. Excluded from "invented".
    invented = [m for m in ms
                if m["kind"] != "underpass"
                and not any(abs(m["si"] - j["si"]) <= a.tol for j in jn)]

    print("route %s: %d span(s); %d mouth(s) on %d span(s)"
          % (os.path.basename(os.path.dirname(a.network)), nspans,
             len(ms), len(mspans)))
    print("real OSM junctions on the route (|lat| <= half-width + %.0f): %d"
          % (a.lat, len(jn)))
    print("  with a mouth within %d span(s): %d" % (a.tol, len(hit)))
    print("  MISSED (no mouth):              %d" % len(miss))
    print("  INVENTED (mouth, no junction):  %d" % len(invented))
    print("-- MISSED --")
    for j in miss:
        print("  span %-5d lat %-7d %s" % (j["si"], j["lat"], ", ".join(j["names"])))
    print("-- INVENTED --")
    for m in invented:
        print("  span %-5d left=%d kind=%s" % (m["si"], m["left"], m["kind"]))
    print("-- MOUTH SPANS --")
    print("  " + " ".join(str(s) for s in mspans))

    if a.json:
        json.dump({"nspans": nspans, "mouths": ms, "mouth_spans": mspans,
                   "junctions": jn, "missed": miss, "invented": invented},
                  open(a.json, "w"), indent=1)
        print("wrote %s" % a.json)


if __name__ == "__main__":
    main()
