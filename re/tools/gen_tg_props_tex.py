#!/usr/bin/env python3
"""Generate td5_tg_props_tex.h -- street-furniture texture pages for the
auto-track generator (round 9, INFRA area).

SOURCE AND PROVENANCE.  re/assets/props/td6_*.png are the 12 breakable
street-furniture prototype textures lifted out of Test Drive 6's static.zip by
re/tools/extract_td6_prop_meshes.py (FURNITURE_TGAS there names the shipped
TGAs).  They are 128x128 RGBA with the TGA alpha channel PRESERVED, and they are
ATLASES: each one packs four 64x64 sub-tiles used by different faces of the COL
prop mesh.  Nothing in the generator placed them, which is what this header
fixes.

WHY 64x64 SUB-TILES ARE THE RIGHT UNIT.  The trackgen page format is exactly
64x64 paletted (see tg_emit_real_page), so one atlas quadrant maps to one page
at NATIVE resolution -- no resampling, no aspect guessing.  A quadrant either
carries real alpha (a cutout, e.g. the bench's cast-iron end) or is fully
opaque; the `type` byte is chosen from the pixels rather than assumed.

Alpha-keyed pages key on palette INDEX 0, so for a cutout quadrant index 0 is
reserved for the key and the artwork is quantised into the remaining 255
entries.  Palettes are written BGR, the order the shipped .DAT stores.

Run from the repo root:  python re/tools/gen_tg_props_tex.py
"""
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "td5mod", "src", "td5re", "td5_tg_props_tex.h")
# A git worktree carries the source but not always the extracted asset tree,
# so allow it to be pointed at the main checkout (same rule as the other
# gen_trackgen_*_tex.py tools).
ASSETS = os.environ.get("TD5RE_ASSETS_ROOT", os.path.join(ROOT, "re", "assets"))

# Atlas quadrant order, matching the 128x128 layout: 0 = top-left, 1 = top-right,
# 2 = bottom-left, 3 = bottom-right.
QUAD = [(0, 0), (64, 0), (0, 64), (64, 64)]

# (C identifier, source png, quadrant, human note). Order IS the page order the
# header exports, and td5_trackgen.c indexes it by the TG_INFRA_* enum, so
# APPEND only -- never reorder.
PAGES = [
    ("crate",    "td6_1crate.png",   1, "wooden crate, X-braced face"),
    ("cratefrg", "td6_1crate.png",   2, "wooden crate, FRAGILE stencil face"),
    ("cardbox",  "td6_k1box.png",    3, "cardboard box, THIS WAY UP face"),
    ("binbody",  "td6_bins.png",     0, "galvanised bin body"),
    ("binlid",   "td6_bins.png",     1, "bin lid, seen from above"),
    ("phone",    "td6_1phone.png",   0, "phone box door + glazing"),
    ("bench",    "td6_bench.png",    0, "bench seat slats"),
    ("benchend", "td6_bench.png",    2, "ornate cast-iron bench end (cutout)"),
    ("canopy",   "td6_canopy.png",   2, "shop awning valance, scalloped"),
    ("worky",    "td6_1worky.png",   1, "roadworks barrier banding"),
    ("redtape",  "td6_redtape.png",  0, "red/white barrier plank (cutout)"),
    ("sign",     "td6_1bollard.png", 1, "circular road sign (cutout)"),
    ("rickshaw", "td6_rick1.png",    0, "rickshaw seat + frame (cutout)"),
]


def load_quad(png, quad):
    path = os.path.join(ASSETS, "props", png)
    im = Image.open(path)
    if im.size != (128, 128):
        raise SystemExit("%s is %dx%d, expected a 128x128 atlas" % (png,) + im.size)
    im = im.convert("RGBA")
    x, y = QUAD[quad]
    return im.crop((x, y, x + 64, y + 64))


def quantise(tile):
    """Return (bgr_palette_bytes, index_plane_4096, type).

    type 1 == alpha-keyed on index 0, type 0 == opaque.  Transparent texels are
    forced to index 0 AFTER quantisation, so no cutout hole can be quantised
    into a visible colour and no visible colour can collide with the key.
    """
    px = list(tile.getdata())
    keyed = [i for i, p in enumerate(px) if p[3] < 128]

    rgb = tile.convert("RGB")
    if keyed:
        # Flood the transparent texels with the MEAN opaque colour before
        # quantising. Left as-is they are whatever junk the TGA stored under the
        # cutout, and median-cut would spend palette entries describing pixels
        # that are never drawn.
        vis = [px[i] for i in range(len(px)) if px[i][3] >= 128]
        if not vis:
            raise SystemExit("tile is fully transparent")
        mean = tuple(sum(c[k] for c in vis) // len(vis) for k in range(3))
        flat = [mean if p[3] < 128 else p[:3] for p in px]
        rgb = Image.new("RGB", (64, 64))
        rgb.putdata(flat)
        ncol = 255
    else:
        ncol = 256

    q = rgb.quantize(colors=ncol, method=Image.Quantize.MEDIANCUT)
    idx = list(q.tobytes())
    pal = q.getpalette()[: ncol * 3]
    paln = max(idx) + 1
    pal = pal[: paln * 3]

    if keyed:
        # Shift every real colour up one so index 0 can be the key.
        idx = [v + 1 for v in idx]
        pal = [0, 0, 0] + pal
        paln += 1
        for i in keyed:
            idx[i] = 0
        ptype = 1
    else:
        ptype = 0

    # RGB -> BGR, the order the shipped .DAT palettes use.
    bgr = []
    for i in range(paln):
        r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        bgr += [b, g, r]
    assert len(idx) == 4096 and max(idx) < paln
    return bytes(bgr), bytes(idx), ptype


def emit_array(w, name, data, per_line=32):
    w("static const unsigned char %s[%d] = {" % (name, len(data)))
    for off in range(0, len(data), per_line):
        chunk = ",".join(str(b) for b in data[off:off + per_line])
        w("%s%s" % (chunk, "," if off + per_line < len(data) else ""))
    w("};")


def main():
    lines = []
    w = lines.append
    w("/* Street-furniture texture pages for the auto-track generator.")
    w(" * GENERATED by re/tools/gen_tg_props_tex.py, do not edit.")
    w(" *")
    w(" * Each page is one 64x64 quadrant of a 128x128 TD6 furniture atlas in")
    w(" * re/assets/props/ (extract_td6_prop_meshes.py), taken at native")
    w(" * resolution. Palettes are BGR, as the shipped .DAT stores them; a page")
    w(" * of type 1 is alpha-keyed on index 0 and its key coverage is noted. */")
    w("#ifndef TD5_TG_PROPS_TEX_H")
    w("#define TD5_TG_PROPS_TEX_H")

    pals, idxs, types, palns = [], [], [], []
    for ident, png, quad, note in PAGES:
        tile = load_quad(png, quad)
        pal, idx, ptype = quantise(tile)
        key = sum(1 for v in idx if v == 0) if ptype == 1 else 0
        w("/* %s q%d -> %s: %s, %d colours, type %d%s */"
          % (png, quad, ident.upper(), note, len(pal) // 3, ptype,
             ", key %.0f%%" % (100.0 * key / 4096.0) if ptype == 1 else ""))
        emit_array(w, "k_props_%s_pal" % ident, pal)
        emit_array(w, "k_props_%s_idx" % ident, idx)
        pals.append(ident)
        idxs.append(ident)
        types.append(ptype)
        palns.append(len(pal) // 3)

    n = len(PAGES)
    w("")
    w("#define TD5_TG_PROPS_TEX_COUNT %d" % n)
    w("static const unsigned char *const k_props_pal[%d] = {" % n)
    w(",".join("k_props_%s_pal" % i for i in pals))
    w("};")
    w("static const int k_props_paln[%d] = {" % n)
    w(",".join(str(v) for v in palns))
    w("};")
    w("static const unsigned char *const k_props_idx[%d] = {" % n)
    w(",".join("k_props_%s_idx" % i for i in idxs))
    w("};")
    w("static const int k_props_type[%d] = {" % n)
    w(",".join(str(t) for t in types))
    w("};")
    w("#endif /* TD5_TG_PROPS_TEX_H */")

    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    sys.stderr.write("wrote %s (%d pages)\n" % (OUT, n))


if __name__ == "__main__":
    main()
