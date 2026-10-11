"""Plot log/bl_graph.csv (TD5RE_GEO_BLOCKLOOP_DUMP=1): the OSM way graph around the route, the route,
the accepted right arms and the graph nodes the planner accepts (ok=1).
  python verify/geo_bl_plot.py [--span A B] [--out file.png]"""
import sys, csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

args = sys.argv[1:]
lo, hi = 0, 10 ** 9
out = "log/bl_graph.png"
i = 0
while i < len(args):
    if args[i] == "--span": lo, hi = int(args[i + 1]), int(args[i + 2]); i += 3
    elif args[i] == "--out": out = args[i + 1]; i += 2
    else: i += 1
R, N, E, A = [], {}, [], []
for row in csv.reader(open("log/bl_graph.csv")):
    if not row: continue
    if row[0] == "R" and row[1] != "i": R.append((int(row[1]), float(row[2]), float(row[3]), float(row[6])))
    elif row[0] == "N" : N[int(row[1])] = (float(row[2]), float(row[3]), float(row[4]), int(row[5]), float(row[6]), int(row[7]))
    elif row[0] == "E": E.append((int(row[1]), int(row[2]), int(row[3])))
    elif row[0] == "A": A.append(row)
rs = [r for r in R if lo <= r[0] <= hi]
x0 = min(r[1] for r in rs) - 80000; x1 = max(r[1] for r in rs) + 80000
z0 = min(r[2] for r in rs) - 80000; z1 = max(r[2] for r in rs) + 80000
fig, ax = plt.subplots(figsize=(14, 14))
for a, b, w in E:
    pa, pb = N[a], N[b]
    if max(pa[0], pb[0]) < x0 or min(pa[0], pb[0]) > x1 or max(pa[1], pb[1]) < z0 or min(pa[1], pb[1]) > z1: continue
    ax.plot([pa[0], pb[0]], [pa[1], pb[1]], "-", color="0.6", lw=0.8)
for k, n in N.items():
    if x0 <= n[0] <= x1 and z0 <= n[1] <= z1:
        ax.plot(n[0], n[1], ".", color=("tab:green" if n[5] else "tab:red"), ms=4)
ax.plot([r[1] for r in rs], [r[2] for r in rs], "-", color="k", lw=2)
for r in rs[::10]: ax.text(r[1], r[2], str(r[0]), fontsize=7)
for a in A:
    si = int(a[2])
    if lo <= si <= hi:
        ax.plot([float(a[7]), float(a[9])], [float(a[8]), float(a[10])], "-", color="tab:blue", lw=3)
        ax.text(float(a[7]), float(a[8]), "e%s si%s" % (a[1], a[2]), color="tab:blue", fontsize=8)
        n = int(a[11])
        if n in N: ax.plot(N[n][0], N[n][1], "o", color="tab:orange", ms=8)
ax.set_xlim(x0, x1); ax.set_ylim(z0, z1); ax.set_aspect("equal")
fig.savefig(out, dpi=90)
print("wrote", out)
