/*
 * td5_pilot_trace_00404EC0.h -- Per-function CSV trace emitter for
 * UpdateAIVehicleDynamics (pool12 / 0x00404EC0).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 *
 * One row per (sim_tick, slot, caller_ra). Inputs captured at entry,
 * outputs at exit. Only slot 0 is emitted.
 * Output: log/port/pool12_00404EC0.csv
 */
#ifndef TD5_PILOT_TRACE_00404EC0_H
#define TD5_PILOT_TRACE_00404EC0_H

#include <stdint.h>
#include "td5_types.h"

void td5_pilot_emit_00404EC0_enter(const TD5_Actor *actor, uintptr_t caller_ra);
void td5_pilot_emit_00404EC0_leave(const TD5_Actor *actor);

#endif
