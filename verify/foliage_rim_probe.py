"""[FOLIAGE RIM 2026-09-04] Does a dark tree-canopy billboard still show a dark RIM
hugging its silhouette against a bright sky?

Why this exists: the retired commit 43a0e73f "feathered" the canopy silhouette by
lowering the alpha-test threshold and ramping the surviving alpha. Master's foliage
now draws through TD5_PRESET_FOLIAGE_CUTOUT with blend_enable=0, so an alpha ramp is
a no-op and the feather cannot be revived as written. Before closing that out we
needed to know whether the SYMPTOM it targeted is actually gone. Answer on master:
it is (Blue Ridge, n=149, excess -3.71). See the traps below before trusting a rerun.

Method: find sky->canopy vertical transitions and average the luminance profile
across them. A dark rim is a luminance UNDERSHOOT at the boundary -- edge pixels
darker than the canopy interior plateau just behind them.

  python verify/foliage_rim_probe.py <frame.png> [--label NAME] [--crop out.png]

THREE TRAPS, ALL HIT FOR REAL WHILE WRITING THIS. Each one produced a confident
false positive that had to be retracted:

 1. THE ESTIMATOR IS BIASED. `rim` compares a MIN over 4 edge samples against a
    MEDIAN over 6 interior samples. On textured foliage that is positive even with
    no rim at all -- measured +1.5 to +9.6 on frames that have none. So the metric
    to read is EXCESS = rim - control, where `control` is the identical estimator
    shifted 8px DEEP inside the canopy, where no edge exists. Never quote `rim`
    alone. Quoting it alone is what made master look like it still had a +3.74 rim
    matching the commit's pre-fix 3.6 -- pure coincidence of a biased statistic.
 2. "BRIGHT" IS NOT "SKY". A luma>=110 pixel is also white render, bright road, a
    stone wall or a lit building. Sydney's start line is a steel bridge with a
    GREEN CHAIN-LINK FENCE: green-dominant, dark-bodied, and it sails through a
    naive foliage test. 235 "canopy edges" there were fence mesh and girders. Hence
    the hard y < 0.45*h sky-region restriction, which correctly drops Sydney to 0.
 3. A NEAR-BLACK CANOPY CANNOT SHOW A RIM. Dusk silhouettes and distant horizon
    tree lines have an interior at ~10 luma, so nothing can be darker than the body
    and the aggregate looks clean whatever the shader does. On the Keswick dusk
    frame those were 82% of all edges. Hence the median(body)>=30 lit requirement.
    It also means low-n results are worthless: Keswick and Maui land at n=7/8 after
    strict filtering and produce absurd numbers (Maui control -20.83). REQUIRE
    n >= ~100 before reading anything into the excess.

Pick a frame with big, LIT canopies against open sky. Blue Ridge (--track 3) at the
start line is the known-good view; Sydney/Maui start lines have no such canopy.
"""
import argparse, sys
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("png")
ap.add_argument("--label", default="")
ap.add_argument("--crop", default="", help="write a zoomed crop around the strongest edge")
ap.add_argument("--sky-luma", type=float, default=110.0, help="min luma to count as sky")
ap.add_argument("--dark-luma", type=float, default=70.0, help="max luma to count as canopy")
ap.add_argument("--min-lit", type=float, default=30.0, help="min canopy-interior luma (trap 3)")
ap.add_argument("--sky-frac", type=float, default=0.45, help="only scan the top N of the frame (trap 2)")
a = ap.parse_args()

im = Image.open(a.png).convert("RGB")
rgb = np.asarray(im).astype(np.float64)
h, w, _ = rgb.shape
Y = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]
G = rgb[:, :, 1]
RB = np.maximum(rgb[:, :, 0], rgb[:, :, 2])

PRE, POST = 3, 20          # POST must cover the control window at +8..+18
ylim = int(h * a.sky_frac)


def edges():
    """Yield (x, y) of every accepted sky->canopy transition."""
    for x in range(w):
        col = Y[:, x]
        yy = PRE + 1
        while yy < ylim - POST:
            if col[yy - 1] >= a.sky_luma and col[yy] <= a.dark_luma:
                if yy - PRE >= 0 and yy + POST <= h:
                    body = col[yy + 2: yy + 10]
                    gdom = G[yy + 2: yy + 10, x] - RB[yy + 2: yy + 10, x]
                    # canopy body must stay dark, be green-dominant, and be LIT
                    if (body.max() <= a.dark_luma + 25 and np.median(gdom) > 1.0
                            and np.median(body) >= a.min_lit):
                        yield x, yy
                yy += POST         # do not re-sample the same edge
            else:
                yy += 1


pts = list(edges())
lab = a.label or a.png
if not pts:
    print("%-10s n=0  -- no lit canopy-vs-sky edge; wrong view for this test" % lab)
    sys.exit(3)

P = np.array([Y[y - PRE: y + POST, x] for x, y in pts])
rim = np.median(P[:, PRE + 4: PRE + 10], axis=1) - P[:, PRE: PRE + 4].min(axis=1)
ctrl = np.median(P[:, PRE + 12: PRE + 18], axis=1) - P[:, PRE + 8: PRE + 12].min(axis=1)
excess = np.median(rim) - np.median(ctrl)

print("%-10s n=%-5d rim=%+6.2f  control=%+6.2f  EXCESS=%+6.2f   %s"
      % (lab, len(P), np.median(rim), np.median(ctrl), excess,
         "DARK RIM PRESENT" if excess > 2.0 else "no rim"))
if len(P) < 100:
    print("  !! n=%d is too small to read (trap 3) -- find a view with more lit canopy" % len(P))
print("  mean profile (sky -> canopy), offset 0 = first dark pixel:")
print("   " + "  ".join("%+d:%.0f" % (i - PRE, v) for i, v in enumerate(P.mean(axis=0))))

if a.crop:
    x, y = pts[int(np.argmax(rim))]
    box = (max(0, x - 60), max(0, y - 40), min(w, x + 60), min(h, y + 60))
    (im.crop(box)
       .resize(((box[2] - box[0]) * 6, (box[3] - box[1]) * 6), Image.NEAREST)
       .save(a.crop))
    print("  crop around strongest edge (x=%d y=%d) -> %s" % (x, y, a.crop))
