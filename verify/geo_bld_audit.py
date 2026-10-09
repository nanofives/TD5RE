#!/usr/bin/env python3
"""geo_bld_audit.py -- ROUND 1014 C: offline audit of every BUILDING in a generated
geo level (MODELS.DAT + MESHTAG.BIN), measured against the ROAD and GROUND meshes
of the same file, so the verdict does not depend on the generator's own
carriageway/terrain authority (the thing under suspicion).

  python verify/geo_bld_audit.py re/assets/levels/level091 [--json out.json]
  python verify/geo_bld_audit.py LEVEL --dump e,s          # vertex dump of one mesh

Building classes (from MESHTAG kind + command layout):
  real    kind city, first command page 68 (house roof) with triangles, then one
          quad command: tg_geo_emit_one, an OSM/Overture footprint extrusion
  proc    kind building, <= 2 commands: tg_emit_street_wall, the procedural
          frontage (ONE mesh per span, both sides)
  prefab  kind building, >= 3 commands: a stamped shipped set piece

Metrics
  intrusion  overlap area (m^2) of the building's ROOF faces (horizontal faces) with
             the quads of `road` / `branchroad` meshes (race road, the opposite
             carriageway of a divided avenue, a real fork's corridor)
  floating   max over the wall-bottom vertices of (vertex y - ground y), where ground
             is the highest terrain/skirt/road face under the vertex that is not
             above the vertex by more than 1.5 m. > 1 m = floats, < -1 m = buried
  height     top - bottom of the walls, metres
"""
import argparse, collections, json, math, os, struct, sys

HDR, CMD, VTX = 0x38, 16, 44
GK = ["other", "skirt", "road", "branchroad", "tunnel", "gantry", "endwall",
      "deck", "water", "coast", "decal", "city", "block", "cross", "flora",
      "parktree", "terrain", "building", "prop", "rail", "branchside"]
UPM = 430.0


def rd(fmt, b, o):
    return struct.unpack_from(fmt, b, o)


def load_level(lvl):
    b = open(os.path.join(lvl, "MODELS.DAT"), "rb").read()
    tag = open(os.path.join(lvl, "MESHTAG.BIN"), "rb").read()
    magic, ver, seed, nent, stride = rd("<5I", tag, 0)
    (nentries,) = rd("<I", b, 0)
    meshes = []
    for e in range(nentries):
        eoff, esize = rd("<II", b, 4 + e * 8)
        if esize == 0:
            continue
        (nmesh,) = rd("<I", b, eoff)
        for s in range(nmesh):
            (mo,) = rd("<I", b, eoff + 4 + s * 4)
            m = eoff + mo
            _mg, _fl, ncmd, nvtx = rd("<HHII", b, m)
            radius, cx, cy, cz = rd("<ffff", b, m + 12)
            cmdoff = rd("<I", b, m + 0x2C)[0]
            vtxoff = rd("<I", b, m + 0x30)[0]
            cmds = []
            for c in range(ncmd):
                co = m + cmdoff + c * CMD
                _d, page, _z, tri, quad, _z2 = rd("<HHIHHI", b, co)
                cmds.append((page, tri, quad))
            k = 0xFF
            if e < nent and s < stride:
                k = tag[20 + e * stride + s]
            meshes.append(dict(e=e, s=s, nv=nvtx, r=radius, c=(cx, cy, cz),
                               cmds=cmds, moff=m, voff=vtxoff,
                               kind=(GK[k] if k < len(GK) else "kind%d" % k)))
    return b, meshes


def verts(b, m):
    return [rd("<fff", b, m["moff"] + m["voff"] + v * VTX) for v in range(m["nv"])]


def faces(b, m):
    """(typ, page, [(x,y,z)..]) in the writer's order: per command tris then quads."""
    vs = verts(b, m)
    o = 0
    for page, tri, quad in m["cmds"]:
        for _ in range(tri):
            yield "t", page, vs[o:o + 3]; o += 3
        for _ in range(quad):
            yield "q", page, vs[o:o + 4]; o += 4


def poly_area(p):
    a = 0.0
    for i in range(len(p)):
        x0, z0 = p[i]; x1, z1 = p[(i + 1) % len(p)]
        a += x0 * z1 - x1 * z0
    return a * 0.5


def _clip(subject, cp0, cp1):
    def inside(p):
        return (cp1[0] - cp0[0]) * (p[1] - cp0[1]) - (cp1[1] - cp0[1]) * (p[0] - cp0[0]) >= 0
    def inter(a, c):
        x1, y1 = a; x2, y2 = c; x3, y3 = cp0; x4, y4 = cp1
        den = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
        if abs(den) < 1e-12:
            return c
        t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / den
        return (x1 + t * (x2 - x1), y1 + t * (y2 - y1))
    out = []
    for i in range(len(subject)):
        cur = subject[i]; prev = subject[i - 1]
        if inside(cur):
            if not inside(prev):
                out.append(inter(prev, cur))
            out.append(cur)
        elif inside(prev):
            out.append(inter(prev, cur))
    return out


def convex_overlap(a, b):
    """Intersection area of two CONVEX polygons, both CCW."""
    out = a
    for i in range(len(b)):
        if not out:
            return 0.0
        out = _clip(out, b[i], b[(i + 1) % len(b)])
    return abs(poly_area(out)) if len(out) >= 3 else 0.0


def ccw(p):
    return p if poly_area(p) >= 0 else p[::-1]


class Grid:
    def __init__(self, cell):
        self.cell = cell
        self.g = collections.defaultdict(list)

    def add(self, item, xs, zs):
        c = self.cell
        for gx in range(int(min(xs) // c), int(max(xs) // c) + 1):
            for gz in range(int(min(zs) // c), int(max(zs) // c) + 1):
                self.g[(gx, gz)].append(item)

    def near(self, x0, x1, z0, z1):
        c = self.cell
        seen = set(); out = []
        for gx in range(int(x0 // c), int(x1 // c) + 1):
            for gz in range(int(z0 // c), int(z1 // c) + 1):
                for it in self.g.get((gx, gz), ()):
                    if id(it) not in seen:
                        seen.add(id(it)); out.append(it)
        return out


def classify(m):
    if m["kind"] == "city":
        c = m["cmds"]
        if len(c) >= 2 and c[0][0] == 68 and c[0][1] > 0 and c[-1][2] > 0:
            return "real"
    if m["kind"] == "building":
        return "prefab" if len(m["cmds"]) >= 3 else "proc"
    return None


def point_in_tri(px, pz, a, b_, c):
    d1 = (px - b_[0]) * (a[1] - b_[1]) - (a[0] - b_[0]) * (pz - b_[1])
    d2 = (px - c[0]) * (b_[1] - c[1]) - (b_[0] - c[0]) * (pz - c[1])
    d3 = (px - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (pz - a[1])
    neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (neg and pos)


def tri_y(px, pz, A, B, C):
    # barycentric y
    x1, z1, y1 = A[0], A[2], A[1]
    x2, z2, y2 = B[0], B[2], B[1]
    x3, z3, y3 = C[0], C[2], C[1]
    den = (z2 - z3) * (x1 - x3) + (x3 - x2) * (z1 - z3)
    if abs(den) < 1e-9:
        return None
    l1 = ((z2 - z3) * (px - x3) + (x3 - x2) * (pz - z3)) / den
    l2 = ((z3 - z1) * (px - x3) + (x1 - x3) * (pz - z3)) / den
    return l1 * y1 + l2 * y2 + (1 - l1 - l2) * y3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level")
    ap.add_argument("--json")
    ap.add_argument("--dump")
    ap.add_argument("--nofloat", action="store_true")
    ap.add_argument("--roads", default="", help="_route/ROADS.JSON: also measure buildings standing on REAL foreign roads")
    a = ap.parse_args()
    b, meshes = load_level(a.level)
    if a.dump:
        e, s = [int(x) for x in a.dump.split(",")]
        for m in meshes:
            if m["e"] == e and m["s"] == s:
                print(m["kind"], classify(m), "nv", m["nv"], "cmds", m["cmds"],
                      "c", m["c"], "r", m["r"])
                for i, v in enumerate(verts(b, m)):
                    print(i, "%.0f %.0f %.0f" % v)
        return
    kinds = collections.Counter(m["kind"] for m in meshes)
    cls = collections.Counter(classify(m) for m in meshes if classify(m))
    print("%d meshes; classes %s" % (len(meshes), dict(cls)))

    cell = 24.0 * UPM
    roads = Grid(cell)
    ground = Grid(8.0 * UPM)
    for m in meshes:
        if m["kind"] in ("road", "branchroad"):
            for typ, page, vs in faces(b, m):
                p = [(v[0], v[2]) for v in vs]
                if abs(poly_area(p)) < 1.0:
                    continue
                if page == 44:
                    sk = m["kind"] + "-pavement"
                elif page == 2:
                    sk = m["kind"] + "-median"
                elif page == 121:
                    continue            # kerb walls
                else:
                    sk = m["kind"]
                roads.add((sk, ccw(p)), [q[0] for q in p], [q[1] for q in p])
        if (not a.nofloat) and m["kind"] in ("terrain", "skirt", "road", "branchroad"):
            for typ, page, vs in faces(b, m):
                if abs(poly_area([(v[0], v[2]) for v in vs])) < 1.0:
                    continue
                tris = [vs[0:3]] if typ == "t" else [vs[0:3], [vs[0], vs[2], vs[3]]]
                for T in tris:
                    ground.add(tuple(T), [q[0] for q in T], [q[2] for q in T])

    def ground_y(px, pz, ref_y):
        best = None; low = None
        for T in ground.near(px, px, pz, pz):
            if not point_in_tri(px, pz, (T[0][0], T[0][2]), (T[1][0], T[1][2]),
                                (T[2][0], T[2][2])):
                continue
            y = tri_y(px, pz, T[0], T[1], T[2])
            if y is None:
                continue
            if y <= ref_y + 1.5 * UPM and (best is None or y > best):
                best = y
            if low is None or y < low:
                low = y
        return best if best is not None else low

    # ---- real OSM roads (foreign = not within 9 m of the generated race road) ----
    segs = Grid(cell)
    racegrid = Grid(cell)
    nroad_seg = 0
    if a.roads:
        for m in meshes:
            if m["kind"] == "road":
                for typ, page, vs in faces(b, m):
                    if page not in (0, 5, 4):
                        continue
                    xs = [v[0] for v in vs]; zs = [v[2] for v in vs]
                    racegrid.add(vs, xs, zs)
        for w in json.load(open(a.roads, encoding="utf-8"))["roads"]:
            if w.get("class") in ("footway", "path", "steps", "cycleway", "pedestrian", "service"):
                pass
            pts = w.get("points") or []
            hw = max(1, int(w.get("lanes") or 2)) * 1500.0 / 2.0
            for i in range(len(pts) - 1):
                x0, z0, x1, z1 = pts[i]["x"], pts[i]["z"], pts[i + 1]["x"], pts[i + 1]["z"]
                segs.add((x0, z0, x1, z1, hw, w.get("name")), [x0, x1], [z0, z1])
                nroad_seg += 1
        print("%d real road segments loaded" % nroad_seg)

    def near_race(px, pz):
        for vs in racegrid.near(px - 9 * UPM, px + 9 * UPM, pz - 9 * UPM, pz + 9 * UPM):
            for v in vs:
                if math.hypot(v[0] - px, v[2] - pz) < 9.0 * UPM:
                    return True
        return False

    def on_foreign_road(px, pz):
        for (x0, z0, x1, z1, hw, nm) in segs.near(px - 3000, px + 3000, pz - 3000, pz + 3000):
            dx, dz = x1 - x0, z1 - z0
            L2 = dx * dx + dz * dz
            t = 0.0 if L2 < 1e-9 else max(0.0, min(1.0, ((px - x0) * dx + (pz - z0) * dz) / L2))
            d = math.hypot(px - (x0 + t * dx), pz - (z0 + t * dz))
            if d < hw * 0.8:
                return nm or "?"
        return None

    res = collections.defaultdict(list)
    for m in meshes:
        cl = classify(m)
        if not cl:
            continue
        fs = list(faces(b, m))
        if not fs:
            continue
        ys = [v[1] for _t, _p, vs in fs for v in vs]
        top, bot = max(ys), min(ys)
        # horizontal faces = roofs / decks (normal mostly vertical, any height)
        roof = []
        wallb = []   # bottom vertices of vertical faces
        for typ, page, vs in fs:
            p = [(v[0], v[2]) for v in vs]
            ar = abs(poly_area(p))
            ymin = min(v[1] for v in vs); ymax = max(v[1] for v in vs)
            if ar > 400.0 and (ymax - ymin) < 0.35 * math.sqrt(ar) + 1.0:
                roof.append(ccw(p))
            elif ar < 400.0 and (ymax - ymin) > 200.0:
                for v in vs:
                    if abs(v[1] - ymin) < 120.0 and v[1] < bot + 2.5 * UPM:
                        wallb.append(v)
        # intrusion
        inter_area = 0.0; hitk = None
        inter_by = collections.defaultdict(float)
        xs = [v[0] for _t, _p, vs in fs for v in vs]
        zs = [v[2] for _t, _p, vs in fs for v in vs]
        cand = roads.near(min(xs), max(xs), min(zs), max(zs))
        for rp in roof:
            rx = [q[0] for q in rp]; rz = [q[1] for q in rp]
            for kind, poly in cand:
                if (max(q[0] for q in poly) < min(rx) or min(q[0] for q in poly) > max(rx)
                        or max(q[1] for q in poly) < min(rz) or min(q[1] for q in poly) > max(rz)):
                    continue
                ov = convex_overlap(rp, poly) if len(rp) <= 4 and len(poly) <= 4 else 0.0
                if ov > 0:
                    inter_by[kind] += ov / (UPM * UPM)
                    if kind in ("road", "branchroad"):
                        inter_area += ov
                        hitk = hitk or kind
        inter_m2 = inter_area / (UPM * UPM)
        # share of roof sample points standing on a REAL foreign road
        on_road = None
        if a.roads and roof:
            samp = []
            for rp in roof:
                cx0 = sum(q[0] for q in rp) / len(rp); cz0 = sum(q[1] for q in rp) / len(rp)
                samp.append((cx0, cz0))
                for q in rp:
                    samp.append((q[0] * 0.8 + cx0 * 0.2, q[1] * 0.8 + cz0 * 0.2))
                for i in range(len(rp)):
                    j = (i + 1) % len(rp)
                    samp.append(((rp[i][0] + rp[j][0]) / 2 * 0.8 + cx0 * 0.2,
                                 (rp[i][1] + rp[j][1]) / 2 * 0.8 + cz0 * 0.2))
            hit = 0; nm = None
            for (px, pz) in samp:
                n_ = on_foreign_road(px, pz)
                if n_ and not near_race(px, pz):
                    hit += 1; nm = nm or n_
            on_road = (hit / float(len(samp)), nm)
        # floating
        gap_max = gap_min = None
        if not a.nofloat and cl in ("real", "proc", "prefab"):
            seen = set(); gaps = []
            for v in wallb:
                key = (round(v[0]), round(v[2]))
                if key in seen:
                    continue
                seen.add(key)
                g = ground_y(v[0], v[2], v[1])
                if g is not None:
                    gaps.append((v[1] - g) / UPM)
            if gaps:
                gap_max = max(gaps); gap_min = min(gaps)
        res[cl].append(dict(e=m["e"], s=m["s"], inter=inter_m2, hitk=hitk,
                            by=dict(inter_by),
                            h=(top - bot) / UPM, gmax=gap_max, gmin=gap_min,
                            nv=m["nv"], onroad=on_road))
    out = {}
    for cl, rows in sorted(res.items()):
        n = len(rows)
        intr = [r for r in rows if r["inter"] > 2.0]
        print("== %s: %d meshes" % (cl, n))
        print("   intrusion > 2 m2 of road/branchroad: %d (%.1f%%), total %.0f m2"
              % (len(intr), 100.0 * len(intr) / max(1, n), sum(r["inter"] for r in intr)))
        byk = collections.Counter(r["hitk"] for r in intr)
        print("   by surface hit:", dict(byk))
        for sk in ("branchroad-pavement", "road-pavement", "branchroad-median", "road-median"):
            n2 = sum(1 for r in rows if r["by"].get(sk, 0) > 2.0)
            print("   overlap > 2 m2 with %-20s %d" % (sk + ":", n2))
        for r in sorted(intr, key=lambda r: -r["inter"])[:6]:
            print("     e%d s%d  %.1f m2 on %s" % (r["e"], r["s"], r["inter"], r["hitk"]))
        if a.roads:
            bad = [r for r in rows if r["onroad"] and r["onroad"][0] >= 0.25]
            print("   standing on a REAL foreign road (>= 25%% of roof samples): %d (%.1f%%)"
                  % (len(bad), 100.0 * len(bad) / max(1, n)))
            for r in sorted(bad, key=lambda r: -r["onroad"][0])[:8]:
                print("     e%d s%d  %.0f%% on %s" % (r["e"], r["s"], 100 * r["onroad"][0], r["onroad"][1]))
        gm = [r["gmax"] for r in rows if r["gmax"] is not None]
        if gm:
            def hist(vals, edges):
                h = [0] * (len(edges) + 1)
                for v in vals:
                    i = 0
                    while i < len(edges) and v >= edges[i]:
                        i += 1
                    h[i] += 1
                return h
            edges = [-1.0, -0.3, 0.3, 1.0, 2.0, 4.0]
            lab = ["<-1", "-1..-.3", "-.3..+.3", ".3..1", "1..2", "2..4", ">4"]
            print("   max wall-bottom gap above ground (m), n=%d: %s" %
                  (len(gm), "  ".join("%s:%d" % (l, c) for l, c in zip(lab, hist(gm, edges)))))
            fl = [r for r in rows if r["gmax"] is not None and r["gmax"] > 1.0]
            print("   floating > 1 m: %d (%.1f%%)" % (len(fl), 100.0 * len(fl) / len(gm)))
        hs = sorted(r["h"] for r in rows)
        def pc(p): return hs[min(len(hs) - 1, int(len(hs) * p))]
        print("   height m: min %.1f p10 %.1f p50 %.1f p90 %.1f max %.1f" %
              (hs[0], pc(.1), pc(.5), pc(.9), hs[-1]))
        out[cl] = rows
    if a.json:
        json.dump(out, open(a.json, "w"))


if __name__ == "__main__":
    main()
