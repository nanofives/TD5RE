#!/usr/bin/env python3
"""Offline sky-probe threshold tuning harness (RT_LIGHTING2 Phase 1).

Mirrors the in-engine probe (td5_render_load_sky) so we can eyeball the
SUNNY / OVERCAST / NIGHT classification table across every track's sky BEFORE
wiring the runtime path. THIS TABLE IS THE PHASE-1 DELIVERABLE — the code is
easy, the thresholds are the work.

Algorithm (must stay in lock-step with the C probe):
  - luma = (R+G+B)/3, order-blind (engine averages BGRA the same way).
  - Box-downsample to <=128x128 to kill single-pixel speculars.
  - L_mean, L_peak over the downsampled image.
  - Peak region = pixels >= 0.90 * L_peak; sun UV = luma-weighted centroid of it;
    peak_area_frac = |region| / N.
  - Classify:
      NIGHT   if L_mean <  NIGHT_THR (80, == s_auto_sky_thr)
      SUNNY   if L_peak/L_mean >= R (1.8) AND L_peak >= P (200)
              AND peak_area_frac < AREA (0.20)   -- a clamped-white band is not a sun
      else    OVERCAST

Usage:
  python re/tools/sky_probe.py                 # all levels, forward skies
  python re/tools/sky_probe.py --back          # backsky variants too
  python re/tools/sky_probe.py --csv out.csv
Env-equivalent overrides (match the C knobs):
  --ratio (TD5RE_SUN_RATIO=1.8)  --peak (TD5RE_SUN_PEAK=200)
  --night (s_auto_sky_thr=80)    --area (0.20)
"""
import argparse, glob, os, sys
import numpy as np
from PIL import Image

DEF_RATIO = 1.8
DEF_PEAK  = 200.0
DEF_NIGHT = 80.0
DEF_AREA  = 0.20
MAXDIM    = 128


def box_downsample(luma, maxdim=MAXDIM):
    h, w = luma.shape
    if max(h, w) <= maxdim:
        return luma
    scale = max(h, w) / float(maxdim)
    nh, nw = max(1, int(round(h / scale))), max(1, int(round(w / scale)))
    # PIL BOX filter = area average, the cheap box-filter the plan asks for.
    img = Image.fromarray(luma.astype(np.float32))
    img = img.resize((nw, nh), Image.BOX)
    return np.asarray(img, dtype=np.float32)


HORIZON = 0.55   # sky occupies the top HORIZON fraction; below is skyline/water
SAT_THR = 0.18   # clear-blue-sky saturation floor
BLUE_THR = 0.55  # fraction of sky-band pixels that must be blue-dominant


def probe(path, ratio, peak_thr, night_thr, area_thr, horizon=HORIZON,
          sat_thr=SAT_THR, blue_thr=BLUE_THR):
    im = Image.open(path).convert("RGB")
    arr = np.asarray(im, dtype=np.float32)
    luma_full = arr.mean(axis=2)          # (R+G+B)/3, order-blind
    luma = box_downsample(luma_full)
    dh, dw = luma.shape
    N = luma.size
    L_mean = float(luma.mean())           # whole-image baseline (engine s_sky_luma)

    # --- sky band only: the sun/brightness cue lives above the horizon; bright
    #     water + skyline below fools every luma test (level002's "sun" was the
    #     bay). Peak, centroid and colour stats are all sky-band restricted. ---
    band_rows = max(1, int(dh * horizon))
    sky = luma[:band_rows, :]
    L_peak = float(sky.max())
    region = sky >= (0.90 * L_peak)
    area_frac = float(region.sum()) / float(sky.size)
    ys, xs = np.nonzero(region)
    wgt = sky[ys, xs]
    if wgt.sum() > 0:
        cu = float((xs * wgt).sum() / wgt.sum()) / float(dw - 1 if dw > 1 else 1)
        cv = float((ys * wgt).sum() / wgt.sum()) / float(dh - 1 if dh > 1 else 1)
    else:
        cu, cv = 0.5, 0.25

    # sky-band colour: saturation + blue-dominance separate clear sunny blue
    # (saturated, hard shadows, sun out of frame) from flat grey overcast.
    band = arr[:max(1, int(im.height * horizon)), :, :]
    mx = band.max(axis=2); mn = band.min(axis=2)
    sat = np.where(mx > 1.0, (mx - mn) / np.maximum(mx, 1.0), 0.0)
    sky_sat = float(sat.mean())
    R, G, B = band[:, :, 0], band[:, :, 1], band[:, :, 2]
    blue_frac = float(np.mean((B >= R) & (B >= G)))

    # sun tint = mean RGB of the peak region, mapped back to full-res
    fh, fw = im.height, im.width
    rys = (ys * fh / dh).astype(int).clip(0, fh - 1)
    rxs = (xs * fw / dw).astype(int).clip(0, fw - 1)
    sun_rgb = arr[rys, rxs, :].mean(axis=0) if len(rys) else np.array([0, 0, 0.])

    r = L_peak / max(1.0, L_mean)
    tight_sun = (r >= ratio and L_peak >= peak_thr and area_frac < area_thr)
    clear_blue = (blue_frac >= blue_thr and sky_sat >= sat_thr and L_mean >= night_thr)
    if L_mean < night_thr:
        cls = "NIGHT"
    elif tight_sun or clear_blue:
        cls = "SUNNY"
    else:
        cls = "OVERCAST"

    return dict(path=path, w=im.width, h=im.height, L_mean=L_mean, L_peak=L_peak,
                ratio=r, area=area_frac, u=cu, v=cv, cls=cls,
                sat=sky_sat, blue=blue_frac, tight=tight_sun, cblue=clear_blue,
                sun_rgb=tuple(round(float(c)) for c in sun_rgb))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="re/assets/levels")
    ap.add_argument("--back", action="store_true", help="also probe BACKSKY")
    ap.add_argument("--ratio", type=float, default=DEF_RATIO)
    ap.add_argument("--peak", type=float, default=DEF_PEAK)
    ap.add_argument("--night", type=float, default=DEF_NIGHT)
    ap.add_argument("--area", type=float, default=DEF_AREA)
    ap.add_argument("--csv", default=None)
    args = ap.parse_args()

    names = ["FORWSKY", "forwsky"]
    if args.back:
        names += ["BACKSKY", "backsky"]
    rows = []
    for lvl in sorted(glob.glob(os.path.join(args.root, "level*"))):
        for nm in names:
            p = os.path.join(lvl, nm + ".png")
            if os.path.exists(p):
                try:
                    rows.append(probe(p, args.ratio, args.peak, args.night, args.area))
                except Exception as e:
                    print(f"ERR {p}: {e}", file=sys.stderr)
                break

    hdr = (f"{'level/sky':<24} {'mean':>6} {'peak':>6} {'ratio':>5} {'sat':>5} {'blue':>5} "
           f"{'area%':>6} {'uv':>13} {'why':>7}  class")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        lvl = os.path.relpath(r["path"], args.root).replace("\\", "/")
        why = "tight" if r["tight"] else ("blue" if r["cblue"] else "-")
        print(f"{lvl:<24} {r['L_mean']:6.1f} {r['L_peak']:6.1f} {r['ratio']:5.2f} "
              f"{r['sat']:5.2f} {r['blue']:5.2f} {100*r['area']:6.1f} "
              f"({r['u']:.2f},{r['v']:.2f}) {why:>7}  {r['cls']}")

    from collections import Counter
    c = Counter(r["cls"] for r in rows)
    print("\nsummary:", dict(c),
          f"| thresholds ratio>={args.ratio} peak>={args.peak} night<{args.night} area<{args.area}")

    if args.csv:
        import csv
        with open(args.csv, "w", newline="") as f:
            wtr = csv.writer(f)
            wtr.writerow(["sky", "w", "h", "L_mean", "L_peak", "ratio", "area_frac",
                          "u", "v", "sun_r", "sun_g", "sun_b", "class"])
            for r in rows:
                lvl = os.path.relpath(r["path"], args.root).replace("\\", "/")
                wtr.writerow([lvl, r["w"], r["h"], f"{r['L_mean']:.2f}", f"{r['L_peak']:.2f}",
                              f"{r['ratio']:.3f}", f"{r['area']:.4f}", f"{r['u']:.3f}",
                              f"{r['v']:.3f}", *r["sun_rgb"], r["cls"]])
        print("wrote", args.csv)


if __name__ == "__main__":
    main()
