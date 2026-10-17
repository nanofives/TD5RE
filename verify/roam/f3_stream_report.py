#!/usr/bin/env python3
"""roam F3-A: summarise one streamed run's log dir (roam_stream.csv + roam_stream.txt +
the ROAM_* log lines): private-bytes curve, resident counts, builds, misses, frame times.

  python -I verify/roam/f3_stream_report.py <run dir> [--every 60] [--warm 60]

The memory curve is sampled every --every seconds; "after warm-up" = t >= --warm.
"""
import argparse, csv, os, re, sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--every", type=float, default=60.0)
    ap.add_argument("--warm", type=float, default=60.0)
    a = ap.parse_args()
    rows = list(csv.DictReader(open(os.path.join(a.run, "roam_stream.csv"))))
    if not rows:
        print("no rows")
        return 1
    f = lambda r, k: float(r[k])
    print("t_s   private_MB  road(tiles/MB)  scenery(tiles/MB)  loaded built misses holds")
    nxt = 0.0
    for r in rows:
        if f(r, "t_s") >= nxt or r is rows[-1]:
            print("%5.0f  %9.1f  %4d / %5.2f    %4d / %6.2f      %5d %5d %6d %5d" % (
                f(r, "t_s"), f(r, "private_mb"), int(r["road_tiles"]), f(r, "road_mb"), int(r["scen_tiles"]),
                f(r, "scen_mb"), int(r["loaded"]), int(r["built"]), int(r["misses"]), int(r["holds"])))
            nxt += a.every
    warm = [r for r in rows if f(r, "t_s") >= a.warm]
    if warm:
        pm = [f(r, "private_mb") for r in warm]
        print("after %.0f s: private MB min %.1f max %.1f (spread %.1f), first %.1f last %.1f, slope %.3f MB/min" % (
            a.warm, min(pm), max(pm), max(pm) - min(pm), pm[0], pm[-1],
            (pm[-1] - pm[0]) / max(1e-9, (f(warm[-1], "t_s") - f(warm[0], "t_s")) / 60.0)))
        rt = [int(r["road_tiles"]) for r in warm]
        st = [int(r["scen_tiles"]) for r in warm]
        print("resident after warm-up: road %d..%d tiles, scenery %d..%d tiles" % (min(rt), max(rt), min(st), max(st)))
    txt = os.path.join(a.run, "roam_stream.txt")
    if os.path.exists(txt):
        print("--- roam_stream.txt")
        print(open(txt).read().strip())
    pat = re.compile(r"ROAM_STREAM_GATE|ROAM_SESSION|ROAM_STREAM_MISS|ROAM_STREAM_HOLD|ROAM_STREAM_STALE|ROUTE VERDICT|standalone verdict")
    builds = []
    for name in sorted(os.listdir(a.run)):
        if not name.endswith(".log"):
            continue
        for line in open(os.path.join(a.run, name), errors="replace"):
            if pat.search(line):
                print(line.rstrip()[:400])
            m = re.search(r"ROAM_STREAM_BUILD tile (-?\d+),(-?\d+) (\d+) ms", line)
            if m:
                builds.append(int(m.group(3)))
    if builds:
        builds.sort()
        print("builds on the fly: %d, ms median %d p95 %d max %d" % (
            len(builds), builds[len(builds) // 2], builds[min(len(builds) - 1, int(0.95 * len(builds)))], builds[-1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
