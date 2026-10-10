"""geo_r1016k_corr.py -- per-tick view of one car through a fork corridor (round 1016 K).

  python verify/geo_r1016k_corr.py <data-prefix> <STRIP.DAT> <corr_base> <corr_len> <slot> [t_from t_to]

<data-prefix> is the log copy a geo_realfork_run.ps1 run left behind
(log/rf_<tag>_), STRIP.DAT the level's strip. Only the FIRST race of a trace is read: an
AutoRace session starts another race when the first ends, so sim_tick restarts and a
naive reader mixes the two (the baseline of round 1016 K had exactly that trap).
lat = offset from the span centre in half-widths (+ = left), dh = velocity heading
minus span heading (deg), steer in locks (1.0 = +-0x18000), thr = throttle cmd, ls = speed
units per tick (long_speed / 256).
"""
import csv, math, struct, sys

pre, strip, base, flen, slot = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
t_from = int(sys.argv[6]) if len(sys.argv) > 6 else 0
t_to = int(sys.argv[7]) if len(sys.argv) > 7 else 10 ** 9
d = open(strip, "rb").read()
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
    hi = -1
    for r in csv.DictReader(open("%s_race_trace_%s.csv" % (pre, name))):
        if r["stage"] != stage:
            continue
        t = int(r["sim_tick"])
        if t < hi - 50:
            break                       # the second race of the session starts here
        hi = max(hi, t)
        out[(r["slot"], t)] = r
    return out


trk = rd("track", "post_track")
pose = rd("pose", "post_physics")
drv = rd("driver", "post_physics")
mot = rd("motion", "post_physics")
last = -999
for (sl, t), r in sorted(trk.items(), key=lambda kv: kv[0][1]):
    if sl != slot or not (t_from <= t <= t_to):
        continue
    raw = int(r["span_raw"])
    if not (base - 12 <= raw < base + flen + 12):
        continue
    p, dv, m = pose.get((slot, t)), drv.get((slot, t)), mot.get((slot, t))
    if not (p and dv and m):
        continue
    if t % 2 and r["track_contact"] == "0":
        continue
    a, b, hw, ln, typ = geom(raw)
    x = int(p["world_x"]) / 256.0
    z = int(p["world_z"]) / 256.0
    tx, tz = b[0] - a[0], b[1] - a[1]
    length = math.hypot(tx, tz) or 1.0
    tx, tz = tx / length, tz / length
    lat = ((x - a[0]) * tz + (z - a[1]) * -tx) / (hw or 1.0)
    pp = pose.get((slot, t - 2))
    yaw = math.atan2(tx, tz)
    if pp:
        vx = x - int(pp["world_x"]) / 256.0
        vz = z - int(pp["world_z"]) / 256.0
        if vx * vx + vz * vz > 1.0:
            yaw = math.atan2(vx, vz)
    dh = (yaw - math.atan2(tx, tz) + math.pi) % (2 * math.pi) - math.pi
    print("t%-5d raw %-5d k%-4s typ%-2d lat %+5.2f dh %+6.1f steer %+5.2f thr %4s brk %s rs %6s ls %7.0f c%s" % (
        t, raw, (raw - base) if raw >= base else "-", typ, lat, math.degrees(dh),
        int(dv["steering_cmd"]) / 98304.0, dv["throttle_cmd"], dv["brake_flag"],
        m["rear_slip"], int(m["long_speed"]) / 256.0 if abs(int(m["long_speed"])) > 3000 else int(m["long_speed"]),
        r["track_contact"]))
