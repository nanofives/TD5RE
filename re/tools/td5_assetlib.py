#!/usr/bin/env python3
"""
td5_assetlib.py -- reuse real GEOMETRY (and its textures) that shipped on other
tracks. Instead of authoring plain textured boxes, lift the actual building/prop
meshes from a source track -- real shapes with wall/window/roof texturing and UVs
already correct -- normalize them into a reusable PROTOTYPE, then stamp instances
of that prototype onto a custom track (translated/rotated), copying the texture
pages the meshes use and remapping the per-command page ids.

Pairs with td5_maptrace (find good source objects via trace_geometry) and
td5_texture_reuse (page-pool merge). Mesh/vertex dicts are the mesh_tool shape,
so instances drop straight into td5_scenery.compose_models / models.bin.

    proto = extract_prototype(assets_root, source_level, cx, cz, radius)
    page_map = build_page_map(target_level_dir, source_level_dir, proto["pages"])
    meshes = instance_prototype(proto, (wx,wy,wz), yaw_deg, page_map)
    # -> td5_scenery.compose_models(model, meshes)

World placement is universal: a source mesh's world vertices = origin/256 + local
pos (mirrors the renderer). Prototypes store meshes in LOCAL space relative to the
selected group's footprint centre + base Y, so instancing is a plain rotate +
translate. Billboards (header tag 1/2) are skipped -- reuse trees via
td5_scenery.make_billboard_mesh; this module is for solid geometry.
"""
from __future__ import annotations

import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def _mod(name):
    import importlib
    return importlib.import_module(name)


def _bounds(points):
    cx = sum(p[0] for p in points) / len(points)
    cy = sum(p[1] for p in points) / len(points)
    cz = sum(p[2] for p in points) / len(points)
    rad = max(math.sqrt((p[0]-cx)**2 + (p[1]-cy)**2 + (p[2]-cz)**2) for p in points) or 1.0
    return [rad, cx, cy, cz]


def _world_verts(mesh):
    ox, oy, oz = mesh["origin"]; inv = 1.0 / 256.0
    return [(ox*inv+v["pos"][0], oy*inv+v["pos"][1], oz*inv+v["pos"][2])
            for v in mesh["vertices"]]


# ---------------------------------------------------------------------------
# Intra-mesh splitting (one sub-mesh -> its separable parts)
# ---------------------------------------------------------------------------
#
# extract_prototype above works at WHOLE SUB-MESH granularity, which is as fine
# as the picker can point: a pick line names `e<entry> s<slot>`, and one slot is
# one sub-mesh. Shipped sub-meshes are routinely fused -- level023 e29 s0 is 29
# commands over 6+ pages, so "the guardrail" and whatever it shares a slot with
# are one indivisible lump to every existing tool.
#
# The format gives us exactly one exact seam and one approximate one:
#   EXACT   -- the per-command texture_page_id. There is no per-face record at
#              all; a face inherits everything from its command, so the page id
#              IS the material. Vertices are de-indexed and consumed by a running
#              cursor, so command k owns a contiguous, unambiguous vertex range.
#   APPROX  -- spatial connectivity. Shipped buildings are flat arrays of
#              unconnected road-facing quads (survey note, td5_trackgen_internal.h
#              :2179), so welding shared positions separates repeated instances
#              (rail segment 1 vs rail segment 2) but will NOT reassemble a
#              facade into one solid. That is a property of the shipped data, not
#              a bug here.

SPLIT_OK = "ok"
SPLIT_CURSOR = "cursor_mismatch"
SPLIT_VPTR = "explicit_vptr"
SPLIT_EMPTY = "empty"


def mesh_faces(mesh):
    """Walk the sequential vertex cursor. Returns (faces, reason).

    faces: [{"cmd", "page", "dispatch", "vi": [vertex indices]}], tris then quads
    per command (the order td5_render_mesh.c and mesh_tool both read).

    The cursor is only trustworthy if it accounts for EVERY vertex, so a mesh
    whose commands do not sum to len(vertices) is REJECTED rather than sliced --
    td5_pick.c:139 bails on the same condition because the stride is then
    unknown, and a silent mis-slice is the worst failure this module can have.
    """
    cmds = mesh.get("commands") or []
    nv = len(mesh.get("vertices") or [])
    if not cmds or not nv:
        return [], SPLIT_EMPTY
    if any(int(c.get("vptr", 0)) != 0 for c in cmds):
        return [], SPLIT_VPTR          # explicit pointers: cursor does not apply
    if sum(int(c["tri"]) * 3 + int(c["quad"]) * 4 for c in cmds) != nv:
        return [], SPLIT_CURSOR
    faces, cur = [], 0
    for ci, c in enumerate(cmds):
        tri, quad = int(c["tri"]), int(c["quad"])
        page, disp = int(c["texture_page_id"]), int(c["dispatch_type"])
        for t in range(tri):
            b = cur + t * 3
            faces.append({"cmd": ci, "page": page, "dispatch": disp,
                          "vi": [b, b + 1, b + 2]})
        qb = cur + tri * 3
        for q in range(quad):
            b = qb + q * 4
            faces.append({"cmd": ci, "page": page, "dispatch": disp,
                          "vi": [b, b + 1, b + 2, b + 3]})
        cur += tri * 3 + quad * 4
    return faces, SPLIT_OK


class _UF:
    def __init__(self, n):
        self.p = list(range(n))

    def find(self, a):
        while self.p[a] != a:
            self.p[a] = self.p[self.p[a]]
            a = self.p[a]
        return a

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.p[rb] = ra


def _cluster_faces(faces, verts, weld):
    """Union-find over faces that share a welded vertex position. `weld` is the
    quantisation cell in render-float world units (the same space the picker
    prints, i.e. raw/256). Returns a list of face-index lists."""
    uf = _UF(len(faces))
    seen = {}
    inv = 1.0 / weld if weld > 0 else 0.0
    for fi, f in enumerate(faces):
        for vi in f["vi"]:
            p = verts[vi]
            key = (round(p[0] * inv), round(p[1] * inv), round(p[2] * inv)) if inv \
                else (p[0], p[1], p[2])
            prev = seen.get(key)
            if prev is None:
                seen[key] = fi
            else:
                uf.union(prev, fi)
    groups = {}
    for fi in range(len(faces)):
        groups.setdefault(uf.find(fi), []).append(fi)
    return list(groups.values())


def _part_from_faces(mesh, faces, fidx, verts):
    """Rebuild one mesh_tool-shaped mesh from a subset of faces, in WORLD space.
    Faces are regrouped into one command per (page, dispatch), tris before quads,
    so the sequential cursor of the result is valid by construction."""
    buckets = {}
    for fi in fidx:
        f = faces[fi]
        buckets.setdefault((f["page"], f["dispatch"]), {"tri": [], "quad": []})[
            "tri" if len(f["vi"]) == 3 else "quad"].append(f)
    cmds, out_v, pts = [], [], []
    for (page, disp), b in sorted(buckets.items()):
        for f in b["tri"] + b["quad"]:
            for vi in f["vi"]:
                src = mesh["vertices"][vi]
                wx, wy, wz = verts[vi]
                out_v.append({"pos": [wx, wy, wz], "view": [0.0, 0.0, 0.0],
                              "light": src["light"], "tex": list(src["tex"]),
                              "proj": [0.0, 0.0]})
                pts.append((wx, wy, wz))
        cmds.append({"dispatch_type": disp, "texture_page_id": page,
                     "reserved_04": 0, "tri": len(b["tri"]), "quad": len(b["quad"]),
                     "vptr": 0})
    return {"render_type": mesh["render_type"], "texture_page_id": 0,
            "bounding": _bounds(pts), "origin": [0.0, 0.0, 0.0], "reserved_28": 0,
            "commands": cmds, "vertices": out_v, "normals": None}


SPLIT_MODES = ("page", "weld", "object")

# A flat slab is anything under this world-Y extent and wider than this in XZ.
# Slabs matter because in `object` mode they are the WRONG thing to merge
# through: measured on level023, plain spatial welding produces one 20..30-page,
# ~24000x12000x29000 blob per entry, because the ground slab physically touches
# the facades, which touch the rails, which touch the next slab. Excluding slabs
# from the merge graph is what turns "one lump per entry" into real objects --
# and it is free, because the plazas we want as their own pieces ARE the slabs.
OBJ_FLAT_Y = 60.0
OBJ_FLAT_XZ = 2000.0
OBJ_GAP = 200.0            # AABB dilation when merging non-flat primitives


def _aabb_of(faces, fidx, verts):
    pts = [verts[vi] for fi in fidx for vi in faces[fi]["vi"]]
    mn = [min(p[i] for p in pts) for i in range(3)]
    mx = [max(p[i] for p in pts) for i in range(3)]
    return mn + mx


def _is_flat(bb):
    return (bb[4] - bb[1]) <= OBJ_FLAT_Y and max(bb[3] - bb[0], bb[5] - bb[2]) > OBJ_FLAT_XZ


def _overlap(a, b, gap):
    return all(a[i] - gap <= b[i + 3] and b[i] - gap <= a[i + 3] for i in range(3))


def _groups_page(faces, verts, weld):
    by = {}
    for fi, f in enumerate(faces):
        by.setdefault(f["page"], []).append(fi)
    out = []
    for _page, fis in sorted(by.items()):
        sub = [faces[i] for i in fis]
        for cl in _cluster_faces(sub, verts, weld):
            out.append([fis[i] for i in cl])
    return out


def _groups_object(faces, verts, weld, gap):
    """Exact page split first, then merge the NON-FLAT primitives that sit on top
    of each other back into whole objects. Flat slabs stay separate."""
    prim = _groups_page(faces, verts, weld)
    boxes = [_aabb_of(faces, g, verts) for g in prim]
    solid = [i for i, b in enumerate(boxes) if not _is_flat(b)]
    uf = _UF(len(prim))
    for ai in range(len(solid)):
        for bi in range(ai + 1, len(solid)):
            i, j = solid[ai], solid[bi]
            if _overlap(boxes[i], boxes[j], gap):
                uf.union(i, j)
    merged = {}
    for i in range(len(prim)):
        merged.setdefault(uf.find(i) if i in set(solid) else ("flat", i), []).extend(prim[i])
    return list(merged.values())


def split_mesh(mesh, weld=1.0, mode="object", gap=OBJ_GAP):
    """Split ONE sub-mesh into its separable parts.

    Returns {"reason": SPLIT_*, "parts": [...]}. Each part is
    {"mesh": <mesh_tool mesh, world space>, "pages": [...], "aabb": [...],
     "nface": n, "extent": [dx,dy,dz]}.

    mode:
      "page"   -- cut on the exact per-command page seam, then cluster spatially
                  inside each page group. The finest honest split, and the one
                  that un-fuses a slot (level023 e29 s0 -> the guardrail pair
                  comes out as its own low ribbon). Shatters facades, which
                  ship as flat arrays of unconnected quads.
      "weld"   -- cluster spatially across pages. Keeps a multi-page building in
                  one piece, but slabs bridge everything: one blob per entry.
      "object" -- page split, then merge overlapping NON-FLAT primitives.
                  The catalogue default.
    """
    if mode not in SPLIT_MODES:
        raise ValueError("mode must be one of %s" % (SPLIT_MODES,))
    faces, reason = mesh_faces(mesh)
    if reason != SPLIT_OK:
        return {"reason": reason, "parts": []}
    verts = _world_verts(mesh)
    if mode == "page":
        groups = _groups_page(faces, verts, weld)
    elif mode == "weld":
        groups = _cluster_faces(faces, verts, weld)
    else:
        groups = _groups_object(faces, verts, weld, gap)

    parts = []
    for fidx in groups:
        m = _part_from_faces(mesh, faces, fidx, verts)
        pts = [v["pos"] for v in m["vertices"]]
        mn = [min(p[i] for p in pts) for i in range(3)]
        mx = [max(p[i] for p in pts) for i in range(3)]
        parts.append({"mesh": m, "pages": sorted({int(c["texture_page_id"])
                                                  for c in m["commands"]}),
                      "aabb": mn + mx, "nface": len(fidx),
                      "extent": [mx[i] - mn[i] for i in range(3)]})
    parts.sort(key=lambda p: -p["nface"])
    return {"reason": SPLIT_OK, "parts": parts}


# ---------------------------------------------------------------------------
# LEVEL-WIDE segmentation (whole objects, not per-sub-mesh pieces)
# ---------------------------------------------------------------------------
#
# split_mesh works inside ONE sub-mesh, which cannot produce whole objects even
# in principle: a landmark can span several sub-meshes, and one sub-mesh can
# hold several unrelated buildings. Measured on level023, its "landmark" objects
# sprawl across 15..28 XZ cells of 4000 units where a single building occupies
# 2..6 -- they are chunks of streetscape, not buildings.
#
# So segmentation has to see the whole track at once, and it has to use the
# TEXTURE ROLE as well as the geometry:
#
#   * GROUND and ROAD primitives are what silently bridge unrelated buildings
#     into one lump -- a pavement slab touches the facade on either side of it.
#     They are 68% of level023's primitives, they are never part of a building,
#     and holding them out of the clustering is what makes the rest work. They
#     become their own plaza / road objects.
#   * FOLIAGE (billboard trees) never belongs to a building either.
#   * What is left -- walls, roofs, signs -- is the building material, and it is
#     agglomerated nearest-pair-first under a hard extent cap so a terrace of
#     touching houses cannot collapse into one object.
#
# There is no ground truth to recover here. The shipped format carries no object
# grouping at all (buildings are flat arrays of unconnected road-facing quads),
# so this is a reconstruction, not a decode. It gets the big pieces right and
# leaves a tail for the studio inspector.

SEG_FLAT_Y = 60.0          # world-Y extent under which a primitive is a slab
SEG_FLAT_XZ = 1500.0
SEG_GAP = 500.0            # XZ dilation when deciding two primitives touch
SEG_MAX_EXTENT = 26000.0   # a single object may not exceed this in X or Z
SEG_MIN_H = 400.0          # below this a non-slab is detail, not structure

GROUND_ROLES = ("road", "ground")
LOOSE_ROLES = ("foliage",)


def _prim_role(pages, page_role):
    if not page_role:
        return "?"
    c = {}
    for p in pages:
        r = page_role.get(p, "?")
        c[r] = c.get(r, 0) + 1
    return max(c.items(), key=lambda kv: kv[1])[0]


def _agglomerate(items, gap, max_extent, require_shared_page=False):
    """Greedy nearest-pair-first merge under an extent cap.

    Plain union-find cannot express "objects may not exceed N": whether a chain
    of touching primitives collapses into one blob then depends on the order
    pairs happen to be visited. Sorting candidate pairs by centre distance and
    refusing any merge that would burst the cap makes the result deterministic
    and bounds object size, which is the property that separates a terrace into
    houses instead of one 36000-unit lump.

    items: [{"aabb":[minx,miny,minz,maxx,maxy,maxz], ...}]. Returns groups of
    indices."""
    n = len(items)
    box = [list(it["aabb"]) for it in items]
    uf = _UF(n)
    cell = max(gap * 2.0, 1.0)
    grid = {}
    for i, b in enumerate(box):
        for cx in range(int((b[0] - gap) // cell), int((b[3] + gap) // cell) + 1):
            for cz in range(int((b[2] - gap) // cell), int((b[5] + gap) // cell) + 1):
                grid.setdefault((cx, cz), []).append(i)

    pairs, seen = [], set()
    for bucket in grid.values():
        for a in range(len(bucket)):
            for b in range(a + 1, len(bucket)):
                i, j = bucket[a], bucket[b]
                if i > j:
                    i, j = j, i
                if (i, j) in seen:
                    continue
                seen.add((i, j))
                bi, bj = box[i], box[j]
                if bi[0] - gap > bj[3] or bj[0] - gap > bi[3]:
                    continue
                if bi[2] - gap > bj[5] or bj[2] - gap > bi[5]:
                    continue
                if require_shared_page and not (
                        set(items[i]["pages"]) & set(items[j]["pages"])):
                    continue           # different wall art => different building
                dx = (bi[0] + bi[3] - bj[0] - bj[3]) * 0.5
                dz = (bi[2] + bi[5] - bj[2] - bj[5]) * 0.5
                pairs.append((dx * dx + dz * dz, i, j))
    pairs.sort()

    merged = {i: list(box[i]) for i in range(n)}
    for _d, i, j in pairs:
        ri, rj = uf.find(i), uf.find(j)
        if ri == rj:
            continue
        a, b = merged[ri], merged[rj]
        nb = [min(a[0], b[0]), min(a[1], b[1]), min(a[2], b[2]),
              max(a[3], b[3]), max(a[4], b[4]), max(a[5], b[5])]
        if (nb[3] - nb[0]) > max_extent or (nb[5] - nb[2]) > max_extent:
            continue                       # would burst the cap -- keep apart
        uf.union(ri, rj)
        merged[uf.find(ri)] = nb
    groups = {}
    for i in range(n):
        groups.setdefault(uf.find(i), []).append(i)
    return list(groups.values())


def segment_level(model, page_role=None, weld=1.0, gap=SEG_GAP,
                  max_extent=SEG_MAX_EXTENT):
    """Segment a WHOLE level into objects. Returns a list of
    {"kind_hint", "prims":[...], "aabb", "pages", "nface"}.

    kind_hint is coarse and structural -- "structure", "slab", "loose" -- not the
    catalogue's final verdict; classify() still names it."""
    prims = []
    for mi, mesh in enumerate(model["meshes"]):
        r = split_mesh(mesh, mode="page")
        if r["reason"] != SPLIT_OK:
            continue
        for p in r["parts"]:
            dx, dy, dz = p["extent"]
            role = _prim_role(p["pages"], page_role)
            if dy <= SEG_FLAT_Y and max(dx, dz) > SEG_FLAT_XZ:
                kind = "slab"
            elif role in LOOSE_ROLES:
                kind = "loose"
            elif role in GROUND_ROLES:
                kind = "slab"          # road/ground art is never a building
            elif dy < SEG_MIN_H:
                kind = "loose"
            else:
                kind = "structure"
            p["mesh_index"] = mi
            p["role"] = role
            p["kind_hint"] = kind
            prims.append(p)

    out = []
    structure = [p for p in prims if p["kind_hint"] == "structure"]
    for g in _agglomerate(structure, gap, max_extent):
        members = [structure[i] for i in g]
        out.append(_merge_prims(members, "structure"))
    for p in prims:
        if p["kind_hint"] != "structure":
            out.append(_merge_prims([p], p["kind_hint"]))
    out.sort(key=lambda o: -o["nface"])
    return out


def _merge_prims(members, kind_hint):
    mn = [min(m["aabb"][i] for m in members) for i in range(3)]
    mx = [max(m["aabb"][i + 3] for m in members) for i in range(3)]
    pages = sorted({p for m in members for p in m["pages"]})
    return {"kind_hint": kind_hint, "prims": members, "aabb": mn + mx,
            "extent": [mx[i] - mn[i] for i in range(3)], "pages": pages,
            "nface": sum(m["nface"] for m in members)}


# ---------------------------------------------------------------------------
# RARITY-SEEDED landmark extraction
# ---------------------------------------------------------------------------
#
# Free agglomeration does not work on this data and the reason is worth keeping.
# TD5 streetscape is per-span wall quads forming CONTINUOUS FRONTAGE, and
# neighbouring buildings share wall pages, so clustering on proximity (or on
# shared pages) chains along the street: measured on level023 it produced
# objects with a median 17801-unit footprint whose top-down outlines were
# diagonal staircases of quads following the road, not buildings. There is no
# building boundary in the source to recover -- the format carries no object
# grouping at all.
#
# What IS recoverable is the DISTINCTIVE architecture, because page usage is
# sharply bimodal. Measured on level023's structure primitives:
#     page 371 .............. 636 primitives  (generic wall, every street)
#     110 pages ............. <= 3 primitives (distinctive art, 191 prims)
# A landmark carries art used almost nowhere else; frontage carries art repeated
# everywhere. So SEED on rare pages and grow a bounded neighbourhood around the
# seed, rather than letting a cluster wander down the road.
#
# Growth is capped against the SEED CENTRE, not the growing box. Capping against
# the box is what lets a cluster walk: each addition moves the box, which admits
# the next neighbour, and the run only stops at the extent limit.

LM_RARE_MAX = 3          # a page used by <= this many primitives is distinctive
LM_GROW_GAP = 900.0      # how close an unclaimed primitive must be to join
# Tuned by LOOKING at top-down footprints, not by a statistic. At radius 9000 /
# extent 20000 the outputs pinned to the cap (p50 16980) and were seeds plus the
# frontage around them. At 4000 / 11000 they come out compact and roughly square
# at 5000..8500 units, many showing the nested-rectangle signature of outer
# walls plus inner detail -- i.e. buildings.
LM_MAX_RADIUS = 4000.0   # from the SEED centre -- the anti-chaining constraint
LM_MAX_EXTENT = 11000.0
LM_MIN_FACES = 20        # below this it is a wall fragment, not a set piece
LM_MIN_PRIMS = 3         # a single quad group is never a landmark


def _aabb_gap_xz(a, b):
    dx = max(0.0, max(a[0] - b[3], b[0] - a[3]))
    dz = max(0.0, max(a[2] - b[5], b[2] - a[5]))
    return (dx * dx + dz * dz) ** 0.5


def structure_prims(model, page_role):
    """Every primitive in the level that could be part of a built structure --
    ground, road and foliage held out, because those are what bridge unrelated
    buildings together."""
    prims = []
    for mi, mesh in enumerate(model["meshes"]):
        r = split_mesh(mesh, mode="page")
        if r["reason"] != SPLIT_OK:
            continue
        for p in r["parts"]:
            dx, dy, dz = p["extent"]
            role = _prim_role(p["pages"], page_role)
            if dy <= SEG_FLAT_Y and max(dx, dz) > SEG_FLAT_XZ:
                continue
            if role in GROUND_ROLES or role in LOOSE_ROLES:
                continue
            if dy < SEG_MIN_H:
                continue
            p["mesh_index"] = mi
            p["role"] = role
            prims.append(p)
    return prims


def extract_landmarks(model, page_role, rare_max=LM_RARE_MAX,
                      grow_gap=LM_GROW_GAP, max_radius=LM_MAX_RADIUS,
                      max_extent=LM_MAX_EXTENT, min_faces=LM_MIN_FACES):
    """Find the distinctive set pieces in one level. Returns objects sorted by
    rarity then size, each {"prims", "aabb", "extent", "pages", "nface",
    "rarity", "seed"}."""
    prims = structure_prims(model, page_role)
    if not prims:
        return []
    use = {}
    for p in prims:
        for q in p["pages"]:
            use[q] = use.get(q, 0) + 1

    # A primitive's rarity is its RAREST page: one distinctive texture is enough
    # to mark a piece as special even when the rest of it is ordinary brick.
    rarity = [min((use[q] for q in p["pages"]), default=10 ** 6) for p in prims]
    order = sorted((i for i in range(len(prims)) if rarity[i] <= rare_max),
                   key=lambda i: (rarity[i], -prims[i]["nface"]))

    cell = max(grow_gap * 2.0, 1.0)
    grid = {}
    for i, p in enumerate(prims):
        b = p["aabb"]
        for cx in range(int(b[0] // cell), int(b[3] // cell) + 1):
            for cz in range(int(b[2] // cell), int(b[5] // cell) + 1):
                grid.setdefault((cx, cz), []).append(i)

    claimed = [False] * len(prims)
    out = []
    for s in order:
        if claimed[s]:
            continue
        sb = prims[s]["aabb"]
        scx, scz = (sb[0] + sb[3]) * 0.5, (sb[2] + sb[5]) * 0.5
        group = [s]
        claimed[s] = True
        box = list(sb)
        frontier = [s]
        while frontier:
            cur = frontier.pop()
            cb = prims[cur]["aabb"]
            near = set()
            for cx in range(int((cb[0] - grow_gap) // cell),
                            int((cb[3] + grow_gap) // cell) + 1):
                for cz in range(int((cb[2] - grow_gap) // cell),
                                int((cb[5] + grow_gap) // cell) + 1):
                    near.update(grid.get((cx, cz), ()))
            for j in sorted(near):
                if claimed[j]:
                    continue
                jb = prims[j]["aabb"]
                if _aabb_gap_xz(cb, jb) > grow_gap:
                    continue
                jx, jz = (jb[0] + jb[3]) * 0.5, (jb[2] + jb[5]) * 0.5
                if ((jx - scx) ** 2 + (jz - scz) ** 2) ** 0.5 > max_radius:
                    continue                       # anti-chaining: seed-anchored
                nb = [min(box[0], jb[0]), min(box[1], jb[1]), min(box[2], jb[2]),
                      max(box[3], jb[3]), max(box[4], jb[4]), max(box[5], jb[5])]
                if (nb[3] - nb[0]) > max_extent or (nb[5] - nb[2]) > max_extent:
                    continue
                claimed[j] = True
                group.append(j)
                frontier.append(j)
                box = nb
        o = _merge_prims([prims[i] for i in group], "landmark")
        o["rarity"] = rarity[s]
        o["seed"] = s
        if o["nface"] >= min_faces and len(o["prims"]) >= LM_MIN_PRIMS:
            out.append(o)
    out.sort(key=lambda o: (o["rarity"], -o["nface"]))
    return out


def _select_whole(cand, cx, cz, seed_gap, max_extent):
    """Connected-component growth from the mesh nearest (cx,cz): repeatedly add
    any candidate whose (approx) footprint is within seed_gap of the growing
    set's XZ bbox, until nothing joins or the set would exceed max_extent. Grabs
    ONE contiguous building instead of a fixed-radius slice (so no partial
    buildings). cand items: (index, cx, cy, cz, radius)."""
    cand = sorted(cand, key=lambda c: (c[1]-cx)**2 + (c[3]-cz)**2)
    seed = cand[0]
    used = {seed[0]}
    minx, maxx = seed[1]-seed[4], seed[1]+seed[4]
    minz, maxz = seed[3]-seed[4], seed[3]+seed[4]
    changed = True
    while changed:
        changed = False
        for c in cand:
            if c[0] in used:
                continue
            cxm, czm, cr = c[1], c[3], c[4]
            nx = max(minx - seed_gap, min(cxm, maxx + seed_gap))
            nz = max(minz - seed_gap, min(czm, maxz + seed_gap))
            if (cxm - nx) ** 2 + (czm - nz) ** 2 > cr * cr:
                continue                               # too far from the set
            nminx, nmaxx = min(minx, cxm-cr), max(maxx, cxm+cr)
            nminz, nmaxz = min(minz, czm-cr), max(maxz, czm+cr)
            if (nmaxx-nminx) > max_extent or (nmaxz-nminz) > max_extent:
                continue                               # would over-grow into neighbours
            used.add(c[0]); minx, maxx, minz, maxz = nminx, nmaxx, nminz, nmaxz
            changed = True
    return used


def extract_prototype(assets_root, source_level, cx, cz, radius=None,
                      seed_gap=2500.0, max_extent=24000.0):
    """Pull a WHOLE contiguous building near (cx,cz) from a source track into a
    reusable prototype. Returns:
      { meshes:[local mesh dicts, origin 0], pages:[src page ids used],
        footprint:[dx,dz], height:h, mesh_count:n, source_level }
    Default (radius=None) grows a connected component from the seed so a whole
    building is captured (base-Y is then the real building base -> instances sit
    flush). Pass an explicit `radius` for the old fixed-circle behaviour. Meshes
    are recentred to the footprint centre with base at y=0. Skips ground
    (flat/wide) and billboards."""
    assets_root = assets_root or _mod("td5_trackgen")._default_assets_root()
    mt = _mod("mesh_tool")
    ld = os.path.join(assets_root, "levels", "level%03d" % int(source_level))
    model = mt.decode(open(os.path.join(ld, "models.bin"), "rb").read(), "models")

    # candidate solid meshes (approx AABB via bounding sphere; drop billboards +
    # obvious ground = big-radius low-centre)
    cand = []
    for idx, m in enumerate(model["meshes"]):
        if m["texture_page_id"] in (1, 2):
            continue
        br, bxc, byc, bzc = m["bounding"]
        if br > 9000 and byc < 1000:
            continue                                   # likely ground/plaza slab
        cand.append((idx, bxc, byc, bzc, br))
    if not cand:
        raise ValueError("no solid meshes in level%03d" % int(source_level))

    if radius is not None:
        r2 = float(radius) * float(radius)
        idxs = [c[0] for c in cand if (c[1]-cx)**2 + (c[3]-cz)**2 <= r2]
    else:
        idxs = list(_select_whole(cand, cx, cz, seed_gap, max_extent))

    sel = []
    for idx in idxs:
        m = model["meshes"][idx]
        wv = _world_verts(m)
        if not wv:
            continue
        mnx = min(p[0] for p in wv); mxx = max(p[0] for p in wv)
        mnz = min(p[2] for p in wv); mxz = max(p[2] for p in wv)
        mny = min(p[1] for p in wv); mxy = max(p[1] for p in wv)
        if (mxy - mny) < 500 and max(mxx-mnx, mxz-mnz) > 1500:
            continue                                   # ground / plaza — skip
        sel.append((m, wv, mny))
    if not sel:
        raise ValueError("no reusable solid meshes near (%d,%d) in level%03d"
                         % (cx, cz, int(source_level)))

    base_y = min(s[2] for s in sel)
    ax = sum((min(p[0] for p in wv) + max(p[0] for p in wv)) / 2 for _, wv, _ in sel) / len(sel)
    az = sum((min(p[2] for p in wv) + max(p[2] for p in wv)) / 2 for _, wv, _ in sel) / len(sel)

    pages = set()
    meshes = []
    ext = [1e30, 1e30, -1e30, -1e30, -1e30]            # minx,minz,maxx,maxz,maxy
    for m, wv, _ in sel:
        for c in m["commands"]:
            pages.add(int(c["texture_page_id"]))
        lm_verts = []
        for v, (wx, wy, wz) in zip(m["vertices"], wv):
            lx, ly, lz = wx - ax, wy - base_y, wz - az
            lm_verts.append({"pos": [lx, ly, lz], "view": [0.0, 0.0, 0.0],
                             "light": v.get("light", 0xFFFFFFFF),
                             "tex": v.get("tex", [0.0, 0.0]), "proj": [0.0, 0.0]})
            ext[0] = min(ext[0], lx); ext[1] = min(ext[1], lz)
            ext[2] = max(ext[2], lx); ext[3] = max(ext[3], lz); ext[4] = max(ext[4], ly)
        meshes.append({"render_type": m["render_type"], "texture_page_id": 0,
                       "bounding": _bounds([v["pos"] for v in lm_verts]),
                       "origin": [0.0, 0.0, 0.0], "reserved_28": 0,
                       "commands": [dict(c) for c in m["commands"]],
                       "vertices": lm_verts, "normals": None})
    return {"meshes": meshes, "pages": sorted(pages),
            "footprint": [round(ext[2]-ext[0], 1), round(ext[3]-ext[1], 1)],
            "height": round(ext[4], 1), "mesh_count": len(meshes),
            "source_level": int(source_level)}


def build_page_map(target_level_dir, source_level_dir, src_pages):
    """Copy the prototype's source pages into the target pool and return the
    {source_page_id: target_page_id} remap. Import once per prototype set (or
    union pages across prototypes and call once) so shared pages dedupe."""
    tr = _mod("td5_texture_reuse")
    new_ids = tr.import_texture_pages(target_level_dir, source_level_dir, list(src_pages))
    return {int(s): int(t) for s, t in zip(src_pages, new_ids)}


def instance_prototype(proto, world_xyz, yaw_deg=0.0, page_map=None, scale=1.0):
    """Place a prototype at world_xyz, rotated yaw_deg about Y and uniform-scaled,
    remapping each command's texture page via page_map. Returns mesh dicts ready
    for td5_scenery.compose_models. page_map defaults to identity."""
    page_map = page_map or {}
    a = math.radians(yaw_deg); ca, sa = math.cos(a), math.sin(a)
    wx0, wy0, wz0 = (float(v) for v in world_xyz)
    out = []
    for m in proto["meshes"]:
        vs = []
        pts = []
        for v in m["vertices"]:
            lx, ly, lz = v["pos"][0]*scale, v["pos"][1]*scale, v["pos"][2]*scale
            rx = lx*ca - lz*sa; rz = lx*sa + lz*ca
            wx, wy, wz = wx0 + rx, wy0 + ly, wz0 + rz
            pts.append((wx, wy, wz))
            vs.append({"pos": [wx, wy, wz], "view": [0.0, 0.0, 0.0],
                       "light": v["light"], "tex": v["tex"], "proj": [0.0, 0.0]})
        cmds = []
        for c in m["commands"]:
            c2 = dict(c)
            c2["texture_page_id"] = page_map.get(int(c["texture_page_id"]),
                                                 int(c["texture_page_id"]))
            c2["vptr"] = 0
            cmds.append(c2)
        out.append({"render_type": m["render_type"], "texture_page_id": 0,
                    "bounding": _bounds(pts), "origin": [0.0, 0.0, 0.0],
                    "reserved_28": 0, "commands": cmds, "vertices": vs, "normals": None})
    return out


if __name__ == "__main__":
    import json
    root = None
    lvl = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    cx = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    cz = int(sys.argv[3]) if len(sys.argv) > 3 else -600000
    rad = int(sys.argv[4]) if len(sys.argv) > 4 else 6000
    p = extract_prototype(root, lvl, cx, cz, rad)
    print(json.dumps({k: p[k] for k in ("pages", "footprint", "height", "mesh_count")}, indent=2))
