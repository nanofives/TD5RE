"""geo_r1017s_eval.py -- La Plata run totals over a fork-harness run-set (round 1017 S).

  python verify/geo_r1017s_eval.py <data-dir> <tag>=<runset-dir>[:arm] [<tag>=<runset-dir>[:arm] ...]

The fork harness (verify/geo_fork_validate.ps1) leaves verify/out/<tag>/<arm>_s<seed>/log/
with the track/motion/pose traces and race.log. geo_r1016k_cmp.py wants them as
<data-dir>/rf_<tag>_s<seed>_race_trace_*.csv, so this copies the files under that name (once)
and prints the same summed table (stalls, stall ticks, contact events, incidents, passes, clean
passes, spins, mean tick to span 600 / 1100). arm = fork (default) or base (forks switched off).
A run-set is judged on the seeds it holds.
"""
import os
import re
import shutil
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
data = sys.argv[1]
os.makedirs(data, exist_ok=True)
tags, seeds = [], set()
for spec in sys.argv[2:]:
    tag, rest = spec.split("=", 1)
    arm = "fork"
    if ":" in rest and rest.rsplit(":", 1)[1] in ("fork", "base"):
        rest, arm = rest.rsplit(":", 1)
    got = []
    for d in sorted(os.listdir(rest)):
        m = re.fullmatch(arm + r"_s(\d+)", d)
        if not m:
            continue
        s = m.group(1)
        got.append(s)
        for src, dst in (("race_trace_track.csv", "race_trace_track.csv"),
                         ("race_trace_motion.csv", "race_trace_motion.csv"),
                         ("race_trace_pose.csv", "race_trace_pose.csv"),
                         ("race.log", "race.log")):
            p = os.path.join(rest, d, "log", src)
            q = os.path.join(data, "rf_%s_s%s_%s" % (tag, s, dst))
            if os.path.exists(p) and (not os.path.exists(q) or os.path.getmtime(p) > os.path.getmtime(q)):
                shutil.copyfile(p, q)
    tags.append(tag)
    seeds.update(got)
cmd = [sys.executable, os.path.join(here, "geo_r1016k_cmp.py"), data] + tags + ["--seeds"] + sorted(seeds, key=int)
subprocess.run(cmd)
