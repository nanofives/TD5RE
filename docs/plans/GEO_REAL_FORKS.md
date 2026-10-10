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
