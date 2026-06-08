#!/usr/bin/env python3
"""Convert a CFF/OTF font to a glyf/TTF that stb_truetype can load.

Used to bring re/assets/frontend/LunaticaDEMO-Display.otf into the frontend's
runtime TTF font system (td5_font.c expects TrueType glyf outlines).
"""
import sys
from fontTools.ttLib import TTFont, newTable
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.ttGlyphPen import TTGlyphPen

MAX_ERR = 1.0
REVERSE = True


def glyphs_to_quadratic(glyphs, max_err=MAX_ERR, reverse=REVERSE):
    quad = {}
    for name in glyphs.keys():
        pen = TTGlyphPen(glyphs)
        glyphs[name].draw(Cu2QuPen(pen, max_err, reverse_direction=reverse))
        quad[name] = pen.glyph()
    return quad


def otf_to_ttf(font):
    assert font.sfntVersion == "OTTO", "input is not a CFF/OTF font"
    assert "CFF " in font, "no CFF table"

    order = font.getGlyphOrder()
    font["loca"] = newTable("loca")
    font["glyf"] = glyf = newTable("glyf")
    glyf.glyphOrder = order
    glyf.glyphs = glyphs_to_quadratic(font.getGlyphSet())
    del font["CFF "]
    if "VORG" in font:
        del font["VORG"]
    glyf.compile(font)

    # maxp must become v1.0 so glyf-derived fields (maxPoints etc.) get written.
    # glyf.compile() recalculates maxPoints/maxContours/maxComponent*, but the
    # hinting-related v1.0 fields have no defaults — set them (stb_truetype
    # ignores them; any sane value works for a non-hinted font).
    maxp = font["maxp"]
    maxp.tableVersion = 0x00010000
    maxp.maxZones = 1
    maxp.maxTwilightPoints = 0
    maxp.maxStorage = 0
    maxp.maxFunctionDefs = 0
    maxp.maxInstructionDefs = 0
    maxp.maxStackElements = 0
    maxp.maxSizeOfInstructions = 0
    maxp.maxComponentElements = 0
    maxp.maxComponentDepth = 0

    post = font["post"]
    post.formatType = 2.0
    post.extraNames = []
    post.mapping = {}
    post.glyphOrder = order

    font.sfntVersion = "\x00\x01\x00\x00"


def main():
    inp, outp = sys.argv[1], sys.argv[2]
    font = TTFont(inp)
    otf_to_ttf(font)
    font.save(outp)

    # Verify the result reloads and has the tables stb_truetype needs.
    chk = TTFont(outp)
    need = ["glyf", "loca", "cmap", "head", "hhea", "hmtx", "maxp"]
    missing = [t for t in need if t not in chk]
    cmap = chk.getBestCmap()
    print("saved:", outp)
    print("sfntVersion:", chk.sfntVersion)
    print("numGlyphs:", chk["maxp"].numGlyphs)
    print("cmap entries:", len(cmap))
    has_AZ = all(ord(c) in cmap for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ")
    print("has A-Z:", has_AZ)
    print("missing tables:", missing or "none")
    if missing or not has_AZ:
        sys.exit("CONVERSION FAILED — incomplete font")


if __name__ == "__main__":
    main()
