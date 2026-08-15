#!/usr/bin/env python3
"""
td5_maptrace.py -- deep read-only TRACE of a TD5 level's geometry + textures, so
an AI author can understand a track's existing content and its surroundings, then
complete/fill gaps in the SAME idiom (matching sizes, heights, setbacks, art).

This is the "eyes" layer on top of td5_describe.py's number summary. Three views:

  trace_geometry(assets_root, level)
      Every world mesh reduced to a world-space AABB + class (road/ground/
      building/billboard/prop), clustered into OBJECTS (native cities have
      thousands of meshes), each tied to its nearest road span with side
      (left/right) + setback. Plus a per-span-bucket profile: how much scenery
      lines each stretch, its median height/setback, and which texture pages it
      uses -- the CONTEXT for matching new geometry to old.

  trace_textures(assets_root, level, out_png)
      Decodes every 64x64 page to RGBA, classifies it (asphalt/wall/foliage/
      sky/sign/...), and renders a labeled MONTAGE contact-sheet PNG so the AI
      can SEE all available art at once (Read the montage) and pick pages that
      match a surface it's extending.

  analyze_gaps(assets_root, level)
      Walks both roadsides and reports EMPTY stretches (span ranges + side +
      length) alongside a local STYLE PROFILE (typical building footprint,
      height, setback, pages nearby) -- the gaps to fill and the idiom to fill
      them in. The AI then places matching scenery via the editor primitives.

World placement is universal: a mesh's world vertices = origin/256 + local pos
(mirrors the renderer: origin is 24.8 integer space scaled by 1/256, and the
generated road/scenery use origin 0 + world verts). Billboards carry world in
origin; opaque scenery carries it in the verts -- both resolve correctly here.

Read-only. No game, no writes except the montage PNG you name.
"""
from __future__ import annotations

import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

CELL = 2500.0                      # object-clustering grid cell (world units)
SPAN_BUCKET = 20                   # spans per profile bucket


def _mod(name):
    import importlib
    return importlib.import_module(name)


def _levels_dir(assets_root):
    return os.path.join(assets_root, "levels")


def _default_assets_root():
    return _mod("td5_trackgen")._default_assets_root()


# ---------------------------------------------------------------------------
# Road centerline (span centers + direction + width), from strip.json
# ---------------------------------------------------------------------------
def _load_centerline(level_dir):
    path = os.path.join(level_dir, "strip.json")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        strip = json.load(f)
    spans, verts = strip["spans"], strip["vertices"]
    ring = max(1, strip["header"][1])
    nv = len(verts)
    centers, widths = [], []
    for i in range(min(ring, len(spans))):
        s = spans[i]
        lanes = max(1, s[3] & 0x0F)
        lvi = s[4]
        if lvi < 0 or lvi >= nv:
            centers.append(None); widths.append(0.0); continue
        ri = min(lvi + lanes, nv - 1)
        L, R = verts[lvi], verts[ri]
        ox, oy, oz = s[8], s[9], s[10]
        cx = ox + (L[0] + R[0]) * 0.5
        cz = oz + (L[2] + R[2]) * 0.5
        cy = oy + (L[1] + R[1]) * 0.5
        centers.append((cx, cy, cz))
        widths.append(math.hypot(R[0] - L[0], R[2] - L[2]))
    return {"ring": ring, "centers": centers, "widths": widths}


def _nearest_span(centerline, x, z):
    """Nearest ring span to (x,z); returns (span_index, dist, side, setback).
    side: -1 left / +1 right relative to travel direction; setback = perpendicular
    distance from the road centerline minus half the road width."""
    centers = centerline["centers"]
    best_i, best_d2 = -1, 1e30
    for i, c in enumerate(centers):
        if c is None:
            continue
        d2 = (c[0] - x) ** 2 + (c[2] - z) ** 2
        if d2 < best_d2:
            best_d2, best_i = d2, i
    if best_i < 0:
        return -1, 0.0, 0, 0.0
    c = centers[best_i]
    # travel direction at best_i (forward difference, wrap)
    j = (best_i + 1) % len(centers)
    cn = centers[j] or c
    dx, dz = cn[0] - c[0], cn[2] - c[2]
    dl = math.hypot(dx, dz) or 1.0
    dx, dz = dx / dl, dz / dl
    # vector from center to point; side = sign of cross(dir, vec) in XZ
    vx, vz = x - c[0], z - c[2]
    cross = dx * vz - dz * vx
    side = 1 if cross >= 0 else -1
    perp = abs(cross)                                  # perpendicular distance
    setback = perp - centerline["widths"][best_i] * 0.5
    return best_i, math.sqrt(best_d2), side, max(0.0, setback)


# ---------------------------------------------------------------------------
# Geometry trace
# ---------------------------------------------------------------------------
def _mesh_world_aabb(mesh):
    ox, oy, oz = mesh["origin"]
    inv = 1.0 / 256.0
    lo = [1e30, 1e30, 1e30]
    hi = [-1e30, -1e30, -1e30]
    for v in mesh["vertices"]:
        p = v["pos"]
        w = (ox * inv + p[0], oy * inv + p[1], oz * inv + p[2])
        for a in range(3):
            if w[a] < lo[a]: lo[a] = w[a]
            if w[a] > hi[a]: hi[a] = w[a]
    if lo[0] > hi[0]:
        # no verts; fall back to bounding_center + radius
        r = mesh["bounding"][0]
        cx, cy, cz = mesh["bounding"][1:4]
        lo = [cx - r, cy - r, cz - r]; hi = [cx + r, cy + r, cz + r]
    return lo, hi


def _classify(lo, hi, header_tag):
    dx, dy, dz = hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]
    if header_tag in (1, 2):
        return "billboard"
    if dy < 500 and max(dx, dz) > 1500:
        return "ground"                                 # road / plaza / terrain
    if dy > 1500:
        return "building"
    return "prop"


def trace_geometry(assets_root=None, level=None):
    """Structured spatial inventory of every world mesh. Returns:
      { summary, objects[], span_profile[] }  (see module docstring)."""
    assets_root = assets_root or _default_assets_root()
    level = int(level)
    level_dir = os.path.join(_levels_dir(assets_root), "level%03d" % level)
    mt = _mod("mesh_tool")
    mpath = os.path.join(level_dir, "models.bin")
    if not os.path.isfile(mpath):
        return {"ok": False, "error": "no models.bin in level%03d" % level}
    model = mt.decode(open(mpath, "rb").read(), "models")
    cl = _load_centerline(level_dir)

    classes = {}
    pages_all = {}
    raw = []                    # per-mesh (lo, hi, cls, pages)
    for m in model["meshes"]:
        lo, hi = _mesh_world_aabb(m)
        cls = _classify(lo, hi, m["texture_page_id"])
        pgs = sorted({int(c["texture_page_id"]) for c in m["commands"]})
        classes[cls] = classes.get(cls, 0) + 1
        for p in pgs:
            pages_all[p] = pages_all.get(p, 0) + 1
        raw.append((lo, hi, cls, pgs))

    # world extents
    wlo = [1e30] * 3; whi = [-1e30] * 3
    for lo, hi, _, _ in raw:
        for a in range(3):
            wlo[a] = min(wlo[a], lo[a]); whi[a] = max(whi[a], hi[a])

    # cluster scenery (building/prop/billboard) into objects by grid cell + class
    buckets = {}
    for lo, hi, cls, pgs in raw:
        if cls == "ground":
            continue                                    # surface, not an "object"
        cx = (lo[0] + hi[0]) * 0.5; cz = (lo[2] + hi[2]) * 0.5
        key = (int(cx // CELL), int(cz // CELL), cls)
        b = buckets.get(key)
        if b is None:
            buckets[key] = b = {"lo": list(lo), "hi": list(hi), "cls": cls,
                                "n": 0, "pages": {}}
        for a in range(3):
            b["lo"][a] = min(b["lo"][a], lo[a]); b["hi"][a] = max(b["hi"][a], hi[a])
        b["n"] += 1
        for p in pgs:
            b["pages"][p] = b["pages"].get(p, 0) + 1

    objects = []
    for b in buckets.values():
        lo, hi = b["lo"], b["hi"]
        cx = (lo[0] + hi[0]) * 0.5; cz = (lo[2] + hi[2]) * 0.5
        cy = (lo[1] + hi[1]) * 0.5
        span, dist, side, setback = (_nearest_span(cl, cx, cz) if cl else (-1, 0, 0, 0))
        objects.append({
            "class": b["cls"], "mesh_count": b["n"],
            "center": [round(cx, 1), round(cy, 1), round(cz, 1)],
            "footprint": [round(hi[0] - lo[0], 1), round(hi[2] - lo[2], 1)],
            "height": round(hi[1] - lo[1], 1),
            "pages": sorted(b["pages"], key=lambda p: -b["pages"][p])[:4],
            "span": span, "side": ("right" if side > 0 else "left"),
            "setback": round(setback, 1),
        })
    objects.sort(key=lambda o: (o["span"], o["center"][0]))

    # per-span-bucket profile (context for matching)
    profile = []
    if cl:
        nb = (cl["ring"] + SPAN_BUCKET - 1) // SPAN_BUCKET
        agg = [{"left": [], "right": [], "pages": {}} for _ in range(nb)]
        for o in objects:
            if o["span"] < 0:
                continue
            bi = o["span"] // SPAN_BUCKET
            if bi >= nb:
                continue
            agg[bi][o["side"]].append(o)
            for p in o["pages"]:
                agg[bi]["pages"][p] = agg[bi]["pages"].get(p, 0) + 1

        def _med(vals):
            vals = sorted(vals)
            return round(vals[len(vals) // 2], 1) if vals else 0.0

        for bi, a in enumerate(agg):
            L, R = a["left"], a["right"]
            allo = L + R
            profile.append({
                "span_start": bi * SPAN_BUCKET,
                "span_end": min((bi + 1) * SPAN_BUCKET - 1, cl["ring"] - 1),
                "left_objects": len(L), "right_objects": len(R),
                "med_height": _med([o["height"] for o in allo]),
                "med_setback": _med([o["setback"] for o in allo]),
                "med_footprint": _med([max(o["footprint"]) for o in allo]),
                "pages": sorted(a["pages"], key=lambda p: -a["pages"][p])[:4],
            })

    return {"ok": True, "level": level,
            "summary": {"mesh_count": len(raw), "object_count": len(objects),
                        "classes": classes,
                        "world_extents": {"min": [round(x, 1) for x in wlo],
                                          "max": [round(x, 1) for x in whi]},
                        "pages_used": sorted(pages_all)},
            "objects": objects, "span_profile": profile}


# ---------------------------------------------------------------------------
# Texture trace + montage
# ---------------------------------------------------------------------------
def _page_rgba(pal_hex, idx):
    pal = bytes.fromhex(pal_hex)                        # BGR triplets
    px = bytearray(64 * 64 * 4)
    ncol = len(pal) // 3
    for i in range(4096):
        c = idx[i]
        if c < ncol:
            b, g, r = pal[c * 3], pal[c * 3 + 1], pal[c * 3 + 2]
        else:
            b = g = r = 0
        px[i * 4] = r; px[i * 4 + 1] = g; px[i * 4 + 2] = b; px[i * 4 + 3] = 255
    return bytes(px)


def _avg_rgb(px):
    """Average (r,g,b) of a 64x64 RGBA buffer."""
    n = 4096; sr = sg = sb = 0
    for i in range(n):
        sr += px[i * 4]; sg += px[i * 4 + 1]; sb += px[i * 4 + 2]
    return sr / n, sg / n, sb / n


# ---- surface-role detection from geometry USAGE (primary) + colour (cue) ----
ROOF_Y = 2500.0            # horizontal face above this world-Y counts as a roof
HORIZ_NY = 0.7             # |face-normal.y| above this => horizontal face


def _tri_ny_area_cen(a, b, c):
    ux, uy, uz = b[0]-a[0], b[1]-a[1], b[2]-a[2]
    vx, vy, vz = c[0]-a[0], c[1]-a[1], c[2]-a[2]
    nx = uy*vz-uz*vy; ny = uz*vx-ux*vz; nz = ux*vy-uy*vx
    mag = math.sqrt(nx*nx+ny*ny+nz*nz) or 1e-9
    return ny/mag, 0.5*mag, ((a[0]+b[0]+c[0])/3.0, (a[1]+b[1]+c[1])/3.0, (a[2]+b[2]+c[2])/3.0)


def _span_grid(cl):
    """Grid of span centres (+ half-width) for O(1) 'is this near the road?'."""
    g = {}
    if not cl:
        return g
    for i, c in enumerate(cl["centers"]):
        if c is None:
            continue
        g.setdefault((int(c[0] // 3000), int(c[2] // 3000)), []).append(
            (c[0], c[2], cl["widths"][i] * 0.5))
    return g


def _near_road(g, x, z):
    cx, cz = int(x // 3000), int(z // 3000)
    for dx in (-1, 0, 1):
        for dz in (-1, 0, 1):
            for (sx, sz, hw) in g.get((cx + dx, cz + dz), ()):
                if (sx - x) ** 2 + (sz - z) ** 2 < (hw + 1500) ** 2:
                    return True
    return False


def _page_usage(model, cl):
    """Per-page face-area accumulated by orientation/height/near-road, so a page's
    surface ROLE follows how the geometry actually uses it. Returns
    page_id -> {low, high, vert, bb, road, area}."""
    grid = _span_grid(cl)
    usage = {}
    for m in model["meshes"]:
        tag = m["texture_page_id"]                      # header billboard tag (1/2)
        ox, oy, oz = m["origin"]; inv = 1.0 / 256.0
        vs = [(ox*inv+v["pos"][0], oy*inv+v["pos"][1], oz*inv+v["pos"][2]) for v in m["vertices"]]
        nv = len(vs); cur = 0
        for c in m["commands"]:
            p = int(c["texture_page_id"])
            u = usage.setdefault(p, {"low": 0.0, "high": 0.0, "vert": 0.0,
                                     "bb": 0.0, "road": 0.0, "area": 0.0})
            faces = []
            for _ in range(c["tri"]):
                if cur + 3 <= nv: faces.append((vs[cur], vs[cur+1], vs[cur+2]))
                cur += 3
            for _ in range(c["quad"]):
                if cur + 4 <= nv:
                    a, b, cc, d = vs[cur], vs[cur+1], vs[cur+2], vs[cur+3]
                    faces.append((a, b, cc)); faces.append((a, cc, d))
                cur += 4
            for (a, b, cc) in faces:
                ny, area, cen = _tri_ny_area_cen(a, b, cc)
                u["area"] += area
                if tag in (1, 2):
                    u["bb"] += area
                elif abs(ny) > HORIZ_NY:
                    if cen[1] > ROOF_Y:
                        u["high"] += area
                    else:
                        u["low"] += area
                        if _near_road(grid, cen[0], cen[2]):
                            u["road"] += area
                else:
                    u["vert"] += area
    return usage


def _final_role(u, rgb):
    """Merge geometry usage (primary) + colour (secondary) into a surface role:
    road | ground | wall | roof | foliage | sky | sign | mixed."""
    r, g, b = rgb
    lum = 0.299*r + 0.587*g + 0.114*b
    mx, mn = max(rgb), min(rgb); sat = (mx - mn) / (mx + 1e-6)
    greenish = g > r + 10 and g > b + 10
    blueish = b > r + 18 and b > g + 8 and lum > 105
    tot = (u["low"] + u["high"] + u["vert"] + u["bb"]) if u else 0.0
    if tot <= 0:                                        # unused page -> colour only
        if blueish: return "sky"
        if greenish: return "foliage"
        if sat > 0.5: return "sign"
        return "ground" if lum >= 95 else "road"
    fbb = u["bb"]/tot; fv = u["vert"]/tot; fh = u["high"]/tot; fl = u["low"]/tot
    if fbb > 0.5:
        return "foliage" if greenish else "sign"
    if fv >= 0.5:
        return "foliage" if greenish else "wall"
    if fh > 0.4 and fh >= fl:
        return "roof"
    if fl > 0.4:
        if greenish: return "ground"
        return "road" if (u["road"] / (u["low"] or 1.0)) > 0.5 else "ground"
    return "mixed"


def trace_textures(assets_root=None, level=None, out_png=None, tile=48, cols=24):
    """Decode every page and detect its surface ROLE (road/ground/wall/roof/
    foliage/sky/sign) from how the geometry USES it (face orientation + height +
    proximity to the road), with colour as a secondary cue. Renders a labeled
    montage PNG (Read it to SEE the art). Returns {pages:[{id,type,role,usage,
    rgb}], montage, page_count, role_counts}."""
    assets_root = assets_root or _default_assets_root()
    level = int(level)
    level_dir = os.path.join(_levels_dir(assets_root), "level%03d" % level)
    tr = _mod("td5_texture_reuse")
    man, indices = tr._load_pool(level_dir)
    pages = man["pages"]
    n = len(pages)

    # geometry usage per page (if the level has a mesh)
    usage = {}
    mpath = os.path.join(level_dir, "models.bin")
    if os.path.isfile(mpath):
        try:
            model = _mod("mesh_tool").decode(open(mpath, "rb").read(), "models")
            usage = _page_usage(model, _load_centerline(level_dir))
        except Exception:
            usage = {}

    infos = []
    rgbas = []
    role_counts = {}
    for i, p in enumerate(pages):
        idx = indices[i * 4096:(i + 1) * 4096]
        rgba = _page_rgba(p.get("palette_hex", ""), idx)
        r, g, b = _avg_rgb(rgba)
        u = usage.get(i)
        role = _final_role(u, (r, g, b))
        role_counts[role] = role_counts.get(role, 0) + 1
        uf = None
        if u and u["area"] > 0:
            t = u["low"] + u["high"] + u["vert"] + u["bb"] or 1.0
            uf = {"low": round(u["low"]/t, 2), "high": round(u["high"]/t, 2),
                  "vert": round(u["vert"]/t, 2), "bb": round(u["bb"]/t, 2),
                  "on_road": round(u["road"]/(u["low"] or 1.0), 2)}
        infos.append({"id": i, "type": int(p.get("type", 0)), "role": role,
                      "usage": uf, "rgb": [round(r), round(g), round(b)]})
        rgbas.append(rgba)

    montage_path = None
    try:
        from PIL import Image, ImageDraw
        label_h = 12
        cell = tile + label_h
        rows = (n + cols - 1) // cols
        W, H = cols * tile, rows * cell
        sheet = Image.new("RGB", (W, H), (30, 30, 34))
        draw = ImageDraw.Draw(sheet)
        for i in range(n):
            img = Image.frombytes("RGBA", (64, 64), rgbas[i]).convert("RGB").resize((tile, tile))
            cx = (i % cols) * tile
            cy = (i // cols) * cell
            sheet.paste(img, (cx, cy))
            draw.text((cx + 1, cy + tile), "%d %s" % (i, infos[i]["role"][:6]),
                      fill=(220, 220, 220))
        out_png = out_png or os.path.join(assets_root, "..", "..", "log",
                                          "texmontage_level%03d.png" % level)
        out_png = os.path.abspath(out_png)
        os.makedirs(os.path.dirname(out_png), exist_ok=True)
        sheet.save(out_png)
        montage_path = out_png
    except Exception as e:
        montage_path = "montage skipped: %s" % e

    return {"ok": True, "level": level, "page_count": n,
            "montage": montage_path, "role_counts": role_counts, "pages": infos}


# ---------------------------------------------------------------------------
# Gap analysis
# ---------------------------------------------------------------------------
def analyze_gaps(assets_root=None, level=None, min_gap_spans=8):
    """Report empty roadside stretches + a local style profile. Uses
    trace_geometry's span_profile. Returns {gaps:[...], style:{...}}."""
    g = trace_geometry(assets_root, level)
    if not g.get("ok"):
        return g
    prof = g["span_profile"]
    objs = g["objects"]

    # style profile: medians over all scenery objects
    def _med(vals):
        vals = sorted(vals); return round(vals[len(vals) // 2], 1) if vals else 0.0
    build = [o for o in objs if o["class"] == "building"]
    style = {
        "building_count": len(build),
        "med_height": _med([o["height"] for o in build]),
        "med_footprint": _med([max(o["footprint"]) for o in build]),
        "med_setback": _med([o["setback"] for o in build]),
        "common_pages": g["summary"]["pages_used"][:8],
    }

    # empty stretches: consecutive buckets with no objects on a side
    gaps = []
    for side in ("left", "right"):
        run_start = None
        for i, b in enumerate(prof):
            empty = (b["left_objects"] if side == "left" else b["right_objects"]) == 0
            if empty and run_start is None:
                run_start = b["span_start"]
            if (not empty or i == len(prof) - 1) and run_start is not None:
                end = prof[i - 1]["span_end"] if not empty else b["span_end"]
                length = (end - run_start + 1)
                if length >= min_gap_spans:
                    gaps.append({"side": side, "span_start": run_start,
                                 "span_end": end, "span_length": length})
                run_start = None
    gaps.sort(key=lambda x: -x["span_length"])
    return {"ok": True, "level": level, "gaps": gaps, "style": style,
            "summary": g["summary"]}


# ---------------------------------------------------------------------------
# Coverage-space VOID detection (measures actual floor coverage, not roadside
# object density) — the honest answer to "where are the void places".
# ---------------------------------------------------------------------------
def _floor_face_grid(model, cell=8000.0, max_y=60000.0):
    """Index near-horizontal GROUND faces (|normal.y|>0.5 to allow relief slopes;
    below max_y to skip only tall vertical structure) as XZ polygons into a grid
    for O(1) point-in-floor tests. max_y is generous so a hilly ground apron
    (terrain relief raises tiles well above the road) still counts as covered."""
    grid = {}
    for m in model["meshes"]:
        ox, oy, oz = m["origin"]; inv = 1.0 / 256.0
        vs = [(ox*inv+v["pos"][0], oy*inv+v["pos"][1], oz*inv+v["pos"][2])
              for v in m["vertices"]]
        cur = 0
        for c in m["commands"]:
            faces = []
            for _ in range(c["tri"]):
                if cur + 3 <= len(vs): faces.append((vs[cur], vs[cur+1], vs[cur+2]))
                cur += 3
            for _ in range(c["quad"]):
                if cur + 4 <= len(vs):
                    a, b, cc, d = vs[cur:cur+4]
                    faces.append((a, b, cc)); faces.append((a, cc, d))
                cur += 4
            for a, b, cc in faces:
                ux, uy, uz = b[0]-a[0], b[1]-a[1], b[2]-a[2]
                vx, vy, vz = cc[0]-a[0], cc[1]-a[1], cc[2]-a[2]
                ny = uz*vx - ux*vz
                mag = math.sqrt((uy*vz-uz*vy)**2 + ny*ny + (ux*vy-uy*vx)**2) or 1.0
                if abs(ny/mag) <= 0.5 or (a[1]+b[1]+cc[1]) / 3.0 >= max_y:
                    continue
                poly = [(a[0], a[2]), (b[0], b[2]), (cc[0], cc[2])]
                xs = [p[0] for p in poly]; zs = [p[1] for p in poly]
                for gx in range(int(min(xs)//cell), int(max(xs)//cell)+1):
                    for gz in range(int(min(zs)//cell), int(max(zs)//cell)+1):
                        grid.setdefault((gx, gz), []).append(poly)
    return grid, cell


def _covered(grid, cell, x, z):
    for poly in grid.get((int(x//cell), int(z//cell)), ()):
        n = len(poly); j = n-1; c = False
        for i in range(n):
            xi, zi = poly[i]; xj, zj = poly[j]
            if ((zi > z) != (zj > z)) and (x < (xj-xi)*(z-zi)/((zj-zi) or 1e-9)+xi):
                c = not c
            j = i
        if c:
            return True
    return False


def capture_terrain(assets_root=None, level=None, reach=120000, dstep=8000):
    """Measure a REAL track's terrain profile so the generator can MIMIC it:
    how the ground rises/falls with distance from the road, how rough it is, and
    how much the road itself climbs. Returns fitted knobs directly usable by
    td5_trackgen.build_ground_grid (flat_radius, slope, max_rise, hill_amp) plus
    diagnostics (rise_by_dist, road_elev_range, undulation, dips)."""
    assets_root = assets_root or _default_assets_root()
    level = int(level)
    level_dir = os.path.join(_levels_dir(assets_root), "level%03d" % level)
    mt = _mod("mesh_tool")
    model = mt.decode(open(os.path.join(level_dir, "models.bin"), "rb").read(), "models")
    cl = _load_centerline(level_dir)
    if not cl:
        return {"ok": False, "error": "no centerline"}
    centers = [c for c in cl["centers"] if c]
    road_ys = [c[1] for c in centers]
    scell = float(dstep)
    sg = {}
    for c in centers:
        sg.setdefault((int(c[0]//scell), int(c[2]//scell)), []).append(c)

    def near_road(x, z):
        best, bd = None, 1e30
        gx, gz = int(x//scell), int(z//scell)
        rr = int(reach//scell) + 1
        for dx in range(-rr, rr+1):
            for dz in range(-rr, rr+1):
                for c in sg.get((gx+dx, gz+dz), ()):
                    d = (c[0]-x)**2 + (c[2]-z)**2
                    if d < bd:
                        bd, best = d, c
        return best, math.sqrt(bd)

    # per-distance-bin relief (face Y relative to nearest road Y)
    bins = {}
    for m in model["meshes"]:
        if m["texture_page_id"] in (1, 2):
            continue
        ox, oy, oz = m["origin"]; inv = 1.0/256.0
        vs = [(ox*inv+v["pos"][0], oy*inv+v["pos"][1], oz*inv+v["pos"][2]) for v in m["vertices"]]
        cur = 0
        for c in m["commands"]:
            faces = []
            for _ in range(c["tri"]):
                if cur+3 <= len(vs): faces.append((vs[cur], vs[cur+1], vs[cur+2]))
                cur += 3
            for _ in range(c["quad"]):
                if cur+4 <= len(vs):
                    a, b, cc, d = vs[cur:cur+4]; faces.append((a, b, cc)); faces.append((a, cc, d))
                cur += 4
            for a, b, cc in faces:
                ux, uy, uz = b[0]-a[0], b[1]-a[1], b[2]-a[2]
                vx, vy, vz = cc[0]-a[0], cc[1]-a[1], cc[2]-a[2]
                ny = uz*vx - ux*vz
                mag = math.sqrt((uy*vz-uz*vy)**2 + ny*ny + (ux*vy-uy*vx)**2) or 1.0
                if abs(ny/mag) <= 0.5:
                    continue                                # not ground-ish (a wall)
                cx = (a[0]+b[0]+cc[0])/3.0; cy = (a[1]+b[1]+cc[1])/3.0; cz = (a[2]+b[2]+cc[2])/3.0
                rc, d = near_road(cx, cz)
                if rc is None or d > reach:
                    continue
                if cy - rc[1] > 60000:                       # skip roof-height slabs
                    continue
                bins.setdefault(int(d // dstep), []).append(cy - rc[1])

    rise_by_dist = []
    for k in sorted(bins):
        vals = bins[k]
        mean = sum(vals) / len(vals)
        std = (sum((x-mean)**2 for x in vals) / len(vals)) ** 0.5
        rise_by_dist.append({"dist": k*dstep, "mean": round(mean, 0), "std": round(std, 0),
                             "n": len(vals)})
    # fit knobs
    flat_radius = 0.0
    for b in rise_by_dist:
        if abs(b["mean"]) < 1500:
            flat_radius = b["dist"] + dstep
        else:
            break
    far = [b for b in rise_by_dist if b["dist"] >= flat_radius and b["n"] >= 20]
    if len(far) >= 2:
        slope = (far[-1]["mean"] - far[0]["mean"]) / max(float(dstep),
                                                         far[-1]["dist"] - far[0]["dist"])
        slope = round(max(-0.6, min(0.6, slope)), 3)                  # clamp sane
        stds = sorted(b["std"] for b in far)
        hill_amp = round(2.0 * stds[len(stds)//2], 0)                 # roughness (2×median std)
        max_rise = round(max(abs(b["mean"]) for b in far) + hill_amp, 0)
    elif far:
        slope = round(max(-0.6, min(0.6, far[0]["mean"] / max(float(dstep), far[0]["dist"]))), 3)
        hill_amp = round(2.0 * far[0]["std"], 0)
        max_rise = round(abs(far[0]["mean"]) + hill_amp, 0)
    else:
        slope, max_rise, hill_amp = 0.05, 8000.0, 6000.0
    dips = any(b["mean"] < -1500 for b in rise_by_dist)
    # ROAD elevation profile (the road's own Y along its length): max grade +
    # how many climbs (Y local maxima) around the track.
    road_max_grade = 0.0
    road_waves = 1
    nC = len(centers)
    if nC >= 4:
        for i in range(nC):
            j = (i + 1) % nC
            da = math.hypot(centers[j][0]-centers[i][0], centers[j][2]-centers[i][2])
            if da > 1e-6:
                road_max_grade = max(road_max_grade, abs(centers[j][1]-centers[i][1]) / da)
        # LOW-PASS the road Y (circular moving average) before counting crests, so
        # we get the DOMINANT hill count (major undulation) instead of every local
        # bump — a noisy count collapses the synth wavelength and the grade cap
        # then flattens the hills. Window ~1/10 of the loop resolves up to ~5 hills.
        ys = [c[1] for c in centers]
        win = max(1, nC // 10)
        sm = []
        for i in range(nC):
            acc = 0.0
            for k in range(-win, win + 1):
                acc += ys[(i + k) % nC]
            sm.append(acc / (2*win + 1))
        crests = 0
        for i in range(nC):
            a, b, c = sm[(i-1) % nC], sm[i], sm[(i+1) % nC]
            if b > a and b >= c:                       # local max on the SMOOTHED curve
                crests += 1
        road_waves = max(1, crests)
    return {"ok": True, "level": level,
            "flat_radius": flat_radius, "slope": slope, "max_rise": max_rise,
            "hill_amp": hill_amp,
            "road_elev_range": round(max(road_ys)-min(road_ys), 0),
            "road_max_grade": round(road_max_grade, 3),
            "road_waves": max(1, road_waves),
            "has_dips": dips,
            "rise_by_dist": rise_by_dist[:16]}


def detect_ground_voids(assets_root=None, level=None, far_cull=195000,
                        step_spans=2):
    """Measure ACTUAL floor coverage around the track (not roadside object
    density). For a set of distance bands out to far_cull, probe both sides of
    each span for whether any near-horizontal floor face covers that point, split
    inward (toward the loop interior) vs outward. Returns coverage% per band +
    the distance where coverage first falls below 95% (= where void begins), so a
    filler knows exactly how far / which direction to extend ground. This is what
    analyze_gaps (span-space object density) is blind to."""
    assets_root = assets_root or _default_assets_root()
    level = int(level)
    level_dir = os.path.join(_levels_dir(assets_root), "level%03d" % level)
    mt = _mod("mesh_tool")
    mpath = os.path.join(level_dir, "models.bin")
    if not os.path.isfile(mpath):
        return {"ok": False, "error": "no models.bin"}
    model = mt.decode(open(mpath, "rb").read(), "models")
    cl = _load_centerline(level_dir)
    if not cl:
        return {"ok": False, "error": "no centerline"}
    grid, cell = _floor_face_grid(model)
    centers = [c for c in cl["centers"] if c]
    gx = sum(c[0] for c in centers) / len(centers)
    gz = sum(c[2] for c in centers) / len(centers)      # loop centroid (inward ref)

    bands = [2000, 10000, 20000, 35000, 50000, 65000, 80000, 100000, 130000, far_cull]
    agg = {d: {"in": [0, 0], "out": [0, 0]} for d in bands}
    ring = cl["ring"]
    for i in range(0, ring, step_spans):
        c = cl["centers"][i]
        if not c:
            continue
        j = (i + 1) % len(cl["centers"])
        cn = cl["centers"][j] or c
        dx, dz = cn[0]-c[0], cn[2]-c[2]
        dl = math.hypot(dx, dz) or 1.0
        px, pz = -dz/dl, dx/dl                          # across-road (perp)
        for d in bands:
            for sgn in (+1, -1):
                x, z = c[0]+px*d*sgn, c[2]+pz*d*sgn
                inward = (px*sgn)*(gx-c[0]) + (pz*sgn)*(gz-c[2]) > 0
                k = "in" if inward else "out"
                agg[d][k][1] += 1
                if _covered(grid, cell, x, z):
                    agg[d][k][0] += 1
    out_bands = []
    first_void = None
    for d in bands:
        a = agg[d]
        ip = 100.0*a["in"][0]/max(1, a["in"][1])
        op = 100.0*a["out"][0]/max(1, a["out"][1])
        tp = 100.0*(a["in"][0]+a["out"][0])/max(1, a["in"][1]+a["out"][1])
        out_bands.append({"dist": d, "inward_pct": round(ip, 1),
                          "outward_pct": round(op, 1), "total_pct": round(tp, 1)})
        if first_void is None and tp < 95.0:
            first_void = d
    return {"ok": True, "level": level, "far_cull": far_cull,
            "coverage_by_band": out_bands,
            "void_begins_at": first_void,
            "note": "coverage<100 beyond void_begins_at = void floor; extend ground "
                    "(esp. outward) to ~far_cull or add an occluder ring/backdrop"}


def _cli():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    cmd, lvl = sys.argv[1], int(sys.argv[2])
    if cmd == "geometry":
        print(json.dumps(trace_geometry(None, lvl)["summary"], indent=2))
    elif cmd == "textures":
        r = trace_textures(None, lvl)
        print("montage:", r["montage"], "pages:", r["page_count"])
    elif cmd == "gaps":
        print(json.dumps(analyze_gaps(None, lvl), indent=2)[:2000])
    else:
        print(__doc__); return 2
    return 0


if __name__ == "__main__":
    sys.exit(_cli())
