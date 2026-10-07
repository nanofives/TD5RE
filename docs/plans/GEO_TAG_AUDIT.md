# GEO TRACK -- the OSM tag path, end to end (audit, 2026-10-07)

Round 1007 item 3: "add all the missing tags". The 2026-09-30 close-out
(GEO_TRACK_OSM_PLAN.md section 6g) found `geo_fetch` dropping
amenity/office/government and costing 9 civic landmarks near the La Plata
route. This is the full walk of the path those three keys were a symptom of.

Measured on the **La Plata cache as it stood before this round** (2 Overpass
responses under `re/assets/geo/la_plata/_cache/`, 6119 unique elements: 5553
ways, 566 nodes, **0 relations**), with
`scratchpad/tag_audit.py` + `tag_audit2.py` (one-off, not committed) and
`re/tools/geo_fixtures/land_relabel.py --dry-run`.

## The path has four gates, not one

```
Overpass QL  ->  convert_osm()  ->  *.JSON writers  ->  C readers  ->  emitters
  (gate 1)        (gate 2)          (gate 3)            (gate 4)
```

Gate 1 decides what is ever downloaded. Gate 2 decides what survives
normalisation. Gate 3 is just `write_json` of whatever gate 2 built, so it
drops nothing of its own. Gate 4 is each C reader's `cJSON_GetObjectItem`
list. **A tag has to clear all four.** The close-out's three keys were stopped
at gate 2; the audit below found losses at gates 1, 2 and 4.

## Gate 1 -- the Overpass query

Before: `way[highway]`, `node[highway=traffic_signals]`,
`way[natural=water]`, `way[natural=coastline]`, `way[waterway]`,
`way[landuse]`, `way[leisure]`, `way[building]`.

A tag on a way the query already selects arrives **free** -- Overpass returns
every tag of a matched element. That is why `amenity` on a building way was in
the cache all along (232 building ways carry it) and the loss was at gate 2.
What gate 1 genuinely loses is an element no clause matches:

| Not selected | Present at La Plata | Consequence |
|---|---|---|
| `way[building:part]` | 0 ways (but `convert_osm` already branches on it) | the 3D part-stack path is unreachable for any part way that is not also tagged `building` -- a dead branch, not a measured loss here |
| `way[amenity]` / `[office]` / `[government]` / `[tourism]` / `[historic]` / `[shop]` / `[man_made]` with no building/landuse/leisure tag | 3 amenity, 0 office/government ways | a walled school ground, a standalone place_of_worship area |
| `way[natural]` other than water/coastline | 3 ways (`natural=*` total 3) | wood / scrub / sand never reach COVER.R8 even though `_OSM_TO_COVER` has a `wood` row |
| `node[highway]` other than `traffic_signals` | query returns only signals, so 0 of 566 nodes are crossings or stop lines | no crossing / stop / give_way node data at all |
| `node[traffic_calming]` | 2 (on signal nodes only) | speed humps invisible |
| `relation[...]` anything | **0 relations in the cache** | a multipolygon building or park (courtyard, donut plaza) is absent entirely |

## Gate 2 + 4 -- the table

`geo_fetch.py` column = where in `convert_osm` the tag is dropped.
"Reader" = the C module that would have to read it.

### Buildings (`BUILDINGS.JSON`, 2047 footprints)

| tag | count | dropped at | what it should drive |
|---|---|---|---|
| `amenity` | 232 | gate 2, buildings branch | **civic landmark**: place_of_worship 21, police 7, townhall, courthouse, theatre, arts_centre, casino. Also a height class |
| `office` | 58 | gate 2 | **civic landmark**: `office=government` x16, `office=diplomatic` x1 |
| `government` | 7 | gate 2 | **civic landmark**: administrative 2, legislative 2, ministry 1, healthcare 1, yes 1 |
| `religion` / `denomination` | 28 / 25 | gate 2 | confirms a place_of_worship; church height class |
| `tourism` | 5 | **kept only as a bool inside `landmark`** | the VALUE matters: `hotel`/`hostel` are ordinary frontage and were being promoted (the old blanket `tourism or historic` is what put a boutique hostel in the landmark list) |
| `historic` / `heritage` | 2 / 1 | same -- bool only | monument, memorial |
| `man_made` | 6 | gate 2 | tower, lighthouse, obelisk, water_tower: landmark silhouettes |
| `leisure` | 27 | gate 2 (buildings) | sports_centre / sports_hall: a big-span shed, not a 3-storey block |
| `shop` | 384 | gate 2 | `shop=mall` x16 is a large single mass; the rest is a retail height class |
| `healthcare` | 39 | gate 2 | clinic/hospital height class |
| `building:levels` | 296 | **consumed then discarded** -- only the derived `height_m` survives | the raw level count is the honest provenance; `building:min_level` x1 is lost outright |
| `roof:levels` | 3 | gate 2 | roof rise without a `roof:height` |
| `roof:material` / `roof:colour` | 3 / 3 | gate 2 | roof page pick |
| `building:colour` / `building:material` | 0 / 3 | **kept in JSON, gate 4** -- `geob_load_buildings` never reads `colour`/`material` | facade tint |
| `name:*`, `official_name`, `alt_name`, `short_name` | 9 / 8 / 13 / 10 | gate 2 (only plain `name`) | screen labels, and `name:es` is the local spelling |
| `operator`, `wikidata`, `wikipedia`, `start_date` | 59 / 10 / 7 / 11 | gate 2 | provenance; `start_date` dates a historic mass |
| `layer` | 29 | gate 2 | a building under a deck |
| `landmark_src` (not an OSM tag) | n/a | never written | **which tag decided** a landmark -- without it a promotion is unattributable |

### Roads (`ROADS.JSON`, 2291 drivable ways)

| tag | count | dropped at | what it should drive |
|---|---|---|---|
| `junction` | 73 | **gate 2 -- dead code.** The `median` expression ends `or t.get("junction") == "roundabout" and False`, which Python evaluates to `False` unconditionally, so `median` is `False` on all 2291 ways | roundabout / circular detection |
| `surface` | 2238 | **kept in JSON, gate 4** -- `td5_geo_roads.c`'s header says it is "deliberately skipped" | side-street surface page. Non-smooth minority is real: sett 87, dirt 10, unpaved 3, gravel 3, cobblestone 1 |
| `oneway` direction | 1997 | **collapsed to a bool** -- `oneway in ("yes","1","-1","true")`, so `-1` (reversed) reads the same as `yes` | which way a one-way street runs |
| `maxspeed` | 1665 | kept in JSON, gate 4 | traffic speed on a real street |
| `lit` | 1795 | gate 2 | street lamps |
| `sidewalk` | 187 | kept in JSON, gate 4 | pavement width |
| `sidewalk:left` / `:right` | 0 / 17 | gate 2 | per-side pavement |
| `lanes:forward` / `:backward` | 0 | gate 2 | asymmetric carriageway |
| `ref` | 150 | gate 2 | route shield on a sign |
| `smoothness` | 89 | gate 2 | grip modifier |
| `incline` | 34 | gate 2 | sanity-check the DEM's own grade |
| `access`, `service`, `tracktype`, `covered`, `area` | 39 / 16 / 0 / 24 / 10 | gate 2 | a `service=parking_aisle` is not a street; `covered` is a tunnel that is not tagged one |
| `bridge` / `tunnel` VALUES | 15 / 19 | **collapsed to bools** | `tunnel=building_passage` is not a bore; `bridge=viaduct` is |
| `name:*`, `alt_name`, `old_name` | 1231 alt / 117 old | gate 2 | street labels |

### Areas (`AREAS.JSON`, 302 kept of ~400 candidate ways)

The whitelist is `leisure in {park, garden, pitch, playground, common,
recreation_ground, dog_park}` or `landuse in {grass, village_green, meadow,
forest, cemetery, allotments, orchard, vineyard, residential, commercial,
retail, industrial, construction}`. Everything else hits `bump("ignored")`.

| tag / value | count | dropped at | what it should drive |
|---|---|---|---|
| `natural=*` as an AREA | 3 | gate 1 **and** gate 2 (`natural` is only consulted for water/coastline) | `wood`, `scrub`, `sand`, `wetland`, `heath`, `bare_rock` are all in COVER.R8's vocabulary and `_OSM_TO_COVER` even has a `wood` row that nothing can reach |
| `landuse=farmland` / `quarry` / `brownfield` | 0 here | **gate 2 whitelist** -- all three are in `_OSM_TO_COVER` but not in the areas whitelist, so the cover rows are unreachable | COVER_CROP / COVER_BARE |
| `landuse=education` | 4 | gate 2 whitelist | a campus block is built, not grass |
| `landuse=railway` | 3 | gate 2 whitelist | bare ballast |
| `landuse=plant_nursery` | 1 | gate 2 whitelist | crop cover |
| `leisure=track` | 7 | gate 2 whitelist | a running track IS a flat pitch-like surface |
| `leisure=stadium` / `sports_centre` / `swimming_pool` / `bleachers` / `fitness_station` | 1 / 2 / 2 / 1 / 1 | gate 2 whitelist | pitch / plaza class |
| `sport` | 53 | gate 2 | which pitch |
| `surface` | 24 | gate 2 | a `surface=asphalt` pitch is not lawn |
| `amenity` on an area | 3 | gate 2 | `amenity=school` ground |
| `barrier` | 2 | gate 2 | a walled compound |
| `access`, `layer`, `man_made`, `tourism`, `historic` | 2 / 12 / 0 / 0 / 0 | gate 2 | private ground; a deck over a park |

### Signals (`SIGNALS.JSON`, 566 nodes)

| tag | count | dropped at | what it should drive |
|---|---|---|---|
| node `id`, `lat`/`lon` | 566 | gate 2 (only x/z survive) | re-identification against a re-fetch |
| `crossing` | 144 | gate 2 | zebra markings at the signal |
| `traffic_signals` (the subtype) | 17 | gate 2 | `=signal` vs `=blinker` |
| `button_operated`, `tactile_paving`, `traffic_signals:sound`/`:vibration` | 14 each | gate 2 | push-button post model |
| `highway=crossing` / `stop` / `give_way`, `traffic_calming` | **0 fetched** | gate 1 | crossings, stop lines, humps |
| `direction` | 0 of 566 non-null | kept in JSON, read-but-unused by design (documented in `td5_geo_signals.c`) | per-node facing |

### Water (inside `convert_osm`, folded into the cover/water rasters)

| tag | count | dropped at | what it should drive |
|---|---|---|---|
| `name` | -- | gate 2 (kind/closed/points only) | river labels |
| `layer`, `tunnel` | 12 / 9 | gate 2 | a **culverted** ditch runs UNDER the ground and must not paint water on the surface |
| `intermittent`, `width` | 0 / 0 | gate 2 | a dry wadi is not a lake |

## What this round changes, and what it deliberately does not

**Done** (see the commits on this branch):

1. Gate 1 widened: `building:part`, `amenity`, `office`, `government`,
   `tourism`, `historic`, `heritage`, `shop`, `man_made`, bare `natural`,
   bare `node[highway]`, `node[traffic_calming]`.
2. Gate 2: every tag in the tables above is carried. The landmark rule is the
   full tag table (ported from `land_relabel.py`, which was the offline
   stand-in for exactly this) plus `landmark_src`.
3. Gate 2: the `median` dead code and the whitelist holes are fixed; the
   areas whitelist is widened to everything `_OSM_TO_COVER` can colour.
4. Gate 4: the C readers read the new fields -- landmark from the tags,
   the widened area/plaza classes, road `surface` and `junction`.

**Not done, and why** -- see the final report:

- **Relations / multipolygons.** Adding `relation[...]` to gate 1 means
  assembling outer/inner rings in `convert_osm` and teaching the ring pool
  about holes. It is its own piece of work, and this round has exactly ONE
  authorised fetch to spend: shipping an untested relation normaliser against
  that one response is the wrong risk. The query is unchanged in that respect,
  so no relation data is in the new cache either.
- **`geo_condition.py` / `ROUTE.JSON`.** Per-node road `surface` on the driven
  route would be the natural home for the surface tag, but the L3 child is
  porting the conditioner to C this round. Touching the schema would collide.
  Road surface therefore goes through `ROADS.JSON` -> the side streets only.
