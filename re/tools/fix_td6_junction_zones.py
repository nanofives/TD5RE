#!/usr/bin/env python3
"""fix_td6_junction_zones.py — fix the broken TD6 junction zones IN-PLACE in the
migrated strip.json, keeping its (models-aligned) coordinate system.

The migration fabricated 28 type-8/11 fork/merge junction pairs that split the
road into 4-lane main + branch + divider, producing narrow(558)->wide(2328)
"junction zones" the car wedges in (London span 317, the "invisible wall at
widened sections" class). The native road is clean width ~1100. This:
  (1) flattens main-ring type-8/11 junctions -> type-1 (no fork/merge routing);
  (2) normalises every main-ring span's RAIL width to ~the ring median by pulling
      the left/right rail verts toward the span centre (keeps centre + coords, so
      models stay aligned and the AI/collision corridor is jitter-free).
Branch spans (>= ring) are left untouched (unreferenced once junctions flatten).
Run from repo root; writes BOTH the worktree and main asset copies."""
import os, json, math, statistics, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WT_ASSETS = os.path.join(ROOT, ".claude", "worktrees", "fix-1780934761-153592-28290", "re", "assets", "levels")
MAIN_ASSETS = os.path.join(ROOT, "re", "assets", "levels")


def fix_strip(sj):
    spans = sj["spans"]; verts = sj["vertices"]
    ring = sj["header"][1]
    def wpos(vi, s): return (s[8] + verts[vi][0], s[9] + verts[vi][1], s[10] + verts[vi][2])
    # ring median width (from clean type-1 lane-8 spans only)
    widths = []
    for i in range(ring):
        s = spans[i]
        lx, ly, lz = wpos(s[4], s); rx, ry, rz = wpos(s[5], s)
        widths.append(math.hypot(rx - lx, rz - lz))
    target = statistics.median(widths)
    flat = 0; norm = 0
    for i in range(ring):
        s = spans[i]
        if s[0] in (8, 11):        # flatten fork/merge -> normal road
            s[0] = 1; s[6] = i + 1 if i < ring - 1 else -1; s[7] = i - 1 if i > 0 else -1
            flat += 1
        # normalise width: pull rails toward centre to `target`
        vl, vr = s[4], s[5]
        Lx, Ly, Lz = wpos(vl, s); Rx, Ry, Rz = wpos(vr, s)
        w = math.hypot(Rx - Lx, Rz - Lz)
        if w < 1.0:
            continue
        if w < target * 0.8 or w > target * 1.2:    # only touch abnormal spans
            cx, cy, cz = (Lx + Rx) / 2, (Ly + Ry) / 2, (Lz + Rz) / 2
            k = target / w
            # new world rail positions, scaled about the centre
            nLx, nLy, nLz = cx + (Lx - cx) * k, cy + (Ly - cy) * k, cz + (Lz - cz) * k
            nRx, nRy, nRz = cx + (Rx - cx) * k, cy + (Ry - cy) * k, cz + (Rz - cz) * k
            verts[vl] = [round(nLx - s[8]), round(nLy - s[9]), round(nLz - s[10])]
            verts[vr] = [round(nRx - s[8]), round(nRy - s[9]), round(nRz - s[10])]
            norm += 1
    return flat, norm, round(target)


def apply(level_num):
    name = f"level{level_num:03d}"
    done = False
    for base in (WT_ASSETS, MAIN_ASSETS):
        for fn in ("strip.json", "stripb.json"):
            p = os.path.join(base, name, fn)
            if not os.path.exists(p):
                continue
            sj = json.load(open(p))
            flat, norm, tgt = fix_strip(sj)
            json.dump(sj, open(p, "w"))
            print(f"  {os.path.relpath(p, ROOT)}: flattened {flat} junctions, normalised {norm} span widths (target {tgt})")
            done = True
    if not done:
        print(f"  no strip.json found for {name}")


if __name__ == "__main__":
    targets = [12] if len(sys.argv) < 2 else [int(a) for a in sys.argv[1:]]
    for lvl in targets:
        print(f"level{lvl:03d}:")
        apply(lvl)
    print("done.")
