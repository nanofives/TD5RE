/*
 * td5_pilot_trace_00434FE0.c -- Port-side CSV emitter for the
 * UpdateActorTrackBehavior pilot (pool9 / 0x00434FE0).
 *
 * Mirrors tools/frida_pool9_00434FE0.js column-for-column. Uses raw byte
 * offsets so the schema is insulated from port-side struct drift.
 */
#include "td5_pilot_trace_00434FE0.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define OUT_PATH "log/port/pool9_00434FE0.csv"

/* RS dword indices (mirror frida probe) */
#define RS_ROUTE_TABLE_PTR       0x00
#define RS_ROUTE_TABLE_SELECTOR  0x03
#define RS_TRACK_OFFSET_BIAS     0x09
#define RS_LEFT_DEVIATION        0x16
#define RS_RIGHT_DEVIATION       0x17
#define RS_FORWARD_TRACK_COMP    0x18
#define RS_TRACK_PROGRESS        0x19
#define RS_SLOT_INDEX            0x35
#define RS_SCRIPT_BASE_PTR       0x3A
#define RS_SCRIPT_IP             0x3B

/* Actor byte offsets */
#define O_SPAN_RAW         0x080
#define O_SPAN_NORMALIZED  0x082
#define O_YAW_ACCUM        0x1F4
#define O_WORLD_POS_X      0x1FC
#define O_WORLD_POS_Z      0x204
#define O_STEERING_CMD     0x30C

#define TARGET_SLOT  0

typedef struct {
    int32_t game_type;
    uint32_t rs_route_table_ptr;
    int32_t  rs_route_table_selector;
    int32_t  rs_slot_index;
    uint32_t rs_script_base_ptr;
    uint32_t rs_script_ip;
    int32_t  rs_track_progress;
    int32_t  rs_track_offset_bias;
    int32_t  rs_forward_track_comp;
    int32_t  rs_left_deviation;
    int32_t  rs_right_deviation;
    int16_t  span_raw;
    int16_t  span_normalized;
    int32_t  yaw_accum;
    int32_t  steering_cmd;
    int32_t  world_pos_x;
    int32_t  world_pos_z;
    int      route_byte_at_idx0;
    int      route_byte_at_idx1;
    int      is_canonical_route;
} PilotSnap;

static FILE     *s_fp = NULL;
static int       s_active = 0;
static uint32_t  s_tick = 0;
static int       s_slot = 0;
static PilotSnap s_inputs;

/* External: known LEFT.TRK pointer global from the port's td5_ai state. We
 * compare RS_ROUTE_TABLE_PTR against g_route_tables[0] (LEFT.TRK selector 0). */
extern int   td5_trace_current_sim_tick(void);
extern void *td5_ai_get_left_route_ptr(void);

static inline int16_t  rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t  rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }

static int read_route_byte(uint32_t rt_ptr_val, int span_norm, int byte_off) {
    if (rt_ptr_val == 0 || span_norm < 0) return -1;
    const uint8_t *p = (const uint8_t *)(uintptr_t)rt_ptr_val;
    return (int)p[(size_t)span_norm * 3u + (unsigned)byte_off];
}

static void snapshot(PilotSnap *s, const int32_t *rs, const void *actor, int32_t game_type) {
    const uint8_t *abase = (const uint8_t *)actor;
    s->game_type               = game_type;
    s->rs_route_table_ptr      = (uint32_t)rs[RS_ROUTE_TABLE_PTR];
    s->rs_route_table_selector = rs[RS_ROUTE_TABLE_SELECTOR];
    s->rs_slot_index           = rs[RS_SLOT_INDEX];
    s->rs_script_base_ptr      = (uint32_t)rs[RS_SCRIPT_BASE_PTR];
    s->rs_script_ip            = (uint32_t)rs[RS_SCRIPT_IP];
    s->rs_track_progress       = rs[RS_TRACK_PROGRESS];
    s->rs_track_offset_bias    = rs[RS_TRACK_OFFSET_BIAS];
    s->rs_forward_track_comp   = rs[RS_FORWARD_TRACK_COMP];
    s->rs_left_deviation       = rs[RS_LEFT_DEVIATION];
    s->rs_right_deviation      = rs[RS_RIGHT_DEVIATION];
    s->span_raw                = rd_i16(abase + O_SPAN_RAW);
    s->span_normalized         = rd_i16(abase + O_SPAN_NORMALIZED);
    s->yaw_accum               = rd_i32(abase + O_YAW_ACCUM);
    s->steering_cmd            = rd_i32(abase + O_STEERING_CMD);
    s->world_pos_x             = rd_i32(abase + O_WORLD_POS_X);
    s->world_pos_z             = rd_i32(abase + O_WORLD_POS_Z);
    s->route_byte_at_idx0      = read_route_byte(s->rs_route_table_ptr, s->span_normalized, 0);
    s->route_byte_at_idx1      = read_route_byte(s->rs_route_table_ptr, s->span_normalized, 1);
    {
        void *left = td5_ai_get_left_route_ptr();
        s->is_canonical_route = (s->rs_route_table_ptr == (uint32_t)(uintptr_t)left) ? 1 : 0;
    }
}

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,"
        "game_type,"
        "rs_route_table_ptr,rs_route_table_selector,rs_slot_index,"
        "rs_script_base_ptr_in,rs_script_ip_in,"
        "rs_track_progress_in,rs_track_offset_bias_in,"
        "rs_forward_track_comp_in,"
        "rs_left_deviation_in,rs_right_deviation_in,"
        "actor_span_raw,actor_span_normalized,"
        "actor_yaw_accum,actor_steering_cmd,"
        "actor_world_pos_x,actor_world_pos_z,"
        "route_byte_at_idx0,route_byte_at_idx1,"
        "left_route_ptr,is_canonical_route,"
        "rs_script_base_ptr_out,rs_script_ip_out,"
        "rs_track_progress_out,rs_track_offset_bias_out,"
        "rs_left_deviation_out,rs_right_deviation_out,"
        "actor_steering_cmd_out\n");
}

void td5_pilot_emit_00434FE0_enter(int slot, const int32_t *rs, const void *actor, int32_t game_type) {
    s_active = 0;
    if (slot != TARGET_SLOT || !rs || !actor) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    s_active = 1;
    s_slot   = slot;
    s_tick   = (uint32_t)td5_trace_current_sim_tick();
    snapshot(&s_inputs, rs, actor, game_type);
}

void td5_pilot_emit_00434FE0_leave(int slot, const int32_t *rs, const void *actor) {
    if (!s_active || !s_fp || slot != s_slot) {
        s_active = 0;
        return;
    }
    PilotSnap out;
    snapshot(&out, rs, actor, s_inputs.game_type);

    const char *paused_str = "0";

    fprintf(s_fp,
        "%u,%s,%d,"
        "%d,"
        "0x%x,%d,%d,"
        "0x%x,0x%x,"
        "%d,%d,"
        "%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "0x%x,%d,"
        "0x%x,0x%x,"
        "%d,%d,"
        "%d,%d,"
        "%d\n",
        s_tick, paused_str, s_slot,
        s_inputs.game_type,
        s_inputs.rs_route_table_ptr, s_inputs.rs_route_table_selector, s_inputs.rs_slot_index,
        s_inputs.rs_script_base_ptr, s_inputs.rs_script_ip,
        s_inputs.rs_track_progress, s_inputs.rs_track_offset_bias,
        s_inputs.rs_forward_track_comp,
        s_inputs.rs_left_deviation, s_inputs.rs_right_deviation,
        s_inputs.span_raw, s_inputs.span_normalized,
        s_inputs.yaw_accum, s_inputs.steering_cmd,
        s_inputs.world_pos_x, s_inputs.world_pos_z,
        s_inputs.route_byte_at_idx0, s_inputs.route_byte_at_idx1,
        (unsigned)(uintptr_t)td5_ai_get_left_route_ptr(), s_inputs.is_canonical_route,
        out.rs_script_base_ptr, out.rs_script_ip,
        out.rs_track_progress, out.rs_track_offset_bias,
        out.rs_left_deviation, out.rs_right_deviation,
        out.steering_cmd);
    fflush(s_fp);
    s_active = 0;
}
