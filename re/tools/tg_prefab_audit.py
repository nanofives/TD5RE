#!/usr/bin/env python3
"""tg_prefab_audit.py -- which SHIPPED LANDMARK PREFABS are open shells?

Mariano, 2026-10-03: "todavia hay edificios que no tienen lados".

tg_building_audit.py (the MODELS.DAT census) narrowed the surviving
see-through buildings on seed 4172065417 to 9 components, of which two are the
SAME landmark prefab -- texture pages in the TD5_TG_PAGE_LM_BASE block, and
`closed_same_pages=0`, meaning that set piece is open on EVERY instance rather
than in some branch. That points at the prefab TABLE, not at the placement
code, so this tool reads the table directly.

td5_tg_prefab_data.h is GENERATED (re/tools/td5_geomlib.py prefabs) by lifting
geometry out of the shipped L23 level. A piece that only ever faced the road
there can be modelled with no back, which is invisible in the original and very
visible once the auto-track stands it in the open.

Method mirrors tg_building_audit.py so the two numbers mean the same thing:
components by vertex weld, wall directions clustered by unsigned angle with
WALL_TOL, and a component counts as OPEN when it is building-like and no two of
its walls are parallel-and-set-apart by at least DEPTH_MIN.

Usage:  python re/tools/tg_prefab_audit.py [--header PATH] [--json OUT.json]
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from tg_building_audit import (                      # noqa: E402
    DEPTH_MIN, WALL_TOL, newell, DSU,
)

DEFAULT_HEADER = os.path.join(
    HERE, "..", "..", "td5mod", "src", "td5re", "td5_tg_prefab_data.h")


def parse_header(path):
    """-> [ {name, verts:[(x,y,z)], cmds:[(page,tri,quad)], fx, fz, height} ]"""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        src = f.read()

    arrays = {}
    for m in re.finditer(
            r"static const (float|unsigned short) (k_pf\d+_[vc])\[\]\s*=\s*\{(.*?)\};",
            src, re.S):
        body = m.group(3)
        nums = [t for t in re.split(r"[,\s]+", body) if t]
        if m.group(1) == "float":
            arrays[m.group(2)] = [float(t.rstrip("f")) for t in nums]
        else:
            arrays[m.group(2)] = [int(t) for t in nums]

    out = []
    tbl = re.search(r"static const TG_PrefabDef k_tg_prefabs\[\]\s*=\s*\{(.*?)\n\};",
                    src, re.S)
    if not tbl:
        raise SystemExit("could not find k_tg_prefabs[] in %s" % path)
    row = re.compile(
        r'\{\s*"([^"]+)"\s*,\s*(k_pf\d+_v)\s*,\s*k_pf\d+_l\s*,\s*(\d+)\s*,'
        r'\s*(k_pf\d+_c)\s*,\s*(\d+)\s*,\s*([-\d.]+)f\s*,\s*([-\d.]+)f\s*,'
        r'\s*([-\d.]+)f\s*\}')
    for m in row.finditer(tbl.group(1)):
        name, vsym, nv, csym, ncmd, fx, fz, h = m.groups()
        v = arrays[vsym]
        c = arrays[csym]
        verts = [(v[i * 5 + 0], v[i * 5 + 1], v[i * 5 + 2]) for i in range(int(nv))]
        cmds = [(c[i * 3 + 0], c[i * 3 + 1], c[i * 3 + 2]) for i in range(int(ncmd))]
        out.append({"name": name, "verts": verts, "cmds": cmds,
                    "fx": float(fx), "fz": float(fz), "height": float(h)})
    return out


def faces_of(pf):
    """Vertices are consumed sequentially, tris before quads, per command --
    the MODELS.DAT rule the whole shipped corpus obeys."""
    verts, cur = pf["verts"], 0
    for (page, ntri, nquad) in pf["cmds"]:
        for _ in range(ntri):
            if cur + 3 > len(verts):
                return
            yield page, [verts[cur + k] for k in range(3)]
            cur += 3
        for _ in range(nquad):
            if cur + 4 > len(verts):
                return
            yield page, [verts[cur + k] for k in range(4)]
            cur += 4


def analyse(pf, weld, minarea, minheight):
    tagged = list(faces_of(pf))
    faces = [t[1] for t in tagged]
    pages = [t[0] for t in tagged]
    inv = 1.0 / weld
    dsu = DSU()
    seen = {}
    for fi, poly in enumerate(faces):
        dsu.find(fi)
        for (x, y, z) in poly:
            k = (int(round(x * inv)), int(round(y * inv)), int(round(z * inv)))
            if k in seen:
                dsu.union(fi, seen[k])
            else:
                seen[k] = fi

    comps = defaultdict(list)
    for fi in range(len(faces)):
        comps[dsu.find(fi)].append(fi)

    recs = []
    for fis in comps.values():
        walls_f, roof, wall_area = [], 0, 0.0
        ys, xs, zs = [], [], []
        pagecount = defaultdict(float)
        for fi in fis:
            poly = faces[fi]
            for (x, y, z) in poly:
                xs.append(x); ys.append(y); zs.append(z)
            nrm, area = newell(poly)
            if area < minarea:
                continue
            if abs(nrm[1]) >= 0.5:
                roof = 1
            else:
                wall_area += area
                pagecount[pages[fi]] += area
                walls_f.append((math.atan2(nrm[2], nrm[0]) % math.pi,
                                sum(v[0] for v in poly) / len(poly),
                                sum(v[2] for v in poly) / len(poly)))
        if not ys:
            continue
        height = max(ys) - min(ys)
        clusters = []
        for ang, cx, cz in walls_f:
            for c in clusters:
                d = abs(ang - c[0])
                if min(d, math.pi - d) <= WALL_TOL:
                    c[1].append((cx, cz))
                    break
            else:
                clusters.append([ang, [(cx, cz)]])

        opposed = False
        for ang, pts in clusters:
            if len(pts) < 2:
                continue
            ux, uz = math.cos(ang), math.sin(ang)
            off = [x * ux + z * uz for (x, z) in pts]
            if (max(off) - min(off)) >= DEPTH_MIN:
                opposed = True
                break

        building_like = (len(clusters) >= 1 and height >= minheight
                         and len(fis) >= 4
                         and wall_area >= 20.0 * 411.0 * 411.0)
        recs.append({
            "faces": len(fis), "walls": len(clusters), "roof": roof,
            "opposed": opposed, "building_like": building_like,
            "height_m": round(height / 411.0, 1),
            "foot_m": [round((max(xs) - min(xs)) / 411.0, 1),
                       round((max(zs) - min(zs)) / 411.0, 1)],
            "wall_area_m2": round(wall_area / (411.0 * 411.0), 1),
            "bbox": (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)),
            "fis": fis,
            "page": (max(pagecount.items(), key=lambda t: t[1])[0]
                     if pagecount else 0),
        })
    return recs, faces


M = 411.0

# A bbox plane counts as ALREADY WALLED when the component has a face roughly
# in it, facing roughly along its axis. Generous on purpose: emitting a second
# quad coplanar with one the prefab already has would z-fight, which looks far
# worse than the hole it was meant to close.
FACE_TOL_FRAC = 0.12
FACE_TOL_MIN = 0.5 * M
AXIS_DOT = 0.7


def close_quads(rec, faces, minarea):
    """Quads that complete this open component's bounding box.

    Only the sides and the roof: the floor is never visible (the piece sits on
    the ground) and emitting it would double every landmark's footprint cost
    for nothing. Each missing plane is filled from the component's OWN bbox, so
    the closure is the piece's real size rather than an invented one, and it
    takes the component's dominant texture page so the new wall reads as part
    of the same building."""
    # Box the WALLS, not the whole component. A vertex-weld island drags in
    # whatever happens to touch it -- low aprons, ground flaps, a pavement
    # skirt -- and those are flat, far-reaching and nowhere near the massing.
    # Boxing the full component bbox made lm12's closure 21.7 x 17.8 m around a
    # building whose walls span 5.5 x 14.2 m, and the resulting slabs buried
    # the landmark completely (verified by rendering the mesh, yaw 0 and 270
    # came out as blank panels). Vertical faces alone describe the body.
    wx, wy, wz = [], [], []
    for fi in rec["fis"]:
        poly = faces[fi]
        nrm, area = newell(poly)
        if area < minarea or abs(nrm[1]) >= 0.5:
            continue
        for (x, y, z) in poly:
            wx.append(x); wy.append(y); wz.append(z)
    bx0, bx1, _by0, _by1, bz0, bz1 = rec["bbox"]
    if wx:
        x0, x1 = min(wx), max(wx)
        y0, y1 = min(wy), max(wy)
        z0, z1 = min(wz), max(wz)
    else:
        x0, x1, y0, y1, z0, z1 = rec["bbox"]
    # How much of the island's own footprint the walls actually occupy. Near 1
    # the island IS the building and a box round it is the building's box; well
    # below 1 the island reaches far past the massing and no box is right.
    full = (bx1 - bx0) * (bz1 - bz0)
    frac = (((x1 - x0) * (z1 - z0)) / full) if full > 0 else 1.0
    ex, ez = x1 - x0, z1 - z0
    tolx = max(ex * FACE_TOL_FRAC, FACE_TOL_MIN)
    tolz = max(ez * FACE_TOL_FRAC, FACE_TOL_MIN)

    def covered(axis, plane, tol):
        for fi in rec["fis"]:
            poly = faces[fi]
            nrm, area = newell(poly)
            if area < minarea or abs(nrm[axis]) < AXIS_DOT:
                continue
            c = sum(v[axis] for v in poly) / len(poly)
            if abs(c - plane) <= tol:
                return True
        return False

    out = []
    if not covered(0, x0, tolx):
        out.append([(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)])
    if not covered(0, x1, tolx):
        out.append([(x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0)])
    if not covered(2, z0, tolz):
        out.append([(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)])
    if not covered(2, z1, tolz):
        out.append([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)])
    if not rec["roof"]:
        out.append([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)])
    return out, frac


# A synthesised closure is only SOUND when the open component's walls fill most
# of the component's own footprint. Where they do not, the island has reached
# far past the massing -- an apron, a ground flap, a canopy arm -- and a box
# drawn round either extent lands in open air beside or in FRONT of the
# architecture instead of behind it. lm12 is the case that proved this: walls
# spanning 5.5 x 14.2 m inside a 21.7 x 17.8 m island, and rendering the mesh
# showed the closure burying the piece at one yaw and fronting it at another.
# Such a piece is not closed here at all; it is dropped from the selectable
# pool instead, and tg_prefab_fit falls back to extruding the real footprint,
# which is an ordinary documented answer in that function.
SOUND_FOOTPRINT_FRAC = 0.5


def emit_close_header(prefabs, args, path):
    UV = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    quads, ranges, excl, notes = [], [], [], []
    for pf in prefabs:
        recs, faces = analyse(pf, args.weld, args.minarea, args.minheight)
        first, n, drop = len(quads), 0, 0
        pend = []
        for r in recs:
            if not (r["building_like"] and not r["opposed"]):
                continue
            qs, frac = close_quads(r, faces, args.minarea)
            if frac < SOUND_FOOTPRINT_FRAC:
                drop = 1
                notes.append("%s: open component %dx%dm h%dm -- walls fill only "
                             "%d%% of it, closure NOT SOUND, prefab EXCLUDED "
                             "from the pool"
                             % (pf["name"], round(r["foot_m"][0]),
                                round(r["foot_m"][1]), round(r["height_m"]),
                                round(frac * 100)))
                continue
            pend.append((r, qs))
        if drop:
            ranges.append((first, 0))
            excl.append(1)
            continue
        for (r, qs) in pend:
            for q in qs:
                quads.append((r["page"], q))
            n += len(qs)
            notes.append("%s: open component %dx%dm h%dm walls=%d roof=%d "
                         "-> %d closing quad(s) on page_local %d"
                         % (pf["name"], round(r["foot_m"][0]), round(r["foot_m"][1]),
                            round(r["height_m"]), r["walls"], r["roof"],
                            len(qs), r["page"]))
        ranges.append((first, n))
        excl.append(0)

    L = []
    L.append("/* GENERATED by re/tools/tg_prefab_audit.py --emit-close -- do not edit.")
    L.append(" *")
    L.append(" * Closing geometry for the shipped LANDMARK PREFABS that are OPEN SHELLS.")
    L.append(" *")
    L.append(" * td5_tg_prefab_data.h lifts set pieces verbatim out of the shipped L23")
    L.append(" * level. A piece that only ever faced the road there can be modelled with")
    L.append(" * no back and no roof, which is invisible in the original and very visible")
    L.append(" * once the auto-track stands it alone in the open -- Mariano, 2026-10-03,")
    L.append(" * \"todavia hay edificios que no tienen lados\".")
    L.append(" *")
    L.append(" * Each quad completes the BOUNDING BOX of one open component, on the planes")
    L.append(" * that component does not already wall, taking that component's dominant")
    L.append(" * texture page. The floor is never emitted. Coordinates are LOCAL, the same")
    L.append(" * space as td5_tg_prefab_data.h, so they rotate and translate with the piece.")
    L.append(" *")
    for t in notes:
        L.append(" *   " + t)
    L.append(" */")
    L.append("#ifndef TD5_TG_PREFAB_CLOSE_DATA_H")
    L.append("#define TD5_TG_PREFAB_CLOSE_DATA_H")
    L.append("")
    L.append("typedef struct {")
    L.append("    unsigned short page_local;")
    L.append("    float          v[20];      /* 4 verts x (x,y,z,u,v) */")
    L.append("} TG_PrefabCloseQuad;")
    L.append("")
    L.append("static const TG_PrefabCloseQuad k_pfclose_quads[] = {")
    for (page, q) in quads:
        vals = []
        for i, (x, y, z) in enumerate(q):
            u, v = UV[i]
            vals.append("%.1ff,%.1ff,%.1ff,%.5ff,%.5ff" % (x, y, z, u, v))
        L.append("    { %d, { %s } }," % (page, ", ".join(vals)))
    L.append("};")
    L.append("")
    L.append("/* first,count into k_pfclose_quads[], indexed by prefab. */")
    L.append("static const unsigned short k_pfclose[][2] = {")
    for i, (f, n) in enumerate(ranges):
        L.append("    { %d, %d },   /* %s */" % (f, n, prefabs[i]["name"]))
    L.append("};")
    L.append("")
    L.append("/* 1 = this prefab's open part cannot be closed soundly from its own")
    L.append(" * footprint, so it is dropped from the selectable pool instead and the")
    L.append(" * caller extrudes the real footprint. See SOUND_FOOTPRINT_FRAC. */")
    L.append("static const unsigned char k_pfclose_exclude[] = {")
    for i, e in enumerate(excl):
        L.append("    %d,   /* %s */" % (e, prefabs[i]["name"]))
    L.append("};")
    L.append("")
    L.append("#define TD5_TG_PREFAB_CLOSE_QUADS %d" % len(quads))
    L.append("")
    L.append("#endif /* TD5_TG_PREFAB_CLOSE_DATA_H */")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")
    print("wrote %s: %d closing quad(s) across %d prefab(s)"
          % (path, len(quads), sum(1 for _, n in ranges if n)))
    for t in notes:
        print("   " + t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", default=DEFAULT_HEADER)
    ap.add_argument("--weld", type=float, default=1.0)
    ap.add_argument("--minarea", type=float, default=169000.0)
    ap.add_argument("--minheight", type=float, default=822.0)
    ap.add_argument("--json", default=None)
    ap.add_argument("--emit-close", default=None,
                    help="write the closing-geometry header and exit")
    args = ap.parse_args()

    prefabs = parse_header(args.header)
    if args.emit_close:
        emit_close_header(prefabs, args, args.emit_close)
        return
    print("%-11s %5s %6s %6s %5s   %s"
          % ("prefab", "faces", "comps", "OPEN", "roof", "footprint / tallest open"))
    results = []
    for i, pf in enumerate(prefabs):
        recs, _faces = analyse(pf, args.weld, args.minarea, args.minheight)
        openrecs = [r for r in recs if r["building_like"] and not r["opposed"]]
        nface = sum(r["faces"] for r in recs)
        worst = max(openrecs, key=lambda r: r["wall_area_m2"], default=None)
        note = "%.1fx%.1fm h%.1f" % (pf["fx"] / 411.0, pf["fz"] / 411.0,
                                     pf["height"] / 411.0)
        if worst:
            note += "  -> open %sx%sm h%s walls=%d roof=%d %sm2" % (
                worst["foot_m"][0], worst["foot_m"][1], worst["height_m"],
                worst["walls"], worst["roof"], worst["wall_area_m2"])
        print("%-11s %5d %6d %6d %5s   %s"
              % (pf["name"], nface, len(recs), len(openrecs),
                 sum(r["roof"] for r in recs), note))
        results.append({"index": i, "name": pf["name"], "faces": nface,
                        "components": len(recs), "open": len(openrecs),
                        "open_detail": openrecs})
    tot = sum(r["open"] for r in results)
    print("\n%d of %d prefabs have at least one OPEN building-like component "
          "(%d open components in total)"
          % (sum(1 for r in results if r["open"]), len(results), tot))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)
        print("wrote %s" % args.json)


if __name__ == "__main__":
    main()
