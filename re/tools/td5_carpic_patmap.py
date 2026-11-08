#!/usr/bin/env python3
"""td5_carpic_patmap.py [round TA 2026-11-08] - paint-PATTERN map for ORIGINAL TD5 car previews.

Why: the car-select preview paints the TWO-TONE / STRIPES / SPLIT secondary colour
from a per-car pattern map (carpicpat0.png: RGB = which preview pixels each pattern
paints, A = body). The ported TD6 cars get it from re/tools/td6_photobooth.py, whose
booth renders the preview itself. The ORIGINAL TD5 cars keep their shipped 1999
preview PHOTOS (carpic0..3 + the baked body mask carpicpaint0.png), taken by an
unknown camera, so they had no map and the frontend fell back to a straight cut
across the photo. On the angled (3/4) view that cut lands on the wrong body part
(Mariano 2026-11-08: the Viper's stripe sat over the front wheel arch on the 3/4
car, at the same screen x as on the side car).

What this does, per car:
  1. Booth MASK sweep: td5re.exe --PhotoBoothCar=<code> with the skin swapped to
     carmask.png (white body), wheels hidden, TD5RE_PB_SWEEP = a grid of
     (azimuth offset from the nose, elevation, distance) poses. One frame per pose.
  2. Fit: for each preview view band (top = 3/4, bottom = side) find the pose +
     2D similarity (scale, shift) whose booth body silhouette best overlaps the
     photo's body mask (IoU). Refine around the best pose with a second sweep.
  3. Booth PATTERN pass at the two winning poses (TD5RE_PB_PATTERN_MAP=1: the game
     bakes the skin as R/G/B = TWO-TONE/STRIPES/SPLIT, classified in MODEL space
     exactly like the in-race paint), warped with the fitted transform into the
     photo, clipped to the photo's body mask; photo body pixels the booth body did
     not cover take the nearest covered pixel's bits.
  4. Write re/assets/cars/<code>/carpicpat0.png at carpicpaint0.png's size.

Usage (worktree or main root; uses that root's td5re.exe):
    python re/tools/td5_carpic_patmap.py              # every car with carpicpaint0 and no carpicpat0
    python re/tools/td5_carpic_patmap.py vip cob      # just these (overwrites)
    python re/tools/td5_carpic_patmap.py --sheet vip  # also write _archive/roam/TA/patmap_<code>.png
"""
import os, sys, time, shutil, subprocess
import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
EXE = os.environ.get("TD5RE_PB_EXE") or os.path.join(PROJ, "td5re.exe")
CARS = os.path.join(PROJ, "re", "assets", "cars")
SHEETS = os.path.join(PROJ, "_archive", "roam", "TA")
VIEWS = 2                      # TD6_PREVIEW_VIEWS (td5_frontend_internal.h)
RES_W, RES_H = 1280, 960       # booth capture resolution
TIMEOUT = 900.0   # the booth quits by itself after the last pose


# ----------------------------------------------------------------------------- booth
def run_sweep(code, poses, mode):
    """mode 'mask' (white body) or 'pat' (pattern bits). Returns list of HxWx3 arrays
    (None where a frame is missing)."""
    cdir = os.path.join(CARS, code)
    outs = [os.path.join(cdir, "_pb_s%02d.png" % i) for i in range(len(poses))]
    for f in outs:
        if os.path.exists(f):
            os.remove(f)
    backups = []
    if mode == "mask":
        for n in ("carskin0.png", "carhub0.png"):
            src = os.path.join(cdir, n)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(cdir, "_bak_" + n))
                backups.append(n)
        Image.open(os.path.join(cdir, "carmask.png")).convert("RGB").save(os.path.join(cdir, "carskin0.png"))
        Image.new("RGBA", (64, 64), (0, 0, 0, 255)).save(os.path.join(cdir, "carhub0.png"))
    env = {k: v for k, v in os.environ.items() if not k.startswith("TD5RE_")}
    env.update({
        "TD5RE_D3D12_CAPTURE": "1",
        "TD5RE_PB_NO_WHEELS": "1",
        "TD5RE_PB_PATTERN_MAP": "1" if mode == "pat" else "0",
        "TD5RE_PB_SWEEP": ";".join("%g,%g,%g" % p for p in poses),
        "TD5RE_RT": "0",
        "TD5RE_WINDOW_TITLE": "td5re patmap booth " + code,
    })
    args = [EXE, "--PhotoBoothCar=" + code, "--DefaultOpponents=0", "--DefaultTrack=0",
            "--SkipIntro=1", "--Windowed=1", "--VSync=0", "--Width=%d" % RES_W, "--Height=%d" % RES_H]
    proc = subprocess.Popen(args, cwd=PROJ, env=env)
    try:
        t0 = time.time()
        while time.time() - t0 < TIMEOUT:
            if proc.poll() is not None:
                break
            time.sleep(0.25)
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()
        for n in backups:
            os.replace(os.path.join(cdir, "_bak_" + n), os.path.join(cdir, n))
    frames = []
    for f in outs:
        if os.path.exists(f):
            frames.append(np.array(Image.open(f).convert("RGB")).astype(np.int16))
            os.remove(f)
        else:
            frames.append(None)
    miss = sum(1 for f in frames if f is None)
    if miss:
        print("  [%s] WARN %s sweep: %d of %d frames missing" % (code, mode, miss, len(frames)))
    return frames


# ----------------------------------------------------------------------------- fit
BODY_MIN = 25   # lit white body lands at gray 40-50 under the booth light (RT off)


def body_of_mask_frame(fr):
    return (fr[:, :, 0] > BODY_MIN) & (fr[:, :, 1] > BODY_MIN) & (fr[:, :, 2] > BODY_MIN)


def bbox(m):
    ys, xs = np.where(m)
    if len(xs) == 0:
        return None
    return xs.min(), xs.max() + 1, ys.min(), ys.max() + 1


def warp(src, s, tx, ty, out_h, out_w, order=0):
    """Place src (cropped) into an out_h x out_w canvas: out(x,y) = src((x-tx)/s, (y-ty)/s)."""
    yy, xx = np.mgrid[0:out_h, 0:out_w].astype(np.float32)
    sx = (xx + 0.5 - tx) / s - 0.5
    sy = (yy + 0.5 - ty) / s - 0.5
    return ndimage.map_coordinates(src.astype(np.float32), [sy, sx], order=order, cval=0.0)


def fit_view(photo, booth):
    """photo, booth: bool masks. booth is the CROPPED body silhouette. Returns
    (iou, s, tx, ty) maximising IoU over scale (seeded from the bbox height) and
    every integer shift (FFT correlation). The photo may clip the car at the image
    edge, so booth pixels falling outside the photo do not count against it."""
    from scipy.signal import fftconvolve
    pb = bbox(photo)
    if pb is None:
        return (0.0, 1.0, 0, 0)
    px0, px1, py0, py1 = pb
    bh, bw = booth.shape
    s0 = (py1 - py0) / float(bh)
    H, W = photo.shape
    P = photo.astype(np.float32)
    ones = np.ones_like(P)
    psum = P.sum()
    best = (-1.0, s0, px0, py0)
    for ds in np.linspace(0.80, 1.25, 31):
        s = s0 * ds
        h2, w2 = max(1, int(round(bh * s))), max(1, int(round(bw * s)))
        Bs = warp(booth, s, 0, 0, h2, w2, order=1) > 0.5
        K = Bs[::-1, ::-1].astype(np.float32)
        # full correlation: index (y + h2 - 1, x + w2 - 1) = booth placed with top-left at (x, y)
        inter = fftconvolve(P, K, mode="full")
        bin_ = fftconvolve(ones, K, mode="full")
        iou = inter / (psum + bin_ - inter + 1e-6)
        # only placements whose top-left is within a sane window of the photo bbox
        yy, xx = np.mgrid[0:iou.shape[0], 0:iou.shape[1]]
        ty_, tx_ = yy - (h2 - 1), xx - (w2 - 1)
        ok = (np.abs(ty_ - py0) <= 20) & (np.abs(tx_ - px0) <= 60)
        iou = np.where(ok, iou, -1)
        k = np.unravel_index(int(np.argmax(iou)), iou.shape)
        if iou[k] > best[0]:
            best = (float(iou[k]), s, int(k[1] - (w2 - 1)), int(k[0] - (h2 - 1)))
    return best


def score_poses(photo_band, frames):
    res = []
    for i, fr in enumerate(frames):
        if fr is None:
            res.append((0.0, None, None))
            continue
        body = body_of_mask_frame(fr)
        bb = bbox(body)
        if bb is None:
            res.append((0.0, None, None))
            continue
        x0, x1, y0, y1 = bb
        crop = body[y0:y1, x0:x1]
        iou, s, tx, ty = fit_view(photo_band, crop)
        res.append((iou, (s, tx, ty), bb))
    return res


def grid(aofs, els, dists):
    return [(a, e, d) for a in aofs for e in els for d in dists]


# ----------------------------------------------------------------------------- compose
def pattern_bits(mask_fr, pat_fr, bb):
    x0, x1, y0, y1 = bb
    mk = mask_fr[y0:y1, x0:x1]
    pk = pat_fr[y0:y1, x0:x1]
    body = body_of_mask_frame(mk)
    lum = np.maximum(np.minimum(np.minimum(mk[:, :, 0], mk[:, :, 1]), mk[:, :, 2]), 24)
    bits = np.zeros(pk.shape, bool)
    for c in range(3):
        bits[:, :, c] = (pk[:, :, c] * 2 >= lum) & (pk[:, :, c] > 12) & body
    return body, bits


def nearest_fill(known, vals, target):
    """For target pixels not in known, copy vals from the nearest known pixel."""
    if not known.any():
        return np.zeros(target.shape + (vals.shape[2],), bool)
    _, (iy, ix) = ndimage.distance_transform_edt(~known, return_indices=True)
    out = vals[iy, ix]
    out[~target] = False
    return out


def process(code, sheet=False):
    cdir = os.path.join(CARS, code)
    paint = np.array(Image.open(os.path.join(cdir, "carpicpaint0.png")).convert("RGBA"))
    H, W = paint.shape[:2]
    photo_body = paint[:, :, 3] > 0
    bands = []
    for v in range(VIEWS):
        ys, ye = H * v // VIEWS, H * (v + 1) // VIEWS
        bands.append((ys, ye, photo_body[ys:ye]))

    # Coarse sweep (both views, ONE launch). A = front-3/4 (TD6 booth: +45), B = side (+90).
    coarse = {
        0: grid([20, 30, 40, 50, 60, 70], [0, 8, 16], [1300, 2600]),
        1: grid([80, 90, 100], [0, 6], [1300, 2600]),
    }
    allp = coarse[0] + coarse[1]
    fr = run_sweep(code, allp, "mask")
    frs = {0: fr[:len(coarse[0])], 1: fr[len(coarse[0]):]}
    seed = {}
    for v in range(VIEWS):
        sc = score_poses(bands[v][2], frs[v])
        i = int(np.argmax([r[0] for r in sc]))
        seed[v] = (coarse[v][i], sc[i][0], frs[v][i])
    # Fine sweep around each coarse winner (ONE launch).
    fine = {}
    for v in range(VIEWS):
        a_, e_, d_ = seed[v][0]
        fine[v] = grid([a_ - 6, a_ - 3, a_, a_ + 3, a_ + 6], [max(0, e_ - 4), e_, e_ + 4],
                       [d_ * 0.75, d_, d_ * 1.4])
    fr2 = run_sweep(code, fine[0] + fine[1], "mask")
    frs2 = {0: fr2[:len(fine[0])], 1: fr2[len(fine[0]):]}
    best_pose, best_mask = {}, {}
    for v in range(VIEWS):
        sc2 = score_poses(bands[v][2], frs2[v])
        j = int(np.argmax([r[0] for r in sc2]))
        if sc2[j][0] >= seed[v][1]:
            best_pose[v], best_mask[v], biou = fine[v][j], frs2[v][j], sc2[j][0]
        else:
            best_pose[v], best_mask[v], biou = seed[v][0], seed[v][2], seed[v][1]
        print("  [%s] view %d coarse best %s iou=%.3f -> final %s iou=%.3f" %
              (code, v, seed[v][0], seed[v][1], best_pose[v], biou))

    # Pattern frames at the two winning poses. The booth framing is deterministic
    # per pose, so the mask frame captured in the sweep lines up with them.
    win = [best_pose[v] for v in range(VIEWS)]
    mk = [best_mask[v] for v in range(VIEWS)]
    pt = run_sweep(code, win, "pat")
    out = np.zeros((H, W, 4), np.uint8)
    out[:, :, 3] = photo_body.astype(np.uint8) * 255
    for v in range(VIEWS):
        ys, ye, pband = bands[v]
        if mk[v] is None or pt[v] is None:
            print("  [%s] FAIL view %d: no frame" % (code, v))
            return False
        # Recompute the fit on THIS frame (identical pose; guards against jitter).
        body_full = body_of_mask_frame(mk[v])
        bb = bbox(body_full)
        body, bits = pattern_bits(mk[v], pt[v], bb)
        iou, s, tx, ty = fit_view(pband, body)
        bh = ye - ys
        wb = warp(body, s, tx, ty, bh, W, order=0) > 0.5
        wbits = np.dstack([warp(bits[:, :, c], s, tx, ty, bh, W, order=0) > 0.5 for c in range(3)])
        known = wb & pband
        filled = nearest_fill(wb, wbits, pband)
        for c in range(3):
            out[ys:ye, :, c] = (filled[:, :, c] & pband).astype(np.uint8) * 255
        cover = known.sum() / float(max(1, pband.sum()))
        print("  [%s] view %d pose %s iou=%.3f body coverage=%.3f" % (code, v, win[v], iou, cover))
    Image.fromarray(out, "RGBA").save(os.path.join(cdir, "carpicpat0.png"))
    if sheet:
        write_sheet(code, out)
    return True


def write_sheet(code, pat):
    """Eyeball sheet: carpic | STRIPES | SPLIT | TWO-TONE (secondary = orange)."""
    cdir = os.path.join(CARS, code)
    pic = np.array(Image.open(os.path.join(cdir, "carpic0.png")).convert("RGBA").resize(
        (pat.shape[1], pat.shape[0])))
    tiles = [pic[:, :, :3]]
    for ch in (1, 0, 2):   # G=STRIPES, B=SPLIT? (R=TWO-TONE, G=STRIPES, B=SPLIT)
        t = pic[:, :, :3].copy()
        sel = pat[:, :, ch] > 127
        t[sel] = (t[sel] * 0.3 + np.array([255, 140, 0]) * 0.7).astype(np.uint8)
        tiles.append(t)
    os.makedirs(SHEETS, exist_ok=True)
    Image.fromarray(np.hstack(tiles)).save(os.path.join(SHEETS, "patmap_%s.png" % code))


def main():
    args = sys.argv[1:]
    sheet = "--sheet" in args
    codes = [a for a in args if not a.startswith("--")]
    if not codes:
        codes = sorted(c for c in os.listdir(CARS)
                       if os.path.exists(os.path.join(CARS, c, "carpicpaint0.png"))
                       and os.path.exists(os.path.join(CARS, c, "carmask.png"))
                       and not os.path.exists(os.path.join(CARS, c, "carpicpat0.png")))
    print("td5_carpic_patmap: %d car(s): %s" % (len(codes), " ".join(codes)))
    ok, fail = [], []
    for c in codes:
        try:
            (ok if process(c, sheet) else fail).append(c)
        except Exception as e:   # keep going; report
            print("  [%s] ERROR %r" % (c, e))
            fail.append(c)
    print("done: %d ok, %d failed %s" % (len(ok), len(fail), " ".join(fail)))


if __name__ == "__main__":
    main()
