#!/usr/bin/env python3
"""
bake_td5_car_paint.py  [TD5 CAR PAINT 2026-09-29]

Offline baker that makes an ORIGINAL (TD5) car repaintable with the free colour
picker the ported TD6 cars already use.

TD6 cars ship a grayscale body + carmask.png, so the runtime multiplies the
masked body texels by the chosen colour. TD5 cars instead ship FOUR pre-painted
skins (carskin0..3) and no mask, so there is nothing to tint. This tool
reconstructs both from the four skins:

    carmask.png         body(primary paint) mask, 255/0 replicated R=G=B
    carskinpaint0.png   neutral GREYSCALE body + every non-body texel verbatim
    carpicpaint0.png    same thing in carpic space, RGBA (alpha = body mask),
                        the menu-preview overlay the frontend draws MODULATEd
                        by the paint colour over the base carpic
    carpaint_preview.png  eyeball sheet (base | classification | repainted blue)

How the body is found (no ML, no training data — it is all in the four skins):

    Only the PAINT changes between carskin0..3; glass, lights, chrome, tyres,
    decals and badges are identical in all four. So a texel's "signature" —
    its colour in all K variants stacked into one K*3 vector — tells us what it
    is. Body texels differ only by a SHADING SCALE, so they all point in the
    same direction in that space; a white stripe that stays white while the body
    goes red/black/blue points somewhere else entirely.

    1. varying   = per-texel colour range across the variants is above a floor
                   (constant texels are not paint).
    2. The uniform axis (equal in every variant and channel) is projected out.
       That axis is the specular highlight / ambient white, which otherwise
       drags a glossy body texel away from its own hue.
    3. Spherical k-means over the remaining direction, weighted by magnitude.
       The LARGEST cluster is the primary body; any other cluster (a second
       body colour, stripes, a two-tone roof) is left alone, which is what
       "recolour the primary colour only" means here.
    4. The body mask is then grown a few texels into adjacent DARK varying
       texels — a shaded silhouette rim carries almost no hue, so step 3 cannot
       classify it, and leaving it out draws the old paint as an outline.

The neutral body is the signature magnitude, rescaled so its 99th percentile
lands on the TD6 grayscale body top (225/255) — that way `grey * colour` lands
a TD5 car at the same brightness as a ported TD6 car painted the same colour.

A car whose four skins are identical (the special / hot-rod / police cars ship
one paint four times) produces nothing and stays non-paintable at runtime; the
runtime predicate is simply "carmask.png AND carskinpaint0.png both exist".

Usage:
    python bake_td5_car_paint.py                # every car under re/assets/cars
    python bake_td5_car_paint.py cob vip        # named car codes
    python bake_td5_car_paint.py --dry-run      # classify + report, write nothing
    python bake_td5_car_paint.py --preview      # also write carpaint_preview.png
"""
import os, sys, glob
import numpy as np
from PIL import Image
from scipy import ndimage

CARS_DIR = os.path.join(os.path.dirname(__file__), "..", "assets", "cars")

TD6_BODY_TOP   = 0.882   # TD6 grayscale body max (225/255) — match it
VAR_THR        = 0.10    # per-texel colour range across variants
CHROMA_FLOOR   = 0.05    # |signature| after removing the white axis
KMEANS_K       = 4
MERGE_COS      = 0.80    # fold a cluster into the body when this close
DARK_GROW      = 12      # iterations of "grow into dark varying neighbours"
DARK_LEVEL     = 0.35
MIN_BODY_FRAC  = 0.15    # of the OPAQUE area (a carpic is mostly background)
MIN_BLOB_FRAC  = 0.80    # a real body is a few big blobs, not speckle


def load_rgba(path):
    a = np.asarray(Image.open(path).convert("RGBA"), np.float32) / 255.0
    return a[..., :3], a[..., 3]


def spherical_kmeans(X, w, k, iters=25, seed=0):
    """Unit-vector k-means, weighted by w. Deterministic (fixed seed) so a
    re-bake of the same art reproduces the same mask."""
    rng = np.random.default_rng(seed)
    C = X[rng.choice(len(X), k, replace=False, p=w / w.sum())].copy()
    for _ in range(iters):
        lab = np.argmax(X @ C.T, 1)
        for j in range(k):
            m = lab == j
            if not m.any():
                C[j] = X[rng.integers(len(X))]
                continue
            v = (X[m] * w[m][:, None]).sum(0)
            n = np.linalg.norm(v)
            if n > 1e-9:
                C[j] = v / n
    return C, np.argmax(X @ C.T, 1)


def derive_primary_body(paths):
    """Primary-paint mask + neutral greyscale shade from K pre-painted variants.

    Returns dict(base_rgb, alpha, varying, primary, shade) or None when the
    variants carry no paint difference (car is not repaintable)."""
    rgb, alpha = zip(*[load_rgba(p) for p in paths])
    S = np.stack(rgb, 0)                                  # K,H,W,3
    A = np.stack(alpha, 0).min(0) > 0.5                   # opaque in every variant
    K, H, W, _ = S.shape

    rng_ = (S.max(0) - S.min(0)).mean(-1)
    varying = (rng_ > VAR_THR) & A
    if varying.sum() < 256:
        return None                                       # identical skins

    F = S.transpose(1, 2, 0, 3).reshape(H, W, K * 3)      # per-texel signature
    white = np.ones(K * 3, np.float32) / np.sqrt(K * 3.0)
    Fp = F - (F @ white)[..., None] * white               # drop specular/ambient
    mag = np.linalg.norm(Fp, axis=-1)
    U = Fp / np.maximum(mag, 1e-6)[..., None]

    cand = varying & (mag > CHROMA_FLOOR)
    if cand.sum() < 256:
        return None
    k = min(KMEANS_K, max(2, int(cand.sum() // 128)))
    C, lab = spherical_kmeans(U[cand], mag[cand], k)
    areas = np.array([(lab == j).sum() for j in range(k)])
    dom = int(np.argmax(areas))
    keep = [j for j in range(k) if float(C[j] @ C[dom]) > MERGE_COS]

    primary = np.zeros((H, W), bool)
    primary[cand] = np.isin(lab, keep)

    # Grow into the shaded silhouette rim: dark texels hold no usable hue, so
    # the clustering cannot claim them, but leaving them behind outlines the car
    # in its OLD paint. Restricted to `varying` texels so a (constant) tyre or
    # shadow never gets pulled in.
    dark = varying & (S.max(-1).max(0) < DARK_LEVEL)
    for _ in range(DARK_GROW):
        primary |= ndimage.binary_dilation(primary, np.ones((3, 3))) & dark

    # Reject speckle. A car whose four "variants" are the same paint with only
    # compression noise between them (ss1, the Shelby Series 1) still produces
    # scattered `varying` texels, and those cluster into a mask that looks like
    # static sprayed over the whole car. A genuine body mask is a handful of big
    # blobs — one per UV chart, or one per stacked view in carpic space — so
    # score the share of the mask held by components >=5% of it. Measured over
    # the shipped cars: every real body scores >=0.86, ss1 scores 0.76 on its
    # skin and 0.44 on its carpic.
    lbl, ncc = ndimage.label(primary)
    if ncc == 0:
        return None
    sizes = ndimage.sum(np.ones_like(lbl), lbl, np.arange(1, ncc + 1))
    total = primary.sum()
    blob_frac = sizes[sizes >= 0.05 * total].sum() / total
    if primary.sum() / max(A.sum(), 1) < MIN_BODY_FRAC or blob_frac < MIN_BLOB_FRAC:
        return None

    shade = np.linalg.norm(F, axis=-1)
    p99 = float(np.percentile(shade[primary], 99))
    shade = np.clip(shade * (TD6_BODY_TOP / max(p99, 1e-3)), 0.0, 1.0)
    return dict(base=S[0], alpha=np.stack(alpha, 0)[0], varying=varying,
                primary=primary, shade=shade, areas=areas.tolist(), kept=len(keep))


def u8(a):
    return np.clip(a * 255.0 + 0.5, 0, 255).astype(np.uint8)


def clear_bake(d, dry_run):
    """Drop a previous bake so a car that no longer qualifies stops being
    paintable. carmask.png is only removed when OUR carskinpaint0.png sits next
    to it — a ported TD6 car's hand-made carmask must never be touched."""
    if dry_run:
        return
    ours = os.path.exists(os.path.join(d, "carskinpaint0.png"))
    names = ["carskinpaint0.png", "carpicpaint0.png", "carpaint_preview.png"]
    if ours:
        names.append("carmask.png")
    for n in names:
        p = os.path.join(d, n)
        if os.path.exists(p):
            os.remove(p)


def bake_car(code, dry_run=False, want_preview=False):
    d = os.path.join(CARS_DIR, code)
    skins = sorted(glob.glob(os.path.join(d, "carskin?.png")))
    if len(skins) < 2:
        return None
    r = derive_primary_body(skins)
    if r is None:
        print(f"  {code}: skins carry no paint variation -> NOT paintable")
        clear_bake(d, dry_run)
        return None

    # carpic space (the menu preview) gets its own classification: the preview
    # renders are a different projection, so the skin's texel mask does not map.
    # Both have to succeed — a car painted in-race but showing its old paint in
    # the car-select preview reads as a bug, so it stays non-paintable instead.
    pics = sorted(glob.glob(os.path.join(d, "carpic?.png")))
    rp = derive_primary_body(pics) if len(pics) >= 2 else None
    if rp is None:
        print(f"  {code}: no usable carpic body -> NOT paintable")
        clear_bake(d, dry_run)
        return None

    pm, sh = r["primary"], r["shade"]
    neutral = r["base"].copy()
    neutral[pm] = sh[pm][:, None]                        # body -> greyscale
    mask = np.zeros(pm.shape + (3,), np.uint8)
    mask[pm] = 255

    print(f"  {code}: body={pm.mean()*100:5.1f}% of atlas  "
          f"(varying {r['varying'].mean()*100:4.1f}%, clusters kept {r['kept']})"
          f"   carpic body={rp['primary'].mean()*100:5.1f}%")

    if not dry_run:
        # Neutral skin FIRST: the "already baked / hand-made TD6 mask" skip below
        # keys on carmask.png, so writing that last keeps an interrupted bake
        # re-runnable instead of permanently skipped.
        Image.fromarray(u8(neutral)).save(os.path.join(d, "carskinpaint0.png"))
        Image.fromarray(mask).save(os.path.join(d, "carmask.png"))
        if rp is not None:
            ov = np.zeros(rp["primary"].shape + (4,), np.uint8)
            ov[..., :3] = u8(np.repeat(rp["shade"][..., None], 3, -1))
            ov[..., 3] = np.where(rp["primary"], 255, 0)
            ov[~rp["primary"], :3] = 0
            Image.fromarray(ov, "RGBA").save(os.path.join(d, "carpicpaint0.png"))

    if want_preview:
        src = rp if rp is not None else r
        base = u8(src["base"])
        cls = base.copy()
        cls[src["primary"]] = (0, 255, 0)
        cls[src["varying"] & ~src["primary"]] = (255, 0, 255)
        tinted = base.astype(np.float32) / 255.0
        tinted[src["primary"]] = (src["shade"][src["primary"]][:, None]
                                  * np.array((0.15, 0.40, 1.0), np.float32))
        Image.fromarray(np.concatenate([base, cls, u8(tinted)], 1)).save(
            os.path.join(d, "carpaint_preview.png"))
    return True


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    prev = "--preview" in sys.argv
    codes = args or sorted(os.path.basename(p)
                           for p in glob.glob(os.path.join(CARS_DIR, "*"))
                           if os.path.isdir(p))
    n = 0
    for c in codes:
        # A ported TD6 car already ships its own hand-made carmask + grayscale
        # skin; never overwrite those.
        if os.path.exists(os.path.join(CARS_DIR, c, "carmask.png")) and \
           not os.path.exists(os.path.join(CARS_DIR, c, "carskinpaint0.png")) and \
           c not in args:
            print(f"  {code_skip(c)}")
            continue
        n += bool(bake_car(c, dry, prev))
    print(f"{'would bake' if dry else 'baked'} {n} car(s)")


def code_skip(c):
    return f"{c}: ships its own carmask (TD6 car) -> skipped"


if __name__ == "__main__":
    main()
