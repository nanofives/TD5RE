"""r1029y_fate.py -- round 1029 Y: what happens to a racer-vs-racer corner-test contact that fires with a hull gap?

    python -I verify/r1029y_fate.py <race.log> [more ...] [--window 8] [--base 6] [--list N]

Reads `v2v_gap:` lines (TD5RE_V2V_GAPLOG=2). Ticks = the sim= field (simulation_tick_counter).
For racer-racer pairs only (both slots < base):
  * "gap events": FULL lines with gap > 0 (TD5RE_RACER_SAT=1: contacts that fired at a gap) and REJ lines
    (TD5RE_RACER_SAT=2: contacts the SAT gate dropped). Grouped into episodes per pair (ticks <= 3 apart).
  * fate of each episode: DELAYED = the same pair has an accepted contact with gap 0 (FULL or BP) within
    --window ticks after the episode's first tick (the bodies really met, the contact happened anyway);
    GRAZE = no gap-0 contact in the window (the bodies never overlapped per SAT, the pair drifted apart).
Also prints, per path, contacts / episodes and the gap / relv distribution, and the first --list gap events.
"""
import re
import sys
from collections import defaultdict

PAT = re.compile(r"v2v_gap: path=(\w+) tick=\d+ a=(\d+) b=(\d+) spanA=(-?\d+) spanB=(-?\d+) gap=([\d.]+) sat=(-?[\d.]+) "
                 r"cdist=([\d.]+) dy=(-?\d+) yawd=(\d+) relv=(-?\d+) wcbA=(-?\d+) wcbB=(-?\d+) vmA=(-?\d+) vmB=(-?\d+)"
                 r".*?sim=(\d+)(?: lon=(-?\d+) lat=(-?\d+))?")


def load(fn, base):
    rows = []
    with open(fn, errors="replace") as f:
        for line in f:
            m = PAT.search(line)
            if not m:
                continue
            g = m.groups()
            a, b = int(g[1]), int(g[2])
            if a >= base or b >= base:
                continue
            rows.append(dict(path=g[0], a=min(a, b), b=max(a, b), span=int(g[3]), gap=float(g[5]), sat=float(g[6]),
                             cdist=float(g[7]), dy=int(g[8]), yawd=int(g[9]), relv=int(g[10]),
                             wcb=(int(g[11]), int(g[12])), vm=(int(g[13]), int(g[14])), sim=int(g[15]),
                             lon=int(g[16]) if g[16] else 0, lat=int(g[17]) if g[17] else 0))
    return rows


def episodes(rows):
    eps = []
    last = {}
    for r in sorted(rows, key=lambda r: (r["a"], r["b"], r["sim"])):
        k = (r["a"], r["b"])
        if k in last and r["sim"] - last[k][-1]["sim"] <= 3:
            last[k].append(r)
        else:
            last[k] = [r]
            eps.append(last[k])
    return eps


def main():
    args = [x for x in sys.argv[1:]]
    opt = {"--window": 8, "--base": 6, "--list": 12}
    files = []
    i = 0
    while i < len(args):
        if args[i] in opt:
            opt[args[i]] = int(args[i + 1]); i += 2
        else:
            files.append(args[i]); i += 1
    W = opt["--window"]
    tot = defaultdict(int)
    for fn in files:
        rows = load(fn, opt["--base"])
        acc = [r for r in rows if r["path"] in ("FULL", "BP") and r["gap"] <= 0.0]
        by_pair = defaultdict(list)
        for r in acc:
            by_pair[(r["a"], r["b"])].append(r["sim"])
        print("== %s: %d racer-racer lines" % (fn, len(rows)))
        for path in ("FULL", "BP", "REJ", "MISS"):
            p = [r for r in rows if r["path"] == path]
            if not p:
                continue
            gp = [r["gap"] for r in p if r["gap"] > 0]
            # depth of the FIRST line of each episode (sat < 0 = hulls overlap by -sat units): does a gate that
            # waits for a real overlap let the cars sink deeper before the first impulse?
            d0 = sorted(max(0.0, -e[0]["sat"]) for e in episodes(p))
            med = d0[len(d0) // 2] if d0 else 0.0
            print("  %-4s lines=%5d episodes=%4d gap>0=%4d max_gap=%5.1f max_relv=%d | first-line depth med=%.1f max=%.1f" %
                  (path, len(p), len(episodes(p)), len(gp), max(gp) if gp else 0.0, max(r["relv"] for r in p),
                   med, d0[-1] if d0 else 0.0))
            if path == "FULL":
                tot["d0"] = tot.get("d0", []) + d0
        ev = [r for r in rows if (r["path"] == "FULL" and r["gap"] > 0) or r["path"] == "REJ"]
        eps = episodes(ev)
        nd = ng = 0
        for e in eps:
            t0 = e[0]["sim"]
            later = [t for t in by_pair[(e[0]["a"], e[0]["b"])] if t0 <= t <= t0 + W]
            fate = "DELAYED +%d" % (min(later) - t0) if later else "GRAZE"
            if later:
                nd += 1
            else:
                ng += 1
            if nd + ng <= opt["--list"]:
                r = e[0]
                print("    %-4s sim=%5d pair=%d-%d span=%d lines=%d gap=%.1f..%.1f relv=%d dy=%d yawd=%d lon=%d lat=%d wcb=%s vm=%s -> %s" %
                      (r["path"], t0, r["a"], r["b"], r["span"], len(e), min(x["gap"] for x in e), max(x["gap"] for x in e),
                       max(x["relv"] for x in e), r["dy"], r["yawd"], r["lon"], r["lat"], r["wcb"], r["vm"], fate))
        print("  gap-event episodes=%d  DELAYED (bodies met within %d ticks)=%d  GRAZE (never overlapped)=%d" % (len(eps), W, nd, ng))
        tot["eps"] += len(eps); tot["d"] += nd; tot["g"] += ng
        tot["full"] += sum(1 for r in rows if r["path"] == "FULL")
        tot["bp"] += sum(1 for r in rows if r["path"] == "BP")
        tot["miss"] += sum(1 for r in rows if r["path"] == "MISS")
        tot["fullgap"] += sum(1 for r in rows if r["path"] == "FULL" and r["gap"] > 0)
        tot["rej"] += sum(1 for r in rows if r["path"] == "REJ")
    if len(files) > 1:
        print("TOTAL: FULL=%d (gap>0 %d) BP=%d REJ=%d MISS=%d | gap-event episodes=%d DELAYED=%d GRAZE=%d" %
              (tot["full"], tot["fullgap"], tot["bp"], tot["rej"], tot["miss"], tot["eps"], tot["d"], tot["g"]))
        d0 = sorted(tot.get("d0", []))
        if d0:
            print("TOTAL FULL first-line depth: episodes=%d med=%.1f p90=%.1f max=%.1f" %
                  (len(d0), d0[len(d0) // 2], d0[min(len(d0) - 1, int(0.9 * len(d0)))], d0[-1]))


if __name__ == "__main__":
    main()
