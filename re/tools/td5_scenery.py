#!/usr/bin/env python3
"""
td5_scenery.py -- place scenery geometry (buildings, props, billboards) into a
custom TD5 level so authored/densified maps feel full instead of an empty ribbon.

Two placement layers, chosen by whether the object is breakable:

  * STATIC scenery (buildings, walls, static trees/signs, ground fills):
    folded directly into the level's models.bin as extra mesh entries. No engine
    code path, no per-type limit, renders through the SAME pipeline as the
    generated road (td5_trackgen.build_road_model). This is the primary layer.

  * BREAKABLE furniture (smashable props): emitted as a LEVEL.MOV instance table
    + PROPMESH.BIN mesh blob, reusing the TD6 prop path (td5_track.c
    td5_track_append_td6_props). Constraint: <=8 distinct prop meshes, collision
    radius comes from the engine's fixed s_td6_col_radius[model].

Mesh/vertex/model dicts match re/tools/mesh_tool.py exactly (this module builds
them and hands them to mesh_tool.build_dat). Two facts pinned from the renderer
(td5_render_mesh.c) drive the mesh shapes:

  * Texture binding is the PER-COMMAND texture_page_id; the MESH-HEADER
    texture_page_id is only the BILLBOARD TAG (==1 or ==2 => camera-facing),
    NOT the sampled page (td5_render_mesh.c:1706-1719). So opaque meshes set the
    header tag to 0 and put the real page on the command -- safe even when the
    real page id happens to be 1 or 2.

  * The track render walks display-list ENTRIES in a span-window around the
    player (entry = span>>2, td5_render_mesh.c:2458). Scenery must therefore be
    attached to the ROAD ENTRY NEAREST its world position, not appended as a new
    entry past the ring (those are never walked on TD5-layout tracks).
    compose_models() does this assignment by nearest bounding-center.

World units are TD5 world units; opaque scenery carries world-space float verts
with origin [0,0,0] (exactly like the road). Billboards carry LOCAL verts
centred at 0 with the world position in origin (24.8 fixed = world*256) and in
bounding_center (render-float world), matching td5_render_mesh.c:1656-1667/1701.
"""
from __future__ import annotations

import math
import os
import struct
import sys

SCENERY_RENDER_TYPE = 259          # CULL_NONE (winding-free), same as the road
DEFAULT_LIGHT = 0xFFFFFFFF          # baked full-bright ARGB (road uses this)
FP_ONE = 256.0                      # 24.8 fixed-point scale for mesh origin

# Default UVs for a face (clockwise from near-left), so a copied real texture
# maps across the whole face. Callers may override per face.
_QUAD_UV = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
_TRI_UV = [(0.0, 0.0), (1.0, 0.0), (0.5, 1.0)]


def _mesh_tool():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mesh_tool
    return mesh_tool


def _vert(pos, uv, light):
    return {"pos": [float(pos[0]), float(pos[1]), float(pos[2])],
            "view": [0.0, 0.0, 0.0], "light": int(light) & 0xFFFFFFFF,
            "tex": [float(uv[0]), float(uv[1])], "proj": [0.0, 0.0]}


def _bounds(points):
    cx = sum(p[0] for p in points) / len(points)
    cy = sum(p[1] for p in points) / len(points)
    cz = sum(p[2] for p in points) / len(points)
    rad = max(math.sqrt((p[0] - cx) ** 2 + (p[1] - cy) ** 2 + (p[2] - cz) ** 2)
              for p in points) or 1.0
    return [rad, cx, cy, cz]


# ---------------------------------------------------------------------------
# Static-scenery mesh builders (world-space, origin 0 -- like the road)
# ---------------------------------------------------------------------------
def make_opaque_mesh(faces, page_id, *, light=DEFAULT_LIGHT):
    """Build one opaque mesh from a list of faces. Each face is a dict:
        {"pts": [(x,y,z), (x,y,z), (x,y,z)[, (x,y,z)]], "uv": [(u,v), ...]?,
         "page": <int>?}
    3-point faces are triangles, 4-point faces are quads. Faces are grouped by
    texture page into one PRR command each (a mesh can carry multiple commands
    with different pages — that's how a building gets wall sides + a roof top),
    defaulting to `page_id` when a face has no "page". Vertices are laid in
    command order (each command consumes its tri*3 then quad*4, sequentially).
    """
    groups = {}            # page -> {"tris":[...], "quads":[...]}
    order = []             # first-seen page order (deterministic output)
    for f in faces:
        npts = len(f["pts"])
        if npts not in (3, 4):
            raise ValueError("faces must have 3 or 4 points")
        pg = int(f.get("page", page_id))
        if pg not in groups:
            groups[pg] = {"tris": [], "quads": []}
            order.append(pg)
        groups[pg]["tris" if npts == 3 else "quads"].append(f)
    verts = []
    all_pts = []
    cmds = []
    for pg in order:
        g = groups[pg]
        for f in g["tris"]:
            for p, t in zip(f["pts"], f.get("uv", _TRI_UV)):
                verts.append(_vert(p, t, light)); all_pts.append(p)
        for f in g["quads"]:
            for p, t in zip(f["pts"], f.get("uv", _QUAD_UV)):
                verts.append(_vert(p, t, light)); all_pts.append(p)
        cmds.append({"dispatch_type": 0, "texture_page_id": pg, "reserved_04": 0,
                     "tri": len(g["tris"]), "quad": len(g["quads"]), "vptr": 0})
    if not all_pts:
        raise ValueError("no geometry")
    return {"render_type": SCENERY_RENDER_TYPE, "texture_page_id": 0,   # tag 0 = not billboard
            "bounding": _bounds(all_pts), "origin": [0.0, 0.0, 0.0],
            "reserved_28": 0, "commands": cmds,
            "vertices": verts, "normals": None}


def _tiled_uv(w, h, tile):
    """UV rect that REPEATS the texture every `tile` world units across a face of
    size (w,h) instead of stretching one copy over the whole face."""
    tw = max(1.0, abs(w) / tile); th = max(1.0, abs(h) / tile)
    return [(0.0, 0.0), (tw, 0.0), (tw, th), (0.0, th)]


def make_box_mesh(center, size_xyz, page_id, *, light=DEFAULT_LIGHT, floor_y=None,
                  roof_page=None, base_page=None, wall_pages=None, tile=None):
    """Axis-aligned box (building block). `center` is the box centre in world
    units; `size_xyz` is (width_x, height_y, depth_z). If `floor_y` is given the
    box sits ON that ground Y (its base at floor_y). `page_id` textures the 4
    walls; pass `roof_page` to texture the TOP face separately (a real roof) and
    `base_page` for the bottom -- each becomes its own PRR command so the engine
    binds the right page per surface (roof pages on tops, wall on sides).
    `wall_pages` (list, cycled over the 4 walls in order -Z,+Z,-X,+X) gives a
    building DIFFERENT textures per face instead of one flat texture on all sides."""
    cx, cy, cz = center
    sx, sy, sz = size_xyz
    hx, hz = sx / 2.0, sz / 2.0
    if floor_y is not None:
        y0, y1 = float(floor_y), float(floor_y) + sy
    else:
        y0, y1 = cy - sy / 2.0, cy + sy / 2.0
    x0, x1 = cx - hx, cx + hx
    z0, z1 = cz - hz, cz + hz
    c = {"000": (x0, y0, z0), "100": (x1, y0, z0), "110": (x1, y0, z1), "010": (x0, y0, z1),
         "001": (x0, y1, z0), "101": (x1, y1, z0), "111": (x1, y1, z1), "011": (x0, y1, z1)}
    def q(a, b, cc, d, page=None, uv=None):
        f = {"pts": [c[a], c[b], c[cc], c[d]]}
        if page is not None:
            f["page"] = int(page)
        if uv is not None:
            f["uv"] = uv
        return f
    wp = list(wall_pages) if wall_pages else None
    def _wp(i):
        return wp[i % len(wp)] if wp else None       # None -> default page_id
    # Per-face tiled UVs so the wall texture REPEATS (correctly aligned) rather
    # than stretching one copy over a whole tall/wide facade. ±Z walls span X×Y,
    # ±X walls span Z×Y, roof/base span X×Z.
    if tile:
        uv_z = _tiled_uv(sx, sy, tile)   # -Z / +Z walls
        uv_x = _tiled_uv(sz, sy, tile)   # -X / +X walls
        uv_t = _tiled_uv(sx, sz, tile)   # roof / base
    else:
        uv_z = uv_x = uv_t = None
    faces = [
        q("001", "101", "111", "011", roof_page, uv_t),   # top (+Y)  -> roof
        q("000", "010", "110", "100", base_page, uv_t),   # bottom (-Y) -> base
        q("000", "100", "101", "001", _wp(0), uv_z),      # -Z wall
        q("110", "010", "011", "111", _wp(1), uv_z),      # +Z wall
        q("010", "000", "001", "011", _wp(2), uv_x),      # -X wall
        q("100", "110", "111", "101", _wp(3), uv_x),      # +X wall
    ]
    return make_opaque_mesh(faces, page_id, light=light)


def make_ground_quad(corners, page_id, *, uv=None, light=DEFAULT_LIGHT):
    """A single flat quad (terrain fill / plaza / apron). `corners` = 4 world
    points in order near-left, near-right, far-right, far-left."""
    face = {"pts": [tuple(p) for p in corners], "uv": uv or _QUAD_UV}
    return make_opaque_mesh([face], page_id, light=light)


def make_guardrail_segment(a_ground, b_ground, page_id, *, height=750.0,
                           light=DEFAULT_LIGHT):
    """One vertical barrier-wall quad between two adjacent rail ground points
    (a_ground -> b_ground), standing `height` tall. Emit ONE per road span per
    side and add each as its own scenery mesh so it single-homes to its local
    road entry and the span-window walk renders the barrier continuously as you
    drive (a single loop-long mesh would only draw near one entry). Sits at the
    road edge, where the engine's rail-wall collision already is -- so this makes
    that otherwise-invisible wall VISIBLE. Opt-in (off by default)."""
    ax, ay, az = (float(v) for v in a_ground)
    bx, by, bz = (float(v) for v in b_ground)
    h = float(height)
    pts = [(ax, ay, az), (bx, by, bz), (bx, by + h, bz), (ax, ay + h, az)]
    uv  = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]   # top edge v=0
    return make_opaque_mesh([{"pts": pts, "uv": uv}], int(page_id), light=light)


def make_banner_mesh(left_world, right_world, page_id, *, bottom_y=2600.0,
                     height=1400.0, light=DEFAULT_LIGHT):
    """A single road-spanning START/FINISH banner panel (ONE quad -- NOT the native
    double-sided pair, which the cull-less renderer would garble). left_world /
    right_world are the road's left/right rail points at the start span; the panel
    hangs bottom_y..bottom_y+height above them, perpendicular to the road so it
    faces down-track. UV puts the TOP edge at v=0 (same convention as
    make_billboard_mesh) so banner art reads upright."""
    lx, ly, lz = (float(v) for v in left_world)
    rx, ry, rz = (float(v) for v in right_world)
    yb = float(bottom_y); yt = yb + float(height)
    pts = [(lx, ly + yb, lz), (rx, ry + yb, rz), (rx, ry + yt, rz), (lx, ly + yt, lz)]
    uv  = [(1.0, 1.0), (0.0, 1.0), (0.0, 0.0), (1.0, 0.0)]   # top edge v=0
    return make_ground_quad(pts, int(page_id), uv=uv, light=light)


def load_model_meshes(path, *, translate=(0.0, 0.0, 0.0), scale=1.0, page=None,
                      light=DEFAULT_LIGHT):
    """Import a real 3D model (glTF/.glb -- OBJ via mesh_tool if it grows one) and
    return it as a list of world-placed opaque scenery mesh dicts. Vertices are
    uniform-scaled and translated into world space; each command's texture page is
    forced to `page` when given (else the model's own page id is kept). Use this to
    drop authored buildings/props around a map. Handy convention: TD5 world units
    are large (~1500/lane), so a metre-scale glb usually needs scale ~ 500-1500."""
    mt = _mesh_tool()
    with open(path, "rb") as f:
        model = mt.import_glb(f.read())
    meshes = model["meshes"] if model.get("kind") == "models" else [model["mesh"]]
    tx, ty, tz = (float(v) for v in translate)
    out = []
    for m in meshes:
        pts, verts = [], []
        for v in m["vertices"]:
            p = (v["pos"][0] * scale + tx, v["pos"][1] * scale + ty,
                 v["pos"][2] * scale + tz)
            pts.append(p)
            lv = v.get("light", light)
            verts.append(_vert(p, v.get("tex", (0.0, 0.0)),
                               lv if isinstance(lv, int) else light))
        if not pts:
            continue
        cmds = []
        for c in m["commands"]:
            c2 = dict(c)
            if page is not None:
                c2["texture_page_id"] = int(page)
            c2["vptr"] = 0
            cmds.append(c2)
        out.append({"render_type": SCENERY_RENDER_TYPE, "texture_page_id": 0,
                    "bounding": _bounds(pts), "origin": [0.0, 0.0, 0.0],
                    "reserved_28": 0, "commands": cmds,
                    "vertices": verts, "normals": None})
    if not out:
        raise ValueError("model had no usable geometry: %s" % path)
    return out


def make_prop_box(size_xyz, *, color_argb=0xFFFFFFFF):
    """A simple box prop mesh for the breakable layer (PROPMESH.BIN). Returns a
    dict {radius, verts:[(x,y,z,u,v,col), ...]} -- a de-indexed tri-list centred
    at 0 (the MOV instance supplies the world position). Use when you don't have a
    real prop mesh to reuse."""
    sx, sy, sz = (float(v) / 2.0 for v in size_xyz)
    c = {"000": (-sx, -sy, -sz), "100": (sx, -sy, -sz), "110": (sx, -sy, sz), "010": (-sx, -sy, sz),
         "001": (-sx, sy, -sz), "101": (sx, sy, -sz), "111": (sx, sy, sz), "011": (-sx, sy, sz)}
    quads = [("001", "101", "111", "011"), ("000", "010", "110", "100"),
             ("000", "100", "101", "001"), ("110", "010", "011", "111"),
             ("010", "000", "001", "011"), ("100", "110", "111", "101")]
    uv = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    verts = []
    for a, b, cc, d in quads:
        pa, pb, pc, pd = c[a], c[b], c[cc], c[d]
        for (p, t) in ((pa, uv[0]), (pb, uv[1]), (pc, uv[2])):      # tri 1
            verts.append((p[0], p[1], p[2], t[0], t[1], int(color_argb) & 0xFFFFFFFF))
        for (p, t) in ((pa, uv[0]), (pc, uv[2]), (pd, uv[3])):      # tri 2
            verts.append((p[0], p[1], p[2], t[0], t[1], int(color_argb) & 0xFFFFFFFF))
    rad = math.sqrt(sx * sx + sy * sy + sz * sz) or 1.0
    return {"radius": rad, "verts": verts}


def make_billboard_mesh(center_xyz, size_wh, page_id, *, tag=1, light=DEFAULT_LIGHT):
    """A camera-facing billboard (tree/sign). `center_xyz` is the world anchor;
    `size_wh` = (width, height). `tag` is the header billboard class (1 or 2).
    Verts are LOCAL (centred at 0); world placement rides in origin (world*256)
    and bounding_center (world), per td5_render_mesh.c."""
    if tag not in (1, 2):
        raise ValueError("billboard tag must be 1 or 2")
    wx, wy, wz = (float(v) for v in center_xyz)
    w, h = float(size_wh[0]), float(size_wh[1])
    hw = w / 2.0
    # Local quad in the X (width) / Y (height) plane, Z=0; camera basis orients it.
    local = [(-hw, 0.0, 0.0), (hw, 0.0, 0.0), (hw, h, 0.0), (-hw, h, 0.0)]
    # UV: card TOP edge (y=h) must carry v=0 so a canopy-up tree PNG is not drawn
    # upside-down. Matches the engine's proven sprite mapping (brake-light
    # billboard, td5_render_effects.c:1325-1345 -> screen-top = v=0). The old
    # _QUAD_UV put v=0 on the bottom edge -> "rotated"/upside-down trees.
    _bb_uv = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]   # bl, br, tr, tl
    verts = [_vert(p, t, light) for p, t in zip(local, _bb_uv)]
    rad = math.sqrt(hw * hw + h * h) or 1.0
    return {"render_type": SCENERY_RENDER_TYPE, "texture_page_id": int(tag),   # header = billboard tag
            "bounding": [rad, wx, wy, wz], "origin": [wx * FP_ONE, wy * FP_ONE, wz * FP_ONE],
            "reserved_28": 0,
            "commands": [{"dispatch_type": 0, "texture_page_id": int(page_id),
                          "reserved_04": 0, "tri": 0, "quad": 1, "vptr": 0}],
            "vertices": verts, "normals": None}


# ---------------------------------------------------------------------------
# models.bin composition + I/O
# ---------------------------------------------------------------------------
def _mesh_world_center(mesh):
    """World position used for entry assignment: bounding_center (already world
    for both opaque and billboard meshes)."""
    return (mesh["bounding"][1], mesh["bounding"][2], mesh["bounding"][3])


def load_road_model(level_dir):
    """Decode the level's existing models.bin into a mesh_tool model dict."""
    mt = _mesh_tool()
    path = os.path.join(level_dir, "models.bin")
    with open(path, "rb") as f:
        return mt.decode(f.read(), "models")


def compose_models(model, scenery_meshes):
    """Append scenery meshes to a model, attaching each to the road ENTRY whose
    existing meshes' centroid is nearest the scenery mesh's world position (so
    the span-windowed render walk visits it). Mutates and returns `model`."""
    entries = model["entries"]
    meshes = model["meshes"]
    # Precompute each entry's centroid from its member meshes' world centers.
    entry_centers = []
    for i, ids in enumerate(entries):
        if not ids:
            entry_centers.append(None)
            continue
        pts = [_mesh_world_center(meshes[mid]) for mid in ids]
        entry_centers.append((sum(p[0] for p in pts) / len(pts),
                              sum(p[1] for p in pts) / len(pts),
                              sum(p[2] for p in pts) / len(pts)))
    populated = [i for i, c in enumerate(entry_centers) if c is not None]
    for sm in scenery_meshes:
        meshes.append(sm)
        mid = len(meshes) - 1
        wc = _mesh_world_center(sm)
        if populated:
            best = min(populated, key=lambda i: (
                (entry_centers[i][0] - wc[0]) ** 2 +
                (entry_centers[i][1] - wc[1]) ** 2 +
                (entry_centers[i][2] - wc[2]) ** 2))
        else:
            best = 0
            if not entries:
                entries.append([])
        entries[best].append(mid)
    model["entry_count"] = len(entries)
    return model


def attach_ground_meshes(model, ground_meshes, *, reach=48000.0):
    """Multi-home each ground tile into EVERY road entry whose centroid is within
    `reach` of the tile (never fewer than the single nearest). Fixes the winding-
    track 'ground slides with the player' culling artifact: a regular grid tile's
    Euclidean-nearest entry can sit on a DIFFERENT arm of a loop, so a single-home
    tile (compose_models) only becomes eligible when the player reaches that arm --
    the span-window render walk (td5_render_mesh.c) draws entries near the player
    IN INDEX ORDER, so world proximity alone is not enough. Multi-homing makes a
    tile eligible from every along-track position physically near it; the per-mesh
    frustum cull does the rest. Dedup in the walk is per-ENTRY pointer, so a tile
    listed in two in-window entries just draws the identical world quad twice
    (exact overlap, negligible overdraw). Mutates and returns `model`."""
    entries = model["entries"]
    meshes = model["meshes"]
    centers = []
    for ids in entries:
        if not ids:
            centers.append(None); continue
        pts = [_mesh_world_center(meshes[m]) for m in ids]
        centers.append((sum(p[0] for p in pts) / len(pts),
                        sum(p[1] for p in pts) / len(pts),
                        sum(p[2] for p in pts) / len(pts)))
    populated = [i for i, c in enumerate(centers) if c is not None]
    r2 = reach * reach
    for sm in ground_meshes:
        meshes.append(sm)
        mid = len(meshes) - 1
        wc = _mesh_world_center(sm)
        if not populated:
            if not entries:
                entries.append([])
            entries[0].append(mid); continue
        d2 = [(((centers[i][0] - wc[0]) ** 2 + (centers[i][2] - wc[2]) ** 2), i)
              for i in populated]
        d2.sort()
        hosts = [i for dd, i in d2 if dd <= r2] or [d2[0][1]]   # nearest as fallback
        for i in hosts:
            entries[i].append(mid)
    model["entry_count"] = len(entries)
    return model


def write_models_bin(level_dir, model):
    """Serialize a model dict to `<level_dir>/models.bin`. Returns mesh count."""
    blob = _mesh_tool().build_dat(model)
    with open(os.path.join(level_dir, "models.bin"), "wb") as f:
        f.write(blob)
    return len(model["meshes"])


def add_scenery_to_level(level_dir, scenery_meshes):
    """Convenience: load the level's road model, compose scenery in, rewrite
    models.bin. Returns the new total mesh count. Requires an existing
    models.bin (the textured road) in the level dir."""
    model = load_road_model(level_dir)
    compose_models(model, scenery_meshes)
    return write_models_bin(level_dir, model)


# ---------------------------------------------------------------------------
# Breakable-prop layer: LEVEL.MOV instance table + PROPMESH.BIN
# ---------------------------------------------------------------------------
def _deg_to_orient_byte(angle_deg):
    """0x1000-unit angle packed into the MOV orientation byte (units>>4)."""
    units = int(round((angle_deg % 360.0) / 360.0 * 4096.0)) & 0xFFF
    return (units >> 4) & 0xFF


def write_mov(level_dir, instances, *, filename="LEVEL.MOV"):
    """Write a 24-byte-record MOV instance table (td5_track_append_td6_props
    layout). `instances` = list of dicts:
        {"model": 0..7, "x": wx, "y": wy, "z": wz, "angle": deg, "mass": 0..255}
    x/y/z are world units (floats accepted; stored as 24.8 s32). Returns record
    count (excludes the terminator)."""
    out = bytearray()
    serial = 1
    for inst in instances:
        r = bytearray(24)
        r[0] = serial & 0xFF if serial < 0xFF else 0xFE   # never emit 0/0xFF as a real serial
        r[4] = int(inst["model"]) & 0x0F
        r[6] = int(inst.get("mass", 0)) & 0xFF
        r[8] = 1                                            # constant marker (matches TD6 data)
        r[9] = _deg_to_orient_byte(float(inst.get("angle", 0.0)))
        px = int(round(float(inst["x"]) * FP_ONE))
        py = int(round(float(inst["y"]) * FP_ONE))
        pz = int(round(float(inst["z"]) * FP_ONE))
        struct.pack_into("<i", r, 12, px)
        struct.pack_into("<i", r, 16, py)
        struct.pack_into("<i", r, 20, pz)
        out += r
        serial += 1
    out += bytes(24)          # terminator record (serial 0 => end)
    with open(os.path.join(level_dir, filename), "wb") as f:
        f.write(bytes(out))
    return len(instances)


def write_prop_meshes(level_dir, prop_meshes):
    """Write PROPMESH.BIN ('PMS2'). `prop_meshes` = list (<=8) of dicts:
        {"radius": float, "verts": [(x,y,z,u,v,col_argb), ...]}   # tri-list
    Returns mesh count."""
    if len(prop_meshes) > 12:
        raise ValueError("at most 12 breakable prop meshes (engine TD6_PROP_MESH_MAX)")
    blob = bytearray(struct.pack("<4sI", b"PMS2", len(prop_meshes)))
    for pm in prop_meshes:
        blob += struct.pack("<If", len(pm["verts"]), float(pm["radius"]))
    for pm in prop_meshes:
        for x, y, z, u, v, col in pm["verts"]:
            blob += struct.pack("<fffffI", float(x), float(y), float(z),
                                float(u), float(v), int(col) & 0xFFFFFFFF)
    with open(os.path.join(level_dir, "PROPMESH.BIN"), "wb") as f:
        f.write(bytes(blob))
    return len(prop_meshes)


if __name__ == "__main__":
    # Tiny self-check: build a box + billboard, round-trip through mesh_tool.
    mt = _mesh_tool()
    box = make_box_mesh((0, 0, 0), (2000, 4000, 2000), 5, floor_y=0)
    bb = make_billboard_mesh((3000, 0, 1000), (1500, 3000), 7, tag=1)
    model = {"kind": "models", "entry_count": 1, "entries": [[]], "meshes": []}
    # fake a road mesh so compose has a populated entry
    model["meshes"].append(make_ground_quad(
        [(-1000, 0, -1000), (1000, 0, -1000), (1000, 0, 1000), (-1000, 0, 1000)], 0))
    model["entries"][0].append(0)
    compose_models(model, [box, bb])
    blob = mt.build_dat(model)
    back = mt.decode(blob, "models")
    print("meshes:", len(back["meshes"]), "entries:", back["entry_count"],
          "-> OK" if len(back["meshes"]) == 3 else "-> FAIL")
