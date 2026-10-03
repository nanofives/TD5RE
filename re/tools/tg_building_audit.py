#!/usr/bin/env python3
"""tg_building_audit.py -- numeric audit of OPEN BUILDING SHELLS in a generated
track's MODELS.DAT.

Mariano, 2026-10-03: "todavia hay edificios que no tienen lados" -- auto-track
buildings still come out with no sides. R23 BUILDING CLOSE reports
`closed > 0`, so SOME emitter is being closed; this tool answers WHICH ones are
not, by number rather than by eye.

Method (read-only; no game, no writes except --out):

  * MODELS.DAT is decoded with re/tools/mesh_tool.py -- the same decoder the
    pack-on-load retirement proved byte-exact over the whole shipped corpus.
  * MESHTAG.BIN (level<NN>/MESHTAG.BIN, written next to MODELS.DAT by
    td5_tg_guard.c) gives the per-(entry, slot) EMITTER KIND (TG_GK_*), so every
    number here is attributed to the code path that produced it instead of to a
    guessed-at shape heuristic. MESHTAG is dev-only; a RELEASE build writes none
    and this tool then falls back to kind "?" for everything.
  * Openness is measured per CONNECTED COMPONENT, not per mesh: the generator
    fuses many separate boxes into one sub-mesh, so a per-mesh watertightness
    number would be meaningless. Components are found by welding vertex
    positions on a grid (--weld, default 1.0 world unit) and union-finding the
    faces that share a welded vertex.
  * For each component:
      - boundary edges  = welded edges used by exactly ONE face
      - open_ratio      = boundary_edges / total_edges
      - walls           = number of distinct horizontal facing directions,
                          binned into 8 x 45 deg sectors, counted over faces
                          whose geometric normal is mostly horizontal
                          (|ny| < 0.5) and whose area is >= --minarea
      - roof            = any face with |ny| >= 0.5
    A closed building box has open_ratio 0.0, walls 4, roof 1.
    A front-wall-only facade has open_ratio 1.0, walls 1, roof 0.

  A component is counted as an OPEN SHELL when it is building-like
  (walls >= 1 and the component is taller than --minheight) and
  walls < 3 -- i.e. you can drive past it and see straight through the side.

Usage:
    python re/tools/tg_building_audit.py LEVELDIR [LEVELDIR ...]
                                         [--weld 1.0] [--minarea 4.0]
                                         [--minheight 3.0] [--json OUT.json]
                                         [--label NAME]

LEVELDIR is a directory holding MODELS.DAT (+ optionally MESHTAG.BIN), e.g.
    re/assets/levels/level090
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mesh_tool as mt              # noqa: E402

# td5_trackgen_internal.h TG_GK_* / td5_tg_guard.c k_guard_kind_name.
GK_NAME = [
    "other", "skirt", "road", "branch-road", "tunnel", "gantry", "end-wall",
    "deck", "water", "coast", "decal", "city", "block", "cross", "flora",
    "park-tree", "terrain", "building", "prop", "rail", "branch-side",
]
MESHTAG_MAGIC = 0x4741544D      # 'MTAG'
MESHTAG_NONE = 0xFF

# Kinds whose meshes are meant to read as a BUILDING (a solid you drive past).
# "city" is the facade-wall authority in td5_tg_city.c, "block" the tall-row
# blocks, "building" the geo/prefab/house emitters. Everything else is either
# ground, road furniture or flora and is reported but not judged.
BUILDING_KINDS = {"city", "block", "building"}

# Two wall faces count as the SAME wall direction within this angle. A back wall
# follows the frontage through a curve (td5_tg_city.c takes its far corners from
# the geometry, not from one lateral), so front and back are routinely ~10 deg
# apart on a bend; anything tighter than this splits a closed box in two.
WALL_TOL = math.radians(25.0)

# How far apart two parallel walls must be before they are a BODY rather than
# one double-sided sheet, in raw world units (411 raw = 1 m).
#
# [J7] This used to be 2.0 m, and the in-component depth test below used
# `minheight` (also 2.0 m) for the same job. Both were wrong, in the same
# direction: a run-end corner prism (tg_facade_push_cap, td5_tg_city.c:1062) is
# a CLOSED six-face box -- outer return, inner return, rear, roof, with the
# frontage grid as its front and the ground as its floor -- but it is only
# `cap_thick` deep, routinely 0.5 to 1.7 m. At a 2.0 m gate every one of those
# closed prisms answered "no depth" and was reported as a see-through building.
# That is where 22 of the first run's 27 "open shells" came from.
#
# 0.25 m is the real question: are these two distinct surfaces, or one quad and
# its back face? The generator submits scenery CULL_NONE and never emits a
# coincident pair, so anything above a weld tolerance is genuine depth.
DEPTH_MIN = 0.25 * 411.0
BACK_MIN = DEPTH_MIN
BACK_MAX = 60.0 * 411.0


def load_meshtag(path):
    """-> (nentries, stride, bytes) or None."""
    try:
        with open(path, "rb") as f:
            hdr = f.read(20)
            if len(hdr) < 20:
                return None
            magic, _ver, _seed, nent, stride = struct.unpack("<5I", hdr)
            if magic != MESHTAG_MAGIC:
                return None
            body = f.read(nent * stride)
            if len(body) != nent * stride:
                return None
            return nent, stride, body
    except OSError:
        return None


def faces_of(mesh):
    """Yield lists of vertex positions, one per face, honouring the sequential
    vertex cursor the renderer uses (tri*3 + quad*4 per command)."""
    verts = mesh["vertices"]
    cur = 0
    for c in mesh["commands"]:
        for _ in range(c["tri"]):
            if cur + 3 > len(verts):
                return
            yield [verts[cur + k]["pos"] for k in range(3)]
            cur += 3
        for _ in range(c["quad"]):
            if cur + 4 > len(verts):
                return
            yield [verts[cur + k]["pos"] for k in range(4)]
            cur += 4


def newell(poly):
    """Geometric normal + twice-area via Newell's method (robust for quads that
    are not perfectly planar, which procedural geometry routinely is not)."""
    nx = ny = nz = 0.0
    n = len(poly)
    for i in range(n):
        ax, ay, az = poly[i]
        bx, by, bz = poly[(i + 1) % n]
        nx += (ay - by) * (az + bz)
        ny += (az - bz) * (ax + bx)
        nz += (ax - bx) * (ay + by)
    mag = math.sqrt(nx * nx + ny * ny + nz * nz)
    if mag <= 1e-9:
        return (0.0, 0.0, 0.0), 0.0
    return (nx / mag, ny / mag, nz / mag), mag * 0.5


class DSU:
    def __init__(self):
        self.p = {}

    def find(self, a):
        p = self.p
        r = a
        while p.get(r, r) != r:
            r = p[r]
        while p.get(a, a) != a:
            a, p[a] = p[a], r
        p.setdefault(r, r)
        return r

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.p[rb] = ra


def audit_level(leveldir, weld, minarea, minheight):
    models = os.path.join(leveldir, "MODELS.DAT")
    with open(models, "rb") as f:
        data = f.read()
    model = mt.decode(data, "models")

    tag = load_meshtag(os.path.join(leveldir, "MESHTAG.BIN"))
    kind_of_mesh = {}           # mesh index -> kind name
    if tag:
        nent, stride, body = tag
        for e, ids in enumerate(model["entries"]):
            if e >= nent:
                break
            for slot, mi in enumerate(ids):
                if slot >= stride:
                    break
                k = body[e * stride + slot]
                if k != MESHTAG_NONE and k < len(GK_NAME):
                    # A mesh shared by several entries keeps the FIRST kind it
                    # was tagged with; building kinds win over "other" so a
                    # shared facade is not hidden behind a generic tag.
                    name = GK_NAME[k]
                    prev = kind_of_mesh.get(mi)
                    if prev is None or (prev not in BUILDING_KINDS
                                        and name in BUILDING_KINDS):
                        kind_of_mesh[mi] = name

    inv = 1.0 / weld
    per_kind = defaultdict(lambda: {
        "components": 0, "open": 0, "closed": 0, "walls_hist": defaultdict(int),
        "open_ratio_sum": 0.0, "roofless": 0,
    })
    worst = []

    page_open = defaultdict(int)
    page_closed = defaultdict(int)
    for mi, mesh in enumerate(model["meshes"]):
        kind = kind_of_mesh.get(mi, "?")
        pages = tuple(sorted({c["texture_page_id"] for c in mesh["commands"]}))
        polys = list(faces_of(mesh))
        if not polys:
            continue

        # Weld every face corner onto a grid so coincident-but-duplicated
        # vertices (the generator emits each face independently) join up.
        keyed = []
        for p in polys:
            keyed.append([(int(round(v[0] * inv)), int(round(v[1] * inv)),
                           int(round(v[2] * inv))) for v in p])

        # Union-find faces that share a welded corner -> connected components.
        dsu = DSU()
        vert_owner = {}
        for fi, kp in enumerate(keyed):
            dsu.find(("f", fi))
            for k in kp:
                if k in vert_owner:
                    dsu.union(("f", vert_owner[k]), ("f", fi))
                else:
                    vert_owner[k] = fi

        comps = defaultdict(list)
        for fi in range(len(keyed)):
            comps[dsu.find(("f", fi))].append(fi)
        mesh_recs = []
        mesh_any = []          # every component, as a candidate BACK wall

        for root, fis in comps.items():
            edge_use = defaultdict(int)
            walls_f = []
            roof = 0
            ymin = float("inf")
            ymax = float("-inf")
            xmin = zmin = float("inf")
            xmax = zmax = float("-inf")
            area_total = 0.0
            wall_area = 0.0
            for fi in fis:
                kp = keyed[fi]
                poly = polys[fi]
                n = len(kp)
                for i in range(n):
                    a, b = kp[i], kp[(i + 1) % n]
                    edge_use[(a, b) if a <= b else (b, a)] += 1
                nrm, area = newell(poly)
                area_total += area
                for v in poly:
                    xmin = min(xmin, v[0]); xmax = max(xmax, v[0])
                    ymin = min(ymin, v[1]); ymax = max(ymax, v[1])
                    zmin = min(zmin, v[2]); zmax = max(zmax, v[2])
                if area < minarea:
                    continue
                if abs(nrm[1]) >= 0.5:
                    roof = 1
                else:
                    wall_area += area
                    # Keep the wall's UNSIGNED line direction (angle mod 180)
                    # and its centroid. Not its facing: the generator does not
                    # choose winding per face (scenery is submitted CULL_NONE,
                    # td5_tg_city.c:288) and the back-wall quad is wound with
                    # the SAME corner order as the front grid, so front and back
                    # share one geometric normal. Anything keyed on facing
                    # cannot tell a closed box from a lone sheet.
                    walls_f.append((math.atan2(nrm[2], nrm[0]) % math.pi,
                                    sum(v[0] for v in poly) / len(poly),
                                    sum(v[2] for v in poly) / len(poly)))

            total_edges = sum(edge_use.values())
            if total_edges == 0:
                continue
            boundary = sum(1 for v in edge_use.values() if v == 1)
            ratio = boundary / float(total_edges)
            height = (ymax - ymin) if ymax > ymin else 0.0

            # Cluster the wall faces by unsigned direction, greedily, with a
            # tolerance. Fixed sectors do not work here: a back wall follows the
            # frontage through a curve, so front and back are routinely ~10 deg
            # apart and a hard bin edge splits a closed box into two "walls".
            clusters = []                     # [[angle, [(cx, cz), ...]], ...]
            for ang, cx, cz in walls_f:
                for c in clusters:
                    d = abs(ang - c[0])
                    if min(d, math.pi - d) <= WALL_TOL:
                        c[1].append((cx, cz))
                        break
                else:
                    clusters.append([ang, [(cx, cz)]])
            walls = len(clusters)

            st = per_kind[kind]
            st["components"] += 1
            st["open_ratio_sum"] += ratio
            st["walls_hist"][walls] += 1
            if not roof:
                st["roofless"] += 1

            # BUILDING-LIKE, by shape rather than by tag alone. The MESHTAG kind
            # is a span-range mark, so TG_GK_BUILDING also covers the verge TREE
            # emitted next to the wall (td5_trackgen.c:3895-3905) and TG_GK_CITY
            # covers pavement slabs. Both are excluded here by demanding real
            # wall area and more than a couple of faces -- a billboard tree is
            # 1-2 quads, a pavement slab is 2 quads with no vertical face.
            building_like = (walls >= 1 and height >= minheight
                             and len(fis) >= 4 and wall_area >= 20.0 * 411.0 * 411.0)
            # SEE-THROUGH test: is there a SECOND wall parallel to and set back
            # from some first wall? That is what makes the thing a body you
            # cannot look through rather than a sheet. Measured by position, not
            # by facing, for the winding reason above. Lateral walls are
            # deliberately omitted on run-interior spans (the abutting neighbour
            # hides them, td5_tg_city.c:2719-2721), so demanding 4 walls would
            # flag that by-design case; having no depth at all never is.
            opposed = False
            for ang, pts in clusters:
                if len(pts) < 2:
                    continue
                # Project every centroid onto ONE canonical direction for the
                # cluster. Projecting each face onto its OWN normal is wrong:
                # opposite-wound faces flip the sign and two walls 5 m apart
                # come out as +351 and -345, i.e. never recognised as a pair.
                ux, uz = math.cos(ang), math.sin(ang)
                off = [x * ux + z * uz for (x, z) in pts]
                # [J7] DEPTH_MIN, not minheight. These are two different
                # questions -- "is this thing tall enough to be a building"
                # and "are its two parallel walls set apart" -- and sharing
                # one 2 m constant between them made every closed prism
                # thinner than 2 m report as see-through. See DEPTH_MIN.
                if (max(off) - min(off)) >= DEPTH_MIN:
                    opposed = True
                    break
            # CANDIDATE BACKERS are every component of the mesh, not just the
            # building-like ones. [J7] The closing walls generated for an open
            # landmark prefab (td5_tg_prefab_close_data.h) are 3 to 5 quads on
            # the open component's bounding box; they do not weld to it, so
            # they form their own small island, and that island is a back wall
            # whether or not it would pass for a building on its own. Judging
            # backers by `building_like` made every such closure invisible to
            # this audit and reported the piece as still open.
            mesh_any.append({"clusters": clusters, "y": (ymin, ymax)})
            if kind in BUILDING_KINDS and building_like:
                mesh_recs.append({
                    "kind": kind, "mesh": mi, "pages": list(pages),
                    "faces": len(fis), "walls": walls, "roof": roof,
                    "opposed": opposed, "clusters": clusters,
                    "y": (ymin, ymax),
                    "open_ratio": round(ratio, 3),
                    "height_m": round(height / 411.0, 1),
                    "footprint_m": [round((xmax - xmin) / 411.0, 1),
                                    round((zmax - zmin) / 411.0, 1)],
                    "area_m2": round(area_total / (411.0 * 411.0), 1),
                })

        # SIBLING BACKING. A component is a vertex-weld island, and the
        # generator emits each face independently -- shipped and generated
        # facades alike are "flat arrays of unconnected quads"
        # (re/tools/td5_geomlib.py). So a building's front grid and its back
        # grid are two ISLANDS unless a roof quad happens to weld them, and
        # judging islands alone counts every such building as see-through.
        # Before flagging, ask whether another island of the SAME MESH supplies
        # a wall parallel to one of this island's walls, set back between
        # BACK_MIN and BACK_MAX, with overlapping height. That is a back wall
        # whoever emitted it.
        for r in mesh_recs:
            if r["opposed"]:
                continue
            for o in mesh_any:
                if o["clusters"] is r["clusters"] or r["opposed"]:
                    continue                       # a wall cannot back itself
                # A backer has to cover MOST OF THE WALL'S HEIGHT. Any overlap
                # at all is far too weak: a prefab mesh carries detail islands
                # (kiosks, canopies, railings) and a 3.9 m kiosk standing 5 m
                # behind a 10.2 m facade would otherwise "back" it while
                # leaving 6.3 m of open wall above -- which is precisely the
                # see-through band you drive past and notice.
                ov = min(r["y"][1], o["y"][1]) - max(r["y"][0], o["y"][0])
                rh = r["y"][1] - r["y"][0]
                if rh <= 0 or ov < 0.6 * rh:
                    continue
                for ang, pts in r["clusters"]:
                    ux, uz = math.cos(ang), math.sin(ang)
                    mine = [x * ux + z * uz for (x, z) in pts]
                    for ang2, pts2 in o["clusters"]:
                        d = abs(ang - ang2)
                        if min(d, math.pi - d) > WALL_TOL:
                            continue
                        theirs = [x * ux + z * uz for (x, z) in pts2]
                        gap = min(abs(a - b) for a in mine for b in theirs)
                        far = max(abs(a - b) for a in mine for b in theirs)
                        if BACK_MIN <= far <= BACK_MAX and gap <= BACK_MAX:
                            r["opposed"] = True
                            break
                    if r["opposed"]:
                        break

        for r in mesh_recs:
            st = per_kind[r["kind"]]
            pg = tuple(r["pages"])
            if r["opposed"]:
                st["closed"] += 1
                page_closed[pg] += 1
            else:
                st["open"] += 1
                page_open[pg] += 1
                worst.append({k: v for k, v in r.items()
                              if k not in ("clusters", "y", "opposed")})

    out = {"level": leveldir, "meshes": len(model["meshes"]),
           "meshtag": bool(tag), "kinds": {}}
    for k, st in sorted(per_kind.items()):
        out["kinds"][k] = {
            "components": st["components"],
            "open_shells": st["open"],
            "closed": st["closed"],
            "roofless": st["roofless"],
            "mean_open_ratio": round(st["open_ratio_sum"] / st["components"], 3),
            "walls_hist": {str(w): c for w, c in sorted(st["walls_hist"].items())},
        }
    # Attribution: the emitter picks the texture page, so grouping the open
    # shells by page set says WHICH code path produced them -- and the closed
    # count on the same page says whether that path is wrong everywhere or only
    # in some branch.
    out_pages = []
    for pg, c in sorted(page_open.items(), key=lambda t: -t[1]):
        out_pages.append({"pages": list(pg), "open": c,
                          "closed_same_pages": page_closed.get(pg, 0)})
    worst.sort(key=lambda w: (-w["area_m2"]))
    out["worst"] = worst[:25]
    out["by_page"] = out_pages[:15]
    out["total_building_components"] = sum(
        out["kinds"][k]["components"] for k in out["kinds"] if k in BUILDING_KINDS)
    out["total_open_shells"] = sum(
        out["kinds"][k]["open_shells"] for k in out["kinds"] if k in BUILDING_KINDS)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("levels", nargs="+")
    ap.add_argument("--weld", type=float, default=1.0)
    # Defaults are in RAW WORLD UNITS. 411 raw per metre
    # (td5_trackgen_internal.h:5226 -- TD5_TG_LANE_WIDTH 1500 over a 3.65 m
    # lane), so minarea 169000 is ~1 m^2 of wall and minheight 822 is 2 m:
    # below that a thing is pavement, kerb or a decal, not a building.
    ap.add_argument("--minarea", type=float, default=169000.0)
    ap.add_argument("--minheight", type=float, default=822.0)
    ap.add_argument("--label", default=None)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    results = []
    for lv in args.levels:
        r = audit_level(lv, args.weld, args.minarea, args.minheight)
        if args.label:
            r["label"] = args.label
        results.append(r)
        print(f"=== {r.get('label', '')} {lv} "
              f"({r['meshes']} meshes, MESHTAG={'yes' if r['meshtag'] else 'NO'}) ===")
        print(f"{'kind':<13}{'comps':>7}{'open':>7}{'closed':>8}"
              f"{'roofless':>10}{'meanOpen':>10}  walls-histogram")
        for k, st in r["kinds"].items():
            mark = "*" if k in BUILDING_KINDS else " "
            print(f"{mark}{k:<12}{st['components']:>7}{st['open_shells']:>7}"
                  f"{st['closed']:>8}{st['roofless']:>10}"
                  f"{st['mean_open_ratio']:>10}  {dict(st['walls_hist'])}")
        print(f"BUILDING-KIND TOTAL: {r['total_open_shells']} open shell(s) "
              f"of {r['total_building_components']} component(s)")
        if r["by_page"]:
            print("open shells by texture page (page set -> open / closed on the same pages):")
            for p in r["by_page"][:8]:
                print(f"  pages={p['pages']}  open={p['open']}  "
                      f"closed_same_pages={p['closed_same_pages']}")
        if r["worst"]:
            print("largest open shells (kind, mesh, faces, walls, roof, height, area):")
            for w in r["worst"][:10]:
                print(f"  {w['kind']:<10} mesh={w['mesh']:<6} faces={w['faces']:<4} "
                      f"walls={w['walls']} roof={w['roof']} h={w['height_m']:<6}m "
                      f"foot={w['footprint_m'][0]}x{w['footprint_m'][1]}m "
                      f"area={w['area_m2']}m2 open={w['open_ratio']}")
        print()

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)
        print(f"wrote {args.json}")


if __name__ == "__main__":
    main()
