"""F0a exploration: confirm casco polygon from ROADS name tags, check duplicates.
Run: python -I explore.py
"""
import glob
import json
import os
import re
from collections import Counter, defaultdict

GEO = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\assets\geo\la_plata_partido\tiles"

ways = {}
dups = 0
for path in sorted(glob.glob(os.path.join(GEO, "ROADS_*.JSON"))):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    for r in d.get("roads", []):
        rid = r.get("id")
        if rid in ways:
            dups += 1
            continue
        ways[rid] = r
print("ROADS files:", len(glob.glob(os.path.join(GEO, "ROADS_*.JSON"))))
print("unique way ids:", len(ways), "duplicate records skipped:", dups)

pat = re.compile(r"\b(av\.?|avenida)\s*(31|32|72|122)\b", re.I)
ext = defaultdict(lambda: [999, -999, 999, -999, 0])  # latmin latmax lonmin lonmax n
names = Counter()
for r in ways.values():
    nm = r.get("name") or ""
    names[nm] += 1
    m = pat.search(nm)
    key = None
    if m:
        key = "Av " + m.group(2)
    elif re.search(r"\b(31|32|72|122)\b", nm):
        key = "other:" + nm
    if key is None:
        continue
    e = ext[key]
    for la, lo in r.get("latlon", []):
        e[0] = min(e[0], la); e[1] = max(e[1], la)
        e[2] = min(e[2], lo); e[3] = max(e[3], lo)
    e[4] += 1
for k, e in sorted(ext.items()):
    print(f"{k:40s} ways={e[4]:4d} lat[{e[0]:.6f},{e[1]:.6f}] lon[{e[2]:.6f},{e[3]:.6f}]")

print("\nTop names by way count:")
for nm, c in names.most_common(40):
    print(f"  {c:4d}  {nm!r}")
