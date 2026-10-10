"""[R1015 C item 4] Facade TILT histogram of a generated level.
For every procedural-frontage mesh (MESHTAG kind `building`, <= 2 commands) the wall
quads are vertical rectangles/parallelograms. Measures per wall quad:
  grade  = atan(dy / horizontal length) of its bottom edge  (roofline slope with a vertical wall)
  lean   = angle of the quad's vertical edge from true vertical (a leaning wall)
Prints a histogram over wall quads and the worst meshes.
  python verify/r1015c_tilt.py re/assets/levels/level091 [--top 12]
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
import collections

def main():
    lvl = sys.argv[1]
    top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 12
    b, meshes = A.load_level(lvl)
    gh = collections.Counter(); lh = collections.Counter()
    worst = []
    nq = 0
    for m in meshes:
        if m["kind"] != "building" or len(m["cmds"]) > 2:
            continue
        vs = A.verts(b, m)
        o = 0
        mg = 0.0; ml = 0.0
        for page, tri, quad in m["cmds"]:
            o += tri * 3
            for q in range(quad):
                P = vs[o:o + 4]; o += 4
                ys = [p[1] for p in P]
                if max(ys) - min(ys) < 400: continue          # a roof / floor quad
                # bottom edge = the two lowest verts
                lo = sorted(P, key=lambda p: p[1])[:2]
                hi = sorted(P, key=lambda p: p[1])[2:]
                hl = math.hypot(lo[0][0] - lo[1][0], lo[0][2] - lo[1][2])
                if hl < 200: continue
                grade = math.degrees(math.atan2(abs(lo[0][1] - lo[1][1]), hl))
                # vertical edges: pair each low vert with the nearest-xz high vert
                lean = 0.0
                for l in lo:
                    h = min(hi, key=lambda p: math.hypot(p[0] - l[0], p[2] - l[2]))
                    lean = max(lean, math.degrees(math.atan2(math.hypot(h[0] - l[0], h[2] - l[2]), h[1] - l[1])))
                gh[min(int(grade), 20)] += 1; lh[min(int(lean * 2) / 2.0, 10)] += 1
                nq += 1; mg = max(mg, grade); ml = max(ml, lean)
        worst.append((mg, ml, m["e"], m["s"]))
    print("wall quads: %d" % nq)
    print("roofline grade (deg, floor) -> quads:", dict(sorted(gh.items())))
    print("lean from vertical (deg, 0.5 steps) -> quads:", dict(sorted(lh.items())))
    over = sum(c for g, c in gh.items() if g >= 3)
    print("quads with grade >= 3 deg: %d (%.1f%%)" % (over, 100.0 * over / max(1, nq)))
    worst.sort(reverse=True)
    for g, l, e, s in worst[:top]:
        print("  grade %.1f lean %.2f  e%d s%d" % (g, l, e, s))
main()
