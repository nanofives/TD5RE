"""geo_r1016k_progress.py -- is race PROGRESS right on a variable-length corridor? (round 1016 K)

  python verify/geo_r1016k_progress.py <data-prefix>

The walker accumulator (span_accum) and the high water (span_high) are what lap, finish,
checkpoint and race position read. On a point-to-point route they must equal the main-road
span the car stands beside (span_norm) everywhere, whichever road it drives. Prints the
largest |accum - norm| per slot, how often it is non-zero, and the first violation.
"""
import csv, sys
pre = sys.argv[1]
worst = {}
hi = -1
cnt = {}
first = {}
for r in csv.DictReader(open(pre + "_race_trace_track.csv", newline="", errors="replace")):
    if r.get("stage") != "post_track":
        continue
    try:
        t, s = int(r["sim_tick"]), int(r["slot"])
        raw, norm, acc, high = int(r["span_raw"]), int(r["span_norm"]), int(r["span_accum"]), int(r["span_high"])
    except (ValueError, TypeError):
        continue
    if t < hi - 50:
        break
    hi = max(hi, t)
    d = abs(acc - norm)
    if d > worst.get(s, 0):
        worst[s] = d
    if d > 2:
        cnt[s] = cnt.get(s, 0) + 1
        first.setdefault(s, (t, raw, norm, acc, high))
for s in sorted(worst):
    print("slot %d: max |accum-norm| = %d, rows over 2: %d%s" % (s, worst[s], cnt.get(s, 0),
          ("   first at tick %d raw %d norm %d accum %d high %d" % first[s]) if s in first else ""))
