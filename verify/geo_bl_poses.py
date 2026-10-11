"""geo_bl_poses.py -- free-cam tour poses along a block-loop corridor, read from the generated STRIP.DAT.

  python verify/geo_bl_poses.py LO HI [N] [--strip path] > poses.txt

N spans (default 8) are picked evenly between LO and HI (the 'corridor LO..HI' of the race.log 'side link: loop' line).
Per span one pose 5500 units behind the span, 1700 up, looking along the corridor, plus a top-down one."""
import math, struct, sys
a = [x for x in sys.argv[1:] if not x.startswith("--")]
strip = "re/assets/levels/level091/STRIP.DAT"
if "--strip" in sys.argv:
    strip = sys.argv[sys.argv.index("--strip") + 1]; a = [x for x in a if x != strip]
lo, hi = int(a[0]), int(a[1]); n = int(a[2]) if len(a) > 2 else 8
d = open(strip, "rb").read()
span_off, ring, vtx_off, vtx_n, total = struct.unpack_from("<5I", d, 0)
def mid(s):
    o = span_off + 24 * s
    t, attr, p0, p1, lvi, rvi, ln, lp, ox, oy, oz = struct.unpack_from("<BBBBHHhhiii", d, o)
    lanes = p1 & 15
    def V(k):
        x, y, z = struct.unpack_from("<hhh", d, vtx_off + 6 * k)
        return ox + x, oy + y, oz + z
    l = V(lvi + lanes // 2); r = V(rvi + lanes // 2)
    return ((l[0] + r[0]) / 2, (l[1] + r[1]) / 2, (l[2] + r[2]) / 2)
out = []
for i in range(n):
    s = lo + 2 + (hi - lo - 6) * i // max(1, n - 1)
    p = mid(s); q = mid(s + 2)
    tx, tz = q[0] - p[0], q[2] - p[2]; l = math.hypot(tx, tz) or 1.0; tx /= l; tz /= l
    yaw = math.degrees(math.atan2(tx, tz))
    out.append("c%d:%d,%d,%d,%.1f,-12" % (s, p[0] - tx * 5500, p[1] + 1700, p[2] - tz * 5500, yaw))
    out.append("c%d_top:%d,%d,%d,%.1f,-82" % (s, p[0] - tx * 400, p[1] + 9000, p[2] - tz * 400, yaw))
print(";".join(out))
