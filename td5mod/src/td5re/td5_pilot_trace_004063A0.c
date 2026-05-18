/*
 * td5_pilot_trace_004063A0.c — Port-side CSV emitter mirroring
 * tools/frida_pool3_004063A0.js for UpdateVehiclePoseFromPhysicsState.
 *
 * Uses raw byte offsets so it survives any port-side struct field rename.
 */
#include "td5_pilot_trace_004063A0.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool3_004063A0.csv"

#define O_SPAN_RAW     0x80
#define O_HEADING_290  0x290
#define O_WCP_BASE     0xF0
#define O_RENDER_MAT   0x120
#define O_RENDER_POS   0x144
#define O_CARDEF_PTR   0x1B8
#define O_EULER_ACCUM  0x1F0
#define O_WORLD_POS    0x1FC
#define O_DISP_ANGLES  0x208
#define O_BODY_OFF     0x210
#define O_PUVAR6_BUF   0x230
#define O_SUSP_POS     0x2DC
#define O_WCB_NEW      0x37C
#define O_WCB_OLD      0x37D

#define G_CARDEF_OFF 0x82
#define TARGET_SLOT  0

static FILE        *s_fp = NULL;
static int          s_active_call = 0;
static uintptr_t    s_caller_ra = 0;
static uint32_t     s_tick = 0;
static int          s_slot = 0;
static uint32_t     s_call_id = 0;
static const TD5_Actor *s_actor = NULL;

typedef struct {
    char     actor_addr[24];
    int32_t  euler_r, euler_y, euler_p;
    int32_t  world_x, world_y, world_z;
    int16_t  disp_r, disp_y, disp_p;
    uint8_t  wcb, dam;
    char     cardef_ptr[24];
    int16_t  cardef_82;
    int32_t  susp[4];
    int16_t  span_raw, heading_290;
    float    rot[9];
} PilotSnap;
static PilotSnap s_input;

extern int td5_trace_current_sim_tick(void);

static inline int16_t  rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t  rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint8_t  rd_u8 (const uint8_t *p) { return *p; }
static inline float    rd_f32(const uint8_t *p) { float v; memcpy(&v, p, sizeof v); return v; }
static inline uintptr_t rd_ptr(const uint8_t *p) { uintptr_t v; memcpy(&v, p, sizeof v); return v; }

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,call_id,caller_ra,"
        "actor_addr,"
        "euler_in_r,euler_in_y,euler_in_p,"
        "world_in_x,world_in_y,world_in_z,"
        "disp_in_r,disp_in_y,disp_in_p,"
        "wcb_in,dam_lockout_in,"
        "cardef_ptr,cardef_0x82,"
        "susp_pos_0,susp_pos_1,susp_pos_2,susp_pos_3,"
        "span_raw_in,heading_290_in,"
        "rot_in_0,rot_in_1,rot_in_2,rot_in_3,rot_in_4,rot_in_5,rot_in_6,rot_in_7,rot_in_8,"
        "euler_out_r,euler_out_y,euler_out_p,"
        "world_out_x,world_out_y,world_out_z,"
        "disp_out_r,disp_out_y,disp_out_p,"
        "wcb_out,dam_lockout_out,"
        "span_raw_out,heading_290_out,"
        "rot_out_0,rot_out_1,rot_out_2,rot_out_3,rot_out_4,rot_out_5,rot_out_6,rot_out_7,rot_out_8,"
        "render_pos_x,render_pos_y,render_pos_z,"
        "wcp0_y,wcp1_y,wcp2_y,wcp3_y,"
        "body_off_0_y,body_off_1_y,body_off_2_y,body_off_3_y,"
        "puvar6_0_y,puvar6_1_y,puvar6_2_y,puvar6_3_y\n");
}

static int actor_slot_index(const TD5_Actor *actor) {
    return actor->slot_index;
}

static void snap_actor(const TD5_Actor *actor, PilotSnap *out) {
    const uint8_t *base = (const uint8_t *)actor;
    uintptr_t cardef = rd_ptr(base + O_CARDEF_PTR);

    snprintf(out->actor_addr, sizeof out->actor_addr, "0x%p", (const void *)actor);
    snprintf(out->cardef_ptr, sizeof out->cardef_ptr, "0x%p", (const void *)cardef);

    out->euler_r = rd_i32(base + O_EULER_ACCUM + 0);
    out->euler_y = rd_i32(base + O_EULER_ACCUM + 4);
    out->euler_p = rd_i32(base + O_EULER_ACCUM + 8);
    out->world_x = rd_i32(base + O_WORLD_POS + 0);
    out->world_y = rd_i32(base + O_WORLD_POS + 4);
    out->world_z = rd_i32(base + O_WORLD_POS + 8);
    out->disp_r  = rd_i16(base + O_DISP_ANGLES + 0);
    out->disp_y  = rd_i16(base + O_DISP_ANGLES + 2);
    out->disp_p  = rd_i16(base + O_DISP_ANGLES + 4);
    out->wcb     = rd_u8 (base + O_WCB_NEW);
    out->dam     = rd_u8 (base + O_WCB_OLD);
    out->cardef_82 = cardef ? rd_i16((const uint8_t *)cardef + G_CARDEF_OFF) : 0;
    for (int i = 0; i < 4; i++)
        out->susp[i] = rd_i32(base + O_SUSP_POS + i * 4);
    out->span_raw    = rd_i16(base + O_SPAN_RAW);
    out->heading_290 = rd_i16(base + O_HEADING_290);
    for (int i = 0; i < 9; i++)
        out->rot[i] = rd_f32(base + O_RENDER_MAT + i * 4);
}

void td5_pilot_emit_004063A0_enter(const TD5_Actor *actor, uintptr_t caller_ra) {
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
    s_slot        = slot;
    s_actor       = actor;
    snap_actor(actor, &s_input);
}

void td5_pilot_emit_004063A0_leave(const TD5_Actor *actor) {
    if (!s_active_call || !s_fp || actor != s_actor) {
        s_active_call = 0;
        return;
    }
    PilotSnap out;
    snap_actor(actor, &out);

    const uint8_t *base = (const uint8_t *)actor;
    float render_pos[3];
    for (int i = 0; i < 3; i++) render_pos[i] = rd_f32(base + O_RENDER_POS + i * 4);
    int32_t wcp_y[4];
    for (int i = 0; i < 4; i++) wcp_y[i] = rd_i32(base + O_WCP_BASE + i * 0xC + 4);
    int16_t body_off_y[4];
    for (int i = 0; i < 4; i++) body_off_y[i] = rd_i16(base + O_BODY_OFF + i * 8 + 2);
    int16_t puvar6_y[4];
    for (int i = 0; i < 4; i++) puvar6_y[i] = rd_i16(base + O_PUVAR6_BUF + i * 8 + 2);

    fprintf(s_fp,
        "%u,0,%d,%u,0x%lx,"
        "%s,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%u,%u,"
        "%s,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%u,%u,"
        "%d,%d,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d\n",
        s_tick, s_slot, s_call_id++, (unsigned long)s_caller_ra,
        s_input.actor_addr,
        s_input.euler_r, s_input.euler_y, s_input.euler_p,
        s_input.world_x, s_input.world_y, s_input.world_z,
        s_input.disp_r, s_input.disp_y, s_input.disp_p,
        s_input.wcb, s_input.dam,
        s_input.cardef_ptr, s_input.cardef_82,
        s_input.susp[0], s_input.susp[1], s_input.susp[2], s_input.susp[3],
        s_input.span_raw, s_input.heading_290,
        s_input.rot[0], s_input.rot[1], s_input.rot[2], s_input.rot[3], s_input.rot[4],
        s_input.rot[5], s_input.rot[6], s_input.rot[7], s_input.rot[8],
        out.euler_r, out.euler_y, out.euler_p,
        out.world_x, out.world_y, out.world_z,
        out.disp_r, out.disp_y, out.disp_p,
        out.wcb, out.dam,
        out.span_raw, out.heading_290,
        out.rot[0], out.rot[1], out.rot[2], out.rot[3], out.rot[4],
        out.rot[5], out.rot[6], out.rot[7], out.rot[8],
        render_pos[0], render_pos[1], render_pos[2],
        wcp_y[0], wcp_y[1], wcp_y[2], wcp_y[3],
        body_off_y[0], body_off_y[1], body_off_y[2], body_off_y[3],
        puvar6_y[0], puvar6_y[1], puvar6_y[2], puvar6_y[3]);
    fflush(s_fp);
    s_active_call = 0;
}
