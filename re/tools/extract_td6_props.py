#!/usr/bin/env python3
"""Copy TD6 breakable-prop tables (level<N>.tcl / Level<N>b.tcl) into the
converted level dirs as LEVEL.TCL / LEVELB.TCL so the source port can load them.

TD6 props (RE: TD6.exe FUN_00441070/FUN_0043e700) are INVISIBLE collision volumes
anchored to the baked world geometry (NOT separate meshes). Each .tcl record is
16 bytes: type@0 (0xFF=terminator), radius@1, strip-seg u16@4, u_min@6, u_max@7,
posX i32@8 (24.8), posZ i32@12 (24.8). Verified: posX/256,posZ/256 are in the
SAME world space as our converted strip vertices. The converter never copied
these, so do it here (raw passthrough — the engine parses the 16B records).
"""
import os, sys, shutil, zipfile, struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TD6_LEVELS = os.path.join(ROOT, "Test Drive 6", "LEVELS")
ASSETS = os.path.join(ROOT, "re", "assets", "levels")

# (TD6 level number, our td5 level dir number, name)
PAIRS = [
    (4, 12, "London"), (0, 8, "Paris"), (1, 9, "NewYork"), (2, 10, "Rome"),
    (3, 11, "HongKong"), (10, 7, "Pelton"), (11, 18, "Ireland"),
    (15, 19, "Tahoe"), (16, 20, "CapeHatteras"), (17, 21, "Switzerland"),
    (18, 22, "Egypt"),
]

def find_entry(z, *cands):
    names = {n.lower(): n for n in z.namelist()}
    for c in cands:
        if c.lower() in names:
            return names[c.lower()]
    return None

def count_props(raw):
    n = 0
    for i in range(len(raw) // 16):
        if raw[i*16] == 0xFF:
            break
        n += 1
    return n

def main():
    only = sys.argv[1:] or None
    for td6, td5, name in PAIRS:
        if only and name not in only and str(td5) not in only:
            continue
        zp = os.path.join(TD6_LEVELS, f"level{td6:03d}.zip")
        od = os.path.join(ASSETS, f"level{td5:03d}")
        if not os.path.exists(zp) or not os.path.isdir(od):
            print(f"{name:14s} level{td6:03d}->{td5:03d}: zip/dir MISSING"); continue
        z = zipfile.ZipFile(zp)
        for src_cands, dst in (((f"level{td6}.tcl", f"level{td6:03d}.tcl"), "LEVEL.TCL"),
                               ((f"Level{td6}b.tcl", f"level{td6}b.tcl"), "LEVELB.TCL")):
            e = find_entry(z, *src_cands)
            if not e:
                continue
            raw = z.read(e)
            with open(os.path.join(od, dst), "wb") as f:
                f.write(raw)
            print(f"{name:14s} {dst}: {len(raw)} bytes, {count_props(raw)} props (from {e})")

if __name__ == "__main__":
    main()
