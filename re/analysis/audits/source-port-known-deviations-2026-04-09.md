# Source Port Known Deviations — 2026-04-09

Purpose: keep intentional approximations, placeholders, disabled behaviors, and
ownership mismatches in one place so they are not mistaken for newly discovered bugs.

## Race Timing / Flow

### 1. Timing-unit ambiguity in `td5_game.c`

- Location:
  - `td5mod/src/td5re/td5_game.c:2190-2222`
- Status: source contract cleaned up, trace still diverges from sim tick 1
- Why it exists:
  - the source recently switched `normalized_frame_dt` and `sim_tick_budget` to explicit 30 Hz normalized semantics, but the original and source traces still disagree immediately on `sim_accum`, `sim_budget`, `frame_dt`, `instant_fps`, and `ticks_this_frame`
- Risk:
  - any remaining caller or trace emission point that still assumes a different frame/tick contract will drift quietly and make later gameplay diagnosis harder
- Exit condition:
  - confirm standing-start and lap-transition traces no longer diverge first on timing state
  - see `re/analysis/race-ticket-validation-2026-04-09.md`

### 2. Folded camera ownership inside the race tick

- Location:
  - `td5mod/src/td5re/td5_game.c:1238-1242`
  - `td5mod/src/td5re/td5_camera.c`
- Status: active source-model deviation
- Why it exists:
  - source folded cache/update camera responsibilities into `td5_camera_tick()`
- Risk:
  - stage-order drift during collision, countdown, and split-screen transitions
- Exit condition:
  - trace-confirm exact binary stage order and re-split only if the drift is real

## Race Progression

### 3. Duplicated race progression ownership

- Location:
  - `td5mod/src/td5re/td5_game.c:1764-1864`
  - `td5mod/src/td5re/td5_track.c:2338-2475`
- Status: source ownership unified in `td5_game.c`, checkpoint/finish-state trace fields now align, span drift still present
- Why it exists:
  - the old duplication came from parallel source paths; the live lap/progression mutation now stays in `td5_game.c`
- Risk:
  - the surviving game-side circuit model is still simpler than the stronger start-line/heading helper that used to exist in `td5_track.c`, and span motion begins drifting from the original trace around shared sim tick `75`
- Exit condition:
  - validate lap/checkpoint transitions and wrong-way recovery with the trace harness
  - see `re/analysis/race-ticket-validation-2026-04-09.md`

### 4. Point-to-point checkpoint progression is still proxy logic

- Location:
  - `td5mod/src/td5re/td5_track.c` checkpoint path
- Status: known incomplete
- Why it exists:
  - source currently uses a simplified progression proxy
- Risk:
  - checkpoint timing/finish bugs on point-to-point tracks
- Exit condition:
  - replace with fully data-driven checkpoint progression

## Race Ordering

### 5. Placeholder race-order path still exists in track module

- Location:
  - `td5mod/src/td5re/td5_game.c`
  - `td5mod/src/td5re/td5_track.c`
- Status: source ownership unified in `td5_game.c`
- Why it exists:
  - the old track-side placeholder owner was removed; race-order writes now happen in `update_race_order()` and are mirrored back into actor state
- Risk:
  - remaining risk is behavioral, not structural: `metric_display_pos` and `race_pos` still diverge significantly from the original trace even though there is only one live owner now
- Exit condition:
  - verify `race_pos` and display ordering across overtake and lap-transition scenarios
  - see `re/analysis/race-ticket-validation-2026-04-09.md`

### 6. Trace schema still produces ambiguous countdown/render-only comparisons

- Location:
  - `td5mod/src/td5re/td5_trace.c`
  - `td5mod/src/td5re/td5_game.c`
- Status: active validation tooling limitation
- Why it exists:
  - the current trace key is still based on `frame`, `sim_tick`, `stage`, `kind`, and `id`, which is not enough to uniquely distinguish repeated countdown or render-only emissions
- Risk:
  - raw comparator output becomes noisy and can hide the first real gameplay divergence
- Exit condition:
  - add a per-emission sequence or reduce validation to one canonical stage per sim tick
  - see `re/analysis/race-ticket-validation-2026-04-09.md`

## Physics / Vehicle Dynamics

### 7. Suspension integrator uses a stable substitute, not the original math

- Location:
  - `td5mod/src/td5re/td5_physics.c`
- Status: intentional approximation
- Why it exists:
  - original XZ projection / coupling path was not fully recovered
- Risk:
  - “physics feel” drift, especially over curbs, crests, and landings
- Exit condition:
  - reconstruct original suspension force projection and validate with trace scenarios

### 8. Suspension-to-attitude torque coupling is disabled

- Location:
  - `td5mod/src/td5re/td5_physics.c`
- Status: intentional disable
- Why it exists:
  - current simplified suspension path became unstable when coupled directly
- Risk:
  - reduced pitch/roll fidelity and landing behavior drift
- Exit condition:
  - restore only after suspension force path is validated

### 9. Recovery / out-of-bounds behavior is disabled or clamped

- Location:
  - `td5mod/src/td5re/td5_physics.c`
- Status: intentional disable
- Why it exists:
  - current source favors stability over incomplete recovery semantics
- Risk:
  - large incident behavior differs from the binary
- Exit condition:
  - isolate exact recovery entry/exit behavior with deterministic traces

### 10. Trig / angle helpers in some source paths are still approximation-heavy

- Location:
  - `td5mod/src/td5re/td5_physics.c`
  - `td5mod/src/td5re/td5_track.c`
  - `td5mod/src/td5re/td5_camera.c`
- Status: active fidelity risk
- Why it exists:
  - host `sin/cos/atan2` replacements were used for stability or convenience
- Risk:
  - quantization drift in steering, wall response, AI heading, and camera interpolation
- Exit condition:
  - restore or emulate the original LUT/quantized path consistently

## AI / Traffic

### 11. Route/script heading logic is simplified

- Location:
  - `td5mod/src/td5re/td5_ai.c`
- Status: known simplification
- Why it exists:
  - route-state semantics are partially reconstructed
- Risk:
  - lane-change, recovery, and encounter steering drift
- Exit condition:
  - direct binary/source comparison on scripted AI events

## FMV / Platform

### 12. FMV path remains a source-port stub boundary

- Location:
  - `td5mod/src/td5re/td5_fmv.c`
- Status: intentional replacement
- Why it exists:
  - full TGQ runtime parity is not a current gameplay blocker
- Risk:
  - FMV behavior is functionally present but not source-identical
- Exit condition:
  - only if FMV parity becomes a project goal

## Use

Before filing a new “bug” or “wrong RE” issue:

1. Check whether the behavior is already covered here.
2. If yes, classify it as:
   - intentional approximation
   - placeholder
   - ownership contradiction
   - unresolved fidelity gap
3. Only then decide whether to fix, defer, or instrument first.
