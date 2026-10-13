"""r1028s_pace.py -- round 1028 S: compare AI PACE between two r1028s_run.py run sets.

    python -I verify/r1028s_pace.py verify/out/r1028s_base verify/out/r1028s_fix

For every run directory present in BOTH sets it reads race_trace_track.csv and, at the
last sim tick the two runs have in common, reports each arm's track progress
(span_accum, the per-slot cumulative span counter) for the leader and for the field
mean.  A slower arm shows a lower progress at the same tick.  Also prints how many
slots never moved (a stall) and the spread of the field.
"""
import csv
import os
import sys


def read_run(d):
    """-> {sim_tick: {slot: span_accum}} for the post_track stage."""
    f = os.path.join(d, "race_trace_track.csv")
    if not os.path.exists(f):
        return {}
    out = {}
    with open(f, newline="") as fh:
        for row in csv.DictReader(fh):
            if row.get("stage") != "post_track":
                continue
            try:
                t = int(row["sim_tick"])
                s = int(row["slot"])
                a = int(row["span_accum"])
            except (KeyError, ValueError):
                continue
            out.setdefault(t, {})[s] = a
    return out


def summarise(runs, tick):
    d = runs.get(tick, {})
    if not d:
        return None
    v = sorted(d.values())
    return dict(n=len(v), lead=v[-1], mean=sum(v) / len(v), last=v[0],
                stalled=sum(1 for x in v if x <= 2))


def main():
    a_root, b_root = sys.argv[1], sys.argv[2]
    names = sorted(set(os.listdir(a_root)) & set(os.listdir(b_root)))
    print(f"{'run':12s} {'tick':>6s} | {'A lead':>8s} {'A mean':>8s} | "
          f"{'B lead':>8s} {'B mean':>8s} | {'lead %':>7s} {'mean %':>7s}  stalls A/B")
    tot = []
    for n in names:
        ra = read_run(os.path.join(a_root, n))
        rb = read_run(os.path.join(b_root, n))
        if not ra or not rb:
            continue
        tick = min(max(ra), max(rb))
        sa, sb = summarise(ra, tick), summarise(rb, tick)
        if not sa or not sb:
            continue
        dl = 100.0 * (sb["lead"] - sa["lead"]) / max(sa["lead"], 1)
        dm = 100.0 * (sb["mean"] - sa["mean"]) / max(sa["mean"], 1)
        tot.append((dl, dm))
        print(f"{n:12s} {tick:6d} | {sa['lead']:8d} {sa['mean']:8.0f} | "
              f"{sb['lead']:8d} {sb['mean']:8.0f} | {dl:+6.1f}% {dm:+6.1f}%  "
              f"{sa['stalled']}/{sb['stalled']}")
    if tot:
        print(f"\nmean over {len(tot)} runs: leader {sum(x[0] for x in tot)/len(tot):+.1f}%, "
              f"field {sum(x[1] for x in tot)/len(tot):+.1f}%  (B relative to A)")


if __name__ == "__main__":
    main()
