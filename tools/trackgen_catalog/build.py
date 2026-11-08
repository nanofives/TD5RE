#!/usr/bin/env python3
"""build.py -- generate the AUTO-TRACK PIECE + TEXTURE CATALOGUE (static site).

    python tools/trackgen_catalog/build.py            # -> tools/trackgen_catalog/site/
    python tools/trackgen_catalog/serve.py            # http://127.0.0.1:6700/

What it reads (nothing is hand-listed, so a rebuild follows the code):
  * the SOURCE (tgcat_code.py): the TEXTURES.DAT slot registry
    (TD5_TG_PAGE_* in td5_trackgen_internal.h), every MESHTAG kind stamp
    (tg_guard_mark / tg_meshtag_set) with the emitter calls before it, every
    use of a page macro, the TD5RE_* knobs each function reads, the k_biomes
    table.
  * BUILT LEVELS (tgcat_level.py): the reference synthetic level
    re/assets/levels/level090 (seed 99991) plus every extra seed stored by
    gen_levels.py under tools/trackgen_catalog/levels/seed<N>/. Per level:
    MODELS.DAT + MESHTAG.BIN + TEXTURES.DAT + GENSTAMP.TXT + race.log (for the
    biome runs).
  * every SHIPPED level's textures.src, to tell a verbatim copy of real art
    from a procedural page.

What a "piece" is here: a connected component of faces (vertices welded on a
1-unit grid) inside one MODELS.DAT sub-mesh slot. Its signature is
(MESHTAG kind, texture pages it uses). Signatures are grouped into FAMILIES by
(kind, page FAMILY), the page family being the registry name without its
variant offset (R7_WALL_TOWER+3 -> R7_WALL_TOWER). Families are the cards on
the page; signatures are the variants inside a family.

Output: tools/trackgen_catalog/site/ (gitignored). Plain HTML + one CSS + one
small JS for the filters. No external CDN.
"""
from __future__ import annotations

import argparse
import collections
import html
import json
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import tgcat_code as C      # noqa: E402
import tgcat_level as L     # noqa: E402
import tgcat_render as R    # noqa: E402
import tgcat_md as MD       # noqa: E402

LEVELS_ROOT = os.path.join(REPO, "re", "assets", "levels")
REF_LEVEL = os.path.join(LEVELS_ROOT, "level090")
STORE = os.path.join(HERE, "levels")
FRAMES = os.path.join(STORE, "frames")

# What each MESHTAG kind is, in one line, and the page section it sits in.
KIND_INFO = {
    "ROAD":       ("Road surface", "Main carriageway quads, avenue divider, bridge-deck road top"),
    "BRANCHROAD": ("Road surface", "Fork / bypass corridor carriageway (spans past the ring)"),
    "SKIRT":      ("Ground & terrain", "Ground skirt either side of the road out to the terrain"),
    "TERRAIN":    ("Ground & terrain", "Side terrain, distant hills, skyline cards, treeline bands"),
    "CITY":       ("City street edge", "Pavement, kerb fence, facade walls along a city road"),
    "BLOCK":      ("City street edge", "City block fill behind the facades, sidewalks, underpass abutments"),
    "CROSS":      ("Side streets", "Side-street mouths, crossings and the facades that line them"),
    "DECAL":      ("Road markings", "Painted decals laid on the road (crossings, arrows)"),
    "BUILDING":   ("Buildings", "Buildings per span, landmark prefabs, verge trees next to them"),
    "FLORA":      ("Vegetation", "Roadside trees and bushes, forest crossings, slope flora"),
    "PARKTREE":   ("Vegetation", "Trees standing in city parks"),
    "PROP":       ("Props & furniture", "Billboard props, lamps, infrastructure, direction signs, signals"),
    "RAIL":       ("Guardrails", "W-beam guardrail on the road edge"),
    "GANTRY":     ("Gantries & track ends", "START / FINISH gantry and its legs"),
    "ENDWALL":    ("Gantries & track ends", "The wall that closes each end of a point-to-point track"),
    "DECK":       ("Bridges & overpasses", "Bridge deck, piers, beams, pylons, underpass deck"),
    "TUNNEL":     ("Tunnels & underpasses", "Bore lining, ceiling, portal surround and face"),
    "WATER":      ("Water & coast", "Sea / river / pond surface, water under bridges"),
    "COAST":      ("Water & coast", "Coast edge strip"),
    "BRANCHSIDE": ("Road surface", "Branch corridor side walls"),
    "OTHER":      ("Untagged", "Geometry the guard did not tag (corridor pads, fallback)"),
}
CAT_ORDER = ["Road surface", "Road markings", "Guardrails", "City street edge", "Side streets",
             "Buildings", "Vegetation", "Props & furniture", "Gantries & track ends",
             "Bridges & overpasses", "Tunnels & underpasses", "Water & coast",
             "Ground & terrain", "Untagged"]

FMT = {0: "opaque", 1: "colour-key", 2: "semi", 3: "opaque(3)"}

# level dir -> track name (schedule table in td5_asset.c + the frontend list).
TRACK_NAMES = {
    "level001": "Keswick", "level002": "San Francisco", "level003": "Bern",
    "level004": "Kyoto", "level005": "Washington", "level006": "Munich",
    "level013": "Honolulu", "level014": "Sydney", "level015": "Tokyo",
    "level016": "Edinburgh", "level017": "Blue Ridge", "level023": "Moscow",
    "level026": "Jarash", "level027": "Courmayeur", "level028": "Maui",
    "level029": "Newcastle", "level030": "Drag strip",
}

MAX_THUMB_FACES = 70


def esc(s):
    return html.escape(str(s), quote=True)


def slug(s):
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")


# ---------------------------------------------------------------------------
# Levels
# ---------------------------------------------------------------------------

def biome_runs(race_log):
    runs = []
    if not race_log or not os.path.exists(race_log):
        return runs
    pat = re.compile(r"biome run\s+\d+: spans\s+(\d+)-\s*(\d+)\s+(\w+)")
    for line in open(race_log, encoding="utf-8", errors="replace"):
        m = pat.search(line)
        if m:
            runs.append((int(m.group(1)), int(m.group(2)), m.group(3)))
        if runs and "biome run(s)" in line:
            break
    return runs


def biome_at(runs, span):
    for lo, hi, name in runs:
        if lo <= span <= hi:
            return name
    return "?"


def list_levels(ref_dir):
    out = []
    if os.path.exists(os.path.join(ref_dir, "MODELS.DAT")):
        st = L.read_genstamp(ref_dir)
        seed = st.get("seed", "?")
        log = os.path.join(REPO, "log", "race.log")
        stored = os.path.join(STORE, f"seed{seed}", "race.log")
        if os.path.exists(stored):
            log = stored
        out.append({"label": f"seed {seed}", "seed": seed, "dir": ref_dir,
                    "ref": True, "log": log})
    if os.path.isdir(STORE):
        for d in sorted(os.listdir(STORE)):
            p = os.path.join(STORE, d)
            if not d.startswith("seed") or not os.path.exists(os.path.join(p, "MODELS.DAT")):
                continue
            st = L.read_genstamp(p)
            seed = st.get("seed", d[4:])
            if any(l["seed"] == seed for l in out):
                continue
            out.append({"label": f"seed {seed}", "seed": seed, "dir": p, "ref": False,
                        "log": os.path.join(p, "race.log")})
    return out


def components(fl):
    parent = list(range(len(fl)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    owner = {}
    for i, (_, f) in enumerate(fl):
        for pos, _uv in f:
            key = (round(pos[0]), round(pos[1]), round(pos[2]))
            j = owner.get(key)
            if j is None:
                owner[key] = i
            else:
                a, b = find(i), find(j)
                if a != b:
                    parent[a] = b
    comps = collections.defaultdict(list)
    for i in range(len(fl)):
        comps[find(i)].append(i)
    return list(comps.values())


def crop_faces(faces, n=MAX_THUMB_FACES):
    if len(faces) <= n:
        return faces

    def cen(f):
        ps = [p for p, _ in f[1]]
        return [sum(c) / len(ps) for c in zip(*ps)]
    c0 = cen(faces[len(faces) // 2])
    return sorted(faces, key=lambda f: sum((a - b) ** 2 for a, b in zip(cen(f), c0)))[:n]


def analyse(levels, kinds, reg, spe):
    """sig -> {kind, pages, fam, count{seed}, biomes{seed:set}, faces{seed}, ex}"""
    fam_of = lambda p: reg["slot_name"].get(p, f"EXTRA{p - reg['count']}"  # noqa: E731
                                              if p >= reg["count"] else f"P{p}").split("+")[0]
    sigs = {}
    page_faces = collections.defaultdict(collections.Counter)   # seed -> page -> faces
    level_pages = {}
    for lv in levels:
        t0 = time.time()
        mt = L.read_meshtag(lv["dir"])
        runs = biome_runs(lv["log"])
        lv["biome_runs"] = runs
        pg = L.pages(lv["dir"])
        level_pages[lv["seed"]] = pg
        nslots = ncomp = 0
        for e, j, mesh in L.slots(lv["dir"]):
            nslots += 1
            k = L.tag_of(mt, e, j)
            kind = kinds[k] if k < len(kinds) else ("UNTAGGED" if k == 255 else f"K{k}")
            fl = list(L.faces(mesh))
            for p, _ in fl:
                page_faces[lv["seed"]][p] += 1
            biome = biome_at(runs, e * spe)
            for comp in components(fl):
                ncomp += 1
                pages = tuple(sorted({fl[i][0] for i in comp}))
                fams = tuple(sorted({fam_of(p) for p in pages}))
                key = (kind, pages)
                s = sigs.get(key)
                if s is None:
                    s = sigs[key] = {"kind": kind, "pages": pages, "fams": fams,
                                     "count": collections.Counter(),
                                     "biomes": collections.defaultdict(set),
                                     "faces": collections.Counter(), "ex": None,
                                     "entries": collections.defaultdict(set)}
                s["count"][lv["seed"]] += 1
                s["faces"][lv["seed"]] += len(comp)
                s["biomes"][lv["seed"]].add(biome)
                s["entries"][lv["seed"]].add(e)
                # example: prefer the reference level, then a mid-sized instance
                if s["ex"] is None or (lv["ref"] and not s["ex"]["ref"]) or (
                        s["ex"]["ref"] == lv["ref"] and s["ex"]["seed"] == lv["seed"]
                        and abs(len(comp) - 6) < abs(s["ex"]["n"] - 6)):
                    s["ex"] = {"seed": lv["seed"], "ref": lv["ref"], "entry": e, "slot": j,
                               "n": len(comp), "faces": [fl[i] for i in comp]}
        lv["nslots"], lv["ncomp"] = nslots, ncomp
        print(f"  {lv['label']:>16}: {nslots} slots, {ncomp} components, "
              f"{len(runs)} biome runs  ({time.time() - t0:.1f}s)", flush=True)
    return sigs, page_faces, level_pages


# ---------------------------------------------------------------------------
# Site
# ---------------------------------------------------------------------------

CSS = """
:root{--bg:#15171b;--panel:#1e2127;--line:#30343c;--fg:#e4e6ea;--dim:#9aa0aa;--acc:#e0a040;--ok:#5cb87a;--real:#4a90d0;--warn:#d06060}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,Segoe UI,sans-serif}
a{color:var(--acc);text-decoration:none}a:hover{text-decoration:underline}
header{display:flex;gap:18px;align-items:center;padding:10px 18px;background:#0e0f12;border-bottom:1px solid var(--line);position:sticky;top:0;z-index:5}
header b{font-size:15px}header nav a{margin-right:14px;color:var(--fg)}header nav a.on{color:var(--acc)}
main{padding:16px 18px;max-width:1500px}
h1{font-size:20px;margin:6px 0 10px}h2{font-size:17px;margin:22px 0 8px;border-bottom:1px solid var(--line);padding-bottom:4px}h3{font-size:15px;margin:16px 0 6px}
.dim{color:var(--dim)}.mono,code{font-family:Consolas,monospace;font-size:12.5px}
code{background:#262a31;padding:1px 4px;border-radius:3px}
pre{background:#0e0f12;border:1px solid var(--line);padding:10px;overflow:auto;border-radius:4px}pre code{background:none;padding:0}
.stats{display:flex;gap:10px;flex-wrap:wrap;margin:6px 0 14px}.stat{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:8px 12px}.stat b{font-size:20px;display:block}
.filters{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:8px 0 12px;background:var(--panel);padding:8px;border-radius:6px;border:1px solid var(--line)}
.filters select,.filters input{background:#0e0f12;color:var(--fg);border:1px solid var(--line);border-radius:4px;padding:4px 6px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:10px}
.grid.tex{grid-template-columns:repeat(auto-fill,minmax(150px,1fr))}
.card{background:var(--panel);border:1px solid var(--line);border-radius:6px;overflow:hidden;display:block;color:var(--fg)}
.card:hover{border-color:var(--acc);text-decoration:none}
.card img{width:100%;display:block;background:#0e0f12;image-rendering:auto}.grid.tex .card img{image-rendering:pixelated}
.card .t{padding:6px 8px}.card .n{font-weight:600;font-size:13px;word-break:break-word}.card .m{color:var(--dim);font-size:12px}
.badge{display:inline-block;font-size:11px;padding:0 5px;border-radius:3px;border:1px solid var(--line);margin:2px 3px 0 0;color:var(--dim)}
.badge.proc{color:var(--ok);border-color:var(--ok)}.badge.real{color:var(--real);border-color:var(--real)}.badge.recol{color:#a080e0;border-color:#a080e0}.badge.approx{color:#60c0c0;border-color:#60c0c0}.badge.unused{color:var(--warn);border-color:var(--warn)}
table{border-collapse:collapse;margin:6px 0 12px}td,th{border:1px solid var(--line);padding:4px 7px;vertical-align:top;text-align:left}th{background:#0e0f12}
.detail{display:grid;grid-template-columns:400px 1fr;gap:18px}.detail img.big{width:400px;border:1px solid var(--line);border-radius:6px}
.pix{image-rendering:pixelated}.vgrid{display:flex;flex-wrap:wrap;gap:8px}.vgrid .card{width:170px}
.hide{display:none!important}.small{font-size:12px}
.guide{max-width:980px}.guide li{margin:3px 0}
"""

JS = """
function tgFilter(){
  var q=(document.getElementById('q')||{}).value||'';q=q.toLowerCase();
  var sels=document.querySelectorAll('.filters select');var n=0,tot=0;
  document.querySelectorAll('.card[data-f]').forEach(function(c){
    var ok=true;tot++;
    sels.forEach(function(s){var v=s.value;if(v&&(' '+c.getAttribute('data-'+s.id)+' ').indexOf(' '+v+' ')<0)ok=false;});
    if(q&&c.getAttribute('data-f').toLowerCase().indexOf(q)<0)ok=false;
    c.classList.toggle('hide',!ok);if(ok)n++;});
  var cnt=document.getElementById('cnt');if(cnt)cnt.textContent=n+' / '+tot;
  document.querySelectorAll('section[data-cat]').forEach(function(s){
    s.classList.toggle('hide',!s.querySelector('.card[data-f]:not(.hide)'));});
}
"""


def page_html(title, body, active, depth=0):
    up = "../" * depth
    nav = [("index.html", "Pieces", "pieces"), ("textures.html", "Textures", "textures"),
           ("code.html", "Code map", "code"), ("frames.html", "In-game frames", "frames"),
           ("howto.html", "How to add", "howto")]
    links = "".join(f'<a href="{up}{h}" class="{"on" if k == active else ""}">{t}</a>'
                    for h, t, k in nav)
    return (f"<!doctype html><html><head><meta charset='utf-8'><title>{esc(title)}</title>"
            f"<link rel='stylesheet' href='{up}style.css'><script src='{up}app.js'></script></head>"
            f"<body><header><b>TD5RE auto-track catalogue</b><nav>{links}</nav></header>"
            f"<main>{body}</main></body></html>")


def src_link(f, line, label=None, depth=0):
    up = "../" * depth
    label = label or f"{f}:{line}"
    return f'<a class="mono" href="{up}src/{f}.html#L{line}">{esc(label)}</a>'


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def build(args):
    t0 = time.time()
    out = args.out
    if os.path.isdir(out):
        for sub in ("pieces", "tex", "img", "src", "frames"):
            shutil.rmtree(os.path.join(out, sub), ignore_errors=True)
    os.makedirs(out, exist_ok=True)

    print("source ...", flush=True)
    reg = C.page_registry()
    kinds = C.kind_names()
    ci = C.CodeIndex()
    esites = C.emitter_sites(ci)
    prefs = C.page_refs(ci)
    biomes = C.biomes(ci)
    spe = reg["values"].get("TD5_TG_SPANS_PER_ENTRY", 4)

    print("levels ...", flush=True)
    levels = list_levels(args.level)
    if not levels:
        sys.exit("no built level: run the game on slot 60 (verify/geo_tags_identity.ps1) "
                 "or tools/trackgen_catalog/gen_levels.py first")
    sigs, page_faces, level_pages = analyse(levels, kinds, reg, spe)
    ref = next((l for l in levels if l["ref"]), levels[0])
    ref_seed = ref["seed"]
    seeds = [l["seed"] for l in levels]

    print("shipped texture index ...", flush=True)
    exact_ix, shape_ix = L.shipped_index(LEVELS_ROOT)
    sig_ix = L.shipped_signatures(LEVELS_ROOT)

    # ---- pages ---------------------------------------------------------------
    rpages = level_pages[ref_seed]
    textures = {p["slot"]: p["image"] for p in rpages}
    own_exact = {}
    page_info = []
    used_by = collections.defaultdict(set)        # slot -> family ids
    for p in rpages:
        s = p["slot"]
        ex, sh = L.page_keys(p["pal"], p["paln"], p["idx"])
        name = reg["slot_name"].get(s) or (f"EXTRA+{s - reg['count']}" if s >= reg["count"]
                                          else f"P{s}")
        fam = name.split("+")[0]
        src, src_badge = "procedural (drawn by td5_tg_pages.c)", "proc"
        if ex in exact_ix:
            lv, pp = exact_ix[ex][0]
            src = f"real: {lv} page {pp}" + (f" ({TRACK_NAMES[lv]})" if lv in TRACK_NAMES else "")
            src_badge = "real"
        elif sh in shape_ix:
            lv, pp = shape_ix[sh][0]
            src = f"recoloured real: {lv} page {pp}" + (f" ({TRACK_NAMES[lv]})" if lv in TRACK_NAMES else "")
            src_badge = "recol"
        else:
            am = L.approx_match(p["image"], *sig_ix)
            if am:
                lv, pp, err = am
                src = (f"close to shipped {lv} page {pp}" + (f" ({TRACK_NAMES[lv]})" if lv in TRACK_NAMES else "")
                       + f", mean colour error {err:.1f}/255 (re-quantized or edited copy)")
                src_badge = "approx"
        flat = len(set(p["idx"])) <= 1
        dup = own_exact.get(ex)
        own_exact.setdefault(ex, s)
        use = {sd: page_faces[sd].get(s, 0) for sd in seeds}
        page_info.append({"slot": s, "name": name, "fam": fam, "chain": reg["slot_chain"].get(s, []),
                          "fmt": FMT.get(p["fmt"], str(p["fmt"])), "paln": p["paln"],
                          "src": src, "badge": src_badge, "flat": flat, "dup": dup,
                          "use": use, "any_use": sum(use.values())})

    imgdir = os.path.join(out, "img")
    os.makedirs(os.path.join(imgdir, "tex"), exist_ok=True)
    for p in rpages:
        im = p["image"]
        im.save(os.path.join(imgdir, "tex", f"{p['slot']:03d}.png"))
        im.resize((192, 192), resample=0).save(os.path.join(imgdir, "tex", f"{p['slot']:03d}_x3.png"))

    # ---- families --------------------------------------------------------------
    fams = collections.OrderedDict()
    for key, s in sorted(sigs.items(), key=lambda kv: (kv[0][0], kv[1]["fams"], kv[0][1])):
        fid = slug(s["kind"] + "-" + "-".join(s["fams"]))
        f = fams.get(fid)
        if f is None:
            info = KIND_INFO.get(s["kind"], ("Untagged", ""))
            f = fams[fid] = {"id": fid, "kind": s["kind"], "fams": s["fams"],
                             "cat": info[0], "kdoc": info[1], "sigs": [],
                             "count": collections.Counter(), "biomes": set(),
                             "entries": collections.defaultdict(set)}
        f["sigs"].append(s)
        for sd, n in s["count"].items():
            f["count"][sd] += n
            f["entries"][sd] |= s["entries"][sd]
        for sd, bs in s["biomes"].items():
            f["biomes"] |= bs
        for pg in s["pages"]:
            used_by[pg].add(fid)
    for f in fams.values():
        f["sigs"].sort(key=lambda s: -sum(s["count"].values()))

    print(f"render {len(fams)} families / {len(sigs)} variants ...", flush=True)
    pdir = os.path.join(imgdir, "pc")
    os.makedirs(pdir, exist_ok=True)
    sig_img = {}
    for f in fams.values():
        for i, s in enumerate(f["sigs"]):
            ex = s["ex"]
            texs = textures if ex["seed"] == ref_seed else {
                p["slot"]: p["image"] for p in level_pages[ex["seed"]]}
            faces = crop_faces(ex["faces"])
            name = f"{f['id']}__{i}"
            img = R.render(faces, texs, size=320 if i == 0 else 200)
            img.save(os.path.join(pdir, name + ".png"))
            sig_img[id(s)] = name

    # ---- write -----------------------------------------------------------------
    write(os.path.join(out, "style.css"), CSS)
    write(os.path.join(out, "app.js"), JS)

    def fam_name(f):
        return f"{f['kind'].lower()} / {' + '.join(f['fams'])}"

    def page_doc(short):
        for e in reg["names"]:
            if e["short"] == short:
                return e
        return None

    # code knowledge per family
    def fam_code(f):
        sites = esites.get(f["kind"], [])
        refs = []
        for pf in f["fams"]:
            for r in prefs.get(pf, []):
                if r["role"] == "geom":
                    refs.append((pf, r))
        knobs = set()
        for s in sites:
            for em in s["emitters"]:
                knobs |= set(ci.knobs_in(em))
        for _, r in refs:
            knobs |= set(ci.knobs_in(r["func"]))
        return sites, refs, sorted(knobs)

    # index.html -- pieces grid
    tot_inst = sum(sum(f["count"].values()) for f in fams.values())
    ref_fams = sum(1 for f in fams.values() if f["count"].get(ref_seed))
    cats = [c for c in CAT_ORDER if any(f["cat"] == c for f in fams.values())]
    all_biomes = sorted({b for f in fams.values() for b in f["biomes"] if b != "?"})
    body = [f"<h1>Every piece the auto track generated</h1>",
            "<p class='dim'>A piece is a connected group of faces in one MODELS.DAT sub-mesh. "
            "It is named by its MESHTAG emitter kind and the texture pages it uses. Cards group "
            "the variants by page family. Click a card for its variants, the code that emits it, "
            "its knobs and its texture pages.</p>",
            "<div class='stats'>",
            f"<div class='stat'><b>{len(fams)}</b>piece families</div>",
            f"<div class='stat'><b>{len(sigs)}</b>variants (kind x pages)</div>",
            f"<div class='stat'><b>{ref_fams}</b>families in the reference level</div>",
            f"<div class='stat'><b>{len(rpages)}</b>texture pages ({reg['count']} registry slots)</div>",
            f"<div class='stat'><b>{len(levels)}</b>levels scanned: {esc(', '.join(seeds))}</div>",
            "</div>",
            "<div class='filters'>Category <select id='cat' onchange='tgFilter()'><option value=''>all</option>"
            + "".join(f"<option value='{slug(c)}'>{esc(c)}</option>" for c in cats) + "</select>"
            "Biome <select id='biome' onchange='tgFilter()'><option value=''>all</option>"
            + "".join(f"<option>{esc(b)}</option>" for b in all_biomes) + "</select>"
            "Seen in <select id='seen' onchange='tgFilter()'><option value=''>any level</option>"
            "<option value='ref'>reference level</option><option value='other'>other seeds only</option></select>"
            "<input id='q' placeholder='search name / page' oninput='tgFilter()'> <span id='cnt' class='dim'></span></div>"]
    for c in cats:
        body.append(f"<section data-cat='{slug(c)}'><h2>{esc(c)}</h2><div class='grid'>")
        for f in fams.values():
            if f["cat"] != c:
                continue
            nref = f["count"].get(ref_seed, 0)
            seen = "ref" if nref else "other"
            img = sig_img[id(f["sigs"][0])]
            body.append(
                f"<a class='card' href='pieces/{f['id']}.html' data-f='{esc(fam_name(f))} {esc(' '.join(f['fams']))}'"
                f" data-cat='{slug(c)}' data-biome='{esc(' '.join(sorted(f['biomes'])))}' data-seen='{seen}'>"
                f"<img loading='lazy' src='img/pc/{img}.png'><div class='t'><div class='n'>{esc(fam_name(f))}</div>"
                f"<div class='m'>{len(f['sigs'])} variant(s), {nref} in reference"
                f"{'' if nref else ' (other seeds: ' + str(sum(f['count'].values())) + ')'}</div>"
                + "".join(f"<span class='badge'>{esc(b)}</span>" for b in sorted(f["biomes"]) if b != "?")
                + "</div></a>")
        body.append("</div></section>")
    body.append("<script>tgFilter()</script>")
    write(os.path.join(out, "index.html"), page_html("Pieces", "".join(body), "pieces"))

    # piece detail pages
    for f in fams.values():
        sites, refs, knobs = fam_code(f)
        b = [f"<p><a href='../index.html'>&larr; all pieces</a></p><h1>{esc(fam_name(f))}</h1>",
             "<div class='detail'><div>",
             f"<img class='big' src='../img/pc/{sig_img[id(f['sigs'][0])]}.png'>",
             "<p class='dim small'>Offline render of the most common variant from "
             f"seed {esc(f['sigs'][0]['ex']['seed'])}, entry {f['sigs'][0]['ex']['entry']} "
             f"slot {f['sigs'][0]['ex']['slot']} (spans {f['sigs'][0]['ex']['entry'] * spe}"
             f"-{f['sigs'][0]['ex']['entry'] * spe + spe - 1}).</p>",
             "</div><div>",
             f"<p><b>{esc(f['cat'])}</b>: {esc(f['kdoc'])}</p>",
             "<table><tr><th>MESHTAG kind</th><td class='mono'>TG_GK_" + esc(f["kind"]) + "</td></tr>",
             "<tr><th>Page families</th><td>" + "<br>".join(
                 f"<span class='mono'>TD5_TG_PAGE_{esc(pf)}</span>"
                 + (f" <span class='dim'>{esc(page_doc(pf)['comment'])}</span>" if page_doc(pf) and page_doc(pf)['comment'] else "")
                 + (f" ({src_link('td5_trackgen_internal.h', page_doc(pf)['line'], 'def', 1)})" if page_doc(pf) else "")
                 for pf in f["fams"]) + "</td></tr>",
             "<tr><th>Instances</th><td>" + ", ".join(
                 f"seed {esc(sd)}: <b>{f['count'].get(sd, 0)}</b> in {len(f['entries'].get(sd, ()))} entries"
                 for sd in seeds) + "</td></tr>",
             "<tr><th>Biomes seen</th><td>" + esc(", ".join(sorted(f["biomes"])) or "-") + "</td></tr>",
             "<tr><th>Knobs read on the way</th><td class='mono small'>" + (esc(", ".join(knobs)) or "-") + "</td></tr>",
             "</table>",
             "<h3>Emitted at (MESHTAG stamp sites for this kind)</h3><table><tr><th>stamp</th><th>in function</th><th>emitter call(s)</th></tr>"]
        for s in sites:
            ems = []
            for em in s["emitters"]:
                if em in ci.by_name:
                    ff, a, _ = ci.by_name[em]
                    ems.append(src_link(ff, a, em, 1))
                else:
                    ems.append(f"<span class='mono'>{esc(em)}</span>")
            b.append(f"<tr><td>{src_link(s['file'], s['line'], None, 1)}</td><td class='mono'>{esc(s['func'])}</td><td>{' '.join(ems)}</td></tr>")
        b.append("</table><h3>Where its pages go on geometry</h3><table><tr><th>page family</th><th>function</th><th>line</th></tr>")
        seen_fn = set()
        for pf, r in refs:
            k = (pf, r["func"])
            if k in seen_fn:
                continue
            seen_fn.add(k)
            b.append(f"<tr><td class='mono'>{esc(pf)}</td><td class='mono'>{esc(r['func'])}</td><td>{src_link(r['file'], r['line'], None, 1)}</td></tr>")
        b.append("</table></div></div>")
        b.append(f"<h2>Variants ({len(f['sigs'])})</h2><div class='vgrid'>")
        for s in f["sigs"]:
            pages = ", ".join(f"<a href='../tex/{p:03d}.html'>{p}</a>" for p in s["pages"])
            cnts = " / ".join(f"{s['count'].get(sd, 0)}" for sd in seeds)
            b.append(f"<div class='card'><img src='../img/pc/{sig_img[id(s)]}.png'><div class='t'>"
                     f"<div class='m'>pages {pages}</div><div class='m'>instances {cnts}</div>"
                     f"<div class='m'>example: seed {esc(s['ex']['seed'])} e{s['ex']['entry']} s{s['ex']['slot']}, {s['ex']['n']} faces</div>"
                     "</div></div>")
        b.append("</div><p class='dim small'>Instance counts are per level, in this order: "
                 + esc(", ".join(seeds)) + ".</p>")
        b.append("<h3>Texture pages</h3><div class='vgrid'>")
        for p in sorted({p for s in f["sigs"] for p in s["pages"]}):
            pi = next((x for x in page_info if x["slot"] == p), None)
            if pi is None:
                continue
            b.append(f"<a class='card' href='../tex/{p:03d}.html' style='width:120px'><img class='pix' src='../img/tex/{p:03d}_x3.png'>"
                     f"<div class='t'><div class='m'>{p} {esc(pi['name'])}</div></div></a>")
        b.append("</div>")
        write(os.path.join(out, "pieces", f["id"] + ".html"),
              page_html(fam_name(f), "".join(b), "pieces", 1))

    # textures.html
    nproc = sum(1 for p in page_info if p["badge"] == "proc")
    nreal = sum(1 for p in page_info if p["badge"] == "real")
    nrec = sum(1 for p in page_info if p["badge"] == "recol")
    napx = sum(1 for p in page_info if p["badge"] == "approx")
    nunused = sum(1 for p in page_info if p["any_use"] == 0)
    pfams = sorted({p["fam"] for p in page_info}, key=lambda n: min(
        x["slot"] for x in page_info if x["fam"] == n))
    tb = ["<h1>Every TEXTURES.DAT page of the auto track</h1>",
          f"<p class='dim'>Decoded from the reference level ({esc(ref['label'])}). A page is "
          "<span class='badge real'>real</span> when its palette and pixels match a shipped track page byte for byte, "
          "<span class='badge recol'>recoloured</span> when only the pixels match, "
          "<span class='badge approx'>close to shipped</span> when its 16x16 colour signature is within 7/255 of a shipped page "
          "(a re-quantized or edited copy), and "
          "<span class='badge proc'>procedural</span> when it matches nothing shipped (drawn by td5_tg_pages.c). "
          "<span class='badge unused'>unused</span> = no face uses it in any scanned level.</p>",
          "<div class='stats'>",
          f"<div class='stat'><b>{len(page_info)}</b>pages</div>",
          f"<div class='stat'><b>{nproc}</b>procedural</div>",
          f"<div class='stat'><b>{nreal}</b>real shipped</div>",
          f"<div class='stat'><b>{nrec}</b>recoloured shipped</div>",
          f"<div class='stat'><b>{napx}</b>close to a shipped page</div>",
          f"<div class='stat'><b>{nunused}</b>unused in scanned levels</div></div>",
          "<div class='filters'>Source <select id='src' onchange='tgFilter()'><option value=''>all</option>"
          "<option value='proc'>procedural</option><option value='real'>real</option><option value='recol'>recoloured</option>"
          "<option value='approx'>close to shipped</option></select>"
          "Use <select id='use' onchange='tgFilter()'><option value=''>all</option><option value='used'>used</option><option value='unused'>unused</option></select>"
          "Family <select id='pfam' onchange='tgFilter()'><option value=''>all</option>"
          + "".join(f"<option>{esc(n)}</option>" for n in pfams) + "</select>"
          "<input id='q' placeholder='search slot / name / source' oninput='tgFilter()'> <span id='cnt' class='dim'></span></div>",
          "<div class='grid tex'>"]
    for p in page_info:
        used = "used" if p["any_use"] else "unused"
        tb.append(
            f"<a class='card' href='tex/{p['slot']:03d}.html' data-f='{p['slot']} {esc(p['name'])} {esc(p['src'])}'"
            f" data-src='{p['badge']}' data-use='{used}' data-pfam='{esc(p['fam'])}'>"
            f"<img loading='lazy' src='img/tex/{p['slot']:03d}_x3.png'><div class='t'><div class='n'>{p['slot']} {esc(p['name'])}</div>"
            f"<span class='badge {p['badge']}'>{ {'proc': 'procedural', 'real': 'real', 'recol': 'recoloured', 'approx': 'close to shipped'}[p['badge']]}</span>"
            + (f"<span class='badge unused'>unused</span>" if not p["any_use"] else "")
            + (f"<span class='badge'>{esc(p['fmt'])}</span>" if p["fmt"] != "opaque" else "")
            + (f"<span class='badge'>dup of {p['dup']}</span>" if p["dup"] is not None else "")
            + "</div></a>")
    tb.append("</div><script>tgFilter()</script>")
    write(os.path.join(out, "textures.html"), page_html("Textures", "".join(tb), "textures"))

    # texture detail pages
    fam_by_id = fams
    for p in page_info:
        d = page_doc(p["fam"])
        writers, users = [], []
        for short in p["chain"]:
            for r in prefs.get(short, []):
                (writers if r["role"] == "page" else users).append((short, r))
        b = [f"<p><a href='../textures.html'>&larr; all textures</a></p>",
             f"<h1>Page {p['slot']}: {esc(p['name'])}</h1><div class='detail'><div>",
             f"<img class='big pix' src='../img/tex/{p['slot']:03d}.png'></div><div><table>",
             f"<tr><th>Slot</th><td>{p['slot']}</td></tr>",
             f"<tr><th>Registry</th><td class='mono'>" + esc(" > ".join(p["chain"]) or p["name"])
             + (f" ({src_link('td5_trackgen_internal.h', d['line'], 'def', 1)})" if d else "") + "</td></tr>",
             f"<tr><th>Comment</th><td>{esc(d['comment']) if d and d['comment'] else '-'}</td></tr>",
             f"<tr><th>Source</th><td><span class='badge {p['badge']}'>{esc(p['badge'])}</span> {esc(p['src'])}</td></tr>",
             f"<tr><th>Format</th><td>{esc(p['fmt'])}, {p['paln']} colours" + (", flat colour" if p["flat"] else "") + "</td></tr>",
             "<tr><th>Faces using it</th><td>" + ", ".join(f"seed {esc(sd)}: {n}" for sd, n in p["use"].items()) + "</td></tr>",
             "<tr><th>Piece families</th><td>" + (" ".join(
                 f"<a href='../pieces/{fid}.html'>{esc(fam_name(fam_by_id[fid]))}</a><br>" for fid in sorted(used_by.get(p["slot"], ())))
                 or "-") + "</td></tr>",
             "</table><h3>Written by (td5_tg_pages.c)</h3><table>"]
        for short, r in writers[:40]:
            b.append(f"<tr><td class='mono'>{esc(short)}</td><td class='mono'>{esc(r['func'])}</td><td>{src_link(r['file'], r['line'], None, 1)}</td></tr>")
        b.append(f"</table><h3>Put on geometry by ({len(users)} refs)</h3><table>")
        for short, r in users[:60]:
            b.append(f"<tr><td class='mono'>{esc(short)}</td><td class='mono'>{esc(r['func'])}</td><td>{src_link(r['file'], r['line'], None, 1)}</td></tr>")
        b.append("</table></div></div>")
        write(os.path.join(out, "tex", f"{p['slot']:03d}.html"),
              page_html(f"Page {p['slot']}", "".join(b), "textures", 1))

    # code.html -- registry, emitters, biomes, gaps
    seen_pfams = {pf for f in fams.values() for pf in f["fams"]}
    seen_kinds = {f["kind"] for f in fams.values()}
    cb = ["<h1>Code map: who emits what</h1>",
          "<h2>MESHTAG kinds and their stamp sites</h2><table><tr><th>kind</th><th>category</th><th>families</th><th>stamp sites (emitter calls)</th></tr>"]
    for k in kinds:
        sites = esites.get(k, [])
        nf = sum(1 for f in fams.values() if f["kind"] == k)
        cb.append(f"<tr><td class='mono'>{esc(k)}</td><td>{esc(KIND_INFO.get(k, ('',))[0])}</td><td>{nf}</td><td>"
                  + "<br>".join(f"{src_link(s['file'], s['line'])} <span class='mono small'>{esc(', '.join(s['emitters']))}</span>" for s in sites)
                  + "</td></tr>")
    cb.append("</table><h2>Gaps: emitted by the code, not seen in any scanned level</h2>")
    gk = [k for k in esites if k not in seen_kinds]
    cb.append("<p>Kinds: " + (esc(", ".join(gk)) or "none") + "</p>")
    cb.append("<table><tr><th>page family</th><th>slots</th><th>geometry refs</th></tr>")
    gaps = []
    used_slots = {pg for sd in seeds for pg, n in page_faces[sd].items() if n}
    for e in sorted(reg["names"], key=lambda e: e["start"]):
        if e.get("block") or any(e["start"] + k in used_slots for k in range(e["n"])):
            continue
        geo = [r for r in prefs.get(e["short"], []) if r["role"] == "geom"]
        if not geo:
            continue
        gaps.append(e["short"])
        cb.append(f"<tr><td class='mono'>{esc(e['short'])}</td><td>{e['start']}..{e['start'] + e['n'] - 1}</td><td>"
                  + "<br>".join(f"{src_link(r['file'], r['line'])} <span class='mono small'>{esc(r['func'])}</span>" for r in geo[:6])
                  + (f"<br><span class='dim'>+{len(geo) - 6} more</span>" if len(geo) > 6 else "") + "</td></tr>")
    cb.append("</table><p class='dim small'>A page family is a gap when a function puts it on geometry but no face in the "
              "scanned levels uses any slot of its range. Most are gated by a biome the seeds did not roll (snow), a knob that is "
              "off by default, night, or a geo-only path.</p>")
    cb.append("<h2>Biomes (k_biomes in td5_tg_terrain.c)</h2><table><tr><th>#</th><th>biome</th><th>row</th><th>levels where it ran</th></tr>")
    for bm in biomes:
        where = [l["seed"] for l in levels if any(r[2] == bm["name"] for r in l.get("biome_runs", []))]
        cb.append(f"<tr><td>{bm['index']}</td><td>{src_link('td5_tg_terrain.c', bm['line'], bm['name'])}</td>"
                  f"<td class='mono small'>{esc(bm['row'][:220])}</td><td>{esc(', '.join(where) or '-')}</td></tr>")
    cb.append("</table><h2>TEXTURES.DAT slot registry</h2><table><tr><th>macro</th><th>slots</th><th>comment</th></tr>")
    for e in sorted(reg["names"], key=lambda e: (e["start"], -e["n"])):
        cb.append(f"<tr><td>{src_link('td5_trackgen_internal.h', e['line'], 'TD5_TG_PAGE_' + e['short'])}</td>"
                  f"<td>{e['start']}..{e['start'] + e['n'] - 1}</td><td class='small'>{esc(e['comment'])}</td></tr>")
    cb.append("</table><h2>Levels scanned</h2><table><tr><th>level</th><th>GENSTAMP</th><th>slots</th><th>components</th><th>biome runs</th></tr>")
    for l in levels:
        st = L.read_genstamp(l["dir"]).get("raw", "")
        cb.append(f"<tr><td>{esc(l['label'])}{' (reference)' if l['ref'] else ''}</td><td class='mono small'>{esc(st)}</td>"
                  f"<td>{l['nslots']}</td><td>{l['ncomp']}</td><td class='small'>"
                  + esc(", ".join(f"{a}-{b} {n}" for a, b, n in l.get("biome_runs", []))) + "</td></tr>")
    cb.append("</table>")
    write(os.path.join(out, "code.html"), page_html("Code map", "".join(cb), "code"))

    # source viewer pages (static, so the site also works opened from disk)
    for fname in ci.files + ["td5_trackgen_internal.h"]:
        lines = ci.lines.get(fname) or C.read_lines(fname)
        rows = "".join(f"<tr id='L{i}'><td class='dim'>{i}</td><td><pre style='margin:0;border:0;padding:0;background:none'>{esc(t)}</pre></td></tr>"
                       for i, t in enumerate(lines, 1))
        write(os.path.join(out, "src", fname + ".html"),
              page_html(fname, f"<h1 class='mono'>td5mod/src/td5re/{esc(fname)}</h1>"
                        "<style>tr:target{background:#3a3220}td{border:0;padding:0 8px}</style>"
                        f"<table class='mono small'>{rows}</table>", "code", 1))

    # frames.html
    fb = ["<h1>In-game frames</h1><p class='dim'>Pieces that only read in context, captured with "
          "<code>verify/geo_tour.ps1 -Track 60</code> on the reference seed (see the how-to for the command).</p>"]
    man = os.path.join(FRAMES, "frames.json")
    if os.path.exists(man):
        os.makedirs(os.path.join(out, "frames"), exist_ok=True)
        for fr in json.load(open(man)):
            src = os.path.join(FRAMES, fr["file"])
            if not os.path.exists(src):
                continue
            shutil.copy2(src, os.path.join(out, "frames", fr["file"]))
            fb.append(f"<h3>{esc(fr['title'])}</h3><p class='dim small'>{esc(fr.get('note', ''))}</p>"
                      f"<img src='frames/{esc(fr['file'])}' style='max-width:100%;border:1px solid #30343c'>")
    else:
        fb.append("<p>No frames captured yet.</p>")
    write(os.path.join(out, "frames.html"), page_html("In-game frames", "".join(fb), "frames"))

    # howto.html
    guide = open(os.path.join(HERE, "HOWTO.md"), encoding="utf-8").read()
    write(os.path.join(out, "howto.html"),
          page_html("How to add", "<div class='guide'>" + MD.to_html(guide) + "</div>", "howto"))

    summary = {"families": len(fams), "variants": len(sigs), "pages": len(page_info),
               "registry_slots": reg["count"], "procedural": nproc, "real": nreal,
               "recoloured": nrec, "close_to_shipped": napx, "unused": nunused, "levels": seeds, "reference": ref_seed,
               "ref_families": ref_fams, "gap_page_families": gaps,
               "gap_kinds": gk, "seconds": round(time.time() - t0, 1)}
    write(os.path.join(out, "summary.json"), json.dumps(summary, indent=1))
    print(json.dumps(summary, indent=1))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--level", default=REF_LEVEL, help="reference level dir (default level090)")
    ap.add_argument("--out", default=os.path.join(HERE, "site"))
    return build(ap.parse_args())


if __name__ == "__main__":
    sys.exit(main())
