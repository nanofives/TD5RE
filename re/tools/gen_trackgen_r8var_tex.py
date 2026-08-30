#!/usr/bin/env python3
"""Generate td5_tg_real_tex_r8var.h -- ART BREADTH for the auto-track (R8 VARIETY,
user item G1 "more building variety ... different start/finish banners, different
guardrails").

Four sets, mined from shipped TD5 levels that the earlier passes never touched:

  LOW / TOWER  more city facades, on top of the 12 from gen_trackgen_city_tex.py
               and the 12 from gen_trackgen_r7city_tex.py. Every page here was
               checked against BOTH of those lists so nothing is mined twice.
  BANN         a SECOND start/finish banner set. The R8 survey scanned every
               shipped level for banner-shaped pages (>=30% near-white plus >=6%
               saturated red, then >=30% pale blue plus >=15% near-black) and
               found exactly two distinct word sets in the whole game: the
               red-on-white Keswick set already in td5_tg_furniture_tex.h
               (level001 337/338/369/370, duplicated verbatim into level002/005/
               030/041/044/045/046) and this one -- black serif on a pale blue
               steel ground, from the Alpine level003. Same two-page split per
               word, so it drops into the existing gantry layout unchanged.
               level013 page 222 carries a THIRD style but puts the whole word
               on ONE page, which the two-half-quad gantry cannot render, so it
               is deliberately left out.
  RAIL         real photographic guardrails. The survey scored every page for
               horizontal banding (row-to-row luminance variance over
               within-row variance > 2.5) plus a keyed band (15-65% index 0) --
               the signature of a beam-and-post rail shot against sky -- and
               these four are the distinct treatments it turned up. All are
               alpha-keyed on index 0, which is what a guardrail has to be:
               real armco is mostly air (the R6 item-13 finding).

Palettes are BGR and indices 64x64, copied verbatim from each level's
textures.src, same source form as the two earlier generators.

Usage:  python re/tools/gen_trackgen_r8var_tex.py [levels_dir]
Writes: td5mod/src/td5re/td5_tg_real_tex_r8var.h
"""

import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LEVELS = sys.argv[1] if len(sys.argv) > 1 \
    else os.path.join(ROOT, "re", "assets", "levels")
OUT = os.path.join(ROOT, "td5mod", "src", "td5re", "td5_tg_real_tex_r8var.h")

# Pages already mined, so the survey below cannot double-count them.
# gen_trackgen_city_tex.py + gen_trackgen_r7city_tex.py.
ALREADY = {
    ("level002", 64), ("level002", 92), ("level002", 133), ("level002", 363),
    ("level023", 129), ("level023", 208),
    ("level015", 60), ("level015", 111), ("level015", 149), ("level015", 11),
    ("level002", 91), ("level002", 93),
    ("level023", 128), ("level023", 209),
    ("level013", 85), ("level013", 86), ("level013", 88), ("level013", 89),
    ("level014", 64),
    ("level015", 61), ("level015", 148), ("level015", 150),
}

LOW = [
    ("level002", 61,  "San Francisco: tan stone, tall arched windows"),
    ("level002", 62,  "San Francisco: orange brick, arched sash windows"),
    ("level002", 76,  "San Francisco: pale green render, sash windows"),
    ("level002", 121, "San Francisco: cream classical, cornices"),
    ("level013", 69,  "Waikiki: balconied apartment frontage"),
    ("level023", 210, "Moscow: white stucco with dark windows"),
]
TOWER = [
    ("level013", 60,  "Waikiki: blue glass curtain wall"),
    ("level013", 66,  "Waikiki: dark blue glass tower"),
    ("level013", 87,  "Waikiki: grey office, ribbon windows"),
    ("level002", 120, "San Francisco: grey concrete pilaster tower"),
]
# ORDER IS LOAD-BEARING: START_L, START_R, FINISH_L, FINISH_R, matching the
# TD5_TG_PAGE_START_L.._FINISH_R order the gantry indexes.
BANN = [
    ("level003", 499, "Alpine: START banner, left half (black serif on pale blue)"),
    ("level003", 500, "Alpine: START banner, right half"),
    ("level003", 439, "Alpine: FINISH banner, left half"),
    ("level003", 440, "Alpine: FINISH banner, right half"),
]
# All alpha-keyed (index 0 = black = transparent). Authored with the rail
# horizontal across the page, which is the axis the roadside rail quad wants
# (u along the road, v up the face).
RAIL = [
    ("level014", 276, "Melbourne: steel W-beam armco on posts"),
    ("level018", 45,  "green timber post-and-rail (rural)"),
    ("level023", 418, "Moscow: dark steel double rail"),
    ("level013", 264, "Waikiki: white steel armco"),
]

TEXELS = 64 * 64


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
    return pal, idx


def emit_array(out, decl, data, per_line=32):
    out.write("static const unsigned char %s[%d] = {" % (decl, len(data)))
    for i, v in enumerate(data):
        out.write(("\n" if i % per_line == 0 else "") + "%d," % v)
    out.write("};\n")


def emit_set(out, name, entries):
    for i, (level, num, why) in enumerate(entries):
        pal, idx = page(level, num)
        key = idx.count(0) * 100.0 / TEXELS
        out.write("/* %s page %03d -> %s %d: %s (%d colours, key %.1f%%) */\n"
                  % (level, num, name.upper(), i, why, len(pal) // 3, key))
        out.write("static const int k_real_r8var_%s%d_paln = %d;\n"
                  % (name, i, len(pal) // 3))
        emit_array(out, "k_real_r8var_%s%d_pal" % (name, i), pal)
        emit_array(out, "k_real_r8var_%s%d_idx" % (name, i), idx)
    n = len(entries)
    out.write("static const int k_real_r8var_%s_count = %d;\n" % (name, n))
    out.write("static const int k_real_r8var_%s_paln[%d] = { %s };\n"
              % (name, n, ", ".join("k_real_r8var_%s%d_paln" % (name, i)
                                    for i in range(n))))
    for kind in ("pal", "idx"):
        out.write("static const unsigned char *const k_real_r8var_%s_%s[%d] = "
                  "{ %s };\n"
                  % (name, kind, n,
                     ", ".join("k_real_r8var_%s%d_%s" % (name, i, kind)
                               for i in range(n))))


def main():
    with open(OUT, "w", newline="\n") as out:
        out.write(
            "/* ART BREADTH pages for the auto-track (R8 VARIETY, item G1),\n"
            " * mined from shipped TD5 levels no earlier pass touched.\n"
            " *   LOW/TOWER  more city facades (on top of city + r7city)\n"
            " *   BANN       the ONE other start/finish word set in the game\n"
            " *              (level003, black serif on pale blue), in\n"
            " *              START_L, START_R, FINISH_L, FINISH_R order\n"
            " *   RAIL       four real guardrail treatments, all alpha-keyed\n"
            " * Palettes are BGR and indices 64x64, copied verbatim from each\n"
            " * level's textures.src. GENERATED by\n"
            " * re/tools/gen_trackgen_r8var_tex.py, do not edit. */\n"
            "#ifndef TD5_TG_REAL_TEX_R8VAR_H\n"
            "#define TD5_TG_REAL_TEX_R8VAR_H\n\n")
        emit_set(out, "low", LOW)
        emit_set(out, "tower", TOWER)
        emit_set(out, "bann", BANN)
        emit_set(out, "rail", RAIL)
        out.write("\n#endif /* TD5_TG_REAL_TEX_R8VAR_H */\n")
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
