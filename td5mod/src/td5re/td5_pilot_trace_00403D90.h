/*
 * td5_pilot_trace_00403D90.h -- Per-function CSV trace emitter for the
 * UpdateVehicleState0fDamping pilot (pool6 / 0x00403D90).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 *
 * One row per (sim_tick, slot, caller_ra). Inputs captured at entry
 * (BEFORE state mutation); outputs captured at exit. Only slot 0 emitted.
 * Output: log/port/pool6_00403D90.csv
 */
#ifndef TD5_PILOT_TRACE_00403D90_H
#define TD5_PILOT_TRACE_00403D90_H

#include <stdint.h>
#include "td5_types.h"

/* Caller-RA tag: __builtin_return_address(0) at the call site (always
 * 0x00406650 because UpdateVehicleActor is the sole caller). */
void td5_pilot_emit_00403D90_enter(const TD5_Actor *actor, uintptr_t caller_ra);
void td5_pilot_emit_00403D90_leave(const TD5_Actor *actor);

#endif
