# AUTO TRACK -- topology-first generator (shipped 2026-09-08)

Branch `feat/autotrack-topology`, steps 0..7 of the plan agreed on 2026-09-08.
Clean replacement of the road-first pipeline: old seeds change, no
compatibility knob. Modules: `td5_tg_world.c` (heightfield + overlay),
`td5_tg_road.c` (walk + structures + profile), `td5_tg_network.c` (street
graph + bypass planner + NETWORK.JSON), on top of the existing `td5_tg_*.c`
emitters and the unchanged strip/route/levelinf/models writers.

## Pipeline (td5_trackgen_build_level, td5_tg_pages.c)

1. `tg_biome_layout` (unchanged, seed hash, span-indexed cells).
2. **WORLD** `tg_world_build`: pure seeded heightfield -- continental fBm
   (wl 420000), masked ridged mountains (wl 50000, 1.5R), hills (wl 60000,
   0.12R), detail (wl 9000, 0.01R); sea level from the SEA roll; rivers as the
   zero band of a wl-380000 field carved 1400 under a surface 500 below the
   large-scale ground. A deterministic spiral finds a sampling offset where
   the origin is dry, flat and the +X lead-in walkable; h(0,0) == 0 so the
   grid spawn keeps y ~ 0. Sparse 32x32-cell chunks (cell = one span) hold the
   conform overlay (max-composited target height + weight), occupancy bits
   (ROAD / STREET / DRIVABLE / WATER / STEEP / NEAR) and painted biome.
   `tg_world_is_water` ignores conformed cells (a road bed is never water).
3. `tg_srand`, then **ROAD** `tg_build_centerline` (td5_tg_road.c): the
   section picker, R21 biome mix and shape hooks, heading budget, lane
   management, fork windows and rollback survive; each section is simulated
   per candidate direction (a straight also tries a gentle bend either way),
   classified against the terrain, and the cheapest wins. A coarse A*
   (`tg_road_guide_plan`, cell 6000, water 12 / steep 7 / hills ramped) from
   the head to +X gives the desired heading; a 48-span straight lookahead
   adds water/steep/structure cost. Sections whose tunnel or bridge would
   exceed `TD5RE_TG_TUNNEL_MAX` (32) / `TD5RE_TG_BRIDGE_MAX` (56) are rejected
   like an overlap; after 20 rejects the terrain yields (forced conform:
   causeway / deep cut, logged) so the walk never ends short.
   PROFILE: causal windowed (64 spans) forward/backward/forward grade limiter
   on the terrain height (water -> surface + 900), per-biome caps, spans
   finalise as the head passes so walk-time gates and the final road agree.
   STRUCTURES: road > 2200 under the ground = TUNNEL, > 2000 over it or any
   water = BRIDGE, else the world is conformed to the bed (verge 2500 +
   blend). Runs coalesced, min lengths (tunnel 6, viaduct 6, water crossing
   2), interlock 10, never in fork windows; a water crossing keeps its deck
   whatever the lane seams do. `tg_apply_elevation` lays chords, the
   raised-cosine deck clearance hump (bounded by the run's lowest cap),
   conforms open spans, paints the road into the raster and builds the
   per-span SHORE table (first wet cell + surface per side).
4. `tg_landmarks_place` (R21, unchanged).
5. `tg_fork_place` (moved out of tg_emit_strip) then **NETWORK**
   `tg_network_build`: bypass planning, corridors + gores painted and
   conformed, underpass crossings, city streets/avenues/continuations
   (facade-rhythm candidates marched on the raster: stop short of any
   carriageway, T-junction on another street, stop at water / tan-30 ground,
   dropped -> frontage stays BUILT), back streets closing blocks, forest
   lanes that may wander on and REJOIN the main road (a second mouth). The
   MOUTH TABLE feeds `tg_facade_built`, `tg_xstreet_here`,
   `tg_xstreet_reach_at`, `tg_block_arm_skew`, `tg_r12_fcross_at`.
   `NETWORK.JSON` written; shore table rebuilt; `tg_world_freeze`.
6. `tg_emit_strip` (uses the placed forks; `tg_fork_br_shift` returns the
   planned lateral for `TG_FORK_BYPASS`), routes, levelinf, files.
7. Scenery (unchanged three phases): the ground authorities sample the
   world -- `tg_ground_side_raw` (5 points to the verge reach; open spans
   keep the conformed bed flat), `tg_topo_chain` rings, `tg_emit_far_band`
   ring heights; water is `tg_emit_water` per span-side from the shore
   table plus the full-width plane under water bridges; facades and
   back rows stand on the world's ground under their frontage; back
   streets and loop tarmac come from `tg_net_emit_entry`.

## Knobs (all TD5RE_-prefixed, so they feed the GENSTAMP env hash)

`TD5RE_TG_WORLD_RELIEF` (% of 15000, roll 70..159), `TD5RE_TG_WORLD_SEA`
(-35..34, roll), `TD5RE_TG_WORLD_MOUNTAINS` / `_RIVERS` (0..100, roll),
`TD5RE_TG_WORLD_DUMP=1` (WORLD.PGM over the road box), `TD5RE_TG_STEER`
(default on), `TD5RE_TG_BRIDGE_MAX` / `TD5RE_TG_TUNNEL_MAX`,
`TD5RE_TG_NET_BACKSTREETS` / `_LOOPS` / `_BYPASS` (default on),
`TD5RE_TG_NET_BYPASS_PCT` (70), `TD5RE_TG_NET_LEFT=1` (opt-in, see
`AUTOTRACK_BRANCH_REWORK.md`). Retired with their code: R8 relief, R8
LONGRUN, R11 bridge coalesce, R17 global water, R21 p90 gain, the hash
bridge/tunnel gates, the R4..R18 shore/gorge skirt cases, the coastline
band, the memoised street ray march.

## Verification tools

- `verify/topo_gen.ps1 -Seed N -Tag T [-Extra @{...}] [-Port P]`: the
  byte-identity protocol (STREAM=0, REUSE=0, RaceTrace) + the [WORLD] /
  [STRUCT] / [NET] lines + sha256 of every level file. Never run two
  generators in one worktree at once (they share level090).
- `re/tools/tg_network_audit.py NETWORK.JSON` (exit 1 on violation):
  planarity, street-off-road, mouths off structures, structure lengths +
  interlock, grade, water-without-deck (forced conforms and cuttings are
  counted, not failed).
- `re/tools/tg_strip_audit.py STRIP.DAT` (unchanged lane/fork invariants).
- `verify/span_capture.py <out> <seed> auto auto`: targets from
  NETWORK.JSON (first span of every structure run, each drivable fork's
  mouth, the first three street mouths); `TD5RE_CAM_TOPDOWN=6000..9000` for
  junctions, chase cam for tunnels; the F12 wireframe via `[Debug]
  Collisions=1` shows the strip against the meshes.

## Measured (2026-09-08)

| seed | spans | bridges (longest) | tunnels (longest) | edges / junctions / loops / bypass | audits |
|---|---|---|---|---|---|
| 20260901 | 2069 | 4 (10) | 0 | 75 / 15 / 2 / 2 | OK |
| 5150 | 1784 | 3 (9) | 0 | 34 / 4 / 0 / 1 | OK |
| 777 | 2669 | 10 (55) + 2 causeways | 6 (18) | 56 / 4 / 0 / 0 | OK |

Determinism: three runs byte-identical per seed (STREAM 0/1 and the MT
terrain prepass included); selftest full 58/58 with the golden traces;
guard rejects 0 road/deck/tunnel meshes. Centerline 0.3 s -> ~2.8 s (A* per
section); network ~35 ms.

## Known gaps / next

- LEFT corridors: generated and mirrored end to end but the walker's
  sub-lane bookkeeping is not proven (opt-in) -- `AUTOTRACK_BRANCH_REWORK.md`.
- Scenery placement still checks the lateral reach authorities, not the
  raster: a prop can land on a back street or a loop's far half.
- Facades only line the main road; street edges get flank walls, not
  their own frontage.
- The far band samples the world at 2 x 4 points per 4-span group: coarse
  on rugged ground. A per-span band with more rings is the next visual win.
- Country roads are FOREST-only (the R12 rhythm); FIELDS lanes need pages.
