#!/usr/bin/env python3
"""
td5_geomlib.py -- build the shipped-geometry LIBRARY: every separable piece of
geometry and every texture page across all shipped/converted tracks, catalogued
once to disk so the auto-track generator and the Track Studio can both consume
one source of truth.

Why this exists: the generator authors everything procedurally, while 38 shipped
tracks carry ~94k meshes and ~7.5k texture pages it cannot reach. Existing tools
each solve one slice -- mesh_tool decodes the container, td5_assetlib lifts a
whole building near a point, td5_maptrace classifies a page's surface ROLE from
how geometry uses it -- but nothing catalogues the corpus or persists the result.

    python re/tools/td5_geomlib.py pages   [--levels 23,14] [--out DIR]
    python re/tools/td5_geomlib.py objects [--levels 23,14] [--out DIR]
    python re/tools/td5_geomlib.py build   [--out DIR]        # pages + objects
    python re/tools/td5_geomlib.py show    --level 23 --entry 29 --slot 0
    python re/tools/td5_geomlib.py verify  [--levels 23]

GRANULARITY -- the one design decision worth knowing:

  A sub-mesh is the finest thing the in-game picker can name (a pick line says
  `e<entry> s<slot>`), and shipped sub-meshes are routinely fused. Splitting on
  the exact per-command page seam un-fuses them, but it also shatters shipped
  facades, which are flat arrays of unconnected quads: level023 yields 32,444
  page-parts from 1,610 meshes. Spatial clustering across pages instead yields
  2,951 -- browsable, but it welds the guardrails back into a 109-face blob.

  So the catalogue stores OBJECTS (spatial clusters, `split_mesh(by_page=False)`)
  and records each object's per-page face/area breakdown. The page split is a
  VIEW, recomputed on demand from the source mesh by `show` / the studio / the
  extractor -- not 1.2M files on disk.

Corpus facts measured by `verify` (2026-09-09, all 38 levels):
  * 94,493 meshes, 406,979 commands, ZERO cursor-validation rejects -- the
    sequential vertex cursor is universally valid in shipped data.
  * every command has dispatch_type 0, so the opcode 2..6 stride worry that
    makes td5_pick.c bail does not arise for any shipped mesh.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import time
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mesh_tool as mt              # noqa: E402
import td5_assetlib as al           # noqa: E402
import td5_maptrace as mtr          # noqa: E402

SCHEMA = 1

# Night tracks per the generator's own sky pools (td5_trackgen.c:2787-2793),
# themselves derived from mean FORWSKY.png luminance. Kept here as the EXPECTED
# set so `pages` cross-checks its own measurement against the shipping decision
# instead of silently re-deriving a second, divergent answer.
EXPECTED_NIGHT = {5, 18, 23, 30, 37}


def _levels_dir():
    return os.path.join(os.path.dirname(os.path.dirname(HERE)), "re", "assets", "levels")


def _all_levels(root):
    out = []
    for p in sorted(glob.glob(os.path.join(root, "level*"))):
        n = os.path.basename(p)[5:]
        if n.isdigit() and os.path.isfile(os.path.join(p, "models.bin")):
            out.append(int(n))
    return out


def _load_model(root, level):
    with open(os.path.join(root, "level%03d" % level, "models.bin"), "rb") as f:
        return mt.decode(f.read(), "models")


# ---------------------------------------------------------------------------
# day / night
# ---------------------------------------------------------------------------

def sky_luma(root, level):
    """Mean luminance of the level's FORWSKY.png, the only thing in shipped data
    that encodes time of day -- LEVELINF.DAT has no such field (verified across
    all 38 levels: track_type, smoke_enable, checkpoint_*, weather_type,
    traffic_enable, density_pairs, sky_animation_index, total_span_count,
    fog_enabled, fog_color_rgb). Returns None if the sky is missing or PIL is not
    available."""
    p = os.path.join(root, "level%03d" % level, "forwsky.png")
    if not os.path.isfile(p):
        return None
    try:
        from PIL import Image
    except ImportError:
        return None
    im = Image.open(p).convert("RGB")
    px = list(im.getdata())
    if not px:
        return None
    return sum(0.299 * r + 0.587 * g + 0.114 * b for r, g, b in px) / len(px)


# ---------------------------------------------------------------------------
# object classification
# ---------------------------------------------------------------------------

FLAT_Y = 60.0          # world-Y extent under which a part is a flat slab
POST_XZ = 600.0        # XZ extent under which a tall part is a post
RIBBON_RATIO = 3.0     # long-axis : short-axis ratio that makes a slab a ribbon


def classify(part, roles):
    """Name an object from its shape plus the ROLES of the pages it uses.
    Deliberately coarse and deliberately fallible -- `unknown` is a valid answer,
    and every verdict is overridable from the studio inspector, because tuning
    thresholds forever is a worse deal than fixing a handful by hand."""
    dx, dy, dz = part["extent"]
    flat = max(dx, dz)
    thin = min(dx, dz)
    pr = Counter(roles.get(p, "unknown") for p in part["pages"])
    top = pr.most_common(1)[0][0] if pr else "unknown"

    if top == "foliage":
        return "tree"
    if dy <= FLAT_Y and flat > 2000.0:
        if top == "road":
            return "road"
        return "plaza" if thin > 1500.0 else "verge"
    if dy > 1500.0 and max(dx, dz) < POST_XZ:
        return "post"
    if dy <= 800.0 and flat > 3000.0 and flat > thin * RIBBON_RATIO:
        return "rail"
    if top == "sign":
        return "sign"
    if dy >= 6000.0 and len(part["pages"]) >= 3:
        return "building"
    if top in ("wall", "roof"):
        return "facade"
    return "unknown"


# A landmark is an OUTLIER for its own level, not anything over a fixed height.
# Measured on level023: an absolute `dy >= 6000` gate called 853 objects
# landmarks, median 25 faces over 8 pages -- i.e. every ordinary Moscow building.
# Rank within the level instead, so a flat town and a tower city each surface
# their own standouts.
LANDMARK_TOP_PCT = 0.02
LANDMARK_MIN_FACES = 40


def promote_landmarks(rows):
    """Promote the largest `building` rows of one level to `landmark`, in place.
    Returns the number promoted."""
    cand = [r for r in rows if r["kind"] == "building" and r["faces"] >= LANDMARK_MIN_FACES]
    if not cand:
        return 0
    cand.sort(key=lambda r: -(r["extent"][0] * r["extent"][1] * r["extent"][2]))
    n = max(1, int(len(rows) * LANDMARK_TOP_PCT))
    for r in cand[:n]:
        r["kind"] = "landmark"
    return min(n, len(cand))


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_pages(root, levels, out):
    """Catalogue every texture page with the surface ROLE td5_maptrace infers
    from geometry usage (face orientation, height, proximity to the road
    centreline) -- not from pixels."""
    os.makedirs(out, exist_ok=True)
    pages, per_level, roles_all = [], {}, Counter()
    for lv in levels:
        t = time.time()
        try:
            r = mtr.trace_textures(level=lv, out_png=os.path.join(out, "montage_l%03d.png" % lv))
        except Exception as e:                        # keep going, record the gap
            print("  level%03d  SKIP (%s)" % (lv, e))
            per_level[lv] = {"error": str(e)}
            continue
        luma = sky_luma(root, lv)
        night = (luma is not None and luma < 80.0)
        for p in r["pages"]:
            pages.append({"level": lv, "page": p["id"], "type": p["type"],
                          "role": p["role"], "usage": p["usage"], "rgb": p["rgb"]})
        roles_all.update(r["role_counts"])
        per_level[lv] = {"page_count": r["page_count"], "roles": r["role_counts"],
                         "sky_luma": round(luma, 1) if luma is not None else None,
                         "night": night}
        print("  level%03d  %4d pages  road=%-4d luma=%-6s night=%-5s  %.1fs"
              % (lv, r["page_count"], r["role_counts"].get("road", 0),
                 round(luma, 1) if luma is not None else "n/a", night, time.time() - t))

    measured = {lv for lv, d in per_level.items() if d.get("night")}
    doc = {"schema": SCHEMA, "kind": "pages", "levels": per_level,
           "role_totals": dict(roles_all), "page_total": len(pages),
           "night_measured": sorted(measured),
           "night_expected": sorted(EXPECTED_NIGHT),
           "night_disagreement": sorted(measured ^ (EXPECTED_NIGHT & set(per_level)))}
    with open(os.path.join(out, "pages.json"), "w", encoding="utf-8") as f:
        json.dump({"index": doc, "pages": pages}, f, indent=1)
    print("\n  %d pages, roles=%s" % (len(pages), dict(roles_all)))
    if doc["night_disagreement"]:
        print("  NIGHT DISAGREEMENT vs the shipped sky pools: %s"
              % doc["night_disagreement"])
    else:
        print("  night set agrees with the generator's sky pools: %s"
              % doc["night_measured"])
    return doc


def cmd_objects(root, levels, out, roles=None):
    """Split every sub-mesh into spatial OBJECTS and record each one's shape,
    pages and per-page breakdown. One JSONL per level (objects/levelNNN.jsonl)."""
    os.makedirs(os.path.join(out, "objects"), exist_ok=True)
    roles = roles or {}
    summary, kinds_all = {}, Counter()
    for lv in levels:
        t = time.time()
        model = _load_model(root, lv)
        lvroles = roles.get(lv, {})
        rej = Counter()
        rows = []
        for e, ids in enumerate(model["entries"]):
            for s, mid in enumerate(ids):
                mesh = model["meshes"][mid]
                r = al.split_mesh(mesh, mode="object")
                if r["reason"] != al.SPLIT_OK:
                    rej[r["reason"]] += 1
                    continue
                for oi, part in enumerate(r["parts"]):
                    per_page = defaultdict(int)
                    for c in part["mesh"]["commands"]:
                        per_page[int(c["texture_page_id"])] += \
                            int(c["tri"]) + int(c["quad"])
                    rows.append({
                        "id": "L%d.e%d.s%d.o%d" % (lv, e, s, oi),
                        "level": lv, "entry": e, "slot": s, "obj": oi,
                        "kind": classify(part, lvroles), "pages": part["pages"],
                        "faces": part["nface"], "per_page": dict(per_page),
                        "aabb": [round(v, 1) for v in part["aabb"]],
                        "extent": [round(v, 1) for v in part["extent"]],
                    })
        # second pass: landmarks are ranked within the level, so it needs them all
        nlm = promote_landmarks(rows)
        path = os.path.join(out, "objects", "level%03d.jsonl" % lv)
        with open(path, "w", encoding="utf-8") as f:
            for r in rows:
                kinds_all[r["kind"]] += 1
                f.write(json.dumps(r) + "\n")
        summary[lv] = {"meshes": len(model["meshes"]), "objects": len(rows),
                       "landmarks": nlm, "rejects": dict(rej)}
        print("  level%03d  %5d meshes -> %6d objects  landmarks=%-4d rejects=%s  %.1fs"
              % (lv, len(model["meshes"]), len(rows), nlm, dict(rej) or "none",
                 time.time() - t))
    print("\n  kinds: %s" % dict(kinds_all.most_common()))
    return {"schema": SCHEMA, "kind": "objects", "levels": summary,
            "kind_totals": dict(kinds_all)}


def cmd_show(root, level, entry, slot):
    """Inspect one picked slot at BOTH granularities -- the object view the
    catalogue stores, and the exact per-page split used to un-fuse it."""
    model = _load_model(root, level)
    mesh = model["meshes"][model["entries"][entry][slot]]
    print("level%03d e%d s%d: %d commands, %d vertices, pages=%s"
          % (level, entry, slot, len(mesh["commands"]), len(mesh["vertices"]),
             sorted({int(c["texture_page_id"]) for c in mesh["commands"]})))
    for label, mode in (("OBJECTS (catalogue view)", "object"),
                        ("PAGE SPLIT (un-fuse view)", "page"),
                        ("WELD (shows why slabs must not bridge)", "weld")):
        r = al.split_mesh(mesh, mode=mode)
        print("\n  %s -- reason=%s, %d parts" % (label, r["reason"], len(r["parts"])))
        for p in r["parts"][:12]:
            print("    faces=%-5d extent=%-26s pages=%s"
                  % (p["nface"], [round(v) for v in p["extent"]], p["pages"][:8]))


def cmd_verify(root, levels):
    """Prove the two corpus invariants the splitter depends on, and round-trip
    every object back through mesh_tool.build_dat."""
    reasons, disp, meshes = Counter(), Counter(), 0
    for lv in levels:
        model = _load_model(root, lv)
        for mesh in model["meshes"]:
            meshes += 1
            for c in mesh["commands"]:
                disp[int(c["dispatch_type"])] += 1
            _f, r = al.mesh_faces(mesh)
            reasons[r] += 1
    print("meshes=%d  cursor=%s  dispatch_types=%s"
          % (meshes, dict(reasons), dict(disp)))

    lv = levels[0]
    model = _load_model(root, lv)
    parts = []
    for ids in model["entries"][:40]:
        for mid in ids:
            parts += [p["mesh"] for p in al.split_mesh(model["meshes"][mid])["parts"]]
    # A block holds at most 256 sub-meshes (the parser's validity definition,
    # td5_track_parser.c:210), so chunk rather than dumping every part into one.
    chunks = [list(range(i, min(i + 256, len(parts)))) for i in range(0, len(parts), 256)]
    doc = {"kind": "models", "entry_count": len(chunks), "entries": chunks,
           "meshes": parts}
    blob = mt.build_dat(doc)
    back = mt.decode(blob, "models")
    ok = len(back["meshes"]) == len(parts)
    vin = sum(len(m["vertices"]) for m in parts)
    vout = sum(len(m["vertices"]) for m in back["meshes"])
    print("round-trip level%03d: %d parts -> %d bytes -> %d meshes, verts %d/%d  %s"
          % (lv, len(parts), len(blob), len(back["meshes"]), vin, vout,
             "OK" if (ok and vin == vout) else "MISMATCH"))
    return 0 if (reasons.get(al.SPLIT_OK, 0) == meshes and ok and vin == vout) else 1


# ---------------------------------------------------------------------------
# road-surface curation (feeds gen_tg_pages.py -> td5_tg_real_tex_roads.h)
# ---------------------------------------------------------------------------
#
# tg_road_page (td5_tg_terrain.c:1032) can only ever return 5 pages, all
# procedural noise. The library finds 687 road-role pages across the shipped
# corpus. They cannot be bulk-imported: the comment at td5_tg_pages.c:3029
# records why ROAD stayed procedural, namely that shipped city road pages are
# sandstone tan and read muddy under the auto-track. So this curates.
#
# Three filters, each for a stated reason:
#   type != 0        -- alpha-keyed pages are markings/decals, not a surface
#   0 carriageway    -- 185 of 687 are road-role by orientation and proximity
#     faces             but never actually carry a driving surface
#   luma <= 1        -- degenerate all-black pages
#
# Ranking inside a class is by CARRIAGEWAY face count from the object
# catalogue, i.e. how much road the shipped game actually paved with it. That
# is a far better signal than any pixel statistic for "is this a good road".
ROAD_CLASSES = ("TARMAC", "CONCRETE", "COBBLE", "DIRT", "ICE")

# Levels 040-046 are PROP LIBRARIES, not drivable tracks: the object sweep gives
# them ~1 object per mesh and 0 landmarks, and 042/043 have objects == meshes
# exactly. Their pages reach the road-role classifier through geometry that is
# not really carriageway, which is how a piece of machinery (level040 page 001)
# ended up ranked as a road surface. Excluded by provenance rather than by
# pixels, because that is what is actually wrong with them.
NON_TRACK_LEVELS = set(range(40, 47))

# Thresholds fitted to hand-labelled examples rather than guessed. Measured:
#   good tarmac   sd  9.2..24.1   stripe 0.83..0.94
#   cobble/setts  sd 12.3..27.7   stripe 0.21..0.68
#   snow          lum 223..235
#   arrow marking sd 57.9         <- the white directional arrow, level008 p151
#   junction tile sd 50.2         <- level004 p186
#   pink fan slab lum 196         <- level011 p073, decorative plaza
# No page that survives inspection exceeds sd 28, and nothing good sits between
# stripe 0.68 and 0.83, so both cuts fall in real gaps.
RC_PAINT_SD = 40.0      # above this the page is painted geometry, not a surface
RC_STRIPE = 0.75        # above = longitudinal (road), below = cell/tile pattern
# 200, not 210: the best snow page (level010 p108, luma 204, 1275 carriageway
# faces) sits just under 210, and the decorative pink fan slab that must NOT be
# ice sits at 196. Eight luma apart, so luma alone cannot arbitrate -- what
# actually excludes the fan is `stripe` 0.46, which routes it to COBBLE where a
# fan-pattern plaza belongs. The luma cut only has to sit between the two.
RC_ICE_LUM = 200.0
RC_SAT_MAX = 0.30       # a road surface is not strongly coloured

# A class ships as many good pages as it HAS, never a fixed quota. Filling every
# class to 8 dragged in pages carrying 4, 4 and 1 carriageway faces -- art the
# shipped game barely used, promoted purely to meet a count. TD5 genuinely has
# few light-concrete or snow road surfaces, and the C side already falls back to
# the procedural generator for any slot the header does not fill, so a short
# class costs nothing.
RC_MIN_FACES = 100


def road_class(f):
    """Bucket a road page from its measured features (see _page_stats).

    The failure this replaces: ranking by carriageway-face count cannot tell a
    SURFACE from something PAINTED ON a surface, because an arrow marking
    legitimately covers carriageway faces. Colour and detail alone then sorted
    the survivors by brightness, which put the cobbles in a "pale" bucket, a
    road arrow and a junction tile in "rough", and a decorative plaza in "ice".

    `stripe` is what actually separates a road from paving: a road page's
    markings run ALONG the road, so one axis explains almost all the variance
    (0.83..0.94), while setts and brick vary in both axes (0.21..0.68). Note
    this deliberately KEEPS pages with lane paint -- that is how TD5 draws roads
    -- and rejects only paint that does not run with the carriageway.
    """
    lum, sat, sd, r, _g, b, stripe, _seam, _spec = f
    if lum <= 1.0 or sat > RC_SAT_MAX:
        return None
    if sd >= RC_PAINT_SD:
        return None                       # arrows, chevrons, junction tiles
    if lum >= RC_ICE_LUM and sat < 0.14:
        return "ICE"
    if r - b > 14.0 and sat >= 0.12:
        return "DIRT"
    if stripe < RC_STRIPE:
        return "COBBLE"                   # setts, brick, slabs -- cell patterns
    if lum >= 140.0:
        return "CONCRETE"
    if lum >= 25.0:
        return "TARMAC"
    return None


def _page_stats(path):
    """(lum, sat, sd, r, g, b, stripe, seam, spec) for one 64x64 page.

    stripe -- how much of the variance ONE axis explains. Longitudinal road
              paint makes every row alike, so a road page scores high; setts and
              brick vary both ways and score low.
    seam   -- wrap discontinuity relative to internal detail. Kept for the
              report; it turned out not to be needed as a cut, because the sd
              gate already removes everything it would have caught.
    spec   -- strongest low-frequency FFT peak, i.e. periodic cell structure.
    """
    import numpy as np
    from PIL import Image
    im = Image.open(path).convert("RGB")
    arr = np.asarray(im, dtype=float)
    r, g, b = arr[:, :, 0].mean(), arr[:, :, 1].mean(), arr[:, :, 2].mean()
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    mx, mn = max(r, g, b), min(r, g, b)
    sat = (mx - mn) / mx if mx else 0.0

    a = 0.299 * arr[:, :, 0] + 0.587 * arr[:, :, 1] + 0.114 * arr[:, :, 2]
    sd = float(a.std())
    tot = sd or 1e-6
    stripe = float(max(a.mean(axis=1).std(), a.mean(axis=0).std()) / tot)
    inner = (float(np.abs(np.diff(a, axis=1)).mean())
             + float(np.abs(np.diff(a, axis=0)).mean())) / 2 or 1e-6
    seam = float(max(np.abs(a[:, 0] - a[:, -1]).mean(),
                     np.abs(a[0, :] - a[-1, :]).mean()) / inner)
    F = np.abs(np.fft.fft2(a - a.mean()))
    F[0, 0] = 0.0
    spec = float(F[1:12, 1:12].max() / (F.sum() / F.size + 1e-6))
    return lum, sat, sd, r, g, b, stripe, seam, spec


def cmd_roads(root, out, per_class=8):
    """Curate the road-role pages into per-surface-class sets and write a
    gen_tg_pages.py manifest."""
    import hashlib
    with open(os.path.join(out, "pages.json"), encoding="utf-8") as f:
        pages = json.load(f)["pages"]

    carriage = Counter()
    for f in glob.glob(os.path.join(out, "objects", "level*.jsonl")):
        for line in open(f, encoding="utf-8"):
            o = json.loads(line)
            if o["kind"] != "road":
                continue
            for pg, n in o["per_page"].items():
                carriage[(o["level"], int(pg))] += n

    buckets, seen, drop = defaultdict(list), {}, Counter()
    for p in pages:
        if p["role"] != "road":
            continue
        if p["level"] in NON_TRACK_LEVELS:
            drop["non_track_level"] += 1
            continue
        if p["type"] != 0:
            drop["alpha_keyed"] += 1
            continue
        rf = carriage[(p["level"], p["page"])]
        if rf <= 0:
            drop["never_a_carriageway"] += 1
            continue
        if rf < RC_MIN_FACES:
            drop["barely_used"] += 1
            continue
        png = os.path.join(root, "level%03d" % p["level"], "textures.src",
                           "pages", "page_%03d.png" % p["page"])
        if not os.path.isfile(png):
            drop["no_png"] += 1
            continue
        f = _page_stats(png)
        h = hashlib.sha256(open(png, "rb").read()).hexdigest()
        if h in seen:
            drop["duplicate_art"] += 1
            continue
        seen[h] = True
        k = road_class(f)
        if not k:
            drop["rejected_or_unclassified"] += 1
            continue
        buckets[k].append({"level": p["level"], "page": p["page"], "faces": rf,
                           "luma": round(f[0], 1), "sat": round(f[1], 2),
                           "detail": round(f[2], 1), "stripe": round(f[6], 2)})

    sets = {}
    for k in ROAD_CLASSES:
        v = sorted(buckets[k], key=lambda d: -d["faces"])[:per_class]
        sets[k.lower()] = [
            ["level%03d" % d["level"], d["page"],
             "%s, luma %.0f sat %.2f detail %.0f, %d carriageway faces"
             % (k.lower(), d["luma"], d["sat"], d["detail"], d["faces"])]
            for d in v]
        print("  %-7s %3d candidates -> keep %d  (top faces %s)"
              % (k, len(buckets[k]), len(v),
                 ", ".join(str(d["faces"]) for d in v[:4])))
    print("  dropped: %s" % dict(drop))

    # This header is wholly generated from this manifest and has no hand edits,
    # so re-running is safe. gen_tg_pages.py otherwise refuses to overwrite, to
    # protect the hand-curated headers from earlier rounds.
    man = {"header": "td5_tg_real_tex_roads.h", "prefix": "k_road",
           "regenerate": True,
           "title": "Real shipped ROAD surfaces, curated by re/tools/td5_geomlib.py "
                    "roads. Ranked by how much carriageway each page actually "
                    "paves in the shipped game, then classed by measured "
                    "features: painted geometry (arrows, junction tiles) is "
                    "rejected on contrast, and COBBLE is split from TARMAC by "
                    "whether the pattern runs ALONG the road or cells both ways.",
           "sets": sets}
    # The manifest is the header's SOURCE, so it must be tracked -- re/assets is
    # gitignored, and a generated header whose input is not in the repo cannot be
    # regenerated by anyone else. Earlier real_tex headers had their page lists
    # baked into one-off gen_trackgen_*_tex.py scripts; this keeps the data out
    # of code instead.
    mdir = os.path.join(HERE, "manifests")
    os.makedirs(mdir, exist_ok=True)
    mp = os.path.join(mdir, "roads.json")
    with open(mp, "w", newline="\n", encoding="utf-8") as f:
        json.dump(man, f, indent=2)
        f.write("\n")
    print("\nwrote %s -- %d pages in %d sets"
          % (mp, sum(len(v) for v in sets.values()), len(sets)))
    return man


# ---------------------------------------------------------------------------
# prefab export (feeds td5_tg_prefab.c + gen_tg_pages.py)
# ---------------------------------------------------------------------------
#
# A prefab is one catalogued OBJECT lifted out of a shipped track and turned
# into something the generator can stamp: local vertices centred on the
# footprint with base y=0, commands carrying a LOCAL page index, and a footprint
# plus height so placement can reserve clearance.
#
# Two outputs, because geometry and art travel differently:
#   re/tools/manifests/landmarks.json -> gen_tg_pages.py -> the page header
#   td5_tg_prefab_data.h              -> the geometry, pages by local index
#
# The convention matches td5_assetlib.extract_prototype (centre XZ, base y=0) so
# a prefab placed by the C emitter and one placed by instance_prototype in the
# Python editor land the same way.


def _prefab_geometry(model, row):
    """Re-derive one catalogued object's geometry and localise it."""
    mesh = model["meshes"][model["entries"][row["entry"]][row["slot"]]]
    parts = al.split_mesh(mesh, mode="object")["parts"]
    if row["obj"] >= len(parts):
        raise ValueError("%s: object %d no longer exists (library stale?)"
                         % (row["id"], row["obj"]))
    part = parts[row["obj"]]
    vs = part["mesh"]["vertices"]
    xs = [v["pos"][0] for v in vs]
    zs = [v["pos"][2] for v in vs]
    ys = [v["pos"][1] for v in vs]
    cx = (min(xs) + max(xs)) / 2.0
    cz = (min(zs) + max(zs)) / 2.0
    base = min(ys)
    local = [(v["pos"][0] - cx, v["pos"][1] - base, v["pos"][2] - cz,
              v["tex"][0], v["tex"][1]) for v in vs]
    # Baked per-vertex ARGB is NOT decoration here and must not be dropped:
    # measured over these prefabs it takes 48 distinct values, and the dominant
    # one is 0xFFA0A0A0 on 1369 of 3084 vertices, with only 390 actually white.
    # tg_write_quad_mesh hardcodes 0xFFFFFFFF, which would brighten most of this
    # geometry by about 60% and flatten a night track's shading.
    light = [int(v["light"]) & 0xFFFFFFFF for v in vs]
    cmds = [(int(c["texture_page_id"]), int(c["tri"]), int(c["quad"]))
            for c in part["mesh"]["commands"]]
    return {"local": local, "light": light, "cmds": cmds,
            "fx": max(xs) - min(xs), "fz": max(zs) - min(zs),
            "height": max(ys) - base}


def _pt_in_quad(px, pz, quad):
    """Even-odd point-in-polygon over a 4-gon's XZ footprint."""
    inside = False
    n = len(quad)
    j = n - 1
    for i in range(n):
        xi, zi = quad[i]
        xj, zj = quad[j]
        if (zi > pz) != (zj > pz) and \
           px < (xj - xi) * (pz - zi) / ((zj - zi) or 1e-9) + xi:
            inside = not inside
        j = i
    return inside


def fill_landmark_holes(o, cell=1500.0, tile=3000.0):
    """Close the gaps in a landmark's ground plane with a tiled grass plane.

    The rarity-seeded segmenter grabs the distinctive architecture plus whatever
    ground quads happen to be welded to it, which leaves the plaza a patchwork:
    grass tiles round the edges with holes between them (visible in the studio as
    a checkerboard the building floats over). This lays a continuous grass grid
    across the whole footprint AT the existing ground plane, skipping cells an
    existing tile already covers, so the holes fill and the real tiles still win
    where they exist. Grass under the building is hidden by the building.

    Returns a synthetic prim in the same shape as a real one (role/pages/nface/
    mesh), or None if the landmark has no ground plane to extend. Placed 4 units
    BELOW the real tiles (y-down: +4) so any overlap z-fights in the real tile's
    favour rather than the fill's."""
    gq = []          # existing ground quads, XZ
    gy = []          # existing ground y values
    gpage = None
    glight = 0xFFE8E8E8
    for p in o["prims"]:
        if p.get("role") != "ground":
            continue
        gpage = gpage or p["pages"][0]
        vs = p["mesh"]["vertices"]
        cur = 0
        for c in p["mesh"]["commands"]:
            tri, quad = int(c["tri"]), int(c["quad"])
            qb = cur + tri * 3
            for q in range(quad):
                b = qb + q * 4
                gq.append([(vs[b + k]["pos"][0], vs[b + k]["pos"][2]) for k in range(4)])
                glight = vs[b]["light"]
            for v in vs:
                gy.append(v["pos"][1])
            cur += tri * 3 + quad * 4
    if gpage is None or not gy:
        return None
    gy.sort()
    plane = gy[len(gy) // 2] + 4.0        # 4 below the real tiles
    xs, zs = [], []
    for p in o["prims"]:
        for v in p["mesh"]["vertices"]:
            xs.append(v["pos"][0]); zs.append(v["pos"][2])
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
    verts, nq = [], 0
    x = x0
    while x < x1 - 1.0:
        z = z0
        while z < z1 - 1.0:
            if not any(_pt_in_quad(x + cell / 2, z + cell / 2, q) for q in gq):
                for vx, vz in ((x, z), (x + cell, z), (x + cell, z + cell), (x, z + cell)):
                    verts.append({"pos": [vx, plane, vz],
                                  "tex": [(vx - x0) / tile, (vz - z0) / tile],
                                  "light": glight})
                nq += 1
            z += cell
        x += cell
    if not nq:
        return None
    return {"role": "ground", "pages": [gpage], "nface": nq,
            "mesh": {"vertices": verts,
                     "commands": [{"texture_page_id": gpage, "tri": 0, "quad": nq}]}}


def fill_landmark_walls(o, roles=("wall", "roof"), maxgap=3000.0,
                        tile=1500.0, ground_frac=0.30):
    """Close the gaps in a landmark's walls and roofs by BRIDGING FREE EDGES.

    A landmark's walls are open quad strips; where the segmenter cut one, the cut
    leaves a FREE edge -- an edge used by exactly one face. The two wings of the
    Kremlin wall each end in a free vertical edge, and those two edges face each
    other across the corner gap. This finds every free edge and bridges facing
    pairs with a quad whose four corners ARE those existing vertices, so the fill
    lands exactly on the real geometry instead of on an invented rectangle.

    Guards against filling a solid panel:
    - only PARALLEL free edges of SIMILAR length are paired;
    - the bridge must point OUTWARD from each edge's owner face (away from its
      centroid), so an edge is only ever zipped to something across open space,
      never back over the panel it belongs to;
    - nearest pair wins, each edge used once.

    Returns one synthetic prim per page (empty list if nothing bridges)."""
    import math
    from collections import defaultdict

    # Only bridge free edges that reach the GROUND BAND -- the bottom `ground_frac`
    # of the landmark's height. The perimeter wall stands on the grass; the
    # cathedral towers sit on a raised base, so this cleanly separates the wall
    # corner (which must close) from the gaps between separate towers (which must
    # NOT web together). Without it, parallel tower edges within maxgap bridge
    # across the central open space and star the model shut.
    all_y = [v["pos"][1] for p in o["prims"] if p.get("role") in roles
             for v in p["mesh"]["vertices"]]
    if not all_y:
        return []
    ground_band = min(all_y) + ground_frac * (max(all_y) - min(all_y))

    def key(pos):
        return (round(pos[0]), round(pos[1]), round(pos[2]))

    edge_count = defaultdict(int)
    edge_data = {}           # boundary edge -> (page, light, owner_centroid)
    for p in o["prims"]:
        if p.get("role") not in roles:
            continue
        vs = p["mesh"]["vertices"]
        cur = 0
        for c in p["mesh"]["commands"]:
            tri, quad = int(c["tri"]), int(c["quad"])
            polys = []
            for t in range(tri):
                polys.append([vs[cur + t * 3 + k] for k in range(3)])
            qb = cur + tri * 3
            for q in range(quad):
                b = qb + q * 4
                polys.append([vs[b + k] for k in range(4)])
            cur += tri * 3 + quad * 4
            page = c["texture_page_id"]
            for poly in polys:
                pts = [v["pos"] for v in poly]
                cen = (sum(v[0] for v in pts) / len(pts),
                       sum(v[1] for v in pts) / len(pts),
                       sum(v[2] for v in pts) / len(pts))
                ids = [key(v) for v in pts]
                n = len(ids)
                for i in range(n):
                    a, bb = ids[i], ids[(i + 1) % n]
                    if a == bb:
                        continue
                    e = frozenset((a, bb))
                    edge_count[e] += 1
                    edge_data.setdefault(e, (page, poly[0]["light"], cen))

    def sub(u, v):
        return (u[0] - v[0], u[1] - v[1], u[2] - v[2])
    def dot(u, v):
        return u[0] * v[0] + u[1] * v[1] + u[2] * v[2]
    def norm(u):
        L = math.sqrt(dot(u, u)) or 1.0
        return (u[0] / L, u[1] / L, u[2] / L)

    E = []
    for e, cnt in edge_count.items():
        if cnt != 1:
            continue
        a, b = list(e)
        pa = (float(a[0]), float(a[1]), float(a[2]))
        pb = (float(b[0]), float(b[1]), float(b[2]))
        L = math.dist(pa, pb)
        if L < 1.0:
            continue
        if min(pa[1], pb[1]) > ground_band:     # not a ground-standing wall edge
            continue
        page, light, cen = edge_data[e]
        mid = ((pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2, (pa[2] + pb[2]) / 2)
        E.append(dict(a=pa, b=pb, u=norm(sub(pb, pa)), mid=mid, L=L,
                      page=page, light=light,
                      out=norm(sub(mid, cen))))   # outward from owner face

    # candidate bridges: parallel, similar length, within maxgap, and each edge
    # sees the other on its OUTWARD side (into open space).
    cands = []
    for i in range(len(E)):
        for j in range(i + 1, len(E)):
            e1, e2 = E[i], E[j]
            if abs(dot(e1["u"], e2["u"])) < 0.85:
                continue
            gap = math.dist(e1["mid"], e2["mid"])
            if gap < 40.0 or gap > maxgap:
                continue
            if min(e1["L"], e2["L"]) / max(e1["L"], e2["L"]) < 0.55:
                continue
            # Reject only bridges that fold BACK OVER a panel (toward its
            # centroid) -- that would duplicate an existing face. A perpendicular
            # bridge (a wall END reaching sideways to a perpendicular wing) is
            # exactly the corner we want, so it must pass.
            d12 = norm(sub(e2["mid"], e1["mid"]))
            if dot(d12, e1["out"]) < -0.25:         # bridge folds over e1's face
                continue
            if dot(d12, e2["out"]) > 0.25:          # bridge folds over e2's face
                continue
            cands.append((gap, i, j))
    cands.sort()

    used = set()
    per_page = defaultdict(list)
    for gap, i, j in cands:
        if i in used or j in used:
            continue
        e1, e2 = E[i], E[j]
        # orient e2 so the quad does not self-cross
        if (math.dist(e1["b"], e2["b"]) + math.dist(e1["a"], e2["a"])) <= \
           (math.dist(e1["b"], e2["a"]) + math.dist(e1["a"], e2["b"])):
            qA, qB = e2["a"], e2["b"]
        else:
            qA, qB = e2["b"], e2["a"]
        if math.dist(e1["a"], qA) > maxgap or math.dist(e1["b"], qB) > maxgap:
            continue
        used.add(i); used.add(j)
        corners = [e1["a"], e1["b"], qB, qA]
        across = math.dist(e1["a"], qA) or 1.0
        uv = [(0.0, 0.0), (0.0, e1["L"] / tile),
              (across / tile, e1["L"] / tile), (across / tile, 0.0)]
        for c, (uu, vv) in zip(corners, uv):
            per_page[e1["page"]].append(
                {"pos": [c[0], c[1], c[2]], "tex": [uu, vv], "light": e1["light"]})

    out = []
    for page, verts in per_page.items():
        nq = len(verts) // 4
        if nq:
            out.append({"role": "wall", "pages": [page], "nface": nq,
                        "mesh": {"vertices": verts,
                                 "commands": [{"texture_page_id": page,
                                               "tri": 0, "quad": nq}]}})
    return out
def _prefab_from_landmark(o, name):
    """Localise a segmented landmark (many world-space primitives) into one
    prefab, same convention as _prefab_geometry: centred in XZ, base y=0."""
    xs, ys, zs = [], [], []
    for m in o["prims"]:
        for v in m["mesh"]["vertices"]:
            xs.append(v["pos"][0]); ys.append(v["pos"][1]); zs.append(v["pos"][2])
    cx = (min(xs) + max(xs)) / 2.0
    cz = (min(zs) + max(zs)) / 2.0
    base = min(ys)
    local, light, cmds = [], [], []
    for m in o["prims"]:
        vs = m["mesh"]["vertices"]
        cur = 0
        for c in m["mesh"]["commands"]:
            tri, quad = int(c["tri"]), int(c["quad"])
            n = tri * 3 + quad * 4
            for k in range(cur, cur + n):
                v = vs[k]
                local.append((v["pos"][0] - cx, v["pos"][1] - base,
                              v["pos"][2] - cz, v["tex"][0], v["tex"][1]))
                light.append(int(v["light"]) & 0xFFFFFFFF)
            cmds.append((int(c["texture_page_id"]), tri, quad))
            cur += n
    return {"name": name, "local": local, "light": light, "cmds": cmds,
            "fx": max(xs) - min(xs), "fz": max(zs) - min(zs),
            "height": max(ys) - base, "nface": o["nface"]}


def cmd_landmarks(root, out, level, hdr_path, man_path, limit=24):
    """Export a level's DISTINCTIVE set pieces, found by rarity-seeded
    segmentation over the WHOLE track.

    Why not the object catalogue: its objects are chunks of street frontage.
    TD5 streetscape is per-span wall quads and neighbouring buildings share wall
    pages, so anything grown by proximity OR by shared texture chains along the
    road -- measured on level023, median footprint 17801 units, outlines that are
    diagonal staircases following the kerb. Distinctive architecture is instead
    marked by RARE art: one wall page is used by 636 primitives while 110 pages
    are used by three or fewer. Seeds are rare-page primitives and growth is
    anchored to the seed centre, which is what stops a cluster walking."""
    with open(os.path.join(out, "pages.json"), encoding="utf-8") as f:
        pages_doc = json.load(f)
    role = {p["page"]: p["role"] for p in pages_doc["pages"]
            if p["level"] == int(level)}
    model = _load_model(root, level)
    found = al.extract_landmarks(
        model, role, level_dir=os.path.join(root, "level%03d" % int(level)))
    print("  level%03d: %d distinctive set piece(s)" % (int(level), len(found)))
    prefabs = [_prefab_from_landmark(o, "L%d.lm%02d" % (int(level), i))
               for i, o in enumerate(found[:limit])]
    return _write_prefabs(prefabs, int(level), hdr_path, man_path)


def cmd_prefabs(root, out, level, entries, kinds, min_faces, hdr_path, man_path):
    rows = []
    with open(os.path.join(out, "objects", "level%03d.jsonl" % level),
              encoding="utf-8") as f:
        for line in f:
            o = json.loads(line)
            if o["entry"] in entries and o["kind"] in kinds \
                    and o["faces"] >= min_faces:
                rows.append(o)
    rows.sort(key=lambda r: -r["faces"])
    if not rows:
        print("  no objects matched"); return None

    model = _load_model(root, level)
    prefabs = []
    for r in rows:
        g = _prefab_geometry(model, r)
        g["name"] = r["id"]
        g["nface"] = r["faces"]
        prefabs.append(g)
    return _write_prefabs(prefabs, level, hdr_path, man_path)


def _write_prefabs(prefabs, level, hdr_path, man_path):
    """Emit the geometry header + the positional page manifest. Shared by
    cmd_prefabs and cmd_landmarks so the two cannot drift on the page-index
    convention -- the one thing that silently re-textures everything if it does."""
    if not prefabs:
        print("  nothing to write")
        return None
    pages, page_ix = [], {}
    for g in prefabs:
        for pg, _t, _q in g["cmds"]:
            if pg not in page_ix:
                page_ix[pg] = len(pages)
                pages.append(pg)

    # page manifest -- one set, indexed positionally by the geometry header
    man = {"header": "td5_tg_real_tex_landmarks.h", "prefix": "k_lm",
           "regenerate": True,
           "title": "Texture pages used by the shipped-geometry PREFABS in "
                    "td5_tg_prefab_data.h. Positional: page i here is local "
                    "index i there, so the two files must be regenerated "
                    "together by re/tools/td5_geomlib.py prefabs.",
           "sets": {"pf": [["level%03d" % level, pg,
                            "prefab page (local index %d)" % i]
                           for i, pg in enumerate(pages)]}}
    os.makedirs(os.path.dirname(man_path), exist_ok=True)
    with open(man_path, "w", newline="\n", encoding="utf-8") as f:
        json.dump(man, f, indent=2)
        f.write("\n")

    nv = sum(len(g["local"]) for g in prefabs)
    with open(hdr_path, "w", newline="\n", encoding="utf-8") as f:
        f.write("/* Shipped-geometry PREFABS, GENERATED by "
                "re/tools/td5_geomlib.py prefabs -- do not edit.\n"
                " *\n"
                " * Vertices are LOCAL: centred on the footprint in XZ with base"
                " y = 0, the same\n"
                " * convention td5_assetlib.extract_prototype uses, so a prefab"
                " stamped by the C\n"
                " * emitter lands where the Python editor would put it. Each"
                " command's page is a\n"
                " * LOCAL INDEX into %s's `pf` set, NOT a\n"
                " * shipped page id -- the two files are positional and must be"
                " regenerated\n"
                " * together.\n"
                " *\n"
                " * Commands consume vertices sequentially, tris before quads,"
                " which is the\n"
                " * MODELS.DAT rule the whole corpus obeys (94,493 meshes, zero"
                " exceptions).\n"
                " */\n" % man["header"])
        f.write("#ifndef TD5_TG_PREFAB_DATA_H\n#define TD5_TG_PREFAB_DATA_H\n\n")
        for i, g in enumerate(prefabs):
            sym = "k_pf%d" % i
            f.write("/* %s  %d faces, %d verts, %d cmds, "
                    "footprint %.0fx%.0f, height %.0f */\n"
                    % (g["name"], g.get("nface", 0), len(g["local"]),
                       len(g["cmds"]), g["fx"], g["fz"], g["height"]))
            f.write("static const float %s_v[] = {\n" % sym)
            for k in range(0, len(g["local"]), 2):
                chunk = g["local"][k:k + 2]
                f.write("    " + " ".join(
                    "%.1ff,%.1ff,%.1ff,%.5ff,%.5ff," % v for v in chunk) + "\n")
            f.write("};\n")
            f.write("static const unsigned int %s_l[] = {\n" % sym)
            for k in range(0, len(g["light"]), 8):
                f.write("    " + " ".join("0x%08Xu," % x
                                          for x in g["light"][k:k + 8]) + "\n")
            f.write("};\n")
            f.write("static const unsigned short %s_c[] = { %s };\n"
                    % (sym, " ".join("%d,%d,%d," % (page_ix[p], t, q)
                                     for p, t, q in g["cmds"])))
            f.write("\n")
        f.write("typedef struct {\n"
                "    const char           *name;\n"
                "    const float          *v;     /* x,y,z,u,v per vertex */\n"
                "    const unsigned int   *l;     /* baked ARGB per vertex */\n"
                "    int                   nv;\n"
                "    const unsigned short *c;     /* page_local,tri,quad */\n"
                "    int                   ncmd;\n"
                "    float                 fx, fz, height;\n"
                "} TG_PrefabDef;\n\n")
        f.write("static const TG_PrefabDef k_tg_prefabs[] = {\n")
        for i, g in enumerate(prefabs):
            f.write('    { "%s", k_pf%d_v, k_pf%d_l, %d, k_pf%d_c, %d, '
                    '%.1ff, %.1ff, %.1ff },\n'
                    % (g["name"], i, i, len(g["local"]), i, len(g["cmds"]),
                       g["fx"], g["fz"], g["height"]))
        f.write("};\n#define TD5_TG_PREFAB_N %d\n" % len(prefabs))
        f.write("#define TD5_TG_PREFAB_PAGES %d\n\n" % len(pages))
        f.write("#endif /* TD5_TG_PREFAB_DATA_H */\n")

    print("  %d prefabs, %d verts, %d distinct pages" % (len(prefabs), nv, len(pages)))
    for g in prefabs[:10]:
        print("    %-14s faces=%-4d %.0fx%.0f h=%.0f"
              % (g["name"], g.get("nface", 0), g["fx"], g["fz"], g["height"]))
    print("  wrote %s" % hdr_path)
    print("  wrote %s" % man_path)
    return man


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["pages", "objects", "build", "show", "verify",
                                    "roads", "prefabs", "landmarks"])
    ap.add_argument("--entries", default="25,27,28,29,32,50,53")
    ap.add_argument("--kinds", default="landmark,building,plaza")
    ap.add_argument("--min-faces", type=int, default=8)
    ap.add_argument("--levels", default="")
    ap.add_argument("--out", default=None)
    ap.add_argument("--level", type=int)
    ap.add_argument("--entry", type=int)
    ap.add_argument("--slot", type=int, default=0)
    a = ap.parse_args()

    root = _levels_dir()
    out = a.out or os.path.join(os.path.dirname(root), "library")
    levels = [int(x) for x in a.levels.split(",") if x.strip()] or _all_levels(root)

    if a.cmd == "show":
        return cmd_show(root, a.level, a.entry, a.slot)
    if a.cmd == "verify":
        return cmd_verify(root, levels)
    if a.cmd == "pages":
        cmd_pages(root, levels, out)
        return 0
    if a.cmd == "roads":
        cmd_roads(root, out)
        return 0
    if a.cmd == "landmarks":
        src = os.path.join(os.path.dirname(os.path.dirname(HERE)),
                           "td5mod", "src", "td5re")
        cmd_landmarks(root, out, a.level or 23,
                      os.path.join(src, "td5_tg_prefab_data.h"),
                      os.path.join(HERE, "manifests", "landmarks.json"))
        return 0
    if a.cmd == "prefabs":
        src = os.path.join(os.path.dirname(os.path.dirname(HERE)),
                           "td5mod", "src", "td5re")
        cmd_prefabs(root, out, a.level or 23,
                    {int(x) for x in a.entries.split(",") if x.strip()},
                    {k.strip() for k in a.kinds.split(",") if k.strip()},
                    a.min_faces,
                    os.path.join(src, "td5_tg_prefab_data.h"),
                    os.path.join(HERE, "manifests", "landmarks.json"))
        return 0

    roles = {}
    pj = os.path.join(out, "pages.json")
    if a.cmd == "build":
        idx = cmd_pages(root, levels, out)
    if os.path.isfile(pj):
        with open(pj, encoding="utf-8") as f:
            blob = json.load(f)
        for p in blob["pages"]:
            roles.setdefault(p["level"], {})[p["page"]] = p["role"]
        idx = blob["index"]
    else:
        idx = None
        print("  (no pages.json yet -- object kinds will fall back to shape only)")

    obj = cmd_objects(root, levels, out, roles)
    with open(os.path.join(out, "library.json"), "w", encoding="utf-8") as f:
        json.dump({"schema": SCHEMA, "generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
                   "pages": idx, "objects": obj}, f, indent=1)
    print("\nwrote %s" % os.path.join(out, "library.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
