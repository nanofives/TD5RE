/*
 * td5_pilot_trace_00405B40.c -- Port-side CSV emitter for
 * ClampVehicleAttitudeLimits (pool4 / session 20).
 *
 * Mirrors tools/frida_pool4_00405B40.js column-for-column.
 * Uses raw byte offsets to be insulated from port-side struct layout drift.
 */
#include "td5_pilot_trace_00405B40.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool4_00405B40.csv"

/* Field offsets (byte) inside actor. */
#define O_ROTATION_M     0x120
#define O_SAVED_ORIENT_M 0x150
#define O_COLLISION_M    0x180
#define O_ANG_VEL_ROLL   0x1C0
#define O_ANG_VEL_YAW    0x1C4
#define O_ANG_VEL_PITCH  0x1C8
#define O_EUL_ROLL       0x1F0
#define O_EUL_YAW        0x1F4
#define O_EUL_PITCH      0x1F8
#define O_DISP_ROLL      0x208
#define O_DISP_YAW       0x20A
#define O_DISP_PITCH     0x20C
#define O_FRAME_COUNTER  0x338
#define O_VEHICLE_MODE   0x379

#define TARGET_SLOT 0

/* Globals — we read this from td5_physics.c (file-static there, so we can't
 * touch it from here). The Frida side reads the original memory value at
 * 0x00463188 directly. To keep parity here we re-read it via the same
 * helper that the port uses: a small accessor exposed by td5_physics.c. */
extern int td5_physics_get_collisions_flag(void);

extern int td5_trace_current_sim_tick(void);

static FILE *s_fp = NULL;

static inline int16_t  rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t  rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint8_t  rd_u8 (const uint8_t *p) { return *p; }
static inline float    rd_f32(const uint8_t *p) { float v; memcpy(&v, p, sizeof v); return v; }

/* Pre-call snapshot. */
typedef struct {
    int32_t  collisions_flag;
    uint16_t disp_roll_raw, disp_pitch_raw;
    int32_t  signed_roll, signed_pitch;
    int32_t  eul_roll, eul_yaw, eul_pitch;
    int32_t  omega_roll, omega_yaw, omega_pitch;
    uint8_t  vehicle_mode_pre;
    int16_t  frame_counter_pre;
    float    rotation_pre[9];
} PilotSnap00405B40;

static PilotSnap00405B40 s_snap;
static int               s_active = 0;
static uintptr_t         s_caller_ra = 0;
static uint32_t          s_tick = 0;
static const TD5_Actor  *s_actor = NULL;

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,slot,caller_ra,collisions_flag,"
        "disp_roll_raw,disp_pitch_raw,signed_roll,signed_pitch,"
        "eul_roll_pre,eul_yaw_pre,eul_pitch_pre,"
        "omega_roll_pre,omega_yaw_pre,omega_pitch_pre,"
        "vehicle_mode_pre,frame_counter_pre,"
        "branch_taken,"
        "eul_roll_post,eul_pitch_post,"
        "omega_roll_post,omega_pitch_post,"
        "disp_roll_post,disp_pitch_post,"
        "vehicle_mode_post,frame_counter_post,"
        "rot_pre00,rot_pre01,rot_pre02,rot_pre03,rot_pre04,rot_pre05,rot_pre06,rot_pre07,rot_pre08,"
        "saved_post00,saved_post01,saved_post02,saved_post03,saved_post04,saved_post05,saved_post06,saved_post07,saved_post08,"
        "collision_post00,collision_post01,collision_post02,collision_post03,collision_post04,collision_post05,collision_post06,collision_post07,collision_post08\n");
}

void td5_pilot_emit_00405B40_enter(const TD5_Actor *actor, uintptr_t caller_ra) {
    s_active = 0;
    if (!actor) return;
    if (actor->slot_index != TARGET_SLOT) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    s_active    = 1;
    s_caller_ra = caller_ra;
    s_tick      = (uint32_t)td5_trace_current_sim_tick();
    s_actor     = actor;

    const uint8_t *base = (const uint8_t *)actor;

    s_snap.collisions_flag   = td5_physics_get_collisions_flag();
    s_snap.disp_roll_raw     = (uint16_t)rd_i16(base + O_DISP_ROLL);
    s_snap.disp_pitch_raw    = (uint16_t)rd_i16(base + O_DISP_PITCH);
    s_snap.signed_roll       = ((((int32_t)s_snap.disp_roll_raw)  - 0x800) & 0xFFF) - 0x800;
    s_snap.signed_pitch      = ((((int32_t)s_snap.disp_pitch_raw) - 0x800) & 0xFFF) - 0x800;
    s_snap.eul_roll          = rd_i32(base + O_EUL_ROLL);
    s_snap.eul_yaw           = rd_i32(base + O_EUL_YAW);
    s_snap.eul_pitch         = rd_i32(base + O_EUL_PITCH);
    s_snap.omega_roll        = rd_i32(base + O_ANG_VEL_ROLL);
    s_snap.omega_yaw         = rd_i32(base + O_ANG_VEL_YAW);
    s_snap.omega_pitch       = rd_i32(base + O_ANG_VEL_PITCH);
    s_snap.vehicle_mode_pre  = rd_u8 (base + O_VEHICLE_MODE);
    s_snap.frame_counter_pre = rd_i16(base + O_FRAME_COUNTER);
    for (int i = 0; i < 9; i++)
        s_snap.rotation_pre[i] = rd_f32(base + O_ROTATION_M + i * 4);
}

void td5_pilot_emit_00405B40_leave(const TD5_Actor *actor, int branch_taken) {
    if (!s_active || !s_fp || actor != s_actor) {
        s_active = 0;
        return;
    }
    const uint8_t *base = (const uint8_t *)actor;

    int32_t eul_roll_post   = rd_i32(base + O_EUL_ROLL);
    int32_t eul_pitch_post  = rd_i32(base + O_EUL_PITCH);
    int32_t omega_roll_p    = rd_i32(base + O_ANG_VEL_ROLL);
    int32_t omega_pitch_p   = rd_i32(base + O_ANG_VEL_PITCH);
    int16_t disp_roll_post  = rd_i16(base + O_DISP_ROLL);
    int16_t disp_pitch_post = rd_i16(base + O_DISP_PITCH);
    uint8_t vm_post         = rd_u8 (base + O_VEHICLE_MODE);
    int16_t fc_post         = rd_i16(base + O_FRAME_COUNTER);

    float saved_post[9], collision_post[9];
    for (int i = 0; i < 9; i++) {
        saved_post[i]     = rd_f32(base + O_SAVED_ORIENT_M + i * 4);
        collision_post[i] = rd_f32(base + O_COLLISION_M    + i * 4);
    }

    fprintf(s_fp,
        "%u,%d,0x%lx,%d,"
        "%u,%u,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%u,%d,"
        "%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%u,%d,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
        s_tick, actor->slot_index, (unsigned long)s_caller_ra, s_snap.collisions_flag,
        s_snap.disp_roll_raw, s_snap.disp_pitch_raw, s_snap.signed_roll, s_snap.signed_pitch,
        s_snap.eul_roll, s_snap.eul_yaw, s_snap.eul_pitch,
        s_snap.omega_roll, s_snap.omega_yaw, s_snap.omega_pitch,
        s_snap.vehicle_mode_pre, s_snap.frame_counter_pre,
        branch_taken,
        eul_roll_post, eul_pitch_post,
        omega_roll_p, omega_pitch_p,
        disp_roll_post, disp_pitch_post,
        vm_post, fc_post,
        s_snap.rotation_pre[0], s_snap.rotation_pre[1], s_snap.rotation_pre[2],
        s_snap.rotation_pre[3], s_snap.rotation_pre[4], s_snap.rotation_pre[5],
        s_snap.rotation_pre[6], s_snap.rotation_pre[7], s_snap.rotation_pre[8],
        saved_post[0], saved_post[1], saved_post[2],
        saved_post[3], saved_post[4], saved_post[5],
        saved_post[6], saved_post[7], saved_post[8],
        collision_post[0], collision_post[1], collision_post[2],
        collision_post[3], collision_post[4], collision_post[5],
        collision_post[6], collision_post[7], collision_post[8]);

    if ((s_tick & 0x3F) == 0)
        fflush(s_fp);

    s_active = 0;
}
