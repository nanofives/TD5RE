"""geo_fork_validate.py -- judge the forks of a geo_fork_validate.ps1 run-set (round 1016 H).

  python verify/geo_fork_validate.py verify/out/<tag> [--out verify/out/fork_validate.json]

A run-set dir holds fork_s<seed>/ (forks built, every fork choice pinned so both arms
get traffic) and base_s<seed>/ (the SAME seed with the real forks switched off). Each has
log/race.log, log/race_trace_{track,motion,pose,driver,rotation}.csv and STRIP.DAT.

Per fork (a window of the main road + its corridor) the analyzer measures, summed over
the seeds, in BOTH arms over the SAME main-span window:

  entries        cars that drove the corridor / stayed on the main road
  wall events    contact 0 -> non-zero edges inside the window; INCIDENTS merge one car's
                 contacts <= INCIDENT_GAP ticks apart; split by zone: entry mouth, exit
                 mouth, body
  stalls         a car inside the window making <= 2 spans of progress for >= STALL_HOLD ticks
  trapped        a car that entered the window and never left it before the race ended
  spins          the car's own heading flips away from the road (see spin_events)
  off-road       lateral offset beyond OFFROAD_LAT half-widths of the span it is on
  speed          mean forward speed in the window vs the same window with forks off, and
                 vs the mean over the plain main road of the same run

and gives each fork a PASS / WARN / FAIL verdict against THRESHOLDS (below, also in
docs/plans/GEO_FORK_VALIDATION.md). Exit status: 0 = no FAIL, 1 = some fork FAILs.
"""
from __future__ import annotations

import argparse
import collections
import csv
import datetime
import json
import math
import os
import re
import struct
import sys

# ------------------------------------------------------------------------------------
# THRESHOLDS -- the whole judgement lives here. Per-pass = per car crossing the window.
# Counts are summed over every seed of the run-set; a fork must see >= MIN_CORR_ENTRIES
# corridor entries over the run-set or it is UNTESTED (reported WARN: nothing was learned).
# ------------------------------------------------------------------------------------
THRESHOLDS = {
    "min_corridor_entries": 3,      # corridor entries over the whole run-set to count as tested
    "trapped_fail": 1,              # cars that entered the window and never left it: FAIL at >= 1
    "stall_extra_fail": 1,          # stalls in the window beyond the forks-off baseline: FAIL at >= 1
    "spin_extra_fail": 2,           # spins beyond baseline: FAIL at >= 2, WARN at 1
    "spin_extra_warn": 1,
    "incident_per_pass_fail": 0.50,  # extra wall incidents per car-pass vs baseline
    "incident_per_pass_warn": 0.20,
    "mouth_incident_per_pass_fail": 0.34,   # same, entry + exit mouth zones only
    "mouth_incident_per_pass_warn": 0.15,
    "offroad_per_pass_fail": 0.34,   # corridor/window off-road episodes per car-pass beyond baseline
    "offroad_per_pass_warn": 0.10,
    "speed_ratio_fail": 0.55,        # window mean speed / forks-off window mean speed
    "speed_ratio_warn": 0.80,
}
STALL_HOLD = 90        # ticks (3 s at 30 Hz)
STALL_TOL = 2          # spans of progress a plateau may contain
INCIDENT_GAP = 90      # ticks: one car's contacts closer than this are ONE incident
WIDEN = 8              # TD5_TG_BRANCH_WIDEN: the approach a fork window keeps (F-8)
TAIL = 2               # R+2
MOUTH_IN = (-WIDEN, 4)   # entry mouth: F-8 .. F+4
MOUTH_OUT = (-4, TAIL)   # exit mouth:  R-4 .. R+2
OFFROAD_LAT = 1.20     # |lateral| in span half-widths beyond which a car is off the road
OFFROAD_MIN_TICKS = 4
SPIN_DH_DEG = 100.0    # velocity heading vs span heading
SPIN_MIN_TICKS = 6
SPIN_MIN_SPEED = 30.0  # world units per tick x 256 / 256 = u/t: ignore a car that has stopped
START_GRID_TICKS = 0   # trace rows before the cars first move are skipped via t0
FINISH_MARGIN = 100    # spans before ring end: a car at/after ring-FINISH_MARGIN is "at the finish"


# ------------------------------------------------------------------------------------
# parsing
# ------------------------------------------------------------------------------------
FORK_LINE = re.compile(r"trackgen: fork (\d+) (\w+) F=(\d+) len=(\d+) corridor=(\d+)\.\.(\d+) "
                       r"rejoin=(\d+) .*?\(ring=(\d+)\)")
REAL_LINE = re.compile(r'trackgen: \[REAL FORK\] (\d+): (\w+) "([^"]*)" F=(\d+) len=(\d+) R=(\d+) '
                       r"lanes (\d+)\+(\d+)")
PLAZA_LINE = re.compile(r'trackgen: \[PLAZA FORK\] (\d+): "([^"]*)" F=(\d+) len=(\d+) R=(\d+)')


def read_forks(log_path):
    """{F: dict(kind, name, F, R, len, c0, c1, ring)} from the strip's own fork lines."""
    forks, ring = {}, None
    if not os.path.exists(log_path):
        return forks, ring
    names = {}
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = REAL_LINE.search(line)
            if m:
                names[int(m.group(4))] = (m.group(2), m.group(3))
                continue
            m = FORK_LINE.search(line)
            if m:
                i, kind, F, L, c0, c1, R, rg = int(m.group(1)), m.group(2), *map(int, m.groups()[2:])
                ring = rg
                if F in forks:
                    continue
                nm = names.get(F, (kind, ""))
                forks[F] = dict(i=i, kind=nm[0] if nm[0] in ("avenue", "parallel", "plaza") else kind,
                                name=nm[1], F=F, R=R, len=L, c0=c0, c1=c1, ring=rg)
    return forks, ring


def read_csv(path, stage, cols):
    out = collections.defaultdict(list)    # slot -> [(tick, *cols)]
    if not os.path.exists(path):
        return out
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if r.get("stage") != stage:
                continue
            try:
                out[int(r["slot"])].append((int(r["sim_tick"]),) + tuple(int(r[c]) for c in cols))
            except (TypeError, ValueError, KeyError):
                continue
    return out


class Strip:
    """Span centre + half width + heading, from STRIP.DAT (same layout as geo_fork_mouth.py)."""

    def __init__(self, path):
        self.ok = os.path.exists(path)
        if not self.ok:
            return
        self.d = open(path, "rb").read()
        self.span_off, self.ring, self.vtx_off, self.vtx_cnt, self.total = struct.unpack_from("<5I", self.d, 0)
        self._c = {}

    def geom(self, i):
        """(ax, az, bx, bz, half_width) of span i: its centre line a->b."""
        if i in self._c:
            return self._c[i]
        d = self.d
        if i < 0 or (i + 1) * 24 + self.span_off > len(d):
            return None
        o = self.span_off + 24 * i
        lanes = d[o + 3] & 15
        lvi, rvi = struct.unpack_from("<HH", d, o + 4)
        org = struct.unpack_from("<3i", d, o + 12)

        def row(vi):
            pts = []
            for j in range(lanes + 1):
                dx, dy, dz = struct.unpack_from("<3h", d, self.vtx_off + 6 * (vi + j))
                pts.append((org[0] + dx, org[2] + dz))
            return pts
        a, b = row(lvi), row(rvi)
        g = ((a[0][0] + a[-1][0]) / 2, (a[0][1] + a[-1][1]) / 2,
             (b[0][0] + b[-1][0]) / 2, (b[0][1] + b[-1][1]) / 2,
             math.hypot(a[0][0] - a[-1][0], a[0][1] - a[-1][1]) / 2)
        self._c[i] = g
        return g


class Run:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path.rstrip("/\\"))
        self.arm, _, s = self.name.partition("_s")
        self.seed = int(s) if s.isdigit() else 0
        lg = os.path.join(path, "log")
        self.forks, self.ring = read_forks(os.path.join(lg, "race.log"))
        self.track = read_csv(os.path.join(lg, "race_trace_track.csv"), "post_track",
                              ("span_raw", "span_norm", "track_contact"))
        self.motion = read_csv(os.path.join(lg, "race_trace_motion.csv"), "post_physics",
                               ("long_speed", "rear_slip"))
        self.pose = read_csv(os.path.join(lg, "race_trace_pose.csv"), "post_physics",
                             ("world_x", "world_z"))
        self.drv = read_csv(os.path.join(lg, "race_trace_driver.csv"), "post_ai",
                            ("steering_cmd",))
        self.strip = Strip(os.path.join(path, "STRIP.DAT"))
        self.slots = sorted(self.track)
        self.max_tick = max((s[-1][0] for s in self.track.values() if s), default=0)
        self.d_motion = {s: {t: v for t, v, _r in rows} for s, rows in self.motion.items()}
        self.d_pose = {s: {t: (x, z) for t, x, z in rows} for s, rows in self.pose.items()}
        self.window = {}        # slot -> (t0, t1): from first movement to reaching the finish area
        self.finish_norm = None

    def set_finish(self, ring):
        self.finish_norm = max(0, ring - FINISH_MARGIN)
        for slot, seq in self.track.items():
            if not seq:
                continue
            n0 = seq[0][2]
            t0 = next((t for t, _r, n, _c in seq if n != n0), seq[0][0])
            t1 = next((t for t, _r, n, _c in seq if n >= self.finish_norm), seq[-1][0] + 1)
            self.window[slot] = (t0, t1)


# ------------------------------------------------------------------------------------
# per-fork measurement
# ------------------------------------------------------------------------------------
def plateaus(seq, hold, tol):
    """seq = [(tick, raw, norm, contact)] already restricted to the zone. Plateau = >= hold
    consecutive samples whose span_norm stays within tol."""
    out = []
    i, n = 0, len(seq)
    while i < n:
        lo = hi = seq[i][2]
        j = i + 1
        while j < n:
            lo2, hi2 = min(lo, seq[j][2]), max(hi, seq[j][2])
            if hi2 - lo2 > tol:
                break
            lo, hi = lo2, hi2
            j += 1
        if j - i >= hold:
            out.append((seq[i][0], seq[j - 1][0], lo, hi))
            i = j
        else:
            i += 1
    return out


def lateral(run, slot, tick, raw):
    """(lateral offset in half-widths, velocity heading minus span heading in degrees, speed u/t)."""
    st = run.strip
    if not st.ok:
        return None
    p = run.d_pose.get(slot, {}).get(tick)
    pp = run.d_pose.get(slot, {}).get(tick - 2)
    g = st.geom(raw)
    if not p or not g:
        return None
    ax, az, bx, bz, hw = g
    x, z = p[0] / 256.0, p[1] / 256.0
    tx, tz = bx - ax, bz - az
    ln = math.hypot(tx, tz) or 1.0
    tx, tz = tx / ln, tz / ln
    lat = ((x - ax) * tz + (z - az) * -tx) / (hw or 1.0)
    dh, spd = 0.0, 0.0
    if pp:
        vx, vz = x - pp[0] / 256.0, z - pp[1] / 256.0
        spd = math.hypot(vx, vz) / 2.0
        if vx * vx + vz * vz > 1.0:
            yaw = math.atan2(vx, vz)
            dh = math.degrees((yaw - math.atan2(tx, tz) + math.pi) % (2 * math.pi) - math.pi)
    return lat, dh, spd


def zone_filter(fk, raw, norm, ring):
    """Which part of the fork a sample sits in: 'ring' (main road window), 'corr' (corridor) or None."""
    if fk["c0"] <= raw <= fk["c1"]:
        return "corr"
    if raw < ring and fk["F"] - WIDEN <= norm <= fk["R"] + TAIL:
        return "ring"
    return None


def measure(run, fk):
    """All the per-fork numbers for one run (one seed, one arm)."""
    ring = run.ring or run.strip.ring if run.strip.ok else (run.ring or 10**9)
    ring = run.ring or (run.strip.ring if run.strip.ok else 10**9)
    m = dict(passes=0, took=0, stayed=0, trapped=0, trapped_slots=[], stalls=[], events=0, incidents=0,
             mouth_in=0, mouth_out=0, body=0, contact_ticks=0, offroad=0, spins=[], speed_sum=0.0,
             speed_n=0, speed_corr_sum=0.0, speed_corr_n=0, speed_ring_sum=0.0, speed_ring_n=0)
    for slot in run.slots:
        seq = run.track[slot]
        a, b = run.window.get(slot, (0, 10**9))
        entered = exited = False
        took = False
        prev_c = 0
        last_t = -10**9
        zone_rows = []
        in_off = in_spin = 0
        for t, raw, norm, c in seq:
            if t < a or t >= b:
                prev_c = 0
                continue
            z = zone_filter(fk, raw, norm, ring)
            if z is None:
                prev_c = 0
                if entered and norm > fk["R"] + TAIL and raw < ring:
                    exited = True
                in_off = in_spin = 0
                continue
            entered = True
            if z == "corr":
                took = True
            zone_rows.append((t, raw, norm, c))
            if c != 0:
                m["contact_ticks"] += 1
                if prev_c == 0:
                    m["events"] += 1
                    # which part: entry mouth / exit mouth / body, by main-span distance
                    k = norm - fk["F"] if z == "ring" else (raw - fk["c0"])
                    kr = norm - fk["R"] if z == "ring" else (raw - fk["c1"])
                    if (z == "ring" and MOUTH_IN[0] <= k <= MOUTH_IN[1]) or (z == "corr" and k <= MOUTH_IN[1]):
                        m["mouth_in"] += 1
                    elif (z == "ring" and MOUTH_OUT[0] <= kr <= MOUTH_OUT[1]) or (z == "corr" and kr >= MOUTH_OUT[0]):
                        m["mouth_out"] += 1
                    else:
                        m["body"] += 1
                if t - last_t > INCIDENT_GAP:
                    m["incidents"] += 1
                last_t = t
            prev_c = c
            mv = run.d_motion.get(slot, {}).get(t)
            if mv is not None:
                v = mv / 256.0
                m["speed_sum"] += v; m["speed_n"] += 1
                if z == "corr":
                    m["speed_corr_sum"] += v; m["speed_corr_n"] += 1
                else:
                    m["speed_ring_sum"] += v; m["speed_ring_n"] += 1
            lf = lateral(run, slot, t, raw)
            if lf:
                lat, dh, spd = lf
                if abs(lat) > OFFROAD_LAT:
                    in_off += 1
                    if in_off == OFFROAD_MIN_TICKS:
                        m["offroad"] += 1
                else:
                    in_off = 0
                if abs(dh) > SPIN_DH_DEG and spd > SPIN_MIN_SPEED / 256.0 * 256.0 * 0 + 1.0:
                    in_spin += 1
                    if in_spin == SPIN_MIN_TICKS:
                        m["spins"].append((slot, t, raw))
                else:
                    in_spin = 0
        if entered:
            m["passes"] += 1
            if took:
                m["took"] += 1
            else:
                m["stayed"] += 1
            if not exited and not took and zone_rows and zone_rows[-1][2] <= fk["R"] + TAIL \
               and zone_rows[-1][0] < run.max_tick - 5 and zone_rows[-1][2] < run.finish_norm:
                m["trapped"] += 1; m["trapped_slots"].append(slot)
            elif took and zone_rows and zone_rows[-1][0] < run.max_tick - 5 and zone_rows[-1][1] <= fk["c1"] \
                    and zone_rows[-1][2] < run.finish_norm and not exited:
                m["trapped"] += 1; m["trapped_slots"].append(slot)
            for p in plateaus(zone_rows, STALL_HOLD, STALL_TOL):
                m["stalls"].append((slot,) + p)
    return m


def main_road_speed(run, forks):
    """Mean speed over the plain main road: pre-finish, outside every fork window/corridor."""
    ring = run.ring or 10**9
    s = n = 0
    for slot in run.slots:
        a, b = run.window.get(slot, (0, 10**9))
        for t, raw, norm, _c in run.track[slot]:
            if t < a + 60 or t >= b or raw >= ring:
                continue
            if any(fk["F"] - WIDEN - 4 <= norm <= fk["R"] + TAIL + 4 for fk in forks.values()):
                continue
            v = run.d_motion.get(slot, {}).get(t)
            if v is not None:
                s += v / 256.0; n += 1
    return (s / n) if n else 0.0


# ------------------------------------------------------------------------------------
# aggregate + verdict
# ------------------------------------------------------------------------------------
def add(dst, src):
    for k, v in src.items():
        if isinstance(v, list):
            dst.setdefault(k, []).extend(v)
        else:
            dst[k] = dst.get(k, 0) + v


def judge(F, fk, fm, bm, n_seeds, main_speed):
    T = THRESHOLDS
    reasons, verdict = [], "PASS"

    def worse(level, why):
        nonlocal verdict
        order = {"PASS": 0, "UNTESTED": 1, "WARN": 2, "FAIL": 3}
        reasons.append("%s: %s" % (level, why))
        if order[level] > order[verdict]:
            verdict = level

    passes = max(1, fm["passes"])
    if fm["took"] < T["min_corridor_entries"]:
        worse("UNTESTED", "only %d corridor entries over %d seed(s) (need %d)" % (fm["took"], n_seeds, T["min_corridor_entries"]))
    if fm["trapped"] >= T["trapped_fail"] and fm["trapped"] > bm.get("trapped", 0):
        worse("FAIL", "%d car(s) trapped in the window (slots %s), baseline %d" % (fm["trapped"], sorted(set(fm["trapped_slots"])), bm.get("trapped", 0)))
    d_stall = len(fm["stalls"]) - len(bm.get("stalls", []))
    if d_stall >= T["stall_extra_fail"]:
        worse("FAIL", "%d stall(s) vs baseline %d" % (len(fm["stalls"]), len(bm.get("stalls", []))))
    d_spin = len(fm["spins"]) - len(bm.get("spins", []))
    if d_spin >= T["spin_extra_fail"]:
        worse("FAIL", "%d spin(s) vs baseline %d" % (len(fm["spins"]), len(bm.get("spins", []))))
    elif d_spin >= T["spin_extra_warn"]:
        worse("WARN", "%d spin(s) vs baseline %d" % (len(fm["spins"]), len(bm.get("spins", []))))
    bpass = max(1, bm.get("passes", 0))
    d_inc = fm["incidents"] / passes - bm.get("incidents", 0) / bpass
    if d_inc >= T["incident_per_pass_fail"]:
        worse("FAIL", "incidents/pass +%.2f vs baseline (%d/%d vs %d/%d)" % (d_inc, fm["incidents"], passes, bm.get("incidents", 0), bpass))
    elif d_inc >= T["incident_per_pass_warn"]:
        worse("WARN", "incidents/pass +%.2f vs baseline (%d/%d vs %d/%d)" % (d_inc, fm["incidents"], passes, bm.get("incidents", 0), bpass))
    fmo = (fm["mouth_in"] + fm["mouth_out"]) / passes
    bmo = (bm.get("mouth_in", 0) + bm.get("mouth_out", 0)) / bpass
    if fmo - bmo >= T["mouth_incident_per_pass_fail"]:
        worse("FAIL", "mouth wall events/pass +%.2f (in %d out %d vs %d/%d)" % (fmo - bmo, fm["mouth_in"], fm["mouth_out"], bm.get("mouth_in", 0), bm.get("mouth_out", 0)))
    elif fmo - bmo >= T["mouth_incident_per_pass_warn"]:
        worse("WARN", "mouth wall events/pass +%.2f (in %d out %d vs %d/%d)" % (fmo - bmo, fm["mouth_in"], fm["mouth_out"], bm.get("mouth_in", 0), bm.get("mouth_out", 0)))
    d_off = fm["offroad"] / passes - bm.get("offroad", 0) / bpass
    if d_off >= T["offroad_per_pass_fail"]:
        worse("FAIL", "off-road episodes/pass +%.2f (%d vs %d)" % (d_off, fm["offroad"], bm.get("offroad", 0)))
    elif d_off >= T["offroad_per_pass_warn"]:
        worse("WARN", "off-road episodes/pass +%.2f (%d vs %d)" % (d_off, fm["offroad"], bm.get("offroad", 0)))
    fs = fm["speed_sum"] / fm["speed_n"] if fm["speed_n"] else 0.0
    bs = bm.get("speed_sum", 0.0) / bm["speed_n"] if bm.get("speed_n") else 0.0
    ratio = (fs / bs) if bs else None
    if ratio is not None:
        if ratio < T["speed_ratio_fail"]:
            worse("FAIL", "window mean speed %.0f%% of the forks-off window" % (100 * ratio))
        elif ratio < T["speed_ratio_warn"]:
            worse("WARN", "window mean speed %.0f%% of the forks-off window" % (100 * ratio))
    return verdict, reasons, dict(speed=fs, base_speed=bs, ratio=ratio,
                                  main_speed=main_speed)


def fmt_table(rows, head):
    w = [max(len(str(r[i])) for r in [head] + rows) for i in range(len(head))]
    line = lambda r: "  ".join(str(c).ljust(w[i]) for i, c in enumerate(r))
    return "\n".join([line(head), "  ".join("-" * x for x in w)] + [line(r) for r in rows])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("setdir")
    ap.add_argument("--out", default=None)
    ap.add_argument("--finish-margin", type=int, default=FINISH_MARGIN)
    ap.add_argument("--no-json", action="store_true")
    ap.add_argument("--warn-allows", action="store_true", help="WARN forks go on the allow list (default: only PASS)")
    args = ap.parse_args()

    setdir = args.setdir
    meta = {}
    mp = os.path.join(setdir, "meta.json")
    if os.path.exists(mp):
        meta = json.load(open(mp, encoding="utf-8-sig"))
    fork_runs, base_runs = {}, {}
    for d in sorted(os.listdir(setdir)):
        p = os.path.join(setdir, d)
        if not os.path.isdir(p) or not os.path.exists(os.path.join(p, "log", "race_trace_track.csv")):
            continue
        r = Run(p)
        (fork_runs if r.arm == "fork" else base_runs)[r.seed] = r
    if not fork_runs:
        print("no fork_s* runs with a track trace in " + setdir)
        return 2

    # the fork table: from the fork runs (every seed builds the same track)
    forks = {}
    ring = None
    for r in fork_runs.values():
        fs, rg = read_forks(os.path.join(r.path, "log", "race.log"))
        for F, fk in fs.items():
            forks.setdefault(F, fk)
        ring = ring or rg
    if not ring:
        for r in fork_runs.values():
            if r.strip.ok:
                ring = r.strip.ring
    if not ring:
        print("no fork table (race.log has no 'trackgen: fork' line) and no STRIP.DAT: cannot place the windows")
        return 2
    for r in list(fork_runs.values()) + list(base_runs.values()):
        r.ring = ring
        r.set_finish(ring)
        r.finish_norm = max(0, ring - args.finish_margin)
        r.set_finish(ring)
    print("run-set %s: %d fork run(s) %s, %d baseline run(s) %s, ring %d, %d fork(s) built"
          % (setdir, len(fork_runs), sorted(fork_runs), len(base_runs), sorted(base_runs), ring, len(forks)))
    for r in list(fork_runs.values()) + list(base_runs.values()):
        print("  %-10s slots %s  last tick %d  trace rows %d  strip %s"
              % (r.name, r.slots, r.max_tick, sum(len(v) for v in r.track.values()), "yes" if r.strip.ok else "NO"))

    results = []
    for F, fk in sorted(forks.items()):
        fm, bm = {}, {}
        per_seed = {}
        for seed, r in sorted(fork_runs.items()):
            one = measure(r, fk)
            per_seed[seed] = one
            add(fm, one)
        for seed, r in sorted(base_runs.items()):
            # the baseline has no corridor: the window is the ring span alone, same F..R
            bk = dict(fk); bk["c0"] = bk["c1"] = -10**9
            add(bm, measure(r, bk))
        msp = [main_road_speed(r, forks) for r in fork_runs.values()]
        main_speed = sum(msp) / len(msp) if msp else 0.0
        verdict, reasons, sp = judge(F, fk, fm, bm, len(fork_runs), main_speed)
        results.append(dict(F=F, fk=fk, fm=fm, bm=bm, verdict=verdict, reasons=reasons, speed=sp, per_seed=per_seed))

    # ---- the table ---------------------------------------------------------------
    head = ["fork", "kind", "F..R", "len", "corr", "entries(c/m)", "events in/body/out", "incid", "stall", "trap",
            "spin", "offrd", "speed(u/t) f/b/main", "base incid/stall/spin", "verdict"]
    rows = []
    for x in results:
        fk, fm, bm, sp = x["fk"], x["fm"], x["bm"], x["speed"]
        rows.append([
            "%s" % (fk["name"] or fk["kind"])[:26], fk["kind"], "%d..%d" % (fk["F"], fk["R"]), fk["len"],
            "%d..%d" % (fk["c0"], fk["c1"]), "%d/%d" % (fm["took"], fm["stayed"]),
            "%d/%d/%d" % (fm["mouth_in"], fm["body"], fm["mouth_out"]), fm["incidents"], len(fm["stalls"]),
            fm["trapped"], len(fm["spins"]), fm["offroad"],
            "%.0f/%.0f/%.0f" % (sp["speed"], sp["base_speed"], sp["main_speed"]),
            "%d/%d/%d" % (bm.get("incidents", 0), len(bm.get("stalls", [])), len(bm.get("spins", []))),
            x["verdict"]])
    print()
    print(fmt_table(rows, head))
    print()
    for x in results:
        print("fork F=%d %s: %s" % (x["F"], x["fk"]["name"] or x["fk"]["kind"], x["verdict"]))
        for r_ in x["reasons"]:
            print("    " + r_)
        for slot, t0, t1, lo, hi in x["fm"]["stalls"]:
            print("    stall slot %d ticks %d..%d spans %d..%d" % (slot, t0, t1, lo, hi))
        for slot, t, raw in x["fm"]["spins"]:
            print("    spin  slot %d tick %d span_raw %d" % (slot, t, raw))

    worst = "PASS"
    order = {"PASS": 0, "UNTESTED": 1, "WARN": 2, "FAIL": 3}
    for x in results:
        if order[x["verdict"]] > order[worst]:
            worst = x["verdict"]
    print("\nOVERALL %s   (%s)" % (worst, ", ".join("F=%d %s" % (x["F"], x["verdict"]) for x in results)))

    # ---- machine-readable verdict file -------------------------------------------
    if args.out and not args.no_json:
        allow_levels = {"PASS", "WARN"} if args.warn_allows else {"PASS"}
        allow = [x["F"] for x in results if x["verdict"] in allow_levels]
        deny = [x["F"] for x in results if x["verdict"] not in allow_levels]
        doc = dict(
            schema=1,
            generated=datetime.datetime.now().isoformat(timespec="seconds"),
            run_set=os.path.basename(setdir.rstrip("/\\")),
            place=meta.get("place"), track=meta.get("track"), exe=meta.get("exe"),
            seeds=sorted(fork_runs), fast_forward=meta.get("fast_forward"),
            fork_env=meta.get("fork_env"), only=meta.get("only"),
            thresholds=THRESHOLDS,
            overall=worst,
            allow=allow, deny=deny,
            env=dict(TD5RE_GEO_FORK_ALLOW=",".join(str(f) for f in allow) if allow else "-1",
                     TD5RE_GEO_FORK_DENY=",".join(str(f) for f in deny)),
            forks=[dict(
                key="F%d" % x["F"], F=x["F"], R=x["fk"]["R"], len=x["fk"]["len"], kind=x["fk"]["kind"],
                name=x["fk"]["name"], corridor=[x["fk"]["c0"], x["fk"]["c1"]],
                verdict=x["verdict"], reasons=x["reasons"],
                fork={k: (v if not isinstance(v, list) else len(v)) for k, v in x["fm"].items() if k != "trapped_slots"},
                base={k: (v if not isinstance(v, list) else len(v)) for k, v in x["bm"].items() if k != "trapped_slots"},
                speed={k: (None if v is None else round(v, 3)) for k, v in x["speed"].items()},
                stalls=[list(s) for s in x["fm"]["stalls"]],
                spins=[list(s) for s in x["fm"]["spins"]],
            ) for x in results],
        )
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
        print("wrote " + args.out)
    return 1 if worst == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
