#!/usr/bin/env python3
"""roam F9b: summary of a long roam drive from its log dir (roam_stream.csv, roam_rebase_trace.csv, race.log).

  python -I verify/roam/f9b_drive_report.py <run dir> [step_s]

Prints the private-bytes curve (one row every step_s seconds of wall time, default 30), the resident ROAD /
SCENERY tiles and MB, the stream counters at the end, the rebases (tick, shift, frame time) and the place-frame
continuity of slot 0 across them (f9b_rebase_cmp.py's check).
"""
import csv, math, os, re, sys


def main():
    d = sys.argv[1]
    step = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    rows = list(csv.DictReader(open(os.path.join(d, "roam_stream.csv"))))
    print("t_s   private_mb  road_tiles road_mb  scen_tiles scen_mb  loaded built  miss hold  frame_worst_ms")
    nxt = 0.0
    for r in rows:
        t = float(r["t_s"])
        if t + 1e-6 >= nxt or r is rows[-1]:
            print("%5.0f %10.1f %10s %7s %10s %7s %7s %5s %5s %4s %8s" % (t, float(r["private_mb"]), r["road_tiles"], r["road_mb"],
                  r["scen_tiles"], r["scen_mb"], r["loaded"], r["built"], r["misses"], r["holds"], r["frame_worst_ms"]))
            nxt = t + step
    pm = [float(r["private_mb"]) for r in rows]
    half = len(pm) // 2
    print("private MB: first %.1f min %.1f max %.1f last %.1f | second half min %.1f max %.1f (spread %.1f)"
          % (pm[0], min(pm), max(pm), pm[-1], min(pm[half:]), max(pm[half:]), max(pm[half:]) - min(pm[half:])))
    rt = os.path.join(d, "roam_rebase_trace.csv")
    if os.path.exists(rt):
        tr = list(csv.DictReader(open(rt)))
        nrb, worst_rb, worst = 0, 0.0, 0.0
        for a, b in zip(tr, tr[1:]):
            if int(b["tick"]) != int(a["tick"]) + 1:
                continue
            sx, sz = float(b["place_x"]) - float(a["place_x"]), float(b["place_z"]) - float(a["place_z"])
            vx, vz = float(a["vx"]) / 256.0, float(a["vz"]) / 256.0
            dev = math.hypot(sx - vx, sz - vz) / 430.0
            if int(b["rebased"]):
                nrb += 1
                worst_rb = max(worst_rb, dev)
            else:
                worst = max(worst, dev)
        mw = max(max(abs(int(r["world_x_fp"])), abs(int(r["world_z_fp"]))) for r in tr) / 256.0
        print("continuity: %d ticks, %d rebase ticks, worst place-frame step vs velocity on a rebase tick %.3f m, elsewhere %.3f m; "
              "max |world| %.0f units (%.0f m, 24.8 = %.0f of 2147483647)" % (len(tr), nrb, worst_rb, worst, mw, mw / 430.0, mw * 256))
    for name in ("engine.log", "race.log"):
        p = os.path.join(d, name)
        if not os.path.exists(p):
            continue
        for line in open(p, errors="replace"):
            if "ROAM_REBASE frame" in line or "ROAM_REBASE #" in line or "VERDICT" in line or "ROAM_STREAM summary" in line \
                    or "ROAM_FRAME_HITCH" in line or "ROAM_STREAM_MISS" in line or "ROAM_STREAM_HOLD" in line:
                print(line.rstrip()[:330])


if __name__ == "__main__":
    main()
