#!/usr/bin/env python3
"""
track_preview_render.py -- Render top-down Test Drive 5 track previews.

Reads a track's STRIP.DAT geometry, walks the FULL span table (main ring +
branch spans), computes the lane centerline (midpoint of each span's left/right
edge vertices), and draws it as a red line over a black (color-key) background.
The output drops straight into the track-selector frontend as
`re/assets/tracks/trak%04d.png` (152x224, black keyed transparent).

Why a new tool (vs strip_viewer.py): strip_viewer parses only `span_count`
(hdr[1]) spans -- the MAIN ring -- and silently ignores every branch span.
Branches live at span indices [span_count, total_spans), where
    total_spans = (vertex_offset - span_offset) / 24   ( == hdr[4] )
Forward-junction spans (type 8) fwd-link into a backward-sentinel (type 9) at a
branch start; backward-junction spans (type 11) bwd-link from a forward-sentinel
(type 10) at a branch end; branch interiors chain by index. This tool renders
all of it, so shortcuts / alternate routes show up in the preview.

RE basis (all confirmed):
  - STRIP.DAT layout: BindTrackStripRuntimePointers @ 0x00444070
    (header = span_offset, span_count, vertex_offset, vertex_count, total_spans)
  - trak TGA number == pool index; pool -> level dir via the pool-to-ZIP table
    @ 0x00466D50 (mirrored in td5_asset.c k_pool_to_zip); pool 19 -> level030.
  - Frontend loads re/assets/tracks/trak%04d.png color-keyed on black
    (apply_colorkey TD5_COLORKEY_BLACK: r<8 && g<8 && b<8 -> alpha 0).

Usage:
  # one track -> one PNG (level dir OR strip.dat OR level zip)
  python track_preview_render.py render re/assets/levels/level023 --out moscow.png

  # regenerate every track-selector preview in place (backs up originals first)
  python track_preview_render.py render-all

  # high-res annotated reference (edges + branch coloring + junctions) for
  # understanding geometry, e.g. when porting a TD6 track
  python track_preview_render.py debug re/assets/levels/level023 --out moscow_dbg.png
"""

import argparse
import json
import math
import os
import statistics
import struct
import sys
import zipfile
from dataclasses import dataclass

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    sys.exit("Pillow required: pip install Pillow")

try:
    import numpy as np
except ImportError:
    np = None


# ---------------------------------------------------------------------------
# Track-selector preview geometry (must match the original assets)
# ---------------------------------------------------------------------------

PREVIEW_W = 152
PREVIEW_H = 224
RED = (255, 0, 0)
BLACK = (0, 0, 0)

# pool index (== trak TGA number) -> level directory number.
# pools 0..18 from td5_asset.c k_pool_to_zip (VA 0x466D50); pool 19 = drag strip.
POOL_TO_LEVEL = {
    0: 1,  1: 2,  2: 3,  3: 4,  4: 5,  5: 6,
    6: 13, 7: 14, 8: 15, 9: 16, 10: 17, 11: 23,
    12: 25, 13: 26, 14: 27, 15: 28, 16: 29, 17: 37, 18: 39,
    19: 30,
}

# pool index -> display name. Derived by composing the two confirmed tables:
#   schedule(=quickrace track idx) -> pool : k_schedule_to_pool (td5_asset.c)
#   schedule -> city name          : quickrace race.track list
# e.g. schedule 5 NEWCASTLE -> pool 16; schedule 0 MOSCOW -> pool 11. Logging only.
POOL_TO_NAME = {
    0: "KESWICK", 1: "SAN_FRANCISCO", 2: "BERN", 3: "KYOTO",
    4: "WASHINGTON", 5: "MUNICH", 6: "HONOLULU", 7: "SYDNEY",
    8: "TOKYO", 9: "EDINBURGH", 10: "BLUE_RIDGE", 11: "MOSCOW",
    12: "CHEDDAR", 13: "JARASH", 14: "COURMAYEUR", 15: "MAUI",
    16: "NEWCASTLE", 17: "HOUSE_OF_BEZ", 18: "MONTEGO", 19: "DRAG_STRIP",
}


# ---------------------------------------------------------------------------
# [H5 2026-10-02] The RACE's checkpoint records (not LEVELINF)
# ---------------------------------------------------------------------------
# The tick marks used to come from LEVELINF checkpoint_count/checkpoint_spans[7]
# (0x08/0x0C). That is the wrong table: the original reads +0x08 only as an
# ENABLE flag and takes the actual span thresholds from g_raceCheckpointTablePtr
# -- CheckRaceCompletionState @ 0x00409e80 does `if (trackEnvironmentConfig[8]
# == 0) return;` and then indexes _g_raceCheckpointTablePtr, never +0x0C. The
# LEVELINF array additionally leads with the track's START span (and two leading
# entries on San Francisco), so every preview drew one or two ticks the race
# does not have, plus a tick on the finish line.
#
# These three tables are the original's own, byte-verified against
# original/TD5_d3d.exe and mirrored in td5_game.c / td5_asset.c:
#   gScheduleToPoolIndex            [CONFIRMED @ 0x00466894]
#   gTrackPoolSpanCountTable        [CONFIRMED @ 0x00466D50]  (forward record)
#   gTrackPoolReverseSpanCountTable [CONFIRMED @ 0x00466E3C]  (-1 = no reverse)
#   g_raceCheckpointTable           [CONFIRMED @ 0x0046CBB0]  (40 x 12 uint16)
# Record layout: count, initial_time, then 5 x (span_threshold, time_bonus).
# NOTE both pool tables hold the original's raw pool-count value; the record
# index is value - 1 (same arithmetic as td5_asset_resolve_checkpoint_record_index).
SCHEDULE_TO_POOL = [11, 9, 7, 10, 13, 16, 15, 14, 6, 8,
                    0, 1, 2, 3, 4, 5, 12, 18, 17, 19]
POOL_SPAN_COUNT_FWD = [1, 2, 3, 4, 5, 6, 13, 14, 15, 16,
                       17, 23, 25, 26, 27, 28, 29, 37, 39, 64]
POOL_SPAN_COUNT_REV = [7, 8, 9, 10, 11, 12, 18, 19, 20, 21,
                       22, 24, -1, -1, -1, -1, -1, -1, -1, 1073741824]
# Only the span thresholds are needed here (bonuses drive the race timer, not
# the preview). Index = record number; value = the record's span list.
CHECKPOINT_RECORD_SPANS = {
    0:  [869, 1511, 2061, 2618, 3074],   6:  [556, 1113, 1663, 2305, 3060],
    1:  [826, 1429, 1652, 1926, 2516],   7:  [715, 989, 1212, 1815, 2508],
    2:  [768, 1379, 2090, 2776, 3221],   8:  [585, 1271, 1982, 2593, 3282],
    3:  [623, 1175, 1751, 2181, 2552],   9:  [466, 896, 1472, 2024, 2528],
    4:  [747, 1006, 1533, 1978, 2754],   10: [901, 1346, 1873, 2132, 2755],
    5:  [609, 1029, 1560, 2140, 2567],   11: [519, 1099, 1630, 2050, 2523],
    12: [651, 1128, 1599, 2115, 2574],   17: [606, 1122, 1593, 2070, 2610],
    13: [486, 1057, 1655, 2071, 2658],   18: [665, 1081, 1679, 2250, 2635],
    14: [660, 1297, 1840, 2193, 2656],   19: [583, 936, 1479, 2116, 2657],
    15: [629, 1182, 1608, 2211, 2644],   20: [544, 1147, 1573, 2126, 2684],
    16: [685, 1446, 1842, 2281, 2988],   21: [827, 1266, 1662, 2423, 2989],
    22: [738, 1116, 1707, 2094, 2649],   23: [694, 1081, 1672, 2050, 2668],
    24: [106, 1511, 2061, 2618, 3120],   25: [25, 1511, 2061, 2618, 3120],
    26: [119, 1511, 2061, 2618, 3120],   27: [56, 1511, 2061, 2618, 3120],
    28: [116, 1511, 2061, 2618, 3120],   29: [204],
    30: [204],                           31: [106, 1511, 2061, 2618, 3120],
    32: [25, 1511, 2061, 2618, 3120],    33: [119, 1511, 2061, 2618, 3120],
    34: [56, 1511, 2061, 2618, 3120],    35: [116, 1511, 2061, 2618, 3120],
    36: [47, 1511, 2061, 2618, 3120],    37: [47, 1511, 2061, 2618, 3120],
    38: [35, 1511, 2061, 2618, 3120],    39: [35, 1511, 2061, 2618, 3120],
}


def race_checkpoint_record(pool, reverse=False):
    """The race's checkpoint span list for a POOL index, or [] when the track
    has no record. Mirrors td5_game_track_checkpoint_record()."""
    if pool is None or not (0 <= pool < len(POOL_SPAN_COUNT_FWD)):
        return []
    raw = POOL_SPAN_COUNT_REV[pool] if reverse else POOL_SPAN_COUNT_FWD[pool]
    if reverse and raw < 0:                      # no reverse data: forward record
        raw = POOL_SPAN_COUNT_FWD[pool]
    return list(CHECKPOINT_RECORD_SPANS.get(raw - 1, []))


def drawable_checkpoints(spans, ring, is_circuit):
    """The subset of a record that earns a TICK, as [(idx, span)].

    Mirrors td5_game_checkpoint_is_tick(). Two entries never get one because
    they are already drawn as marker DOTS: on a point-to-point track the LAST
    entry is the finish line, on a circuit the FIRST is the lap/start-finish
    line (a circuit ends on laps, not on checkpoints). Spans outside [1, ring)
    are inert at race time too -- the shipped circuit records carry four spans
    copied from Keswick (1511/2061/2618/3120) that sit far past every circuit's
    own ring, and the HUD minimap drops them the same way."""
    n = len(spans)
    out = []
    for i, sp in enumerate(spans):
        if sp <= 0 or (ring > 0 and sp >= ring):
            continue
        if i == (0 if is_circuit else n - 1):
            continue
        out.append((i, int(sp)))
    return out


# ---------------------------------------------------------------------------
# STRIP.DAT parsing (branch-aware)
# ---------------------------------------------------------------------------

@dataclass
class Span:
    index: int
    span_type: int
    left_vi: int
    right_vi: int
    fwd_link: int
    bwd_link: int
    origin_x: int
    origin_y: int
    origin_z: int


@dataclass
class Strip:
    span_count_main: int   # hdr[1]: main ring spans [0, span_count_main)
    total_spans: int       # derived: includes branch spans [span_count_main, total)
    spans: list            # list[Span], length == total_spans
    verts: list            # list[(x,y,z)] int16 triples


JUNCTION_FWD = 8     # forward-junction (main): fwd_link -> branch sentinel
SENTINEL_BWD = 9     # backward-sentinel (branch start)
SENTINEL_FWD = 10    # forward-sentinel (branch end)
JUNCTION_BWD = 11    # backward-junction (main): bwd_link <- branch sentinel


def parse_strip(data: bytes) -> Strip:
    if len(data) < 20:
        raise ValueError(f"STRIP.DAT too small: {len(data)} bytes")
    span_off, span_count, vtx_off, vtx_count, hdr4 = struct.unpack_from("<5I", data, 0)

    # Total span count = how many 24-byte records fill span_off..vtx_off.
    # This equals hdr4 on every shipped track but is derived from offsets so the
    # parse is self-validating (and survives a TD6 track with a different hdr4).
    region = vtx_off - span_off
    if region <= 0 or region % 24 != 0:
        raise ValueError(f"bad span region: span_off={span_off} vtx_off={vtx_off}")
    total = region // 24
    if hdr4 != total:
        print(f"  [note] hdr[4]={hdr4} != derived total={total}; using derived",
              file=sys.stderr)

    spans = []
    for i in range(total):
        off = span_off + i * 24
        (st, _attr, _b2, _pk, lvi, rvi, fwd, bwd, ox, oy, oz) = \
            struct.unpack_from("<BBBBHHhhiii", data, off)
        spans.append(Span(i, st, lvi, rvi, fwd, bwd, ox, oy, oz))

    verts = []
    n_verts = (len(data) - vtx_off) // 6
    for i in range(n_verts):
        vx, vy, vz = struct.unpack_from("<hhh", data, vtx_off + i * 6)
        verts.append((vx, vy, vz))

    return Strip(span_count_main=span_count, total_spans=total, spans=spans, verts=verts)


def _s16(v: int) -> int:
    """strip.json stores the two 16-bit link fields in their RAW unsigned form
    (a -1 sentinel appears as 65535), while parse_strip unpacks them signed via
    "<h". Normalize so both loaders agree."""
    return v - 65536 if v > 32767 else v


def parse_strip_json(doc: dict) -> Strip:
    """Build a Strip from the editable strip.json (td5_assetsrc.c's source form
    for STRIP.DAT: `header` = the 5 u32s, `spans` = the same 11-field records
    parse_strip unpacks, `vertices` = int16 triples). The .dat files were retired
    from re/assets/levels, so this is the live path."""
    hdr = doc.get("header")
    if isinstance(hdr, dict):          # tolerate the annotated {"value": [...]} form
        hdr = hdr.get("value")
    rows = doc.get("spans")
    vrows = doc.get("vertices")
    if not (isinstance(hdr, list) and len(hdr) == 5 and rows and vrows):
        raise ValueError("strip.json: missing header/spans/vertices")
    span_count = int(hdr[1])
    spans = []
    for i, r in enumerate(rows):
        if len(r) != 11:
            raise ValueError(f"strip.json: span {i} has {len(r)} fields, expected 11")
        st, _attr, _b2, _pk, lvi, rvi, fwd, bwd, ox, oy, oz = (int(x) for x in r)
        spans.append(Span(i, st, lvi, rvi, _s16(fwd), _s16(bwd), ox, oy, oz))
    verts = [tuple(_s16(int(c)) for c in v) for v in vrows]
    return Strip(span_count_main=span_count, total_spans=len(spans),
                 spans=spans, verts=verts)


def load_strip(path: str) -> Strip:
    """Accept a level dir, a raw strip.dat / strip.json, or a level zip."""
    if os.path.isdir(path):
        for cand in ("strip.dat", "STRIP.DAT"):
            p = os.path.join(path, cand)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    return parse_strip(f.read())
        # .dat retired -> the editable JSON source is the live form.
        for cand in ("strip.json", "STRIP.JSON"):
            p = os.path.join(path, cand)
            if os.path.isfile(p):
                with open(p, "r", encoding="utf-8") as f:
                    return parse_strip_json(json.load(f))
        raise FileNotFoundError(f"no strip.dat / strip.json in {path}")
    if path.lower().endswith(".json"):
        with open(path, "r", encoding="utf-8") as f:
            return parse_strip_json(json.load(f))
    if path.lower().endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            names = {n.lower(): n for n in z.namelist()}
            for cand in ("strip.dat",):
                if cand in names:
                    return parse_strip(z.read(names[cand]))
        raise FileNotFoundError(f"no strip.dat in zip {path}")
    with open(path, "rb") as f:
        return parse_strip(f.read())


# ---------------------------------------------------------------------------
# Centerline + segment topology
# ---------------------------------------------------------------------------

def span_center(sp: Span, verts: list):
    """Lane center at a span = midpoint of left & right edge vertices (world XZ).

    World pos = origin + vertex. The span `origin` is a coarse 24.8 cell anchor
    shared by several consecutive spans; each `vertex` is a signed 24.8 offset
    that places the actual road geometry around it. (strip_viewer.py mistakenly
    used origin/256 + vertex, weighting the vertex 256x too heavily, which
    scatters the points; verified empirically that origin+vertex yields a
    uniform single-line track, CV(step)=0.15.) Units left in raw 24.8 -- only
    the 2D shape matters for a preview, so the global /256 is irrelevant."""
    nv = len(verts)
    if 0 <= sp.left_vi < nv and 0 <= sp.right_vi < nv:
        lx, _ly, lz = verts[sp.left_vi]
        rx, _ry, rz = verts[sp.right_vi]
        cx = sp.origin_x + (lx + rx) / 2.0
        cz = sp.origin_z + (lz + rz) / 2.0
        return (cx, cz)
    return (float(sp.origin_x), float(sp.origin_z))


def build_topology(strip: Strip):
    """Return (centers, main_segs, branch_segs, stitch_segs).

    Each *_segs is a list of (i, j) index pairs to draw as a line.
      - main_segs:   the main ring, index-adjacent + distance-gated, loop-closed
                     if the ends meet (circuit).
      - branch_segs: branch-interior chains, index-adjacent + distance-gated.
      - stitch_segs: junction<->sentinel connectors that tie branches into the
                     main road at the forks.
    """
    centers = [span_center(sp, strip.verts) for sp in strip.spans]
    n_main = strip.span_count_main
    total = strip.total_spans

    def dist(i, j):
        a, b = centers[i], centers[j]
        return math.hypot(a[0] - b[0], a[1] - b[1])

    steps = [dist(i, i + 1) for i in range(n_main - 1)]
    med = statistics.median(steps) if steps else 1.0
    gap_thr = med * 6.0           # break the line across teleports
    stitch_thr = med * 40.0       # cap fork connectors so they can't draw chords

    main_segs = []
    for i in range(n_main - 1):
        if dist(i, i + 1) <= gap_thr:
            main_segs.append((i, i + 1))
    # circuit closure: if the ring's ends are adjacent, close it.
    if n_main >= 3 and dist(n_main - 1, 0) <= gap_thr:
        main_segs.append((n_main - 1, 0))

    branch_segs = []
    for i in range(n_main, total - 1):
        if dist(i, i + 1) <= gap_thr:
            branch_segs.append((i, i + 1))

    stitch_segs = []
    for sp in strip.spans:
        tgt = None
        if sp.span_type == JUNCTION_FWD and 0 <= sp.fwd_link < total:
            tgt = sp.fwd_link
        elif sp.span_type == JUNCTION_BWD and 0 <= sp.bwd_link < total:
            tgt = sp.bwd_link
        if tgt is not None and dist(sp.index, tgt) <= stitch_thr:
            stitch_segs.append((sp.index, tgt))

    return centers, main_segs, branch_segs, stitch_segs


def edge_polylines(strip: Strip):
    """Left/right edge world-XZ points per span (for the debug render)."""
    nv = len(strip.verts)
    lefts, rights = [], []
    for sp in strip.spans:
        if 0 <= sp.left_vi < nv and 0 <= sp.right_vi < nv:
            lx, _, lz = strip.verts[sp.left_vi]
            rx, _, rz = strip.verts[sp.right_vi]
            lefts.append((sp.origin_x + lx, sp.origin_z + lz))
            rights.append((sp.origin_x + rx, sp.origin_z + rz))
        else:
            lefts.append(None)
            rights.append(None)
    return lefts, rights


# ---------------------------------------------------------------------------
# Projection (world XZ -> image pixels), with auto-orient
# ---------------------------------------------------------------------------

# The 8 dihedral (D4) orientations of the world XZ plane. Each is applied to
# every (x,z) point BEFORE fitting to the box, so all 8 fill the canvas. Used by
# render-all's "match the original art" mode (the 1999 previews each used their
# own rotation/mirror, not derivable from geometry -- so we just try all 8 and
# keep whichever overlaps the backed-up original best).
DIHEDRAL = [
    lambda p: (p[0], p[1]),     # 0   identity
    lambda p: (-p[1], p[0]),    # 1   rot 90
    lambda p: (-p[0], -p[1]),   # 2   rot 180
    lambda p: (p[1], -p[0]),    # 3   rot 270
    lambda p: (-p[0], p[1]),    # 4   mirror
    lambda p: (p[1], p[0]),     # 5   mirror + rot 90
    lambda p: (p[0], -p[1]),    # 6   mirror + rot 180
    lambda p: (-p[1], -p[0]),   # 7   mirror + rot 270
]


class Projector:
    def __init__(self, pts, w, h, margin, auto_orient=True):
        xs = [p[0] for p in pts if p is not None]
        zs = [p[1] for p in pts if p is not None]
        self.min_x, self.max_x = min(xs), max(xs)
        self.min_z, self.max_z = min(zs), max(zs)
        ext_x = (self.max_x - self.min_x) or 1.0
        ext_z = (self.max_z - self.min_z) or 1.0
        avail_w = max(1.0, w - 2 * margin)
        avail_h = max(1.0, h - 2 * margin)

        # orientation 0: x->horizontal, z->vertical;  orientation 90: swap.
        s0 = min(avail_w / ext_x, avail_h / ext_z)
        s90 = min(avail_w / ext_z, avail_h / ext_x)
        self.rot = (auto_orient and s90 > s0)
        self.scale = s90 if self.rot else s0

        if self.rot:
            uext, vext = ext_z, ext_x
        else:
            uext, vext = ext_x, ext_z
        # center the drawn bbox in the canvas
        self.off_u = margin + (avail_w - uext * self.scale) / 2.0
        self.off_v = margin + (avail_h - vext * self.scale) / 2.0
        self.w, self.h = w, h

    def __call__(self, p):
        x, z = p
        if self.rot:
            u = (z - self.min_z) * self.scale
            v = (x - self.min_x) * self.scale
        else:
            u = (x - self.min_x) * self.scale
            v = (z - self.min_z) * self.scale
        px = self.off_u + u
        py = self.h - (self.off_v + v)   # flip so +axis points up
        return (px, py)


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def _bake_transparent(rgb_img, color):
    """Convert a red-on-black RGB render into an RGBA image with a transparent
    background: alpha = per-pixel coverage (max channel), RGB = `color` on
    covered texels and black elsewhere. Falls back to a hard colorkey if numpy
    is unavailable."""
    if np is None:
        rgba = rgb_img.convert("RGBA")
        px = rgba.load()
        w, h = rgba.size
        for y in range(h):
            for x in range(w):
                r, g, b, _ = px[x, y]
                cov = max(r, g, b)
                px[x, y] = (color[0], color[1], color[2], cov) if cov else (0, 0, 0, 0)
        return rgba
    arr = np.asarray(rgb_img.convert("RGB"))
    cov = arr.max(axis=2).astype("uint8")
    m = cov > 0
    rgba = np.zeros((arr.shape[0], arr.shape[1], 4), dtype="uint8")
    rgba[..., 0] = np.where(m, color[0], 0)
    rgba[..., 1] = np.where(m, color[1], 0)
    rgba[..., 2] = np.where(m, color[2], 0)
    rgba[..., 3] = cov
    return Image.fromarray(rgba, "RGBA")


def preview_projection(strip: Strip, w=PREVIEW_W, h=PREVIEW_H, ss=4, margin=6,
                       auto_orient=True, rotate=0, orient=None):
    """The projection half of render_preview, shared so anything that needs a
    span's position in the FINAL image uses EXACTLY the transform the PNG was
    drawn with.

    Returns (centers, segs, proj, uv, rot):
      centers  world-plane span centers (after any `orient` dihedral transform)
      segs     (main_segs, branch_segs, stitch_segs) from build_topology
      proj     the supersampled world -> pixel Projector
      uv       span index -> normalized (u,v) in the final image, top-left origin
      rot      the normalized rotate in [0,360)
    """
    centers, main_segs, branch_segs, stitch_segs = build_topology(strip)
    if orient is not None:
        tf = DIHEDRAL[orient % 8]
        centers = [tf(c) for c in centers]
        auto_orient = False
        rotate = 0
    used = {i for seg in (main_segs + branch_segs + stitch_segs) for i in seg}
    pts = [centers[i] for i in used] or centers
    proj = Projector(pts, w * ss, h * ss, margin * ss, auto_orient)
    ssw, ssh = float(w * ss), float(h * ss)
    rot = rotate % 360

    def uv(idx):
        if idx < 0 or idx >= len(centers):
            idx = 0
        px, py = proj(centers[idx])
        u, v = px / ssw, py / ssh
        # The final image is rotated AFTER projection, so rotate the normalized
        # coordinate the same way (90/270 also squeeze back to w x h, which this
        # normalized form already accounts for).
        if rot == 90:
            u, v = 1.0 - v, u
        elif rot == 180:
            u, v = 1.0 - u, 1.0 - v
        elif rot == 270:
            u, v = v, 1.0 - u
        return (min(1.0, max(0.0, u)), min(1.0, max(0.0, v)))

    return centers, (main_segs, branch_segs, stitch_segs), proj, uv, rot


def checkpoint_marks(uv, n_main, cp_spans, w=PREVIEW_W, h=PREVIEW_H, window=3):
    """[(span, u, v, tu, tv)] for each checkpoint that lies on the main ring.

    `cp_spans` is either a list of spans, or a list of (label_span,
    project_span) pairs -- the second form is what a REVERSE record needs: the
    span the race compares against is in reverse numbering, while the preview
    image is projected from the FORWARD strip, so the point is looked up at
    ring-1-span while the emitted label stays the race's own span.

    (tu,tv) is the UNIT local tangent in preview-IMAGE PIXEL space (not in
    normalized uv, which is anisotropic because the preview is 152x224). The
    frontend turns it 90 degrees to get the tick direction, after rescaling for
    the on-screen panel aspect. A checkpoint always sits on the race line, so a
    span outside [1, n_main) is dropped rather than guessed at."""
    out = []
    for item in cp_spans:
        if isinstance(item, (tuple, list)):
            label, sp = int(item[0]), int(item[1])
        else:
            label = sp = int(item)
        if sp <= 0 or sp >= n_main:
            continue
        a = max(0, sp - window)
        b = min(n_main - 1, sp + window)
        if b <= a:
            continue
        u, v = uv(sp)
        u0, v0 = uv(a)
        u1, v1 = uv(b)
        du = (u1 - u0) * w
        dv = (v1 - v0) * h
        m = math.hypot(du, dv)
        if m < 1e-6:          # degenerate neighbourhood: no usable tangent
            continue
        out.append((label, u, v, du / m, dv / m))
    return out


def render_preview(strip: Strip, w=PREVIEW_W, h=PREVIEW_H, ss=4,
                   color=RED, bg=BLACK, margin=6, auto_orient=True,
                   line_px=1.8, rotate=0, orient=None, return_markers=False,
                   start_idx=0):
    """Top-down red lane centerline (branch-aware) on a black background.

    `rotate` (0/90/180/270, degrees clockwise) is applied to the finished image.
    `orient` (0..7), if given, applies one of the 8 DIHEDRAL world-plane
    orientations before fitting (and disables auto_orient/rotate) -- used by the
    match-original search."""
    centers, segs, proj, uv, rotate = preview_projection(
        strip, w, h, ss, margin, auto_orient, rotate, orient)
    main_segs, branch_segs, stitch_segs = segs

    img = Image.new("RGB", (w * ss, h * ss), bg)
    d = ImageDraw.Draw(img)
    lw = max(1, round(line_px * ss))

    def draw(segs):
        for i, j in segs:
            d.line([proj(centers[i]), proj(centers[j])], fill=color, width=lw)

    # all centerline (main + branches + fork stitches) in one red colour
    draw(main_segs)
    draw(branch_segs)
    draw(stitch_segs)

    out = img.resize((w, h), Image.LANCZOS)
    rotate %= 360
    if rotate == 180:
        out = out.transpose(Image.ROTATE_180)
    elif rotate in (90, 270):
        # ROTATE_90 is counter-clockwise in PIL; we want clockwise degrees.
        out = out.transpose(Image.ROTATE_270 if rotate == 90 else Image.ROTATE_90)
        if out.size != (w, h):           # square-ize back to target
            out = out.resize((w, h), Image.LANCZOS)

    # Bake a TRANSPARENT background (RGBA), matching the original trak%04d.png
    # which ship as RGBA with alpha=0 on the black background. The frontend's
    # runtime black colorkey does NOT reliably transparent-out a plain RGB
    # preview (the originals work because the alpha is in the file), so we put it
    # there: alpha = line coverage, RGB = pure red on the covered texels, black
    # elsewhere. Anti-aliased edges keep partial alpha for a smooth line.
    out = _bake_transparent(out, color)

    if not return_markers:
        return out

    # Normalized (u,v) of the track start (span 0) and end (last main span) in
    # the FINAL image (top-left origin, 0..1). Same projection as the drawn
    # preview, so the frontend can overlay start/finish dots that line up.
    start_uv = uv(start_idx)
    end_uv = uv(strip.span_count_main - 1)
    return out, (start_uv, end_uv)


def render_debug(strip: Strip, w=900, h=1100, margin=24, auto_orient=True):
    """High-res annotated reference: gray edges, red main centerline, orange
    branch centerline, fork stitches, junction dots, start marker."""
    centers, main_segs, branch_segs, stitch_segs = build_topology(strip)
    lefts, rights = edge_polylines(strip)
    allpts = [p for p in centers if p] + [p for p in lefts if p] + [p for p in rights if p]
    proj = Projector(allpts, w, h, margin, auto_orient)

    img = Image.new("RGB", (w, h), (16, 16, 28))
    d = ImageDraw.Draw(img)

    def poly_seg(pts_list, color, width):
        for i in range(len(pts_list) - 1):
            a, b = pts_list[i], pts_list[i + 1]
            if a and b:
                # skip teleports between disjoint chains
                if math.hypot(a[0] - b[0], a[1] - b[1]) > 1e6:
                    continue
                d.line([proj(a), proj(b)], fill=color, width=width)

    poly_seg(lefts[:strip.span_count_main], (70, 90, 140), 1)
    poly_seg(rights[:strip.span_count_main], (70, 90, 140), 1)
    for i, j in stitch_segs:
        d.line([proj(centers[i]), proj(centers[j])], fill=(120, 120, 120), width=2)
    for i, j in branch_segs:
        d.line([proj(centers[i]), proj(centers[j])], fill=(255, 160, 0), width=3)
    for i, j in main_segs:
        d.line([proj(centers[i]), proj(centers[j])], fill=(255, 40, 40), width=3)

    for sp in strip.spans:
        if sp.span_type in (JUNCTION_FWD, JUNCTION_BWD, SENTINEL_FWD, SENTINEL_BWD):
            cx, cy = proj(centers[sp.index])
            col = (0, 220, 255) if sp.span_type in (JUNCTION_FWD, JUNCTION_BWD) else (255, 255, 0)
            d.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], outline=col, width=2)

    if centers:
        sx, sy = proj(centers[0])
        d.ellipse([sx - 7, sy - 7, sx + 7, sy + 7], fill=(0, 255, 120), outline=(255, 255, 255))

    n_branch = strip.total_spans - strip.span_count_main
    d.text((margin, 8), f"spans: {strip.span_count_main} main + {n_branch} branch "
                        f"({strip.total_spans} total)", fill=(220, 220, 220))
    return img


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_render(args):
    strip = load_strip(args.input)
    img = render_preview(strip, w=args.width, h=args.height,
                         auto_orient=not args.no_auto_orient,
                         color=tuple(int(c) for c in args.color.split(",")),
                         line_px=args.line, rotate=args.rotate)
    img.save(args.out)
    n_branch = strip.total_spans - strip.span_count_main
    print(f"wrote {args.out} ({args.width}x{args.height}) "
          f"[{strip.span_count_main} main + {n_branch} branch spans]")


def cmd_debug(args):
    strip = load_strip(args.input)
    img = render_debug(strip, auto_orient=not args.no_auto_orient)
    img.save(args.out)
    n_branch = strip.total_spans - strip.span_count_main
    print(f"wrote {args.out} (debug) [{strip.span_count_main} main + {n_branch} branch spans]")


def _red_mask(img, dilate=5):
    """Boolean mask of line pixels, dilated for match tolerance. Uses alpha when
    present (both my RGBA output and the original RGBA previews), else red."""
    if img.mode in ("RGBA", "LA"):
        m = np.asarray(img.convert("RGBA"))[:, :, 3] > 96
    else:
        a = np.asarray(img.convert("RGB"), dtype=np.int16)
        m = (a[:, :, 0] > 96) & (a[:, :, 1] < 96) & (a[:, :, 2] < 96)
    mi = Image.fromarray((m * 255).astype("uint8"))
    if dilate > 1:
        mi = mi.filter(ImageFilter.MaxFilter(dilate))
    return np.asarray(mi) > 0


def _iou(a, b):
    union = (a | b).sum()
    return float((a & b).sum()) / float(union) if union else 0.0


def best_match_orientation(strip, orig_img, w=PREVIEW_W, h=PREVIEW_H):
    """Return (image, orient_index, score, markers) for the DIHEDRAL orientation
    whose red centerline overlaps the original preview best."""
    om = _red_mask(orig_img.convert("RGB").resize((w, h), Image.LANCZOS))
    best, best_o, best_s, best_mk = None, 0, -1.0, None
    for o in range(8):
        cand, mk = render_preview(strip, w=w, h=h, orient=o, return_markers=True)
        s = _iou(_red_mask(cand), om)
        if s > best_s:
            best, best_o, best_s, best_mk = cand, o, s, mk
    return best, best_o, best_s, best_mk


def load_levelinf_json(ldir):
    """The editable LEVELINF source (levelinf.json) as a plain {field: value}
    dict. Each field is stored annotated ({"value": ..., "offset": ...}), so
    unwrap it. Returns None when the file is absent."""
    p = os.path.join(ldir, "levelinf.json")
    if not os.path.isfile(p):
        return None
    with open(p, "r", encoding="utf-8") as f:
        doc = json.load(f)
    out = {}
    for k, v in doc.items():
        if k.startswith("_"):
            continue
        out[k] = v.get("value") if isinstance(v, dict) else v
    return out


def level_checkpoint_spans(ldir):
    """The track's LEVELINF checkpoint_count + checkpoint_spans[7] (0x08 / 0x0C).
    The array is zero-padded beyond the count, so trailing zeros are dropped.
    Returns [] when the level has no checkpoints or no levelinf.

    [H5 2026-10-02] NOT the race's checkpoint source for a shipped TD5 track --
    use race_checkpoint_record(). The original reads +0x08 only as an ENABLE
    flag and takes the thresholds from the exe's own record table; this array
    additionally leads with the START span, so driving preview ticks off it drew
    checkpoints the race does not have. Kept because it is still how a custom /
    auto-generated level declares its own checkpoints."""
    inf = load_levelinf_json(ldir)
    if inf is None:
        p = os.path.join(ldir, "levelinf.dat")
        if not os.path.isfile(p):
            return []
        with open(p, "rb") as f:
            d = f.read(0x28)
        if len(d) < 0x28:
            return []
        n = struct.unpack_from("<I", d, 0x08)[0]
        spans = list(struct.unpack_from("<7I", d, 0x0C))
    else:
        n = int(inf.get("checkpoint_count") or 0)
        spans = list(inf.get("checkpoint_spans") or [])
    if n <= 0:
        return []
    return [int(s) for s in spans[:min(n, 7)] if int(s) > 0]


def level_circuit_flag(ldir):
    """Authoritative circuit flag from LEVELINF.DAT DWORD[0] (==1 circuit, ==0
    P2P; confirmed @ 0x42AE6B). Returns True/False, or None if unavailable."""
    inf = load_levelinf_json(ldir)
    if inf is not None and inf.get("track_type") is not None:
        return int(inf["track_type"]) == 1
    p = os.path.join(ldir, "levelinf.dat")
    if not os.path.isfile(p):
        for alt in ("LEVELINF.DAT", "Levelinf.dat"):
            ap = os.path.join(ldir, alt)
            if os.path.isfile(ap):
                p = ap
                break
        else:
            return None
    with open(p, "rb") as f:
        d = f.read(4)
    return struct.unpack("<i", d)[0] == 1 if len(d) == 4 else None


def is_circuit(strip):
    """Geometry circuit test (fallback when LEVELINF.DAT is absent, e.g. a TD6
    track): the main ring's first and last span centers meet."""
    centers = [span_center(sp, strip.verts) for sp in strip.spans]
    n = strip.span_count_main
    if n < 3:
        return False
    steps = [math.hypot(centers[i + 1][0] - centers[i][0],
                        centers[i + 1][1] - centers[i][1]) for i in range(n - 1)]
    med = statistics.median(steps) if steps else 1.0
    a, b = centers[0], centers[n - 1]
    return math.hypot(a[0] - b[0], a[1] - b[1]) <= med * 6.0


def cp_json(marks):
    """Serialize checkpoint_marks() output. `span` is the span the RACE compares
    against (reverse numbering on a reverse record), so the runtime can check
    each tick against td5_game_track_checkpoint_record() and refuse to draw one
    the race does not have."""
    return [
        {"span": int(sp), "u": round(u, 6), "v": round(v, 6),
         "tu": round(tu, 6), "tv": round(tv, 6)}
        for (sp, u, v, tu, tv) in marks
    ]


def markers_to_entries(markers, index_key, index_base, name_map, count):
    """markers: {index: ((su,sv),(eu,ev),circuit[,checkpoints[,checkpoints_rev]])}
    keyed by absolute index (pool for TD5, tga for TD6). Produce the JSON entry
    list for indices in [index_base, index_base+count) that actually have data
    (zero/placeholder slots are omitted). `checkpoints` /`checkpoints_rev`, when
    present, are checkpoint_marks() lists emitted as optional arrays."""
    entries = []
    for i in range(count):
        key = index_base + i
        m = markers.get(key)
        if not m:
            continue
        cps = cps_rev = None
        if len(m) == 5:
            (su, sv), (eu, ev), circ, cps, cps_rev = m
        elif len(m) == 4:
            (su, sv), (eu, ev), circ, cps = m
        else:
            (su, sv), (eu, ev), circ = m
        e = {index_key: key}
        nm = name_map.get(key) if name_map else None
        if nm:
            e["name"] = nm
        e["start_u"] = round(float(su), 6)
        e["start_v"] = round(float(sv), 6)
        e["end_u"] = round(float(eu), 6)
        e["end_v"] = round(float(ev), 6)
        e["circuit"] = 1 if circ else 0
        if cps:
            e["checkpoints"] = cp_json(cps)
        if cps_rev:
            e["checkpoints_rev"] = cp_json(cps_rev)
        entries.append(e)
    return entries


def write_markers_json(path, entries):
    """Editable replacement for the retired TMK1 .dat. The runtime loader
    (td5_frontend.c frontend_load_track_markers / _td6) parses this directly
    via cJSON, placing each entry by its 'pool' (TD5) / 'tga' (TD6) field."""
    doc = {
        "_format": "td5_track_markers",
        "_version": 1,
        "_note": ("Track-preview start/finish dots. u,v are normalized 0..1 in "
                  "the 152x224 preview, top-left origin. circuit=1 -> single "
                  "start/finish dot; circuit=0 -> separate start + end dots that "
                  "swap with the Forwards/Backwards toggle. Indexed by 'pool' "
                  "(TD5 trak TGA 0..19) or 'tga' (TD6 preview TGA >=90). "
                  "Generated by re/tools/track_preview_render.py."),
        "markers": entries,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")


def read_markers_dat(path):
    """Read a legacy TMK1 .dat -> {file_slot: ((su,sv),(eu,ev),circuit)}.
    All-zero placeholder slots are skipped. file_slot is the record's index in
    the file (== pool for TD5; == tga-TD6_PREVIEW_BASE for TD6)."""
    out = {}
    with open(path, "rb") as f:
        if f.read(4) != b"TMK1":
            raise ValueError(f"{path}: not a TMK1 file")
        (count,) = struct.unpack("<I", f.read(4))
        for i in range(count):
            rec = f.read(20)
            if len(rec) < 20:
                break
            su, sv, eu, ev, circ = struct.unpack("<ffffBxxx", rec)
            if su == 0.0 and sv == 0.0 and eu == 0.0 and ev == 0.0 and circ == 0:
                continue  # zero-filled placeholder slot
            out[i] = ((su, sv), (eu, ev), circ)
    return out


def cmd_render_all(args):
    assets = args.assets
    tracks_dir = os.path.join(assets, "tracks")
    levels_dir = os.path.join(assets, "levels")
    if not os.path.isdir(tracks_dir):
        sys.exit(f"no tracks dir: {tracks_dir}")

    # back up originals once
    if args.backup:
        bdir = os.path.join(tracks_dir, "_orig_backup")
        os.makedirs(bdir, exist_ok=True)
        for pool in range(20):
            src = os.path.join(tracks_dir, f"trak{pool:04d}.png")
            dst = os.path.join(bdir, f"trak{pool:04d}.png")
            if os.path.isfile(src) and not os.path.isfile(dst):
                with open(src, "rb") as fi, open(dst, "wb") as fo:
                    fo.write(fi.read())
        print(f"backed up originals -> {bdir}")

    ok = 0
    markers = {}
    for pool in range(20):
        lvl = POOL_TO_LEVEL.get(pool)
        name = POOL_TO_NAME.get(pool, f"pool{pool}")
        if lvl is None:
            print(f"  trak{pool:04d}: no level mapping, skipped")
            continue
        ldir = os.path.join(levels_dir, f"level{lvl:03d}")
        if not any(os.path.isfile(os.path.join(ldir, c))
                   for c in ("strip.dat", "strip.json")):
            print(f"  trak{pool:04d} ({name}): no strip.dat / strip.json in "
                  f"{ldir}, skipped")
            continue
        if args.match and not os.path.isdir(os.path.join(tracks_dir, "_orig_backup")):
            sys.exit("REFUSING: --match needs tracks/_orig_backup (the 1999 art) "
                     "to pick each track's orientation, and it is not there. "
                     "Re-rendering without it would orient the previews "
                     "differently from the ones on disk and move every marker. "
                     "Pass --no-match only if you intend that.")
        try:
            strip = load_strip(ldir)
            out = os.path.join(tracks_dir, f"trak{pool:04d}.png")
            nb = strip.total_spans - strip.span_count_main
            circ = level_circuit_flag(ldir)
            if circ is None:
                circ = is_circuit(strip)
            orig_path = os.path.join(tracks_dir, "_orig_backup", f"trak{pool:04d}.png")
            tag = ""
            if args.match and np is not None and os.path.isfile(orig_path):
                # closest match to the original 1999 art (per-track orientation)
                with Image.open(orig_path) as orig:
                    img, o, s, mk = best_match_orientation(strip, orig)
                tag = f"  match=orient{o} iou={s:.2f}"
                proj_kw = dict(orient=o)
            else:
                img, mk = render_preview(strip, auto_orient=not args.no_auto_orient,
                                         rotate=args.rotate, return_markers=True)
                proj_kw = dict(auto_orient=not args.no_auto_orient,
                               rotate=args.rotate)
            img.save(out)
            # [W3 2026-09-29] Checkpoint ticks alongside the start/finish dots,
            # from the SAME projection this preview was just drawn with, so a
            # re-render never silently drops them from the marker file.
            # [H5 2026-10-02] ...and sourced from the RACE's checkpoint record
            # (both directions), not from LEVELINF -- see the table block at the
            # top of this file.
            _c, _s2, _p, uv, _r = preview_projection(strip, **proj_kw)
            ring = strip.span_count_main
            cps = checkpoint_marks(uv, ring, [
                sp for (_i, sp) in drawable_checkpoints(
                    race_checkpoint_record(pool, False), ring, bool(circ))])
            cps_rev = []
            if 0 <= pool < len(POOL_SPAN_COUNT_REV) and POOL_SPAN_COUNT_REV[pool] >= 0:
                cps_rev = checkpoint_marks(uv, ring, [
                    (sp, ring - 1 - sp) for (_i, sp) in drawable_checkpoints(
                        race_checkpoint_record(pool, True), ring, bool(circ))])
            markers[pool] = (mk[0], mk[1], circ, cps, cps_rev)
            print(f"  trak{pool:04d} <- level{lvl:03d} {name:14s} "
                  f"({strip.span_count_main} main + {nb} branch){tag}"
                  f"  {'circuit' if circ else 'P2P'}")
            ok += 1
        except Exception as e:
            print(f"  trak{pool:04d} ({name}): ERROR {e}")
    markers_path = os.path.join(tracks_dir, "trak_markers.json")
    entries = markers_to_entries(markers, "pool", 0, POOL_TO_NAME, 20)
    write_markers_json(markers_path, entries)
    print(f"regenerated {ok}/20 previews in {tracks_dir}")
    print(f"wrote start/finish markers ({len(entries)} entries) -> {markers_path}")


# ---------------------------------------------------------------------------
# Migrated TD6 tracks
# ---------------------------------------------------------------------------
# Mirror of the runtime TD6 registry (td5_asset.c k_td6_slots + td5_frontend.c
# s_track_schedule_to_tga_index / display names). One row per migrated TD6 track:
#   (converted TD5 level number, preview TGA number, race start span, circuit?, name)
# The preview TGA number must match s_track_schedule_to_tga_index[slot]; markers
# are keyed by (tga - TD6_PREVIEW_BASE) to match the C loader. Extend as tracks
# are migrated. start_span = [Game] OverrideStartSpan for that track (the grid /
# start-finish line), so the preview dot lands where the car actually starts.
TD6_PREVIEW_BASE = 90
TD6_TRACKS = [
    # (converted TD5 level, preview tga, start span, circuit?, name)
    ( 7, 90, 312, True, "PELTON RACEWAY"),   # TD6 level010 (circuit)
    (18, 91,  70, True, "IRELAND"),          # TD6 level011 (circuit)
    (19, 92,  32, True, "LAKE TAHOE"),       # TD6 level015 (circuit)
    (20, 93, 371, True, "CAPE HATTERAS"),    # TD6 level016 (circuit)
    (21, 94, 346, True, "SWITZERLAND"),      # TD6 level017 (circuit)
    (22, 95,  10, True, "EGYPT"),            # TD6 level018 (circuit)
    ( 8, 96,  20, False, "PARIS"),           # TD6 level000 (P2P)
    ( 9, 97,  20, False, "NEW YORK"),        # TD6 level001 (P2P)
    (10, 98,  20, False, "ROME"),            # TD6 level002 (P2P)
    (11, 99,  20, False, "HONG KONG"),       # TD6 level003 (P2P)
    (12,100,  20, False, "LONDON"),          # TD6 level004 (P2P)
]


def cmd_render_td6(args):
    """Render migrated TD6 track previews + a separate start/finish marker file
    (trak_markers_td6.dat). Same projection as the PNG, so dots line up. The
    start dot uses the real race start span, not span 0."""
    tracks_dir = os.path.join(args.assets, "tracks")
    levels_dir = os.path.join(args.assets, "levels")
    if not os.path.isdir(tracks_dir):
        sys.exit(f"no tracks dir: {tracks_dir}")
    markers = {}
    for (lvl, tga, start_span, circuit, name) in TD6_TRACKS:
        ldir = os.path.join(levels_dir, f"level{lvl:03d}")
        try:
            strip = load_strip(ldir)
        except Exception as e:
            print(f"  trak{tga:04d} ({name}): ERROR {e}")
            continue
        si = start_span if 0 <= start_span < strip.span_count_main else 0
        img, mk = render_preview(strip, auto_orient=not args.no_auto_orient,
                                 rotate=args.rotate, return_markers=True, start_idx=si)
        out = os.path.join(tracks_dir, f"trak{tga:04d}.png")
        img.save(out)
        # [W3 2026-09-29] Same projection -> checkpoint ticks. TD6 levelinf
        # carries checkpoint_count 0, so the spans come from the mirror of
        # td5_asset_td6_checkpoint_spans (circuits legitimately have none).
        _c, _s2, _p, uv, _r = preview_projection(
            strip, auto_orient=not args.no_auto_orient, rotate=args.rotate)
        cps = checkpoint_marks(uv, strip.span_count_main,
                               TD6_CHECKPOINT_SPANS.get(lvl, []))
        markers[tga] = (mk[0], mk[1], circuit, cps)
        print(f"  trak{tga:04d} <- level{lvl:03d} {name:16s} start_span={si} "
              f"({strip.span_count_main} main)  {'circuit' if circuit else 'P2P'}")
    n = max((t - TD6_PREVIEW_BASE for t in markers), default=-1) + 1
    td6_names = {tga: name for (_lvl, tga, _ss, _circ, name) in TD6_TRACKS}
    path = os.path.join(tracks_dir, "trak_markers_td6.json")
    entries = markers_to_entries(markers, "tga", TD6_PREVIEW_BASE, td6_names, n)
    write_markers_json(path, entries)
    print(f"wrote TD6 start/finish markers ({len(entries)} entries) -> {path}")


# Mirror of td5_asset.c td5_asset_td6_checkpoint_spans (keyed by CONVERTED TD5
# level number). The migrated TD6 levelinf carries checkpoint_count = 0, so the
# spans only exist in that table -- keep the two in sync by hand. The 6 TD6
# circuits are lap-based and have no checkpoints.
TD6_CHECKPOINT_SPANS = {
     8: [641, 1113, 1685, 2211],        # PARIS
     9: [600, 1008, 1619, 1998],        # NEW YORK
    10: [51, 505, 1056, 1500, 1838],    # ROME
    11: [540, 832, 1196, 1567],         # HONG KONG
    12: [515, 906, 1289, 1692],         # LONDON
}

# How far the recovered start/end u,v may drift from the stored values before we
# refuse to attach ticks. The JSON rounds to 6 decimals, so an exact match lands
# around 1e-6; 1e-3 is generous and still far below "wrong orientation" (those
# residuals are 0.5+, see the orientation table).
ORIENT_MATCH_TOL = 1e-3


def recover_uv_mapper(strip, entry, start_idx):
    """Find the projection that produced an EXISTING trak_markers entry.

    The previews are already on disk and correct; we only want to add ticks to
    them. Rather than assume which orientation flags the entry was rendered with
    (they differ between the TD5 `--match` search and the TD6 auto-orient path,
    and the _orig_backup that drove `--match` is long gone), reproduce the stored
    start/end u,v with every candidate projection and keep the best. Returns
    (uv, residual) -- residual is the L1 error over the four stored values, so
    the caller can refuse anything that did not actually match."""
    want = (float(entry.get("start_u", 0.0)), float(entry.get("start_v", 0.0)),
            float(entry.get("end_u", 0.0)), float(entry.get("end_v", 0.0)))
    end_idx = strip.span_count_main - 1
    cands = [dict(orient=o) for o in range(8)]
    for r in (0, 90, 180, 270):
        cands.append(dict(orient=None, auto_orient=True, rotate=r))
        cands.append(dict(orient=None, auto_orient=False, rotate=r))
    best_uv, best_d = None, float("inf")
    for kw in cands:
        _c, _s, _p, uv, _r = preview_projection(
            strip, auto_orient=kw.get("auto_orient", True),
            rotate=kw.get("rotate", 0), orient=kw.get("orient"))
        su, sv = uv(start_idx)
        eu, ev = uv(end_idx)
        d = (abs(su - want[0]) + abs(sv - want[1]) +
             abs(eu - want[2]) + abs(ev - want[3]))
        if d < best_d:
            best_uv, best_d = uv, d
    return best_uv, best_d


def cmd_checkpoints(args):
    """Add per-checkpoint tick marks to the EXISTING trak_markers*.json, in place.

    Deliberately does NOT re-render any PNG and does NOT recompute start/end
    dots or circuit flags: those are shipped art + shipped data and must not move.
    For each entry the orientation is RECOVERED from its own stored start/end u,v
    (see recover_uv_mapper), so a tick lands on the same red line the PNG draws.
    An entry whose orientation cannot be reproduced is reported and left alone."""
    tracks_dir = os.path.join(args.assets, "tracks")
    levels_dir = os.path.join(args.assets, "levels")
    td6_by_tga = {tga: (lvl, ss) for (lvl, tga, ss, _c, _n) in TD6_TRACKS}

    for fname, index_key in (("trak_markers.json", "pool"),
                             ("trak_markers_td6.json", "tga")):
        path = os.path.join(tracks_dir, fname)
        if not os.path.isfile(path):
            print(f"skip (absent): {path}")
            continue
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
        total_cp = 0
        for e in doc.get("markers", []):
            key = int(e[index_key])
            if index_key == "pool":
                lvl, start_idx = POOL_TO_LEVEL.get(key), 0
                cp_src = None
            else:
                lvl, start_idx = td6_by_tga.get(key, (None, 0))
                cp_src = TD6_CHECKPOINT_SPANS.get(lvl, [])
            name = e.get("name", str(key))
            if lvl is None:
                print(f"  {index_key} {key} ({name}): no level mapping, skipped")
                continue
            ldir = os.path.join(levels_dir, f"level{lvl:03d}")
            try:
                strip = load_strip(ldir)
            except Exception as ex:
                print(f"  {index_key} {key} ({name}): ERROR {ex}")
                continue
            ring = strip.span_count_main
            circuit = bool(e.get("circuit"))
            # [H5 2026-10-02] Source = the RACE's checkpoint record, not
            # LEVELINF. See the table block at the top of this file for why.
            if cp_src is not None:
                # TD6: synthesized banner spans, all intermediate (a TD6 P2P
                # track finishes on its own separate finish span, and the
                # circuits ship none), so nothing is dropped beyond the
                # out-of-ring guard inside checkpoint_marks.
                fwd_items, rev_items = list(cp_src), []
                record = list(cp_src)
            else:
                record = race_checkpoint_record(key, reverse=False)
                fwd_items = [sp for (_i, sp)
                             in drawable_checkpoints(record, ring, circuit)]
                # A reverse race runs its OWN record, in REVERSE span numbering,
                # while this preview image is projected from the forward strip:
                # label with the race's span, project at ring-1-span. Tracks
                # with no reverse record keep the forward one at race time too,
                # so they emit no separate list and the runtime reuses 'checkpoints'.
                rev_items = []
                if 0 <= key < len(POOL_SPAN_COUNT_REV) and POOL_SPAN_COUNT_REV[key] >= 0:
                    rev_rec = race_checkpoint_record(key, reverse=True)
                    rev_items = [(sp, ring - 1 - sp) for (_i, sp)
                                 in drawable_checkpoints(rev_rec, ring, circuit)]
            if not fwd_items and not rev_items:
                e.pop("checkpoints", None)
                e.pop("checkpoints_rev", None)
                print(f"  {index_key} {key:3d} {name:16s} no ticks "
                      f"(record {record} -> all start/finish or out of ring {ring})")
                continue
            uv, resid = recover_uv_mapper(strip, e, start_idx)
            if resid > ORIENT_MATCH_TOL:
                print(f"  {index_key} {key:3d} {name:16s} ORIENTATION NOT "
                      f"RECOVERED (residual {resid:.4f}) -- left unchanged")
                continue
            marks = checkpoint_marks(uv, ring, fwd_items)
            e["checkpoints"] = cp_json(marks)
            total_cp += len(marks)
            if rev_items:
                rmarks = checkpoint_marks(uv, ring, rev_items)
                e["checkpoints_rev"] = cp_json(rmarks)
                total_cp += len(rmarks)
            else:
                e.pop("checkpoints_rev", None)
            print(f"  {index_key} {key:3d} {name:16s} {len(marks)} ticks "
                  f"{[sp for (sp, *_r) in marks]} of record {record} "
                  f"(ring {ring}, {'circuit' if circuit else 'P2P'}"
                  f"{', +%d reverse' % len(rev_items) if rev_items else ''}"
                  f", resid {resid:.2e})")
        cp_note = ("'checkpoints' holds each checkpoint's u,v plus the unit "
                   "local tangent tu,tv in 152x224 image pixels; the frontend "
                   "draws a white tick perpendicular to it.")
        note = doc.get("_note", "")
        if cp_note not in note:                     # re-runnable: append once
            doc["_note"] = (note + " " + cp_note) if note else cp_note
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
        print(f"wrote {total_cp} checkpoint ticks -> {path}")


def cmd_dat2json(args):
    """One-time migration: convert the legacy trak_markers*.dat (TMK1) files to
    the editable JSON the runtime now reads, preserving the exact current values
    (no re-render needed, so it works even after STRIP.DAT retirement)."""
    tracks_dir = os.path.join(args.assets, "tracks")
    # TD5: file slot == pool index (0..19)
    td5_dat = os.path.join(tracks_dir, "trak_markers.dat")
    if os.path.isfile(td5_dat):
        raw = read_markers_dat(td5_dat)
        entries = markers_to_entries(raw, "pool", 0, POOL_TO_NAME, 20)
        out = os.path.join(tracks_dir, "trak_markers.json")
        write_markers_json(out, entries)
        print(f"{td5_dat} -> {out}  ({len(entries)} entries)")
    else:
        print(f"skip (absent): {td5_dat}")
    # TD6: file slot i == tga (TD6_PREVIEW_BASE + i)
    td6_dat = os.path.join(tracks_dir, "trak_markers_td6.dat")
    if os.path.isfile(td6_dat):
        raw = read_markers_dat(td6_dat)
        markers = {TD6_PREVIEW_BASE + i: v for i, v in raw.items()}
        td6_names = {tga: name for (_lvl, tga, _ss, _circ, name) in TD6_TRACKS}
        n = max((t - TD6_PREVIEW_BASE for t in markers), default=-1) + 1
        entries = markers_to_entries(markers, "tga", TD6_PREVIEW_BASE, td6_names, n)
        out = os.path.join(tracks_dir, "trak_markers_td6.json")
        write_markers_json(out, entries)
        print(f"{td6_dat} -> {out}  ({len(entries)} entries)")
    else:
        print(f"skip (absent): {td6_dat}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("render", help="render one track preview PNG")
    r.add_argument("input", help="level dir, strip.dat, or level zip")
    r.add_argument("--out", default="track_preview.png")
    r.add_argument("--width", type=int, default=PREVIEW_W)
    r.add_argument("--height", type=int, default=PREVIEW_H)
    r.add_argument("--color", default="255,0,0", help="line color R,G,B")
    r.add_argument("--line", type=float, default=1.8, help="line width (final px)")
    r.add_argument("--rotate", type=int, default=0, choices=(0, 90, 180, 270),
                   help="rotate finished image (deg clockwise)")
    r.add_argument("--no-auto-orient", action="store_true")
    r.set_defaults(func=cmd_render)

    dbg = sub.add_parser("debug", help="high-res annotated geometry reference")
    dbg.add_argument("input", help="level dir, strip.dat, or level zip")
    dbg.add_argument("--out", default="track_debug.png")
    dbg.add_argument("--no-auto-orient", action="store_true")
    dbg.set_defaults(func=cmd_debug)

    ra = sub.add_parser("render-all", help="regenerate all 20 frontend previews")
    ra.add_argument("--assets", default="re/assets", help="assets root (default re/assets)")
    ra.add_argument("--no-backup", dest="backup", action="store_false")
    ra.add_argument("--no-match", dest="match", action="store_false",
                   help="don't orient each preview to match the backed-up "
                        "original; use --rotate / auto-orient instead")
    ra.add_argument("--rotate", type=int, default=180, choices=(0, 90, 180, 270),
                   help="rotation used only when --no-match is set (deg cw)")
    ra.add_argument("--no-auto-orient", action="store_true")
    ra.set_defaults(func=cmd_render_all, backup=True, match=True)

    rt = sub.add_parser("render-td6",
                        help="render migrated TD6 track previews + start markers")
    rt.add_argument("--assets", default="re/assets", help="assets root (default re/assets)")
    rt.add_argument("--rotate", type=int, default=0, choices=(0, 90, 180, 270),
                    help="rotate finished image (deg cw)")
    rt.add_argument("--no-auto-orient", action="store_true")
    rt.set_defaults(func=cmd_render_td6)

    cp = sub.add_parser("checkpoints",
                        help="add checkpoint ticks to the existing "
                             "trak_markers*.json (no PNG re-render)")
    cp.add_argument("--assets", default="re/assets",
                    help="assets root (default re/assets)")
    cp.set_defaults(func=cmd_checkpoints)

    d2j = sub.add_parser("dat2json",
                         help="convert legacy trak_markers*.dat (TMK1) to JSON")
    d2j.add_argument("--assets", default="re/assets",
                     help="assets root (default re/assets)")
    d2j.set_defaults(func=cmd_dat2json)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
