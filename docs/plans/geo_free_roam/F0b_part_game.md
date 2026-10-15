# Part: td5_game.c / frontend / selftest / input / asset (agent aac38c40 report, spot-verified)

Verdicts: A works unchanged if span=-1/synthetic, B locator, C gate off in roam, D rewrite for route/graph (F5-F8).
Paths under `td5mod/src/td5re/`.

## Key findings
1. **Spawn silently skips the actor when the strip has no spans.** `init_race_spawn_actors` (`td5_game.c:4044`): `track_span_count==0` gives `span_index=1`, `td5_track_get_span(1)` returns NULL (`td5_track.c:11673`), `td5_game.c:4607-4608` does `continue`. The actor is never placed (slot_index unset, world_pos origin, no reset, no AI seed). Documented symptom at `td5_asset.c:2434-2439`.
2. **Missing STRIP.DAT leaves a stale `g_td5.track_span_ring_length`.** `td5_asset.c:2478-2480` calls `td5_track_load_strip(NULL,0)` (`td5_track.c:4921-4938`), zeroing `s_span_count` and `s_strip_header` but NOT the ring length (only written at `td5_track.c:4983`, `:4770`). Roam must set it to 0 explicitly (never -1).
3. **The per-tick wrap normalizer clobbers a synthetic edge id.** `td5_track_normalize_actor_wrap` (`td5_track.c:8362`), called from `td5_game.c:7700` (countdown) and `:7896` (live) for every actor, guards only `ring_length == 0` (`td5_track.c:8388`). With a stale positive ring it rewrites `+0x82` from `+0x84` every tick. Divides by ring (8425, 8438-8448). Gate it in roam or keep ring at 0.
4. **Circuit lap logic self-disables when ring <= 0:** `advance_pending_finish_state` returns at `td5_game.c:9656-9658`. Safe, and no finish ever fires.
5. **Checkpoints would load for a roam track slot:** `init_race_checkpoints` (`td5_game.c:5237`) keys on `g_td5.track_index`; slots < 19 load a stale record (`:5264-5295`) that arms `tick_pending_finish_timer` and the P2P path (timer expiry marks every slot DNF). Force `checkpoint_count=0`, `g_special_encounter=0`.
6. `build_results_table` (`td5_game.c:10228-10231`) synthesizes finish times from `rand()` for unfinished racers: gate in roam.

## Tick order (`td5_game_run_race_frame` 8748)
Per frame: `frame_check_completion` (6590) -> `advance_pending_finish_state` per racer (6611, lap/checkpoint/finish detector, per FRAME not per tick) + `check_race_completion` (6629).
`frame_run_sim_loop` (6725), 30 Hz: input -> `td5_ai_tick` (7771) -> `td5_physics_tick` (7811; walker + probes inside) -> metrics -> `td5_vfx_tick` -> `td5_vfx_update_ambient_density` (reads +0x80) -> `update_race_order` (7867) -> camera solve -> `td5_track_normalize_actor_wrap` loop (7885-7899) -> per-slot timers/metrics (7924-7937) -> `battle_chase_tick`, `td5_chaos_tick`. Countdown branch (7415-7704) runs the same set with paused physics steps.
Tick-level functions that assume a strip: `td5_ai_tick`, `td5_physics_tick` (track update + probes), `td5_track_normalize_actor_wrap`, `update_race_order`, `advance_pending_finish_state`, `tick_pending_finish_timer`, `battle_chase_tick`, `td5_vfx_update_ambient_density`, camera terrain probes.

## Spawn: minimum a roam spawner must do (per slot)
zero actor memory; set `slot_index`; XZ in 24.8; heading (`euler_accum.yaw`); `td5_physics_reset_actor_state` so Y is snapped (sentinel `world_pos.y=0xC0000000`, `td5_game.c:4728`); seed AI progress (or skip AI); write span fields (-1 or edge id) in `+0x80/84/86`, sub-lane `+0x8C`.

## Consumers: td5_game.c
| file:function:line | reads | when | verdict | note |
|---|---|---|---|---|
| td5_game.c:2753-2756 init_race_modes_and_seed | start_span_offset=150 (battle) | init | C | battle only |
| 2901, 3507-3508, 3539 | g_track_is_circuit, g_special_encounter | init | C | set g_special_encounter=0 |
| 4044-4890 init_race_spawn_actors | strip spans, lane vertices, track_span_count, start_span | init | B + D | finding 1 |
| 4230-4330 | span count, start-span tables | init | C | level_num 1..39 guarded (4233) |
| 4408 | writes track_start_span_index | init | C | read only by 9653 |
| 4410-4472 | TD6 finish/checkpoint spans, ring length | init | C | self-guarded |
| 4591-4600 | span_index % ringw | init, circuit | C | ringw guarded |
| 4602-4608 | td5_track_get_span | init | B | NULL => continue |
| 4621-4697 | lane count, td6 road band, grid main lane | init | C | bounds-checked in td5_track.c |
| 4684-4686, 4707, 4856 | writes +0x80/84/86/8C | init | B | int16/uint8 truncation (big edge id wraps) |
| 4712-4728 | td5_track_get_span_lane_world | init | B (height) | 0 out of range; fallback reads sp->origin |
| 4741 | td5_track_compute_heading | init | B | early return if !s_span_array (td5_track.c:7902) |
| 4863 | td5_ai_seed_actor_track_progress_offset | init | C or D | strip-based |
| 5237-5346 init_race_checkpoints | track_index, k_checkpoint_table | init | C | stale record (finding 5) |
| 5312-5342 | drag finish span, ring | init, drag | C | cap guarded ring>4 |
| 5376-5567 | LEVELINF checkpoint spans, TD6 timer | init | C | NULL guarded |
| 5580-5600 | adjust_checkpoint_timers / timer 0x7FFF | init | C | |
| 5749-5764 trace_stage_impl | +0x82 vs max_span | per trace stage | C | read-only |
| 5843-5863 trace track row | +0x80..86, get_span_route_frame | per trace stage | A | guarded |
| 7700, 7896 normalize_actor_wrap | +0x80/82/84, ring | per tick | C | finding 3 |
| 7598, 7867 update_race_order (10646) | +0x86 high-water, checkpoint_index | per tick | A inert / D | all tie at -1; position stuck, no index risk |
| 7933 tick_pending_finish_timer (9435) | checkpoint_count, timer_ticks, special_encounter | per tick | C | early returns |
| 6611 advance_pending_finish_state (9551) | +0x82, +0x84, track_start, ring, checkpoints | per frame | D | |
| 9585-9623 | +0x84, s_td6_cp_spans[] | per frame | C | cp_index guarded |
| 9652-9658 | total_spans<=0 early return | per frame | A | |
| 9676-9683 | lap_split_times[lap] | per frame | C | lap<9 |
| 9723-9740 | % total_spans | per frame | C | unreachable at total_spans<=0 |
| 9802-9827 | get_span/get_vertex(track_span_raw,+1) | on finish | B | NULL-checked |
| 9870-9957 | sector bitmask, s_td6_finish_span | per frame | C | |
| 9971-10039 | LEVELINF checkpoints[cp] | per frame | C | cp guarded by checkpoint_count only (array of 5) |
| 10030 | (actor_span*1500)/average_speed_raw | on finish | C | guarded >0 |
| 10101 accumulate_speed_bonus | +0x82 < +0x86 | per tick | A | |
| 10150-10205 | table | init | C | |
| 10215-10231 build_results_table | post_finish_metric_base | end | C | synthesizes times |
| 10536-10574 drag length fns | ring | init, drag | C | ring<16 => 300 |
| 10786 td5_game_force_finish_race | display_position | event | A | |
| 12255-12333 battle_chase_tick / battle_racer_progress | +0x82, span count | per tick, battle | C | guarded |
| 1829 td5_game_get_slot_span | +0x82 | accessor | A | returns -1; hud/camera:2847/trackgen_stream:257 readers outside this audit; td5_fe_race clamps negatives |
| 1877-1890 td5_game_get_slot_progress | +0x82, ring, lap | accessor | A, D | ring<=0 returns folded span; constant => selftest "no progress" WARN |
| 1820 td5_game_get_player_lap | checkpoint_index | accessor | A | |
| 903-926 minimap checkpoint accessors | s_active_checkpoint, s_td6_cp_spans | per HUD frame | C | clamped to 5 |
| 946-999 track_checkpoint_record, checkpoint_is_tick | k_checkpoint_table | frontend preview | C | record_idx 0..39 |
| 8311, 8430 | +0x80 to debug_emit_collision_lines | dev | A | callee guards -1 |
| 12762 update_split_screen_balance | track_span_accumulated | per frame, split | A, B | |

## Consumers: other modules
| file:function:line | reads | when | verdict | note |
|---|---|---|---|---|
| td5_selftest.c:1010 st_invariants_sample | td5_game_get_slot_progress | per sample | A | "no progress" check (1029) is WARN-only; constant span => WARN |
| td5_selftest.c:1014-1015 | get_slot_span(0) as peak_span | per sample | A | telemetry |
| td5_selftest.c:668, 1725-1730, 2260 | start_span_offset, dbg_end_checkpoint, laps | scenario setup | C | roam scenarios need their own |
| td5_fe_race.c:10599-10790 | span count, slot span, lap | results screen | C | divisions guarded, negatives clamped to 0 |
| td5_fe_race.c:8604-8664 | grid_span, cp_span, finish_span | studio only | C | guarded |
| td5_fe_race.c:7476-7520, 1397-1725, 9649-9683; td5_frontend.c:5422-5523, 6838-7554, 8621-8991, 9845 | per-level tables, laps, checkpoint timers | menu | C | |
| td5_input.c:982-983 | dev_gap/median_drive_bits | per tick, dev, env | C | guard span<0/>=count/ring<=0 |
| td5_input.c:2270-2272 | +0x80 vs auto_throttle_stop_span | per tick, dev | C | acts only when stop_span>0 |
| td5_input.c:2887-3124 | start_span_offset in replay header | record/replay | C | |
| td5_asset.c:2476-2494 | STRIP/LEFT/RIGHT.TRK load | level load | C | finding 2 |
| td5_asset.c:2529-2545 | LEVELINF track_type, environment config | level load | C | missing file defaults to CIRCUIT |
| td5_asset.c:2611-2780 | CHECKPT.NUM remap, reverse texture swap | level load | C | keep reverse_direction=0 |
| td5_asset.c:2635 | td5_track_bind_boundary_sentinels | level load | C | |
| td5_asset.c:4670-4800, 1498-1650, 2336-2371 | pool/checkpoint resolvers, per-level tables | init/menu | C | pool_row bound-checked |
| td5_assetsrc.c:141-149, 372-707 | LEVELINF names, TRK CSV, STRIP JSON encode | build/convert | C | link guarded (523) |
| td5_control.c:311-322 | lap, slot_span, slot_progress | JSON request | A | reports -1 |
| td5_trace.c:48-54, 410-440 | actor +0x80..86 | per trace stage | A | read-only |
| td5_fe_geo.c:186, 329-1919 | s_spans (route span count display) | UI | A | display |
| td5re.c:51-52 | td5_track_init, td5_track_registry_init | init | A | keep |
| main.c:947-1763; td5_save.c (several) | INI keys Laps/MaxSpan/StartSpanOffset..., checkpoint_mode | boot, save | C | config plumbing |
No span/ring/checkpoint consumers in td5_inputscript.c, td5_benchmark.c, td5_tutorial.c, td5_pending.c.

## Crash-risk (this part)
No unguarded array index driven by span/ring/checkpoint turned up. Hazards are stale or inconsistent state, not missing bounds checks:
- `td5_game.c:4602-4608` NULL span => actor unplaced (not a crash).
- `td5_game.c:7700, 7896` + `td5_track.c:8388, 8438-8450`: `raw_span / ring_length`, `% ring_length` guarded only for ring==0. Negative ring unguarded; NULL strip leaves the stale ring.
- `td5_game.c:4533, 4684-4686, 4856`: int16 truncation of a big synthetic edge id wraps.
- `td5_game.c:9983`: `s_active_checkpoint.checkpoints[cp]` (array of 5) guarded by `checkpoint_index < checkpoint_count` only; other sites clamp to 5 (914-915, 970, 5299, 10191), this one does not.
Roam setup must set: ring length 0, track_start_span_index 0/-1, checkpoint_count 0, g_special_encounter 0, s_levelinf_checkpoint_config 0, s_td6_cp_count 0, s_td6_finish_span 0, reverse_direction 0.
