"""geo_lf_trace.py -- [1019 V] print one car's path through a stretch of a harness run:
tick, span_raw/norm, contact flag, lateral position in half-widths of the span the walker says it is on
(and of the main span `norm` when the car is on a corridor), speed, steering, rear slip.

  python verify/geo_lf_trace.py verify/out/<tag>/fork_s11 <slot> <tick0> <tick1>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_fork_validate as V  # noqa: E402


def main():
    path, slot, t0, t1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    run = V.Run(path)
    rows = {t: (r, n, c) for t, r, n, c in run.track.get(slot, [])}
    drv = {t: s for t, s in ((t, v[0]) for t, *v in run.drv.get(slot, []))}
    ms = {t: (v, s) for t, v, s in run.motion.get(slot, [])}
    print("tick  raw  norm  cont  lat_raw  lat_norm   spd  steer  rslip   dh")
    for t in range(t0, t1 + 1):
        if t not in rows:
            continue
        raw, norm, cont = rows[t]
        lr = V.lateral(run, slot, t, raw)
        ln = V.lateral(run, slot, t, norm)
        print("%5d %4d %5d %5d  %7s  %8s  %5s %6s %6s  %5s" % (
            t, raw, norm, cont, "%.2f" % lr[0] if lr else "-", "%.2f" % ln[0] if ln else "-",
            "%.0f" % ms[t][0] if t in ms else "-", drv.get(t, "-"), "%d" % ms[t][1] if t in ms else "-",
            "%.0f" % lr[1] if lr else "-"))


if __name__ == "__main__":
    main()
