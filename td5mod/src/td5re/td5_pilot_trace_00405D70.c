/*
 * td5_pilot_trace_00405D70.c -- Port-side CSV emitter for the
 * ResetVehicleActorState pilot (pool5 / 0x00405D70).
 *
 * Mirrors tools/frida_pool5_00405D70.js column-for-column. Uses raw byte
 * offsets (not struct field access) so the schema is insulated from port-
 * side struct drift and matches exactly what Frida reads from TD5_d3d.exe.
 *
 * Function fires only on spawn/respawn/recycle (rare, ~once per race) — no
 * performance gate is needed.
 */
#include "td5_pilot_trace_00405D70.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define OUT_PATH "log/port/pool5_00405D70.csv"

/* Actor field offsets — match frida_pool5_00405D70.js exactly. */
#define O_RENDER_POS    0x144     /* float[3] */
#define O_ANG_VEL_ROLL  0x1C0
#define O_ANG_VEL_YAW   0x1C4
#define O_ANG_VEL_PITCH 0x1C8
#define O_LIN_VEL_X     0x1CC
#define O_LIN_VEL_Y     0x1D0
#define O_LIN_VEL_Z     0x1D4
#define O_EULER_ROLL    0x1F0
#define O_EULER_YAW     0x1F4
#define O_EULER_PITCH   0x1F8
#define O_WORLD_POS_X   0x1FC
#define O_WORLD_POS_Y   0x200
#define O_WORLD_POS_Z   0x204
#define O_DISP_ROLL     0x208
#define O_DISP_YAW      0x20A
#define O_DISP_PITCH    0x20C
#define O_SUSP_POS      0x2DC
#define O_SPRING_DV     0x2EC
#define O_LOAD_ACCUM    0x2FC
#define O_ENGINE        0x310
#define O_FRAME_COUNTER 0x338
#define O_GEAR          0x36B
#define O_SURF_FLAGS    0x376
#define O_VEH_MODE      0x379
#define O_WCB           0x37C
#define O_DAM_LOCKOUT   0x37D

#define TARGET_SLOT     0

static FILE             *s_fp = NULL;
static int               s_active_call = 0;
static const TD5_Actor  *s_actor = NULL;

/* PRE-state stash (captured at enter, emitted at leave). */
static int32_t  s_world_pos_x_pre;
static int32_t  s_world_pos_y_pre;
static int32_t  s_world_pos_z_pre;
static int32_t  s_euler_yaw_pre;
static uint8_t  s_dam_lockout_pre;

/* sim_tick + slot for keying. */
static uint32_t s_tick;
static int      s_slot;
static char     s_actor_addr[20];

extern int td5_trace_current_sim_tick(void);

static inline int16_t  rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t  rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint8_t  rd_u8 (const uint8_t *p) { return *p; }
static inline float    rd_f32(const uint8_t *p) { float v; memcpy(&v, p, sizeof v); return v; }

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,caller_ra,actor_addr,"
        "world_pos_x_pre,world_pos_y_pre,world_pos_z_pre,"
        "euler_yaw_pre,dam_lockout_pre,"
        "surface_contact_flags,vehicle_mode,"
        "ang_vel_roll,ang_vel_yaw,ang_vel_pitch,"
        "lin_vel_x,lin_vel_y,lin_vel_z,"
        "frame_counter,wheel_contact_bitmask,damage_lockout,"
        "world_pos_y_post,current_gear,engine_speed,"
        "susp_pos_0,susp_pos_1,susp_pos_2,susp_pos_3,"
        "spring_dv_0,spring_dv_1,spring_dv_2,spring_dv_3,"
        "load_accum_0,load_accum_1,load_accum_2,load_accum_3,"
        "render_pos_x,render_pos_y,render_pos_z,"
        "euler_roll_post,euler_yaw_post,euler_pitch_post,"
        "disp_roll,disp_yaw,disp_pitch\n");
}

static int actor_slot_index(const TD5_Actor *actor) {
    return actor->slot_index;
}

void td5_pilot_trace_00405D70_enter(const TD5_Actor *actor) {
    s_active_call = 0;
    if (!actor) return;
    int slot = actor_slot_index(actor);
    if (slot != TARGET_SLOT) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    s_active_call = 1;
    s_actor = actor;
    s_slot  = slot;
    s_tick  = (uint32_t)td5_trace_current_sim_tick();
    snprintf(s_actor_addr, sizeof s_actor_addr, "0x%p", (const void *)actor);

    const uint8_t *base = (const uint8_t *)actor;
    s_world_pos_x_pre = rd_i32(base + O_WORLD_POS_X);
    s_world_pos_y_pre = rd_i32(base + O_WORLD_POS_Y);
    s_world_pos_z_pre = rd_i32(base + O_WORLD_POS_Z);
    s_euler_yaw_pre   = rd_i32(base + O_EULER_YAW);
    s_dam_lockout_pre = rd_u8 (base + O_DAM_LOCKOUT);
}

void td5_pilot_trace_00405D70_leave(const TD5_Actor *actor) {
    if (!s_active_call || !s_fp || actor != s_actor) {
        s_active_call = 0;
        return;
    }
    const uint8_t *base = (const uint8_t *)actor;

    fprintf(s_fp,
        "%u,0,%d,0x0,%s,"            /* tick,paused,slot,caller_ra,actor_addr */
        "%d,%d,%d,"                  /* world_pos_*_pre */
        "%d,%u,"                     /* euler_yaw_pre, dam_lockout_pre */
        "%u,%u,"                     /* surface_contact_flags, vehicle_mode */
        "%d,%d,%d,"                  /* ang_vel */
        "%d,%d,%d,"                  /* lin_vel */
        "%d,%u,%u,"                  /* frame_counter, wcb, dam_lockout */
        "%d,%u,%d,"                  /* world_pos_y_post, gear, engine */
        "%d,%d,%d,%d,"               /* susp_pos[0..3] */
        "%d,%d,%d,%d,"               /* spring_dv[0..3] */
        "%d,%d,%d,%d,"               /* load_accum[0..3] */
        "%.9g,%.9g,%.9g,"            /* render_pos */
        "%d,%d,%d,"                  /* euler post */
        "%d,%d,%d\n",                /* disp angles */
        s_tick, s_slot, s_actor_addr,
        s_world_pos_x_pre, s_world_pos_y_pre, s_world_pos_z_pre,
        s_euler_yaw_pre, (unsigned)s_dam_lockout_pre,
        (unsigned)rd_u8 (base + O_SURF_FLAGS),
        (unsigned)rd_u8 (base + O_VEH_MODE),
        rd_i32(base + O_ANG_VEL_ROLL),
        rd_i32(base + O_ANG_VEL_YAW),
        rd_i32(base + O_ANG_VEL_PITCH),
        rd_i32(base + O_LIN_VEL_X),
        rd_i32(base + O_LIN_VEL_Y),
        rd_i32(base + O_LIN_VEL_Z),
        (int)rd_i16(base + O_FRAME_COUNTER),
        (unsigned)rd_u8(base + O_WCB),
        (unsigned)rd_u8(base + O_DAM_LOCKOUT),
        rd_i32(base + O_WORLD_POS_Y),
        (unsigned)rd_u8(base + O_GEAR),
        rd_i32(base + O_ENGINE),
        rd_i32(base + O_SUSP_POS  + 0),
        rd_i32(base + O_SUSP_POS  + 4),
        rd_i32(base + O_SUSP_POS  + 8),
        rd_i32(base + O_SUSP_POS  + 12),
        rd_i32(base + O_SPRING_DV + 0),
        rd_i32(base + O_SPRING_DV + 4),
        rd_i32(base + O_SPRING_DV + 8),
        rd_i32(base + O_SPRING_DV + 12),
        rd_i32(base + O_LOAD_ACCUM + 0),
        rd_i32(base + O_LOAD_ACCUM + 4),
        rd_i32(base + O_LOAD_ACCUM + 8),
        rd_i32(base + O_LOAD_ACCUM + 12),
        rd_f32(base + O_RENDER_POS + 0),
        rd_f32(base + O_RENDER_POS + 4),
        rd_f32(base + O_RENDER_POS + 8),
        rd_i32(base + O_EULER_ROLL),
        rd_i32(base + O_EULER_YAW),
        rd_i32(base + O_EULER_PITCH),
        (int)rd_i16(base + O_DISP_ROLL),
        (int)rd_i16(base + O_DISP_YAW),
        (int)rd_i16(base + O_DISP_PITCH));
    fflush(s_fp);
    s_active_call = 0;
}
