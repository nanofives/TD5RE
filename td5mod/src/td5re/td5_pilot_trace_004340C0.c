/*
 * td5_pilot_trace_004340C0.c -- Port-side CSV emitter for the
 * UpdateActorSteeringBias pilot (pool10 / 0x004340C0).
 *
 * Mirrors tools/frida_pool10_004340C0.js column-for-column. Uses raw byte
 * offsets so the schema is insulated from port-side struct drift.
 */
#include "td5_pilot_trace_004340C0.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define OUT_PATH "log/port/pool10_004340C0.csv"

/* RS dword indices (must match the AI module — local copies to keep this
 * file decoupled). */
#define RS_LEFT_DEVIATION   0x16
#define RS_RIGHT_DEVIATION  0x17
#define RS_SLOT_INDEX       0x35

/* Actor byte offsets */
#define O_STEERING_CMD          0x30C
#define O_LONGITUDINAL_SPEED    0x314
#define O_REAR_AXLE_SLIP        0x320
#define O_STEERING_RAMP_ACCUM   0x33A

#define TARGET_SLOT  0

typedef struct {
    int32_t rs_left_deviation;
    int32_t rs_right_deviation;
    int32_t rs_slot_index;
    int32_t actor_lspd;
    int32_t actor_rear_slip;
    int32_t actor_steering_cmd;
    int16_t actor_ramp_accum;
} PilotSnap;

static FILE     *s_fp        = NULL;
static int       s_active    = 0;
static uint32_t  s_tick      = 0;
static int       s_slot      = 0;
static int32_t   s_weight    = 0;
static const char *s_callsite = "";
static PilotSnap s_inputs;

extern int td5_trace_current_sim_tick(void);

static inline int16_t rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }

static void snapshot(PilotSnap *s, const int32_t *rs, const void *actor) {
    const uint8_t *abase = (const uint8_t *)actor;
    s->rs_left_deviation   = rs[RS_LEFT_DEVIATION];
    s->rs_right_deviation  = rs[RS_RIGHT_DEVIATION];
    s->rs_slot_index       = rs[RS_SLOT_INDEX];
    s->actor_lspd          = rd_i32(abase + O_LONGITUDINAL_SPEED);
    s->actor_rear_slip     = rd_i32(abase + O_REAR_AXLE_SLIP);
    s->actor_steering_cmd  = rd_i32(abase + O_STEERING_CMD);
    s->actor_ramp_accum    = rd_i16(abase + O_STEERING_RAMP_ACCUM);
}

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,call_site,"
        "param_2_steer_weight,"
        "rs_left_deviation_in,rs_right_deviation_in,rs_slot_index,"
        "actor_longitudinal_speed_in,actor_rear_axle_slip_in,"
        "actor_steering_cmd_in,actor_steering_ramp_accum_in,"
        "actor_steering_cmd_out,actor_steering_ramp_accum_out\n");
}

void td5_pilot_emit_004340C0_enter(int slot,
                                   const int32_t *rs,
                                   const void *actor,
                                   int32_t steer_weight,
                                   const char *call_site) {
    s_active = 0;
    if (slot != TARGET_SLOT || !rs || !actor) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    s_active   = 1;
    s_slot     = slot;
    s_tick     = (uint32_t)td5_trace_current_sim_tick();
    s_weight   = steer_weight;
    s_callsite = call_site ? call_site : "unknown";
    snapshot(&s_inputs, rs, actor);
}

void td5_pilot_emit_004340C0_leave(int slot,
                                   const int32_t *rs,
                                   const void *actor) {
    if (!s_active || !s_fp || slot != s_slot) {
        s_active = 0;
        return;
    }
    PilotSnap out;
    snapshot(&out, rs, actor);

    const char *paused_str = "0";

    fprintf(s_fp,
        "%u,%s,%d,%s,"
        "%d,"
        "%d,%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d\n",
        s_tick, paused_str, s_slot, s_callsite,
        s_weight,
        s_inputs.rs_left_deviation, s_inputs.rs_right_deviation, s_inputs.rs_slot_index,
        s_inputs.actor_lspd, s_inputs.actor_rear_slip,
        s_inputs.actor_steering_cmd, (int)s_inputs.actor_ramp_accum,
        out.actor_steering_cmd, (int)out.actor_ramp_accum);
    fflush(s_fp);
    s_active = 0;
}
