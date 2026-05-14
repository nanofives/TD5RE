/*
 * td5_pilot_trace_0042EB10.h -- Per-call CSV trace emitter for the precise-port
 * pilot targeting TransformShortVec3ByRenderMatrixRounded @ 0x0042EB10.
 *
 * Mirrors tools/frida_pool4_0042EB10.js exactly. One row per inlined
 * transform invocation inside td5_physics_refresh_wheel_contacts.
 *
 * Output: log/port/pool4_0042EB10.csv (5000-row cap).
 */
#ifndef TD5_PILOT_TRACE_0042EB10_H
#define TD5_PILOT_TRACE_0042EB10_H

#include <stdint.h>
#include "td5_types.h"

/* kind values match the Frida CSV column. */
#define PILOT_0042EB10_KIND_WCP   "wcp"
#define PILOT_0042EB10_KIND_HIRES "hires"
#define PILOT_0042EB10_KIND_PROBE "probe"

/* Emit one CSV row mirroring an inline TransformShortVec3 call.
 *   tick      : current sim tick (td5_trace_current_sim_tick())
 *   slot      : actor slot (0..5); only slot 0 is emitted
 *   kind      : "wcp"/"hires"/"probe" (call site identifier)
 *   wheel     : wheel index 0..3
 *   caller_ra : faux return address (kind→RA mapping) so rows align with Frida
 *   param1    : pointer to source short vec3 (for actor_addr column parity)
 *   param2    : pointer to destination int32 vec3
 *   p0..p2    : 3 int16 inputs
 *   matrix    : 12 floats (rot[0..8] then transl[0..2])
 *   out0..2   : 3 int32 outputs (after the function would have written them)
 */
void td5_pilot_emit_0042EB10(
    uint32_t tick,
    int      slot,
    const char *kind,
    int      wheel,
    uint32_t caller_ra,
    const void *param1,
    const void *param2,
    int16_t p0, int16_t p1, int16_t p2,
    const float matrix[12],
    int32_t out0, int32_t out1, int32_t out2);

#endif
