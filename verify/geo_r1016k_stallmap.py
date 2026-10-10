"""geo_r1016k_stallmap.py -- where do the stalls sit? (round 1016 K)

  python verify/geo_r1016k_stallmap.py <datadir> <tag> [tag...] [--seeds 11 22 ...] [--bin 20]

Plateau stalls (>= 90 ticks within 2 spans, before the finish) summed over seeds, per
span bin (span_norm). One column per tag: "count/ticks".
"""
import argparse, collections, subprocess, sys, re, os
ap = argparse.ArgumentParser()
ap.add_argument("datadir"); ap.add_argument("tags", nargs="+")
ap.add_argument("--seeds", nargs="+", default=["11", "22", "33", "44", "55", "66"])
ap.add_argument("--bin", type=int, default=20)
a = ap.parse_args()
here = os.path.dirname(os.path.abspath(__file__))
tab = {}
for tag in a.tags:
    c = collections.Counter(); t = collections.Counter()
    for s in a.seeds:
        out = subprocess.run([sys.executable, os.path.join(here, "geo_r1016k_report.py"),
                              "%s/rf_%s_s%s" % (a.datadir, tag, s), "--finish", "1205"],
                             capture_output=True, text=True).stdout
        for m in re.finditer(r"slot (\d+) ticks (\d+)\.\.(\d+) \((\d+)\) spans (\d+)\.\.(\d+)", out):
            b = (int(m.group(5)) // a.bin) * a.bin
            c[b] += 1; t[b] += int(m.group(4))
    tab[tag] = (c, t)
bins = sorted(set(k for c, t in tab.values() for k in c))
print("%-6s" % "span" + "".join("%16s" % x for x in a.tags))
for b in bins:
    print("%-6d" % b + "".join("%16s" % ("%d/%d" % (tab[x][0][b], tab[x][1][b]) if tab[x][0][b] else ".") for x in a.tags))
print("%-6s" % "TOTAL" + "".join("%16s" % ("%d/%d" % (sum(tab[x][0].values()), sum(tab[x][1].values()))) for x in a.tags))
