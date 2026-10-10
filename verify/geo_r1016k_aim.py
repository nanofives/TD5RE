"""geo_r1016k_aim.py -- where is the AI aiming? (round 1016 K)

  python verify/geo_r1016k_aim.py <data-prefix> <STRIP.DAT> <slot> <span_from> <span_to>

Reads the race.log target_probe lines (slot, span=norm, tspan, route byte, target x/z,
actor x/z -- logged every AI tick) and puts the TARGET in the target span's own frame:
u = 0 at the left rail, 1 at the right rail (the AI's convention), lateral offset in lanes.
Only the first race of the log. The actor's own u is taken in the frame of tspan-4.
"""
import math, re, struct, sys
pre, strip, slot, lo, hi = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
d = open(strip, "rb").read()
span_off, ring, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", d, 0)


def frame(i):
    o = span_off + 24 * i
    t = d[o]; lanes = d[o + 3] & 15
    lvi, rvi = struct.unpack_from("<HH", d, o + 4)
    org = struct.unpack_from("<3i", d, o + 12)
    def pt(vi, j):
        dx, dy, dz = struct.unpack_from("<3h", d, vtx_off + 6 * (vi + j))
        return (org[0] + dx, org[2] + dz)
    L = pt(lvi, 0); R = pt(lvi, lanes)
    return t, lanes, L, R


def u_of(x, z, i):
    t, lanes, L, R = frame(i)
    ex, ez = R[0] - L[0], R[1] - L[1]
    w2 = ex * ex + ez * ez
    return ((x - L[0]) * ex + (z - L[1]) * ez) / w2, lanes, math.sqrt(w2)


pat = re.compile(r"target_probe: slot=%s span=(\d+) lin=(\d+) tspan=(\d+) rb=(\d+) tx=(-?\d+) tz=(-?\d+) dx=(-?\d+) dz=(-?\d+) sel=(\d) actor=\((-?\d+),(-?\d+)\)" % slot)
prev = -1
n = 0
for line in open(pre + "_race.log", encoding="utf-8", errors="replace"):
    m = pat.search(line)
    if not m:
        continue
    span, lin, tspan, rb = int(m[1]), int(m[2]), int(m[3]), int(m[4])
    if span < prev - 20 and n > 50:
        pass
    prev = span
    if not (lo <= span <= hi):
        continue
    tx, tz = int(m[5]) / 256.0, int(m[6]) / 256.0
    ax, az = int(m[10]) / 256.0, int(m[11]) / 256.0
    ut, lanes_t, wt = u_of(tx, tz, tspan)
    ua, _l, _w = u_of(ax, az, tspan)          # the car in the TARGET span's frame (extrapolated)
    ub, _l, _w = u_of(ax, az, max(tspan - 4, 0)) if tspan >= ring else (0.0, 0, 0)
    print("span %-5d tspan %-5d rb %3d sel %s  target u %.2f  car u(in tspan frame) %.2f  car u(in tspan-4 frame) %.2f  (lanes %d, w %.1f m)" % (
        span, tspan, rb, m[9], ut, ua, ub, lanes_t, wt / 430.0))
    n += 1
    if n > 60:
        break
