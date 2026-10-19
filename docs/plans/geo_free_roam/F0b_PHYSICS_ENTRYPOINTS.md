# F0b - physics contact data and roam entry points (author: claude-sonnet-5-5, verified by reading code 2026-10-15)

All line numbers are `td5mod/src/td5re/` at master 29f89b22 (working tree).

## 1. What physics actually consumes per wheel (all produced inside ONE function)

`td5_physics_refresh_wheel_contacts` (`td5_physics_suspension.c:3540`, port of 0x00403720) is the single producer.
Per tick, per racer (slots `< g_traffic_slot_base`), it loops the 4 wheels and writes:

| Datum | Field written | Source today | Line |
|---|---|---|---|
| Ground height under wheel (24.8, world Y) | local `ground_y`, then `wheel_contact_pos[i].y` (snapped when grounded), `wheel_load_accum[i]` = `(wheel_y - ground_y) + g_gravity_constant` | `td5_track_compute_contact_height_bounded(span, sub_lane, x, z, normal)` | 3815, 3895, 3963, 3970 |
| Ground normal (int16[3], magnitude 4096, world space) | `wheel_contact_velocities[i][0..2]` (+0x250), normalised by the caller | out_normal of the same call | 3813-3869 |
| "Capped extrapolation" flag (forces the wheel airborne) | `wheel_capped` | `td5_track_last_contact_was_capped()` | 3839, 3955 |
| Surface type (per lead wheel only) | `actor->surface_type_chassis` | `td5_track_get_surface_type(actor, 4+i)` (reads `wheel_probes[i].span_index` + `sub_lane_index`) | 3828, 4121 |
| Airborne mask | `wheel_contact_bitmask` (+0x37C), `damage_lockout` (+0x37D = previous mask) | derived: `force > 0x800 || wheel_capped` | 3592, 3955-3959 |
| Wheel probe state (span/lane) | `wheel_probes[i].{span_index,sub_lane_index,contact_vertex_a/b,...}` | `td5_track_update_probe_position` (walker) + `td5_track_compute_probe_contact_vertices` | 3608, 3723, 3764 |

Then a second loop over the **4 body corners** (3045-4118): the corner's ground Y is written into `probe_FL/FR/RL/RR.y`
(`td5_track_compute_contact_height_with_normal(probe_span, probe_lane, x, z, NULL)`, 4088; normal discarded) and the corner
walker is run on `body_probes[i]`. Those four corner Ys feed `td5_physics_compute_surface_gravity`
(`td5_physics_drivetrain.c:1291`: diagonal differences of `probe_*`, i.e. downhill gravity push on `linear_velocity_x/z`), so
**a second, 4-point ground height set is consumed** (not only the 4 wheels).

Per-wheel data NOT produced by a contact resolver: `wheel_contact_normals` (+0x230) is the body-rotated wheel vector, no ground data.

Other per-tick surface data:

| Datum | Consumer | Source |
|---|---|---|
| Per-wheel surface type (5 samples: chassis cache + `body_probes[0..3]`) -> grip tables | `td5_physics_player_phase_grip` `td5_physics.c:1837` | `td5_track_get_surface_type(actor, 0..3)` (reads `body_probes[i].span_index`, `sub_lane_index`) |
| Chassis surface normal `heading_normal` (+0x290, int16[3]) | slope force `td5_physics.c:3057-3066`, missing-wheel correction `td5_physics.c:4236` (multiplies by `hn.y`), recovery/tumble tests `td5_physics.c:5586, 5957`, traffic | `td5_track_compute_runtime_heading_normal` (`td5_track.c:8105`), called from `td5_physics_suspension.c:2090, 2435, 3261`. **It silently returns without writing when `!s_span_array` or span out of range (8121, 8134, 8195): stale/zero normal stays in the actor.** |
| Wall push | `td5_physics_wall_response(actor, wall_angle12, penetration(<0), side, probe_x_fp8, probe_z_fp8)` `td5_physics_collision.c:179` | rail-LUT resolvers (`td5_track_resolve_wall_contacts`, `td5_track.c:2502`), or `geo_resolve_walls` (`td5_track.c:2314`) |

So the full list the roam provider must supply per tick for a racer:
(1) 4 wheel ground heights + (2) 4 wheel normals + (3) capped flag (or just return "no cap") + (4) one surface type per wheel
(plus a chassis value) + (5) 4 body-corner ground heights + (6) one chassis normal (`heading_normal`) + (7) wall push as
(wall_angle, penetration, probe x/z) per offending body corner.

## 2. Tick order (where a provider hooks)

Racer, normal vehicle mode, per sim tick (confirmed by reading):
`td5_physics_integrate_pose` (`td5_physics_suspension.c:2357`) -> integrate velocity into `world_pos` (2387-2389) ->
`td5_track_update_actor_position(actor)` (2396; the chassis walker, writes `track_span_*`) -> span clamp block (2409-2424, no-op
when `td5_track_get_span_count()==0`) -> `td5_track_compute_runtime_heading_normal` (2435) -> ... -> `update_vehicle_pose_from_physics`
(3172; also calls the walker at 3245 and heading normal at 3261) -> `td5_physics_refresh_wheel_contacts` (3540; wheel + corner probes,
heights, normals, surface) -> suspension response (`td5_physics_update_suspension_response`, 741; reads `wheel_contact_velocities`
for gravity projection and landing spring) -> then, in the vehicle update tail (`td5_physics.c:1709-1712`, `slot < g_traffic_slot_base`):
`td5_track_resolve_reverse_contacts`, `td5_track_resolve_forward_contacts`, `td5_track_resolve_wall_contacts`
-> `td5_physics_wall_response` + `td5_physics_rebuild_pose`.
Scripted/recovery mode (`vehicle_mode==1`) uses `td5_physics_render_vehicle_actor_model` (`td5_physics.c:5649`) instead of refresh_wheel_contacts
(walker at 5728/5777, `ComputeActorTrackContactNormal` for the heights) and `td5_physics_scripted_contact_refresh` (6185).

## 3. Minimal roam contact-provider entry points

Every strip access in the contact path funnels through ~9 functions in `td5_track.c`. All of them early-out
silently when no strip is loaded (`!s_span_array`) and return `0` / `TD5_SURFACE_DRY_ASPHALT` / no write:
a ROAM mode with no strip therefore does NOT crash in physics but the car sits at ground Y = 0 with a dead wall resolver and stale
`heading_normal`. The provider must replace exactly these:

| # | Function (td5_track.c) | Callers (physics side) | Roam replacement (locator field) |
|---|---|---|---|
| 1 | `td5_track_update_actor_position` (6745) | `td5_physics_suspension.c:2079, 2396, 3245`; `td5_physics.c:6136, 6190`; `td5_physics_assists.c:2764` | seed/refresh the locator hint (edge id, optionally into `track_span_raw` as -1/edge id); no walker |
| 2 | `td5_track_update_probe_position` (6910) | `td5_physics_suspension.c:2240, 3723, 4072`; `td5_physics.c:5728, 5777`; camera 1366/1688-1694; shadow `td5_track.c:7770` | per-probe locate: edge id + triangle |
| 3 | `td5_track_compute_probe_contact_vertices` (6951) | suspension 3764/4078, physics 5731/5780 | none (vertex ids are span data; leave unwritten, nothing reads them in roam) |
| 4 | `td5_track_compute_contact_height_bounded` (7606), `_with_normal`, `_bestlane`, `td5_track_probe_height` | suspension 3815, 4088/4107; physics 5739 via `ComputeActorTrackContactNormal`; render shadow 7869; HUD/camera | locator **height** + **normal** at (x, z, y-hint) |
| 5 | `td5_track_last_contact_was_capped` | suspension 3839 | return 0 (or "locator found no triangle within tolerance") |
| 6 | `td5_track_get_surface_type` (5376) | `td5_physics.c:1837`, suspension 3828, `td5_ai.c:2087` | locator **surface type** (asphalt / kerb / grass / cobble) |
| 7 | `td5_track_compute_runtime_heading_normal` (8105) | suspension 2090, 2435, 3261 | locator **normal** at chassis XZ (must write `heading_normal`, y never 0) |
| 8 | `td5_track_resolve_wall_contacts` (2502) + `_reverse_/_forward_contacts` | `td5_physics.c:1710-1712` | locator **inside flag + push vector** -> `td5_physics_wall_response` (same call as `geo_resolve_walls`, `td5_track.c:2356`) |
| 9 | `td5_track_get_span_count` (max_sp clamps) | suspension 2409, 3730-3785, 4075; physics 5675 | return 0 in roam so every clamp is a no-op (already safe: all guards are `max_sp > 0 &&`) |

Notes on the existing seed:
- `geo_resolve_walls` is the PRIMARY wall handler only when `geo_active()` = `geo_is_enabled() && g_td5.reverse_direction && g_active_td6_level == 0`
  (`td5_track.c:1761`). Forward custom tracks run the rail-LUT path and only call `geo_rescue_offmap` (2446) as a backstop
  (`geo_query_active`, 1768). `geo_locate` (2058) returns `{span, lane, inside, pushx, pushz, pushdist}` (struct `GeoHit`, 2044): it has
  NO height and NO normal (heights come from the strip triangles), so the roam locator needs those two outputs added; it scans
  +/-8 grid cells (about 32k world units) and brute-forces only in `geo_nearest_global`.
- `geo_resolve_walls` takes the push from the four BODY corners `probe_FL/FR/RL/RR` (`td5_track.c:2316`), keeps only the lateral
  component relative to heading (2337-2347), caps the push (`GEO_PUSH_CAP`) and calls `td5_physics_wall_response` +
  `td5_physics_rebuild_pose` per corner. The roam provider can reuse this body verbatim after swapping `geo_locate` for the roam locator.
- Spawn/reset: actors are placed with `world_pos.y = 0xC0000000` (ground-snap sentinel; `td5_game.c:4728`,
  `td5_physics.c:4068`, `td5_physics_assists.c:2258`) and the first `integrate_pose` snaps Y from the wheel ground height.
  Roam spawn must either set a real Y from the locator or rely on provider #4 being live at that first tick.
