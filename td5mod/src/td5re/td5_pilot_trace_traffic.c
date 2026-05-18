/*
 * td5_pilot_trace_traffic.c — Port-side CSV emitter for the traffic-physics
 * pilot (pool8). Mirrors tools/frida_pool8_traffic.js column-for-column so
 * diff_func_trace.py can pair rows by (sim_tick, slot, which_fn).
 *
 * Uses raw byte offsets (not struct-field access) so we are insulated from
 * port-side struct layout drift; offsets match what Frida reads from the
 * original binary.
 */
#include "td5_pilot_trace_traffic.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool8_traffic.csv"

/* Field offsets — must match frida_pool8_traffic.js exactly. */
#define O_ANG_VEL_YAW  0x1C4
#define O_LIN_VEL_X    0x1CC
#define O_LIN_VEL_Z    0x1D4
#define O_EULER_YAW    0x1F4
#define O_WORLD_POS_X  0x1FC
#define O_WORLD_POS_Z  0x204
#define O_WSP0         0x2DC
#define O_WSP1         0x2E0
#define O_WSDV0        0x2EC
#define O_WSDV1        0x2F0
#define O_STEER_CMD    0x30C
#define O_LONG_SPEED   0x314
#define O_ENC_STEER    0x33E

/* Traffic slot range (must match port's racer/traffic split) */
#define TRAFFIC_SLOT_MIN 6
#define TRAFFIC_SLOT_MAX 11

typedef struct {
    int32_t lin_vel_x, lin_vel_z;
    int32_t ang_vel_yaw, yaw_accum;
    int32_t world_pos_x, world_pos_z;
    int32_t long_speed;
    int32_t steer_cmd;
    int16_t enc_steer;
    int32_t pos0, vel0, pos1, vel1;
} TrafficSnap;

static FILE *s_fp_traffic = NULL;

/* Per-call state for the two emitters */
typedef struct {
    int       active;
    uint32_t  tick;
    int       paused;
    int       slot;
    const TD5_Actor *actor;
    TrafficSnap snap;
    int32_t   arg_lat;
    int32_t   arg_long;
} CallState;

static CallState s_state_friction;
static CallState s_state_susp;

extern int td5_trace_current_sim_tick(void);

static inline int16_t rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,which_fn,actor_addr,"
        "in_lin_vel_x,in_lin_vel_z,in_ang_vel_yaw,in_yaw_accum,"
        "in_world_pos_x,in_world_pos_z,in_long_speed,"
        "in_steering_cmd,in_encounter_steer,"
        "in_pos0,in_vel0,in_pos1,in_vel1,"
        "arg_lateral,arg_longitudinal,"
        "out_lin_vel_x,out_lin_vel_z,out_ang_vel_yaw,out_yaw_accum,"
        "out_world_pos_x,out_world_pos_z,out_long_speed,"
        "out_pos0,out_vel0,out_pos1,out_vel1\n");
}

static FILE *open_traffic_fp(void) {
    if (!s_fp_traffic) {
        s_fp_traffic = fopen(OUT_PATH, "w");
        if (s_fp_traffic) emit_header(s_fp_traffic);
    }
    return s_fp_traffic;
}

static void snap_actor_state(const TD5_Actor *actor, TrafficSnap *out) {
    const uint8_t *b = (const uint8_t *)actor;
    out->lin_vel_x   = rd_i32(b + O_LIN_VEL_X);
    out->lin_vel_z   = rd_i32(b + O_LIN_VEL_Z);
    out->ang_vel_yaw = rd_i32(b + O_ANG_VEL_YAW);
    out->yaw_accum   = rd_i32(b + O_EULER_YAW);
    out->world_pos_x = rd_i32(b + O_WORLD_POS_X);
    out->world_pos_z = rd_i32(b + O_WORLD_POS_Z);
    out->long_speed  = rd_i32(b + O_LONG_SPEED);
    out->steer_cmd   = rd_i32(b + O_STEER_CMD);
    out->enc_steer   = rd_i16(b + O_ENC_STEER);
    out->pos0        = rd_i32(b + O_WSP0);
    out->vel0        = rd_i32(b + O_WSDV0);
    out->pos1        = rd_i32(b + O_WSP1);
    out->vel1        = rd_i32(b + O_WSDV1);
}

static int actor_traffic_slot(const TD5_Actor *actor) {
    int slot = actor->slot_index;
    return slot;
}

static void emit_row(FILE *fp, const CallState *st, const char *which_fn,
                     const TrafficSnap *out) {
    fprintf(fp,
        "%u,%d,%d,%s,0x%p,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,%d\n",
        st->tick, st->paused, st->slot, which_fn, (const void *)st->actor,
        st->snap.lin_vel_x, st->snap.lin_vel_z, st->snap.ang_vel_yaw, st->snap.yaw_accum,
        st->snap.world_pos_x, st->snap.world_pos_z, st->snap.long_speed,
        st->snap.steer_cmd, st->snap.enc_steer,
        st->snap.pos0, st->snap.vel0, st->snap.pos1, st->snap.vel1,
        st->arg_lat, st->arg_long,
        out->lin_vel_x, out->lin_vel_z, out->ang_vel_yaw, out->yaw_accum,
        out->world_pos_x, out->world_pos_z, out->long_speed,
        out->pos0, out->vel0, out->pos1, out->vel1);
}

/* ===== friction (IntegrateVehicleFrictionForces) ========================= */

void td5_pilot_emit_traffic_friction_enter(const TD5_Actor *actor) {
    s_state_friction.active = 0;
    int slot = actor_traffic_slot(actor);
    if (slot < TRAFFIC_SLOT_MIN || slot > TRAFFIC_SLOT_MAX) return;
    FILE *fp = open_traffic_fp();
    if (!fp) return;
    s_state_friction.active   = 1;
    s_state_friction.tick     = (uint32_t)td5_trace_current_sim_tick();
    s_state_friction.paused   = 0;
    s_state_friction.slot     = slot;
    s_state_friction.actor    = actor;
    s_state_friction.arg_lat  = 0;
    s_state_friction.arg_long = 0;
    snap_actor_state(actor, &s_state_friction.snap);
}

void td5_pilot_emit_traffic_friction_leave(const TD5_Actor *actor) {
    if (!s_state_friction.active || actor != s_state_friction.actor) {
        s_state_friction.active = 0;
        return;
    }
    FILE *fp = s_fp_traffic;
    if (!fp) { s_state_friction.active = 0; return; }
    TrafficSnap out;
    snap_actor_state(actor, &out);
    emit_row(fp, &s_state_friction, "friction", &out);
    fflush(fp);
    s_state_friction.active = 0;
}

/* ===== susp (ApplyDampedSuspensionForce) ================================= */

void td5_pilot_emit_traffic_susp_enter(const TD5_Actor *actor,
                                       int32_t lateral, int32_t longitudinal) {
    s_state_susp.active = 0;
    int slot = actor_traffic_slot(actor);
    if (slot < TRAFFIC_SLOT_MIN || slot > TRAFFIC_SLOT_MAX) return;
    FILE *fp = open_traffic_fp();
    if (!fp) return;
    s_state_susp.active   = 1;
    s_state_susp.tick     = (uint32_t)td5_trace_current_sim_tick();
    s_state_susp.paused   = 0;
    s_state_susp.slot     = slot;
    s_state_susp.actor    = actor;
    s_state_susp.arg_lat  = lateral;
    s_state_susp.arg_long = longitudinal;
    snap_actor_state(actor, &s_state_susp.snap);
}

void td5_pilot_emit_traffic_susp_leave(const TD5_Actor *actor) {
    if (!s_state_susp.active || actor != s_state_susp.actor) {
        s_state_susp.active = 0;
        return;
    }
    FILE *fp = s_fp_traffic;
    if (!fp) { s_state_susp.active = 0; return; }
    TrafficSnap out;
    snap_actor_state(actor, &out);
    emit_row(fp, &s_state_susp, "susp", &out);
    fflush(fp);
    s_state_susp.active = 0;
}
