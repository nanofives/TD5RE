#!/usr/bin/env python3
"""TD5RE Track Studio -- web GUI to import and edit custom tracks.

Sibling of the Car Studio (re/tools/car_studio/). A self-contained stdlib HTTP
server + three.js viewport for authoring the neutral centerline spec that
td5_trackgen.py turns into a drivable TD5 level:

  - import an existing track (any levelNNN/) back into an editable centerline,
  - or start from a built-in sample / blank,
  - drag/add/delete centerline nodes, set per-node lanes/width/surface,
  - add branches and checkpoints, set params (circuit/P2P, weather, fog, traffic),
  - Build -> writes re/assets/levels/levelNNN/ + registers it in custom_tracks.json
    (via td5_trackgen.build_track), so it's drivable with no recompile.

Run:  python re/tools/track_studio/td5_track_studio.py [--port 8766] [--no-browser]
"""

import argparse
import importlib.util
import io
import json
import os
import posixpath
import re
import sys
import threading
import traceback
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STUDIO_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.dirname(STUDIO_DIR)                       # re/tools
REPO_ROOT = os.path.dirname(os.path.dirname(TOOLS_DIR))       # .../TD5RE
VENDOR_DIR = os.path.join(STUDIO_DIR, "vendor")

ASSETS_DIR = os.path.join(REPO_ROOT, "re", "assets")
LIGHTS_DIR = os.path.join(REPO_ROOT, "td5mod", "src", "td5re", "lights")
THREE_VER = "0.160.0"
CDN = f"https://unpkg.com/three@{THREE_VER}"
ENTRY_ADDONS = ["controls/OrbitControls.js", "loaders/GLTFLoader.js"]  # GLTFLoader for env geometry
USE_LOCAL_VENDOR = False


# --------------------------------------------------------------------------
# Load the converter module (re/tools/td5_trackgen.py) -- the build engine.
# --------------------------------------------------------------------------
def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


trackgen = _load_module("td5_trackgen", os.path.join(TOOLS_DIR, "td5_trackgen.py"))
try:
    mesh_tool = _load_module("mesh_tool", os.path.join(TOOLS_DIR, "mesh_tool.py"))  # needs numpy
except Exception as _e:  # noqa
    mesh_tool = None
_page_cache = {}   # (level, page) -> rgba png bytes with transparency baked


# --------------------------------------------------------------------------
# three.js vendoring (mirrors car_studio: download on first run, CDN fallback).
# --------------------------------------------------------------------------
def _fetch(url):
    import urllib.request
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read()


def ensure_vendor(revendor=False):
    global USE_LOCAL_VENDOR
    three_path = os.path.join(VENDOR_DIR, "three.module.js")
    jsm_dir = os.path.join(VENDOR_DIR, "jsm")
    have = all(os.path.isfile(os.path.join(jsm_dir, a)) for a in ENTRY_ADDONS)
    if not revendor and os.path.isfile(three_path) and have:
        USE_LOCAL_VENDOR = True
        return True
    try:
        os.makedirs(jsm_dir, exist_ok=True)
        print(f"  vendoring three.js r{THREE_VER} -> {os.path.relpath(VENDOR_DIR)} ...")
        with open(three_path, "wb") as f:
            f.write(_fetch(f"{CDN}/build/three.module.js"))
        seen, queue, n = set(), list(ENTRY_ADDONS), 0
        while queue and n < 100:
            rel = queue.pop()
            if rel in seen:
                continue
            seen.add(rel); n += 1
            src = _fetch(f"{CDN}/examples/jsm/{rel}").decode("utf-8")
            dst = os.path.join(jsm_dir, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "w", encoding="utf-8") as f:
                f.write(src)
            for ref in re.findall(r"""from\s+['"]([^'"]+)['"]""", src):
                if ref == "three":
                    continue
                if ref.startswith("three/addons/"):
                    queue.append(ref[len("three/addons/"):])
                elif ref.startswith("./") or ref.startswith("../"):
                    queue.append(posixpath.normpath(posixpath.join(posixpath.dirname(rel), ref)))
        USE_LOCAL_VENDOR = True
        print(f"  vendored {n} addon file(s) + three core.")
        return True
    except Exception as e:
        print(f"  three.js download failed ({e}); using CDN importmap (needs internet).")
        USE_LOCAL_VENDOR = False
        return False


def importmap_json():
    if USE_LOCAL_VENDOR:
        return json.dumps({"imports": {"three": "./vendor/three.module.js",
                                       "three/addons/": "./vendor/jsm/"}})
    return json.dumps({"imports": {"three": f"{CDN}/build/three.module.js",
                                   "three/addons/": f"{CDN}/examples/jsm/"}})


# --------------------------------------------------------------------------
# API helpers
# --------------------------------------------------------------------------
def _levels_dir():
    return trackgen.levels_dir(ASSETS_DIR)


# level number -> display name, mirroring the engine tables
# (td5_frontend.c s_track_display_names + the schedule maps; td5_asset.c
# k_td6_menu_slots). Lets the import list show "MOSCOW, RUSSIA" not "LEVEL 23".
def _level_names():
    names = ["DRAG STRIP", "MONTEGO BAY, JAMAICA", "HOUSE OF BEZ, ENGLAND",
             "NEWCASTLE, ENGLAND", "MAUI, HAWAII, USA", "COURMAYEUR, ITALY",
             "JARASH, JORDAN", "CHEDDAR CHEESE, ENGLAND", "MOSCOW, RUSSIA",
             "BLUE RIDGE PARKWAY, NC, USA", "EDINBURGH, SCOTLAND", "TOKYO, JAPAN",
             "SYDNEY, AUSTRALIA", "HONOLULU, HAWAII, USA", "MUNICH, GERMANY",
             "WASHINGTON, DC, USA", "KYOTO, JAPAN", "BERN, SWITZERLAND",
             "SAN FRANCISCO, CA, USA", "KESWICK, ENGLAND",
             "CHAMPIONSHIP CUP", "ERA CUP", "CHALLENGE CUP", "PITBULL CUP",
             "MASTERS CUP", "ULTIMATE CUP",
             "PELTON RACEWAY", "IRELAND", "LAKE TAHOE, USA", "CAPE HATTERAS, USA",
             "SWITZERLAND", "EGYPT", "PARIS, FRANCE", "NEW YORK, USA", "ROME, ITALY",
             "HONG KONG, CHINA", "LONDON, ENGLAND"]
    sched_name = [8, 10, 12, 9, 6, 3, 4, 5, 13, 11, 19, 18, 17, 16, 15, 14, 7, 1, 2]
    sched_pool = [11, 9, 7, 10, 13, 16, 15, 14, 6, 8, 0, 1, 2, 3, 4, 5, 12, 18, 17]
    pool_zip = [1, 2, 3, 4, 5, 6, 13, 14, 15, 16, 17, 23, 25, 26, 27, 28, 29, 37, 39]
    m = {}
    for s in range(19):
        m[pool_zip[sched_pool[s]]] = names[sched_name[s]]
    for lvl, ni in {7: 26, 18: 27, 19: 28, 20: 29, 21: 30, 22: 31,
                    8: 32, 9: 33, 10: 34, 11: 35, 12: 36}.items():
        m[lvl] = names[ni]
    m[30] = "DRAG STRIP"
    return m


def list_tracks():
    """Custom tracks (from the manifest) + every levelNNN/ available to import."""
    manifest = trackgen.load_manifest(ASSETS_DIR)
    custom_by_level = {int(t["level"]): t for t in manifest.get("tracks", [])}
    names = _level_names()
    levels = []
    ld = _levels_dir()
    if os.path.isdir(ld):
        for name in sorted(os.listdir(ld)):
            if not (name.startswith("level") and name[5:8].isdigit()):
                continue
            num = int(name[5:8])
            strip = os.path.join(ld, name, "strip.json")
            if not os.path.isfile(strip):
                continue
            entry = custom_by_level.get(num)
            disp = entry["name"] if entry else names.get(num, "LEVEL %d" % num)
            levels.append({"level": num, "name": disp, "custom": bool(entry),
                           "slot": entry["slot"] if entry else None})
    return {"custom": manifest.get("tracks", []), "levels": levels,
            "slot_base": trackgen.CUSTOM_SLOT_BASE}


def do_import(level, name=None):
    level = int(level)
    manifest = trackgen.load_manifest(ASSETS_DIR)
    custom = next((t for t in manifest.get("tracks", []) if int(t["level"]) == level), None)
    if not name:
        name = (custom["name"] if custom else None) or _level_names().get(level)
    spec, warnings = trackgen.extract_track(ASSETS_DIR, level, name=name, decimate_to=110)
    if custom:                       # so a rebuild overwrites the same slot/level
        spec["_slot"] = int(custom["slot"]); spec["_level"] = level
    return {"ok": True, "spec": spec, "warnings": warnings}


def _level_dir(level):
    return os.path.join(_levels_dir(), "level%03d" % int(level))


def _pages_dir(level):
    return os.path.join(_level_dir(level), "textures.src", "pages")


def _texjson(level):
    p = os.path.join(_level_dir(level), "textures.src", "textures.json")
    if os.path.isfile(p):
        with open(p) as f:
            return json.load(f)
    return None


def _page_types(level):
    """page index -> type (0 opaque, 1 index-0 keyed, 2 semi, 3 additive)."""
    tj = _texjson(level)
    return {i: int(p.get("type", 0)) for i, p in enumerate(tj.get("pages", []))} if tj else {}


def list_assets(level):
    """Real per-page textures (textures.src/pages/page_NNN.png), their transparency
    types, skybox images, and whether env geometry exists -- backs the loaders."""
    ld = _level_dir(level)
    pdir = _pages_dir(level)
    if os.path.isdir(pdir):
        textures = sorted(f for f in os.listdir(pdir) if f.lower().endswith(".png"))
    else:                                        # fall back to the (stale) flat textures/
        td = os.path.join(ld, "textures")
        textures = sorted(f for f in os.listdir(td) if f.lower().endswith((".png", ".tga"))) if os.path.isdir(td) else []
    sky = [f for f in ("forwsky.png", "backsky.png") if os.path.isfile(os.path.join(ld, f))]
    return {"textures": textures, "skybox": sky, "page_types": _page_types(level),
            "has_models": os.path.isfile(os.path.join(ld, "models.bin")) and mesh_tool is not None}


def _decode_page_rgba(level, page):
    """page_NNN.png with alpha baked per the page 'type' (faithful to the engine's
    keyed/semi/additive decode): type 1/3 -> palette index 0 transparent, type 2 ->
    half alpha. Returns PNG bytes, or None to fall back to the raw file."""
    key = (int(level), int(page))
    if key in _page_cache:
        return _page_cache[key]
    png_path = os.path.join(_pages_dir(level), "page_%03d.png" % int(page))
    if not os.path.isfile(png_path):
        return None
    try:
        from PIL import Image
        import numpy as np
    except Exception:
        return None
    tj = _texjson(level)
    pages = tj.get("pages", []) if tj else []
    ptype = int(pages[page].get("type", 0)) if page < len(pages) else 0
    arr = np.array(Image.open(png_path).convert("RGBA"))
    if ptype in (1, 3):
        idx_path = os.path.join(_level_dir(level), "textures.src", "indices.bin")
        if os.path.isfile(idx_path):
            with open(idx_path, "rb") as f:
                f.seek(int(page) * 4096)
                idx = np.frombuffer(f.read(4096), dtype=np.uint8)
            if idx.size == 4096:
                arr[:, :, 3] = np.where(idx.reshape(64, 64) == 0, 0, 255).astype(np.uint8)
    elif ptype == 2:
        arr[:, :, 3] = 0x80
    out = io.BytesIO()
    Image.fromarray(arr, "RGBA").save(out, "PNG")
    _page_cache[key] = out.getvalue()
    return _page_cache[key]


def serve_asset(level, name):
    """(bytes, content_type) for a level page texture / skybox PNG, or None.
    Page textures get their transparency baked in from the page type."""
    safe = os.path.basename(name or "")
    m = re.match(r"page_(\d+)\.png$", safe)
    if m:
        dec = _decode_page_rgba(level, int(m.group(1)))
        if dec:
            return dec, "image/png"
    ld = _level_dir(level)
    for cand in (os.path.join(_pages_dir(level), safe),
                 os.path.join(ld, "textures", safe), os.path.join(ld, safe)):
        if os.path.isfile(cand):
            ct = "image/png" if safe.lower().endswith(".png") else "application/octet-stream"
            with open(cand, "rb") as f:
                return f.read(), ct
    return None


# NOTE: there was a second, separate whole-level exporter here
# (build_model_glb). It decoded models.bin again, told billboards from
# structural meshes by comparing vertex magnitude against bounding-centre
# magnitude, and baked the billboards FLAT -- so the track view and the library
# view of the same level did not agree, and only one of them was pickable.
# build_prims_glb is now the single source: it splits page-exactly, tags every
# vertex with _PRIMID, and leaves billboards to the client to draw
# camera-facing off the mesh HEADER TAG rather than a magnitude heuristic.


def glb_from_page_groups(pos_by, uv_by):
    """Pack {page -> flat triangle positions} + {page -> uvs} into a GLB, one
    node per page with the page id in mesh extras so the client can fetch the
    right texture. Shared by the whole-level view and the prefab preview -- they
    differ only in where the triangles come from."""
    import numpy as np
    gb = mesh_tool._Glb()
    meshes, nodes = [], []
    for page in sorted(pos_by):
        if not pos_by[page]:
            continue
        P = np.array(pos_by[page], np.float32).reshape(-1, 3)
        U = np.array(uv_by[page], np.float32).reshape(-1, 2)
        attrs = {"POSITION": gb.add(P, mesh_tool.COMP_FLOAT, "VEC3", minmax=True),
                 "TEXCOORD_0": gb.add(U, mesh_tool.COMP_FLOAT, "VEC2")}
        meshes.append({"primitives": [{"attributes": attrs, "mode": 4}],
                       "extras": {"page": int(page)}})
        nodes.append({"mesh": len(meshes) - 1})
    gltf = {"asset": {"version": "2.0", "generator": "td5_track_studio"},
            "buffers": [{"byteLength": len(gb.bin)}],
            "bufferViews": gb.bufferViews, "accessors": gb.accessors,
            "meshes": meshes, "nodes": nodes,
            "scenes": [{"nodes": list(range(len(nodes)))}], "scene": 0}
    return mesh_tool._pack_glb(gltf, bytes(gb.bin))


# --------------------------------------------------------------------------
# LIBRARY browser -- the shipped-geometry catalogue built by
# re/tools/td5_geomlib.py. Read-only over library.json / objects/*.jsonl, plus
# ONE writable file: tags.json, where a human overrides the classifier.
#
# Overrides live in their own file rather than being written back into the
# catalogue, because the catalogue is REGENERABLE -- re-running the sweep must
# not throw away hand corrections, and a wrong verdict fixed here has to survive
# the next `td5_geomlib.py build`.
# --------------------------------------------------------------------------
LIBRARY_DIR = os.path.join(ASSETS_DIR, "library")
_prefab_glb_cache = {}
_geomlib = None


def _lib():
    """td5_geomlib, loaded lazily: it pulls in numpy and PIL, and the studio is
    still usable for track editing without them."""
    global _geomlib
    if _geomlib is None:
        _geomlib = _load_module("td5_geomlib",
                                os.path.join(TOOLS_DIR, "td5_geomlib.py"))
    return _geomlib


def _tags_path():
    return os.path.join(LIBRARY_DIR, "tags.json")


def _load_tags():
    return _load_json(_tags_path()) or {}


def library_index():
    """Catalogue summary + which levels have objects + the curated road sets."""
    idx = _load_json(os.path.join(LIBRARY_DIR, "library.json"))
    if not idx:
        return {"ok": False,
                "error": "no library at %s -- run: python re/tools/td5_geomlib.py "
                         "build" % os.path.relpath(LIBRARY_DIR, REPO_ROOT)}
    levels = []
    for name in sorted(os.listdir(os.path.join(LIBRARY_DIR, "objects"))
                       if os.path.isdir(os.path.join(LIBRARY_DIR, "objects")) else []):
        if name.startswith("level") and name.endswith(".jsonl"):
            levels.append(int(name[5:8]))
    pages = (idx.get("pages") or {}).get("levels") or {}
    return {"ok": True, "levels": levels,
            "generated": idx.get("generated"),
            "kind_totals": (idx.get("objects") or {}).get("kind_totals") or {},
            "page_levels": {str(k): v for k, v in pages.items()},
            "roads": _load_json(os.path.join(TOOLS_DIR, "manifests", "roads.json")),
            "tags": _load_tags()}


def library_objects(level, kind=None, min_faces=0, limit=400):
    """Rows for one level, newest classifier verdict with any human override
    applied on top, biggest first."""
    path = os.path.join(LIBRARY_DIR, "objects", "level%03d.jsonl" % int(level))
    if not os.path.isfile(path):
        return {"ok": False, "error": "no objects for level %s" % level}
    tags = _load_tags()
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            o = json.loads(line)
            t = tags.get(o["id"])
            if t and t.get("kind"):
                o["kind"] = t["kind"]
                o["overridden"] = True
            if kind and o["kind"] != kind:
                continue
            if o["faces"] < int(min_faces):
                continue
            rows.append(o)
    rows.sort(key=lambda r: -r["faces"])
    return {"ok": True, "level": int(level), "total": len(rows),
            "objects": rows[:int(limit)]}


# --------------------------------------------------------------------------
# FREE SELECTION over a whole track.
#
# Automatic segmentation gets the big pieces right and is wrong at the edges --
# where one building ends and its neighbour begins is not recoverable from the
# format, which carries no object grouping at all. So the browser lets a human
# select geometry directly and say what it is.
#
# The selectable unit is the PRIMITIVE: one page-exact run of faces, the finest
# thing the format has an unambiguous seam for. level023 is 31,570 of them but
# only 109,269 triangles, so the whole track fits in one scene and selection
# needs no region streaming.
#
# Picking works by carrying a per-vertex _PRIMID through the GLB. Geometry is
# still grouped one node per page (458 draw calls, same as the env view), and a
# raycast hit gives a face index, which reads back the id. That is much cheaper
# than 31,570 separate meshes.
# --------------------------------------------------------------------------
_prims_cache = {}       # level -> (prims, glb, index)


def _level_prims_cached(level):
    level = int(level)
    if level in _prims_cache:
        return _prims_cache[level]
    gl = _lib()
    with open(os.path.join(LIBRARY_DIR, "pages.json"), encoding="utf-8") as f:
        pages_doc = json.load(f)
    role = {p["page"]: p["role"] for p in pages_doc["pages"]
            if p["level"] == level}
    model = gl._load_model(gl._levels_dir(), level)
    # EVERY primitive, not just the ones segmentation cares about: in a
    # selection view, geometry you cannot see is geometry you cannot pick.
    prims = gl.al.all_prims(model, role)
    for i, p in enumerate(prims):
        p["_pid"] = i
    _prims_cache[level] = (prims, None, None)
    return _prims_cache[level]


def _prims_center(prims):
    """Track centre. The whole level sits in raw world coordinates -- level023
    is centred near (623121, -344, 295836) -- so geometry added at the scene
    origin lands hundreds of thousands of units from the camera and looks like
    nothing loaded at all. The env view already offsets by -centre; the
    selection view has to do the same."""
    if not prims:
        return [0.0, 0.0, 0.0]
    mn = [min(p["aabb"][i] for p in prims) for i in range(3)]
    mx = [max(p["aabb"][i + 3] for p in prims) for i in range(3)]
    return [round((mn[i] + mx[i]) * 0.5, 1) for i in range(3)]


def build_prims_glb(level):
    """Whole level as pickable geometry: one node per page, every vertex tagged
    with its primitive id in _PRIMID."""
    import numpy as np
    from collections import defaultdict
    level = int(level)
    prims, glb, idx = _level_prims_cached(level)
    if glb is not None:
        return glb
    pos_by, uv_by, id_by = defaultdict(list), defaultdict(list), defaultdict(list)
    for p in prims:
        if p.get("billboard"):
            continue          # drawn camera-facing by the client, not baked here
        pid = float(p["_pid"])
        vs = p["mesh"]["vertices"]
        cur = 0
        for c in p["mesh"]["commands"]:
            tri, quad, page = int(c["tri"]), int(c["quad"]), int(c["texture_page_id"])
            for t in range(tri):
                for k in (cur + t * 3, cur + t * 3 + 1, cur + t * 3 + 2):
                    v = vs[k]
                    pos_by[page].append(v["pos"]); uv_by[page].append(v["tex"])
                    id_by[page].append([pid])
            qb = cur + tri * 3
            for q in range(quad):
                b = qb + q * 4
                for k in (b, b + 1, b + 2, b, b + 2, b + 3):
                    v = vs[k]
                    pos_by[page].append(v["pos"]); uv_by[page].append(v["tex"])
                    id_by[page].append([pid])
            cur += tri * 3 + quad * 4

    gb = mesh_tool._Glb()
    meshes, nodes = [], []
    for page in sorted(pos_by):
        P = np.array(pos_by[page], np.float32).reshape(-1, 3)
        U = np.array(uv_by[page], np.float32).reshape(-1, 2)
        I = np.array(id_by[page], np.float32).reshape(-1, 1)
        attrs = {"POSITION": gb.add(P, mesh_tool.COMP_FLOAT, "VEC3", minmax=True),
                 "TEXCOORD_0": gb.add(U, mesh_tool.COMP_FLOAT, "VEC2"),
                 "_PRIMID": gb.add(I, mesh_tool.COMP_FLOAT, "SCALAR")}
        meshes.append({"primitives": [{"attributes": attrs, "mode": 4}],
                       "extras": {"page": int(page)}})
        nodes.append({"mesh": len(meshes) - 1})
    gltf = {"asset": {"version": "2.0", "generator": "td5_track_studio"},
            "buffers": [{"byteLength": len(gb.bin)}],
            "bufferViews": gb.bufferViews, "accessors": gb.accessors,
            "meshes": meshes, "nodes": nodes,
            "scenes": [{"nodes": list(range(len(nodes)))}], "scene": 0}
    glb = mesh_tool._pack_glb(gltf, bytes(gb.bin))
    _prims_cache[level] = (prims, glb, idx)
    return glb


def library_overview():
    """Everything the library currently HOLDS, in one answer.

    Deliberately reports what is on disk rather than what could be regenerated,
    because the interesting question is "is my catalogue stale" -- the pieces
    come from several passes (catalogue sweep, road curation, landmark export,
    hand selections) that are regenerated independently and can drift apart."""
    idx = _load_json(os.path.join(LIBRARY_DIR, "library.json")) or {}
    roads = _load_json(os.path.join(TOOLS_DIR, "manifests", "roads.json")) or {}
    lms = _load_json(os.path.join(TOOLS_DIR, "manifests", "landmarks.json")) or {}
    tags = _load_tags()
    sels = (_load_json(_selections_path()) or {}).get("selections", [])

    objdir = os.path.join(LIBRARY_DIR, "objects")
    levels = sorted(int(n[5:8]) for n in os.listdir(objdir)
                    if n.startswith("level") and n.endswith(".jsonl")) \
        if os.path.isdir(objdir) else []

    sel_by_kind = {}
    for s in sels:
        sel_by_kind[s["kind"]] = sel_by_kind.get(s["kind"], 0) + 1

    hdr = os.path.join(REPO_ROOT, "td5mod", "src", "td5re", "td5_tg_prefab_data.h")
    prefab_n = 0
    if os.path.isfile(hdr):
        with open(hdr, encoding="utf-8", errors="ignore") as f:
            for line in f:
                if line.startswith("#define TD5_TG_PREFAB_N"):
                    prefab_n = int(line.split()[-1])
                    break
    return {
        "ok": True,
        "generated": idx.get("generated"),
        "levels": levels,
        "objects_total": sum((idx.get("objects") or {}).get("kind_totals", {}).values()),
        "kind_totals": (idx.get("objects") or {}).get("kind_totals", {}),
        "page_total": (idx.get("pages") or {}).get("page_total"),
        "page_roles": (idx.get("pages") or {}).get("role_totals", {}),
        "night": (idx.get("pages") or {}).get("night_measured", []),
        "road_sets": {k: len(v) for k, v in (roads.get("sets") or {}).items()},
        "landmark_pages": len((lms.get("sets") or {}).get("pf", [])),
        "prefabs_in_build": prefab_n,
        "tags": len(tags),
        "selections": len(sels),
        "selections_by_kind": sel_by_kind,
    }


def library_billboards(level):
    """Billboard primitives as centre + size + page, for the client to draw
    camera-facing.

    They cannot be baked into the merged geometry like everything else: the
    engine rebuilds a billboard against the camera basis every frame
    (td5_render_mesh.c), storing its world position in `origin` and its
    vertices locally. Baked flat they show edge-on or face an arbitrary
    direction. 1406 of level023's primitives are billboards -- trees, signs,
    lamp glows -- so this is not a rounding error in the view."""
    prims, _glb, _idx = _level_prims_cached(level)
    out = []
    for p in prims:
        if not p.get("billboard"):
            continue
        a = p["aabb"]
        out.append({"id": p["_pid"], "page": p["pages"][0],
                    "role": p.get("role"),
                    "c": [round((a[0] + a[3]) * 0.5, 1), round((a[1] + a[4]) * 0.5, 1),
                          round((a[2] + a[5]) * 0.5, 1)],
                    "w": round(max(a[3] - a[0], a[5] - a[2]), 1),
                    "h": round(a[4] - a[1], 1)})
    return {"ok": True, "level": int(level), "count": len(out),
            "center": _prims_center(prims), "billboards": out}


def library_prim_index(level):
    """Metadata for every selectable primitive, parallel to _PRIMID."""
    prims, glb, idx = _level_prims_cached(level)
    if idx is None:
        idx = [{"id": p["_pid"], "page": p["pages"][0], "pages": p["pages"],
                "role": p.get("role"), "faces": p["nface"],
                "kind": p.get("kind_hint"),
                "structure": p.get("kind_hint") == "structure",
                "billboard": bool(p.get("billboard")),
                "aabb": [round(v, 1) for v in p["aabb"]],
                "extent": [round(v, 1) for v in p["extent"]]}
               for p in prims]
        _prims_cache[int(level)] = (prims, glb, idx)
    kinds = {}
    for p in idx:
        kinds[p["kind"]] = kinds.get(p["kind"], 0) + 1
    return {"ok": True, "level": int(level), "count": len(idx),
            "center": _prims_center(prims), "kinds": kinds, "prims": idx}


# Categories a selection can be filed under. These are the kinds the auto-track
# side already understands, so a hand-made selection lands in the same taxonomy
# the segmenter and the generator use -- not a parallel vocabulary.
SELECTION_KINDS = ("landmark", "building", "facade", "plaza", "verge",
                   "rail", "post", "sign", "tree", "road", "exclude")


def _selections_path():
    return os.path.join(LIBRARY_DIR, "selections.json")


def save_selection(req):
    """Persist one hand-made selection: a set of primitive ids plus the category
    the user filed it under. Merged by name, so re-saving edits in place.

    A selection may optionally carry FACES as [prim_id, face_index_within_prim]
    pairs. The index is local to the primitive, not to the merged page buffer
    the viewer draws, because that buffer is a rendering detail that changes
    whenever the GLB packer changes -- a primitive's own face order is the
    MODELS.DAT command order and is stable. An empty `faces` means "the whole
    primitive", which is the common case."""
    level = int(req.get("level", -1))
    name = (req.get("name") or "").strip()
    kind = req.get("kind")
    ids = req.get("ids") or []
    faces = req.get("faces") or []
    if level <= 0 or not name:
        return False, {"error": "need level + name"}
    if kind not in SELECTION_KINDS and not req.get("delete"):
        return False, {"error": "kind must be one of %s" % (SELECTION_KINDS,)}
    doc = _load_json(_selections_path()) or {"_format": "td5_selections",
                                             "_version": 1, "selections": []}
    sel = [s for s in doc["selections"]
           if not (s["level"] == level and s["name"] == name)]
    if not req.get("delete"):
        row = {"level": level, "name": name, "kind": kind,
               "ids": sorted(int(i) for i in ids)}
        if faces:
            row["faces"] = sorted([int(a), int(b)] for a, b in faces)
        sel.append(row)
    doc["selections"] = sorted(sel, key=lambda s: (s["level"], s["name"]))
    os.makedirs(LIBRARY_DIR, exist_ok=True)
    with open(_selections_path(), "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    return True, {"ok": True, "name": name, "kind": kind, "prims": len(ids),
                  "faces": len(faces), "total": len(doc["selections"]),
                  "path": os.path.relpath(_selections_path(), REPO_ROOT).replace("\\", "/")}


def library_genkit():
    """What the AUTO TRACK GENERATOR can actually place, as opposed to what the
    catalogue on disk holds.

    The distinction matters and is easy to lose: the catalogue is ~205k objects
    swept out of 38 shipped levels, but the generator only reaches the subset
    that was baked into generated headers and compiled in. Everything reported
    here is parsed back out of those headers, so it cannot drift from the build
    the way a hand-maintained list would."""
    src = os.path.join(REPO_ROOT, "td5mod", "src", "td5re")
    prefabs, count = [], 0
    hdr = os.path.join(src, "td5_tg_prefab_data.h")
    if os.path.isfile(hdr):
        # Provenance lives in the per-prefab banner comment the exporter emits,
        # e.g. "/* L23.lm00  258 faces, 975 verts, 48 cmds, footprint AxB, height H */"
        pat = re.compile(r"^/\*\s+(L\d+\.\w+)\s+(\d+) faces, (\d+) verts, (\d+) cmds, "
                         r"footprint (\d+)x(\d+), height (\d+)")
        with open(hdr, encoding="utf-8", errors="ignore") as f:
            for line in f:
                if line.startswith("#define TD5_TG_PREFAB_N"):
                    count = int(line.split()[-1])
                m = pat.match(line)
                if m:
                    prefabs.append({"id": m.group(1), "faces": int(m.group(2)),
                                    "verts": int(m.group(3)), "cmds": int(m.group(4)),
                                    "footprint": [int(m.group(5)), int(m.group(6))],
                                    "height": int(m.group(7))})
    roads = (_load_json(os.path.join(TOOLS_DIR, "manifests", "roads.json")) or {}).get("sets") or {}
    lms = (_load_json(os.path.join(TOOLS_DIR, "manifests", "landmarks.json")) or {}).get("sets") or {}
    return {"ok": True, "prefab_count": count, "prefabs": prefabs,
            "roads": {k: [{"level": int(str(a)[5:]), "page": b, "desc": c}
                          for a, b, c in v] for k, v in roads.items()},
            "landmark_pages": len(lms.get("pf", [])),
            "header": os.path.relpath(hdr, REPO_ROOT).replace("\\", "/")}


def list_selections(level=None):
    doc = _load_json(_selections_path()) or {"selections": []}
    sel = doc["selections"]
    if level is not None:
        sel = [s for s in sel if s["level"] == int(level)]
    return {"ok": True, "kinds": list(SELECTION_KINDS), "selections": sel}


_lm_cache = {}          # level -> segmented landmark objects


def _landmarks(level):
    """Rarity-seeded set pieces for one level, segmented over the WHOLE track.

    Distinct from the object catalogue on purpose: catalogue objects are chunks
    of street frontage (TD5 streetscape is per-span wall quads and neighbours
    share wall pages, so anything grown by proximity chains along the kerb).
    These are the pieces the generator actually ships. Cached -- segmentation
    re-splits every mesh in the level."""
    level = int(level)
    if level in _lm_cache:
        return _lm_cache[level]
    gl = _lib()
    with open(os.path.join(LIBRARY_DIR, "pages.json"), encoding="utf-8") as f:
        pages_doc = json.load(f)
    role = {p["page"]: p["role"] for p in pages_doc["pages"]
            if p["level"] == level}
    model = gl._load_model(gl._levels_dir(), level)
    _lm_cache[level] = gl.al.extract_landmarks(model, role)
    return _lm_cache[level]


def library_landmarks(level):
    out = []
    for i, o in enumerate(_landmarks(level)):
        out.append({"id": "L%d.lm%02d" % (int(level), i), "level": int(level),
                    "kind": "landmark", "faces": o["nface"],
                    "pages": o["pages"], "slabs": o.get("slabs", 0),
                    "rarity": o.get("rarity", 0),
                    "extent": [round(v, 1) for v in o["extent"]],
                    "aabb": [round(v, 1) for v in o["aabb"]],
                    "prims": len(o["prims"])})
    return {"ok": True, "level": int(level), "total": len(out), "objects": out}


def build_landmark_glb(level, idx, fill=False):
    """GLB for one segmented landmark, in the local frame the C emitter uses.

    fill=True lays a continuous grass plane across the footprint to close the
    holes the segmenter leaves in the plaza (fill_landmark_holes)."""
    from collections import defaultdict
    import copy
    gl = _lib()
    found = _landmarks(level)
    if not (0 <= idx < len(found)):
        raise ValueError("no landmark %d on level %s" % (idx, level))
    lm = found[idx]
    lm_id = "L%s.lm%02d" % (level, idx)
    patches = []
    # AUTHORED fills (account3 'Claude authoring with web reference') are curated
    # truth, so they always load -- they are not the deterministic guess.
    patches.extend(gl.load_authored_fills(lm_id))
    if fill:
        gp = gl.fill_landmark_holes(lm)
        if gp:
            patches.append(gp)
        patches.extend(gl.fill_landmark_walls(lm))
    if patches:
        lm = dict(lm)
        lm["prims"] = list(lm["prims"]) + patches
        lm["nface"] = lm.get("nface", 0) + sum(p["nface"] for p in patches)
    g = gl._prefab_from_landmark(lm, "L%s.lm%02d" % (level, idx))
    pos_by, uv_by = defaultdict(list), defaultdict(list)
    cur = 0
    for page, tri, quad in g["cmds"]:
        for t in range(tri):
            for k in (cur + t * 3, cur + t * 3 + 1, cur + t * 3 + 2):
                v = g["local"][k]
                pos_by[page].append([v[0], v[1], v[2]]); uv_by[page].append([v[3], v[4]])
        qb = cur + tri * 3
        for q in range(quad):
            b = qb + q * 4
            for k in (b, b + 1, b + 2, b, b + 2, b + 3):
                v = g["local"][k]
                pos_by[page].append([v[0], v[1], v[2]]); uv_by[page].append([v[3], v[4]])
        cur += tri * 3 + quad * 4
    return glb_from_page_groups(pos_by, uv_by)


def build_prefab_glb(obj_id):
    """GLB for ONE catalogued object, in its own local frame (centred in XZ,
    base y=0) -- the same convention the C emitter places it with, so what the
    browser shows is what the generator will stamp."""
    if mesh_tool is None:
        raise RuntimeError("mesh_tool/numpy unavailable")
    from collections import defaultdict
    if obj_id in _prefab_glb_cache:
        return _prefab_glb_cache[obj_id]
    m = re.match(r"^L(\d+)\.e(\d+)\.s(\d+)\.o(\d+)$", obj_id or "")
    if not m:
        raise ValueError("bad object id %r" % obj_id)
    level, entry, slot, obj = (int(g) for g in m.groups())
    gl = _lib()
    model = gl._load_model(gl._levels_dir(), level)
    g = gl._prefab_geometry(model, {"id": obj_id, "entry": entry,
                                    "slot": slot, "obj": obj})
    pos_by, uv_by = defaultdict(list), defaultdict(list)
    cur = 0
    for page, tri, quad in g["cmds"]:
        for t in range(tri):
            for k in (cur + t * 3, cur + t * 3 + 1, cur + t * 3 + 2):
                v = g["local"][k]
                pos_by[page].append([v[0], v[1], v[2]]); uv_by[page].append([v[3], v[4]])
        qb = cur + tri * 3
        for q in range(quad):
            b = qb + q * 4
            for k in (b, b + 1, b + 2, b, b + 2, b + 3):   # quad -> two tris
                v = g["local"][k]
                pos_by[page].append([v[0], v[1], v[2]]); uv_by[page].append([v[3], v[4]])
        cur += tri * 3 + quad * 4
    glb = glb_from_page_groups(pos_by, uv_by)
    _prefab_glb_cache[obj_id] = glb
    return glb


def save_library_tags(req):
    """Persist ONE object's human verdict. Merge, never replace: two people (or
    two sessions) tagging different objects must not clobber each other."""
    oid = req.get("id")
    if not oid:
        return False, {"error": "need id"}
    tags = _load_tags()
    entry = tags.get(oid, {})
    for k in ("kind", "biomes", "daynight", "fit", "note"):
        if k in req:
            entry[k] = req[k]
    if req.get("clear"):
        tags.pop(oid, None)
    else:
        tags[oid] = entry
    os.makedirs(LIBRARY_DIR, exist_ok=True)
    with open(_tags_path(), "w", encoding="utf-8", newline="\n") as f:
        json.dump(tags, f, indent=1, sort_keys=True)
        f.write("\n")
    return True, {"ok": True, "id": oid, "tagged": len(tags),
                  "path": os.path.relpath(_tags_path(), REPO_ROOT).replace("\\", "/")}


# --------------------------------------------------------------------------
# Lights editor (curated per-level street lights -- td5_lightsrc.c runtime).
# Reads/writes the TRACKED source files td5mod/src/td5re/lights/, plus the
# TD5RE_LAMP_FREEZE capture seeds (levelNNN_lights.captured.json).
# --------------------------------------------------------------------------
def _lights_path(level, captured=False):
    suffix = "_lights.captured.json" if captured else "_lights.json"
    return os.path.join(LIGHTS_DIR, "level%03d%s" % (int(level), suffix))


def _load_json(path):
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def get_lights(level):
    """Curated lights file + captured seed + the level's additive (type-3) pages
    (suppression candidates -- painted glow halos are additive)."""
    level = int(level)
    additive = sorted(p for p, t in _page_types(level).items() if t == 3)
    return {
        "lights": _load_json(_lights_path(level)),
        "captured": _load_json(_lights_path(level, captured=True)),
        "additive_pages": additive,
        "path": os.path.relpath(_lights_path(level), REPO_ROOT).replace("\\", "/"),
    }


def save_lights(req):
    level = int(req.get("level", -1))
    data = req.get("data")
    if level <= 0 or not isinstance(data, dict):
        return False, {"error": "need level + data"}
    doc = {
        "_format": "td5_lights",
        "_version": 1,
        "level": level,
        "defaults": data.get("defaults") or
                    {"range": 2400, "intensity": 1.0, "color": [1.0, 0.82, 0.55]},
        "lights": [
            {k: round(float(L[k]), 1) if k != "color" else L[k]
             for k in ("x", "y", "z", "range", "intensity", "color") if k in L}
            for L in (data.get("lights") or [])
            if all(k in L for k in ("x", "y", "z"))
        ],
        "emissive_pages": [
            {"page": int(p["page"]), "suppress": bool(p.get("suppress"))}
            for p in (data.get("emissive_pages") or []) if "page" in p
        ],
    }
    os.makedirs(LIGHTS_DIR, exist_ok=True)
    path = _lights_path(level)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    return True, {"ok": True, "path": os.path.relpath(path, REPO_ROOT).replace("\\", "/"),
                  "lights": len(doc["lights"]),
                  "suppressed": sum(1 for p in doc["emissive_pages"] if p["suppress"])}


def do_sample(kind):
    spec = trackgen.sample_spec(kind)
    # return the editable raw form (nodes carry resolved width/lanes already)
    return {"ok": True, "spec": spec, "warnings": []}


def do_loadfile(req):
    """Parse an external centerline file (CSV/JSON, multi-path) and auto-detect
    lanes (from width) + branches (from extra paths) into an editable spec."""
    text = req.get("text", "")
    filename = req.get("filename", "")
    if not text.strip():
        return False, {"error": "empty file"}
    paths, meta = trackgen.parse_paths_text(text, filename)
    base = os.path.splitext(os.path.basename(filename))[0].upper()[:30]
    spec = trackgen.detect_spec_from_paths(paths, name=meta.get("name") or base or "IMPORTED",
                                           circuit=req.get("circuit"), meta=meta)
    lanes = sorted({n.get("lanes", 4) for n in spec["nodes"]})
    warnings = ["lanes detected: %s" % lanes]
    if spec.get("branches"):
        warnings.append("detected %d branch(es) from extra path(s)" % len(spec["branches"]))
    return True, {"ok": True, "spec": spec, "warnings": warnings}


def do_build(req):
    spec = req.get("spec")
    if not isinstance(spec, dict):
        return False, {"error": "missing 'spec'"}
    slot = req.get("slot", spec.get("_slot"))
    level = req.get("level", spec.get("_level"))
    buf = io.StringIO()
    import contextlib
    with contextlib.redirect_stderr(buf):
        res = trackgen.build_track(spec, assets_root=ASSETS_DIR, slot=slot, level=level)
    res["log"] = buf.getvalue()
    res["drive"] = ("td5re.exe --AutoRace=1 --SkipIntro=1 --DefaultTrack=%d"
                    % res["slot"])
    return True, res


# --------------------------------------------------------------------------
# HTTP server
# --------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        if not isinstance(body, (bytes, bytearray)):
            if ctype.startswith("application/json"):
                body = json.dumps(body).encode("utf-8")
            else:
                body = str(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        # This is a live dev tool edited constantly; without an explicit policy
        # the browser heuristically caches track_studio.js and index.html and
        # keeps running yesterday's code after a reload (the "I don't see the
        # change" trap). Always revalidate.
        self.send_header("Cache-Control", "no-store, must-revalidate")
        self.end_headers()
        self.wfile.write(body)

    def _serve_file(self, path, ctype):
        if not os.path.isfile(path):
            self._send(404, {"error": "not found: %s" % os.path.basename(path)})
            return
        with open(path, "rb") as f:
            self._send(200, f.read(), ctype)

    def do_GET(self):
        p = self.path.split("?", 1)[0]
        q = {}
        if "?" in self.path:
            from urllib.parse import parse_qs
            q = {k: v[0] for k, v in parse_qs(self.path.split("?", 1)[1]).items()}
        try:
            if p in ("/", "/index.html"):
                html = open(os.path.join(STUDIO_DIR, "index.html"), encoding="utf-8").read()
                html = html.replace("__IMPORTMAP__", importmap_json())
                self._send(200, html, "text/html; charset=utf-8")
            elif p == "/track_studio.js":
                self._serve_file(os.path.join(STUDIO_DIR, "track_studio.js"),
                                 "text/javascript; charset=utf-8")
            elif p.startswith("/vendor/"):
                safe = posixpath.normpath(p[len("/vendor/"):]).lstrip("/.")
                ctype = "text/javascript" if safe.endswith(".js") else "application/octet-stream"
                self._serve_file(os.path.join(VENDOR_DIR, *safe.split("/")), ctype)
            elif p == "/api/tracks":
                self._send(200, list_tracks())
            elif p == "/api/import":
                self._send(200, do_import(q.get("level"), q.get("name")))
            elif p == "/api/sample":
                self._send(200, do_sample(q.get("kind", "oval")))
            elif p == "/api/assets":
                self._send(200, list_assets(q.get("level")))
            elif p == "/api/asset":
                a = serve_asset(q.get("level"), q.get("name"))
                if a:
                    self._send(200, a[0], a[1])
                else:
                    self._send(404, {"error": "asset not found"})
            elif p == "/api/model":
                # Kept as an alias so a bookmarked URL still works; the track
                # view and the library view must resolve to the SAME bytes.
                self._send(200, build_prims_glb(q.get("level")), "model/gltf-binary")
            elif p == "/api/lights":
                self._send(200, get_lights(q.get("level")))
            elif p == "/api/library":
                self._send(200, library_index())
            elif p == "/api/library/objects":
                self._send(200, library_objects(q.get("level"), q.get("kind"),
                                                q.get("min_faces", 0),
                                                q.get("limit", 400)))
            elif p == "/api/library/landmarks":
                self._send(200, library_landmarks(q.get("level")))
            elif p == "/api/library/prims":
                self._send(200, build_prims_glb(q.get("level")), "model/gltf-binary")
            elif p == "/api/library/primindex":
                self._send(200, library_prim_index(q.get("level")))
            elif p == "/api/library/billboards":
                self._send(200, library_billboards(q.get("level")))
            elif p == "/api/library/overview":
                self._send(200, library_overview())
            elif p == "/api/library/genkit":
                self._send(200, library_genkit())
            elif p == "/api/library/selections":
                self._send(200, list_selections(q.get("level")))
            elif p == "/api/library/prefab":
                # Two id shapes: catalogue objects (L23.e53.s0.o0) and segmented
                # landmarks (L23.lm00). They come from different passes, so the
                # id has to say which.
                oid = q.get("id") or ""
                m = re.match(r"^L(\d+)\.lm(\d+)$", oid)
                if m:
                    self._send(200, build_landmark_glb(int(m.group(1)),
                                                       int(m.group(2)),
                                                       fill=q.get("fill") == "1"),
                               "model/gltf-binary")
                else:
                    self._send(200, build_prefab_glb(oid), "model/gltf-binary")
            elif p == "/api/library/page":
                a = serve_asset(q.get("level"), "page_%03d.png" % int(q.get("page", 0)))
                if a:
                    self._send(200, a[0], a[1])
                else:
                    self._send(404, {"error": "page not found"})
            else:
                self._send(404, {"error": "not found"})
        except Exception as e:
            self._send(500, {"error": str(e), "trace": traceback.format_exc()})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n) if n else b"{}"
        try:
            req = json.loads(raw.decode("utf-8"))
        except Exception as e:
            self._send(400, {"error": f"bad JSON: {e}"})
            return
        if self.path == "/api/build":
            try:
                ok, res = do_build(req)
                self._send(200 if ok else 400, res)
            except Exception as e:
                self._send(500, {"error": str(e), "trace": traceback.format_exc()})
        elif self.path == "/api/loadfile":
            try:
                ok, res = do_loadfile(req)
                self._send(200 if ok else 400, res)
            except Exception as e:
                self._send(500, {"error": str(e), "trace": traceback.format_exc()})
        elif self.path == "/api/lights":
            try:
                ok, res = save_lights(req)
                self._send(200 if ok else 400, res)
            except Exception as e:
                self._send(500, {"error": str(e), "trace": traceback.format_exc()})
        elif self.path == "/api/library/selection":
            try:
                ok, res = save_selection(req)
                self._send(200 if ok else 400, res)
            except Exception as e:
                self._send(500, {"error": str(e), "trace": traceback.format_exc()})
        elif self.path == "/api/library/tags":
            try:
                ok, res = save_library_tags(req)
                self._send(200 if ok else 400, res)
            except Exception as e:
                self._send(500, {"error": str(e), "trace": traceback.format_exc()})
        else:
            self._send(404, {"error": "not found"})


def main():
    ap = argparse.ArgumentParser(description="TD5RE Track Studio (web GUI)")
    ap.add_argument("--port", type=int, default=8766)
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--revendor", action="store_true", help="re-download three.js")
    args = ap.parse_args()

    if not os.path.isdir(STUDIO_DIR):
        print(f"ERROR: {STUDIO_DIR} missing.", file=sys.stderr)
        return 2
    ensure_vendor(args.revendor)

    url = f"http://localhost:{args.port}/"
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"TD5 Track Studio -> {url}  (assets: {os.path.relpath(ASSETS_DIR)})  Ctrl+C to stop")
    if not args.no_browser:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
