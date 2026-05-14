/*
 * td5_pilot_trace_0042F030.h -- Per-function CSV trace emitter for the
 * ComputeDriveTorqueFromGearCurve pilot (pool13 / 0x0042F030).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 * Audit:    re/analysis/pilot_0042F030_audit.md
 *
 * One row per call, keyed by (sim_tick, slot, caller_ra). Inputs captured
 * at entry (BEFORE state mutation — this function is pure-leaf so no
 * mutation occurs); output captured at exit. Only slot 0 is emitted.
 * Output: log/port/pool13_0042F030.csv
 */
#ifndef TD5_PILOT_TRACE_0042F030_H
#define TD5_PILOT_TRACE_0042F030_H

#include <stdint.h>
#include "td5_types.h"

void td5_pilot_emit_0042F030_enter(const TD5_Actor *actor, uintptr_t caller_ra);
void td5_pilot_emit_0042F030_leave(const TD5_Actor *actor, int32_t return_value,
                                   int32_t lut_index_used, int32_t lut_frac_used);

#endif
