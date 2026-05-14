/*
 * td5_pilot_trace_00432D60.h -- Per-function CSV trace emitter for the
 * ComputeAIRubberBandThrottle pilot (pool11 / 0x00432D60).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 *
 * Schema mirrors tools/frida_pool11_00432D60.js. One CSV row per AI slot per
 * call (per UpdateRaceActors tick). Inputs captured at entry (BEFORE the
 * function mutates g_actor_route_steer_bias / g_live_throttle); outputs
 * captured at exit. We emit ALL 6 slots so the diff can spot slot-state
 * branch divergences as well as per-slot bias divergences.
 *
 * Output: log/port/pool11_00432D60.csv
 */
#ifndef TD5_PILOT_TRACE_00432D60_H
#define TD5_PILOT_TRACE_00432D60_H

#include <stdint.h>

/* Snapshot of all inputs read by ComputeAIRubberBandThrottle.
 * Collected at function entry (BEFORE state mutation). Slots 0..5 only;
 * the original's effective cap is min(g_racerCount, 6). */
typedef struct {
    int network_active;
    int racer_count;
    int slot_state[6];
    int ai_span_accum[6];
    int player0_span_accum;
} PilotSnapshot_00432D60;

/* Called BEFORE the rubber-band loop runs. Snapshots all inputs. */
void td5_pilot_emit_00432D60_enter(void);

/* Called AFTER the rubber-band loop runs. Emits one row per slot with
 * input-snapshot values + post-function output values. */
void td5_pilot_emit_00432D60_leave(void);

/* Implemented in td5_ai.c — reads slot-state and span_accum arrays that
 * are private to that translation unit. */
void td5_pilot_00432D60_collect(PilotSnapshot_00432D60 *snap);

#endif
