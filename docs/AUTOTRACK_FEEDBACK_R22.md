# Autotrack feedback round 22 (2026-09-09)

The first manual-drive defect list against the **topology-first** generator
(master `61d7c964`, merged the night before; `5594e72e` with its water-table
fix is the base of this branch). Branch `feat/autotrack-r22`. Reporting seed
**2082186171**, which the report named and which reproduces exactly.

Numbered R22 after R21's studio randomization round.

## Verbatim items

1. on auto track studio the indicators of start and finish are not following
   the actual end of the track
2. all the options in the auto track studio and in reality should be random
   instead of showing the `~`
3. the seed should be a text input
4. there's basically no geometry rendered here, only tiles at the side of the
   road, no buildings, no scenery or trees
5. there's a height gap between `AUTO e8 s4 skirt p5:GROUND` and the road
6. you can keep the topology as it is but the road has to have a level of
   smoothness of how it tackles the terrain, otherwise everything feels sloppy
7. part of the grass `AUTO e81 s5 road p2:GREEN` is rendered over the road
8. there's a triangle without geometry near `AUTO e84 s0 skirt p2:GREEN`
9. there's an inexplicable shadow on `AUTO e95 s10 road p43:ROAD_VARIANT`
10. the water levels are not consistent, `AUTO e96 s15 water p39:WATER` is
    rendered over the road
11. the whole median on `AUTO e194 s11 road p2:GREEN+121` is not taking the
    whole space, there's only median near the right track but no geometry near
    the left track
12. topology left some edges without geometry near `AUTO e198 s6 skirt p5:GROUND`
13. this bridge `AUTO e222 s5 road p83:BRIDGE_DECK` is too short and looks
    sloppy, bridges should be longer and help aliviate the steep changes in
    height as well as passing over rivers
14. `AUTO e230 s7 road p42:ROAD_VARIANT` snow road near green grass should be
    incompatible
15. if there's no buildings around you should trim less of the topology near the
    road so more scenery can be seen
16. topology should go over tunnels
17. this bits of grass are rendered inside the tunnel
    `AUTO e398 s4 skirt p2:GREEN`

## Reading the labels

`AUTO e<entry> s<slot>`: `entry` is the MODELS.DAT display-list index and one
entry is `TD5_TG_SPANS_PER_ENTRY` = **4 spans**, so `e<N>` covers spans
`4N..4N+3`. `slot` is the submesh index inside the entry, which is generator
emit order. `kind` comes from MESHTAG and is the **guard policy** kind, not a
material: everything in the road group (road quad + fork gore + avenue divider)
is marked `road`, which is why a grass gore prints `road`. `r` is the
bounding-sphere radius, `v` the de-indexed vertex count (4 per quad), `c` the
command (page-segment) count.

So: e8 -> spans 32-35, e81 -> 324-327, e84 -> 336-339, e95 -> 380-383,
e96 -> 384-387, e194 -> 776-779, e198 -> 792-795, e222 -> 888-891,
e230 -> 920-923, e398 -> 1592-1595.

## Item 4 was not what it looked like: the corridors are bare

Measured over the seed's own MODELS.DAT, per display-list entry:

| | entries | mean meshes/entry | min |
|---|---|---|---|
| main ring (spans < 1800) | 450 | 29.9 | 12 |
| **fork corridors (spans >= 1800)** | 68 | **8.8** | 2 |

Every one of the 44 entries in the whole track with 8 or fewer meshes is a
corridor entry -- a clean separation, with nothing in the main ring close to it.
Corridor entry e482 holds 8 meshes, all 12-vertex 3-quad, pages 0 and 44 only.
The comparable main-ring city entry e199 holds 34 meshes across 13 pages:
facades, billboards, street furniture, rails, crossings. The "tiles at the side
of the road" are those page-44 verge quads, and they are all that is emitted.

That is **272 of 2072 spans, 13% of the drivable strip**, with no scenery by
construction. The gate is one line, `td5_trackgen.c`'s scenery loop:

    if (si >= ring) continue;   /* pad + corridor carry road only */

It is not an oversight that can be deleted: every L2 hook takes a `TG_FBHook`
carrying `nl` and `si` and reads `nl->v[si]`, and a corridor span has **no node
in the centreline** -- the road for those spans is built from the main node its
corridor runs beside plus a lateral bow. Emitting corridor scenery therefore
needs a frame for corridor spans (main node + lateral offset) that every
outboard emitter honours; an emitter that ignored the offset would place its
scenery on the main road and z-fight it. Left for its own round, with the
measurement above as the acceptance test.

This also explains why driving found nothing: the start, span 34, span 799 and
every span the report labelled are all dense. Only the density sweep located it.

## Two premises in the report that measurement contradicted

### Item 3: the seed already WAS a text input

Enter on the SEED row opens `at_seed_edit_begin`, and keyboard Enter does reach
it (`td5_frontend.c`'s `TD5_NAVKEY_ENTER` assigns `s_button_index =
s_selected_button`). What was missing was the only part a player can see: the
row always drew the **committed** env value, never the edit buffer, so pressing
Enter and typing changed nothing on screen. A text input with no feedback reads
as a dead number. Fixed by drawing the live buffer plus a caret -- the idiom
`td5_raceopts_span_edit` already uses for the frontend's other digit field.

### Item 2: 24 of the 47 rows were already genuinely random

Only the **23 PRESENCE rows** were fake, and deliberately so: R21 shipped them
on `k_tgr_w_keep = { 0, 100 }`, weighted to today's value, and its own round doc
called that "a later round tunes one byte". The STYLE rows (twistiness, corners,
gradient, hills, length, rails, sky, ...) have carried real weights since R21.

## What shipped

### Items 1, 3: the studio

START and FINISH are now **published by the generator** (`grid_span`,
`finish_span` on `TD5_TrackGenPreviewStats`) instead of guessed by the screen.
The screen drew start at point 0 and finish at `ring_len`; the grid is an
interior span (24) and the finish is placed RUN-OFF spans before the end of the
ring **and then walked further back** out of any fork gore or tunnel bore it
lands in. So the finish marker sat past the end of the race by the whole RUN-OFF
row -- 100 spans by default, up to 400 -- and no caller could have computed it,
because `tg_finish_span` consults the fork and structure tables. The struct
field's own comment said "finish lives here", which is the stale premise the
screen trusted. `-1` (a ring too short to hold a race) draws no finish dot,
which is correct: there is no finish line in the level either.

### Item 2: the presence rows really roll

Graded OFF rates by how much a row removes, not by taste: `k_tgr_w_scarce`
(12/88) for rows that take away a whole layer of the world, `k_tgr_w_mostly_on`
(25/75) for a feature, `k_tgr_w_even` (50/50) for a detail nobody would call
missing. Salts are untouched, so re-weighting changes *which* seeds drop a row
without renumbering anything.

R21's worry was legitimate, so the hedge moved from the weights to a **presence
budget**: a cap on how many presence rows may be absent at once, restoring by a
fixed `pres_rank` (1 = restore first) so the budget can never become its own
source of variance. It runs inside `td5_trackgen_resolve_rolls`, the one
function both the studio display and the build consume -- a budget applied
anywhere else would let the screen promise a row the track does not have.
Pinned rows are counted but never restored: six things switched off by hand is
the track the player asked for, and counting them stops "pin three, roll four"
producing the seven-holes-at-once track the budget exists to prevent.

**The cap is measured, not picked.** Over 20000 seeds the raw absence count
averages 5.45 (sd ~1.9):

| cap | budget binds | a rank<=8 row actually stays OFF |
|---|---|---|
| 4 | 67.8% | 18.2% |
| 6 | 28.3% | 46.6% |
| **8** | **6.2%** | **60.9%** |
| 10 | 0.7% | 63.5% |

This first shipped at 4, where the budget fired on two thirds of all seeds and,
restoring lowest rank first, held the structural rows ON almost always -- which
is precisely the "only pretending to be random" behaviour the item is about. 8
fires on 6% of seeds and captures nearly all the available variety; past it the
curve is flat while the number of simultaneous holes keeps growing. Reported as
`[R22 PRESENCE] N of 23 presence row(s) OFF (budget 8), M restored by rank`.

### Items 16, 17: the ground skirt and tunnels

`tg_emit_ground` runs on **every** span with no tunnel test anywhere -- not at
the call site, not in `tg_ground_side_raw`. Two tunnel-only corrections in the
cross-section:

- **Start outside the bore.** The profile's first point sits at the road EDGE
  (`d = 0`) while the bore's visible wall is pushed a further
  `TD5_TG_TUNNEL_WALL_T` (300) outward, so the first quad ran laterally inside
  the tunnel for that thickness -- a green sliver along the wall/ceiling
  junction. Forks are excluded from structure runs, so a tunnel bore is never
  laterally shifted and the clearance is just the wall thickness.
- **Sample the natural surface.** `tg_world_h` carries the conform overlay, and
  `tg_apply_elevation` conforms the world to the road bed at each structure
  run's **mouth**. On the first and last tunnel span that overlay returns ROAD
  level, so the skirt was laid at bore-floor height, inside the tunnel -- the
  largest of the reported patches. A tunnel span has no exposed bed to conform
  to; its bed is the bore. `tg_world_h_base` is the pre-conform surface.

Together these are also item 16: with the natural surface restored the two
skirt slabs climb the hill above the bore instead of stopping level with a
road-width slot cut through it.

### An inconsistency fixed that turned out NOT to be item 10

`tg_road_shore_build` marched its outward ray comparing raw
`tg_world_h < tg_world_water_y`, bypassing `tg_world_is_water` -- the one
function that carries the rule the world module exists to enforce: a
**conformed cell is a road bed and is never water**, "even where it lies below a
neighbouring river's surface (a cutting beside a river)". The shore build was
contradicting a rule written down three functions away, so it now asks
`tg_world_is_water` and marches on to the real bank.

**Measured, it changes nothing on the seeds tested, and it does not explain the
report.** The `[WATER]` line reports the count under the new *and* the old raw
test in one run:

| seed | over water | coastal | shore inside the bed (new) | (old raw test) |
|---|---|---|---|---|
| 2082186171 (rivers 96%) | 17 | 228 | 0 | **0** |
| 777 | 65 | 511 | 0 | **0** |

No open span on either seed has water within a bed-width of its edge. The fix
is kept because it removes a real contradiction and because the counter is a
permanent invariant check, but item 10 is **not** fixed. See "Not delivered".

Worth recording as a method note: the first version of this counter reported
only the post-fix number. It came out 0, and 0 *looks* like success -- it says
the invariant holds, not that anything was ever wrong, because both sides of the
A/B were the fixed build. Reporting both tests in one run is what turned a false
green into a real result.

### Item 13: bridges are too short

A span's kind is the **OR of its two end nodes**, so a single wet node yields
exactly two BRIDGE spans -- and `TG_ROAD_WATER_MIN` is 2, so that two-span deck
survives as the shortest legal bridge: 3000 units of road, 12 vertices. The
classifier cannot help; it only ever trims or deletes a run, there is no pass
anywhere that lengthens one, and its own comment on `WATER_MIN` promises
"(+abutments)" that were never written.

Raising `WATER_MIN` is the wrong fix: a run shorter than the minimum is
**deleted**, so a higher minimum turns each stream crossing into a causeway
through the water, which is worse than a short deck. `tg_road_pad_bridges` grows
the run instead, from both ends together so the deck stays centred on the water,
until it reaches `TD5RE_R22_BRIDGE_PAD` (10) or `TD5RE_TG_BRIDGE_MAX`. It runs
after the walk on the final table -- padding during the walk would extend into
spans a later revision reclassifies -- and mirrors every guard the classifier
applies (grid straight, fork runs, the bridge/tunnel interlock), so it can never
produce a table the classifier itself would have refused. Everything downstream
reads that table, so the chords, the clearance hump, the skirt's `open` flag,
the water emitter and the guard all see the padded run.

Reported as `[R22 BRIDGE PAD] N short deck(s) grown by M abutment span(s)`.

A/B on seed 2082186171 with `TD5RE_R21_ROLL=0` pinned, so the roll re-weighting
could not confound it:

| | bridge runs | bridge spans | longest | conformed open spans |
|---|---|---|---|---|
| `BRIDGE_PAD=0` | 5 | 52 | 14 | 1735 |
| `BRIDGE_PAD=10` | 5 | **57** | 14 | **1730** |

Two short decks grown by 5 abutment spans. The run count and the longest run are
unchanged -- it padded the short runs, not the long one -- and the conformed
count falls by exactly the 5 spans that stopped being open, which is the
cross-check that the padded spans really did become structure. Seed 777 grows 4
decks by 21 spans. Both stop short of the target of 10 because those runs reach
the grid, a fork window or the bridge/tunnel interlock first.

## Not delivered

Each of these is diagnosed to file and line with a confirming measurement, so
the next round is implementation rather than investigation.

- **Item 4, corridor scenery.** Needs a frame for corridor spans; see above.
- **Item 10, water over the road.** The shore-ray inconsistency above was the
  candidate cause and measurement rejected it: 0 under both the old and the new
  test on two seeds, one with rivers at 96%. So the e96 quad's surface comes
  from one of the **other** authorities. There are four, and they are
  independent: the per-side shore height; the bridge-run **minimum** over wet
  nodes (so a run's flanking open spans, which use their own shore height, step
  at every run end); a **deck-relative** level used on DRY viaducts
  (`deck_y - CHASM - 300 + 100`, unrelated to any water); and sea level returned
  for **any** cell below it, connected to the sea or not, so an inland pit
  becomes sea-level water beside a river-surface neighbour. Each quad then
  extends `TD5_TG_WATER_EXTENT` (50000) outward. That set is also the whole of
  "the water levels are not consistent". Next step is to identify which
  authority produced e96 (spans 384-387) rather than to change the shore ray
  again.
- **Item 13's second half, grade relief.** There is no path at all from grade to
  a structure. Steepness is absorbed entirely by the causal grade limiter
  bending the road down onto the terrain; a viaduct only appears once the
  limiter has *already* left the road over 2000 above ground, which happens in
  brief 1-4 span dips that `TG_ROAD_BRIDGE_MIN` (6) then deletes. Making bridges
  relieve steep ground means letting the profile stay high across a dip, i.e.
  changing the order of the profile and structure passes -- the topology
  pipeline detects structures *after* solving the profile.
- **Item 6, road smoothness.** Same root: this seed cuts up to **12214 units**
  deep and fills up to 5672, while the ground skirt reaches only 12000
  laterally. The road is trenched into the terrain, which is both the "sloppy"
  feel and part of why scenery is not visible.
- **Item 7, grass over the road.** `TD5_TG_GORE_OVERLAP` (240) deliberately
  pushes the gore floor into the carriageway while `TD5_TG_GORE_DROP` is only 4
  units, which is a z-fight, not an underlap (the ground skirt uses 70 for the
  same job). Worse on a bend: the gore is ONE flat quad chorded between two node
  frames while the road is 3 subdivisions of the curve, so its straight
  road-side edge bulges toward the inside of the bend well past 240. The guard
  cannot catch it -- `road`-kind meshes are `TG_GKC_EXEMPT` and
  `tg_guard_quad_bad` returns before measuring anything, so the reassuring
  "0 road/branch-road/deck/gantry/tunnel/end-wall meshes rejected" summary is a
  tautology over exactly the never-reject set, not evidence.
- **Item 11, one-sided median.** With `side = -1` the main carriageway occupies
  laterals `[w(0.5-fm), w*0.5]`, so its inner edge is at `w(0.5-fm)`, not 0 --
  but the gore runs `+240..bl` and the island `bl..0`. For any asymmetric split
  (`fm != 0.5`) that leaves a bare strip of width `w(0.5-fm)` beside the main
  carriageway while every raised piece hugs the branch side; on a 6-lane MAJOR
  fork (`fm = 1/3`) that is 1500 units, a full lane. Already parked in
  `AUTOTRACK_BRANCH_REWORK.md`. `tg_emit_avenue_divider` also has no
  `tg_fork_side` handling at all, so a LEFT corridor gets a mirrored gore and no
  median.
- **Item 14, snow beside grass.** The ALPINE row itself pairs `road_surf =
  RS_ICE` with `ground_page = GREEN`; only the override in
  `tg_ground_page_for_span` papers over it, and that override does not cover
  tunnel spans or the dithered far band (ground uses the hard biome cell,
  the far band uses `tg_biome_for_span`, which dithers around cell boundaries).
- **Items 8, 12, holes and a bare triangle.** The far band's far end uses the
  **neighbour span's** profile (`se = e ? g1 : g0`, then the profile of `g1`) at
  a node whose skirt used `g1+1`, so every 4-span group boundary can open a
  ring; `TD5_TG_FAR_TUCK` (2000) is the whole margin. Separately, where a cap is
  driven to its floor the cross-section collapses to a 1-unit sliver and the
  slab degenerates to edge-on triangles.
- **Item 15, trim near the road.** `tg_topo_road_cap`, `tg_verge_reach` and the
  fold caps are all facade-blind. The only two places that consult facades cull
  *more* when buildings exist, never less when they do not. Also:
  **`TG_ZONE_TRIM` is declared and never begun**, so the `[TRIM]` pattern
  `verify/topo_gen.ps1` greps for matches nothing and the GENPERF "world trim"
  row is always 0.
- **Item 9, inexplicable shadow.** The generator bakes no shade into road
  meshes: every road/gore/quad vertex ships `0xFFFFFFFF`, auto-track meshes
  carry no normals, and the CPU lighting pass skips derived-normal meshes. So it
  is a runtime pass or the page. First A/B is `SunShadows=0`; if it persists it
  is the page (p43 is COBBLE, and a page change at a span boundary reads as a
  hard-edged dark patch).

## Measurement hygiene

The presence re-weighting changes **every seed's track**, because a different
roll is a different `spec_hash`. Seed 2082186171 now rolls BRANCHES off and so
has no corridors at all -- which is a different track, not a fix for item 4.
Pin `TD5RE_R21_ROLL=0` when measuring geometry, as the A/B below does, so roll
changes cannot confound it.
