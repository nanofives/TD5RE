/*
 * td5_pilot_trace_00406980.h — Per-call CSV trace emitter for WallResponse.
 *
 * Mirrors tools/frida_pool7_00406980.js column-for-column so the diff tool
 * can match rows by (sim_tick, slot, call_idx). One row per call to
 * td5_physics_wall_response.
 */
#ifndef TD5_PILOT_TRACE_00406980_H
#define TD5_PILOT_TRACE_00406980_H

#include <stdint.h>
#include "td5_types.h"

/* Call at the very top of td5_physics_wall_response, before any state
 * mutation. Captures the actor, args, and pre-state snapshot. */
void td5_pilot_emit_00406980_enter(const TD5_Actor *actor,
                                    int32_t force_x_fp8,
                                    int32_t force_y_fp8,
                                    int32_t force_z_fp8,
                                    uint32_t angle,
                                    int32_t magnitude,
                                    uint32_t flags);

/* Call at the bottom of td5_physics_wall_response after all writes complete.
 * Captures post-state, computes deltas, emits the row. */
void td5_pilot_emit_00406980_leave(const TD5_Actor *actor);

#endif
