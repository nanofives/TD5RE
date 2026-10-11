"""shipped_trace_cmp.py -- diff two verify/out/shipped_<tag>_t<N> trace dirs row by row (pose, motion, track, driver)."""
import sys, os, csv
a, b = sys.argv[1], sys.argv[2]
tot = 0; bad = 0
for f in sorted(os.listdir(a)):
    if not f.endswith(".csv") or not os.path.exists(os.path.join(b, f)):
        continue
    ra = list(csv.reader(open(os.path.join(a, f)))); rb = list(csv.reader(open(os.path.join(b, f))))
    n = min(len(ra), len(rb)); d = 0; first = None
    for i in range(1, n):
        # column 0 is the render frame number: not part of the sim state
        if ra[i][1:] != rb[i][1:]:
            d += 1
            if first is None: first = (i, ra[i][1:4], rb[i][1:4])
    print("%-28s rows %d/%d  differing rows %d %s" % (f, len(ra), len(rb), d, first or ""))
    tot += n; bad += d
print("TOTAL rows %d, differing %d -> %s" % (tot, bad, "IDENTICAL" if bad == 0 else "DIFFERENT"))
