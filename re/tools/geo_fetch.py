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
# Shelf depth the bathymetry is clamped to before the DEM lowpass (see
# build_height_raster). Shallow enough that the blur cannot drag the shore under
# the water plane, deep enough to stay below any sea cell's mean height guard.
SEA_FLOOR_CLAMP_M = -4.0
# A region bounded by coastline is SEA only if more than SEA_LOW_FRAC_MIN of its
# cells sit below SEA_LOW_M. Measured on Valparaiso: the sea side 0.69, the land
# side 0.00. Not the MEAN: Terrarium stays a metre or two above zero for up to
# ~1.7 km offshore there, which put the sea region's mean at 1.7 m.
SEA_LOW_M = 0.5
# Sea is cleared this far beyond a non-bridge road's half width (see
# rasterize_layers): OSM roads that are not bridges are on land.
SEA_ROAD_CLEAR_M = 6.0
# A hole enclosed by sea becomes sea if >95% of it is under this (an island is not).
SEA_HOLE_MAX_M = 3.0
SEA_LOW_FRAC_MIN = 0.5

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
# The PEDESTRIAN subset of the above. These stay fully non-drivable -- they are
# still in HIGHWAY_NONDRIVABLE, so ROADS.JSON, the router and every drivable
# reader see exactly what they saw before -- but instead of being counted and
# thrown away they are written to their own FOOTWAYS.JSON layer.
#
# Measured on the La Plata cache (2.7 km radius): 865 ways carried a
# non-drivable highway value and ALL 865 are in this subset -- footway 737,
# steps 58, cycleway 38, path 26, pedestrian 6. Not one `track`, `bridleway`,
# `corridor`, `platform`, `construction` or `proposed` way exists there. So the
# old `highway_nondrivable: 865` census line was never about tracks and farm
# roads; it was the entire mapped pavement network of a planned city, which is
# the single biggest road-layer loss the 2026-10-08 attribute audit named.
#
# The rest of the set keeps being dropped: a `platform` is a railway object, and
# `construction`/`proposed` describe a way that does not exist on the ground.
HIGHWAY_PEDESTRIAN = {"footway", "path", "cycleway", "pedestrian", "steps"}

# Storey height for the building:levels fallback. 3.0 m is the usual planning
# figure for mixed residential/commercial and is an OPEN question in the plan
# (section 10 item 3): per-country or per-landuse would be better.
STOREY_HEIGHT_M = 3.0

# Bumped whenever the tag set written into the cache changes, and recorded in
# PLACE.JSON as `tag_schema`. A reader can then tell a pre-2026-10-07 cache
# (no amenity/office/government on a building, `landmark` decided by the old
# narrow rule) from one fetched after, without guessing from which keys happen
# to be present on the footprint it is looking at.
#   1  the original Phase 1..5 set
#   2  2026-10-07, round 1007: the full tag path (docs/plans/GEO_TAG_AUDIT.md)
#   3  2026-10-09, round 1011 C4: FOOTWAYS.JSON exists; node[] carries
#      highway=bus_stop with its shelter/bench/bin tags; crossing nodes promote
#      `crossing` / `crossing_markings` to fields. A schema-2 cache simply has
#      no FOOTWAYS.JSON and no bus_stop nodes, which every reader added in this
#      round treats as "this place has none" rather than as an error.
TAG_SCHEMA = 3

# ------------------------------------------------------------ landmark rule ---
#
# THIS IS THE PERMANENT HOME the 2026-09-30 close-out asked for. The rule was
# `tourism or historic or (name and building in {cathedral, church, stadium,
# museum, train_station, civic, public})`, which flagged 10 of La Plata's 2047
# footprints and only 2 within 100 m of the route -- so the whole landmark path
# shipped exercised twice. The tags that catch the rest were never carried into
# BUILDINGS.JSON, so no C-side rule could see them.
#
# The table below is `re/tools/geo_fixtures/land_relabel.py`'s RULE, which was
# written as the offline stand-in for this and measured on the raw responses:
# La Plata 10 -> 77 cache landmarks, 2 of 54 -> 11 of 54 within 100 m of the
# route. `land_relabel.py` stays as the tool that upgrades an OLD cache in
# place without the network; the two tables are kept identical on purpose.
#
# tag -> the values that name a landmark, or True for "any value".
#
# ORDER IS THE ATTRIBUTION ORDER, not a precedence that can change the answer:
# `landmark` is an OR over every row, so reordering moves only which tag is
# named in `landmark_src`. Most specific statement of WHAT THE STRUCTURE IS
# first. Measured reason for this order: with `tourism` ahead of `building`,
# La Plata's Cathedral came out as "landmark because tourism=viewpoint", which
# is true, useless, and hides `building=cathedral` sitting right beside it.
LANDMARK_RULE = {
    "building": {"cathedral", "church", "chapel", "basilica", "mosque",
                 "synagogue", "temple", "monastery", "shrine", "stadium",
                 "museum", "palace", "castle", "monument", "memorial",
                 "train_station", "courthouse", "townhall", "government",
                 "civic", "public", "theatre", "opera_house"},
    "government": True,
    "office": {"government", "diplomatic"},
    "amenity": {"place_of_worship", "theatre", "townhall", "courthouse",
                "arts_centre", "police", "fire_station", "embassy", "casino",
                "cinema", "conference_centre", "exhibition_centre",
                "monastery", "public_building"},
    "historic": True,
    "heritage": True,
    "man_made": {"tower", "lighthouse", "obelisk", "water_tower", "campanile"},
    "tourism": {"attraction", "museum", "gallery", "artwork", "viewpoint",
                "theme_park", "aquarium", "zoo", "monument"},
}
# Deliberately NOT promoted, and why: a hotel or a hostel carries `tourism=*`
# and is ordinary street frontage. The OLD blanket `tourism or historic` is
# what put "UNICO Eco Hostel Boutique" in the La Plata landmark list, so
# widening the rule and narrowing this one are the same change.
# `amenity=community_centre` is out for the same reason -- La Plata has 14 and
# they are neighbourhood social clubs in ordinary shopfronts.
# `university`, `school`, `hospital`, `office` and `retail` as BUILDING values
# stay out too: 29 universities and 13 schools in one cache is urban fabric,
# not landmarks (the same judgement the C reader's geob_class_is_landmark
# records).
TOURISM_NOT_LANDMARK = {"hotel", "hostel", "guest_house", "motel", "apartment",
                        "chalet", "camp_site", "caravan_site", "information"}


def landmark_decide(t: dict) -> tuple[bool, str | None]:
    """(is_landmark, "key=value") for one tag dict, or (False, None).

    The deciding tag is returned so a promotion is ATTRIBUTABLE: it goes into
    BUILDINGS.JSON as `landmark_src` and into the build log's census. Without
    it "22 landmarks" cannot be told from "22 hostels".
    """
    for key, allow in LANDMARK_RULE.items():
        v = t.get(key)
        if not v:
            continue
        if key == "tourism" and v in TOURISM_NOT_LANDMARK:
            continue
        if allow is True or v in allow:
            return True, "%s=%s" % (key, v)
    return False, None


# ------------------------------------------------------------- tag carriage ---
#
# Which keys are copied verbatim into each layer's records, over and above the
# fields the normaliser derives. Carried as a flat `tags` sub-object so adding
# a key is a one-line change here and costs no schema churn: a C reader that
# does not know a key simply never asks for it, and the derived fields the
# readers DO use (class, lanes, height_m, landmark, ...) keep their old names
# and old types. That is what makes this backward compatible.
BUILDING_TAG_KEYS = (
    # civic identity -- the close-out's gap
    "amenity", "office", "government", "tourism", "historic", "heritage",
    "man_made", "religion", "denomination", "leisure", "shop", "healthcare",
    "emergency", "craft", "club",
    # 3D form
    "building", "building:part", "building:levels", "building:min_level",
    "height", "min_height", "roof:shape", "roof:height", "roof:levels",
    "roof:material", "roof:colour", "roof:orientation", "roof:direction",
    "roof:angle", "building:colour", "building:material", "building:use",
    # identity / provenance
    "name", "name:en", "name:es", "alt_name", "short_name", "official_name",
    "operator", "brand", "wikidata", "wikipedia", "start_date", "layer",
    "addr:street", "addr:housenumber", "addr:city",
)
ROAD_TAG_KEYS = (
    "highway", "name", "name:en", "name:es", "alt_name", "old_name", "ref",
    "lanes", "lanes:forward", "lanes:backward", "width", "surface",
    "smoothness", "tracktype", "maxspeed", "oneway", "junction", "roundabout",
    "sidewalk", "sidewalk:left", "sidewalk:right", "footway", "crossing",
    # [ROUND 1011 C2] the MEASURED per-side pavement width. Zero ways carry any
    # of these on the La Plata cache, so keeping them changes nothing there --
    # they are here so that a place which DOES tag a width gets the real number
    # instead of the frontage rule, and so the reader has something to read.
    "sidewalk:width", "sidewalk:both:width",
    "sidewalk:left:width", "sidewalk:right:width",
    "lit", "incline", "bridge", "bridge:structure", "tunnel", "covered",
    "layer", "access", "service", "motor_vehicle", "area", "lane_markings",
    "dual_carriageway", "divider", "destination", "destination:street",
)
AREA_TAG_KEYS = (
    "leisure", "landuse", "natural", "amenity", "tourism", "historic",
    "man_made", "sport", "surface", "barrier", "access", "layer", "name",
    "name:en", "name:es", "operator", "wikidata", "water", "area", "place",
)
WATER_TAG_KEYS = (
    "natural", "waterway", "water", "landuse", "name", "layer", "tunnel",
    "covered", "intermittent", "width", "bridge",
)
NODE_TAG_KEYS = (
    "highway", "traffic_signals", "traffic_signals:direction", "crossing",
    "crossing:markings", "button_operated", "tactile_paving",
    "traffic_calming", "direction", "stop", "give_way", "railway",
    # Round 1011 C4, the transit keys. SAFE TO ADD WITHOUT MOVING signals[]:
    # measured over the La Plata cache, the complete key set present on its 566
    # highway=traffic_signals nodes is highway, crossing, traffic_signals,
    # tactile_paving, source, button_operated, traffic_signals:sound,
    # traffic_signals:vibration and traffic_calming. Not one of them carries any
    # key below, so every signals[] record is byte-identical to schema 2. The
    # counts these DO reach are the 370 bus-stop nodes: public_transport 370,
    # bus 335, name 329, shelter 105, bench 62, ref 56, bin 39, lit 36.
    "public_transport", "bus", "shelter", "shelter_type", "bench", "bin",
    "name", "name:en", "name:es", "ref", "local_ref", "network", "operator",
    "lit", "covered", "departures_board", "wheelchair", "bus_stop",
)
# A pedestrian way carries a different vocabulary from a drivable one: the
# subtype (`footway=sidewalk` vs `=crossing`), the crossing paint, stair count
# and handrail, segregation of a shared foot/cycle path. `width` and `surface`
# overlap with ROAD_TAG_KEYS and are kept under the same spelling on purpose, so
# a reader that already parses a road width parses a footway width identically.
FOOTWAY_TAG_KEYS = (
    "highway", "footway", "name", "name:en", "name:es",
    "crossing", "crossing:markings", "crossing:island", "tactile_paving",
    "width", "est_width", "surface", "smoothness", "lit", "covered",
    "incline", "ramp", "step_count", "handrail", "segregated",
    "foot", "bicycle", "horse", "wheelchair", "access", "motor_vehicle",
    "bridge", "tunnel", "layer", "area", "indoor", "oneway", "material",
)


def _keep_tags(t: dict, keys) -> dict:
    """The subset of `t` whose keys are in `keys` and whose value is non-empty.

    Values stay RAW STRINGS, exactly as Overpass hands them over. The 2026-09-30
    `min_height` bug was the opposite mistake made once already -- geo_fetch
    stored the string and the C reader's cJSON_IsNumber check read 0 -- so the
    rule is: raw here, parse at the point of use, and never two spellings of
    the same fact in one file.
    """
    return {k: t[k] for k in keys if t.get(k)}


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

# GATE 1 of the tag path (docs/plans/GEO_TAG_AUDIT.md). A tag on a way this
# query already selects arrives FREE -- Overpass returns every tag of a matched
# element -- which is why `amenity` was in the La Plata cache all along and the
# 2026-09-30 loss was in the NORMALISER, not here. What a clause buys is the
# element no other clause matches.
#
# WIDENED 2026-10-07:
#   building:part   the part-stack branch in convert_osm was unreachable for a
#                   part way not also tagged `building`, which is most of them.
#   amenity/office/government/tourism/historic/heritage/shop/man_made
#                   a standalone civic AREA (a walled school ground, a
#                   place_of_worship mapped as an area with no building tag).
#   natural         bare, not just water/coastline: `_OSM_TO_COVER` carries a
#                   `wood` row that nothing could reach.
#   node["highway"] bare, not just traffic_signals: crossings, stop lines and
#                   give-ways were 0 of 566 nodes because the query never asked.
#   node[traffic_calming]  speed humps.
#
# NOT widened: relation[...]. A multipolygon needs outer/inner ring assembly in
# convert_osm and a ring pool that understands holes; shipping that untested
# against the ONE authorised re-fetch is the wrong risk. See the audit's
# "Not done" section.
OVERPASS_QL = """[out:json][timeout:180];
(
  way["highway"](%(bbox)s);
  node["highway"](%(bbox)s);
  node["traffic_calming"](%(bbox)s);
  way["natural"](%(bbox)s);
  way["waterway"](%(bbox)s);
  way["landuse"](%(bbox)s);
  way["leisure"](%(bbox)s);
  way["building"](%(bbox)s);
  way["building:part"](%(bbox)s);
  way["amenity"](%(bbox)s);
  way["office"](%(bbox)s);
  way["government"](%(bbox)s);
  way["tourism"](%(bbox)s);
  way["historic"](%(bbox)s);
  way["heritage"](%(bbox)s);
  way["shop"](%(bbox)s);
  way["man_made"](%(bbox)s);
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
    # SEA FLOOR CLAMP (2026-09-30, Valparaiso). Terrarium carries BATHYMETRY:
    # the Valparaiso cache read -320 m offshore. The game never sees the sea
    # floor (WATER.R8 lays a water plane there), but the 200 m lowpass below
    # would average that trench into the shore and sink the waterfront road
    # tens of metres below the water. Clamp the floor to a shallow shelf first.
    # A no-op wherever the DEM stays above it (La Plata's minimum is -2 m).
    below = int((out < SEA_FLOOR_CLAMP_M).sum())
    if below:
        out = np.maximum(out, SEA_FLOOR_CLAMP_M)
    provenance["sea_floor_clamp_m"] = SEA_FLOOR_CLAMP_M
    provenance["sea_floor_clamped_cells"] = below

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


# GATE 2's second use of the new tags: the HEIGHT ESTIMATOR.
#
# A building tagged only `building=yes` but also `amenity=place_of_worship` or
# `office=government` is not a 3-storey house, and until this round the
# estimator could not see either tag. These are levels for the non-`building`
# key that names the use, applied only when `building` itself is untyped
# (`yes` / absent) -- a mapper who wrote `building=apartments` has already
# said more than `shop=supermarket` does, so the building value keeps priority.
#
# ONLY USES THAT DESCRIBE THE WHOLE STRUCTURE. This is the distinction that
# makes the table defensible, and it was MEASURED rather than assumed: the first
# cut also carried the tenant-shaped tags (restaurant, pharmacy, bank,
# supermarket, office=company/insurance/estate_agent, fitness_centre, hotel) and
# moved 256 of La Plata's 2047 footprints -- 133 of them SHORTER, p50 -1.1 m,
# worst -7.5 m. That is the wrong direction and for a knowable reason: on a
# planned city's 110 m blocks `amenity=restaurant` on a building way means
# "there is a restaurant in this block", not "this block is a restaurant", so
# reading it as a 2-storey use DEMOLISHED four floors off a block the area
# context had sized correctly.
#
# A church, a townhall, a courthouse, a fire station, a stadium or a ministry
# IS the building. A pharmacy is a shopfront in one. Only the first kind is
# here; the second keeps the area/context estimate, which is what it was for.
# Same judgement the landmark rule and the C reader's geob_class_is_landmark
# already record about `university`/`school`/`retail`.
#
# Values are the same boring order of magnitude as the building table below:
# a parish church is a tall single volume, a ministry is an office block, a
# fire station is low-rise, a sports hall is one tall storey.
#
# (levels, min_area_m2). THE AREA FLOOR IS THE SECOND HALF OF THE SAME
# ARGUMENT, and it was also measured rather than assumed. With no floor, six La
# Plata footprints of 115..324 m2 tagged `amenity=hospital` or
# `office=government` -- "Sancor Salud" at 155 m2, "PAMI" at 154 m2,
# "Ministerio del Interior y Transporte" at 125 m2 -- rose by 7.5 to 10.5 m to
# 5-7 storeys. A 5-storey hospital does not have a 155 m2 footprint; those are
# branch offices and consulting rooms at street level, i.e. the tenant read
# again, and for them v1's AREA ladder (under 120 m2 -> 1 storey, under 400 ->
# 2) is the better signal and is kept.
#
# 400 m2 is v1's own "big footprint" threshold, so the two estimators agree on
# where a footprint stops being a house-sized thing.
#
# A floor of 0 means the use needs no floor area to be what it claims: a tower,
# a campanile, a water tower and a monument are legitimately small and tall,
# and a blanket minimum would flatten exactly the silhouettes worth having.
_USE_MIN_AREA_M2 = 400.0
_USE_LEVELS = {
    # civic / religious -- the building is its use, and it takes room
    ("amenity", "place_of_worship"): (3.0, 0.0),   # a chapel is small and tall
    ("amenity", "townhall"): (4.0, _USE_MIN_AREA_M2),
    ("amenity", "courthouse"): (4.0, _USE_MIN_AREA_M2),
    ("amenity", "police"): (2.0, _USE_MIN_AREA_M2),
    ("amenity", "fire_station"): (2.0, _USE_MIN_AREA_M2),
    ("amenity", "embassy"): (3.0, _USE_MIN_AREA_M2),
    ("amenity", "prison"): (3.0, _USE_MIN_AREA_M2),
    ("amenity", "theatre"): (3.0, _USE_MIN_AREA_M2),
    ("amenity", "cinema"): (3.0, _USE_MIN_AREA_M2),
    ("amenity", "arts_centre"): (3.0, _USE_MIN_AREA_M2),
    ("amenity", "conference_centre"): (3.0, _USE_MIN_AREA_M2),
    ("amenity", "exhibition_centre"): (2.0, _USE_MIN_AREA_M2),
    ("amenity", "casino"): (2.0, _USE_MIN_AREA_M2),
    ("amenity", "hospital"): (5.0, _USE_MIN_AREA_M2),
    ("amenity", "school"): (2.0, _USE_MIN_AREA_M2),
    ("amenity", "college"): (3.0, _USE_MIN_AREA_M2),
    ("amenity", "university"): (3.0, _USE_MIN_AREA_M2),
    ("amenity", "library"): (2.0, _USE_MIN_AREA_M2),
    ("amenity", "marketplace"): (1.0, _USE_MIN_AREA_M2),
    ("amenity", "fuel"): (1.0, 0.0),      # a forecourt canopy, always low
    ("amenity", "parking"): (1.0, 0.0),   # a car-park deck, always low
    # government -- `government=*` and `office=government` name the whole seat,
    # not a desk in it, PROVIDED the footprint is a seat-sized building
    ("government", "administrative"): (6.0, _USE_MIN_AREA_M2),
    ("government", "ministry"): (7.0, _USE_MIN_AREA_M2),
    ("government", "legislative"): (5.0, _USE_MIN_AREA_M2),
    ("government", "healthcare"): (4.0, _USE_MIN_AREA_M2),
    ("government", "yes"): (5.0, _USE_MIN_AREA_M2),
    ("office", "government"): (6.0, _USE_MIN_AREA_M2),
    ("office", "diplomatic"): (4.0, _USE_MIN_AREA_M2),
    # big-span sheds
    ("leisure", "sports_hall"): (1.0, _USE_MIN_AREA_M2),
    ("leisure", "sports_centre"): (2.0, _USE_MIN_AREA_M2),
    ("leisure", "stadium"): (4.0, _USE_MIN_AREA_M2),
    ("shop", "mall"): (2.0, _USE_MIN_AREA_M2),   # a mall IS the building; a
                                      # supermarket is often a ground floor,
                                      # so it is out of the table entirely
    # free-standing vertical structures -- small footprint, tall on purpose
    ("man_made", "tower"): (8.0, 0.0),
    ("man_made", "water_tower"): (6.0, 0.0),
    ("man_made", "lighthouse"): (6.0, 0.0),
    ("man_made", "campanile"): (8.0, 0.0),
    ("historic", "monument"): (3.0, 0.0),
    ("historic", "castle"): (5.0, _USE_MIN_AREA_M2),
    ("tourism", "museum"): (3.0, _USE_MIN_AREA_M2),
    # `historic=memorial` is NOT here: it is a plaque on a wall as often as it
    # is a structure. La Plata's "Espacio Memoria ex Comisaria 5ta" is a 1381
    # m2 former police station, and reading the memorial tag as one storey took
    # 7.5 m off a real building -- the single worst drop the A/B found.
}
# Order a use is consulted in when a footprint carries several. Most specific
# statement of "what this building IS" first.
_USE_KEY_ORDER = ("government", "office", "amenity", "historic",
                  "tourism", "man_made", "leisure", "shop")


def _use_levels(t: dict, area_m2: float) -> tuple[float | None, str | None]:
    """Levels implied by a non-`building` use tag, or (None, None).

    A row whose footprint is under its own `min_area_m2` is REFUSED rather than
    clamped: the tag is being read as a tenant and the caller's area/context
    ladder is the better estimate, so the right answer is to decline and let it
    run. The FIRST matching key decides -- a row that declines does not fall
    through to a lower-priority key, because a small `office=government` is
    still a government office and consulting its `amenity` would be guessing
    twice.
    """
    for key in _USE_KEY_ORDER:
        v = t.get(key)
        if not v:
            continue
        row = _USE_LEVELS.get((key, v))
        if row is None:
            continue
        lv, min_area = row
        if min_area > 0.0 and area_m2 < min_area:
            return None, None
        return lv, "%s=%s" % (key, v)
    return None, None


def _estimate_levels(t: dict, area_m2: float, built_frac: float,
                     estimator: int = 2) -> tuple[float, str]:
    """Levels for a building OSM did not tag, from proxies.

    Per the plan's decision: footprint area, building class, and how built-up the
    surroundings are. Deliberately conservative and boring -- the point is a
    credible skyline, not a guess dressed up as a measurement. Every building
    carries `height_src` so a bad skyline is attributable here rather than hunted
    in the emitters.

    `estimator` 1 is the pre-2026-10-07 behaviour, kept so the La Plata
    MODELS.DAT delta can be split into "new landmarks" and "new heights"
    instead of asserted to be one of them. Selected with
    `--levels-estimator v1`; the estimator in force is recorded in
    BUILDINGS.JSON's `height_provenance` and in PLACE.JSON.
    """
    cls = (t.get("building") or "yes").lower()
    if estimator >= 2 and cls in ("yes", ""):
        # Only an UNTYPED building defers to its use tag: `building=apartments`
        # is already a statement about form, `shop=supermarket` is a statement
        # about trade that happens to correlate with one.
        lv, why = _use_levels(t, area_m2)
        if lv is not None:
            return max(1.0, lv + 1.5 * built_frac), "estimated_use:" + why
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


def _int_tag(t: dict, key: str) -> int:
    """A signed integer tag, 0 when absent or not an integer. OSM `layer` is the
    case: it is free text and "-1", " 2" and "1;2" all occur."""
    m = re.match(r"\s*(-?\d+)", t.get(key, "") or "")
    return int(m.group(1)) if m else 0


def _oneway_dir(t: dict) -> int:
    """+1 forward, -1 against the way's own drawing order, 0 two-way.

    The old code collapsed this to a bool with `oneway in ("yes","1","-1",
    "true")`, so a street tagged `oneway=-1` (1997 ways carry some oneway value
    at La Plata) read as "one way, forward" -- the correct flag with the wrong
    direction. The bool is still written under the old name and keeps the old
    meaning; the direction is a new field beside it.
    """
    v = (t.get("oneway") or "").strip().lower()
    if v in ("yes", "1", "true"):
        return 1
    if v in ("-1", "reverse", "backward"):
        return -1
    return 0


def _yes_tag(t: dict, key: str) -> bool:
    """A yes/no OSM tag as a bool. Absent is False, and so is an EXPLICIT
    negative: OSM really does carry `shelter=no` where a mapper surveyed that
    there is none, and a bare truthiness test would build one there."""
    return (t.get(key) or "").strip().lower() not in ("", "no", "false", "0")


def _width_m(t: dict, key: str = "width") -> float | None:
    """A width tag in METRES, or None when absent or unparseable.

    OSM width is free text: "2", "2.5", "2 m", "1.8m" and the odd "3;4" all
    occur on La Plata's 27 tagged pedestrian ways. Feet ("4'") are left
    unparsed and return None rather than being read as 4 metres -- a silently
    wrong width is worse than a missing one, because the fallback that replaces
    a missing one is a measured class default.
    """
    v = (t.get(key) or "").strip().lower()
    if not v:
        return None
    m = re.match(r"\s*(\d+(?:\.\d+)?)\s*(m|metre|metres|meter|meters)?\s*$", v)
    if not m:
        return None
    w = float(m.group(1))
    # A pavement wider than a motorway or narrower than a kerbstone is a typo.
    return w if 0.2 <= w <= 40.0 else None


# How a crossing is PAINTED, decided from the two tags that describe the paint
# and nothing else. Returned as a string that goes straight into the JSON, so
# the rule lives here (where the raw tags are) and the C emitter only switches
# on four known values instead of re-deriving them from a tag soup.
#
#   marked     paint a zebra. `crossing:markings` is the modern, explicit key
#              and WINS over `crossing` when both are present: a mapper who
#              wrote markings=no on a crossing=marked way is correcting the
#              older tag, which is exactly why the newer key was introduced.
#              `uncontrolled` is the legacy spelling of "marked, no signals".
#   signals    signal-controlled, no marking stated. The lamp already comes
#              from SIGNALS.JSON; this gets a stop bar, not a zebra.
#   unmarked   explicitly NOT painted. Nothing is drawn.
#   unknown    highway=crossing with no paint tag at all -- 309 of La Plata's
#              583 crossing nodes. Deliberately NOT folded into `marked`:
#              "most crossings in this city are painted" is a true statement
#              about Argentina and not a fact in the data, and the whole point
#              of this round is to replace the generator's own rule with real
#              data. The C side carries a knob to opt them in.
def _crossing_paint(t: dict) -> str:
    c = (t.get("crossing") or "").strip().lower()
    m = (t.get("crossing:markings") or "").strip().lower()
    if m:
        return "unmarked" if m in ("no", "none") else "marked"
    if c in ("marked", "zebra", "uncontrolled"):
        return "marked"
    if c == "unmarked":
        return "unmarked"
    if c in ("traffic_signals", "signals"):
        return "signals"
    return "unknown"


def convert_osm(osm: dict, proj: LocalProjection,
                levels_estimator: int = 2) -> dict:
    """Split one Overpass response into the cache's vector layers, in world units.

    GATE 2 of the tag path -- see docs/plans/GEO_TAG_AUDIT.md. Every layer now
    carries a `tags` sub-object with the raw OSM values (the *_TAG_KEYS tables
    above) ALONGSIDE the derived fields the C readers already use. The derived
    fields keep their names, types and meanings exactly, so a reader built
    against the old schema is unaffected and a new reader can ask for a tag
    without the normaliser having to decide first what the tag is for.
    """
    roads: list[dict] = []
    footways: list[dict] = []
    buildings: list[dict] = []
    areas: list[dict] = []
    signals: list[dict] = []
    nodes: list[dict] = []
    water: list[dict] = []
    counts: dict = {}
    lm_src: dict[str, int] = {}
    ignored_sample: list[dict] = []

    def bump(k: str) -> None:
        counts[k] = counts.get(k, 0) + 1

    for el in osm.get("elements", []):
        t = _tags(el)
        if el.get("type") == "node":
            hw = t.get("highway")
            # WHAT THIS NODE IS, in one field, so a reader does not re-derive
            # it from a tag soup. `traffic_signals` keeps its own kind because
            # SIGNALS.JSON's membership must not change -- see below.
            kind = (hw if hw in ("traffic_signals", "crossing", "stop",
                                 "give_way", "mini_roundabout", "turning_circle",
                                 "speed_camera", "traffic_mirror",
                                 # Round 1011 C4. 370 of La Plata's 374
                                 # `node_ignored` were bus stops -- the query
                                 # has asked for bare node["highway"] since the
                                 # 2026-10-07 widening, so they were fetched and
                                 # then refused one line later. Their
                                 # shelter/bench/bin tags arrive free with them.
                                 "bus_stop", "street_lamp")
                    else ("traffic_calming" if t.get("traffic_calming")
                          else None))
            if kind is None:
                bump("node_ignored")
                continue
            x, z = proj.to_world(el["lat"], el["lon"])
            rec = {
                "id": el.get("id"),
                "kind": kind,
                "x": round(x, 2), "z": round(z, 2),
                "lat": round(el["lat"], 7), "lon": round(el["lon"], 7),
                "tags": _keep_tags(t, NODE_TAG_KEYS),
            }
            # SIGNALS.JSON STAYS TRAFFIC-SIGNALS-ONLY. The query now also pulls
            # crossings, stop lines and humps (0 of 566 La Plata nodes before,
            # because it never asked), and td5_geo_signals.c emits a LAMP MAST
            # at every entry of `signals[]` -- so folding them in would grow a
            # traffic light on every zebra and move MODELS.DAT for a reason that
            # has nothing to do with tags. They go in a new `nodes[]` array,
            # which no reader reads yet, and `signals[]` has byte-identical
            # membership and field set to before plus `id`/`kind`/`lat`/`lon`.
            if kind == "traffic_signals":
                rec["direction"] = t.get("traffic_signals:direction")
                signals.append(rec)
                bump("signals")
                continue
            if kind == "crossing":
                # PROMOTED to fields, for the same reason the building civic
                # keys were in round 1007: `paint` is what the emitter switches
                # on, and a reader should not have to know the rule lives one
                # level down in `tags` -- nor re-implement it, which is how two
                # spellings of one fact start.
                rec["paint"] = _crossing_paint(t)
                rec["crossing"] = t.get("crossing")
                rec["crossing_markings"] = t.get("crossing:markings")
                rec["tactile_paving"] = t.get("tactile_paving")
                bump("crossing_paint_" + rec["paint"])
            elif kind == "bus_stop":
                # The street-furniture cluster, as BOOLS, because "is there a
                # shelter" is the only question the emitter asks. OSM spells the
                # negative explicitly (`shelter=no` is 0 here but common
                # elsewhere), so a bare truthiness test would build a shelter
                # where a mapper surveyed that there is none.
                rec["shelter"] = _yes_tag(t, "shelter")
                rec["bench"] = _yes_tag(t, "bench")
                rec["bin"] = _yes_tag(t, "bin")
                rec["lit"] = _yes_tag(t, "lit")
                rec["public_transport"] = t.get("public_transport")
                for f in ("shelter", "bench", "bin"):
                    if rec[f]:
                        bump("bus_stop_" + f)
            nodes.append(rec)
            bump("nodes_" + kind)
            continue
        if el.get("type") != "way":
            # Relations are not requested (see OVERPASS_QL) and would need ring
            # assembly; counted rather than silently skipped so a future cache
            # that does carry them is visible in PLACE.JSON.
            bump("non_way_" + str(el.get("type")))
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
                if hw not in HIGHWAY_PEDESTRIAN:
                    bump("highway_nondrivable")
                    continue
                # A MAPPED PAVEMENT, not a road. It goes to its own layer and
                # NEVER to `roads`, so ROADS.JSON, the router, the median
                # detector and every drivable reader are untouched: the
                # non-drivable test above still rejects it before any of that.
                sub = t.get("footway")
                footways.append({
                    "id": el.get("id"),
                    "name": t.get("name"),
                    # `class` is the OSM highway value (footway / steps /
                    # cycleway / path / pedestrian) and `footway` the subtype
                    # (sidewalk / crossing / traffic_island). Both, because
                    # they answer different questions: `class` is what the
                    # surface is, `footway` is what it is FOR, and only the
                    # pairing distinguishes a mapped kerbside pavement
                    # (footway + sidewalk) from a zebra (footway + crossing).
                    "class": hw,
                    "footway": sub,
                    # Pre-resolved so no reader has to repeat the pairing.
                    # SUBTYPE FIRST, then the highway value. A way tagged
                    # footway=sidewalk or =crossing is that thing whatever its
                    # highway value says; everything else falls back to its
                    # class. The untagged majority stays `footway` rather than
                    # being called a `path`: 621 of La Plata's 737 footways
                    # carry no subtype, and they are ordinary city pavement,
                    # not the 26 countryside ways that really are highway=path.
                    "kind": ("sidewalk" if sub == "sidewalk"
                             else "crossing" if sub == "crossing"
                             else hw),
                    "paint": (_crossing_paint(t) if sub == "crossing" else None),
                    "width": t.get("width"),
                    # PARSED, unlike the road layer's `width`. A footway width
                    # is the ONE measured per-side pavement figure the
                    # 2026-10-08 audit went looking for and could not find:
                    # `sidewalk:width` is absent on all 2291 La Plata ways,
                    # while 27 pedestrian ways carry a plain `width`.
                    "width_m": _width_m(t),
                    "surface": t.get("surface"),
                    "lit": _yes_tag(t, "lit"),
                    "covered": _yes_tag(t, "covered"),
                    "bridge": bool(t.get("bridge")),
                    "tunnel": bool(t.get("tunnel")),
                    "layer": _int_tag(t, "layer"),
                    "step_count": _int_tag(t, "step_count"),
                    "area": _yes_tag(t, "area"),
                    "closed": closed,
                    "points": wpts,
                    "latlon": [[round(la, 7), round(lo, 7)] for la, lo in ll],
                    "tags": _keep_tags(t, FOOTWAY_TAG_KEYS),
                })
                bump("footways")
                bump("footway_" + footways[-1]["kind"])
                continue
            lanes, lanes_src = _lanes_for(t)
            junction = t.get("junction")
            roads.append({
                "id": el.get("id"),
                "name": t.get("name"),
                "class": hw,
                "lanes": lanes,
                "lanes_src": lanes_src,
                "oneway": t.get("oneway") in ("yes", "1", "-1", "true"),
                # The DIRECTION the old bool threw away. `oneway` above keeps
                # its exact old value so nothing that reads it changes.
                "oneway_dir": _oneway_dir(t),
                "surface": t.get("surface"),
                "bridge": bool(t.get("bridge")),
                "tunnel": bool(t.get("tunnel")),
                # The VALUES, because they are not all equivalent:
                # `tunnel=building_passage` is an archway, not a bore, and
                # `bridge=viaduct` is a structure a deck page should depict.
                "bridge_kind": t.get("bridge"),
                "tunnel_kind": t.get("tunnel"),
                "covered": bool(t.get("covered")
                                and t.get("covered") not in ("no", "false")),
                "layer": _int_tag(t, "layer"),
                # DEAD CODE FIXED. This read
                #   ... or t.get("junction") == "roundabout" and False
                # and `and` binds tighter than `or`, so the whole third clause
                # was the constant False: `median` was False on all 2291 La
                # Plata ways and no roundabout could ever set it. The roundabout
                # is not a median anyway -- it is its own geometry -- so it gets
                # its own field and the median keeps only the two tags that
                # really mean "divided carriageway".
                "median": (t.get("dual_carriageway") == "yes"
                           or t.get("divider") is not None),
                "junction": junction,
                "roundabout": junction in ("roundabout", "circular"),
                "sidewalk": t.get("sidewalk"),
                "width": t.get("width"),
                "maxspeed": t.get("maxspeed"),
                "points": wpts,
                "latlon": [[round(la, 7), round(lo, 7)] for la, lo in ll],
                "tags": _keep_tags(t, ROAD_TAG_KEYS),
            })
            bump("roads")
            if junction:
                bump("road_junction_tagged")
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
            is_lm, lm_why = landmark_decide(t)
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
                "roof_levels": t.get("roof:levels"),
                "min_height_m": t.get("min_height"),
                "levels": t.get("building:levels"),
                "min_level": t.get("building:min_level"),
                "colour": t.get("building:colour"),
                "material": t.get("building:material"),
                "roof_material": t.get("roof:material"),
                "roof_colour": t.get("roof:colour"),
                "layer": _int_tag(t, "layer"),
                # The civic keys the close-out named, promoted to FIELDS and not
                # just left in `tags`, because they are what the landmark rule
                # and the height estimator read and a reader should not have to
                # know they live one level down.
                "amenity": t.get("amenity"),
                "office": t.get("office"),
                "government": t.get("government"),
                "tourism": t.get("tourism"),
                "historic": t.get("historic"),
                "heritage": t.get("heritage"),
                "man_made": t.get("man_made"),
                "religion": t.get("religion"),
                "denomination": t.get("denomination"),
                "leisure": t.get("leisure"),
                "shop": t.get("shop"),
                "healthcare": t.get("healthcare"),
                "landmark": is_lm,
                # WHICH TAG DECIDED. Without this "22 landmarks" cannot be
                # told from "22 hostels" -- the old blanket `tourism or
                # historic` really did promote a boutique hostel.
                "landmark_src": lm_why,
                "points": wpts,
                "tags": _keep_tags(t, BUILDING_TAG_KEYS),
            })
            bump("buildings")
            if is_lm:
                bump("landmarks")
                key = lm_why.split("=", 1)[0]
                lm_src[key] = lm_src.get(key, 0) + 1
            continue

        leisure = t.get("leisure")
        landuse = t.get("landuse")
        natural = t.get("natural")
        waterway = t.get("waterway")

        if natural in ("water", "coastline") or waterway or landuse == "reservoir":
            # A CULVERTED ditch runs under the ground. 9 of La Plata's
            # non-highway ways carry `tunnel` and 12 carry `layer`, and water
            # painted on the surface above a culvert is a river through a
            # street. Carried as a field so the rasteriser (and a future
            # reader) can refuse it; the `kind`/`closed`/`points` contract the
            # sea labelling keys off is untouched.
            water.append({
                "id": el.get("id"),
                "name": t.get("name"),
                "kind": natural or waterway or landuse,
                "closed": closed,
                "layer": _int_tag(t, "layer"),
                # `tunnel=no` and `covered=no` are explicit NEGATIVES and OSM
                # does carry them, so a bare truthiness test would culvert an
                # open ditch.
                "culvert": (t.get("tunnel") not in (None, "", "no", "false")
                            or t.get("covered") not in (None, "", "no", "false")),
                "intermittent": t.get("intermittent") in ("yes", "1", "true"),
                "width": t.get("width"),
                "points": wpts,
                "tags": _keep_tags(t, WATER_TAG_KEYS),
            })
            bump("water")
            continue

        if (leisure in AREA_KEEP_LEISURE or landuse in AREA_KEEP_LANDUSE
                or natural in AREA_KEEP_NATURAL
                or t.get("amenity") in AREA_KEEP_AMENITY):
            areas.append({
                "id": el.get("id"),
                "name": t.get("name"),
                # `kind` is still ONE value and still the key _OSM_TO_COVER is
                # looked up by, so the rasteriser is unchanged. The precedence
                # leisure > landuse > natural is the sharpest-first order: a
                # mapper who drew `leisure=park` over `natural=wood` meant the
                # park, and `natural` is the broad-brush fallback. `amenity`
                # sits last because a school ground tagged `leisure=pitch` is
                # a pitch first and a school second.
                "kind": (leisure or landuse or natural
                         or (t.get("amenity")
                             if t.get("amenity") in AREA_KEEP_AMENITY else None)),
                "leisure": leisure,
                "landuse": landuse,
                "natural": natural,
                "sport": t.get("sport"),
                "surface": t.get("surface"),
                "amenity": t.get("amenity"),
                "tourism": t.get("tourism"),
                "historic": t.get("historic"),
                "man_made": t.get("man_made"),
                "barrier": t.get("barrier"),
                "access": t.get("access"),
                "layer": _int_tag(t, "layer"),
                "closed": closed,
                "points": wpts,
                "tags": _keep_tags(t, AREA_TAG_KEYS),
            })
            bump("areas")
            continue
        # What is left after a whitelist that now covers every _OSM_TO_COVER
        # row. Recorded WITH ITS TAGS (capped) rather than as a bare count, so
        # the next place's residue is a list to read instead of a number to
        # wonder about -- the old `ignored: 22` is what hid leisure=track and
        # landuse=education for a week.
        bump("ignored")
        ignored_sample.append(
            {k: v for k, v in sorted(t.items())
             if k in ("leisure", "landuse", "natural", "amenity", "barrier",
                      "man_made", "place", "tourism", "historic", "railway",
                      "power", "aeroway", "military")})

    if lm_src:
        counts["landmark_by_tag"] = lm_src
    if ignored_sample:
        # Distinct tag signatures, most common first, capped: a sentence for
        # the operator, not a dump of 22 near-identical dicts.
        sig: dict[str, int] = {}
        for d in ignored_sample:
            k = ",".join("%s=%s" % kv for kv in sorted(d.items())) or "(untagged)"
            sig[k] = sig.get(k, 0) + 1
        counts["ignored_kinds"] = dict(sorted(sig.items(), key=lambda kv: -kv[1])[:24])

    return {"roads": roads, "footways": footways, "buildings": buildings,
            "areas": areas, "signals": signals, "nodes": nodes, "water": water,
            "counts": counts, "levels_estimator": levels_estimator}


# COVER class per OSM tag, used to bootstrap COVER.R8 before ESA WorldCover is
# wired in. OSM is sharper than 10 m where it is tagged, and WorldCover's job is
# to fill what OSM leaves blank -- so this ordering survives that addition.
#
# 2026-10-07: `wood`, `farmland`, `quarry` and `brownfield` had rows here that
# NOTHING COULD REACH, because the areas whitelist below (the only caller) did
# not accept them -- `wood` is a `natural` value and the branch only consulted
# `natural` for water, and the other three were simply missing from the landuse
# list. Reachability is now a property of AREA_KEEP_* rather than of two lists
# that had drifted apart: every key here appears there, asserted at import.
_OSM_TO_COVER = {
    "forest": COVER_TREE, "wood": COVER_TREE, "orchard": COVER_TREE,
    "tree_row": COVER_TREE, "scrub": COVER_SHRUB, "heath": COVER_SHRUB,
    "grass": COVER_GRASS, "village_green": COVER_GRASS, "meadow": COVER_GRASS,
    "park": COVER_GRASS, "garden": COVER_GRASS, "pitch": COVER_GRASS,
    "playground": COVER_GRASS, "common": COVER_GRASS, "dog_park": COVER_GRASS,
    "recreation_ground": COVER_GRASS, "cemetery": COVER_GRASS,
    "track": COVER_GRASS, "golf_course": COVER_GRASS, "grassland": COVER_GRASS,
    "farmland": COVER_CROP, "allotments": COVER_CROP, "vineyard": COVER_CROP,
    "farmyard": COVER_CROP, "plant_nursery": COVER_CROP, "greenhouse_horticulture": COVER_CROP,
    "residential": COVER_BUILT, "commercial": COVER_BUILT,
    "retail": COVER_BUILT, "industrial": COVER_BUILT,
    "education": COVER_BUILT, "institutional": COVER_BUILT,
    # Institutional PRECINCTS mapped as an amenity area rather than a landuse.
    # BUILT, and all but inert: WorldCover already reads these as built-up, so
    # the OSM outline sharpens an edge instead of inventing a class.
    "school": COVER_BUILT, "university": COVER_BUILT, "college": COVER_BUILT,
    "hospital": COVER_BUILT, "prison": COVER_BUILT, "parking": COVER_BUILT,
    # A sports ground is PITCHES AND GRASS, not built-up. MEASURED: mapping
    # these to BUILT painted 53142 cells of La Plata (two club grounds plus
    # the stadium, 0.65 km2) as built-up, which is both wrong and the single
    # largest cover change of the tag round. GRASS also agrees with
    # geob_area_kind_of, which classes them PITCH -- a flat laid ground.
    "stadium": COVER_GRASS, "sports_centre": COVER_GRASS,
    "construction": COVER_BARE, "quarry": COVER_BARE, "brownfield": COVER_BARE,
    "landfill": COVER_BARE, "sand": COVER_BARE,
    "beach": COVER_BARE, "bare_rock": COVER_BARE, "scree": COVER_BARE,
    "shingle": COVER_BARE, "mud": COVER_BARE,
    # `landuse=railway` is NOT here, deliberately. Three La Plata polygons
    # would have painted 41311 cells BARE, and 19562 of those cells were
    # WorldCover BUILT and 7898 TREE -- the OSM outline covers a whole station
    # precinct, buildings and trees included, so reading it as ballast
    # OVERRIDES A MEASUREMENT WITH A GUESS over half a square kilometre. The
    # 10 m classifier already knows what is there.
    "glacier": COVER_SNOW,
    "wetland": COVER_WETLAND, "marsh": COVER_WETLAND, "swamp": COVER_WETLAND,
    "swimming_pool": COVER_WATER, "basin": COVER_WATER,
}

# GATE 2's area whitelist, as three sets instead of a literal tuple inside the
# branch. A way that matches none of these is `bump("ignored")`.
#
# WIDENED 2026-10-07 to everything `_OSM_TO_COVER` can colour plus the
# leisure/landuse values La Plata actually carries and the old list refused:
# `leisure=track` x7, `swimming_pool` x2, `sports_centre` x2, `stadium`,
# `bleachers`, `fitness_station`, and `landuse=education` x4 / `railway` x3 /
# `plant_nursery` x1. Those 22 ways were the whole of the cache's `ignored`
# count, so the layer is now lossless on this place by construction.
AREA_KEEP_LEISURE = frozenset((
    "park", "garden", "pitch", "playground", "common", "recreation_ground",
    "dog_park", "track", "stadium", "sports_centre", "sports_hall",
    "swimming_pool", "bleachers", "fitness_station", "fitness_centre",
    "golf_course", "nature_reserve", "marina", "water_park", "picnic_site",
))
AREA_KEEP_LANDUSE = frozenset((
    "grass", "village_green", "meadow", "forest", "cemetery", "allotments",
    "orchard", "vineyard", "residential", "commercial", "retail",
    "industrial", "construction", "farmland", "farmyard", "quarry",
    "brownfield", "greenfield", "landfill", "education", "institutional",
    "religious", "military", "plant_nursery",
    "greenhouse_horticulture", "recreation_ground", "grassland", "basin",
))
# An institutional PRECINCT mapped as an amenity area rather than a landuse.
# The widened query made these visible for the first time -- 36 school grounds,
# 34 car parks, 14 hospital and 9 university precincts at La Plata, which were
# the bulk of the 135 `ignored` ways the first re-fetch reported.
AREA_KEEP_AMENITY = frozenset((
    "school", "university", "college", "hospital", "prison", "parking",
))
# `natural` as an AREA. water and coastline are NOT here: they are claimed by
# the water branch above this one and must stay there, because the sea
# labelling keys off `kind == "coastline"`.
AREA_KEEP_NATURAL = frozenset((
    "wood", "tree_row", "scrub", "heath", "grassland", "sand", "beach",
    "bare_rock", "scree", "shingle", "mud", "wetland", "marsh", "swamp",
    "glacier", "cliff", "ridge", "peak", "valley",
))

# A row in _OSM_TO_COVER that no whitelist accepts is dead code, which is the
# bug this round found. Asserted at import so it cannot come back.
_UNREACHABLE_COVER = sorted(set(_OSM_TO_COVER)
                            - AREA_KEEP_LEISURE - AREA_KEEP_LANDUSE
                            - AREA_KEEP_NATURAL - AREA_KEEP_AMENITY)
assert not _UNREACHABLE_COVER, (
    "_OSM_TO_COVER rows no AREA_KEEP_* set can reach: %s" % _UNREACHABLE_COVER)


# ---------------------------------------------------------------- land cover
# ESA WorldCover 2021 v200 (10 m, CC-BY 4.0) and Meta/WRI Global Canopy Height
# (1.2 m, CC-BY 4.0), both cloud-optimised GeoTIFFs on AWS S3, read by WINDOW
# through geo_cog (HTTP Range, cached under the place's _cache). Network only
# when the cache is cold, like every other layer.
WORLDCOVER_URL = ("https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/"
                  "map/ESA_WorldCover_10m_2021_v200_%s_Map.tif")
CANOPY_URL = ("https://dataforgood-fb-data.s3.amazonaws.com/forests/v1/"
              "alsgedi_global_v6_float/chm/%s.tif")
CANOPY_ZOOM = 9


def _grid_latlon(proj: LocalProjection, height: Raster):
    """lat/lon of every cell centre. The local projection is affine in the
    world frame (equirectangular about the centre + rotation + offset), so
    three exact samples give the whole grid."""
    h, w = height.data.shape
    ox, oz, c = height.origin_x, height.origin_z, height.cell
    la0, lo0 = proj.world_to_latlon(ox, oz)
    la1, lo1 = proj.world_to_latlon(ox + c, oz)
    la2, lo2 = proj.world_to_latlon(ox, oz + c)
    ix = np.arange(w, dtype=np.float64)[None, :]
    iz = np.arange(h, dtype=np.float64)[:, None]
    lat = la0 + (la1 - la0) * ix + (la2 - la0) * iz
    lon = lo0 + (lo1 - lo0) * ix + (lo2 - lo0) * iz
    return lat, lon


def _worldcover_tile(lat: float, lon: float) -> str:
    la = int(math.floor(lat / 3.0) * 3)
    lo = int(math.floor(lon / 3.0) * 3)
    return "%s%02d%s%03d" % ("S" if la < 0 else "N", abs(la),
                             "W" if lo < 0 else "E", abs(lo))


def worldcover_grid(lat, lon, cache_dir: str) -> tuple:
    """ESA WorldCover class id per cell (nearest 10 m pixel), 0 where unread."""
    import geo_cog
    out = np.zeros(lat.shape, dtype=np.uint8)
    names = np.vectorize(_worldcover_tile)(np.round(lat, 6), np.round(lon, 6))
    tiles = sorted(set(names.ravel().tolist()))
    pulled = 0
    for t in tiles:
        cog = geo_cog.RemoteCOG(WORLDCOVER_URL % t, cache_dir)
        sel = names == t
        px, py = cog.xy_to_pixel(lon[sel], lat[sel])
        px = np.floor(px).astype(np.int64)
        py = np.floor(py).astype(np.int64)
        x0, x1, y0, y1 = int(px.min()), int(px.max()) + 1, int(py.min()), int(py.max()) + 1
        win = cog.read_window(x0, y0, x1, y1)
        out[sel] = win[np.clip(py - y0, 0, win.shape[0] - 1),
                       np.clip(px - x0, 0, win.shape[1] - 1)]
        pulled += win.size
    return out, {"tiles": tiles, "pixels_read": int(pulled)}


def _quadkey(lat: float, lon: float, z: int) -> str:
    s_ = math.sin(math.radians(lat))
    x = (lon + 180.0) / 360.0
    y = 0.5 - math.log((1 + s_) / (1 - s_)) / (4 * math.pi)
    n = 1 << z
    tx, ty = min(n - 1, int(x * n)), min(n - 1, int(y * n))
    q = ""
    for i in range(z, 0, -1):
        m = 1 << (i - 1)
        q += str((1 if tx & m else 0) + (2 if ty & m else 0))
    return q


def canopy_grid(lat, lon, cache_dir: str) -> tuple:
    """Canopy height in metres per cell: the MAX of the ~3x3 1.2 m pixels the
    3.49 m cell covers, so a single crown is not averaged away."""
    import geo_cog
    from scipy import ndimage
    out = np.zeros(lat.shape, dtype=np.uint8)
    names = np.vectorize(lambda a, b: _quadkey(a, b, CANOPY_ZOOM))(
        np.round(lat, 6), np.round(lon, 6))
    keys = sorted(set(names.ravel().tolist()))
    pulled = 0
    R = 6378137.0
    for k in keys:
        cog = geo_cog.RemoteCOG(CANOPY_URL % k, cache_dir)
        sel = names == k
        mx = np.radians(lon[sel]) * R
        my = np.log(np.tan(np.pi / 4 + np.radians(lat[sel]) / 2)) * R
        px, py = cog.xy_to_pixel(mx, my)
        px = np.floor(px).astype(np.int64)
        py = np.floor(py).astype(np.int64)
        x0, x1 = int(px.min()) - 2, int(px.max()) + 3
        y0, y1 = int(py.min()) - 2, int(py.max()) + 3
        win = cog.read_window(x0, y0, x1, y1)
        win = ndimage.maximum_filter(win, size=3)
        out[sel] = win[np.clip(py - max(0, y0), 0, win.shape[0] - 1),
                       np.clip(px - max(0, x0), 0, win.shape[1] - 1)]
        pulled += win.size
    return out, {"quadkeys": keys, "pixels_read": int(pulled)}


def rasterize_layers(vec: dict, proj: LocalProjection, height: Raster,
                     bbox: tuple | None = None,
                     base_cover: "np.ndarray | None" = None,
                     ) -> tuple[Raster, Raster, dict]:
    """COVER.R8 and WATER.R8 on the SAME grid as HEIGHT.R16.

    Sharing the grid exactly is what lets td5_tg_world.c sample all of them with
    one index computation, and removes any chance of the water mask being offset
    from the terrain it is masking.
    """
    h, w = height.data.shape
    # WorldCover, when wired, is the BASE; OSM areas and buildings are painted
    # over it below (an OSM park outline is sharper than a 10 m classifier).
    cover = (base_cover.copy() if base_cover is not None
             else np.full((h, w), COVER_NONE, dtype=np.uint8))
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

    painted["water_culverted"] = 0
    for wy in vec["water"]:
        # A CULVERTED watercourse runs UNDER the ground: `waterway=ditch` with
        # `tunnel=culvert` is a pipe, and painting water on the surface above it
        # is a river through a street. 9 of La Plata's non-highway ways carry a
        # tunnel tag. Refused here rather than in the road-buffer pass below,
        # because that pass only clears water near a mapped ROAD and a culvert
        # under open ground would survive it.
        if wy.get("culvert"):
            painted["water_culverted"] += 1
            continue
        if wy["closed"]:
            painted["water_cells"] += fill_polygon(
                wy["points"], lambda ix, iz: (water.__setitem__((iz, ix), 1),
                                              cover.__setitem__((iz, ix), COVER_WATER))[0])
        else:
            for ix, iz in cells_of(wy["points"]):
                water[iz, ix] = 1
                cover[iz, ix] = COVER_WATER

    # SEA FROM COASTLINE (2026-09-30, Valparaiso). natural=coastline is a LINE in
    # OSM, never a polygon, so the loop above only stamped its outline and the
    # open sea stayed dry: the first Valparaiso fetch painted 6211 water cells
    # out of 1.8 M in a box that is half ocean. Rasterise every coastline as a
    # barrier, label the regions it cuts the box into, and mark as sea each
    # region that (a) touches the coast and (b) sits below sea level on the DEM
    # (most cells under SEA_LOW_M). (b) is what makes this independent of the
    # coastline's winding and of the frame's axis handedness, and it refuses a
    # land region that a gap in the coast would otherwise flood.
    coast = [wy for wy in vec["water"] if wy["kind"] == "coastline"]
    painted["sea_cells"] = 0
    if coast:
        from scipy import ndimage
        barrier = np.zeros((h, w), dtype=bool)
        step = height.cell * 0.5
        for wy in coast:
            pts = wy["points"]
            for a, b in zip(pts, pts[1:]):
                dx, dz = b["x"] - a["x"], b["z"] - a["z"]
                n = max(1, int(math.hypot(dx, dz) / step))
                for k in range(n + 1):
                    x = a["x"] + dx * k / n
                    z = a["z"] + dz * k / n
                    ix = int(round((x - height.origin_x) / height.cell))
                    iz = int(round((z - height.origin_z) / height.cell))
                    if 0 <= ix < w and 0 <= iz < h:
                        barrier[iz, ix] = True
        # Only the Overpass query box is KNOWN: the grid is axis-aligned in the
        # route's rotated frame, so it overhangs the lat/lon box at the corners,
        # and there the coastline simply is not in the data. Labelling across
        # that overhang joined sea and land into one region on the first
        # route-frame Valparaiso build (14 regions, 8 sea cells). Label inside
        # the box; the overhang is filled from the DEM below.
        inside = np.ones((h, w), dtype=bool)
        if bbox is not None:
            s_, w_, n_, e_ = bbox
            quad = [proj.to_world(la, lo) for la, lo in
                    ((s_, w_), (s_, e_), (n_, e_), (n_, w_))]
            gx = height.origin_x + np.arange(w) * height.cell
            gz = height.origin_z + np.arange(h) * height.cell
            X, Z = np.meshgrid(gx, gz)
            pos = np.ones((h, w), dtype=bool)
            neg = np.ones((h, w), dtype=bool)
            for (ax, az), (bx, bz) in zip(quad, quad[1:] + quad[:1]):
                c = (bx - ax) * (Z - az) - (bz - az) * (X - ax)
                pos &= c >= 0
                neg &= c <= 0
            inside = pos | neg              # same side of all four edges
        labels, nlab = ndimage.label(~barrier & inside)
        touching = np.unique(np.concatenate([
            labels[np.roll(barrier, s_, axis=ax)] for ax in (0, 1) for s_ in (1, -1)]))
        metres = (height.data.astype(np.float64) * height.scale + height.bias) \
            / proj.units_per_metre
        lows = ndimage.mean((metres < SEA_LOW_M).astype(np.float64), labels,
                            index=np.arange(1, nlab + 1))
        sea = np.zeros((h, w), dtype=bool)
        kept, refused = 0, 0
        for lab in touching:
            if lab <= 0:
                continue
            if lows[lab - 1] > SEA_LOW_FRAC_MIN:
                sea |= labels == lab
                kept += 1
            else:
                refused += 1
        sea |= barrier & ndimage.binary_dilation(sea)
        # Overhang outside the query box: extend the sea through low cells
        # connected to it, DEM only (there is no vector data out there).
        if not inside.all() and sea.any():
            low = metres < SEA_LOW_M
            llab, _ = ndimage.label(low | sea)
            keep = np.unique(llab[sea])
            keep = keep[keep > 0]
            sea |= np.isin(llab, keep) & ~inside & low
        # Enclosed holes in the sea with no relief are sea too: the DEM reads a
        # little above SEA_LOW_M offshore in patches, and the first route-frame
        # Valparaiso build left a 1 km dry blob in the bay. A real island has
        # height, so the test is "almost every cell under SEA_HOLE_MAX_M".
        holes = ndimage.binary_fill_holes(sea) & ~sea
        if holes.any():
            hlab, hn = ndimage.label(holes)
            hlow = ndimage.mean((metres < SEA_HOLE_MAX_M).astype(np.float64),
                                hlab, index=np.arange(1, hn + 1))
            for i, f in enumerate(hlow, start=1):
                if f > 0.95:
                    sea |= hlab == i
        water[sea] = 1
        cover[sea] = COVER_WATER
        # Sink the sea floor to the clamp shelf under the water plane: the DEM's
        # near-shore sea reads +1..2 m, which would stand the "floor" above the
        # water surface across a 1.7 km band.
        floor_raw = int(round((SEA_FLOOR_CLAMP_M * proj.units_per_metre
                               - height.bias) / height.scale))
        floor_raw = max(-32000, min(32000, floor_raw))
        height.data[sea] = np.minimum(height.data[sea], floor_raw).astype(height.data.dtype)
        painted["sea_cells"] = int(sea.sum())
        painted["water_cells"] += painted["sea_cells"]
        print("  coastline: %d way(s), %d region(s), %d kept as sea, %d refused "
              "(%.0f%% or fewer cells under %.1f m), %d sea cells"
              % (len(coast), nlab, kept, refused, SEA_LOW_FRAC_MIN * 100, SEA_LOW_M,
                 painted["sea_cells"]))

    from scipy import ndimage
    step = height.cell * 0.5
    # A mapped road that is not a bridge stands on LAND -- for every kind of
    # water, sea or not. Along a waterfront
    # (Valparaiso's Av. Espana runs on the seawall) the coastline sits a few
    # metres off the carriageway, so the sea mask reached under the road's
    # own width and the generator got 23 "open spans over water" on dry
    # ground 9 m above the sea. Clear the sea from a buffer of half the
    # road's width plus SEA_ROAD_CLEAR_M around every non-bridge,
    # non-tunnel drivable way. Real bridges keep their water. The same
    # happened inland: Vina's street beside the Estero Marga-Marga canal
    # polygon put 23 route spans "over water" 11-16 m up.
    road = np.zeros((h, w), dtype=bool)
    cell_m = height.cell / proj.units_per_metre
    rads = {}
    for rd in vec["roads"]:
        if rd.get("bridge") or rd.get("tunnel"):
            continue
        try:                                  # OSM width is free text
            wd = float(str(rd.get("width") or "").split()[0].replace(",", "."))
        except (ValueError, IndexError):
            wd = 0.0
        try:
            ln = float(rd.get("lanes") or 2)
        except (TypeError, ValueError):
            ln = 2.0
        half_m = (wd if wd > 0.0 else ln * 3.5) * 0.5
        rc = max(1, int(math.ceil((half_m + SEA_ROAD_CLEAR_M) / cell_m)))
        mask = rads.setdefault(rc, np.zeros((h, w), dtype=bool))
        pts = rd["points"]
        for a, b in zip(pts, pts[1:]):
            dx, dz = b["x"] - a["x"], b["z"] - a["z"]
            n = max(1, int(math.hypot(dx, dz) / step))
            for k in range(n + 1):
                ix = int(round((a["x"] + dx * k / n - height.origin_x) / height.cell))
                iz = int(round((a["z"] + dz * k / n - height.origin_z) / height.cell))
                if 0 <= ix < w and 0 <= iz < h:
                    mask[iz, ix] = True
    for rc, mask in rads.items():
        yy, xx = np.mgrid[-rc:rc + 1, -rc:rc + 1]
        road |= ndimage.binary_dilation(mask, structure=(xx * xx + yy * yy) <= rc * rc)
    wet_now = water.astype(bool)
    dried = int((wet_now & road).sum())
    water[road] = 0
    cover[road & (cover == COVER_WATER)] = COVER_NONE
    painted["water_dried_under_roads"] = dried
    if dried:
        print("  water cleared under %d road cell(s) (non-bridge roads are on land)"
              % dried)

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


def estimate_building_heights(vec: dict, cover: Raster,
                              estimator: int = 2) -> dict:
    """Fill every untagged building's height, flagging it as estimated."""
    h, w = cover.data.shape
    built = (cover.data == COVER_BUILT)
    stats: dict = {"osm_height": 0, "osm_levels": 0, "estimated": 0,
                   "estimator": estimator}
    by_use: dict[str, int] = {}

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
        # The ESTIMATOR'S VIEW OF THE BUILDING. Before this round it was one
        # key -- `{"building": b["class"]}` -- so a footprint tagged only
        # `building=yes` plus `office=government` was indistinguishable from a
        # bare shed. The use tags are now in the record (see convert_osm), so
        # hand the estimator the whole thing.
        levels, src = _estimate_levels(
            {"building": b["class"], "amenity": b.get("amenity"),
             "office": b.get("office"), "government": b.get("government"),
             "healthcare": b.get("healthcare"), "historic": b.get("historic"),
             "tourism": b.get("tourism"), "man_made": b.get("man_made"),
             "leisure": b.get("leisure"), "shop": b.get("shop")},
            b["area_m2"], frac, estimator)
        b["levels_est"] = round(levels, 2)
        b["height_m"] = round(levels * STOREY_HEIGHT_M, 2)
        b["height_src"] = src
        b["built_frac"] = round(frac, 3)
        stats["estimated"] += 1
        if src.startswith("estimated_use:"):
            key = src.split(":", 1)[1]
            by_use[key] = by_use.get(key, 0) + 1
    if by_use:
        stats["by_use_tag"] = dict(sorted(by_use.items(),
                                          key=lambda kv: -kv[1])[:30])
    return stats


# ------------------------------------------------- extra building sources ---
#
# [ROUND 1012 D1] OSM has 2326 footprints for La Plata's 29 km2 box, and a facade
# within 25 m of only 3-9% of street sides. Overture's buildings theme (OSM +
# Esri + Microsoft + Google, already conflated by Overture) has the rest of the
# city, and Google Open Buildings 2.5D Temporal has a measured height for every
# pixel of it. geo_buildings_extra.py fetches both ONCE into _cache/; this is
# the offline half that merges them into BUILDINGS.JSON, so a re-normalise is
# free.
#
# FOOTPRINT RULE: OSM wins wherever it has the building. An Overture record is
# dropped as a duplicate when (a) Overture itself says its source is the OSM
# way we already hold, or (b) it overlaps an OSM footprint by
# IoU >= XB_DEDUPE_IOU, or covers XB_DEDUPE_COVER of the smaller of the two.
# (b) catches the Overture copy of an OSM way edited since Overture's
# snapshot, and an ML footprint Overture kept beside a slightly different OSM
# trace. Both thresholds are reported with the IoU histogram so a different
# choice is attributable.
#
# HEIGHT LADDER (first that answers wins, recorded in `height_src`):
#   osm_height / osm_levels    measured OSM tag (unchanged)
#   overture_height            Overture `height` (Esri / OSM 3D sources)
#   overture_floors            Overture `num_floors` x STOREY_HEIGHT_M
#   openbuildings_raster       median of the 2023 building_height raster over
#                              the footprint (>= XB_OB_MIN_M, floored to one
#                              storey)
#   estimated_*                the L1 estimator, as before
XB_DEDUPE_IOU = 0.30
XB_DEDUPE_COVER = 0.60
# Below this the raster is reading ground, a courtyard or a tree, not a roof:
# 4 m effective pixels blur a 1-storey house into its yard, so the median of a
# real house sits around 2.5-3.5 m and an empty lot sits near 0.
XB_OB_MIN_M = 2.0
# Above the dataset's own ceiling ("height relative to the terrain in range
# [0m, 100m]") a value is an artefact.
XB_OB_MAX_M = 100.0


def _xb_numeric_id(gers: str) -> int:
    """A stable NEGATIVE integer for an Overture GERS id. The C reader hashes a
    numeric `id` for page picks (geob_id_hash), OSM way ids are positive, so a
    negative 52-bit value can never collide with one and survives the double
    round trip exactly."""
    h = int(hashlib.sha256(gers.encode("utf-8")).hexdigest()[:13], 16)
    return -(h + 1)


def _xb_osm_way(sources) -> int | None:
    """The OSM way id Overture says this record came from, else None."""
    for s in sources or []:
        if (s or {}).get("dataset") == "OpenStreetMap":
            rid = (s.get("record_id") or "")
            if rid.startswith("w"):
                num = re.match(r"w(\d+)", rid)
                if num:
                    return int(num.group(1))
    return None


def _xb_dataset(sources) -> str:
    """Which upstream dataset Overture took the GEOMETRY from."""
    best = None
    for s in sources or []:
        s = s or {}
        prop = s.get("property") or ""
        if prop in ("", "/geometry", "geometry"):
            return s.get("dataset") or "unknown"
        best = best or s.get("dataset")
    return best or "unknown"


def _ob_grid(path: str):
    z = np.load(path)
    return (z["height"], float(z["north"]), float(z["west"]),
            float(z["dlat"]), float(z["dlon"]))


def _ob_median(grid, ring_ll) -> tuple[float | None, int]:
    """Median raster height over a lat/lon ring, and the sample count."""
    import shapely
    h, north, west, dlat, dlon = grid
    lats = [p[0] for p in ring_ll]
    lons = [p[1] for p in ring_ll]
    r0 = max(0, int(math.floor((north - max(lats)) / dlat)))
    r1 = min(h.shape[0] - 1, int(math.ceil((north - min(lats)) / dlat)))
    c0 = max(0, int(math.floor((min(lons) - west) / dlon)))
    c1 = min(h.shape[1] - 1, int(math.ceil((max(lons) - west) / dlon)))
    if r1 < r0 or c1 < c0:
        return None, 0
    poly = shapely.Polygon([(lo, la) for la, lo in ring_ll])
    if not poly.is_valid:
        poly = poly.buffer(0)
    rr, cc = np.mgrid[r0:r1 + 1, c0:c1 + 1]
    clat = north - rr * dlat
    clon = west + cc * dlon
    inside = shapely.contains_xy(poly, clon.ravel(), clat.ravel())
    vals = h[r0:r1 + 1, c0:c1 + 1].ravel()[inside]
    if vals.size == 0:
        # Smaller than a cell: the cell under the centroid.
        c = poly.centroid
        r = int(round((north - c.y) / dlat))
        k = int(round((c.x - west) / dlon))
        if 0 <= r < h.shape[0] and 0 <= k < h.shape[1]:
            vals = np.array([h[r, k]], np.float32)
    vals = vals[~np.isnan(vals)]
    if vals.size == 0:
        return None, 0
    return float(np.median(vals)), int(vals.size)


def conflate_extra_buildings(vec: dict, proj: LocalProjection, out: str,
                             bbox) -> dict | None:
    """Merge the cached Overture footprints and Open Buildings heights into
    vec["buildings"] in place. Offline: returns None (and changes nothing)
    when the cached products are missing. See the block comment above."""
    import geo_buildings_extra as xb
    import shapely
    from shapely.strtree import STRtree

    prod = xb.products(out, bbox)
    have_ov = prod["overture"] and os.path.exists(prod["overture"])
    have_ob = os.path.exists(prod["ob"])
    if not have_ov and not have_ob:
        return None
    stats: dict = {"osm_in": len(vec["buildings"])}

    # --- OSM footprints in metres (the dedupe frame) ---------------------
    osm_ll = []
    osm_polys = []
    osm_ids = {}
    for i, b in enumerate(vec["buildings"]):
        ll = [proj.world_to_latlon(p["x"], p["z"]) for p in b["points"]]
        osm_ll.append(ll)
        m = [proj.to_metres(la, lo) for la, lo in ll]
        poly = shapely.Polygon(m) if len(m) >= 3 else shapely.Polygon()
        if not poly.is_valid:
            poly = poly.buffer(0)
        osm_polys.append(poly)
        if b.get("id") is not None and not b.get("part"):
            osm_ids[b["id"]] = i
        b.setdefault("footprint_src", "osm")
    tree = STRtree(osm_polys)

    # --- Overture ---------------------------------------------------------
    added = []
    by_ds: dict[str, int] = {}
    ov_height_for_osm: dict[int, tuple[float, str]] = {}
    iou_hist = [0] * 10
    alt = {"iou>=0.10": 0, "iou>=0.30": 0, "iou>=0.50": 0}
    if have_ov:
        import pyarrow.parquet as pq
        rows = pq.read_table(prod["overture"]).to_pylist()
        stats["overture_rows"] = len(rows)
        n_osm_same = n_iou = n_under = n_degen = 0
        for r in rows:
            if r.get("is_underground"):
                n_under += 1
                continue
            h_ov, f_ov = r.get("height"), r.get("num_floors")
            src = None
            if h_ov and h_ov > 0:
                src = (float(h_ov), "overture_height")
            elif f_ov and f_ov > 0:
                src = (float(f_ov) * STOREY_HEIGHT_M, "overture_floors")
            way = _xb_osm_way(r.get("sources"))
            if way is not None and way in osm_ids:
                n_osm_same += 1
                if src:
                    ov_height_for_osm[osm_ids[way]] = src
                continue
            geom = shapely.from_wkb(r["geometry"])
            parts = list(getattr(geom, "geoms", [geom]))
            ds = _xb_dataset(r.get("sources"))
            for k, g in enumerate(parts):
                if g.geom_type != "Polygon" or g.is_empty:
                    n_degen += 1
                    continue
                ring = list(g.exterior.coords)       # (lon, lat)
                m = shapely.Polygon([proj.to_metres(la, lo) for lo, la in ring])
                if not m.is_valid:
                    m = m.buffer(0)
                if m.is_empty or m.area < 4.0:
                    n_degen += 1
                    continue
                best_iou, best_cov, best_i = 0.0, 0.0, -1
                for j in tree.query(m):
                    o = osm_polys[j]
                    inter = m.intersection(o).area
                    if inter <= 0.0:
                        continue
                    iou = inter / (m.area + o.area - inter)
                    cov = inter / min(m.area, o.area)
                    if iou > best_iou:
                        best_iou, best_i = iou, int(j)
                    best_cov = max(best_cov, cov)
                if best_iou > 0.0:
                    iou_hist[min(9, int(best_iou * 10))] += 1
                    for t in (0.10, 0.30, 0.50):
                        if best_iou >= t:
                            alt["iou>=%.2f" % t] += 1
                if best_iou >= XB_DEDUPE_IOU or best_cov >= XB_DEDUPE_COVER:
                    n_iou += 1
                    if src and best_i >= 0 and best_i not in ov_height_for_osm:
                        ov_height_for_osm[best_i] = src
                    continue
                wpts = []
                for lo, la in ring:
                    x, z = proj.to_world(la, lo)
                    wpts.append({"x": round(x, 1), "z": round(z, 1)})
                gid = r.get("id") or ""
                rec = {
                    "id": _xb_numeric_id(gid + ("#%d" % k if k else "")),
                    "gers_id": gid,
                    "name": None,
                    "class": r.get("class") or "yes",
                    "part": False,
                    "area_m2": round(m.area, 1),
                    "height_m": round(src[0], 2) if src else None,
                    "height_src": src[1] if src else None,
                    "footprint_src": "overture:" + ds,
                    "roof_shape": r.get("roof_shape"),
                    "roof_height_m": ("%g" % r["roof_height"]
                                      if r.get("roof_height") else None),
                    "min_height_m": ("%g" % r["min_height"]
                                     if r.get("min_height") else None),
                    "levels": ("%d" % f_ov if f_ov else None),
                    "colour": r.get("facade_color"),
                    "roof_colour": r.get("roof_color"),
                    "material": r.get("facade_material"),
                    "roof_material": r.get("roof_material"),
                    "layer": 0,
                    "landmark": False,
                    "landmark_src": None,
                    "points": wpts,
                    "_ll": [(la, lo) for lo, la in ring],
                }
                added.append(rec)
                by_ds[ds] = by_ds.get(ds, 0) + 1
        stats.update({
            "overture_same_osm_way": n_osm_same,
            "overture_dup_by_overlap": n_iou,
            "overture_underground": n_under,
            "overture_degenerate": n_degen,
            "overture_added": len(added),
            "overture_added_by_dataset": by_ds,
            "dedupe_rule": "same OSM way, or IoU >= %.2f, or cover >= %.2f"
                           % (XB_DEDUPE_IOU, XB_DEDUPE_COVER),
            "dedupe_best_iou_histogram": iou_hist,
            "dedupe_alt_thresholds": alt,
        })

    # --- heights ----------------------------------------------------------
    grid = _ob_grid(prod["ob"]) if have_ob else None
    hs: dict[str, int] = {}
    val = []                 # (measured OSM height, raster median) pairs
    for i, b in enumerate(vec["buildings"]):
        measured = b.get("height_src") in ("osm_height", "osm_levels")
        if grid is not None:
            med, npx = _ob_median(grid, osm_ll[i])
            if med is not None:
                b["ob_height_m"] = round(med, 2)
                b["ob_samples"] = npx
            if measured and med is not None:
                val.append((b["height_m"], med))
        if measured:
            continue
        if i in ov_height_for_osm:
            h, s = ov_height_for_osm[i]
            b["height_m"], b["height_src"] = round(h, 2), s
        elif b.get("ob_height_m") is not None and \
                XB_OB_MIN_M <= b["ob_height_m"] <= XB_OB_MAX_M:
            b["height_m"] = round(max(STOREY_HEIGHT_M, b["ob_height_m"]), 2)
            b["height_src"] = "openbuildings_raster"
        if b.get("height_src") and not b["height_src"].startswith("estimated"):
            b["levels_est"] = round(b["height_m"] / STOREY_HEIGHT_M, 2)
    for b in added:
        ll = b.pop("_ll")
        if grid is not None:
            med, npx = _ob_median(grid, ll)
            if med is not None:
                b["ob_height_m"] = round(med, 2)
                b["ob_samples"] = npx
        if b["height_m"] is None and b.get("ob_height_m") is not None and \
                XB_OB_MIN_M <= b["ob_height_m"] <= XB_OB_MAX_M:
            b["height_m"] = round(max(STOREY_HEIGHT_M, b["ob_height_m"]), 2)
            b["height_src"] = "openbuildings_raster"
        if b["height_m"] is not None:
            b["levels_est"] = round(b["height_m"] / STOREY_HEIGHT_M, 2)
    vec["buildings"].extend(added)
    for b in vec["buildings"]:
        k = b.get("height_src") or "(estimator)"
        hs[k] = hs.get(k, 0) + 1
    stats["height_src_before_estimator"] = hs
    stats["footprints"] = {"osm": stats["osm_in"],
                           "overture": len(added),
                           "total": len(vec["buildings"])}
    if val:
        a = np.array(val, np.float64)
        d = a[:, 1] - a[:, 0]
        stats["ob_vs_osm_measured"] = {
            "n": int(len(a)),
            "bias_m": round(float(d.mean()), 2),
            "mae_m": round(float(np.abs(d).mean()), 2),
            "median_abs_m": round(float(np.median(np.abs(d))), 2),
            "corr": round(float(np.corrcoef(a[:, 0], a[:, 1])[0, 1]), 3)
            if len(a) > 2 else None,
        }
    vec["counts"]["buildings"] = len(vec["buildings"])
    vec["counts"]["buildings_osm"] = stats["osm_in"]
    vec["counts"]["buildings_overture"] = len(added)
    return stats


# --------------------------------------------------------------------- main ---

def fetch_place(name: str, lat: float, lon: float, radius_m: float,
                root: str = CACHE_ROOT,
                units_per_metre: float = UNITS_PER_METRE_DERIVED,
                dem_override: str | None = None,
                cell: float = TG_WORLD_CELL,
                rotation_rad: float = 0.0,
                offset_x: float = 0.0, offset_z: float = 0.0,
                smooth_m: float = DEM_SMOOTH_M_DEFAULT,
                land_cover: str | None = None,
                canopy: bool | None = None,
                levels_estimator: int = 2,
                dem_probe: bool = True,
                extra_buildings: bool | None = None,
                extra_online: bool = False,
                extra_plan_only: bool = False,
                overture_release: str | None = None,
                extra_max_mb: float = 600.0) -> dict:
    slug = slugify(name)
    out = place_dir(slug, root)
    os.makedirs(out, exist_ok=True)
    # Land-cover choices are STICKY: a re-fetch that does not say (the
    # selector's route-frame pass) keeps what the place was built with.
    prev = {}
    if os.path.exists(os.path.join(out, "PLACE.JSON")):
        try:
            with open(os.path.join(out, "PLACE.JSON"), encoding="utf-8") as f:
                prev = json.load(f).get("sources", {})
        except Exception:                              # noqa: BLE001
            prev = {}
    if land_cover is None:
        land_cover = prev.get("land_cover", "osm")
    if canopy is None:
        canopy = bool(prev.get("canopy", False))
    if extra_buildings is None:
        extra_buildings = bool(prev.get("extra_buildings", False))

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
    # NOT CACHED (probe_ign_dem calls urlopen directly) and its result is not
    # written into PLACE.JSON, so on a re-fetch it is one outbound request that
    # buys nothing. --skip-dem-probe exists for a run with a fetch budget.
    if not dem_probe:
        print("  skipped (--skip-dem-probe): the probe is uncached and its "
              "result is not stored, so a re-fetch gains nothing from it")
    else:
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
    vec = convert_osm(osm, proj, levels_estimator)
    vec["roads"], clip = clip_roads(vec["roads"])
    if clip:
        vec["counts"]["roads"] = len(vec["roads"])
        vec["counts"].update(clip)
        print("  DENSE CENTRE: clipped %d drivable ways to %d (cap %d); dropped "
              "%s" % (clip["clipped_from"], len(vec["roads"]), clip["cap"],
                      ", ".join(clip["dropped_classes"]) or "nothing"))
    for k in sorted(vec["counts"]):
        print("  %-20s %s" % (k, vec["counts"][k]))
    if vec["counts"].get("landmark_by_tag"):
        print("  landmarks by deciding tag: %s"
              % ", ".join("%s x%d" % kv for kv in
                          sorted(vec["counts"]["landmark_by_tag"].items(),
                                 key=lambda kv: -kv[1])))

    xb_stats = None
    if extra_buildings:
        print("\n[3b] extra buildings (Overture + Open Buildings heights)")
        import geo_buildings_extra as xb
        xb_prov = xb.fetch_extra(out, bbox, extra_online, overture_release,
                                 int(extra_max_mb * 1024 * 1024),
                                 extra_plan_only)
        if extra_plan_only:
            print("  plan only: %s" % json.dumps(xb_prov))
            return {"plan": xb_prov}
        xb_stats = conflate_extra_buildings(vec, proj, out, bbox)
        if xb_stats is None:
            print("  no cached products; BUILDINGS.JSON stays OSM-only")
        else:
            xb_stats["provenance"] = {
                k: v for k, v in xb.products(out, bbox)["prov"].items()
                if k != "requests"}
            fp = xb_stats["footprints"]
            print("  footprints: %d OSM + %d Overture = %d"
                  % (fp["osm"], fp["overture"], fp["total"]))
            for k in ("overture_rows", "overture_same_osm_way",
                      "overture_dup_by_overlap", "overture_underground",
                      "overture_degenerate"):
                print("  %-26s %s" % (k, xb_stats.get(k)))
            print("  added by dataset: %s"
                  % xb_stats.get("overture_added_by_dataset"))
            print("  dedupe best-IoU histogram (0.0..1.0 by 0.1): %s"
                  % xb_stats.get("dedupe_best_iou_histogram"))
            print("  heights before estimator: %s"
                  % xb_stats["height_src_before_estimator"])
            if xb_stats.get("ob_vs_osm_measured"):
                print("  raster vs measured OSM heights: %s"
                      % xb_stats["ob_vs_osm_measured"])

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
    base_cover, wc_prov, canopy_r, cn_prov = None, None, None, None
    if land_cover == "worldcover" or canopy:
        glat, glon = _grid_latlon(proj, height)
        cache_dir = os.path.join(out, "_cache")
        if land_cover == "worldcover":
            base_cover, wc_prov = worldcover_grid(glat, glon, cache_dir)
            cls, cnt = np.unique(base_cover, return_counts=True)
            wc_prov["classes"] = {int(c): int(n) for c, n in zip(cls, cnt)}
            print("  WorldCover: tiles %s, classes %s"
                  % (",".join(wc_prov["tiles"]), wc_prov["classes"]))
        if canopy:
            cg, cn_prov = canopy_grid(glat, glon, cache_dir)
            canopy_r = Raster(cg, height.origin_x, height.origin_z, height.cell,
                              1.0, 0.0, 255, height.rotation_rad)
            cn_prov["cells_over_3m"] = int((cg >= 3).sum())
            cn_prov["max_m"] = int(cg.max())
            print("  canopy: quadkeys %s, %d cells >= 3 m, max %d m"
                  % (",".join(cn_prov["quadkeys"]), cn_prov["cells_over_3m"],
                     cn_prov["max_m"]))
        del glat, glon
    cover, water, painted = rasterize_layers(vec, proj, height, bbox, base_cover)
    bh = estimate_building_heights(vec, cover, levels_estimator)
    print("  cover cells %d, water cells %d, built stamps %d"
          % (painted["cover_cells"], painted["water_cells"],
             painted["built_cells"]))
    # `bh` carries a nested `by_use_tag` breakdown as well as the scalar
    # counts, so this cannot be a blanket %d -- it raised TypeError on the
    # first dry run.
    print("  building heights: %s"
          % ", ".join("%s=%s" % (k, v) for k, v in sorted(bh.items())
                      if not isinstance(v, dict)))
    if isinstance(bh.get("by_use_tag"), dict):
        print("  estimated from a use tag: %s"
              % ", ".join("%s x%d" % kv for kv in bh["by_use_tag"].items()))

    # ---- write the contract ---------------------------------------------
    height.write(os.path.join(out, "HEIGHT.R16"))
    cover.write(os.path.join(out, "COVER.R8"))
    if canopy_r is not None:
        canopy_r.write(os.path.join(out, "CANOPY.R8"))
    elif os.path.exists(os.path.join(out, "CANOPY.R8")):
        os.remove(os.path.join(out, "CANOPY.R8"))
    water.write(os.path.join(out, "WATER.R8"))
    write_json(os.path.join(out, "ROADS.JSON"), {"roads": vec["roads"]})
    # A SEPARATE FILE, not a section of ROADS.JSON, for the same reason
    # SIGNALS.JSON's `nodes[]` is separate from its `signals[]`: every existing
    # reader of ROADS.JSON treats `roads[]` as the DRIVABLE network, and the
    # router, the median detector and the mouth table would all have to learn a
    # new "ignore these" rule for a benefit of zero. A reader that wants
    # pavements opens a file named after them; one that does not never sees it.
    # A place fetched before schema 3 has no FOOTWAYS.JSON at all, which the C
    # reader treats as "this place has no mapped pavements".
    write_json(os.path.join(out, "FOOTWAYS.JSON"),
               {"footways": vec["footways"]})
    bdoc = {"buildings": vec["buildings"], "storey_height_m": STOREY_HEIGHT_M,
            "height_provenance": bh}
    if xb_stats is not None:
        # Optional, additive: every reader that predates round 1012 ignores it.
        bdoc["conflation"] = xb_stats
    write_json(os.path.join(out, "BUILDINGS.JSON"), bdoc)
    write_json(os.path.join(out, "AREAS.JSON"), {"areas": vec["areas"]})
    # `signals` keeps its exact old membership (highway=traffic_signals only);
    # `nodes` is the new array for the crossings / stop lines / humps the
    # widened query brings in. Separate keys rather than a `kind` filter the
    # reader has to apply, so an old reader cannot accidentally mast a zebra.
    write_json(os.path.join(out, "SIGNALS.JSON"),
               {"signals": vec["signals"], "nodes": vec["nodes"]})

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
            "buildings_extra": ({
                "source": "Overture Maps buildings theme (OSM + Esri Community "
                          "Maps + Microsoft ML + Google Open Buildings "
                          "footprints, conflated by Overture) + Google Open "
                          "Buildings 2.5D Temporal building_height",
                "overture_release": xb_stats["provenance"].get(
                    "overture", {}).get("release"),
                "open_buildings_year": xb_stats["provenance"].get(
                    "open_buildings", {}).get("year"),
                "licence": "ODbL 1.0 (Overture); ODbL 1.0 chosen of the "
                           "CC BY 4.0 / ODbL dual licence (Open Buildings)",
                "citation": xb_stats["provenance"].get(
                    "open_buildings", {}).get("citation"),
                "footprints": xb_stats["footprints"],
                "height_src": bh,
            } if xb_stats else {
                "source": "NOT WIRED for this place -- Overture buildings + "
                          "Open Buildings heights (--extra-buildings on)",
            }),
            "height": dem_prov,
            "cover": ({
                "source": "ESA WorldCover 10 m v200 + OSM landuse/leisure overlay",
                "vintage": "2021 (WorldCover), live OSM overlay",
                "licence": "CC-BY 4.0 (ESA WorldCover), ODbL (OSM)",
                "vocabulary": "ESA WorldCover class ids",
                "worldcover": wc_prov,
                "painted": painted,
            } if wc_prov else {
                "source": "OSM landuse/leisure (ESA WorldCover not fetched for this place)",
                "vocabulary": "ESA WorldCover class ids",
                "painted": painted,
            }),
            "canopy": ({
                "source": "Meta/WRI Global Canopy Height 1 m (alsgedi v6), CANOPY.R8 "
                          "in metres, max of the 1.2 m pixels per cell",
                "vintage": "2018-2020 imagery",
                "licence": "CC-BY 4.0",
                "detail": cn_prov,
            } if cn_prov else {
                "source": "NOT WIRED for this place -- Meta/WRI Global Canopy Height, "
                          "1 m, CC-BY 4.0 (fetch with --canopy)",
            }),
        },
        # STICKY source choices, plus the tag-path version. `tag_schema` is what
        # lets a reader tell a pre-2026-10-07 cache (narrow landmark rule, no
        # civic tags on a footprint) from one fetched after, without guessing
        # from which keys happen to be present -- see TAG_SCHEMA.
        "sources": {"land_cover": land_cover, "canopy": bool(canopy),
                    "tag_schema": TAG_SCHEMA,
                    "levels_estimator": levels_estimator,
                    "extra_buildings": bool(extra_buildings)},
        "tag_schema": TAG_SCHEMA,
        "attribution": [
            "Map data (c) OpenStreetMap contributors, ODbL 1.0",
            "Elevation: %s" % dem_prov.get("source"),
        ] + (["Land cover: ESA WorldCover 2021, CC-BY 4.0"] if wc_prov else [])
          + (["Tree canopy: Meta and WRI Global Canopy Height, CC-BY 4.0"]
             if cn_prov else [])
          + (["Buildings: Overture Maps Foundation, release %s, ODbL 1.0"
              % xb_stats["provenance"].get("overture", {}).get("release"),
              "Building heights: Google Open Buildings 2.5D Temporal %s, "
              "ODbL 1.0. %s. Contains modified Copernicus Sentinel-2 data"
              % (xb_stats["provenance"].get("open_buildings", {}).get("year"),
                 xb_stats["provenance"].get("open_buildings", {}).get(
                     "citation"))]
             if xb_stats else []),
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
    ap.add_argument("--land-cover", choices=("osm", "worldcover"), default=None,
                    help="COVER.R8 base: OSM only, or ESA WorldCover 10 m with "
                         "OSM painted over it (network once, then cached). "
                         "Default: whatever the place was last built with")
    ap.add_argument("--canopy", choices=("on", "off"), default=None,
                    help="write CANOPY.R8 from the Meta/WRI 1 m canopy map "
                         "(network once, then cached). Default: as last built")
    ap.add_argument("--levels-estimator", choices=("v1", "v2"), default="v2",
                    help="height estimator for a building OSM did not tag. v2 "
                         "(default) also reads the use tags amenity / office / "
                         "government / leisure / shop; v1 is the "
                         "pre-2026-10-07 building-class-only behaviour, kept so "
                         "a MODELS.DAT delta can be split into 'new landmarks' "
                         "and 'new heights' instead of asserted to be one")
    ap.add_argument("--skip-dem-probe", action="store_true",
                    help="do not call the IGN WFS coverage probe. It is "
                         "uncached and its answer is not stored in PLACE.JSON, "
                         "so on a re-fetch it is one outbound request for "
                         "nothing")
    ap.add_argument("--extra-buildings", choices=("on", "off"), default=None,
                    help="merge Overture footprints + Open Buildings 2.5D "
                         "heights into BUILDINGS.JSON from the cached products "
                         "in _cache/ (see geo_buildings_extra.py). Default: as "
                         "last built. Never touches the network by itself")
    ap.add_argument("--fetch-extra", action="store_true",
                    help="allow the ONE network session that fills the extra "
                         "building products. Without it a missing product is "
                         "a dry run that names every request it would make")
    ap.add_argument("--extra-plan-only", action="store_true",
                    help="stop after the extra-buildings metadata step and "
                         "print the data budget")
    ap.add_argument("--overture-release",
                    help="pin an Overture release (default: the STAC 'latest' "
                         "at fetch time, recorded in the provenance)")
    ap.add_argument("--extra-max-mb", type=float, default=600.0,
                    help="byte budget for the extra-buildings session")
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
                a.dem_smooth_m, a.land_cover,
                None if a.canopy is None else a.canopy == "on",
                1 if a.levels_estimator == "v1" else 2,
                not a.skip_dem_probe,
                None if a.extra_buildings is None else a.extra_buildings == "on",
                a.fetch_extra, a.extra_plan_only, a.overture_release,
                a.extra_max_mb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
