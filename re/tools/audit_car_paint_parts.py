#!/usr/bin/env python3
"""
audit_car_paint_parts.py  [TD5 CAR PAINT GEOMETRY 2026-10-04]

Scores a carmask.png bake on the question Mariano actually asked: does the paint
land on the WHEELS, the WINDOWS or the HEADLIGHTS?

For every paint-baked TD5 car it builds three reference regions and reports how
much of each the mask claims:

    GLASS  weak-response, dark, weakly saturated, window-sized
    LAMP   weak-response, within the lamp hardpoint seed radius
    WHEEL  weak-response, within the wheel hardpoint seed radius

and one REGRESSION GUARD:

    BODY   strong-response texels (across-variant range >= BODY_STRONG of this
           car's own body response) -- paint by the baker's own premise. This
           number must NOT fall. Everything else in the report is a count we
           want to go DOWN, so without a guard the trivially "best" bake is an
           empty mask; BODY is what makes the score honest.

The regions are rebuilt from the four factory skins + himodel.bin + carparam.json
on every run, so the audit never reads the artefact it is judging and cannot be
satisfied by a mask that simply agrees with a previously baked one.

Usage:
    python audit_car_paint_parts.py                     # score the current bake
    python audit_car_paint_parts.py --baseline _paintbak  # and diff vs a backup
                                                          # of the previous bake
    python audit_car_paint_parts.py jag gto             # named cars only

--baseline DIR expects the flat layout written by the /fix backup step:
DIR/<car>__carmask.png.
"""
import os, sys, glob
import numpy as np
from PIL import Image
from scipy import ndimage

import bake_td5_car_paint as B
import car_mesh_uv

# A texel moving at least this fraction of the car's own body response is paint
# by the baker's premise. Deliberately well ABOVE part_veto's WEAK_REL (0.45) so
# the guard and the veto cannot both be satisfied by the same texels -- the guard
# only watches texels the veto would never touch.
BODY_STRONG = 0.80


def regions(code):
    """(glass, lamp, wheel, body_strong, opaque) boolean maps, or None."""
    d = os.path.join(B.CARS_DIR, code)
    skins = sorted(glob.glob(os.path.join(d, "carskin?.png")))
    if len(skins) < 2:
        return None
    rgb, alpha = zip(*[B.load_rgba(p) for p in skins])
    S = np.stack(rgb, 0)
    A = np.stack(alpha, 0).min(0) > 0.5
    K, H, W, _ = S.shape
    rng = (S.max(0) - S.min(0)).mean(-1)
    varying = (rng > B.VAR_THR) & A

    # Same seed the baker uses, so "this car's body response" is the same number
    # in the audit and in the veto. Re-deriving it here (rather than importing a
    # cached one) is what keeps the audit independent of the bake.
    r = B.derive_primary_body(skins, B.HINTS.get(code), geo=None)
    if r is None:
        return None
    seed = r["seed"]
    body_rng = float(np.median(rng[seed])) if seed.any() else 0.0
    if body_rng < 1e-3:
        return None
    weak = A & (rng < max(B.WEAK_REL * body_rng, B.VAR_THR))
    strong = A & (rng >= BODY_STRONG * body_rng)

    geo = car_mesh_uv.load_car_geometry(code, B.CARS_DIR)
    lamp = np.zeros_like(A)
    wheel = np.zeros_like(A)
    if geo is not None:
        tg = car_mesh_uv.texel_geometry(geo, W, H)
        lo, hi = geo["mesh"].bbox()
        seed_r = B.SEED_FRAC * float(np.linalg.norm(np.asarray(hi) - np.asarray(lo)))
        lamp = weak & np.isfinite(tg["lamp_d"]) & (tg["lamp_d"] <= seed_r)
        wheel = weak & np.isfinite(tg["wheel_d"]) & (tg["wheel_d"] <= seed_r)

    base = S[0]
    lum = 0.299 * base[..., 0] + 0.587 * base[..., 1] + 0.114 * base[..., 2]
    mx, mn = base.max(-1), base.min(-1)
    sat = (mx - mn) / np.maximum(mx, 1e-3)
    dark = weak & (lum >= 0.004) & (lum < B.GLASS_LUM_HI) & (sat < B.GLASS_SAT_HI)
    lbl, ncc = ndimage.label(dark)
    glass = np.zeros_like(A)
    if ncc:
        sizes = np.bincount(lbl.ravel(), minlength=ncc + 1)
        keep = [i for i in range(1, ncc + 1) if sizes[i] >= B.GLASS_MIN_FRAC * A.sum()]
        if keep:
            glass = np.isin(lbl, keep)
    return glass, lamp, wheel, strong, A


def read_mask(path, shape):
    if not os.path.exists(path):
        return None
    m = np.asarray(Image.open(path).convert("L"), np.float32) / 255.0
    return (m > 0.5) if m.shape == shape else None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    baseline = None
    if "--baseline" in sys.argv:
        i = sys.argv.index("--baseline")
        if i + 1 < len(sys.argv):
            baseline = sys.argv[i + 1]
            args = [a for a in args if a != baseline]
    codes = args or sorted(os.path.basename(p) for p in glob.glob(os.path.join(B.CARS_DIR, "*"))
                           if os.path.isdir(p))

    hdr = "%-5s %8s %8s %8s %9s" % ("car", "glass", "lamp", "wheel", "BODY")
    print(hdr if not baseline else hdr + "    (before -> after)")
    print("-" * (len(hdr) + (18 if baseline else 0)))
    tot = dict(g=0, l=0, w=0, b=0)
    tot0 = dict(g=0, l=0, w=0, b=0)
    n = 0
    worse = []
    for c in codes:
        d = os.path.join(B.CARS_DIR, c)
        # Only cars WE baked: a ported TD6 car's hand-made carmask is not ours.
        if not os.path.exists(os.path.join(d, "carskinpaint0.png")):
            continue
        rg = regions(c)
        if rg is None:
            continue
        glass, lamp, wheel, strong, A = rg
        cur = read_mask(os.path.join(d, "carmask.png"), A.shape)
        if cur is None:
            continue
        old = read_mask(os.path.join(baseline, "%s__carmask.png" % c), A.shape) if baseline else None
        n += 1

        def cnt(m, mk):
            return int((m & mk).sum())
        g, l, w, b = cnt(glass, cur), cnt(lamp, cur), cnt(wheel, cur), cnt(strong, cur)
        tot["g"] += g; tot["l"] += l; tot["w"] += w; tot["b"] += b
        if old is None:
            print("%-5s %8d %8d %8d %9d" % (c, g, l, w, b))
            continue
        g0, l0, w0, b0 = cnt(glass, old), cnt(lamp, old), cnt(wheel, old), cnt(strong, old)
        tot0["g"] += g0; tot0["l"] += l0; tot0["w"] += w0; tot0["b"] += b0
        flag = ""
        if b < b0:
            flag = "  <-- BODY DROPPED %d" % (b0 - b)
            worse.append((c, b0 - b))
        print("%-5s %4d->%-4d %4d->%-4d %4d->%-4d %4d->%-4d%s"
              % (c, g0, g, l0, l, w0, w, b0, b, flag))

    print("-" * (len(hdr) + (18 if baseline else 0)))
    if baseline:
        def pct(a, b_):
            return (a - b_) / max(a, 1) * 100.0
        print("%-5s %4d->%-4d %4d->%-4d %4d->%-4d %4d->%-4d   (%d cars)"
              % ("ALL", tot0["g"], tot["g"], tot0["l"], tot["l"],
                 tot0["w"], tot["w"], tot0["b"], tot["b"], n))
        print("       glass %+.1f%%   lamp %+.1f%%   wheel %+.1f%%   BODY %+.1f%%"
              % (-pct(tot0["g"], tot["g"]), -pct(tot0["l"], tot["l"]),
                 -pct(tot0["w"], tot["w"]), -pct(tot0["b"], tot["b"])))
        if worse:
            print("  REGRESSION: %d car(s) lost strong-body texels: %s"
                  % (len(worse), ", ".join("%s(-%d)" % x for x in worse)))
        else:
            print("  BODY guard: no car lost a strong-response body texel.")
    else:
        print("%-5s %8d %8d %8d %9d   (%d cars)"
              % ("ALL", tot["g"], tot["l"], tot["w"], tot["b"], n))


if __name__ == "__main__":
    main()
