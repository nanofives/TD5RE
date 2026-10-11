"""Plot spans of a generated STRIP.DAT as lane quads, optionally with a car's trajectory.

  python verify/geo_strip_plot.py --spans 730 775 --spans 2175 2287 [--strip re/assets/levels/level091/STRIP.DAT]
                                  [--car 3 --ticks 2790 2840] [--out log/strip.png] [--zoom X Z HALF_WIDTH]
Rows are labelled with their span index every few spans; type 8 / 9 / 10 / 11 are colour coded. The pose
CSV is log/race_trace_pose.csv (slot's world x, z / 256)."""
import csv, struct, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon

args = sys.argv[1:]
strip = "re/assets/levels/level091/STRIP.DAT"
ranges, car, ticks, out, zoom = [], None, None, "log/strip.png", None
i = 0
while i < len(args):
    a = args[i]
    if a == "--spans": ranges.append((int(args[i + 1]), int(args[i + 2]))); i += 3
    elif a == "--strip": strip = args[i + 1]; i += 2
    elif a == "--car": car = int(args[i + 1]); i += 2
    elif a == "--ticks": ticks = (int(args[i + 1]), int(args[i + 2])); i += 3
    elif a == "--out": out = args[i + 1]; i += 2
    elif a == "--zoom": zoom = (float(args[i + 1]), float(args[i + 2]), float(args[i + 3])); i += 4
    else: i += 1
d = open(strip, "rb").read()
span_off, ring, vtx_off, vtx_n, total = struct.unpack_from("<5I", d, 0)
COL = {8: "tab:red", 9: "tab:green", 10: "tab:purple", 11: "tab:orange"}
fig, ax = plt.subplots(figsize=(13, 13))
xs, zs = [], []
for lo, hi in ranges:
    for s in range(lo, min(hi, total - 1) + 1):
        o = span_off + 24 * s
        t, attr, p0, p1, lvi, rvi, ln, lp, ox, oy, oz = struct.unpack_from("<BBBBHHhhiii", d, o)
        lanes = p1 & 15
        def V(k):
            x, y, z = struct.unpack_from("<hhh", d, vtx_off + 6 * k)
            return ox + x, oz + z
        for j in range(lanes):
            q = [V(lvi + j), V(lvi + j + 1), V(rvi + j + 1), V(rvi + j)]
            ax.add_patch(Polygon(q, closed=True, fill=True, alpha=0.35, fc=COL.get(t, "tab:blue" if (j & 1) == 0 else "tab:cyan"), ec="k", lw=0.4))
            xs += [p[0] for p in q]; zs += [p[1] for p in q]
        if (s - lo) % 4 == 0:
            ax.text(V(lvi + lanes // 2)[0], V(lvi + lanes // 2)[1], str(s), fontsize=6)
if car is not None:
    tx, tz = [], []
    for r in csv.DictReader(open("log/race_trace_pose.csv")):
        if r["stage"] != "post_physics" or int(r["slot"]) != car: continue
        t = int(r["sim_tick"])
        if ticks and not (ticks[0] <= t <= ticks[1]): continue
        tx.append(float(r["world_x"]) / 256); tz.append(float(r["world_z"]) / 256)
    ax.plot(tx, tz, "-", color="k", lw=1.4)
    ax.plot(tx[:1], tz[:1], "o", color="g"); ax.plot(tx[-1:], tz[-1:], "x", color="r", ms=10)
    xs += tx; zs += tz
if zoom:
    ax.set_xlim(zoom[0] - zoom[2], zoom[0] + zoom[2]); ax.set_ylim(zoom[1] - zoom[2], zoom[1] + zoom[2])
else:
    ax.set_xlim(min(xs) - 3000, max(xs) + 3000); ax.set_ylim(min(zs) - 3000, max(zs) + 3000)
ax.set_aspect("equal")
fig.savefig(out, dpi=85)
print("wrote", out)
