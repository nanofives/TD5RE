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
