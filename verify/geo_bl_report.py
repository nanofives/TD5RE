"""Block-loop pass report for one AI race (log/race_trace_*.csv, or a run dir).

  python verify/geo_bl_report.py [log_dir] [--loop LO HI D A]   (corridor span range, departure / arrival main spans)

Default: reads the corridor range from race.log ("side link: loop 0 D=.. A=.. corridor lo..hi").
Per slot and per pass through the loop: tick in / out, duration, slowest speed (units/tick), whether the car
came out on the main road past A, and whether it stalled (speed < 25 units/tick for 60+ ticks)."""
import csv, os, re, sys

args = sys.argv[1:]
d = "log"
loop = None
i = 0
while i < len(args):
    if args[i] == "--loop":
        loop = tuple(int(x) for x in args[i + 1:i + 5]); i += 5
    else:
        d = args[i]; i += 1

if loop is None:
    loops = []
    for ln in open(os.path.join(d, "race.log"), errors="replace"):
        m = re.search(r"side link: loop (\d+) D=(\d+) A=(\d+) corridor (\d+)\.\.(\d+)", ln)
        if m and int(m.group(1)) == len(loops):
            loops.append((int(m.group(4)), int(m.group(5)), int(m.group(2)), int(m.group(3))))
    if not loops:
        print("no side-link loop in race.log"); sys.exit(1)
else:
    loops = [loop]

track = os.path.join(d, "race_trace_track.csv")
motion = os.path.join(d, "race_trace_motion.csv")
spd = {}
if os.path.exists(motion):
    for r in csv.DictReader(open(motion)):
        if r["stage"] == "post_physics":
            spd[(int(r["sim_tick"]), int(r["slot"]))] = int(r["long_speed"]) / 256.0
rows = {}
first_tick = None
for r in csv.DictReader(open(track)):
    if r["stage"] != "post_physics":
        continue
    try:
        t = int(r["sim_tick"])
    except (ValueError, TypeError):
        continue
    # the first race only: a restarted race begins again at tick 1
    try:
        rows.setdefault(int(r["slot"]), []).append((t, int(r["span_raw"]), int(r["span_accum"])))
    except (ValueError, TypeError):
        continue                                # a row cut short when the game was stopped

for li, (lo, hi, D, A) in enumerate(loops):
    print("loop %d: D=%d A=%d corridor %d..%d" % (li, D, A, lo, hi))
    tot = {"entered": 0, "exited": 0, "stalled": 0}
    for slot in sorted(rows):
        seq = []
        last_t = 0
        for t, sp, acc in rows[slot]:
            if t < last_t:
                break
            last_t = t
            seq.append((t, sp, acc))
        inl = None
        passes = []
        for t, sp, acc in seq:
            if lo <= sp <= hi:
                if inl is None:
                    inl = [t, t, 1e9, 0, 0]
                inl[1] = t
                v = spd.get((t, slot), 0.0)
                inl[2] = min(inl[2], v)
                # stall: consecutive ticks under 25 units/tick
                if v < 25.0:
                    inl[3] += 1
                    inl[4] = max(inl[4], inl[3])
                else:
                    inl[3] = 0
            elif inl is not None:
                inl.append((t, sp))
                passes.append(inl); inl = None
        if inl is not None:
            passes.append(inl)
        for p in passes:
            out = p[5] if len(p) > 5 else None
            ok = out is not None and out[1] >= A - 4 and out[1] < lo
            stall = p[4] >= 60
            tot["entered"] += 1
            tot["exited"] += 1 if ok else 0
            tot["stalled"] += 1 if stall else 0
            print("  slot %d: in %5d  out %s  dur %4d  slowest %5.0f  worst-run<25: %3d  -> %s%s" % (
                slot, p[0], ("%5d (span %d)" % (out[0], out[1])) if out else "  never", p[1] - p[0] + 1,
                p[2] if p[2] < 1e8 else -1, p[4], "OK (back on the road past A)" if ok else "DID NOT REJOIN", "  STALL" if stall else ""))
        if not passes:
            print("  slot %d: stayed on the road" % slot)
    print("  entered %d  rejoined %d  stalled %d" % (tot["entered"], tot["exited"], tot["stalled"]))
