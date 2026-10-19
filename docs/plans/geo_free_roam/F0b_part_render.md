# Part: render / camera / HUD / light (agent ac639faf report; crash item 2 re-verified by me at td5_camera.c:5707-5731)

Verdicts: A works unchanged, B locator, C gate off or replace with tile cull, D rewrite for route/graph.
Paths under `td5mod/src/td5re/`.

## td5_render_mesh.c / td5_render.c
| file:function:line | reads | when | verdict | note |
|---|---|---|---|---|
| render_mesh:td5_render_actors_for_view:2482-2518 | `track_span_raw` (or `track_span_normalized` if raw>=ring) as cull centre `player_span` | per frame/view | C | -1 clamped to 0 only `if (ring>0)`; with ring==0 -1 flows into window math, getters are bounds-checked |
| :2536-2542 | `td5_camera_tour_cull_span()` | per frame, dev | C (dev) | |
| :2474-2475, 2587-2619 | `VIEW_DIST_FWD_SPANS`/`BACK_SPANS`=64, `fwd_window`, `back_window`, `actor_cull_window` (cull_max 64/32, x2, x traffic mult, drag 260) | per frame | C | pure arithmetic; port as world-distance radius |
| :2709-2751 | ring, total_spans, `eff_player` (reverse mirror `ring-1-span`), `walk_window`, `start_entry=(eff_player-walk_window)>>2` | per frame/view | C | negative start_entry OK (circuit wrap loops 2873-2874 end only if ring_entries>0) |
| :2780-2800 | TD6 proportional span->entry `lo=(span_lo*re)/ring` | per frame, TD6 | C (n/a) | guarded `g_active_td6_level>0 && td6_entries>0 && ring>0` |
| :2863-2903 | entry walk `td5_track_get_display_list_entry(entry_idx)` | per frame/view | C | getter bounds-checks `[0,s_models_dl_count)` (track.c:11732-11740); `s_submitted[4096]` capped (2898) |
| :2946-2966 | branch corridors `branch_to_main_span`, `get_display_list(span_index)` | per frame if total>ring | C | getter bounds-checks (track.c:11782-11825) |
| :2990-3000 + render.c:3900-4040 `td5_render_fallback_strip_ribbon` | eff_player, 256 window, get_span, `get_vertex(li+lanes)`, `scenery_span_settled` | per frame if no mesh table or streamed watermark | C | inert at s_span_count==0 but loops ~513x per frame; `get_vertex` has no upper bound |
| :3008-3031 + drag helpers 2154-2290 (render.c:3748-3880) | drag insert span, display-list entries 18/26/27/35, `get_span_lane_world` | drag only | A (never reached) | hard-coded entry indices, drag-gated |
| :2385-2400 (shadow prepass), :3245-3268 (body loop) | `track_span_normalized` delta actor vs owner ring-wrapped vs `actor_cull_window` | per frame/actor | C | equal synthetic ids => delta 0 => nothing culled; replace with XZ distance |
| :3393 | `track_span_raw` in debug log | dev | A | |
| :4600-4630 td5_render_init_environs | `s_environs_level`, zeroes `s_actor_light_zone[]` | per race | A | |
| render.c:td5_render_apply_track_lighting:3007-3018 | `(int16_t)track_span_raw`, mirrored by `tl_reverse_mirror_span`, `update_actor_light_zone` | per frame/actor | A | `span<0` takes explicit fallback (3008-3013): static light, no tunnel darkening |
| render.c:update_actor_light_zone:2300-2329 | `td5_light_zones[first+idx].span_lo/hi` | per frame/actor | A | idx clamped, slot range-checked |
| render.c:tl_apply_case1_midspan:2757-2790, case2:2805-2840, tl_blend_from_start:2571, from_end:2641 | get_span, get_vertex, zone span_lo/hi | per frame after zone selected | A | `aligned` can go negative, get_span NULL; `spacing==0` guarded (2767, 2812) |
| render.c:td5_render_update_projection_effect:3169-3170 | `track_span_raw` straight to `update_actor_light_zone` | per frame/actor | A (quirk) | NO `span<0` early-out: -1 walks to zone 0 and picks zone 0's environs page (cosmetic; gate) |

## td5_render_effects.c / td5_light.c / td5_pick.c / others
| file:function:line | reads | when | verdict | note |
|---|---|---|---|---|
| render_effects shadow grid:760-767, 835 | span count; `track_span_raw` clamped `[0,max_sp-1]`; sub_lane; `td5_track_shadow_probe_height` | per tick, only when `wp_ok` false | A on default wheel-plane path; B (locator height at node XZ) on fallback | guarded `max_sp>0`; no strip => height 0, `[floorY,ceilY]` clamp keeps it sane (my read of 750-849 agrees) |
| render_effects:1146-1178 (TD6 props) | `td5_track_td6_prop_get_mov`, `prop_ground_y` | per frame | B if roam uses props else no-op | gated by `prop_count()>0` |
| light.c:660-668 tunnel lamp drop | `td5_track_probe_height(lamp x,z,track_span_raw)` | once per lamp batch | B (locator height at lamp XZ) | probe_height clamps span to `[0,count-1]`, returns 0 if count<=0 (track.c:7495-7510); fallback `py-L[1]` |
| pick.c:483 | `td5_track_display_list_index(block)` | per frame, dev freecam | A | range-checked, -1 |
| render_pipeline.c, render_internal.h, light2.c, font.c, profile.c | none (comments only) | | A | |
| fe_geo.c | none (own router `s_spans`) | frontend | A | |
| minimap_streets.c:273-290, 528-535, 107-120, 639-672 | `g_strip_span_count`, `td5_track_get_span_edges` (bounds-checked), geo-route bounds | per sync/race | A | strip stamp guarded `g_strip_span_count>0` (528); no route => `have_bounds`=0, no corridor clip |

## td5_hud.c
| file:function:line | reads | when | verdict | note |
|---|---|---|---|---|
| hud:actor_span_index:1041-1048 | `+0x80` raw int16 | helper | n/a | |
| hud:3468 odometer | `actor_span_index % 10000` | per frame | A (cosmetic) | -1 shows 5295 |
| hud:3837-3850, 7545 street name | `td5_game_get_slot_span` into `td5_geo_attrs_name`; `td5_track_loop_of_corridor(track_span_raw)` | per frame | B (edge id -> name); code at 3847-3850 already falls back to `td5_geo_roads_nearest(world pos)` | `ga_in` bounds-checks |
| hud:3817-3820 td5_hud_draw_street_lines | `td5_geo_route_count()<2` returns | per frame | B or D | draws nothing without a route |
| hud:6372-6448 wrong-way | `+0x80` cur vs prev, counter>2, plus `td5_compute_heading_delta(route_state)` | per frame/view | D, or B (locator edge heading vs yaw) | span -1: counter never increments, never fires; the heading-delta call still runs every frame (see crash item 2) |
| hud:5749, 5824 position/lap | `race_position`, `td5_game_get_player_lap` | per frame | D (game-side) | |
| hud:6749-6764 dev stats SPAN/LANE | `+0x82`, `+0x80`, span count, lane count | dev | A | guarded |
| hud:7625-7641 td5_hud_render_minimap | `g_strip_span_count<=0` returns | per frame | D | WHOLE minimap blank without a strip (including the world-position-based street layer) |
| hud:7696-7770 | `+0x80`, ring, `branch_to_main_span`, `minimap_branch_remap` | per frame | D | -1 gives `start_span=(-1/24-6)*24`, clamped to 0 at 7777; scan bounded |
| hud:7848-7870, 7990-8020 road quads | `s_minimap_seg_*`, `span_base+near_idx*24` | per frame | D | bounds 7861-7862 and `minimap_span_edge:7069` |
| hud:8115-8135, 8319-8340 checkpoint connectors | `td5_game_get_minimap_checkpoint_span`, `main_to_branch_span` | per frame | D | `minimap_emit_*` range-check (6902, 7299) |
| hud:8225-8300 circuit walk | ring, `% ring` | per frame | D | ring from strip count if <=0; 7641 early-out |
| hud:8367-8420, 8487-8510 racer/traffic dots | `actor_span_index`, `minimap_normalize_span`, `% ring`, `% g_strip_span_count` | per frame/actor | D or B (window by world distance) | unreachable without a strip; dot positions use world X/Z |
| hud:6848-6862 minimap_normalize_span | ring, `branch_to_main_span` | helper | D | |
| hud:4664-4770, 4960-5030 hud_filler_draw_map (pane map) | `g_strip_span_base`, `g_strip_vertex_base`, ring walk, branch corridors | per frame | D (draw tile or graph) | returns if base NULL or count<=1 (4673); span index checked; vertex index `vi*6` NOT checked against the vertex count |
| hud:8670-8860 td5_hud_init_minimap | `g_strip_total_segments`, `span_base[s*24]`, `sb3[j*24]`, `sb2[i2*24]` | per race | D | segments and base cleared together (track.c:11875-11908) |

## td5_camera.c
| file:function:line | reads | when | verdict | note |
|---|---|---|---|---|
| camera:td5_camera_probe_ground_y:1351-1392 | `+0x80`, `+0x8C` seed a probe; `update_probe_position` x12; `compute_contact_height_bestlane` | per tick, 3 floor paths | **B** | crash item 4 (false trust) |
| camera:UpdateChaseCamera:1666-1696 | seeds 3 probes; `ComputeActorTrackContactNormal` | per tick/view | **B (3-point height/normal at an XZ)** | no strip: probe call returns at once, `out_y=0`; pitch=roll=0, no crash |
| camera:raycast_to_wall:2008-2070 (calls 2178-2185, 3261-3263) | `+0x80`, span count, `get_span`, `get_vertex(li+lane_count)` over span +-4 | per tick/frame | B (wall segments near the car) | guarded 2013; roam loses wall-avoid, no crash |
| camera:replay_eye_floor_clamp:1443-1460, 2231, 3324 | `probe_ground_floor` | per frame (replay trackside, invisible-car chase, paused fallback) | B | crash item 4 |
| camera:2724-2790, 2840-2848 tour_* | `get_span_center_world`, `get_slot_span` | dev | C (dev) | range-checked |
| camera:4251-4338 InitializeTracksideCameraProfiles | profile anchor_span, span_ofs into `g_spanTable + anchor*0x18` | per race, replay | A or C | crash item 1 |
| camera:4393-4434 SelectTracksideCameraProfile | `+0x80` vs profile ranges, raw `g_spanTable` indexing | per frame, replay | C | crash item 1 |
| camera:4528-4557, 4618-4660, 4806-4818, 4896 | raw `g_spanTable`/`g_vertexTable`; `g_camAnchorSpan[v]=(int)*(short*)(actor+0x80)` | per frame, replay trackside | C | crash item 1 |
| camera:5012-5071 debug fly-around | `get_span_*`, `% span_count` | no actors only | C (dev) | guarded (5027, 5066) |
| camera:5707-5731 td5_compute_heading_delta | `+0x82`, `route_table[(uint16_t)span_norm*3+1]` | per frame (hud.c:6392, physics_collision.c:487) | D | crash item 2 |

Camera modes: pose-only = chase heading (`display_angles.yaw` + orbit integrator 1607-1632), top-down (3820+), bumper/in-car, replay behaviours 1/2/7/8 and orbit 4/5/9/10 (`UpdateTracksideOrbitCamera` 3371+), spectate/AI panes, dev freecam, countdown fly-in (off by default, `TD5RE_FLYIN_ENABLED=0`; countdown uses preset 4). Span+probe seeded and degrade without strip: chase and fly-in via `UpdateChaseCamera` (lose slope tilt and wall clipping), ground-floor clamps. Need the span/vertex tables (replay only): trackside static (behaviours 0, 3), spline (6; template spans -1..+41), profile selection by `+0x80`; run only when `g_replay_mode && td5_camera_replay_trackside_ready()` (camera.c:2421, 5440); live play never uses trackside modes.

## Crash-risk (this part)
1. **Trackside camera raw span table, no bounds, NULL in roam.** camera.c:4300-4309 (`g_spanTable + anchor_span*0x18`, then `*(short*)(span_addr+4/+0x0C/+0x14)`), 4423-4434, 4534-4543, 4640-4652 (`span_idx = span_delta + g_camAnchorSpan[v]`, `va + vidx*6`), 4808-4816. Comment at 4262-4272 admits no length check. `g_spanTable` is NULL after `td5_camera_bind_track_geometry(NULL,NULL)` (track.c:4930): NULL deref at init (4300) if any replay profile is bound. `g_camAnchorSpan[v]=-1` (4896) reads at base-24; spline reads up to anchor+41 past the end. `s_splineTemplates[spline_type]` (4635, 4915) has 6 rows but `spline_type=g_camSplineNodeCount[v]` is an unsigned short from profile data (4482): OOB for >=6. Replay only.
2. **td5_compute_heading_delta, camera.c:5727 (VERIFIED by me).** `route_table[(uint16_t)span_norm*3u+1u]` has no length check. span_norm=-1 => byte offset 196606. `td5_ai_route_table` returns NULL only for an invalid handle (td5_ai.c:77-81). Runs every frame/view from hud.c:6392 (before the wrong-way test) and from physics_collision.c:487 (inside `td5_physics_wall_response`, i.e. on every wall hit). OOB read if roam keeps a route handle with span_norm=-1. Correct pattern: `td5_ai_route_speed_hint` (td5_ai.c:~84) checks `g_route_table_sizes`.
3. **td5_track_get_vertex (track.c:11680) has no upper bound.** Callers with computed indices: camera.c:2039-2042, 5039-5042; render.c:3970-3973, 2578-2579, 2645-2646, 2733-2735; hud.c strip readers use raw `vert_base + vi*6` (hud.c:4722-4733, 4600-4602, 7069+). Safe only after a bounds-checked `td5_track_get_span`; synthetic spans with garbage vertex indices would be OOB.
4. **Camera ground probe false trust.** camera.c:1355-1392, 1397-1404, call sites 1450, 2231, 3324. No strip: `update_probe_position` no-op, `probe.span_index` stays == `car_span` and the gap check passes; `compute_contact_height_bestlane(-1)` returns 0 (track.c:7557-7562); only rejection is `|0-car_y| > 0xC0000` (~3072 wu): below that it reports a TRUSTED ground of 0 => `floor_y=0x1800`, eye raised to ~Y=24 wu in three places (invisible-car floor, replay eye floor, paused fallback). Chase tilt: norm_*=0 => pitch=roll=0; sanity block at 1716 skipped (`norm_b_y==0`).
5. **Stale ring length.** `g_td5.track_span_ring_length` assigned only at track.c:4770 and 4983 and never reset on shutdown (`g_strip_*` and `g_spanTable` are reset, 11875-11908, 4930). After a strip race into roam ring>0 with span count 0: render_mesh.c:2515-2518 clamps player_span to ring-1, `ring_entries=(ring+3)>>2` (2820) and `is_circuit` wraps, `tl_reverse_mirror_span` mirrors by ring. Not a crash (getters bounds-check) but wrong windows. Reset ring or gate on a roam flag.
6. Checked and guarded: `entry_idx` (render_mesh:2864), `get_display_list(span_index)` (2953), `(eff_player-walk_window)>>2` (2745), `si` window (render.c:3946-3956), `s_actor_light_zone[slot]` (render.c:2313-2316), `td5_light_zones[first+idx]` (2321-2324), camera `chassis_span+-4` (2018-2021), `% span_count` (camera 5033, 5069-5070), hud `% ring`/`% g_strip_span_count` (8227-8505, early-out 7641), hud `span_base[s*24]` (8683, 8760-8800), `remapped_span/24-6` (7777), light.c:667 and effects.c:760-765 probe_height clamps. `td5_track_scenery_span_settled` returns 0 for span<0 but `e = span / TD5_SCN_SPANS_PER_ENTRY` is bounded only against `s_scn_reserved`.
7. Unbounded loops: `td6_banner_roadcenter_x` (render.c:1107-1120) O(spans) per banner mesh per frame (TD6 + banner align only); `td5_render_fallback_strip_ribbon` ~513 iterations/frame while `s_models_dl_count==0`.

Minimum roam work in these modules: replace the span-window walk with tile cull in `td5_render_actors_for_view` (entry walk, corridor loop, ribbon, two actor cull blocks); rewrite HUD minimap and pane map (both bail with no strip); fix camera probe false trust (item 4) and span-index bounds (items 1-2); provide a locator-backed height/normal query at an arbitrary XZ for chase tilt, camera floors and lamp drop.
