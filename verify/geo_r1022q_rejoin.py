"""geo_r1022q_rejoin.py -- what happens at a fork's REJOIN window (round 1022 Q).

  python verify/geo_r1022q_rejoin.py verify/out/<tag> [--arm fork] [--pre 24] [--post 14] [-v]

The fork harness (geo_fork_validate.ps1) judges the whole window F-8..R+2. A corridor car merging
back onto the road is a CAR-CAR conflict that the wall-contact counters do not see, so this reads
the same run-set and looks only at the rejoin window: main-road span_norm R-pre .. R+post (the
corridor's own cars carry the span_norm of the road beside them).

Per fork, summed over the seeds of the run-set:

  passes / via corr   cars that crossed the window / of those, cars that drove the corridor (raw >= ring) in it
  pairs / merge       distinct car pairs that came within PAIR_DIST of each other inside the window / of those, pairs
                      with one car on the corridor (or off it < MERGE_MEMORY ticks ago) and the other on the road
  hit / crash         a pair event after which either car lost >= 40 % (<= 15 %) of its speed within 25 ticks
                      (the same grading as the harness' wall incidents)
  stall / jam         plateau >= 90 / >= 300 ticks making <= 2 spans, inside the window
  spin                the harness' spin test (nose off the road or side-on slide), inside the window

A "merge hit" is the case the round-1021 A / 1020 N notes describe: a main-road car at ~600 u/t
struck by a corridor car joining at ~140..420 u/t.
"""
from __future__ import annotations

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_fork_validate as H   # noqa: E402

PAIR_DIST = 1500.0      # world units: V2V contact fires at ~1100, the half-diagonals sum to ~1500
PAIR_GAP = 60           # ticks: one pair's closeness closer than this is ONE event
MERGE_MEMORY = 40       # ticks after leaving the corridor that a car still counts as merging
PAIR_LOOK = 25
SNAP_TICKS = 8          # an impact takes its speed in this many ticks ...
SNAP_LOSS = 0.25        # ... this fraction of it (governor / yield braking is ~5 units/tick^2 = 0.1 per 8 ticks at 400)


def window_rows(run, slot, lo, hi):
    out = []
    for t, raw, norm, c in run.track.get(slot, []):
        a, b = run.window.get(slot, (0, 10**9))
        if t < a or t >= b:
            continue
        if lo <= norm <= hi:
            out.append((t, raw, norm, c))
    return out


def speed(run, slot, t):
    v = run.d_motion.get(slot, {}).get(t)
    return None if v is None else v / 256.0


def graded(run, a, b, t0, t1):
    """'crash' / 'hit' / None for the pair event [t0, t1]. A car-car IMPACT is a sudden loss: >= SNAP_LOSS of a
    car's speed inside SNAP_TICKS ticks while the pair is in contact range (a governor or a yield brakes at
    ~5 units/tick^2, an impact loses 15+). A crash is a car left at <= 15 % of its speed (or reversing)
    within PAIR_LOOK ticks of the event's end."""
    worst = None
    for s in (a, b):
        v0 = speed(run, s, t0 - 3)
        if v0 is None or v0 < H.HIT_MIN_SPEED:
            continue
        snap = 0.0
        for k0 in range(t0 - 3, t1 + 5):
            va, vb = speed(run, s, k0), speed(run, s, k0 + SNAP_TICKS)
            if va is not None and vb is not None and va >= H.HIT_MIN_SPEED:
                snap = max(snap, 1.0 - max(vb, 0.0) / va)
        vmin = min((speed(run, s, t1 + k) for k in range(0, PAIR_LOOK) if speed(run, s, t1 + k) is not None),
                   default=None)
        if vmin is None:
            continue
        if snap >= SNAP_LOSS and (vmin <= v0 * H.CRASH_FRAC or vmin < 0):
            return "crash", s, round(v0), round(vmin)
        if snap >= SNAP_LOSS and worst is None:
            worst = ("hit", s, round(v0), round(vmin))
    return worst if worst else (None, None, 0, 0)


def measure_rejoin(run, fk, pre, post):
    lo, hi = fk["R"] - pre, fk["R"] + post
    ring = run.ring
    m = dict(passes=0, via_corr=0, pairs=0, merge_pairs=0, hits=0, crashes=0, merge_hits=0, merge_crashes=0,
             stalls=[], jams=0, spins=[], log=[])
    rows = {s: window_rows(run, s, lo, hi) for s in run.slots}
    # the ticks a car was on the corridor (raw >= ring) within the rejoin window or just before it
    on_corr = {}
    for s in run.slots:
        ticks = set()
        for t, raw, norm, c in run.track.get(s, []):
            if raw >= ring and fk["R"] - pre - 40 <= norm <= hi:
                ticks.add(t)
        on_corr[s] = ticks
    for s in run.slots:
        seq = rows[s]
        if not seq:
            continue
        m["passes"] += 1
        if any(r >= ring for _t, r, _n, _c in seq):
            m["via_corr"] += 1
        for p in H.plateaus([(t, r, n, c) for t, r, n, c in seq], H.STALL_HOLD, H.STALL_TOL):
            m["stalls"].append((run.seed, s) + p)
            if p[1] - p[0] + 1 >= H.TRAP_TICKS:
                m["jams"] += 1
        in_spin = 0
        for t, raw, norm, c in seq:
            lf = H.lateral(run, s, t, raw)
            if not lf:
                continue
            lat, dh, spd, dn, nv = lf
            bad = abs(dn) > H.SPIN_BACK_DEG or (abs(nv) > H.SPIN_SLIDE_DEG and spd > H.SPIN_SLIDE_MIN_SPEED)
            if bad:
                in_spin += 1
                if in_spin == H.SPIN_MIN_TICKS:
                    m["spins"].append((run.seed, s, t, raw))
            else:
                in_spin = 0
    # car-car proximity events
    slots = [s for s in run.slots if rows[s]]
    for i, a in enumerate(slots):
        ta = {t: (r, n) for t, r, n, _c in rows[a]}
        for b in slots[i + 1:]:
            tb = {t: (r, n) for t, r, n, _c in rows[b]}
            last = -10**9
            ev = None
            for t in sorted(set(ta) & set(tb)):
                pa, pb = run.d_pose.get(a, {}).get(t), run.d_pose.get(b, {}).get(t)
                if not pa or not pb:
                    continue
                d = math.hypot((pa[0] - pb[0]) / 256.0, (pa[1] - pb[1]) / 256.0)
                if d >= PAIR_DIST:
                    continue
                if t - last > PAIR_GAP:
                    if ev:
                        _close_pair(run, m, ev, a, b)
                    mrg = False
                    for s_, o_ in ((a, b), (b, a)):
                        if any((t - k) in on_corr[s_] for k in range(0, MERGE_MEMORY + 1)) and \
                           not any((t - k) in on_corr[o_] for k in range(0, MERGE_MEMORY + 1)):
                            mrg = True
                    ev = dict(t0=t, t1=t, raw=(ta[t][0], tb[t][0]), norm=ta[t][1], merge=mrg, dmin=d)
                else:
                    ev["t1"] = t
                    ev["dmin"] = min(ev["dmin"], d)
                last = t
            if ev:
                _close_pair(run, m, ev, a, b)
    return m


def _close_pair(run, m, ev, a, b):
    m["pairs"] += 1
    kind, who, v0, vmin = graded(run, a, b, ev["t0"], ev["t1"])
    if ev["merge"]:
        m["merge_pairs"] += 1
    if kind:
        m["hits"] += 1
        if ev["merge"]:
            m["merge_hits"] += 1
        if kind == "crash":
            m["crashes"] += 1
            if ev["merge"]:
                m["merge_crashes"] += 1
    m["log"].append((run.seed, ev["t0"], a, b, ev["raw"], ev["norm"], "M" if ev["merge"] else "-", kind or "-", who, v0, vmin,
                     round(ev["dmin"])))


def add(dst, src):
    for k, v in src.items():
        if isinstance(v, list):
            dst.setdefault(k, []).extend(v)
        else:
            dst[k] = dst.get(k, 0) + v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("setdir")
    ap.add_argument("--arm", default="fork")
    ap.add_argument("--pre", type=int, default=24)
    ap.add_argument("--post", type=int, default=14)
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--forks", default="", help="comma list of F values to report (default all)")
    args = ap.parse_args()
    runs = []
    for d in sorted(os.listdir(args.setdir)):
        p = os.path.join(args.setdir, d)
        if os.path.isdir(p) and d.startswith(args.arm + "_s") and os.path.exists(os.path.join(p, "log", "race_trace_track.csv")):
            runs.append(H.Run(p))
    if not runs:
        print("no %s_s* runs in %s" % (args.arm, args.setdir))
        return 2
    forks, ring = {}, None
    for r in runs:
        fs, rg = H.read_forks(os.path.join(r.path, "log", "race.log"))
        for F, fk in fs.items():
            forks.setdefault(F, fk)
        ring = ring or rg
    for r in runs:
        r.ring = ring
        r.set_finish(ring)
    want = {int(x) for x in args.forks.split(",") if x.strip()}
    print("run-set %s arm %s: %d runs %s ring %d window R-%d..R+%d" %
          (args.setdir, args.arm, len(runs), [r.seed for r in runs], ring, args.pre, args.post))
    head = "%-22s %-9s %6s %8s %6s %6s %5s %6s %6s %5s %4s %5s" % (
        "fork", "F..R", "passes", "via corr", "pairs", "merge", "hit", "crash", "mhit", "stall", "jam", "spin")
    print(head)
    tot = {}
    for F, fk in sorted(forks.items()):
        if want and F not in want:
            continue
        agg = {}
        for r in runs:
            add(agg, measure_rejoin(r, fk, args.pre, args.post))
        add(tot, agg)
        print("%-22s %-9s %6d %8d %6d %6d %5d %6d %6d %5d %4d %5d" % (
            (fk["name"] or fk["kind"])[:22], "%d..%d" % (F, fk["R"]), agg["passes"], agg["via_corr"], agg["pairs"],
            agg["merge_pairs"], agg["hits"], agg["crashes"], agg["merge_hits"], len(agg["stalls"]), agg["jams"],
            len(agg["spins"])))
        if args.v:
            for ev in sorted(agg["log"], key=lambda e: (e[0], e[1])):
                print("    s%-3d t%-5d slots %d,%d raw %s norm %d %s %s(slot %s) v0 %d vmin %d dmin %d" % (
                    ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7], ev[8], ev[9], ev[10], ev[11]))
            for s_ in agg["stalls"]:
                print("    stall s%d slot %d ticks %d..%d spans %d..%d" % s_)
            for s_ in agg["spins"]:
                print("    spin  s%d slot %d tick %d raw %d" % s_)
    if tot:
        print("%-22s %-9s %6d %8d %6d %6d %5d %6d %6d %5d %4d %5d" % (
            "TOTAL", "", tot["passes"], tot["via_corr"], tot["pairs"], tot["merge_pairs"], tot["hits"], tot["crashes"],
            tot["merge_hits"], len(tot["stalls"]), tot["jams"], len(tot["spins"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
