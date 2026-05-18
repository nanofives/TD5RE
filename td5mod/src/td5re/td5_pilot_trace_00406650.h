/*
 * td5_pilot_trace_00406650.h -- Per-call CSV trace emitter for the precise-port
 * pilot @ 0x00406650 UpdateVehicleActor.
 *
 * One row per (sim_tick, slot, phase) where phase = "ENTER" or "LEAVE".
 * Captures actor scalar fields + relevant globals so the diff can pair port
 * and original snapshots taken at the same logical instant.
 *
 * Output goes to log/port/pool1_00406650.csv. The matching Frida script is
 * tools/frida_pool1_00406650.js.
 */
#ifndef TD5_PILOT_TRACE_00406650_H
#define TD5_PILOT_TRACE_00406650_H

#include <stdint.h>
#include "td5_types.h"

void td5_pilot_emit_00406650_enter(const TD5_Actor *actor);
void td5_pilot_emit_00406650_leave(const TD5_Actor *actor);

#endif
