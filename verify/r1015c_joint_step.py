"""[R1015 C item 4] Vertical STEP between consecutive procedural front walls at their joint.
Floor-0 bottom edges of every `building` (<=2 cmd) mesh are chained: for each wall end, the nearest
other wall end within 0.6 m in plan. Step = |y_this_end - y_other_end| in metres. Also the base
slope of each wall vs the slope of the ground chain.
  python verify/r1015c_joint_step.py LEVEL
"""
import math, os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
UPM = 430.0
def main():
    b, meshes = A.load_level(sys.argv[1])
    ends = A.Grid(4 * UPM); E = []
    for m in meshes:
        if m["kind"] != "building" or len(m["cmds"]) > 2: continue
        vs = A.verts(b, m); o = 0; ymin = min(v[1] for v in vs)
        for page, tri, quad in m["cmds"]:
            o += tri * 3
            for q in range(quad):
                P = vs[o:o + 4]; o += 4
                lo = sorted(P, key=lambda p: p[1])[:2]
                if lo[0][1] > ymin + 300 or max(p[1] for p in P) - min(p[1] for p in P) < 400: continue
                a, c = lo
                if math.hypot(a[0] - c[0], a[2] - c[2]) < 400: continue
                for k, (p, qq) in enumerate(((a, c), (c, a))):
                    it = (len(E), m["e"], m["s"], p, qq)
                    E.append(it); ends.add(it, [p[0]], [p[2]])
    hist = collections.Counter(); n = 0; worst = []
    for (i, e, s, p, qq) in E:
        best = None
        for (j, e2, s2, p2, q2) in ends.near(p[0] - 0.6 * UPM, p[0] + 0.6 * UPM, p[2] - 0.6 * UPM, p[2] + 0.6 * UPM):
            if (e2, s2) == (e, s): continue
            d = math.hypot(p[0] - p2[0], p[2] - p2[2])
            if d < 0.6 * UPM and (best is None or d < best[0]): best = (d, abs(p[1] - p2[1]) / UPM, e2, s2)
        if best:
            hist[min(round(best[1] * 10) / 10.0, 3.0)] += 1; n += 1
            if best[1] > 0.3: worst.append((best[1], e, s, best[2], best[3]))
    tot = max(1, n)
    print("joints measured: %d" % n)
    print("vertical step at joint (m, 0.1 steps) ->", dict(sorted(hist.items())))
    big = sum(c for g, c in hist.items() if g >= 0.2)
    print("joints with step >= 0.2 m: %d (%.1f%%)" % (big, 100.0 * big / tot))
    worst.sort(reverse=True); print("worst:", [(round(w[0], 2),) + w[1:] for w in worst[:6]])
main()
