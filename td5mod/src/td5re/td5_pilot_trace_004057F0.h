/*
 * td5_pilot_trace_004057F0.h -- Per-call CSV trace emitter for
 * UpdateVehicleSuspensionResponse @ 0x004057F0 (precise-port pilot pool6).
 *
 * Output goes to log/port/pool6_004057F0.csv with a header matching the
 * matching Frida script's CSV exactly (tools/frida_pool6_004057F0.js).
 * Only actor slot 0 is emitted; one row per call.
 */
#ifndef TD5_PILOT_TRACE_004057F0_H
#define TD5_PILOT_TRACE_004057F0_H

#include <stdint.h>
#include "td5_types.h"

/* Snapshot inputs (rotation matrix, lock/prev_air, gravity, per-wheel arms/wcv/wfdh,
 * and pre-call angular_velocity_{roll,pitch} + linear_velocity_y) at function entry.
 * Call BEFORE any state mutation in td5_physics_update_suspension_response.
 * `gravity` should be the value of the static `g_gravity_constant` in td5_physics.c. */
void td5_pilot_emit_004057F0_enter(const TD5_Actor *actor, uintptr_t caller_ra, int32_t gravity);

/* Read post-call angular_velocity_{roll,pitch} + linear_velocity_y, compute deltas
 * vs the entry snapshot, and emit one row to the CSV. Call AT FUNCTION EXIT. */
void td5_pilot_emit_004057F0_leave(const TD5_Actor *actor);

#endif
