"""[R1015 C item 4] Gap between a procedural facade's BASE and the pavement under/in front of it.
For each `building` (<=2 cmds) mesh, for each wall quad bottom vertex, find the nearest
SIDEWALK-page (44) top quad point (any kind) within REACH of the vertex, interpolate its y.
Reports gap = wall_base_y - pavement_y (m) and the along-wall slope mismatch.
  python verify/r1015c_pave_gap.py re/assets/levels/level091 [--pick e,s]
"""
import math, os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
UPM = 430.0

def main():
    lvl = sys.argv[1]
    pick = tuple(int(x) for x in sys.argv[sys.argv.index("--pick") + 1].split(",")) if "--pick" in sys.argv else None
    b, meshes = A.load_level(lvl)
    G = A.Grid(8.0 * UPM)
    for m in meshes:
        if m["kind"] not in ("road", "branchroad", "city", "block", "cross"):
            continue
        for typ, page, vs in A.faces(b, m):
            if page != 44:
                continue
            xs = [v[0] for v in vs]; zs = [v[2] for v in vs]; ys = [v[1] for v in vs]
            if max(ys) - min(ys) > 400:     # kerb wall, not a slab top
                continue
            tris = [vs[0:3]] if typ == "t" else [vs[0:3], [vs[0], vs[2], vs[3]]]
            for T in tris:
                G.add(tuple(T), [q[0] for q in T], [q[2] for q in T])
    def pave_y(px, pz, reach):
        best = None
        for T in G.near(px - reach, px + reach, pz - reach, pz + reach):
            if A.point_in_tri(px, pz, (T[0][0], T[0][2]), (T[1][0], T[1][2]), (T[2][0], T[2][2])):
                y = A.tri_y(px, pz, T[0], T[1], T[2])
                if y is not None: return y
        return None
    hist = collections.Counter(); n = 0; none = 0
    rows = []
    for m in meshes:
        if m["kind"] != "building" or len(m["cmds"]) > 2: continue
        if pick and (m["e"], m["s"]) != pick: continue
        vs = A.verts(b, m); o = 0
        gaps = []
        ymin_m = min(v[1] for v in vs)
        for page, tri, quad in m["cmds"]:
            o += tri * 3
            for q in range(quad):
                P = vs[o:o + 4]; o += 4
                ys = [p[1] for p in P]
                if max(ys) - min(ys) < 400: continue
                for p in sorted(P, key=lambda p: p[1])[:2]:
                    if p[1] > ymin_m + 0.8 * UPM: continue          # floor 0 only
                    # step 1.2 m toward the street is unknown; probe the vertex itself, then +-1.2 m on a ring
                    y = None
                    for r in (0, 1.0 * UPM, 2.0 * UPM, 3.0 * UPM):
                        for k in range(8 if r else 1):
                            a = k * math.pi / 4
                            y = pave_y(p[0] + r * math.cos(a), p[2] + r * math.sin(a), 1.0)
                            if y is not None: break
                        if y is not None: break
                    if y is None: none += 1; continue
                    gaps.append((p[1] - y) / UPM)
        if gaps:
            g0 = min(gaps); g1 = max(gaps)
            rows.append((max(abs(g0), abs(g1)), g0, g1, m["e"], m["s"]))
            for g in gaps: hist[round(g * 4) / 4.0] += 1; n += 1
    print("bottom vertices measured: %d (no pavement within 3 m: %d)" % (n, none))
    print("gap histogram (m, 0.25 steps):", dict(sorted(hist.items())))
    rows.sort(reverse=True)
    for r in rows[:12 if not pick else 5]:
        print("  |gap|max %.2f  [%.2f .. %.2f]  e%d s%d" % r)
main()
