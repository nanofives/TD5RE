#!/usr/bin/env python3
"""
build_bodytext_bitmap.py  --  TD5RE main-menu font swap.

Rasterises a TrueType face into the SAME 10-col x 23-row grid the original
BodyText.tga used, so the frontend UV math (col/row -> source rect) and the
auto-measured advance table keep working verbatim. Only the glyph shapes change.

Two output uses, selected by --cellpx:
  * --cellpx 24  -> the RUNTIME bitmap atlas (re/assets/frontend/BodyText.png).
    The C side (frontend_init_font_metrics_from_pixels) auto-measures advances
    from this 24px grid, so it MUST stay 24px. Also feeds the VectorUI-off path.
  * --cellpx 48 (or higher) -> a high-resolution SOURCE for build_msdf_font.py.
    The default frontend (VectorUI=1) renders text from the MSDF atlas; tracing
    the distance field from a 2x source gives noticeably crisper / more even
    glyphs at the small on-screen sizes than tracing from the 24px grid.

Quality: every glyph is rendered SUPERSAMPLED (--ss) and box/LANCZOS downscaled,
which removes hinting/grid-fit unevenness and yields smooth coverage.

Output format matches the extracted original: RGBA, white glyph (R=G=B=255) with
coverage in ALPHA, transparent (0,0,0,0) background. build_msdf_font.py reads
coverage = max(R,G,B)/255 * alpha/255, so one PNG feeds both paths.

Vertical placement: --baseline puts the glyph baseline at that cell row (in
24px-cell units). The original art sits caps at rows ~8..23, which centers them
in the 32px button (fe_draw_text draws the 24px cell from the button TOP).

Trial fonts: faces like MontBlanc Trial stamp the symbol glyphs they withhold
with an identical "TRIAL TEXT" watermark across many codepoints. Those (and any
truly-missing glyph) are detected and sourced from --fallback (the original
atlas) so coverage/quality never regress; real letters+digits stay.

Usage (the two runs the font swap needs):
  python re/tools/build_bodytext_bitmap.py --font <ttf> --size 21 --baseline 23 \
      --margin 0 --padding 1 --cellpx 24 --ss 4 \
      --fallback re/assets/frontend/BodyText.orig.png \
      --out re/assets/frontend/BodyText.png --advout re/analysis/_adv.txt
  python re/tools/build_bodytext_bitmap.py --font <ttf> --size 21 --baseline 23 \
      --margin 0 --padding 1 --cellpx 48 --ss 3 \
      --fallback re/assets/frontend/BodyText.orig.png \
      --out re/assets/frontend/BodyText_src.png      # MSDF source only
"""
import argparse, sys
from PIL import Image, ImageDraw, ImageFont

COLS = 10
ROWS = 23
BASE_CELL = 24                 # logical cell (px units the C side assumes)
ADV_MIN = 4


def render_cell(font_ss, ch, cellpx, baseline, margin, xscale, ss):
    """Return a (cellpx x cellpx) 'L' coverage tile for one character.
    Rendered at ss-supersample then downscaled. `baseline`/`margin` are in
    OUTPUT (cellpx) units; ink-left -> margin, baseline -> row `baseline`."""
    blank = Image.new("L", (cellpx, cellpx), 0)
    if ch == " " or ord(ch) == 0x7F:        # space + DEL: non-printing
        return blank
    bbox = font_ss.getbbox(ch)              # supersampled px
    if bbox is None or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
        return blank                        # face lacks a real glyph
    cs = cellpx * ss
    pad = 16 * ss
    TW, TH = cs + 2 * pad, cs + 2 * pad
    BASE = TH - pad
    tmp = Image.new("L", (TW, TH), 0)
    ImageDraw.Draw(tmp).text((-bbox[0], BASE), ch, font=font_ss, fill=255, anchor="ls")
    if abs(xscale - 1.0) > 1e-3:
        tmp = tmp.resize((max(1, int(round(TW * xscale))), TH), Image.LANCZOS)
    superc = Image.new("L", (cs, cs), 0)
    superc.paste(tmp, (margin * ss, baseline * ss - BASE))
    return superc.resize((cellpx, cellpx), Image.LANCZOS)


def measure_advance(tile, ch, cellpx, pad_px):
    """Mirror frontend_init_font_metrics_from_pixels, in cellpx units."""
    if ch == " ":
        return round(8 * cellpx / BASE_CELL)
    px = tile.load()
    last = -1
    for x in range(cellpx):
        for y in range(cellpx):
            if px[x, y] != 0:
                last = x
                break
    if last < 0:
        return None
    adv = last + 1 + pad_px
    return max(ADV_MIN, min(cellpx, adv))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default=r"C:/Windows/Fonts/bahnschrift.ttf")
    ap.add_argument("--variation", default="")
    ap.add_argument("--size", type=float, default=21, help="font size in 24px-cell units")
    ap.add_argument("--baseline", type=float, default=23, help="baseline cell-row (24px units)")
    ap.add_argument("--margin", type=float, default=0, help="left bearing (24px units)")
    ap.add_argument("--padding", type=float, default=1, help="trailing advance pad (24px units)")
    ap.add_argument("--xscale", type=float, default=1.0, help="horizontal condense (<1)")
    ap.add_argument("--cellpx", type=int, default=24, help="output cell px (24=runtime, 48=MSDF source)")
    ap.add_argument("--ss", type=int, default=4, help="supersample factor for AA quality")
    ap.add_argument("--out", default="re/assets/frontend/BodyText.png")
    ap.add_argument("--advout", default=None, help="write measured advance table (24px units)")
    ap.add_argument("--fallback", default=None,
                    help="atlas to source watermarked/missing glyphs from (e.g. original BodyText)")
    a = ap.parse_args()

    scale = a.cellpx / BASE_CELL
    font_px = max(1, int(round(a.size * scale * a.ss)))
    font_ss = ImageFont.truetype(a.font, font_px)
    if a.variation:
        try:
            font_ss.set_variation_by_name(a.variation)
        except Exception as e:
            print(f"WARN: variation '{a.variation}' not set: {e}", file=sys.stderr)

    cell = a.cellpx
    baseline_o = int(round(a.baseline * scale))
    margin_o = int(round(a.margin * scale))
    pad_o = int(round(a.padding * scale))

    atlas = Image.new("RGBA", (COLS * cell, ROWS * cell), (0, 0, 0, 0))
    advances = [None] * 96
    white = Image.new("RGBA", (cell, cell), (255, 255, 255, 0))
    fallback = Image.open(a.fallback).convert("RGBA") if a.fallback else None
    fb_used = []

    # Phase 1 -- render every glyph tile (supersampled, downscaled).
    tiles = [render_cell(font_ss, chr(0x20 + gi), cell, baseline_o, margin_o, a.xscale, a.ss)
             for gi in range(96)]
    sigs = [t.tobytes() for t in tiles]

    # Phase 2 -- TRIAL-FONT WATERMARK DETECTION: a real face has a unique bitmap
    # per char, so any non-blank tile whose exact pixels repeat across >=3
    # codepoints is the withheld-glyph watermark -> fall back. >=3 avoids ever
    # flagging an incidental real-glyph pair.
    from collections import Counter
    counts = Counter(sigs[gi] for gi in range(96) if tiles[gi].getbbox() is not None)
    watermark = {s for s, c in counts.items() if c >= 3}

    # Phase 3 -- compose.
    for gi in range(96):
        ch = chr(0x20 + gi)
        col, row = gi % COLS, gi // COLS
        tile = tiles[gi]
        blank = tile.getbbox() is None
        is_wm = sigs[gi] in watermark
        if (blank or is_wm) and ch not in (" ", chr(0x7F)) and fallback is not None:
            fb = fallback.crop((col * BASE_CELL, row * BASE_CELL,
                                (col + 1) * BASE_CELL, (row + 1) * BASE_CELL))
            if cell != BASE_CELL:
                fb = fb.resize((cell, cell), Image.LANCZOS)
            if fb.getchannel("A").getbbox() is not None:
                atlas.paste(fb, (col * cell, row * cell))
                advances[gi] = measure_advance(fb.getchannel("A"), ch, cell, pad_o)
                fb_used.append(ch)
                continue
        advances[gi] = measure_advance(tile, ch, cell, pad_o)
        cell_rgba = white.copy()
        cell_rgba.putalpha(tile)
        atlas.paste(cell_rgba, (col * cell, row * cell))

    atlas.save(a.out)
    nonblank = sum(1 for v in advances if v is not None)
    print(f"Wrote {a.out}  {atlas.width}x{atlas.height}  cellpx={cell} ss={a.ss} "
          f"inked={nonblank}/96")
    if fb_used:
        print(f"fallback (missing/trial-watermark, n={len(fb_used)}): {''.join(fb_used)}")

    # Advance table in 24px units (only meaningful / written for the 24px run).
    table = []
    for v in advances:
        if v is None:
            table.append(8)
        else:
            table.append(max(ADV_MIN, min(BASE_CELL, int(round(v * BASE_CELL / cell)))))
    body = "\n".join("   " + "".join(f"{table[r + i]:3d}," for i in range(12))
                     for r in range(0, 96, 12))
    print("---- advance table (ascii 0x20..0x7F, 24px units) ----")
    print(body)
    if a.advout:
        with open(a.advout, "w") as f:
            f.write(body + "\n")
        print(f"Wrote {a.advout}")


if __name__ == "__main__":
    main()
