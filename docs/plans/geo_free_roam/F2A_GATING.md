# F2-A: ROAM mode entry, session and gating (what was gated, what was guarded, what is left)

Task F2-A of GEO FREE ROAM. Code: `td5_roam_mode.c/.h` (session), plus the gates listed below.
Source of the consumer list: `F0b_REPORT.md` and `F0b_part_*.md`. Verdict C = "gate off in roam".

## Mode life cycle

`td5_roam_begin()` ARMS (geo map FREE ROAM button, or dev CLI). `td5_roam_active()` is 1 from
`td5_roam_session_start()` (top of `td5_game_init_race_session`) until `td5_roam_end()`. The geo map
screen init calls `td5_roam_cancel()` so a stale armed session can never turn an ordinary race into
a roam race.

Entry: geo map -> FREE ROAM (enabled when the START pin lies in a built roam tile) ->
`frontend_roam_prepare_car_select()` -> normal CAR_SELECTION -> OK launches (no track select, no race
options; `s_selected_track = td5_roam_host_track()`), BACK / ESC returns to the map (parent screen
of CAR_SELECTION while armed).
Exit: pause row QUIT TO MENU is relabelled LEAVE FREE ROAM (also its confirm text) -> result 2 ->
`td5_roam_end()` -> geo map (no results screen). END RACE NOW is hidden in roam (both the row math in
`td5_game.c` and the draw in `td5_hud.c`).

Host level: roam still wants a level for sky, static pages and LEVELINF. Default schedule index 0
(Moscow), `TD5RE_ROAM_HOST_TRACK` overrides. Its STRIP / LEFT / RIGHT / MODELS / CHECKPT / TRAFFIC.BUS
are NOT loaded in roam (`td5_asset.c`), `td5_track_load_routes(NULL..)` drops the previous race's
route tables, boundary sentinels are bound to level 25 (`{-1, 9999}`: both handlers off, not a registry
level).

World origin: `roam_grid_origin(grid, tile_of(spawn))`, the (0,0) corner of the lattice tile holding
the spawn point, fixed for the session. Actor `world_pos` = `FP_SCALE(place - origin)`.

## Verdict-C consumers: status

| Consumer (F0b) | Status | How |
|---|---|---|
| `td5_ai_tick` (rubber band, dynamic traffic tick, cop scheduler, phantom-peer search, `update_race_actors`) | GATED | both call sites `if (!td5_roam_active())` (`td5_game.c`) |
| `td5_ai_init_traffic_actors` / dynamic traffic init | INERT | one racer, `traffic_enabled=0`: `racer_count <= traffic_slot_base` returns |
| `td5_ai_recycle_traffic_actor` :582 (crash risk 6) | GUARDED | `q_span` bounded by `td5_track_get_span_count()` |
| `td5_ai_update_traffic_route_plan` :5698 (crash risk 2) | GUARDED | `td5_ai_route_span_valid()` |
| `td5_compute_heading_delta` camera.c:5727 (crash risk 1) | GUARDED | `td5_ai_route_span_valid()` -> 0 |
| trackside replay camera (crash risk 3) | GATED | `td5_camera_replay_trackside_ready()` = 0 in roam (replay falls back to chase), `td5_camera_bind_trackside_profiles(0)` at race init so `InitializeTracksideCameraProfiles` never reads `g_spanTable` |
| `vertex_at` / `td5_track_get_vertex` (crash risk 4) | PARTIAL | `vertex_at` returns NULL with no vertex table (the many `!v` guards are live). No UPPER bound: callers still pass indices from a bounds-checked span, and no span exists in roam |
| `compute_boundary_bits` sub_lane / span_type (crash risk 5) | NOT NEEDED | unreachable with span count 0 (`get_span` returns NULL first). Not changed |
| `init_race_modes_and_seed` start span offset, `g_special_encounter` | GATED | `g_special_encounter = 0`; battle offset is battle-only |
| `init_race_checkpoints` (checkpoint table, LEVELINF spans, TD6 timers, P2P timer) | GATED | roam zeroes every record, timers 0x7FFF, returns early |
| LEVELINF `track_type` (circuit flag would arm laps) | GATED | forced `TD5_TRACK_POINT_TO_POINT` after the host level load |
| `td5_track_normalize_actor_wrap` (rewrites +0x82 from +0x84 each tick) | GATED | `norm_count = 0` at both call sites; ring stays 0 (`apply_overrides`) |
| `tick_pending_finish_timer`, `frame_check_completion` (lap/finish detector, results) | GATED | roam returns / skips |
| `update_race_order`, `td5_vfx_update_ambient_density` | GATED | skipped in roam |
| `build_results_table` (invents finish times) | UNREACHABLE | roam has no finish and no results screen |
| `td5_asset.c` STRIP / LEFT / RIGHT / MODELS / CHECKPT / TRAFFIC.BUS / stream begin | GATED | `roam_host` branch |
| HUD flags (position, laps, timers, metric digits, U-turn warning), minimap, street label | GATED | speedometer only; `td5_hud_render_minimap` and `td5_hud_draw_street_lines` early-out |
| spawn (silently skips the actor when the strip has no spans) | REPLACED | `td5_roam_session_spawn` (after `td5_ai_init_race_actor_runtime`), span fields written -1 afterwards |
| countdown | GATED | `reset_race_countdown` returns unpaused, no fly-in |

## Not done here (left for B / C / later)

- **Camera ground probe** (F0b silent item 2): with no strip `compute_contact_height_bestlane(-1)` returns
  0 and the chase eye floor sits near y = 24 wu. It needs the locator (task B contact provider) to be
  correct. Until then the roam camera is wrong on a hill.
- **Probe-height clamp to span 0** (`td5_physics_suspension.c:3781-3786`, `td5_track_probe_height`,
  `td5_track_shadow_probe_height`): belongs to the physics provider (task B).
- **`td5_render_fallback_strip_ribbon` loop (~513 iterations/frame at span count 0) and the actor span cull
  / window math**: render side (task C). Measured here: `v_track` averages 0.0 ms in a 60 s roam drive
  (`PROFILE ... v_track=0.0/1.0`), so it is not a cost problem today.
- Light zones fall back to static light, `td5_render_update_projection_effect` picks zone 0's environs
  page (cosmetic), weather comes from the geo options only when F3+ wires it.
- A roam selftest scenario (the invariant checker's "no progress" WARN needs a distance based progress).
- Yaw along the nearest street needs the resident locator (`td5_roam_set_resident`). Without it the car
  starts with yaw 0.
