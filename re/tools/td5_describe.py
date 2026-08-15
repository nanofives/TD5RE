#!/usr/bin/env python3
"""
td5_describe.py -- read-only inventory of a TD5 level, so an AI author can reason
about where the road is, what's already placed, and which textures are available
before densifying ("fill around existing areas") or extending a map.

Everything is read straight from the level's editable source (strip.json,
levelinf.json, textures.src/, models.bin, LEVEL.MOV) -- no game, no writes.

    describe_track(assets_root, level) -> dict   (see _describe for the shape)

CLI:
    python td5_describe.py <level> [<assets_root>]
"""
from __future__ import annotations

import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def _load_module(name):
    import importlib
    return importlib.import_module(name)


def _levels_dir(assets_root):
    return os.path.join(assets_root, "levels")


def _default_assets_root():
    tg = _load_module("td5_trackgen")
    return tg._default_assets_root()


def _strip_summary(level_dir):
    """World extents + a per-span summary (center, width, lanes, surface) from
    strip.json. spans[i] = [type, surface, ?, lanes_flags, lvi, rvi, ?, ?, ox, oy, oz]."""
    path = os.path.join(level_dir, "strip.json")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        strip = json.load(f)
    hdr, spans, verts = strip["header"], strip["spans"], strip["vertices"]
    ring = max(1, hdr[1])
    total = hdr[4] if len(hdr) > 4 else len(spans)
    nv = len(verts)
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    per_span = []
    import math
    for i in range(min(ring, len(spans))):
        s = spans[i]
        lanes = max(1, s[3] & 0x0F)
        lvi = s[4]
        if lvi < 0 or lvi >= nv:
            continue
        ri = min(lvi + lanes, nv - 1)
        L, R = verts[lvi], verts[ri]
        ox, oy, oz = s[8], s[9], s[10]
        cx = ox + (L[0] + R[0]) * 0.5
        cy = oy + (L[1] + R[1]) * 0.5
        cz = oz + (L[2] + R[2]) * 0.5
        width = math.hypot(R[0] - L[0], R[2] - L[2])
        for wpt in (L, R):
            for a in range(3):
                v = (ox, oy, oz)[a] + wpt[a]
                lo[a] = min(lo[a], v)
                hi[a] = max(hi[a], v)
        per_span.append({"span": i, "center": [round(cx, 1), round(cy, 1), round(cz, 1)],
                         "width": round(width, 1), "lanes": lanes,
                         "surface": s[1] & 0x0F})
    extents = None
    if lo[0] != float("inf"):
        extents = {"min": [round(x, 1) for x in lo], "max": [round(x, 1) for x in hi]}
    return {"ring_spans": ring, "total_spans": total,
            "world_extents": extents, "spans": per_span}


def _mov_props(level_dir, filename="LEVEL.MOV"):
    path = os.path.join(level_dir, filename)
    if not os.path.isfile(path):
        return None
    raw = open(path, "rb").read()
    props = []
    for i in range(len(raw) // 24):
        r = raw[i * 24:(i + 1) * 24]
        if r[0] == 0:
            continue
        if r[0] == 0xFF:
            break
        px = struct.unpack_from("<i", r, 12)[0] / 256.0
        py = struct.unpack_from("<i", r, 16)[0] / 256.0
        pz = struct.unpack_from("<i", r, 20)[0] / 256.0
        angle = ((r[9] << 4) & 0xFFF) * 360.0 / 4096.0
        props.append({"model": r[4] & 0x0F, "mass": r[6],
                      "world_xyz": [round(px, 1), round(py, 1), round(pz, 1)],
                      "angle": round(angle, 1)})
    return props


def _model_pages(level_dir):
    path = os.path.join(level_dir, "models.bin")
    if not os.path.isfile(path):
        return None
    mt = _load_module("mesh_tool")
    model = mt.decode(open(path, "rb").read(), "models")
    pages = set()
    billboards = 0
    for m in model["meshes"]:
        if m["texture_page_id"] in (1, 2):
            billboards += 1
        for c in m["commands"]:
            pages.add(int(c["texture_page_id"]))
    return {"mesh_count": len(model["meshes"]), "entry_count": model["entry_count"],
            "pages_used": sorted(pages), "billboard_meshes": billboards}


def describe_track(assets_root=None, level=None):
    """Read-only inventory of levelNNN. Returns:
    {level, name, circuit, ring_spans, total_spans, world_extents,
     spans:[{span,center,width,lanes,surface}], checkpoints, branches,
     props:[{model,world_xyz,angle,mass}], propsb:[...], textures:{page_count,pages},
     model:{mesh_count,entry_count,pages_used,billboard_meshes}}"""
    assets_root = assets_root or _default_assets_root()
    level = int(level)
    level_dir = os.path.join(_levels_dir(assets_root), "level%03d" % level)
    if not os.path.isdir(level_dir):
        raise FileNotFoundError("no level dir: %s" % level_dir)

    out = {"level": level, "dir": level_dir}

    tg = _load_module("td5_trackgen")
    # centerline + environment via the proven extractor (best-effort)
    try:
        spec, warnings = tg.extract_track(assets_root, level)
        out["name"] = spec.get("name")
        out["circuit"] = bool(spec.get("circuit"))
        out["checkpoints"] = spec.get("checkpoints")
        out["branches"] = [{"lanes": b.get("lanes"), "nodes": len(b.get("nodes", []))}
                           for b in spec.get("branches", [])]
        out["extract_warnings"] = warnings
    except Exception as e:
        out["extract_warnings"] = ["extract_track failed: %s" % e]

    strip = _strip_summary(level_dir)
    if strip:
        out.update(strip)

    # texture pool
    try:
        tr = _load_module("td5_texture_reuse")
        pages = tr.list_pages(level_dir)
        out["textures"] = {"page_count": len(pages), "pages": pages}
    except Exception as e:
        out["textures"] = {"error": str(e)}

    m = _model_pages(level_dir)
    if m is not None:
        out["model"] = m

    props = _mov_props(level_dir, "LEVEL.MOV")
    if props is not None:
        out["props"] = props
    propsb = _mov_props(level_dir, "LEVELB.MOV")
    if propsb is not None:
        out["propsb"] = propsb

    return out


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    lvl = int(sys.argv[1])
    root = sys.argv[2] if len(sys.argv) > 2 else None
    print(json.dumps(describe_track(root, lvl), indent=2))
