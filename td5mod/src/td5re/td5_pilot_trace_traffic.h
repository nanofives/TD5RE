/*
 * td5_pilot_trace_traffic.h — Per-call CSV trace emitter for the
 * traffic-physics pair (precise-port pool8 pilot).
 *
 * Pair:
 *   0x004437C0 ApplyDampedSuspensionForce       (which_fn = "susp")
 *   0x004438F0 IntegrateVehicleFrictionForces   (which_fn = "friction")
 *
 * One CSV row per call. Schema matches tools/frida_pool8_traffic.js
 * column-for-column so diff_func_trace.py can pair rows by
 * (sim_tick, slot, which_fn) and report per-column divergence.
 */
#ifndef TD5_PILOT_TRACE_TRAFFIC_H
#define TD5_PILOT_TRACE_TRAFFIC_H

#include <stdint.h>
#include "td5_types.h"

/* friction = IntegrateVehicleFrictionForces (called once per traffic actor per tick) */
void td5_pilot_emit_traffic_friction_enter(const TD5_Actor *actor);
void td5_pilot_emit_traffic_friction_leave(const TD5_Actor *actor);

/* susp = ApplyDampedSuspensionForce (called once from inside friction) */
void td5_pilot_emit_traffic_susp_enter(const TD5_Actor *actor,
                                       int32_t lateral, int32_t longitudinal);
void td5_pilot_emit_traffic_susp_leave(const TD5_Actor *actor);

#endif
