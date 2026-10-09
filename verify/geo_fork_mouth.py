"""geo_fork_mouth.py -- what a car does through a fork's corridor mouth (round 1013 F2b).

  pwsh verify/geo_realfork_run.ps1 -Tag t -Seed 55 -Modules "track,driver,motion,progress,pose"
  copy re/assets/levels/level091/STRIP.DAT log/rf_t_STRIP.DAT
  python verify/geo_fork_mouth.py t <corridor_base> <fork_len> <slot> [<slot> ...]

Parses the strip (span records + vertex rows) for the real corridor centreline and, per
tick, prints each car's lateral position in the span's half-widths (+ = left), the
heading of its VELOCITY against the span's heading, steering, rear slip and speed.
That is how the mouth spins were found: every car arrives at corridor step 0 leaning
13..28 degrees toward the main road and goes lock to lock; arrival speed decides who
spins. Heading is the velocity direction (the pose trace's yaw is not in turns of 4096).
"""
import csv
import math
import os
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "log")
tag, base, flen = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
slots = sys.argv[4:]

d = open(os.path.join(ROOT, "rf_%s_STRIP.DAT" % tag), "rb").read()
span_off, ring, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", d, 0)


def span(i):
    o = span_off + 24 * i
    typ = d[o]
    lanes = d[o + 3] & 15
    lvi, rvi = struct.unpack_from("<HH", d, o + 4)
    org = struct.unpack_from("<3i", d, o + 12)
    return typ, lanes, lvi, rvi, org


def row(vi, lanes, org):
    pts = []
    for j in range(lanes + 1):
        dx, dy, dz = struct.unpack_from("<3h", d, vtx_off + 6 * (vi + j))
        pts.append((org[0] + dx, org[2] + dz))
    return pts


def geom(i):
    typ, lanes, lvi, rvi, org = span(i)
    a = row(lvi, lanes, org)
    b = row(rvi, lanes, org)
    ca = ((a[0][0] + a[-1][0]) / 2, (a[0][1] + a[-1][1]) / 2)
    cb = ((b[0][0] + b[-1][0]) / 2, (b[0][1] + b[-1][1]) / 2)
    hw = math.hypot(a[0][0] - a[-1][0], a[0][1] - a[-1][1]) / 2
    return ca, cb, hw, lanes, typ


def rd(name, stage):
    out = {}
    path = os.path.join(ROOT, "rf_%s_race_trace_%s.csv" % (tag, name))
    for r in csv.DictReader(open(path)):
        if r["stage"] == stage:
            out[(r["slot"], int(r["sim_tick"]))] = r
    return out


trk = rd("track", "post_track")
pose = rd("pose", "post_physics")
drv = rd("driver", "post_ai")
mot = rd("motion", "post_physics")

print("corridor spans %d..%d" % (base, base + flen - 1))
print("corridor centreline heading per step (deg, 0 = the heading at step 0), type, width:")
c0, c1, _hw0, _ln0, _t0 = geom(base)
h0 = math.atan2(c1[0] - c0[0], c1[1] - c0[1])
line = []
for k in range(0, min(flen, 24)):
    a, b, hw, ln, typ = geom(base + k)
    h = math.atan2(b[0] - a[0], b[1] - a[1])
    dh = (h - h0 + math.pi) % (2 * math.pi) - math.pi
    line.append("%d:%.1f/t%d/%.1fm" % (k, math.degrees(dh), typ, hw * 2 / 430.0))
print("  ".join(line))

for s in slots:
    print("\n=== slot %s: k = corridor step, lat = offset from the span centre in half-widths "
          "(+ = left), dh = velocity heading minus span heading (deg), steer in locks, "
          "rs = rear slip, ls = units per tick" % s)
    for (sl, t), r in sorted(trk.items(), key=lambda kv: kv[0][1]):
        if sl != s:
            continue
        raw = int(r["span_raw"])
        if not (base - 16 <= raw < base + 20):
            continue
        p, dv, m = pose.get((s, t)), drv.get((s, t)), mot.get((s, t))
        if not (p and dv and m) or t % 2:
            continue
        a, b, hw, ln, typ = geom(raw)
        x = int(p["world_x"]) / 256.0
        z = int(p["world_z"]) / 256.0
        tx, tz = b[0] - a[0], b[1] - a[1]
        length = math.hypot(tx, tz) or 1.0
        tx, tz = tx / length, tz / length
        lat = ((x - a[0]) * tz + (z - a[1]) * -tx) / (hw or 1.0)
        pp = pose.get((s, t - 2))
        yaw = math.atan2(tx, tz)
        if pp:
            vx = x - int(pp["world_x"]) / 256.0
            vz = z - int(pp["world_z"]) / 256.0
            if vx * vx + vz * vz > 1.0:
                yaw = math.atan2(vx, vz)
        dh = (yaw - math.atan2(tx, tz) + math.pi) % (2 * math.pi) - math.pi
        print("  t%-5d raw %-5d k%-3s typ%-2d lat %+5.2f dh %+6.1f steer %+6.2f rs %6d ls %7.0f c%s" % (
            t, raw, (raw - base) if raw >= base else "-", typ, lat, math.degrees(dh),
            int(dv["steering_cmd"]) / 98304.0, int(m["rear_slip"]),
            int(m["long_speed"]) / 256.0, r["track_contact"]))
