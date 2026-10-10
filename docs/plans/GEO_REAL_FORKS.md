# GEO REAL FORKS (round 1013 F2 + F2b)

Driveable forks built from the real map on a GEO track. Module: `td5_tg_realfork.c`.

> "when there's avenues by default those should be driveable as forks, with the
> plazas as well, by default every road that is too close to the main road should
> be treated a driveable fork" -- Mariano, after testing master bc143d22

## Why round 1009 was wrong and this is not it

Round 1009 made a divided avenue an ISLAND fork: the race road was widened
symmetrically to lanes(A)+lanes(B) and split in two, and the far half was a **bowed
copy** of the race road (`sep` 0.16 is `TD5_TG_BRANCH_SEP_MIN`, a bow scale, not a
width). Round 1010 removed it ("you created branches instead of using the actual
map"). This keeps the engine's fork machinery and changes what feeds it.

| | round 1009 | now |
|---|---|---|
| corridor lateral | half-sine bow x `sep` | **measured gap between the two carriageway edges** (AVENUES.JSON `off`, or the matched OSM way) |
| corridor lanes | half of a widened road | the **real** lane count of the way it follows |
| race carriageway | displaced by half the other road (symmetric widening) | **stays where the map has it** (one-sided widening, see below) |
| mouths | wherever the detector put them | **real median openings / run ends / OSM way boundaries with a cross street** |

## How real geometry becomes a fork

1. **Candidates.** `rf_gen_avenue` reads `td5_geo_avenue_at` per span (AVENUES.JSON,
   F1's detector output, untouched): gates are the two run ends and each opening run
   (`open`); a candidate is the block between a gate and the next one far enough
   away (>= 24 spans, merged with the following block when shorter).
   `rf_gen_parallel` finds, per route node, the nearest OSM way within 25 m
   (`TD5RE_GEO_FORK_PAR_MAX_M`) and within 20 deg (`..._ANG_DEG`) of parallel or
   anti-parallel, with at least 0.5 m between the two carriageway edges; its gates
   are run ends and matched-way changes, each kept only if a real cross street cuts
   between the route and that road there (`rf_cross_cut_at`), or a way sharing an
   OSM vertex reaches the route, or the road merges into the route.
2. **Validation.** Not in the start grid, 24-span tail margin kept, route lane count
   constant over the whole window, lanes(A)+lanes(B) <= 8, bend <= 0.045 rad/span
   (`TD5_TG_FORK_MAX_TURN`), corridor on the -t side only (left corridors are parked
   in the engine, see `TD5RE_TG_NET_LEFT`). Every rejection is logged under
   `TD5RE_GEO_FORK_DIAG=1`. **Every fork window, taper tail included, ends
   `TG_RF_FINISH_GAP` (8) spans before the FINISH line**, so nothing is built in the
   run-off past it: `tg_realfork_finish_span()` is the one place that says where the
   finish is (pure `route spans - TD5RE_AUTOTRACK_RUNOFF` arithmetic, because
   `tg_finish_span()` reads fork and tunnel tables that do not exist yet when the forks
   are chosen). A finish the route itself fixes (round 1013 F3: ROUTE.JSON
   `finish_span`, `td5_geo_route_finish_span()`) replaces that function's body and
   nothing else. Checked with a simulated 100-span run-off: the fork at 820..857 is
   refused as past the finish (finish 851) and the selector re-packs the rest.
3. **Selection.** Weighted interval scheduling over the windows (weight = min(len,90),
   avenue x1.05, parallel x0.9): the FULL-width windows `F-8 .. R+2` of two forks stay
   `TG_RF_MIN_GAP` (6) nodes apart; their tapers may overlap and share nodes (the node
   takes the larger width). Parallel candidates that overlap an avenue candidate are
   dropped (the avenue source wins), so a detector that learns a new avenue (F1 adds
   Diagonal 73's second run, spans 337..588) takes over from the parallel source on
   its own.
4. **Window and taper.** The engine's fork has one cross-section at F: main half +
   corridor half. So over each window the route's ring nodes carry lanes(A)+lanes(B),
   widened on the **corridor's side only**: the node moves half the added width
   toward the corridor, which keeps the race carriageway's own edge at the real
   position. The move is recorded as the node's JOG (`jx/jz`), which the tangent pass
   already subtracts, so node tangents and the AI route-heading table do not kink.
   **The width is ramped, not stepped** (F2b): node width = route lanes + `lanes_b *
   f(i)` lanes, f linear over `TG_RF_TAPER_DEF` = 16 nodes (56 m, 7 m of width, about
   7 degrees) in front of `F-8` and behind `R+2`; the lane COUNT is its rounding.
   Before F2b the lane count stepped one lane per node, a 45 degree edge taper: cars
   leaving the rejoin hit it at 130-190 km/h (5 wall events and two pile-ups at spans
   119..124 behind fork 0, none on that stretch with real forks off).
   `TD5RE_GEO_FORK_TAPER` sets the length, `..._TAPER_SMOOTH=1` a smoothstep shape
   (measured no better over 5 seeds).
5. **Corridor.** Fork kind `TG_FORK_AVENUE` + `TG_Fork.real`. `tg_fork_br_shift` for a
   real fork is `-(w*fm/2 + median(k))`: the main half's edge plus the real median,
   `median(k) = |offset(k)| - half of both carriageways`, pinned to 0 at both mouths
   and rate-limited to `TD5RE_GEO_FORK_SLOPE` (default 0.12 span/span, 6.8 degrees)
   from each end. `TD5_TG_BRANCH_RATE` (0.35, 19 degrees) is the ceiling every fork
   stays under, not a shape: with it the median opened as an S that cars at 200 km/h
   could not follow (fork 2: 3 cars jammed at corridor step 6 for 670 ticks at full
   lock). The slope only ever LOWERS the median, so the corridor is never farther
   from the race road than the map puts it. Everything downstream (strip rows, road
   mesh, gore, kerbed island, branch pavement, carriageway reach, occupancy paint, AI,
   traffic) already reads that function.
6. **No double road.** Over a real fork's main spans `tg_emit_geo_avenue` skips the
   scenery carriageway and island (the fork draws them) and keeps only the far-side
   footway, which carries the real per-side width. In the taper spans the scenery
   road is clipped to start where the widened race road ends.
7. **AI lookahead** (geo tracks only, `td5_ai.c`). `td5_ai_smart_branch` looks 7..15
   spans ahead for a fork: a second or less at the 180 km/h a real straight gives.
   `TD5RE_AI_BRANCH_LOOK_SEC` (default 1.5, 0 = the old window) adds that much
   travel, capped at 40 spans. Gated on a loaded geo route, so synthetic races and
   netplay behave as before.

## Direction of the real way (decision)

Diagonal 73's second carriageway flows **against** the race direction on this route
(measured: `oneway_dir` against the route bearing, `verify/geo_peer_direction.py`),
which is the normal opposite carriageway on the right-hand-traffic side. The fork is
driveable **in race direction**, as asked. Consequences: a car on the corridor
travels against the OSM one-way flow; there is no oncoming traffic simulation
(traffic and AI on a corridor follow the span direction, like every fork), so the real
contra-flow is not represented. If a one-way sign/arrow layer is ever added it must
skip real forks. Noted in the in-game CHANGELOG.

## Suspect-fork check

`tg_validate_geometry_safety` capped a span's carriageway reach at the derived
synthetic bow ceiling (`road half + w*(0.25 + bow + 0.5)`), which a real avenue
legitimately exceeds: **76 "suspect fork spans" on Avenida 13 on master bc143d22**
before any real fork existed, now **0**. The ceiling on a span is now
`max(derived, tg_geo_avenue_reach + 1)`, so an avenue's own measured reach is the
limit there and everything else keeps the derived one: a reach past the real
carriageway still trips it.

## Knobs

`TD5RE_GEO_REAL_FORKS=0` all off (the round 1012 behaviour). `TD5RE_GEO_FORK_AVENUE`,
`..._PARALLEL`, `..._PLAZA` per source (PLAZA only toggles the report, see below).
`TD5RE_GEO_FORK_PAR_MAX_M`, `..._PAR_ANG_DEG`, `..._TAPER`, `..._TAPER_SMOOTH`,
`..._SLOPE`, `TD5RE_GEO_FORK_DIAG=1` (reasons). `TD5RE_GEO_FORKS=0` still pins the
synthetic ladder and so also disables these. Dev only:
`TD5RE_AI_BRANCH_FORCE_P0=0|1` pins slot 0's fork choice for a capture.
`TD5RE_AI_BRANCH_LOOK_SEC` as above.

## Plazas (source 2): measured, not built, and the design that builds them

Not this round (agreed with the orchestrator). `rf_plaza_report` logs the numbers per
ring so the next round starts from data. On Mariano's route (race.log, `[REAL FORK] plaza`):

| | Plaza Miguel de Azcuenaga |
|---|---|
| ring (OSM junction=circular/roundabout, named) | r = 91 m, 182 m across |
| the route runs along it | nodes 260..341 (81 spans), 270 m of arc |
| the other side of the ring | 303 m of arc (87 spans), up to 182 m from the route |
| what the corridor machinery allows | 65 m lateral (`TD5_TG_R8_LAT_MAX`), a rate limit, ground that stops 70 m from the route, corridor step k = main node F+1+k |

So the far arc cannot be a corridor of the existing kind: it is 2.8x further out than
the lateral cap, it has 87 spans where the main side has 81 (a corridor has exactly
R-F-1 spans, each riding one main node), and nothing past 70 m has ground under it.
Round 1013 F1 (sibling branch) separately vetoes facade meshes inside the ring.

### Design: the FREE corridor

A fork corridor that owns its geometry instead of riding main node + lateral shift.
Everything else (span records, jump table, type 8/9/10/11 spans, AI fork choice) stays.

1. **Data.** `TG_FreeCorr { int F, R, len; TG_Node *v; }`: `len+1` nodes of its own
   (x, y, z, tangent, width, lanes; jog 0) in a new `td5_tg_realfork_free.c`. Built
   from the ring's OSM ways ordered from the entry mouth to the exit mouth along the
   far arc, resampled to `TD5_TG_SPAN_LENGTH`, height from the same
   `tg_road_node_fill` chain the main nodes use. `len` is the arc length in spans
   (87), no longer R-F-1.
2. **One accessor.** `tg_fork_corr_frame(fi, k, &x,&y,&z,&tx,&tz,&w)` returns
   `nl->v[F+1+k] + shift*normal` for every existing fork (byte-identical, so the
   synthetic gate stays 00FB76E8) and the free node for a free one. The 14 sites that
   call `tg_fork_br_shift` today switch to it: `td5_trackgen.c` (strip rows x2, corridor
   trees, road mesh, branch pavement, the tunnel/bridge tie: 6), `td5_tg_branch.c`
   (carriageway reach, verge, kerbed island: 3), `td5_tg_bridge.c`,
   `td5_tg_network.c` (x2), `td5_tg_avenue.c`.
3. **AI + span mapping.** `tg_emit_routes` maps a corridor span to main node F+1+ck for
   its heading byte: it takes the free node's tangent instead. The corridor's
   normalised span (`span_norm`) needs `k -> main span` for a corridor longer than the
   main side: nearest main node by arc length, stored once per free fork.
4. **Ground + scenery.** `tg_far_reach()` (30000 = 70 m) bounds the far band; the free
   corridor adds its own ground skirt along its nodes (the plaza interior is a park in
   OSM: AREAS.JSON), and `tg_carriageway_reach` has to answer for main spans near the
   plaza with the distance to the far arc, or the facade/tree emitters stand scenery on
   the road (the 76 "suspect fork" warnings were exactly this question asked of a
   scenery road).
5. **Validation.** Same as an avenue corridor: not in the grid, bend <= 0.045 rad/span
   (a 91 m ring is 0.038 rad/span at 3.5 m spans, so it passes), 24-span tail.

Size: about 900 to 1300 lines. New `td5_tg_realfork_free.c` (ring ordering + resample +
validate, ~350), the accessor and the 14 call-site conversions (~250 touched,
mechanical), ground skirt + carriageway reach (~300), AI/span mapping +
`tg_fork_of_corridor` for unequal lengths (~150), verification harness (~100). Risk is
the strip writer and the byte gate, not the geometry. One dedicated round, with
`verify/geo_realfork_run.ps1` unchanged as the judge (it already bins by the fork
table).

## Verification

`verify/geo_realfork_run.ps1` (AI race with trace, car damage off, one tag per run,
`-Seed`, `-FramedumpSpans`) and `verify/geo_realfork_report.py` (fork entries per fork,
stalls excluding parked rows, contact events and INCIDENTS per segment, binned over
the same spans before and after). The framedump hook reads the ring-normalised span,
so a frame INSIDE a corridor is asked for by its main-span number with
`TD5RE_AI_BRANCH_FORCE_P0=1`.

### Measured on Mariano's route (La Plata, 951 spans, 6 AI cars, no traffic, damage off)

`TD5RE_RACE_SEED` under a trace only varies each car's fork roll (the rest of the race
is deterministic), so "before" is ONE run and "after" is five seeds (11, 22, 33, 44, 55).

| | before (`TD5RE_GEO_REAL_FORKS=0`) | after (5 seeds) |
|---|---|---|
| forks on the route | 0 | 5: Diagonal 73 x3 (F 59, 171, and the parallel-road source's 420), Avenida 13 x2 (757, 820) |
| car-passes that took the corridor (30 per fork) | - | 15, 18, 17, 15, 12 |
| cars finishing | 6/6 | 6/6 in every seed, mean finish tick 3587 vs 3408..3758 |
| stalls (plateau >= 90 ticks, <= 2 spans, before the finish area) | 20, 5363 ticks, longest 1100 | 4..10 per seed, 520..1927 ticks (mean 991), longest 171..607 |
| contact events / incidents (events merged when one car's contacts are <= 90 ticks apart) | 696 / 44 | 303 / 72 |
| `[REAL FORK]` suspect fork spans (geometry-safety) | 76 | 0 |

Before-stalls are mostly the sharp bend at 884..891 (11 of the 20) and the start grid,
the plaza and 597..605, none of them forks. After-stalls: the same pre-existing spots
plus pile-ups 6..12 spans into a corridor (spans 177..183, 428..430, 826..829) in
about half the seeds, and a spin after fork 0's rejoin in one. Every one of those is a
car at 150..190 km/h at full lock with 26000..36000 of rear slip: the geometry is smooth
at those spans (the corridor leaves at 6.8 degrees), the AI is the weak part. Things
that did NOT fix it, measured over the same seeds: a smoothstep taper (mean stalled
ticks 2576), a 28 or 40 node taper, the route-heading byte following the corridor
(trace bit-identical, the AI does not read it there), a 0.06 or 0.03 opening slope
(1008, 798: inside the noise, and 0.03 builds 1.5..2.9 m medians where the map has 3.7..7.5).
What helped: the 16 node linear taper (the stretch behind fork 0's rejoin, spans
119..162, went from 29 events / 5 incidents with two cars stuck 300+ ticks to 0, 0, 80,
5, 1 events over the five seeds; the 80 is one car stuck at spans 137..143) and the
gentler opening (the S at fork 2 that jammed 3 cars for 670 ticks).

### Corridor-mouth spins: measured cause, four fixes tried, none kept (F2b time box)

`verify/geo_fork_mouth.py` parses the strip and prints, per tick, each car's lateral
position, velocity heading error, steering, rear slip and speed through a mouth; the
AI lane brain was dumped per tick as well (temporary instrumentation, not kept).
Seed 55, fork 1 (F=171), a spinner (slot 2) against clean cars (slots 3, 4):

- Every car, spinner or clean, arrives at corridor step 0 with its VELOCITY 13..28
  degrees off the corridor, pointing toward the main road, at lateral +0.5 (left half of
  the 2-lane corridor), and goes from steering +0.1 to -1.0 (full lock) within two
  ticks. Clean cars lose ~40% of their speed in those two ticks (446 -> 253, 351 -> 207),
  rear slip 10000..16000, and recover; the spinner arrived at 613 units/tick, kept
  sliding (rear slip 39000 at step 6) and parked on the right wall at step 8.
- The corridor itself is smooth: its centreline heading is 0.0 degrees for steps 0..7
  and 6.8 degrees only where the median opens (steps 9..), so this is not geometry.
- The AI's lateral target is what flips. On the ring a car that rolled "take" holds the
  take anchor (0.86 of the 4-lane road). In the last three ticks it falls to 0.70, then
  0.30..0.60: (a) `td5_ai_smart_branch` scans from d = 1, so ON the fork span the pull is
  gone (`bpull` 1.00 -> 0.00); (b) the ray avoidance term follows the MAIN ring, where the
  road past F is only the left half, and pulls the target 0.15..0.20 left; (c) the
  `[GEO CORNERS]` wall margin is measured on the look-ahead span (span + 2), which past F
  is the narrow main half (margin 0.30), and clamps the 4-lane target to [0.30, 0.70]
  (the exact 0.699 and 0.300 in the dump). The car then enters the corridor at corridor-
  frame u 0.3..0.45 (its target is 0.62..0.8), leaning left.

Tried, 5 seeds each, mouth-spin stalls (stalls starting F+4..F+14) and mean stalled ticks;
baseline is 11 and 991:

| change | mouth stalls | mean stalled ticks |
|---|---|---|
| keep the pull through the fork span + no leftward ray avoidance while a take is held | 20 | 1454 |
| measure the wall margin on the current span while a pull is held | 24 | 1908 |
| kinematic approach-speed cap into every real split, 400 units/tick at the split | 20 | 1922 |
| same cap at 500 units/tick (cars rarely exceed it) | 11 | 1079 |

Every one that changes what the field does near a mouth makes it worse, and the cap
shows it is not arrival speed alone (a slower, tighter pack jams). So the table above is
NOT a recipe; the next attempt should start from the frame hand-off, where the car's
lateral state in the 4-lane ring frame has to become a corridor-frame state (u 0.70 in
the ring is u 0.40 in the corridor) while three AI terms disagree about which road it is
on, and should add a per-tick lateral-state trace to the harness first.

Unverified by a negative control: the relaxed suspect-fork ceiling. This route has no
far parallel road to trip it (80 m / 60 degrees finds no candidate, 60 m / 25 degrees finds
the same five forks); the ceiling on an avenue span is that avenue's own measured reach.

## Round 1014 A: continuous avenues, median ends, mouths (merged forks)

Mariano's six picks on La Plata (level091, master 22c2bfda), all decoded through
MODELS.DAT (entry, slot) + MESHTAG.BIN (centre, radius, vertex count all matched the
rebuild, so the rebuild IS his build): 2 and 9 `branch-road p0:ROAD` (the scenery
opposite carriageway, not driveable), 3 `other p44:SIDEWALK` (the corridor's branch
slab), 4 and 16 `road p2:GREEN+121` (a median island), 15 `other p44:SIDEWALK` in a
corridor.

### Causes (measured)

1. **Flicker, "one block road, next block avenue" (2, 9, 15).** The avenue's real median
   openings are 2 spans wide (`open` runs `58-59, 114-115, 169-170, 225-226`), so the
   candidate blocks (`F=58 R=115`, `F=114 R=170`, ...) have full-width windows `F-8 .. R+2`
   that overlap across the opening. `rf_select` keeps windows `TG_RF_MIN_GAP` apart, so
   it took every other block, and the blocks in between were left as a scenery
   carriageway that looks like road and has no span. 7 forks, 314 corridor spans, 40%
   of the 784 avenue spans.
2. **Start grid.** `F` was clamped to `GRID_SPAN + 12 = 36`, a value whose window
   (`F - 8 - 16`) always failed "inside the start grid", so the first fork could not
   begin before span 58 and 34 spans of avenue after the grid were scenery (item 2's
   pick sits at span 52..55 in the widening taper, where the scenery quad is a 1.2 m
   sliver beyond the widened race road).
3. **Median (4, 15).** `tg_emit_avenue_divider` only fills kerb to kerb when the gore is
   at most `TD5_TG_R11_MEDIAN_MAX` (6 m). A real median is 3.7..8.7 m, so beyond 6 m it
   fell to the 0.32-scale island capped at 520 units with bare gore floor either side,
   and the planted / barrier / kerbed ladder (`fork_index % 3`) changed the strip's
   look from one avenue to the next.
4. **Pavement at the mouth (3).** (a) `tg_geo_avenue_reach` reported the REAL opposite
   carriageway's far edge over a real fork's corridor spans, up to a median-width (1.6 m
   at the mouth) beyond the corridor's own edge, and the ground skirt starts at
   `reach + 200`: a see-through wedge beside the first corridor spans (the cyan in the
   free-cam frame). (b) The corridor's own branch slab (1081 units, biome width) and the
   ring's far footway (real per-side width) were both laid on the same lateral, 40 units
   apart. (c) `tg_av_emit_far_pavement` dropped the first corridor span: its
   `|edge| < node half width + 50` test reads `3000 < 3050` for the span where the
   corridor's outer edge IS the node's outer edge.
5. **Median opening (16).** At an `open` span the scenery path emitted the opposite
   carriageway and then NOTHING (no island, no footway, no fill), and the skirt starts
   past the far carriageway: a slot to the sky between the two roads. The scenery island
   was an open prism (no end caps), unlike the fork island.

### What changed

| | change | knob (default on, `=0` restores) |
|---|---|---|
| 1 | avenue candidates for EVERY gate pair, weighted by full length (cap 90 removed), so one fork runs through the 2-span openings | `TD5RE_GEO_FORK_MERGE` |
| 2 | F clamp = `GRID + 2 + widen + taper` (50), the first F whose window clears the grid | (same knob) |
| 3 | a real fork's island is always kerb to kerb, planted; `tg_median_at_raw` mirrors it | `TD5RE_GEO_FORK_ISLAND` |
| 4a | avenue reach over a real corridor span = the corridor's own (fork loop answers) | `TD5RE_GEO_FORK_REACH` |
| 4b | no branch slab where the ring's far footway lays the pavement (`tg_realfork_walk_owned`) | (MERGE) |
| 4c | far footway only dropped when its edge is genuinely inside the race road (-1 not +50 in a fork) | (MERGE) |
| 5 | paved flush opening between carriageways; footway continues; scenery island capped at both ends | `TD5RE_GEO_AVENUE_OPENING`, `TD5RE_GEO_AVENUE_CAPS` |

Inside a merged fork the median opening is NOT an opening: the corridor is one carriageway
across the cross street, the island is continuous, the footway is continuous.

### Dev tooling

`TD5RE_FREECAM_TOUR="name:x,y,z,yawdeg,pitchdeg;..."` (td5_camera.c, dev only) puts the
free camera at world poses and dumps a PNG per pose; `TD5RE_FREECAM_TOUR_SPAN=N` starts it
when the player reaches span N and `_FREEZE=1` pauses the sim there. **A fork corridor
only draws while the player is near it**, so a pose far from the car shows void (cyan) where
the corridor is; park the car in the fork first (`TD5RE_AI_BRANCH_FORCE_P0=1` +
`_TOUR_SPAN`). `verify/geo_r1014a_tour.ps1` runs it, `verify/geo_r1014a_poses.py` turns a
pick string into a pose (behind the pick, and a top-down one).

### Measured (La Plata, Mariano's route, 784 avenue spans)

`verify/geo_r1014a_coverage.py` reads MODELS.DAT + MESHTAG.BIN and says what each avenue span
is made of (a `branch-road` page-0 mesh in a RING entry is a scenery carriageway: road
surface, no span record).

| | master 22c2bfda | this branch |
|---|---|---|
| forks / corridor spans | 7 / 314 | 4 / 587 |
| avenue spans with a scenery carriageway ("looks driveable, is not") | 469 (60%) | 196 (25%) |
| avenue spans with neither island nor opening fill | 291 | 61 |
| avenue spans with no far footway | 42 | 2 |
| first fork starts at | 58 | 50 |

Scenery-only that is left, with the reason: 25..50 start grid; 226..253 and 335..364 bends of
0.089 / 0.213 rad/span (cap 0.044, the shifted carriageways fold); 547..586, 699..725 and
1032..1074 the route's own lane count changes inside the window (the fork's lane arithmetic
needs one number). Those stretches keep the kerbed island with closed ends and the paved
openings, and nothing is claimed drivable there.

Synthetic gate (`verify/geo_tags_identity.ps1 -Arm synthetic`, SELECTED.TXT moved aside):
MODELS.DAT 12982584 B `298DB07B141160AAFED83C3941DD1580`, STRIP.DAT 144714 B
`0641EDB7787600D236AE7B51186888DD`, TEXTURES.DAT 1605328 B `F69A8CBB6A3FFCA4757360F5AC39234B`,
identical between master's exe and this branch's. Self-test smoke 13/13.

### AI races: what the long forks cost (3 seeds each way, 6 AI cars, damage off)

`geo_realfork_run.ps1 -Seed N` (11, 22, 33), `geo_realfork_report.py --ring 1261 --finish 1150`.
new_22 was cut by the harness deadline at tick 3613 (the others finish near 4500), so it
understates the new arm.

| | master | branch |
|---|---|---|
| contact incidents inside fork windows + corridors | 151 | 74 |
| wall events inside fork windows + corridors (one stuck car in seed 11 is 395 of the new 569) | 577 | 569 |
| wall events in the stretches BETWEEN forks | 763 | 1282 |
| plateau stalls | 31 | 61 |

Cause, measured: the cars reach span 240 at 630..860 units of speed (`long_speed / 256`)
against 450..560 on master. Master's five mouths between spans 58 and 226 each cost the
pack ~40% of its speed (the open mouth-spin item), so it never got up to speed; with one fork
the cars run 175 spans without a mouth and arrive at the 0.089 rad/span bend at 250..275
(and at 335, 565..610, 880..890) at ~280 km/h. The same bends stall cars on master (seeds
11 and 22: spans 265..267), it is the pack's speed that changed. Not fixed here: it is the
AI's corner-speed model, not the road (`td5_ai_driver.c`, `TD5RE_AI_DRIVER_CORNER_*`).
`TD5RE_GEO_FORK_MERGE=0` brings master's selection back.

## Round 1015 E: plaza forks (BUILT)

Mariano, round 1015 item 9: "near plaza azcuenaga this road became undriveable (level091 L91
e53 s14 road p0:ROAD pos 248795,-186,-185415), if a road passes through a plaza by default i
should be able to drive to both sides of the plaza". The pick is route node 216, 10.9 m beside
the route: the scenery far carriageway of Diagonal 73 (a `branch-road` page-0 mesh in a ring
entry, no span record) in the 17 spans between the end of avenue fork 0 (R=205) and the ring.
Module `td5_tg_plazafork.c`; the free-geometry corridor this document asked for above.

### Shape

A route that runs along a `junction=circular` ring (named or not) gets ONE fork whose corridor
goes round the far side of the ring:

| rows (corridor step k) | geometry |
|---|---|
| `0..k1` (entry, classic) | the main node's ORIGINAL centre line (node minus its jog) plus the corridor lateral `(la+lb)*lw/2 + median(k)`; the median opens at 0.12 span/span and is held at the real gap the avenue sidecar gives (3.74 m on Diagonal 73), so these rows ARE the far carriageway and the avenue code (island, far footway, scenery skip) treats them as an avenue fork's |
| `k1..len-kx` (free) | a node chain of its own: the ring's far arc, entered along the street's axis and left onto the exit street's axis, Laplacian-rounded (300 passes, 10 m pinned at each end), resampled to equal steps |
| `len-kx..len` (exit, classic) | mirrored: converges on the main half over the rate-limited median |

`k1 = (last straight route node before the bend) - TD5RE_GEO_PLAZA_LEAD_IN (14) - (F+1)`, `kx =
R - (first straight node after the exit bend) - LEAD_OUT (10)`. The corridor leaves the avenue's
line 14 nodes before the bend and rejoins 10 after it, so it takes the junction as a wide
diagonal: tightest corner of the whole chain 24.9 m (13.4 m with lead 4/0 and 100 passes, the
first cut: a car crashed at the exit mouth, seed 33, 245 events), straying at most 4.1 m from the ring way where it follows it
(the lawn starts 9 m from the ring way). The fork has exactly `R-F-1` spans like every other
fork (span records, type 8 / 9 / 1 / 10 / 11, jump table, `lanes(F) = lanes(F+1) + lanes(B0)`);
on the free rows a span is `chain length / free rows` long (3.4 m on Azcuenaga, 0.97x a route
span; the road texture's V runs at that rate, `tg_road_v_scale`).

Everything that needed the corridor's position reads ONE view, `tg_pf_view(fi)`: a node list
whose `v[F+1+k]` is corridor row k, so the emitters that already take `(nl, mb)` take
`(view, mb)` with shift 0 (strip rows, road quad, branch sidewalk). The consumers of the old
lateral were each taught the free geometry in their own place:

| consumer | what changed |
|---|---|
| strip rows, jump table, `LEFT/RIGHT.TRK` heading bytes | rows from the view (`td5_trackgen.c`, `tg_emit_routes`) |
| main half | `tg_fork_main_shift` / `tg_pf_main_wscale`: the route's own `la` lanes at every node width the wedge ramps through |
| ring road between the classic zones | NOT a fork span: `tg_fork_of_main`, `tg_span_in_fork_clear`, `tg_fork_side_at`, the far-band fork gate and the strip's main-half rows stop at the classic zone + wedge, so the plaza arc's lanes, sidewalks, lawn, ribbons and streets are exactly what they were |
| the corridor leaving the road (wedge, `TG_PF_WEDGE` = 7 spans) | a paved quad between the main half's right edge and the corridor's left edge (`tg_pf_emit_wedge`); the classic gore cannot, it is a lateral strip; the skirt starts at the road's own edge there (reach 0), or the crotch of each mouth was a slot to the sky (found on the coverage map) |
| carriageway reach | classic spans only (`tg_pf_reach`); the free rows are 100..180 m out |
| occupancy raster + ground bed | the free rows are painted DRIVABLE and the terrain conformed under them BEFORE the plaza ribbons are laid, so the ribbons (round 1014 B) stop short of the corridor: no second road on the far arc |
| far-band ground | the apron's ring points sample the natural terrain and its planes rose 1..3 m above the corridor (buried road: the car drove UNDER the surface); `tg_emit_far_band` pins ring 2 on the corridor crossing, 300 units under the road |
| avenue scenery (road, island, far footway, reach) | skipped where a corridor row lies within three lanes of it (`tg_pf_scenery_clash`) and over the wedge |
| preview points, geometry-safety ceiling, NETWORK.JSON | the corridor's own rows |

### Window rules (what the selector does)

A plaza candidate is `(F, R)` from the route alone: the straight street before the entry bend
and after the exit bend (heading within 1 degree over 9 nodes), the ring run (route nodes within
12 m of the ring way), and the free chain checked on the RAW route first (street axis meets the
ring, corner >= 8 m, stretch within `TD5RE_GEO_PLAZA_STRETCH_MAX`). Options per ring run: F ten
nodes before the bend, and right after each avenue fork candidate that ends on that street
(`F = R_avenue + 1`); R five nodes after the straight resumes, and right before each avenue
fork candidate that starts on the exit street (`R = F_avenue - 1`). The corridor carries
`min(ring lanes, route lanes)` lanes (the reader gives every plaza-named ring way the 3-lane
plaza floor, the street is 2), which makes lanes(A)+lanes(B) equal to the avenue forks' 2+2,
and that is what lets `rf_compat` allow end-to-end adjacency (full-width nodes of two forks
overlap only when they agree). Without adjacency the usual 6-node gap applies. On La Plata the
selector chose avenue 0 (94..205) + PLAZA AZCUENAGA (206..342) + avenue (343..526): the far
carriageway of Diagonal 73 runs on through both cross streets and round the ring, driveable end
to end. Plaza Dardo Rocha is REFUSED (`[PLAZA FORK] REJECT`, 2.23x to 2.49x): its far arc is
395 m against the route's 142 m, so a corridor with one span per main span has spans 7.8 m long.
It was built once with the limit at 2.3 and the AI batch showed it: seed 22 had 2150 wall events
and 8191 contact-ticks in the plaza window, seed 11 745 contact-ticks (the driver's corner cap
reads the normalised span ring, which is 2.2x too short there). Building it needs a
variable-length corridor (a jump table that maps more than one corridor span per main span) or a
stretch-aware driver, see "What remains".

Reasons are logged per refusal under `TD5RE_GEO_FORK_DIAG=1` (`[PLAZA FORK] REJECT ...`), and
`[PLAZA FORK] summary` names every ring plaza the route runs along and whether it got a fork.

### Knobs

`TD5RE_GEO_FORK_PLAZA=0` restores master (plaza report only). `TD5RE_GEO_PLAZA_STRETCH_MAX`
(1.6) refuses a plaza whose far arc needs spans longer than that many route spans (Azcuenaga
needs 0.97x, Dardo Rocha 2.23x).
`TD5RE_GEO_PLAZA_SMOOTH` (300) Laplacian passes, `TD5RE_GEO_PLAZA_LEAD_IN` (14) /
`TD5RE_GEO_PLAZA_LEAD_OUT` (10) how early the chain leaves / how late it rejoins the avenue's line.
`TD5RE_GEO_PLAZA_DUMP=1` writes `log/pf_chain_<fork>.csv`. Tools: `verify/geo_r1015e_strip.py`
(STRIP.DAT: types, lanes, continuity, clearance), `verify/geo_r1015e_run.ps1` (the 1013 harness
with `|` separated `-Extra` and a window size, for free-cam tours).

### Traps found

* **The scenery stream is not done when the race starts.** The corridor's entries are the LAST
  of the 457 (worker finished in 16.6 s); at the harness's 4x fast-forward the car reaches span
  273 before them, and the untextured fallback ribbon (flat grey, no lane paint) is drawn in
  their place. Free-cam frames of a corridor need `-FastForward 1.0` (this is also the "free-cam
  tour renders the corridor flat grey" of round 1014 A).
* The far-band apron is a 3-quad planar patch per 4 spans whose ring points sample the natural
  terrain: anything 100+ m out on a slope can be under it.
* A coverage map (rasterise MODELS.DAT's quads by kind, magenta = no quad) finds the see-through
  slots a frame misses.

## Round 1015 A: the window fits the room, the median breaks, the finish covers the avenue

Mariano's picks 1, 3, 6, 13, 17, 18 on La Plata (level091). Master e7d89db0, route derived 2026-10-09 21:27
(spans 1310, finish 1206): 3 real forks (F=94 / 343 / 945), census 71 arms accepted, 23 beyond an avenue, 0 corridor
drops. Note: that route's lane profile (3 lanes up to span 38 and 245..311, 2 elsewhere) is what refused the first fork.

**Window fit (`rf_fit_window`).** The full-width window F-8..R+2 and the ramps in front of and behind it used to need
the route's lane count constant over `[F-8-16, R+2+16]`. Now: the fork body F..R+1 keeps the exact count `lanes(A)`; the
approach nodes and the ramps may carry MORE lanes than `lanes(A)` (up to `lanes(A)+lanes(B)`): the node width is
`max(route lanes, lanes(A) + lanes(B) * f)`, so a 3-lane flare is only widened by what is missing (`extra =
want - route lanes`, the node moves half of `extra` toward the corridor). A node with FEWER lanes ends the ramp. Each
ramp is as long as the grid (`GRID+2`), the finish and the lane profile allow, 6 nodes at least (`TG_RF_TAPER_MIN`,
9.5 degrees), 16 at most. The first fork of a run SLIDES its start (up to 24 spans) to the first clean window, because
the road where an avenue begins is usually still turning onto it (Diagonal 73 leaves Calle 40 at spans 35..38).
`TD5RE_GEO_FORK_TAPER_FIT=0` restores the round-1014 rule.

| | master e7d89db0 | now |
|---|---|---|
| Diagonal 73, first run | F=94 R=205 (spans 47..93 scenery only) | F=46 R=209 |
| Diagonal 73, second run | F=343 R=526 | F=328 R=565 |
| Avenida 60 | F=945 R=1063 | F=901 R=1075 |
| Avenida 7 | none (window refused) | F=1130 R=1174 |

Still scenery-only: Diagonal 73 spans 209..236 and 315..327 (bend of 0.18..0.23 rad/span in the window), Avenida 13
(678..708, bend 0.47), Avenida 7 1115..1129 (bend) and 1175..1306 (the finish is at 1206, forks stop 8 spans short).

**Openings.** `AVENUES.JSON open` was measured at a RAW-polyline point within 1.7 m of a street's centre line: for a calle
at 45 degrees the footprint on the median is 3 spans long and the sampled span is a few spans off the written one. The
generator now re-measures at load (`td5_geo_avenues.c av_refine_openings`): conditioned route node + median midline,
nearest non-avenue way within its OWN half carriageway, parallel ways (<25 degrees) excluded. 32 -> 87 open spans over
the five avenues. In a real fork's window an open span gets no island (the neighbours get end caps through
`tg_median_at_raw`) and its gore floor is laid FLUSH with the road page (`tg_emit_gore(..., flush)`). The corridor itself
does not break: it is one span chain, and a car cannot cross the paved gap. `TD5RE_GEO_FORK_OPENING=0`,
`TD5RE_GEO_AVENUE_OPEN_REFINE=0`.

**Start of an avenue.** `av_backfill_starts` walks each avenue's first row back up to 12 spans on the same road graph
(same-name one-way way, anti-parallel, same side, 375 units per span): Avenida 60 now starts at 893 (was 904, the first
route vertex after the plaza), Diagonal 73 at 39 (was 47). `TD5RE_GEO_AVENUE_BACKFILL=0`.

**Holes.** `tg_geo_avenue_reach` took one span of slack ahead of the first row, so the pavement, railing and ground skirt
stood down over a span with no carriageway (`TD5RE_GEO_AVENUE_ENTRY_SLACK=1` restores it), and the skirt's inner edge,
pulled out to clear a carriageway that starts a node later, left an uncovered strip: `tg_emit_ground` now closes it with
a ground quad from the road edge (not on a fork's main spans, whose gore floor is at the same height). A quad with two
coincident corners is dropped by the renderer: the filler is at least 1 unit wide at both ends. A median that tapers to
nothing (under 0.7 m) now gets a flush paved sliver instead of an empty gap (`TD5RE_GEO_AVENUE_WEDGE=0`).

**Finish.** On a divided avenue (Avenida 7, span 1206) the gantry's far leg and the banner go to the far carriageway's
outer edge and a second chequered band is laid on it (`TD5RE_GEO_FINISH_AVENUE=0`).

**Not done.** Item 17 at span 853 (Avenida 13, 836..858): OSM does map the opposite carriageway 10 m away, but the route
drives that stretch AGAINST the way's one-way direction, so the in-direction carriageway is on the LEFT, and left
corridors are parked (`TD5RE_TG_NET_LEFT`). The detector also ends the run at 708 (the gap narrows from 15.6 to 9.7 m,
outside the +-1 lane band).

### Results (round 1015 E, master e7d89db0 route; same 3 seeds 11 / 22 / 33, all-AI field, 640 s)

`verify/geo_r1015e_run.ps1` (the 1013 harness), base = master exe, after = this branch with the
final defaults. Plaza window = ring spans 198..344 plus the corridor.

| | base s11 / s22 / s33 | after s11 / s22 / s33 |
|---|---|---|
| plaza window wall events | 51 / 34 / 78 | 17 / 18 / 12 |
| plaza window incidents | 15 / 14 / 15 | 5 / 6 / 5 |
| plaza corridor events | 5 / 2 / 1 | 16 / 13 / 17 |
| cars on the plaza corridor | 4 / 3 / 1 (of 6) | 4 / 3 / 4 |
| whole-route wall events | 297 / 583 / 280 | 183 / 279 / 654 |
| whole-route incidents | 70 / 74 / 73 | 63 / 65 / 65 |

Stalls inside the plaza window: base had plateaus at spans 226-229 and 243-246 (3 seeds), after has
none. The route total moves with seed chaos at spans 540-585 and 800-815, a pile-up site master
already has (route 529..936 events, base 192 / 501 / 149, after 70 / 179 / 584: 842 against 833
in sum). The corridor itself carries 13-17 events per seed (a 114 m corridor with a 25 m corner):
more than master's 1-5, but they were 245 events in the first cut and 0 stalls at the mouths now.

After merging master (integ-1015, group A's sliding fork windows) the plaza fork still builds:
`[REAL FORK] 1: plaza Azcuenaga F=210 len=116 R=327 lanes 2+2` between avenue forks 0 (46..209) and
2 (328..565), tightest corner 18.9 m, strays 4.2 m, strip check (types, lanes 2+2, continuity 0.00,
span 3.40-3.52 m), geometry-safety clean.

### What remains

* **Plaza Dardo Rocha** (route spans ~1068..1128): the far arc is 395 m against the route's 142 m,
  so a one-span-per-span corridor is 2.2x stretched. Needs a variable-length corridor (the jump
  table maps one main span to several corridor spans, `track_span_normalized` ring tables to give
  the driver the right corner radius) or a corridor-aware corner cap in the driver model.
* Left-hand corridors (a far arc on the engine-left) are unsupported: the walker's sub-lane
  bookkeeping is wrong there. Only plazas whose far arc is on the right are built.
* Squares bounded by streets (Plaza Moreno, ...) are not rings; they are not handled.
* The two mouths are still the spot where fast cars spin (13-17 corridor events per seed).
  Larger radii are the remedy; the driver's corner cap uses sqrt(R / 34000 units).

### Rebased on master 4e97638d (round 1015 A-D + F): check

Merged (not rebased) at `aadb5d13`; code merged without conflicts, group A's sliding windows kept.

* Synthetic gate on the merged build: MODELS.DAT 12772392 B `E2F1F33C221D61CAB0EFF701484011F0`
  (master's new baseline), STRIP.DAT 144714 B, TEXTURES.DAT 1605328 B.
* La Plata slot 61 census, seed 11: `72 accepted (street 63 avenue 8 continuation 1), 24 street(s)
  start beyond an avenue, corridor 0`, identical to master. `[REAL FORK]`: 5 forks, 730 corridor
  spans = master's 4 forks / 614 spans (F=46, 328, 901, 1130) + the plaza fork F=210 R=327 (116
  spans), nothing dropped; the plaza fork sits end to end between avenue forks 0 (..209) and 2 (328..).
* One AI seed (22), same exe, `TD5RE_GEO_FORK_PLAZA=0` (master behaviour) vs default:

  | | plaza off | plaza on |
  |---|---|---|
  | plaza window (ring 202..329) events / incidents | 39 / 11 | 19 / 6 |
  | plaza corridor events | 2 | 10 |
  | whole route events / incidents | 323 / 70 | 217 / 70 |
  | stall plateaus | 15 (one in the plaza, 227-229) | 12 (none in the plaza) |
  | cars on the plaza corridor | n/a | 3 of 6 |

### Plan: a plaza whose far arc is much longer than the route (Dardo Rocha)

Refused today (`TD5RE_GEO_PLAZA_STRETCH_MAX` 1.6): arc 443..485 m over 51..64 route spans, 2.2x to
2.5x. What the engine allows, from the code:

* The jump table record `[branch_lo, branch_hi, main_target]` (td5_track.c
  `td5_track_branch_corridor_span`) is strictly 1:1: main span m maps to corridor span
  `branch_lo + (m - main_target)`, and the parallel main range is `[main_target, main_target +
  (branch_hi - branch_lo)]`. A corridor therefore has exactly as many spans as the main window it
  bypasses, and a longer arc can only make each span longer. No format change gives N corridor
  spans for M main spans.
* So the stretch can only be cut by making the route window longer, or by making the AI and the
  progress logic aware of it.

Tried first, cheapest: longer leads (`TD5RE_GEO_PLAZA_LEAD_IN/OUT` 40, GenOnly on the same route).
Result: no change. The F option that would give the longest window (F=1031, right after the
Avenida 60 fork) is refused with "the street's axis never meets the ring": the street bends before
the plaza, so there is no straight run to lengthen. The 1068 option stays at 2.4x.

Steps that would work, in order:

1. **Corridor-aware driver tables** (td5_ai_driver.c): the corner cap and the lookahead read
   `track_span_normalized` as an index into a ring point table (`s_pt_count`, main ring only).
   Build entries for corridor spans from the strip rows, take the radius from the corridor's own
   rows, and scale the lookahead in spans by `1/stretch` (a corridor span is 7.8 m, so 8 spans
   look 62 m ahead instead of 28). Without this the cap is sqrt(R / R_ref) with R_ref = 34000 units and reads
   the main ring, so it drives the 14 m corridor corners at 145 km/h (the pile-ups measured at 2.2x).
2. **Progress**: a car on the stretched corridor advances one span per 7.8 m, so it gains span
   count 2.2x faster than on the main road: race position and rubber-banding see the long way as
   the short one. Weight the span count by the stretch, or leave it (taking the long way would
   then look like a shortcut to the position logic: checkpoints are by span index, so a car that
   takes the corridor is not penalised for the extra 300 m).
3. Lift `TD5RE_GEO_PLAZA_STRETCH_MAX` to 2.6 once 1 and 2 pass an AI batch with no more than the
   Azcuenaga window's event rate (17 / 18 / 12 events on seeds 11 / 22 / 33).
4. Verify with `verify/geo_r1015e_run.ps1` + `geo_realfork_report.py --windows`, on seeds 11 / 22 /
   33; the pile-up signature of the stretched build was seed 22: 2150 events, 8191 contact-ticks.

## Round 1016 K: corridor-aware AI, variable-length corridors, Plaza Dardo Rocha

Brief (orchestrator): make forks and lanes much more permissive WITHOUT AI pile-ups. Diagnosis in the
brief: "the AI reads the MAIN road's corner cap and lookahead even while on a corridor". Steps 1 and 2 of 3
here; step 3 (relaxing the bend / lane-change / left-corridor vetoes) is NOT in this round.

Harness: `verify/geo_r1016k_batch.ps1` runs N AI races of La Plata (`la_plata_partido`, slot 61) in
parallel, one directory each (`_r1016k_run1..6` next to the worktree: a copy of `re/assets`, `horns`,
`inputscripts`, `verify/*.ps1`), ~100 s for six seeds (`geo_realfork_run.ps1 -IdleStop 40` ends a run once
the trace has been silent 40 s). Judges: `geo_r1016k_report.py` (one run), `geo_r1016k_cmp.py` (table over
seeds), `geo_r1016k_inc.py` / `_stallmap.py` (where), `_corr.py` / `_geom.py` / `_aim.py` (one car through
a corridor, the strip's bends, where the AI aims), `_progress.py` (race progress on a variable corridor).
**Trap**: an AutoRace session starts a SECOND race when the first ends and `sim_tick` restarts; every judge
reads the first race only (the round-1015 reports mixed the two for the seeds that finish early).

### What was actually wrong (measured on the 1015 master, 3 seeds, 18 cars)

1. **The branch-road ADAPT block undid every brake on a corridor.** `td5_ai_smart_speed` forces any
   coasting/braking car on a span past the ring back to throttle 0xA0 with the brake OFF. The corner cap, the
   wall ray and any governor that ran before it were therefore dead on every corridor.
2. **The SMART corner cap is not a governor.** It scales the THROTTLE COMMAND by 0.45..1 and brakes only below
   0.55; at 700..850 units/tick (190..230 km/h, what a 3.5 m-span avenue gives) that is no brake at all. Every
   car reached Plaza Azcuenaga's corridor at 700..850 and met a 24 m bend 25 spans in: steering at the +-1.0
   lock, rear slip 35000..50000, full lock + throttle against the wall for 100..250 ticks (no recovery on a
   branch: `smart_on_branch` cancels scripts). 0 of 11 plaza passes were clean.
3. **The corner/ray/heading look-ahead walked `span_raw + d`**: off a corridor's last span it runs into an
   UNRELATED corridor, and from the main road it never followed the fork the car had committed to take.
4. **Adjacent forks** (Diagonal 73's corridor ends at R=209, the plaza fork's F is 210): `td5_ai_smart_branch`
   treated the REJOIN span (type 11) as a fork, rolled for it (burning an RNG step) and never saw the real fork
   in time, so the car aimed at main span 213 while the walker put it on the corridor.
5. **Heading rows**: the route-table heading was read at the PARALLEL MAIN span; a corridor that turns away
   from the road (a plaza's far arc) fed `fwd_comp` a heading up to 90 degrees off.
6. Every plaza pass arrived with the velocity 10..20 degrees off the corridor: the two median openings (6.8
   degrees each, pinned at the mouths) make a 13.7 degree kink where one corridor ends and the next begins.
   Geometry, not AI; an AI that brakes for it passes (see "What step 3 should relax first").

### Step 1: the AI follows its PATH (all behind knobs, generated tracks only)

`TD5RE_AI_CORRIDOR=0` restores everything in this list; shipped tracks are untouched by construction
(`s_corr_on` needs a generated slot; 3 shipped tracks traced: driver/motion/track CSVs bit-identical to master).

| piece | change |
|---|---|
| `ai_path_next` / `smart_build_path` | span+1, except a committed fork (type 8, `link_next`) and a corridor end (type 10, `link_next`); circuits wrap at the ring |
| corner eval, ray sensors, heading look-ahead | read the spans the car WILL drive (they used `span_raw + d`) |
| branch scan | walks the path, scans from d = 0 (the pull stays on the fork span), skips the merge span of the travel direction |
| aim point | 6000 units down the path, in METRES not spans (a free corridor's spans can be 2.5x longer), crossing into the corridor with the car |
| lane brain | on a corridor: the road frame, lane count and the geo wall margin of the CORRIDOR span (the margin was skipped on every branch) |
| route heading | the corridor's own table row (`tg_emit_routes` already writes one per corridor span) |
| traffic | its corner eval no longer runs into the next corridor |
| **speed governor** (`TD5RE_AI_CORR_GOV=0` off) | along the path: bend radius over every 2-span window from the span headings, `v_corner = sqrt(a_lat * R)` floored at a yaw-authority minimum, allowed speed now = min over 36 spans of `sqrt(v_corner^2 + 2 a_brake d)`; coast over it, brake 8% over; runs LAST (after the ADAPT block) |

Governor parameters, swept over 6 seeds each (`TD5RE_AI_GOV_LAT` 14 m/s^2, `_BRAKE` 9.8, `_LOOK` 36 spans,
`_FLOOR` 340 units/tick, `TD5RE_AI_CORR_GOV_SCOPE` 1 = the whole route). Measured grip on this route: ~1.9 g in a
26 m bend, braking ~1.2 g (110 tick finish stop). **The floor is the key parameter**: below ~200 units/tick the
car has no yaw authority and stops against the corner's wall (the 90 degree street corners at 680..686 stalled
52 times at floor 220); floor 230 -> 38 stalls, 260 -> 21, 300 -> 8, 340 -> 7, 400 -> 9. A governor limited to
paths that touch a corridor (`SCOPE=0`) leaves the same stall sites on the ring (568..583, 800..815, 1084..1090)
that master already had; the whole-route one removes them.

#### Results (La Plata, all-AI field, 6 cars, damage off, Traffic 0; judge = `geo_r1016k_report.py`)

12 seeds (11 22 33 44 55 66 | 77 88 99 111 122 133, the second six were not used to tune):

| | master cdce612c | this branch |
|---|---|---|
| stall plateaus (>= 90 ticks within 2 spans) / ticks | 126 / 19670 | 10 / 1662 |
| contact events / incidents (merged) | 2907 / 653 | 1180 / 482 |
| corridor passes / clean (no contact event, no stall) | 154 / 25 | 202 / 127 |
| spins (a pass that hit a wall and lost the speed, or reversed) | 19 | 4 |
| mean tick to span 600 / 1100 | 2005 / 3588 | 2164 / 3776 (+8% / +5%) |

The parent's 3 seeds (11 22 33): stalls 32 / 4924 ticks -> 1 / 100, events 697 -> 209, incidents 169 -> 122,
clean 8 of 45 -> 28 of 47, spins 6 -> 0. Per fork on those seeds (passes / clean; master has no Dardo Rocha fork,
its fork 4 is Avenida 7):

| fork | master | this branch |
|---|---|---|
| 0 Diagonal 73 (F=46) | 11 / 4 | 11 / 5 |
| 1 Plaza Azcuenaga (210) | 11 / 0, 3 stalled, 213 events | 6 / 0, 0 stalled, 20 events |
| 2 Diagonal 73 (328) | 11 / 4 | 7 / 7 |
| 3 Avenida 60 (901) | 5 / 0 | 10 / 8 |
| 4 Plaza Dardo Rocha (1031) | (refused) | 6 / 1, 0 stalled, 7 events |
| 5 Avenida 7 (1130) | 7 / 0 (4 spins) | 7 / 7 |

Other tracks: the synthetic auto track (seed 99991, 3 seeds): stalls 89 -> 65, incidents 223 -> 180, clean 9/55
-> 32/56, spins 15 -> 0; shipped tracks Moscow / Edinburgh / Newcastle: AI traces identical. With Traffic 6 (3
seeds): stalls 42 -> 36, incidents 157 -> 133, clean 6/43 -> 26/52. Full selftest 64/0/0 (some runs show a WARN on
a peak-jump threshold in a chaos/cop step with either exe; it is frame-timing noise).

Not forks, now gone with the governor: the stall sites at 568..583, 800..815 and 1084..1090 (bends the cars
crawled through on master). The plaza mouths still scrape (edge contacts, no stalls).

### Step 2: variable-length corridors

A corridor may have N spans beside M main spans. Used by the plaza (free) corridor only: its classic rows
(entry 0..k1, exit) stay one per main node, its free rows are one per `TD5_TG_SPAN_LENGTH` of the arc.

* **Plan** (`td5_tg_plazafork.c`): `nfree2 = round(chain length / 1500)` (12 .. 4x the main nodes), the chain is
  validated again at that row count; `clen = k1 + nfree2 + kx`. `TG_Fork.clen` (== `len` for every other fork).
  `TD5RE_GEO_PLAZA_STRETCH_MAX` now caps the arc / route ratio (default 3.0; Azcuenaga 0.98, Dardo Rocha
  2.21); `TD5RE_GEO_PLAZA_VARLEN=0` + `STRETCH_MAX=1.6` give master's bytes back (hashes checked: STRIP 135144 B
  EBE847F2, MODELS 15700624 B 2D3607EB, TEXTURES 1605328 B 5B4E005A, identical to master's exe).
* **Two views** of a free corridor: `rview[F+1+k]` is corridor ROW k (strip rows, road mesh, routes, preview,
  network paint) and `view[F+1+j]` the corridor beside MAIN node j (throat / wedge / reach / clash code), so the
  1015 code that pairs a main span with the corridor next to it is unchanged. `tg_fork_row_node(fi, k)` = the
  main node a row stands beside (identity for every other fork).
* **Strip**: the corridor spans are appended as before (`cbase .. cbase + clen - 1`), the jump record covers
  all of them, and a TRAILER after the vertex table carries the map: `u32 'CMRP', u32 n, per fork {u32 lo, n, m,
  base, u16 off[n+1]}`, `off[k]` = main offset corridor span k stands beside (`off[n] = m`). Only forks with
  N != M write one, so every other strip is byte-identical (synthetic gate: MODELS 12772392 B E2F1F33C, STRIP
  144714 B, TEXTURES 1605328 B, identical to master's exe).
* **Engine** (`td5_track.c`): `corr_map_*`. `td5_track_branch_to_main_span` (the normalised span), `main_to_branch_span`,
  `count/branch_corridor_span`, `corridor_info` (main range = base .. base + m - 1), `branch_to_junction`,
  `route_junction_path2a_match`, `apply_target_span_remap`, `resolve_actor_segment_boundary` read it.
* **Progress** (decision): the accumulator `span_accum` and the high water the race order sorts by are in
  MAIN-ROAD spans. After every chassis walker step the accumulator is restored to
  `accum + (mainIndex(new) - mainIndex(old))` where `mainIndex(corridor span) = base + off[k]` and a main span is
  itself; the walker's own +-1 is right everywhere else. So lap, finish, checkpoints, race position, the AI's
  rubber band and the minimap read main spans on the corridor (verified: `|span_accum - span_norm| <= 2` on every
  tick of 12 AI races; the 2 is the tick a car is between two spans). A car on the long way is not ahead of the
  car on the short way; it gains main spans as it covers them.
* **Recovery**: `td5_track_get_recovery_pose` stepped back with `from_span % ring`, which on a corridor span is an
  unrelated main span (the breakdown path steps back 30); it now steps back ALONG the corridor and past its start
  onto the main road it left.
* Traffic spawns by main span -> corridor span through the same API (checked: Traffic 6 races spawn on
  the 172-span corridor with `norm`/`acc` right).

Plaza Dardo Rocha is built: F=1031 R=1129, 97 main spans, 172 corridor spans (32 classic rows in, 3 out, free chain
478 m over 137 rows, span pitch 1.00x), tightest corner 26 m; Avenida 60's fork became F=901..1030 (it ended at
1075 before). Azcuenaga was rebuilt through the same path (114 spans beside 116). Frames at spans 1040 / 1060 /
1095 / 1125 checked by eye (entry from Avenida 60, the arc with its kerb and lawn, the merge into Avenida 7 with its rail).

### Knobs added

`TD5RE_AI_CORRIDOR=0` (all path-following), `TD5RE_AI_CORR_GOV=0`, `TD5RE_AI_CORR_GOV_SCOPE=0|1`,
`TD5RE_AI_GOV_LAT` (tenths of m/s^2, 140), `_BRAKE` (98), `_LOOK` (36), `_FLOOR` (340 units/tick),
`TD5RE_AI_GOV_DIAG=<slot>` (dev: one log line a tick), `TD5RE_GEO_PLAZA_VARLEN=0`.

### What step 3 should relax first (measured, in order of value per risk)

1. **Adjacent forks (R+1 == F)**: Diagonal 73 -> plaza -> Diagonal 73 is three forks back to back and each pins its
   median to 0 at the shared span, so the corridor weaves in and out by 3.7 m with a 6.8 degree kink at each
   throat (13.7 degrees where two meet). With the governor it passes (plaza mouths: edge scrapes, no stalls) but a
   merged corridor with no pin at the shared junction would remove the last mouth incidents (fork 1: 20 events on 6
   passes).
2. **Constant lane count in the window** (Avenida 60 893..950, 893..1030 refused: "the route's own lane count
   changes inside the window"): an AI-side non-issue now (the corridor reads its own lanes); the generator's
   one-number lane arithmetic is the only reason.
3. **Bend cap 0.044 rad/span**: Diagonal 73 209..236 and 315..327, Avenida 13 678..708 are rejected for 0.18..0.35
   rad/span bends. The governor reads the bend of the path now, so the cap can follow geometry (does the shifted
   carriageway fold? an offset of half a lane-pair from a 10 m radius bend is the limit), not a fixed number.
4. **Left corridors**: still parked (walker sub-lane bookkeeping); needs engine work, not AI work. Last.

### How step 4 (perpendicular block loops) and N-way / nested / overlapping corridors plug into this design

Orchestrator constraints, answered against what exists after step 2. Nothing below is built; it says what the
representation already carries and what each wish still needs.

**Representation.** A corridor is a jump-table record `[lo, hi, base]` plus, when N != M, a `CorrMap`
(`off[k]`, `first_k[j]`) read by `corr_map_*`; its geometry is a list of free rows (`rview`); AI, traffic,
minimap and progress reach it only through `td5_track_branch_to_main_span`, `main_to_branch_span`,
`count/branch_corridor_span`, `corridor_info`, `corr_progress_delta` and `ai_path_next`. None of that assumes
one corridor per main span or non-overlapping windows: `count_branch_corridors` already enumerates every record
that covers a main span (the original TD5 tracks use two). No rewrite is needed at this layer.

| wish | what already works | what it still needs (cost) |
|---|---|---|
| (a) mouth at the SIDE of the road, 90 degrees | the free chain is arbitrary geometry; the AI aims 6000 units along the corridor's own rows and the governor reads the corridor's own bends (it already takes the route's 90 degree street corners at the 340 units/tick floor); N may be 3-6x M (the plan caps `nfree2` at 4x the main nodes: raise it; the map arrays hold 4000 spans per corridor) | the planner demands `k1 >= TG_PF_K1MIN` classic rows beside the road before the chain (`k1 = 0` + a chain pinned perpendicular to the road edge is a planner change, ~150 lines); the strip's type-8 span carries the lane the car must be in to take the branch (`sub_lane >= lanes(F+1)`), so the mouth lane has to be part of F's cross-section (a stub lane on the side), not a gap |
| (d) the mouth as a GAP in the main road's wall/kerb | nothing | **engine work, the one hard blocker**: collision walls are the strip rows' own rail vertices, there is no per-span "open edge". A real gap needs a span flag (or a type) that drops one rail's wall in `wall_contact` plus a geometric (not sub-lane) branch decision in the walker. Without it the corridor must start from a widened F row (above) |
| (b) sharp corners in a corridor | governor from the corridor's own heading steps; metre-based aim | a floor below 340 for 90 degree corners at low speed (the street corners already in the route stall below ~220: a floor under 260 is a regression) |
| (c) 3-6x length | `N != M` map, progress in main spans, accumulator compensation | `TD5_TG_MAX_SPANS` 3000 (La Plata uses 2171 with both plazas), 12 forks (`TD5_TG_BRANCH_MAX`), the jump-record block (32 records) |
| N-way (several corridors off one main span) | several records may cover a main span; per-record `CorrMap`; minimap and traffic spawner enumerate all of them | the walker: a type-8 span has ONE `link_next` and decides by `sub_lane >= lanes(F+1)`; a third way needs a second link (a trailer table keyed by span) and a lane BAND per destination (`resolve_neighbor` case 8, ~60 lines); the AI commit is one bit per slot (`g_smart_branch_commit_take`, `td5_ai_smart_branch` pull +-1): make it a way index and the anchor a lane-band centre; traffic's fork choice is a bit too; cross-section lanes(A)+lanes(B)+lanes(C) <= 8 (the rail range) |
| (b) overlapping windows | the engine does not care | the generator: `tg_fork_of_main(si)` returns ONE fork, the main half's shift/width scale (`tg_fork_main_shift`) is one number per span, and `rf_select` forbids overlap. Needs a list of carriageways per node, i.e. the strip's lane arithmetic generalised from 2 parts to k. The biggest generator change |
| nesting / corridor-to-corridor rejoin | `ai_path_next` follows any `link_next`; progress deltas use `corr_progress_index` | `corr_progress_index` returns the span itself for an UNMAPPED corridor (1:1): give every jump record a map entry (or a linear fallback) so a step between two corridors has a main-index on both ends; `corr_progress_delta` only fires when one end is mapped; `branch_to_main_span` maps to the PARENT's main index (a nested corridor's `base` = the parent's index at its mouth) |

Limits to design around, with their cost: the AI branch decision is a per-slot boolean rolled once per fork when
it comes into range (one RNG step, replicated seed): an index costs nothing, a lane-band anchor needs the lane
brain to know the corridor's lane count at the mouth. Race progress is correct for any corridor that maps to main
spans, so a nested or overlapping design only has to say which main span each corridor stands beside. The minimap
draws a corridor per MAIN span (one point per main span, so a 6x corridor is subsampled 6:1: iterate the
corridor's own spans instead, ~30 lines in `td5_hud.c`). Replay and netplay carry spans only (no new state).
Verdict: no rewrite before step 4; the three items that cannot be avoided are the walker's wall-gap/multi-way
junction (engine), the generator's k-carriageway cross-section (for overlap) and the planner's `k1 = 0` entry.
