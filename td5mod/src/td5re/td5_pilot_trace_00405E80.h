/*
 * td5_pilot_trace_00405E80.h -- Per-call CSV trace emitter for the
 * precise-port pilot targeting 0x00405E80 IntegrateVehiclePoseAndContacts.
 *
 * Pool tag: pool2
 * Output:   log/port/pool2_00405E80.csv
 *
 * Schema mirrors tools/frida_pool2_00405E80.js column-for-column.
 *
 * Call enter() at the very top of td5_physics_integrate_pose, before any
 * state mutation. Call leave() immediately before returning (after the
 * tail-call UpdateVehicleSuspensionResponse equivalent).
 */
#ifndef TD5_PILOT_TRACE_00405E80_H
#define TD5_PILOT_TRACE_00405E80_H

#include <stdint.h>
#include "td5_types.h"

void td5_pilot_emit_00405E80_enter(const TD5_Actor *actor, uintptr_t caller_ra);
void td5_pilot_emit_00405E80_leave(const TD5_Actor *actor);

#endif /* TD5_PILOT_TRACE_00405E80_H */
