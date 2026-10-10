"""geo_r1016k_inc.py -- where do the contact incidents sit? (round 1016 K)

  python verify/geo_r1016k_inc.py <datadir> <tag> [seeds...]

Histogram of contact INCIDENTS (one car's contacts <= 90 ticks apart merge) by place:
ring spans in bins of 10 (norm), corridor spans labelled "fork i +k" (bins of 10 steps).
"""
import collections, csv, re, sys
d, tag = sys.argv[1], sys.argv[2]
seeds = sys.argv[3:] or ["11", "22", "33"]
bins = collections.Counter()
for s in seeds:
    pre = "%s/rf_%s_s%s" % (d, tag, s)
    forks = []
    for line in open(pre + "_race.log", encoding="utf-8", errors="replace"):
        m = re.search(r"trackgen: fork (\d+) \w+ F=(\d+) len=(\d+) corridor=(\d+)\.\.(\d+) rejoin=(\d+)", line)
        if m and int(m.group(1)) not in [f[0] for f in forks]:
            forks.append(tuple(map(int, m.groups())))
    hi, last = -1, {}
    prev = {}
    for r in csv.DictReader(open(pre + "_race_trace_track.csv", newline="", errors="replace")):
        if r.get("stage") != "post_track":
            continue
        try:
            t, sl, raw, norm, c = int(r["sim_tick"]), int(r["slot"]), int(r["span_raw"]), int(r["span_norm"]), int(r["track_contact"])
        except (ValueError, TypeError):
            continue
        if t < hi - 50:
            break
        hi = max(hi, t)
        if c and t - last.get(sl, -999) > 90:
            lab = None
            for (i, F, L, c0, c1, R) in forks:
                if c0 <= raw <= c1:
                    lab = "fork %d corridor +%03d" % (i, ((raw - c0) // 10) * 10)
            if lab is None:
                lab = "ring %04d" % ((norm // 10) * 10)
            bins[lab] += 1
        if c:
            last[sl] = t
for k, v in sorted(bins.items()):
    print("%-26s %3d %s" % (k, v, "#" * v))
