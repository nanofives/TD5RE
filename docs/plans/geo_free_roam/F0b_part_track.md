# Part: td5_track.c / td5_track.h API and internal resolvers (agent ac5dc14e report; spot-verified: 3215-3257, 5459-5521, 4955-4984, 2058-2159, 2314-2359)

T = td5_track.c, S = td5_physics_suspension.c, P = td5_physics.c, C = td5_physics_collision.c, A = td5_physics_assists.c. Paths under `td5mod/src/td5re/`.
Facts: `vertex_at()` (T:1172) is `&s_vertex_table[index]`, no bounds, never NULL (every `if (!v)` on it is dead code). `track_span_raw` is `int16_t`, `track_sub_lane_index` is `uint8_t`. `geo_locate` already is a nearest-quad locator but returns only `{span, lane, inside, push}`; its quad soup is built only from the STRIP. T `codemap` rows are stale; `geo_locate` 2058 and `geo_resolve_walls` 2314 match.

## 1a. Contact and position functions
| Function (T line) | Reads | Span -1 / OOR | Guard | External callers |
|---|---|---|---|---|
| `td5_track_update_actor_position` (6745) | strip spans/vertices, sublane/median tables, jump table | walker no-op; state stays -1 | `!actor \|\| !s_span_array \|\| s_span_count==0` return, then `compute_boundary_bits` 0 for OOR | 6: S:2079, 2396, 3245; A:2764; P:6136, 6190 |
| `td5_track_update_probe_position` (6910) | same walker, single_step=1 | no-op | 6913 | 9: S:2240, 3723, 4072; P:5728, 5777; camera.c:1366, 1688, 1691, 1694 |
| `td5_track_compute_probe_contact_vertices` (6951) | span type, `k_quad_vertex_offsets` | early return, leaves vertex A/B stale | 6957 | 5: S:2244, 3764, 4078; P:5731, 5780 |
| `td5_track_get_surface_type` (5376) | `probe->span_index`, lane, `surface_attribute`, lane bitmask, TD6 grid | `TD5_SURFACE_DRY_ASPHALT` | 5396 | 2: P:1837 (x4 body probes), S:3828 |
| `td5_track_compute_contact_height_with_normal` (7482) | span, `probe_span_lane_height`, `triangle_height` | returns 0, does NOT write `out_normal` | 7488 | 5: S:2101, 2249, 4088, 4107; C:1671; T:7756, 7775, 12056 |
| `..._compute_contact_height_bounded` (7606) | span, carried lane +-1 | 0, normal untouched | 7614 | S:3815; T:7869 |
| `..._compute_contact_height_bestlane` (7557) | all lanes | 0 | 7564 | camera.c:1379 |
| `td5_track_last_contact_was_capped` (7059) | static flag | n/a | n/a | S:3839, 4110 |
| `td5_track_probe_height` (7495) | span, all lanes, surface | **clamps span to [0,count-1]: -1 reads span 0** | 7506-7509 | 7: S:3138, 3565-3569 (x5); td5_light.c:667 |
| `td5_track_shadow_probe_height` (7726) | span clamp, chassis plane | clamps -1 to 0 (7737) | 7737-7738 | td5_render_effects.c:835 |
| `td5_track_compute_runtime_heading_normal` (8105) | span, sub_lane, triangle; writes `actor+0x88/0x8A`, `heading_normal` | silent return, stale `heading_normal` | 8134 | 3: S:2090, 2435, 3261 |
| `td5_track_compute_heading` (7881) | span vertices, type | silent return, no heading | 7908 | ~12: A:2266, 2437; ai.c:1421, 8495; ai_traffic.c:612, 683, 1516, 1543, 3756, 5668; game.c:4741 |
| `td5_track_init_actor_segment_placement` (3582) | span, lane, 4 vertices | writes `span_accum = span_high = -1` and ZEROES `out_pos` (caller then writes world_pos = 0,0,0) | 3611 | 6: A:2435; ai_traffic.c:600, 676, 988, 1134, 3753 |
| `td5_track_get_recovery_pose` (3461) | ring, jump table | **wraps -1 modulo ring and returns a real ring span: car teleported onto the strip** | `ring>1` branches 3482-3507 | A:2240 |
| `ComputeActorTrackContactNormal` / `UpdateActorTrackPosition` wrappers (11974 / 11940) | thin | `*out_y = 0` / clamp span | 12052 / 11963 | camera.c:1689, 1692, 1695; P:5739, 5784 |
| `td5_track_normalize_actor_wrap` (8362) | `s_strip_header[1]` or ring, `+0x84`, jump table | uses accumulator not span; 0 if ring 0 | 8388 `ring_length==0` | 9: game.c:7700, 7896; ai_traffic.c:630, 704, 1073, 1251, 1521, 1548, 3761 |
| `td5_track_resolve_actor_segment_boundary` (8493) | jump table, `s_cmap` | -1 <= ring so copies -1 into +0x82/+0x84 | `entry_count<=0 \|\| !entry_base` | ai_traffic.c:338, 697, 3797 |
| `td5_track_resolve_wall_contacts` (2502) | span rails, fork divider, side-link/median tables, geo | silent return; **`geo_active()` (2519) is tested BEFORE the span guard (2535)** | 2507, 2511, 2514, 2535 | P:1712 |
| `td5_track_resolve_forward_contacts` / `_reverse_` (3293 / 3298) | boundary-sentinel span vertices | **forward handler does NOT exit for span -1** (3232: `chassis_span > boundary+1` is false for -1; verified) | `boundary<0 \|\| >=s_span_count` (3225) | P:1711 / P:1710 |

## 1b. Span / geometry getters
| Function | Reads | Span -1 | Guard | Callers |
|---|---|---|---|---|
| `td5_track_get_span_count` (10057) / `get_ring_length` (10062) | `s_span_count` / `g_td5.track_span_ring_length` | 0 when no strip (ring is STALE, see game part finding 2) | none | ~44 / ~51 (ai, ai_traffic, S, camera, hud, render*, game, chaos, ai_driver) |
| `get_span` (11673) / `get_vertex` (11680) | arrays | NULL / **no upper bound on vertex** | `index<0 \|\| >=s_span_count` / `index<0` only | ~15 in S, ~8 camera, ~14 render, game.c:4602-4605, 9803-9807, ai.c:2092 |
| `get_span_center_world` (3345), `get_span_lane_world` (3382) | 4 quad vertices | 0 | 3357 / 3394 (`!src[x]` dead) | camera.c:2730, 2771; render.c:1102-1122, 3810; render_mesh.c:2180, 2186; ai.c:8988; ai_traffic.c:2199, 6269, 6311; game.c:4712; laneassist.c:429 |
| `get_span_edges` (1387) | edges | zeros | 1395 | td5_minimap_streets.c:281 |
| `get_span_lane_count` (11536) / `span_lane_count_at` (3679) | lane nibble | 1 / 0 | 11538 / 3681 | ~30 / 13 |
| `get_span_lane_surface` (4381), `surface_is_slow` (4411) | `surface_type_for_span_lane` | DRY_ASPHALT | 4386 | 9 in ai.c, ai_traffic.c |
| `td5_track_td6_road_band` (4437) | lane surface classes | n/a | none | game.c:4624, 4642; ai_traffic.c:3895, 3946 |
| `td5_track_get_lane_quad_world` (4310) | 4 vertices | 0 | 4316 | td5_rt.c:361, 410, 416 |
| `get_primary_route_heading` (12084) | span vertices | 0 | 12092 | ai.c (7), ai_driver.c:523-524 |
| `get_span_route_frame` (8853) | vertices | 0 | 8865 | ai.c (9), ai_driver.c:342, 676, game.c:5863 |
| display-list / scenery / models / banner / lamp functions (11782...) | MODELS.DAT blob | NULL | `span_index>=0` etc. | render*, rt, trackgen*, camera.c:2789-2906. No physics use |

## 1c. Route, junction, loop and traffic tables (AI/traffic only)
`sample_target_point` (8730), `compute_span_progress` (8601), `compute_signed_offset` (8668), `compute_spline_position` (8967): vertices, 0 for OOR, guards 8741/8609/8676, ~40 sites in ai.c. `apply_target_span_remap` (8892): `lin += count` if negative so -1 wraps to count-1. `load_routes` (9053): stores LEFT/RIGHT.TRK (3 B/span) and hands to `td5_ai_set_route_tables`, no row-count API. `branch_to_main_span` (10945) returns span unchanged for <0 or ring 0; `main_to_branch_span` (10994) -1; `branch_to_junction` (11544) returns -1 unchanged; `route_junction_path2a_match` (11596) 0 for <0; `corridor_count/info` guarded; `traffic_next_span` (5660), `traffic_fork_ahead` (5601), `span_lane_base` (5646), `traffic_taper_lane` (5559), `nearest_road_lane` (10976) all `<0 || >=s_span_count`; `td5_track_loop_*` (~14 functions 10226-10375): linear scan, no index; `fork_is_left` (static 257): `s_fork_left[span]`, internal bound not confirmed; `td5_track_laneassist_target` (3907) guard 3742; `grid_main_lane` (3696) returns lane. Verdict A for the pure lookups (return -1/input/0 with no table), C/D for the AI users.
`td5_track_parser.c` (MODELS.DAT->meshes) and `td5_track_registry.c/.h` touch no contact data. `td5_track_bind_boundary_sentinels` (T:2920) sets `s_track_custom_registry` from `td5_track_registry_has_level` and decides whether `geo_query_active()` is armed: **a roam level must not register as a custom level or the forward-custom sentinel/rescue logic arms.**

## 2. Internal resolvers
- Walker: `update_position_recursive` (T:6173) works on `int16 track_state[8]` ([0] span, [1] norm, [2] accum, [3] high_water, byte 12 sub_lane); loop capped at `TRACK_MAX_RECURSION` = 8, rolls back on non-convergence (6693). `compute_boundary_bits` (T:5459): `span<0 || >=count` returns 0 so span -1 means "inside" and the walker never moves. `resolve_neighbor` (T:5719) clamps new_span to [0,count-1] and lane (6060-6068); `&s_span_array[span_idx]` at 5724 unguarded but reached only when bits != 0. Per tick a racer runs 1 chassis walk + 8 probe walks; every wall hit adds a chassis walk (through `update_vehicle_pose_from_physics`).
- Height/normal: `probe_span_lane_height` (T:7374): span record, lane clamped to [0,max_lane], world x/z 24.8 -> height `FP_SCALE(origin_y + h)`, optional int16[3] normal magnitude 4096 (y never 0), sets `s_last_contact_capped`. `triangle_height` (7060): upper cap = top vertex + Y-span + 1024 (sets capped flag), lower side open. `diagonal_cross` (7009).
- Wall/edge order: `td5_physics_update_vehicle_actor` (P:1244): reverse (P:1710), forward (P:1711), lateral (P:1712); gate `slot_index < g_traffic_slot_base`, traffic skipped (T:2511). Legacy lateral (T:2502-2809) builds rail normals from the span's vertices, penetration = dot against rail normal, reject at -2000 (T:2603), median-nose / side-link / gap exemptions before the push; hit -> `td5_physics_wall_response(actor, wall_angle12, pen<0, side{1,2,-1}, probe_x_fp8, probe_z_fp8)` (C:179) then `td5_physics_rebuild_pose` (P:5283). The actor is written by `td5_physics_wall_response`: `world_pos.x/z`, `linear_velocity_x/z`, `angular_velocity_yaw`, `track_contact_flag` (C:222-227, 415-436). `rebuild_pose` calls `update_vehicle_pose_from_physics` (S:3172) which re-runs the chassis walk, heading normal and `refresh_wheel_contacts`.
- `geo_resolve_walls` (T:2314): inputs actor, 4 body-corner probes `probe_FL..RR`, hint = `track_span_raw`. Output per probe `GeoHit{span,lane,inside,pushx,pushz,pushdist}` from `geo_locate`; keeps only the lateral component (perpendicular to car forward, discarded under 0.5, T:2337-2347); cap `GEO_PUSH_CAP` = 600 (T:2354); `wall_response(angle = atan2(dx,-dz), pen = -push, side 1)`. Active only when `geo_active()` = env on AND reverse AND not TD6 (T:1761).
- `geo_rescue_offmap` (T:2446): forward-custom backstop, pulls back when more than `OOB_RESCUE_DIST` = 1200 clear of every quad, cap 4000; `geo_nearest_global` (O(quads) brute force) when off-grid.
- Forward/reverse contacts (`fwd_rev_handler` T:3215): push from the boundary-sentinel span's edge, reject at -2000.
- `geo_locate` (T:2058)/`geo_build` (T:1876): signature `(px, pz, py, has_y, hint_span, GeoHit*)`, integer world units (not 24.8). `GeoQuad{x[4],z[4],cx,cz,cy,minx..maxz,span:int16,lane:int16}` (T:1567), `cy` = deck centroid Y used only for xspan ranking. Built one quad per lane from `s_span_array`+`s_vertex_table`, bowties re-sorted CCW (T:1955). Grid CSR `s_geo_cell_start[ncell+1]`, `s_geo_cell_items[]`, cell = mean AABB (w+d) min 64, doubled until `gnx*gnz <= 500000` (T:1994). Query: cells via `geo_cell_of` (clamps off-grid to the border cell), radius 1..8 rescanned in full each radius, hit = AABB + `geo_point_in_quad` (convex, edge-inclusive), every non-containing quad in scanned cells also runs `geo_quad_nearest`; stop at first inside hit or at radius>=2 once a nearest edge exists. Ranking: default smallest `|s_ring_pos[q->span] - hint_pos|` (ring-wrapped lap position); with `xspan_enabled()` and a valid hint: key0 in-window (`|gap| <= 24`), key1 `|cy-py|` when has_y, key2 span gap. Returns 0 if `!s_geo_quads || count<=0 || !s_geo_cell_start`. NOT provided: height, normal, surface type, quad index (it is `qi` internally), lateral offset. Cost: `geo_resolve_walls` 4 calls/racer/tick (reverse native only); `geo_rescue_offmap` 1/racer/tick forward custom plus O(N) fallback. Built only when `geo_active() || geo_query_active()` (T:2934-2935, 5175), both require `!g_active_td6_level`; with no STRIP `geo_build` returns early (T:1886) and `geo_locate` returns 0.
- Surface: `surface_type_for_span_lane(sp, lane)` (T:4351): `lane<0 -> 0`; if bit `lane` of `sp->pad_02[0]` set returns `(surface_attribute>>4)|0x10` else `surface_attribute & 0x0F`; `td5_track_get_surface_type` wraps with TD6 grid override and DRY_ASPHALT for OOR span (5396).

## 3. Verdicts (agent) for ROAM
| Function | Verdict | Do |
|---|---|---|
| `td5_track_update_actor_position` (6745) | A if no strip loaded (returns at 6750); C if a strip is loaded and synthetic id in `[0,s_span_count)` (walker would move it) | provider writes `track_span_raw` itself |
| `td5_track_update_probe_position` (6910) | same | probes are re-seeded from `track_span_raw` every tick (S:3607-3619) so -1 flows through |
| `compute_probe_contact_vertices` (6951) | A | none |
| `compute_contact_height_bounded` / `_with_normal` / `_bestlane` | **B** | `loc.height_fp` + `loc.normal[3]` (int16, mag 4096, y!=0); current -1 path returns 0 and the caller then re-reads span 0 |
| `td5_track_get_surface_type` (5376) | **B** | `loc.surface_type` (0..0x0F or 0x10|alt) |
| `compute_runtime_heading_normal` (8105) | **B** | write `heading_normal` from locator normal (stale y=0 neuters `td5_physics_missing_wheel_correction`, P:4236) |
| `td5_track_resolve_wall_contacts` (2502) | **B**, template `geo_resolve_walls` | new roam pass on `loc.inside` + push; legacy path inert at -1 (2535) |
| forward/reverse contacts (3293/3298) | **C** | forward does not exit at -1: would fire against the strip's sentinel edge; safe only when `s_boundary_fwd_sentinel` is -1 or no STRIP |
| `geo_locate`/`geo_build`/`geo_nearest_global` | **D** | reuse algorithm + CSR grid from road tiles; extend `GeoHit` with height, normal, surface, edge id (quad index), lateral offset, push |
| `geo_rescue_offmap`, `xspan_*` | C | strip-specific |
| `td5_track_probe_height` (7495), `shadow_probe_height` (7726) | **B** | both clamp -1 to span 0; redirect to the locator |
| `ComputeActorTrackContactNormal` (11974) | A / B | camera ground floor needs the locator height |
| `get_recovery_pose` (3461), `init_actor_segment_placement` (3582), `grid_main_lane` | **C** | gate off |
| `normalize_actor_wrap`, `resolve_actor_segment_boundary` | A | safe with ring 0; gate if a strip is loaded |
| route/AI functions (`sample_target_point`, `compute_span_progress`, `signed_offset`, `apply_target_span_remap`, lane assist, traffic_*) | D if AI cars exist in roam else C | |
| loop/corridor/junction/`branch_to_*` tables | A | pure lookups |
| MODELS.DAT, parser, scenery, registry | A (not contact) | |

## 4. Crash-risk (this part)
| # | Location | Expression | Guarded? |
|---|---|---|---|
| 1 | S:3781-3786 | `probe_span<0 -> track_span_raw -> if <0 probe_span=0`, then `compute_contact_height_bounded(0, ...)` | silent wrong read (span 0's plane at roam coordinates). Upward capped (forces airborne), downward open |
| 2 | T:1172 `vertex_at` | `&s_vertex_table[index]` | **no bound anywhere, no vertex count stored**; `!v` checks at 1913, 2373, 3373, 3412, 4323, 8626, 8692, 10399 are dead |
| 3 | T:5487-5490 (verified) | `vertex_at(left_vertex_index + left_off + sub_lane (+1))` | `sub_lane` not clamped in `compute_boundary_bits`; a stale probe lane of +127 reads ~1 KB past the span row (heap, usually no crash) |
| 4 | T:5510, 5512 (verified) | `s_edge_mask_first[sp->span_type]`, `s_edge_mask_last[sp->span_type]` (arrays of 12, type is uint8) | no `<12` clamp here, unlike 5481 (loader integrity only) |
| 5 | T:8620, 8686 | `k_target_vertex_offsets[(int)sp->span_type][1]` | no clamp (8769, 8871 clamp) |
| 6 | T:7012-7013, 7073-7083, 8090, 3326-3329, 12098-12117, 7915-7918 | `vertex_at(...)->x` | none; depends on loader integrity |
| 7 | T:11680 `td5_track_get_vertex` | only `index<0` rejected | no upper bound; S:1562, 1703-1704, 1917-1918, 2011-2012; camera.c:2039-2042, 5039-5042; render.c; game.c:9806-9807 |
| 8 | T:4966, 4971 (`td5_track_load_strip`; line 4966 verified) | `physical_span_count = (int)((vertex_offset - span_offset) / 24)` | if the offsets are unsigned and vertex_offset < span_offset the subtraction underflows (I did not check their types); only checked against `>= size`; corrupt-file case, not a roam issue |
| 9 | T:5724 `resolve_neighbor` | `&s_span_array[span_idx]` | safe because callers pass a valid span |
| 10-14 | T:3499-3503, 1849, 1863-1864, 8425-8451, 8643-8657, 4742-4745 | `% ring`, `/ ring_length`, `% span_len`, `j % L` | guarded (`ring>1`, `ring>0`, `ring_length==0` at 8388, `span_len==0` at 8644, `L<4` at 4728). 8443-8451 can store exactly `ring_length` |
| 15 | T:4408, 4365 | `1u << lane` | masked `&7` at 4408 but not on the call from 5414 (lane int8 up to 127): UB beyond 31, no crash on x86 |
| 16-18 | T:2616, 2703, 2075-2076, 2104, 1832-1838 | `s_fork_divider[span]`, `s_sl_of[span]`, `s_ring_pos[...]`, `/ s_geo_gcell` | guarded |
| 19 | T:3609-3613, 3642-3655 | `init_actor_segment_placement` | zeroes `out_pos`: caller's world_pos becomes origin (behavioural teleport) |
| 20 | T:5019-5021 | jump-table reads | guarded at 5008 |
| 22 | td5_hud.c:8381-8387, 8495-8499 | `% ring` where ring = `track_span_ring_length`, fallback `g_strip_span_count` | both can be 0 with no strip; agent did not trace an outer guard. The render-agent found the minimap early-out at hud.c:7641 (`g_strip_span_count<=0`), which covers it |
| 26 | td5_ai.c route reads | route[span*3] | T:4893-4898 records a previous 0xC0000005 from a stale span indexing a smaller route table |

## 5. Roam contact provider entry points (agent answer; mine is in PHYSICS_ENTRYPOINTS.md and matches)
Per racer per tick (slot < `g_traffic_slot_base`, mode 0), `td5_physics_update_vehicle_actor` (P:1244): (1) `td5_physics_clamp_attitude` (S:4158, P:1323), no track data. (2) `td5_physics_update_player` (P:1933) / `update_ai` (P:3362): `td5_track_get_surface_type(actor, 0..3)` (P:1837; reads LAST tick's body probes), `td5_physics_compute_surface_gravity` (`td5_physics_drivetrain.c:1291`; reads `probe_FL..RR`, whose `.y` is written in 3f). (3) `td5_physics_integrate_pose` (S:2357, P:1669): a. integrate position; b. `td5_track_update_actor_position` (S:2396) + span clamps (S:2409-2423); c. `td5_track_compute_runtime_heading_normal` (S:2435); d. `td5_physics_refresh_wheel_contacts` (S:3540, called S:2537); e. per wheel: seed probes (S:3607-3619), `update_probe_position` (S:3723), `compute_probe_contact_vertices` (S:3764), `compute_contact_height_bounded` (S:3815; writes ground_y and `wheel_contact_velocities`), `get_surface_type(actor, 4+i)` (S:3828, first wheel -> `surface_type_chassis`), `last_contact_was_capped` (S:3839); f. 4 body corners: `update_probe_position` (S:4072), `compute_probe_contact_vertices` (S:4078), `compute_contact_height_with_normal` (S:4088) -> `probe_FL..RR.y`; g. `td5_physics_update_suspension_response` (S:3082). (4) `td5_track_resolve_reverse_contacts` (T:3298, P:1710), `_forward_` (T:3293, P:1711), `_wall_` (T:2502, P:1712); each hit -> `td5_physics_wall_response` (C:179) -> `td5_physics_rebuild_pose` (P:5283) -> re-runs 3b, 3c and `refresh_wheel_contacts` via `update_vehicle_pose_from_physics` (S:3172, 3245, 3261, 3266).
Minimal replacement set: (1) `update_actor_position` (T:6745) (2) `update_probe_position` (T:6910) (3) `compute_contact_height_bounded` (T:7606) (4) `compute_contact_height_with_normal` (T:7482) (5) `get_surface_type` (T:5376) (6) `compute_runtime_heading_normal` (T:8105) (7) `resolve_wall_contacts` (T:2502) + gate 3293/3298 (8) `last_contact_was_capped` (T:7059). Also fix the span-0 fallback at S:3781-3786 and the `probe_height`/`shadow_probe_height` clamps (T:7506, 7737). Camera ground reads (camera.c:1351-1392, 1688-1695) are not physics but return height 0 at span -1; the camera rejects only via `TD5_CAM_GROUND_SANE_DY` (camera.c:1386).
