/*
 * td5_pilot_trace.c -- Port-side CSV emitter for the precise-port pilot.
 *
 * Mirrors tools/frida_pool1_00403720.js column-for-column so the diff tool
 * can match rows by (sim_tick, slot, wheel, caller_ra) and report which
 * column diverges first.
 *
 * Uses raw byte offsets, not struct field access, so we are insulated from
 * port-side struct layout drift. Offsets are the same as what Frida reads
 * from the original binary.
 */
#include "td5_pilot_trace.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool1_00403720.csv"

/* Field offsets — matches frida_pool1_00403720.js exactly. */
#define O_SPAN_RAW   0x80
#define O_SUB_LANE   0x8C
#define O_PROBE_BASE 0x90
#define O_WCP_BASE   0xF0
#define O_RENDER_MAT 0x120
#define O_CARDEF_PTR 0x1B8
#define O_BODY_OFF   0x210
#define O_WCV_BASE   0x250
#define O_GAP270     0x270
#define O_HIRES      0x298
#define O_LOAD_ACCUM 0x2FC
#define O_WCB_NEW    0x37C
#define O_WCB_OLD    0x37D

#define G_CARDEF_OFF 0x82
#define TARGET_SLOT  0

/* Per-wheel snapshot of inputs captured at entry. */
typedef struct {
    char     actor_addr[20];
    int16_t  span_raw;
    int8_t   sub_lane;
    char     cardef_ptr[20];
    int16_t  cardef_0x82;
    float    rot[9];
    float    transl[3];
    int16_t  body_off_x, body_off_y_pre, body_off_z;
    uint8_t  old_wcb, old_dam_lockout;
    int32_t  old_wcp_x, old_wcp_y, old_wcp_z;
} PilotInputSnap;

static FILE        *s_fp = NULL;
static int          s_active_call = 0;
static uintptr_t    s_caller_ra = 0;
static uint32_t     s_tick = 0;
static int          s_paused = 0;
static int          s_slot = 0;
static const TD5_Actor *s_actor = NULL;
static PilotInputSnap s_inputs[4];

/* Forward decl from td5_game.c. The pause flag is file-local in td5_physics.c
 * so we leave the `paused` column zero-filled — informational only, not used
 * by the diff. */
extern int td5_trace_current_sim_tick(void);

static inline int16_t  rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t  rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint8_t  rd_u8 (const uint8_t *p) { return *p; }
static inline int8_t   rd_i8 (const uint8_t *p) { return (int8_t)*p; }
static inline float    rd_f32(const uint8_t *p) { float v; memcpy(&v, p, sizeof v); return v; }
static inline uintptr_t rd_ptr(const uint8_t *p) { uintptr_t v; memcpy(&v, p, sizeof v); return v; }

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,wheel,caller_ra,"
        "actor_addr,span_raw,sub_lane,cardef_ptr,cardef_0x82,"
        "rot0,rot1,rot2,rot3,rot4,rot5,rot6,rot7,rot8,"
        "transl_x,transl_y,transl_z,"
        "body_off_x,body_off_y_pre,body_off_z,"
        "old_wcb,old_dam_lockout,"
        "old_wcp_x,old_wcp_y,old_wcp_z,"
        "new_wcp_x,new_wcp_y,new_wcp_z,"
        "hires_x,hires_y,hires_z,"
        "wcv_x,wcv_y,wcv_z,"
        "gap270_x,gap270_y,gap270_z,"
        "load_accum,"
        "new_wcb,new_dam_lockout,"
        "probe_w_x,probe_w_y,probe_w_z\n");
}

static int actor_slot_index(const TD5_Actor *actor) {
    return actor->slot_index;
}

static void snap_inputs(const TD5_Actor *actor, int w, PilotInputSnap *out) {
    const uint8_t *base = (const uint8_t *)actor;
    uintptr_t cardef = rd_ptr(base + O_CARDEF_PTR);

    snprintf(out->actor_addr, sizeof out->actor_addr, "0x%p", (const void *)actor);
    snprintf(out->cardef_ptr, sizeof out->cardef_ptr, "0x%p", (const void *)cardef);

    out->span_raw    = rd_i16(base + O_SPAN_RAW);
    out->sub_lane    = rd_i8 (base + O_SUB_LANE);
    out->cardef_0x82 = cardef ? rd_i16((const uint8_t *)cardef + G_CARDEF_OFF) : 0;

    for (int i = 0; i < 9; i++)
        out->rot[i] = rd_f32(base + O_RENDER_MAT + i * 4);
    for (int i = 0; i < 3; i++)
        out->transl[i] = rd_f32(base + O_RENDER_MAT + (9 + i) * 4);

    out->body_off_x     = rd_i16(base + O_BODY_OFF + w * 8 + 0);
    out->body_off_y_pre = rd_i16(base + O_BODY_OFF + w * 8 + 2);
    out->body_off_z     = rd_i16(base + O_BODY_OFF + w * 8 + 4);
    out->old_wcb        = rd_u8 (base + O_WCB_NEW);
    out->old_dam_lockout= rd_u8 (base + O_WCB_OLD);
    out->old_wcp_x      = rd_i32(base + O_WCP_BASE + w * 0xC + 0);
    out->old_wcp_y      = rd_i32(base + O_WCP_BASE + w * 0xC + 4);
    out->old_wcp_z      = rd_i32(base + O_WCP_BASE + w * 0xC + 8);
}

void td5_pilot_emit_00403720_enter(const TD5_Actor *actor, uintptr_t caller_ra) {
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
    for (int w = 0; w < 4; w++)
        snap_inputs(actor, w, &s_inputs[w]);
}

void td5_pilot_emit_00403720_leave(const TD5_Actor *actor) {
    if (!s_active_call || !s_fp || actor != s_actor) {
        s_active_call = 0;
        return;
    }
    const uint8_t *base = (const uint8_t *)actor;
    for (int w = 0; w < 4; w++) {
        const PilotInputSnap *in = &s_inputs[w];
        int32_t new_wcp_x = rd_i32(base + O_WCP_BASE   + w * 0xC + 0);
        int32_t new_wcp_y = rd_i32(base + O_WCP_BASE   + w * 0xC + 4);
        int32_t new_wcp_z = rd_i32(base + O_WCP_BASE   + w * 0xC + 8);
        int32_t hires_x   = rd_i32(base + O_HIRES      + w * 0xC + 0);
        int32_t hires_y   = rd_i32(base + O_HIRES      + w * 0xC + 4);
        int32_t hires_z   = rd_i32(base + O_HIRES      + w * 0xC + 8);
        int16_t wcv_x     = rd_i16(base + O_WCV_BASE   + w * 8 + 0);
        int16_t wcv_y     = rd_i16(base + O_WCV_BASE   + w * 8 + 2);
        int16_t wcv_z     = rd_i16(base + O_WCV_BASE   + w * 8 + 4);
        int16_t g270_x    = rd_i16(base + O_GAP270     + w * 8 + 0);
        int16_t g270_y    = rd_i16(base + O_GAP270     + w * 8 + 2);
        int16_t g270_z    = rd_i16(base + O_GAP270     + w * 8 + 4);
        int32_t load_acc  = rd_i32(base + O_LOAD_ACCUM + w * 4);
        uint8_t new_wcb   = rd_u8 (base + O_WCB_NEW);
        uint8_t new_dam   = rd_u8 (base + O_WCB_OLD);
        int32_t probe_x   = rd_i32(base + O_PROBE_BASE + w * 0xC + 0);
        int32_t probe_y   = rd_i32(base + O_PROBE_BASE + w * 0xC + 4);
        int32_t probe_z   = rd_i32(base + O_PROBE_BASE + w * 0xC + 8);

        fprintf(s_fp,
            "%u,%d,%d,%d,0x%lx,"
            "%s,%d,%d,%s,%d,"
            "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
            "%.6f,%.6f,%.6f,"
            "%d,%d,%d,"
            "%u,%u,"
            "%d,%d,%d,"
            "%d,%d,%d,"
            "%d,%d,%d,"
            "%d,%d,%d,"
            "%d,%d,%d,"
            "%d,"
            "%u,%u,"
            "%d,%d,%d\n",
            s_tick, s_paused, s_slot, w, (unsigned long)s_caller_ra,
            in->actor_addr, in->span_raw, in->sub_lane, in->cardef_ptr, in->cardef_0x82,
            in->rot[0], in->rot[1], in->rot[2], in->rot[3], in->rot[4],
            in->rot[5], in->rot[6], in->rot[7], in->rot[8],
            in->transl[0], in->transl[1], in->transl[2],
            in->body_off_x, in->body_off_y_pre, in->body_off_z,
            in->old_wcb, in->old_dam_lockout,
            in->old_wcp_x, in->old_wcp_y, in->old_wcp_z,
            new_wcp_x, new_wcp_y, new_wcp_z,
            hires_x, hires_y, hires_z,
            wcv_x, wcv_y, wcv_z,
            g270_x, g270_y, g270_z,
            load_acc,
            new_wcb, new_dam,
            probe_x, probe_y, probe_z);
    }
    fflush(s_fp);
    s_active_call = 0;
}
