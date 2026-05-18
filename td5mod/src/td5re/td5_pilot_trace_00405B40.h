/*
 * td5_pilot_trace_00405B40.h -- Port-side CSV emitter for
 * ClampVehicleAttitudeLimits (pool4 / session 20).
 *
 * Hook is in td5_physics.c td5_physics_clamp_attitude. Writes one row per
 * call to log/port/pool4_00405B40.csv matching tools/frida_pool4_00405B40.js
 * column-for-column so tools/diff_func_trace.py can pair rows by
 * (sim_tick, slot).
 *
 * Audit: re/analysis/pilot_00405B40_audit.md
 */
#ifndef TD5_PILOT_TRACE_00405B40_H
#define TD5_PILOT_TRACE_00405B40_H

#include <stdint.h>
#include "td5_types.h"

/* Call at function entry (before any state mutation) to snapshot inputs. */
void td5_pilot_emit_00405B40_enter(const TD5_Actor *actor, uintptr_t caller_ra);

/* Call at every return (or at the function tail). Records the branch
 * taken (0=skip, 1=mode1, 2=mode0_latch) and final field values. */
void td5_pilot_emit_00405B40_leave(const TD5_Actor *actor, int branch_taken);

#endif
