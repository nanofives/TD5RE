# F0b REPORT: span-consumer table for GEO FREE ROAM

Model: claude-sonnet-5-5. Task: READ-ONLY. Base: master 29f89b22 working tree (only `docs/plans/GEO_FREE_ROAM.md` untracked). Date 2026-10-15.
No tracked file touched, nothing built or run. Everything below comes from reading code (line numbers are for this tree).

## How this was produced and how far to trust it
- Five read-only agents each swept a module group and returned a consumer table plus a crash-risk list. I read the code behind every claim that drives a decision (physics contact path, wall resolvers, `geo_locate`, spawn, camera probes, route-table reads, selftest invariants) and re-verified the crash items marked **(verified)**. Items marked **(agent)** are the agents' reading and were NOT re-read by me.
- **The tables live in per-module files in this folder** (I could not concatenate them: Bash `cat >` is denied in this session):
  - `PHYSICS_ENTRYPOINTS.md` (mine): per-wheel data physics consumes, tick order, the roam provider entry points.
  - `part_physics.md`: td5_physics*.c, assists, laneassist, chaos, light, vfx, rt, replay, net, sound, damage.
  - `part_track.md`: td5_track.h public API (every function, callers, guards), internal resolvers, `geo_locate`/`geo_resolve_walls` anatomy.
  - `part_ai.md`: td5_ai.c, td5_ai_traffic.c, td5_ai_driver.c, plus what the Driver Model needs to be fed a polyline.
  - `part_game.md`: td5_game.c, frontend, selftest, input, asset, control, trace.
  - `part_render.md`: render/camera/HUD/minimap/light.
- Not covered by an agent or by me: `td5_trackgen*.c` / `td5_tg_*.c` / `td5_geo_*.c` (the strip BUILD path itself, which is not a runtime consumer: roam does not run it), `td5_physics_internal.h` users outside the files above, `td5_hud.c` outside the span sites listed, `td5_fe_*.c` beyond `td5_fe_race.c`/`td5_fe_geo.c`. The strip BUILD modules write the span world; they are "retired in F11", not roam consumers.

## Counts per verdict (approximate)
Counted by regex over the consumer-table rows in the five `part_*.md` files, by the LEADING verdict letter of each row (rows with two verdicts, e.g. "B + D", count once, under the first letter). Expect about +-10%: some rows are grouped sites, some cells are free text.

| Verdict | Meaning | Rows |
|---|---|---|
| A | works unchanged with span -1 / synthetic id | ~58 |
| B | must read the locator (field named in each row) | ~38 |
| C | gate off in roam | ~84 |
| D | rewrite for route/graph (F5-F8) | ~80 |
| total rows | | ~260 |

Reading it: most race plumbing (td5_game.c lap/checkpoint/finish/results, frontend, asset loaders, TD6/drag/battle code, replay camera) is **C**: switch off. The span-keyed AI/traffic/HUD/minimap/catch-up code is **D**: that is the F5-F8 work. Only ~38 rows need the locator (**B**), and they funnel into the 8 functions in the physics answer below.

## The answer in one paragraph
Physics never touches a strip directly. Everything it needs per tick is produced by **one function** (`td5_physics_refresh_wheel_contacts`, `td5_physics_suspension.c:3540`) and **two small resolver families** in `td5_track.c`, and every strip access inside them already returns 0 / dry asphalt / no-op when no strip is loaded. So a ROAM provider is 8 replaceable functions (below), not a rewrite of physics. With NO provider and no strip, the car does not crash: it drives on a flat plane at y=0 with a (0,4096,0) normal, dry asphalt, a zero `heading_normal` (no slope forces, recovery exit stuck) and dead walls.

## Actor span fields in roam: recommendation
Write **-1 in all four span fields and the lane**, unload the strip (so `s_span_count==0`) AND set `g_td5.track_span_ring_length = 0` explicitly, and keep the roam edge id in a side table keyed by slot, not in the int16 actor fields. Reasons (all in the part files):
1. A synthetic id >= `td5_track_get_span_count()` is silently **rewritten** to `count-1` by the clamps at `td5_physics_suspension.c:2412-2423, 3730-3732, 4075-4077` and `td5_physics.c:5729-5730, 5778-5779`.
2. The fields are `int16_t`: ids > 32767 wrap negative and read as "invalid" everywhere (also in `td5_replay.c:49-55`).
3. An id >= 0 indexes route tables with only a `>=0` check in places (`td5_ai_traffic.c:5698`, **verified**), and with no check at all in `td5_compute_heading_delta` (`td5_camera.c:5727`, **verified**, -1 is the crashing value there, see list).
4. With a strip still loaded, a synthetic id in range makes the walker move the car (`td5_track.c:6745`). With span count 0 every walker/clamp/modulo guard is inert.
5. `td5_ai_invalidate_actor_track_state` (`td5_ai.c:1366`, called from `td5_track_load_strip`) writes 0, not -1: roam must write -1 after it.
6. `g_td5.track_span_ring_length` is NOT reset when the strip unloads (`td5_track.c:4921-4938`; written only at 4770 and 4983): a strip race followed by roam leaves ring > 0 with span count 0 and wrong windows in render/HUD/normalize_wrap. **(agents, game+render, consistent)**

## What physics consumes per tick (full detail in PHYSICS_ENTRYPOINTS.md)
Per wheel (4): ground height (24.8), ground normal (int16[3], magnitude 4096, world space, y never 0; stored in `wheel_contact_velocities` +0x250, NOT in `wheel_contact_normals` +0x230, which is a rotated body offset), a "capped extrapolation" flag that forces the wheel airborne, airborne bit (derived from height). Surface type: per body probe for grip (`td5_physics.c:1837`, reads last tick's probes) and one value for the chassis (`surface_type_chassis`, from the first wheel). Per body corner (4): ground height into `probe_FL/FR/RL/RR.y`, which feeds `td5_physics_compute_surface_gravity` (downhill pull). Chassis: `heading_normal` (+0x290) for slope force, missing-wheel correction (multiplies by `.y`), recovery/tumble tests. Wall: `td5_physics_wall_response(actor, wall_angle12, penetration<0, side, probe_x_fp8, probe_z_fp8)` (`td5_physics_collision.c:179`), then `td5_physics_rebuild_pose`.

## Minimal ROAM contact-provider entry points (call order, per racer per tick)
All in `td5_track.c`; physics call sites in brackets. Details and the full call chain in `part_track.md` section 5 and `PHYSICS_ENTRYPOINTS.md`.

| # | Function (td5_track.c) | Physics callers | Replace with locator field |
|---|---|---|---|
| 1 | `td5_track_update_actor_position` (6745) | susp 2079, 2396, 3245; phys 6136, 6190; assists 2764 | seed locator hint; write span fields (-1) |
| 2 | `td5_track_update_probe_position` (6910) | susp 2240, 3723, 4072; phys 5728, 5777; camera 1366, 1688-1694 | per-probe locate |
| 3 | `td5_track_compute_contact_height_bounded` (7606) | susp 3815 | **height + normal** |
| 4 | `td5_track_compute_contact_height_with_normal` (7482) | susp 2101, 2249, 4088, 4107; coll 1671 | **height** (+normal) for body corners, traffic |
| 5 | `td5_track_get_surface_type` (5376) | phys 1837; susp 3828; ai 2087 | **surface type** |
| 6 | `td5_track_compute_runtime_heading_normal` (8105) | susp 2090, 2435, 3261 | **normal** into `heading_normal` (y never 0) |
| 7 | `td5_track_resolve_wall_contacts` (2502) (+ gate `_forward_`/`_reverse_` 3293/3298) | phys 1710-1712 | **inside flag + push vector** into `td5_physics_wall_response` |
| 8 | `td5_track_last_contact_was_capped` (7059) | susp 3839, 4110 | return 0, or a locator "extrapolated" flag |
Also fix/redirect: the span-0 fallback at `td5_physics_suspension.c:3781-3786`, and the `[0,count-1]` clamps in `td5_track_probe_height` (7495) and `td5_track_shadow_probe_height` (7726) (they silently read span 0 for -1; used by `td5_light.c:667`, shadow fallback, dev telemetry).
Gate (C): `td5_track_get_recovery_pose` (3461), `td5_track_init_actor_segment_placement` (3582), forward/reverse sentinel contacts.

Correction to the plan's premise: **`geo_resolve_walls` is the primary wall handler only when `geo_active()`** = env on AND `g_td5.reverse_direction` AND not TD6 (`td5_track.c:1761`). Forward custom tracks run the rail-LUT path with `geo_rescue_offmap` (2446) as an off-map backstop only (`geo_query_active`, 1768). `geo_locate` (2058) returns `GeoHit{span, lane, inside, pushx, pushz, pushdist}` and has NO height, normal, surface type, quad index or lateral offset. `geo_resolve_walls` is nevertheless the right template for provider #7: it takes the 4 body corners, keeps only the lateral push (perpendicular to heading, dropped under 0.5), caps it at `GEO_PUSH_CAP`=600 and calls `wall_response` + `rebuild_pose`. A roam provider can reuse its body verbatim after swapping the locator call. The roam level must NOT register as a custom registry level (`td5_track_bind_boundary_sentinels`, `td5_track.c:2920`) or the forward-custom sentinel/rescue logic arms.

## Things that break silently in roam (no crash, wrong behaviour), ranked by how much they would waste an F2 debugging session
1. **Spawn silently skips the actor** when the strip has no spans (`td5_game.c:4602-4608`, `td5_asset.c:2434-2439`): never placed, world_pos origin, slot_index unset. Roam needs its own spawner (`part_game.md` lists the minimum).
2. **Camera ground probe trusts height 0** (`td5_camera.c:1355-1392`): with no strip `compute_contact_height_bestlane(-1)` returns 0 and the only rejection is `|0-car_y| > 0xC0000` (~3072 wu), so the chase/replay/paused eye is clamped to ~Y=24 wu in three places. **(agent)**
3. **HUD minimap and pane map are blank** without a strip (`td5_hud.c:7625-7641`, `4673`), including the world-position-based street layer. RT road BLAS is not built (`td5_rt.c:341-420`); RT scenery window centres on `track_span_normalized` (`td5_rt.c:819-840`). **(agent)**
4. **Render window/cull**: actor span-cull (`td5_render_mesh.c:2385-2400, 3245-3268`) compares `track_span_normalized`; equal -1s give delta 0 so nothing is culled. Track display-list walk (`:2709-2903`) and `td5_render_fallback_strip_ribbon` (render.c:3900, ~513 iterations/frame at span count 0) are strip-only: tile cull replaces them. (verified the actor-cull code; rest agent)
5. **Selftest invariants**: `st_invariants_result` flags "no progress" (WARN) when `td5_game_get_slot_progress` is constant (`td5_selftest.c:1029`, `td5_game.c:1877-1890`). Roam scenarios need a distance-based progress.
6. **vfx weather density** never matches (`td5_vfx.c:1351-1396`: `current_segment == seg_id`): roam weather must come from the geo options. **Light zones** fall back to static light with no tunnel darkening; `td5_render_update_projection_effect` (render.c:3169-3170) has no `span<0` early-out and picks zone 0's environs page (cosmetic). **(agent)**
7. **Selftest, race order, catch-up, rubber band** read `+0x82/+0x84/+0x86`: all slots tie at -1, so nothing swaps and `race_position` is stuck (no index risk). AI peer search treats every slot as a phantom peer at distance 0 (`td5_ai.c:3642`): gate the AI tick wholesale (`td5_ai_tick`, `td5_ai.c:2189`), but note `td5_ai_update_race_actors:8748` also refreshes route state for human slots (`RS_FORWARD_TRACK_COMP`), which physics reads: check before freezing it. **(agent; physics side not checked)**
8. **Per-tick wrap normalizer** `td5_track_normalize_actor_wrap` (`td5_track.c:8362`, called `td5_game.c:7700, 7896`) rewrites `+0x82` from `+0x84` for every actor each tick; guarded only for `ring_length == 0`. Gate it or keep ring 0.
9. **Checkpoints/timers load for a roam "track slot"** if `track_index < 19` (`td5_game.c:5264-5295`), arming the P2P timer (expiry DNFs every slot); `build_results_table` invents finish times from `rand()` for unfinished racers (`td5_game.c:10228-10231`). Force `checkpoint_count=0`, `g_special_encounter=0`.
10. **Recovery**: `td5_physics_recover_player` breakdown path with span -1 gets a ring-wrapped pose from `td5_track_get_recovery_pose` (teleport onto the strip if one is loaded) or aborts (`td5_physics_assists.c:2244`, car left frozen). In-place default branch is fine.

## CRASH-RISK list (consolidated, ranked)
**Real out-of-bounds or NULL risks reachable in or near roam**
1. **`td5_compute_heading_delta`, `td5_camera.c:5727` (verified).** `route_table[(uint16_t)span_norm*3u+1u]` with no length check. `span_norm = -1` -> index 65535*3+1 = byte offset 196606. `td5_ai_route_table` returns NULL only for an invalid handle (`td5_ai.c:77-81`). Called every frame per view from `td5_hud.c:6392` (before the wrong-way test) and from `td5_physics_collision.c:487` (in `td5_physics_wall_response`, i.e. on EVERY wall hit). OOB read (garbage heading or fault) whenever roam keeps a route handle in `rs[RS_ROUTE_TABLE_PTR]` with span -1. Safe pattern to copy: `td5_ai_route_speed_hint` (`td5_ai.c:92-99`) checks `g_route_table_sizes`. Fix: bounds-check or return 0 when `span_norm < 0`.
2. **`td5_ai_traffic.c:5698` (verified).** `rb[(size_t)(unsigned)ref_span_norm*3u+1u]` checked only for `rb != NULL && ref_span_norm >= 0`, no table-size check. A synthetic id >= 0 (or a stale span past a smaller table; `td5_track.c:4893-4898` records a previous 0xC0000005 of exactly this kind) reads OOB. -1 is safe. Moot if traffic is gated; real if roam traffic (F8) uses ids.
3. **Trackside replay camera, raw span table (agent, not re-read).** `td5_camera.c:4300-4309, 4423-4434, 4534-4543, 4640-4652, 4808-4816` index `g_spanTable + anchor*0x18` with no bounds; `g_spanTable` is NULL after `td5_camera_bind_track_geometry(NULL,NULL)` (`td5_track.c:4930`), so NULL deref at `InitializeTracksideCameraProfiles` (4300) if any replay profile is bound; `g_camAnchorSpan[v] = -1` (4896) reads at base-24; spline reads up to anchor+41 past the end; `s_splineTemplates[spline_type]` (6 rows) with `spline_type` an unsigned short from profile data (4482). Replay only: gate replay in roam.
4. **Unbounded vertex access.** `vertex_at` (`td5_track.c:1172`, `&s_vertex_table[index]`, **verified**, no count stored, every `!v` guard on it is dead) and `td5_track_get_vertex` (`td5_track.c:11680`, lower bound only, agent). Safe only because callers pass indices from a bounds-checked span. Callers: susp 1703-1704, 1783-1784, 1917-1918, 2011-2012; camera 2039-2042, 5039-5042; render.c 3970-3973, 2578-2579, 2645-2646, 2733-2735; game.c 9806-9807; hud.c raw `vert_base + vi*6` (4722-4733, 4600-4602, 7069+) with no vertex-count check. Synthetic spans with garbage vertex indices would be OOB.
5. **`compute_boundary_bits` (verified, `td5_track.c:5487-5490, 5510-5512`).** `sub_lane` is not clamped (a stale probe lane of +127 reads ~1 KB past the span row, heap, usually no crash), and `s_edge_mask_first/last[sp->span_type]` (arrays of 12, type is uint8) has no `<12` clamp unlike line 5481 (loader-integrity only). Same for `k_target_vertex_offsets[sp->span_type]` (8620, 8686, agent) and `1u << lane` (4365 via 5414, UB beyond 31).
6. **Legacy traffic recycle, `td5_ai_traffic.c:582` (verified by the agent's reading; I read 5685-5702 only).** `strip[(size_t)q_span*0x18+3]`, `q_span` from TRAFFIC.BUS checked only against -1. Removed by gating Dynamic=0 and the queue in roam. Also `td5_ai_traffic.c:998-1000, 1158-1163` (vertex pool indices trusted).
7. **Corrupt-file only (not roam):** `td5_track.c:4966` `(int)((vertex_offset - span_offset) / 24)`. If the offsets are unsigned the subtraction underflows (I did not check their types) and is only compared against `>= size`.

**int16 width:** `track_span_*` and probe `span_index` are `int16_t`; `(int16_t)span` writes at `td5_game.c:4533, 4684-4686, 4856`, `td5_ai_traffic.c:3750`, assists 2246-2248, 2434-2443 wrap ids above 32767.

**Silent wrong data / behavioural teleports (no crash), must be gated or redirected**
- `td5_physics_suspension.c:3781-3786`: probe span -1 -> chassis span -> **0**: wheel reads span 0's plane. `td5_track_probe_height` (7495) and `td5_track_shadow_probe_height` (7726) clamp -1 to 0 and return success.
- `td5_physics_suspension.c:2412-2423, 3730-3732, 4075-4077; td5_physics.c:5729-5730, 5778-5779`: clamps rewrite a synthetic id >= count to `count-1`.
- `td5_track_init_actor_segment_placement` (`td5_track.c:3609-3613`) ZEROES `world_pos` for an out-of-range span (callers: assists 2435 battle-only; ai_traffic 600, 676, 988, 1134, 3753).
- `td5_track_get_recovery_pose` (3461) wraps -1 modulo ring onto a real strip span.
- Forward sentinel contacts do not exit at span -1 (`td5_track.c:3232`, **verified**): with a strip loaded they fire against the strip's sentinel edge.
- `td5_physics.c:5586-5605`: collision-alignment recovery exit never passes while `heading_normal` is (0,0,0): tumble recovery stuck.
- `td5_game.c:9983`: `s_active_checkpoint.checkpoints[cp]` (array of 5) guarded only by `checkpoint_index < checkpoint_count` (other sites clamp to 5). Only matters for a record with more than 5 checkpoints.
- `td5_hud.c:8381-8387, 8495-8499` `% ring` with ring 0 possible without a strip: covered by the minimap early-out `td5_hud.c:7625-7641`; I did not trace every other caller.

**Verified guarded (do not re-audit):** `td5_track_get_span`, `span_lane_count*`, `get_span_lane_world`, `get_span_center_world`, `get_span_route_frame`, `compute_probe_contact_vertices`, `get_surface_type`, `compute_contact_height_*` (return 0 for OOR), `ComputeActorTrackContactNormal`, `td5_track_normalize_actor_wrap` at ring 0 (`td5_track.c:8388`), the V2V broadphase bucket (`td5_physics_collision.c:3634-3668`, index masked `& 255`: -1 -> bucket 0, no OOB; ids >= 1024 alias buckets), `td5_game.c:9652-9658` (circuit lap logic self-disables at `total_spans <= 0`), `td5_game_get_slot_progress` (`ring<=0 || ring>=9000` early return), all `% ring` / `% span_count` sites listed in `part_ai.md` and `part_game.md`.

## Open items / not verified
- No code was run. Nothing here was exercised in a build.
- The agents' lines for `td5_camera.c` trackside code (item 3), `hud.c` pane map vertex indexing, `td5_rt.c` window, and the whole `td5_ai.c` smart-AI block are their reading only.
- `td5_ai_update_race_actors:8748` refresh of route state for human slots: physics reads `td5_ai_get_route_state` (agent); what physics does with `RS_FORWARD_TRACK_COMP` when frozen is unchecked.
- Whether `fork_is_left` (`td5_track.c:257`) bounds its index internally: not confirmed.
- `td5_trackgen*`, `td5_tg_*`, `td5_geo_*` modules were not swept (not runtime consumers in roam).
- The counts above are regex tallies of agent tables, not a hand count.

## Questions for the next phase (F1/F2)
1. Do you want the locator to also serve **the camera** (any-XZ height/normal, provider #3/#4 plus camera.c:1351 and 1688-1695), or keep camera terrain probes gated? It is a hard dependency of chase tilt and the three eye-floor paths, and they currently trust height 0.
2. Roam locator return struct (suggested, from the table): `{edge_id, tri_id, inside, height_fp, normal[3] (int16, mag 4096, y!=0), surface_type, lateral_off, push_x, push_z, push_dist, capped}`; every B row maps onto one of those fields.
3. Decide now whether F2 keeps the **normal span-based `td5_physics_tick` order** (provider swapped in at the 8 functions) or gets a roam-specific tick. The first is much smaller: no physics file needs editing beyond the span-0 fallback at `td5_physics_suspension.c:3781-3786`.
