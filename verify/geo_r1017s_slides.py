"""geo_r1017s_slides.py -- high-speed SLIDE onsets over a fork-harness run-set (round 1017 S).

  python verify/geo_r1017s_slides.py <runset-dir>[:arm] [<runset-dir>[:arm] ...] [--vmin 350] [--big 20000]

A slide ONSET is a tick where a car's rear axle starts to slip (rear_slip 0 -> > 0) at more than
--vmin units/tick of forward speed with the rear axle gripping on the 3 ticks before. A BIG slide
is an onset whose rear slip reaches --big within 20 ticks (the snap-oversteer of the F=46 incident:
18 -> 54790). Counted over the first race only, every car, every span, split into the corridors
(span_raw > ring) and the main road. A pile-up counts as ONE incident however long it lasts,
so this is a far less noisy measure of the mechanism than wall-contact events.
"""
import csv
import collections
import os
import sys

args = [a for a in sys.argv[1:] if not a.startswith("--")]
vmin, big = 350, 20000
for i, a in enumerate(sys.argv):
    if a == "--vmin":
        vmin = int(sys.argv[i + 1]); args = [x for x in args if x != sys.argv[i + 1]]
    if a == "--big":
        big = int(sys.argv[i + 1]); args = [x for x in args if x != sys.argv[i + 1]]

for spec in args:
    arm = "fork"
    d = spec
    if ":" in spec and spec.rsplit(":", 1)[1] in ("fork", "base"):
        d, arm = spec.rsplit(":", 1)
    tot = collections.Counter()
    for sub in sorted(os.listdir(d)):
        if not sub.startswith(arm + "_s"):
            continue
        base = os.path.join(d, sub, "log")
        span = {}
        hi = -1
        for r in csv.DictReader(open(os.path.join(base, "race_trace_track.csv"))):
            if r["stage"] != "post_track":
                continue
            t = int(r["sim_tick"])
            if t < hi - 50:
                break
            hi = max(hi, t)
            span[(t, int(r["slot"]))] = int(r["span_raw"])
        mot = collections.defaultdict(list)
        hi = -1
        for r in csv.DictReader(open(os.path.join(base, "race_trace_motion.csv"))):
            if r["stage"] != "post_physics":
                continue
            t = int(r["sim_tick"])
            if t < hi - 50:
                break
            hi = max(hi, t)
            mot[int(r["slot"])].append((t, int(r["long_speed"]) // 256, int(r["rear_slip"])))
        for slot, rows in mot.items():
            rows.sort()
            n = len(rows)
            i = 3
            while i < n:
                t, v, rs = rows[i]
                if rs > 0 and v > vmin and all(rows[i - k][2] == 0 for k in (1, 2, 3)):
                    sp = span.get((t, slot), 0)
                    where = "corr" if sp > 1310 else "main"
                    mx = max(r[2] for r in rows[i:i + 20])
                    tot[where + "_onsets"] += 1
                    if mx >= big:
                        tot[where + "_big"] += 1
                    i += 20
                else:
                    i += 1
    print("%-34s main onsets %3d big %3d | corridor onsets %3d big %3d" % (
        spec, tot["main_onsets"], tot["main_big"], tot["corr_onsets"], tot["corr_big"]))
