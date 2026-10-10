"""geo_r1017s_perfork.py -- per-fork sums of the geo_r1016k_report JSONs (round 1017 S).

  python verify/geo_r1017s_perfork.py <data-dir> <tag> [<tag> ...]

Run geo_r1017s_eval.py first (it writes <data-dir>/rep_<tag>_s<seed>.json). One row per fork
(index in the fork table: 0 F=46, 1 F=210, 2 F=328, 3 F=901, 4 F=1031, 5 F=1130), columns
passes / clean / stalled / spin / events summed over the seeds of each tag.
"""
import glob
import json
import os
import re
import sys

d = sys.argv[1]
names = ["F=46", "F=210", "F=328", "F=901", "F=1031", "F=1130"]
print("%-8s" % "fork" + "".join("%-30s" % t for t in sys.argv[2:]))
tab = {}
for t in sys.argv[2:]:
    agg = {}
    for p in sorted(glob.glob(os.path.join(d, "rep_%s_s*.json" % t))):
        if not re.fullmatch(r"rep_%s_s\d+\.json" % re.escape(t), os.path.basename(p)):
            continue
        for k, v in json.load(open(p))["forks"].items():
            a = agg.setdefault(int(k), dict.fromkeys(("passes", "clean", "stalled", "spin", "events"), 0))
            for f in a:
                a[f] += v.get(f, 0)
    tab[t] = agg
for i, n in enumerate(names):
    row = "%-8s" % n
    for t in sys.argv[2:]:
        a = tab[t].get(i)
        row += "%-30s" % ("p%d c%d st%d sp%d ev%d" % (a["passes"], a["clean"], a["stalled"], a["spin"], a["events"]) if a else "-")
    print(row)
