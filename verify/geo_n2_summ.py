"""geo_n2_summ.py -- summarise the routes verify/geo_n2_build.ps1 kept under verify/out/n2_routes (prefix argument).

One line per route: fingerprint, the real forks built as (kind letter + side letter, F, R) -- 'AR' = avenue, right corridor,
'AL' = avenue, LEFT corridor, 'LR' / 'LL' = block loop leaving through the right / left edge -- and the loop candidates
(side, F, R, shares-a-real-fork-window flag).
"""
import glob
import os
import re
import sys

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(prefix):
    for d in sorted(glob.glob(os.path.join(WT, "verify", "out", "n2_routes", prefix + "*"))):
        lg = os.path.join(d, "race.log")
        if not os.path.exists(lg):
            continue
        t = open(lg, encoding="utf-8", errors="replace").read()
        fp = re.search(r"route fingerprint (\w+)", t)
        forks = re.findall(r"trackgen: fork \d+ (\w+) F=(\d+) len=\d+ corridor=\d+\.\.\d+ rejoin=(\d+).*?side=(\w)", t)
        lines = re.findall(r"CANDIDATE .*$", t, re.M)
        cands = []
        for ln in lines:
            m = re.search(r"side (\w+) F=(\d+) R=(\d+)", ln)
            if m:
                cands.append((m.group(1)[0], int(m.group(2)), int(m.group(3)), 1 if "[BESIDE REAL FORK]" in ln else 0))
        print(os.path.basename(d), fp.group(1) if fp else None, "forks", [(k[0] + sd, int(f), int(r)) for k, f, r, sd in forks], "cands", cands)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "")
