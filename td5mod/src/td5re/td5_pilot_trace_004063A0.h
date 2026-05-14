/*
 * td5_pilot_trace_004063A0.h — Per-call CSV trace emitter for the precise-port
 * pilot of UpdateVehiclePoseFromPhysicsState (0x004063A0).
 *
 * One row per call: snapshot at entry, snapshot at exit, joined by call_id.
 * Output: log/port/pool3_004063A0.csv. Matches tools/frida_pool3_004063A0.js
 * column-for-column.
 */
#ifndef TD5_PILOT_TRACE_004063A0_H
#define TD5_PILOT_TRACE_004063A0_H

#include <stdint.h>
#include "td5_types.h"

void td5_pilot_emit_004063A0_enter(const TD5_Actor *actor, uintptr_t caller_ra);
void td5_pilot_emit_004063A0_leave(const TD5_Actor *actor);

#endif
