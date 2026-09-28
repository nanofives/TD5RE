#!/usr/bin/env python3
"""TD6 car preview (carpic) generator — drives td5re.exe's photo-booth boot mode.

For each TD6 car code it:
  1. launches  td5re.exe --PhotoBoothCar=<code> --DefaultOpponents=0
     (the booth spawns the car alone, hides track/HUD/VFX, clears to chroma
      BLUE88 (0,0,88), holds a front-3/4 angle then a pure-side angle, grabbing
      one frame each via the D3D11 read-back, writing _pb_a.png / _pb_b.png,
      then self-quits),
  2. crops each frame to the car (everything that is not the BLUE88 chroma),
  3. stacks the front-3/4 (top) over the side profile (bottom) into a single
     408x280 carpic on a BLUE88 background (the menu colour-keys it out),
  4. writes carpic0..3.png into re/assets/cars/<code>/ (all four identical —
     TD6 cars are recoloured at runtime by the paint tint, not by 4 skins).

The grayscale booth render is intentional: the menu MODULATE-tints the carpic
by the chosen paint colour, so a luminance carpic * colour = a coloured preview.

Usage:
    python re/tools/td6_photobooth.py                 # all 39, launch+compose
    python re/tools/td6_photobooth.py db7 mcl         # just these
    python re/tools/td6_photobooth.py --no-launch db7 # recompose existing frames
    python re/tools/td6_photobooth.py --main          # also copy into main-tree re/assets
    python re/tools/td6_photobooth.py --pattern-only  # only (re)build carpicpat0.png
    (TD5RE_PB_EXE=<path> runs a copy of the exe, so the tree can keep building)
"""
import os, sys, time, subprocess, shutil
from PIL import Image
import numpy as np

HERE       = os.path.dirname(os.path.abspath(__file__))
PROJ       = os.path.abspath(os.path.join(HERE, "..", ".."))   # worktree (or main) root
EXE        = os.environ.get("TD5RE_PB_EXE") or os.path.join(PROJ, "td5re.exe")
CARS_DIR   = os.path.join(PROJ, "re", "assets", "cars")
MAIN_CARS  = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\assets\cars"

TD6_CODES = [
    "390", "400", "atl", "att", "aud", "bmw", "cer", "chd", "chr", "cp1",
    "cp2", "cp3", "cp4", "db7", "eli", "esp", "flx", "g40", "grf", "gts",
    "lgt", "lit", "lot", "mam", "mcj", "mcl", "mgt", "pan", "pro", "pwr",
    "s12", "shl", "sub", "sup", "toy", "tur", "tus", "xjr", "xk1",
]

# Supersample factor. The booth renders the 3D scene at the [Display] resolution
# (the wrapper scales the 640x480 design space up to native), so a 4x display res
# => a 4x-sharper capture, and the carpic is emitted at 4x so it stays crisp when
# the menu draws it at the user's native display resolution.
SCALE = 4
DESIGN_W, DESIGN_H = 640, 480
DISPLAY_W, DISPLAY_H = DESIGN_W * SCALE, DESIGN_H * SCALE   # booth render/capture res
CARPIC_W, CARPIC_H = 408 * SCALE, 280 * SCALE              # output carpic size
A_REGION_H = 150 * SCALE          # top region (front-3/4 hero)
B_REGION_H = 130 * SCALE          # bottom region (side profile)
MARGIN = 6 * SCALE
BG = (0, 0, 88)                    # BLUE88 chroma — must match TD5_COLORKEY_BLUE88
LAUNCH_TIMEOUT = 40.0             # seconds to wait for both frames (4x is slower)


_BOOTH_PID = None


def kill_exe():
    """Stop ONLY the booth process this script launched (PID-scoped). A
    name-based kill would also reap any other session's td5re.exe."""
    global _BOOTH_PID
    if _BOOTH_PID:
        subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                        f"Stop-Process -Id {_BOOTH_PID} -Force -ErrorAction SilentlyContinue"],
                       capture_output=True)
        _BOOTH_PID = None


def launch_booth(code, out_suffix=""):
    """Run the booth for one car; wait until both frames are written, then kill.
    If out_suffix is given, the frames are renamed to _pb_a<suffix>/_pb_b<suffix>."""
    cdir = os.path.join(CARS_DIR, code)
    fa = os.path.join(cdir, "_pb_a.png")
    fb = os.path.join(cdir, "_pb_b.png")
    for f in (fa, fb):
        try: os.remove(f)
        except OSError: pass
    kill_exe(); time.sleep(0.4)
    global _BOOTH_PID
    r = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                        "$env:TD5RE_D3D12_CAPTURE='1'; "
                        f"$env:TD5RE_PB_NO_WHEELS='{1 if out_suffix in ('_mask', '_pat') else 0}'; "
                        f"$env:TD5RE_PB_PATTERN_MAP='{1 if out_suffix == '_pat' else 0}'; "
                        f"(Start-Process -FilePath '{EXE}' -WorkingDirectory '{PROJ}' -PassThru "
                        f"-ArgumentList '--PhotoBoothCar={code}','--DefaultOpponents=0',"
                        f"'--DefaultTrack=0','--SkipIntro=1').Id"],
                       capture_output=True, text=True)
    try:
        _BOOTH_PID = int(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        _BOOTH_PID = None
    t0 = time.time()
    while time.time() - t0 < LAUNCH_TIMEOUT:
        if os.path.exists(fa) and os.path.exists(fb):
            time.sleep(0.3)   # let the second write flush
            break
        time.sleep(0.4)
    kill_exe(); time.sleep(0.3)
    ok = os.path.exists(fa) and os.path.exists(fb)
    if ok and out_suffix:
        for t in ("a", "b"):
            dst = os.path.join(cdir, f"_pb_{t}{out_suffix}.png")
            try: os.remove(dst)
            except OSError: pass
            os.replace(os.path.join(cdir, f"_pb_{t}.png"), dst)
    return ok


# --- Mask pass: swap the skin to the (texture-space) paint mask and the hub to
#     black, re-run the booth, and capture an authoritative body silhouette that
#     aligns pixel-for-pixel with the carpic. Body renders white(+lit), glass/
#     lights/chrome render black (carmask=0), wheels render black (black hub). ---
def swap_to_mask(code):
    cdir = os.path.join(CARS_DIR, code)
    mask = os.path.join(cdir, "carmask.png")
    if not os.path.exists(mask):
        return False
    # Back up the real skin/hub.
    for n in ("carskin0.png", "carhub0.png"):
        src = os.path.join(cdir, n)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(cdir, "_bak_" + n))
    # Skin = mask as RGB (white body / black fixed); hub = solid black.
    Image.open(mask).convert("RGB").save(os.path.join(cdir, "carskin0.png"))
    Image.new("RGBA", (64, 64), (0, 0, 0, 255)).save(os.path.join(cdir, "carhub0.png"))
    return True


# --- Pattern pass [RUN1 2026-09-26]: the game itself bakes the skin as a
#     pattern map when TD5RE_PB_PATTERN_MAP=1 (td5_asset.c: R/G/B = TWO-TONE /
#     STRIPES / SPLIT secondary regions, classified in MODEL space exactly like
#     the in-race paint), so the preview and the race share one rule. Rendered
#     through the preview camera this gives, per preview pixel, what each
#     pattern paints on the car. ---


def restore_after_mask(code):
    cdir = os.path.join(CARS_DIR, code)
    for n in ("carskin0.png", "carhub0.png"):
        bak = os.path.join(cdir, "_bak_" + n)
        if os.path.exists(bak):
            os.replace(bak, os.path.join(cdir, n))


def car_mask(a):
    """Boolean foreground mask: True where the pixel is the car (not BLUE88)."""
    R, G, B = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    bg = (R < 48) & (G < 48) & (np.abs(B - 88) < 48) & (B > R + 24) & (B > G + 24)
    return ~bg


def erode4(m):
    """4-neighbour erosion: drop the outer 1px ring so the anti-aliased blue
    fringe between car and chroma becomes transparent instead of a halo."""
    er = m.copy()
    er[1:, :]  &= m[:-1, :]
    er[:-1, :] &= m[1:, :]
    er[:, 1:]  &= m[:, :-1]
    er[:, :-1] &= m[:, 1:]
    return er


def dilate4(m, n=1):
    """4-neighbour dilation, n iterations (grow the mask by n px)."""
    for _ in range(n):
        d = m.copy()
        d[1:, :]  |= m[:-1, :]
        d[:-1, :] |= m[1:, :]
        d[:, 1:]  |= m[:, :-1]
        d[:, :-1] |= m[:, 1:]
        m = d
    return m


def crop_pair(carpic_path, mask_path):
    """Crop the car from the carpic frame, and crop the ALIGNED mask frame with
    the SAME bbox. Returns (rgba_crop, body_bool_crop):
      * rgba_crop      — car opaque, BLUE88 chroma transparent (the preview).
      * body_bool_crop — True where the mask pass rendered the body (white+lit);
                         glass/lights/chrome/wheels are black there -> False.
    Both share the identical bbox so they overlay pixel-for-pixel."""
    im = Image.open(carpic_path).convert("RGB")
    a = np.array(im).astype(np.int16)
    fg = erode4(car_mask(a))
    if not fg.any():
        return None, None
    ys, xs = np.where(fg)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    rgb = a[y0:y1, x0:x1].astype(np.uint8)
    alpha = (fg[y0:y1, x0:x1].astype(np.uint8)) * 255
    rgba = Image.fromarray(np.dstack([rgb, alpha]), "RGBA")

    # Mask pass: body = bright gray (white skin * lighting, all channels high);
    # blue chroma bg (R,G low) and black fixed/wheels (all low) -> not body.
    mk = np.array(Image.open(mask_path).convert("RGB")).astype(np.int16)
    mR, mG, mB = mk[:, :, 0], mk[:, :, 1], mk[:, :, 2]
    body_full = (mR > 50) & (mG > 50) & (mB > 50)
    # DILATE (not erode): the overlay must fully cover the gray body to its edges,
    # else the gray base shows as a light rim. Clip to the car silhouette (fg) so
    # the growth can't spill past the body into the background — it may extend a
    # px or two into adjacent glass, which is invisible (glass is dark).
    body = dilate4(body_full, 2)[y0:y1, x0:x1] & fg[y0:y1, x0:x1]
    crop_pair.last = (x0, x1, y0, y1, np.minimum(np.minimum(mR, mG), mB),
                      body_full[y0:y1, x0:x1])
    return rgba, body


def crop_pattern(pat_path, body):
    """Crop the pattern-pass frame with the bbox of the LAST crop_pair call and
    return an HxWx3 bool array (TWO-TONE, STRIPES, SPLIT) inside the body mask.
    A channel counts as set when it is at least half the mask pass's lit-white
    luminance at that pixel (same lighting, so shading cancels out)."""
    x0, x1, y0, y1, lum, sampled = crop_pair.last
    pk = np.array(Image.open(pat_path).convert("RGB")).astype(np.int16)[y0:y1, x0:x1]
    lum = np.maximum(lum[y0:y1, x0:x1], 24)
    sampled = sampled & body
    bits = np.zeros(pk.shape, bool)
    for c in range(3):
        bits[:, :, c] = (pk[:, :, c] * 2 >= lum) & (pk[:, :, c] > 12) & sampled
    # The body mask was DILATED 2px past the lit body (see crop_pair); those rim
    # pixels have no pattern sample, so extend each bit into them from its
    # sampled neighbours (never into other sampled pixels).
    rim = body & ~sampled
    for c in range(3):
        b = bits[:, :, c]
        for _ in range(3):
            b = b | (dilate4(b, 1) & rim)
        bits[:, :, c] = b
    return bits


def fit_into(crop, box_w, box_h):
    """Scale to fit box (preserve aspect). crop may be an RGBA Image or a bool
    ndarray (nearest-resized to stay binary). Returns the resized object."""
    if isinstance(crop, np.ndarray):
        h, ch = crop.shape[0], crop.shape[1]
        s = min(box_w / ch, box_h / h)
        im = Image.fromarray((crop.astype(np.uint8) * 255), "L")
        im = im.resize((max(1, int(ch * s)), max(1, int(h * s))), Image.NEAREST)
        return np.array(im) > 127
    cw, ch = crop.size
    s = min(box_w / cw, box_h / ch)
    return crop.resize((max(1, int(cw * s)), max(1, int(ch * s))), Image.LANCZOS)


def compose(code):
    cdir = os.path.join(CARS_DIR, code)
    fa, fb = os.path.join(cdir, "_pb_a_car.png"), os.path.join(cdir, "_pb_b_car.png")
    ma, mb = os.path.join(cdir, "_pb_a_mask.png"), os.path.join(cdir, "_pb_b_mask.png")
    if not all(os.path.exists(p) for p in (fa, fb, ma, mb)):
        print(f"  [{code}] SKIP — missing booth/mask frames")
        return False
    pa, pb = os.path.join(cdir, "_pb_a_pat.png"), os.path.join(cdir, "_pb_b_pat.png")
    have_pat = os.path.exists(pa) and os.path.exists(pb)
    ca, ba = crop_pair(fa, ma)
    qa = crop_pattern(pa, ba) if (have_pat and ca is not None) else None
    cb, bb = crop_pair(fb, mb)
    qb = crop_pattern(pb, bb) if (have_pat and cb is not None) else None
    if ca is None or cb is None:
        print(f"  [{code}] SKIP — empty car mask")
        return False

    canvas = Image.new("RGBA", (CARPIC_W, CARPIC_H), (0, 0, 0, 0))
    paintcv = Image.new("RGBA", (CARPIC_W, CARPIC_H), (0, 0, 0, 0))
    patcv = np.zeros((CARPIC_H, CARPIC_W, 4), np.uint8)
    a_box = (CARPIC_W - 2 * MARGIN, A_REGION_H - 2 * MARGIN)
    b_box = (CARPIC_W - 2 * MARGIN, B_REGION_H - 2 * MARGIN)

    def place(crop, bodymask, box, y_off, region_h, patbits=None):
        ra = fit_into(crop, *box)
        rb = fit_into(bodymask, *box)
        if patbits is not None:
            # RGB = pattern bits (255 = SECONDARY), A = body. Same placement as
            # the paint overlay so the two stay pixel-aligned.
            ph, pw = rb.shape
            x_, y_ = (CARPIC_W - ra.size[0]) // 2, y_off + (region_h - ra.size[1]) // 2
            for c in range(3):
                pc = fit_into(patbits[:, :, c], *box)[:ph, :pw]
                patcv[y_:y_ + ph, x_:x_ + pw, c] |= (pc & rb).astype(np.uint8) * 255
            patcv[y_:y_ + ph, x_:x_ + pw, 3] |= rb.astype(np.uint8) * 255
        x = (CARPIC_W - ra.size[0]) // 2
        y = y_off + (region_h - ra.size[1]) // 2
        canvas.paste(ra, (x, y), ra)
        # Body-only overlay = the carpic crop but keep ONLY body pixels (grayscale)
        # and transparent elsewhere, so MODULATE recolours just the body.
        rar = np.array(ra)
        po = rar.copy()
        po[~rb, 3] = 0
        paintcv.paste(Image.fromarray(po, "RGBA"), (x, y), Image.fromarray(po, "RGBA"))

    place(ca, ba, a_box, 0, A_REGION_H, qa)
    place(cb, bb, b_box, A_REGION_H, B_REGION_H, qb)

    if PATTERN_ONLY:
        pass   # keep the shipped carpic0..3 / carpicpaint0 (framing is identical)
    else:
      for i in range(4):
        canvas.save(os.path.join(cdir, f"carpic{i}.png"))
    # The paint overlay is just a body MASK (tint region) drawn over the carpic,
    # so it doesn't need the carpic's 4x detail. Emit it at 2x (816x560) to keep
    # the frontend's per-car VRAM/load cost down (the full-size overlay made the
    # car-select screen stutter on cycle). 2x is pixel-sharp at typical windows.
    PAINT_W, PAINT_H = 408 * 2, 280 * 2
    if not PATTERN_ONLY:
        paintcv.resize((PAINT_W, PAINT_H), Image.LANCZOS).save(
            os.path.join(cdir, "carpicpaint0.png"))

    if qa is not None and qb is not None:
        # Same 2x size as carpicpaint0, NEAREST so the bits stay binary.
        Image.fromarray(patcv, "RGBA").resize((PAINT_W, PAINT_H), Image.NEAREST).save(
            os.path.join(cdir, "carpicpat0.png"))
    body_px = int((np.array(paintcv)[:, :, 3] > 0).sum())
    print(f"  [{code}] OK — carpic0..3 + carpicpaint0 (body {body_px}px)")
    # tidy the intermediate frames
    for p in (fa, fb, ma, mb, pa, pb):
        try: os.remove(p)
        except OSError: pass
    return True


def copy_to_main(code):
    src = os.path.join(CARS_DIR, code)
    dst = os.path.join(MAIN_CARS, code)
    if os.path.abspath(src) == os.path.abspath(dst):
        return
    os.makedirs(dst, exist_ok=True)
    names = (["carpicpat0.png"] if PATTERN_ONLY else
             [f"carpic{i}.png" for i in range(4)] + ["carpicpaint0.png", "carpicpat0.png", "carmask.png"])
    for n in names:
        s = os.path.join(src, n)
        if os.path.exists(s):
            shutil.copy2(s, os.path.join(dst, n))


INI_PATH = os.path.join(PROJ, "td5re.ini")


def set_display_res(w, h):
    """Back up td5re.ini and set [Display] Width/Height so the booth renders (and
    captures) at the supersample resolution. Returns the backup path."""
    if not os.path.exists(INI_PATH):
        return None
    bak = INI_PATH + ".pbbak"
    shutil.copy2(INI_PATH, bak)
    out, in_disp = [], False
    for ln in open(INI_PATH, encoding="utf-8", errors="ignore").read().splitlines():
        s = ln.strip().lower()
        if s.startswith("["):
            in_disp = (s == "[display]")
        if in_disp and s.startswith("width"):
            ln = f"Width          = {w}"
        elif in_disp and s.startswith("height"):
            ln = f"Height         = {h}"
        out.append(ln)
    open(INI_PATH, "w", encoding="utf-8").write("\n".join(out) + "\n")
    return bak


def restore_display_res(bak):
    if bak and os.path.exists(bak):
        os.replace(bak, INI_PATH)


PATTERN_ONLY = False


def main():
    global PATTERN_ONLY
    args = sys.argv[1:]
    # --pattern-only: run all passes but write ONLY carpicpat0.png, keeping the
    # shipped carpic0..3 / carpicpaint0 (the booth framing is deterministic).
    PATTERN_ONLY = "--pattern-only" in args
    no_launch = "--no-launch" in args
    to_main   = "--main" in args
    codes = [a for a in args if not a.startswith("--")] or TD6_CODES

    print(f"td6_photobooth: {len(codes)} car(s); launch={not no_launch} "
          f"scale={SCALE}x ({DISPLAY_W}x{DISPLAY_H}) carpic={CARPIC_W}x{CARPIC_H} main_copy={to_main}")
    ini_bak = set_display_res(DISPLAY_W, DISPLAY_H) if not no_launch else None
    ok, fail = [], []
    try:
        for code in codes:
            os.makedirs(os.path.join(CARS_DIR, code), exist_ok=True)
            if not no_launch:
                # Pass 1: the carpic (normal skin/hub).
                if not launch_booth(code, out_suffix="_car"):
                    print(f"  [{code}] FAIL — booth produced no frames")
                    fail.append(code); continue
                # Pass 2: the paint mask (skin=carmask white/black, hub=black).
                if swap_to_mask(code):
                    try:
                        if not launch_booth(code, out_suffix="_mask"):
                            print(f"  [{code}] FAIL — mask pass produced no frames")
                            fail.append(code); restore_after_mask(code); continue
                    finally:
                        restore_after_mask(code)
                else:
                    print(f"  [{code}] FAIL — no carmask.png (re-run convert_td6_cars.py)")
                    fail.append(code); continue
                # Pass 3: the secondary-paint pattern map (skin = pattern bits).
                if not launch_booth(code, out_suffix="_pat"):
                    print(f"  [{code}] WARN — pattern pass produced no frames")
            if compose(code):
                if to_main:
                    copy_to_main(code)
                ok.append(code)
            else:
                fail.append(code)
    finally:
        restore_display_res(ini_bak)
        kill_exe()
    print(f"\nDone. {len(ok)} ok, {len(fail)} failed.")
    if fail:
        print("  failed:", " ".join(fail))


if __name__ == "__main__":
    main()
