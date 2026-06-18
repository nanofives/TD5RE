#!/usr/bin/env python3
"""apply_td6_spline_routes.py — regenerate a TD6 track's AI route tables
(left/right.trk.csv + b-variants) from TD6's AUTHORED RACING LINE in the
SPLINE*.TD6 files, instead of the flat road-centreline the old converter wrote.

RE'd 2026-06-12 (reference_td6_spline_racing_line_route_system): each SPLINE entry
is [laneA,flagA,laneB,flagB] per main-road span; laneA/laneB are 0..255 lateral
fractions tracing the racing-line corridor (narrows + shifts to the apex on
corners). build_routes(..., spline=) maps laneA->left.trk, laneB->right.trk and
derives a curvature corner-brake threshold. This rewrites ONLY the 4 route CSVs
(runtime data — no rebuild needed) and backs up the originals to *.prespline.bak.

Run from repo root.  python3 re/tools/apply_td6_spline_routes.py [asset td6 circuit]
"""
import os, sys, zipfile, importlib.util, shutil

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TD6_LEVELS = os.path.join(ROOT, "Test Drive 6", "LEVELS")
ASSET_ROOTS = [
    os.path.join(ROOT, "re", "assets", "levels"),
    os.path.join(ROOT, ".claude", "worktrees", "fix-1780934761-153592-28290",
                 "re", "assets", "levels"),
]

_spec = importlib.util.spec_from_file_location(
    "conv", os.path.join(os.path.dirname(__file__), "convert_td6_tracks.py"))
conv = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(conv)


def write_csv(table, path):
    n = len(table) // 3
    if os.path.exists(path) and not os.path.exists(path + ".prespline.bak"):
        shutil.copyfile(path, path + ".prespline.bak")
    with open(path, "w") as f:
        f.write("# td5 route table -- one row per span: lane,heading,threshold (raw u8)\n")
        for i in range(n):
            f.write(f"{table[i*3]},{table[i*3+1]},{table[i*3+2]}\n")


def read_entry(z, name):
    lut = {n.lower(): n for n in z.namelist()}
    return z.read(lut[name]) if name in lut else None


def regen(asset_level, td6_level, circuit):
    zp = os.path.join(TD6_LEVELS, f"level{td6_level:03d}.zip")
    z = zipfile.ZipFile(zp)
    # forward strip + spline1 ; reverse stripb + spline1b
    sets = [
        ("strip.dat",  "spline1.td6",  "left.trk.csv",  "right.trk.csv"),
        ("stripb.dat", "spline1b.td6", "leftb.trk.csv", "rightb.trk.csv"),
    ]
    for strip_name, spline_name, l_csv, r_csv in sets:
        strip = read_entry(z, strip_name)
        spline = read_entry(z, spline_name)
        if strip is None or spline is None:
            print(f"  {strip_name}/{spline_name}: missing — skip")
            continue
        left, right = conv.build_routes(strip, circuit=bool(circuit), spline=spline)
        ring = len(spline) // 4
        for base in ASSET_ROOTS:
            d = os.path.join(base, f"level{asset_level:03d}")
            if not os.path.isdir(d):
                continue
            write_csv(left, os.path.join(d, l_csv))
            write_csv(right, os.path.join(d, r_csv))
        # quick stats: how much does the racing line move vs a flat 128?
        la = [spline[i * 4] for i in range(ring)]
        lb = [spline[i * 4 + 2] for i in range(ring)]
        print(f"  {l_csv}/{r_csv}: ring={ring} laneA[{min(la)}..{max(la)}] "
              f"laneB[{min(lb)}..{max(lb)}] (was flat 128/edge)")


if __name__ == "__main__":
    # default: ALL 11 migrated TD6 tracks (asset_level, td6_level, circuit). circuit
    # per k_td6_menu_slots: London/Paris/NewYork/Rome/HongKong = P2P (0); the rest
    # (Pelton/Ireland/Tahoe/CapeHatteras/Switzerland/Egypt) = circuit (1).
    targets = [(12, 4, 0), (8, 0, 0), (9, 1, 0), (10, 2, 0), (11, 3, 0),
               (7, 10, 1), (18, 11, 1), (19, 15, 1), (20, 16, 1), (21, 17, 1), (22, 18, 1)]
    if len(sys.argv) >= 4:
        targets = [(int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]))]
    for asset, td6, circ in targets:
        print(f"level{asset:03d} (TD6 {td6}, circuit={circ}):")
        regen(asset, td6, circ)
    print("done — route CSVs rewritten (originals -> *.prespline.bak).")
