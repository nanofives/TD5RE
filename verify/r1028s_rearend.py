"""r1028s_rearend.py -- count AI-vs-AI REAR-END contacts in one or more race logs (round 1028 S).

    python -I verify/r1028s_rearend.py log/r1028s_base_*_race.log [--base 6] [--csv out.csv]

Reads the `v2v_gap:` lines written by TD5RE_V2V_GAPLOG=2 (every contact, racer pairs
included).  Round 1028 S appends four fields to that line:

    lon=<n>   B's centre AHEAD of A, in A's heading frame, model units (>0 = B in front)
    lat=<n>   B's centre to A's +u side, same frame
    vlA/vlB   each car's longitudinal_speed, raw 24.8 (u/tick = value/256)

A contact is counted as a REAR-END when, at the first tick of the episode:
  * both slots are racers (slot < --base) and neither is the human (all AI when the
    race ran with --PlayerIsAI=1),
  * the two cars are roughly aligned (|yaw difference| <= --yaw, default 30 deg),
  * the offset is longitudinal, not side-by-side: |lon| > |lat|,
  * the car BEHIND is the faster one (it is closing on the one in front).

An episode = one pair's run of consecutive ticks (a break of more than 3 ticks starts a
new one), so a single shunt that lasts 40 ticks counts once.
"""
import re
import sys
from collections import defaultdict

PAT = re.compile(
    r"v2v_gap: path=(\w+) tick=(\d+) a=(\d+) b=(\d+) spanA=(-?\d+) spanB=(-?\d+) "
    r"gap=([\d.]+) sat=(-?[\d.]+) cdist=([\d.]+) dy=(-?\d+) yawd=(\d+).*?"
    r"lon=(-?\d+) lat=(-?\d+) vlA=(-?\d+) vlB=(-?\d+)"
)


def yaw_deg(yawd):
    """yawd is (yawA - yawB) & 0xFFF.  Return the absolute angle in degrees, 0..180."""
    d = yawd & 0xFFF
    if d > 0x800:
        d -= 0x1000
    return abs(d) * 360.0 / 4096.0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    base = 6
    yaw_lim = 30.0
    csv_out = ""
    if "--base" in sys.argv:
        base = int(sys.argv[sys.argv.index("--base") + 1])
        args = [a for a in args if a != str(base)]
    if "--yaw" in sys.argv:
        yaw_lim = float(sys.argv[sys.argv.index("--yaw") + 1])
        args = [a for a in args if a != str(yaw_lim)]
    if "--csv" in sys.argv:
        csv_out = sys.argv[sys.argv.index("--csv") + 1]
        args = [a for a in args if a != csv_out]

    rows = []
    for fn in args:
        with open(fn, errors="replace") as f:
            for line in f:
                m = PAT.search(line)
                if not m:
                    continue
                (path, tick, a, b, spa, spb, gap, sat, cd, dy, yawd,
                 lon, lat, vla, vlb) = m.groups()
                rows.append(dict(fn=fn, path=path, tick=int(tick), a=int(a), b=int(b),
                                 spanA=int(spa), spanB=int(spb), gap=float(gap),
                                 sat=float(sat), cdist=float(cd), dy=int(dy),
                                 yawd=int(yawd), lon=int(lon), lat=int(lat),
                                 vlA=int(vla), vlB=int(vlb)))

    # episodes: (file, pair) run of consecutive ticks, 3-tick break
    eps = []
    cur = {}
    for r in sorted(rows, key=lambda r: (r["fn"], min(r["a"], r["b"]), max(r["a"], r["b"]), r["tick"])):
        k = (r["fn"], min(r["a"], r["b"]), max(r["a"], r["b"]))
        if k in cur and r["tick"] - cur[k]["last"] <= 3:
            cur[k]["last"] = r["tick"]
            cur[k]["n"] += 1
            continue
        e = dict(key=k, first=r, last=r["tick"], n=1)
        eps.append(e)
        cur[k] = e

    def racer_pair(r):
        return r["a"] < base and r["b"] < base

    def rear_end(r):
        if not racer_pair(r):
            return None
        if r["path"] == "MISS":
            return None
        if yaw_deg(r["yawd"]) > yaw_lim:
            return None
        if abs(r["lon"]) <= abs(r["lat"]):
            return None
        if r["lon"] > 0:            # B is in front -> A follows
            follower, leader = r["a"], r["b"]
            vf, vl = r["vlA"], r["vlB"]
        else:
            follower, leader = r["b"], r["a"]
            vf, vl = r["vlB"], r["vlA"]
        if vf <= vl:                # not closing: the one behind is slower
            return None
        return (follower, leader, vf - vl, abs(r["lon"]), abs(r["lat"]))

    rr_eps = [e for e in eps if racer_pair(e["first"]) and e["first"]["path"] != "MISS"]
    re_eps = [e for e in eps if rear_end(e["first"])]
    all_rr = [r for r in rows if racer_pair(r) and r["path"] != "MISS"]
    all_re = [r for r in rows if rear_end(r)]

    print(f"{len(rows)} contact lines from {len(args)} log(s), racer slots < {base}, "
          f"alignment limit {yaw_lim:.0f} deg")
    print(f"racer-racer       contacts={len(all_rr):6d}  episodes={len(rr_eps):5d}")
    print(f"  of which REAR-END contacts={len(all_re):6d}  episodes={len(re_eps):5d}")
    if re_eps:
        dv = sorted(rear_end(e["first"])[2] for e in re_eps)
        ln = sorted(e["n"] for e in re_eps)
        print(f"  rear-end episode closing speed u/tick: med={dv[len(dv)//2]/256.0:6.1f} "
              f"max={dv[-1]/256.0:6.1f}   ticks per episode: med={ln[len(ln)//2]} max={ln[-1]}")
        per_file = defaultdict(int)
        for e in re_eps:
            per_file[e["key"][0]] += 1
        for fn in sorted(per_file):
            tot = sum(1 for x in rr_eps if x["key"][0] == fn)
            print(f"    {fn.split(chr(92))[-1].split('/')[-1]:44s} rear-end={per_file[fn]:4d}  "
                  f"racer-racer={tot:4d}")
    else:
        for fn in sorted(set(e["key"][0] for e in rr_eps)):
            tot = sum(1 for x in rr_eps if x["key"][0] == fn)
            print(f"    {fn.split(chr(92))[-1].split('/')[-1]:44s} rear-end=   0  racer-racer={tot:4d}")

    if csv_out:
        with open(csv_out, "w") as f:
            f.write("log,first_tick,ticks,follower,leader,closing_u_tick,lon,lat,gap,path\n")
            for e in re_eps:
                fo, le, dv, lon, lat = rear_end(e["first"])
                f.write(f"{e['key'][0]},{e['first']['tick']},{e['n']},{fo},{le},"
                        f"{dv/256.0:.1f},{lon},{lat},{e['first']['gap']:.1f},{e['first']['path']}\n")
        print(f"wrote {csv_out}")


if __name__ == "__main__":
    main()
