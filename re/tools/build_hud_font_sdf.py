#!/usr/bin/env python3
"""
build_hud_font_sdf.py -- SDF atlas for the in-race HUD font (VectorUI).

Builds a resolution-independent distance-field version of the ORIGINAL TD5 HUD
font (the FONT entry on tpage5 @ 96,192 size 160x64, a 16x4 grid of 8x12 glyphs)
so the in-race HUD text keeps its original typeface but renders crisp at any
resolution via ps_msdf.hlsl. Output keeps the SAME 256x256 page layout and glyph
texel positions as tpage5, so the runtime glyph-table UVs work verbatim -- only
the texture page + pixel shader change at draw time.

Encoding matches ps_msdf.hlsl: median(r,g,b) is the signed distance, stored as
val = 0.5 + dist_atlas_texels / 12  (PXRANGE = 2*range, range = 6), positive
INSIDE the glyph. A single channel duplicated into RGB works because the shader
takes the median of three equal values. Distance is computed PER GLYPH CELL so
neighbouring glyphs (2px apart) never bleed into each other's field.

Usage:
  python re/tools/build_hud_font_sdf.py
  python re/tools/build_hud_font_sdf.py --src re/assets/static/tpage5.dat \
        --out re/assets/static/hudfont_sdf.png
"""
import argparse, struct, sys
import numpy as np
from scipy import ndimage
from PIL import Image

PAGE = 256
# Defaults: the FONT entry on tpage5 (16x4 grid of 8x12 glyphs, 10/16 strides).
# Overridable per-font via CLI (e.g. the PAUSETXT 16x16-cell font on tpage12).
FONT_X, FONT_Y = 96, 192
COLS, ROWS = 16, 4
COL_STRIDE, ROW_STRIDE = 10, 16                      # glyph table strides
RANGE = 6.0                                          # ps_msdf: PXRANGE = 2*RANGE = 12


def load_tpage(src):
    """Load a 256x256 page as a luminance coverage array in [0,1]."""
    if src.lower().endswith(".png"):
        im = Image.open(src).convert("RGBA")
        a = np.asarray(im, dtype=np.float32)
        rgb = a[:, :, :3].mean(2)
        alpha = a[:, :, 3]
    else:
        data = np.frombuffer(open(src, "rb").read(), dtype=np.uint8)
        data = data[: PAGE * PAGE * 4].reshape(PAGE, PAGE, 4).astype(np.float32)  # BGRA
        rgb = data[:, :, :3].mean(2)
        alpha = data[:, :, 3]
    # "inside" = a lit (white) pixel that is not transparent
    cover = ((rgb > 90.0) & (alpha > 40.0)).astype(np.float32)
    return cover


def sdf_cell(mask):
    """Signed distance (texels, +inside) to the 0.5 contour of a binary mask."""
    inside = mask > 0.5
    if inside.all():
        return np.full(mask.shape, 8.0, np.float32)
    if (~inside).all():
        return np.full(mask.shape, -8.0, np.float32)
    d_out = ndimage.distance_transform_edt(~inside)   # >0 outside
    d_in = ndimage.distance_transform_edt(inside)     # >0 inside
    # signed: positive inside; subtract 0.5 so the contour sits between texels
    return (d_in - d_out).astype(np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="re/assets/static/tpage5.dat")
    ap.add_argument("--out", default="re/assets/static/hudfont_sdf.png")
    ap.add_argument("--font-x", type=int, default=FONT_X)
    ap.add_argument("--font-y", type=int, default=FONT_Y)
    ap.add_argument("--cols", type=int, default=COLS)
    ap.add_argument("--rows", type=int, default=ROWS)
    ap.add_argument("--col-stride", type=int, default=COL_STRIDE)
    ap.add_argument("--row-stride", type=int, default=ROW_STRIDE)
    ap.add_argument("--merge", action="store_true",
                    help="bake this region INTO the existing --out instead of starting blank")
    args = ap.parse_args()

    fx, fy = args.font_x, args.font_y
    cols, rows = args.cols, args.rows
    cstride, rstride = args.col_stride, args.row_stride

    cover = load_tpage(args.src)
    out_val = np.zeros((PAGE, PAGE), np.float32)  # 0 => far outside
    import os
    if args.merge and os.path.exists(args.out):
        out_val = (np.asarray(Image.open(args.out).convert("RGB"),
                              dtype=np.float32)[:, :, 0] / 255.0)

    pad = int(RANGE) + 2
    for row in range(rows):
        for col in range(cols):
            # full stride cell so the field has room, clamped to the page
            cx0 = fx + col * cstride
            cy0 = fy + row * rstride
            x0 = max(cx0 - pad, 0); x1 = min(cx0 + cstride + pad, PAGE)
            y0 = max(cy0 - pad, 0); y1 = min(cy0 + rstride + pad, PAGE)
            cell = cover[y0:y1, x0:x1]
            sd = sdf_cell(cell)
            val = np.clip(0.5 + sd / (2.0 * RANGE), 0.0, 1.0)
            # write back only the inner (non-pad) stride region to avoid overlap
            wx0, wy0 = cx0, cy0
            wx1 = min(cx0 + cstride, PAGE); wy1 = min(cy0 + rstride, PAGE)
            out_val[wy0:wy1, wx0:wx1] = val[(wy0 - y0):(wy1 - y0), (wx0 - x0):(wx1 - x0)]

    g = (out_val * 255.0 + 0.5).astype(np.uint8)
    rgba = np.zeros((PAGE, PAGE, 4), np.uint8)
    rgba[:, :, 0] = g; rgba[:, :, 1] = g; rgba[:, :, 2] = g; rgba[:, :, 3] = 255
    Image.fromarray(rgba, "RGBA").save(args.out)
    fh = min(rows * rstride, PAGE - fy)
    fw = min(cols * cstride, PAGE - fx)
    inside_px = int((cover[fy:fy + fh, fx:fx + fw] > 0.5).sum())
    print(f"wrote {args.out}  (font coverage px={inside_px})")


if __name__ == "__main__":
    main()
