# GEO REGION EXTENT (round 1015 F)

"Download all the data from partido de La Plata, not just a square within the city
centre."

## What the extent used to be

`geo_fetch.fetch_place` takes a centre and a radius and makes a **square** of side
`2 * radius` (`FETCH_MIN_RADIUS_M` 500, `FETCH_MAX_RADIUS_M` 4000). La Plata was
fetched at radius 2713 m: a 5.4 x 5.4 km box, 29 km2, around -34.924, -57.956.
Every layer was one file (`ROADS.JSON`, `BUILDINGS.JSON` ... and four rasters of
~2200 x 2200 cells), and `td5_geo_route_commit` re-gridded **the whole place**
into the route frame. So the cost of a BUILD, and every loader cap, scaled with
the area of the place.

## What the partido is

OSM relation **2499263**, `boundary=administrative`, `admin_level=5`, name
"Partido de La Plata" (the city is a different relation, 3266014, admin_level 8).
Looked up with `is_in(lat,lon)` + `rel(pivot)`, then fetched with
`out geom`:

* mainland ring: 622 points, **886.0 km2**, bbox S -35.2354 W -58.2886 N -34.8350
  E -57.7546, i.e. **49.1 x 44.9 km**. The polygon fills about 41 % of its bbox.
* a second outer ring of 1.94 km2 at -34.18, -58.25 is **Isla Martin Garcia**, an
  exclave 60 km north. It is dropped by `min_ring_frac` (default 2 % of the largest
  ring) and reported in `PLACE.JSON boundary.dropped_exclave_rings`.

## The design: a TILED place

One Overpass answer for 886 km2 does not exist, and one BUILDINGS.JSON would be
~150 MB of cJSON. So a place fetched for an administrative area is built in pieces
by `re/tools/geo_region.py` and stored as a **tiled place**:

* Blocks of ~6 km (3 x 3 tiles of ~2 km), only those the boundary polygon touches
  (43 of 72), each fetched with a 400 m halo through the *same* code geo_fetch
  uses for a whole place (`fetch_osm`, `convert_osm`, the Overture + Open Buildings
  step, `rasterize_layers`, `estimate_building_heights`).
* Every vector entry is kept by exactly one block (the one holding its first
  vertex; if that block was not fetched, the first fetched block that returned it),
  and is written into `tiles/<LAYER>_<ix>_<iz>.JSON` with `tiles/INDEX.JSON`
  recording each tile's extent.
* Rasters (`HEIGHT.R16`, `COVER.R8`, `WATER.R8`, `CANOPY.R8`) are one flat
  `TD5GEOR1` file each over the whole area on **one global cell lattice**
  (origin a multiple of the 1500-unit cell), HEIGHT with one global scale/bias.
* The frame is **unrotated, offset 0**, about the area centre.
* `PLACE.JSON` gains `tiled` (index path, tile size, grid) and `boundary`
  (simplified ring, 15 m).

Nothing ever *races* the source frame. `td5_geo_route_commit` cuts a **window**
out of it - the route's box plus `GR_TILED_WINDOW_MARGIN_M` (1500 m) - in the
route frame and writes the usual `_route/` derived frame. A place that is not
tiled takes none of the new branches (A/B: byte-identical `_route/` for the old
La Plata between the master exe and this branch).

The router reads a window too: tiles under the waypoints' box plus 4 km
(`GR_TILED_GRAPH_MARGIN_M`), reloaded when a request comes within 1.5 km of the
loaded window's edge.

`td5_geo_route_place_at` also tests the boundary polygon (a click inside the bbox
but outside the partido is "outside every downloaded place"), and when two places
hold a point the larger bbox wins. The map draws the polygon outline instead of the
bbox wash for a place that has one.

## Generic use

```
python re/tools/geo_region.py --spec re/tools/geo_regions/<place>.json --dry-run
python re/tools/geo_region.py --spec ... --online [--only bx:bz]
```
The spec names the relation (`name` + `admin_level` + `near`, or `id`). Needs
`pyproj` and `s2sphere` (the Open Buildings reader), kept in a throwaway venv.
Resumable per block (`_region/done/`).

## The fetch budget, proved offline first (`geo_region.py --dry-run`)

`urllib.request.urlopen` replaced by a raising recorder, every cache consulted
(la_plata's 302 cached responses were copied in first, so its Terrarium tiles,
canopy/WorldCover headers and Overture footer were reused):

| source | requests | bytes |
|---|---|---|
| Overpass, one query per block | 43 | not knowable offline; ceiling 43 x 46 km2 x La Plata's densest 0.16 MB/km2 = 316 MB |
| Terrarium z14 tiles | 405 uncached of 417 | ~36 MB |
| ESA WorldCover + Meta canopy COGs (headers cached) | range reads | 123.6 MB planned (31+6015+40029 tiles) |
| Overture buildings (2 files, row groups unioned over blocks) | metadata only | 94.7 MB planned |
| Open Buildings 2.5D heights (27 TIFFs) | metadata only | 68.9 MB planned |

The Overture/Open Buildings plan needed one footer and the Open Buildings TIFF
headers that were not cached: a metadata-only pass of a few dozen small range
requests (each <= 1 MB), done before any data was read.

**Actual**, from the cache directory: 43 Overpass answers 44.6 MB (+ the boundary,
71 KB, and one `is_in` tags lookup), 405 Terrarium tiles 31.3 MB, 37 COG range
files 138.5 MB, 276 Overture/Open Buildings range files 305 MB (the plan counted
each row group once; the real run re-read some across blocks because
neighbouring blocks coalesce their spans differently -- 1.8x the plan, inside the
900 MB session cap). About 760 requests, ~520 MB, ~67 min of block time.

## Result

43 blocks, 516 358 building footprints, 17 310 drivable ways, 3 440 areas,
2 057 footways, 1 036 signals, 2 337 street-furniture nodes. On disk (the part that
is copied): rasters 14075 x 12863 cells (HEIGHT 346 MB, COVER/WATER/CANOPY 173 MB
each), 1393 tiles = 230 MB, PLACE.JSON, and the built `_route/` 62 MB -- 1.2 GB.
(`_cache` 788 MB and `_region/dem_m.f32` 691 MB are build scratch.)

## Limits found

* **24.8 fixed point.** The source frame spans +-10.56 M world units, past the
  2^23 = 8.39 M a 24.8 value holds. It never reaches the generator: the derived
  frame is route-local, and a 3000-span route (4.5 M units) plus the 1500 m
  margin (0.65 M) stays under 5.2 M.
* **Reader caps.** `gr_slurp` / `GR_MAX_FILE` 64 MB, `GEOB_MAX_JSON` 192 MB,
  `GEO_ROADS_MAX` 8192, `GR_MAX_ROADS` 16 384: all apply to a window, never to the
  area. Measured on the 4 km graph window: 8 181 ways in the city, 4 246 / 3 750
  in City Bell / Los Hornos. Largest tile file 6.3 MB (BUILDINGS_15_16).
* **Raster dims.** `GR_GUARD_MAX_GRID` 16 384 is checked on the derived window
  (1.5-1.7 k cells), not on the 14 075 x 12 863 source. Source rasters are read by
  a seek, never slurped.
* **Uncompacted tiles.** The first tiles kept every default-valued key of an
  Overture record: 326 MB / a 68 MB derived BUILDINGS.JSON, +190 MB of peak memory.
  `_xb_compact` (the same as fetch_place) brought them to 220 MB / 47 MB.
* **Per-city rules keyed by slug.** `td5_geo_sidewalk.c` matched "la_plata"
  exactly, so the partido took the generic row (calle carriageway 3 -> 2 lanes,
  curvature limit 10.6 -> 32.3 deg, a 1316-span route). Now "<slug>" or
  "<slug>_*".
* **The old Python selector** (`geo_selector.py`) does not know tiled places.
