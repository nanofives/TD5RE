# GEO TRACK -- generate an auto-track from a real place on Earth (plan, 2026-09-29)

Drive the existing AUTO TRACK generator from **real-world data** (OpenStreetMap
geometry, a real DEM, and satellite-derived vegetation and land-cover rasters)
instead of a seeded synthetic world, selected by the user on an interactive map.

Status: **planned, not started.** No branch yet. Greenfield -- a grep of
`re/tools/`, `scripts/` and `docs/` finds no OSM/DEM/lat-lon/GeoJSON ingestion
anywhere, and no existing path that feeds an externally supplied polyline into a
`TG_NodeList`.

## 0. Decisions (Mariano, 2026-09-29)

| Topic | Decision |
|---|---|
| Route mode | **Point-to-point with draggable waypoints**, Google-Maps style |
| Scale | 1:1 real scale |
| Geometry outside the drivable envelope | **Exaggerate elevation 1.5x, then clamp to the cap** |
| Multiplayer | **Single-player only for now** (avoids the lockstep determinism problem) |
| Crossing-safe span localiser (Option B) | **In parallel with Phase 5** |
| Buildings | **Real OSM footprints and real heights**, estimated from proxies where untagged |
| Traffic lights | **Decoration only** (the one genuinely new emitter) |
| Forks (median avenues, alternative routes) | **Detect and offer; the user confirms in the selector** |
| Landmarks | **Real geometry where OSM 3D tags exist, existing prefab table as fallback** |
| Plazas | **Full real polygon**, including internal paths, beds and interior trees |
| Primary region | **Argentina, La Plata especially**; fixtures = Buenos Aires + La Plata |
| DEM | **IGN MDE-Ar 30 m automated + 5 m local override** -- resolved in §6: 5 m covers La Plata/BA but is request-gated |

Also required: lanes, medians, traffic lights, plazas and their grass, sidewalk
widths, approximate road surface textures, city search, coordinate paste, map
panning, and an OSM-derived UI skinned to TD5's aesthetic. All of these are
folded into the sections below.

Guiding requirement, in Mariano's words: **maximum achievable precision, so the
surroundings are recognisable while playing.**

## 1. Why the data side is cheap: the pipeline is already inverted

The 2026-09-08 topology rework (`AUTOTRACK_TOPOLOGY.md`) split the generator
into three data layers under a stack of emitters that do not care where the data
came from:

| Layer | Module | What it publishes |
|---|---|---|
| **WORLD** | `td5_tg_world.c` | height, slope, water, terrain class, occupancy raster, biome paint |
| **ROAD** | `td5_tg_road.c` | the centerline `TG_NodeList`, the structure table, the grade profile |
| **NETWORK** | `td5_tg_network.c` | the planar street graph, the (span,side) mouth table, `NETWORK.JSON` |

`td5_tg_world.h:3-35` states the contract outright: this file is the **only**
place that knows how terrain is synthesised, and everything else asks
`tg_world_h` / `tg_world_class` / `tg_world_is_water`. That holds in practice --
all 130 call sites across `td5_tg_road.c` (26), `td5_tg_network.c` (25),
`td5_tg_terrain.c` (13), `td5_tg_pages.c` (5), `td5_tg_bridge.c` (3),
`td5_tg_city.c` (2), `td5_tg_prefab.c` (1) and `td5_trackgen.c` (3) go through
the public header. None reach into `s_w`, `TG_WorldParams` or the noise
functions.

The ROAD layer is equally clean. `tg_build_centerline`
(`td5_tg_road.c:690-1007`) is the **only** function that writes node x/z, and it
has exactly three callers: `td5_tg_pages.c:3881` (real build),
`td5_trackgen.c:4245` (studio preview), `td5_trackgen.c:4330` (regenerate). A
seam inside it is inherited by all three automatically.

### The whole feature list maps onto emitters that already exist

Verified by grep, 2026-09-29:

| Requirement | Already implemented in |
|---|---|
| Lane count | `TG_Node.lanes`, `tg_row_points`, `tg_span_type_for` |
| Medians / divided avenues | `td5_tg_branch.c` -- carriageway query, `tg_median_at_raw`, ISLAND forks, `TD5_TG_MEDIAN_MIN_FORK_LEN` |
| Plazas and their grass | `td5_tg_streets.c:552-589` -- lawn from the kerb, hedge border, occasional set-back house |
| Sidewalk widths | `tg_city_sidewalk_w` / `tg_city_sidewalk_w_at`, per-span, `TD5_TG_SIDEWALK_MIN` |
| Road surface textures | `tg_emit_roadset_pages`, `TD5RE_AUTOTRACK_ROAD_SET` (9 sets) |
| Fork from a median avenue | carriageway query, `td5_tg_branch.c` |
| Larger fork for two comparable routes | the long diverging branch, same module |
| Landmarks | `tg_landmarks_place` -- table-driven, hash-gated (consumes no RNG), prefabs via `re/tools/td5_geomlib.py` |
| Trees, tree bands, forest lanes | `td5_tg_terrain.c` |
| Snow | `TD5RE_AUTOTRACK_SNOW` |

**The only genuinely new emitter in the entire requirement list is traffic
lights.** Nothing matching `traffic_light`, `signal` or equivalents exists in
`td5_tg_furniture.c`, `td5_tg_city.c` or `td5_tg_streets.c`.

So the bulk of the work is **mapping OSM tags and raster samples onto knobs that
already exist**, not writing emitters. Two exceptions, both chosen deliberately:
traffic-light decoration (new, small) and the plaza emitter (rewritten for real
polygons, §7 Phase 5).

**The hard part is not the plumbing. It is §5.**

## 2. Scale -- the number everything depends on

Four independent shipped constants agree on the world-unit-to-metre factor:

| Constant | Value (world units) | Real-world equivalent | Implied units/m |
|---|---|---|---|
| `TD5_TG_LANE_WIDTH` (`td5_trackgen.h:27`) | 1500 | a 3.5 m traffic lane | 428 |
| `TG_ROAD_BRIDGE_LIFT` (`td5_tg_road.c:41`) | 2000 | ~4.6 m road underpass | 435 |
| `TG_ROAD_TUNNEL_DEPTH` (`:40`) | 2200 | ~5.1 m of cover | 430 |
| `TG_WORLD_SHORE_BAND` (`td5_tg_world.h:66`) | 1200 | ~2.8 m above sea | 430 |

**Working figure: 1 metre ~= 430 world units.** Three consequences:

- `TG_WORLD_CELL` = `TD5_TG_SPAN_LENGTH` = 1500 units = **~3.5 m per span**. The
  default `LENGTH` of 1800 spans is **~6.3 km of road** -- a real city drive.
  1:1 real scale is viable; no compression needed.
- Hard ceiling `TD5_TG_MAX_SPANS` = 3000 (`td5_trackgen_internal.h:629`, clamped
  `td5_tg_pages.c:3480-3481`), so **~10.5 km maximum route**. `s_struct[]`
  (`td5_tg_road.c:63`) and `s_rn[]` (`:108`) are indexed by node with **no bounds
  check** in `tg_road_node_fill` (`:123-132`) -- past ~3008 nodes they overrun.
  The conditioner enforces the cap; it must not be left to the engine.
- A 3.5 m cell is what every source raster must be resampled to.

**Phase 0 measures this factor rather than trusting the derivation** -- drive a
car of known top speed for N fixed ticks under `RaceTrace=1` and divide world
units travelled by time. Everything is calibrated off this number, so it gets
its own step and its result is recorded here.

### Achieved fidelity per layer (the answer to "maximum precision")

With the sources of §6, at La Plata:

| Layer | Source resolution | World cells per sample |
|---|---|---|
| Roads, buildings, plazas, medians | OSM vector | exact, no resampling |
| Terrain | IGN MDE-Ar, **5 m** | ~1.4 -- effectively cell-native |
| Trees (position and height) | Meta/WRI canopy, **1 m** | ~12 samples **per** cell |
| Land cover / biome class | ESA WorldCover, **10 m** | ~2.9 |

The coarsest layer is land cover at 10 m, and it only drives class and biome
decisions, never geometry. **Terrain stops being the weak link**, which was the
concern that prompted revisiting the DEM choice.

### Small-scale ground texture still comes from the generator

`tg_w_raw` (`td5_tg_world.c:128-160`) is a sum of separable octaves:
continental (wl 420 000, ~977 m), mountain mask (260 000, ~605 m), ridges
(50 000, ~116 m), hills (60 000, ~140 m), **surface detail (9 000, ~21 m)**.

Replace the first four with the real DEM and **keep the surface-detail octave**.
This matters more for La Plata than anywhere: the Pampa is flat, so the 1.5x
exaggeration of a near-zero gradient is still near-zero, and the detail octave is
what keeps the ground from reading as a billiard table. Real relief where it
exists, synthetic texture where it does not.

## 3. Elevation: exaggerate 1.5x, then clamp

Order is fixed by decision: scale the real gradient by 1.5, then let the existing
cap clip it. With `TD5RE_AUTOTRACK_GRADE` defaulting to 120 (grade 0.12,
`td5_tg_pages.c:3506-3508`, absolute ceiling `TG_ROAD_GRADE_ABSMAX` 0.20 at
`td5_tg_road.c:46`), a real 8% slope becomes 12% and lands exactly on the cap. A
flat city stays flat and leans on the detail octave; a steep one is tamed for
free.

Two places where elevation is genuinely **driven** rather than capped, both of
which must keep working untouched: the straight chord across every structure run
(`td5_tg_road.c:1400-1413`) and the raised-cosine clearance hump over a water
deck (`:1414-1437`).

## 4. The data contract -- one on-disk intermediate

The C side must never know about HTTP, TLS, tile servers, Overpass or map
projections. One cached directory per place, in the spirit of `td5_assetsrc.c`'s
editable-source layer:

```
re/assets/geo/<slug>/
  PLACE.JSON     name, centre lat/lon, bbox, projection params, scale factor,
                 per-layer source + licence + vintage, fetch timestamp
  HEIGHT.R16     DEM resampled to the world cell grid; origin, cell size,
                 vertical datum, sea-level Y
  CANOPY.R8      canopy height, sub-cell resolution retained where available
  COVER.R8       land-cover class per cell (tree / grass / built / water / bare)
  WATER.R8       water mask, OSM hydrography cross-checked against COVER
  ROADS.JSON     road graph in world units: nodes, edges, lanes, oneway, surface,
                 median, sidewalk width, bridge/tunnel/layer, name, class
  BUILDINGS.JSON footprints in world units + height (real or estimated, flagged
                 which), roof:shape, building:part, landmark flag
  AREAS.JSON     plaza / park / pitch polygons with internal paths and beds
  SIGNALS.JSON   traffic-signal positions from highway=traffic_signals
  FORKS.JSON     detected fork candidates (median carriageways, comparable
                 alternative routes) with the user's confirm/reject per site
  ROUTE.JSON     the CONDITIONED drive (see §5): polyline in world units, rotated
                 onto +X, lead-in prepended, resampled to TD5_TG_SPAN_LENGTH,
                 certified non-self-crossing
  ROUTE_RAW.JSON the route as drawn, for re-conditioning and diffing
```

Everything upstream is Python (`requests`, `numpy`, `PIL` already available).
Everything downstream is C reading local files. The geo pipeline is then testable
with no game running, and the game with no network.

Since multiplayer is out of scope for now, this directory needs no determinism
guarantee across machines. Revisit if MP is ever wanted -- see §10.

## 5. The conditioning problem -- the real work

A real route breaks the invariant the entire generator is built on.
`td5_trackgen_internal.h:1332-1373` documents it, including what was tried and
rejected:

> The walk is **non-trapping** because with `|deviation| <= 80 deg` from the +X
> axis, every span advances the axis coordinate by at least
> `span_length * cos(limit) > 0`. That coordinate is strictly increasing, so two
> nodes far enough apart in span index **can never coincide** --
> self-intersection is geometrically impossible, not merely rejected.

Three facts from that comment set the difficulty:

- **Rejection sampling was tried first and traps.** A self-avoiding 2D walk
  paints itself into a cul-de-sac: 1800 spans requested, 300 delivered.
- **The failure at an overlap is not cosmetic.** The engine localises a car to a
  span by proximity, so the walker snaps to the wrong span and spawn, progress
  and ground-probe all break. Recorded symptom: *car placed at span 1791 of 1800
  on the start line, airborne.*
- A true `>= 180 deg` down-track hairpin is **deliberately not offered**, being
  incompatible with a single-axis monotone guarantee.

A real A-to-B city drive is monotone in no axis, so the guarantee is
unconditionally lost. The only remaining guard, `tg_too_close`
(`td5_trackgen.c:1760-1772`), is described in its own comment as *"a pure
backstop that never fires"* and exempts the last ~13 neighbours.

### The five invariants a raw polyline breaks

| # | Invariant | Where | Consequence |
|---|---|---|---|
| 1 | Node 0 at `(0,0)`, then 40 nodes straight along +X (`AXIS = PI/2`) | `td5_tg_road.c:733-741`, `:663-664` | the grid/start line, the structure ban for `s <= GRID_SPAN+24` (`:235`) and the `y[0]=0` spawn anchor (`:336`, `:1448-1453`) all live there |
| 2 | **No self-crossing of the main route** | `internal.h:1332-1348` | span walker snaps to the wrong span |
| 3 | Turn radius `>= (width/2) * curve_safety` | `td5_tg_road.c:638-641` | row quads fold (`td5_trackgen.c:2254-2258`); `tg_adjacent_skip` is *derived* from this floor |
| 4 | **Uniform** span length | `:130`, `:512-513`, `:562`, `:1465` | no per-node arclength field exists in `TG_Node`, so uneven spacing is silently mis-measured, never rejected |
| 5 | Fork windows | `td5_tg_branch.c:447-468` | benign: stateless in span index, a sharp bend just makes `tg_fork_place` skip that fork |

Invariant 3 is **not** the obstacle it looks like. At the default `CURVESAFE` of
180 with 2 lanes the floor is `(3000/2) * 1.8 = 2700` units = **~6.3 m radius**.
Real urban corners are 10 m and up, so the floor is permissive and a 90 degree
city corner spends 3 to 5 spans. Invariant 4 is resampling. Invariant 1 is a
translate-rotate plus a synthetic lead-in.

**Invariant 2 is the feature's one genuine unsolved problem.**

### Measured 2026-09-29: invariant 2 is far narrower than it reads

`re/tools/geo_condition.py --self-test` runs six synthetic routes around La Plata
through the real separation test. The result changed the risk assessment.

The engine's test is `need = (w_a + w_b) * 0.5 + lane_width * 0.25`
(tg_too_close, `td5_trackgen.c:1760-1772`). For a 2-lane road that is **3375
world units = 7.85 m**. It is a *physical* non-overlap test, so at 430 units/m
the separations that trigger it are tiny:

| Situation | Centerline separation | Triggers? |
|---|---|---|
| Two city streets one block apart (100 m) | 43 000 units | no, by 13x |
| Dual carriageway with a 10 m median | 4 300 units | no, marginally |
| Route doubling back 700 m away (hairpin) | 301 000 units | **no, by 89x** |
| Route retracing the same street (6 m) | 2 580 units | **yes** |
| Closed loop returning to its own start | 572 units | **yes** |

So the binding constraint is not "the route must be axis-monotone". It is **"the
route must not drive down the same piece of road twice, and must not cross
itself"**. Both are visible on a map and both are one waypoint drag to fix.

Two consequences:

1. **The heading budget is sufficient for safety, not necessary.** A route inside
   +-80 degrees inherits the engine's proof outright and needs no checking at
   all. A route outside it is not thereby unsafe -- it just has to be tested. The
   self-test confirms this: the `grid` staircase passes with 8 spans over budget,
   and `hairpin_wide` passes at a full 180 degrees of doubling back.
2. **Far more real routes are usable than the invariant's wording suggests.**
   Ordinary A-to-B city drives, detours and hairpins are fine. What fails is
   retracing and genuine self-crossing, which is exactly the set Option B exists
   to unlock (loops, figure-eights, flyovers).

This does not retire Option A -- the conditioner is still what enforces the
curvature floor, the uniform spacing, the lead-in frame and the cap, and it is
what *proves* a given route is safe rather than assuming it. It does mean the
"some real routes are unusable" cost is much smaller than the plan first
estimated.

Worth keeping in view: the curvature floor is what licenses the engine's
adjacent-skip exemption (pairs within 16 spans are not tested, because with the
floor holding the chord bound proves they cannot be too close). So enforcing the
floor is not cosmetic -- it is the premise the exemption rests on, and a real
polyline is the first thing that could violate it.


### Option A -- condition the route (ships first)

A Python conditioner that:
1. Runs PCA on the drawn polyline and **rotates its principal axis onto +X**. A
   typical A-to-B drive is elongated, so this alone makes most real routes
   monotone or near-monotone. Cheap, high leverage.
2. Translates so the route start meets the end of a synthetic 40-node lead-in
   tangentially.
3. Resamples to exactly `span_length` with a spline, enforcing the radius floor.
4. Tests every non-adjacent span pair for centerline separation below the sum of
   half-widths plus a quarter-lane -- the same test as `tg_too_close`, run
   offline where failing is cheap.
5. On a violation, **reports the crossing to the selector** so the user drags a
   waypoint to resolve it. One drag, and the draggable route mode is exactly the
   right UI for it.

Option A preserves the engine invariant exactly and needs no change to
`td5_track.c`. Its cost: some real routes are unusable, and a closed city loop
almost always is.

### Option B -- crossing-safe span localisation (with Phase 5, per decision)

Give the span walker either 3D awareness (use Y, so a flyover and the road
beneath it are distinguishable) or a span-continuity hint so it cannot snap
across a grade separation. This is what makes genuine city loops, cloverleaves
and out-and-back routes possible.

It touches the span walker in `td5_track.c`, core position tracking shared with
**every shipped track** and covered by the golden traces (`trace_goldens.txt`).
That is the right guard rail, not a reason to avoid it: a behaviour change on a
shipped track shows up immediately as a golden mismatch, and a change that is
genuinely additive for geo tracks leaves the goldens untouched.

Risk note on the chosen schedule: Phase 5 and Option B are the two hard pieces
and running them together concentrates risk in one window. Mitigation is that
they touch disjoint code (`td5_tg_network.c` vs `td5_track.c`) and have separate
acceptance gates (`tg_network_audit.py` vs the golden traces), so they can be
developed and verified independently even while scheduled in parallel.

Note that invariant 2 concerns the **main route** crossing itself. *Streets*
crossing the road are already handled by the NETWORK layer's underpass crossings,
so Phase 5 does not itself depend on Option B.

## 6. Real-world data sources

All free, all licence-compatible with a credit line, all sampled the same way.

| Layer | Source | Resolution | Licence | Notes |
|---|---|---|---|---|
| Roads, buildings, plazas, medians, signals, surface | **OpenStreetMap** via Overpass | vector | ODbL, credit required | rate-limited and intermittently down, so cache hard |
| Terrain, default path | **IGN MDE-Ar v2.1** (`proyecto = MDE 30 m`) | 30 m, ~2 m vertical | free, downloads unauthenticated | SRTM 2000 + ALOS fused, vertical datum **SRVN16**, national continental coverage. **Confirmed fetchable**: `pid=7` returns 200 + `application/octet-stream`, ~7.7 MB `.zip` per 1:100 000 sheet |
| Terrain, precision override | **IGN MDE 5 m aerophotogrammetric** (`proyecto = MDE 5m`) | **5 m**, sub-metre vertical | free, but **request-gated** | SAD flights (Vexcel UltraCam Xp + GNSS + IMU). **Confirmed to cover La Plata and Buenos Aires** (project `0008 - 2013 - AMBA - Sector 1.1`). **Not fetchable**: `pid=3` 302-redirects to FAQ #30 regardless of referer/UA; must be requested from asesoriatecnica@ign.gob.ar |
| Terrain, outside Argentina | global terrain tile service | ~30 m | free | one code path, same resampler |
| Trees: position **and height** | **Meta/WRI Global Canopy Height** | **1 m** | CC-BY 4.0 | global, on AWS S3 as cloud-optimised GeoTIFF, no AWS account needed; nominally 2018-2020 |
| Land cover (11 classes) | **ESA WorldCover** | 10 m | free, no use restriction | stated global accuracy ~75-77%; 2020 and 2021 editions |

**Resolved 2026-09-29 by querying the IGN's own WFS.** The coverage layer is
`ign:mde` on `https://wms.ign.gob.ar/geoserver/ows`, queryable by bbox, one
feature per downloadable tile with fields `nombre`, `proyecto`, `archivo` and
`link`. A bbox over La Plata (`-58.10,-35.05,-57.80,-34.80`) returns **16
features: 14 at `MDE 5m` and 4 at `MDE 30 m`**, so the primary region has 5 m
coverage and the national 30 m sheets underneath it.

Two things this settled, both of which changed the plan:

1. **Coverage of the 5 m product is partial, not national** -- the announcement's
   "national" framing does not hold, so `geo_fetch.py` must *discover* coverage
   per bbox rather than assume it. Doing that through the WFS costs one request
   and also yields the tile names and per-tile download links.
2. **The 5 m tiles are request-gated.** `pid=3` 302-redirects to FAQ #30
   ("todas las solicitudes deberán ser remitidas a Asesoría Técnica del IGN")
   with or without a browser referer and user agent, while `pid=7` (30 m)
   returns the file. So 5 m cannot be automated.

**Design that follows:** 30 m is the automated default everywhere, and 5 m is a
**local override slot** -- `geo_fetch.py` prefers any `.img` the user has dropped
into `re/assets/geo/_dem_override/`, and otherwise fetches 30 m. Since the
region that matters is AMBA, one email to Asesoría Técnica covers La Plata and
Buenos Aires permanently, after which the override slot makes those two fixtures
5 m offline and forever. `PLACE.JSON` records which source each tile came from,
so a terrain complaint is attributable to resolution rather than hunted in the
emitters.

The WFS query is worth keeping regardless of the gate: it is how the pipeline
knows whether a 5 m tile *exists* for a place, which is what tells the selector
to say "this place can be 5 m if you request the sheet" instead of silently
serving 30 m.

**Precaution to validate, not a claimed defect:** in dense urban areas, mask the
canopy raster with WorldCover's built-up class. A vegetation-height model can
read a tall building as tall canopy, and planting a tree on a rooftop is exactly
the kind of error that destroys the recognisability this feature is for.

### Where each source lands

- Canopy 1 m -> tree position **and per-tree height** into `td5_tg_terrain.c`'s
  flora emitters, which already take positions and now get real ones.
- WorldCover grassland -> plaza and verge grass, cross-checked against OSM
  `leisure=park`.
- WorldCover built-up -> the building-height estimator of §7 Phase 5, and the
  canopy mask above.
- WorldCover tree cover -> tree bands and forest lanes.
- WorldCover water -> cross-check and fill `WATER.R8` against OSM hydrography.
- WorldCover snow and ice -> can auto-set `TD5RE_AUTOTRACK_SNOW`.
- OSM `surface=*` -> pick among the 9 `TD5RE_AUTOTRACK_ROAD_SET` texture sets.
- OSM `sidewalk=*` and width tags -> `tg_city_sidewalk_w_at` per span.

## 6b. Built and verified 2026-09-29 (Phase 1 tooling)

`re/tools/geo_common.py`, `geo_raster.py`, `geo_fetch.py`, `geo_route.py`,
`geo_condition.py`, `geo_audit.py`. End-to-end on real La Plata data:
**15 audit checks pass, 1 warning, 0 failures.**

    geo_fetch    --name "La Plata" --lat -34.9215 --lon -57.9545 --radius 2200
    geo_route    --place la_plata --from A --to B --out route_raw.json
    geo_condition --in route_raw.json --out ROUTE.JSON
    geo_fetch    ... --frame-from ROUTE.JSON        # second pass, network cached
    geo_audit    --place la_plata --route ROUTE.JSON

Measured on a 5.20 km route across La Plata (Avenida 53, Diagonal 101,
Diagonal 102, Avenida 19): 1492 spans, 97% axis-monotone, worst turn smoothed
79.0 -> 30.9 deg, no self-crossing. Cache: 1651 roads, 1219 buildings, 472
traffic signals, 188 areas (60 park + 38 grass + 24 pitch), 71.7 m of relief.
45% of roads carry an OSM `lanes` tag; 95% of building heights are estimated,
matching the global tagging rates in section 9.

### Corrections this work forced on the plan

**The IGN 30 m product cannot be the automated default.** Both IGN elevation
products ship as ERDAS IMAGINE HFA (the file opens `EHFA_HEADER_TAG`), which
needs GDAL, and this environment has tifffile, numpy, PIL and shapely but no
GDAL, rasterio or pyproj. The default is therefore **Terrarium PNG terrain
tiles** -- plain PNGs, `(R*256 + G + B/256) - 32768` metres, global, no
dependency. Verified over La Plata: -2..61 m, median 16 m, 15.7 m/px at z13.
The IGN 5 m path is unchanged in spirit but now explicitly a **local override
slot** that takes a GeoTIFF or PNG, since the sheet has to be requested and
converted once anyway.

**ONE FRAME FOR THE WHOLE CACHE.** The conditioner rotates the route so its
start tangent lands on +X and TRANSLATES it so node 0 sits at the origin
(`td5_tg_road.c:733` requires that). The vectors and the rasters must be built
in that same frame. The first version shared neither, and nothing raised an
error -- `geo_audit` R8 found 896 of 1451 route nodes outside their own terrain,
and R9 (frame equality) now exists to catch it directly. Pipeline order is
consequently **fetch unrotated -> route -> condition (which DECIDES the frame)
-> re-fetch with `--frame-from`**, and the second pass is free because every
network response is cached. The raster header carries `rotation_rad` purely so
this can be asserted.

**Resample by CHORD, not arc.** The engine integrates
`x += sin(h)*span_len; z += cos(h)*span_len` (`td5_tg_road.c:663-664`), so its
nodes are one span apart in straight-line distance. Arc-length resampling puts
them one span apart along the path, and at a 31.6 deg corner the chord falls 34.6
units short -- which `geo_audit` R4 failed on. The resampler now places each next
point where the circle of radius `span_length` about the previous one first
crosses the polyline.

**A rotation-sign bug was corrupting every orientation measurement.**
`_rotate(pts, theta)` maps a heading phi to `phi - theta`, so carrying the start
tangent onto +X needs `theta = h0 - pi/2`, not `pi/2 - h0`. It went unnoticed
because the audit's lead-in check (R3) only inspects the SYNTHETIC lead-in, which
is +X by construction, so nothing tested the body's first heading. With the sign
corrected the same La Plata route reads **97% axis-monotone with its principal
axis 0.9 deg off the start tangent**, where before it read 24% monotone and 83.7
deg off. The earlier reading was entirely the artefact -- real A-to-B city routes
are far better behaved than that suggested.

**New constraint found: the AI route table's heading dead zone.** ROUTES.DAT byte
1 is an absolute 12-bit heading, `hb = round(h12 * 256 / 4140)` clamped to 4..253
because `byte < 4` is a junction-zone sentinel (`tg_emit_routes`,
`td5_trackgen.c:2598-2640`; recovered as `(byte * 0x102C) >> 8` in
`td5_ai.c:1280`). The generator never meets it -- its +-80 deg budget about +X
keeps every heading clear. A route that wanders through **+Z** does, and those
spans carry a heading up to **5.7 deg** wrong. The clamp avoids emitting a
sentinel, so this is bounded angular error in AI routing and not a geometry
failure: reported as a quality metric, not a rejection. Measured 40 spans on the
La Plata route.

**The curvature limit must be LOCAL.** Using the widest road's turn limit
everywhere over-smooths narrow streets: 2 lanes tolerate 32.3 deg per span, 4
lanes 16.0, 12 lanes only 5.3. Per-node widths raised the preserved worst turn
from 20.5 to 31.6 deg on the same route, which is exactly the tight residential
corners that make a city recognisable.

## 7. Phases

### Phase 0 -- calibration and ground truth
- Measure world-units-per-metre by trace; record it here.
- **Confirm IGN 5 m coverage for La Plata and Buenos Aires** in the download
  tool (§6).
- Commit both fixtures (Buenos Aires, La Plata) as cache directories so every
  later phase demos offline and the audits have real inputs.
- Set the bbox cap from the measurement (a 6.3 km route wants roughly a 4 x 4 km
  box with margin; hard ceiling 3000 spans / ~10.5 km).

La Plata is a strong primary fixture and worth saying why: it is a planned city
whose signature is a regular grid cut by diagonal avenues, with plazas placed at
regular intervals by design. That exercises, in one place, the non-90-degree
intersection machinery (the diagonals), the plaza work (many, regular, with
internal paths), and median-avenue forks. Being flat, it also isolates the city
layers from the terrain layers, which makes failures easier to attribute. Buenos
Aires adds scale and denser tagging. Neither exercises bridges, tunnels or real
gradient, so a hilly coastal third fixture is worth adding once Phase 3 lands.

### Phase 1 -- the Python geo pipeline and the conditioner (no game changes)
`re/tools/geo_fetch.py` + `re/tools/geo_condition.py`:
- Overpass query for highways, water, coastline, landuse, buildings, plazas and
  traffic signals in a bbox, cached hard on disk.
- DEM fetch (IGN 5 m in Argentina, global tiles elsewhere) resampled to the cell
  grid; canopy and land-cover rasters likewise.
- Projection: local transverse Mercator about the route centre, so metric
  distortion over a 4 km box is negligible.
- Road graph build and simplification; class and tags to lanes, surface,
  sidewalk width, median.
- Routing: A-to-B shortest path plus **waypoint insertion and re-route**.
- **Fork detection**: median carriageways, and pairs of routes with comparable
  length to the destination, emitted as candidates for the user to confirm.
- Building heights: real `height`, else `building:levels` x storey height, else
  **estimated from footprint area, land use, street class, built-up density and
  distance to centre**, with every building flagged as measured or estimated.
- The **conditioner** of §5 Option A. This is the deliverable that matters.
- An audit in the style of `tg_network_audit.py`, exit 1 on: self-crossing,
  radius below floor, non-uniform spacing, route off the DEM box, span count
  over cap, route too short to hold a grid plus a race plus a run-off.

### Phase 2 -- WORLD reads the real rasters
Smallest C change with a visible result: a real place's **terrain**, with the
existing synthetic road walked over it.
- `tg_world_build(seed, target_spans)` keeps its signature; its body branches on
  "is a geo cache loaded". The geo path loads `HEIGHT.R16` + `WATER.R8` +
  `COVER.R8`, sets `s_w.sea_abs` from the real datum, and takes its origin from
  `ROUTE.JSON` instead of the 900-offset spiral search
  (`td5_tg_world.c:573-605`).
- `tg_w_raw` gains a geo branch: bilinear DEM sample plus the retained detail
  octave, with the 1.5x exaggeration applied here.
- `tg_world_water_y` / `tg_world_is_water` read `WATER.R8` instead of the river
  noise field.
- `tg_world_class` keeps its slope thresholds -- they work on a real DEM -- with
  `COVER.R8` overriding to city, grass, shore and so on.
- **Zero changes to any of the 130 call sites.**

Also Phase 2, near-free since both already exist: a `set_location` /
`geo_status` command on the live-control socket (`td5_control.c`) and a matching
tool in the `td5re` MCP server (`scripts/td5re_mcp/`), so the feature is
automatable and testable from the first day it works.

### Phase 3 -- ROAD follows the conditioned polyline
The seam is precise. **Stub exactly one thing: the section loop body of
`tg_build_centerline`, `td5_tg_road.c:744-974`.** Keep the signature
(`internal.h:1385`) and keep, unchanged:
- the prologue `:715-723` (world ready, `s_spec`, `s_max_grade`, table resets) --
  the profile and structure code reads these statics;
- `tg_nodes_push` (`td5_trackgen.c:1721`) as the append, so the preview hook and
  the default field values keep working;
- a `tg_road_revise(nl, NULL, NULL)` call every 64 nodes or fewer as nodes are
  appended (mirroring `:742` / `:964`), so `s_rn`, `y` and `s_struct` fill in
  windowed exactly as today, plus `tg_road_finalize_to` (`:970`);
- the tangent pass `:993-1005`.

**Do not touch** `tg_road_solve` (`:152`), `tg_road_classify` (`:222`),
`tg_road_revise` (`:327`), `tg_road_finalize_to` (`:575`) or `tg_apply_elevation`
(`:1374`). They are already polyline-agnostic: they read only `x, z, width` and
write `y`.

Goes unused, with no other callers: `tg_walk_push_section` (`:615`),
`tg_walk_save` (`:605`), `TG_SectionPlan` (`:588`), `tg_road_guide_plan`
(`:435`), `tg_road_cost` (`:533`), `tg_road_lookahead_cost` (`:504`),
`tg_pick_section` (`td5_trackgen.c:1874`).

Per-node fill, all via `tg_nodes_push` plus the kept tangent pass:

| field | read by | requirement |
|---|---|---|
| `x, z` | everything | resampled to exactly `spec->span_length` per segment |
| `y` | strip `:2257`, chords `:1407`, conform `:1573` | leave 0; `tg_road_revise` / `tg_apply_elevation` write it |
| `width` | `:2254`, `:1569`, `:1574`, `tg_too_close` | `lanes * spec->lane_width` |
| `lanes` | `tg_row_points`, `tg_span_type_for` | from OSM `lanes` / highway class |
| `lane_base` | span byte 3, `:2290` | `TD5_TG_HEIGHT_NIBBLE` (8), free from `tg_nodes_push:1730` |
| `lane_side` | `tg_span_type_for`, seam trim `:257` | 0 unless emitting transition types |
| `jx, jz` | tangent pass only | 0 |
| `tx, tz` | strip `:2249`, `tg_emit_routes:2626` | filled by the kept pass `:993-1005` |

**Structures need no OSM tags to work.** Bridge and tunnel are decided *during*
the walk, by `tg_road_revise` into `tg_road_classify`, from `tg_road_node_kind`
(`:207-216`): wet gives BRIDGE, more than 2200 under the ground gives TUNNEL,
more than 2000 over it gives BRIDGE. On a **real** DEM those predicates fire
where reality puts them -- a route crossing a real river is wet, so it gets a
real bridge for free. OSM `bridge` / `tunnel` / `layer` tags are a **refinement**
adding what terrain cannot infer, chiefly flyovers over dry land.

Fork confirmation from `FORKS.JSON` also lands here, since the existing
carriageway query and long diverging branch live in `td5_tg_branch.c`.

### Phase 4 -- the map selector (browser), mandatory
A local HTML page, Leaflet over OSM tiles, **skinned to TD5's frontend
aesthetic** rather than looking like a stock web map:
- pan and zoom the map;
- **city and place search**;
- **paste coordinates** (accept bare `lat, lon`, an OSM permalink and a Google
  Maps URL);
- click A and B, then **drag the line to insert waypoints and re-route**;
- draw the conditioned route, and **mark any self-crossing** Option A found so it
  can be dragged away;
- show detected **fork candidates** (median avenues, comparable alternative
  routes) as toggles the user confirms;
- show the bbox, the span count against the 3000 cap, and the per-layer data
  source and vintage.

The draggable route makes this the selector, not merely the preferred one:
dragging a route is not a thing to do with a controller. It also avoids putting
TLS in the game, caching tiles, or touching the OSM tile usage policy.

A `LOCATION` row joins `k_at_rows` (`td5_fe_race.c:7286`) showing the selected
place, and `td5_track_registry_set_auto(slot, level, name, ...)`
(`td5_track_registry.h:45`) already takes a name, so the track appears in the
frontend as the real place name rather than "AUTO".

### Phase 5 -- real streets, real buildings, real plazas (+ Option B in parallel)
The largest phase, and where recognisability is won.

- **NETWORK from real streets.** `td5_tg_network.c` is the "(span,side) street
  authority" and its mouth table feeds `tg_facade_built`, `tg_xstreet_here`,
  `tg_xstreet_reach_at`, `tg_block_arm_skew` and `tg_r12_fcross_at`. The existing
  code **plants** streets wherever the occupancy raster has room; the geo version
  must **accept** streets that already exist and may not satisfy those rules.
  La Plata's diagonals make non-90-degree mouths a first-class case, not an edge
  case.
- **Real building footprints and heights**, with the estimator of Phase 1 filling
  the untagged majority and each building flagged measured or estimated.
- **Landmarks**: extrude the real footprint with `height`, `roof:shape` and
  `building:part` where the 3D tags exist, and fall back to the existing
  `tg_landmarks_place` prefab table where they do not. Famous buildings are the
  subset where OSM 3D tagging is well above average, because mappers invest
  effort there -- so this path pays off more than the global tagging rate
  suggests.
- **Plazas rewritten for real polygons**: exact outline, internal paths, beds,
  and interior trees from the 1 m canopy raster. This replaces the gap-based park
  emitter at `td5_tg_streets.c:552-589` rather than reusing it, which is the one
  place a decision deliberately bought fidelity at the cost of a rewrite. La
  Plata, with regular designed plazas, is the right place to prove it.
- **Traffic lights**: the one new emitter. Mesh plus emissive at
  `highway=traffic_signals`, cycling visually, obeyed by nobody.
- **Option B** (§5) proceeds in parallel, in `td5_track.c`, gated on the golden
  traces.

Sequencing note: Phase 3 ships first and is independently demonstrable (a real
route through procedural surroundings), so Phase 5 slipping does not block
everything behind it.

### Phase 6 -- optional: in-game map screen
Technically unblocked. The port already has a PNG decoder
(`td5_png_decode_fast`, `td5_asset.c:1766`) and a complete raw-Winsock HTTP/1.1
client (`upnp_http_exchange`, `td5_upnp.c:217`; `upnp_tcp_connect`, `:165`). The
AUTO TRACK STUDIO screen already hosts a live, debounced, background-worker route
preview with pan and normalise (`at_preview_request` / `at_preview_tick`,
`td5_fe_race.c:7607` / `:7645`), so a raster map underlay has an obvious home.

Two real blockers, which is why it is last and optional: canonical OSM tile
servers are HTTPS-only (the existing client is plaintext, so this needs WinHTTP
or a permitted plaintext mirror), and the tile usage policy restricts application
use. Neither blocks Phases 1 to 5, and the browser selector remains the only one
that can drag a route.

## 8. Route selection -- point to point with draggable waypoints

The user clicks A and B, gets the routed line, and drags it to insert waypoints
that re-route, like the Google Maps route editor. This suits the engine: the
generator default is already point-to-point (`spec.circuit = 0`), and an
elongated A-to-B route is the case the PCA rotation of §5 handles best. It is
also the right UI for invariant 2 -- a crossing is marked and resolved in one
drag.

Later modes, in this order: follow a single named OSM way end to end (trivial,
and gives the most spectacular places), then loop-from-a-point (best for circuit
races, wants iterative deepening with a turn-angle penalty rather than a
shortest-path call, **and** it is the mode that most needs Option B, since a city
loop almost always self-crosses).

## 9. Risks and specific gotchas

- **Invariant 2 is the schedule risk.** Everything else is mechanical. Budget the
  conditioner properly and treat the crossing report as a first-class UI feature,
  not an error path.
- **Phase 5 and Option B run in parallel by decision**, concentrating both hard
  pieces in one window. They touch disjoint code with separate acceptance gates,
  so keep their branches and verification separate even so.
- **The plaza rewrite is the one place fidelity was bought with a rewrite.**
  Scope it explicitly rather than letting it grow inside Phase 5.
- **Building heights are mostly estimated, not measured.** Global `height`
  coverage is under 10% and by some analyses near 3%; `building:levels` is around
  5% globally, with wide regional variation (Paris above 50%). Footprints are
  near-universal, heights are not. Flag every building's provenance so a bad
  skyline can be attributed to the estimator rather than hunted in the emitter.
- **Synthetic builds must stay byte-identical.** The geo path must be a distinct
  code path and must not perturb the single RNG stream -- the standing rule at
  `td5_trackgen_internal.h:1290-1296`: never add a `tg_rand`/`tg_frand`/
  `tg_range` call, because an extra draw moves the road for every existing seed.
  Verify by the established method: compare MODELS.DAT bytes, one knob at a time,
  via `verify/topo_gen.ps1 -Seed N`.
- **The audit harness already exists and should be reused unchanged**:
  `re/tools/tg_network_audit.py` (planarity, street-off-road, mouths off
  structures, structure lengths and interlock, grade, water-without-deck),
  `re/tools/tg_strip_audit.py`, `verify/topo_gen.ps1`, `verify/span_capture.py`.
  A geo build that passes these will not fall over. That is the acceptance
  criterion and it costs nothing to adopt.
- **OSM data quality varies wildly.** Rural coverage is sparse; a dense centre
  holds ten thousand ways in a 2 km box. Needs the bbox cap, a way-count cap, and
  a graceful "not enough road here" path in the selector rather than a failed
  build.
- **Raster vintage differs per layer** (canopy nominally 2018-2020, WorldCover
  2020/2021, IGN flights 2011-2016, OSM current). A tree felled in 2022 is still
  there, a street opened in 2024 has no terrain under it. Record each layer's
  vintage in `PLACE.JSON` so a surprise is explainable.
- **Attribution is mandatory.** OSM is ODbL, canopy height is CC-BY 4.0. An
  on-screen credit is required wherever a geo track is shown, and every layer's
  source belongs in `PLACE.JSON`.
- **Do not build at module init.** `td5_trackgen_init` currently builds a track
  so the selector entry exists from the main menu on, and boot already takes tens
  of seconds. A geo build adds DEM, raster and graph loading, so the geo path
  must be on-demand only.
- **`level090` is written relative to the CWD**, and two generators in one
  worktree share it. Unchanged hazard, but a geo build is slower, so the
  collision window is wider.

## 10. Open

1. **A hilly coastal third fixture** once Phase 3 lands -- neither Buenos Aires
   nor La Plata exercises bridges, tunnels or real gradient.
2. **Multiplayer**, if ever wanted: the netplay is lockstep and needs
   MODELS.DAT identical byte-for-byte on both peers, which a fetched cache does
   not guarantee. Host-ships-the-cache is the only option that survives OSM data
   changing over time.
3. **Storey height** for the `building:levels` fallback -- one global constant,
   or per-country, or per land-use class?
