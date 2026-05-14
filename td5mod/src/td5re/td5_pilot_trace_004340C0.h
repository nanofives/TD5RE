/*
 * td5_pilot_trace_004340C0.h -- Per-function CSV trace emitter for the
 * UpdateActorSteeringBias pilot (pool10 / 0x004340C0).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 *
 * One row per (sim_tick, slot, call_site). Inputs captured at entry BEFORE
 * state mutation; outputs captured at exit. Only slot 0 is emitted.
 * Output: log/port/pool10_004340C0.csv
 */
#ifndef TD5_PILOT_TRACE_004340C0_H
#define TD5_PILOT_TRACE_004340C0_H

#include <stdint.h>

void td5_pilot_emit_004340C0_enter(int slot,
                                   const int32_t *rs,
                                   const void *actor,
                                   int32_t steer_weight,
                                   const char *call_site);
void td5_pilot_emit_004340C0_leave(int slot,
                                   const int32_t *rs,
                                   const void *actor);

#endif
