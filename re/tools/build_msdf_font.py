#!/usr/bin/env python3
"""
build_msdf_font.py  -- TD5RE frontend MSDF font atlas generator.

Converts a grid bitmap font atlas (e.g. BodyText.png, 10 cols x 23 rows of
24x24 white-on-black glyph cells) into a Multi-channel Signed Distance Field
(MSDF) atlas with the SAME logical grid, so the existing frontend UV math
(u0=col/cols, v0=row/rows, ...) works verbatim -- only the texture page and
pixel shader change at draw time.

Self-contained: numpy + scipy + scikit-image + opencv (cv2) + Pillow.
No external msdfgen / potrace binary needed.

Pipeline per glyph cell:
  1. extract the source cell, build a coverage field in [0,1]
  2. trace the 0.5 iso-contour (sub-pixel) with skimage.measure.find_contours
  3. simplify each closed contour (Douglas-Peucker / cv2.approxPolyDP)
  4. edge-color the contour segments (Chlumsky "simple" coloring) so sharp
     corners are preserved by the runtime median(r,g,b)
  5. for each colour channel, signed distance = sign(inside) * min distance to
     the nearest segment carrying that channel; encode into [0,1] over a fixed
     px range
  6. pack the SxS MSDF cell into the atlas at (col*S, row*S)

Offline validation (default on): reconstruct every glyph using the exact
runtime shader math (median + smoothstep) at high resolution, tile into a
sheet, and report mean IoU vs a supersampled rasterisation of the source so we
KNOW the atlas is faithful before building the game.

Usage:
  python re/tools/build_msdf_font.py
  python re/tools/build_msdf_font.py --in re/assets/frontend/BodyText.png \
        --out re/assets/frontend/BodyText_msdf.png --cell 64 --range 6
"""
import argparse, json, os, sys
import numpy as np

try:
    import cv2
except Exception as e:
    print("ERROR: opencv (cv2) required:", e); sys.exit(1)
from skimage import measure
from PIL import Image

# ---- MSDF edge colours (bit masks over R=1,G=2,B=4) -------------------------
BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE = 0, 1, 2, 3, 4, 5, 6, 7


def switch_color(color, seed, banned=BLACK):
    """msdfgen edgeColoringSimple switchColor()."""
    combined = color & banned
    if combined in (RED, GREEN, BLUE):
        return combined ^ WHITE, seed
    if color in (BLACK, WHITE):
        start = (CYAN, MAGENTA, YELLOW)
        c = start[seed % 3]
        return c, seed // 3
    shifted = color << (1 + (seed & 1))
    c = (shifted | (shifted >> 3)) & WHITE
    return c, seed >> 1


def simplify_contour(pts, eps):
    """pts: Nx2 float (x,y). Returns simplified closed polygon Mx2 float."""
    cnt = pts.astype(np.float32).reshape(-1, 1, 2)
    approx = cv2.approxPolyDP(cnt, eps, True).reshape(-1, 2).astype(np.float64)
    return approx


def color_contour(poly, angle_thresh_deg):
    """Assign an edge colour mask to each edge of a closed polygon.
    Returns list of (p0, p1, colormask). Smooth runs share a colour; colour
    switches at every sharp corner so median(r,g,b) reconstructs the corner."""
    n = len(poly)
    if n < 2:
        return []
    # edge directions
    edges = [(poly[i], poly[(i + 1) % n]) for i in range(n)]
    dirs = []
    for a, b in edges:
        d = b - a
        L = np.hypot(d[0], d[1])
        dirs.append(d / L if L > 1e-9 else np.array([0.0, 0.0]))
    cos_th = np.cos(np.radians(angle_thresh_deg))
    # corner at vertex i = between edge i-1 and edge i
    is_corner = []
    for i in range(n):
        din = dirs[i - 1]
        dout = dirs[i]
        dot = float(np.dot(din, dout))
        is_corner.append(dot < cos_th)  # sharp turn => corner
    n_corners = sum(is_corner)
    colored = []
    if n_corners == 0:
        # smooth loop (e.g. 'O' outline): single colour is wrong for median,
        # split into two halves with two colours sharing one channel.
        col = CYAN
        for i, (a, b) in enumerate(edges):
            c = CYAN if i < n // 2 else MAGENTA
            colored.append((a, b, c))
        return colored
    # start at the first corner so spline runs are contiguous
    start = next(i for i in range(n) if is_corner[i])
    seed = 0
    color = WHITE
    color, seed = switch_color(color, seed)
    for k in range(n):
        i = (start + k) % n
        if k > 0 and is_corner[i]:
            color, seed = switch_color(color, seed)
        a, b = edges[i]
        colored.append((a, b, color))
    return colored


def seg_distance(px, py, segs):
    """Min unsigned distance from each grid point to a list of segments.
    px,py: (H,W) grids. segs: list of (x0,y0,x1,y1). Returns (H,W)."""
    if not segs:
        return np.full(px.shape, 1e9)
    best = np.full(px.shape, 1e18)
    for (x0, y0, x1, y1) in segs:
        dx, dy = x1 - x0, y1 - y0
        L2 = dx * dx + dy * dy
        if L2 < 1e-12:
            d2 = (px - x0) ** 2 + (py - y0) ** 2
        else:
            t = ((px - x0) * dx + (py - y0) * dy) / L2
            t = np.clip(t, 0.0, 1.0)
            cx = x0 + t * dx
            cy = y0 + t * dy
            d2 = (px - cx) ** 2 + (py - cy) ** 2
        np.minimum(best, d2, out=best)
    return np.sqrt(best)


def make_msdf_cell(cov, out_h, out_w, rng_units, eps, angle_thresh, blur, level, up=8):
    """cov: (cell,cell) coverage in [0,1]. Returns (S,S,3) float MSDF in [0,1].
    rng_units: distance range in *source-cell units*."""
    cell_h, cell_w = cov.shape[0], cov.shape[1]
    out = np.full((out_h, out_w, 3), 0.0, dtype=np.float64)  # deep outside
    # Upsample the coverage (cubic) before tracing so the iso-contour is smooth
    # instead of following the low-res source's pixel stairsteps -- the
    # stairsteps are what made glyph edges look lumpy/wavy at high magnification.
    # A higher-res source already has fine stairsteps, so fewer UP steps are
    # needed there (keeps the trace fast): use --up 3..4 for a 48px source.
    UP = up
    big = np.clip(cv2.resize(cov, (cell_w * UP, cell_h * UP),
                             interpolation=cv2.INTER_CUBIC), 0.0, 1.0)
    # Optional light gaussian to take stairstep waviness off the iso-contour.
    # Tuned per font: helpful for the 24px BodyText, but it DILATES the 0.5
    # contour outward, which on a 12px source fattens glyphs and closes the
    # counters of a/e/8/0 -> use --blur 0 for small/low-res fonts.
    if blur > 0.0:
        big = cv2.GaussianBlur(big, (0, 0), UP * blur)
    # Pad with a deep-outside (0) border so glyphs that touch the cell edge still
    # produce CLOSED contours. Without this, find_contours returns an OPEN
    # contour and approxPolyDP(closed=True) joins its ends with a phantom segment
    # straight across the glyph -> a spurious diagonal distance ridge.
    PAD = UP
    big = np.pad(big, PAD, mode="constant", constant_values=0.0)
    contours = measure.find_contours(big, level)
    # collect coloured segments per channel
    chan_segs = {1: [], 2: [], 4: []}  # R,G,B
    all_edges = []
    any_seg = False
    for c in contours:
        if len(c) < 3:
            continue
        # find_contours returns (row, col) == (y, x); undo PAD then /UP -> cell units
        pts = (np.stack([c[:, 1], c[:, 0]], axis=1) - PAD) / UP  # (x,y) cell units
        poly = simplify_contour(pts, eps)
        if len(poly) < 2:
            continue
        for (a, b, col) in color_contour(poly, angle_thresh):
            seg = (a[0], a[1], b[0], b[1])
            for bit in (1, 2, 4):
                if col & bit:
                    chan_segs[bit].append(seg)
            all_edges.append(seg)
            any_seg = True
    if not any_seg:
        return out  # empty glyph
    # output grid centres mapped to source-cell coords
    ys = (np.arange(out_h) + 0.5) / out_h * cell_h
    xs = (np.arange(out_w) + 0.5) / out_w * cell_w
    gx, gy = np.meshgrid(xs, ys)
    # inside test from upsampled coverage
    # Single-channel signed distance field, replicated to RGB. The renderer's
    # median(r,g,b) is then a no-op (all channels equal) -- which deliberately
    # avoids every MSDF median pathology (diagonal medial-axis seams, thin-stroke
    # cracks, holes, and re-seaming under bilinear interpolation) that a
    # hand-rolled multi-channel field is prone to. The cost is very slightly
    # rounded sharp corners (bounded by the distance range), imperceptible at the
    # 24px source's detail level. chan_segs / edge colouring are retained above
    # only to populate all_edges; the colours are intentionally ignored here.
    up = cv2.resize(cov, (out_w, out_h), interpolation=cv2.INTER_LINEAR)
    sign = np.where(up >= level, 1.0, -1.0)
    d_all = seg_distance(gx, gy, all_edges)
    true_val = np.clip(sign * d_all / (2.0 * rng_units) + 0.5, 0.0, 1.0)
    for ch in range(3):
        out[:, :, ch] = true_val
    return out


def median3(a, b, c):
    return np.maximum(np.minimum(a, b), np.minimum(np.maximum(a, b), c))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", default="re/assets/frontend/BodyText.png")
    ap.add_argument("--out", dest="out", default="re/assets/frontend/BodyText_msdf.png")
    ap.add_argument("--json", dest="js", default=None)
    ap.add_argument("--cols", type=int, default=10)
    ap.add_argument("--rows", type=int, default=23)
    ap.add_argument("--srccell", type=int, default=24)
    ap.add_argument("--cell", type=int, default=64, help="output MSDF cell px")
    ap.add_argument("--range", type=float, default=6.0,
                    help="distance range in ATLAS px (converted to src units)")
    ap.add_argument("--eps", type=float, default=0.35, help="approxPolyDP epsilon (src px)")
    ap.add_argument("--angle", type=float, default=28.0, help="corner angle threshold deg")
    ap.add_argument("--blur", type=float, default=0.55,
                    help="contour-smoothing gaussian sigma in UP units; 0 = none "
                         "(use 0 for small/low-res fonts to avoid counter-closing)")
    ap.add_argument("--level", type=float, default=0.5,
                    help="coverage iso-level to trace; >0.5 thins glyphs / opens "
                         "counters on low-res fonts (e.g. 0.6 for the 12px small font)")
    ap.add_argument("--up", type=int, default=8,
                    help="coverage upsample factor before tracing; use 3-4 for a high-res "
                         "(48px+) source to keep the trace fast (detail comes from the source)")
    ap.add_argument("--novalidate", action="store_true")
    ap.add_argument("--valout", default="re/analysis/frontend_layout/msdf_validation.png")
    ap.add_argument("--single", action="store_true",
                    help="treat the whole input image as ONE cell (title strips). "
                         "Output preserves aspect at --outh height.")
    ap.add_argument("--outh", type=int, default=96,
                    help="output height in px for --single mode")
    a = ap.parse_args()

    # ---- Single whole-image SDF (title strips: one phrase per image) ----
    if a.single:
        si = np.array(Image.open(a.inp).convert("RGBA")).astype(np.float64)
        sH, sW = si.shape[:2]
        cov = si[:, :, :3].max(axis=2) / 255.0
        if si.shape[2] == 4:
            cov = cov * (si[:, :, 3] / 255.0)
        out_h = a.outh
        out_w = max(1, int(round(sW * out_h / sH)))
        rng_units = a.range * sH / out_h  # range_px(atlas) -> source px
        cell = make_msdf_cell(cov, out_h, out_w, rng_units, a.eps, a.angle, a.blur, a.level, a.up)
        out_img = (np.clip(cell, 0, 1) * 255.0 + 0.5).astype(np.uint8)
        os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
        Image.fromarray(out_img, "RGB").save(a.out)
        print(f"Wrote {a.out}  {out_w}x{out_h}  (single, src {sW}x{sH})")
        return

    src = np.array(Image.open(a.inp).convert("RGBA")).astype(np.float64)
    H, W = src.shape[:2]
    exp_w, exp_h = a.cols * a.srccell, a.rows * a.srccell
    if (W, H) != (exp_w, exp_h):
        print(f"WARN: {a.inp} is {W}x{H}, expected {exp_w}x{exp_h} "
              f"({a.cols}x{a.rows} of {a.srccell}px). Continuing with actual grid.")
    # coverage = max(R,G,B)/255 (white glyph on black). Respect alpha if present.
    rgb = src[:, :, :3].max(axis=2) / 255.0
    if src.shape[2] == 4:
        rgb = rgb * (src[:, :, 3] / 255.0)

    S = a.cell
    rng_units = a.range * a.srccell / S  # atlas px -> source-cell units
    atlas = np.zeros((a.rows * S, a.cols * S, 3), dtype=np.float64)

    n_glyphs = a.cols * a.rows
    nonempty = 0
    for gi in range(n_glyphs):
        col = gi % a.cols
        row = gi // a.cols
        y0, x0 = row * a.srccell, col * a.srccell
        cov = rgb[y0:y0 + a.srccell, x0:x0 + a.srccell]
        if cov.shape != (a.srccell, a.srccell):
            continue
        if cov.max() < 0.25:
            continue  # blank cell (space etc.)
        cellmsdf = make_msdf_cell(cov, S, S, rng_units, a.eps, a.angle, a.blur, a.level, a.up)
        atlas[row * S:(row + 1) * S, col * S:(col + 1) * S, :] = cellmsdf
        nonempty += 1

    out_img = (np.clip(atlas, 0, 1) * 255.0 + 0.5).astype(np.uint8)
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    Image.fromarray(out_img, "RGB").save(a.out)
    print(f"Wrote {a.out}  {out_img.shape[1]}x{out_img.shape[0]}  glyphs={nonempty}")

    meta = {
        "atlas": os.path.basename(a.out),
        "cols": a.cols, "rows": a.rows,
        "src_cell": a.srccell, "out_cell": S,
        "distance_range_atlas_px": a.range,
        "distance_range_src_units": rng_units,
        "note": "Same logical grid as source bitmap font; reuse existing UV math.",
    }
    jp = a.js or (os.path.splitext(a.out)[0] + ".json")
    with open(jp, "w") as f:
        json.dump(meta, f, indent=2)
    print(f"Wrote {jp}")

    if not a.novalidate:
        validate(rgb, atlas, a, S)


def validate(src_cov, atlas, a, S):
    """Reconstruct each glyph via the runtime shader math and compare IoU."""
    R = 96  # reconstruction resolution per glyph
    sheet_cols = a.cols
    sheet = np.zeros((a.rows * R * 2, sheet_cols * R, 3), dtype=np.uint8)
    ious = []
    for gi in range(a.cols * a.rows):
        col = gi % a.cols
        row = gi // a.cols
        cy0, cx0 = row * S, col * S
        cell = atlas[cy0:cy0 + S, cx0:cx0 + S, :]
        if cell.max() <= 0.0:
            continue
        # bilinear upsample the MSDF to R, then median + smoothstep like the shader
        up = cv2.resize(cell, (R, R), interpolation=cv2.INTER_LINEAR)
        sd = median3(up[:, :, 0], up[:, :, 1], up[:, :, 2])
        # screen-space width ~ derivative; approximate with a fixed small w
        # (shader uses fwidth; for validation use the analytic slope)
        w = 0.5 * a.range * (R / S) / S  # rough; just for AA preview
        w = max(w, 1.0 / R)
        recon = np.clip((sd - (0.5 - w)) / (2 * w), 0, 1)
        recon_bin = sd >= 0.5
        # supersample the source glyph to R
        sc0y, sc0x = row * a.srccell, col * a.srccell
        srcg = src_cov[sc0y:sc0y + a.srccell, sc0x:sc0x + a.srccell]
        srcup = cv2.resize(srcg, (R, R), interpolation=cv2.INTER_LINEAR)
        src_bin = srcup >= 0.5
        inter = np.logical_and(recon_bin, src_bin).sum()
        union = np.logical_or(recon_bin, src_bin).sum()
        if union > 0:
            ious.append(inter / union)
        # build comparison tile: top = MSDF recon, bottom = source
        rr = (recon * 255).astype(np.uint8)
        sb = (np.clip(srcup, 0, 1) * 255).astype(np.uint8)
        sheet[row * R * 2:row * R * 2 + R, col * R:(col + 1) * R, :] = rr[:, :, None]
        sheet[row * R * 2 + R:row * R * 2 + 2 * R, col * R:(col + 1) * R, :] = sb[:, :, None]
    os.makedirs(os.path.dirname(a.valout) or ".", exist_ok=True)
    Image.fromarray(sheet, "RGB").save(a.valout)
    miou = float(np.mean(ious)) if ious else 0.0
    print(f"Validation: mean IoU(recon vs source) = {miou:.4f} over {len(ious)} glyphs")
    print(f"  (top row = MSDF reconstruction, bottom row = source) -> {a.valout}")
    if miou < 0.90:
        print("  WARN: IoU < 0.90 -- tune --eps/--angle/--range before shipping.")


if __name__ == "__main__":
    main()
