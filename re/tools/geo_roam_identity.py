"""geo_roam_identity.py -- keep the base place's ROAMTILEs valid in an EXTENDED place (round S, roam SB).

A ROAMTILE's input hash (td5_roam_tile.c ctx_input_hash) covers the WHOLE PLACE.JSON text and the
HEIGHT / WATER raster headers. Growing La Plata into Gran La Plata changes both (new name, bbox,
boundary, grid width and height), so every La Plata tile would fail its hash and rebuild, although
not one of its inputs changed: the base blocks' tile files and raster cells are byte-identical.

This writes PLACE.JSON extension.roam_identity:
    place_fnv, hdr_fnv   the base place's values, FNV-1a exactly as td5_roam_tile.c computes them
                         (PLACE.JSON bytes; 72 header bytes + int64 file size of HEIGHT.R16 then WATER.R8)
    boxes                source-frame [minx, minz, maxx, maxz] of every raster block where ANY cell of
                         HEIGHT / COVER / WATER / CANOPY differs from the base (or holds data past the
                         base grid)
The C side hashes the base identity for a tile window clear of every box, and the real values for a
window that meets one. Changed or new TILE files need no box: their INDEX rows are in the hash.

Run it LAST (after geo_water_fill), against the base place's shipped files.

usage: python geo_roam_identity.py <extended place> <base place dir> [--write]
"""
from __future__ import annotations

import json
import math
import os
import struct
import sys

import numpy as np

HS = 72
FNV_INIT = 2166136261


def fnv(h: int, data: bytes) -> int:
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def place_fnv(path: str) -> int:
    with open(path, "rb") as f:
        return fnv(FNV_INIT, f.read())


def hdr_fnv(height: str, water: str) -> int:
    h = FNV_INIT
    for p in (height, water):
        with open(p, "rb") as f:
            hd = f.read(HS)
        h = fnv(h, hd if len(hd) == HS else bytes(HS))
        h = fnv(h, struct.pack("<q", os.path.getsize(p)))
    return h


def main() -> int:
    place, base = sys.argv[1], sys.argv[2]
    pj = json.load(open(os.path.join(place, "PLACE.JSON"), encoding="utf-8"))
    ext = pj.get("extension")
    if not ext:
        raise SystemExit("not an extended place")
    W, H = pj["tiled"]["grid"]
    ow, oh = ext["base_grid"]
    cell = pj["cell_units"]
    x0, z0 = pj["tiled"]["origin"]
    bc = int(round(pj["tiled"]["tile_size_units"] / cell)) * int(pj["tiled"]["block_tiles"])
    rasters = (("HEIGHT.R16", np.int16, -32768), ("COVER.R8", np.uint8, 255),
               ("WATER.R8", np.uint8, 255), ("CANOPY.R8", np.uint8, 255))
    changed: dict = {}      # (bx, bz) -> [j0, i0, j1, i1] bbox of the differing cells (global cells)
    for name, dt, nd in rasters:
        a = np.memmap(os.path.join(base, name), dtype=dt, mode="r", offset=HS, shape=(oh, ow))
        b = np.memmap(os.path.join(place, name), dtype=dt, mode="r", offset=HS, shape=(H, W))
        for bz in range(int(math.ceil(H / bc))):
            for bx in range(int(math.ceil(W / bc))):
                j0, j1, i0, i1 = bz * bc, min(H, (bz + 1) * bc), bx * bc, min(W, (bx + 1) * bc)
                bb = np.asarray(b[j0:j1, i0:i1])
                old = np.full(bb.shape, nd, dtype=dt)      # past the base grid the base is nodata
                ja, ia = max(0, min(j1, oh) - j0), max(0, min(i1, ow) - i0)
                if ja > 0 and ia > 0:
                    old[:ja, :ia] = np.asarray(a[j0:j0 + ja, i0:i0 + ia])
                d = bb != old
                if not d.any():
                    continue
                rows, cols = np.where(d.any(axis=1))[0], np.where(d.any(axis=0))[0]
                box = [j0 + int(rows[0]), i0 + int(cols[0]), j0 + int(rows[-1]) + 1, i0 + int(cols[-1]) + 1]
                o = changed.get((bx, bz))
                changed[(bx, bz)] = box if o is None else [min(o[0], box[0]), min(o[1], box[1]),
                                                           max(o[2], box[2]), max(o[3], box[3])]
        del a, b
    keys = sorted(changed, key=lambda t: (t[1], t[0]))
    boxes = [[x0 + changed[k][1] * cell, z0 + changed[k][0] * cell,
              x0 + changed[k][3] * cell, z0 + changed[k][2] * cell] for k in keys]
    base_fetched = {tuple(v) for v in ext.get("base_fetched_blocks", [])}
    rid = {"place_fnv": place_fnv(os.path.join(base, "PLACE.JSON")),
           "hdr_fnv": hdr_fnv(os.path.join(base, "HEIGHT.R16"), os.path.join(base, "WATER.R8")),
           "boxes": boxes,
           "blocks": [list(k) for k in keys],
           "base_fetched_blocks_changed": [list(k) for k in keys if k in base_fetched],
           "tool": "geo_roam_identity.py"}
    print(json.dumps({k: v for k, v in rid.items() if k != "boxes"}, indent=1))
    comp = {tuple(v) for v in ext.get("completed_blocks", [])}
    bad = [k for k in keys if k in base_fetched and k not in comp]
    if bad:
        print("WARNING: base fetched block(s) changed that are not completion blocks: %s" % bad)
    for k in keys:
        if k in comp and changed[k][0] < oh and changed[k][1] < ow:
            print("WARNING: completion block %s changed inside the base grid" % (k,))
    if "--write" in sys.argv:
        ext["roam_identity"] = rid
        tmp = os.path.join(place, "PLACE.JSON.tmp")
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(pj, f, indent=1, sort_keys=True)
            f.write("\n")
        os.replace(tmp, os.path.join(place, "PLACE.JSON"))
        print("wrote extension.roam_identity")
    return 0


if __name__ == "__main__":
    sys.exit(main())
