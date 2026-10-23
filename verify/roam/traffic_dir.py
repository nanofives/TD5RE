#!/usr/bin/env python3
"""roam QD: offline check of free-roam traffic DIRECTION against the raw OSM ways.

Input: log/roam_traffic_dir.csv written by a dev td5re.exe run with TD5RE_ROAM_TRAFFIC_DIRCSV=1 (one sample per live
traffic car per second: place x,z in the GAME frame, x = -east, and the unit velocity), plus the place's
tiles/ROADS_*.JSON (SOURCE frame, x = east, place units 430 per metre).

Every sample is snapped to the nearest drivable OSM way segment within 6 m. A sample is NOT judged when another way's
segment lies within 10 m (a junction: two streets meet, the snap is ambiguous) or when the car is crossing the way
(|cos| < 0.8). Judged samples:
  * against: the way is one-way (oneway tag, or junction=roundabout, which implies it) and the car drives against
    its point order (OSM point order is the legal direction, oneway_dir in the data is +1 everywhere here).
  * wrong side: a two-way way and the car's centre is more than 0.3 m LEFT of the way's centre line (Argentina
    drives on the right). Skipped where the way is half of a divided pair (oneway) or a roundabout.
Each sample stands for one car-second.

Usage: python -I verify/roam/traffic_dir.py <roam_traffic_dir.csv> <place dir> [--list N]
"""
import csv
import glob
import json
import math
import os
import sys
from collections import Counter, defaultdict

UPM = 430.0
CELL = 30.0 * UPM
DRIVE = {"motorway", "motorway_link", "trunk", "trunk_link", "primary", "primary_link", "secondary", "secondary_link",
         "tertiary", "tertiary_link", "unclassified", "residential", "living_street", "service", "road"}


def load_ways(place):
    ways = {}
    for f in glob.glob(os.path.join(place, "tiles", "ROADS_*.JSON")):
        with open(f, encoding="utf-8") as fh:
            for r in json.load(fh)["roads"]:
                if r["id"] in ways or r.get("class") not in DRIVE or len(r["points"]) < 2:
                    continue
                ways[r["id"]] = r
    return ways


def build_grid(ways):
    grid = defaultdict(list)
    for wid, r in ways.items():
        P = r["points"]
        for i in range(len(P) - 1):
            ax, az, bx, bz = P[i]["x"], P[i]["z"], P[i + 1]["x"], P[i + 1]["z"]
            for cx in range(int(math.floor(min(ax, bx) / CELL)) - 1, int(math.floor(max(ax, bx) / CELL)) + 2):
                for cz in range(int(math.floor(min(az, bz) / CELL)) - 1, int(math.floor(max(az, bz) / CELL)) + 2):
                    grid[(cx, cz)].append((wid, i))
    return grid


def seg_proj(ax, az, bx, bz, px, pz):
    dx, dz = bx - ax, bz - az
    l2 = dx * dx + dz * dz
    t = 0.0 if l2 <= 0 else max(0.0, min(1.0, ((px - ax) * dx + (pz - az) * dz) / l2))
    qx, qz = ax + t * dx, az + t * dz
    return math.hypot(px - qx, pz - qz), qx, qz, dx, dz


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, place = sys.argv[1], sys.argv[2]
    nlist = int(sys.argv[sys.argv.index("--list") + 1]) if "--list" in sys.argv else 10
    ways = load_ways(place)
    grid = build_grid(ways)
    c = Counter()
    bad = []
    by_way = Counter()
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            gx, gz = float(row["x"]), float(row["z"])
            sx, sz = -gx, gz                          # game -> source frame
            vx, vz = -float(row["vx"]), float(row["vz"])
            c["samples"] += 1
            cands = []
            seen = set()
            for (wid, i) in grid.get((int(math.floor(sx / CELL)), int(math.floor(sz / CELL))), ()):
                if (wid, i) in seen:
                    continue
                seen.add((wid, i))
                P = ways[wid]["points"]
                d, qx, qz, dx, dz = seg_proj(P[i]["x"], P[i]["z"], P[i + 1]["x"], P[i + 1]["z"], sx, sz)
                cands.append((d, wid, qx, qz, dx, dz))
            if not cands:
                c["no_way"] += 1
                continue
            cands.sort()
            d, wid, qx, qz, dx, dz = cands[0]
            if d > 6.0 * UPM:
                c["no_way_6m"] += 1
                continue
            if any(o[1] != wid and o[0] < 10.0 * UPM for o in cands[1:]):
                c["junction"] += 1
                continue
            l = math.hypot(dx, dz)
            if l <= 0:
                continue
            tx, tz = dx / l, dz / l
            cos = vx * tx + vz * tz
            if abs(cos) < 0.8:
                c["crossing"] += 1
                continue
            r = ways[wid]
            one = bool(r["oneway"]) or bool(r["roundabout"])
            c["judged"] += 1
            kind = "rb" if r["roundabout"] else ("oneway" if r["oneway"] else "twoway")
            c["judged_" + kind] += 1
            if one:
                legal = cos * (r.get("oneway_dir") or 1) > 0
                if not legal:
                    c["against_" + kind] += 1
                    by_way[(wid, r.get("name"), kind)] += 1
                    bad.append(("against", wid, r.get("name"), r["class"], row["slot"], row["tick"], gx, gz))
            else:
                ux, uz = (tx, tz) if cos > 0 else (-tx, -tz)
                lat = ((sx - qx) * uz + (sz - qz) * -ux) / UPM    # + = right of travel, east/north frame: right = (uz, -ux)
                if lat < -0.3:
                    c["wrong_side"] += 1
                    by_way[(wid, r.get("name"), "wrongside")] += 1
                    bad.append(("wrong-side %.1f m" % lat, wid, r.get("name"), r["class"], row["slot"], row["tick"], gx, gz))
    print("samples (car-s) %d | judged %d (two-way %d, one-way %d, roundabout %d) | not judged: junction %d, crossing %d, "
          "no way within 6 m %d" % (c["samples"], c["judged"], c["judged_twoway"], c["judged_oneway"], c["judged_rb"],
                                    c["junction"], c["crossing"], c["no_way"] + c["no_way_6m"]))
    print("AGAINST a one-way: %d car-s (tagged one-way %d, roundabout %d) | WRONG SIDE of a two-way: %d car-s"
          % (c["against_oneway"] + c["against_rb"], c["against_oneway"], c["against_rb"], c["wrong_side"]))
    for (wid, name, kind), n in by_way.most_common(nlist):
        print("  way %d %-28s %-9s %d car-s" % (wid, name, kind, n))
    for b in bad[:nlist]:
        print("  sample", b)
    return 0


if __name__ == "__main__":
    sys.exit(main())
