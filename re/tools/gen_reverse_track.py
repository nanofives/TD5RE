#!/usr/bin/env python3
"""Offline reverse-direction asset generator for native TD5 CIRCUIT tracks.

Emits the three editable-source files the runtime needs to offer a reverse race
-- stripb.json (reverse geometry), leftb.trk.csv + rightb.trk.csv (reverse AI
corridor) -- into a level dir under re/assets/levels/levelNNN/. Once present,
td5_asset_track_has_reverse() (td5_asset.c) auto-returns true for that track and
the existing reverse pipeline (texture swap, minimap/render span math, span
progress) engages: NO runtime/hot-path change.

WHY this is needed
------------------
The original game only shipped authored STRIPB/LEFTB/RIGHTB for the 12
point-to-point tracks. All 7 native CIRCUIT tracks ship forward-only
(strip.dat/left.trk/right.trk only -- verified in original/level0{25..29,37,39}.zip),
so reverse was impossible. A circuit is a closed ring, so its reverse geometry
IS derivable from the forward strip -- this tool does that derivation.

REQUIRES a one-line engine companion change (already in td5_game.c): the
reverse-circuit start-span mirror (rev_ss = ring - fwd_ss) must fire for native
circuits, not just TD6 ones, plus the position-independent circuit lap logic for
reverse circuits whose mirrored start sits past the ring midpoint. See td5_game.c
"reverse circuit start span" and "NATIVE REVERSE CIRCUIT".

THE RING TRANSFORM (validated functionally on level025/Cheddar -- loads, grids on
the start straight, drives smooth full laps, lap counter increments, race
finishes):

  rev[k] = fwd[(R - k) mod R]   (R = header[1] = main-ring span count)

  Reverse the ring as a SHARED-row ribbon matching the forward STRIP.DAT layout
  exactly -- span i's FAR row IS span i+1's NEAR row (contiguous; RVI_i==LVI_i+1).
  Each row is laterally reversed (left<->right) so the surface winding is correct
  for the reversed travel direction.

  ORIGIN-BLOCK HANDLING (critical). Vertices are int16 RELATIVE to a per-span
  origin that changes in blocks. Forward shares a boundary row between two spans
  ONLY when they have the same origin; at an origin boundary it emits a SEPARATE
  far row re-based into the earlier span's frame (verified: RVI != next.LVI at
  every origin change, never within a block). A naive always-shared ribbon reads
  the boundary row with two origins -> a 32768-unit vertical seam -> the car gets
  launched. Two independent rows per span instead leaves REDUNDANT far-row
  surfaces the clip system rests the car on (spawn freeze). So we replicate
  forward: share within a block, re-base + duplicate the far row at each origin
  change. Result: world-continuous geometry, ~one row per span.

BRANCHES (junctions). Branched circuits store divergent corridors as extra spans
after the ring, joined by junction spans: a ring span (type 8) FWD-links to a
branch ENTRY (type 9); the branch EXIT (type 10) FWD-links to the ring REJOIN
(type 11). A header "jump table" (pre_span_hex, count @file 0x14, records @0x18
for native TD5: [lo,hi,base] u16) maps branch spans to their parallel ring spans
for route lookup. In reverse we:
  - reverse each branch corridor as an OPEN ribbon (same origin-aware logic),
  - SWAP fork<->merge: the forward rejoin R becomes the reverse departure, the
    forward departure D becomes the reverse rejoin, re-linked into the reversed
    ring numbering (revD = R - D mapped, etc.),
  - rebuild the jump-table records for the reversed branch span indices.
Routes (LEFTB/RIGHTB) are ring-only (forward routes have exactly R rows), so the
branch spans need no route rows.

Usage:
  python gen_reverse_track.py --circuits            # all 7 native circuits
  python gen_reverse_track.py re/assets/levels/level025 [more dirs...]
"""
import json, os, sys, math, struct
from collections import Counter

# span field indices in the JSON 11-tuple
# [type, surf, rs, packed, left_vtx_idx, right_vtx_idx, fwd_link, bwd_link, ox, oy, oz]
T, SURF, RS, PK, LVI, RVI, FWD, BWD, OX, OY, OZ = range(11)
FF = 0xFFFF

NATIVE_CIRCUIT_LEVELS = [25, 26, 27, 28, 29, 37, 39]
LEVELS_ROOT = "re/assets/levels"

# Fork-area experiments (off by default = faithful full-width geometry, both forks).
NARROW_FORKS = False
RIDGE_OFFSET = 0


# --------------------------------------------------------------------------- #
# Geometry reversal helpers (faithful per-span mirror of the forward strip)
# --------------------------------------------------------------------------- #
# Engine right-rail LUT (k_rail_lut_l/r in td5_track.c). A span's NEAR row right
# edge is vtx[LVI+rc+lut_l], its FAR row right edge vtx[RVI+rc+lut_r] (rc=lanes).
RAIL_LUT_L = (0, 0, -1, -1, -2, 0, 0, 0, 0, 0, 0, 0)
RAIL_LUT_R = (0, 0, 0, 0, 0, -1, -1, -2, 0, 0, 0, 0)


def _row_widths(spans, j):
    """Forward span j's NEAR and FAR row vertex counts, derived from the rail LUT.
    A uniform span is rc+1 / rc+1; a type-7 (narrow-2) span is rc+1 near, rc-1 far;
    a type-4 (widen-2) span is rc-1 near, rc+1 far; etc. This is how the engine
    itself reads the two rows, so mirroring these widths is byte-faithful."""
    s = spans[j]
    t = s[T] if 0 <= s[T] < 12 else 0
    rc = max(s[PK] & 0x0F, 1)
    return rc + RAIL_LUT_L[t] + 1, rc + RAIL_LUT_R[t] + 1


_WALL_WIDE_WARN = [0]   # spans whose |lane delta| > 2 (LUT can't express)

def _finish_span(s, w_near, w_far):
    """Set LVI/RVI, lane nibble, and the span TYPE so the engine's right-rail LUT
    (k_rail_lut_l/r in td5_track.c) lands on the span's real right edge.

    Right rail: NE=vtx[LVI+rc+lut_l[t]], SE=vtx[RVI+rc+lut_r[t]] (rc=lane_count).
    The LUT encodes the lane-count DELTA between the near and far rows:
        delta 0  -> type 1 (uniform)
        widen +1 -> type 2     widen +2 -> type 4
        narrow-1 -> type 5     narrow-2 -> type 7
    Emitting type 5 for a 2-lane narrowing (as before) made SE overrun the narrow
    far row -> the rail cut across the road = the invisible wall at junctions
    (where the road jumps 4->2 lanes for the branch). |delta|>2 can't be expressed
    by the LUT (max offset -2); those fall back to type 1 and are counted/warned."""
    s[RVI] = s[LVI] + w_near
    lanes = max(w_near, w_far) - 1
    s[PK] = (s[PK] & 0xF0) | (lanes & 0x0F)
    d = w_far - w_near
    if   d == 0:  s[T] = 1
    elif d == 1:  s[T] = 2     # widen 1 lane
    elif d == 2:  s[T] = 4     # widen 2 lanes
    elif d == -1: s[T] = 5     # narrow 1 lane
    elif d == -2: s[T] = 7     # narrow 2 lanes
    else:
        s[T] = 1               # |delta| > 2: LUT can't express it
        _WALL_WIDE_WARN[0] += 1
    s[FWD] = FF
    s[BWD] = FF


def _latflip(verts, base, w):
    """w vertices starting at `base`, lateral-reversed (left<->right)."""
    return [list(verts[base + t]) for t in range(w - 1, -1, -1)]


def _mirror_span(spans, verts, j, new_verts, shared):
    """Emit one reverse span mirroring forward span j: its NEAR row is the
    lateral-flip of j's FAR row and its FAR row the flip of j's NEAR row, copied
    VERBATIM in j's own origin frame (byte-faithful). `shared` is (base,width) of
    a row carried over as this span's near (None to emit fresh). Returns the new
    span record (links cleared; junction links set by the caller)."""
    wn_f, wf_f = _row_widths(spans, j)
    if shared is not None:
        near_base, w_near = shared
    else:
        row = _latflip(verts, spans[j][RVI], wf_f)      # forward FAR row -> reverse near
        near_base = len(new_verts); new_verts.extend(row); w_near = wf_f
    row = _latflip(verts, spans[j][LVI], wn_f)           # forward NEAR row -> reverse far
    far_base = len(new_verts); new_verts.extend(row); w_far = wn_f
    s = list(spans[j])
    s[LVI] = near_base
    _finish_span(s, w_near, w_far)
    return s, (far_base, w_far)


def reverse_ring(spans, verts, ring, new_verts):
    """Faithful per-span mirror of the forward ring. Reverse span k carries
    forward span j=(ring-1-k). Rows are SHARED exactly where forward shared them
    (LVI_j == RVI_{j-1}) and kept SEPARATE at forks/origin boundaries -- so the
    reverse inherits forward's connected, gap-0 geometry by construction instead
    of re-deriving (and corrupting) it. Closing span duplicates rev[0]."""
    out = []
    shared = None
    for k in range(ring):
        j = (ring - k) % ring                             # reverse span k carries fwd j
        s, far = _mirror_span(spans, verts, j, new_verts, shared)
        out.append(s)
        jprev = (j - 1) % ring                            # next reverse span carries fwd j-1
        shared = far if (k < ring - 1 and spans[j][LVI] == spans[jprev][RVI]) else None
    out.append(list(out[0]))
    return out


def reverse_corridor(spans, verts, E, X, new_verts):
    """Faithful per-span mirror of an OPEN branch corridor [E..X], driven X->E in
    reverse. Reverse span p carries forward span (X-p). out[0] = reverse entry (at
    the forward exit X), out[-1] = reverse exit (at the forward entry E)."""
    out = []
    shared = None
    L = X - E + 1
    for p in range(L):
        j = X - p
        s, far = _mirror_span(spans, verts, j, new_verts, shared)
        out.append(s)
        shared = far if (p < L - 1 and spans[j][LVI] == spans[j - 1][RVI]) else None
    return out


def taper_transitions(spans, verts):
    """Spread multi-lane (|delta|>=2) lane changes over consecutive spans so each
    span changes by at most 1 lane (rail-LUT type 1/2/5) and the lane ends
    GRADUALLY instead of as an abrupt collision wall.

    In reverse the branch sits on the LEFT (the lateral flip), so the branch
    lanes drop on the left. The engine's left rail follows vtx[+0], so a 1-lane
    drop tapers cleanly; a 2+-lane drop in ONE span makes the left rail jump
    inward across the lane = the invisible wall on the inner lane the user hit.
    We insert an extrapolated-left vertex into the shared boundary row to split
    the drop. Only touches CONTIGUOUS (shared) rows (RVI_k == LVI_{k+1}); a
    transition straddling an origin boundary is left alone (counted).

    Junction span types (8/9/10/11) are preserved; everything else is re-typed
    by its post-taper width delta."""
    def w(k):
        return spans[k][RVI] - spans[k][LVI]

    def shift(p, inclusive):
        # vertex inserted at index p. inclusive=False (front-of-row insert): the
        # new vertex IS the row's new first, so an index == p stays. inclusive=True
        # (back-of-row insert): the new vertex is the row's new last, so index == p
        # (the next row's start) must move to p+1.
        for s in spans:
            if s[LVI] > p or (inclusive and s[LVI] == p): s[LVI] += 1
            if s[RVI] > p or (inclusive and s[RVI] == p): s[RVI] += 1

    def d2(a, b):
        return (a[0] - b[0]) ** 2 + (a[2] - b[2]) ** 2

    skipped_boundary = [0]
    guard = 0
    changed = True
    while changed and guard < 5000:
        changed = False
        guard += 1
        for k in range(len(spans) - 1):
            if spans[k][RVI] != spans[k + 1][LVI]:
                continue                      # origin-boundary (separate rows)
            wn, wf = w(k), w(k + 1)
            d = wf - wn
            if abs(d) < 2:
                continue
            # narrow row = the row to widen; wide row = the neighbour it tapers to.
            #  drop -> narrow is span k's FAR row (=span k+1 near, base RVI_k),
            #          wide is span k's NEAR row (base LVI_k).
            #  rise -> narrow is span k's NEAR row (base LVI_k),
            #          wide is span k's FAR row (base RVI_k).
            if d <= -2:
                nb, nw, wb, ww = spans[k][RVI], wf, spans[k][LVI], wn
            else:
                nb, nw, wb, ww = spans[k][LVI], wn, spans[k][RVI], wf
            # Determine the BRANCH side by edge-matching: the array-end where the
            # narrow row diverges from the wide row is the side that loses/gains a
            # lane (the lateral flip means it can be either vtx[0] or vtx[-1]).
            if d2(verts[nb], verts[wb]) >= d2(verts[nb + nw - 1], verts[wb + ww - 1]):
                # branch at the vtx[0] (front) end -> extend left using wide row's
                # first lane vector; insert at the row's start.
                w0, w1 = verts[wb], verts[wb + 1]
                p = nb
                inclusive = False
            else:
                # branch at the vtx[-1] (back) end -> extend using wide row's last
                # lane vector; insert after the row's last vertex.
                w0, w1 = verts[wb + ww - 1], verts[wb + ww - 2]
                p = nb + nw
                inclusive = True
            anchor = verts[p if not inclusive else p - 1]
            verts.insert(p, [anchor[0] + (w0[0] - w1[0]),
                             anchor[1] + (w0[1] - w1[1]),
                             anchor[2] + (w0[2] - w1[2])])
            shift(p, inclusive)
            changed = True
            break

    # leftover |delta|>=2 spans straddle an origin boundary -> couldn't taper
    for k in range(len(spans) - 1):
        if spans[k][RVI] == spans[k + 1][LVI] and abs(w(k + 1) - w(k)) >= 2:
            skipped_boundary[0] += 1

    for k in range(len(spans) - 1):
        if spans[k][T] in (8, 9, 10, 11):
            continue
        wn, wf = w(k), w(k + 1)
        lanes = max(wn, wf) - 1
        spans[k][PK] = (spans[k][PK] & 0xF0) | (lanes & 0x0F)
        d = wf - wn
        spans[k][T] = (1 if d == 0 else 2 if d == 1 else 4 if d == 2
                       else 5 if d == -1 else 7 if d == -2 else 1)
    return skipped_boundary[0]


def find_branches(spans, ring):
    """Locate branches by junction links: ring span (type 8, FWD->entry),
    corridor entry(type 9)..exit(type 10), exit FWD->rejoin ring span (type 11)."""
    branches = []
    for i in range(ring):
        if spans[i][T] == 8 and spans[i][FWD] != FF:
            D, E = i, spans[i][FWD]
            x = E
            while x < len(spans) and spans[x][T] != 10:
                x += 1
            if x >= len(spans):
                continue
            X = x
            R = spans[X][FWD]
            branches.append({'D': D, 'E': E, 'X': X, 'R': R})
    return branches


def reverse_track(spans, verts, ring):
    """Reverse the ring + every branch; re-link junctions; return
    (all_spans, new_verts, jump_records)."""
    new_verts = []
    ring_spans = reverse_ring(spans, verts, ring, new_verts)   # indices 0..ring
    branches = find_branches(spans, ring)

    branch_spans = []
    jump_records = []
    base_idx = len(ring_spans)                                 # = ring + 1
    for br in branches:
        D, E, X, R = br['D'], br['E'], br['X'], br['R']
        # With the faithful per-span mirror (reverse span k carries forward span
        # ring-k, and reverse near = flip of forward FAR row), the reverse span at
        # forward R's location is ring-R and carries R's geometry directly. The
        # branch connects on that span's FAR edge (= flip of near(R), where the
        # branch merged in forward) -> type-8 semantics, NO off-by-one needed.
        revR = (ring - R) % ring         # forward rejoin    -> reverse DEPARTURE (type 8)
        revD = (ring - D) % ring         # forward departure -> reverse REJOIN   (type 11)
        rev_corr = reverse_corridor(spans, verts, E, X, new_verts)
        entry_gi = base_idx + len(branch_spans)
        exit_gi = entry_gi + len(rev_corr) - 1
        # reversed branch connectors
        rev_corr[0][T] = 9
        rev_corr[0][BWD] = revR
        rev_corr[-1][T] = 10
        rev_corr[-1][FWD] = revD
        # reversed ring junctions (fork<->merge swapped)
        ring_spans[revR][T] = 8
        ring_spans[revR][FWD] = entry_gi
        ring_spans[revD][T] = 11
        ring_spans[revD][BWD] = exit_gi
        # jump record: reversed branch spans [entry_gi, exit_gi] parallel ring
        # spans starting at revR (route-lookup normalization).
        jump_records.append((entry_gi, exit_gi, revR))
        branch_spans.extend(rev_corr)

    return ring_spans + branch_spans, new_verts, jump_records


# --------------------------------------------------------------------------- #
# Routes (ring-only) -- same build_routes() the forward / TD6 routes use
# --------------------------------------------------------------------------- #
def raise_fork_dividers(spans, verts, ring, offset):
    """Raise the Y of the main/branch DIVIDER vertices through each reverse fork's
    same-width (4-lane) run, giving the road a solid MEDIAN RIDGE. The car then
    physically cannot sit in the gore (the full-width centre line that drives
    straight into the divide) -- it stays on the main or branch lane, each of which
    flows nearly straight through the split. Collision rails use only X/Z, so the
    walls are unchanged; only the drivable surface gets the ridge. Returns the count
    of raised vertices."""
    raised = set()
    for i in range(ring):
        t = spans[i][T]
        if t not in (8, 11):
            continue
        lc = spans[i][PK] & 0x0F
        link = spans[i][FWD] if t == 8 else spans[i][BWD]
        brl = (spans[link][PK] & 0x0F) if (link != FF and 0 <= link < len(spans)) else 0
        if not (0 < brl < lc):
            continue
        lo = i
        while lo > 0 and (spans[lo - 1][PK] & 0x0F) == lc:
            lo -= 1
        hi = i
        while hi < ring - 1 and (spans[hi + 1][PK] & 0x0F) == lc:
            hi += 1
        for k in range(lo, hi + 1):
            # Near-row divider only: the contiguous near rows ARE the previous
            # span's far rows, so this covers the whole run as one continuous ridge
            # without touching the junction's SEPARATE far row (raising that made a
            # degenerate steep triangle -> physics blow-up).
            vi = spans[k][LVI] + brl
            if vi not in raised and 0 <= vi < len(verts):
                verts[vi][1] += offset
                raised.add(vi)
    return len(raised)


def narrow_fork_to_main(spans, ring):
    """DRIVE-THROUGH-FIRST: collapse each reverse fork's wide (4-lane) run down to
    just the MAIN (right) lanes so there is no gore for the car to stick in -- the
    ring becomes one clean continuous road through the junction. Each fork-region
    span is shifted to its right lanes (LVI/RVI += branch_lane_count, lane nibble
    reduced) and re-typed uniform; the junction's branch link is dropped (the
    branch corridor is left in place but unreachable for now). Returns span count
    touched."""
    touched = 0
    handled = set()
    for i in range(ring):
        t = spans[i][T]
        if t not in (8, 11) or i in handled:
            continue
        lc = spans[i][PK] & 0x0F
        link = spans[i][FWD] if t == 8 else spans[i][BWD]
        brl = (spans[link][PK] & 0x0F) if (link != FF and 0 <= link < len(spans)) else 0
        if not (0 < brl < lc):
            continue
        lo = i
        while lo > 0 and (spans[lo - 1][PK] & 0x0F) == lc:
            lo -= 1
        hi = i
        while hi < ring - 1 and (spans[hi + 1][PK] & 0x0F) == lc:
            hi += 1
        for k in range(lo, hi + 1):
            s = spans[k]
            kept = (s[PK] & 0x0F) - brl            # main lane count (right lanes)
            s[LVI] += brl
            s[RVI] += brl
            s[PK] = (s[PK] & 0xF0) | (kept & 0x0F)
            s[T] = 1
            s[FWD] = FF
            s[BWD] = FF
            handled.add(k)
            touched += 1
    return touched


def build_routes(spans, verts, ring, circuit=True):
    SHOULDER_FRAC = 0.25
    K = 3

    # Fork-region spans (4-lane runs around each type-8/11 junction). The AI route
    # heading is taken from the MAIN-side centre (the RIGHT lanes, past the divider)
    # so the racing line aims down the main lane and through the split -- NOT down
    # the full-width centre, which points straight into the gore where the car
    # sticks. fork_main[span] = divider lateral index (= branch lane count).
    fork_main = {}
    for i in range(ring):
        t = spans[i][T]
        if t not in (8, 11):
            continue
        lc = spans[i][PK] & 0x0F
        link = spans[i][FWD] if t == 8 else spans[i][BWD]
        brl = (spans[link][PK] & 0x0F) if (link != FF and 0 <= link < len(spans)) else 0
        if not (0 < brl < lc):
            continue
        lo = i
        while lo > 0 and (spans[lo - 1][PK] & 0x0F) == lc:
            lo -= 1
        hi = i
        while hi < ring - 1 and (spans[hi + 1][PK] & 0x0F) == lc:
            hi += 1
        for k in range(lo, hi + 1):
            fork_main[k] = brl

    def center(i):
        s = spans[i]
        if i in fork_main:                       # main-side (right lanes) centre
            lc = s[PK] & 0x0F
            lo_v, hi_v = s[LVI] + fork_main[i], s[LVI] + lc
            n = hi_v - lo_v + 1
            xs = sum(verts[v][0] for v in range(lo_v, hi_v + 1))
            zs = sum(verts[v][2] for v in range(lo_v, hi_v + 1))
            return (s[OX] + xs / n, s[OZ] + zs / n)
        lv = verts[s[LVI]]; rv = verts[s[RVI] - 1]   # near-row left & right rails
        return (s[OX] + (lv[0] + rv[0]) / 2.0, s[OZ] + (lv[2] + rv[2]) / 2.0)

    def heading_byte(s):
        if circuit:
            ax, az = center((s - K) % ring)
            bx, bz = center((s + K) % ring)
        else:
            lo = s - K if s >= K else 0
            hi = s + K if s + K <= ring - 1 else ring - 1
            if hi == lo:
                return 4
            ax, az = center(lo); bx, bz = center(hi)
        dx, dz = bx - ax, bz - az
        if dx == 0 and dz == 0:
            return 4
        h12 = int(round((math.atan2(dx, dz) / (2 * math.pi)) * 4096)) & 0xFFF
        return max(4, min(253, int(round(h12 * 256 / 0x102C))))

    left, right = [], []
    for s in range(ring):
        b = heading_byte(s)
        lanes = spans[s][PK] & 0x0F
        Ln = lanes if lanes >= 1 else 1
        sh = 0 if lanes <= 2 else max(1, int(round(lanes * SHOULDER_FRAC)))
        if s in fork_main:
            # Keep the AI corridor on the MAIN (right) lanes, clear of the divider
            # ridge: lateral byte 0=left edge .. 255=right edge, main = [brl/lc, 1].
            brl = fork_main[s]
            left_byte  = int(round((brl + 0.35) / Ln * 255))
            right_byte = 252
        else:
            left_byte = 128
            right_byte = max(left_byte + 8, min(255, int(round((Ln - sh) / Ln * 255))))
        left.append((left_byte, b, 255))
        right.append((right_byte, b, 255))
    return left, right


# --------------------------------------------------------------------------- #
# IO
# --------------------------------------------------------------------------- #
def patch_jump_table(pre_hex, jump_records):
    """Rewrite the branch jump table inside pre_span_hex: count (u32 LE) at byte
    0 (= file 0x14), then `count` records of 3x u16 LE (= file 0x18). The native
    record count is preserved (one branch in -> one branch out), so the table
    size is unchanged; only the values change. Everything after the records is
    kept verbatim."""
    pre = bytearray.fromhex(pre_hex)
    struct.pack_into('<I', pre, 0, len(jump_records))
    for i, (lo, hi, base) in enumerate(jump_records):
        struct.pack_into('<HHH', pre, 4 + i * 6, lo & 0xFFFF, hi & 0xFFFF, base & 0xFFFF)
    return pre.hex()


def write_strip_json(path, src_obj, rev_spans, rev_verts, ring, jump_records):
    """Emit stripb.json in the strip_tool.py layout (byte-faithful re-pack via
    td5_assetsrc Tier 2). Header recomputed (re-emitted vertex table); the branch
    jump table inside pre_span_hex is rewritten for the reversed branch spans."""
    pre_span_hex = patch_jump_table(src_obj["pre_span_hex"], jump_records)
    pre_span_n = len(pre_span_hex) // 2
    pre_vtx_n = len(src_obj["pre_vertex_hex"]) // 2
    nspans, nverts = len(rev_spans), len(rev_verts)
    span_off = 20 + pre_span_n
    vtx_off = span_off + 24 * nspans + pre_vtx_n
    hdr = [span_off, ring, vtx_off, nverts, nspans]
    sp = ",\n".join(json.dumps(s, separators=(",", ":")) for s in rev_spans)
    vx = ",\n".join(json.dumps(v, separators=(",", ":")) for v in rev_verts)
    text = (
        "{\n"
        '"_format":"td5_strip","_version":1,\n'
        f'"header":{json.dumps(hdr)},\n'
        f'"pre_span_hex":"{pre_span_hex}",\n'
        f'"spans":[\n{sp}\n],\n'
        f'"pre_vertex_hex":"{src_obj["pre_vertex_hex"]}",\n'
        f'"vertices":[\n{vx}\n],\n'
        f'"tail_hex":"{src_obj["tail_hex"]}"\n'
        "}\n"
    )
    open(path, "w", encoding="utf-8").write(text)


def write_route_csv(path, rows, name):
    lines = [f"# td5 reverse route ({name}) -- one row per span: lane0,lane1,lane2 (raw u8)"]
    lines += [f"{a},{b},{c}" for (a, b, c) in rows]
    open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")


def process(level_dir):
    sp_path = os.path.join(level_dir, "strip.json")
    if not os.path.isfile(sp_path):
        print(f"  {level_dir}: no strip.json -> skip"); return False
    obj = json.load(open(sp_path, encoding="utf-8"))
    spans = obj["spans"]; verts = obj["vertices"]
    ring = obj["header"][1]
    name = os.path.basename(level_dir)

    branches = find_branches(spans, ring)
    nontype1 = sum(1 for s in spans[:ring] if s[T] != 1)
    if nontype1:
        print(f"  {name}: NOTE {nontype1}/{ring} ring spans are non-type-1 "
              f"(LUT row offsets) -- re-emission assumes type-1 row width; verify.")

    rev_spans, rev_verts, jump_records = reverse_track(spans, verts, ring)
    # No taper: the faithful per-span mirror reproduces forward's transitions
    # (type-swapped) exactly, and forward already drives them correctly.
    if NARROW_FORKS:
        # DRIVE-THROUGH-FIRST: collapse forks to the main lanes (branch dropped).
        ntw = narrow_fork_to_main(rev_spans, ring)
        jump_records = []
        if ntw:
            print(f"  {name}: narrowed {ntw} fork-region spans to main lanes "
                  f"(branch dropped)")
    elif RIDGE_OFFSET:
        nridge = raise_fork_dividers(rev_spans, rev_verts, ring, RIDGE_OFFSET)
        if nridge:
            print(f"  {name}: median ridge on {nridge} fork-divider vertices "
                  f"(offset {RIDGE_OFFSET})")
    left, right = build_routes(rev_spans, rev_verts, ring, circuit=True)

    write_strip_json(os.path.join(level_dir, "stripb.json"), obj,
                     rev_spans, rev_verts, ring, jump_records)
    write_route_csv(os.path.join(level_dir, "leftb.trk.csv"), left, "left")
    write_route_csv(os.path.join(level_dir, "rightb.trk.csv"), right, "right")
    print(f"  {name}: stripb.json ({len(rev_spans)} spans, ring={ring}, "
          f"{len(rev_verts)} verts, {len(branches)} branch(es)) + "
          f"leftb/rightb.trk.csv ({ring} rows)")
    return True


def main():
    args = sys.argv[1:]
    if "--circuits" in args:
        dirs = [os.path.join(LEVELS_ROOT, f"level{n:03d}") for n in NATIVE_CIRCUIT_LEVELS]
        dirs += [a for a in args if not a.startswith("--")]
    else:
        dirs = [a for a in args if not a.startswith("--")]
    if not dirs:
        print(__doc__); sys.exit(2)
    ok = 0
    for d in dirs:
        ok += 1 if process(d) else 0
    print(f"Done: {ok}/{len(dirs)} level(s) emitted reverse data.")


if __name__ == "__main__":
    main()
