/*
 * td5_pilot_trace_00405D70.h -- Per-call CSV trace emitter for the
 * ResetVehicleActorState pilot (pool5 / 0x00405D70).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 *
 * One row per call, keyed by (sim_tick, slot, caller_ra). Inputs that survive
 * the reset are captured at entry; the (zeroed/seeded) post-state is captured
 * after the body completes. Only slot 0 is emitted.
 * Output: log/port/pool5_00405D70.csv
 */
#ifndef TD5_PILOT_TRACE_00405D70_H
#define TD5_PILOT_TRACE_00405D70_H

#include <stdint.h>
#include "td5_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wrap-style emit: call once at the end of td5_physics_reset_actor_state
 * (after the post-integrate cleanup writes). The emitter snapshots the
 * pre-state on first entry per call by stashing the actor's incoming state
 * before the body runs, then emits both PRE + POST in a single CSV row.
 *
 * To keep the schema in lockstep with Frida (which captures PRE in onEnter +
 * POST in onLeave), we expose a simple two-call API:
 *
 *   td5_pilot_trace_00405D70_enter(actor)   – capture PRE
 *   td5_pilot_trace_00405D70_leave(actor)   – emit full row using stashed PRE
 *
 * If only the single-emit form is needed, td5_pilot_trace_emit_00405D70()
 * does both internally (PRE captured from the actor as-is at call time
 * — i.e. mid-function, AFTER zeroes have been applied — which is wrong for
 * input columns). Use the enter/leave pair for byte-exact mirror to Frida. */
void td5_pilot_trace_00405D70_enter(const TD5_Actor *actor);
void td5_pilot_trace_00405D70_leave(const TD5_Actor *actor);

#ifdef __cplusplus
}
#endif

#endif /* TD5_PILOT_TRACE_00405D70_H */
