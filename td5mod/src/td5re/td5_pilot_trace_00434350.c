/*
 * td5_pilot_trace_00434350.c -- Port-side CSV emitter for the
 * InitializeActorTrackPose pilot (pool14 / 0x00434350).
 *
 * Mirrors tools/frida_pool14_00434350.js column-for-column. Reads using raw
 * byte offsets so the schema is insulated from port-side struct drift.
 */
#include "td5_pilot_trace_00434350.h"

#include "td5_track.h"
#include "td5_types.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define OUT_PATH "log/port/pool14_00434350.csv"

/* Actor byte offsets (match Frida) */
#define O_SPAN_RAW        0x080
#define O_SPAN_NORMALIZED 0x082
#define O_SPAN_ACCUM      0x084
#define O_SPAN_HIGH       0x086
#define O_SUB_LANE        0x08C
#define O_YAW_ACCUM       0x1F4
#define O_WORLD_POS_X     0x1FC
#define O_WORLD_POS_Y     0x200
#define O_WORLD_POS_Z     0x204

/* RouteState accessors (RS dword 0x19 = TRACK_PROGRESS, 0x09 = TRACK_OFFSET_BIAS) */
extern int32_t *td5_ai_get_route_state(int slot);

static FILE *s_fp = NULL;

static inline int16_t rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }

static void emit_header(FILE *fp) {
    fprintf(fp,
        "call_idx,slot,"
        "param_span,param_sub_lane,param_flip,"
        "span_type,lane_count_nibble,left_vi,right_vi,"
        "vl0_x,vl0_z,vl1_x,vl1_z,vl2_x,vl2_z,"
        "vr0_x,vr0_z,vr1_x,vr1_z,vr2_x,vr2_z,"
        "actor_span_raw,actor_span_norm,actor_span_accum,actor_span_high,"
        "actor_sub_lane_post,"
        "actor_yaw_accum,"
        "actor_world_x,actor_world_y,actor_world_z,"
        "rs_track_progress,rs_track_offset_bias\n");
}

void td5_pilot_emit_00434350_row(int call_idx,
                                  int slot,
                                  int param_span,
                                  int param_sub_lane,
                                  int param_flip,
                                  const void *actor)
{
    if (!actor) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    const uint8_t *abase = (const uint8_t *)actor;

    /* Look up span record + vertices via the port's track API. We read the
     * same record fields the original reads at 0x00434395-0x004343BF. */
    int span_type = -1;
    int lane_count_nibble = -1;
    int left_vi = -1, right_vi = -1;
    int vl0x = 0, vl0z = 0, vl1x = 0, vl1z = 0, vl2x = 0, vl2z = 0;
    int vr0x = 0, vr0z = 0, vr1x = 0, vr1z = 0, vr2x = 0, vr2z = 0;
    int span_raw = (int)rd_i16(abase + O_SPAN_RAW);
    {
        TD5_StripSpan *sp = td5_track_get_span(span_raw);
        if (sp) {
            span_type = sp->span_type;
            lane_count_nibble = td5_track_span_lane_count_at(span_raw);
            left_vi = (int)(uint16_t)sp->left_vertex_index;
            right_vi = (int)(uint16_t)sp->right_vertex_index;
            TD5_StripVertex *vl0 = td5_track_get_vertex(left_vi);
            TD5_StripVertex *vl1 = td5_track_get_vertex(left_vi + 1);
            TD5_StripVertex *vl2 = td5_track_get_vertex(left_vi + 2);
            TD5_StripVertex *vr0 = td5_track_get_vertex(right_vi);
            TD5_StripVertex *vr1 = td5_track_get_vertex(right_vi + 1);
            TD5_StripVertex *vr2 = td5_track_get_vertex(right_vi + 2);
            if (vl0) { vl0x = (int)vl0->x; vl0z = (int)vl0->z; }
            if (vl1) { vl1x = (int)vl1->x; vl1z = (int)vl1->z; }
            if (vl2) { vl2x = (int)vl2->x; vl2z = (int)vl2->z; }
            if (vr0) { vr0x = (int)vr0->x; vr0z = (int)vr0->z; }
            if (vr1) { vr1x = (int)vr1->x; vr1z = (int)vr1->z; }
            if (vr2) { vr2x = (int)vr2->x; vr2z = (int)vr2->z; }
        }
    }

    int32_t *rs = td5_ai_get_route_state(slot);
    int32_t progress = rs ? rs[0x19] : 0;
    int32_t bias     = rs ? rs[0x09] : 0;

    fprintf(s_fp,
        "%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,"
        "%d,"
        "%d,"
        "%d,%d,%d,"
        "%d,%d\n",
        call_idx, slot,
        param_span, param_sub_lane, param_flip,
        span_type, lane_count_nibble, left_vi, right_vi,
        vl0x, vl0z, vl1x, vl1z, vl2x, vl2z,
        vr0x, vr0z, vr1x, vr1z, vr2x, vr2z,
        (int)rd_i16(abase + O_SPAN_RAW),
        (int)rd_i16(abase + O_SPAN_NORMALIZED),
        (int)rd_i16(abase + O_SPAN_ACCUM),
        (int)rd_i16(abase + O_SPAN_HIGH),
        (int)abase[O_SUB_LANE],
        (int)rd_i32(abase + O_YAW_ACCUM),
        (int)rd_i32(abase + O_WORLD_POS_X),
        (int)rd_i32(abase + O_WORLD_POS_Y),
        (int)rd_i32(abase + O_WORLD_POS_Z),
        (int)progress, (int)bias);
    fflush(s_fp);
}
