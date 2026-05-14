/*
 * td5_pilot_trace_00406980.c — Port-side CSV emitter for the WallResponse pilot.
 *
 * Mirrors tools/frida_pool7_00406980.js column-for-column. Diff key is
 * (sim_tick, slot, call_idx). Per-tick call_idx is reset when the tick changes.
 *
 * Reads actor fields via raw byte offsets (NOT struct member access) to stay
 * isolated from port-side struct layout drift; offsets match the Frida probe
 * which reads the original binary.
 */
#include "td5_pilot_trace_00406980.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define OUT_PATH "log/port/pool7_00406980.csv"

/* Actor field offsets — original-binary layout, must match the Frida probe. */
#define O_YAW   0x1C4
#define O_LV_X  0x1CC
#define O_LV_Z  0x1D4
#define O_POS_X 0x1FC
#define O_POS_Z 0x204
#define O_SLOT  0x375

static FILE *s_fp = NULL;
static int   s_active_call = 0;
static int   s_last_tick = -1;
static int   s_call_idx = 0;

/* Per-call captured state. */
static const TD5_Actor *s_actor = NULL;
static int      s_slot = 0;
static int      s_tick = 0;
static int      s_paused = 0;
static int      s_call_idx_current = 0;
static int32_t  s_force_x = 0, s_force_y = 0, s_force_z = 0;
static uint32_t s_angle = 0;
static int32_t  s_magnitude = 0;
static uint32_t s_flags = 0;
static int32_t  s_pre_lv_x = 0, s_pre_lv_z = 0, s_pre_yaw = 0;
static int32_t  s_pre_pos_x = 0, s_pre_pos_z = 0;

extern int td5_trace_current_sim_tick(void);

static inline int32_t rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint8_t  rd_u8 (const uint8_t *p) { return *p; }

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,call_idx,caller_ra,"
        "actor_addr,force_x_fp8,force_y_fp8,force_z_fp8,angle,magnitude,flags,"
        "pre_lv_x,pre_lv_z,pre_yaw,pre_pos_x,pre_pos_z,"
        "post_lv_x,post_lv_z,post_yaw,post_pos_x,post_pos_z,"
        "delta_lv_x,delta_lv_z,delta_yaw,delta_pos_x,delta_pos_z\n");
}

void td5_pilot_emit_00406980_enter(const TD5_Actor *actor,
                                    int32_t force_x_fp8,
                                    int32_t force_y_fp8,
                                    int32_t force_z_fp8,
                                    uint32_t angle,
                                    int32_t magnitude,
                                    uint32_t flags)
{
    if (!actor) { s_active_call = 0; return; }

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) { s_active_call = 0; return; }
        emit_header(s_fp);
    }

    const uint8_t *base = (const uint8_t *)actor;
    s_active_call = 1;
    s_actor       = actor;
    s_slot        = rd_u8(base + O_SLOT);
    s_tick        = td5_trace_current_sim_tick();
    s_paused      = 0;

    if (s_tick != s_last_tick) {
        s_last_tick = s_tick;
        s_call_idx = 0;
    }
    s_call_idx_current = s_call_idx++;

    s_force_x   = force_x_fp8;
    s_force_y   = force_y_fp8;
    s_force_z   = force_z_fp8;
    s_angle     = angle & 0xFFF;
    s_magnitude = magnitude;
    s_flags     = flags;

    s_pre_lv_x  = rd_i32(base + O_LV_X);
    s_pre_lv_z  = rd_i32(base + O_LV_Z);
    s_pre_yaw   = rd_i32(base + O_YAW);
    s_pre_pos_x = rd_i32(base + O_POS_X);
    s_pre_pos_z = rd_i32(base + O_POS_Z);
}

void td5_pilot_emit_00406980_leave(const TD5_Actor *actor) {
    if (!s_active_call || !s_fp || actor != s_actor) { s_active_call = 0; return; }

    const uint8_t *base = (const uint8_t *)actor;
    int32_t post_lv_x  = rd_i32(base + O_LV_X);
    int32_t post_lv_z  = rd_i32(base + O_LV_Z);
    int32_t post_yaw   = rd_i32(base + O_YAW);
    int32_t post_pos_x = rd_i32(base + O_POS_X);
    int32_t post_pos_z = rd_i32(base + O_POS_Z);

    /* caller_ra is unknowable from inside the C function; emit "0x0" so the
     * diff tool can still match by (tick, slot, call_idx) — the column is
     * informational only. */
    fprintf(s_fp,
        "%d,%d,%d,%d,0x0,"
        "0x%p,%d,%d,%d,%u,%d,%u,"
        "%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d\n",
        s_tick, s_paused, s_slot, s_call_idx_current,
        (const void *)actor,
        s_force_x, s_force_y, s_force_z, s_angle, s_magnitude, s_flags,
        s_pre_lv_x, s_pre_lv_z, s_pre_yaw, s_pre_pos_x, s_pre_pos_z,
        post_lv_x, post_lv_z, post_yaw, post_pos_x, post_pos_z,
        (post_lv_x - s_pre_lv_x), (post_lv_z - s_pre_lv_z),
        (post_yaw  - s_pre_yaw),
        (post_pos_x - s_pre_pos_x), (post_pos_z - s_pre_pos_z));
    fflush(s_fp);

    s_active_call = 0;
}
