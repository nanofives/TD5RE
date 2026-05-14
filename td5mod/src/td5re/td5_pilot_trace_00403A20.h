/*
 * td5_pilot_trace_00403A20.h -- Per-function CSV trace emitter for the
 * IntegrateWheelSuspensionTravel pilot (pool5 / 0x00403A20).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 *
 * One row per (sim_tick, slot, wheel, caller_ra). Inputs captured at entry
 * (BEFORE state mutation); outputs captured at exit. Only slot 0 is emitted.
 * Output: log/port/pool5_00403A20.csv
 */
#ifndef TD5_PILOT_TRACE_00403A20_H
#define TD5_PILOT_TRACE_00403A20_H

#include <stdint.h>
#include "td5_types.h"

/* Caller tag: __builtin_return_address(0) at the call site. */
void td5_pilot_emit_00403A20_enter(const TD5_Actor *actor, int32_t accel_x, int32_t accel_z, uintptr_t caller_ra);
void td5_pilot_emit_00403A20_leave(const TD5_Actor *actor);

#endif
