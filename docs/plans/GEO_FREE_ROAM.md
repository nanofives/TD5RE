# GEO FREE ROAM: drive the real city, race inside it (PORT-ONLY)

Status: **F0, F1, F2 done (2026-10-17) on integ-roam; F3 (streaming) next.** Written 2026-10-15 from Mariano's answers
(recorded in "Decisions" below). The technical design for F1-F4 is
`GEO_FREE_ROAM_DESIGN.md` (F0c, with F0a numbers and the F0b span-consumer
verdicts folded in); where the two disagree, the design doc wins.

## Goal

1. **Free roam:** pick a spot on the geo selector map and drive every street of
   La Plata. The scenery streams in tiles around the car, nothing is prebuilt.
2. **Plan a race while you roam:** drop START / waypoints / FINISH while
   driving, or click them on a map overlay. Save the route to a file.
3. **Race inside the same world:** the route is closed off with NFSU2-style
   barriers. Racers (you + AI) bounce off them, traffic drives through them.
4. Eventually **replace** today's strip-based geo BUILD.

## Decisions (Mariano, 2026-10-15)

| Topic | Decision |
|---|---|
| First area | Casco urbano, **streamed in tiles from day one**. Measured (F0a): a diamond of ~5.2 km sides, 27 km2, rotated -42.13 deg; 484 km drivable, 2170 ways, 85k buildings, 535 signals |
| Race model | **Live walls inside the free-roam world** (not compiled to a strip) |
| What is solid in roam | **Roads only**: an invisible wall at the drivable-surface edge, like today's tracks. Plazas: only the OSM ring road, never the park inside |
| Scenery | **Same detail as races**: buildings, sidewalks, mast-arm lights, plates, trees, ported to per-tile emitters |
| Opponents | **AI racers on the route** (up to 5) |
| Street life | **Ambient traffic** obeying the real signals, **then cops** |
| Route planning | **Both**: drive-and-drop and a map overlay, one shared route |
| Entry point | A **FREE ROAM** button in the geo selector, then car select, then spawn where you clicked |
| Race types | **Sprint A to B first, then circuits** (laps) |
| Barriers | **Glowing see-through panels + chevron arrows** at every closed mouth and turn. **Racer-only collision**, traffic passes through (NFSU2) |
| Traffic / cops in a race | **Toggle per race** in race setup |
| Time / weather | Existing geo options, chosen before spawning |
| Old strip geo BUILD | Kept until the live-wall race reaches parity, **then retired** |
| Saved routes | **Saved by name** to a small file, re-raceable, shareable |
| Process | This doc, Mariano reviews, then one child round per phase with selftest gates |

## Why this is not "a bigger geo track"

Today a geo track is ONE span strip (the route, about 2150 spans) plus fork
corridors that leave and rejoin it (`AUTOTRACK_BRANCHES.md`). Everything keys
off the span index: physics contacts (`TD5_TrackProbe.span_index`, `int16`,
`td5_types.h:758`), walls, AI, laps, minimap, render window (+-64 spans),
scenery emitters (lateral distance from the main route). Side streets are
scenery (`td5_tg_network.c:1685`, `:1870`). Caps: `TD5_TG_MAX_SPANS` 3000,
`TD5_TG_BRANCH_MAX` 20.

A city is a GRAPH with thousands of junctions. The casco alone holds about
2,170 drivable ways, 1,640 junctions of degree 3+ and 87 roundabout ways
(F0a). Forcing that into one
strip is the wrong shape, so free roam gets its own world model and the span
strip stays only for the old BUILD path.

The seed of the new model already exists: `geo_locate` / `geo_resolve_walls`
(`td5_track.c:2058`, `:2314`) is a grid-bucketed "nearest drivable lane quad"
locator with a geometric wall push. Free roam generalises that from "quads of
one strip" to "quads of every street in the loaded tiles".

## Architecture

### World frame and tiles

- **Tile** = a 256 m square on a lattice rotated to the casco axes (F0a: 462
  tiles, median 1.1 km of road and 205 buildings each). Lattice in `ROAMGRID.JSON`,
  addressed `(tx, tz)` in the place frame. **Tile content is a pure function of
  (place, tile id, spec, exe)**, never of load order or the player's path. That
  is the invariant that makes tiles cacheable on disk, testable byte-for-byte,
  and later netplay-safe. The cache KEY is a hand-bumped `ROAM_RULES_VERSION`
  plus the input hash, not the exe (that would invalidate every tile per build).
- **Origin rebasing:** the place frame overflows 24.8 fixed point past about
  +-19.5 km (`GEO_REGION_EXTENT.md` "Limits"). The casco fits, the partido does
  not. The world keeps a `world_origin` (tile units). When the car is more
  than N tiles from it, shift every actor, camera, particle and loaded tile by
  a whole number of tiles at the top of a sim tick. **A fixed origin at the spawn
  tile is needed from F2**: the casco's north corner sits at 84% of the 24.8
  range, past the 4.19 M point where `a + b` of two positions overflows (F0c).
  Dynamic shifting can wait for F9.
- **Resident set:** a square of tiles around the car (start 5x5 at 256 m =
  1.28 km, about view distance), plus a lead in the velocity direction.
  Hysteresis on unload. Memory budget per tile measured in F0.

### Road world (`td5_roam_world.c`, new)

Built per tile from the cached OSM roads (`re/assets/geo/<place>/tiles/ROADS_*`):

- **Street graph:** nodes = junctions, edges = way pieces, with lanes, width
  (`td5_geo_sidewalk.c` rules), one-way, signal nodes (`td5_geo_signals.c`).
  Ownership is per ribbon QUAD (by its centre-segment midpoint) and per
  junction cluster (by centroid), not per edge: edges can be km long.
- **Drivable surface:** carriageway ribbons per edge plus a **junction patch**
  polygon per junction cluster. This is NEW code: the strip's
  `GEO_MULTI_JUNCTION.md` rules only handle a route's mouth table (F0c 5.1).
  Stored in the strip's own `TD5_StripSpan` / `TD5_StripVertex` format so the
  existing height code gives bit-identical results on an identical road.
- **Locator:** a per-tile grid of surface triangles (same idea as
  `geo_locate`), answering for any (x, z, y): ground height, normal,
  surface type, edge id, lateral offset, and "inside / push vector to the
  nearest edge". The push vector is the invisible wall.
- Grade separations (overpasses, `td5_tg_bridge.c`) need the `has_y` path of
  `geo_locate` from day one, or a car under a bridge snaps onto its deck.

### Physics hookup

A new contact provider selected in ROAM mode. Everything physics reads comes
through `td5_physics_refresh_wheel_contacts` (`td5_physics_suspension.c:3540`);
the provider replaces 8 `td5_track.c` functions (list in F0b, design 2.3). In
roam the strip is unloaded, ring length 0, every span field -1, the roam edge
id lives in a per-slot side table. F0b classified ~260 span consumers (~58
work unchanged, ~38 need the locator, ~84 gated off, ~80 rewritten in F5-F8):
`docs/plans/geo_free_roam/F0b_REPORT.md`. Known crash to fix in F2: `td5_camera.c:5727`
indexes the route table with span -1 unchecked.

Walls: the old geo nearest-edge push wedged cars at sharp fork bends
(`td5_track.c:1745-1760`). Roam uses one combined push per actor per tick,
filleted block corners (>= 3 m), a kerb overhang tolerance for body corners and
a build-time swept-width check (design 2.5). Traffic (slots >= 6) skips the wall
resolver today, so in roam traffic is kept on its lane by the lane follower.

### Rendering and scenery

- Each loaded tile owns its render meshes (road, sidewalks, buildings, props),
  drawn by distance cull, not by the +-64 span window.
- **Scenery parity means porting the emitters**, which today key off the main
  route span and its lateral distance, and share about 102 mutable statics
  (single instance only). They get re-keyed to (edge, side, offset) and moved
  into a per-tile context struct so a worker can build one tile while the
  renderer draws others. This is the largest piece of work in the plan.
- The streamed scenery worker (`td5_trackgen_stream.c`) is the template for
  the tile worker: fence-published entries, cancel flag, start gate.

### Route, race and barriers (`td5_roam_route.c`, new)

- **Route** = an ordered list of graph edges (A* via the existing
  `td5_geo_route_build`, `td5_geo_route.c:3864`, rebased on the roam graph),
  plus a polyline with distance-along. Progress, positions, checkpoints and
  laps all use distance-along the polyline, no spans.
- **Save file:** `re/assets/geo/<place>/routes/<name>.json`: place slug,
  waypoints (lat/lon), type (sprint / circuit), laps, edge list hash. Waypoints
  are the source of truth, the edge list is re-derived and the hash flags a
  map change.
- **Barriers:** at every junction where the route passes, each mouth the route
  does not take gets a barrier across it, plus chevrons on the outside of each
  turn. Look: glowing translucent panels with arrows (NFSU2). Collision: a new
  **collision layer** so the barrier is solid for racers and ignored by
  traffic. Barriers are race-time objects, not tile content (tiles stay
  route-independent).

### AI racers

The closed-loop Driver Model AI (`td5_ai_driver.c`) already drives from a path
table of midpoints, arc length and curvature with pure pursuit and a braking
pass. Feed it the route polyline instead of the span table. Rubber-band and
positions on distance-along.

### Ambient traffic and cops

Traffic cars spawn and despawn in a ring around the player on the street
graph, follow lanes, turn at junctions, obey the lamp cycle of
`td5_geo_signals.c`, ignore race barriers. Cops reuse cop-chase behaviour on
the graph. Race setup gets traffic / cops toggles.

## Phases (one child round each)

| # | Phase | Delivers | Acceptance |
|---|---|---|---|
| F0 | **Measure + audit** (read-only) **DONE 2026-10-15** | F0a casco numbers + tile pick, F0b span-consumer table, F0c design (`GEO_FREE_ROAM_DESIGN.md`) | Done |
| F1 | **Road world, offline** **DONE 2026-10-16** (T1-T10) | Tasks T1-T8 of design section 6: cache reader, raster window, graph + junction patches, ribbons + heights, ROAMTILE + determinism harness, locator, coverage audit, dev viewer | Same bytes twice / reversed order / worker thread. 0 uncovered drivable ways in the casco polygon, 0 seam mismatches. Viewer PNGs looked at |
| F2 | **Drive it (casco, flat look)** **DONE 2026-10-17** (A mode/entry, B physics, C render, D selftest rows `roam` + `roam-corners`, E acute corners) | ROAM mode, FREE ROAM button in the geo selector, car select, spawn on the clicked point, fixed world origin at the spawn tile, locator physics + invisible walls, all tiles loaded at once, plain road/ground render only | Selftest `roam` scenario: scripted drive across 20+ junctions, never falls through, never stuck, never leaves the surface, under bridges stays under. Corner test: 90-deg turns at 20 junctions per kind at 40 and 80 km/h, 0 pinches. Strip-mirror A/B on an identical road |
| F3 | **Tile streaming** | Tile worker, resident set, unload with hysteresis, memory budget, start gate | 10-minute scripted drive: private bytes flat after the first lap of tiles, no hitch > 1 frame on tile publish, no missing tile in front of the car |
| F4 | **Scenery parity** (3-4 rounds, design 4.4) | Emitters ported to per-tile context: buildings, sidewalks, mast-arm lights + plates, trees, furniture | geo_tour poses on the casco look like today's geo track, side by side |
| F5 | **Route planner** | Drive-and-drop keys, map overlay (reuse the geo selector map), route line on the minimap and the road, save / load by name | Plan, save, quit, reload, same route |
| F6 | **Sprint race with barriers** | Race start from a saved route, NFSU2 barriers + chevrons, racer-only collision layer, checkpoints, finish, results | Selftest: solo sprint completes, player cannot exit the route at any closed mouth |
| F7 | **AI racers** | Driver Model on the route polyline, positions, rubber-band | 5 AI finish a 3 km casco sprint with no stuck cars, invariant checker green |
| F8 | **Traffic** | Ambient traffic on the graph, signals obeyed, passes through barriers, race toggle | Traffic density holds steady over 10 min roam, no traffic inside buildings |
| F9 | **Circuits + whole partido** | Laps, origin rebasing, partido tile index. **F9a circuits DONE 2026-10-18** (roam-F9a): SPRINT / CIRCUIT + laps 1-10 in the map and the setup, closing leg, open start line, lap tracker on the total distance (`td5_roam_lap.c`), AI laps, lap HUD / results, selftest rows `roam-circuit` + `roam-circuit-smoke`. Origin rebasing + partido index still open | Drive from the casco to City Bell without a hitch |
| F10 | **Cops** | Police on the graph in roam, per-race toggle | |
| F11 | **Retire strip BUILD** | Parity checklist (forks, signals, AI quality, load time) then remove | Mariano signs off |

## Open questions (to settle in F0 or later)

- **Car pick (settled):** FREE ROAM opens the normal car-select screen, then
  spawns on the clicked map point.
- **Plazas and roundabouts (settled):** roads only. The ring road is drivable
  as OSM maps it, the park inside is behind the invisible wall.
- **Multiplayer:** out of scope for now. The pure-function tile invariant
  keeps the door open.
- **Exit:** pause menu "Leave free roam" back to the geo selector.

## Risks

- **Physics fidelity.** TD5 suspension reads span triangles. The locator must
  give the same height and normal the span path gives on an identical road,
  or cars feel different in roam. F2 compares both on the same street.
- **Emitter port size (F4).** Largest phase, may need splitting into 2-3
  rounds (buildings / sidewalks / furniture).
- **Tile seams.** Edges and buildings crossing a border must be owned by
  exactly one tile, or they double-draw or vanish.
- **Old-path regressions.** Roam lives in new modules and a new mode. The
  strip geo BUILD and every original track must stay byte-identical until F11
  (selftest full suite + `geo_level_identity.py` on level091 each round).
