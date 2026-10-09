"""geo_realfork_report.py -- judge a geo_realfork_run.ps1 race (round 1013 F2).

  python verify/geo_realfork_report.py log/rf_after1  [--windows log/rf_after1]

Reads <prefix>_race_trace_track.csv and, for the fork table, <windows>_race.log
(defaults to the same prefix). Reports, per run:

  * FORK ENTRIES per fork: how many cars drove the corridor (span_raw inside the
    corridor's appended span range), and which slots;
  * STALLS: plateaus (>= --hold ticks, <= 2 spans of progress) BEFORE the car
    first reaches the finish area. Rows after that are the car PARKED at the end
    of a point-to-point route and are excluded: parked rows fake wall contacts;
  * WALL CONTACTS per segment: contact EVENTS (track_contact_flag 0 -> non-zero),
    for the ring segments around each fork window, the corridor itself, and the
    rest of the route. Segments are defined from the fork table, so a "before"
    trace (no forks, same route) is binned over the SAME spans as the "after".

track_contact_flag: 0 none, 1 wall, 2 edge, 3 both (td5_physics_collision.c:435).
"""
from __future__ import annotations

import argparse
import collections
import csv
import os
import re
import sys

WIDEN = 8          # TD5_TG_BRANCH_WIDEN + 2: the approach a fork window keeps
FINISH_AREA = 940  # span_norm at/after which a car is at the end of the route
INCIDENT_GAP = 90  # ticks: one car's contacts closer than this are one incident


def read_forks(log_path):
    """[(i, F, len, R, c0, c1)] from the strip's own fork lines."""
    forks = []
    pat = re.compile(r"trackgen: fork (\d+) \w+ F=(\d+) len=(\d+) corridor=(\d+)\.\.(\d+) rejoin=(\d+)")
    if not os.path.exists(log_path):
        return forks
    seen = set()
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                i, F, L, c0, c1, R = map(int, m.groups())
                if i in seen:
                    continue
                seen.add(i)
                forks.append((i, F, L, R, c0, c1))
    return forks


def read_track(path):
    out = collections.defaultdict(list)    # slot -> [(tick, raw, norm, contact)]
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if r.get("stage") != "post_track":
                continue
            try:
                out[int(r["slot"])].append((int(r["sim_tick"]), int(r["span_raw"]),
                                            int(r["span_norm"]), int(r["track_contact"])))
            except (TypeError, ValueError):
                continue
    return out


def plateaus(seq, hold, tol=2):
    out = []
    i, n = 0, len(seq)
    while i < n:
        lo = hi = seq[i][2]
        j = i + 1
        while j < n:
            lo2, hi2 = min(lo, seq[j][2]), max(hi, seq[j][2])
            if hi2 - lo2 > tol:
                break
            lo, hi = lo2, hi2
            j += 1
        if j - i >= hold:
            out.append((seq[i][0], seq[j - 1][0], lo, hi))
            i = j
        else:
            i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--windows", default=None,
                    help="prefix whose race.log carries the fork table to bin by")
    ap.add_argument("--hold", type=int, default=90)
    ap.add_argument("--ring", type=int, default=951)
    args = ap.parse_args()

    tr = read_track(args.prefix + "_race_trace_track.csv")
    if not tr:
        print("no track trace at " + args.prefix + "_race_trace_track.csv")
        return 2
    forks_here = read_forks(args.prefix + "_race.log")
    wpre = args.windows or args.prefix
    forks_win = read_forks(wpre + "_race.log") or forks_here
    print("run %s: slots %s, forks in THIS run: %d, windows from %s: %d"
          % (args.prefix, sorted(tr), len(forks_here), wpre, len(forks_win)))

    # --- per slot: the moving, pre-finish window -------------------------------
    win = {}
    for slot, seq in tr.items():
        t0 = next((t for t, _r, n, _c in seq if n != seq[0][2]), seq[0][0])
        t1 = next((t for t, _r, n, _c in seq if n >= FINISH_AREA), seq[-1][0] + 1)
        win[slot] = (t0, t1)

    # --- fork entries ----------------------------------------------------------
    print("\nFORK ENTRIES (cars that drove the corridor, before the finish area)")
    for (i, F, L, R, c0, c1) in forks_here:
        took, stayed = [], []
        for slot, seq in sorted(tr.items()):
            a, b = win[slot]
            inside = False
            ent = 0
            for t, raw, norm, _c in seq:
                if t < a or t >= b:
                    continue
                now = c0 <= raw <= c1
                if now and not inside:
                    ent += 1
                inside = now
            passed = any(a <= t < b and F + 1 <= norm <= F + L for t, _r, norm, _c in seq)
            if ent:
                took.append((slot, ent))
            elif passed:
                stayed.append(slot)
        print("  fork %d  F=%d len=%d R=%d corridor %d..%d : entries %d  taken by %s  stayed main %s"
              % (i, F, L, R, c0, c1, sum(e for _s, e in took),
                 [s for s, _e in took], stayed))
    if not forks_here:
        print("  (no forks in this run)")

    # --- stalls ----------------------------------------------------------------
    print("\nSTALLS (plateau >= %d ticks / <= 2 spans, moving + pre-finish only)" % args.hold)
    stalls = 0
    for slot, seq in sorted(tr.items()):
        a, b = win[slot]
        sub = [x for x in seq if a <= x[0] < b]
        reached = max((n for _t, _r, n, _c in sub), default=0)
        pl = plateaus(sub, args.hold)
        stalls += len(pl)
        print("  slot %d: ticks %d..%d, max span %d%s" % (slot, a, b - 1, reached,
              "" if not pl else "   PLATEAUS " + "; ".join(
                  "ticks %d..%d spans %d..%d" % p for p in pl)))

    # --- wall contacts per segment --------------------------------------------
    segs = []   # (name, kind, lo, hi) over span_norm, or corridor over span_raw
    prev_end = 0
    for (i, F, L, R, c0, c1) in sorted(forks_win, key=lambda x: x[1]):
        lo, hi = F - WIDEN, R + 2
        if lo > prev_end:
            segs.append(("route %d..%d" % (prev_end, lo - 1), "ring", prev_end, lo - 1))
        segs.append(("fork %d window %d..%d (ring)" % (i, lo, hi), "ring", lo, hi))
        segs.append(("fork %d corridor %d..%d" % (i, c0, c1), "corr", c0, c1))
        prev_end = hi + 1
    segs.append(("route %d..%d" % (prev_end, args.ring), "ring", prev_end, args.ring))

    print("\nWALL/EDGE CONTACT EVENTS per segment (0 -> non-zero), summed over all cars")
    print("  (events count every bounce; INCIDENTS merge one car's contacts that are <= %d "
          "ticks apart, so one stuck car is ONE incident, not 500 events)" % INCIDENT_GAP)
    tot_ev = tot_tk = tot_inc = 0
    for name, kind, lo, hi in segs:
        ev = tk = inc = 0
        for slot, seq in tr.items():
            a, b = win[slot]
            prev = 0
            last_t = -10**9
            for t, raw, norm, c in seq:
                if t < a or t >= b:
                    prev = 0
                    continue
                if kind == "corr":
                    here = lo <= raw <= hi
                else:
                    here = (raw < args.ring) and lo <= norm <= hi
                if not here:
                    prev = 0
                    continue
                if c != 0:
                    tk += 1
                    if prev == 0:
                        ev += 1
                    if t - last_t > INCIDENT_GAP:
                        inc += 1
                    last_t = t
                prev = c
        tot_ev += ev
        tot_tk += tk
        tot_inc += inc
        print("  %-38s events %5d  incidents %4d  contact-ticks %6d" % (name, ev, inc, tk))
    print("  %-38s events %5d  incidents %4d  contact-ticks %6d"
          % ("TOTAL", tot_ev, tot_inc, tot_tk))
    print("\nSUMMARY stalls=%d contact_events=%d contact_incidents=%d"
          % (stalls, tot_ev, tot_inc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
