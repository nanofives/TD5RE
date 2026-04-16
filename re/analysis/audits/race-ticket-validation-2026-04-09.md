# Race Ticket Validation — 2026-04-09

Purpose: record the first runtime validation pass after the race ticket
implementation work landed in source.

Inputs used:

- `log/race_trace.csv`
- `log/race_trace_original.csv`
- `tools/compare_race_trace.py`
- `re/analysis/race-fix-tickets-2026-04-09.md`

## Summary

The three race tickets improved the structural model, but the current trace data
does not yet support a clean full-key comparison.

Two separate issues are visible:

1. timing still diverges immediately
2. the trace schema still produces awkward comparison behavior for countdown and
   render-only frames

The strongest positive result is that the progression-state fields now align far
better than the raw comparator output suggested.

Important caveat:

- the original capture used for this validation pass was taken before the Frida
  trace script was repaired
- that capture logged `post_progress` at `EndRaceScene` entry and left
  `ticks_this_frame` stuck at `0`
- it is useful for broad progression/ranking clues, but it is not a trustworthy
  timing baseline

## Trace Comparison Constraints

### Countdown / render-only key instability

The port trace still emits duplicate logical states when compared by:

- `frame`
- `sim_tick`
- `stage`
- `kind`
- `id`

This is most obvious in countdown and render-only spans where multiple stage
records share the same `frame` and `sim_tick`.

Because of that, the full comparator output is noisy and not a reliable first
signal for the ticket validation pass.

### Active-race filtered comparison

To get a usable first comparison, the traces were filtered to:

- stages: `frame_begin`, `post_progress`, `frame_end`
- rows where `sim_tick > 0`

Filtered files:

- `temp_analysis/trace_orig_shared.csv`
- `temp_analysis/trace_port_shared.csv`

The useful validation view is a sim-tick aligned comparison of
`post_progress` rows, not the raw full-key compare.

## Measured Results

### Coverage

- original shared sim ticks: `1..321`
- port shared sim ticks: `1..283`
- overlapping sim ticks: `283`

### Progression-state result

For `post_progress` actor rows, these fields matched across all `283` shared
sim ticks:

- `metric_checkpoint`
- `metric_mask`
- `finish_time`
- `lap_progress`

This is the strongest confirmation that the race progression ownership cleanup
did not regress the basic checkpoint / finish-state path in the sampled trace.

### Remaining race-order / ranking mismatches

For `post_progress` actor rows:

- `metric_display_pos` mismatched on `282 / 283` shared ticks
- `race_pos` mismatched on `109 / 283` shared ticks

Observed pattern:

- first `race_pos` mismatch: sim tick `1` (`orig=2`, `port=0`)
- first `metric_display_pos` mismatch: sim tick `2` (`orig=0`, `port=2`)
- `metric_display_pos` stays offset for almost the whole shared window

This means the race-order owner is structurally singular now, but the ranking
semantics are still not aligned to the original trace.

### Spatial drift

For `post_progress` actor rows:

- `span_norm` mismatched on `209 / 283` shared ticks
- `span_accum` mismatched on `209 / 283` shared ticks
- `span_high` mismatched on `209 / 283` shared ticks

Observed pattern:

- first span mismatch: sim tick `75` (`orig=112`, `port=111`)
- by later shared ticks, the original trace reaches `121` while the port stays
  at `111`

That suggests the player-route / progression position is drifting after the
initial shared startup window even though checkpoint mask state remains aligned.

### Timing drift

For `post_progress` frame rows, these fields mismatched on all `283 / 283`
shared ticks:

- `sim_accum`
- `sim_budget`
- `frame_dt`
- `instant_fps`
- `ticks_this_frame`

Observed pattern at sim tick `1`:

- `sim_accum`: `59636` vs `6248`
- `sim_budget`: `1.119975` vs `0.180000`
- `frame_dt`: `0.209999` vs `0.180000`
- `instant_fps`: `142.857131` vs `166.666672`
- `ticks_this_frame`: `0` vs `1`

This suggests timing remains the first material runtime divergence, but that
claim must be re-verified with a fresh original capture taken after the Frida
hook fix.

## Ticket Status After Validation

### Ticket 1: timing model cleanup

Status: not validated successfully yet

Reason:

- timing fields diverge from sim tick `1`
- `ticks_this_frame` does not agree at all in the sampled overlap
- the original timing capture itself was taken with an invalid minimal hook set

Next move:

- recapture `log/race_trace_original.csv` with the repaired Frida script
- then re-audit `td5_game_update_frame_timing()` and the `post_progress`
  emission point against the corrected original hook stage

### Ticket 2: progression ownership cleanup

Status: structurally successful, behavior still needs follow-up

Reason:

- checkpoint / finish-state fields align across the shared trace window
- span-based motion still drifts later in the run

Next move:

- inspect the route / span ownership path around the first span mismatch at
  sim tick `75`
- focus on `post_track` and `post_progress` actor movement state

### Ticket 3: race-order ownership cleanup

Status: structurally successful, ranking semantics still off

Reason:

- there is one owner now
- `metric_display_pos` and `race_pos` still diverge significantly

Next move:

- inspect how `s_metrics[i].display_position` is expected to map to the
  original ranking trace
- verify whether the original trace field is player-centric placement,
  global display ranking, or a different slot mapping entirely

## Recommended Next Steps

1. Make the port trace schema unambiguous for countdown / render-only frames,
   either by adding a per-emission sequence or by narrowing comparison to one
   canonical stage per sim tick.
2. Re-capture the original trace with the repaired Frida script before drawing
   any more conclusions from timing fields.
3. Run a focused timing audit from the first overlapping sim tick instead of
   broad race debugging.
4. Inspect the rank/display-position mapping before treating the order mismatch
   as a gameplay bug.
5. Investigate the first span drift at sim tick `75` as the most likely
   gameplay-side divergence after timing.
