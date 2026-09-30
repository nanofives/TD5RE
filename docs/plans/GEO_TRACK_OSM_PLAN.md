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
| Multiplayer | **Single-player only.** Out of scope by decision, and out of bounds by the no-networking rule (see 7 Phase 6) |
| Networking | **NONE.** No sockets, no net tests, and `td5_net.c` / `td5_upnp.c` / `td5_fe_net.c` are not to be touched. In-game map tiles are ruled out; the browser selector is the selector |
| Test runs | **No RT, minimum graphics** for anything that is not the selftest suite: `TD5RE_RT=0`, never `TD5RE_AUTO_PERF=2`, and the 16 graphics flags in 7 Phase 0. Watch the `td5re.ini` write-back |
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

## 6c. Phase 2 shipped and verified 2026-09-30

`td5_geo.c/.h` plus THREE branches in `td5_tg_world.c` (`tg_w_raw`,
`tg_world_water_y`, `tg_world_build`). Everything else untouched: `tg_world_h`,
`_h_base`, `_slope`, `_is_water`, `_class`, the whole conform/occupancy overlay,
and all 130 call sites.

**Byte-identity proven, not asserted.** Built a pre-geo baseline exe from the
previous commit and ran `verify/topo_gen.ps1 -Seed 20260901` against both. All
eight generated level files are byte-identical -- STRIP.DAT, LEFT/RIGHT.TRK,
LEVELINF.DAT, MODELS.DAT (`98E749869051ACE3`, 10271488 bytes), TEXTURES.DAT,
MESHTAG.BIN, NETWORK.JSON. Only GENSTAMP.TXT differs, and it carries an `exe=`
identity hash, so two different binaries must differ there; since the eight data
files match, its `spec=` and `env=` fields necessarily agreed.

`verify/topo_gen.ps1` gained `-MinGfx`, default ON (RT off + the 16
minimum-graphics flags), per the standing run rule. A switch rather than
unconditional because that script's own `-Race` + `TD5RE_FRAMEDUMP` path exists
to look at visuals.

### The finding that mattered: SRTM is a SURFACE model, so it reads rooftops

The first geo build of La Plata came out with its grade profile PINNED at the
absolute ceiling -- p90 0.1550, p99 and max both 0.1999 against
`TG_ROAD_GRADE_ABSMAX` 0.20 -- and classified the terrain `flat 61% hill 36%
mountain 1%`. For a city on the Pampa that is wrong.

Measured rather than guessed: the DEM held **4.8 m steps between adjacent 3.49 m
cells** (138% grade), 297 cells from the nearest edge, so not a clamping
artefact. Terrarium is SRTM-derived and SRTM is a **surface** model: over a city
it measures rooftops, not ground.

Filtering at the source's native 30 m GSD barely helped (p99 0.227 -> 0.198),
because the artefacts are whole **city blocks** -- 30 to 80 m of rooftop plateau
-- so a 31 m window cannot touch them. Measured the trade directly:

| lowpass | relief kept | p99 grade x1.5 | worst cell |
|---|---|---|---|
| 30 m (native) | 67.6 m | 0.297 | 0.993 |
| 60 m | 57.4 m | 0.208 | 0.543 |
| 120 m | 44.2 m | 0.127 | 0.273 |
| **200 m (default)** | **33.1 m** | **0.076** | **0.153** |
| 500 m | 24.6 m | 0.037 | 0.047 |

**200 m** is the first width where the exaggerated p99 clears the default
`TD5RE_AUTOTRACK_GRADE` cap (0.12) and the worst cell clears the absolute ceiling
(0.20). Real relief survives because it lives at a much larger scale -- a
mountain pass turns over 500 m to 2 km. `--dem-smooth-m` overrides it; 0 disables.

After the fix, the same seed on the same place: p90 0.0602, max 0.1833, terrain
`flat 96% hill 2%`, 0 bridges (that route crosses no real water), and 1800 open
spans conformed with **zero cut and zero fill** -- the road follows the ground
instead of fighting it.

**This also raises the value of the IGN request.** Their 5 m product is published
as MDE *and MDT* -- Modelo Digital de **TERRENO**, bare earth. That is not merely
6x finer than Terrarium, it is the right KIND of model, and it needs no smoothing
at all. The gated sheet is therefore worth more than the resolution number
suggests.

## 6d. Phase 3 shipped and verified 2026-09-30 (offline, synthetic relief)

The ROAD now follows a conditioned polyline. Built with no network: the La Plata
place cache went with the geo-track worktree, so the test used the committed raw
fixture conditioned offline (`re/tools/geo_fixtures/la_plata_ROUTE.json`) over
the synthetic world.

- `td5_geo` loads the route: `TD5RE_GEO_ROUTE=<ROUTE.JSON>` (no place needed), else
  `re/assets/geo/<slug>/ROUTE.JSON` of the loaded place. It validates the contract
  and rejects rather than half-uses: node 0 at the origin, `span_length` 1500,
  every chord 1500 +-1, at most 3001 nodes.
- `tg_geo_apply_spec` (after `tg_rolls_apply_spec` at all three spec-fold sites:
  build, studio preview, streamed rederive) sets `target_spans` to the route and
  forces point-to-point, before the GENSTAMP spec hash.
- `tg_geo_walk` in `td5_tg_road.c` replaces only the lead-in + section loop of
  `tg_build_centerline`: `tg_nodes_push` per node, `tg_road_revise` every 32 nodes,
  forced conform where a structure would exceed its cap (the walk's own last
  resort), `tg_road_finalize_to`, then the kept tangent pass. No RNG draw.
  `tg_too_close` re-checks the route and the count is logged.

Verified:
- **Synthetic byte-identity:** seed 20260901, master exe vs this build, all 8
  level files identical (MODELS.DAT `98E749869051ACE3`); only GENSTAMP differs (exe id).
- **Geo build:** 1493 of 1493 nodes, 1492 spans, 0 too-close nodes, finish span
  1292 with 200 of run-off, 6 bridges / 89 spans (3 over synthetic water), one
  32-span chunk forced to conform (a 64-span bridge over its 56 cap).
- **Audits:** `tg_strip_audit` 4 lane seams, 0 violations. `tg_network_audit`
  planarity 0 crossings, 0 street points on the road, 0 mouths on structures,
  0 structure-length violations, worst grade 0.200, 0 undecked water spans: OK.
- **Deterministic:** two geo builds, identical level files.
- **Race:** loads and runs, car on the grid at span 15.

Open for Phase 3 with real data: re-fetch La Plata (network, ask first), then
confirm bridges land on real water and the conformed chunk goes away.

## 6e. Phase 4 shipped 2026-09-30 (selector + game side)

**Selector.** `python re/tools/geo_selector.py` from the repo root, then open
`http://127.0.0.1:8765/`. Stdlib HTTP server bound to 127.0.0.1 only, serving
`re/tools/geo_selector/index.html` (Leaflet over OSM tiles, TD5 skin). Search a
city (Nominatim, from the browser) or paste `lat, lon`, an OSM permalink or a
Google Maps link; FETCH MAP CENTRE runs `geo_fetch` for a circle (1.5 to 4 km);
click A and B; click the line to add a waypoint, drag markers to re-route,
right-click to remove. Every change routes + conditions and shows the verdict,
spans against the 3000 cap, worst turn, straight-ahead share, street names, and
a red marker on every self-overlap. SEND TO GAME writes ROUTE_RAW.JSON +
ROUTE.JSON, rebuilds the rasters in the route's frame and writes SELECTED.TXT.

Frame fix found on the way: `geo_fetch --frame-from` takes the route's rotation
and offset but keeps the PLACE centre as the projection origin, while the
conditioner's origin is the ROUTE centroid. The selector passes the route's
origin explicitly (radius widened to still cover the original area). The CLI
recipe in 6b has the same trap when `--lat/--lon` differ from the route centroid.

**Game.** `re/assets/geo/SELECTED.TXT` fills an unset `TD5RE_GEO_PLACE` at boot;
`td5_geo_sync()` (inside `tg_geo_apply_spec`, so at every spec fold) loads or
drops the place and its ROUTE.JSON to match the knob, so no restart is needed.
AUTO TRACK STUDIO > TERRAIN > **LOCATION** cycles SYNTHETIC plus every place
with a ROUTE.JSON and writes SELECTED.TXT. The track registers under the real
place name, and the studio preview shows the OSM credit while a place is set.

Verified offline: `geo_selector.py --self-test` (synthetic grid place: state,
route + condition, retrace flagged with 2 markers, save, 404); synthetic build
still byte-identical to master; a flat test place selected only through
SELECTED.TXT built the full 1492-span route, 0 forced conforms, registered as
the place name. Not verified here (needs network or a person): the page in a
browser, FETCH, and the LOCATION row on screen.

Still open from the Phase 4 list: fork-candidate toggles (FORKS.JSON) and the
per-layer vintage display.

## 6f. Phase 5 shipped 2026-09-30 (real La Plata data, one authorised fetch)

La Plata fetched once into the MAIN tree (`re/assets/geo/la_plata`, outside any
worktree so it cannot be lost again), route saved through the selector in its
own frame; `geo_audit` 15 pass / 1 warning / 0 failures. Four parallel
workstreams, each on its own branch, merged here:

| Part | What | La Plata numbers |
|---|---|---|
| G1 streets (`td5_geo_roads.c`, `td5_tg_network.c`) | real OSM ways become mouths/crossings/underpasses; skew cap 65 deg (synthetic 28); march stand-off scales 1/cos(skew) | 94 arms considered, 53 accepted after the biome fix (25 before); up to 51 deg diagonals |
| G2 buildings/landmarks/plazas (`td5_geo_buildings.c`, `td5_tg_city.c`, `td5_tg_streets.c`) | real footprints extruded (measured vs estimated kept), tagged landmarks extruded, plaza polygons replace the gap park emitter on the geo path | 54 buildings (12 measured / 42 estimated), 2 landmarks, 13 plazas |
| G3 traffic lights (`td5_geo_signals.c`, `td5_tg_furniture.c`, render hook) | head at each signal node near the route, lenses tagged 4/5/6 on the shipped glow page, colour written per frame from a wall clock; cosmetic | 566 in cache, 25 near route, 11 placed |
| G4 Option B (`td5_track.c`, `geo_condition.py --allow-crossings`) | `TD5RE_XSPAN`: the unhinted global localiser takes the previous span as a hint on geo tracks | closed loop: 564 -> 132 snap gone; Moscow + TD6 sim identical tick by tick |
| biome (`td5_tg_terrain.c`) | geo cells from COVER.R8 (>=35% built CITY, >=40% tree FOREST, else FIELDS) | 9 city + 1 fields; street biome drops 37 -> 1 |

Gates on the merged branch: synthetic seed 20260901 byte-identical (8 files),
network + strip audits OK on La Plata, structure lint OK (83/84).

Follow-ups done 2026-09-30 (before merge):
- Traffic lights verified in race: heads beside the kerb at spans 696/702, the
  lit lens went green -> red within 4 s. Placement spans are now logged
  (`[GEO SIGNALS]   head N: span S side`).
- Plaza paths: G2's three Y tiers hold; a full-resolution frame of the plaza at
  span ~399 shows clean lawn/path edges, no flicker. Plaza first spans are now
  logged (`[GEO PLAZA]   area N first at span S`).
- Selector: the first SEND TO GAME pins the routing area in PLACE.JSON
  (`route_graph_bbox`), so re-routing a saved A/B over the wider re-fetch gives
  the same route (La Plata 1492 spans before and after; was 1200).

Still open at the 6f merge, all closed 2026-09-30:
- Plaza lawn texture stretched along long strips: closed in 6g (the hedge and
  beds were what stretched, not the lawn).
- Landmark prefab fallback and roof:shape only exercised on 2 buildings:
  closed in 6g (20-building fixture, six defects fixed).
- `TD5RE_AUTOTRACK_STREAM=0` with REUSE at its default produced one build with
  no TEXTURES.DAT in level090.zip and no scenery. **Not STREAM, not REUSE.** It
  was the R22 SCENERY presence roll, OFF on 12% of seeds (`k_tgr_w_scarce`),
  and an OFF SCENERY deletes MODELS.DAT and TEXTURES.DAT by design. The harness
  never saw it because it pins seed 20260901 (SCENERY ON). Repro: La Plata,
  seed 20260902. Fix: on a geo track (TD5RE_GEO_PLACE or TD5RE_GEO_ROUTE set)
  the resolver holds the rows that describe the real place ON (SCENERY,
  TERRAIN, BACKDROP, COASTLINE, BRIDGES, TUNNELS, DISTRICTS, BUILDING MASS,
  CROSSINGS, SIDE STREETS, ROAD MARKS, INTERSECTIONS, SIDEWALKS) and SNOW OFF,
  human pins excepted, logged as `HELD by the real place (GEO)`. Seed 20260902
  now builds 373 meshes; seed 20260901 and synthetic builds byte-identical.

## 6g. Plaza texel density and landmark coverage, closed 2026-09-30

The two Phase 5 follow-ups from 6f, done offline on `la_plata` plus a new
synthetic fixture. Files: `td5_tg_streets.c`, `td5_tg_city.c`,
`td5_geo_buildings.c/.h`, `td5_tg_prefab.c`, and `re/tools/geo_fixtures/land_*`.

### Item 8 -- the plaza was not stretched where the report said it was

Measured before touching anything, with a new offline probe
(`re/tools/geo_fixtures/land_models_probe.py`, which fits the plane-to-UV
Jacobian of every face and reports its two principal texel densities).
Plaza Mariano Moreno, span 701:

| page | faces | units/repeat | anisotropy p50 | worst |
|---|---|---|---|---|
| 65 lawn | 27 | 3400 | 1.00 | 1.2 |
| 2 beds (GREEN) | 8 | 12336..58912 | 3.13 | 4.7 |
| 66 boundary hedge | 23 | 520..40219 | **13.46** | **78.0** |
| 44 paving + paths | 184 | 1496 | 1.01 | 1.7 |

The LAWN was never the stretched surface: it was already planar in world XZ,
just 2.3x coarser than the generator's own grass. What followed the outline
were the **hedge** (u 0..1 per ring edge, so a 94 m edge got one repeat) and
the **beds** (u,v 0..1 across a whole wedge). Everything is now pinned to
`TD5_TG_GEOP_TILE` = the span length, which is exactly the density
`tg_block_emit_park` lays for a procedural park.

| page | after | anisotropy p50 | worst |
|---|---|---|---|
| 65 lawn | 1500 | 1.00 | 1.2 |
| 2 beds | 1500 | **1.00** | 1.0 |
| 66 hedge | 520..1502 | **2.88** | **2.9** |
| 44 paving | 1496 (unchanged) | 1.01 | 1.7 |

The residual 2.88 on the hedge is the procedural park hedge's OWN ratio
(1500 along / 520 up), which is the reference, not a defect. The paving was
already isotropic and was left alone. `TD5RE_GEO_PLAZA_TILE=0` restores the
old UVs, so the before/after is one variable on one exe.

### Item 7 -- landmarks, exercised on a fixture because the real data is thin

Why a fixture: the La Plata cache carries **4** `roof:shape` tags in 2047
footprints (3 mansard, 1 flat), **zero** `building:part`, **zero**
`min_height`, and 2 of its 10 tagged landmarks within 100 m of the route. The
whole path shipped exercised twice, and a richer city needs the network.
`re/tools/geo_fixtures/land_fixture_build.py` writes a place whose road,
terrain and route are La Plata's, byte for byte, and whose BUILDINGS.JSON is
20 controlled cases one per span: every roof:shape the reader accepts, a
three-part stack with min_height, two tagged landmarks with no 3D tags, an
L-shaped (concave) footprint, and the degenerate rings the emitter has to
refuse.

What the fixture found, and what was fixed:

1. **`min_height` never arrived.** Overpass returns every tag as a string and
   `geo_fetch` stores it raw, so the reader's `cJSON_IsNumber` check read 0
   for `min_height_m` and `roof_height_m` on every footprint in every cache.
   `geob_num_tag` now accepts a leading numeric prefix ("12", "12 m").
2. **A part was extruded its FULL height above its base.** OSM's `min_height`
   is the base and `height` is the top; the emitter added the base and then
   extruded the top, so a part tagged 20..34 m came out 34 m tall starting at
   20. A three-part stack was three times the building.
3. **Every sloped roof was a centroid pyramid.** Gabled, hipped, skillion and
   mansard all got a tent. `tg_geo_roof_ridge` now builds one ridge per shape
   -- full-length (gabled), inset at both ends (hipped), at the far edge
   (skillion), collapsed to a point (pyramidal, dome, onion, round) -- and a
   mansard gets a truncated pyramid with a flat deck. One loop, one
   parameter; a zero-length ridge reproduces the old pyramid exactly.
4. **`roof:height` was ignored.** Tagged, the eaves now sit at
   `height - roof:height` (OSM's own reading) instead of the roof stacking on
   top; untagged, the old default pitch above the walls is kept, because
   subtracting a made-up rise from an ESTIMATED height would shorten the
   building for nothing. Wall storeys are repeated over the wall, not over
   the building, so a tagged roof no longer squashes the windows.
5. **A non-simple ring produced garbage.** `td5_geob_ring_simple` refuses a
   self-intersecting or zero-area footprint (the fixture's bowtie emitted a
   roof with two faces wound against each other; the collinear one emitted a
   wall sheet with no roof). No real La Plata footprint fails it -- the geo
   MODELS.DAT hash is unchanged by the test.
6. **The base sink ate 0.7 m off every building.** `TD5_TG_GEO_BASE_SINK`
   exists so uneven terrain under a big flat footprint cannot show daylight
   under a wall, which is a statement about the BOTTOM edge -- but it was
   subtracted from the base before the top was derived from it, so every geo
   building came out 300 units short of its tagged height. Invisible on one
   building; on the fixture's three-part stack the plinth's top face landed
   at 6375 while the block above it started at 6675, a 0.7 m ring of
   daylight between two masses that are supposed to meet. The sink now
   lowers the base only, so a mass's top lands on its tagged height.
   MEASURED after, on the same stack: plinth 2935..6675, block
   6375..11835, tower 11535..19555 (17855 plus a 1700 pyramidal rise) --
   every top exactly on its tag, and each part's base now sits 300 units
   INSIDE the one below instead of 300 above it, which is what the sink is
   for. Footprints shrink 18.6 -> 14.5 -> 10.3 m as tagged.
7. **The prefab-table fallback was a counter, not a fallback.** A landmark
   with no 3D tags was extruded as a generic box and `s_geo_lm_fallback` was
   bumped. `tg_geo_emit_landmark_prefab` now picks the biggest shipped set
   piece that FITS INSIDE the real footprint and stamps it at the centroid,
   facing the footprint's principal axis. Fitting inside is what makes "does
   not intersect the road" true by construction: the ring has already been
   nudged clear of the carriageway and the pavement. `TD5RE_GEO_LM_PREFAB=0`
   restores the plain extrusion.

Fixture result after the fixes, `re/tools/geo_fixtures/land_geom_audit.py`
over 578 meshes in the 18 fixture entries: **DEGEN 0, FLIP 0, ROOFGAP 0**
(roof eaves meet the wall top exactly on every case). On-road guard: 0
rejections of geo meshes.

The census now names the shape each roof was BUILT as, not just how many
were shaped, because "12 shaped" cannot tell 12 pyramids from a ridge, a
hip and a shed. On the fixture it reads

    roofs BUILT by shape: 2 flat, 5 apex (pyramidal/dome/onion/round),
    3 gabled, 1 hipped, 1 skillion, 2 mansard, 3 with no roof tag

and every one of the 20 cases is accounted for: apex 5 = pyramidal, dome,
round, onion and the part-stack's tower; gabled 3 = gabled, half-hipped and
the 2 m test case; flat 2 = the flat case plus the concave L falling back;
no-tag 3 = the two lower parts and the landmark no set piece fitted; plus 1
set piece stamped and 2 degenerate rings refused. 18 emitted, 20 in.

**On the REAL place these six fixes are almost entirely inert, and that is
worth stating plainly rather than leaving a reader to infer it.** La Plata's
census reads `2 flat, 0 apex, 0 gabled, 0 hipped, 0 skillion, 0 mansard, 52
with no roof tag`, 0 parts stacked and 0 set pieces stamped: the cache has
no `roof:shape` on any footprint near the route, no `building:part` at all,
and no near-route landmark that lacks 3D tags. The only visible change on La
Plata is the base sink, which moved the geo MODELS.DAT hash. So the coverage
this round buys is proven ON THE FIXTURE, and the fixture is the committed
artefact for exactly that reason -- the next real place with 3D tagging gets
the benefit, and the regression net already exists when it arrives.

**What the framedumps can and cannot show.** There is one capture per roof
type in `re/tools/geo_fixtures/out/` (gitignored; regenerate with
`land_frame.py`). They confirm the fixture buildings stand on the ground, at
the right spans, clear of the carriageway. They do NOT settle which
silhouette a roof came out as, and three framings were tried before
accepting that: a 34000-unit overhead is too far to resolve a ridge, a
13000-unit oblique puts the camera inside the surrounding procedural towers,
and at every altitude the roof page's tiled texture hides the geometry while
the dense procedural city makes one building hard to pick out. No knob
clears the procedural frontage without also clearing the real buildings
(`TD5RE_AUTOTRACK_FACADE_MASS=0` only flattens the mass). So the shape proof
is the per-shape census plus the audit above, and the framedumps are
supporting evidence rather than the measurement -- which is the right way
round anyway, since a hash and a face count do not depend on where a camera
happened to be pointing.

An audit trap worth recording: the first cut of `land_geom_audit.py` read
every VERTICAL roof face as an inverted one and reported 5 flips. A gable END
and a shed's high wall are vertical by definition. The audit now classes
`|n_y|/|n| < 0.05` separately, and the count fell 5 -> 1, the survivor being
the bowtie, which is a real inversion and is now refused outright.

### Landmark coverage: what the filter misses, and where the fix belongs

`geo_fetch`'s rule is `tourism or historic or (name and building in
{cathedral, church, stadium, museum, train_station, civic, public})`. It
flags 10 of 2047 La Plata footprints, 2 of them near the route.

Two changes, because the data is in two places:

- **In the reader** (`geob_class_is_landmark`), the unmistakable `building=*`
  values -- cathedral, chapel, basilica, mosque, synagogue, stadium, museum,
  palace, castle, monument, memorial, train_station, courthouse, townhall,
  government, civic, public, theatre, opera_house -- count on their own, with
  or without a name. La Plata: cache landmarks 10 -> 22, near-route landmarks
  extruded 2 -> 3. `university`, `school`, `hospital`, `office` and `retail`
  are deliberately NOT promoted: 29 universities and 13 schools in one cache
  is ordinary urban fabric.
- **The tags that would catch the rest are not in BUILDINGS.JSON at all.**
  Walking the raw Overpass response for the 54 footprints within 100 m of the
  route finds nine more landmark-worthy objects -- `office=government` x4,
  `government=administrative/ministry/legislative/yes`, `amenity=theatre`,
  `amenity=place_of_worship`, `amenity=police` -- and `geo_fetch` keeps none
  of those keys, so no reader-side rule can see them. The permanent home is
  `geo_fetch.py`'s tag list, which this workstream does not own and cannot
  re-run without the network. `re/tools/geo_fixtures/land_relabel.py` is the
  offline stand-in: it re-stamps `landmark` (plus a `landmark_src` naming the
  deciding tag) straight from the cached Overpass responses. On La Plata it
  takes the cache from 10 to 77 landmarks and, within 100 m of the route,
  from 2 of 54 to **11 of 54**. It is not run by default.

### Gates

| gate | result |
|---|---|
| synthetic seed 20260901, no geo knobs | MODELS.DAT `98E749869051ACE3`, STRIP.DAT `7C392E1958498B53` -- all 8 files identical to master |
| La Plata geo MODELS.DAT | `50C8E8FC902D4BAA` -> `0E3C3CA41A7038D0` (12248316 bytes both) |
| La Plata geo fixture `land_test` MODELS.DAT | `993681FC9709351C` |
| two geo builds | identical |
| `tg_network_audit.py` | OK (0 crossings, 0 street points on the road, worst grade 0.1147) |
| `tg_strip_audit.py` | 0 violations, 4 lane seams |
| on-road guard | 2 city rejects at spans 339 and 571, unchanged; 0 geo meshes rejected |
| structure lint | OK (warnings 83 against a baseline of 84) |

## 6h. La Plata driven end to end (item 10, 2026-09-30)

Automated: `verify/xspan_run.ps1 -Route re/assets/geo/la_plata/ROUTE.JSON
-Extra @{TD5RE_GEO_PLACE='la_plata'}`, 5 AI racers, RaceTrace on every slot,
300-420 s. The engine side is clean: all 5 cars finish (span 1292), span_raw
continuous on every slot (0 tick-to-tick jumps over 3 spans), no respawn, no
OOB rescue, no all-wheels-off run longer than 15 ticks.

What it found was the AI, not the track. 1367 wall-contact ticks before the
line, 86% at three corners (410, 970, 1473), each the SECOND corner of an S.
Traced per tick: the SMART racing line clamps at `SMART_RAY_MARGIN` = 6% of
the road WIDTH from the rail, the car sat at u=0.14 on a 2-lane street where
its half-width is ~0.14, and it pinned against the inside kerb with full lock
for 1-13 s while the cars behind piled in. The strip itself was checked for a
folded inner edge at those spans (inner-edge advance never below 683 of 1500
units): it does not fold.

Fix (`td5_ai.c`, SECTION "GEO corner edge"): on the auto-track slot with a geo
route the final lateral clamp keeps `TD5RE_AI_GEO_EDGE` = 900 track units off
each rail. Same seed, same route:

| Variant | Wall ticks before finish | Mean finish tick |
|---|---|---|
| master | 1367 | 5824 |
| edge 900 (shipped) | 238 | 4648 |
| braking-distance governor only (rejected) | 3466 | 7608 |
| DRIVER model, for reference | 797 | 5152 |

The braking governor (corner speed `C*sqrt(R)`, measured decel) was built and
dropped on these numbers: a slower car cuts the same apex and pins harder.
Shipped tracks: Moscow and TD6 run the unchanged path (the gate needs the auto
slot and a geo route) and were A/B'd tick by tick against the master exe.

## 6i. Phase 4 finished 2026-09-30 (fork candidates, layer vintage, caps)

The three selector items left open at the end of 6e/6f. All offline: no fetch,
no browser, no network.

### Fork candidates (the Phase 4 item that had real engine work in it)

`re/tools/geo_forks.py` detects two shapes along the chosen route. A **median
avenue** is a pair of one-way OSM ways carrying the same name, running
anti-parallel 4 to 45 m apart, covering at least 50% of a run of at least
120 m; coverage is a **union over every same-name way**, which is the detail
that matters -- per-way was tried first and scored a 1055 m divided stretch of
Calle 54 at 0.18, because OSM splits its opposing carriageway into ten ways.
The union scores it 0.76. A **comparable alternative route** is found by banning
a leg's own edges and re-routing; it is capped at a quarter of the route and
1200 m, because the uncapped re-route of the La Plata leg returns a 5482 m path
against the 5124 m chosen -- comparable by every other test, and useless as a
fork, since its span range covers the whole track.

Measured union coverage per run on the reference La Plata route, which is where
the 0.50 threshold comes from:

| run | length | cover | verdict |
|---|---|---|---|
| Diagonal 101, Diagonal 102, Calle 6 | 8-116 m | 0.00 | undivided, correct |
| Calle 54 (20-28) | 327 m | 0.44 | opposing carriageway mapped for part only |
| Avenida 53 (44-54) | 568 m | 0.55 | divided |
| plaza ring | 102 m | 0.62 | under the 120 m floor anyway |
| Avenida 52 | 358 m | 0.70 | divided |
| Calle 54 (66-98) | 1055 m | 0.76 | divided |
| Avenida 53 (32-37) | 416 m | 0.83 | divided |
| Avenida 53 (102-109) | 683 m | 1.00 | divided |

0.50 sits in the 0.44 -> 0.55 gap, so five real divided avenues are taken and
the one partially-mapped stretch is refused.

**The lane contract is the part that is easy to get wrong.** The engine refuses
a fork whose span at F carries fewer than `tg_fork_kind_min_lanes(kind)` lanes
(4 for an ISLAND) and refuses it again unless the lane count is uniform across
`F-TD5_TG_BRANCH_WIDEN-2 .. R+2` (`td5_tg_branch.c:301`, `:314`). A real median
arrives as two one-way ways of 2 lanes each, so the route's own count is 2 and
every fork would be skipped with "2 lanes, needs 4". Confirming a fork therefore
**widens the route to lanes(A)+lanes(B) over the fork window and re-conditions**,
feeding the wider lanes into `condition_route`'s own `lanes` argument rather than
patching the result -- the curvature floor is `radius >= (width/2)*curve_safety`,
so the wider road has the tighter limit and enforcing it afterwards would store
geometry smoothed for the narrow road. Visible consequence, and the page says
so: toggling a fork moves the span count (1492 -> 1479 on La Plata).

Two index mappings are kept apart deliberately. The span range is read by
**arclength** (where the avenue is on the ground); the lane window is written
through `condition_route`'s own **index-fraction** mapping (`_lane_at`), which
is what decides which stored node gets which lane count. They disagree whenever
the raw vertices are unevenly spaced, so the achieved range is read BACK off the
conditioned lanes array (`_fit_fork`) instead of assumed.

**A defect the strip audit caught, worth recording.** Widening straight from 2
to 4 lanes puts a two-lane step at each end of the window. The emitter types
that as "add/drop BOTH sides" (span types 4 and 7) and the generator's own
invariant is that a both-sides change also moves the lane BASE nibble by one.
The synthetic walk does that bookkeeping; `tg_geo_walk` has no lane management
at all and leaves every span on `TD5_TG_HEIGHT_NIBBLE`. First build:
`lane-change seams on ring: 8 violations: 6`, every two-lane seam BAD and every
one-lane seam fine. The profile is now ramped one lane per seam
(`_ramp_one_lane_per_seam`), which types each seam as a right-side add/drop
(types 2 and 5) and leaves the base alone: 14 seams, 0 violations. The cost is
that the approach to an avenue gains a lane about a dozen spans early, which is
roughly the flare a real avenue has.

Game side: `td5_geo_forks.c/.h` loads FORKS.JSON and validates it (ascending,
disjoint, past the grid, inside the ring, known kind); `td5_tg_branch.c` injects
the geo answers at the **three** points that decide fork geometry and nowhere
else -- `tg_fork_count_planned`, `tg_fork_plan`, and the `pos` cursor read by
`tg_fork_place` / `tg_span_in_fork_run` / `tg_fork_window_ahead`. That is the
set the comment at `tg_fork_first_off()` records as having to agree.
`TD5RE_GEO_FORKS=0` pins the synthetic placement for an A/B.

La Plata result, five confirmed medians, ring 1479 + 984 corridor spans:

| fork | F | len | R | split | carriageway separation |
|---|---|---|---|---|---|
| Avenida 53 | 177 | 228 | 406 | 4 -> 2+2 | 7.0 m at the ends, 9.7 m mid |
| Calle 54 | 419 | 300 | 720 | 5 -> 2+3 | 12.1 m mid |
| Avenida 53 | 733 | 180 | 914 | 4 -> 2+2 | 9.7 m mid |
| Avenida 53 | 1011 | 143 | 1155 | 4 -> 2+2 | 9.7 m mid |
| Avenida 52 | 1313 | 131 | 1445 | 4 -> 2+2 | 9.7 m mid |

Measured off STRIP.DAT rather than eyeballed: the two carriageways meet at the
split and the rejoin and bow apart in the middle, which is the ISLAND shape. The
generator's median is slimmer than the real gap (OSM measures 12.3 to 33.2 m),
because the ISLAND `sep` is 0.16 and the bow is capped.

Driven with AutoRace from span 150 for 260 s (`--PlayerIsAI=1`, RT off, minimum
graphics, RaceTrace on): **0 span-localiser jumps over 3599 ticks**, and forks
0-3 fully traversed (230/230, 302/302, 182/182, 145/145 spans). 1166 of 3599
ticks were spent on a corridor span, i.e. the car really drives the second
carriageway. Airborne inside the fork windows measured 13.2% against 8.4%
outside, which looked like the fork geometry until the `TD5RE_GEO_FORKS=0` A/B
was run on the same route and the same widths: **13.8% inside with the forks
off**, slightly worse. It is La Plata's real grade at those spans and the AI's
pace, not the forks.

Two things to know about the result:
- **Fork 4 is built but never raced.** The finish lands at span 1279 of the 1479
  ring (200 spans of run-off past the line), so 1313-1445 sits in the run-off.
  The placement rule bounds against the RING, not the finish. Left as is: the
  avenue is genuinely on the route the user drew, and refusing it would drop
  real data for a generator-chosen finish position.
- Confirming all five puts 66% of the track on a divided avenue and adds 984
  corridor spans. That is what La Plata is; it is also a much bigger build, so
  the toggles matter.

### Per-layer source and vintage

`layer_rows()` flattens PLACE.JSON `layers` into four rows (roads/buildings,
elevation, land cover, tree canopy) with source, vintage, licence and a count,
and the page renders them in the TD5 skin. `wired` is false only when the source
string SAYS it is not, so the page never claims a layer is live because a key
happened to exist: canopy shows **NOT WIRED** on La Plata, which is the truth,
and is why the plaza trees are procedural. A place with no `layers` block at all
reports four NOT WIRED rows rather than an empty panel.

### "Not enough road here", and the caps

Every refusal is a sentence in the page before anything downstream has to cope,
and `ok: false` leaves SEND TO GAME disabled. Thresholds measured on the two
fixtures (la_plata: 2291 ways cached, 1650 inside the pinned routing bbox, 4907
graph nodes, one component; the self-test grid: 26 ways, 338 nodes):

| check | threshold | why there |
|---|---|---|
| too few ways / nodes | 8 ways, 40 nodes | clears the smaller fixture by 3x and 8x; still refuses an ocean tile or a one-street hamlet |
| fragmented graph | largest component < 35% | both fixtures are a single component (1.00) |
| click off the road | 250 m | more than one La Plata block (110 m), so a click in a plaza still finds the street round it |
| bbox | radius 500 to 4000 m | 4 km is an 8 x 8 km box (64 km2), which holds the 10.5 km hard-ceiling route; the old code clamped silently at 6000 m, a 144 km2 box, four times what section 0 sized |
| way count | 6000 drivable ways | 2.6x La Plata's 99 ways/km2; over it the fetch is CLIPPED (service and living-street first, arterials last) and PLACE.JSON records `clipped_from` |

The bbox cap **refuses** rather than clamping: the old `min(radius, 6000)` gave
the user a different number from the one they picked with no way to know.

### Gates

Synthetic seed 20260901 byte-identical: MODELS.DAT `98E749869051ACE3`,
STRIP.DAT `7C392E1958498B53` (all 8 files match; note `SELECTED.TXT` has to be
cleared first, or it fills an unset `TD5RE_GEO_PLACE` and the "synthetic" run is
not synthetic). Two geo builds identical (MODELS `194E0D5EF2F7F276`, STRIP
`6FC7BAFC4A53AD79`, NETWORK `797FE96EF89E89E7`). `tg_network_audit.py` RESULT
OK; `tg_strip_audit.py` 14 seams / 0 violations, all five forks sum OK. Structure
lint OK (warnings 83 against a baseline of 84). `geo_selector.py --self-test`
covers all three items offline, 17 checks.

## 6j. Option B complete: a self-crossing route builds a flyover (item 5, 2026-09-30)

`geo_condition.py --allow-crossings` now emits the crossing list into
ROUTE.JSON (old files without it still load). The generator
(`td5_tg_road.c`, deck geometry in `td5_tg_bridge.c`) lifts the LATER leg onto
a deck over the earlier one, chosen deterministically by ramp room (91 spans
against 82 on the fixture), with grade-limited ramps each side. No RNG draw.

Fixture: `re/tools/geo_fixtures/figure8_*` (Gerono lemniscate, 657 spans, one
crossing at nodes 131..135 against 562..566, 62 degrees). Build with
`-Seed 20260902` AND `TD5RE_AUTOTRACK_SCENERY=1` (see the fixture README:
20260901 lays the route over water, and 20260902 alone rolls SCENERY OFF).

| Measure | Result |
|---|---|
| `[GEO XSEP] site 0 BUILT` | deck 3079 units (7.1 m) over the lower road, soffit 2599 above it, ramps 35 spans, ramp grade 0.092 |
| deck Y vs lower Y at the crossing probe | 5667 vs 2201 (3466 apart); master exe 136 apart |
| localiser at the probe | hint 132 -> 132, hint 564 -> 564 [HELD] |
| drive, both legs (`verify/xspan_leg.ps1`) | 0 span jumps; slots cross 131-135 underneath and later 560-569 on the deck |
| audits on the fixture | network RESULT OK, strip 0 violations |
| La Plata (no crossing) | MODELS `50C8E8FC902D4BAA`, STRIP `1F41A4FD204368C7` = master |
| synthetic seed 20260901 | MODELS `98E749869051ACE3`, STRIP `7C392E1958498B53` |

`td5_track.c` is unchanged: the G4 localiser already keeps the hint once the
two decks are thousands of units apart in Y. The two legs never share a frame
(the renderer culls to +-64 spans, they are 430 apart), so the visual proof is
per-leg framedumps plus the probe numbers above.

## 7. Phases

### Phase 0 -- calibration and ground truth
- Measure world-units-per-metre by trace; record it here. **This is the only step
  in the whole plan that runs the game**, so it carries the run rules below.
- **Confirm IGN 5 m coverage for La Plata and Buenos Aires** in the download
  tool (§6).
- Commit both fixtures (Buenos Aires, La Plata) as cache directories so every
  later phase demos offline and the audits have real inputs.
- Set the bbox cap from the measurement (a 6.3 km route wants roughly a 4 x 4 km
  box with margin; hard ceiling 3000 spans / ~10.5 km).

#### How to run the calibration measurement (standing rules, 2026-09-29)

Every test that is NOT the selftest suite -- AutoRace, framedump, `--Control`,
zone harnesses, repros -- runs with **no RT and minimum graphics**. This
measurement is an AutoRace plus a RaceTrace, so it is one of them. It is a
distance/physics measurement, not a graphics one, so the rule costs nothing and
makes the run faster.

Environment: `TD5RE_RT=0` (this overrides the INI) and **never**
`TD5RE_AUTO_PERF=2`.

All 16 flags below were verified 2026-09-29 to exist as real INI keys with `--Key`
overrides:

    set TD5RE_RT=0
    td5re.exe --AutoRace=1 --SkipIntro=1 ^
      --Lighting=0 --Quality=0 --SunShadows=0 --Reflections=0 --WetRoads=0 ^
      --StreetLights=0 --CarLights=0 --LegacyShadows=0 --GIQuality=0 ^
      --ShadowRays=0 --ReflectionQuality=0 --CarShadows=0 --VFX=0 ^
      --WorldBillboards=0 --FoliageAA=0 --RenderScale=50 ^
      --RaceTrace=1

Two hazards specific to this step:

1. **`td5_ini_persist_options()` (`main.c:402`) writes these values back to the
   `td5re.ini` beside the exe.** In the main tree that file is Mariano's real
   configuration. **Run the worktree's own exe**, or back the INI up and restore
   it. A run that passes through an options screen or commits race options is
   enough to trigger the write -- it does not need anyone to change a setting on
   purpose.
2. Trace CSVs need `[Logging] Enabled=1`, and `RaceTrace=1` deliberately fixes
   the RNG seed (which is what makes an A/B run comparable). Separately, a
   non-zero `RaceTraceMaxSimTicks` **quits the game on its own**, even with
   `RaceTrace=0` -- so set it deliberately or leave it alone.

If the measurement ever needs RT or real graphics settings to mean anything, stop
and ask Mariano rather than relaxing the rule.

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

### Phase 6 -- in-game map screen: RULED OUT (2026-09-29)

**Do not build this.** Standing instruction from Mariano, same session the
net-loopback test was removed from the selftest: no networking work, no tests
that open sockets, and `td5_net.c` / `td5_upnp.c` / `td5_fe_net.c` are not to be
touched. An in-game map needs an HTTP client in the game, and the plan's earlier
route to one was to reuse `upnp_http_exchange` from `td5_upnp.c` -- squarely
inside that prohibition. If in-game map tiles ever come back up, ask first.

The browser selector of Phase 4 is therefore not "the shipping selector for now",
it is **the selector**. That costs nothing this plan was counting on: it is the
only option that can drag a route anyway, and it already avoids TLS, tile caching
and the OSM tile usage policy. The AUTO TRACK STUDIO screen
(`at_preview_request` / `at_preview_tick`, `td5_fe_race.c:7607` / `:7645`) keeps
drawing the conditioned route from the local cache, which needs no network.

Note on Phase 1's own networking, so the boundary is explicit: `geo_fetch.py`
makes outbound HTTPS requests from **Python**, to Overpass, AWS S3 and the IGN
WFS. It opens no listening socket, adds no test, and changes no game code, so it
is outside the scope of the rule -- the Windows firewall prompt comes from
LISTENING, which is what the removed net-loopback test did. Every response is
disk-cached, so a place is fetched once and the game itself never touches the
network.

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
2. **Multiplayer: CLOSED, not open.** Already out of scope by decision, and now
   also out of bounds: the only workable design was host-ships-the-cache over the
   net transport, which means `td5_net.c`. Covered by the same standing
   instruction as Phase 6. Do not revisit without asking Mariano.
3. **Storey height** for the `building:levels` fallback -- one global constant,
   or per-country, or per land-use class?
