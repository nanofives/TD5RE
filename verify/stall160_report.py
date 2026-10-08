"""stall160_report.py -- judge a stall160_run.ps1 race from its trace CSVs.

The question this round exists to answer is "does every car get through the
span 123..200 section", so this reports, per slot:

  * the span reached, and the sim tick each slot first passed span 200,
  * every PLATEAU (a window of >= --hold ticks whose span range is <= 2), which
    is what "the AI piles up and stops" looks like in the data,
  * how many ticks the car spent inside the racer-unstick steer-around latch
    (fingerprinted on its exact command triple: |steer| == 0x12000, throttle in
    {0x18, 0x50}, brake == 0) and at the +-0x18000 steering saturation clamp.

  python verify/stall160_report.py log/s160_base --hold 120
"""
from __future__ import annotations

import argparse
import collections
import csv
import os
import sys

LATCH_STEER = 0x12000
LATCH_THR = (0x18, 0x50)
SAT_STEER = 0x18000


def read_track(path):
    spans = collections.defaultdict(list)   # slot -> [(tick, span)]
    if not os.path.exists(path):
        return spans
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if r.get("stage") != "post_track":
                continue
            try:
                spans[int(r["slot"])].append((int(r["sim_tick"]), int(r["span_raw"])))
            except (TypeError, ValueError):
                continue
    return spans


def read_driver(path):
    rows = collections.defaultdict(list)    # slot -> [(tick, steer, thr, brake, v)]
    if not os.path.exists(path):
        return rows
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if r.get("stage") != "post_ai":
                continue
            try:
                rows[int(r["slot"])].append((int(r["sim_tick"]), int(r["steering_cmd"]),
                                             int(r["throttle_cmd"]), int(r["brake_flag"]),
                                             int(r["long_speed"])))
            except (TypeError, ValueError):
                continue
    return rows


def plateaus(seq, hold, tol=2):
    """Maximal windows of >= `hold` consecutive samples spanning <= `tol` spans."""
    out = []
    i = 0
    n = len(seq)
    while i < n:
        j = i + 1
        lo = hi = seq[i][1]
        while j < n:
            lo2, hi2 = min(lo, seq[j][1]), max(hi, seq[j][1])
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
    ap.add_argument("prefix", help="e.g. log/s160_base (expects _race_trace_*.csv)")
    ap.add_argument("--hold", type=int, default=120, help="ticks a plateau must last")
    ap.add_argument("--goal", type=int, default=200, help="span every car must pass")
    ap.add_argument("--min-ticks", type=int, default=3000,
                    help="a run shorter than this is INCONCLUSIVE, not FAIL: the "
                         "wall-clock kill can cut a healthy race off before the "
                         "cars reach the goal (seen at 793 ticks vs a twin's 5241)")
    args = ap.parse_args()

    tr = read_track(args.prefix + "_race_trace_track.csv")
    dv = read_driver(args.prefix + "_race_trace_driver.csv")
    if not tr:
        print("no track trace at " + args.prefix + "_race_trace_track.csv")
        return 2

    last_tick = max(t for s in tr.values() for t, _ in s)
    print("ticks traced: %d   slots: %s" % (last_tick, sorted(tr)))
    ok = True
    for slot in sorted(tr):
        seq = tr[slot]
        reached = max(sp for _, sp in seq)
        passed = next((t for t, sp in seq if sp >= args.goal), None)
        latch = sat = 0
        for _t, st, th, br, _v in dv.get(slot, []):
            if abs(st) == LATCH_STEER and th in LATCH_THR and br == 0:
                latch += 1
            if abs(st) >= SAT_STEER:
                sat += 1
        n = max(len(dv.get(slot, [])), 1)
        if passed is None:
            ok = False
        print("slot %d: max span %4d  passed %d at tick %-6s  unstick-latch %5d (%4.1f%%)"
              "  full-lock %5d (%4.1f%%)"
              % (slot, reached, args.goal, passed if passed is not None else "NEVER",
                 latch, 100.0 * latch / n, sat, 100.0 * sat / n))
        for a, b, lo, hi in plateaus(seq, args.hold):
            print("        PLATEAU ticks %6d..%-6d (%5d) spans %d..%d"
                  % (a, b, b - a + 1, lo, hi))
    if last_tick < args.min_ticks:
        print("VERDICT: INCONCLUSIVE (only %d ticks traced, need %d -- run was "
              "cut off, not a stall)" % (last_tick, args.min_ticks))
        return 2
    print("VERDICT: %s (every slot past span %d)"
          % ("PASS" if ok else "FAIL", args.goal))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
