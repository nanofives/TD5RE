"""geo_r1016k_geom.py -- centre line, width and bend radius of a span range (round 1016 K).

  python verify/geo_r1016k_geom.py <STRIP.DAT> <first_span> <count>

Per span: type, lanes, half width (m), heading (deg), turn to the next span (deg) and the
radius that turn implies over the span pitch (m). 430 units = 1 m.
"""
import math, struct, sys
d = open(sys.argv[1], "rb").read()
first, cnt = int(sys.argv[2]), int(sys.argv[3])
span_off, ring, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", d, 0)


def span(i):
    o = span_off + 24 * i
    lanes = d[o + 3] & 15
    lvi, rvi = struct.unpack_from("<HH", d, o + 4)
    org = struct.unpack_from("<3i", d, o + 12)
    return d[o], lanes, lvi, rvi, org


def row(vi, lanes, org):
    out = []
    for j in range(lanes + 1):
        dx, dy, dz = struct.unpack_from("<3h", d, vtx_off + 6 * (vi + j))
        out.append((org[0] + dx, org[2] + dz))
    return out


def cen(i):
    t, ln, lvi, rvi, org = span(i)
    a, b = row(lvi, ln, org), row(rvi, ln, org)
    ca = ((a[0][0] + a[-1][0]) / 2, (a[0][1] + a[-1][1]) / 2)
    cb = ((b[0][0] + b[-1][0]) / 2, (b[0][1] + b[-1][1]) / 2)
    hw = math.hypot(a[0][0] - a[-1][0], a[0][1] - a[-1][1]) / 2
    return t, ln, ca, cb, hw


prev = None
for i in range(first, first + cnt):
    t, ln, ca, cb, hw = cen(i)
    h = math.degrees(math.atan2(cb[0] - ca[0], cb[1] - ca[1]))
    ln_m = math.hypot(cb[0] - ca[0], cb[1] - ca[1]) / 430.0
    turn = ""
    if prev is not None:
        dh = (h - prev + 180) % 360 - 180
        r = (ln_m / math.radians(abs(dh))) if abs(dh) > 0.05 else 9999
        turn = "turn %+6.1f  R %6.1f m" % (dh, r)
    print("span %-5d t%-2d lanes %d hw %5.2f m  hdg %7.1f  len %4.2f m  %s" % (i, t, ln, hw / 430.0, h, ln_m, turn))
    prev = h
