#!/usr/bin/env python3
"""Scan the 7 native TD5 circuit strips: ring length, span-count vs ring,
span-type histogram (junction=8/11, transition=3/4, reversed=6/7), and whether
the main ring is closed/contiguous. Tells us which circuits are clean pure-rings
(safe to reverse) vs branched (need care)."""
import json, os
from collections import Counter

CIRCUITS = {25:"Cheddar",26:"Jarash",27:"Courmayeur",28:"Maui",
            29:"Newcastle",37:"HouseOfBez",39:"MontegoBay"}
ROOT = "re/assets/levels"

for lv, name in CIRCUITS.items():
    p = os.path.join(ROOT, f"level{lv:03d}", "strip.json")
    if not os.path.isfile(p):
        print(f"level{lv:03d} {name}: NO strip.json"); continue
    obj = json.load(open(p, encoding="utf-8"))
    spans = obj["spans"]; verts = obj["vertices"]; hdr = obj["header"]
    ring = hdr[1]
    types = Counter(s[0] for s in spans)
    lanes = Counter(s[3] & 0x0F for s in spans)
    # explicit (non-sentinel) fwd/bwd links among main-ring spans?
    expl = sum(1 for s in spans[:ring] if s[6] != 0xFFFF or s[7] != 0xFFFF)
    junction = types.get(8,0) + types.get(11,0)
    branchish = sum(types.get(t,0) for t in (3,4,6,7,8,11))
    print(f"level{lv:03d} {name:11s}: ring={ring:5d} spans={len(spans):5d} "
          f"verts={len(verts):5d} appended={len(spans)-ring:4d}  "
          f"junction(8/11)={junction} transition/reversed(3/4/6/7)="
          f"{sum(types.get(t,0) for t in (3,4,6,7))}  explicit_links={expl}")
    print(f"    types={dict(types)}  lanes={dict(lanes)}")
