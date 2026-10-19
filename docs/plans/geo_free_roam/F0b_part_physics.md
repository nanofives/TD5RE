# Part: physics / assists / laneassist / chaos / light / vfx / rt / replay / net / sound / damage (agent a73130c7 report)

Abbrev: susp = td5_physics_suspension.c, phys = td5_physics.c, coll = td5_physics_collision.c, assist = td5_physics_assists.c, trk = td5_track.c.
No span/strip consumers at all in: td5_net.c (only `td5_track_registry_fingerprint_for_slot`, 796), td5_sound.c (only `surface_contact_flags`/`wheel_contact_bitmask`, 1776-1785), td5_damage.c, td5_chaos_fold.c, td5_light2.c, td5_material.c, td5_carbalance.c.

## How span = -1 behaves in the track layer (strip loaded)
| Function | Behaviour |
|---|---|
| `td5_track_get_span` (trk:11675) | bounded both sides, NULL |
| `td5_track_get_vertex` (trk:11680) | LOWER bound only |
| `update_position_recursive` | `compute_boundary_bits` returns 0 out of range (trk:5470), walker breaks, span is never self-healed |
| `compute_contact_height_*` | returns 0 (ground y=0) for invalid span (trk:7488, 7564, 7614) |
| `td5_track_probe_height` (trk:7506-7510) | CLAMPS to [0,count-1]: silently samples span 0 and returns 1 |
| `td5_track_get_surface_type` (trk:5396) | dry asphalt |
| `compute_probe_contact_vertices` (trk:6957), `compute_heading` (7908), `compute_runtime_heading_normal` (8134) | no-op (heading_normal goes stale) |
| `td5_track_get_recovery_pose` (trk:3461) | wraps -1 modulo ring or clamps to 0 |
With NO strip loaded every helper returns early: flat y=0, normal (0,4096,0), dry asphalt, heading_normal zero. No crash, no slopes/grip/hill assist.

## td5_physics_suspension.c
| file:function:line | uses | when | verdict | note |
|---|---|---|---|---|
| susp:refresh_wheel_contacts:3608-3619 | writes chassis span+sub_lane into 4 wheel + 4 body probes | per tick/actor | B (seed from locator edge id or -1) | |
| :3723, 4072 | `td5_track_update_probe_position` | per wheel per tick | C | walker returns early on invalid span |
| :3730-3732, 4075-4077 | `span_index >= max_sp -> max_sp-1` clamp | per wheel per tick | A | -1 safe; synthetic id >= span_count is silently rewritten to count-1 |
| :3764, 4078 | `compute_probe_contact_vertices` | per wheel per tick | A (-1 no-op) | nothing reads the result |
| :3781-3785 | probe_span fallback probe -> chassis -> 0 | per wheel per tick | B | -1 silently reads span-0 geometry (wrong ground, not a crash) |
| :3815 | `contact_height_bounded(...)` ground_y + normal | per wheel per tick | **B: height + normal** | returns 0 for invalid span |
| :3828, 4121 | `td5_track_get_surface_type(actor, 4+i)` -> `surface_type_chassis` | per tick (first wheel) | **B: surface type** | |
| :3839 | `td5_track_last_contact_was_capped()` | per wheel per tick | B (locator reports cap or returns 0) | |
| :3866-3868 | writes normal to `wheel_contact_velocities` | per wheel per tick | B: normal | |
| :4085-4116 | body-corner `contact_height_with_normal` -> `probe_FL..RR.y` (+TD6 re-probe 4103) | per tick x4 | **B: height per body corner** | guarded `probe_span>=0 && <max_sp` |
| :3555-3583 | LATLEAK diag, `probe_height` x5 | slot 0, every 30 ticks | C | probe_height clamps to span 0: logs garbage |
| integrate_pose:2396 | `td5_track_update_actor_position` | per tick | C | |
| :2409-2423 | clamp `track_span_raw >= (uint16_t)max_span`; wheel probe reset if `|probe-chassis|>50` | per tick | A for -1 | corrupts synthetic id >= count |
| :2435, 3261, 2090 | `compute_runtime_heading_normal` | per tick | **B: normal** | no-op on invalid span => stale |
| :3138 | `probe_height` (CLIP telemetry) | per tick slot 0 | C | |
| update_vehicle_pose_from_physics:3245, 3261, 3266 | chassis pos, heading normal, refresh_wheel_contacts | per tick + per wall/V2V push | B | |
| integrate_traffic_pose:2079-2102 | traffic Y from `contact_height_with_normal(track_span_raw, ...)`, roll/pitch from normal | per tick/traffic car | C (no traffic) else B | invalid span => traffic snaps to y=0 |
| :2227-2252 | body-corner probes | per tick/traffic car | C | |
| process_traffic_segment_edge:1665-1833 | `get_span(track_span_raw)` (NULL => done), sp->origin, vertices | per tick/traffic car | C (D if traffic re-routed) | span_type LUT guarded 0..11 |
| process_traffic_route_advance:1877-1888 | fires only at `span == total-1` | per tick | C | |
| process_traffic_forward_checkpoint_pass:1964-1969 | `span == fwd_sentinel+1` | per tick | C | |
| traffic_route_heading_delta:1098-1118 | `+0x82`, `route_table[span_norm*3+1]` after `td5_ai_route_span_valid` | per tick traffic | D / C | guarded; :1098 `td5_track_get_span(0) - 0x80` is unused pointer arithmetic (UB if NULL, benign) |
| apply_simple_track_surface_force:1176 | pure edge angle/pen math | on event | A (reusable for locator wall push) | |

## td5_physics.c
| file:function:line | uses | when | verdict | note |
|---|---|---|---|---|
| phys:player_phase_grip:1837 | `td5_track_get_surface_type(actor, 0..3)` on body probes -> grip tables | per wheel per tick, player | **B: surface type per wheel** | masked `&0x1F`; asphalt if invalid |
| :1834, 3387 | `surface_type_chassis` | per tick | B | |
| :1840, 3390; drivetrain:1291 | `compute_surface_gravity` reads `probe_FL..RR` | per tick | **B: height per body corner** | no div by zero |
| :2215, 3057-3066 | `heading_normal` hill assist, slope decel | per tick player | **B: normal** | zero normal degrades silently |
| :4218, 4236 | `sum/cnt * heading_normal.y` | per tick | B: normal.y | cnt>=1 |
| :5586-5605, 5957-5964 | `heading_normal` in collision-alignment + tumble | per tick, recovery | B: normal | alignment check never passes with (0,0,0): recovery exit stuck |
| :1709-1712 | `td5_track_resolve_reverse/forward/wall_contacts` | per tick/racer | **B: inside + push + lateral offset** (fwd/rev sentinels C) | |
| :4117-4128 | reverse-direction only: writes span+lane into the 8 probes | spawn/reset | B | |
| :4132 | `reset_actor_state` -> `integrate_pose` ground-snap from `world_pos.y = 0xC0000000` | on event | B: height | |
| :5655-5790 | `td5_physics_render_vehicle_actor_model` (mode 1) 8 probes | per tick mode 1 | B: height per probe | `ComputeActorTrackContactNormal` (trk:12052) guards |
| :6136, 6190 | `update_actor_position` in scripted motion | per tick mode 1 | C | |
| :858-873 | catch-up passes (HWM) | per tick | D | see assist |
| :1350, 3855 | `td5_physics_update_traffic` | per tick | A | |

## td5_physics_collision.c
| line | uses | when | verdict | note |
|---|---|---|---|---|
| coll:3634-3640 | V2V broadphase bucket `(track_span_normalized clamped >=0) >>2 & 255` into `s_collision_grid[256]` (uint8) | per tick/actor | **B: world-XZ spatial hash** (or A if synthetic ids stay spatially coherent) | -1 clamped to 0, mask => no OOB. All roam cars land in bucket 0; ids >= 1024 alias buckets (correctness, not OOB) |
| :3661-3668 | walks buckets -1, 0, +1 | per tick | B | `COLLISION_MAX_WALK` bounds chain |
| :3759-3844 (anti-tunnel), 3855-3913 (player vs traffic) | world-XZ O(n^2) pair tests | per tick | A | already span-free |
| :1663-1678 `traffic_clamp_above_ground` (2535, 2670) | `contact_height_with_normal(track_span_raw ...)` | on event | **B: height** | returns 0 => no-op |
| :179-500 `td5_physics_wall_response` | (angle, pen, side, probe x/z); no track reads | on event | A: wall-push sink, reusable | |

## td5_physics_assists.c
| line | uses | when | verdict | note |
|---|---|---|---|---|
| assist:405-418, 487-500, 1865-1889 | `track_span_high_water` gaps (AI/MP/hard catch-up) | per tick | **D** | arithmetic only |
| :2233-2278 recover_player | default: in place (y sentinel + compute_heading + reset); breakdown: `td5_track_get_recovery_pose` | on event | **B** position, A default branch | breakdown with span -1: pose -> span 0 or returns 0 and recovery aborts (:2244) leaving a knocked-out car frozen |
| :2392-2443 battle_antireverse_tick | ring, hwm, progress, writes span fields, `td5_track_init_actor_segment_placement` | per tick, battle humans | **C** | `hwm % ring` guarded; invalid span => `init_actor_segment_placement` ZEROES `world_pos` (trk:3609-3613) |
| :2611-2631 wedge | `track_span_normalized` best, gated on generated slot | per tick AI | D or C | |
| :2764 gentle_recovery | `update_actor_position` | per tick mode 1 | C | |

## td5_laneassist.c
| line | uses | verdict | note |
|---|---|---|---|
| 296 | `track_sub_lane_index` | B (lane/lateral) or C | |
| 372 | `td5_track_laneassist_target(track_span_raw, lane, lookahead, ...)` | **C** (or B with route look-ahead) | guarded (trk:3936) => "no_target", inert |
| 429 | `get_span_lane_world`, drag only | C | guarded (trk:3394) |
| 326 | `laneassist_sweep_diag` env | C | |
| 366, 380, 547 | log only | A | |

## chaos / light / vfx / rt / replay / net / sound / damage
| file:function:line | uses | when | verdict | note |
|---|---|---|---|---|
| td5_chaos.c:237-243 | ring, `branch_to_main_span(get_slot_span)`, `lap*k + span*k/ring` | per tick, circuit only | **D** (TIME trigger already span-free) | guarded ring<=0, span<0, >=ring |
| td5_light.c:667-668 | `probe_height(L.x*256, L.z*256, track_span_raw)` tunnel-lamp floor | first resolve | **B: height at lamp XZ** or C (gate tunnel lamps) | probe_height clamps -1 to 0 |
| td5_vfx.c:1351-1396 update_ambient_density | `+0x80` matched against LEVELINF (seg_id, density) pairs | per tick/view | **C** (or B by world position) | -1 never matches: target never updates; pair count capped |
| td5_vfx.c:2725, 2806, 2849, 2953 | `surface_type_chassis`, `vfx_is_hard_surface` | per tick/car | **B: surface type** | |
| td5_vfx.c:1823-1829 | `wheel_contact_pos` tire-mark anchor | per tick | A | |
| td5_rt.c:341-420 td5_rt_level_build | loops `s in [0,span_count)` lane counts + lane quads -> road BLAS | init | **C/B** (feed tile mesh) | returns early at span_count<=0: NO RT road |
| td5_rt.c:819-840 rt_view_window | centre = `track_span_normalized` (clamped only if ring>0), `>>2` | per frame/view | **B** (centre from locator/camera XZ) | downstream `c<0||c>=n` / `c%=n` bound s_entries |
| td5_rt.c:856-857, 625-626 | models display-list count, ring, blob ptr test | per frame | A/B | |
| td5_replay.c:49-55, 189-195, 274-280 | copies track_span_* , contact_vertex_A/B, sub_lane raw | per tick record / per frame play | **A** | records synthetic ids verbatim; trackside replay cam + race order read them back |
| td5_net.c:796 | `td5_track_registry_fingerprint_for_slot` | per race | A | lockstep carries no span data; roam physics must be deterministic across peers |
| td5_sound.c:1776-1785 | `surface_contact_flags`, `wheel_contact_bitmask` | per tick | A | |
| td5_damage.c | none | | A | fed by wall_response |

## Span-window/ring/wrap arithmetic
coll:3636,3663 `(seg>>2)&255` B; rt:827-837 `span +- fwd/back`, `>>2`, reverse mirror B; susp:2419-2422 `|probe-chassis|>50` A for -1, meaningless for edge ids; susp:1882/1969 C; chaos:243 D; assist:2429 `hwm % ring` C; assist:405-498, 1865-1883 D; trk:3499-3503 recovery pose wrap C.

## Crash-risk (this part)
No hard OOB read/write found for span=-1 with or without a strip. Items:
1. `susp:2412-2423` `track_span_raw >= (uint16_t)max_span` (int16 promotes signed: -1 safe). A synthetic id >= `td5_track_get_span_count()` is OVERWRITTEN with `max_span-1` (guard `max_span>0`).
2. `susp:3730-3732, 4075-4077, 2242-2243; phys:5729-5730, 5778-5779` same corrupting clamp for ids >= count.
3. `susp:3781-3785` -1 falls back to chassis span then 0: reads span-0 geometry.
4. `td5_track_get_vertex` (trk:11680-11684): lower bound only (`&s_vertex_table[index]`). Callers susp:1703-1704, 1783-1784, 1917-1918, 2011-2012 pass indices from a valid span.
5. `vertex_at` (trk:1172) unbounded; used in `probe_span_lane_height`, `triangle_height`, wall resolvers, `diagonal_cross`; callers guard span.
6. `trk:6571` `&s_span_array[new_span_back]` unguarded, from `resolve_neighbor`; unreachable with -1 (walker exits with bits==0).
7. `susp:1098` `td5_track_get_span(0) - 0x80` on a possibly-NULL pointer, unused result.
8. `susp:1107-1118` `route_table[span_norm*3+1]` guarded (clamp + `td5_ai_route_span_valid`).
9. `coll:3636, 3663, 3457` bucket index masked: safe.
10. `td5_rt.c:822-840` negative span with ring<=0 unclamped but bounded downstream.
11. int16 width: a synthetic edge id above 32767 wraps negative and reads as invalid everywhere (also in td5_replay.c:49-55).
12. Side effects without crash: `td5_track_init_actor_segment_placement` zeroes `world_pos` for an out-of-range span (trk:3609-3613; assist:2435 battle-only); traffic/`traffic_clamp_above_ground` use height 0; `td5_physics_recover_player` breakdown path aborts without moving the car when the pose fails.
