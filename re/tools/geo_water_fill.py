"""geo_water_fill.py -- finish the WATER of an EXTENDED tiled place (round S, roam SB).

geo_region's extension fetched Berisso and Ensenada block by block. Two kinds of
water are then still missing, both only in the NEW area:

  A. SEA PAST THE FETCHED BLOCKS. The Rio de la Plata beyond the last fetched
     block is nodata, and a nodata cell reads as dry ground at the HEIGHT bias
     (td5_roam_height.c): a flat plate at -1.5 m where the river should be.
     Every nodata component that touches a SEA cell (WATER 1 at the sea-floor
     clamp, what rasterize_layers writes for sea) becomes sea: HEIGHT at the
     clamp, WATER 1, COVER 80 (water), CANOPY 0.
  B. RIVERS OSM MAPS AS A RELATION. The block query fetches ways, so a
     riverbank multipolygon (the Rio Santiago, the port basins) never reaches
     WATER.R8, while ESA WorldCover marks it water (class 80). A COVER 80 cell
     that is dry becomes wet (WATER 1, HEIGHT untouched: the same as an OSM
     river way), except under a non-bridge road plus SEA_ROAD_CLEAR_M, the rule
     rasterize_layers applies to every kind of water.

Only cells OUTSIDE the base place's fetched blocks are ever written
(PLACE.JSON extension.base_fetched_blocks), so La Plata stays byte-identical;
the run proves it on exit.

usage: python geo_water_fill.py <place dir>            census only
       python geo_water_fill.py <place dir> --apply    write the rasters
"""
from __future__ import annotations

import json
import math
import os
import struct
import sys

import numpy as np
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from geo_raster import HEADER_SIZE, _HEADER  # noqa: E402

SEA_FLOOR_M = -4.0          # geo_fetch.SEA_FLOOR_CLAMP_M
SEA_ROAD_CLEAR_M = 6.0      # geo_fetch.SEA_ROAD_CLEAR_M
COVER_WATER = 80


def drop_wet_buildings(place: str, pj: dict, ext: dict, wat, apply: bool) -> dict:
    """C. A footprint whose centroid is on a WET cell, in a BUILDINGS tile homed outside the base fetched
    blocks, is dropped from the tile (the scenery has no water rule and builds it: 26 of 26 logged were
    emitted). Census of Gran La Plata: 70, 68 of them Overture machine-learned footprints over the port
    basins and the Rio Santiago, 2 OSM huts of 14 / 23 m2 on the shore's sea cells. The base place keeps
    its own 15 (a La Plata change otherwise). Rewrites the tile files and the INDEX rows it changes."""
    import geo_region as gr
    W, H = pj["tiled"]["grid"]
    x0, z0 = pj["tiled"]["origin"]
    cell = pj["cell_units"]
    side = float(pj["tiled"]["tile_size_units"])
    bt = int(pj["tiled"]["block_tiles"])
    base = {tuple(v) for v in ext["base_fetched_blocks"]}
    tdir = os.path.join(place, "tiles")
    dropped, files, by_src = 0, 0, {}
    for fn in sorted(os.listdir(tdir)):
        if not (fn.startswith("BUILDINGS_") and fn.endswith(".JSON")):
            continue
        try:
            ix, iz = (int(v) for v in fn[len("BUILDINGS_"):-5].split("_"))
        except ValueError:
            continue
        if (ix // bt, iz // bt) in base:
            continue
        path = os.path.join(tdir, fn)
        doc = json.load(open(path, encoding="utf-8"))
        keep = []
        for b in doc.get("buildings", []):
            p = b.get("points") or []
            if p:
                cx = sum(q["x"] for q in p) / len(p)
                cz = sum(q["z"] for q in p) / len(p)
                i = min(W - 1, max(0, int((cx - x0) / cell)))
                j = min(H - 1, max(0, int((cz - z0) / cell)))
                if wat[j, i] == 1:
                    dropped += 1
                    s = str(b.get("footprint_src") or "osm")
                    by_src[s] = by_src.get(s, 0) + 1
                    continue
            keep.append(b)
        if len(keep) != len(doc.get("buildings", [])):
            files += 1
            if apply:
                doc["buildings"] = keep
                with open(path + ".tmp", "w", encoding="utf-8", newline="\n") as f:
                    json.dump(doc, f, sort_keys=True, separators=(",", ":"))
                    f.write("\n")
                os.replace(path + ".tmp", path)
    if apply and files:
        class _F:
            pass
        fr = _F()
        fr.cell, fr.x0, fr.z0, fr.w, fr.h = cell, x0, z0, W, H
        lay = gr.Layout(fr, int(round(side / cell)), bt, [], 0.0)
        gr.build_index(tdir, lay, keep_other=True)
    return {"C_buildings_on_water_dropped": dropped, "C_tile_files": files, "C_by_source": by_src}


def main(argv=None) -> int:
    a = sys.argv[1:] if argv is None else argv
    place, apply = a[0], "--apply" in a
    pj = json.load(open(os.path.join(place, "PLACE.JSON"), encoding="utf-8"))
    ext = pj.get("extension") or {}
    if "base_fetched_blocks" not in ext:
        raise SystemExit("PLACE.JSON extension.base_fetched_blocks missing: not an extended place")
    W, H = pj["tiled"]["grid"]
    ow, oh = ext["base_grid"]
    bc = int(round(pj["tiled"]["tile_size_units"] / pj["cell_units"])) * int(pj["tiled"]["block_tiles"])
    upm = pj["projection"]["units_per_metre"]
    cell = pj["cell_units"]
    x0, z0 = pj["tiled"]["origin"]
    hdr = struct.unpack(_HEADER, open(os.path.join(place, "HEIGHT.R16"), "rb").read(HEADER_SIZE))
    sc, bi = hdr[7], hdr[8]
    floor_raw = int(round((SEA_FLOOR_M * upm - bi) / sc))
    mode = "r+" if apply else "r"
    mm = lambda n, dt: np.memmap(os.path.join(place, n), dtype=dt, mode=mode,  # noqa: E731
                                 offset=HEADER_SIZE, shape=(H, W))
    hgt, cov, wat, can = (mm("HEIGHT.R16", np.int16), mm("COVER.R8", np.uint8),
                          mm("WATER.R8", np.uint8), mm("CANOPY.R8", np.uint8))
    # the window: every new / completed block plus one block round it, clipped
    blks = ext.get("new_blocks", []) + ext.get("completed_blocks", [])
    bx0 = max(0, min(b[0] for b in blks) - 1)
    bz0 = max(0, min(b[1] for b in blks) - 1)
    i0, j0 = bx0 * bc, bz0 * bc
    i1, j1 = W, H
    h = np.asarray(hgt[j0:j1, i0:i1])
    c = np.asarray(cov[j0:j1, i0:i1])
    w = np.asarray(wat[j0:j1, i0:i1])
    # cells the base place owns (its fetched blocks inside the old grid): never written
    base = np.zeros(h.shape, bool)
    for bx, bz in ext["base_fetched_blocks"]:
        ja, jb = max(bz * bc, j0) - j0, min((bz + 1) * bc, oh) - j0
        ia, ib = max(bx * bc, i0) - i0, min((bx + 1) * bc, ow) - i0
        if ja < jb and ia < ib:
            base[ja:jb, ia:ib] = True
    rep = {"window_cells": [int(i0), int(j0), int(i1), int(j1)], "floor_raw": floor_raw}

    # ---- A. sea past the fetched blocks
    nod = (h == -32768) & ~base
    sea = (w == 1) & (h <= floor_raw + 2)
    lab, n = ndimage.label(nod)
    touch = np.unique(lab[ndimage.binary_dilation(sea) & nod])
    touch = touch[touch > 0]
    fill = np.isin(lab, touch)
    rep["A_nodata_components"] = int(n)
    rep["A_components_touching_sea"] = int(touch.size)
    rep["A_cells_to_sea"] = int(fill.sum())
    rep["A_nodata_left"] = int((nod & ~fill).sum())

    # ---- B. WorldCover water that WATER.R8 does not mark
    cand = (c == COVER_WATER) & (w == 0) & (h != -32768) & ~base
    road = np.zeros(h.shape, bool)
    idx = json.load(open(os.path.join(place, "tiles", "INDEX.JSON"), encoding="utf-8"))
    wx0, wz0 = x0 + i0 * cell, z0 + j0 * cell
    wx1, wz1 = x0 + i1 * cell, z0 + j1 * cell
    rads: dict = {}
    nroads = 0
    for row in idx["layers"]["ROADS"]["tiles"]:
        tx, tz, _, mnx, mnz, mxx, mxz = row
        if mxx < wx0 or mnx > wx1 or mxz < wz0 or mnz > wz1:
            continue
        for rd in json.load(open(os.path.join(place, "tiles", "ROADS_%d_%d.JSON" % (tx, tz)),
                                 encoding="utf-8")).get("roads", []):
            if rd.get("bridge") or rd.get("tunnel"):
                continue
            try:
                wd = float(str(rd.get("width") or "").split()[0].replace(",", "."))
            except (ValueError, IndexError):
                wd = 0.0
            try:
                ln = float(rd.get("lanes") or 2)
            except (TypeError, ValueError):
                ln = 2.0
            half_m = (wd if wd > 0.0 else ln * 3.5) * 0.5
            rc = max(1, int(math.ceil((half_m + SEA_ROAD_CLEAR_M) / (cell / upm))))
            m = rads.setdefault(rc, [])
            pts = rd.get("points") or []
            for p, q in zip(pts, pts[1:]):
                dx, dz = q["x"] - p["x"], q["z"] - p["z"]
                k = max(1, int(math.hypot(dx, dz) / (cell * 0.5)))
                for t in range(k + 1):
                    ix = int(round((p["x"] + dx * t / k - wx0) / cell))
                    iz = int(round((p["z"] + dz * t / k - wz0) / cell))
                    if 0 <= ix < h.shape[1] and 0 <= iz < h.shape[0]:
                        m.append((iz, ix))
            nroads += 1
    for rc, cells in rads.items():
        if not cells:
            continue
        mk = np.zeros(h.shape, bool)
        ca = np.array(cells)
        mk[ca[:, 0], ca[:, 1]] = True
        yy, xx = np.mgrid[-rc:rc + 1, -rc:rc + 1]
        road |= ndimage.binary_dilation(mk, structure=(xx * xx + yy * yy) <= rc * rc)
    wet_b = cand & ~road
    rep["B_roads_in_window"] = nroads
    rep["B_road_mask_cells"] = int(road.sum())
    rep["B_wc80_dry"] = int(cand.sum())
    rep["B_kept_dry_under_roads"] = int((cand & road).sum())
    rep["B_cells_to_wet"] = int(wet_b.sum())

    # ---- C. footprints standing in the water (new area only)
    rep.update(drop_wet_buildings(place, pj, ext, wat, apply))

    if apply:
        h2 = np.where(fill, floor_raw, h).astype(np.int16)
        w2 = np.where(fill | wet_b, 1, w).astype(np.uint8)
        c2 = np.where(fill, COVER_WATER, c).astype(np.uint8)
        cn = np.asarray(can[j0:j1, i0:i1])
        n2 = np.where(fill, 0, cn).astype(np.uint8)
        assert not (base & ((h2 != h) | (w2 != w) | (c2 != c) | (n2 != cn))).any()
        hgt[j0:j1, i0:i1] = h2
        wat[j0:j1, i0:i1] = w2
        cov[j0:j1, i0:i1] = c2
        can[j0:j1, i0:i1] = n2
        for r in (hgt, wat, cov, can):
            r.flush()
        wf = pj.setdefault("layers", {}).setdefault("water_fill", {})
        for k, v in (("sea_cells_filled", rep["A_cells_to_sea"]),
                     ("worldcover_water_cells_wet", rep["B_cells_to_wet"]),
                     ("kept_dry_under_roads", rep["B_kept_dry_under_roads"]),
                     ("buildings_on_water_dropped", rep["C_buildings_on_water_dropped"])):
            wf[k] = wf.get(k, 0) + v        # a rerun adds what it did (0 once applied)
        wf["tool"] = "geo_water_fill.py"
        wf["rule"] = "only cells / tiles outside extension.base_fetched_blocks"
        tmp = os.path.join(place, "PLACE.JSON.tmp")
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(pj, f, indent=1, sort_keys=True)
            f.write("\n")
        os.replace(tmp, os.path.join(place, "PLACE.JSON"))
    print(json.dumps(rep, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
