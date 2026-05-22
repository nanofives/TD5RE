# Session 08 — Triage older deferred items

## Goal
Close out smaller previously-deferred items via short individual investigations.
Each item gets 15-30 minutes of triage; either ship a quick fix or document the
specific reason it must remain deferred.

## Items to triage

### 8a — Checkpoint timer doesn't add time
- Per `memory/todo_checkpoint_no_time_added_2026-05-19.md`
- Suspect: `s_active_checkpoint.checkpoint_count=0` OR scale=0 from `adjust_checkpoint_timers` OR HUD reads wrong field
- Approach: add TD5_LOG_I at `td5_game.c:3893` when IS-wired, capture during a P2P race
- Check: does HUD show the bonus add (visual) or does only the underlying timer increment?

### 8b — Police chase audio doesn't trigger
- Per `memory/todo_police_chase_no_audio_2026-05-19.md`
- Suspect: `wanted_mode_enabled` never set to 1 OR siren gate at `td5_sound.c:552`
- Approach: check if user races as game_type=8 explicitly. If yes, verify `ConfigureGameTypeFlags` sets `wanted_mode_enabled=1` (td5_frontend.c:2810). If no, document as feature-gated to Cop Chase mode only.

### 8c — View Race Data flashes back
- Per `memory/todo_view_race_data_broken_2026-05-19.md`
- Suspect: state-3 early-exit gate at `td5_frontend.c:9188`
- Approach: trace s_inner_state per frame when entering View Race Data; identify which state gate kicks back.

### 8d — Edinburgh span 539 launch (re-verify)
- Per `memory/todo_edinburgh_span_539_launch_2026-05-19.md`
- Previously hypothesized to close via chassis-snap lrintf fix
- Approach: drive port to span 539, observe. If still launches, separate audit.

### 8e — Camera no slope adjustment (re-verify)
- Per `memory/todo_camera_no_slope_adjustment_2026-05-16.md`
- Hypothesized to close via chassis-snap lrintf fix
- Approach: drive port on hilly section, observe camera pitch behavior

### 8f — View Replay restarts race
- Per `memory/todo_view_replay_restarts_race_2026-05-19.md`
- Per memory `t5_global_naming_sweep`: arch-divergence (orig uses M2DX)
- Approach: confirm arch-divergence verdict; document as won't-fix.

### 8g — Slot 0 lat_offset_bias 11u residue
- From earlier session diagnosis
- Per `memory/reference_lateral_dir_inversion_fix_2026-05-21.md`
- Hypothesized to close via session 03 (RNG alignment)
- Approach: re-verify after session 03 ships. If still 11u, separate audit.

### 8h — Traffic clipping ground / not moving in Edinburgh
- Per user feedback from earlier batch
- Phase 3 fix shipped (traffic cardef seed). User confirms STILL clipping.
- Approach: capture per-tick traffic state log (world_y, brake_flag, surface_contact) and compare against orig probe (already captured: countdown shows y=256, brake=0, surface_contact=0). Find where port diverges.

## Tools

- Per-item: short TD5_LOG_I instrumentation
- Or Frida probe for the relevant orig RVA

## Success criteria

Each item ends in one of:
- SHIPPED (code change + verified)
- VERIFIED-CLOSED (no longer reproduces; prior fix held)
- DOCUMENTED-DEFERRED (specific block reason; e.g. M2DX arch-divergence)
- LARGER-SCOPE (extracted into its own session brief)

## Estimated time
1-2 hours combined.
