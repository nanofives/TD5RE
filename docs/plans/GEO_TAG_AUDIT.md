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

## Measured after: the one authorised re-fetch, 2026-10-07

### The fetch cost exactly one outbound request, proved before spending it

A TRUE dry run: the real `fetch_place` with `urllib.request.urlopen` replaced
by a raiser and the old cached response substituted for the new query. It ran
to completion with **12 cache HITS and 0 MISSES**. The bbox is unchanged, so
all 12 Terrarium tiles and all 248 WorldCover/canopy COG byte ranges were
already on disk; both Overpass mirrors share one cache key (it is keyed on the
QUERY, not the URL), so a mirror failover costs nothing; and `--skip-dem-probe`
(new) suppresses the uncached IGN WFS probe, whose answer was never stored in
PLACE.JSON anyway.

That dry run also caught a real bug in this round's own code -- the
building-heights print became a blanket `%d` over a dict that now nests
`by_use_tag`, and it raised **after** the rasters were built and **before** the
contract was written. Unfixed it would have burned the fetch and written
nothing.

Re-running the normaliser after the fetch is free, and that was verified too:
`_cache` file count 263 before and 263 after the second pass.

### What the re-fetch brought back

Overpass elements **6119 -> 7504**.

| | before | after |
|---|---|---|
| building footprints | 2047 | **2326** |
| ... with a measured height | 301 | 376 |
| ... with `roof:shape` | 4 | **45** |
| `building:part` masses stacked | 0 | **31** |
| areas | 302 | **416** |
| `ignored` ways | 22 | 43 (all small or deliberate) |
| traffic signals | 566 | **566** (membership identical) |
| other highway nodes (new `nodes[]`) | 0 | 596 |
| cache landmarks | 10 | **74** |
| landmarks within 100 m of the route | 2 | **11** |
| footprints within 100 m of the route | 54 | 88 |

`building:part` is the quiet win. Section 6g had to prove the roof/part work on
a synthetic fixture because La Plata carried **zero** `building:part` and four
`roof:shape` -- and the reason was gate 1: `convert_osm` branched on
`building:part` but the query never asked for a part way, and most part ways
carry no `building` tag. Those six fixes are now exercised on the real place:
31 part stacks and 45 roof shapes.

### The 11 landmarks on the route, named

9 of these 11 are new. Span is the route node, distance is to the centroid.

| deciding tag | name | span | dist | height |
|---|---|---|---|---|
| `building=cathedral` | Iglesia Catedral Nuestra Señora de los Dolores | 772 | 78 m | 20.0 m (tagged) |
| `heritage=1` | Casa Curutchet | 217 | 37 m | 9.0 m (levels) |
| `government=administrative` | Torre Ing. Luis Monteverde | 648 | 42 m | 54.0 m (levels) |
| `government=ministry` | Ministerio de Desarrollo Social de la Pcia. | 433 | 120 m | 30.0 m (levels) |
| `government=legislative` | Honorable Cámara de Diputados de la Pcia. | 518 | 112 m | 27.0 m (levels) |
| `government=yes` | Colegio de Arquitectos de la Pcia. | 216 | 58 m | 9.0 m (levels) |
| `building=government` | Residencia del Gobernador de la Pcia. | 369 | 79 m | 20.0 m (tagged) |
| `office=government` | Ministerio de Salud | 899 | 122 m | 22.5 m (use tag) |
| `amenity=theatre` | Taller de Teatro UNLP | 573 | 40 m | 12.0 m (area ctx) |
| `amenity=place_of_worship` | Ntra. Sra. de la Victoria | 1150 | 42 m | 13.5 m (use tag) |
| `amenity=police` | Comisaría La Plata 1ª | 566 | 66 m | 7.8 m (use tag) |

The two that were already landmarks are the Cathedral and Casa Curutchet.

### Gates

| gate | result |
|---|---|
| **synthetic seed 99991, slot 60** | **BYTE-IDENTICAL to master.** MODELS.DAT 13071384 `00FB76E8A4742DA94FE14BF12AC79AC2`, STRIP.DAT 144714 `0641EDB7787600D236AE7B51186888DD`, TEXTURES.DAT 1605328 `F69A8CBB6A3FFCA4757360F5AC39234B` -- all three equal on master (0999633c, built in its own worktree) and on this branch |
| La Plata STRIP.DAT | **65730 `22E37FB227540EF63882E3764A4145B7`, unchanged** -- the drivable track did not move |
| La Plata TEXTURES.DAT | unchanged |
| La Plata MODELS.DAT | 12573804 -> 12660532 (+86728), decomposed below |
| HEIGHT.R16 / WATER.R8 / CANOPY.R8 / ROUTE.JSON / ROUTE_RAW.JSON | **byte-identical after the re-fetch** |
| COVER.R8 | 38413 cells differ (0.79%), attributed below |
| structure lint | OK: extern_in_c 3/3, game_h_includers 23/23, warnings 83 vs baseline 84 (the same "improved" master itself reports, so it is pre-existing) |
| build | dev + release OK |

The synthetic gate is the one that matters: every geo path is gated on
`td5_geo_loaded()`, so a synthetic build must not move, and it does not.
Note that commit 400c9751's recorded 13069388 / `63919f5d...` is NOT a valid
baseline any more -- master has moved since (round 1004). The baseline above
was rebuilt from 0999633c for this comparison, which is the only honest way to
run the gate.

### The MODELS.DAT change, split by cause rather than asserted

Three arms, one seed, one slot. This is what the A/B knobs are for.

| arm | exe | cache | knobs | MODELS.DAT | landmarks |
|---|---|---|---|---|---|
| base | master | old | n/a | 12573804 | 22 |
| noknobs | this branch | **new** | all three OFF | 12667824 | **22** |
| new | this branch | new | all three ON | 12660532 | **74** |

- **The new DATA accounts for +94020 bytes** (base -> noknobs): 279 more
  footprints, 114 more areas, 34 more footprints bound within reach of the
  route, 31 part stacks, 41 more roof shapes.
- **The new READER RULES account for -7292 bytes** (noknobs -> new), and the
  sign is the interesting part: 52 more landmarks make the file SMALLER,
  because 3 of them have no 3D tags and are now stamped as a shipped set piece
  instead of extruded as a full wall-and-roof box. The build log says so
  directly: "3 shipped set piece(s) stamped on a real footprint, 1 found no
  piece small enough".

**The knobs-off arm reproduces the pre-round landmark count exactly: 22, from
`flag=10, building=12`.** That is the documented pre-round state of section 6g
("cache landmarks 10 -> 22" via the reader's class rule) recovered on the NEW
cache, which is what makes `geob_legacy_landmark` a real reconstruction of the
old rule rather than an approximation of it.

Road surface, same two arms: `2187 smooth, 88 cobbled, 16 loose; 73 roundabout
way(s)` with the knob on, `2291 smooth, 0 cobbled, 0 loose; 0 roundabout` with
it off.

### COVER.R8, attributed

The first pass through the new data moved 77119 cells (1.59%) and most of it
was wrong. Both causes were found by attributing the delta per newly-accepted
area kind rather than by accepting the total:

- `landuse=railway` painted 41311 cells BARE, of which 19562 were WorldCover
  BUILT and 7898 TREE. The OSM outline covers a whole station precinct,
  buildings and trees included, so reading it as ballast **overrides a
  measurement with a guess** over half a square kilometre. Removed.
- `leisure=sports_centre` / `stadium` painted 53142 cells BUILT. A sports
  ground is pitches and grass, and GRASS also agrees with the reader's own
  `geob_area_kind_of`, which classes them PITCH -- the two tables were saying
  different things about one way. Changed to GRASS.

After both: **38413 cells (0.79%)**, dominated by BUILT -> GRASS on those club
grounds, which is exactly what `leisure=park` already does over WorldCover by
design.

### Reproducing any of this

`verify/geo_tags_identity.ps1` runs one arm end to end (windowed, RT off,
minimum graphics, PID-scoped shutdown, `TD5RE_TG_DOUBLE_BUILD=1` +
`TD5RE_AUTOTRACK_REUSE=0` so the build is complete rather than streamed and
the second pass is not a GENSTAMP reuse):

```
pwsh verify/geo_tags_identity.ps1 -Arm synthetic -Tag base
pwsh -Command "& verify/geo_tags_identity.ps1 -Arm geo -Tag noknobs ``
     -Extra @{TD5RE_GEO_LM_TAGS='0';TD5RE_GEO_AREA_TAGS='0';TD5RE_GEO_ROAD_TAGS='0'}"
```

Pass `-Extra` through `-Command`, not `-File`: `-File` stringifies every
argument and the hashtable will not bind.
