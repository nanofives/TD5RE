"""geo_r1016k_report.py -- AI-on-forks judge for round 1016 K (first race of a trace only).

  python verify/geo_r1016k_report.py <data-prefix> [--ring 1310 --finish 1206] [--json out.json]

<data-prefix> = log/rf_<tag>_ from verify/geo_realfork_run.ps1 (needs the track, motion
and pose traces and the race.log fork table). Reads only the FIRST race (an AutoRace
session restarts sim_tick when the race ends).

Per run it prints
  * cars that finished (span_norm >= finish) and their finish ticks,
  * STALL plateaus (>= 90 ticks within 2 spans, before the car reaches the finish),
  * wall/edge contact EVENTS and INCIDENTS (one car's contacts <= 90 ticks apart merge),
    in total and in four buckets: fork corridors, fork mouths (corridor steps 0..19 and
    ring spans F-4..F+4 / the rejoin R-10..R+8), ring windows, plain route,
  * per fork: car-passes on the corridor, how many passed CLEAN (no contact event, no
    plateau) and how many stalled,
  * MOUTH SPINS: a pass that loses >= 45% of its speed inside the first 24 corridor
    steps and ends the corridor slower than 25% of the entry speed, or reverses.
The one-line SUMMARY is what the before/after tables quote.
"""
import argparse
import collections
import csv
import json
import re

INCIDENT_GAP = 90
HOLD = 90


def first_race(path, stage, keep_cols):
    out = collections.defaultdict(list)
    hi = -1
    for r in csv.DictReader(open(path, newline="", encoding="utf-8", errors="replace")):
        if r.get("stage") != stage:
            continue
        try:
            t = int(r["sim_tick"])
            slot = int(r["slot"])
            vals = tuple(int(r[c] or 0) for c in keep_cols)
        except (ValueError, TypeError):
            continue                      # a row cut short when the run was stopped
        if t < hi - 50:
            break
        hi = max(hi, t)
        out[slot].append((t,) + vals)
    return out


def read_forks(log_path):
    forks, seen = [], set()
    pat = re.compile(r"trackgen: fork (\d+) \w+ F=(\d+) len=(\d+) corridor=(\d+)\.\.(\d+) rejoin=(\d+)")
    for line in open(log_path, encoding="utf-8", errors="replace"):
        m = pat.search(line)
        if m:
            i, F, L, c0, c1, R = map(int, m.groups())
            if i not in seen:
                seen.add(i)
                forks.append((i, F, L, R, c0, c1))
    return forks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--ring", type=int, default=1310)
    ap.add_argument("--finish", type=int, default=1206)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    trk = first_race(a.prefix + "_race_trace_track.csv", "post_track",
                     ["span_raw", "span_norm", "track_contact"])
    mot = first_race(a.prefix + "_race_trace_motion.csv", "post_physics", ["long_speed"])
    forks = read_forks(a.prefix + "_race.log")
    ring = a.ring
    res = {"forks": {}, "finishers": 0, "finish_ticks": []}

    # per slot: moving window = first span change .. first time at the finish
    win = {}
    for slot, seq in trk.items():
        t0 = next((t for t, _r, n, _c in seq if n != seq[0][2]), seq[0][0])
        t1 = next((t for t, _r, n, _c in seq if n >= a.finish), seq[-1][0] + 1)
        win[slot] = (t0, t1)
        if any(n >= a.finish for _t, _r, n, _c in seq):
            res["finishers"] += 1
            res["finish_ticks"].append(t1)
    # pace: tick at which each car first reaches span 600 / 1100 (a slower field = bigger)
    for mark in (600, 1100):
        ts = []
        for slot, seq in trk.items():
            for t, _r, n, _c in seq:
                if n >= mark:
                    ts.append(t)
                    break
        res["t%d" % mark] = (sum(ts) / len(ts)) if ts else 0
        res["n%d" % mark] = len(ts)
    print("run %s: slots %s, forks %d, finishers %d/%d, finish ticks %s" % (
        a.prefix, sorted(trk), len(forks), res["finishers"], len(trk), sorted(res["finish_ticks"])))

    # ---- stalls --------------------------------------------------------------
    stalls = []
    for slot, seq in sorted(trk.items()):
        t0, t1 = win[slot]
        sub = [x for x in seq if t0 <= x[0] < t1]
        i = 0
        while i < len(sub):
            lo = hi = sub[i][2]
            j = i + 1
            while j < len(sub):
                lo2, hi2 = min(lo, sub[j][2]), max(hi, sub[j][2])
                if hi2 - lo2 > 2:
                    break
                lo, hi = lo2, hi2
                j += 1
            if j - i >= HOLD:
                stalls.append((slot, sub[i][0], sub[j - 1][0], lo, hi, sub[i][1]))
                i = j
            else:
                i += 1
    print("\nSTALLS: %d plateaus, %d ticks" % (len(stalls), sum(b - a_ for _s, a_, b, _l, _h, _r in stalls)))
    for s, ta, tb, lo, hi, raw in stalls:
        print("  slot %d ticks %d..%d (%d) spans %d..%d raw %d" % (s, ta, tb, tb - ta, lo, hi, raw))
    res["stalls"] = len(stalls)
    res["stall_ticks"] = sum(b - a_ for _s, a_, b, _l, _h, _r in stalls)

    # ---- buckets -------------------------------------------------------------
    def bucket(raw, norm):
        for (i, F, L, R, c0, c1) in forks:
            if c0 <= raw <= c1:
                return "mouth" if (raw - c0 < 20 or c1 - raw < 8) else "corridor"
            if raw < ring and (F - 4 <= norm <= F + 4 or R - 10 <= norm <= R + 8):
                return "mouth"
        for (i, F, L, R, c0, c1) in forks:
            if raw < ring and F - 8 <= norm <= R + 2:
                return "window"
        return "route"

    ev = collections.Counter()
    inc = collections.Counter()
    for slot, seq in sorted(trk.items()):
        t0, t1 = win[slot]
        prev, last_t = 0, -10 ** 9
        for t, raw, norm, c in seq:
            if t < t0 or t >= t1:
                prev = 0
                continue
            if c != 0:
                b = bucket(raw, norm)
                if prev == 0:
                    ev[b] += 1
                if t - last_t > INCIDENT_GAP:
                    inc[b] += 1
                last_t = t
            prev = c
    tot_ev, tot_inc = sum(ev.values()), sum(inc.values())
    print("\nCONTACT EVENTS / INCIDENTS (0->nonzero; incidents merge contacts <= %d ticks apart)" % INCIDENT_GAP)
    for b in ("corridor", "mouth", "window", "route"):
        print("  %-9s events %4d  incidents %3d" % (b, ev[b], inc[b]))
    print("  %-9s events %4d  incidents %3d" % ("TOTAL", tot_ev, tot_inc))
    res.update(events=tot_ev, incidents=tot_inc, ev=dict(ev), inc=dict(inc))

    # ---- per fork passes ------------------------------------------------------
    print("\nFORK PASSES (cars that drove the corridor)")
    spins = 0
    for (i, F, L, R, c0, c1) in forks:
        rec = {"passes": 0, "clean": 0, "stalled": 0, "spin": 0, "events": 0, "stay": 0}
        for slot, seq in sorted(trk.items()):
            t0, t1 = win[slot]
            sp = dict((x[0], x[1]) for x in mot.get(slot, []))
            inside = [(t, raw, c) for t, raw, norm, c in seq if t0 <= t < t1 and c0 <= raw <= c1]
            if not inside:
                if any(t0 <= t < t1 and F + 1 <= norm <= F + L and raw < ring for t, raw, norm, c in seq):
                    rec["stay"] += 1
                continue
            rec["passes"] += 1
            ta, tb = inside[0][0], inside[-1][0]
            nev = 0
            prevc = 0
            for t, raw, c in inside:
                if c and not prevc:
                    nev += 1
                prevc = c
            v_in = abs(sp.get(ta, 0)) or 1
            early = [abs(sp.get(t, 0)) for t, raw, c in inside if raw - c0 < 24]
            v_min = min(early) if early else v_in
            v_out = abs(sp.get(tb, 0))
            stalled = any(s == slot and pa <= tb + 5 and pb >= ta - 5 for s, pa, pb, _l, _h, _r in stalls)
            spin = (v_min < 0.55 * v_in and v_out < 0.25 * v_in) or any(sp.get(t, 0) < -200 for t, _r, _c in inside)
            rec["events"] += nev
            rec["stalled"] += 1 if stalled else 0
            rec["spin"] += 1 if spin else 0
            rec["clean"] += 1 if (nev == 0 and not stalled) else 0
        spins += rec["spin"]
        res["forks"][i] = rec
        print("  fork %d (F=%d len=%d R=%d): passes %d clean %d stalled %d spin %d events %d stayed-main %d" % (
            i, F, L, R, rec["passes"], rec["clean"], rec["stalled"], rec["spin"], rec["events"], rec["stay"]))
    res["spins"] = spins
    res["passes"] = sum(r["passes"] for r in res["forks"].values())
    res["clean"] = sum(r["clean"] for r in res["forks"].values())
    print("\nSUMMARY finishers=%d stalls=%d stall_ticks=%d events=%d incidents=%d passes=%d clean=%d spins=%d" % (
        res["finishers"], res["stalls"], res["stall_ticks"], tot_ev, tot_inc, res["passes"], res["clean"], spins))
    if a.json:
        json.dump(res, open(a.json, "w"))


if __name__ == "__main__":
    main()
