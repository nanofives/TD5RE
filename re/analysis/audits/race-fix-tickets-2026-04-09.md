# Race Fix Tickets — 2026-04-09

Purpose: turn the highest-risk contradictions from the race RE audit into
implementation-ready tickets with exact source targets and trace-based
acceptance criteria.

Use together with:

- `re/analysis/race-path-contradiction-ledger-2026-04-09.md`
- `re/analysis/race-path-arithmetic-fidelity-audit-2026-04-09.md`
- `re/analysis/race-trace-harness.md`
- `re/analysis/race-validation-pack-2026-04-09.md`
- `tools/compare_race_trace.py`

---

## Ticket 1: Restore A Coherent Race Timing Model

### Problem

The current timing path mixes at least two unit systems:

- wall-clock seconds in `g_td5.normalized_frame_dt`
- 30 Hz tick accumulator units in `g_td5.sim_time_accumulator`
- partially normalized values passed into race-completion and HUD paths

This makes the race loop harder to reason about and is a likely source of drift
in cooldown, fade, HUD timing, and trace alignment.

### Source Targets

- `td5mod/src/td5re/td5_game.c:1178-1181`
- `td5mod/src/td5re/td5_game.c:1200-1205`
- `td5mod/src/td5re/td5_game.c:2122-2124`
- `td5mod/src/td5re/td5_game.c:2190-2222`
- `td5mod/src/td5re/td5_hud.c:2661-2665`
- `re/analysis/tick-rate-dependent-constants.md`

### Current Contradiction

- The notes document a 30 Hz normalized timebase.
- The port currently stores `delta_ms / 1000.0f` into `g_td5.normalized_frame_dt`.
- The same field is later scaled by `65536.0f` and fed into race-completion logic.
- HUD entry points disagree on whether they want frame-dt or sim-budget semantics.

### Implementation Goal

Make one timing contract explicit and consistent:

1. Define what `g_td5.normalized_frame_dt` means.
2. Define what `g_td5.sim_tick_budget` means.
3. Stop passing one as if it were the other.
4. Keep `g_td5.sim_time_accumulator` as the authoritative per-tick drain source.

### Proposed Change Set

1. In `td5_game_update_frame_timing()`, either:
   - restore the documented normalized value `delta_ms * 0.001f * 30.0f`, or
   - rename the field and audit every downstream user so no code assumes normalized units.
2. In `td5_game_run_race_frame()`, make the conversion passed into `check_race_completion()` derive from the chosen authoritative timing unit, not from an ambiguous field.
3. In `td5_hud_render()` and `td5_hud_render_overlays()`, standardize the parameter contract:
   - either always pass normalized frame dt
   - or always pass wall-clock seconds
   - but not `sim_tick_budget` in one path and `normalized_frame_dt` in another.
4. Update comments in `td5_game.c` so the code matches the chosen model exactly.

### Trace Validation

Use the existing port trace plus the original Frida trace.

Primary scenarios:

1. Standing start
2. Lap / checkpoint transition

Compare these stages first:

1. `frame_begin`
2. `post_progress`
3. `frame_end`

Compare these fields first:

- `sim_accum`
- `sim_budget`
- `frame_dt`
- `instant_fps`
- `countdown_timer`
- `ticks_this_frame`

Acceptance criteria:

- No unit mismatch remains in the code comments or call sites.
- The first divergence for Scenario 1 is no longer at `frame_begin` timing state.
- HUD overlay calls use one stable timing contract.

### Definition Of Done

- `td5_game_update_frame_timing()` has one documented unit model.
- `check_race_completion()` and HUD calls consume timing values consistently.
- The trace for Scenario 1 shows stable per-frame timing semantics across 10-20 seconds.

---

## Ticket 2: Unify Race Progression Ownership

### Problem

Circuit and checkpoint completion are currently split across two live paths:

- `advance_pending_finish_state()` in `td5_game.c`
- `td5_track_update_circuit_lap()` in `td5_track.c`

Those paths are not equivalent and both mutate race-progression state.

### Source Targets

- `td5mod/src/td5re/td5_game.c:1377-1395`
- `td5mod/src/td5re/td5_game.c:1764-1864`
- `td5mod/src/td5re/td5_track.c:2338-2475`
- `re/analysis/race-progression-system.md`
- `re/analysis/race-validation-pack-2026-04-09.md`

### Current Contradiction

- `td5_game.c` uses a simplified 4-sector center-based completion path.
- `td5_track.c` uses start-line gating, heading alignment, wrong-way reset, and cooldown.
- Both run in the live tick path.
- The RE notes place completion ownership in the `CheckRaceCompletionState` family, not in two competing live owners.

### Implementation Goal

Choose one authoritative owner for race progression and subordinate or remove the other path.

Recommended owner:

- keep authoritative progression state in `td5_game.c`
- demote `td5_track.c` to track geometry / wrap / helper responsibilities only

This matches the existing source organization better and reduces duplicated state mutation.

### Proposed Change Set

1. Decide the canonical owner for:
   - lap completion
   - checkpoint advancement
   - finish state mutation
   - wrong-way reset / cooldown
2. Remove duplicate finish-state writes from the non-authoritative path.
3. If `td5_track_update_circuit_lap()` remains, narrow it to helper-only logic:
   - span normalization support
   - heading / start-line helper outputs
   - no direct finish-state ownership
4. Refactor `advance_pending_finish_state()` so its circuit branch is either:
   - replaced with the stronger start-line / heading / cooldown model, or
   - fed by helper outputs from `td5_track.c`
5. Update `race-progression-system.md` or the source comments only after the code path is singular again.

### Trace Validation

Primary scenarios:

1. Lap / checkpoint transition
2. Standing start on a circuit
3. Reverse / wrong-way recovery if reproducible

Compare these stages first:

1. `post_track`
2. `post_progress`
3. `frame_end`

Compare these fields first:

- `span_norm`
- `span_accum`
- `metric_checkpoint_index`
- `metric_checkpoint_mask`
- `finish_time`
- `race_pos`

Acceptance criteria:

- Only one code path mutates finish/lap progression state.
- Circuit lap completion does not depend on two concurrent models.
- The first divergence for Scenario 2 is no longer in checkpoint/lap state ownership.

### Definition Of Done

- There is one authoritative progression owner in the live race loop.
- Duplicate lap/finish state mutations are removed.
- Comments and docs point to one owner, not two.

---

## Ticket 3: Unify Race-Order Ownership And Remove Placeholder Track Ordering

### Problem

Race order is currently owned twice:

- real implementation in `update_race_order()` inside `td5_game.c`
- placeholder-backed `td5_track_update_race_order()` still called from `td5_track_tick()`

The track version does not read real actor data and should not remain in the live path.

### Source Targets

- `td5mod/src/td5re/td5_game.c:1371-1372`
- `td5mod/src/td5re/td5_game.c:2075-2116`
- `td5mod/src/td5re/td5_track.c:2797-2855`
- `td5mod/src/td5re/td5_track.c:3664-3668`
- `re/analysis/race-progression-system.md`

### Current Contradiction

- The game module has the real bubble-sort implementation using `s_metrics[].normalized_span`.
- The track module still claims ownership in comments and still calls a placeholder function every tick.
- That placeholder zeroes the actor bases and never writes race positions back.

### Implementation Goal

Make race ordering a single-owner system, with `td5_game.c` as the owner.

### Proposed Change Set

1. Remove `td5_track_update_race_order()` from the live `td5_track_tick()` path.
2. Delete or quarantine the placeholder implementation in `td5_track.c`.
3. Update `td5_track_tick()` comments so it no longer claims to correspond to `UpdateRaceActors`.
4. Keep race-order writes centralized in `update_race_order()` inside `td5_game.c`.
5. If the track module still needs to surface ordering-related helpers, expose read-only helpers rather than a second owner.

### Trace Validation

Primary scenarios:

1. AI overtake
2. Lap / checkpoint transition
3. Traffic merge / pass

Compare these stages first:

1. `post_track`
2. `post_progress`
3. `frame_end`

Compare these fields first:

- `metric_display_position`
- `race_pos`
- `span_high`
- `span_norm`
- `finish_time`

Acceptance criteria:

- There is no live placeholder ordering pass in `td5_track_tick()`.
- `race_pos` and display ordering are written by one source owner only.
- Overtake scenarios do not produce inconsistent intermediate ordering between stages.

### Definition Of Done

- `td5_track_tick()` no longer calls placeholder order logic.
- `td5_game.c` is the sole owner of race-order mutation.
- Module comments and trace expectations reflect that ownership cleanly.

---

## Recommended Execution Order

1. Ticket 3 first.
   It is the smallest structural cleanup and removes obvious noise from the live tick path.
2. Ticket 2 second.
   Progression ownership is the highest-value gameplay cleanup after race-order duplication is removed.
3. Ticket 1 third.
   Timing touches many downstream users and is easiest to validate once ordering and progression ownership are no longer split.

## Validation Rule

For each ticket:

1. change one owner/model only
2. rebuild
3. run one short golden scenario
4. compare traces
5. stop at the first remaining divergent stage

Do not merge multiple behavioral fixes into one trace cycle, or the evidence gets muddy again.
