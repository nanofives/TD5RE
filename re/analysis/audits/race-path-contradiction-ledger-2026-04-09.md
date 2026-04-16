# Race Path Contradiction Ledger — 2026-04-09

Target: source-side mismatches between the current port and the documented binary model for the racing pipeline.

Scope:
- `td5_game.c`
- `td5_physics.c`
- `td5_track.c`
- `td5_ai.c`
- `td5_camera.c`
- `re/analysis/main-game-loop-decomposition.md`
- `re/analysis/race-frame-hook-points.md`
- `re/analysis/race-progression-system.md`
- `re/analysis/vehicle-dynamics-complete.md`
- `re/analysis/ai-system-deep-dive.md`
- `re/analysis/collision-system.md`
- `re/analysis/camera-and-math-utilities.md`
- `re/analysis/tick-rate-dependent-constants.md`

## Highest Risk

### 1. Frame timing units are not aligned with the documented binary model

Binary model:
- `tick-rate-dependent-constants.md` documents `g_normalizedFrameDt = elapsed_ms * 0.001 * 30.0`.
- `race-progression-system.md` describes finish cooldown accumulation in simulation-time units via `param_1`.

Current source:
- `td5_game.c:2201-2213` sets `g_td5.normalized_frame_dt = delta_ms / 1000.0f`, which is wall-clock seconds, not 30 Hz normalized units.
- `td5_game.c:2206-2208` accumulates `sim_tick_budget` in those same seconds.
- `td5_game.c:1180-1181` and `td5_game.c:2123-2124` pass `normalized_frame_dt * 65536.0f` into race-completion logic.
- `td5_game.c:1518` passes `g_td5.sim_tick_budget` to `td5_hud_render_overlays`, while `td5_hud.c:1692` treats the parameter as a frame-dt-style scalar.

Why it matters:
- Any code that expects the original 30 Hz normalized timebase will drift.
- Cooldown, fade, HUD pulse, replay/network frame timing, and trace comparisons all become harder to reason about because the accumulator and the `normalized_frame_dt` field are on different scales.

### 2. The port currently has two competing circuit/lap completion models

Documented binary model:
- `race-progression-system.md` places lap/checkpoint completion in `CheckRaceCompletionState` with sector-bitmask logic and two-phase finish detection.

Current source:
- `td5_game.c:1377-1387` calls `td5_track_normalize_actor_wrap()` and `td5_track_update_circuit_lap()` every tick.
- `td5_game.c:1390-1395` then also runs `advance_pending_finish_state()` every tick.
- `td5_game.c:1764-1868` implements one lap/checkpoint/finish path in `advance_pending_finish_state()`.
- `td5_track.c:2338-2475` implements a second, different circuit-lap path in `td5_track_update_circuit_lap()`.

Behavioral mismatch:
- `advance_pending_finish_state()` uses a simplified "sector center within +/-1 span" model and directly sets finish state.
- `td5_track_update_circuit_lap()` uses start-line gating, heading checks, wrong-way reset, and cooldown.
- Both functions are active in the live tick loop, so race completion semantics are split between two non-identical implementations.

Why it matters:
- This is a direct source of race-progression bugs, duplicated state mutation, and hard-to-reproduce finish/lap errors.

### 3. Race-order ownership is duplicated, and one of the copies is still a placeholder

Documented binary model:
- `race-progression-system.md` describes a single `UpdateRaceOrder` path that bubble-sorts by forward progress and writes display positions.

Current source:
- `td5_game.c:1371-1372` calls the local `update_race_order()`.
- `td5_game.c:2075-2116` contains a real implementation based on `s_metrics[].normalized_span`.
- `td5_track.c:3664-3667` also calls `td5_track_update_race_order()` every tick.
- `td5_track.c:2797-2855` is still placeholder logic: it nulls actor bases, reads no actor data, and never writes actor positions back.

Why it matters:
- Source ownership of race ordering is not cleanly mapped.
- The live game loop calls both the real implementation and the placeholder path.
- This creates RE noise and makes it harder to trust track-module behavior during audits.

## Medium Risk

### 4. `td5_track_tick()` is documented as `UpdateRaceActors`, but the actual source split says otherwise

Documented binary model:
- `race-frame-hook-points.md` and `collision-system.md` describe `UpdateRaceActors` as the main per-tick gameplay dispatcher before global V2V collision.

Current source:
- `td5_ai.c:1873-1972` owns the real "master dispatcher" behavior for AI racers.
- `td5_track.c:3658-3667` still claims `td5_track_tick()` corresponds to `UpdateRaceActors`, but it only calls placeholder race ordering and traffic recycling.

Why it matters:
- The docs and module ownership are out of sync.
- This does not necessarily change runtime behavior, but it actively misleads further RE work.

### 5. Circuit finish semantics in `advance_pending_finish_state()` are materially looser than the RE notes

Documented binary model:
- `race-progression-system.md` describes ordered sector thresholds and finish completion through the race-completion path.

Current source:
- `td5_game.c:1787-1803` marks sectors when `abs(normalized_span - sector_center) <= 1`.
- `td5_game.c:1801-1818` immediately converts `0x0F` sector bitmask to lap completion and finish state.

Why it matters:
- The source path is simpler than the documented binary behavior.
- Even if functionally "close", this is a known bug attractor around start/finish, replay, reverse tracks, and edge-of-sector crossings.

### 6. AI track-script steering still contains simplified route-heading logic

Documented binary model:
- `ai-system-deep-dive.md` describes route selection, target sampling, heading delta calculation, and script-driven alignment using route-state data.

Current source:
- `td5_ai.c:1049` uses `int32_t route_heading = rs[RS_FORWARD_TRACK_COMP]; /* simplified */`.

Why it matters:
- The script VM's turn-alignment path is explicitly marked simplified.
- This is likely to affect AI recovery turns, lane changes, and scripted encounter behavior.

## Lower Risk But Still Worth Tracking

### 7. HUD delta-time call site is inconsistent with the HUD module’s own entry point

Current source:
- `td5_game.c:1518` calls `td5_hud_render_overlays(g_td5.sim_tick_budget)`.
- `td5_hud.c:2663-2665` calls the same function with `g_td5.normalized_frame_dt`.

Why it matters:
- Even if only one caller is live, the interface contract is inconsistent.
- This should be normalized before trace-based bug hunting expands further.

### 8. The race-frame hook map no longer matches the source’s camera ownership split one-for-one

Documented binary model:
- `race-frame-hook-points.md` describes `CacheVehicleCameraAngles`, then gameplay, then `UpdateChaseCamera`.

Current source:
- `td5_game.c:1237-1242` calls `td5_camera_tick()` near the top of each tick.
- `td5_camera.c:1737-1754` states that `td5_camera_tick()` intentionally folds `CacheVehicleCameraAngles + UpdateChaseCamera` into one source-level tick function.

Why it matters:
- This is not automatically a bug, but the mapping from original hook sites to source responsibilities is no longer one-to-one.
- Any future differential instrumentation must treat camera state as a folded subsystem, not as separate top-level calls.

## Priority Follow-up

1. Unify race progression so only one circuit/checkpoint completion path is authoritative.
2. Reconcile the timing model: either restore 30 Hz normalized `normalized_frame_dt`, or rename the field and stop feeding it to code that assumes original units.
3. Remove or quarantine placeholder track-side race-order logic from the live tick path.
4. Replace stale module comments so the docs and source split describe the same ownership model.
