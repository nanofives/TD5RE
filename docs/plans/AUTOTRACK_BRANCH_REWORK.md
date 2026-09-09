# AUTO TRACK -- branch rework (PARKED, 2026-09-08)

Requested alongside the topology-first generator refactor (see
`AUTOTRACK_TOPOLOGY.md`): keep today's fork behaviour, but rework branches so
that

1. lanes can be split in better ways than the five ladder kinds (AVENUE /
   ISLAND / WIDE / SLIP / MAJOR) plus BYPASS;
2. a car can drive onto a branch that is NOT a split of the existing lanes:
   a turn-off junction where the branch leaves from the road edge (a side
   street you can take), i.e. a type-8/11 pair whose corridor starts at the
   kerb rather than sharing the widened road's lanes;
3. LEFT-side corridors work end to end (see below).

## What exists today (after the topology-first steps 5 and 6)

- `td5_tg_network.c` plans a BYPASS corridor for long forks: a lateral
  profile clipped by the world (other streets, water, tan-30 ground),
  rate-limited, pinned to the ordinary mouth geometry; `tg_fork_br_shift`
  returns it for `TG_FORK_BYPASS`. Right side verified by driving (seed
  20260901 forks 609 and 806).
- `TG_Fork.side` (+1 left / -1 right) is carried through
  `tg_fork_main_shift`, `tg_fork_br_shift`, `tg_carriageway_reach`,
  `tg_side_blocked` / `tg_side_corridor_here` / `tg_side_geom` /
  `tg_pavement_side_width` (via `tg_fork_side_at`), the corridor sidewalk,
  verge, flora and gore emitters (mirrored edges + `tg_quads_mirror`
  winding flip), the skirt's branch clearance, and NETWORK.JSON.
- Engine (`td5_track.c`): `fork_left_compute` decides per type-8/11 span
  from the strip rows whether the corridor is on the LEFT (native tracks,
  computed in `td5_track_bind_runtime_pointers`); `resolve_neighbor` takes
  the mirrored decision path (geometric first, `sl < br_lanes` fallback,
  main `sl -= br_lanes`) for those spans; `laneassist_step_forward` and
  `td5_track_laneassist_target` mirror the lane band. Shipped tracks have
  no left forks, so they resolve to 0 and keep the confirmed path (full
  selftest 58/58 with the golden traces after the change).

## The open defect (why left corridors ship OPT-IN: `TD5RE_TG_NET_LEFT=1`)

Seed 20260901 with the coin on: fork 2 (slip, F=609 L=32 R=642) became a
LEFT bypass, `tg_strip_audit.py` and `tg_network_audit.py` pass, the engine
logs "fork span 609 (type 8): corridor on the LEFT" and the same for 642.
A lane-assisted drive from span 600 (`verify/span_capture.py`, top-down
9000 with the F12 collision wireframe):

- span 615: car centred on the wide 6-lane road, strip under it (OK);
- span 625: HUD `SPAN 625 sub=2 lanes=4`, `WHEELS --lr`, lane-assist
  `lat_err` small (-3) -- the walker says "main lane 2, centred" -- while the
  car is physically on the snow GORE between the two carriageways, the main
  strip's rails to its right-rear and the corridor's rails to its left
  (frames in the session scratchpad, `capleft2/capleft3`);
- no `jfwd_*` line was logged for the player at 609 (the AI-driven probe
  logs them on right forks), which points at the crossing being absorbed by
  the off-map rescue (`geo_rescue_offmap`) rather than `resolve_neighbor`;
- span 645 (after the rejoin): car back on the merged road, fine.

So the strip-side lane bookkeeping for a low-lanes branch is not right yet:
either the type-8 quad/edge tables (`k_quad_vertex_offsets[8]`,
`s_edge_mask_first[8]`, [CONFIRMED] original tables) assume the branch on
the high-index side, or the post-step `-1` / `sub_lane -= br_lanes`
arithmetic is off by one for the mirrored case, and the lane geometry the
car is steered to (`td5_track_get_span_lane_world`) lands in the gore.
Mirroring the lane rule in lane assist alone (done) does not change the
outcome, so the walker is the place to look next.

Until that is proven with a drive, `tg_net_bypass_plan` keeps every bypass
on the right unless `TD5RE_TG_NET_LEFT=1`.

## Suggested order for the rework

1. Instrument the walker at a left fork (trace the player's span, sub_lane,
   li/ri vertex indices and world lane centre per tick through 605..645)
   and fix the bookkeeping; re-record nothing (shipped tracks unaffected).
2. Then generalise the divider / gore for asymmetric splits (`fm != 0.5`):
   `tg_emit_gore` still starts at the road centre, not at the main half's
   edge.
3. Then the turn-off junction kind: corridor starting at the kerb with its
   own lanes (type 8 with `lanes(F) = lanes(F+1)`, branch lanes added, not
   split) -- needs the walker's `sub_lane >= next_lanes -> branch` rule to
   accept a branch whose lanes are not part of the fork span's count, or a
   new span type; `td5_track_branch_to_main_span` must keep working.
4. Better split kinds on top (lane gain profiles, staggered rejoins).
