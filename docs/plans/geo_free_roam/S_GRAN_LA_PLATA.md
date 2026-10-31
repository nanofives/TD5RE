# Round S / SB: Berisso and Ensenada in the free-roam place ("Gran La Plata")

Mariano, 2026-10-30: "también quiero que agregues el partido de Berisso y de Ensenada".
Decision: ONE continuous place. Slug stays `la_plata_partido`, display name "Gran La Plata".

## Why not just re-run geo_region with a bigger relation list

1. `geo_region.Frame` centres the projection on the bbox. The union bbox grows 6.2 km north
   and 4.1 km east, so the origin would move about 3 km and every place coordinate with it
   (ROAMTILE names, saved routes, selftest and tour poses).
2. The 43 La Plata block answers of 10 Oct are gone. The main tree `_cache/` holds 12 files,
   9.4 MB: the answers of the later producers (pois, s3d, parks, restrictions, lanes,
   stadia, rails, outskirts) and three count probes. The 788 MB block cache was build
   scratch (GEO_REGION_EXTENT.md "Result"). A re-run would re-fetch La Plata and pull in
   every OSM edit made since.

So the tool does not re-derive La Plata at all. That is stronger than "served from the
cache": its tiles and raster cells are copied, not regenerated.

## EXTENSION mode (`geo_region.py`, spec key `frame_from`)

Spec: `re/tools/geo_regions/gran_la_plata.json` (`relations`: La Plata, Berisso, Ensenada;
`frame_from: re/assets/geo/la_plata_partido`; Overture release pinned to La Plata's
`2026-09-23.1`). The single `relation` form and a plain run are unchanged (no new branch is
taken without `frame_from` / `relations`).

* **Boundary.** One ring per relation, `min_ring_frac` per relation (default the spec's).
  La Plata's ring comes from its PLACE.JSON (simplified 15 m), so 0 requests. Berisso and
  Ensenada come from the cached boundary answers (`_archive/roam/S/boundary_cache`, same
  query string, so the same cache key). Union in world units, then a 20 m closing
  (`DISSOLVE_M`) welds the shared borders: 1 ring, 214 points, 0 holes.
  PLACE.JSON `boundary.rings` = the union, `boundary.parts` = each partido.
* **Frame pinned.** Projection (lat0 -35.0352183, lon0 -58.0215769, 430 u/m), cell 1500,
  lattice origin (-10555500, -9646500), tile 859500 u (573 cells), 3x3 tiles per block.
  Grid 14075 x 12863 -> **15261 x 14638** (+1186, +1775 cells). Growing west or south of
  the origin is refused (it would renumber cells). No old block, tile or cell moves.
* **Old blocks.** Read off the base HEIGHT.R16 (a fetched block wrote every interior cell):
  43, equal to PLACE.JSON `tiled.blocks`. Their tile files and raster cells are copied.
* **New blocks** (touched by the union, not fetched before): (6,6) (7,6) (8,6) (4,7) (5,7)
  (6,7) (3,8) (4,8). Fetched and converted with the same code as a plain run (Overpass,
  Overture + Open Buildings, WorldCover, canopy, Terrarium). Ownership lists the old blocks
  first, so an entry anchored in an old block is never written again. A tile homed in a new
  block is written whole; a stale file there gets an empty document (main-tree re/assets
  cannot delete). Those tiles held the old blocks' 400 m halo copies of Berisso / Ensenada
  entries (the smoke test counted up to 3021 buildings in one block): they are replaced by
  the new vintage. None of them is La Plata: a new block by definition does not touch the
  La Plata polygon.
* **Completion blocks** (8,4) (8,5) (3,7): old blocks cut by the old lattice edge whose part
  past it meets the union. A block always fetched its whole box, so the vectors exist; only
  raster cells past the old edge are missing. They are painted from the vectors ON DISK
  (roads, areas, buildings from the tiles) plus `water` from a fresh Overpass answer (rivers
  and the coastline are not stored in any tile layer). No Overture, no tile writes.
* **HEIGHT.** Scale 0.6562968 and bias -656.1254 are kept. int16 raw spans -32000..32000
  about the old minimum, so it represents **[-50.37 m, 47.31 m]**. River cells are NOT
  clamped beyond what geo_fetch does: the -4 m sea-floor clamp (and the coastline sink to
  -4 m) fits as is. A new cell above 47.31 m or below -50.37 m would be clipped: the run
  counts it and exits 4. Berisso / Ensenada are coastal plain, so none is expected.
* **Proof built into the run** (`_region/extend_report.json`, exit 4 on failure): every
  raster cell of the 43 old blocks equal to the base file (119 207 493 cells per raster),
  every tile homed in an old block byte-identical, changed tiles classified.

## Producers (one Overpass call per box)

PLACE.JSON gains `extension.base_bbox` (La Plata's bbox as fetched) and
`extension.added_bboxes`: the bbox of the NEW relations minus the base bbox, as strips:

| box | S | W | N | E | km2 |
|---|---|---|---|---|---|
| base | -35.2374008 | -58.2905675 | -34.8330358 | -57.7525863 | 2192.7 |
| north strip | -34.8330358 | -58.0861711 | -34.7772067 | -57.7072278 | 213.8 |
| east strip | -34.9736883 | -57.7525863 | -34.8330358 | -57.7072278 | 64.4 |

`geo_rails.fetch_place`: the base query string is the old one byte for byte, so its answer
is the cached one. An element of an added box is kept only if the base answer does not
hold it AND its anchor (node point, first way vertex / `center`, relation bounds centre)
lies outside the base query box: an OSM edit made inside the old area since it was fetched
cannot leak in. Applied to geo_pois, geo_s3d_relations, geo_plaza_parks,
geo_turn_restrictions, geo_lane_paint (added boxes use `out tags center;` to get an anchor),
geo_rails (bbox + its 0.01 deg margin) and geo_outskirts (same rule, own copy, stdlib).
geo_stadia keeps the stadia the existing STADIA.JSON holds in the old query and gives new
stadia their own query.

geo_plaza_parks also keeps base records verbatim (`geo_rails.keep_base_records`): the
shipped PARK_RELATIONS.JSON predates `mark_dups` (d6659e32), and a rerun would mark
relations 1650630 and 16786134 `dup_of`, which td5_roam_scenery.c skips. That would change
La Plata, so the extended run keeps the shipped records and only appends.

Not producers of the tiled place: geo_rederive_buildings.py (rewrites a strip `_route/`) and
geo_renormalise_road_tags.py (single-file ROADS.JSON). Nothing to change.

## Phase A evidence (offline)

* `_archive/roam/SB/producers_check.py`: every producer rerun on a scratch copy of the
  current place from the cache, urlopen raising: POIS, S3D_RELATIONS, PLAZA_FURNITURE,
  RESTRICTIONS, LANES, STADIA byte-identical, 1788 tile files identical, INDEX layers
  identical, `build_index(keep_other=True)` reproduces INDEX.JSON. PARK_RELATIONS differs
  only by the 2 `dup_of` above, and the unpatched master code gives the same relations.
* `_archive/roam/SB/producers_dryrun.py`: on a provisional extended PLACE.JSON every base
  query is a cache hit; each of the 7 bbox producers needs 2 new POSTs; stadia 0 on the
  current tiles (1 if the new AREAS tiles bring a stadium).
* `geo_region.py --dry-run`: 11 Overpass block queries, 131 Terrarium z14 tiles, 3 COG
  headers not cached (WorldCover S36W060, canopy 210321301 / 210321303, the same files as
  La Plata), Overture needs `collections.parquet` of 2026-09-23.1 (metadata).
* `_archive/roam/SB/seed_check.py` after a 0-block run: the whole old grid (181 M cells)
  identical in all four rasters, headers identical except width/height, every cell outside
  the old grid nodata, 1787 tiles identical.
* `_archive/roam/SB/extend_smoke.py`: the full extension path with synthetic Overpass / DEM
  / cover inputs: exit 0, 0 differing cells in the old blocks, 0 changed tiles homed in old
  blocks, 35 changed tiles all homed in new blocks, 24 new files, INDEX keeps RAILS and
  OUTSKIRTS, PLACE.JSON grid 15261 x 14638, 51 blocks.

## Fetch (phase A -> B handoff)

Run from the roam-SB worktree, staging root `_archive/roam/S/place` in the MAIN tree (already
seeded: tiles, base cells in the rasters, `_cache` with the boundary and producer answers).

1. `python re/tools/geo_region.py --spec re/tools/geo_regions/gran_la_plata.json --root <ROOT> --plan-metadata`
2. `python re/tools/geo_region.py --spec re/tools/geo_regions/gran_la_plata.json --root <ROOT> --online`
3. `python -I re/tools/<producer>.py --place <ROOT>/la_plata_partido --online` for
   geo_plaza_parks, geo_stadia, geo_pois, geo_s3d_relations, geo_turn_restrictions,
   geo_lane_paint, geo_rails, geo_outskirts `--chunks 1`, in that order (stadia reads
   PARK_RELATIONS, pois / rails / outskirts read the new tiles, rails and outskirts put
   their INDEX layers back).

## Phase B prep (no new data yet)

### Boundary readers: the union ring needs no code
Every reader takes `boundary.rings` with an even-odd test over all rings:
`gr_place_contains` / `td5_geo_route_place_at` (td5_geo_route.c), `td5_roam_partido_load` (up to
`RP_RING_MAX` 8 rings), whose `contains` / `dist_m` / `tile_ok` gate the roam spawn (td5_roam_mode.c),
the planner region (td5_roam_route.c), the stream table (td5_roam_stream.c) and the planner map outline
(td5_roam_route_ui.c). The union is 1 ring of 214 points; the geo map keeps up to `GEO_RING_MAX` 400.
New: `td5_geo_route_place_part_ring` reads `boundary.parts[i]`, and the geo map draws each partido's
border as a 1 px line over the 2 px outline (`GEO_PART_MAX` 4, `GEO_PART_RING_MAX` 240; parts are 69 /
118 / 99 points). A place without `parts` draws as before.

### Display name
Both readers (td5_geo.c `s_geo.name`, `s_places.name`) take PLACE.JSON `name`, so "Gran La Plata"
comes with the data. No hard-coded "La Plata" string in a frontend or HUD path.

### Caps
| cap | where | scope | verdict |
|---|---|---|---|
| 2048 tiles | td5_roam_tile.c `--RoamBuildTiles=casco`, GC_MAX_TILES, RS_MAX_TILES | the casco list (404 tiles) | unchanged |
| `list:` tile list | td5_roam_tile.c | realloc, unbounded | unchanged |
| ROAMGRID | roam/ROAMGRID.JSON | unbounded lattice (u/v about origin 0,0, 42.13 deg) | new tiles get new indices |
| street layer | td5_roam_partido.c | realloc vectors + a grid sized to the ways' bbox | La Plata: 17201 ways, 102283 points, 413 ROADS tiles, 4874 ms, **1.5 MB** resident, grid 60 x 56 (now logged). Measure again on the union |
| planner region | td5_roam_route.c | tiles round the car / waypoints / corridor, realloc | scales with the route, not the place |
| RP_RING_MAX 8 | td5_roam_partido.c | boundary rings | 1 used |
| MMS_MAX_SEGS 40000 | td5_minimap_streets.c | the strip route's minimap (geo route), not roam | unchanged |

### Water and coast: the plan
What the code does today: a nodata HEIGHT cell reads as the bias (-1.53 m) and a nodata WATER cell as dry
(td5_roam_height.c). A tile is built only within 181 m + ROAM_HALO_M 300 m of the boundary
(`td5_roam_partido_tile_ok`). The water plane (roam LE) and the shore sub-mesh (roam PC) draw from WATER.R8.
So a river cell outside every fetched block would be a dry ground plate at sea level.

What the boundaries say (cached relations): Berisso is 1 ring (340 points) reaching E -57.7092; Ensenada is
1 ring (400 points) reaching N -34.7792. No island is a separate ring, so Isla Paulino and Isla Santiago are
inside them. Both bboxes reach well past where the shore should be (to check on the data), so the polygons
probably include river water and the blocks over it are fetched.

Phase B steps, in order, once the data is in:
1. Census on the new rasters: sea / water cells per new block (`extend_report.json` painted), the
   coastline ways in the block answers, and the distance from the shoreline to the boundary along the
   Rio de la Plata and the Rio Santiago.
2. If water reaches the boundary edge anywhere a car can see (500 m render radius): a water-only tile
   rule. A tile outside `tile_ok` whose cells are all sea in WATER.R8, within 1 km of a built tile, is
   built as water plane only (no ground, no scenery). La Plata has 0 sea cells, so its tiles cannot change.
3. If a fetched block ends on open water (nodata past it): treat a nodata WATER cell whose nearest data
   cell is sea as sea, in td5_roam_height.c only. Same La Plata argument.
4. Buildings, lots and lawns on water: census of footprint centroids on wet cells, and the same for
   park / landuse rings, before any fix.
5. Frames: the Punta Lara coast, the port and the Rio Santiago, the YPF refinery, Isla Paulino.

### La Plata ROAMTILE baseline (master, read only)
`_archive/roam/SB/roamtile_baseline.py <main place dir> roamtile_baseline_master.json`, every ROAMTILE
with sha256 / md5, centre lat-lon and the distance to the new partidos:

| dir | tiles | casco | City Bell (1.5 km) | within 1 km of Berisso/Ensenada | within 3 km | casco digest | City Bell digest |
|---|---|---|---|---|---|---|---|
| roam/ | 1812 | 404 | 0 | 267 | 717 | 5eecd90c9f81f399 | (none) |
| roam/x100/ | 1142 | 404 | 50 | 166 | 549 | b88d7d061375a79d | 74b114cce42c31d0 |

The casco's NE edge (Av 122) IS the border, so casco tiles near it can change legitimately; phase B
compares with `--compare <baseline> <place> --near-km 1.0` and lists every change by its distance.

### Selftest rows (skeletons, full suite)
`roam-berisso` (roam=22) and `roam-ensenada` (roam=23): `TD5_RSPRINT_BERISSO` / `_ENSENADA` in
td5_roam_race_drive.c, START Plaza Moreno, FINISH each relation's OSM admin_centre node (Berisso
198449765 at -34.86886, -57.88287; Ensenada 198440814 at -34.85749, -57.90772). The planner chooses the
streets. Same checks as roam-partido: 0 stream misses / holds, >= 1 rebase, no actor past 4.19 M units,
>= 15 junctions, >= 3 km. Standalone: `TD5RE_ROAM_SPRINT_TEST=berisso|ensenada`.
