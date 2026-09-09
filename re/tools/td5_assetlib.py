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

# Structure primitives that are NOT building fabric. Measured on level023's
# 9339 structure primitives: 531 ribbons and 12 posts. Both must go -- a
# guardrail dragged into a landmark is furniture, not architecture, and the
# guardrail page (451) primitives all measure like 1055 x 508 x 5906, i.e.
# exactly this ribbon shape.
POST_ASPECT = 1.5


def _struct_shape(p):
    dx, dy, dz = p["extent"]
    lo, hi = min(dx, dz), max(dx, dz)
    # POST by ASPECT, not by absolute width. The first version tested
    # `max(dx,dz) < 600` and let every lamp through: a level023 lamp measures
    # 562 x 1516 x 3430, narrow in one axis but 1516 deep because of its arm.
    # Height-over-footprint separates cleanly instead -- that lamp scores 2.26
    # while every facade in the same piece scores 0.12..0.67.
    if dy > 1500.0 and dy > hi * POST_ASPECT:
        return "post"                       # lamp post, sign pole
    if dy <= 900.0 and hi > 2500.0 and hi > lo * 3.0:
        return "ribbon"                     # guardrail, kerb rail, fence run
    if dy < 400.0:
        return "detail"
    return "wall"


LM_SEED_MERGE = 5000.0   # two rare primitives this close seed the SAME building
LM_RARE_MAX = 3          # a page used by <= this many primitives is distinctive
LM_GROW_GAP = 900.0      # how close an unclaimed primitive must be to join
# Tuned by LOOKING at top-down footprints, not by a statistic. At radius 9000 /
# extent 20000 the outputs pinned to the cap (p50 16980) and were seeds plus the
# frontage around them. At 4000 / 11000 they come out compact and roughly square
# at 5000..8500 units, many showing the nested-rectangle signature of outer
# walls plus inner detail -- i.e. buildings.
LM_MAX_RADIUS = 8000.0   # ownership reach from the SEED centre
LM_MAX_EXTENT = 11000.0   # (unused by the nearest-seed path; kept for callers)
LM_MIN_FACES = 20        # below this it is a wall fragment, not a set piece
LM_MIN_PRIMS = 3         # a single quad group is never a landmark


def _aabb_gap_xz(a, b):
    dx = max(0.0, max(a[0] - b[3], b[0] - a[3]))
    dz = max(0.0, max(a[2] - b[5], b[2] - a[5]))
    return (dx * dx + dz * dz) ** 0.5


def level_prims(model, page_role):
    """(structure, slabs) for a whole level.

    structure -- what a building can be made of.
    slabs     -- flat ground/road/paving. Held OUT of clustering because they
                 are what bridges unrelated buildings into one lump, but kept
                 so attach_slabs can put a landmark's own paving back under it
                 afterwards."""
    structure, slabs = [], []
    for mi, mesh in enumerate(model["meshes"]):
        r = split_mesh(mesh, mode="page")
        if r["reason"] != SPLIT_OK:
            continue
        for p in r["parts"]:
            dx, dy, dz = p["extent"]
            role = _prim_role(p["pages"], page_role)
            p["mesh_index"] = mi
            p["role"] = role
            flat = dy <= SEG_FLAT_Y and max(dx, dz) > SEG_FLAT_XZ
            if flat or role in GROUND_ROLES:
                slabs.append(p)
            elif role in LOOSE_ROLES or dy < SEG_MIN_H:
                continue
            else:
                structure.append(p)
    return structure, slabs


def structure_prims(model, page_role):
    return level_prims(model, page_role)[0]


# A landmark's PAVING, put back after segmentation. Two gates, both needed:
#   size  -- a road ribbon runs the length of the track, and attaching one would
#            turn a 7000-unit building into a track-long prefab. A plaza is
#            plaza-sized, so cap the slab's own extent.
#   height-- paving belongs at the building's BASE. Without this the deck of a
#            flyover passing overhead would be adopted as a forecourt.
SLAB_PAD = 1200.0        # how far past the footprint a slab CENTRE may sit
SLAB_Y_TOL = 2500.0
SLAB_GROWTH = 1.45       # the object may not exceed this x its bare footprint


def attach_slabs(landmarks, slabs, pad=SLAB_PAD, y_tol=SLAB_Y_TOL,
                 growth=SLAB_GROWTH):
    """Give each landmark the flat paving that sits UNDER it.

    Three gates, and the first two were learned the hard way -- attaching every
    slab that merely touched the footprint doubled the median object (7051 ->
    13762) and pushed the largest to 27703, i.e. straight back to the
    frontage-chunk scale this whole pass exists to escape:

      CENTRE-IN  the slab's centre must lie inside the padded footprint, not
                 merely overlap its edge. An edge-touching slab is the
                 neighbour's forecourt, or the road.
      GROWTH     the object may not exceed `growth` x its bare structural
                 footprint. This is the real bound: paving DRESSES a building,
                 it does not redefine how big it is.
      BASE       paving belongs at the building's base, so a flyover deck
                 passing overhead is not adopted as a forecourt.

    A slab is claimed once, so two neighbours cannot both take the same court.
    """
    if not landmarks or not slabs:
        return 0
    taken = [False] * len(slabs)
    n = 0
    for o in landmarks:
        a = list(o["aabb"])
        base = a[1]
        lim_x = max((a[3] - a[0]) * growth, 4000.0)
        lim_z = max((a[5] - a[2]) * growth, 4000.0)
        box = list(a)
        add = []
        for i, s in enumerate(slabs):
            if taken[i]:
                continue
            # ROAD is never context. Sidewalk and grass ARE -- a landmark should
            # arrive with the ground it stood on so it sits in a plausible
            # setting -- but carriageway dragged along with a building reads as
            # a piece of street torn out with it.
            if s.get("role") == "road":
                continue
            b = s["aabb"]
            scx, scz = (b[0] + b[3]) * 0.5, (b[2] + b[5]) * 0.5
            if not (a[0] - pad <= scx <= a[3] + pad
                    and a[2] - pad <= scz <= a[5] + pad):
                continue
            if abs(b[1] - base) > y_tol:
                continue
            nb = [min(box[0], b[0]), min(box[1], b[1]), min(box[2], b[2]),
                  max(box[3], b[3]), max(box[4], b[4]), max(box[5], b[5])]
            if (nb[3] - nb[0]) > lim_x or (nb[5] - nb[2]) > lim_z:
                continue
            taken[i] = True
            box = nb
            add.append(s)
        if add:
            merged = _merge_prims(o["prims"] + add, o["kind_hint"])
            o["prims"] = merged["prims"]
            o["aabb"] = merged["aabb"]
            o["extent"] = merged["extent"]
            o["pages"] = merged["pages"]
            o["nface"] = merged["nface"]
            o["slabs"] = len(add)
            n += len(add)
    return n


# ---------------------------------------------------------------------------
# ROAD-CORRIDOR segmentation
# ---------------------------------------------------------------------------
#
# Geometry alone cannot say where one building ends: TD5 frontage is per-span
# wall quads that physically touch, so neither adjacency nor texture identity
# marks a boundary. The road does. Buildings FRONT ONTO a street, so the street
# supplies the cut lines a person would use -- a corner, or a break in the
# frontage where a side street comes in.
#
# The corner signal is unusually clean. Measured on level023's 2789 ring spans,
# per-span heading change is p50 0.51 deg, p90 3.75 deg, but p99 71.34 deg: the
# road is either essentially straight or it genuinely turns, with nothing in
# between, so any threshold in the middle of that gap behaves the same.
#
# *** MEASURED AND REJECTED -- NOT WIRED INTO extract_landmarks. ***
#
# Kept because the idea is sound and someone will think of it again. It loses to
# the nearest-seed segmentation on every measure, for two reasons the numbers
# make plain:
#
#   * A long STRAIGHT street with unbroken frontage carries no corner and no
#     gap, so it comes out as ONE run: p90 111320, max 264633. Capping the run
#     by span count then makes things worse, not better -- p50 rises from 9594
#     to 22727 and 100 runs still exceed 30000 -- because a span count does not
#     bound spatial extent, and bucketing everything within 20000 laterally
#     makes a run a wide swath rather than a building line.
#   * 3326 of level023's 8796 wall primitives (38%) sit too far from the ring to
#     belong to any frontage at all, so a corridor-only pass simply cannot see
#     more than half the fabric.
#
# For comparison, nearest-seed with the furniture exclusions gives p50 12352.
CORRIDOR_CORNER_DEG = 25.0
CORRIDOR_GAP_SPANS = 6      # this many empty spans = a side street or a gap
CORRIDOR_MAX_DIST = 20000.0
CORRIDOR_MAX_RUN = 20      # spans; the fallback cut once corners and gaps run out
CROSSING_MARGIN = 900.0    # past the kerb, still "over the road"
OVERHEAD_MIN_Y = 2500.0    # above local ground before "overhead" can apply
SUPPORT_TOL = 300.0        # fabric this much lower counts as holding it up


def _ground_grid(slabs, cell=4000.0):
    """Local ground height per XZ cell, from the flat primitives."""
    g = {}
    for s in slabs:
        b = s["aabb"]
        for cx in range(int(b[0] // cell), int(b[3] // cell) + 1):
            for cz in range(int(b[2] // cell), int(b[5] // cell) + 1):
                k = (cx, cz)
                if k not in g or b[1] < g[k]:
                    g[k] = b[1]
    return g, cell


def _ground_under(g, cell, b):
    vals = [g[(cx, cz)]
            for cx in range(int(b[0] // cell), int(b[3] // cell) + 1)
            for cz in range(int(b[2] // cell), int(b[5] // cell) + 1)
            if (cx, cz) in g]
    return min(vals) if vals else None


def overhead_mask(walls, slabs, cl, grid, cell):
    """Indices of wall primitives that are OVERHEAD FURNITURE, not architecture.

    Two independent signals, both measured on level023 rather than assumed:

    UNSUPPORTED -- a building's upper storey has building beneath it; a lamp
      glow or a hanging sign has only air. 235 of 7904 walls are elevated with
      nothing under them, and 150 of those are ONE page (474), which is a
      radial light halo with median extent 1000 x 0, i.e. a flat billboard.
      The post itself is already caught by aspect; this catches its glow.

    STRADDLING -- a banner or gantry spans the carriageway, a facade stands
      beside it. 200 walls straddle the centreline, but most are road-level
      medians (page 371 at h=625, page 418 at h=680), so straddling ALONE is
      not enough; it has to be elevated too. That combination is what isolates
      the banner (page 196, straddling at h=4477).
    """
    gg, gcell = _ground_grid(slabs)
    idx = {}
    for i, p in enumerate(walls):
        b = p["aabb"]
        for cx in range(int(b[0] // gcell), int(b[3] // gcell) + 1):
            for cz in range(int(b[2] // gcell), int(b[5] // gcell) + 1):
                idx.setdefault((cx, cz), []).append(i)

    drop = set()
    for i, p in enumerate(walls):
        b = p["aabb"]
        gnd = _ground_under(gg, gcell, b)
        if gnd is None or b[1] - gnd < OVERHEAD_MIN_Y:
            continue                                  # not elevated: keep

        cand = set()
        for cx in range(int(b[0] // gcell), int(b[3] // gcell) + 1):
            for cz in range(int(b[2] // gcell), int(b[5] // gcell) + 1):
                cand.update(idx.get((cx, cz), ()))
        supported = False
        for j in cand:
            if j == i:
                continue
            q = walls[j]["aabb"]
            if q[0] > b[3] or b[0] > q[3] or q[2] > b[5] or b[2] > q[5]:
                continue
            if q[1] < b[1] - SUPPORT_TOL:
                supported = True
                break
        if not supported:
            drop.add(i)
            continue

        if cl:                                        # straddles the carriageway?
            sides = set()
            ok = True
            for x in (b[0], b[3]):
                for z in (b[2], b[5]):
                    si, d, sd = _corridor_nearest(cl, grid, cell, x, z)
                    if si < 0 or d > 15000.0:
                        ok = False
                    else:
                        sides.add(sd)
            if ok and len(sides) == 2:
                drop.add(i)
    return drop


def _corridor_index(centerline, cell=6000.0):
    grid = {}
    for i, c in enumerate(centerline["centers"]):
        if c:
            grid.setdefault((int(c[0] // cell), int(c[2] // cell)), []).append(i)
    return grid, cell


def _corridor_nearest(centerline, grid, cell, x, z):
    C = centerline["centers"]
    bi, bd = -1, 1e30
    cx, cz = int(x // cell), int(z // cell)
    for dx in (-1, 0, 1):
        for dz in (-1, 0, 1):
            for i in grid.get((cx + dx, cz + dz), ()):
                c = C[i]
                d = (c[0] - x) ** 2 + (c[2] - z) ** 2
                if d < bd:
                    bd, bi = d, i
    if bi < 0:
        return -1, 0.0, 0
    c = C[bi]
    n = C[(bi + 1) % len(C)] or c
    dx, dz = n[0] - c[0], n[2] - c[2]
    dl = math.hypot(dx, dz) or 1.0
    cross = (dx / dl) * (z - c[2]) - (dz / dl) * (x - c[0])
    return bi, math.sqrt(bd), (1 if cross >= 0 else -1)


def _corridor_corners(centerline, corner_deg):
    """Span indices where the road turns hard enough to end a frontage run."""
    C = centerline["centers"]
    hd = []
    for i, c in enumerate(C):
        if c is None:
            hd.append(None)
            continue
        n = C[(i + 1) % len(C)] or c
        hd.append(math.degrees(math.atan2(n[2] - c[2], n[0] - c[0])))
    corners = set()
    for i in range(len(hd)):
        a, b = hd[i], hd[(i + 1) % len(hd)]
        if a is None or b is None:
            continue
        if abs((b - a + 180.0) % 360.0 - 180.0) >= corner_deg:
            corners.add(i)
    return corners


def segment_by_corridor(model, page_role, level_dir,
                        corner_deg=CORRIDOR_CORNER_DEG,
                        gap_spans=CORRIDOR_GAP_SPANS,
                        max_dist=CORRIDOR_MAX_DIST,
                        max_run_spans=CORRIDOR_MAX_RUN):
    """Segment building fabric into frontage RUNS along the road.

    A run is a maximal stretch of one side of the street carrying wall fabric,
    ended by a corner or by a gap of `gap_spans` empty spans. That is the same
    cut a person makes reading a street, and unlike clustering it does not need
    the buildings to be physically separated -- which they are not.

    Returns (objects, unmatched): objects are runs; unmatched are wall
    primitives too far from the ring to belong to any frontage (back rows and
    off-ring geometry), returned rather than silently dropped."""
    mtr = _mod("td5_maptrace")
    cl = mtr._load_centerline(level_dir)
    if not cl:
        return [], []
    allp, _slabs = level_prims(model, page_role)
    walls = [p for p in allp if _struct_shape(p) == "wall"]
    grid, cell = _corridor_index(cl)
    corners = _corridor_corners(cl, corner_deg)

    bucket, unmatched = {}, []
    for p in walls:
        b = p["aabb"]
        x, z = (b[0] + b[3]) * 0.5, (b[2] + b[5]) * 0.5
        si, d, side = _corridor_nearest(cl, grid, cell, x, z)
        if si < 0 or d > max_dist:
            unmatched.append(p)
            continue
        bucket.setdefault((side, si), []).append(p)

    out = []
    for side in (-1, 1):
        spans = sorted(s for (sd, s) in bucket if sd == side)
        if not spans:
            continue
        run, prev = [], None
        def flush(r):
            # A long STRAIGHT street with unbroken frontage carries no corner and
            # no gap, so it comes out as one run spanning the whole street --
            # measured p90 111320, max 264633. There is no boundary signal left
            # in the data there, so the run is chopped into equal blocks. That
            # cut is admittedly arbitrary; it is bounded and repeatable, which is
            # the most the source supports once corners and gaps are used up.
            if len(r) <= max_run_spans:
                return [r]
            n = (len(r) + max_run_spans - 1) // max_run_spans
            step = (len(r) + n - 1) // n
            return [r[i:i + step] for i in range(0, len(r), step)]
        for s in spans:
            brk = (prev is not None
                   and (s - prev > gap_spans
                        or any(k in corners for k in range(prev, s + 1))))
            if brk and run:
                for part in flush(run):
                    out.append(_merge_prims(
                        [p for k in part for p in bucket[(side, k)]], "frontage"))
                run = []
            run.append(s)
            prev = s
        if run:
            for part in flush(run):
                out.append(_merge_prims(
                    [p for k in part for p in bucket[(side, k)]], "frontage"))
    out.sort(key=lambda o: -o["nface"])
    return out, unmatched


def extract_landmarks(model, page_role, rare_max=LM_RARE_MAX,
                      grow_gap=LM_GROW_GAP, max_radius=LM_MAX_RADIUS,
                      max_extent=LM_MAX_EXTENT, min_faces=LM_MIN_FACES,
                      with_slabs=True, level_dir=None):
    """Find the distinctive set pieces in one level. Returns objects sorted by
    rarity then size, each {"prims", "aabb", "extent", "pages", "nface",
    "rarity", "seed"}.

    Clustering runs on STRUCTURE only -- slabs bridge unrelated buildings -- and
    the paving is put back afterwards by attach_slabs, which is what makes a
    piece read as sited rather than dropped on the terrain."""
    allprims, slabs = level_prims(model, page_role)
    # Building FABRIC only. Guardrails, kerb rails and lamp posts are furniture
    # that happens to stand next to architecture; dragged into a landmark they
    # make it look like a chunk of street.
    # Building fabric is WALL and ROOF art. SIGN art is not architecture -- it
    # is signage, banners and lamp glows. Measured on level023: 1288 of the
    # wall-shaped primitives carry sign pages, 245 of them page 474 alone, which
    # is a radial light halo of median extent 1000 x 0 (a flat billboard sprite
    # hanging above the lamp posts that the aspect filter already removed).
    # Excluding the role is cleaner than chasing each sprite by shape.
    prims = [p for p in allprims
             if _struct_shape(p) == "wall" and p.get("role") != "sign"]
    # OVERHEAD CROSSINGS -- banners, gantries, signs spanning the carriageway.
    # Not separable by height: a banner's underside sits 4508 above the local
    # base, but so do a building's upper floors (measured 3031 and 6023 in the
    # same piece), because multi-storey facades put quads at every floor. What
    # distinguishes a banner is WHERE it is: over the road. A facade stands
    # beside the carriageway, a crossing stands on top of it.
    cl = _mod("td5_maptrace")._load_centerline(level_dir) if level_dir else None
    grid, cell = _corridor_index(cl) if cl else ({}, 1.0)
    drop = overhead_mask(prims, slabs, cl, grid, cell)
    if drop:
        prims = [p for i, p in enumerate(prims) if i not in drop]
    if not prims:
        return []
    use = {}
    for p in prims:
        for q in p["pages"]:
            use[q] = use.get(q, 0) + 1

    # A primitive's rarity is its RAREST page: one distinctive texture is enough
    # to mark a piece as special even when the rest of it is ordinary brick.
    rarity = [min((use[q] for q in p["pages"]), default=10 ** 6) for p in prims]
    seeds = [i for i in range(len(prims)) if rarity[i] <= rare_max]
    if not seeds:
        return []

    # SEED CLUSTERS. Several rare primitives usually belong to ONE building (a
    # spire, its clock face, its doorway), so merge nearby seeds before growing
    # or the same building is claimed three times and comes out in thirds.
    seed_items = [{"aabb": prims[i]["aabb"], "pages": prims[i]["pages"]}
                  for i in seeds]
    centres = []
    for g in _agglomerate(seed_items, LM_SEED_MERGE, LM_SEED_MERGE * 2.0):
        xs = [(seed_items[k]["aabb"][0] + seed_items[k]["aabb"][3]) * 0.5 for k in g]
        zs = [(seed_items[k]["aabb"][2] + seed_items[k]["aabb"][5]) * 0.5 for k in g]
        best = min(g, key=lambda k: rarity[seeds[k]])
        centres.append({"cx": sum(xs) / len(xs), "cz": sum(zs) / len(zs),
                        "rarity": rarity[seeds[best]], "members": []})

    # NEAREST-SEED assignment, not greedy claiming. Greedy first-come lets one
    # seed swallow its neighbour as soon as the radius is generous, which is
    # what kept the radius small and the buildings fragmented. Assigning each
    # primitive to its NEAREST seed splits two adjacent landmarks at the
    # midpoint between them, so the radius can be widened without them merging.
    for i, p in enumerate(prims):
        b = p["aabb"]
        px, pz = (b[0] + b[3]) * 0.5, (b[2] + b[5]) * 0.5
        best, bestd = -1, max_radius
        for ci, c in enumerate(centres):
            d = ((px - c["cx"]) ** 2 + (pz - c["cz"]) ** 2) ** 0.5
            if d < bestd:
                best, bestd = ci, d
        if best >= 0:
            centres[best]["members"].append(i)

    # CONTIGUITY, applied inside each owner set. Nearest-seed alone assigns every
    # primitive to some seed however far away it is, which produced sparse cells
    # of scattered fragments spread over 18000..26000 -- a Voronoi partition of
    # the city, not buildings. Ownership decides WHICH landmark may claim a
    # primitive; adjacency decides whether it is actually part of it. Keeping
    # only the component reachable from the seed drops the strays and leaves the
    # coherent fabric, which is what lets the radius be generous.
    out = []
    for c in centres:
        if not c["members"]:
            continue
        mem = c["members"]
        boxes = {i: prims[i]["aabb"] for i in mem}
        start = min(mem, key=lambda i: ((boxes[i][0] + boxes[i][3]) * 0.5 - c["cx"]) ** 2
                    + ((boxes[i][2] + boxes[i][5]) * 0.5 - c["cz"]) ** 2)
        seen, frontier = {start}, [start]
        while frontier:
            cur = frontier.pop()
            cb = boxes[cur]
            for j in mem:
                if j in seen:
                    continue
                if _aabb_gap_xz(cb, boxes[j]) <= grow_gap:
                    seen.add(j)
                    frontier.append(j)
        o = _merge_prims([prims[i] for i in sorted(seen)], "landmark")
        o["rarity"] = c["rarity"]
        if o["nface"] >= min_faces and len(o["prims"]) >= LM_MIN_PRIMS:
            out.append(o)
    out.sort(key=lambda o: (o["rarity"], -o["nface"]))
    if with_slabs:
        attach_slabs(out, slabs)
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
