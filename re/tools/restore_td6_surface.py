#!/usr/bin/env python3
"""Restore the TD6 per-span SURFACE byte (strip span +0x01) into the converted
strip.json / stripb.json.

convert_td6_tracks.py's remap_surface() ZEROED (road-only'd) the surf byte for
urban tracks, on the now-disproven assumption that surf was a TD5 surface
attribute. RE of TD6.exe (FUN_004465b0) showed surf is actually the ROW INDEX
into the per-track SURFACE GRID at strip header[6]: class = grid[surf*8 + lane_cell],
class -> grip(0x0049d7b8)/drag(0x0049d7f8) tables. So the raw surf byte must be
preserved for the grid-based friction system. This tool copies the original
strip.dat surf bytes back into the converted JSON (span order is 1:1 passthrough,
verified by matching type-byte histograms).

Idempotent: re-running just re-copies the same bytes. Verifies span count + type
histogram match before touching anything.
"""
import os, sys, json, struct, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TD6_LEVELS = os.path.join(ROOT, "Test Drive 6", "LEVELS")
ASSETS = os.path.join(ROOT, "re", "assets", "levels")

# (TD6 level zip number, our td5 level dir number) — from k_td6_menu_slots
PAIRS = [
    (4, 12, "London"), (0, 8, "Paris"), (1, 9, "NewYork"), (2, 10, "Rome"),
    (3, 11, "HongKong"), (10, 7, "Pelton"), (11, 18, "Ireland"),
    (15, 19, "Tahoe"), (16, 20, "CapeHatteras"), (17, 21, "Switzerland"),
    (18, 22, "Egypt"),
]

def orig_surf_bytes(zip_path, strip_name):
    z = zipfile.ZipFile(zip_path)
    names = {n.lower(): n for n in z.namelist()}
    if strip_name.lower() not in names:
        return None, None
    raw = z.read(names[strip_name.lower()])
    if len(raw) < 32:
        return None, None
    h = struct.unpack('<8I', raw[:32])
    span_off, total = h[0], h[4]
    surf, types = [], []
    for i in range(total):
        b = span_off + i*24
        if b+24 > len(raw): break
        types.append(raw[b]); surf.append(raw[b+1])
    return surf, types

def patch_json(json_path, surf, types):
    d = json.load(open(json_path))
    spans = d['spans']
    if len(spans) != len(surf):
        return f"SKIP span count {len(spans)} != orig {len(surf)}"
    # safety: type histogram must match (1:1 order)
    jt = [s[0] for s in spans]
    if jt != types:
        nmatch = sum(1 for a,b in zip(jt,types) if a==b)
        return f"SKIP type mismatch ({nmatch}/{len(jt)} match) — span order differs"
    changed = sum(1 for s,v in zip(spans, surf) if s[1] != v)
    for s, v in zip(spans, surf):
        s[1] = v
    json.dump(d, open(json_path, 'w'), separators=(',',':'))
    from collections import Counter
    return f"OK restored {changed} surf bytes; surf dist now {dict(sorted(Counter(surf).items())[:6])}..."

def main():
    only = sys.argv[1:] or None
    for td6, td5, name in PAIRS:
        if only and name not in only and str(td5) not in only:
            continue
        zp = os.path.join(TD6_LEVELS, f"level{td6:03d}.zip")
        od = os.path.join(ASSETS, f"level{td5:03d}")
        if not os.path.exists(zp):
            print(f"{name:14s} level{td6:03d}->{td5:03d}: orig zip MISSING"); continue
        for strip_dat, strip_json in (("strip.dat","strip.json"), ("stripb.dat","stripb.json")):
            jp = os.path.join(od, strip_json)
            if not os.path.exists(jp):
                continue
            surf, types = orig_surf_bytes(zp, strip_dat)
            if surf is None:
                print(f"{name:14s} {strip_dat}: not in zip"); continue
            res = patch_json(jp, surf, types)
            print(f"{name:14s} {strip_json}: {res}")

if __name__ == "__main__":
    main()
