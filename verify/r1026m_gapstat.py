"""r1026m_gapstat.py -- summarise `v2v_gap:` lines of one or more race logs (round 1026 M).

    python -I verify/r1026m_gapstat.py log/r1026m_base_t0_ai_race.log [more logs ...] [--base 6]

Each `v2v_gap:` line is ONE contact that fired a detector (FULL = corner test, BP = broadphase
sphere for scripted/airborne cars, S20 = player-vs-traffic world-proximity supplement) with the
TRUE body-to-body gap (SAT over the silhouette hull, world units; 0 = bodies overlap).
Prints per path and per pair class: contacts, how many have a real gap > 0 / > 50 / > 200 and the
distribution of the gap; plus "episodes" (a pair's run of consecutive ticks counted once).
"""
import re
import sys
from collections import defaultdict

PAT = re.compile(r"v2v_gap: path=(\w+) tick=(\d+) a=(\d+) b=(\d+) .*?gap=([\d.]+) sat=(-?[\d.]+) cdist=([\d.]+) dy=(-?\d+) yawd=(\d+)")


def klass(a, b, base):
    ta, tb = a >= base, b >= base
    if ta and tb:
        return "traffic-traffic"
    if not ta and not tb:
        return "racer-racer"
    return "racer-traffic"


def pct(v, p):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))]


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    base = 6
    if "--base" in sys.argv:
        base = int(sys.argv[sys.argv.index("--base") + 1])
        args = [x for x in args if x != str(base)]
    rows = []
    for fn in args:
        with open(fn, errors="replace") as f:
            for line in f:
                m = PAT.search(line)
                if m:
                    path, tick, a, b, gap, sat, cd, dy, yawd = m.groups()
                    rows.append((path, int(tick), int(a), int(b), float(gap), float(sat), float(cd), int(dy), int(yawd), fn))
    print(f"{len(rows)} contact lines from {len(args)} log(s), traffic slot base {base}")
    groups = defaultdict(list)
    for r in rows:
        groups[(klass(r[2], r[3], base), r[0])].append(r)
        if r[0] not in ("MISS", "REJ"):   # [1029 Y] REJ = dropped by the SAT gate, not a contact
            groups[(klass(r[2], r[3], base), "ALL")].append(r)
    for key in sorted(groups):
        g = groups[key]
        gaps = [r[4] for r in g]
        n = len(g)
        pos = [x for x in gaps if x > 0.0]
        eps = set()
        last = {}
        for r in sorted(g, key=lambda r: (r[9], r[2], r[3], r[1])):
            k = (r[9], r[2], r[3])
            if k not in last or r[1] - last[k] > 3:
                eps.add((k, r[1]))
            last[k] = r[1]
        epos = 0
        # episode "has a real gap" = first line of the episode had gap > 0
        first = {}
        for r in sorted(g, key=lambda r: (r[9], r[2], r[3], r[1])):
            k = (r[9], r[2], r[3])
            if (k, r[1]) in eps:
                first[(k, r[1])] = r[4]
        epos = sum(1 for v in first.values() if v > 0)
        print(f"{key[0]:16s} {key[1]:4s} contacts={n:5d} gap>0={len(pos):5d} ({100.0*len(pos)/max(n,1):5.1f}%) "
              f">50={sum(1 for x in gaps if x>50):5d} >200={sum(1 for x in gaps if x>200):5d} "
              f"med(gap>0)={pct(pos,0.5):6.1f} p90={pct(pos,0.9):6.1f} max={max(gaps) if gaps else 0:7.1f} "
              f"| episodes={len(eps):4d} with-gap={epos:4d}")


if __name__ == "__main__":
    main()
