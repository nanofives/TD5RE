# Part: td5_ai.c / td5_ai_traffic.c / td5_ai_driver.c (agent a4433427 report; spot-verified: ai_traffic.c:5685-5702, ai_driver.c:320-585, ai.c:1480-1510, 4075-4105)

Verdicts: A works unchanged, B locator, C gate off in roam, D rewrite for route/graph (F5-F8). Paths under `td5mod/src/td5re/`.

## Gating notes
- Entry points to gate for F2: everything hangs off `td5_ai_tick` (td5_ai.c:2189): `td5_ai_compute_rubber_band` -> `td5_ai_traffic_dynamic_tick` -> `td5_ai_update_race_actors` (8740). `td5_ai_pre_tick` (9514) and `td5_ai_update_actor` (9579) have no live callers.
- Side effect of gating wholesale: `td5_ai_update_race_actors:8748` calls `td5_ai_refresh_route_state()` every tick for ALL active slots (humans too), writing `RS_FORWARD_TRACK_COMP` and `g_actor_forward_track_component[]`. Skipping freezes them; cop/escape/traffic/racer-AI read them and so does `td5_physics.c` via `td5_ai_get_route_state` (the agent did not check what physics does with them).
- -1 is the safe sentinel: every `td5_track_*` span function bounds-checks and returns 0/default; `ai_route_byte` and `td5_ai_route_speed_hint` do too. A synthetic edge id >= 0 is NOT safe while a strip or route table is still loaded (int16 fields; ids > 32767 wrap).
- `td5_ai_invalidate_actor_track_state` (td5_ai.c:1366, called from `td5_track_load_strip`) writes 0, not -1: roam must set -1 after that call.
- Ring length and span count may be stale with no strip (see game part finding 2): `%` guards only protect when they are really 0.

## td5_ai.c
| file:function:line | reads | when | verdict | note |
|---|---|---|---|---|
| td5_ai.c:`td5_ai_route_speed_hint`:92 | LEFT.TRK byte[2] at `span_norm*3+2` | per race/span, driver path build | D (-1 falls back to curvature: A-degraded) | guarded (`span<0`, `off>=size` -> -1) |
| :`ai_route_byte`:798 | `route[span*3+k]`, central accessor | per tick, ~25 sites | D (A if table NULL: returns 0) | guarded NULL, `span<0`, `off>=sz`; warns once |
| :`route_table_row_out_of_range`:830, `td5_ai_route_span_valid`:841 | same bound, exported | per tick | A | guarded |
| :`ai_route_row_for_actor`:852 | `span_norm`, or `span_raw` when raw>ring on generated tracks | per tick | D | ring 0 skips corridor arm |
| :`ai_route_heading_for_actor`:862 | route byte[1], heading `(b*0x102C>>8)&0xFFF` | per tick (script VM, recovery trigger) | D (heading from polyline tangent) | guarded via `ai_route_byte` |
| :`td5_ai_invalidate_actor_track_state`:1366 | zeroes span_raw/norm/accum/hw/sub-lane all slots | per track load | A | writes 0 not -1 |
| :`td5_ai_set_route_tables`:1394 | stores LEFT/RIGHT.TRK ptr+size, refreshes slots | per race load | D | |
| :`td5_ai_correct_spawn_heading`:1427 | `span_raw` into route byte[1]; fallback samples target points s0, s0+4 | per race, AI slots | D (spawn heading from route tangent) | guarded `span<0` return, `span_count<=1` return (so `(s0+4)%span_count` safe; verified 1495-1499) |
| :`td5_ai_update_actor_track_bounds`:1550 | `span_raw`, `span_norm`, 4 probe positions into `compute_span_progress`, `compute_signed_offset` with LEFT/RIGHT byte[0] | per tick, racers (human slot 0 included) | B (lateral offset, left/right extent) | route reads bounds-checked; track fns return 0 for -1 |
| :`td5_ai_remap_for_classify`:1652, `..._offset_clamp`:1676 | `span_norm+4` wrapped by ring, junction fold, `sample_target_point`, `compute_span_progress` | per tick (v1, peer pass 1) | D | single-subtraction wrap; byte idx clamped >=0 via `ai_route_byte` |
| :`td5_ai_refresh_route_state_slot`:1786 | PATH 1/2a/2b: `span_raw>ring` selects RIGHT table, junction match on `span_norm`; heading row; `fwd_comp = v . route_heading`; racers: `span_progress`, bounds, classify v2, signed offsets, `td5_track_get_span(span_raw)` lane mask + surface nibble | per tick, all active slots (also at load) | `fwd_comp`: B (tangent from locator) or D; surface>0x0F zeroing the bias: B (surface); rest D | span clamped `[0,route_count-1]`; get_span NULL-checked; `route_count==0` early path |
| :`td5_ai_tick`:2189 | calls rubber band, `traffic_dynamic_tick`, `update_race_actors` | per tick | C (gate here) | |
| :`catchup_gap_to_human_ahead`:2289 | `SPAN_HIGH_WATER` humans vs AI (int16) | per tick/AI slot | D, C in F2 | no index; int16 wrap only |
| :`td5_ai_compute_rubber_band`:2305 | `SPAN_ACCUM` slot 0 vs each AI (int16 delta) | per tick | D, C in F2 | equal spans => delta 0 => modifier 0; divisors constants 0x37..0x80 |
| :`td5_ai_init_race_actor_runtime`:2461, 2613 | `td5_track_get_junction_count/entries` picks LEFT/RIGHT table; rubber-band params | per race | D, C | `jt+j*6+4` bounded by junction_count |
| :`td5_ai_update_steering_bias`:3118 | `RS_LEFT/RIGHT_DEVIATION` only | per tick | A | pure |
| :`td5_ai_update_route_threshold`:3368 | `span_norm` into LEFT/RIGHT byte[2] threshold | per tick racers | D | guarded `span>=0 && idx+2<size` else 0xFF |
| :`td5_ai_classify_track_offset_clamp_v2`:3537 | `span_norm+4` wrapped by `g_strip_span_count`, remap walker, route byte[0], `sample_target_point` | per tick | D | single-subtract wrap; read via `ai_route_byte`; warns on OOB |
| :`td5_ai_find_offset_peer`:3642 | peer search by `span_norm` (dist `<0x28`/`<=0x28`), `span_raw`, route handle, `RS_ACTIVE_*` extents; pass 1 traffic, pass 2 racers | per tick, classic racers | D or C | **in roam all spans are -1 and dist is 0: every slot with matching `rs[3]` becomes a phantom peer; gate it**; route reads guarded |
| :`td5_ai_setup_phantom_peer_for_solo0`:4060 | `span+4 % g_strip_span_count` | per tick, solo TT | C | guarded `span_count>=1` (4091; verified) |
| :`td5_ai_update_track_offset_bias`:4138 | peer `span_norm` dist clamped [1,0x28]; strip nibble `strips[peer_span_raw*0x18+3]`; peer sub-lane | per tick | D | guarded `0<=idx<g_strip_span_count` |
| :`td5_ai_seed_actor_track_progress_offset`:4329 | `span_raw` into `compute_span_progress`; route byte[0] row 0 | per spawn (racers, traffic recycle/place/unstick) | B (lateral offset), D | track fns return 0 for -1/OOB |
| :`td5_ai_advance_track_script`:4413 (script VM) | heading vs route heading; case 9 `strips[span_raw*0x18+3]` | per tick while a script is active | D (heading source), C | case 9 guarded `span_raw>=0 && <g_strip_span_count`; `base[ip]` no length bound but all programs end in opcode 0 |
| :`ai_peer_is_present`:5146 | slot state, car_definition_ptr, parked flag | per tick | A | |
| :`smart_span_frame`:5092, `smart_actor_u`:5110 | `td5_track_get_span_route_frame` rails; lateral u | per tick | B (lateral offset + road width) | guarded `span<0 \|\| >=sc` |
| :`smart_span_gap`:5122, `smart_gather_blockers`:5165 | wrap gap by span_count; peers' span_raw within [-1..lookahead] | per tick | D (gap along route) | `span_count<=0` no wrap; loops terminate for count>=1 |
| :`ai_path_next`:5271, `smart_build_path`:5331 | `strips[span*0x18]` type, `link_next` at +8, ring/circuit wrap, loop mouths, commit | per tick (corridor mode generated slots; else modulo walk) | D (graph successor) | `ai_path_next` guarded; `smart_build_path` `%span_count`, callers check `span_count>0` |
| :`ai_corr_aim_rb`:5363, `ai_fork_aim_frame`:5407 | strip types 8/11, lane counts, rail frames | per tick, generated tracks | D | range-checked vs span_count and ring |
| :`smart_corner_eval`:5522 | `primary_route_heading` over a 3-span window; apex, u_line, speed cap | per tick | D (curvature from polyline) | `ps[48]`, D clamped; `span_count<=0` returns |
| :`smart_sense`:5591 | rails of next 8 spans as polylines (ray vs wall), lane count, peers by span gap | per tick | D (wall segments from edge polylines; B/D) | `ps[10]`; `npts<2` returns |
| :`td5_ai_smart_branch`:5805 | scans `strips[(span+d)*0x18]` for type 8/11 forks (d up to 40/47), `link_next`, peer congestion by span_raw, private RNG | per tick | D (graph decision at intersection), C | single-subtraction wrap at 5888; unsafe only in non-corridor mode when `span_count<look` |
| :`td5_ai_smart_lane_bias`:6127 | `span_norm`, `span_raw`, ring, rails, lane count at `look_span`, route byte[0] (`u_base`), peers' u, surface per lane, geo edge margin | per tick | B (lateral offset, surface per lane, road width) + D (`u_base` racing line) | `span_count<=0 \|\| span<0` returns; `lane_score[32]` bounded by clamped L |
| :`ai_gov_allowed`:6580, `ai_merge_*`:6643-6782, `ai_gov_apply`:6785 | path spans into heading windows, radius, speed cap; merge uses ring, corridor spans, peers' span_raw | per tick, generated tracks (`s_ylim_on_any`) | D, C | arrays 48, n clamped; `ai_merge_probe`: `ring>0`, span_raw in range |
| :`ai_follow_nearest`:6921, `ai_follow_apply`:6976 | span gap filter, then road-frame lon/lat in world units | per tick | D (filter), A (clearance math) | `span_count<=0` returns -1 |
| :`td5_ai_smart_speed_core`:7029 | `span_raw`; heading 7 ahead; corner eval; lane count; blockers; ring; `speed_cap_units(span_raw,0)` | per tick | D; speed cap B (edge id -> limit) | `(span_raw+7)%span_count` guarded `span_count>0` |
| :`td5_ai_smart_traffic_lane`:7315 | self+peers' span_raw and sub-lane (occupancy), `surface_is_slow(lane_surface(target_span,l))`, authored lanes | per tick traffic | D, C | `occ[pl]` guarded; `s_traffic_escape_lane_ttl[slot]` unchecked (slot from traffic caller) |
| :`td5_ai_smart_traffic_cruise_scale`:7456 | same-lane span_raw gap <=1/2/<=4 | per tick traffic | D, C | |
| :`td5_ai_update_track_behavior`:7478 | `span_norm`, ring: `lin=(span+4)%ring_len`, `apply_target_span_remap`, `sample_target_point(target_span, route_byte[lin], bias)`, corridor aim, recovery trigger (route heading vs yaw, hysteresis 0xA0), watchdog 150 ticks, smart-on-branch (`span_raw>=ring`) | per tick, racers + chasing cops | D (aim = polyline point 4 ahead + lateral bias) | guarded `span_count>0 && span>=0`; `ring_len` falls back to `span_count`; `pth[10]` indexed <= 9; at span -1 the deviation block is skipped and `RS_LEFT/RIGHT_DEVIATION` stays stale |
| :`td5_ai_update_special_encounter`:8368 (cop scheduler) | `span_raw` into `branch_to_main_span`, `smart_span_gap(…, ring_len)`; acquire window [0,60], max gap 110, `forward_track_comp>0`, speed gate | per tick | C, later D | no index |
| :`ai_update_single_racer`:8877 drag branch 8977-8988 | `span_raw+6`, `span_lane_count_at`, `get_span_lane_world` | per tick, drag only | C | negative aim_span => 0 lanes, no steer |
| :`ai_geo_slot_finished`:8863 | `td5_geo_loaded`, `route_count`, finished-at-line | per tick | D (finish on route) | |
| :`traffic_drive_normal`:9100 | `td5_track_nearest_road_lane(span_raw, sub_lane)` | per tick, TD6 | C | guarded in track |
| :`cop_corner_imminent`:9234, `td5_ai_cop_chase_throttle_boost_q8`:9269 | `branch_to_main_span`, heading window `% ring` | per tick (physics calls it) | D, C | `ring<=0` returns 0 before `%` |
| :`cop_drive`:9296 | gap via `branch_to_main_span` + `smart_span_gap(ring)` or `|cop_span-tgt_span|` when ring==0 | per tick | D, C | guarded `ring>0` |
| :`mp_cop_pick_target`:9540 | `|span_raw diff|` nearest suspect | per tick | D (metric distance), C | |

## td5_ai_traffic.c
| file:function:line | reads | when | verdict | note |
|---|---|---|---|---|
| ai_traffic.c:`td5_ai_find_nearest_route_peer`:105 | span_raw ordering ahead/behind per polarity, same sub-lane, same route handle, dist `0<d<=0x20`; scans all 22 slots incl. inactive | per tick/traffic slot | D (peer ahead on same lane by route progress), C | no index; in roam every dist 0 => returns self ("no peer") |
| :`ai_player_span_lead/trailing`:380 | `span_norm` of local humans | per tick | D, C | |
| :`td5_ai_recycle_traffic_actor`:425 (legacy, Dynamic=0) | player span_norm, queue `q_span`, strip lane nibble, remap, init placement; recycle >0x28 spans behind | per tick | C | **unguarded: :582 `strip[(size_t)q_span*0x18+3]`, only `strip!=NULL` and `q_span != -1` checked** |
| :`td5_ai_init_traffic_actors`:849 | queue records (span, lane), strip lane nibble, vertex pool for heading, `compute_span_progress`, route byte[0] | per race | C | strip nibble and `sp` range-checked; :998-1000 and :1163 index `vpool[sp->left/right_vertex_index]` and `psVar[0..8]` with no vertex bound (trusting the strip) |
| :`traffic_smart_antifreeze`:1493, `traffic_force_unstick`:1541 | recovery stage; `compute_heading`, seed progress, `normalize_wrap` | per tick | C (re-seed at locator = B) | |
| :`td5_ai_traffic_note_offroad`:1566 | off-road depth from physics edge containment, 45-tick counter | per tick | B (locator off-road/push) | slot checked |
| :`traffic_collision_escape`:1638 | `SPAN_ACCUM` progress over 30 ticks (<1 span = jam), speed, `traffic_escape_pick_side` | per tick traffic | D (route progress or displacement), C | int16 difference only |
| :`traffic_lane_is_clear`:1738 | peers' span_raw offset [-2..6] + same sub-lane | per tick | D, C | |
| :`racer_collision_escape`:1883, `racer_escape_pick_side`:1845 | speed, V2V contact, SPAN_ACCUM tie-break, `lane_count(span_norm)` | per tick AI racers | A mostly; pick_side D/B, C | lc clamped >=1 |
| :`traffic_smart_react_to_peer`:2058, `choose_lane`:2123, `wall_nudge`:2185 | `surface_is_slow(lane_surface(span,lane))`, `get_span_lane_world(span,inward)` | per tick | B (surface), D | lane indices bounds-checked |
| :`trf_dyn_lane_direction`:2476, `lane_change_blocked`:2531 | lane direction map (TRAFFIC.BUS), polarity | per tick | D | lane index and slot checked |
| :`trf_dyn_race_span_lead`:2506, `trf_battle_evade_lane`:2554 | `span_norm` of racers; nearest racer by `|span - target_span|` <=10 | per tick/spawn | D, C | |
| :`trf_anchor_span`:2908, `trf_dyn_cluster_count`:2917 | anchor span_norm; union-find share radius = despawn+eff_hi | per tick | D, C | `parent[TD5_MAX_RACER_SLOTS]`, n clamped |
| :`trf_dyn_min_player_dist`:3075, `trf_dyn_span_dist`:3107 | `|dspan|` wrapped by ring on circuits only | per tick (despawn, spawn gate) | D (distance along route) | `while(d>ring/2)` guarded ring>0 |
| :`trf_dyn_count_traffic_near`:3124 | traffic count within radius spans; folds branch cars via `branch_to_main_span` | per spawn | D | |
| :`trf_cluster_radius/max`:3165-3188 | radius 8 spans, max 2 (4 at volume>=4) | per spawn | D | |
| :`trf_dyn_clear_lane_count`:3205, `trf_unwall_should_yield`:3261 | `lane_count_at(span_raw)==2`, other-lane peers `|dspan|<=1`, release at >=4 | per tick traffic | D, C | |
| :`trf_dyn_starved_player_span`:3323, battle HWM `trf_battle_hwm_*`:3366-3446 | span_norm; `td5_game_get_slot_progress` (lap*ring+span); `hwm % ring` | per tick | D | `ring>0` guarded; `ring<9000` is a heuristic circuit test |
| :`trf_dyn_effective_spawn_window`:3655, `spawn_dist_x16`:3586, `front_keep_floor`:3622, `rear_keep`:2830, `start_clear`:2815 | spawn/despawn windows in spans | per tick | D (metres or route distance) | window clamped to `ring/3` when ring>0; INI-clamped (`main.c:1592-1599`); fade_ticks clamped [1,255] |
| :`trf_dyn_place`:3739 | writes `span_raw` (int16), sub_lane; `init_actor_segment_placement` (accum = high-water = raw), `compute_heading`, seed progress, `normalize_wrap`, corridor fold via `resolve_actor_segment_boundary` | per spawn | D (place on graph edge/lane), B (height) | `(int16_t)span` truncates ids > 32767 |
| :`td5_ai_td6_drivable_band`:3877 | route tables byte[0] at `route_span*3`, td6 road band | per spawn/tick (TD6) | D, C | guarded `idx>=sizes` -> fallback band |
| :`trf_authored_*`:4000-4037 | TRAFFIC.BUS authored lanes within 160 spans; `lane_count_at` | per spawn/tick | D | bounded by count |
| :`trf_dyn_pick_lane_dir`:4039 | lane_count, surface per lane, `% lane_count` | per spawn | B (surface), D | guarded `lane_count>1`; loop bounded |
| :`trf_dyn_pick_branch_main_span`:4103 | corridor info; ring-shifted range | per spawn | D (graph) | guarded `ring>0`, `nq>0` |
| :`trf_dyn_spawn_in_window`:4287 | ring/span_count (`limit`); `span=ps+dist` mod ring (circuits); edge guard `span<3 \|\| span>=limit-8`; start clearance vs `track_start_span_index`; `lane_count_at`; `branch_corridor_span` | per spawn (8 tries) | D (spawn point along route/graph) | `limit<=16` returns; `%(a_win_hi-a_win_lo+1)` >=1 given clamps |
| :`trf_dyn_seed_window`:4570 | seed band relative to start line | per race | D, C | |
| :`td5_ai_traffic_dynamic_race_init`:4621 | TRAFFIC.BUS: `lane_count_at(q_span)`, `votes[lc][lane][pol]`; seeds at `track_start_span_index` | per race | C | lc in [1..MAX], lane<lc checked; loop capped 4096 |
| :`td5_ai_traffic_dynamic_tick`:4802 | state machine: span_norm, `behind = sp - trailing`, `ahead = sp - lead`, ring wrap; despawn on rear (>65, battle 50), `behind < -rear_keep`, `ahead > front_keep`, `min_dist > front_keep` (floor 228), stuck, wreck; spawn cadence; fade 12 ticks | per tick | D, C | `fade_step=255/fade_ticks` safe; wraps guarded |
| :`td5_ai_update_traffic_route_plan`:5472 Stage 1 | dynamic parked check or legacy recycle | per tick | C | |
| :route_plan stuck detector:5558 | span_raw unchanged and `|v|<0x1000` for 90 ticks => force-unstick or despawn | per tick | D (displacement), C | |
| :route_plan Stage 2:5694 | `ref_span_norm` into `rb[...*3+1]`, heading mismatch band (0x400,0xC00), corridor skip (raw>=ring) | per tick | D | **unguarded: :5698 `rb[(size_t)(unsigned)ref_span_norm*3u+1u]` checks only `rb!=NULL && span_norm>=0`, no `g_route_table_sizes` check (verified)** |
| :route_plan Stage 3:5807 | near-edge `span_raw<3 \|\| span_raw>=ring-8` (non-circuit, selector==0), recovery latch | per tick | D | guarded ring>0 |
| :route_plan Stage 4:5849-5912 | cruise scale; `td5_geo_attrs_speed_cap_units(span_raw,1)` | per tick | limit = B (edge id -> limit), rest A | guarded `span<0 \|\| >=nspans` returns 0 |
| :route_plan ray brain:5919 | `smart_corner_eval`/`smart_sense` at span_raw | per tick | D | `tsc>0` guarded |
| :route_plan Stage 5:5989-6384 | `lookup=(span_norm+1)%span_count`, `fallback=(raw+-1)`, remap, `traffic_next_span`, `lane_base`, taper, `fork_ahead(12)`, drivable band, `get_span_lane_world(target_span, sub_lane)`, one more lookahead, gap <12000 | per tick | D (aim = lane point ahead on route/edge), B (height) | outer guard `span_count>0 && span_norm>=0`: with -1 the whole block is skipped and deviations go stale |
| :route_plan Stage 6:6387 | `RS_LEFT/RIGHT_DEVIATION` into `update_steering_bias(0x8000)` | per tick | A | |
| :route_plan Stage 7:6390-6512 | `find_nearest_route_peer`, `span_diff_dir`, TTC `dspan*0x100`, brake gates | per tick | D (replace dspan with route distance), C | divisions guarded for non-zero |
Dynamic-traffic window defaults (spans): spawn min 25 / max 50 scaled x2 by default (50..100), capped `ring/3`; despawn 65 (>= max+5); forward keep `max(despawn+eff_hi, 228)`; rear keep 65 (battle 50); start-line clearance 200 (16 seeding); cluster radius 8, max 2 (4 at volume>=4); cop acquire window [0,60] (`td5_ai.c:575-588`), max chase gap 110 (`:491`); legacy recycle 0x28.

## td5_ai_driver.c
| file:function:line | reads | when | verdict | note |
|---|---|---|---|---|
| ai_driver.c:`pt_span_rails`:339 | `td5_track_get_span_route_frame(span)` left/right rail points | per race/span | D | guarded in track |
| :`driver_build_path_table`:363 | `count = ring_length` (or span_count if ring<=2); `s_pt_circuit = (track_type==CIRCUIT)`; per span rails, centre `0.5*(L+R)`, `seg_len`; curvature cap from `primary_route_heading(s)` vs `(s+3)%count` (12-bit turn / `CURV_SHARP=0x2A0`) or line radius if `LINE=1` (default off); authored hint `td5_ai_route_speed_hint(s)` blended by `ROUTE_PULL=0.70`; backward braking pass + forward accel pass | per race (once) | D | `count<=2` returns with `s_pt_valid=0`; realloc failure and smaller later ring handled (`s_pt_cap` reset); all `%count` have count>2 (verified 363-585) |
| :`td5_ai_driver_race_init`:591 | calls build; persona seeds from (slot, tier) | per race | D | |
| :`driver_racecraft`:659 | self `span_norm`: `hw` from `route_frame(((span%ring)+ring)%ring)`, fallback 0.25e6; others by world position only | per tick | B (half road width = locator lateral extents), A (rest) | `ring<=2` returns before `%` (verified 668-669) |
| :`drv_geo_speed_cap`:798 | `span_norm` into `td5_geo_attrs_speed_cap_units(span,0)`/`maxspeed(span)` | per tick | B (edge id -> limit) | guarded |
| :`td5_ai_driver_tick`:828 | `span_norm` into `s_pt_frac[]` (clamped >=0, `%count`); `lane_count(span_norm)` into `aim_lane`; `laneassist_target(span_raw, aim_lane, lookahead 3+<=4, fork_commit=1, fork_diverge, lane_band=1)`; racing line `s_pt_lat[(span+look)%count]`; `span_norm/3` jitter hash; `race_position` leash; dynamics + `track_contact_flag` | per tick, AI racers | D | indices guarded; span -1 reads profile index 0 (wrong data, no crash); `laneassist_target` returns 0 so `have_target=0` and steering bleeds to centre |
| :`td5_ai_driver_target_speed`:1222, `rec_state`:1230 | per-slot arrays | per tick | A | slot range-checked |
| :recovery FSM:926-1023, controller:1077-1220 | speed, yaw, slip only | per tick | A (once aim point + speed profile exist) | |

### What td5_ai_driver.c needs as input to be fed a route polyline
Per race (path table): an indexed ring with `count>2` (today `td5_track_get_ring_length()` else `td5_track_get_span_count()`; index = `span_normalized`); per index left/right rail points (world track units, int) from `td5_track_get_span_route_frame(i)` (centre `0.5*(L+R)`; with LINE=0 only the centre is used; segment length `|mid(i+1)-mid(i)|` with `(i+1)%count`, so a wrap flag); heading `td5_track_get_primary_route_heading(i)` and `(i+3)%count` as 12-bit angles (a polyline tangent angle is a drop-in; curvature cap = `1 - 0.72*min(1,|dheading|/0x2A0)` floored 0.28); circuit flag `g_td5.track_type==TD5_TRACK_CIRCUIT` (wrap + 2 backward passes vs 1); optional authored speed hint `td5_ai_route_speed_hint(i)` 0..255 or -1 (-1 everywhere is supported).
Per tick: progress index = actor `track_span_normalized`; lane count `td5_track_get_span_lane_count(i)` (only for `aim_lane`); aim point `td5_track_laneassist_target(span_raw, aim_lane, lookahead 1..8, ..., &tx,&tz)`, the only geometry-heavy dependency: a near-weighted blend of lane-band-centre points (weights `lookahead-i+1`, i=1..lookahead) in world 24.8, following forks (a polyline version only needs "point k ahead on the route, optional lateral offset", then the same blend); posted limit `td5_geo_attrs_speed_cap_units(i,0)` (optional); half road width at self (fallback 0.25e6); other actors by world position, `longitudinal_speed`, slot state only (racecraft does not need span ordering).

## Crash-risk (this part)
Unguarded or weak, relevant to roam:
1. `td5_ai_traffic.c:5698` `rb[(size_t)(unsigned)ref_span_norm*3u+1u]`: only `rb!=NULL && ref_span_norm>=0` (verified). A synthetic id >= 0 or any stale span past the table reads OOB. -1 is safe.
2. `td5_ai_traffic.c:582` `strip[(size_t)q_span*0x18+3]` in the legacy queue recycle (`q_span` from TRAFFIC.BUS checked only against -1). Gating Dynamic=0 + the queue off in roam removes it.
3. `td5_ai_traffic.c:998-1000, 1158-1159` `&vpool[sp->left_vertex_index]`, `psVar[0..8]`: sp range-checked, vertex indices trusted.
4. `td5_ai.c:5888-5890, 5921` `strips[(size_t)s*0x18]` after a single `s -= span_count`: unsafe only in non-corridor mode with `span_count < look` (look up to 40/47); safe for any real track.
5. `td5_ai_traffic.c:3750` `ACTOR_I16(a, ACTOR_SPAN_RAW) = (int16_t)span`: ids > 32767 wrap negative (same for accum/high-water writes).
6. `td5_ai.c:1713/1622, 3553-3579`: wraps subtract the span count once; later reads go through `ai_route_byte` or bounds-checked `td5_track_*`: not a crash.
7. `td5_ai.c:7346` + traffic-lane caller: `s_traffic_escape_lane_ttl[slot]` no range check (slot <= TD5_MAX_TOTAL_ACTORS from caller).
8. `td5_ai.c:4584-4587` `base[ip]` in the script VM: ip unbounded by program length, programs end in opcode 0; not span-related.
9. `route_state(slot)` and `actor_ptr(slot)` (`td5_ai_internal.h:202-208`) do no bounds checking; callers check slot.
Guarded (verified by agent): `ai_route_byte` (td5_ai.c:798), `td5_ai_route_speed_hint` (92-99), explicit `*3<sizes` checks at td5_ai.c:1617-1624, 3406-3408, 2641 and traffic:3917, refresh row clamp (1920-1928), strip reads (4256-4268, 4670); the `td5_track_*` functions listed in part_track.md; `td5_geo_attrs_speed_cap_units`. `%` by ring/span_count 0: ring 0 with span_count>0 accepted (`ring_len` falls back); guarded sites td5_ai.c:1499, 4094-4095, 5335, 5457, 7052, 7877/7953, traffic:6001/6003, td5_ai.c:9251-9252, traffic:3382, 4138, 4394, 4401, 4279, driver 406, 424-425, 676, 861, 914; `td5_track_normalize_actor_wrap` returns 0 at ring 0. Other division sites: `255/traffic_dyn_fade_ticks` clamped >=1 (`main.c:1598`), `g_rb_*_range` constants >=0x37, TTC divisions (traffic:6476/6478) guarded, `% (uint32_t)(...)` at traffic:4385, 4136, 4479, 4480 have denominators >=1, `% lane_count` at traffic:4064/4042 guarded.
