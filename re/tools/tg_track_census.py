#!/usr/bin/env python3
"""tg_track_census.py -- sweep every shipped / converted / custom level and
tabulate the strip-level ELEMENTS the auto-track generator could make
recurrent: lane counts, span types, surface codes, branch (jump-table)
records and the lane add/drop transition patterns.

Read-only over re/assets/levels/level*/{strip.json,levelinf.json,
textures.src/textures.json,models.bin}. No game, no writes except --out.

Why this exists (2026-09-06, autotrack element catalogue): the generator
carried two premises that a sweep disproves in one run --
  * "shipped tracks only ever use 2-4 lanes"  -> TD5 uses 2..10 per span
    (Honolulu / Maui reach 10, Washington is 6/8 only);
  * "a branch is a bow-and-rejoin detour"     -> 147 of 147 shipped forks
    rejoin through a type-11 span and 126 of them run PARALLEL to the main
    road (corridor length within 15% of the main distance).
Re-run after converting more TD6 levels or adding custom tracks.

Usage:
    python re/tools/tg_track_census.py [--levels DIR] [--out FILE.md]
                                       [--changes LEVEL]

    --changes LEVEL   dump the spans around every lane-count change in one
                      level (e.g. 1 for Keswick) to see the type 2..7 rows.

Span record (strip.json "spans", per re/tools/td5_trackgen.py):
    [type, surface, lane_mask, packed(lanes | lane_base<<4),
     lvi, rvi, link_next, link_prev, ox, oy, oz]
Jump record (pre_span_hex): u32 count, then 6-byte (lo, hi, base) u16 each;
native TD5 at +0x18 in the file == offset 4 of pre_span_hex, TD6-converted
at +0x20 == offset 12 (td5_track.c:3526). Detected by validity here.
"""

import argparse
import collections
import glob
import json
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_LEVELS = os.path.join(ROOT, "re", "assets", "levels")

# TD5 level zip -> track (td5_asset.c k_pool_to_zip + s_track_display_names).
TD5_NAMES = {
    1: "KESWICK", 2: "SAN FRANCISCO", 3: "BERN", 4: "KYOTO", 5: "WASHINGTON",
    6: "MUNICH", 13: "HONOLULU", 14: "SYDNEY", 15: "TOKYO", 16: "EDINBURGH",
    17: "BLUE RIDGE", 23: "MOSCOW", 25: "CHEDDAR", 26: "JARASH",
    27: "COURMAYEUR", 28: "MAUI", 29: "NEWCASTLE", 37: "HOUSE OF BEZ",
    39: "DRAG STRIP",
}
# Converted TD6 levels (td5_asset.c k_td6_menu_slots).
TD6_NAMES = {
    7: "PELTON RACEWAY", 18: "IRELAND", 19: "LAKE TAHOE", 20: "CAPE HATTERAS",
    21: "SWITZERLAND", 22: "EGYPT", 8: "PARIS", 9: "NEW YORK", 10: "ROME",
    11: "HONG KONG", 12: "LONDON",
}
SPAN_TYPE = {
    1: "QUAD_A", 2: "ADD1_R", 3: "ADD1_L", 4: "ADD2", 5: "DROP1_R",
    6: "DROP1_L", 7: "DROP2", 8: "JUNC_FWD", 9: "SENT_START",
    10: "SENT_END", 11: "JUNC_BWD",
}
# td5_types.h names for the same ids, kept for cross-reference:
#   2 QUAD_B, 3 TRANSITION_A, 4 TRANSITION_B, 5 QUAD_C, 6 REVERSED_A, 7 REVERSED_B.
# The ADD/DROP reading comes from the shipped data (see lane_changes()) and
# from the vertex-offset LUTs the port confirmed against the binary
# (td5_track.c k_target_vertex_offsets / k_quad_vertex_offsets @ 0x00474E40:
# types 2/3/4 far row wider by 1/1/2, types 5/6/7 narrower by 1/1/2).


def load_level(d):
    p = os.path.join(d, "strip.json")
    if not os.path.exists(p):
        return None
    return json.load(open(p))


def level_name(num, custom):
    if num in TD5_NAMES:
        return TD5_NAMES[num], "TD5"
    if num in TD6_NAMES:
        return TD6_NAMES[num], "TD6"
    if num in custom:
        return custom[num], "custom"
    return "(unregistered custom/test)", "custom"


def jump_records(s):
    sp = s["spans"]
    ring = s["header"][1]
    pre = bytes.fromhex(s.get("pre_span_hex", ""))
    if len(pre) < 4:
        return []
    nj = struct.unpack("<I", pre[:4])[0]
    if nj == 0:
        return []

    def recs(off):
        out = []
        for k in range(nj):
            if off + 6 * k + 6 > len(pre):
                return None
            out.append(struct.unpack_from("<HHH", pre, off + 6 * k))
        return out

    def valid(rs):
        return rs is not None and all(
            ring < lo <= hi < len(sp) and 0 < base <= ring for lo, hi, base in rs)

    for off in (4, 12):
        rs = recs(off)
        if valid(rs):
            return rs
    return recs(4) or []


def lanes_of(sp, i):
    return sp[i][3] & 0xF


def base_of(sp, i):
    return (sp[i][3] >> 4) & 0xF


def level_table(levels_dir, custom):
    rows = []
    for d in sorted(glob.glob(os.path.join(levels_dir, "level*", ""))):
        n = os.path.basename(os.path.normpath(d))
        try:
            num = int(n[5:])
        except ValueError:
            continue
        s = load_level(d)
        name, src = level_name(num, custom)
        if s is None:
            rows.append((n, name if src != "custom" else "AUTO (runtime)", src,
                         None, None, None, {}, {}, 0, "?", 0))
            continue
        sp = s["spans"]
        lanes = collections.Counter(lanes_of(sp, i) for i in range(len(sp)))
        types = collections.Counter(SPAN_TYPE.get(x[0], x[0]) for x in sp)
        surf = collections.Counter(x[1] for x in sp)
        li_p = os.path.join(d, "levelinf.json")
        circ = None
        if os.path.exists(li_p):
            circ = json.load(open(li_p)).get("track_type", {}).get("value")
        ntex = "?"
        ts = os.path.join(d, "textures.src", "textures.json")
        if os.path.exists(ts):
            tj = json.load(open(ts))
            ntex = tj.get("page_count", len(tj.get("pages", [])))
        mb = os.path.join(d, "models.bin")
        msz = os.path.getsize(mb) // 1024 if os.path.exists(mb) else 0
        rows.append((n, name, src, circ, s["header"][1], len(jump_records(s)),
                     dict(sorted(lanes.items())), dict(types), len(surf),
                     ntex, msz))
    return rows


def branch_table(levels_dir, custom):
    rows = []
    kinds = collections.Counter()
    splits = collections.Counter()
    lengths = []
    for d in sorted(glob.glob(os.path.join(levels_dir, "level*", ""))):
        n = os.path.basename(os.path.normpath(d))
        s = load_level(d)
        if s is None:
            continue
        num = int(n[5:])
        name, _ = level_name(num, custom)
        sp = s["spans"]
        ring = s["header"][1]
        for k, (lo, hi, base) in enumerate(jump_records(s)):
            F = base - 1
            if not (0 <= F < len(sp) and lo <= hi < len(sp)):
                rows.append((name, k, "BAD record (%d,%d,%d)" % (lo, hi, base)))
                continue
            t9 = [i for i in range(lo, hi + 1) if sp[i][0] == 9]
            t10 = [i for i in range(lo, hi + 1) if sp[i][0] == 10]
            R = sp[t10[0]][6] if t10 else None
            rtype = sp[R][0] if R is not None and 0 <= R < len(sp) else None
            L = (t10[0] - t9[0] + 1) if (t9 and t10) else (hi - lo + 1)
            D = (R - F) % ring if (R is not None and 0 <= R < ring) else None
            if D is None or D == 0:
                kind = "dangling/unknown"
            elif rtype != 11:
                kind = "one-way merge (no type-11)"
            elif L < 0.85 * D:
                kind = "SHORTCUT"
            elif L > 1.15 * D:
                kind = "DETOUR"
            else:
                kind = "PARALLEL"
            a, b, c = lanes_of(sp, F), lanes_of(sp, F + 1), lanes_of(sp, lo)
            splits[(a, b, c)] += 1
            kinds[kind] += 1
            lengths.append(L)
            rl = ("%d->%d" % (lanes_of(sp, R), lanes_of(sp, R + 1))
                  if R is not None and R + 1 < len(sp) else "?")
            rows.append((name, k, F, "%d->%d+%d" % (a, b, c), L, D,
                         "%s (%s)" % (R, rtype), rl, kind,
                         "sum OK" if a == b + c else "SUM MISMATCH"))
    return rows, kinds, splits, lengths


def lane_changes(levels_dir, custom):
    """Pattern census of (type of the transition span, lanes before, lanes
    after, lane_base before, lane_base after) over every level."""
    pat = collections.Counter()
    shared_at_change = [0, 0]
    for d in sorted(glob.glob(os.path.join(levels_dir, "level*", ""))):
        s = load_level(d)
        if s is None:
            continue
        sp = s["spans"]
        ring = s["header"][1]
        for i in range(ring - 1):
            a, b = sp[i], sp[i + 1]
            if a[0] >= 8 or b[0] >= 8:
                continue
            la, lb = a[3] & 0xF, b[3] & 0xF
            if la != lb:
                shared_at_change[1] += 1
                if a[5] == b[4]:
                    shared_at_change[0] += 1
                # The TRANSITION span carries the WIDER lane count: a lane ADD
                # is typed on the span after the seam (2/3/4, its far row is
                # wider than its near row), a lane DROP on the span before it
                # (5/6/7, far row narrower). Key the pattern on that span.
                t = b[0] if lb > la else a[0]
                pat[(SPAN_TYPE.get(t, t), la, lb,
                     (a[3] >> 4) & 0xF, (b[3] >> 4) & 0xF)] += 1
    return pat, shared_at_change


def dump_changes(levels_dir, num, limit=6):
    d = os.path.join(levels_dir, "level%03d" % num)
    s = load_level(d)
    if s is None:
        sys.exit("no strip.json in %s" % d)
    sp, V, ring = s["spans"], s["vertices"], s["header"][1]
    shown = 0
    for i in range(1, ring - 2):
        if lanes_of(sp, i) != lanes_of(sp, i + 1) and shown < limit:
            shown += 1
            print("--- lane change at %d -> %d" % (i, i + 1))
            for k in range(i - 1, i + 3):
                x = sp[k]
                L, h = x[3] & 0xF, (x[3] >> 4) & 0xF
                near = [v[0] for v in V[x[4]:x[4] + L + 1]]
                far = [v[0] for v in V[x[5]:x[5] + L + 1]]
                print("  span %d: type=%d(%s) lanes=%d base=%d lvi=%d rvi=%d "
                      "next.lvi=%d" % (k, x[0], SPAN_TYPE.get(x[0], "?"), L, h,
                                       x[4], x[5], sp[k + 1][4]))
                print("     near xs=%s" % near)
                print("     far  xs=%s" % far)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--levels", default=DEFAULT_LEVELS)
    ap.add_argument("--out", default=None, help="write markdown here")
    ap.add_argument("--changes", type=int, default=None,
                    help="dump spans around lane-count changes of LEVEL")
    args = ap.parse_args()

    if args.changes is not None:
        dump_changes(args.levels, args.changes)
        return

    custom = {}
    cj = os.path.join(args.levels, "custom_tracks.json")
    if os.path.exists(cj):
        custom = {t["level"]: t["name"]
                  for t in json.load(open(cj)).get("tracks", [])}

    out = []
    w = out.append
    w("# Track census (%s)\n" % os.path.relpath(args.levels, ROOT))
    w("## Levels\n")
    w("| level | name | src | circ | ring | jumps | lanes {n: spans} | "
      "span types {type: spans} | #surf codes | tex pages | models KB |")
    w("|---|---|---|---|---|---|---|---|---|---|---|")
    for r in level_table(args.levels, custom):
        w("| " + " | ".join(str(c) if c is not None else "" for c in r) + " |")

    rows, kinds, splits, lengths = branch_table(args.levels, custom)
    w("\n## Branches (one row per jump record)\n")
    w("split = lanes(F) -> lanes(F+1) + lanes(B0). kind: corridor length L vs "
      "main distance D fork->rejoin: SHORTCUT L<0.85D, DETOUR L>1.15D, else "
      "PARALLEL.\n")
    w("| level | # | F | split | L | D | rejoin (type) | lanes R->R+1 | kind | fork sum |")
    w("|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        w("| " + " | ".join(str(c) for c in r) + " |")
    w("\nkinds: %s" % dict(kinds))
    w("\nsplit patterns (lanes F -> main after + corridor), by frequency:")
    for (a, b, c), n in splits.most_common():
        shape = ("symmetric" if b == c else
                 "slip-road (corridor narrower)" if c < b else
                 "main narrower than corridor")
        w("- %d -> %d + %d: %d  (%s)" % (a, b, c, n, shape))
    if lengths:
        lengths.sort()
        w("\ncorridor length spans: min %d, median %d, max %d; <= 8 spans "
          "(crossing stubs / islands): %d" % (lengths[0], lengths[len(lengths) // 2],
                                             lengths[-1],
                                             sum(1 for l in lengths if l <= 8)))

    pat, shared = lane_changes(args.levels, custom)
    w("\n## Lane-count transitions (main ring, all levels)\n")
    w("%d lane-count changes, %d of them with a SHARED vertex row across the "
      "seam (rvi(k) == lvi(k+1)) -- so lane changes do NOT need a row break.\n"
      % (shared[1], shared[0]))
    w("| transition span type | lanes before -> after | lane_base before -> after | count |")
    w("|---|---|---|---|")
    for (t, la, lb, ha, hb), n in pat.most_common(40):
        w("| %s | %d -> %d | %d -> %d | %d |" % (t, la, lb, ha, hb, n))

    text = "\n".join(out) + "\n"
    if args.out:
        with open(args.out, "w", newline="\n", encoding="utf-8") as f:
            f.write(text)
        print("wrote", args.out)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
