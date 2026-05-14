/*
 * td5_pilot_trace_v2v.c -- Port-side CSV emitter for the pool14_v2v pilot
 *                          (ApplyVehicleCollisionImpulse 0x004079C0).
 *
 * Mirrors tools/frida_pool14_v2v.js column-for-column so the diff tool can
 * pair rows by (sim_tick, slotA, slotB, angle, impactForce).
 *
 * Output: log/port/pool14_v2v.csv
 */
#include "td5_pilot_trace_v2v.h"
#include "td5_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Match Frida's struct layout so the diff is byte-identical. */
typedef struct OBB_CornerData_TraceShim {
    int16_t proj_x;
    int16_t proj_z;
    int16_t pen_x;
    int16_t pen_z;
    int16_t own_x;
    int16_t own_z;
} OBB_CornerData_TraceShim;

#define OUT_PATH "log/port/pool14_v2v.csv"

/* Field offsets from byte-faithful actor struct.
 * Match tools/frida_pool14_v2v.js OFF_* constants exactly.
 * slot_index is at +0x375 (uint8) per re/include/td5_actor_struct.h. */
#define O_SLOT_IDX  0x375
#define O_OMEGA_YAW 0x1C4
#define O_VEL_X     0x1CC
#define O_VEL_Z     0x1D4
#define O_EULER_YAW 0x1F4
#define O_POS_X     0x1FC
#define O_POS_Z     0x204

typedef struct {
    int     slot;
    int32_t posX, posZ;
    int32_t velX, velZ;
    int32_t omega;
    int32_t eulerYaw;
} ActorSnap;

static FILE   *s_fp = NULL;
static int     s_active_call = 0;
static int32_t s_tick = 0;
static int32_t s_angle = 0;
static int32_t s_impactForce = 0;
static int16_t s_proj_x = 0, s_proj_z = 0, s_own_x = 0, s_own_z = 0;
static ActorSnap s_preA, s_preB;
static const TD5_Actor *s_A = NULL;
static const TD5_Actor *s_B = NULL;

extern int td5_trace_current_sim_tick(void);

static inline int32_t rd_i32(const uint8_t *p, int off) {
    int32_t v; memcpy(&v, p + off, sizeof v); return v;
}
static inline uint8_t rd_u8(const uint8_t *p, int off) {
    return p[off];
}

static void snap_actor(const TD5_Actor *act, ActorSnap *s) {
    const uint8_t *base = (const uint8_t *)act;
    s->slot     = rd_u8(base, O_SLOT_IDX);
    s->posX     = rd_i32(base, O_POS_X);
    s->posZ     = rd_i32(base, O_POS_Z);
    s->velX     = rd_i32(base, O_VEL_X);
    s->velZ     = rd_i32(base, O_VEL_Z);
    s->omega    = rd_i32(base, O_OMEGA_YAW);
    s->eulerYaw = rd_i32(base, O_EULER_YAW);
}

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,"
        "slotA,slotB,"
        "angle,impactForce,"
        "proj_x,proj_z,own_x,own_z,"
        "pre_posAx,pre_posAz,pre_velAx,pre_velAz,pre_omegaA,pre_eulerYawA,"
        "pre_posBx,pre_posBz,pre_velBx,pre_velBz,pre_omegaB,pre_eulerYawB,"
        "post_posAx,post_posAz,post_velAx,post_velAz,post_omegaA,post_eulerYawA,"
        "post_posBx,post_posBz,post_velBx,post_velBz,post_omegaB,post_eulerYawB,"
        "retval\n");
}

void td5_pilot_v2v_enter(const TD5_Actor *A, const TD5_Actor *B,
                         const OBB_CornerData *corner,
                         int32_t angle, int32_t impactForce)
{
    s_active_call = 0;
    if (!A || !B || !corner) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    const OBB_CornerData_TraceShim *cs =
        (const OBB_CornerData_TraceShim *)corner;

    s_active_call = 1;
    s_A           = A;
    s_B           = B;
    s_tick        = td5_trace_current_sim_tick();
    s_angle       = angle;
    s_impactForce = impactForce;
    s_proj_x      = cs->proj_x;
    s_proj_z      = cs->proj_z;
    s_own_x       = cs->own_x;
    s_own_z       = cs->own_z;
    snap_actor(A, &s_preA);
    snap_actor(B, &s_preB);
}

void td5_pilot_v2v_leave(const TD5_Actor *A, const TD5_Actor *B, int32_t retval)
{
    if (!s_active_call || !s_fp) { s_active_call = 0; return; }
    if (A != s_A || B != s_B)   { s_active_call = 0; return; }

    ActorSnap postA, postB;
    snap_actor(A, &postA);
    snap_actor(B, &postB);

    fprintf(s_fp,
        "%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,"
        "%d\n",
        s_tick,
        s_preA.slot, s_preB.slot,
        s_angle, s_impactForce,
        s_proj_x, s_proj_z, s_own_x, s_own_z,
        s_preA.posX,  s_preA.posZ,  s_preA.velX, s_preA.velZ, s_preA.omega, s_preA.eulerYaw,
        s_preB.posX,  s_preB.posZ,  s_preB.velX, s_preB.velZ, s_preB.omega, s_preB.eulerYaw,
        postA.posX,   postA.posZ,   postA.velX,  postA.velZ,  postA.omega,  postA.eulerYaw,
        postB.posX,   postB.posZ,   postB.velX,  postB.velZ,  postB.omega,  postB.eulerYaw,
        retval);
    fflush(s_fp);
    s_active_call = 0;
}
