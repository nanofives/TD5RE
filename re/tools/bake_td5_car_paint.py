#!/usr/bin/env python3
"""
bake_td5_car_paint.py  [TD5 CAR PAINT 2026-09-29, classifier reworked 2026-10-01]

Offline baker that makes an ORIGINAL (TD5) car repaintable with the free colour
picker the ported TD6 cars already use.

TD6 cars ship a grayscale body + carmask.png, so the runtime multiplies the
masked body texels by the chosen colour. TD5 cars instead ship FOUR pre-painted
skins (carskin0..3) and no mask, so there is nothing to tint. This tool
reconstructs both from the four skins:

    carmask.png         body(primary paint) mask. R=G=B = the blend WEIGHT:
                        255 inside the body, ramping to 0 over the last
                        FEATHER texels at the silhouette (the runtime lerps
                        between untinted and tinted by this value, so a hard
                        0/255 mask still reproduces the old behaviour).
    carskinpaint0.png   neutral GREYSCALE body + every non-body texel verbatim
    carpicpaint0.png    same thing in carpic space, RGBA (alpha = body mask),
                        the menu-preview overlay the frontend draws MODULATEd
                        by the paint colour over the base carpic
    carpaint_preview.png  eyeball sheet (base | classification | repainted blue)

How the body is found (no ML, no training data -- it is all in the four skins):

    Only the PAINT changes between carskin0..3; glass, lights, chrome, tyres,
    decals and badges are identical in all four. A texel's "signature" -- its
    colour in all K variants stacked into one K*3 vector -- therefore tells us
    what it is.

    1. SEED. varying texels (per-texel colour range across the variants above a
       floor) are projected off the uniform white axis and clustered by
       direction (spherical k-means, magnitude-weighted). The largest cluster is
       the primary paint. This stage only has to find the paint, not to label
       every texel of it.
    2. PAINT REFERENCE. From the seed's MID-LIT, UNCLIPPED texels, take the mean
       colour per variant -> P[k], normalised to unit mean level. That is the
       primary paint as it appears in each of the four skins.
    3. FIT. Every opaque texel is fitted against the shading model

           S[k][c] ~= shade * P[k][c] + ambient

       by weighted least squares in (shade, ambient), with SATURATED samples
       (a channel at the 0/255 rail) given zero weight. A texel is body when the
       fit residual is small and the variation the model predicts for it
       (shade * the contrast of P across variants) actually exceeds the varying
       floor. Non-primary art fails the fit: a second body colour, a stripe or a
       flame is not `shade * P + ambient` for ANY single (shade, ambient).
    4. CLEAN UP. Close 1-texel gaps, fill enclosed holes, grow a few texels into
       adjacent DARK varying texels (a shaded silhouette rim carries almost no
       hue), and drop speckle components.

    WHY STAGE 3 REPLACED "keep the clusters within MERGE_COS of the dominant
    one" (the 2026-09-29 version): direction is not preserved by the real
    shading. Where a variant's paint CLIPS (a white or very bright skin rails
    its channels to 255) or where ambient fill dominates (deep shadow, specular),
    the texel's direction swings away from the paint axis even though the texel
    is plainly the same panel. Those texels fell into non-dominant clusters and
    were dropped, so the baked neutral skin kept carskin0's FACTORY paint there
    -- at runtime that region stayed red while its neighbours took the chosen
    colour, which is the "abrupt hard cut / paint not coherent across panels"
    this rework fixes. Measured over the 27 shipped bakes, the old classifier
    left factory paint on 2-89% of each car's body (chv 89%, 128 42%, fhm 25%,
    day 24%) plus 1.3k-12k enclosed holes per car.

The neutral body is the fitted (shade + ambient) level, rescaled so its 99th
percentile lands on the TD6 grayscale body top (225/255) -- that way
`grey * colour` lands a TD5 car at the same brightness as a ported TD6 car
painted the same colour.

A car whose four skins are identical (the special / hot-rod / police cars ship
one paint four times) produces nothing and stays non-paintable at runtime; the
runtime predicate is simply "carmask.png AND carskinpaint0.png both exist".

Usage:
    python bake_td5_car_paint.py                # every car under re/assets/cars
    python bake_td5_car_paint.py cob vip        # named car codes
    python bake_td5_car_paint.py --dry-run      # classify + report, write nothing
    python bake_td5_car_paint.py --preview      # also write carpaint_preview.png
"""
import os, sys, glob, json
import numpy as np
from PIL import Image
from scipy import ndimage

CARS_DIR = os.path.join(os.path.dirname(__file__), "..", "assets", "cars")
HINTS_PATH = os.path.join(os.path.dirname(__file__), "car_paint_hints.json")


def load_hints():
    """Per-car judgement calls, see car_paint_hints.json. Missing file = defaults."""
    try:
        with open(HINTS_PATH, "r", encoding="utf-8") as f:
            return {k: v for k, v in json.load(f).items()
                    if not k.startswith("_") and isinstance(v, dict)}
    except Exception:
        return {}


HINTS = load_hints()

TD6_BODY_TOP   = 0.882   # TD6 grayscale body max (225/255) — match it
VAR_THR        = 0.10    # per-texel colour range across variants
CHROMA_FLOOR   = 0.05    # |signature| after removing the white axis
KMEANS_K       = 4
MERGE_COS      = 0.80    # fold a cluster into the SEED when this close
DARK_GROW      = 12      # iterations of "grow into dark varying neighbours"
DARK_LEVEL     = 0.35
MIN_BODY_FRAC  = 0.15    # of the OPAQUE area (a carpic is mostly background)
MIN_BLOB_FRAC  = 0.80    # a real body is a few big blobs, not speckle

# ---- stage 3 (model fit) ----------------------------------------------------
SAT_HI         = 0.97    # a channel at/above this rails -> drop that sample
SAT_LO         = 0.02    # ...and at/below this (the black rail) likewise
FIT_RES        = 0.055   # max RMS residual of `shade*P + ambient`, in 0..1
FIT_MIN_VAR    = 0.075   # the model's predicted across-variant range must clear
                         # this (a little under VAR_THR: the fit sees through
                         # clipping, so it legitimately accepts texels whose
                         # RAW range was flattened by the rails)
MIN_SAMPLES    = 6       # unclipped (variant,channel) samples needed to fit
# Uncertainty budget for the NEUTRAL GREY, in grey levels (0..1). The fitted
# level `shade + ambient` carries its own standard deviation
# `rms * sqrt(var_unit)`; above this it stops being worth more than the
# closed-form mean and neutral_level() blends toward that instead.
# 0.010 ~ 2.5/255: trust the fit only where it is pinned to within a quarter of
# a visible step. Swept 0.005..0.20 over the 27 shipped bakes, scored by the
# invented-seam count of re/tools/audit_td5_car_paint.py:
#     SD_OK    0.005  0.010  0.015  0.020  0.030  0.050   inf
#     seams      223    360    805   1032   1323   1998   5520
#     fit used   29%    49%    61%    68%    78%    87%    96%
# (3526 seams before this rework). 0.010 halves the seam count again versus
# 0.015 while still letting the fit carry half the body, and no car scores worse
# than it did before. Going lower buys little: the fit is what sees THROUGH
# clipping to the true level, and the global affine calibration cannot do that
# locally. Detail is not the trade-off here — Laplacian energy inside the body
# runs 0.165 (no fit) to 0.176 (all fit) against 0.179 for the raw art, so the
# extra the fit contributes is mostly noise.
SD_OK          = 0.010
FEATHER        = 1.5     # texels of inward ramp on the written mask
# How far the reference paint must travel across the four skins for there to be
# a paint worth replacing at all. This is what keeps ss1 (the Shelby Series 1 —
# four byte-near-identical skins that differ only by JPEG-ish compression noise)
# non-paintable now that the closing/hole-fill step merges its speckle into
# blobs and the old MIN_BLOB_FRAC test no longer rejects it. Measured over the
# shipped cars: every genuine four-paint car scores >= 0.64 (vet, the lowest),
# ss1 scores 0.16.
MIN_PCONTRAST  = 0.35
# CHROME GUARD. A chrome bumper, grille or wheel rim MIRRORS the body, so its
# colour across the four skins really is `shade*P + ambient` and the fit accepts
# it -- but with a tiny paint term under a large achromatic (white highlight)
# term. Real paint is the other way round. Measured over the shipped cars, the
# seed (paint by construction) has amb/shade at the 90th percentile <= 0.23 in
# every car and both spaces, while the chrome the fit was claiming sits at
# 1.6-8.5. 0.35 separates them with room to spare.
WHITE_DOM      = 0.35

# ---- clean-up ---------------------------------------------------------------
CLOSE_ITERS    = 2       # binary closing: bridge 1-2 texel gaps inside a panel
MIN_COMP_FRAC  = 0.004   # drop accepted components smaller than this share


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


def paint_reference(S, seed):
    """Per-variant primary paint colour P (K,3) from the SEED texels.

    Uses the mid-lit band only: the brightest body texels carry the specular
    highlight (which would wash the reference toward white) and the darkest
    carry only ambient (which would darken and desaturate it). Samples sitting
    on a channel rail in ANY variant are excluded outright -- a clipped texel
    does not report its own paint. Normalised to unit mean level so the fitted
    `shade` reads as a brightness."""
    K = S.shape[0]
    sel = seed & ~np.any((S >= SAT_HI) | (S <= SAT_LO), axis=(0, 3))
    if sel.sum() < 64:
        sel = seed
    lvl = S[:, sel].mean(axis=(0, 2))                  # per-texel mean level
    lo, hi = np.percentile(lvl, [40.0, 75.0])
    band = sel.copy()
    band[sel] = (lvl >= lo) & (lvl <= hi)
    if band.sum() < 32:
        band = sel
    P = S[:, band].mean(1)                             # K,3
    m = float(P.mean())
    if m < 1e-4:
        return None
    return P / m


def fit_shade_ambient(S, A, P):
    """Weighted least-squares fit of S[k][c] ~= shade*P[k][c] + ambient, per
    texel, over the K*3 samples. Saturated samples get zero weight so a variant
    whose paint clips (a white skin) stops dragging the fit. Returns
    (shade, ambient, rms_residual, n_samples, var_unit).

    `var_unit` is the variance of the SUM `shade + ambient` for unit residual
    variance — the quantity the neutral grey is built from, so this is how well
    that grey is actually pinned down. For the design [x, 1] with weights w,
    cov = inv(X'WX) = [[n, -sx], [-sx, sxx]] / det, hence

        Var(shade + ambient) = (n - 2*sx + sxx) / det = sum_i w_i (x_i - 1)^2 / det

    which blows up exactly when it should: too few surviving samples, or all of
    them at the same x (every unclipped variant carrying the same paint level),
    leaving the shade/ambient split undetermined. neutral_level() uses it to
    decide per texel how much to trust the fit."""
    K, H, W, _ = S.shape
    Y = S.transpose(1, 2, 0, 3).reshape(H, W, K * 3)
    x = P.reshape(K * 3).astype(np.float64)
    w = ((Y > SAT_LO) & (Y < SAT_HI)).astype(np.float64)
    w[~A] = 0.0

    n    = w.sum(-1)
    sx   = (w * x).sum(-1)
    sxx  = (w * x * x).sum(-1)
    sy   = (w * Y).sum(-1)
    sxy  = (w * x * Y).sum(-1)

    det  = sxx * n - sx * sx
    ok   = (n >= MIN_SAMPLES) & (np.abs(det) > 1e-9)
    detz = np.where(ok, det, 1.0)
    shade = (sxy * n - sx * sy) / detz
    amb   = (sxx * sy - sx * sxy) / detz

    # Neither term can be negative physically. Re-solve shade alone where the
    # ambient came out below zero (a saturated, strongly lit texel does that).
    neg = ok & (amb < 0.0)
    if neg.any():
        amb = np.where(neg, 0.0, amb)
        shade = np.where(neg, sxy / np.maximum(sxx, 1e-9), shade)
    shade = np.where(ok, np.maximum(shade, 0.0), 0.0)
    amb   = np.where(ok, np.maximum(amb, 0.0), 0.0)

    res = Y - (shade[..., None] * x + amb[..., None])
    rms = np.sqrt((w * res * res).sum(-1) / np.maximum(n, 1.0))
    rms = np.where(ok, rms, 1e9)

    # `neg` texels were re-solved with ambient PINNED to zero, so there the sum
    # is just `shade` and its variance is the one-parameter 1/sxx, not the
    # two-parameter form.
    var_unit = np.where(ok, (sxx - 2.0 * sx + n) / detz, 1e9)
    var_unit = np.where(neg, 1.0 / np.maximum(sxx, 1e-9), var_unit)
    var_unit = np.where(ok, np.maximum(var_unit, 0.0), 1e9)
    return shade, amb, rms, n, var_unit


def paint_probability(S, A, seed, shade, amb, rms, pcon):
    """Per-texel PROBABILITY that a texel is primary paint, in 0..1.

    Replaces three shared hard thresholds (FIT_RES / FIT_MIN_VAR / WHITE_DOM)
    with three soft terms whose reference levels are measured PER CAR from that
    car's own seed — the texels the direction clustering already proved are
    paint. A global constant cannot suit both a glossy black concept car and a
    matt white muscle car; a reference read off the car itself can.

      p_fit   how well `shade*P + ambient` explains the four skins
      p_var   how much of the across-skin movement the model attributes to
              PAINT (a texel the paint barely moves is not worth claiming)
      p_white how little of the texel is achromatic highlight — this is the
              chrome/glass discriminator (chrome mirrors the paint, so it fits
              and it varies, but its paint term sits under a big white one)

    Combined as a geometric mean so any single term near zero vetoes the texel,
    which is the behaviour the old AND-of-thresholds had.
    """
    eps = 1e-6
    ratio = amb / np.maximum(shade, eps)
    pvar_raw = shade * pcon

    # fit_shade_ambient reports rms = 1e9 for a texel it could not solve (fewer
    # than MIN_SAMPLES unclipped samples). Those must NOT enter the reference —
    # on a heavily saturated car (cat, vip) most of the seed is unsolvable and a
    # naive percentile returned sigma = 1.6e9, which makes p_fit == 1 everywhere
    # and silently switches the fit term off. Calibrate on the solvable seed
    # only, and fall back to the fixed residual budget when too little of it is.
    sr = rms[seed]
    sr = sr[np.isfinite(sr) & (sr < 1.0)]
    if sr.size >= 64:
        sigma = max(float(np.percentile(sr, 90)) * 1.6, 0.012)
    else:
        sigma = FIT_RES
    var_ref = max(float(np.percentile(pvar_raw[seed], 10)), 0.04)
    # Clamped BOTH ways: a noisy seed pushed white_ref to 3.5 on gto, which all
    # but disables the chrome discriminator. WHITE_DOM is the designed level, so
    # allow a per-car stretch of a bit over 2x and no more.
    white_ref = float(np.percentile(ratio[seed], 90)) * 2.0
    white_ref = min(max(white_ref, 0.30), 0.80)

    p_fit = np.exp(-0.5 * (rms / sigma) ** 2)
    p_var = np.clip(pvar_raw / var_ref, 0.0, 1.0)
    p_white = np.clip(1.0 - ratio / white_ref, 0.0, 1.0)
    prob = np.cbrt(np.maximum(p_fit * p_var * p_white, 0.0))
    prob[~A] = 0.0
    return prob, dict(sigma=sigma, var_ref=var_ref, white_ref=white_ref)


def neutral_level(S, primary, shade, amb, rms, var_unit, sigma):
    """Brightness each body texel would have under a WHITE paint — on ONE scale.

    There are two ways to get it. `shade + amb` is the fitted answer, and
    `S.mean(0).mean(-1)` is the closed-form one: with P normalised to unit mean,
    mean_k,c(S) = shade*mean(P) + amb = shade + amb, so the two are
    algebraically the SAME number. Measured on fully unclipped, well-fit body
    texels they agree to a median ratio of 1.000.

    They stop agreeing the moment a sample rails. The fit DROPS railed samples
    and extrapolates from the rest; the mean keeps them at the rail. Clipping is
    not rare — on jag 4.8 of the 12 (variant,channel) samples sit on the black
    rail across the whole body, and the two answers then differ by 1.36x. Over
    the 27 shipped bakes the gap runs 1.00x (vet) to 1.61x (gto).

    The old code picked between them PER TEXEL on `rms < FIT_RES`. Two
    estimators with different scales, switched on a noise-driven contour, is a
    brightness step of up to 61% along a ragged line through the middle of a
    panel. Measured as the step in the baked grey across that contour versus the
    step carskin0 itself has in the same place, the baker was ADDING 7.6x (gto),
    3.9x (crg), 3.8x (tvr), 2.9x (frd), 1.4x (jag). That is the reported "bad
    sectors / hard edges on a repainted car": on these cars the MASK is right
    (jag claims 100% of what the four paints move) and the grey underneath it is
    not. FIT_RES is also a leftover — the 2026-10-01 rework moved every other
    decision onto a per-car calibrated residual scale, so on a car whose sigma
    is 0.26 (vet) the fixed 0.055 shunted 95% of the body to the other
    estimator.

    Fixed two ways, both of which make a hard edge structurally impossible:

      1. CALIBRATE the closed-form estimator onto the fit's scale with a robust
         affine map measured on this car's own well-fit body texels. After it
         the two agree by construction, so switching cannot create a step.
      2. BLEND instead of switching, weighted by HOW WELL PINNED DOWN the fitted
         level actually is: `sd = rms * sqrt(var_unit)`, the standard deviation
         of `shade + ambient` itself. This is the term that matters, and a
         residual test alone does not capture it — a texel can fit its surviving
         samples perfectly and still have a meaningless shade/ambient split when
         those samples all sit at the same paint level. The fit carries real
         detail the clipped mean loses, so it should win WHERE IT IS DETERMINED
         and only there. A continuous weight also has no contour to step across.

    SD_OK is in grey levels, so it is comparable across cars: 0.03 is about
    8/255, just under the point where a seam becomes visible on a flat panel.
    """
    fit = shade + amb
    raw = S.mean(0).mean(-1)
    solved = np.isfinite(rms) & (rms < 1.0)
    budget = max(float(sigma), FIT_RES)

    good = primary & solved & (rms < budget)
    cal = raw
    if good.sum() >= 64:
        x, y = raw[good], fit[good]
        try:
            a, c = np.polyfit(x, y, 1)
            # Two trimming passes: the top decile of the residual is specular
            # and mis-classified texels, and letting them set the scale is how
            # the calibration would import the very bias it exists to remove.
            for _ in range(2):
                r = np.abs(y - (a * x + c))
                k = r <= max(float(np.percentile(r, 90)), 1e-6)
                if k.sum() < 32:
                    break
                a, c = np.polyfit(x[k], y[k], 1)
            if np.isfinite(a) and np.isfinite(c) and a > 0.0:
                cal = a * raw + c
        except Exception:
            pass

    sd = np.where(solved, rms * np.sqrt(var_unit), 1e9)
    w = np.exp(-0.5 * (np.minimum(sd, 8.0 * SD_OK) / SD_OK) ** 2)
    w = np.where(solved, w, 0.0)
    return np.maximum(w * fit + (1.0 - w) * cal, 0.0)


def regularize_probability(prob, S, A):
    """Make the decision REGIONAL instead of per-texel.

    The reported symptom was "the paint is not coherent across panels", and a
    per-texel threshold is structurally incapable of being coherent: it decides
    each texel on its own noise. SLIC superpixels over the car's mean colour cut
    the atlas into colour-homogeneous patches that follow panel/glass/light
    boundaries, and averaging the probability inside each patch lets a whole
    panel carry its weakest texels instead of dropping them.

    Blended half-and-half with the raw probability rather than replacing it, so a
    superpixel that happens to straddle a real boundary cannot repaint the wrong
    side wholesale.
    """
    try:
        from skimage.segmentation import slic
        from skimage.color import rgb2lab
    except Exception:
        return prob
    H, W = prob.shape
    n_seg = max(64, (H * W) // 48)
    lab = rgb2lab(np.clip(S.mean(0), 0.0, 1.0).astype(np.float64))
    try:
        seg = slic(lab, n_segments=n_seg, compactness=10.0, channel_axis=-1,
                   convert2lab=False, start_label=1, mask=A, enforce_connectivity=True)
    except Exception:
        return prob
    ids = np.unique(seg)
    ids = ids[ids > 0]
    if ids.size == 0:
        return prob
    means = ndimage.mean(prob, seg, ids)
    lut = np.zeros(int(seg.max()) + 1, np.float64)
    lut[ids] = means
    region = lut[seg]
    out = np.where(seg > 0, 0.5 * prob + 0.5 * region, prob)
    out[~A] = 0.0
    return out


def choose_threshold(prob, A, seed):
    """Per-car cut on the regularised probability.

    Otsu over this car's own opaque histogram: the split between "paint" and
    "everything else" is where that car's two modes separate, which is not the
    same number on every car. Clamped so a degenerate histogram (a car that is
    almost all body, or almost none) cannot pick an absurd cut, and lowered if
    it would reject the seed the reference was built from.
    """
    vals = prob[A]
    thr = 0.45
    try:
        from skimage.filters import threshold_otsu
        if vals.size >= 256 and vals.max() > vals.min():
            thr = float(threshold_otsu(vals))
    except Exception:
        pass
    # No "lower the cut until it accepts the seed" step: the seed is unioned
    # back into the mask by the caller regardless, so lowering here protected
    # nothing and let a few weak seed texels drag the cut to the floor (jag,
    # cat and vip all collapsed to the 0.15 clamp, discarding Otsu's answer).
    return float(min(max(thr, 0.25), 0.70))


def derive_primary_body(paths, hint=None):
    """Primary-paint mask + neutral greyscale shade from K pre-painted variants.

    `hint` is this car's entry from car_paint_hints.json (or None).

    Returns dict(base, alpha, varying, primary, shade, ...) or None when the
    variants carry no paint difference (car is not repaintable)."""
    hint = hint or {}
    rgb, alpha = zip(*[load_rgba(p) for p in paths])
    S = np.stack(rgb, 0)                                  # K,H,W,3
    A = np.stack(alpha, 0).min(0) > 0.5                   # opaque in every variant
    K, H, W, _ = S.shape

    rng_ = (S.max(0) - S.min(0)).mean(-1)
    varying = (rng_ > VAR_THR) & A
    if varying.sum() < 256:
        return None                                       # identical skins

    # ---- stage 1: SEED the primary paint by signature direction -------------
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

    seed = np.zeros((H, W), bool)
    seed[cand] = np.isin(lab, keep)
    if seed.sum() < 256:
        return None

    # ---- stage 2+3: fit every opaque texel against the seed's paint ---------
    P = paint_reference(S, seed)
    if P is None:
        return None
    # Across-variant contrast of the reference: how much the model says a
    # unit-shade body texel should move between the four skins.
    pcon = float((P.max(0) - P.min(0)).mean())
    if pcon < MIN_PCONTRAST:
        return None                                       # no paint to replace
    shade, amb, rms, nsam, var_unit = fit_shade_ambient(S, A, P)

    # Soft, per-car-calibrated evidence, then made panel-coherent, then cut at a
    # threshold chosen from this car's own histogram. The old shared constants
    # (FIT_RES / FIT_MIN_VAR / WHITE_DOM) survive only as the floors inside
    # paint_probability's reference estimates.
    prob, refs = paint_probability(S, A, seed, shade, amb, rms, pcon)
    prob = regularize_probability(prob, S, A)
    thr = choose_threshold(prob, A, seed)
    fit_ok = A & (prob > thr)
    # The seed is paint by construction; never let the fit throw it away.
    primary = fit_ok | seed

    # [HINTS] Four-LIVERY cars: see car_paint_hints.json. Claim everything the
    # four paints MOVE, except the specular/chrome-dominated texels, which stay
    # protected by the same ratio the probability model uses.
    if hint.get("paint_all_varying"):
        ratio = amb / np.maximum(shade, 1e-6)
        primary |= varying & (ratio <= refs["white_ref"])

    if primary.sum() < 256:
        return None

    # What the clean-up below is ALLOWED to claim. A texel that neither moves
    # across the four paints nor fits the paint model is some OTHER material —
    # a tyre, a wheel rim, a chrome grille, glass, a badge — and painting it is
    # a worse bug than leaving a gap. Without this gate binary_fill_holes
    # swallowed whole enclosed non-body regions: the '69 Charger's chrome
    # grille and its entire front wheel came out flat body-colour.
    allow = A & (varying | fit_ok)

    # ---- stage 4: clean up --------------------------------------------------
    # Close 1-2 texel gaps (specular pinholes, panel-gap antialiasing), then
    # fill everything strictly enclosed by body. Both are confined to the
    # OPAQUE area so the mask can never spill onto the transparent background.
    #
    # The closing is spelled out as dilate-then-erode with border_value=1 on the
    # erosion rather than calling ndimage.binary_closing: that helper erodes with
    # border_value=0, so any part of the mask touching the atlas edge is eaten.
    # A carskin IS edge-to-edge UV charts, and the default cost ~600-1300 genuine
    # body texels per car. The union with the input also makes the closing
    # explicitly extensive, so this step can only ever ADD.
    st = np.ones((3, 3), bool)
    closed = ndimage.binary_erosion(
        ndimage.binary_dilation(primary, st, iterations=CLOSE_ITERS),
        st, iterations=CLOSE_ITERS, border_value=1)
    # The closing is BOUNDED (it can only reach CLOSE_ITERS texels past the
    # existing body), so it is left ungated — that reach is the panel-gap
    # antialiasing seam. binary_fill_holes is UNBOUNDED, so it gets the gate.
    primary = (primary | closed) & A
    primary = primary | (ndimage.binary_fill_holes(primary) & allow)

    # Grow into the shaded silhouette rim: dark texels hold no usable hue, so
    # neither the clustering nor the fit can claim them, but leaving them behind
    # outlines the car in its OLD paint. Restricted to `varying` texels so a
    # (constant) tyre or shadow never gets pulled in.
    dark = varying & (S.max(-1).max(0) < DARK_LEVEL)
    for _ in range(DARK_GROW):
        primary |= ndimage.binary_dilation(primary, np.ones((3, 3))) & dark

    # Drop accepted speckle: a stray component a few texels across is a fit
    # false positive (compression noise), never a panel. A component the SEED
    # already claimed is exempt — the direction clustering is independent
    # evidence that it is paint, and a skin atlas legitimately carries small UV
    # charts (mirrors, sills, a door handle). Without this exemption the filter
    # threw away ~1000 genuine texels per car, re-creating the very hard cuts
    # this rework exists to remove.
    lbl, ncc = ndimage.label(primary)
    if ncc == 0:
        return None
    sizes = ndimage.sum(np.ones_like(lbl), lbl, np.arange(1, ncc + 1))
    seeded = np.zeros(ncc + 1, bool)
    seeded[np.unique(lbl[seed])] = True
    total = primary.sum()
    ids = np.arange(1, ncc + 1)
    small = ids[(sizes < MIN_COMP_FRAC * total) & ~seeded[ids]]
    if small.size:
        primary &= ~np.isin(lbl, small)
    if primary.sum() < 256:
        return None

    # Reject speckle at the CAR level. A car whose four "variants" are the same
    # paint with only compression noise between them (ss1, the Shelby Series 1)
    # still produces scattered `varying` texels, and those form a mask that
    # looks like static sprayed over the whole car. A genuine body mask is a
    # handful of big blobs — one per UV chart, or one per stacked view in carpic
    # space — so score the share of the mask held by components >=5% of it.
    lbl, ncc = ndimage.label(primary)
    sizes = ndimage.sum(np.ones_like(lbl), lbl, np.arange(1, ncc + 1))
    total = primary.sum()
    blob_frac = sizes[sizes >= 0.05 * total].sum() / total
    if total / max(A.sum(), 1) < MIN_BODY_FRAC or blob_frac < MIN_BLOB_FRAC:
        return None

    # ---- neutral grey + feathered weight -----------------------------------
    # The brightness this texel would have under a WHITE paint — exactly what
    # `grey * colour` needs. See neutral_level(): the fitted and closed-form
    # answers are the same number until a sample rails, so they are calibrated
    # onto one scale and blended rather than switched between.
    lvl = neutral_level(S, primary, shade, amb, rms, var_unit, refs["sigma"])
    p99 = float(np.percentile(lvl[primary], 99))
    shade_out = np.clip(lvl * (TD6_BODY_TOP / max(p99, 1e-3)), 0.0, 1.0)

    # Blend weight: 1 inside, ramping to 0 over the last FEATHER texels of the
    # mask. Feathering INWARD (never outward) keeps the paint off the glass and
    # off the transparent background, and the ramp texels are greyscale in the
    # neutral skin, so the seam reads as a soft highlight instead of a
    # stair-stepped colour edge.
    if FEATHER > 0.0:
        dist = ndimage.distance_transform_edt(primary)
        weight = np.clip(dist / FEATHER, 0.0, 1.0).astype(np.float32)
    else:
        weight = primary.astype(np.float32)

    return dict(base=S[0], alpha=np.stack(alpha, 0)[0], varying=varying,
                primary=primary, weight=weight, shade=shade_out,
                areas=areas.tolist(), kept=len(keep), seed=seed, pcon=pcon,
                thr=thr, refs=refs, prob=prob)


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
    hint = HINTS.get(code)
    r = derive_primary_body(skins, hint)
    if r is None:
        print(f"  {code}: skins carry no paint variation -> NOT paintable")
        clear_bake(d, dry_run)
        return None

    # carpic space (the menu preview) gets its own classification: the preview
    # renders are a different projection, so the skin's texel mask does not map.
    # Both have to succeed — a car painted in-race but showing its old paint in
    # the car-select preview reads as a bug, so it stays non-paintable instead.
    pics = sorted(glob.glob(os.path.join(d, "carpic?.png")))
    rp = derive_primary_body(pics, hint) if len(pics) >= 2 else None
    if rp is None:
        print(f"  {code}: no usable carpic body -> NOT paintable")
        clear_bake(d, dry_run)
        return None

    pm, sh, wt = r["primary"], r["shade"], r["weight"]
    neutral = r["base"].copy()
    neutral[pm] = sh[pm][:, None]                        # body -> greyscale
    mask = np.repeat(u8(wt)[..., None], 3, -1)           # R=G=B = blend weight

    print(f"  {code}: body={pm.mean()*100:5.1f}% of atlas  "
          f"(varying {r['varying'].mean()*100:4.1f}%, seed {r['seed'].mean()*100:4.1f}%, "
          f"Pcon {r['pcon']:.2f})"
          f"  cal: thr={r['thr']:.2f} sigma={r['refs']['sigma']:.3f} "
          f"var_ref={r['refs']['var_ref']:.2f} white_ref={r['refs']['white_ref']:.2f}"
          f"   carpic body={rp['primary'].mean()*100:5.1f}% thr={rp['thr']:.2f}")

    if not dry_run:
        # Neutral skin FIRST: the "already baked / hand-made TD6 mask" skip below
        # keys on carmask.png, so writing that last keeps an interrupted bake
        # re-runnable instead of permanently skipped.
        Image.fromarray(u8(neutral)).save(os.path.join(d, "carskinpaint0.png"))
        Image.fromarray(mask).save(os.path.join(d, "carmask.png"))
        ov = np.zeros(rp["primary"].shape + (4,), np.uint8)
        ov[..., :3] = u8(np.repeat(rp["shade"][..., None], 3, -1))
        ov[..., 3] = u8(rp["weight"])
        ov[~rp["primary"], :3] = 0
        Image.fromarray(ov, "RGBA").save(os.path.join(d, "carpicpaint0.png"))

    if want_preview:
        src = rp
        base = u8(src["base"])
        cls = base.copy()
        cls[src["primary"]] = (0, 255, 0)
        cls[src["varying"] & ~src["primary"]] = (255, 0, 255)
        # Repaint exactly the way the runtime does: lerp white->tint by the
        # mask weight, multiply the neutral body by it.
        nb = src["base"].copy()
        nb[src["primary"]] = src["shade"][src["primary"]][:, None]
        tint = np.array((0.15, 0.40, 1.0), np.float32)
        w3 = src["weight"][..., None]
        tinted = nb * (w3 * tint + (1.0 - w3))
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
