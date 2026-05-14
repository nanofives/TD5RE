/*
 * td5_pilot_trace_00404030.h -- Per-function CSV trace emitter for
 * UpdatePlayerVehicleDynamics (pool0 / 0x00404030).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 *
 * One row per (sim_tick, slot). Inputs captured at entry, outputs at exit.
 * Only slot 0 (player) is emitted.
 * Output: log/port/pool0_00404030.csv
 */
#ifndef TD5_PILOT_TRACE_00404030_H
#define TD5_PILOT_TRACE_00404030_H

#include <stdint.h>
#include "td5_types.h"

void td5_pilot_emit_00404030_enter(const TD5_Actor *actor, uintptr_t caller_ra);
void td5_pilot_emit_00404030_leave(const TD5_Actor *actor);

#endif
