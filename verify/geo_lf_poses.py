"""geo_lf_poses.py -- [1019 V] free-cam tour poses at chosen spans of a generated strip (main or corridor spans).

  python verify/geo_lf_poses.py STRIP.DAT 120,131,1262 [--top] [--back 5500] [--up 1700] [--side 0] > poses.txt

Per span: a chase pose BACK units behind the span's centre (SIDE units toward the left, + = left), UP above it,
looking along the road, and with --top a top-down pose over it. The span's heading comes from the next span (a
corridor's last span / a type-10 span falls back to the previous one)."""
import math, struct, sys
a = [x for x in sys.argv[1:] if not x.startswith("--")]
def opt(n, d):
    return float(sys.argv[sys.argv.index(n) + 1]) if n in sys.argv else d
strip = a[0]; spans = [int(x) for x in a[1].split(",")]
back, up, side = opt("--back", 5500), opt("--up", 1700), opt("--side", 0)
if "--back" in sys.argv: a = [x for x in a if x != str(int(back))]
d = open(strip, "rb").read()
span_off, ring, vtx_off, vtx_n, total = struct.unpack_from("<5I", d, 0)
def mid(s):
    o = span_off + 24 * s
    t, attr, p0, p1, lvi, rvi, ln, lp, ox, oy, oz = struct.unpack_from("<BBBBHHhhiii", d, o)
    lanes = p1 & 15
    def V(k):
        x, y, z = struct.unpack_from("<hhh", d, vtx_off + 6 * k)
        return ox + x, oy + y, oz + z
    l = V(lvi); r = V(lvi + lanes)
    l2 = V(rvi); r2 = V(rvi + lanes)
    return (((l[0] + r[0] + l2[0] + r2[0]) / 4), ((l[1] + r[1] + l2[1] + r2[1]) / 4), ((l[2] + r[2] + l2[2] + r2[2]) / 4))
out = []
for s in spans:
    p = mid(s); q = mid(s + 3); r = mid(s - 3)
    tx, tz = q[0] - r[0], q[2] - r[2]; l = math.hypot(tx, tz) or 1.0; tx /= l; tz /= l
    yaw = math.degrees(math.atan2(tx, tz))
    lx, lz = tz, -tx
    out.append("s%d:%d,%d,%d,%.1f,-12" % (s, p[0] - tx * back + lx * side, p[1] + up, p[2] - tz * back + lz * side, yaw))
    if "--top" in sys.argv:
        out.append("s%d_top:%d,%d,%d,%.1f,-82" % (s, p[0] - tx * 400, p[1] + 9000, p[2] - tz * 400, yaw))
print(";".join(out))
