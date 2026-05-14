/*
 * td5_pilot_trace_00405E80.c -- Port-side CSV emitter for the precise-port
 * pilot targeting 0x00405E80 IntegrateVehiclePoseAndContacts.
 *
 * One row per call (slot 0 only), captures function-entry inputs + function-
 * exit outputs side-by-side. Mirrors tools/frida_pool2_00405E80.js exactly so
 * tools/diff_func_trace.py can pair rows by (sim_tick, caller_ra).
 *
 * Field reads use raw byte offsets (not struct member access) so this module
 * is insulated from struct layout drift in the port.
 */
#include "td5_pilot_trace_00405E80.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool2_00405E80.csv"

/* Field offsets — verified against re/include/td5_actor_struct.h and the
 * Ghidra listing of 0x00405E80. */
#define O_TRACK_SPAN     0x80
#define O_ROT_MATRIX     0x120     /* 9 floats */
#define O_CARDEF_PTR     0x1B8
#define O_AV_ROLL        0x1C0     /* angular_velocity_roll  int32 */
#define O_AV_YAW         0x1C4     /* angular_velocity_yaw   int32 */
#define O_AV_PITCH       0x1C8     /* angular_velocity_pitch int32 */
#define O_LV_X           0x1CC     /* linear_velocity_x      int32 */
#define O_LV_Y           0x1D0
#define O_LV_Z           0x1D4
#define O_EU_ROLL        0x1F0     /* euler_accum.roll       int32 */
#define O_EU_YAW         0x1F4
#define O_EU_PITCH       0x1F8
#define O_WORLD_X        0x1FC
#define O_WORLD_Y        0x200
#define O_WORLD_Z        0x204
#define O_DISP_ROLL      0x208     /* int16 */
#define O_DISP_YAW       0x20A
#define O_DISP_PITCH     0x20C
#define O_PREV_FRAME_Y   0x2D8
#define O_AFC            0x360     /* uint16 — airborne_frame_counter */
#define O_WCB_NEW        0x37C     /* uint8  — wheel_contact_bitmask */
#define O_WCB_OLD        0x37D     /* uint8  — damage_lockout */
#define O_RENDER_X       0x144     /* 3 floats — render_pos */
#define O_RENDER_Y       0x148
#define O_RENDER_Z       0x14C

#define G_CARDEF_OFF     0x82
#define TARGET_SLOT      0

typedef struct {
    /* Inputs */
    int32_t  world_pos_x, world_pos_y, world_pos_z;
    int32_t  lin_vel_x, lin_vel_y, lin_vel_z;
    int32_t  ang_vel_roll, ang_vel_yaw, ang_vel_pitch;
    int32_t  eul_roll, eul_yaw, eul_pitch;
    int16_t  disp_roll, disp_yaw, disp_pitch;
    int32_t  prev_frame_y;
    uint16_t afc;
    uint8_t  wcb_new, wcb_old;
    int16_t  track_span;
    int16_t  cardef_0x82;
} PilotInputSnap;

static FILE        *s_fp = NULL;
static int          s_active_call = 0;
static uintptr_t    s_caller_ra = 0;
static uint32_t     s_tick = 0;
static int          s_paused = 0;
static int          s_slot = 0;
static const TD5_Actor *s_actor = NULL;
static PilotInputSnap   s_inputs;

extern int td5_trace_current_sim_tick(void);

static inline int16_t  rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t  rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint16_t rd_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint8_t  rd_u8 (const uint8_t *p) { return *p; }
static inline float    rd_f32(const uint8_t *p) { float v; memcpy(&v, p, sizeof v); return v; }
static inline uintptr_t rd_ptr(const uint8_t *p) { uintptr_t v; memcpy(&v, p, sizeof v); return v; }

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,caller_ra,"
        /* inputs */
        "in_world_x,in_world_y,in_world_z,"
        "in_lvx,in_lvy,in_lvz,"
        "in_avr,in_avy,in_avp,"
        "in_eur,in_euy,in_eup,"
        "in_dr,in_dy,in_dp,"
        "in_prev_y,in_afc,in_wcb,in_dlk,"
        "in_track_span,in_cardef_0x82,"
        /* outputs */
        "out_world_x,out_world_y,out_world_z,"
        "out_lvx,out_lvy,out_lvz,"
        "out_avr,out_avy,out_avp,"
        "out_eur,out_euy,out_eup,"
        "out_dr,out_dy,out_dp,"
        "out_prev_y,out_afc,out_wcb,out_dlk,"
        "out_track_span,"
        "out_rot0,out_rot1,out_rot2,out_rot3,out_rot4,"
        "out_rot5,out_rot6,out_rot7,out_rot8,"
        "out_render_x,out_render_y,out_render_z\n");
}

static int actor_slot_index(const TD5_Actor *actor) {
    return actor->slot_index;
}

static void snap_inputs(const TD5_Actor *actor, PilotInputSnap *out) {
    const uint8_t *base = (const uint8_t *)actor;
    out->world_pos_x   = rd_i32(base + O_WORLD_X);
    out->world_pos_y   = rd_i32(base + O_WORLD_Y);
    out->world_pos_z   = rd_i32(base + O_WORLD_Z);
    out->lin_vel_x     = rd_i32(base + O_LV_X);
    out->lin_vel_y     = rd_i32(base + O_LV_Y);
    out->lin_vel_z     = rd_i32(base + O_LV_Z);
    out->ang_vel_roll  = rd_i32(base + O_AV_ROLL);
    out->ang_vel_yaw   = rd_i32(base + O_AV_YAW);
    out->ang_vel_pitch = rd_i32(base + O_AV_PITCH);
    out->eul_roll      = rd_i32(base + O_EU_ROLL);
    out->eul_yaw       = rd_i32(base + O_EU_YAW);
    out->eul_pitch     = rd_i32(base + O_EU_PITCH);
    out->disp_roll     = rd_i16(base + O_DISP_ROLL);
    out->disp_yaw      = rd_i16(base + O_DISP_YAW);
    out->disp_pitch    = rd_i16(base + O_DISP_PITCH);
    out->prev_frame_y  = rd_i32(base + O_PREV_FRAME_Y);
    out->afc           = rd_u16(base + O_AFC);
    out->wcb_new       = rd_u8 (base + O_WCB_NEW);
    out->wcb_old       = rd_u8 (base + O_WCB_OLD);
    out->track_span    = rd_i16(base + O_TRACK_SPAN);

    uintptr_t cardef = rd_ptr(base + O_CARDEF_PTR);
    out->cardef_0x82 = cardef ? rd_i16((const uint8_t *)cardef + G_CARDEF_OFF) : 0;
}

void td5_pilot_emit_00405E80_enter(const TD5_Actor *actor, uintptr_t caller_ra) {
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
    snap_inputs(actor, &s_inputs);
}

void td5_pilot_emit_00405E80_leave(const TD5_Actor *actor) {
    if (!s_active_call || !s_fp || actor != s_actor) {
        s_active_call = 0;
        return;
    }
    const uint8_t *base = (const uint8_t *)actor;

    int32_t  out_world_x = rd_i32(base + O_WORLD_X);
    int32_t  out_world_y = rd_i32(base + O_WORLD_Y);
    int32_t  out_world_z = rd_i32(base + O_WORLD_Z);
    int32_t  out_lvx     = rd_i32(base + O_LV_X);
    int32_t  out_lvy     = rd_i32(base + O_LV_Y);
    int32_t  out_lvz     = rd_i32(base + O_LV_Z);
    int32_t  out_avr     = rd_i32(base + O_AV_ROLL);
    int32_t  out_avy     = rd_i32(base + O_AV_YAW);
    int32_t  out_avp     = rd_i32(base + O_AV_PITCH);
    int32_t  out_eur     = rd_i32(base + O_EU_ROLL);
    int32_t  out_euy     = rd_i32(base + O_EU_YAW);
    int32_t  out_eup     = rd_i32(base + O_EU_PITCH);
    int16_t  out_dr      = rd_i16(base + O_DISP_ROLL);
    int16_t  out_dy      = rd_i16(base + O_DISP_YAW);
    int16_t  out_dp      = rd_i16(base + O_DISP_PITCH);
    int32_t  out_prev_y  = rd_i32(base + O_PREV_FRAME_Y);
    uint16_t out_afc     = rd_u16(base + O_AFC);
    uint8_t  out_wcb     = rd_u8 (base + O_WCB_NEW);
    uint8_t  out_dlk     = rd_u8 (base + O_WCB_OLD);
    int16_t  out_track   = rd_i16(base + O_TRACK_SPAN);
    float    rot[9];
    for (int i = 0; i < 9; i++) rot[i] = rd_f32(base + O_ROT_MATRIX + i * 4);
    float    rxf = rd_f32(base + O_RENDER_X);
    float    ryf = rd_f32(base + O_RENDER_Y);
    float    rzf = rd_f32(base + O_RENDER_Z);

    fprintf(s_fp,
        "%u,%d,%d,0x%lx,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%u,%u,%u,"
        "%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%u,%u,%u,"
        "%d,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f\n",
        s_tick, s_paused, s_slot, (unsigned long)s_caller_ra,
        /* inputs */
        s_inputs.world_pos_x, s_inputs.world_pos_y, s_inputs.world_pos_z,
        s_inputs.lin_vel_x, s_inputs.lin_vel_y, s_inputs.lin_vel_z,
        s_inputs.ang_vel_roll, s_inputs.ang_vel_yaw, s_inputs.ang_vel_pitch,
        s_inputs.eul_roll, s_inputs.eul_yaw, s_inputs.eul_pitch,
        s_inputs.disp_roll, s_inputs.disp_yaw, s_inputs.disp_pitch,
        s_inputs.prev_frame_y, s_inputs.afc, s_inputs.wcb_new, s_inputs.wcb_old,
        s_inputs.track_span, s_inputs.cardef_0x82,
        /* outputs */
        out_world_x, out_world_y, out_world_z,
        out_lvx, out_lvy, out_lvz,
        out_avr, out_avy, out_avp,
        out_eur, out_euy, out_eup,
        out_dr, out_dy, out_dp,
        out_prev_y, out_afc, out_wcb, out_dlk,
        out_track,
        rot[0], rot[1], rot[2], rot[3], rot[4],
        rot[5], rot[6], rot[7], rot[8],
        rxf, ryf, rzf);
    fflush(s_fp);
    s_active_call = 0;
}
