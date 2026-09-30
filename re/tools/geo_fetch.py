"""geo_fetch.py -- pull a real place off the internet into the GEO TRACK cache.

Writes the on-disk contract of docs/plans/GEO_TRACK_OSM_PLAN.md section 4 into
re/assets/geo/<slug>/. Everything network-facing lives here; the C side only ever
reads local files and knows nothing about HTTP, Overpass, tiles or projections.

SOURCES, and why each one (all free, all credit-compatible):

  OpenStreetMap via Overpass    roads, water, buildings, plazas, signals, landuse
                               ODbL, credit required. Rate-limited and sometimes
                               down, so every response is cached on disk and a
                               re-run costs nothing.

  Terrarium PNG terrain tiles   the DEFAULT elevation path. Plain PNGs, decoded
                               with PIL, elevation = (R*256 + G + B/256) - 32768
                               metres. Global, no GDAL, no API key. Verified over
                               La Plata: -2..61 m, median 16 m, 15.7 m/px at z13.

  DEM override (optional)       a GeoTIFF or PNG the user drops into
                               re/assets/geo/_dem_override/. This is where a
                               higher-precision national DEM goes.

WHY NOT THE IGN .img DIRECTLY. Argentina's IGN publishes a 5 m aerophotogrammetric
MDE with sub-metre vertical accuracy, and its WFS coverage layer confirms it
covers La Plata and Buenos Aires (project "0008 - 2013 - AMBA - Sector 1.1").
Two things stop it being the automatic path, both verified 2026-09-29:

  1. The 5 m tiles are REQUEST-GATED. pid=3 302-redirects to FAQ #30
     ("todas las solicitudes deberán ser remitidas a Asesoría Técnica del IGN")
     with or without a browser referer and user agent. The 30 m product (pid=7)
     does download freely.
  2. Both products ship as ERDAS IMAGINE HFA (the file opens with
     "EHFA_HEADER_TAG"), which needs GDAL. This environment has tifffile, numpy,
     PIL and shapely but no GDAL, rasterio or pyproj.

So `--probe-dem` reports whether 5 m exists for a bbox, and the override slot is
how it gets used: request the sheet once, convert it to GeoTIFF in QGIS (the
IGN's own FAQ lists QGIS as supported), drop it in. One request covers AMBA
permanently, after which La Plata and Buenos Aires are 5 m and offline.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo_common import (  # noqa: E402
    CACHE_ROOT,
    DEM_OVERRIDE_DIR,
    LocalProjection,
    TD5_TG_LANE_WIDTH,
    TG_WORLD_CELL,
    UNITS_PER_METRE_DERIVED,
    place_dir,
    slugify,
    write_json,
)
from geo_raster import KIND_U8, Raster  # noqa: E402

UA = "TD5RE-geo-track/0.1 (+https://github.com/; source port research tool)"

OVERPASS_ENDPOINTS = (
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
)
TERRARIUM_URL = ("https://s3.amazonaws.com/elevation-tiles-prod/terrarium"
                 "/%d/%d/%d.png")
# Ground sample distance of the DATA BEHIND the tiles, which is not the tile
# grid's own resolution. Terrarium is SRTM-derived, so ~30 m, and z13 tiles at
# 15.7 m/px are already oversampled relative to that.
TERRARIUM_NATIVE_M = 30.0

# Lowpass width applied to a fetched DEM, metres. NOT the native GSD: measured on
# La Plata (a flat city) with Terrarium, which is SRTM-derived and therefore a
# SURFACE model that reads rooftops as ground.
#
#   lowpass   relief kept   p99 grade x1.5   max grade
#   30 m         67.6 m         0.297          0.993     <- pinned at the cap
#   60 m         57.4 m         0.208          0.543
#   120 m        44.2 m         0.127          0.273
#   200 m        33.1 m         0.076          0.153     <- default
#   500 m        24.6 m         0.037          0.047
#
# 200 m is the first width where the exaggerated p99 clears the default
# TD5RE_AUTOTRACK_GRADE cap (0.12) AND the worst cell clears the absolute ceiling
# TG_ROAD_GRADE_ABSMAX (0.20), so the road profile stops sitting pinned. Filtering
# at the native 30 m does NOT fix it: SRTM's rooftop artefacts are whole CITY
# BLOCKS, 30-80 m of plateau, so a 31 m window barely touches them.
#
# Real relief survives because it lives at a much larger scale -- a mountain pass
# turns over 500 m to 2 km. Override with --dem-smooth-m, and use 0 to disable.
#
# THE REAL FIX is a bare-earth model. The IGN publishes MDT (Modelo Digital de
# TERRENO) as well as MDE (surface) at 5 m, which is exactly the right product and
# needs no smoothing at all -- one more reason to request the AMBA sheet.
DEM_SMOOTH_M_DEFAULT = 200.0
IGN_MDE_WFS = "https://wms.ign.gob.ar/geoserver/ows"

# ESA WorldCover class ids, kept as the COVER.R8 vocabulary even when the values
# are derived from OSM landuse instead, so the C side has one enum either way.
# https://collections.sentinel-hub.com/worldcover/readme.html
COVER_NONE = 0
COVER_TREE = 10
COVER_SHRUB = 20
COVER_GRASS = 30
COVER_CROP = 40
COVER_BUILT = 50
COVER_BARE = 60
COVER_SNOW = 70
COVER_WATER = 80
COVER_WETLAND = 90

# Lanes per highway class when the way carries no `lanes` tag. The generator
# clamps its own base lane count to 2..4 (see td5_trackgen.h's apply_config
# note), and a 1-lane racing line is not drivable, so the floor here is 2.
HIGHWAY_LANES = {
    "motorway": 4, "motorway_link": 2,
    "trunk": 3, "trunk_link": 2,
    "primary": 3, "primary_link": 2,
    "secondary": 2, "secondary_link": 2,
    "tertiary": 2, "tertiary_link": 2,
    "unclassified": 2, "residential": 2, "living_street": 2,
    "service": 2, "road": 2,
}
# Ways we never route on but still want as scenery context.
HIGHWAY_NONDRIVABLE = {
    "footway", "path", "cycleway", "pedestrian", "steps", "track",
    "bridleway", "corridor", "platform", "construction", "proposed",
}

# Storey height for the building:levels fallback. 3.0 m is the usual planning
# figure for mixed residential/commercial and is an OPEN question in the plan
# (section 10 item 3): per-country or per-landuse would be better.
STOREY_HEIGHT_M = 3.0


# ------------------------------------------------------------------- cache ---

def _cache_path(root: str, key: str, ext: str) -> str:
    h = hashlib.sha256(key.encode("utf-8")).hexdigest()[:20]
    return os.path.join(root, "_cache", "%s%s" % (h, ext))


def _cached_get(url: str, root: str, ext: str = ".bin", data: bytes | None = None,
                key: str | None = None, timeout: int = 180,
                retries: int = 3) -> bytes:
    """GET/POST with an on-disk cache keyed by url+body.

    Overpass is rate-limited and intermittently down, and a place is fetched many
    times while iterating on a route, so caching is not an optimisation here --
    it is what makes the tool usable at all.
    """
    ck = key or (url + "\x00" + (data.decode("utf-8", "replace") if data else ""))
    path = _cache_path(root, ck, ext)
    if os.path.exists(path) and os.path.getsize(path) > 0:
        with open(path, "rb") as f:
            return f.read()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    last: Exception | None = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, data=data,
                                         headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                body = r.read()
            with open(path, "wb") as f:
                f.write(body)
            return body
        except (urllib.error.URLError, urllib.error.HTTPError, OSError) as exc:
            last = exc
            if attempt < retries - 1:
                # Overpass answers 429/504 under load; backing off is the
                # documented way to be a good citizen of a free service.
                time.sleep(4.0 * (attempt + 1))
    raise RuntimeError("fetch failed after %d tries: %s (%s)"
                       % (retries, url, last))


# ------------------------------------------------------- bbox / way caps ---
#
# Plan section 9: "OSM data quality varies wildly. Rural coverage is sparse; a
# dense centre holds ten thousand ways in a 2 km box. Needs the bbox cap, a
# way-count cap, and a graceful 'not enough road here' path in the selector
# rather than a failed build." Section 0 sizes the bbox: "a 6.3 km route wants
# roughly a 4 x 4 km box with margin; hard ceiling 3000 spans / ~10.5 km".
#
# BBOX. fetch_place makes a square of side 2*radius. 4000 m of radius is an
# 8 x 8 km box (64 km2), which holds the 10.5 km hard-ceiling route with room to
# drag waypoints, and it is the largest option the page offers. The old code
# clamped silently at 6000 m -- a 144 km2 box, four times what the plan sized,
# arrived at with no message. Refused with a sentence now instead.
FETCH_MIN_RADIUS_M = 500.0
FETCH_MAX_RADIUS_M = 4000.0
#
# WAY COUNT. MEASURED on the La Plata cache, which is the densest real fixture
# this ships with: 2291 drivable ways inside a 2713 m radius = 23.1 km2, i.e.
# 99 ways/km2, for a planned city centre on a 110 m block grid. The cap is set
# at 6000 -- 2.6x La Plata's count, and still under half of the "ten thousand
# ways in a 2 km box" the plan names as the case to survive. A denser centre at
# the 4 km radius ceiling (50 km2) hits it at roughly 120 ways/km2, a fifth more
# than La Plata.
#
# OVER THE CAP THE FETCH IS CLIPPED, NOT REFUSED. Dropping the lowest road
# classes first is the right trade: service alleys and living streets are
# already the most expensive classes in geo_route's CLASS_COST (2.60 / 2.20), so
# a racing line was never going to use them, while the arterials that make a
# route recognisable are the cheapest and are kept to the last. The place
# records what it lost (PLACE.JSON layers.osm.counts.clipped_from) and the
# selector says so on screen, so a thin side-street network is attributable.
FETCH_MAX_DRIVABLE_WAYS = 6000
# Dropped in this order, least useful for a race first.
FETCH_CLIP_ORDER = (
    "service", "living_street", "track", "road", "unclassified",
    "residential", "tertiary_link", "tertiary", "secondary_link",
)


def area_cap_reasons(radius_m: float) -> list[str]:
    """Why this radius cannot be fetched, as sentences for the page. Empty = go.
    Pure, so the selector and the self-test can both check it with no network."""
    out = []
    if radius_m < FETCH_MIN_RADIUS_M:
        out.append("radius %.0f m is below the %.0f m floor: a smaller circle "
                   "rarely holds a whole route."
                   % (radius_m, FETCH_MIN_RADIUS_M))
    if radius_m > FETCH_MAX_RADIUS_M:
        out.append("radius %.0f m is over the %.0f m cap (a %.0f x %.0f km box, "
                   "%.0f km2). The hard span ceiling is a 10.5 km route, so a "
                   "bigger area only slows the fetch down."
                   % (radius_m, FETCH_MAX_RADIUS_M,
                      radius_m / 500.0, radius_m / 500.0,
                      (2.0 * radius_m / 1000.0) ** 2))
    return out


def clip_roads(roads: list[dict], cap: int = FETCH_MAX_DRIVABLE_WAYS
               ) -> tuple[list[dict], dict]:
    """Drop the least useful road classes until the way count is under `cap`.

    Returns (kept, report). Under the cap this is the identity and the report
    is empty, so the common path costs one comparison."""
    if len(roads) <= cap:
        return roads, {}
    kept = list(roads)
    dropped: list[str] = []
    for cls in FETCH_CLIP_ORDER:
        if len(kept) <= cap:
            break
        n0 = len(kept)
        kept = [r for r in kept if r.get("class") != cls]
        if len(kept) != n0:
            dropped.append("%s (%d)" % (cls, n0 - len(kept)))
    return kept, {"clipped_from": len(roads), "cap": cap,
                  "dropped_classes": dropped}


# ---------------------------------------------------------------- overpass ---

OVERPASS_QL = """[out:json][timeout:180];
(
  way["highway"](%(bbox)s);
  node["highway"="traffic_signals"](%(bbox)s);
  way["natural"="water"](%(bbox)s);
  way["natural"="coastline"](%(bbox)s);
  way["waterway"](%(bbox)s);
  way["landuse"](%(bbox)s);
  way["leisure"](%(bbox)s);
  way["building"](%(bbox)s);
);
out body geom;
"""


def fetch_osm(bbox: tuple[float, float, float, float], root: str) -> dict:
    """One Overpass call for every layer, to stay inside the fair-use budget.

    `out body geom` inlines each way's node coordinates, so no second pass to
    resolve node ids is needed -- one request instead of two, and no node table
    to hold in memory.
    """
    south, west, north, east = bbox
    ql = OVERPASS_QL % {"bbox": "%.6f,%.6f,%.6f,%.6f" % (south, west, north, east)}
    last: Exception | None = None
    for ep in OVERPASS_ENDPOINTS:
        try:
            raw = _cached_get(ep, root, ".json", data=ql.encode("utf-8"),
                              key="overpass\x00" + ql)
            return json.loads(raw)
        except Exception as exc:          # noqa: BLE001 - try the next mirror
            last = exc
    raise RuntimeError("every Overpass endpoint failed: %s" % last)


# ------------------------------------------------------------------ IGN WFS ---

def probe_ign_dem(bbox: tuple[float, float, float, float]) -> dict:
    """Which IGN elevation products cover this bbox?

    The coverage layer is `ign:mde`, one feature per downloadable tile with
    `nombre`, `proyecto`, `archivo` and `link`. Reporting this is worth a request
    even though the 5 m tiles cannot be fetched: it lets the selector say "this
    place can be 5 m if you request the sheet" instead of silently serving a
    coarser model.
    """
    south, west, north, east = bbox
    q = urllib.parse.urlencode({
        "service": "WFS", "version": "1.0.0", "request": "GetFeature",
        "typeName": "ign:mde", "outputFormat": "application/json",
        "bbox": "%.6f,%.6f,%.6f,%.6f" % (west, south, east, north),
    })
    try:
        req = urllib.request.Request(IGN_MDE_WFS + "?" + q,
                                     headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=90) as r:
            fc = json.loads(r.read())
    except Exception as exc:              # noqa: BLE001
        return {"available": False, "error": "%s: %s" % (type(exc).__name__, exc)}

    by_project: dict[str, list[dict]] = {}
    for f in fc.get("features", []):
        p = f.get("properties", {})
        proj = (p.get("proyecto") or "?").strip()
        link = p.get("link") or ""
        m = re.search(r'href="([^"]+)"', link)
        by_project.setdefault(proj, []).append({
            "sheet": p.get("archivo"),
            "project": p.get("nombre"),
            "url": m.group(1) if m else None,
        })
    best = None
    for name in ("MDE 5m", "MDE 30 m"):
        if name in by_project:
            best = name
            break
    return {
        "available": bool(by_project),
        "projects": {k: len(v) for k, v in by_project.items()},
        "best": best,
        # Only the fetchable product's links are worth carrying forward.
        "tiles": by_project.get(best, [])[:64] if best else [],
        "note": ("MDE 5m tiles are request-gated (pid=3 redirects to FAQ #30) and "
                 "ship as ERDAS HFA .img, which needs GDAL. Request from "
                 "asesoriatecnica@ign.gob.ar, convert to GeoTIFF, drop into "
                 + DEM_OVERRIDE_DIR),
    }


# --------------------------------------------------------------------- DEM ---

def _terrarium_tile(z: int, x: int, y: int, root: str) -> np.ndarray:
    """One 256x256 Terrarium tile as metres above the ellipsoid."""
    from PIL import Image
    raw = _cached_get(TERRARIUM_URL % (z, x, y), root, ".png")
    im = Image.open(io.BytesIO(raw)).convert("RGB")
    a = np.asarray(im, dtype=np.float64)
    return (a[:, :, 0] * 256.0 + a[:, :, 1] + a[:, :, 2] / 256.0) - 32768.0


def _lonlat_to_tilef(lon: float, lat: float, z: int) -> tuple[float, float]:
    n = 2.0 ** z
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
    return x, y


def _terrarium_zoom(lat: float, target_m_per_px: float) -> int:
    """Smallest zoom whose ground resolution is at least as fine as the target.

    Terrarium's underlying data is ~30 m (SRTM heritage), so asking for a cell of
    3.49 m would only oversample. Capped at 14 to keep the tile count sane: the
    surface-detail octave the generator keeps (wavelength 9000 units, ~21 m) is
    what supplies sub-tile texture anyway, by design -- see the plan's section 2.
    """
    for z in range(8, 15):
        if 156543.03392 * math.cos(math.radians(lat)) / (2.0 ** z) <= target_m_per_px:
            return z
    return 14


def build_height_raster(proj: LocalProjection,
                        bbox: tuple[float, float, float, float],
                        root: str, cell: float = TG_WORLD_CELL,
                        dem_override: str | None = None,
                        smooth_m: float = DEM_SMOOTH_M_DEFAULT
                        ) -> tuple[Raster, dict]:
    """Sample elevation onto the world cell grid.

    The grid is axis-aligned in WORLD space (already rotated by the conditioner),
    so a cell maps back to lat/lon through the projection and forward into the
    tile pyramid. Bilinear within the tile mosaic; nearest at the mosaic edge.
    """
    south, west, north, east = bbox
    # World-space extent of the bbox corners, after the projection's rotation.
    corners = [proj.to_world(la, lo)
               for la in (south, north) for lo in (west, east)]
    xs = [c[0] for c in corners]
    zs = [c[1] for c in corners]
    x0, x1 = min(xs), max(xs)
    z0, z1 = min(zs), max(zs)
    w = max(2, int(math.ceil((x1 - x0) / cell)) + 1)
    h = max(2, int(math.ceil((z1 - z0) / cell)) + 1)

    if dem_override:
        src = _load_override(dem_override)
        provenance = {"source": "override", "file": os.path.basename(dem_override),
                      **src["meta"]}
        sampler = src["sample"]
    else:
        lat_mid = 0.5 * (south + north)
        z = _terrarium_zoom(lat_mid, cell / proj.units_per_metre)
        mosaic, mx0, my0 = _build_mosaic(bbox, z, root)
        provenance = {
            "source": "terrarium",
            "url": TERRARIUM_URL % (z, 0, 0),
            "zoom": z,
            "ground_res_m": 156543.03392 * math.cos(math.radians(lat_mid)) / (2.0 ** z),
            "native_m": TERRARIUM_NATIVE_M,
            "licence": "ODbL / public-domain mix, see AWS elevation-tiles-prod",
            "vintage": "SRTM-derived, see the dataset for per-region sources",
            "tiles": int(mosaic.shape[0] * mosaic.shape[1] // (256 * 256)),
        }

        def sampler(lat: float, lon: float) -> float:
            tx, ty = _lonlat_to_tilef(lon, lat, z)
            px = (tx - mx0) * 256.0
            py = (ty - my0) * 256.0
            return _bilinear(mosaic, px, py)

    # Sample. A 4.4 km box at 1500-unit (3.49 m) cells is ~1265 cells PER SIDE,
    # so 1.6M samples and a 3.2 MB HEIGHT.R16. That is fine, but it grows as the
    # square of the radius -- which is one of the reasons the plan caps the bbox.
    out = np.zeros((h, w), dtype=np.float64)
    for iz in range(h):
        wz = z0 + iz * cell
        for ix in range(w):
            wx = x0 + ix * cell
            la, lo = proj.world_to_latlon(wx, wz)
            out[iz, ix] = sampler(la, lo)

    # LOWPASS TO THE SOURCE'S NATIVE RESOLUTION.
    #
    # This is not cosmetic. Terrarium is SRTM-derived, and SRTM is a SURFACE
    # model: over a city it measures rooftops, not ground. Sampled straight onto
    # 3.49 m cells that puts building-height steps into the terrain -- measured on
    # La Plata, a flat city, as a 4.8 m step between adjacent cells (138% grade)
    # and a p99 grade of 22.7%, which made the road profile sit pinned at its
    # 0.20 absolute cap for the whole track.
    #
    # The fix is to stop pretending to have detail the source does not have. The
    # plan's design already says the generator's own surface-detail octave
    # (wavelength 9000 units, ~21 m) supplies the fine texture and the DEM only
    # the large scale; this is the missing half of that. A separable box blur at
    # the native GSD removes building-scale steps and keeps real relief.
    cell_m = cell / proj.units_per_metre
    rad = int(round(0.5 * smooth_m / cell_m)) if (cell_m > 0.0 and smooth_m > 0.0) else 0
    if rad >= 1:
        out = _box_blur(out, rad)
        provenance["lowpass_m"] = smooth_m
        provenance["lowpass_radius_cells"] = rad
    else:
        provenance["lowpass_m"] = None
        provenance["lowpass_radius_cells"] = 0

    # Store as int16 world units. A metre is ~430 units, so a raw int16 would
    # saturate at 76 m of relief; scale by the cell-appropriate step instead and
    # record it in the header.
    metres = out
    lo_m, hi_m = float(metres.min()), float(metres.max())
    bias_u = lo_m * proj.units_per_metre
    span_u = max(1.0, (hi_m - lo_m) * proj.units_per_metre)
    scale = span_u / 32000.0
    raw = np.clip(np.rint((metres * proj.units_per_metre - bias_u) / scale),
                  -32000, 32000).astype(np.int16)
    r = Raster(raw, origin_x=x0, origin_z=z0, cell=cell,
               scale=scale, bias=bias_u, nodata_raw=-32768,
               rotation_rad=proj.describe()["rotation_rad"])
    provenance.update({
        "min_m": lo_m, "max_m": hi_m, "relief_m": hi_m - lo_m,
        "quantum_units": scale, "quantum_m": scale / proj.units_per_metre,
    })
    return r, provenance


def _build_mosaic(bbox, z: int, root: str) -> tuple[np.ndarray, int, int]:
    south, west, north, east = bbox
    x0f, y0f = _lonlat_to_tilef(west, north, z)
    x1f, y1f = _lonlat_to_tilef(east, south, z)
    tx0, ty0 = int(math.floor(x0f)), int(math.floor(y0f))
    tx1, ty1 = int(math.floor(x1f)), int(math.floor(y1f))
    nx, ny = tx1 - tx0 + 1, ty1 - ty0 + 1
    if nx * ny > 64:
        raise RuntimeError("bbox needs %d terrain tiles at z=%d; shrink it "
                           "(the plan caps the box for this reason)" % (nx * ny, z))
    mosaic = np.zeros((ny * 256, nx * 256), dtype=np.float64)
    for j in range(ny):
        for i in range(nx):
            mosaic[j * 256:(j + 1) * 256, i * 256:(i + 1) * 256] = \
                _terrarium_tile(z, tx0 + i, ty0 + j, root)
    return mosaic, tx0, ty0


def _box_blur(a: np.ndarray, rad: int) -> np.ndarray:
    """Separable moving average of radius `rad` cells, edge-extended.

    Two 1-D cumulative-sum passes, so it is O(cells) rather than O(cells * k^2)
    and a 1786x1786 grid with a radius of 4 costs milliseconds.
    """
    k = 2 * rad + 1
    padx = np.pad(a, ((0, 0), (rad, rad)), mode="edge")
    cs = np.cumsum(padx, axis=1)
    cs = np.concatenate([np.zeros((cs.shape[0], 1)), cs], axis=1)
    tmp = (cs[:, k:] - cs[:, :-k]) / float(k)

    padz = np.pad(tmp, ((rad, rad), (0, 0)), mode="edge")
    cs = np.cumsum(padz, axis=0)
    cs = np.concatenate([np.zeros((1, cs.shape[1])), cs], axis=0)
    return (cs[k:, :] - cs[:-k, :]) / float(k)


def _bilinear(a: np.ndarray, px: float, py: float) -> float:
    h, w = a.shape
    px = min(max(px, 0.0), w - 1.0)
    py = min(max(py, 0.0), h - 1.0)
    ix, iy = int(px), int(py)
    jx, jy = min(ix + 1, w - 1), min(iy + 1, h - 1)
    fx, fy = px - ix, py - iy
    return float((a[iy, ix] * (1 - fx) + a[iy, jx] * fx) * (1 - fy)
                 + (a[jy, ix] * (1 - fx) + a[jy, jx] * fx) * fy)


def _load_override(path: str) -> dict:
    """Read a user-supplied DEM: GeoTIFF (via tifffile) or a Terrarium-coded PNG.

    GeoTIFF georeferencing is read from the two standard tags rather than through
    GDAL: 33550 ModelPixelScale and 33922 ModelTiepoint. That covers the
    north-up, unrotated case, which is what a DEM export is.
    """
    ext = os.path.splitext(path)[1].lower()
    if ext in (".tif", ".tiff"):
        import tifffile
        with tifffile.TiffFile(path) as tf:
            page = tf.pages[0]
            arr = page.asarray().astype(np.float64)
            tags = page.tags
            if 33550 not in tags or 33922 not in tags:
                raise RuntimeError("%s: no GeoTIFF ModelPixelScale/ModelTiepoint "
                                   "tags; re-export as a north-up GeoTIFF" % path)
            sx, sy = tags[33550].value[0], tags[33550].value[1]
            tie = tags[33922].value
            ox, oy = tie[3], tie[4]
        meta = {"format": "geotiff", "pixel_scale": [sx, sy],
                "tiepoint": [ox, oy], "shape": list(arr.shape)}

        def sample(lat: float, lon: float) -> float:
            px = (lon - ox) / sx
            py = (oy - lat) / sy
            return _bilinear(arr, px, py)
    elif ext == ".png":
        from PIL import Image
        a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
        arr = (a[:, :, 0] * 256.0 + a[:, :, 1] + a[:, :, 2] / 256.0) - 32768.0
        side = os.path.splitext(path)[0] + ".json"
        if not os.path.exists(side):
            raise RuntimeError("%s: a PNG override needs %s beside it with "
                               '{"west":..,"north":..,"deg_per_px_x":..,'
                               '"deg_per_px_y":..}' % (path, side))
        with open(side, encoding="utf-8") as f:
            sc = json.load(f)
        meta = {"format": "terrarium_png", **sc, "shape": list(arr.shape)}

        def sample(lat: float, lon: float) -> float:
            px = (lon - sc["west"]) / sc["deg_per_px_x"]
            py = (sc["north"] - lat) / sc["deg_per_px_y"]
            return _bilinear(arr, px, py)
    else:
        raise RuntimeError("%s: override must be .tif/.tiff or .png (an IGN .img "
                           "is ERDAS HFA and needs GDAL; convert it in QGIS "
                           "first)" % path)
    return {"sample": sample, "meta": meta}


# ------------------------------------------------------------- OSM -> cache ---

def _tags(el: dict) -> dict:
    return el.get("tags") or {}


def _geom_latlon(el: dict) -> list[tuple[float, float]]:
    g = el.get("geometry") or []
    return [(p["lat"], p["lon"]) for p in g if p.get("lat") is not None]


def _lanes_for(t: dict) -> tuple[int, str]:
    """Lane count plus how it was decided, so provenance survives into the cache."""
    raw = t.get("lanes")
    if raw:
        m = re.match(r"\s*(\d+)", raw)
        if m:
            n = int(m.group(1))
            if 1 <= n <= 12:
                # A oneway's `lanes` counts only its own direction; a two-way's
                # counts both, which is already what the generator wants.
                return max(2, min(12, n)), "osm_lanes"
    hw = t.get("highway", "")
    return HIGHWAY_LANES.get(hw, 2), "highway_class"


def _estimate_levels(t: dict, area_m2: float, built_frac: float) -> tuple[float, str]:
    """Levels for a building OSM did not tag, from proxies.

    Per the plan's decision: footprint area, building class, and how built-up the
    surroundings are. Deliberately conservative and boring -- the point is a
    credible skyline, not a guess dressed up as a measurement. Every building
    carries `height_src` so a bad skyline is attributable here rather than hunted
    in the emitters.
    """
    cls = (t.get("building") or "yes").lower()
    base = {
        "house": 1.0, "detached": 1.0, "bungalow": 1.0, "hut": 1.0,
        "garage": 1.0, "garages": 1.0, "shed": 1.0, "carport": 1.0,
        "terrace": 2.0, "semidetached_house": 2.0,
        "apartments": 4.0, "residential": 3.0, "dormitory": 4.0,
        "commercial": 3.0, "retail": 2.0, "supermarket": 1.0,
        "office": 6.0, "hotel": 6.0, "hospital": 5.0,
        "industrial": 2.0, "warehouse": 1.0, "factory": 2.0,
        "school": 2.0, "university": 3.0, "public": 3.0, "civic": 3.0,
        "church": 3.0, "cathedral": 6.0, "chapel": 2.0, "temple": 3.0,
        "train_station": 2.0, "stadium": 4.0,
    }.get(cls)
    if base is None:
        # Untyped `building=yes`: lean on area and surroundings. A big footprint
        # in a built-up core is usually a block, a small one a house.
        if area_m2 < 120.0:
            base = 1.0
        elif area_m2 < 400.0:
            base = 2.0
        else:
            base = 3.0
        base += 2.0 * built_frac
        return max(1.0, base), "estimated_area_context"
    # A typed building still gains a little in a dense core.
    return max(1.0, base + 1.5 * built_frac), "estimated_class_context"


def _polygon_area_m2(pts_m: list[tuple[float, float]]) -> float:
    if len(pts_m) < 3:
        return 0.0
    s = 0.0
    for i in range(len(pts_m)):
        x0, y0 = pts_m[i]
        x1, y1 = pts_m[(i + 1) % len(pts_m)]
        s += x0 * y1 - x1 * y0
    return abs(s) * 0.5


def convert_osm(osm: dict, proj: LocalProjection) -> dict:
    """Split one Overpass response into the cache's vector layers, in world units."""
    roads: list[dict] = []
    buildings: list[dict] = []
    areas: list[dict] = []
    signals: list[dict] = []
    water: list[dict] = []
    counts: dict[str, int] = {}

    def bump(k: str) -> None:
        counts[k] = counts.get(k, 0) + 1

    for el in osm.get("elements", []):
        t = _tags(el)
        if el.get("type") == "node":
            if t.get("highway") == "traffic_signals":
                x, z = proj.to_world(el["lat"], el["lon"])
                signals.append({"x": round(x, 2), "z": round(z, 2),
                                "direction": t.get("traffic_signals:direction")})
                bump("signals")
            continue
        if el.get("type") != "way":
            continue
        ll = _geom_latlon(el)
        if len(ll) < 2:
            continue
        pts = [proj.to_world(la, lo) for la, lo in ll]
        wpts = [{"x": round(x, 2), "z": round(z, 2)} for x, z in pts]
        closed = (len(ll) > 3
                  and abs(ll[0][0] - ll[-1][0]) < 1e-9
                  and abs(ll[0][1] - ll[-1][1]) < 1e-9)

        hw = t.get("highway")
        if hw:
            if hw in HIGHWAY_NONDRIVABLE:
                bump("highway_nondrivable")
                continue
            lanes, lanes_src = _lanes_for(t)
            roads.append({
                "id": el.get("id"),
                "name": t.get("name"),
                "class": hw,
                "lanes": lanes,
                "lanes_src": lanes_src,
                "oneway": t.get("oneway") in ("yes", "1", "-1", "true"),
                "surface": t.get("surface"),
                "bridge": bool(t.get("bridge")),
                "tunnel": bool(t.get("tunnel")),
                "layer": int(t["layer"]) if re.fullmatch(r"-?\d+", t.get("layer", "") or "") else 0,
                "median": (t.get("dual_carriageway") == "yes"
                           or t.get("divider") is not None
                           or t.get("junction") == "roundabout" and False),
                "sidewalk": t.get("sidewalk"),
                "width": t.get("width"),
                "maxspeed": t.get("maxspeed"),
                "points": wpts,
                "latlon": [[round(la, 7), round(lo, 7)] for la, lo in ll],
            })
            bump("roads")
            continue

        if t.get("building") or t.get("building:part"):
            m = [proj.to_metres(la, lo) for la, lo in ll]
            area = _polygon_area_m2(m)
            h_tag = t.get("height")
            lv_tag = t.get("building:levels")
            height_m, src = None, None
            if h_tag:
                mm = re.match(r"\s*([\d.]+)", h_tag)
                if mm:
                    height_m, src = float(mm.group(1)), "osm_height"
            if height_m is None and lv_tag:
                mm = re.match(r"\s*([\d.]+)", lv_tag)
                if mm:
                    height_m = float(mm.group(1)) * STOREY_HEIGHT_M
                    src = "osm_levels"
            buildings.append({
                "id": el.get("id"),
                "name": t.get("name"),
                "class": t.get("building") or t.get("building:part"),
                "part": bool(t.get("building:part")),
                "area_m2": round(area, 1),
                "height_m": round(height_m, 2) if height_m else None,
                "height_src": src,           # filled below when estimated
                "roof_shape": t.get("roof:shape"),
                "roof_height_m": t.get("roof:height"),
                "min_height_m": t.get("min_height"),
                "colour": t.get("building:colour"),
                "material": t.get("building:material"),
                "landmark": bool(t.get("tourism") or t.get("historic")
                                 or t.get("name") and t.get("building") in
                                 ("cathedral", "church", "stadium", "museum",
                                  "train_station", "civic", "public")),
                "points": wpts,
            })
            bump("buildings")
            continue

        leisure = t.get("leisure")
        landuse = t.get("landuse")
        natural = t.get("natural")
        waterway = t.get("waterway")

        if natural in ("water", "coastline") or waterway or landuse == "reservoir":
            water.append({"kind": natural or waterway or landuse,
                          "closed": closed, "points": wpts})
            bump("water")
            continue

        if leisure in ("park", "garden", "pitch", "playground", "common",
                       "recreation_ground", "dog_park") or landuse in (
                "grass", "village_green", "meadow", "forest", "cemetery",
                "allotments", "orchard", "vineyard", "residential",
                "commercial", "retail", "industrial", "construction"):
            areas.append({
                "id": el.get("id"),
                "name": t.get("name"),
                "kind": leisure or landuse,
                "leisure": leisure,
                "landuse": landuse,
                "closed": closed,
                "points": wpts,
            })
            bump("areas")
            continue
        bump("ignored")

    return {"roads": roads, "buildings": buildings, "areas": areas,
            "signals": signals, "water": water, "counts": counts}


# COVER class per OSM tag, used to bootstrap COVER.R8 before ESA WorldCover is
# wired in. OSM is sharper than 10 m where it is tagged, and WorldCover's job is
# to fill what OSM leaves blank -- so this ordering survives that addition.
_OSM_TO_COVER = {
    "forest": COVER_TREE, "wood": COVER_TREE, "orchard": COVER_TREE,
    "grass": COVER_GRASS, "village_green": COVER_GRASS, "meadow": COVER_GRASS,
    "park": COVER_GRASS, "garden": COVER_GRASS, "pitch": COVER_GRASS,
    "playground": COVER_GRASS, "common": COVER_GRASS, "dog_park": COVER_GRASS,
    "recreation_ground": COVER_GRASS, "cemetery": COVER_GRASS,
    "farmland": COVER_CROP, "allotments": COVER_CROP, "vineyard": COVER_CROP,
    "residential": COVER_BUILT, "commercial": COVER_BUILT,
    "retail": COVER_BUILT, "industrial": COVER_BUILT,
    "construction": COVER_BARE, "quarry": COVER_BARE, "brownfield": COVER_BARE,
}


def rasterize_layers(vec: dict, proj: LocalProjection, height: Raster,
                     ) -> tuple[Raster, Raster, dict]:
    """COVER.R8 and WATER.R8 on the SAME grid as HEIGHT.R16.

    Sharing the grid exactly is what lets td5_tg_world.c sample all of them with
    one index computation, and removes any chance of the water mask being offset
    from the terrain it is masking.
    """
    h, w = height.data.shape
    cover = np.full((h, w), COVER_NONE, dtype=np.uint8)
    water = np.zeros((h, w), dtype=np.uint8)

    def cells_of(points: list[dict]) -> list[tuple[int, int]]:
        out = []
        for p in points:
            ix = int(round((p["x"] - height.origin_x) / height.cell))
            iz = int(round((p["z"] - height.origin_z) / height.cell))
            if 0 <= ix < w and 0 <= iz < h:
                out.append((ix, iz))
        return out

    try:
        from shapely.geometry import Polygon, Point   # noqa: F401
        have_shapely = True
    except Exception:                                  # noqa: BLE001
        have_shapely = False

    def fill_polygon(points: list[dict], setter) -> int:
        """Scanline-fill a closed way on the cell grid.

        shapely does the containment test when present, because an OSM plaza can
        be concave and a bbox fill would paint the street outside it. Without
        shapely this degrades to stamping the outline, which is visibly worse but
        never wrong in the other direction.
        """
        cs = cells_of(points)
        if not cs:
            return 0
        if not have_shapely or len(cs) < 4:
            for ix, iz in cs:
                setter(ix, iz)
            return len(cs)
        from shapely.geometry import Polygon
        try:
            poly = Polygon([(p["x"], p["z"]) for p in points]).buffer(0)
        except Exception:                              # noqa: BLE001
            for ix, iz in cs:
                setter(ix, iz)
            return len(cs)
        if poly.is_empty:
            return 0
        minx, minz, maxx, maxz = poly.bounds
        i0 = max(0, int((minx - height.origin_x) / height.cell))
        i1 = min(w - 1, int(math.ceil((maxx - height.origin_x) / height.cell)))
        j0 = max(0, int((minz - height.origin_z) / height.cell))
        j1 = min(h - 1, int(math.ceil((maxz - height.origin_z) / height.cell)))
        n = 0
        from shapely.geometry import Point
        for jz in range(j0, j1 + 1):
            for ix in range(i0, i1 + 1):
                cx = height.origin_x + ix * height.cell
                cz = height.origin_z + jz * height.cell
                if poly.contains(Point(cx, cz)):
                    setter(ix, jz)
                    n += 1
        return n

    painted = {"cover_cells": 0, "water_cells": 0, "built_cells": 0}

    for a in vec["areas"]:
        cls = _OSM_TO_COVER.get(a["kind"])
        if cls is None:
            continue
        if a["closed"]:
            painted["cover_cells"] += fill_polygon(
                a["points"], lambda ix, iz, c=cls: cover.__setitem__((iz, ix), c))
        else:
            for ix, iz in cells_of(a["points"]):
                cover[iz, ix] = cls
                painted["cover_cells"] += 1

    for wy in vec["water"]:
        if wy["closed"]:
            painted["water_cells"] += fill_polygon(
                wy["points"], lambda ix, iz: (water.__setitem__((iz, ix), 1),
                                              cover.__setitem__((iz, ix), COVER_WATER))[0])
        else:
            for ix, iz in cells_of(wy["points"]):
                water[iz, ix] = 1
                cover[iz, ix] = COVER_WATER

    # Buildings imply BUILT even where no landuse polygon says so, which is what
    # makes the built-up fraction usable as a height proxy.
    for b in vec["buildings"]:
        for ix, iz in cells_of(b["points"]):
            if cover[iz, ix] in (COVER_NONE,):
                cover[iz, ix] = COVER_BUILT
            painted["built_cells"] += 1

    cover_r = Raster(cover, height.origin_x, height.origin_z, height.cell,
                     1.0, 0.0, 255, height.rotation_rad)
    water_r = Raster(water, height.origin_x, height.origin_z, height.cell,
                     1.0, 0.0, 255, height.rotation_rad)
    return cover_r, water_r, painted


def estimate_building_heights(vec: dict, cover: Raster) -> dict:
    """Fill every untagged building's height, flagging it as estimated."""
    h, w = cover.data.shape
    built = (cover.data == COVER_BUILT)
    stats = {"osm_height": 0, "osm_levels": 0, "estimated": 0}

    for b in vec["buildings"]:
        if b["height_m"]:
            stats[b["height_src"]] = stats.get(b["height_src"], 0) + 1
            continue
        # Built-up fraction in a 5-cell (~17 m) neighbourhood of the footprint.
        ix = iz = None
        if b["points"]:
            p = b["points"][0]
            ix = int(round((p["x"] - cover.origin_x) / cover.cell))
            iz = int(round((p["z"] - cover.origin_z) / cover.cell))
        frac = 0.0
        if ix is not None and 0 <= ix < w and 0 <= iz < h:
            i0, i1 = max(0, ix - 2), min(w, ix + 3)
            j0, j1 = max(0, iz - 2), min(h, iz + 3)
            win = built[j0:j1, i0:i1]
            if win.size:
                frac = float(win.mean())
        levels, src = _estimate_levels(
            {"building": b["class"]}, b["area_m2"], frac)
        b["levels_est"] = round(levels, 2)
        b["height_m"] = round(levels * STOREY_HEIGHT_M, 2)
        b["height_src"] = src
        b["built_frac"] = round(frac, 3)
        stats["estimated"] += 1
    return stats


# --------------------------------------------------------------------- main ---

def fetch_place(name: str, lat: float, lon: float, radius_m: float,
                root: str = CACHE_ROOT,
                units_per_metre: float = UNITS_PER_METRE_DERIVED,
                dem_override: str | None = None,
                cell: float = TG_WORLD_CELL,
                rotation_rad: float = 0.0,
                offset_x: float = 0.0, offset_z: float = 0.0,
                smooth_m: float = DEM_SMOOTH_M_DEFAULT) -> dict:
    slug = slugify(name)
    out = place_dir(slug, root)
    os.makedirs(out, exist_ok=True)

    # Degrees for the requested radius, at this latitude.
    dlat = radius_m / 110_574.0
    dlon = radius_m / (111_320.0 * math.cos(math.radians(lat)))
    bbox = (lat - dlat, lon - dlon, lat + dlat, lon + dlon)

    # ONE frame for the whole cache. The route is rotated so its start tangent
    # lands on +X, and the vectors AND the rasters must be expressed in that same
    # frame -- otherwise the roads and the terrain both sit at an angle to the
    # road the game actually drives, with nothing raising an error. Pipeline
    # order is therefore: fetch unrotated -> route -> condition (which DECIDES
    # the rotation) -> re-fetch with --rotation. The second pass is free because
    # every network response is already cached.
    proj = LocalProjection(lat, lon, units_per_metre)
    proj.set_rotation(rotation_rad)
    proj.set_offset(offset_x, offset_z)

    print("place  : %s (%s)" % (name, slug))
    print("centre : %.6f, %.6f  radius %.0f m" % (lat, lon, radius_m))
    print("bbox   : S%.5f W%.5f N%.5f E%.5f" % bbox)
    print("frame  : rotation %.6f rad (%.2f deg), offset %.0f, %.0f units"
          % (rotation_rad, math.degrees(rotation_rad), offset_x, offset_z))

    print("\n[1/5] IGN elevation coverage probe")
    ign = probe_ign_dem(bbox)
    if ign.get("available"):
        print("  projects: %s" % ", ".join("%s x%d" % (k, v)
                                           for k, v in ign["projects"].items()))
        if "MDE 5m" in ign.get("projects", {}):
            print("  5 m EXISTS here -- request the sheet and drop a GeoTIFF in %s"
                  % DEM_OVERRIDE_DIR)
    else:
        print("  none (outside Argentina, or the service did not answer)")

    print("\n[2/5] Overpass")
    osm = fetch_osm(bbox, out)
    print("  elements: %d" % len(osm.get("elements", [])))

    print("\n[3/5] vector layers -> world units")
    vec = convert_osm(osm, proj)
    vec["roads"], clip = clip_roads(vec["roads"])
    if clip:
        vec["counts"]["roads"] = len(vec["roads"])
        vec["counts"].update(clip)
        print("  DENSE CENTRE: clipped %d drivable ways to %d (cap %d); dropped "
              "%s" % (clip["clipped_from"], len(vec["roads"]), clip["cap"],
                      ", ".join(clip["dropped_classes"]) or "nothing"))
    for k in sorted(vec["counts"]):
        print("  %-20s %s" % (k, vec["counts"][k]))

    print("\n[4/5] elevation")
    height, dem_prov = build_height_raster(proj, bbox, out, cell, dem_override,
                                           smooth_m)
    hs = height.stats()
    print("  grid %dx%d cells of %.0f units (%.2f m)"
          % (hs["width"], hs["height"], cell, cell / units_per_metre))
    print("  source %s, relief %.1f m (%.1f..%.1f), lowpass %s"
          % (dem_prov["source"], dem_prov["relief_m"],
             dem_prov["min_m"], dem_prov["max_m"],
             ("%.0f m (%d cells)" % (dem_prov["lowpass_m"],
                                     dem_prov["lowpass_radius_cells"]))
             if dem_prov.get("lowpass_m") else "none"))

    print("\n[5/5] rasterize cover/water, estimate building heights")
    cover, water, painted = rasterize_layers(vec, proj, height)
    bh = estimate_building_heights(vec, cover)
    print("  cover cells %d, water cells %d, built stamps %d"
          % (painted["cover_cells"], painted["water_cells"],
             painted["built_cells"]))
    print("  building heights: %s" % ", ".join("%s=%d" % kv
                                               for kv in sorted(bh.items())))

    # ---- write the contract ---------------------------------------------
    height.write(os.path.join(out, "HEIGHT.R16"))
    cover.write(os.path.join(out, "COVER.R8"))
    water.write(os.path.join(out, "WATER.R8"))
    write_json(os.path.join(out, "ROADS.JSON"), {"roads": vec["roads"]})
    write_json(os.path.join(out, "BUILDINGS.JSON"),
               {"buildings": vec["buildings"], "storey_height_m": STOREY_HEIGHT_M,
                "height_provenance": bh})
    write_json(os.path.join(out, "AREAS.JSON"), {"areas": vec["areas"]})
    write_json(os.path.join(out, "SIGNALS.JSON"), {"signals": vec["signals"]})

    place = {
        "name": name,
        "slug": slug,
        "centre": {"lat": lat, "lon": lon},
        "radius_m": radius_m,
        "bbox": {"south": bbox[0], "west": bbox[1],
                 "north": bbox[2], "east": bbox[3]},
        "projection": proj.describe(),
        "rotation_rad": rotation_rad,
        "offset_x": offset_x,
        "offset_z": offset_z,
        "cell_units": cell,
        "cell_m": cell / units_per_metre,
        "fetched_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "layers": {
            "osm": {
                "source": "OpenStreetMap via Overpass",
                "licence": "ODbL 1.0 -- attribution required on screen",
                "vintage": "live at fetch time",
                "counts": vec["counts"],
            },
            "height": dem_prov,
            "cover": {
                "source": "OSM landuse/leisure (ESA WorldCover not yet wired)",
                "vocabulary": "ESA WorldCover class ids",
                "painted": painted,
            },
            "canopy": {
                "source": "NOT YET WIRED -- Meta/WRI Global Canopy Height, 1 m, "
                          "CC-BY 4.0, cloud-optimised GeoTIFF on AWS S3 "
                          "(registry.opendata.aws/dataforgood-fb-forestsv2). "
                          "Needs a COG range reader or GDAL; this environment has "
                          "tifffile but no GDAL.",
            },
        },
        "attribution": [
            "Map data (c) OpenStreetMap contributors, ODbL 1.0",
            "Elevation: %s" % dem_prov.get("source"),
        ],
    }
    write_json(os.path.join(out, "PLACE.JSON"), place)
    print("\nwrote %s" % out)
    return place


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", required=True)
    ap.add_argument("--lat", type=float, required=True)
    ap.add_argument("--lon", type=float, required=True)
    ap.add_argument("--radius", type=float, default=2200.0,
                    help="metres; a 6.3 km route wants ~2200 with margin")
    ap.add_argument("--root", default=CACHE_ROOT)
    ap.add_argument("--units-per-metre", type=float,
                    default=UNITS_PER_METRE_DERIVED)
    ap.add_argument("--dem-override",
                    help="GeoTIFF or Terrarium-coded PNG to use instead of tiles")
    ap.add_argument("--dem-smooth-m", type=float, default=DEM_SMOOTH_M_DEFAULT,
                    help="DEM lowpass width in metres; 0 disables. See "
                         "DEM_SMOOTH_M_DEFAULT for the measurements behind the "
                         "default")
    ap.add_argument("--rotation", type=float, default=0.0,
                    help="frame rotation in radians; pass the rotation_rad from "
                         "a conditioned ROUTE.JSON so the cache and the route "
                         "share one frame")
    ap.add_argument("--frame-from",
                    help="read rotation_rad AND offset_x/offset_z from this "
                         "conditioned ROUTE.JSON -- the route's full frame. Use "
                         "this rather than --rotation: the conditioner also "
                         "TRANSLATES the route so node 0 lands at the origin, and "
                         "a cache built with only the rotation puts the terrain "
                         "somewhere else.")
    ap.add_argument("--probe-dem", action="store_true",
                    help="only report IGN elevation coverage, fetch nothing")
    a = ap.parse_args(argv)

    if a.probe_dem:
        dlat = a.radius / 110_574.0
        dlon = a.radius / (111_320.0 * math.cos(math.radians(a.lat)))
        info = probe_ign_dem((a.lat - dlat, a.lon - dlon,
                              a.lat + dlat, a.lon + dlon))
        print(json.dumps(info, indent=1, ensure_ascii=False))
        return 0

    rot, ox, oz = a.rotation, 0.0, 0.0
    if a.frame_from:
        with open(a.frame_from, encoding="utf-8") as f:
            fr = json.load(f)
        rot = fr["rotation_rad"]
        ox, oz = fr.get("offset_x", 0.0), fr.get("offset_z", 0.0)
        print("frame from %s" % a.frame_from)

    fetch_place(a.name, a.lat, a.lon, a.radius, a.root,
                a.units_per_metre, a.dem_override, TG_WORLD_CELL, rot, ox, oz,
                a.dem_smooth_m)
    return 0


if __name__ == "__main__":
    sys.exit(main())
