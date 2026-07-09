# Session 01 — AI stuck at Edinburgh span 137-147

## Status: CLOSED 2026-05-22 (success criteria met)

Root cause: `td5_input_update_player_control(0)` ran for slot 0 under PlayerIsAI=1 and wrote `s_steering_cmd[0]=0` (no keyboard) into actor+0x30C every tick. The AI cascade in `td5_ai_update_steering_bias` is additive (`param_2 = ACTOR_STEERING_CMD + delta`), so resetting the field each tick pinned the output at exactly `delta` — for the emergency-snap branch (LEFT≥0x800 AND RIGHT≥0x401) that's -0x4000 = -16384. The ±0x18000 clamp never engaged because the value never accumulated.

Fix: `td5mod/src/td5re/td5_input.c:925` — skip the +0x30C writeback when `slot == 0 && g_td5.ini.player_is_ai`. Other field writes (+0x33E throttle, +0x36D brake, +0x378 auto-gearbox) preserved because AI overwrites those as absolute values after input runs.

Result on Edinburgh 2000-tick PlayerIsAI=1 race:
- before: span locked at 147 from tick 440-1840
- after: span 62→99→117→stalls 440-1200→125→175→213→**233** at tick 2000

Pre/post-AI trace confirms the AI value now persists across ticks (pre_physics value matches previous post_ai). See `memory/reference_playerisai_input_steering_overwrite_2026-05-22.md`.

Residual: post-440 stall (~800 ticks before recovery) is a different mechanism — likely wall-stick recovery with saturated steering at -98304. Separate follow-up needed for "span 400+" bonus criterion.

---

## Goal
Make port AI advance past Edinburgh span 200 in a 2000-tick `PlayerIsAI=1` race
(orig reaches span 689; current port stops at span 147 with steer locked at
-16384 from sim_tick 440 onwards).

## Context — read these first

- `memory/reference_root_cause_chain_2026-05-22.md` — confirms this is SEPARATE from countdown contact_delta cascade
- `memory/reference_fix_10_items_2026-05-22.md` (task #30 outcome)
- `memory/reference_pool13_dynamic_diff_2026-05-21.md` — pool13 paired-diff workflow
- `memory/reference_lateral_dir_inversion_fix_2026-05-21.md` — find_offset_peer sign fix already shipped

What's already shipped (don't redo): classify v1→v2, sign inversion, wanted_damage gate, fwd_track_comp sqrt-divisor, MSVC rand override, chassis-snap lrintf, countdown contact_delta gate, HUD glyph capacity, path correction in steering.

## Observable symptom

Port `td5re.exe --DefaultTrack=1 --DefaultCar=0 --PlayerIsAI=1 --RaceTrace=1 --RaceTraceMaxSimTicks=2000 --TraceFastForward=5`:
- tick 240 span=108 (advancing normally)
- tick 440 span=136 steer=-16384 (LOCKED HERE)
- tick 1840 span=147 steer=-16384 (never advances past 147)

Slot 0 (PlayerIsAI=1) — log shows `recovery: slot=N` only for slots 1/3/4/5 NOT slot 0. So slot 0's stuck steering is from a different writer than the recovery script.

## Approach

1. **Add port-side per-tick trace** for slot 0:
   - In `td5_ai_update_actor_track_behavior` (td5_ai.c around the steering compute), emit a log line tagged `STEER_TRACE` capturing every contributor to the final steering_cmd: route heading, lateral_offset_bias (RS[9]), peer-push sources, target_angle, the final write to actor+0x30C.
   - Gate the log to slot==0 and span_raw in [130, 200] to limit volume.

2. **Build + run port** with `--RaceTraceMaxSimTicks=1000`.

3. **Identify which AI sub-module is writing -16384**:
   - If it's `td5_ai_find_offset_peer` peer-push → check the peer it picks at span 137
   - If it's the look-ahead waypoint sampling → check `td5_track_sample_target_point` output at span 137
   - If it's the heading-error PID → check route_heading at span 137

4. **Cross-reference with orig** using `tools/_probes/extended_physics_probe.js` extended with steering_cmd writebacks (currently captures the field but not the WRITER). Add a Frida hook on orig `0x00434FE0` (UpdateActorTrackBehavior) onLeave to log the same fields.

5. **Apply fix** based on whichever sub-module diverges.

## Tools

- Ghidra MCP: `bash scripts/ghidra_pool.sh acquire`, open `TD5_pool11` read-only
- Port instrumentation: `TD5_LOG_I("ai", ...)` (logs to `log/race.log`)
- Frida probes via `tools/_probes/` + `tools/orig_probe_queue.py`

## Success criteria

Port 2000-tick AI race reaches span 200+ (any advance past 147 is progress).
Bonus: span 400+ (matches orig closely).

## Files likely touched

- `td5mod/src/td5re/td5_ai.c` (the AI dispatch logic for slot 0)
- `tools/_probes/ai_steering_trace_probe.js` (new probe to write)
- Update `td5_pilot_trace_00436A70.c` extended physics CSV to also capture steering_cmd already done — verify columns are present.

## Risk
LOW. Investigation work; fix scope depends on findings. May need recursion if root is further upstream.

## Estimated time
1-2 hours active work.
