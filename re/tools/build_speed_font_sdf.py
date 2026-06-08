#!/usr/bin/env python3
"""
build_speed_font_sdf.py -- SDF for the in-race speedometer digits (VectorUI).

Rasterises a TTF (default Technology.ttf -- a 7-segment LCD digital font) digits
0-9 into a 5x2 grid of 30x48 cells at the TOP-LEFT of the HUD-font SDF page
(re/assets/static/hudfont_sdf.png), so the speed readout uses a real digital
typeface, crisp at any resolution via ps_msdf. Merges into the existing page
(FONT @ (96,192) + NUMBERS @ (0,208) regions are untouched).

Encoding matches ps_msdf.hlsl: val = 0.5 + dist_atlas_texels / 12, median(rgb).

Usage:
  python re/tools/build_speed_font_sdf.py \
      --ttf td5mod/dist/fonts/Technology.ttf \
      --out re/assets/static/hudfont_sdf.png
"""
import argparse, os
import numpy as np
from scipy import ndimage
from PIL import Image, ImageFont, ImageDraw

PAGE = 256
COLS, ROWS = 5, 2
CELL_W, CELL_H = 30, 48
RANGE = 6.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", default="td5mod/dist/fonts/Technology.ttf")
    ap.add_argument("--out", default="re/assets/static/hudfont_sdf.png")
    ap.add_argument("--size", type=int, default=46)
    args = ap.parse_args()

    font = ImageFont.truetype(args.ttf, args.size)
    # supersample the rasterisation for a smoother coverage field
    SS = 4
    cover = np.zeros((PAGE, PAGE), np.float32)
    for d in range(10):
        col, row = d % COLS, d // COLS
        cx0, cy0 = col * CELL_W, row * CELL_H
        # render the glyph big, then downscale into the cell (centred)
        big = Image.new("L", (CELL_W * SS, CELL_H * SS), 0)
        dr = ImageDraw.Draw(big)
        s = str(d)
        bb = dr.textbbox((0, 0), s, font=font)
        gw, gh = bb[2] - bb[0], bb[3] - bb[1]
        ox = (CELL_W * SS - gw) / 2 - bb[0]
        oy = (CELL_H * SS - gh) / 2 - bb[1]
        # scale the font up by SS by re-loading at SS*size
        bigfont = ImageFont.truetype(args.ttf, args.size * SS)
        bb2 = dr.textbbox((0, 0), s, font=bigfont)
        gw2, gh2 = bb2[2] - bb2[0], bb2[3] - bb2[1]
        ox = (CELL_W * SS - gw2) / 2 - bb2[0]
        oy = (CELL_H * SS - gh2) / 2 - bb2[1]
        dr.text((ox, oy), s, font=bigfont, fill=255)
        cell = np.asarray(big.resize((CELL_W, CELL_H), Image.BILINEAR), np.float32) / 255.0
        cover[cy0:cy0 + CELL_H, cx0:cx0 + CELL_W] = (cell > 0.5).astype(np.float32)

    # per-cell signed distance
    out_val = None
    if os.path.exists(args.out):
        out_val = (np.asarray(Image.open(args.out).convert("RGB"), np.float32)[:, :, 0] / 255.0)
    else:
        out_val = np.zeros((PAGE, PAGE), np.float32)

    pad = int(RANGE) + 2
    for d in range(10):
        col, row = d % COLS, d // COLS
        cx0, cy0 = col * CELL_W, row * CELL_H
        x0 = max(cx0 - pad, 0); x1 = min(cx0 + CELL_W + pad, PAGE)
        y0 = max(cy0 - pad, 0); y1 = min(cy0 + CELL_H + pad, PAGE)
        m = cover[y0:y1, x0:x1] > 0.5
        if m.all() or (~m).all():
            sd = np.full(m.shape, 8.0 if m.all() else -8.0, np.float32)
        else:
            sd = (ndimage.distance_transform_edt(m) -
                  ndimage.distance_transform_edt(~m)).astype(np.float32)
        val = np.clip(0.5 + sd / (2.0 * RANGE), 0.0, 1.0)
        out_val[cy0:cy0 + CELL_H, cx0:cx0 + CELL_W] = \
            val[(cy0 - y0):(cy0 - y0 + CELL_H), (cx0 - x0):(cx0 - x0 + CELL_W)]

    g = (out_val * 255.0 + 0.5).astype(np.uint8)
    rgba = np.zeros((PAGE, PAGE, 4), np.uint8)
    rgba[:, :, 0] = g; rgba[:, :, 1] = g; rgba[:, :, 2] = g; rgba[:, :, 3] = 255
    Image.fromarray(rgba).save(args.out)
    print(f"wrote speed digits (Technology) into {args.out}")


if __name__ == "__main__":
    main()
