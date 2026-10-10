"""geo_r1016k_cmp.py -- before/after table over seeds (round 1016 K).

  python verify/geo_r1016k_cmp.py <datadir> <tagA> <tagB> ... [--seeds 11 22 33] [--ring N --finish N]

Runs geo_r1016k_report.py's analysis on <datadir>/rf_<tag>_s<seed>_* for every tag and
prints one row per tag (summed over seeds) with the per-seed numbers beside it.
"""
import argparse, json, os, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("datadir")
ap.add_argument("tags", nargs="+")
ap.add_argument("--seeds", nargs="+", default=["11", "22", "33"])
ap.add_argument("--ring", default="1310")
ap.add_argument("--finish", default="1205")
a = ap.parse_args()
here = os.path.dirname(os.path.abspath(__file__))
keys = ["stalls", "stall_ticks", "events", "incidents", "fork_ev", "fork_inc", "route_ev", "passes", "clean", "spins", "t600", "t1100"]
print("%-12s %s" % ("tag", "  ".join("%11s" % k for k in keys)))
for tag in a.tags:
    tot = dict.fromkeys(keys, 0)
    per = []
    for s in a.seeds:
        pre = os.path.join(a.datadir, "rf_%s_s%s" % (tag, s))
        out = os.path.join(a.datadir, "rep_%s_s%s.json" % (tag, s))
        if not os.path.exists(pre + "_race_trace_track.csv"):
            per.append("s%s:missing" % s)
            continue
        subprocess.run([sys.executable, os.path.join(here, "geo_r1016k_report.py"), pre,
                        "--ring", a.ring, "--finish", a.finish, "--json", out],
                       stdout=subprocess.DEVNULL)
        r = json.load(open(out))
        ev, inc = r.get("ev", {}), r.get("inc", {})
        r["fork_ev"] = ev.get("corridor", 0) + ev.get("mouth", 0)
        r["fork_inc"] = inc.get("corridor", 0) + inc.get("mouth", 0)
        r["route_ev"] = ev.get("route", 0) + ev.get("window", 0)
        for k in keys:
            tot[k] += r.get(k, 0)
        r["t600"], r["t1100"] = int(r.get("t600", 0)), int(r.get("t1100", 0))
        per.append("s%s: stall %d ev %d (fork %d / other %d) inc %d (fork %d) clean %d/%d spin %d" % (
            s, r["stalls"], r["events"], r["fork_ev"], r["route_ev"], r["incidents"], r["fork_inc"],
            r["clean"], r["passes"], r["spins"]))
    tot["t600"] //= max(1, len(a.seeds)); tot["t1100"] //= max(1, len(a.seeds))
    print("%-12s %s" % (tag, "  ".join("%11d" % tot[k] for k in keys)))
    for p in per:
        print("             " + p)
