"""Where do cars touch walls around a block loop? Histogram of contact events (track_contact 0 -> non-zero)
per corridor step / main span, over every race of a run set.  python verify/geo_bl_hits.py verify/out/<tag> LO HI [D A]"""
import csv, glob, os, sys
d = sys.argv[1]; lo = int(sys.argv[2]); hi = int(sys.argv[3])
D = int(sys.argv[4]) if len(sys.argv) > 4 else 735
A = int(sys.argv[5]) if len(sys.argv) > 5 else 772
hist = {}
for run in sorted(glob.glob(os.path.join(d, "fork_s*"))):
    prev = {}
    last_t = {}
    for r in csv.DictReader(open(os.path.join(run, "log", "race_trace_track.csv"))):
        if r["stage"] != "post_physics": continue
        try: s = int(r["slot"]); t = int(r["sim_tick"]); sp = int(r["span_raw"]); c = int(r["track_contact"])
        except (ValueError, TypeError): continue
        if t < last_t.get(s, 0): continue            # second race
        last_t[s] = t
        if c and not prev.get(s, 0):
            if lo <= sp <= hi: key = ("corr", sp - lo)
            elif D - 8 <= sp <= A + 2: key = ("main", sp)
            else: key = None
            if key: hist[key] = hist.get(key, 0) + 1
        prev[s] = c
for k in sorted(hist): print(k, hist[k])
