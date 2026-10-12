#!/usr/bin/env python3
"""[R1021 C items 4+9] Which REAL building extrusions (kind city, first page 68) stand on a
generated side-street carriageway (kind cross meshes: the quads tg_city_emit_crossstreet lays)?

  python verify/r1021c_bld_on_street.py LEVELDIR [--mesh e,s ...] [--json out] [--share 0.5]

A building is ON the street when >= `share` of its plan sample points (ring vertices, edge
midpoints of the convex hull, centroid) lie inside a cross-street quad, the same rule the emitter
uses for a real foreign road (td5_tg_city.c, [ROUND 1014 C]).  Reads only MODELS.DAT + MESHTAG.BIN.
"""
import argparse, collections, json, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A


def hull(pts):
    pts = sorted(set(pts))
    if len(pts) < 3:
        return pts
    def cr(o, a, b): return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lo = []
    for p in pts:
        while len(lo) >= 2 and cr(lo[-2], lo[-1], p) <= 0: lo.pop()
        lo.append(p)
    up = []
    for p in reversed(pts):
        while len(up) >= 2 and cr(up[-2], up[-1], p) <= 0: up.pop()
        up.append(p)
    return lo[:-1] + up[:-1]


def inside_quad(q, x, z):
    sgn = 0
    for i in range(4):
        a, b = q[i], q[(i + 1) % 4]
        c = (b[0] - a[0]) * (z - a[1]) - (b[1] - a[1]) * (x - a[0])
        if abs(c) < 1e-9: continue
        s = 1 if c > 0 else -1
        if sgn == 0: sgn = s
        elif s != sgn: return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level")
    ap.add_argument("--mesh", nargs="*", default=[])
    ap.add_argument("--json")
    ap.add_argument("--share", type=float, default=0.5)
    a = ap.parse_args()
    b, meshes = A.load_level(a.level)
    cell = 20000.0
    grid = collections.defaultdict(list)
    nq = 0
    for m in meshes:
        # the carriageway quads of tg_city_emit_crossstreet: kind city (or cross) on the
        # R4_CROSS lane-marking page (101), cut into span-length pieces by the follow mode
        if m["kind"] not in ("city", "cross") or m["cmds"][0][0] not in (101, 40, 41, 42, 43):
            continue
        for typ, page, vs in A.faces(b, m):
            if len(vs) != 4:
                continue
            q = [(v[0], v[2]) for v in vs[:4]]
            if abs(A.poly_area(q)) < 1.0:
                continue
            # horizontal carriageway quads only (skip kerb / wall faces)
            ys = [v[1] for v in vs[:4]]
            if max(ys) - min(ys) > 0.2 * math.sqrt(abs(A.poly_area(q))):
                continue
            xs = [p[0] for p in q]; zs = [p[1] for p in q]
            for cx in range(int(min(xs) // cell), int(max(xs) // cell) + 1):
                for cz in range(int(min(zs) // cell), int(max(zs) // cell) + 1):
                    grid[(cx, cz)].append(q)
            nq += 1
    # arm pavement slabs (kind block, page 44): the sidewalk the street's corner carries
    pave = collections.defaultdict(list)
    for m in meshes:
        if m["kind"] != "block":
            continue
        for typ, page, vs in A.faces(b, m):
            if page != 44 or len(vs) != 4:
                continue
            q = [(v[0], v[2]) for v in vs[:4]]
            if abs(A.poly_area(q)) < 1.0:
                continue
            ys = [v[1] for v in vs[:4]]
            if max(ys) - min(ys) > 0.2 * math.sqrt(abs(A.poly_area(q))):
                continue
            xs = [p[0] for p in q]; zs = [p[1] for p in q]
            for cx in range(int(min(xs) // cell), int(max(xs) // cell) + 1):
                for cz in range(int(min(zs) // cell), int(max(zs) // cell) + 1):
                    pave[(cx, cz)].append(q)
    out = []
    tot = 0
    allr = []
    def is_box(m):
        """real extrusion, or a street flank / back-row box: kind city, ONE command on a wall-variant page"""
        if A.classify(m) == "real":
            return True
        return (m["kind"] == "city" and len(m["cmds"]) == 1 and 273 <= m["cmds"][0][0] <= 392
                and m["nv"] >= 24 and m["cmds"][0][1] == 0)
    for m in meshes:
        if not is_box(m):
            continue
        tot += 1
        vs = A.verts(b, m)
        pl = sorted(set((round(v[0]), round(v[2])) for v in vs))
        h = hull(pl)
        if len(h) < 3:
            continue
        h = A.ccw(h)
        ar = abs(A.poly_area(h))
        if ar < 1.0:
            continue
        cx = sum(p[0] for p in h) / len(h); cz = sum(p[1] for p in h) / len(h)
        xs = [p[0] for p in h]; zs = [p[1] for p in h]
        seen = set(); ov_s = 0.0; ov_p = 0.0
        for gx in range(int(min(xs) // cell), int(max(xs) // cell) + 1):
            for gz in range(int(min(zs) // cell), int(max(zs) // cell) + 1):
                for q in grid.get((gx, gz), []):
                    if id(q) in seen: continue
                    seen.add(id(q)); ov_s += A.convex_overlap(h, A.ccw(q))
                for q in pave.get((gx, gz), []):
                    if id(q) in seen: continue
                    seen.add(id(q)); ov_p += A.convex_overlap(h, A.ccw(q))
        rec = (m["e"], m["s"], round(ov_s / ar, 2), round(ov_p / ar, 2), round(cx), round(cz), round(ar / 430.0 / 430.0, 1))
        allr.append(rec)
        if ov_s / ar >= a.share:
            out.append(rec)
    # procedural frontage / set pieces: judged per WALL face (the base edge of every vertical quad),
    # because one mesh covers both sides of the road
    wall_on = collections.Counter(); wall_tot = collections.Counter(); wall_hit = []
    for m in meshes:
        cl = A.classify(m)
        if cl not in ("proc", "prefab"):
            continue
        hit_here = 0
        for typ, page, vs in A.faces(b, m):
            if len(vs) != 4:
                continue
            ys = [v[1] for v in vs]
            if max(ys) - min(ys) < 200.0:
                continue                      # horizontal (roof / floor)
            base = sorted(vs, key=lambda v: v[1])[:2]
            if abs(base[0][0] - base[1][0]) + abs(base[0][2] - base[1][2]) < 300.0:
                continue
            wall_tot[cl] += 1
            n_in = 0
            for k in range(1, 6):
                t = k / 6.0
                x = base[0][0] + (base[1][0] - base[0][0]) * t
                z = base[0][2] + (base[1][2] - base[0][2]) * t
                for q in grid.get((int(x // cell), int(z // cell)), []):
                    if inside_quad(q, x, z):
                        n_in += 1; break
            if n_in >= 3:
                wall_on[cl] += 1; hit_here += 1
        if hit_here:
            wall_hit.append((m["e"], m["s"], cl, hit_here))
    print("procedural/prefab walls with >= 3 of 5 base points on a street carriageway: proc %d of %d wall faces (%d meshes), prefab %d of %d"
          % (wall_on["proc"], wall_tot["proc"], sum(1 for w in wall_hit if w[2] == "proc"), wall_on["prefab"], wall_tot["prefab"]))
    for e_s in a.mesh:
        e, s = map(int, e_s.split(","))
        for w in wall_hit:
            if (w[0], w[1]) == (e, s):
                print("  mesh e%d s%d (%s): %d wall face(s) on a street" % (w[0], w[1], w[2], w[3]))
    out.sort(key=lambda r: -r[2])
    for r in allr:
        if (r[0], r[1]) in set(tuple(int(v) for v in x.split(",")) for x in a.mesh):
            print("  e%d s%d: street %.2f pavement %.2f at (%d,%d) area %.1f m2" % r)
    n_any = sum(1 for r in allr if r[2] > 0.0)
    n_pave = sum(1 for r in allr if r[3] >= a.share)
    print("pavement-arm overlap >= %.2f of footprint: %d meshes; any street overlap: %d meshes" % (a.share, n_pave, n_any))
    print("%d cross-street carriageway quads; %d real + flank/back-row box meshes; %d cover >= %.2f of their footprint with a street"
          % (nq, tot, len(out), a.share))
    for o in out[:60]:
        print("  e%d s%d street=%.2f pavement=%.2f at (%d,%d) area %.1f m2" % o)
    if a.json:
        json.dump(out, open(a.json, "w"))


main()
