/*
 * td5_pilot_trace_00434FE0.h -- Per-function CSV trace emitter for the
 * UpdateActorTrackBehavior pilot (pool9 / 0x00434FE0).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 *
 * One row per (sim_tick, slot). Inputs captured at entry BEFORE state
 * mutation; outputs captured at exit. Only slot 0 is emitted.
 * Output: log/port/pool9_00434FE0.csv
 */
#ifndef TD5_PILOT_TRACE_00434FE0_H
#define TD5_PILOT_TRACE_00434FE0_H

#include <stdint.h>

void td5_pilot_emit_00434FE0_enter(int slot, const int32_t *rs, const void *actor, int32_t game_type);
void td5_pilot_emit_00434FE0_leave(int slot, const int32_t *rs, const void *actor);

#endif
