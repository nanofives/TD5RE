#!/usr/bin/env python3
"""Patch the localized glyphs into the (trial) menu font for the es-AR build.

MontBlanc Trial (re/assets/frontend/menu.ttf) replaces the OUTLINES of its
accented / punctuation glyphs with a repeated "TRIAL FONT" watermark stamp, so
Spanish text renders in the arialbd fallback. This rebuilds the few glyphs the
Spanish UI actually needs, from the font's own CLEAN base glyphs:

  * ntilde / Ntilde  = n / N  +  a tilde contour transplanted from hud.ttf
                       (Rajdhani, a complete font with a clean tilde), scaled
                       and positioned above the letter.
  * exclamdown (¡)   = exclam (!)  rotated 180 deg about its bbox centre.
  * questiondown (¿) = question (?) rotated 180 deg.

Accent VOWELS (á é í ó ú …) are NOT touched here — they are folded to their base
vowel at render time (fold_accent_cp in td5_font.c), which also covers hud.ttf and
title.ttf. So this only needs to fix ñ/Ñ and ¿/¡ in the menu face.

Idempotent: re-run after tuning the constants below. Backs up the pristine trial
font to menu.ttf.trial.bak on first run. RESEARCH/BUILD tool.
"""
import os
import copy
from array import array
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates
from fontTools.ttLib.tables import ttProgram

ROOT = r"C:/Users/maria/Desktop/Proyectos/TD5RE"
MENU = ROOT + "/re/assets/frontend/menu.ttf"
HUD  = ROOT + "/re/assets/frontend/hud.ttf"
BAK  = MENU + ".trial.bak"

# --- tunables (design units, unitsPerEm=1000) -----------------------------
# tilde transplant transform:  new = (x*S + DX, y*S + DY)
NTILDE = dict(S=1.30, DX=103, DY=-220)   # over lowercase n  (n bbox 47..609, top 530)
CAPTILDE = dict(S=1.50, DX=125, DY=-150) # over uppercase N  (N bbox 55..714, top 700)


def get_contours(g):
    coords = list(g.coordinates)
    flags = list(g.flags)
    ends = list(g.endPtsOfContours)
    out, start = [], 0
    for e in ends:
        out.append((coords[start:e + 1], flags[start:e + 1]))
        start = e + 1
    return out


def make_glyph(contours):
    xy, fl, ends = [], [], []
    for cs, fs in contours:
        xy.extend(cs)
        fl.extend(fs)
        ends.append(len(xy) - 1)
    g = Glyph()
    g.numberOfContours = len(ends)
    g.coordinates = GlyphCoordinates([(int(round(x)), int(round(y))) for x, y in xy])
    g.flags = array('B', [f & 0x01 for f in fl])   # keep only on-curve bit
    g.endPtsOfContours = ends
    g.program = ttProgram.Program()
    g.program.fromBytecode(b'')
    return g


def xform(contours, S, DX, DY):
    return [([(x * S + DX, y * S + DY) for x, y in cs], fs) for cs, fs in contours]


def rotate180(g):
    cx = (g.xMin + g.xMax) / 2.0
    cy = (g.yMin + g.yMax) / 2.0
    cs = get_contours(g)
    return make_glyph([([(2 * cx - x, 2 * cy - y) for x, y in c], f) for c, f in cs])


def main():
    if not os.path.exists(BAK):
        import shutil
        shutil.copyfile(MENU, BAK)
        print("backed up pristine trial font ->", BAK)

    # Always patch from the pristine backup so re-runs with new constants are clean.
    menu = TTFont(BAK)
    hud = TTFont(HUD)
    mg, hg, hm = menu['glyf'], hud['glyf'], menu['hmtx']
    mcmap = menu.getBestCmap()

    tilde = get_contours(hg['tilde'])   # clean 1-contour tilde from Rajdhani

    # ntilde / Ntilde
    for tgt, base, t in (('ntilde', 'n', NTILDE), ('Ntilde', 'N', CAPTILDE)):
        ng = make_glyph(get_contours(mg[base]) + xform(tilde, t['S'], t['DX'], t['DY']))
        ng.recalcBounds(mg)
        mg[tgt] = ng
        hm[tgt] = (hm[base][0], ng.xMin)
        print("%-10s <- %s + tilde  bbox=(%d,%d,%d,%d) adv=%d"
              % (tgt, base, ng.xMin, ng.yMin, ng.xMax, ng.yMax, hm[tgt][0]))

    # exclamdown / questiondown (rotate the clean ! / ?)
    for tgt, cp in (('exclamdown', 0x21), ('questiondown', 0x3F)):
        src = mcmap[cp]
        rg = rotate180(mg[src])
        rg.recalcBounds(mg)
        mg[tgt] = rg
        hm[tgt] = hm[src]
        print("%-10s <- %s rot180   bbox=(%d,%d,%d,%d) adv=%d"
              % (tgt, src, rg.xMin, rg.yMin, rg.xMax, rg.yMax, hm[tgt][0]))

    menu.save(MENU)
    print("wrote", MENU)


if __name__ == "__main__":
    main()
