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
