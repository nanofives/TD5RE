"""geo_fork_verdicts_gen.py -- turn harness runs into the committed per-route fork verdict table.

  python verify/geo_fork_verdicts_gen.py verify/out/pieces6.json verify/out/full6.json ...
  python verify/geo_fork_verdicts_gen.py --check            # print the table the header holds now

Reads verify/out/*.json written by verify/geo_fork_validate.ps1 (schema 2: route_fp + route_place +
one entry per fork with F, R, verdict) and rewrites the block between GENERATED-BEGIN and
GENERATED-END in td5mod/src/td5re/td5_geo_fork_verdicts.h. A fork measured in several run-sets
keeps its WORST verdict (a PASS in one configuration does not hide a FAIL in another);
verify/geo_fork_keep.json then promotes named WARN forks to "keep" with the reason that
justified it (a WARN with no entry there is dropped by the generator).

The record is keyed by (place, route fingerprint): see the header. Run-sets measured on a
different route fingerprint than the first file are refused.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(ROOT, "td5mod", "src", "td5re", "td5_geo_fork_verdicts.h")
KEEP = os.path.join(ROOT, "verify", "geo_fork_keep.json")
RANK = {"PASS": 0, "WARN": 2, "FAIL": 3, "UNTESTED": 4}
CODE = {"PASS": "TD5_FV_PASS", "WARN": "TD5_FV_WARN", "FAIL": "TD5_FV_FAIL", "UNTESTED": "TD5_FV_UNTESTED",
        "KEEP": "TD5_FV_WARN_KEEP"}


def main():
    args = sys.argv[1:]
    if args == ["--check"]:
        s = open(HDR, encoding="utf-8").read()
        print(s[s.index("GENERATED-BEGIN"):s.index("GENERATED-END")])
        return 0
    keep = {}
    if os.path.exists(KEEP):
        for k in json.load(open(KEEP, encoding="utf-8")):
            keep[(k["place"], int(k["F"]), int(k["R"]))] = k["reason"]
    rec = {}
    fp = place = None
    runsets = {}
    for path in args:
        d = json.load(open(path, encoding="utf-8"))
        if d.get("schema", 1) < 2 or not d.get("route_fp"):
            sys.exit("%s: no route fingerprint (schema %s): rerun the harness with an exe that logs it" % (path, d.get("schema")))
        if fp is None:
            fp, place = d["route_fp"], d["route_place"]
        elif (d["route_fp"], d["route_place"]) != (fp, place):
            sys.exit("%s: route %s/%s differs from %s/%s" % (path, d["route_place"], d["route_fp"], place, fp))
        for f in d["forks"]:
            key = (int(f["F"]), int(f["R"]))
            v = f["verdict"]
            n = len(d.get("seeds", []))
            old = rec.get(key)
            runsets.setdefault(key, []).append("%s:%s(%dseeds)" % (d.get("run_set"), v, n))
            if old is None or RANK[v] > RANK[old]:
                rec[key] = v
    if not rec:
        sys.exit("nothing to write")
    lines = []
    for (F, R) in sorted(rec):
        v = rec[(F, R)]
        why = ""
        if v == "WARN" and (place, F, R) in keep:
            v = "KEEP"
            why = " KEEP: " + keep[(place, F, R)][:150] + " (full text: verify/geo_fork_keep.json)"
        lines.append('    { "%s", 0x%sU, %d, %d, %s },   /* %s%s */' % (
            place, fp, F, R, CODE[v], " ".join(runsets[(F, R)]), why.replace("*/", "* /")))
    body = ("static const TD5_ForkVerdict k_td5_fork_verdicts[] = {\n" + "\n".join(lines) +
            "\n};\nstatic const int k_td5_fork_verdicts_n = %d;\n" % len(lines))
    s = open(HDR, encoding="utf-8").read()
    a = s.index("/* GENERATED-BEGIN */") + len("/* GENERATED-BEGIN */\n")
    b = s.index("/* GENERATED-END */")
    s = s[:a] + body + s[b:]
    open(HDR, "w", encoding="utf-8", newline="\n").write(s)
    print("wrote %d records for %s %s" % (len(lines), place, fp))
    for l in lines:
        print(l.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
