#!/usr/bin/env python3
"""
connect_branches.py -- branch-unification tooling (re/analysis/branch_unification_plan.md).

Goal: make appended branch corridors geometrically contiguous with the main ring so
the engine drives them as plain road, then the displaced-branch runtime band-aids can
be deleted. Operates on the editable-source strip.json / stripb.json (same format as
strip_tool.py) plus models.bin (road meshes) [relocate, not yet implemented here].

Commands:
  characterize <strip.json> [<strip.json> ...]   # Phase 0: per-corridor gap report

Strip.json layout (per strip_tool.py / re/include/td5_level_formats.h):
  header: 5 u32 [span_table_off, RING(=hdr[1]), vertex_table_off, secondary, auxiliary]
  pre_span_hex: bytes[20..span_off) -- for TD6 8-u32 strips this holds header[5,6,7],
     the branch jump table (count at file 0x14 = pre_span[0:4]; 6B [lo,hi,base] records
     start at file 0x20 = pre_span[12:]) and the surface grid.
  spans:    [type, surf, reserved, packed, lvi, rvi, fwd(link_next), bwd(link_prev), ox,oy,oz]
            packed: low nibble = lane_count, high nibble = height_offset
  vertices: [x, y, z] int16  (world = span.origin + vertex)

Span corners (lvi indexes the NEAR row, rvi the FAR row; each row has lane_count+1 verts):
  NW = origin + vtx[lvi]            NE = origin + vtx[lvi + lc]
  SW = origin + vtx[rvi]            SE = origin + vtx[rvi + lc]

Jump table semantics [CONFIRMED td5_track.c:2057-2066]: a branch span s in [lo,hi] maps
to main span s + (base - lo). So a corridor [lo,hi,base] parallels main [base .. base+(hi-lo)];
its ENTRY fork is on the main ring at ~base-1 (type 8, link_next=lo) and its REJOIN fork at
~base+(hi-lo) (type 11, link_prev~=hi).
"""

import json
import math
import struct
import sys


def load_strip(path):
    with open(path, "r", encoding="utf-8") as fh:
        obj = json.load(fh)
    return obj


def parse_jump_table(obj):
    """Return list of (lo, hi, base) corridors from pre_span_hex.
    TD6 8-u32-header strips put records at file 0x20 (pre[12:]); native TD5
    5-u32-header strips at file 0x18 (pre[4:]). Count is at file 0x14 (pre[0:4])
    for both. Auto-detect by validating records against ring/total."""
    pre = bytes.fromhex(obj["pre_span_hex"])
    if len(pre) < 4:
        return []
    count = struct.unpack_from("<I", pre, 0)[0]
    if count <= 0 or count > 4096:
        return []
    ring = int(obj["header"][1])
    total = len(obj["spans"])

    def try_off(off):
        recs = []
        for i in range(count):
            if off + 6 > len(pre):
                return None
            lo, hi, base = struct.unpack_from("<3H", pre, off)
            recs.append((lo, hi, base))
            off += 6
        ok = all(ring <= lo <= hi < total and 0 <= base < ring
                 for (lo, hi, base) in recs)
        return recs if ok else None

    for off in (12, 4):                  # TD6 (0x20) first, then native (0x18)
        recs = try_off(off)
        if recs is not None:
            return recs
    return try_off(12) or []             # fall back to raw TD6 read for reporting


def span_lane_count(sp):
    lc = sp[3] & 0x0F
    return lc if lc >= 1 else 1


def vtx_world(obj, span_idx, vidx):
    """World XZ of vertex vidx referenced from span span_idx's origin."""
    sp = obj["spans"][span_idx]
    v = obj["vertices"][vidx]
    return (sp[8] + v[0], sp[10] + v[2])   # ox + vx, oz + vz


def near_edge_center(obj, span_idx):
    sp = obj["spans"][span_idx]
    lc = span_lane_count(sp)
    lvi = sp[4]
    x0, z0 = vtx_world(obj, span_idx, lvi)
    x1, z1 = vtx_world(obj, span_idx, lvi + lc)
    return ((x0 + x1) / 2.0, (z0 + z1) / 2.0)


def far_edge_center(obj, span_idx):
    sp = obj["spans"][span_idx]
    lc = span_lane_count(sp)
    rvi = sp[5]
    x0, z0 = vtx_world(obj, span_idx, rvi)
    x1, z1 = vtx_world(obj, span_idx, rvi + lc)
    return ((x0 + x1) / 2.0, (z0 + z1) / 2.0)


def edge_pts(obj, span_idx, row="near"):
    """World endpoints (left, right) of a span's near (lvi) or far (rvi) edge."""
    sp = obj["spans"][span_idx]
    lc = span_lane_count(sp)
    base_vi = sp[4] if row == "near" else sp[5]
    left = vtx_world(obj, span_idx, base_vi)
    right = vtx_world(obj, span_idx, base_vi + lc)
    return left, right


def fork_opening_edge(obj, fork_idx, kind):
    """Branch-opening edge of a fork span. For a type-8 (fwd) fork the branch
    takes the lanes beyond the main continuation (span+1); the opening is the
    fork's FAR edge over those right lanes. For a type-11 (bwd) fork it is the
    fork's NEAR edge over the right lanes (driving backward into the corridor).
    Falls back to the full edge if the main lane count can't be read."""
    sp = obj["spans"][fork_idx]
    lc = span_lane_count(sp)
    ring = int(obj["header"][1])
    if kind == 8:
        adj = fork_idx + 1
        row_vi = sp[5]            # far edge
    else:
        adj = fork_idx - 1
        row_vi = sp[4]            # near edge
    main_lc = lc
    if 0 <= adj < len(obj["spans"]):
        main_lc = span_lane_count(obj["spans"][adj])
    if not (1 <= main_lc < lc):
        main_lc = 0               # whole edge if no clean split
    left = vtx_world(obj, fork_idx, row_vi + main_lc)
    right = vtx_world(obj, fork_idx, row_vi + lc)
    return left, right


def find_forks(obj):
    """Map link-target span -> (fork_span_idx, type) for every type-8/11 ring fork."""
    ring = int(obj["header"][1])
    total = len(obj["spans"])
    out = {}
    for i in range(min(ring, total)):
        sp = obj["spans"][i]
        t = sp[0]
        if t == 8:
            tgt = sp[6]           # link_next
        elif t == 11:
            tgt = sp[7]           # link_prev
        else:
            continue
        if ring <= tgt < total:
            out[tgt] = (i, t)
    return out


def rigid_fit_2pt(s0, s1, t0, t1):
    """Rigid transform (rotation theta + translation) mapping s0->t0, s1->t1
    (uniform-scale ignored; rotation from the edge direction, translation pins s0->t0)."""
    a = math.atan2(s1[1] - s0[1], s1[0] - s0[0])
    b = math.atan2(t1[1] - t0[1], t1[0] - t0[0])
    theta = b - a
    c, s = math.cos(theta), math.sin(theta)
    tx = t0[0] - (c * s0[0] - s * s0[1])
    tz = t0[1] - (s * s0[0] + c * s0[1])
    return (theta, c, s, tx, tz)


def apply_xf(xf, p):
    _, c, s, tx, tz = xf
    return (c * p[0] - s * p[1] + tx, s * p[0] + c * p[1] + tz)


def heading(p_from, p_to):
    """Heading angle (radians) of the segment from p_from to p_to."""
    return math.atan2(p_to[1] - p_from[1], p_to[0] - p_from[0])


def ang_diff_deg(a, b):
    d = (a - b + math.pi) % (2 * math.pi) - math.pi
    return abs(math.degrees(d))


def dist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def edge_center(e):
    return ((e[0][0] + e[1][0]) / 2.0, (e[0][1] + e[1][1]) / 2.0)


def span_center(obj, idx):
    n = near_edge_center(obj, idx)
    f = far_edge_center(obj, idx)
    return ((n[0] + f[0]) / 2.0, (n[1] + f[1]) / 2.0)


def characterize(path):
    obj = load_strip(path)
    ring = int(obj["header"][1])
    total = len(obj["spans"])
    corridors = parse_jump_table(obj)
    forks = find_forks(obj)            # link-target span -> (fork_idx, type)
    print(f"\n=== {path} ===")
    print(f"ring={ring} total_spans={total} corridors={len(corridors)} forks={len(forks)}")
    print(f"{'#':>3} {'lo':>5} {'hi':>5} {'len':>4} {'entryF':>6} {'rejF':>6} "
          f"{'entry_gap':>9} {'rej_before':>10} {'rej_AFTER':>9} {'rej_dHdg':>8}  verdict")
    n_easy = n_hard = n_bad = 0
    for i, (lo, hi, base) in enumerate(corridors):
        clen = hi - lo
        # The two forks that link into this corridor: one at lo (entry), one near hi (rejoin).
        members = [(tgt, f, t) for tgt, (f, t) in forks.items() if lo <= tgt <= hi]
        entry = forks.get(lo)
        if entry is None and members:
            members.sort()
            entry = (members[0][1], members[0][2]); entry_tgt = members[0][0]
        else:
            entry_tgt = lo
        rej = None
        for tgt, f, t in sorted(members, reverse=True):
            if (f, t) != entry:
                rej = (f, t); rej_tgt = tgt; break
        if entry is None or rej is None:
            print(f"{i:>3} {lo:>5} {hi:>5} {clen:>4}  forks-not-found "
                  f"(members={[(m[0],m[2]) for m in members]})")
            n_bad += 1
            continue
        ef_idx, ef_t = entry
        rf_idx, rf_t = rej
        # T_entry: map corridor entry edge (span entry_tgt near edge) onto the
        # entry fork's branch-opening edge.
        s0, s1 = edge_pts(obj, entry_tgt, "near")
        t0, t1 = fork_opening_edge(obj, ef_idx, ef_t)
        xf = rigid_fit_2pt(s0, s1, t0, t1)
        entry_gap = dist(edge_center((s0, s1)), edge_center((t0, t1)))
        # Rejoin: corridor exit span center vs rejoin fork opening center,
        # before and AFTER applying T_entry to the whole corridor.
        cc = span_center(obj, rej_tgt)
        rf_e = fork_opening_edge(obj, rf_idx, rf_t)
        rfc = edge_center(rf_e)
        rej_before = dist(cc, rfc)
        cc_after = apply_xf(xf, cc)
        rej_after = dist(cc_after, rfc)
        # heading residual at the rejoin after T_entry
        ce0, ce1 = edge_pts(obj, rej_tgt, "far")
        ch = heading(apply_xf(xf, ce0), apply_xf(xf, ce1))
        rh = heading(rf_e[0], rf_e[1])
        rej_dhdg = ang_diff_deg(ch, rh)
        hard = (rej_after > 2500 or rej_dhdg > 30)
        verdict = "HARD(connector)" if hard else "easy(rigid)"
        if hard:
            n_hard += 1
        else:
            n_easy += 1
        print(f"{i:>3} {lo:>5} {hi:>5} {clen:>4} {ef_idx:>6} {rf_idx:>6} "
              f"{entry_gap:>9.0f} {rej_before:>10.0f} {rej_after:>9.0f} "
              f"{rej_dhdg:>8.1f}  {verdict}")
    print(f"SUMMARY {path}: corridors={len(corridors)} easy={n_easy} "
          f"hard={n_hard} bad={n_bad}")


def main():
    if len(sys.argv) < 3 or sys.argv[1] != "characterize":
        print(__doc__)
        sys.exit(2)
    for path in sys.argv[2:]:
        characterize(path)


if __name__ == "__main__":
    main()
