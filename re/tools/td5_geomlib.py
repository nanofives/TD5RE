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
ROAD_CLASSES = ("TARMAC", "PALE", "DIRT", "ROUGH", "ICE")


def road_class(lum, sat, sd, r, b):
    """Bucket a road page by colour and detail.

    NOT separated here: gravel from cobble. Peak autocorrelation over the ROUGH
    bucket runs 0.356..0.953 as a smooth continuum with no bimodality (p25 0.796,
    p50 0.863), because a 64x64 tiling page is periodic at the tile boundary
    whatever it depicts. So ROUGH serves both RS_GRAVEL and RS_COBBLE and wants
    a human split in the studio browser. Recording the failed measurement here
    so nobody re-derives it.
    """
    if lum >= 185.0 and sat < 0.14:
        return "ICE"
    if r - b > 14.0 and sat >= 0.12:
        return "DIRT"
    if sd >= 30.0:
        return "ROUGH"
    if lum >= 138.0:
        return "PALE"
    if lum >= 25.0:
        return "TARMAC"
    return None


def _page_stats(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    px = list(im.getdata())
    n = len(px)
    r = sum(q[0] for q in px) / n
    g = sum(q[1] for q in px) / n
    b = sum(q[2] for q in px) / n
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    mx, mn = max(r, g, b), min(r, g, b)
    sat = (mx - mn) / mx if mx else 0.0
    lums = [0.299 * q[0] + 0.587 * q[1] + 0.114 * q[2] for q in px]
    mean = sum(lums) / n
    sd = (sum((v - mean) ** 2 for v in lums) / n) ** 0.5
    return lum, sat, sd, r, g, b


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
        if p["type"] != 0:
            drop["alpha_keyed"] += 1
            continue
        rf = carriage[(p["level"], p["page"])]
        if rf <= 0:
            drop["never_a_carriageway"] += 1
            continue
        png = os.path.join(root, "level%03d" % p["level"], "textures.src",
                           "pages", "page_%03d.png" % p["page"])
        if not os.path.isfile(png):
            drop["no_png"] += 1
            continue
        lum, sat, sd, r, _g, b = _page_stats(png)
        if lum <= 1.0:
            drop["degenerate_black"] += 1
            continue
        h = hashlib.sha256(open(png, "rb").read()).hexdigest()
        if h in seen:
            drop["duplicate_art"] += 1
            continue
        seen[h] = True
        k = road_class(lum, sat, sd, r, b)
        if not k:
            drop["unclassified"] += 1
            continue
        buckets[k].append({"level": p["level"], "page": p["page"], "faces": rf,
                           "luma": round(lum, 1), "sat": round(sat, 2),
                           "detail": round(sd, 1)})

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
                    "paves in the shipped game. ROUGH serves both gravel and "
                    "cobble: they are not separable by texture statistics.",
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["pages", "objects", "build", "show", "verify",
                                    "roads"])
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
