#!/usr/bin/env python3
"""Generate td5_tg_real_tex_r11signs.h -- DIRECTION SIGNAGE for the auto-track
(R11 SIGNS, user item 16 "on curves, add street signs indicating where the track
goes").

THREE pages, and they are the ONLY arrow artwork in the shipped game. The R11
survey scored every 64x64 page of every level*/textures.src for a saturated
primary ground plus near-white glyph shapes plus a low colour count, and eyeballed
contact sheets of every candidate. Everything else that scored was START/FINISH
banner halves, sponsor and shopfront signage, Cyrillic/Japanese text panels or
flags. level023 (Moscow) pages 443/444/445 are a matched set of European direction
signs -- white arrow on a blue panel with a white border -- in LEFT, RIGHT,
STRAIGHT order:

  443  turn LEFT
  444  turn RIGHT
  445  STRAIGHT ahead

CROP, AND WHY IT HAPPENS HERE. Measured on the raw indices, not on a rendered
PNG: the sign panel occupies COLUMNS 0..30 and ROWS 0..61 of the 64x64 page. The
right half is not merely empty -- columns 31..63 of rows 0,1,60,61 carry palette
index 9 (BGR 212121, near-black grey), a 4-row atlas bleed artifact that is NOT
index 0 and therefore is NOT alpha-keyed away. Drawn with a plain u 0..1 quad the
page renders as a sign occupying the left 48% of its billboard with a thin dark
streak floating beside it.

Cropping at MINE TIME rather than adding a partial-UV parameter to
tg_emit_billboard_mesh keeps that shared emitter -- used by every tree, prop and
person in the generator -- untouched. The crop is a nearest-neighbour index
resample (never an interpolation: these are palette INDICES, and averaging two
indices produces an unrelated colour), so every output texel is verbatim one
input texel and the palette is copied whole.

ASPECT IS NOW THE CALLER'S JOB. The source panel is 31 x 62, i.e. 1:2, and the
crop stretches it to fill 64x64. The emitter must therefore draw the billboard at
height == 2 * width or the arrow comes out fat. td5_trackgen.c states this at the
sign dimension constants.

Palettes are BGR and indices 64x64, copied verbatim from the level's
textures.src, same source form as the four earlier generators.

Usage:  python re/tools/gen_trackgen_r11signs_tex.py [levels_dir]
Writes: td5mod/src/td5re/td5_tg_real_tex_r11signs.h
"""

import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LEVELS = sys.argv[1] if len(sys.argv) > 1 \
    else os.path.join(ROOT, "re", "assets", "levels")
OUT = os.path.join(ROOT, "td5mod", "src", "td5re", "td5_tg_real_tex_r11signs.h")

TEXELS = 64 * 64

# Pages already mined by gen_trackgen_city_tex.py, gen_trackgen_r7city_tex.py and
# gen_trackgen_r8var_tex.py, so this generator cannot double-count one.
ALREADY = {
    ("level002", 64), ("level002", 92), ("level002", 133), ("level002", 363),
    ("level023", 129), ("level023", 208),
    ("level015", 60), ("level015", 111), ("level015", 149), ("level015", 11),
    ("level002", 91), ("level002", 93),
    ("level023", 128), ("level023", 209),
    ("level013", 85), ("level013", 86), ("level013", 88), ("level013", 89),
    ("level014", 64),
    ("level015", 61), ("level015", 148), ("level015", 150),
    ("level002", 61), ("level002", 62), ("level002", 76), ("level002", 121),
    ("level013", 69), ("level023", 210),
    ("level013", 60), ("level013", 66), ("level013", 87), ("level002", 120),
    ("level003", 499), ("level003", 500), ("level003", 439), ("level003", 440),
    ("level014", 276), ("level018", 45), ("level023", 418), ("level013", 264),
}

# ORDER IS LOAD-BEARING: LEFT, RIGHT, STRAIGHT, matching the
# TD5_TG_PAGE_R11_SIGN_LEFT .. _STRAIGHT order the emitter indexes.
SIGNS = [
    ("level023", 443, "Moscow: blue direction sign, white LEFT turn arrow"),
    ("level023", 444, "Moscow: blue direction sign, white RIGHT turn arrow"),
    ("level023", 445, "Moscow: blue direction sign, white STRAIGHT arrow"),
]

# Measured visible extent of the panel inside the 64x64 page (see the module
# docstring). Asserted per page below rather than trusted.
CROP_W = 31
CROP_H = 62


def page(level, num):
    src = os.path.join(LEVELS, level, "textures.src")
    meta = json.load(open(os.path.join(src, "textures.json")))
    if num >= meta["page_count"]:
        sys.exit("%s has no page %d (page_count %d)"
                 % (level, num, meta["page_count"]))
    if (level, num) in ALREADY:
        sys.exit("%s page %d is ALREADY MINED by an earlier generator"
                 % (level, num))
    pal = bytes.fromhex(meta["pages"][num]["palette_hex"])
    with open(os.path.join(src, "indices.bin"), "rb") as f:
        f.seek(num * TEXELS)
        idx = f.read(TEXELS)
    if len(idx) != TEXELS:
        sys.exit("%s page %d: short index block" % (level, num))
    if max(idx) >= len(pal) // 3:
        sys.exit("%s page %d: index %d past palette (%d colours)"
                 % (level, num, max(idx), len(pal) // 3))
    return pal, idx, meta["pages"][num]["type"]


def crop_scale(level, num, idx):
    """Nearest-neighbour resample of the CROP_W x CROP_H panel up to 64x64.

    VERIFY THE PREMISE, do not assume it: the panel must actually be the only
    substantial content in the crop region, and the region outside it must carry
    nothing but the known artifact. A page that stops matching (a re-extract, a
    different level revision) must stop the generator, not silently ship a
    mis-cropped sign.
    """
    rows = [idx[y * 64:(y + 1) * 64] for y in range(64)]
    # Every texel of the panel must be inside the crop box.
    for y in range(64):
        for x in range(64):
            if rows[y][x] == 0:
                continue
            if x < CROP_W and y < CROP_H:
                continue
            # Outside the box, the ONLY thing tolerated is the measured
            # near-black bleed on rows 0,1,60,61 at columns >= CROP_W.
            if x >= CROP_W and y in (0, 1, 60, 61):
                continue
            sys.exit("%s page %d: unexpected texel at (%d,%d) index %d -- the "
                     "crop box %dx%d no longer describes this page"
                     % (level, num, x, y, rows[y][x], CROP_W, CROP_H))
    out = bytearray(TEXELS)
    for y in range(64):
        sy = (y * CROP_H) // 64
        for x in range(64):
            sx = (x * CROP_W) // 64
            out[y * 64 + x] = rows[sy][sx]
    return bytes(out)


def emit_array(out, decl, data, per_line=32):
    out.write("static const unsigned char %s[%d] = {" % (decl, len(data)))
    for i, v in enumerate(data):
        out.write(("\n" if i % per_line == 0 else "") + "%d," % v)
    out.write("};\n")


def emit_set(out, name, entries):
    for i, (level, num, why) in enumerate(entries):
        pal, idx, typ = page(level, num)
        if typ != 1:
            sys.exit("%s page %d: type %d, expected 1 (alpha-keyed on index 0) "
                     "-- a sign standing on a post MUST key its background"
                     % (level, num, typ))
        cropped = crop_scale(level, num, idx)
        key = cropped.count(0) * 100.0 / TEXELS
        out.write("/* %s page %03d -> %s %d: %s (%d colours, key %.1f%% after "
                  "the %dx%d crop) */\n"
                  % (level, num, name.upper(), i, why, len(pal) // 3, key,
                     CROP_W, CROP_H))
        out.write("static const int k_real_r11sign_%s%d_paln = %d;\n"
                  % (name, i, len(pal) // 3))
        emit_array(out, "k_real_r11sign_%s%d_pal" % (name, i), pal)
        emit_array(out, "k_real_r11sign_%s%d_idx" % (name, i), cropped)
    n = len(entries)
    out.write("static const int k_real_r11sign_%s_count = %d;\n" % (name, n))
    out.write("static const int k_real_r11sign_%s_paln[%d] = { %s };\n"
              % (name, n, ", ".join("k_real_r11sign_%s%d_paln" % (name, i)
                                    for i in range(n))))
    for kind in ("pal", "idx"):
        out.write("static const unsigned char *const "
                  "k_real_r11sign_%s_%s[%d] = { %s };\n"
                  % (name, kind, n,
                     ", ".join("k_real_r11sign_%s%d_%s" % (name, i, kind)
                               for i in range(n))))


def main():
    with open(OUT, "w", newline="\n") as out:
        out.write(
            "/* DIRECTION SIGNAGE pages for the auto-track (R11 SIGNS, item\n"
            " * 16), mined from level023 (Moscow) -- the only arrow artwork in\n"
            " * the shipped game. In LEFT, RIGHT, STRAIGHT order.\n"
            " *\n"
            " * CROPPED at mine time to the panel's measured 31x62 extent and\n"
            " * nearest-neighbour resampled to 64x64, so the emitter draws a\n"
            " * plain u 0..1 quad and tg_emit_billboard_mesh stays untouched.\n"
            " * The crop also removes a 4-row near-black atlas bleed in the\n"
            " * page's right half that is NOT index 0 and so would render as a\n"
            " * dark streak beside the sign.\n"
            " *\n"
            " * The panel is 1:2, so the billboard MUST be drawn at\n"
            " * height == 2 * width or the arrow comes out fat.\n"
            " *\n"
            " * Palettes are BGR and indices 64x64. GENERATED by\n"
            " * re/tools/gen_trackgen_r11signs_tex.py, do not edit. */\n"
            "#ifndef TD5_TG_REAL_TEX_R11SIGNS_H\n"
            "#define TD5_TG_REAL_TEX_R11SIGNS_H\n\n")
        emit_set(out, "arrow", SIGNS)
        out.write("\n#endif /* TD5_TG_REAL_TEX_R11SIGNS_H */\n")
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
