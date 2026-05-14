/*
 * td5_pilot_trace_00432D60.c -- Port-side CSV emitter for the
 * ComputeAIRubberBandThrottle pilot (pool11 / 0x00432D60).
 *
 * Mirrors tools/frida_pool11_00432D60.js column-for-column.
 *
 * One row per slot per call. Slots 0..5 emitted (6 rows per call).
 */
#include "td5_pilot_trace_00432D60.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool11_00432D60.csv"
#define N_SLOTS  6

/* Externs from td5_ai.c -- the 8 globals we need to inspect.
 * Actor +0x84 (span_accum) is read via the public ACTOR_I16 helper. */
extern int32_t g_default_throttle[14];
extern int32_t g_live_throttle[14];
extern int32_t g_actor_route_steer_bias[];
extern int32_t g_rb_behind_scale;
extern int32_t g_rb_ahead_scale;
extern int32_t g_rb_behind_range;
extern int32_t g_rb_ahead_range;

/* From td5_game.c / td5_trace.c */
extern int td5_trace_current_sim_tick(void);

static FILE *s_fp = NULL;
static int   s_active_call = 0;
static uint32_t s_tick = 0;

static PilotSnapshot_00432D60 s_in_snap;

/* Pre-call snapshot of OUTPUT fields (so we can diff orig vs port even
 * though the port reuses the same memory). g_actor_route_steer_bias and
 * g_live_throttle are written by the function — capture pre and post. */
static int32_t s_pre_steer_bias[N_SLOTS];
static int32_t s_pre_live_throttle[14];

static int32_t s_pre_rb_behind_scale;
static int32_t s_pre_rb_ahead_scale;
static int32_t s_pre_rb_behind_range;
static int32_t s_pre_rb_ahead_range;

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,slot,phase,"
        /* inputs (call-wide constants — same value across all slot rows) */
        "network_active,racer_count,"
        "rb_behind_scale,rb_ahead_scale,rb_behind_range,rb_ahead_range,"
        "player0_span_accum,"
        /* inputs (per-slot) */
        "slot_state,ai_span_accum,default_throttle,"
        /* outputs (per-slot) */
        "delta,modifier,live_throttle,bias_out\n");
}

void td5_pilot_emit_00432D60_enter(void) {
    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    s_active_call = 1;
    s_tick = (uint32_t)td5_trace_current_sim_tick();

    /* Capture the four rubber-band params + pre-call output arrays. */
    s_pre_rb_behind_scale  = g_rb_behind_scale;
    s_pre_rb_ahead_scale   = g_rb_ahead_scale;
    s_pre_rb_behind_range  = g_rb_behind_range;
    s_pre_rb_ahead_range   = g_rb_ahead_range;

    for (int i = 0; i < N_SLOTS; i++)
        s_pre_steer_bias[i] = g_actor_route_steer_bias[i];
    memcpy(s_pre_live_throttle, g_live_throttle, sizeof s_pre_live_throttle);

    /* Snapshot per-slot inputs from the port's runtime state. */
    td5_pilot_00432D60_collect(&s_in_snap);
}

void td5_pilot_emit_00432D60_leave(void) {
    if (!s_active_call || !s_fp) {
        s_active_call = 0;
        return;
    }

    for (int slot = 0; slot < N_SLOTS; slot++) {
        int32_t delta = 0, modifier = 0;
        if (s_in_snap.slot_state[slot] == 0) {
            delta = s_in_snap.ai_span_accum[slot] - s_in_snap.player0_span_accum;
            /* Apply the same clamps the function would have applied. The
             * goal here is to make `delta` and `modifier` self-consistent
             * with the recorded bias_out. The Frida side computes these
             * the same way for byte-equal columns. */
            if (delta < 0) {
                int32_t r = s_pre_rb_behind_range;
                int32_t s = s_pre_rb_behind_scale;
                /* Dead upper-clamp (matches listing): if (delta > r) delta = r; */
                if (delta > r) delta = r;
                if (r != 0) modifier = (s * delta) / r;
            } else {
                int32_t r = s_pre_rb_ahead_range;
                int32_t s = s_pre_rb_ahead_scale;
                if (delta > r) delta = r;
                if (r != 0) modifier = (s * delta) / r;
            }
        }

        fprintf(s_fp,
            "%u,%d,leave,"
            "%d,%d,"
            "%d,%d,%d,%d,"
            "%d,"
            "%d,%d,%d,"
            "%d,%d,%d,%d\n",
            s_tick, slot,
            s_in_snap.network_active, s_in_snap.racer_count,
            s_pre_rb_behind_scale, s_pre_rb_ahead_scale,
            s_pre_rb_behind_range, s_pre_rb_ahead_range,
            s_in_snap.player0_span_accum,
            s_in_snap.slot_state[slot], s_in_snap.ai_span_accum[slot],
            g_default_throttle[slot],
            delta, modifier,
            g_live_throttle[slot], g_actor_route_steer_bias[slot]);
    }
    fflush(s_fp);
    s_active_call = 0;
}
