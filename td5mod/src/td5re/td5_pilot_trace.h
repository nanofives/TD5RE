/*
 * td5_pilot_trace.h -- Per-function CSV trace emitter for the precise-port
 * pilot (workflow doc: re/analysis/precise_port_workflow.md).
 *
 * Pool/function tag is baked into the function name + output path; one .c
 * module per pilot keeps the schema explicit and side-effect-free. Output
 * goes to log/port/pool<N>_<addr>.csv with a header matching the matching
 * Frida script's CSV exactly (tools/frida_pool<N>_<addr>.js).
 */
#ifndef TD5_PILOT_TRACE_H
#define TD5_PILOT_TRACE_H

#include <stdint.h>
#include "td5_types.h"

/* Pool 1 / 0x00403720 / RefreshVehicleWheelContactFrames.
 * Call once at function entry (before any state mutation) and once at exit
 * (after all writes complete). Only slot 0 is emitted; rows are appended
 * one per wheel (4 rows per call). Caller-PC is recorded so the diff can
 * distinguish IntegrateVehiclePoseAndContacts vs UpdateVehiclePoseFromPhysicsState. */
void td5_pilot_emit_00403720_enter(const TD5_Actor *actor, uintptr_t caller_ra);
void td5_pilot_emit_00403720_leave(const TD5_Actor *actor);

#endif
