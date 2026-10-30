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
