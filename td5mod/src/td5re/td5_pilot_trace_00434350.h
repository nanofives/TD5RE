/*
 * td5_pilot_trace_00434350.h -- Per-call CSV trace emitter for the
 * InitializeActorTrackPose pilot (pool14 / 0x00434350).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 *
 * One row per spawn-time invocation (per slot, init-only). Captures the
 * params + span_record + relevant vertices + post-init actor state and
 * RouteState (RS_TRACK_PROGRESS, RS_TRACK_OFFSET_BIAS).
 *
 * Output: log/port/pool14_00434350.csv
 */
#ifndef TD5_PILOT_TRACE_00434350_H
#define TD5_PILOT_TRACE_00434350_H

#include <stdint.h>

/* Single-shot emitter: call AFTER td5_track_compute_heading +
 * td5_physics_reset_actor_state + td5_ai_seed_actor_track_progress_offset
 * have all run for a slot. Reads inputs (param_*, span_record, vertices) and
 * outputs (actor state + RouteState) and appends one row.
 */
void td5_pilot_emit_00434350_row(int call_idx,
                                  int slot,
                                  int param_span,
                                  int param_sub_lane,
                                  int param_flip,
                                  const void *actor);

#endif
