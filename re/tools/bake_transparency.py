"""
bake_transparency.py - One-shot pre-processor that bakes every runtime colorkey
and alpha_bleed_rgb pass into the PNG files on disk. After running this, the
engine can pass TD5_COLORKEY_NONE everywhere and the loaders just upload pixels
as-is.

Covers:
  - BLACK key   (R<8,G<8,B<8 → alpha=0) applied to frontend title text,
    BodyText/ButtonBits/ButtonLights, bg gallery, track previews
  - BLUE88 key  (R<8,G<8,80<=B<=96 → alpha=0) applied to car preview pictures
  - RED key     (R>=248,G<8,B<8 → alpha=0, RGB zeroed) applied to
    ArrowButtonz and snkmouse cursor
  - Slot-4 tpage alpha keying (alpha=0x00 for RGB==0, alpha=0x80 otherwise)
    applied to tpage0.png (the SPEEDO/HUD atlas that ends up on D3D slot 4)
  - alpha_bleed_rgb: 4-pass 8-neighbour RGB dilation into alpha=0 texels so
    LINEAR sampling doesn't halo

Idempotent: re-running over already-baked PNGs produces the same bytes (apart
from alpha_bleed fill converging), so it's safe to run repeatedly.
"""
import glob
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow required. pip install Pillow", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# Colour key kernels
# ---------------------------------------------------------------------------

def key_black(r, g, b, a):
    if r < 8 and g < 8 and b < 8:
        return (0, 0, 0, 0)
    return (r, g, b, a)

def key_blue88(r, g, b, a):
    if r < 8 and g < 8 and 80 <= b <= 96:
        return (0, 0, 0, 0)
    return (r, g, b, a)

def key_red(r, g, b, a):
    if r >= 248 and g < 8 and b < 8:
        return (0, 0, 0, 0)
    return (r, g, b, a)

def key_slot4_speedo(r, g, b, a):
    # Slot 4 tpage rule from UploadRaceTexturePage @ 0x0040b590
    if r + g + b == 0:
        return (0, 0, 0, 0x00)
    return (r, g, b, 0x80)


# ---------------------------------------------------------------------------
# alpha_bleed_rgb: 4-pass 8-neighbour RGB dilation into alpha=0 texels
# Matches td5_asset.c alpha_bleed_rgb() exactly.
# ---------------------------------------------------------------------------

def alpha_bleed_rgb(img, iterations=4):
    w, h = img.size
    px = list(img.getdata())
    for _ in range(iterations):
        filled = 0
        new_px = list(px)
        for y in range(h):
            for x in range(w):
                i = y * w + x
                if px[i][3] != 0:
                    continue
                sr = sg = sb = 0
                n = 0
                for dy in (-1, 0, 1):
                    yy = y + dy
                    if yy < 0 or yy >= h:
                        continue
                    for dx in (-1, 0, 1):
                        if dx == 0 and dy == 0:
                            continue
                        xx = x + dx
                        if xx < 0 or xx >= w:
                            continue
                        j = yy * w + xx
                        if px[j][3] != 0:
                            sr += px[j][0]
                            sg += px[j][1]
                            sb += px[j][2]
                            n += 1
                if n > 0:
                    new_px[i] = (sr // n, sg // n, sb // n, 0)
                    filled += 1
        px = new_px
        if filled == 0:
            break
    img.putdata(px)
    return img


def bake(png_path, keyfn, bleed=True):
    img = Image.open(png_path).convert("RGBA")
    px = [keyfn(r, g, b, a) for (r, g, b, a) in img.getdata()]
    img.putdata(px)
    if bleed:
        alpha_bleed_rgb(img, iterations=4)
    img.save(png_path)


# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

BLACK_KEY = [
    "re/assets/frontend/MainMenuText.png",
    "re/assets/frontend/RaceMenuText.png",
    "re/assets/frontend/QuickRaceText.png",
    "re/assets/frontend/OptionsText.png",
    "re/assets/frontend/SelectCarText.png",
    "re/assets/frontend/TrackSelectText.png",
    "re/assets/frontend/HighScoresText.png",
    "re/assets/frontend/ResultsText.png",
    "re/assets/frontend/NetPlayText.png",
    "re/assets/frontend/BodyText.png",
    "re/assets/frontend/ButtonBits.png",
    "re/assets/frontend/ButtonLights.png",
    "re/assets/extras/pic1.png",
    "re/assets/extras/pic2.png",
    "re/assets/extras/pic3.png",
    "re/assets/extras/pic4.png",
    "re/assets/extras/pic5.png",
]

RED_KEY = [
    "re/assets/frontend/ArrowButtonz.png",
    "re/assets/frontend/snkmouse.png",
]


def main():
    baked = 0
    skipped = 0

    # Track previews (glob)
    for p in sorted(glob.glob("re/assets/tracks/*.png")):
        bake(p, key_black); baked += 1

    # Car preview pictures (glob — every carpic*.png under cars/*/)
    for p in sorted(glob.glob("re/assets/cars/*/carpic*.png", recursive=False)):
        bake(p, key_blue88); baked += 1

    # Frontend + extras black keys
    for p in BLACK_KEY:
        if os.path.exists(p):
            bake(p, key_black); baked += 1
        else:
            skipped += 1
            print(f"  MISSING {p}", file=sys.stderr)

    # Red keys
    for p in RED_KEY:
        if os.path.exists(p):
            bake(p, key_red); baked += 1
        else:
            skipped += 1
            print(f"  MISSING {p}", file=sys.stderr)

    # Slot-4 tpage alpha keying (tpage0.png = slot 4 atlas)
    tpage4 = "re/assets/static/tpage0.png"
    if os.path.exists(tpage4):
        img = Image.open(tpage4).convert("RGBA")
        px = [key_slot4_speedo(r, g, b, a) for (r, g, b, a) in img.getdata()]
        img.putdata(px)
        alpha_bleed_rgb(img, iterations=4)
        img.save(tpage4); baked += 1

    print(f"Baked transparency into {baked} PNGs, skipped {skipped}")


if __name__ == "__main__":
    main()
