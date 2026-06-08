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


def load_strip(path: str) -> Strip:
    """Accept a level dir, a raw strip.dat, or a level zip."""
    if os.path.isdir(path):
        for cand in ("strip.dat", "STRIP.DAT"):
            p = os.path.join(path, cand)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    return parse_strip(f.read())
        raise FileNotFoundError(f"no strip.dat in {path}")
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


def render_preview(strip: Strip, w=PREVIEW_W, h=PREVIEW_H, ss=4,
                   color=RED, bg=BLACK, margin=6, auto_orient=True,
                   line_px=1.8, rotate=0, orient=None, return_markers=False,
                   start_idx=0):
    """Top-down red lane centerline (branch-aware) on a black background.

    `rotate` (0/90/180/270, degrees clockwise) is applied to the finished image.
    `orient` (0..7), if given, applies one of the 8 DIHEDRAL world-plane
    orientations before fitting (and disables auto_orient/rotate) -- used by the
    match-original search."""
    centers, main_segs, branch_segs, stitch_segs = build_topology(strip)
    if orient is not None:
        tf = DIHEDRAL[orient % 8]
        centers = [tf(c) for c in centers]
        auto_orient = False
        rotate = 0
    used = {i for seg in (main_segs + branch_segs + stitch_segs) for i in seg}
    pts = [centers[i] for i in used] or centers
    proj = Projector(pts, w * ss, h * ss, margin * ss, auto_orient)

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
    ssw, ssh = float(w * ss), float(h * ss)

    def _nrm(idx):
        if idx < 0 or idx >= len(centers):
            idx = 0
        px, py = proj(centers[idx])
        return (px / ssw, py / ssh)

    def _rot(uv):
        u, v = uv
        if rotate == 90:
            u, v = 1.0 - v, u
        elif rotate == 180:
            u, v = 1.0 - u, 1.0 - v
        elif rotate == 270:
            u, v = v, 1.0 - u
        return (min(1.0, max(0.0, u)), min(1.0, max(0.0, v)))

    start_uv = _rot(_nrm(start_idx))
    end_uv = _rot(_nrm(strip.span_count_main - 1))
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


def level_circuit_flag(ldir):
    """Authoritative circuit flag from LEVELINF.DAT DWORD[0] (==1 circuit, ==0
    P2P; confirmed @ 0x42AE6B). Returns True/False, or None if unavailable."""
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


def markers_to_entries(markers, index_key, index_base, name_map, count):
    """markers: {index: ((su,sv),(eu,ev),circuit)} keyed by absolute index
    (pool for TD5, tga for TD6). Produce the JSON entry list for indices in
    [index_base, index_base+count) that actually have data (zero/placeholder
    slots are omitted)."""
    entries = []
    for i in range(count):
        key = index_base + i
        m = markers.get(key)
        if not m:
            continue
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
        sp_path = os.path.join(ldir, "strip.dat")
        if not os.path.isfile(sp_path):
            print(f"  trak{pool:04d} ({name}): missing {sp_path}, skipped")
            continue
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
            else:
                img, mk = render_preview(strip, auto_orient=not args.no_auto_orient,
                                         rotate=args.rotate, return_markers=True)
            img.save(out)
            markers[pool] = (mk[0], mk[1], circ)
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
        markers[tga] = (mk[0], mk[1], circuit)
        print(f"  trak{tga:04d} <- level{lvl:03d} {name:16s} start_span={si} "
              f"({strip.span_count_main} main)  {'circuit' if circuit else 'P2P'}")
    n = max((t - TD6_PREVIEW_BASE for t in markers), default=-1) + 1
    td6_names = {tga: name for (_lvl, tga, _ss, _circ, name) in TD6_TRACKS}
    path = os.path.join(tracks_dir, "trak_markers_td6.json")
    entries = markers_to_entries(markers, "tga", TD6_PREVIEW_BASE, td6_names, n)
    write_markers_json(path, entries)
    print(f"wrote TD6 start/finish markers ({len(entries)} entries) -> {path}")


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

    d2j = sub.add_parser("dat2json",
                         help="convert legacy trak_markers*.dat (TMK1) to JSON")
    d2j.add_argument("--assets", default="re/assets",
                     help="assets root (default re/assets)")
    d2j.set_defaults(func=cmd_dat2json)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
