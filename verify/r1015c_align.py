"""[R1015 C item 4] Plan alignment of a procedural facade with the SIDEWALK slab it stands behind.
For every `building` (<=2 cmd) wall quad on floor 0: angle between the wall's bottom edge and the
nearest sidewalk-slab (page 44, flat) edge, and the lateral distance from the wall base to that edge.
"""
import math, os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
UPM = 430.0
def main():
    b, meshes = A.load_level(sys.argv[1])
    G = A.Grid(10.0 * UPM)
    for m in meshes:
        if m["kind"] not in ("road", "branchroad", "city", "cross", "block"): continue
        for typ, page, vs in A.faces(b, m):
            if page != 44: continue
            ys = [v[1] for v in vs]
            if max(ys) - min(ys) > 400: continue
            n = len(vs)
            for i in range(n):
                p, q = vs[i], vs[(i + 1) % n]
                L = math.hypot(q[0] - p[0], q[2] - p[2])
                if L < 400: continue
                G.add((p, q, L), [p[0], q[0]], [p[2], q[2]])
    ah = collections.Counter(); dh = collections.Counter(); n = 0
    worst = []
    for m in meshes:
        if m["kind"] != "building" or len(m["cmds"]) > 2: continue
        vs = A.verts(b, m); o = 0
        ymin_m = min(v[1] for v in vs)
        for page, tri, quad in m["cmds"]:
            o += tri * 3
            for q in range(quad):
                P = vs[o:o + 4]; o += 4
                lo = sorted(P, key=lambda p: p[1])[:2]
                if lo[0][1] > ymin_m + 0.8 * UPM or max(p[1] for p in P) - min(p[1] for p in P) < 400: continue
                a, c = lo
                L = math.hypot(c[0] - a[0], c[2] - a[2])
                if L < 400: continue
                mx, mz = (a[0] + c[0]) / 2, (a[2] + c[2]) / 2
                best = None
                for (p, qq, LL) in G.near(mx - 4 * UPM, mx + 4 * UPM, mz - 4 * UPM, mz + 4 * UPM):
                    # distance from wall midpoint to this edge segment
                    dx, dz = qq[0] - p[0], qq[2] - p[2]
                    t = max(0, min(1, ((mx - p[0]) * dx + (mz - p[2]) * dz) / (LL * LL)))
                    d = math.hypot(mx - (p[0] + t * dx), mz - (p[2] + t * dz))
                    if best is None or d < best[0]:
                        best = (d, math.degrees(math.atan2(dz, dx)))
                if best is None or best[0] > 3.0 * UPM: continue
                wa = math.degrees(math.atan2(c[2] - a[2], c[0] - a[0]))
                diff = abs((wa - best[1] + 90) % 180 - 90)
                ah[min(int(diff), 30)] += 1; dh[round(best[0] / UPM * 2) / 2.0] += 1; n += 1
                if diff > 8: worst.append((diff, m["e"], m["s"]))
    print("wall bases matched to a slab edge: %d" % n)
    print("angle wall vs slab edge (deg) ->", dict(sorted(ah.items())))
    print("lateral distance wall base to slab edge (m, .5 steps) ->", dict(sorted(dh.items())))
    worst.sort(reverse=True); print("worst:", worst[:8])
main()
