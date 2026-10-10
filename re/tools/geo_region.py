"""geo_region.py -- fetch a WHOLE ADMINISTRATIVE AREA as one geo place.

geo_fetch.fetch_place makes a square of side 2*radius (4 km of radius is the
hard cap, FETCH_MAX_RADIUS_M) and holds every layer in ONE file. That is the
right shape for a 5 km city-centre box and the wrong one for "the whole Partido
de La Plata" (about 926 km2): one Overpass answer for it does not exist, and one
BUILDINGS.JSON for it would be ~150 MB of cJSON.

This tool builds the same place CONTRACT (see geo_common.CONTRACT_FILES) for an
area bounded by an OSM admin relation, in pieces:

  1. BOUNDARY   one Overpass call for the relation -> outer ring(s). The place
                bbox is the ring's bbox; the ring itself is kept in PLACE.JSON
                (`boundary`) so a click outside the polygon but inside the bbox
                is "outside the area", and unfetched blocks are nodata.
  2. BLOCKS     the bbox is cut into BLOCK_M squares of one fixed frame; only the
                blocks the ring touches are fetched.
  3. PER BLOCK  the SAME code geo_fetch uses for a whole place -- fetch_osm,
                convert_osm, the Overture + Open Buildings step, rasterize_layers,
                estimate_building_heights -- on the block plus a HALO, so nothing
                near a block edge is decided on half a neighbourhood.
  4. MERGE      every entry has an id (OSM way/node id, Overture GERS hash). An
                entry is KEPT BY ONE BLOCK ONLY: the block whose interior holds
                its first vertex, or, when that block was not fetched, the first
                fetched block that returned it. Vectors are written as TILES.
  5. RASTERS    HEIGHT / COVER / WATER / CANOPY are written into four flat files
                on ONE global cell lattice (origin a multiple of the cell), so a
                reader can seek straight to the window it wants. HEIGHT uses one
                global scale/bias for the whole area.

THE ON-DISK FORMAT (PLACE.JSON `tiled` block, read by td5_geo_route.c)

    <slug>/PLACE.JSON      + "tiled": {"version":1,"size":S,"x0":..,"z0":..,
                                       "nx":..,"nz":..,"layers":{...}}
                           + "boundary": [[lat,lon],...]  (simplified ring)
    <slug>/HEIGHT.R16 ...  the usual TD5GEOR1 rasters, but the whole area
    <slug>/tiles/<LAYER>_<ix>_<iz>.JSON
                           the usual layer document ({"roads":[...]} etc.)
                           holding the entries whose ANCHOR is in tile (ix,iz);
                           `ext` in the index is the bbox of everything the tile
                           holds, so a window query is exact even for a long way.
    <slug>/tiles/INDEX.JSON  per tile: counts and `ext` for each layer.

The frame is UNROTATED (rotation 0, offset 0) about the area centre. The game
never races this frame: td5_geo_route_commit cuts a WINDOW around the route out of
it and writes the usual `_route/` derived frame, so the track generator still
sees a place of about the size it always saw.

NETWORK DISCIPLINE. `--online` is required for any request that is not already in
the cache; without it the run is a DRY RUN that names every request it would make
(and cannot make the Overpass calls whose bbox depends on the boundary). All
Overpass calls are serial with RATE_SLEEP_S between uncached ones.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import geo_fetch as gf  # noqa: E402
from geo_common import (  # noqa: E402
    CACHE_ROOT,
    LocalProjection,
    TG_WORLD_CELL,
    UNITS_PER_METRE_DERIVED,
    place_dir,
    slugify,
    write_json,
)
from geo_raster import Raster  # noqa: E402

BLOCK_M = 6000.0          # side of one fetch block, metres
HALO_M = 400.0            # fetched beyond the block on every side
TILE_M = 2000.0           # side of one vector tile, metres (see tile_size_units)
RATE_SLEEP_S = 4.0        # between UNCACHED Overpass calls (fair use)
BOUNDARY_SIMPLIFY_M = 15.0
TILED_VERSION = 1

# Vector layers a tiled place splits, file -> (array key, second array key).
# Same table td5_geo_route.c's k_vec holds; keep them in step.
TILED_LAYERS = {
    "ROADS.JSON": ("roads", None),
    "BUILDINGS.JSON": ("buildings", None),
    "AREAS.JSON": ("areas", None),
    "SIGNALS.JSON": ("signals", "nodes"),
    "FOOTWAYS.JSON": ("footways", None),
}


# ----------------------------------------------------------------- network ---

class Budget:
    """Counts every request geo_region itself makes (the Overpass calls, the
    terrarium tiles and the boundary). The COG and building-extra layers keep
    their own counters."""

    def __init__(self, online: bool):
        self.online = online
        self.would: list[str] = []
        self.hits = 0
        self.fetched: list[tuple[str, int]] = []

    def note_hit(self):
        self.hits += 1


def cached_or_would(budget: Budget, url: str, root: str, ext: str,
                    data: bytes | None, key: str | None) -> bytes | None:
    """Return the cached body, or fetch it when online, or record the miss."""
    ck = key or (url + "\x00" + (data.decode("utf-8", "replace") if data else ""))
    path = gf._cache_path(root, ck, ext)
    if os.path.exists(path) and os.path.getsize(path) > 0:
        budget.note_hit()
        with open(path, "rb") as f:
            return f.read()
    desc = url + (" [POST %d B]" % len(data) if data else "")
    if not budget.online:
        budget.would.append(desc)
        return None
    body = gf._cached_get(url, root, ext, data=data, key=key)
    budget.fetched.append((desc, len(body)))
    if "overpass" in url:
        time.sleep(RATE_SLEEP_S)
    return body


# ----------------------------------------------------------------- boundary ---

BOUNDARY_QL = """[out:json][timeout:120];
rel["boundary"="administrative"]["admin_level"="%(level)s"]["name"="%(name)s"]%(around)s;
out geom;
"""
BOUNDARY_ID_QL = """[out:json][timeout:120];
rel(%(id)d);
out geom;
"""


def boundary_query(spec: dict) -> str:
    rel = spec["relation"]
    if rel.get("id"):
        return BOUNDARY_ID_QL % {"id": int(rel["id"])}
    around = ""
    if rel.get("near"):
        around = "(around:%d,%.5f,%.5f)" % (int(rel.get("near_m", 60000)),
                                             rel["near"][0], rel["near"][1])
    return BOUNDARY_QL % {"level": rel["admin_level"], "name": rel["name"],
                          "around": around}


def _join_rings(ways: list[list[tuple[float, float]]]
                ) -> list[list[tuple[float, float]]]:
    """Chain outer ways end to end into closed rings. OSM splits a boundary
    into many ways that share end nodes; the relation lists them unordered."""
    rings: list[list[tuple[float, float]]] = []
    pool = [list(w) for w in ways if len(w) >= 2]
    while pool:
        cur = pool.pop(0)
        grew = True
        while grew and cur[0] != cur[-1]:
            grew = False
            for i, w in enumerate(pool):
                if w[0] == cur[-1]:
                    cur.extend(w[1:])
                elif w[-1] == cur[-1]:
                    cur.extend(reversed(w[:-1]))
                elif w[-1] == cur[0]:
                    cur = w[:-1] + cur
                elif w[0] == cur[0]:
                    cur = list(reversed(w[1:])) + cur
                else:
                    continue
                pool.pop(i)
                grew = True
                break
        rings.append(cur)
    return rings


def parse_boundary(raw: bytes) -> dict:
    """Overpass `rel ... out geom` -> {id, name, tags, rings:[[(lat,lon)...]]}.
    When several relations match, the one with the largest ring wins."""
    doc = json.loads(raw)
    best = None
    for el in doc.get("elements", []):
        if el.get("type") != "relation":
            continue
        outer = []
        for m in el.get("members", []):
            if m.get("type") == "way" and m.get("role") == "outer" and m.get("geometry"):
                outer.append([(g["lat"], g["lon"]) for g in m["geometry"]])
        rings = [r for r in _join_rings(outer) if len(r) >= 4 and r[0] == r[-1]]
        if not rings:
            continue
        area = max(_ring_area_m2(r) for r in rings)
        cand = {"id": el["id"], "tags": el.get("tags", {}), "rings": rings,
                "area_m2": area}
        if best is None or area > best["area_m2"]:
            best = cand
    if best is None:
        raise RuntimeError("no boundary relation with a closed outer ring in the "
                           "Overpass answer")
    return best


def _ring_area_m2(ring) -> float:
    lat0 = sum(p[0] for p in ring) / len(ring)
    ky = 110_574.0
    kx = 111_320.0 * math.cos(math.radians(lat0))
    a = 0.0
    for (la0, lo0), (la1, lo1) in zip(ring, ring[1:]):
        a += (lo0 * kx) * (la1 * ky) - (lo1 * kx) * (la0 * ky)
    return abs(a) * 0.5


def simplify_ring(ring, tol_m: float) -> list[tuple[float, float]]:
    """Douglas-Peucker on a lat/lon ring, tolerance in metres."""
    from shapely.geometry import LineString
    lat0 = sum(p[0] for p in ring) / len(ring)
    ky = 110_574.0
    kx = 111_320.0 * math.cos(math.radians(lat0))
    ls = LineString([(lo * kx, la * ky) for la, lo in ring])
    s = ls.simplify(tol_m, preserve_topology=True)
    out = [(y / ky, x / kx) for x, y in s.coords]
    if out[0] != out[-1]:
        out.append(out[0])
    return out


# ------------------------------------------------------------------- frame ---

class Frame:
    """The one frame of a region: unrotated local tangent plane, offset 0, with
    a cell lattice whose origin is a multiple of `cell` (so every block, every
    raster and every later window agree on where cell (0, 0) is)."""

    def __init__(self, bbox_ll, cell: float, upm: float):
        s, w, n, e = bbox_ll
        self.proj = LocalProjection(0.5 * (s + n), 0.5 * (w + e), upm)
        self.cell = cell
        self.upm = upm
        xs, zs = [], []
        for la in (s, n):
            for lo in (w, e):
                x, z = self.proj.to_world(la, lo)
                xs.append(x)
                zs.append(z)
        self.x0 = math.floor(min(xs) / cell) * cell
        self.z0 = math.floor(min(zs) / cell) * cell
        self.w = int(math.ceil((max(xs) - self.x0) / cell)) + 1
        self.h = int(math.ceil((max(zs) - self.z0) / cell)) + 1

    def cells_per_m(self) -> float:
        return self.upm / self.cell


def plan_blocks(frame: Frame, rings_ll, tile_cells: int, block_tiles: int):
    """Blocks of the frame that the boundary touches, in a stable order.

    The tile grid and the block grid share the lattice origin, and a block is
    `block_tiles` tiles on a side, so every tile belongs to exactly one block.
    A block is an axis-aligned square in WORLD units; with rotation 0 that is a
    lat/lon rectangle too, which is what Overpass wants."""
    from shapely.geometry import Polygon, box
    poly = None
    for r in rings_ll:
        pts = [frame.proj.to_world(la, lo) for la, lo in r]
        p = Polygon(pts).buffer(0)
        poly = p if poly is None else poly.union(p)
    bcells = tile_cells * block_tiles
    side = bcells * frame.cell
    nbx = int(math.ceil(frame.w / bcells))
    nbz = int(math.ceil(frame.h / bcells))
    blocks = []
    for bz in range(nbz):
        for bx in range(nbx):
            bx0 = frame.x0 + bx * side
            bz0 = frame.z0 + bz * side
            if poly.intersects(box(bx0, bz0, bx0 + side, bz0 + side)):
                blocks.append({"bx": bx, "bz": bz, "x0": bx0, "z0": bz0,
                               "x1": bx0 + side, "z1": bz0 + side,
                               "i0": bx * bcells, "j0": bz * bcells,
                               "cells": bcells})
    return blocks, poly, side


def world_box_to_ll(frame: Frame, x0, z0, x1, z1):
    """(south, west, north, east) of a world box (rotation 0 => exact)."""
    la0, lo0 = frame.proj.world_to_latlon(x0, z0)
    la1, lo1 = frame.proj.world_to_latlon(x1, z1)
    return (min(la0, la1), min(lo0, lo1), max(la0, la1), max(lo0, lo1))


# --------------------------------------------------------------------- spec ---

def load_spec(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        spec = json.load(f)
    spec.setdefault("slug", slugify(spec["name"]))
    spec.setdefault("block_m", BLOCK_M)
    spec.setdefault("halo_m", HALO_M)
    spec.setdefault("tile_m", TILE_M)
    return spec


# ====================================================== grids and the DEM ===

def latlon_grid(frame: Frame, i0: int, i1: int, j0: int, j1: int):
    """lat/lon of every cell centre in [i0,i1) x [j0,j1) of the global lattice.
    World -> lat/lon is affine, so this is a handful of broadcast multiplies."""
    p = frame.proj
    ii = np.arange(i0, i1, dtype=np.float64)[None, :]
    jj = np.arange(j0, j1, dtype=np.float64)[:, None]
    xm = (frame.x0 + ii * frame.cell - p._off_x) / p.units_per_metre
    zm = (frame.z0 + jj * frame.cell - p._off_z) / p.units_per_metre
    e = xm * p._cos_t + zm * p._sin_t
    n = -xm * p._sin_t + zm * p._cos_t
    return (p.lat0 + n / p._m_per_deg_lat), (p.lon0 + e / p._m_per_deg_lon)


def _tilef_vec(lon, lat, z):
    n = 2.0 ** z
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - np.arcsinh(np.tan(np.radians(lat))) / np.pi) / 2.0 * n
    return x, y


def terrarium_tiles_for(lat, lon, z):
    x, y = _tilef_vec(lon, lat, z)
    return (int(math.floor(x.min())), int(math.floor(x.max())),
            int(math.floor(y.min())), int(math.floor(y.max())))


def load_mosaic(budget: Budget, root: str, z: int, tx0, tx1, ty0, ty1):
    """Terrarium tiles tx0..tx1 x ty0..ty1 as one float64 mosaic of metres, or
    None (and the misses recorded) when a tile is not cached and we are offline."""
    nx, ny = tx1 - tx0 + 1, ty1 - ty0 + 1
    if nx * ny > 144:
        raise RuntimeError("block needs %d terrain tiles at z=%d" % (nx * ny, z))
    from PIL import Image
    import io
    mosaic = np.zeros((ny * 256, nx * 256), dtype=np.float64)
    ok = True
    for j in range(ny):
        for i in range(nx):
            url = gf.TERRARIUM_URL % (z, tx0 + i, ty0 + j)
            raw = cached_or_would(budget, url, root, ".png", None, None)
            if raw is None:
                ok = False
                continue
            a = np.asarray(Image.open(io.BytesIO(raw)).convert("RGB"),
                           dtype=np.float64)
            mosaic[j * 256:(j + 1) * 256, i * 256:(i + 1) * 256] = \
                (a[:, :, 0] * 256.0 + a[:, :, 1] + a[:, :, 2] / 256.0) - 32768.0
    return mosaic if ok else None


def dem_metres(frame: Frame, budget: Budget, root: str,
               i0: int, i1: int, j0: int, j1: int, smooth_m: float,
               zoom: int | None = None):
    """Elevation in metres for cells [i0,i1) x [j0,j1), same recipe as
    geo_fetch.build_height_raster (sea-floor clamp, then a box lowpass), but
    vectorised -- 132 M cells cannot go through a per-cell Python loop.

    The caller pads the range by the lowpass radius on every side it is not on
    the lattice edge, so the blur sees real neighbours (the original edge-pads at
    the grid edge, and so does this)."""
    lat, lon = latlon_grid(frame, i0, i1, j0, j1)
    cell_m = frame.cell / frame.upm
    z = zoom or gf._terrarium_zoom(float(lat.mean()), cell_m)
    tx0, tx1, ty0, ty1 = terrarium_tiles_for(lat, lon, z)
    mosaic = load_mosaic(budget, root, z, tx0, tx1, ty0, ty1)
    if mosaic is None:
        return None, z
    tx, ty = _tilef_vec(lon, lat, z)
    h, w = mosaic.shape
    px = np.clip((tx - tx0) * 256.0, 0.0, w - 1.0)
    py = np.clip((ty - ty0) * 256.0, 0.0, h - 1.0)
    ix = np.minimum(px.astype(np.int64), w - 1)
    iy = np.minimum(py.astype(np.int64), h - 1)
    jx = np.minimum(ix + 1, w - 1)
    jy = np.minimum(iy + 1, h - 1)
    fx, fy = px - ix, py - iy
    out = ((mosaic[iy, ix] * (1 - fx) + mosaic[iy, jx] * fx) * (1 - fy)
           + (mosaic[jy, ix] * (1 - fx) + mosaic[jy, jx] * fx) * fy)
    del tx, ty, px, py, ix, iy, jx, jy, fx, fy, lat, lon
    out = np.maximum(out, gf.SEA_FLOOR_CLAMP_M)
    rad = int(round(0.5 * smooth_m / cell_m)) if (cell_m > 0 and smooth_m > 0) else 0
    if rad >= 1:
        out = gf._box_blur(out, rad)
    return out.astype(np.float32), z


# ============================================================ raster files ===

class RasterFile:
    """One flat TD5GEOR1 raster on disk, written block by block through a
    memmap. The header is rewritten at the end (HEIGHT only learns its scale and
    bias once every block has been seen)."""

    def __init__(self, path: str, kind: int, w: int, h: int, frame: Frame,
                 nodata_raw: int, scale=1.0, bias=0.0, rotation=0.0):
        import struct
        from geo_raster import _HEADER, HEADER_SIZE, MAGIC
        self.path, self.kind, self.w, self.h = path, kind, w, h
        self.frame, self.nodata_raw = frame, nodata_raw
        self.scale, self.bias, self.rotation = scale, bias, rotation
        self._struct, self._HEADER, self.HEADER_SIZE, self.MAGIC = \
            struct, _HEADER, HEADER_SIZE, MAGIC
        self.dtype = np.int16 if kind == 1 else np.uint8
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if not os.path.exists(path) or \
                os.path.getsize(path) != HEADER_SIZE + w * h * np.dtype(self.dtype).itemsize:
            with open(path, "wb") as f:
                f.write(self.header_bytes())
                row = np.full(w, nodata_raw, dtype=self.dtype).tobytes()
                for _ in range(h):
                    f.write(row)
        self.mm = np.memmap(path, dtype=self.dtype, mode="r+",
                            offset=HEADER_SIZE, shape=(h, w))

    def header_bytes(self) -> bytes:
        return self._struct.pack(self._HEADER, self.MAGIC, self.kind, self.w,
                                 self.h, self.frame.x0, self.frame.z0,
                                 self.frame.cell, self.scale, self.bias,
                                 self.nodata_raw, self.rotation)

    def finish(self) -> None:
        self.mm.flush()
        del self.mm
        with open(self.path, "r+b") as f:
            f.write(self.header_bytes())


# ======================================================== entries and tiles ===

def entry_anchor_bbox(e: dict):
    """(ax, az, minx, minz, maxx, maxz) of one vector entry, or None."""
    pts = e.get("points")
    if pts:
        xs = [p["x"] for p in pts]
        zs = [p["z"] for p in pts]
        return pts[0]["x"], pts[0]["z"], min(xs), min(zs), max(xs), max(zs)
    if "x" in e and "z" in e:
        return e["x"], e["z"], e["x"], e["z"], e["x"], e["z"]
    return None


class Layout:
    """Where tiles and blocks sit. One object so the writer and the owner rule
    cannot disagree about a tile boundary."""

    def __init__(self, frame: Frame, tile_cells: int, block_tiles: int,
                 blocks: list, halo_units: float):
        self.frame = frame
        self.tile_cells = tile_cells
        self.block_tiles = block_tiles
        self.tile_side = tile_cells * frame.cell
        self.block_side = self.tile_side * block_tiles
        self.nx = int(math.ceil(frame.w / tile_cells))
        self.nz = int(math.ceil(frame.h / tile_cells))
        self.blocks = blocks
        self.fetched = {(b["bx"], b["bz"]): k for k, b in enumerate(blocks)}
        self.halo = halo_units

    def tile_of(self, x: float, z: float):
        return (int(math.floor((x - self.frame.x0) / self.tile_side)),
                int(math.floor((z - self.frame.z0) / self.tile_side)))

    def block_of_tile(self, ix: int, iz: int):
        return ix // self.block_tiles, iz // self.block_tiles

    def owner(self, ax, az, minx, minz, maxx, maxz):
        """The ONE block that keeps this entry (see the module docstring)."""
        ix, iz = self.tile_of(ax, az)
        bk = self.block_of_tile(ix, iz)
        if bk in self.fetched:
            return bk
        for b in self.blocks:       # first fetched block whose halo box meets it
            if (maxx >= b["x0"] - self.halo and minx <= b["x1"] + self.halo
                    and maxz >= b["z0"] - self.halo and minz <= b["z1"] + self.halo):
                return (b["bx"], b["bz"])
        return None


def split_owned(vec: dict, lay: Layout, blk: dict) -> dict:
    """{tile (ix,iz): {layer file: {array key: [entries]}}} for the entries this
    block owns, plus the count of orphans (anchor in an unfetched block)."""
    me = (blk["bx"], blk["bz"])
    out: dict = {}
    n_orph = 0
    for fname, (k1, k2) in TILED_LAYERS.items():
        for key in (k1, k2):
            if not key:
                continue
            for e in vec.get(key, []):
                bb = entry_anchor_bbox(e)
                if bb is None:
                    continue
                ow = lay.owner(*bb)
                if ow != me:
                    continue
                t = lay.tile_of(bb[0], bb[1])
                if lay.block_of_tile(*t) != me:
                    n_orph += 1
                out.setdefault(t, {}).setdefault(fname, {}).setdefault(key, []).append(e)
    return out, n_orph


def layer_name(fname: str) -> str:
    return fname.rsplit(".", 1)[0]


def write_tile_docs(tiles_dir: str, owned: dict, orph_tag: str | None,
                    storey_m: float) -> int:
    """Write one block's owned entries. A tile whose home block is this one is
    written whole; entries anchored elsewhere (orphans) go to a side file that
    the final merge folds in."""
    n = 0
    for (ix, iz), layers in owned.items():
        for fname, arrs in layers.items():
            doc = dict(arrs)
            k1, k2 = TILED_LAYERS[fname]
            doc.setdefault(k1, [])
            if k2:
                doc.setdefault(k2, [])
            if fname == "BUILDINGS.JSON":
                doc["storey_height_m"] = storey_m
                # The same default-dropping fetch_place applies to an Overture
                # record before it writes BUILDINGS.JSON: the uncompacted tile
                # was 68 MB of derived JSON for 102k footprints, vs 41 MB.
                doc["buildings"] = [gf._xb_compact(b) for b in doc["buildings"]]
            name = "%s_%d_%d.JSON" % (layer_name(fname), ix, iz)
            path = os.path.join(tiles_dir, name if not orph_tag else
                                os.path.join("_orph", name + "." + orph_tag))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path + ".tmp", "w", encoding="utf-8", newline="\n") as f:
                json.dump(doc, f, sort_keys=True, separators=(",", ":"))
                f.write("\n")
            os.replace(path + ".tmp", path)
            n += 1
    return n


def split_home_orphans(owned: dict, lay: Layout, me):
    """Separate tiles homed in `me` from tiles homed elsewhere."""
    home, away = {}, {}
    for t, v in owned.items():
        (home if lay.block_of_tile(*t) == me else away)[t] = v
    return home, away


def merge_orphans(tiles_dir: str) -> int:
    """Fold every tiles/_orph/<name>.<tag> file into tiles/<name>."""
    od = os.path.join(tiles_dir, "_orph")
    if not os.path.isdir(od):
        return 0
    groups: dict[str, list[str]] = {}
    for fn in sorted(os.listdir(od)):
        if fn.endswith(".tmp"):
            continue
        base = fn.rsplit(".JSON.", 1)[0] + ".JSON"
        groups.setdefault(base, []).append(os.path.join(od, fn))
    for base, files in groups.items():
        path = os.path.join(tiles_dir, base)
        doc = None
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                doc = json.load(f)
        for fp in files:
            with open(fp, encoding="utf-8") as f:
                d = json.load(f)
            if doc is None:
                doc = d
                continue
            for k, v in d.items():
                if isinstance(v, list):
                    doc.setdefault(k, []).extend(v)
                else:
                    doc.setdefault(k, v)
        with open(path + ".tmp", "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, sort_keys=True, separators=(",", ":"))
            f.write("\n")
        os.replace(path + ".tmp", path)
    for base, files in groups.items():
        for fp in files:
            os.remove(fp)
    return len(groups)


def build_index(tiles_dir: str, lay: Layout) -> dict:
    """INDEX.JSON: per layer, per tile: [ix, iz, n, minx, minz, maxx, maxz]
    where the box is the extent of EVERYTHING the tile holds (a long road
    anchored in one tile still reaches the next), so a window query is exact."""
    layers = {}
    for fname, (k1, k2) in TILED_LAYERS.items():
        ln = layer_name(fname)
        rows = []
        for fn in sorted(os.listdir(tiles_dir)):
            if not fn.startswith(ln + "_") or not fn.endswith(".JSON"):
                continue
            try:
                ix, iz = [int(v) for v in fn[len(ln) + 1:-5].split("_")]
            except ValueError:
                continue
            with open(os.path.join(tiles_dir, fn), encoding="utf-8") as f:
                doc = json.load(f)
            n = 0
            ext = [1e300, 1e300, -1e300, -1e300]
            for key in (k1, k2):
                for e in doc.get(key, []) if key else []:
                    n += 1
                    bb = entry_anchor_bbox(e)
                    if bb is None:
                        continue
                    ext[0] = min(ext[0], bb[2])
                    ext[1] = min(ext[1], bb[3])
                    ext[2] = max(ext[2], bb[4])
                    ext[3] = max(ext[3], bb[5])
            if n:
                rows.append([ix, iz, n] + [round(v, 2) for v in ext])
        layers[ln] = {"file": fname, "key": k1, "key2": k2, "tiles": rows}
    idx = {"version": TILED_VERSION, "cell": lay.frame.cell,
           "size": lay.tile_side, "x0": lay.frame.x0, "z0": lay.frame.z0,
           "nx": lay.nx, "nz": lay.nz, "layers": layers}
    write_json(os.path.join(tiles_dir, "INDEX.JSON"), idx)
    return idx


# ============================================================== per block ===

class Ctx:
    """Everything one run shares across blocks."""

    def __init__(self, spec, frame, lay, budget, work, online):
        self.spec, self.frame, self.lay = spec, frame, lay
        self.budget, self.work, self.online = budget, work, online
        self.tiles_dir = os.path.join(work, "tiles")
        self.region_dir = os.path.join(work, "_region")
        self.dem = None
        self.cover = self.water = self.canopy = None
        self.opt = spec.get("layers", {})
        self.smooth_m = float(spec.get("dem_smooth_m", gf.DEM_SMOOTH_M_DEFAULT))
        self.levels_estimator = int(spec.get("levels_estimator", 2))


def overpass_for_box(ctx: Ctx, bbox_ll):
    """The Overpass answer for a box, from cache or (online) the network. The
    cache key is geo_fetch.fetch_osm's, so an answer fetched by either tool
    serves both."""
    s, w, n, e = bbox_ll
    ql = gf.OVERPASS_QL % {"bbox": "%.6f,%.6f,%.6f,%.6f" % (s, w, n, e)}
    last = None
    for ep in gf.OVERPASS_ENDPOINTS:
        try:
            raw = cached_or_would(ctx.budget, ep, ctx.work, ".json",
                                  ql.encode("utf-8"), "overpass\x00" + ql)
        except Exception as exc:               # noqa: BLE001 - try the mirror
            last = exc
            continue
        if raw is None:
            return None
        return json.loads(raw)
    raise RuntimeError("every Overpass endpoint failed: %s" % last)


def block_ranges(ctx: Ctx, blk: dict):
    """Cell ranges of one block: interior, the halo grid G, and the DEM grid D
    (G widened by the lowpass radius), all clipped to the lattice."""
    fr = ctx.frame
    hc = int(math.ceil(ctx.lay.halo / fr.cell))
    cell_m = fr.cell / fr.upm
    rad = int(round(0.5 * ctx.smooth_m / cell_m)) if ctx.smooth_m > 0 else 0

    def clip(a0, a1, hi):
        return max(0, a0), min(hi, a1)

    i0, i1 = clip(blk["i0"], blk["i0"] + blk["cells"], fr.w)
    j0, j1 = clip(blk["j0"], blk["j0"] + blk["cells"], fr.h)
    g = (clip(i0 - hc, i1 + hc, fr.w), clip(j0 - hc, j1 + hc, fr.h))
    d = (clip(g[0][0] - rad - 1, g[0][1] + rad + 1, fr.w),
         clip(g[1][0] - rad - 1, g[1][1] + rad + 1, fr.h))
    return (i0, i1, j0, j1), g, d


def process_block(ctx: Ctx, blk: dict, plan_only: bool = False) -> dict | None:
    """Fetch (or read from cache) and convert ONE block. Returns its summary, or
    None when offline and something is not cached."""
    fr, lay = ctx.frame, ctx.lay
    tag = "b%d_%d" % (blk["bx"], blk["bz"])
    done = os.path.join(ctx.region_dir, "done", tag + ".json")
    if os.path.exists(done) and not plan_only:
        with open(done, encoding="utf-8") as f:
            return json.load(f)
    t0 = time.time()
    halo = lay.halo
    bbox_ll = world_box_to_ll(fr, blk["x0"] - halo, blk["z0"] - halo,
                              blk["x1"] + halo, blk["z1"] + halo)
    interior, G, D = block_ranges(ctx, blk)

    osm = overpass_for_box(ctx, bbox_ll)
    # DEM tiles are requested even on a dry run so the inventory is complete.
    dem, z = dem_metres(fr, ctx.budget, ctx.work, D[0][0], D[0][1], D[1][0],
                        D[1][1], ctx.smooth_m)
    if osm is None or dem is None:
        return None
    if plan_only:
        return {"planned": True}

    vec = gf.convert_osm(osm, fr.proj, ctx.levels_estimator)
    stats_x = None
    if ctx.opt.get("extra_buildings", True):
        import geo_buildings_extra as xb
        xb.fetch_extra(ctx.work, bbox_ll, ctx.online,
                       ctx.spec.get("overture_release"),
                       int(ctx.spec.get("extra_max_mb", 900) * 1024 * 1024), False)
        stats_x = gf.conflate_extra_buildings(vec, fr.proj, ctx.work, bbox_ll)

    # Crop the DEM to the halo grid G and give rasterize_layers a template.
    gi0, gi1 = G[0]
    gj0, gj1 = G[1]
    ci0, cj0 = gi0 - D[0][0], gj0 - D[1][0]
    dem_g = dem[cj0:cj0 + (gj1 - gj0), ci0:ci0 + (gi1 - gi0)]
    metres = dem_g.astype(np.float64)
    lo_m, hi_m = float(metres.min()), float(metres.max())
    bias_u = lo_m * fr.upm
    span_u = max(1.0, (hi_m - lo_m) * fr.upm)
    scale = span_u / 32000.0
    raw = np.clip(np.rint((metres * fr.upm - bias_u) / scale),
                  -32000, 32000).astype(np.int16)
    tmpl = Raster(raw, fr.x0 + gi0 * fr.cell, fr.z0 + gj0 * fr.cell, fr.cell,
                  scale=scale, bias=bias_u, nodata_raw=-32768,
                  rotation_rad=fr.proj.describe()["rotation_rad"])
    raw0 = raw.copy()

    base_cover = canopy_arr = wc_prov = cn_prov = None
    lat, lon = latlon_grid(fr, gi0, gi1, gj0, gj1)
    cache_dir = os.path.join(ctx.work, "_cache")
    if ctx.opt.get("land_cover", "worldcover") == "worldcover":
        base_cover, wc_prov = gf.worldcover_grid(lat, lon, cache_dir)
    if ctx.opt.get("canopy", True):
        canopy_arr, cn_prov = gf.canopy_grid(lat, lon, cache_dir)
    del lat, lon
    cover_r, water_r, painted = gf.rasterize_layers(vec, fr.proj, tmpl, bbox_ll,
                                                    base_cover)
    bh = gf.estimate_building_heights(vec, cover_r, ctx.levels_estimator)

    # A coastline sinks the sea floor inside rasterize_layers; carry that back
    # into the metres so the global HEIGHT sees it.
    changed = tmpl.data != raw0
    if changed.any():
        dem_g = np.where(changed,
                         (tmpl.data.astype(np.float64) * scale + bias_u) / fr.upm,
                         dem_g).astype(np.float32)

    # ---- write the interior of every grid into the global files ----------
    ii0, ii1, jj0, jj1 = interior
    sl = (slice(jj0 - gj0, jj1 - gj0), slice(ii0 - gi0, ii1 - gi0))
    gs = (slice(jj0, jj1), slice(ii0, ii1))
    ctx.dem[gs] = dem_g[sl]
    ctx.cover.mm[gs] = cover_r.data[sl]
    ctx.water.mm[gs] = water_r.data[sl]
    if ctx.canopy is not None and canopy_arr is not None:
        ctx.canopy.mm[gs] = canopy_arr[sl]

    # ---- vectors: keep only what this block owns -------------------------
    owned, n_orph = split_owned(vec, lay, blk)
    me = (blk["bx"], blk["bz"])
    home, away = split_home_orphans(owned, lay, me)
    storey = gf.STOREY_HEIGHT_M
    nt = write_tile_docs(ctx.tiles_dir, home, None, storey)
    nt += write_tile_docs(ctx.tiles_dir, away, tag, storey)
    counts = {}
    for t, layers in owned.items():
        for fname, arrs in layers.items():
            for key, lst in arrs.items():
                counts[key] = counts.get(key, 0) + len(lst)
    kept_b = [b for t, ls in owned.items()
              for b in ls.get("BUILDINGS.JSON", {}).get("buildings", [])]
    summary = {
        "block": [blk["bx"], blk["bz"]], "bbox_ll": list(bbox_ll),
        "counts": counts, "orphans": n_orph, "tiles_written": nt,
        "seconds": round(time.time() - t0, 1),
        "buildings_landmark": sum(1 for b in kept_b if b.get("landmark")),
        "buildings_overture": sum(1 for b in kept_b
                                  if str(b.get("footprint_src", "")).startswith("overture")),
        "height_src": bh, "painted": painted, "dem_zoom": z,
        "dem_min_m": float(np.nanmin(dem_g)), "dem_max_m": float(np.nanmax(dem_g)),
        "wc": wc_prov, "canopy": cn_prov,
        "conflation": ({k: stats_x.get(k) for k in
                        ("footprints", "overture_rows", "overture_added",
                         "overture_same_osm_way", "overture_dup_by_overlap")}
                       if stats_x else None),
        "osm_counts": {k: v for k, v in vec["counts"].items()
                       if isinstance(v, int)},
    }
    os.makedirs(os.path.dirname(done), exist_ok=True)
    with open(done, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1, default=str)
    print("  block %s: %s  %.0f s" % (tag, counts, summary["seconds"]))
    return summary


# ================================================================= driver ===

def prepare(spec: dict, work: str, budget: Budget):
    """Boundary -> frame -> blocks -> layout. Returns (ctx_parts dict)."""
    q = boundary_query(spec)
    raw = cached_or_would(budget, gf.OVERPASS_ENDPOINTS[0], work, ".json",
                          q.encode("utf-8"), "overpass-boundary\x00" + q)
    if raw is None:
        raise RuntimeError("the boundary relation is not cached and we are "
                           "offline; run once with --online (one small request)")
    bd = parse_boundary(raw)
    big = max(_ring_area_m2(r) for r in bd["rings"])
    keep = [r for r in bd["rings"]
            if _ring_area_m2(r) >= float(spec.get("min_ring_frac", 0.02)) * big]
    dropped = [{"points": len(r), "km2": round(_ring_area_m2(r) / 1e6, 2),
                "at": [round(r[0][0], 4), round(r[0][1], 4)]}
               for r in bd["rings"] if r not in keep]
    lats = [p[0] for r in keep for p in r]
    lons = [p[1] for r in keep for p in r]
    pad = 0.002
    bbox_ll = (min(lats) - pad, min(lons) - pad, max(lats) + pad, max(lons) + pad)
    frame = Frame(bbox_ll, float(spec.get("cell", TG_WORLD_CELL)),
                  float(spec.get("units_per_metre", UNITS_PER_METRE_DERIVED)))
    tile_cells = int(round(spec["tile_m"] * frame.upm / frame.cell))
    block_tiles = max(1, int(round(spec["block_m"] / spec["tile_m"])))
    blocks, poly, side = plan_blocks(frame, keep, tile_cells, block_tiles)
    lay = Layout(frame, tile_cells, block_tiles, blocks,
                 spec["halo_m"] * frame.upm)
    return {"bd": bd, "rings": keep, "dropped_rings": dropped, "bbox_ll": bbox_ll,
            "frame": frame, "blocks": blocks, "poly": poly, "lay": lay}


def estimate(prep: dict) -> dict:
    fr, lay = prep["frame"], prep["lay"]
    cells = fr.w * fr.h
    return {
        "relation_id": prep["bd"]["id"],
        "relation_name": prep["bd"]["tags"].get("name"),
        "admin_level": prep["bd"]["tags"].get("admin_level"),
        "area_km2": round(prep["bd"]["area_m2"] / 1e6, 1),
        "dropped_exclave_rings": prep["dropped_rings"],
        "bbox_ll_S_W_N_E": [round(v, 5) for v in prep["bbox_ll"]],
        "grid_cells": [fr.w, fr.h], "cells": cells,
        "grid_km": [round(fr.w * fr.cell / fr.upm / 1000, 1),
                    round(fr.h * fr.cell / fr.upm / 1000, 1)],
        "raster_MB": {"HEIGHT.R16": round(cells * 2 / 1e6),
                      "COVER.R8": round(cells / 1e6),
                      "WATER.R8": round(cells / 1e6),
                      "CANOPY.R8": round(cells / 1e6)},
        "tile_grid": [lay.nx, lay.nz], "blocks": len(prep["blocks"]),
        "world_extent_units_max_abs": max(abs(fr.x0), abs(fr.z0),
                                          abs(fr.x0 + fr.w * fr.cell),
                                          abs(fr.z0 + fr.h * fr.cell)),
    }


def open_outputs(ctx: Ctx):
    fr = ctx.frame
    rot = fr.proj.describe()["rotation_rad"]
    os.makedirs(ctx.region_dir, exist_ok=True)
    dem_path = os.path.join(ctx.region_dir, "dem_m.f32")
    fresh = not os.path.exists(dem_path)
    ctx.dem = np.memmap(dem_path, dtype=np.float32, mode="w+" if fresh else "r+",
                        shape=(fr.h, fr.w))
    if fresh:
        for j in range(0, fr.h, 512):
            ctx.dem[j:j + 512, :] = np.nan
    mk = lambda name, kind, nd: RasterFile(os.path.join(ctx.work, name), kind,
                                           fr.w, fr.h, fr, nd, rotation=rot)
    ctx.cover = mk("COVER.R8", 2, 255)
    ctx.water = mk("WATER.R8", 2, 255)
    ctx.canopy = mk("CANOPY.R8", 2, 255) if ctx.opt.get("canopy", True) else None
    ctx.height = mk("HEIGHT.R16", 1, -32768)


def finish_height(ctx: Ctx) -> dict:
    """Quantise the float DEM into HEIGHT.R16 with ONE scale/bias."""
    fr = ctx.frame
    lo, hi = np.inf, -np.inf
    for j in range(0, fr.h, 512):
        s = np.asarray(ctx.dem[j:j + 512])
        if np.isfinite(s).any():
            lo = min(lo, float(np.nanmin(s)))
            hi = max(hi, float(np.nanmax(s)))
    bias_u = lo * fr.upm
    span_u = max(1.0, (hi - lo) * fr.upm)
    scale = span_u / 32000.0
    for j in range(0, fr.h, 512):
        s = np.asarray(ctx.dem[j:j + 512]).astype(np.float64)
        raw = np.clip(np.rint((s * fr.upm - bias_u) / scale), -32000, 32000)
        raw = np.where(np.isfinite(s), raw, -32768).astype(np.int16)
        ctx.height.mm[j:j + 512, :] = raw
    ctx.height.scale, ctx.height.bias = scale, bias_u
    return {"min_m": lo, "max_m": hi, "relief_m": hi - lo,
            "quantum_units": scale, "quantum_m": scale / fr.upm}


def write_place(ctx: Ctx, prep: dict, sums: list, hprov: dict) -> dict:
    fr, spec = ctx.frame, ctx.spec

    def add(key):
        t = {}
        for s in sums:
            for k, v in (s.get(key) or {}).items():
                if isinstance(v, (int, float)):
                    t[k] = t.get(k, 0) + v
        return t

    counts = add("counts")
    kept = add("height_src")
    painted = add("painted")
    xb_prov = {}
    try:
        import geo_buildings_extra as xb
        for s in sums:
            bb = tuple(s["bbox_ll"])
            xb_prov = xb.products(ctx.work, bb)["prov"]
            if xb_prov:
                break
    except Exception:                                   # noqa: BLE001
        pass
    rings = [simplify_ring(r, BOUNDARY_SIMPLIFY_M) for r in prep["rings"]]
    lat0, lon0 = fr.proj.lat0, fr.proj.lon0
    s, w, n, e = prep["bbox_ll"]
    wc_tiles = sorted({t for s_ in sums for t in ((s_.get("wc") or {}).get("tiles") or [])})
    cn_keys = sorted({t for s_ in sums for t in ((s_.get("canopy") or {}).get("quadkeys") or [])})
    place = {
        "name": spec["name"], "slug": spec["slug"],
        "centre": {"lat": lat0, "lon": lon0},
        "radius_m": 0.5 * max(fr.w, fr.h) * fr.cell / fr.upm,
        "bbox": {"south": s, "west": w, "north": n, "east": e},
        "projection": fr.proj.describe(),
        "rotation_rad": 0.0, "offset_x": 0.0, "offset_z": 0.0,
        "cell_units": fr.cell, "cell_m": fr.cell / fr.upm,
        "fetched_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "tiled": {"version": TILED_VERSION, "index": "tiles/INDEX.JSON",
                  "tile_size_units": ctx.lay.tile_side,
                  "block_tiles": ctx.lay.block_tiles, "blocks": len(ctx.lay.blocks),
                  "grid": [fr.w, fr.h], "origin": [fr.x0, fr.z0],
                  "layers": [k for k in TILED_LAYERS]},
        "boundary": {"relation_id": prep["bd"]["id"],
                     "name": prep["bd"]["tags"].get("name"),
                     "admin_level": prep["bd"]["tags"].get("admin_level"),
                     "area_km2": round(prep["bd"]["area_m2"] / 1e6, 1),
                     "simplify_m": BOUNDARY_SIMPLIFY_M,
                     "rings": [[[round(a, 6), round(b, 6)] for a, b in r]
                               for r in rings],
                     "dropped_exclave_rings": prep["dropped_rings"]},
        "layers": {
            "osm": {"source": "OpenStreetMap via Overpass, one query per block",
                    "licence": "ODbL 1.0 -- attribution required on screen",
                    "vintage": "live at fetch time",
                    "counts": counts},
            "buildings_extra": ({
                "source": "Overture Maps buildings theme + Google Open Buildings "
                          "2.5D Temporal building_height",
                "overture_release": (xb_prov.get("overture") or {}).get("release"),
                "open_buildings_year": (xb_prov.get("open_buildings") or {}).get("year"),
                "licence": "ODbL 1.0",
                "citation": (xb_prov.get("open_buildings") or {}).get("citation"),
                "height_src": kept,
            } if ctx.opt.get("extra_buildings", True) else {"source": "not wired"}),
            "height": {"source": "terrarium", "zoom": sums[0].get("dem_zoom") if sums else None,
                       "lowpass_m": ctx.smooth_m, "native_m": gf.TERRARIUM_NATIVE_M,
                       "sea_floor_clamp_m": gf.SEA_FLOOR_CLAMP_M, **hprov},
            "cover": {"source": "ESA WorldCover 10 m v200 + OSM landuse/leisure overlay",
                      "vintage": "2021 (WorldCover), live OSM overlay",
                      "licence": "CC-BY 4.0 (ESA WorldCover), ODbL (OSM)",
                      "vocabulary": "ESA WorldCover class ids",
                      "worldcover": {"tiles": wc_tiles}, "painted": painted},
            "canopy": ({"source": "Meta/WRI Global Canopy Height 1 m (alsgedi v6)",
                        "vintage": "2018-2020 imagery", "licence": "CC-BY 4.0",
                        "detail": {"quadkeys": cn_keys}}
                       if ctx.opt.get("canopy", True) else {"source": "not wired"}),
        },
        "sources": {"land_cover": ctx.opt.get("land_cover", "worldcover"),
                    "canopy": bool(ctx.opt.get("canopy", True)), "tag_schema": gf.TAG_SCHEMA,
                    "levels_estimator": ctx.levels_estimator,
                    "extra_buildings": bool(ctx.opt.get("extra_buildings", True))},
        "tag_schema": gf.TAG_SCHEMA,
        "attribution": [
            "Map data (c) OpenStreetMap contributors, ODbL 1.0",
            "Elevation: terrarium",
            "Land cover: ESA WorldCover 2021, CC-BY 4.0",
            "Tree canopy: Meta and WRI Global Canopy Height, CC-BY 4.0",
            "Buildings: Overture Maps Foundation, release %s, ODbL 1.0"
            % (xb_prov.get("overture") or {}).get("release"),
            "Building heights: Google Open Buildings 2.5D Temporal %s, ODbL 1.0. %s. "
            "Contains modified Copernicus Sentinel-2 data"
            % ((xb_prov.get("open_buildings") or {}).get("year"),
               (xb_prov.get("open_buildings") or {}).get("citation")),
        ],
    }
    write_json(os.path.join(ctx.work, "PLACE.JSON"), place)
    return place


def run(spec: dict, out_root: str, online: bool, only: list | None,
        limit: int | None, finalize: bool = True) -> int:
    work = os.path.join(out_root, spec["slug"])
    os.makedirs(work, exist_ok=True)
    budget = Budget(online)
    prep = prepare(spec, work, budget)
    print(json.dumps(estimate(prep), indent=1))
    ctx = Ctx(spec, prep["frame"], prep["lay"], budget, work, online)
    blocks = prep["blocks"]
    if only:
        want = {tuple(int(v) for v in o.split(":")) for o in only}
        blocks = [b for b in blocks if (b["bx"], b["bz"]) in want]
    if limit:
        blocks = blocks[:limit]
    open_outputs(ctx)
    sums = []
    for b in blocks:
        s = process_block(ctx, b)
        if s is None:
            print("  block b%d_%d: NOT AVAILABLE OFFLINE" % (b["bx"], b["bz"]))
            return 2
        sums.append(s)
    if not finalize or len(blocks) != len(prep["blocks"]):
        print("partial run (%d of %d blocks): no finalize" %
              (len(blocks), len(prep["blocks"])))
        return 0
    hprov = finish_height(ctx)
    for rf in (ctx.height, ctx.cover, ctx.water, ctx.canopy):
        if rf is not None:
            rf.finish()
    n_merged = merge_orphans(ctx.tiles_dir)
    idx = build_index(ctx.tiles_dir, ctx.lay)
    place = write_place(ctx, prep, sums, hprov)
    print("orphan tiles merged: %d; tile index layers: %s" % (
        n_merged, {k: len(v["tiles"]) for k, v in idx["layers"].items()}))
    print("wrote %s" % work)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--spec", required=True)
    ap.add_argument("--root", default=CACHE_ROOT)
    ap.add_argument("--online", action="store_true",
                    help="allow network requests that are not in the cache")
    ap.add_argument("--only", action="append",
                    help="bx:bz -- process just these blocks (repeatable)")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--dry-run", action="store_true",
                    help="prove the fetch budget offline (urlopen patched)")
    ap.add_argument("--plan", action="store_true",
                    help="print the region/blocks/estimates and exit")
    a = ap.parse_args(argv)
    spec = load_spec(a.spec)
    if a.dry_run:
        rep = dry_run(spec, a.root)
        print(json.dumps(rep, indent=1, default=str))
        return 0
    if a.plan:
        work = os.path.join(a.root, spec["slug"])
        prep = prepare(spec, work, Budget(a.online))
        print(json.dumps(estimate(prep), indent=1))
        return 0
    return run(spec, a.root, a.online, a.only, a.limit)



# ============================================================== dry run =====

class NetBlocked(RuntimeError):
    """Raised by the patched urlopen: a request the dry run would have made."""


def dry_run(spec: dict, out_root: str, plan_metadata_online: bool = False) -> dict:
    """The fetch budget, proved OFFLINE (see memory
    reference_geo_fetch_budget_dryrun_and_stale_baseline): urllib's urlopen is
    replaced by a function that records the request and raises, every cache the
    real run would read is consulted, and the answer is the list of requests
    that WOULD go out plus the bytes that can be known without them.

    What is exact: the Overpass block queries, the Terrarium tiles, the Overture
    row-group bytes and the Open Buildings tile bytes (when the footers and
    manifests are cached). What is not: Overpass answer sizes (estimated from
    La Plata's measured density) and any COG whose header is not cached (named)."""
    import urllib.request
    work = os.path.join(out_root, spec["slug"])
    wouldnet: list[str] = []

    def blocked(req, *a, **k):
        url = getattr(req, "full_url", req)
        rng = ""
        try:
            rng = " " + (req.headers.get("Range") or "")
        except Exception:                               # noqa: BLE001
            pass
        wouldnet.append(str(url) + rng)
        raise NetBlocked("dry run: WOULD FETCH " + str(url))

    real = urllib.request.urlopen
    urllib.request.urlopen = blocked
    try:
        budget = Budget(False)
        prep = prepare(spec, work, budget)
        ctx = Ctx(spec, prep["frame"], prep["lay"], budget, work, False)
        fr = ctx.frame
        report = {"estimate": estimate(prep)}
        ov_miss = tile_miss = 0
        tiles_all: set = set()
        for blk in prep["blocks"]:
            halo = ctx.lay.halo
            bbox_ll = world_box_to_ll(fr, blk["x0"] - halo, blk["z0"] - halo,
                                      blk["x1"] + halo, blk["z1"] + halo)
            n0 = len(budget.would)
            overpass_for_box(ctx, bbox_ll)
            ov_miss += len(budget.would) - n0 > 0
            interior, G, D = block_ranges(ctx, blk)
            lat, lon = latlon_grid(fr, D[0][0], D[0][1], D[1][0], D[1][1])
            z = gf._terrarium_zoom(float(lat.mean()), fr.cell / fr.upm)
            tx0, tx1, ty0, ty1 = terrarium_tiles_for(lat, lon, z)
            for tx in range(tx0, tx1 + 1):
                for ty in range(ty0, ty1 + 1):
                    tiles_all.add((z, tx, ty))
        for (z, tx, ty) in sorted(tiles_all):
            url = gf.TERRARIUM_URL % (z, tx, ty)
            p = gf._cache_path(work, url + "\x00", ".png")
            if not (os.path.exists(p) and os.path.getsize(p) > 0):
                tile_miss += 1
        over = [w for w in budget.would if "overpass" in w]
        report["overpass"] = {"block_queries": len(prep["blocks"]),
                              "uncached": len(over),
                              "serial_sleep_s": RATE_SLEEP_S}
        report["terrarium"] = {"tiles_needed": len(tiles_all),
                               "uncached": tile_miss, "zoom": z}
        report.update(plan_cogs_and_extra(ctx, prep))
        report["net_attempts_recorded"] = wouldnet
    finally:
        urllib.request.urlopen = real
    return report


def plan_cogs_and_extra(ctx: Ctx, prep: dict) -> dict:
    """WorldCover + canopy COG bytes and the Overture / Open Buildings plans,
    per block, as a UNION (a row group two blocks share is read once)."""
    import geo_cog
    import geo_buildings_extra as xb
    fr = ctx.frame
    cache = os.path.join(ctx.work, "_cache")
    out: dict = {"cog": {}, "extra": {}}
    cogs: dict = {}
    need: dict = {}
    miss_hdr: set = set()

    def cog_for(url):
        if url in cogs:
            return cogs[url]
        try:
            cogs[url] = geo_cog.RemoteCOG(url, cache)
        except NetBlocked as exc:
            miss_hdr.add(url)
            cogs[url] = None
        except Exception as exc:                          # noqa: BLE001
            miss_hdr.add(url + " (%s)" % type(exc).__name__)
            cogs[url] = None
        return cogs[url]

    for blk in prep["blocks"]:
        _, G, _ = block_ranges(ctx, blk)
        lat, lon = latlon_grid(fr, G[0][0], G[0][1], G[1][0], G[1][1])
        corners = [(float(lat.min()), float(lon.min())), (float(lat.max()), float(lon.max())),
                   (float(lat.min()), float(lon.max())), (float(lat.max()), float(lon.min()))]
        wanted = {}
        if ctx.opt.get("land_cover", "worldcover") == "worldcover":
            for la, lo in corners:
                wanted.setdefault(gf.WORLDCOVER_URL % gf._worldcover_tile(la, lo), []).append((la, lo, "wc"))
        if ctx.opt.get("canopy", True):
            for la, lo in corners:
                wanted.setdefault(gf.CANOPY_URL % gf._quadkey(la, lo, gf.CANOPY_ZOOM), []).append((la, lo, "cn"))
        # a block can straddle two canopy quadkeys: treat the whole block box
        # as needed in every quadkey a corner falls in (a safe over-estimate).
        for url, pts in wanted.items():
            cg = cog_for(url)
            if cg is None:
                continue
            kind = pts[0][2]
            las = [p[0] for p in pts] + [float(lat.min()), float(lat.max())]
            los = [p[1] for p in pts] + [float(lon.min()), float(lon.max())]
            if kind == "wc":
                xs, ys = min(los), max(las)
                x1, y1 = max(los), min(las)
                px0, py0 = cg.xy_to_pixel(xs, ys)
                px1, py1 = cg.xy_to_pixel(x1, y1)
            else:
                R = 6378137.0
                mx = [math.radians(v) * R for v in (min(los), max(los))]
                my = [math.log(math.tan(math.pi / 4 + math.radians(v) / 2)) * R
                      for v in (min(las), max(las))]
                px0, py0 = cg.xy_to_pixel(mx[0], my[1])
                px1, py1 = cg.xy_to_pixel(mx[1], my[0])
            px0, px1 = int(max(0, math.floor(px0) - 3)), int(min(cg.width, math.ceil(px1) + 3))
            py0, py1 = int(max(0, math.floor(py0) - 3)), int(min(cg.height, math.ceil(py1) + 3))
            if px1 <= px0 or py1 <= py0:
                continue
            tx_n = (cg.width + cg.tw - 1) // cg.tw
            cnts = cg.page.databytecounts
            s = need.setdefault(url, set())
            for ty in range(py0 // cg.th, (py1 - 1) // cg.th + 1):
                for tx in range(px0 // cg.tw, (px1 - 1) // cg.tw + 1):
                    i = ty * tx_n + tx
                    if cnts[i]:
                        s.add(i)
    for url, idxs in need.items():
        cg = cogs[url]
        out["cog"][url.rsplit("/", 1)[-1]] = {
            "tiles_or_strips": len(idxs),
            "bytes": int(sum(cg.page.databytecounts[i] for i in idxs)),
            "px": [cg.width, cg.height],
            "compression": str(cg.page.compression)}
    out["cog_header_not_cached"] = sorted(miss_hdr)
    out["cog_bytes_total"] = sum(v["bytes"] for v in out["cog"].values())

    # ---- Overture + Open Buildings: per-block plans, unioned -------------
    ov_spans: dict = {}
    ob_spans: dict = {}
    nplan = nblocked = 0
    first_block = None
    for blk in prep["blocks"]:
        halo = ctx.lay.halo
        bb = world_box_to_ll(fr, blk["x0"] - halo, blk["z0"] - halo,
                             blk["x1"] + halo, blk["z1"] + halo)
        try:
            xb.fetch_extra(ctx.work, bb, False, ctx.spec.get("overture_release"),
                           int(ctx.spec.get("extra_max_mb", 900) * 1024 * 1024), True)
        except Exception as exc:                          # noqa: BLE001
            nblocked += 1
            first_block = first_block or "%s: %s" % (type(exc).__name__, str(exc)[:300])
            continue
        pf = os.path.join(cache, "xb_plan_%s.json" % xb.bbox_key(bb))
        if not os.path.exists(pf):
            continue
        nplan += 1
        with open(pf, encoding="utf-8") as f:
            pl = json.load(f)
        for fl in pl["overture"]["files"]:
            ov_spans.setdefault(fl["url"], set()).update(map(tuple, fl["spans"]))
        for t in pl["open_buildings"]["tiffs"]:
            ob_spans.setdefault(t["url"], set()).update(map(tuple, t["spans"]))
    out["extra"] = {
        "blocks_planned": nplan, "blocks_blocked_offline": nblocked,
        "first_block_error": first_block,
        "overture_files": len(ov_spans),
        "overture_bytes": int(sum(b - a for sp in ov_spans.values() for a, b in sp)),
        "open_buildings_tiffs": len(ob_spans),
        "open_buildings_bytes": int(sum(b - a for sp in ob_spans.values() for a, b in sp)),
    }
    return out


if __name__ == "__main__":
    sys.exit(main())
