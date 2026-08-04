#!/usr/bin/env python3
"""
bake_car_material_mask.py  [CAR REFL 2026-08-04]

Offline "car material mask" baker. For each cars/<code>/carskin<N>.png it writes
carmatmask<N>.png whose RED channel encodes a per-texel MATERIAL ID that the
runtime samples to pick reflectivity per car part:

    matid 1 (DEFAULT) = body paint   -> low reflectivity (subtle sheen)
    matid 3 (GLASS)   = windows/glass -> strong, mirror-like reflection
    matid 4 (GLOW)    = lights        -> emissive, no reflection

"Recognition" is classical CV, not a neural net (no training data / no in-engine
inference / no model downloads needed): the carmask.png paint mask separates
body from non-body where present, then color analysis + connected-components
splits the rest into glass (large dark, ~neutral or tinted blobs) and lights
(saturated red/orange blobs). A *_preview.png (colour-coded) is written alongside
so the classification can be eyeballed.

Usage:
    python bake_car_material_mask.py            # all cars under re/assets/cars
    python bake_car_material_mask.py vip        # one car code
    python bake_car_material_mask.py vip --preview-only
"""
import os, sys, glob
import numpy as np
from PIL import Image
from scipy import ndimage

# TD5_MAT_* ids (mirror td5_material.h)
MAT_BODY   = 1   # DEFAULT  (refl 0.10)
MAT_GLASS  = 3   # GLASS    (refl 0.60)
MAT_LIGHT  = 4   # GLOW     (refl 0.00, emissive)

CARS_DIR = os.path.join(os.path.dirname(__file__), "..", "assets", "cars")

def load_rgb(path):
    im = Image.open(path).convert("RGB")
    return np.asarray(im, dtype=np.float32) / 255.0

def load_gray(path):
    im = Image.open(path).convert("L")
    return np.asarray(im, dtype=np.float32) / 255.0

def derive_body_mask(variants, carmask):
    """Body(paint) mask. Ground-truth from carmask.png if present; else derived
    from per-texel colour VARIANCE across the paint variants (the body is
    repainted per variant; glass/lights/chrome are identical in all), which is
    what makes this robust to red / black / dark cars where colour alone fails."""
    h, w, _ = variants[0].shape
    if carmask is not None:
        cm = carmask
        if cm.shape != (h, w):
            cm = np.asarray(Image.fromarray((carmask * 255).astype(np.uint8))
                            .resize((w, h), Image.NEAREST), np.float32) / 255.0
        return cm > 0.5, "carmask"
    if len(variants) >= 2:
        stack = np.stack(variants, axis=0)              # K x H x W x 3
        rng = (stack.max(0) - stack.min(0)).mean(axis=-1)   # per-texel colour range
        return rng > 0.12, "variant-variance"
    return None, "none"

def classify(skin, body_mask):
    """skin: HxWx3 [0,1]; body_mask: HxW bool or None. Returns HxW uint8 matid.
    Glass/lights are sub-classified only OUTSIDE the body (or everywhere if no
    body mask)."""
    h, w, _ = skin.shape
    r, g, b = skin[..., 0], skin[..., 1], skin[..., 2]
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    mx  = np.maximum(np.maximum(r, g), b)
    mn  = np.minimum(np.minimum(r, g), b)
    sat = (mx - mn) / np.maximum(mx, 1e-3)

    non_body = (~body_mask) if body_mask is not None else np.ones((h, w), bool)

    # Lights: saturated red/orange, in the non-body region.
    lights = non_body & (r > 0.30) & (r > g * 1.5) & (r > b * 1.5) & (sat > 0.35)

    # Glass via SEED + REGION-GROW so a window that shades from dark-tinted down
    # to near-black is captured WHOLE, without pulling in matte-black tyres/trim
    # or the pure-black atlas background:
    #   * dark  = the full connected dark region a window can occupy. Floor 0.015
    #             keeps near-black GLASS but drops the pure-black background/gaps.
    #   * seed  = the CONFIDENT part (dark but not black, >0.04), window-sized —
    #             this is what identifies "this dark blob is a window".
    #   * glass = every `dark` component that CONTAINS a seed (grows the window to
    #             its full extent, incl. its black lower rim). Isolated black
    #             tyres/trim have no seed -> excluded.
    dark = non_body & (lum >= 0.004) & (lum < 0.35) & (sat < 0.55) & (~lights)
    # Close thin body seams so a window split into sub-panes (e.g. a rear window
    # with a near-black lower band divided by a pillar strip) merges into ONE
    # component that the seed can claim.
    dark = ndimage.binary_closing(dark, structure=np.ones((5, 5)))
    dark &= non_body
    seed = ndimage.binary_opening(dark & (lum > 0.04), structure=np.ones((3, 3)))
    lbl_s, ns = ndimage.label(seed)
    seed_big = np.zeros_like(seed)
    if ns > 0:
        min_area = max(120, int(0.003 * h * w))   # >=0.3% of the atlas
        sizes = ndimage.sum(np.ones_like(lbl_s), lbl_s, index=np.arange(1, ns + 1))
        for i, s in enumerate(sizes, start=1):
            if s >= min_area:
                seed_big |= (lbl_s == i)

    glass = np.zeros_like(dark)
    lbl_d, nd = ndimage.label(dark)
    if nd > 0 and seed_big.any():
        keep = np.unique(lbl_d[seed_big])          # dark components holding a seed
        keep = keep[keep != 0]
        glass = np.isin(lbl_d, keep)
    # Solid window: close small notches + fill interior highlight holes.
    glass = ndimage.binary_closing(glass, structure=np.ones((5, 5)))
    glass = ndimage.binary_fill_holes(glass)
    glass &= non_body
    # Bounded edge-grow: extend the window a few px into ADJACENT DARK non-body
    # texels only (its near-black lower rim), never into the bright body — the
    # seeded region-grow can stop at a faint luma dip inside a window.
    dark_edge = non_body & (lum < 0.12) & (~lights)   # near-black window rim only
    for _ in range(3):
        # ADD adjacent near-black rim texels (keep the window interior).
        glass = glass | (ndimage.binary_dilation(glass, structure=np.ones((3, 3))) & dark_edge)
    glass = ndimage.binary_fill_holes(glass)

    matid = np.full((h, w), MAT_BODY, dtype=np.uint8)
    matid[glass]  = MAT_GLASS
    matid[lights] = MAT_LIGHT
    return matid

PREVIEW_COLORS = {
    MAT_BODY:  (40, 40, 90),     # dark blue  = body paint
    MAT_GLASS: (0, 220, 220),    # cyan       = glass/windows
    MAT_LIGHT: (230, 30, 30),    # red        = lights
}

def preview(matid):
    h, w = matid.shape
    out = np.zeros((h, w, 3), np.uint8)
    for mid, col in PREVIEW_COLORS.items():
        out[matid == mid] = col
    return out

def bake_car(code, preview_only=False):
    d = os.path.join(CARS_DIR, code)
    skins = sorted(glob.glob(os.path.join(d, "carskin*.png")))
    if not skins:
        return 0
    carmask_path = os.path.join(d, "carmask.png")
    carmask = load_gray(carmask_path) if os.path.exists(carmask_path) else None
    variants = [load_rgb(p) for p in skins]
    body_mask, src = derive_body_mask(variants, carmask)
    # Parts (glass/lights) sit at identical UVs in every paint variant, so ONE
    # mask serves all skins. Classify on variant 0. Sub-classification reads its
    # colours, but only in the (paint-independent) non-body region.
    matid = classify(variants[0], body_mask)
    # Degenerate guard: if the split is implausible (variant-variance failed —
    # e.g. near-identical variants, or an all-red/all-dark skin), a huge glass or
    # lights fraction would render the whole car wrong. Fall back to uniform body
    # (safe: reflects like the old single-CARBODY look, no per-texel claim).
    gfrac = (matid == MAT_GLASS).mean(); lfrac = (matid == MAT_LIGHT).mean()
    degenerate = (gfrac > 0.25) or (lfrac > 0.15)
    if degenerate:
        matid[:] = MAT_BODY
    Image.fromarray(preview(matid)).save(os.path.join(d, "carmatmask0_preview.png"))
    if not preview_only:
        # matid replicated to R=G=B so the runtime can read it from any channel
        # regardless of the PNG loader's R<->B (BGRA) swap; alpha=255.
        m = np.zeros((*matid.shape, 3), np.uint8)
        m[..., 0] = matid; m[..., 1] = matid; m[..., 2] = matid
        Image.fromarray(m).save(os.path.join(d, "carmatmask0.png"))
    gl = int((matid == MAT_GLASS).sum()); li = int((matid == MAT_LIGHT).sum())
    tot = matid.size
    print(f"  {code}: skins={len(skins)} body_src={src} "
          f"glass={gl/tot*100:.1f}% lights={li/tot*100:.1f}%"
          f"{'  [DEGENERATE->all-body]' if degenerate else ''}")
    return 1

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    preview_only = "--preview-only" in sys.argv
    if args:
        codes = args
    else:
        codes = sorted(os.path.basename(p) for p in glob.glob(os.path.join(CARS_DIR, "*"))
                       if os.path.isdir(p))
    total = 0
    for code in codes:
        total += bake_car(code, preview_only)
    print(f"baked {total} skin(s)")

if __name__ == "__main__":
    main()
