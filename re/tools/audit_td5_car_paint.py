#!/usr/bin/env python3
"""
audit_td5_car_paint.py  [TD5 CAR PAINT audit, 2026-10-03]

Numeric QA for the masks bake_td5_car_paint.py produces. Answers the question
the eyeball cannot: "which cars still keep factory paint on part of the body,
and how bad is it?"

The measurement does not need a ground-truth mask. The four factory skins ARE
the ground truth: a texel whose colour MOVES between carskin0..3 is paint by
definition (glass, chrome, tyres, badges and decals are byte-identical in all
four). So:

    moved     = opaque texels whose across-skin colour range clears VAR_THR
    claimed   = carmask weight > 0.5
    LEFTOVER  = moved & ~claimed
                a texel the factory repainted but our mask refuses to ->
                at runtime it keeps carskin0's colour next to a repainted
                neighbour. This is exactly the "bad sector" symptom.

Two headline numbers per car:

    coverage   claimed&moved / moved                   (want high)
    leftover   biggest leftover COMPONENT, in texels   (want small)

Component size is what matters, not the raw percentage: 3000 texels scattered
one-by-one along panel-gap antialiasing are invisible, while a single 800-texel
blob is a whole door that stayed red. Speckle below --speckle texels is
reported separately and excluded from the "worst blob" ranking.

    hard_edges  boundary texels between a claimed and a leftover texel. A high
                count with a large blob is the visible hard cut.

The mask is only half the story, and on the car that prompted this tool it was
the RIGHT half: the XJ220 claims 100% of what its four paints move and still
showed bad sectors, because the defect was in the NEUTRAL GREY the runtime
multiplies by the chosen colour. So the table also reports, and ranks by:

    jumps      body texels carrying an edge in carskinpaint0.png that the four
               skins do not have (see grey_jumps). An invented seam inside the
               mask reads as a panel that did not take the paint.

Usage:
    python audit_td5_car_paint.py              # every baked car, worst first
    python audit_td5_car_paint.py jag cob      # named cars only
    python audit_td5_car_paint.py --png jag    # also write carpaint_audit.png
    python audit_td5_car_paint.py --json=out.json
    python audit_td5_car_paint.py --speckle=40 # raise the "invisible noise" cut
"""
import os, sys, glob, json
import numpy as np
from PIL import Image
from scipy import ndimage

CARS_DIR = os.path.join(os.path.dirname(__file__), "..", "assets", "cars")

VAR_THR = 0.10          # same floor the baker uses for `varying`
SPECKLE = 24            # a leftover component below this is invisible noise
TINT = (0.15, 0.40, 1.0)


def load_rgba(path):
    a = np.asarray(Image.open(path).convert("RGBA"), np.float32) / 255.0
    return a[..., :3], a[..., 3]


def audit_space(skins, mask_path, speckle=SPECKLE, weight_in_alpha=False):
    """One UV space (carskin* or carpic*) against the mask baked for it.

    carmask.png stores the blend weight as R=G=B. carpicpaint0.png is the
    menu-preview OVERLAY: RGB is the neutral grey body and the weight lives in
    ALPHA, so reading R there would measure brightness, not coverage."""
    rgb, alpha = zip(*[load_rgba(p) for p in skins])
    S = np.stack(rgb, 0)
    A = np.stack(alpha, 0).min(0) > 0.5
    moved = ((S.max(0) - S.min(0)).mean(-1) > VAR_THR) & A

    m = np.asarray(Image.open(mask_path).convert("RGBA"), np.float32) / 255.0
    w = m[..., 3] if weight_in_alpha else m[..., 0]
    claimed = w > 0.5

    inter = (moved & claimed).sum()
    coverage = inter / max(moved.sum(), 1)

    leftover = moved & ~claimed
    lbl, n = ndimage.label(leftover)
    sizes = (ndimage.sum(np.ones_like(lbl), lbl, np.arange(1, n + 1))
             if n else np.zeros(0))
    big = sizes[sizes >= speckle]
    worst = int(big.max()) if big.size else 0

    # Hard edges: a claimed texel 4-adjacent to a NON-speckle leftover texel.
    keep_ids = np.arange(1, n + 1)[sizes >= speckle] if n else np.zeros(0, int)
    big_mask = np.isin(lbl, keep_ids) if keep_ids.size else np.zeros_like(leftover)
    nb = np.zeros_like(big_mask)
    nb[1:, :] |= big_mask[:-1, :]; nb[:-1, :] |= big_mask[1:, :]
    nb[:, 1:] |= big_mask[:, :-1]; nb[:, :-1] |= big_mask[:, 1:]
    hard_edges = int((claimed & nb).sum())

    return dict(
        moved=int(moved.sum()), claimed=int(claimed.sum()),
        coverage=float(coverage),
        leftover=int(leftover.sum()),
        leftover_big=int(big.sum()) if big.size else 0,
        worst_blob=worst, n_big=int(big.size), hard_edges=hard_edges,
        _S=S, _A=A, _moved=moved, _claimed=claimed,
        _leftover_big=big_mask, _w=w)


def grey_jumps(S, claimed, neutral_path, excess=0.06):
    """Discontinuities the BAKER invented in the neutral grey.

    Coverage only scores the mask. A car can claim 100% of its body and still
    show "bad sectors", because what the runtime multiplies by the chosen
    colour is carskinpaint0.png, and a brightness step inside the mask reads as
    a panel that did not take the paint.

    The four skins give a reference with no baker in it: their mean luminance.
    What is scored is WHERE the edges are, not how much contrast the bake has —
    the reference gradient is scaled to the bake's own gradient level first (by
    their 90th percentiles), so a bake that is uniformly punchier than the mean
    is not penalised. What IS penalised is a strong edge in the bake with no
    counterpart in the art: that is a seam the baker invented, and inside the
    paint mask it reads as a panel that did not take the colour.

    `excess` = 0.06 (~15/255 across one texel) is where an edge becomes visible
    on an otherwise flat panel.
    """
    g = np.asarray(Image.open(neutral_path).convert("RGB"), np.float32)[..., 0] / 255.0
    ref = S.mean(0).mean(-1)
    if not claimed.any():
        return 0, 0.0

    def grad(a):
        gx = np.zeros_like(a); gy = np.zeros_like(a)
        gx[:, 1:-1] = (a[:, 2:] - a[:, :-2]) * 0.5
        gy[1:-1, :] = (a[2:, :] - a[:-2, :]) * 0.5
        return np.hypot(gx, gy)

    # Erode the body by one texel so the mask's own silhouette (a real step from
    # body to background in both images) is not counted as an invented jump.
    inner = ndimage.binary_erosion(claimed, np.ones((3, 3), bool), border_value=1)
    gb, gr = grad(g), grad(ref)
    if not inner.any():
        return 0, 0.0
    s = (float(np.percentile(gb[inner], 90))
         / max(float(np.percentile(gr[inner], 90)), 1e-6))
    n = int((inner & ((gb - s * gr) > excess)).sum())
    return n, float(n / max(inner.sum(), 1) * 100.0)


def write_png(code, d, r, rp):
    """base | claimed(green)/leftover(red) | repainted — for both spaces."""
    rows = []
    for src in (r, rp):
        if src is None:
            continue
        base = np.clip(src["_S"][0] * 255, 0, 255).astype(np.uint8)
        cls = base.copy()
        cls[src["_claimed"]] = (0, 200, 0)
        cls[src["_leftover_big"]] = (255, 0, 0)
        cls[src["_moved"] & ~src["_claimed"] & ~src["_leftover_big"]] = (255, 160, 0)
        w3 = src["_w"][..., None]
        grey = src["_S"][0].mean(-1)[..., None]
        body = np.where(src["_claimed"][..., None], grey, src["_S"][0])
        tint = np.array(TINT, np.float32)
        rep = body * (w3 * tint + (1.0 - w3))
        rep = np.clip(rep * 255, 0, 255).astype(np.uint8)
        rows.append(np.concatenate([base, cls, rep], 1))
    if not rows:
        return
    width = max(r0.shape[1] for r0 in rows)
    rows = [np.pad(r0, ((0, 0), (0, width - r0.shape[1]), (0, 0))) for r0 in rows]
    Image.fromarray(np.concatenate(rows, 0)).save(
        os.path.join(d, "carpaint_audit.png"))


def audit_car(code, want_png=False, speckle=SPECKLE):
    d = os.path.join(CARS_DIR, code)
    mask = os.path.join(d, "carmask.png")
    neutral = os.path.join(d, "carskinpaint0.png")
    if not (os.path.exists(mask) and os.path.exists(neutral)):
        return None                                   # not a paint-baked TD5 car
    skins = sorted(glob.glob(os.path.join(d, "carskin?.png")))
    pics = sorted(glob.glob(os.path.join(d, "carpic?.png")))
    if len(skins) < 2:
        return None
    r = audit_space(skins, mask, speckle)
    r["jumps"], r["jumps_pct"] = grey_jumps(r["_S"], r["_claimed"], neutral)
    pic_mask = os.path.join(d, "carpicpaint0.png")
    rp = (audit_space(pics, pic_mask, speckle, weight_in_alpha=True)
          if len(pics) >= 2 and os.path.exists(pic_mask) else None)
    if want_png:
        write_png(code, d, r, rp)
    out = dict(code=code, skin={k: v for k, v in r.items() if not k.startswith("_")})
    if rp:
        out["pic"] = {k: v for k, v in rp.items() if not k.startswith("_")}
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want_png = "--png" in sys.argv
    speckle = SPECKLE
    for a in sys.argv[1:]:
        if a.startswith("--speckle="):
            speckle = int(a.split("=", 1)[1])
    codes = args or sorted(os.path.basename(p)
                           for p in glob.glob(os.path.join(CARS_DIR, "*"))
                           if os.path.isdir(p))
    rows = []
    for c in codes:
        try:
            r = audit_car(c, want_png, speckle)
        except Exception as e:
            print(f"  {c}: AUDIT FAILED {e}")
            continue
        if r:
            rows.append(r)

    rows.sort(key=lambda r: -r["skin"]["jumps"])
    print(f"{'car':5} {'cover%':>7} {'worst':>7} {'blobs':>6} {'edges':>7} "
          f"{'jumps':>7} {'jump%':>6} | {'pic cov%':>8} {'pic worst':>9}")
    for r in rows:
        s, p = r["skin"], r.get("pic")
        print(f"{r['code']:5} {s['coverage']*100:7.1f} {s['worst_blob']:7d} "
              f"{s['n_big']:6d} {s['hard_edges']:7d} "
              f"{s['jumps']:7d} {s['jumps_pct']:6.2f} | "
              f"{(p['coverage']*100 if p else float('nan')):8.1f} "
              f"{(p['worst_blob'] if p else -1):9d}")
    tot = sum(r["skin"]["jumps"] for r in rows)
    print(f"TOTAL grey jumps across {len(rows)} cars: {tot}")
    for a in sys.argv[1:]:
        if a.startswith("--json="):
            with open(a.split("=", 1)[1], "w", encoding="utf-8") as f:
                json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
