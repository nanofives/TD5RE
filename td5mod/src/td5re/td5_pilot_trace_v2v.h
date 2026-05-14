/*
 * td5_pilot_trace_v2v.h -- Pool14 V2V impulse byte-exact trace emitter
 *                          (precise-port pilot, pool14_v2v).
 *
 * Captures inputs + pre/post state of ApplyVehicleCollisionImpulse
 * (0x004079C0). Schema mirrors tools/frida_pool14_v2v.js column-for-column
 * so the diff tool can pair rows by (sim_tick, slotA, slotB, angle, impactForce).
 *
 * Calling discipline:
 *   td5_pilot_v2v_enter(A, B, corner, angle, impactForce);
 *   ... apply_collision_response body ...
 *   td5_pilot_v2v_leave(A, B, retval);
 *
 * Wrapped automatically inside apply_collision_response when compiled with
 * TD5_PILOT_V2V_TRACE.
 */
#ifndef TD5_PILOT_TRACE_V2V_H
#define TD5_PILOT_TRACE_V2V_H

#include <stdint.h>
#include "td5_types.h"

typedef struct OBB_CornerData OBB_CornerData;

void td5_pilot_v2v_enter(const TD5_Actor *A, const TD5_Actor *B,
                         const OBB_CornerData *corner,
                         int32_t angle, int32_t impactForce);
void td5_pilot_v2v_leave(const TD5_Actor *A, const TD5_Actor *B,
                         int32_t retval);

#endif
