/*
 * td5_pilot_trace_004057F0.c -- Port-side CSV emitter for
 * UpdateVehicleSuspensionResponse @ 0x004057F0 (precise-port pilot pool6).
 *
 * Mirrors tools/frida_pool6_004057F0.js column-for-column so the diff tool
 * can pair rows by (sim_tick, slot, caller_ra) and report which column
 * diverges first.
 *
 * Uses raw byte offsets, not struct field access, so we are insulated from
 * port-side struct layout drift. Offsets match what Frida reads from the
 * original binary.
 */
#include "td5_pilot_trace_004057F0.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool6_004057F0.csv"

/* Field offsets — matches frida_pool6_004057F0.js exactly. */
#define O_ROT        0x120
#define O_AV_ROLL    0x1C0
#define O_AV_PITCH   0x1C8
#define O_VY         0x1D0
#define O_ARMS       0x210
#define O_WCV        0x250
#define O_WFDH       0x270
#define O_WCB        0x37C
#define O_WCB_OLD    0x37D

#define G_GRAV_ADDR  0x00467380   /* gGravityConstant in original */

#define TARGET_SLOT  0

/* Captured at entry, replayed at exit. */
typedef struct {
    int16_t lat, loni;
    int16_t wcv_x, wcv_y, wcv_z;
    int16_t wfdh_x, wfdh_y, wfdh_z;
} PilotSuspWheelSnap;

static FILE        *s_fp = NULL;
static int          s_active_call = 0;
static uintptr_t    s_caller_ra = 0;
static uint32_t     s_tick = 0;
static int          s_paused = 0;
static int          s_slot = 0;
static const TD5_Actor *s_actor = NULL;

static uint8_t      s_lock = 0;
static uint8_t      s_prev_air = 0;
static int32_t      s_gravity = 0;
static float        s_rot[9];
static PilotSuspWheelSnap s_wheels[4];
static int32_t      s_pre_av_roll = 0;
static int32_t      s_pre_av_pitch = 0;
static int32_t      s_pre_vy = 0;

/* Forward decl from td5_game.c. */
extern int td5_trace_current_sim_tick(void);

static inline int16_t  rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t  rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint8_t  rd_u8 (const uint8_t *p) { return *p; }
static inline float    rd_f32(const uint8_t *p) { float v; memcpy(&v, p, sizeof v); return v; }

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,caller_ra,"
        "lock,prev_air,gravity,"
        "rot0,rot1,rot2,rot3,rot4,rot5,rot6,rot7,rot8,"
        "w0_lat,w0_loni,w0_wcv_x,w0_wcv_y,w0_wcv_z,w0_wfdh_x,w0_wfdh_y,w0_wfdh_z,"
        "w1_lat,w1_loni,w1_wcv_x,w1_wcv_y,w1_wcv_z,w1_wfdh_x,w1_wfdh_y,w1_wfdh_z,"
        "w2_lat,w2_loni,w2_wcv_x,w2_wcv_y,w2_wcv_z,w2_wfdh_x,w2_wfdh_y,w2_wfdh_z,"
        "w3_lat,w3_loni,w3_wcv_x,w3_wcv_y,w3_wcv_z,w3_wfdh_x,w3_wfdh_y,w3_wfdh_z,"
        "pre_av_roll,pre_av_pitch,pre_vy,"
        "post_av_roll,post_av_pitch,post_vy,"
        "d_av_roll,d_av_pitch,d_vy\n");
}

static int actor_slot_index(const TD5_Actor *actor) {
    return actor->slot_index;
}

void td5_pilot_emit_004057F0_enter(const TD5_Actor *actor, uintptr_t caller_ra, int32_t gravity) {
    s_active_call = 0;
    int slot = actor_slot_index(actor);
    if (slot != TARGET_SLOT) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    s_active_call = 1;
    s_caller_ra   = caller_ra;
    s_tick        = (uint32_t)td5_trace_current_sim_tick();
    s_paused      = 0;
    s_slot        = slot;
    s_actor       = actor;

    const uint8_t *base = (const uint8_t *)actor;
    s_lock     = rd_u8 (base + O_WCB);
    s_prev_air = rd_u8 (base + O_WCB_OLD);
    s_gravity  = gravity;
    for (int i = 0; i < 9; i++)
        s_rot[i] = rd_f32(base + O_ROT + i * 4);

    for (int w = 0; w < 4; w++) {
        const uint8_t *a = base + O_ARMS + w * 8;
        const uint8_t *v = base + O_WCV  + w * 8;
        const uint8_t *f = base + O_WFDH + w * 8;
        s_wheels[w].lat    = rd_i16(a + 0);
        s_wheels[w].loni   = rd_i16(a + 4);
        s_wheels[w].wcv_x  = rd_i16(v + 0);
        s_wheels[w].wcv_y  = rd_i16(v + 2);
        s_wheels[w].wcv_z  = rd_i16(v + 4);
        s_wheels[w].wfdh_x = rd_i16(f + 0);
        s_wheels[w].wfdh_y = rd_i16(f + 2);
        s_wheels[w].wfdh_z = rd_i16(f + 4);
    }

    s_pre_av_roll  = rd_i32(base + O_AV_ROLL );
    s_pre_av_pitch = rd_i32(base + O_AV_PITCH);
    s_pre_vy       = rd_i32(base + O_VY      );
}

void td5_pilot_emit_004057F0_leave(const TD5_Actor *actor) {
    if (!s_active_call || !s_fp || actor != s_actor) {
        s_active_call = 0;
        return;
    }
    const uint8_t *base = (const uint8_t *)actor;
    int32_t post_av_roll  = rd_i32(base + O_AV_ROLL );
    int32_t post_av_pitch = rd_i32(base + O_AV_PITCH);
    int32_t post_vy       = rd_i32(base + O_VY      );

    fprintf(s_fp,
        "%u,%d,%d,0x%lx,"
        "%u,%u,%d,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%d,%d,%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d\n",
        s_tick, s_paused, s_slot, (unsigned long)s_caller_ra,
        (unsigned)s_lock, (unsigned)s_prev_air, s_gravity,
        s_rot[0], s_rot[1], s_rot[2], s_rot[3], s_rot[4],
        s_rot[5], s_rot[6], s_rot[7], s_rot[8],
        s_wheels[0].lat, s_wheels[0].loni,
        s_wheels[0].wcv_x, s_wheels[0].wcv_y, s_wheels[0].wcv_z,
        s_wheels[0].wfdh_x, s_wheels[0].wfdh_y, s_wheels[0].wfdh_z,
        s_wheels[1].lat, s_wheels[1].loni,
        s_wheels[1].wcv_x, s_wheels[1].wcv_y, s_wheels[1].wcv_z,
        s_wheels[1].wfdh_x, s_wheels[1].wfdh_y, s_wheels[1].wfdh_z,
        s_wheels[2].lat, s_wheels[2].loni,
        s_wheels[2].wcv_x, s_wheels[2].wcv_y, s_wheels[2].wcv_z,
        s_wheels[2].wfdh_x, s_wheels[2].wfdh_y, s_wheels[2].wfdh_z,
        s_wheels[3].lat, s_wheels[3].loni,
        s_wheels[3].wcv_x, s_wheels[3].wcv_y, s_wheels[3].wcv_z,
        s_wheels[3].wfdh_x, s_wheels[3].wfdh_y, s_wheels[3].wfdh_z,
        s_pre_av_roll, s_pre_av_pitch, s_pre_vy,
        post_av_roll, post_av_pitch, post_vy,
        post_av_roll  - s_pre_av_roll,
        post_av_pitch - s_pre_av_pitch,
        post_vy       - s_pre_vy);
    fflush(s_fp);
    s_active_call = 0;
}
