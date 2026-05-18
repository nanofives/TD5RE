/*
 * td5_pilot_trace_pool15_spline.c — Port-side CSV trace mirror for the
 * paired pilot 0x00434670 / 0x00434800.
 *
 * Schema matches tools/frida_pool15_spline.js. One file, two `which_fn`
 * tag values distinguishing the addresses.
 *
 * Writes to log/port/pool15_spline.csv relative to the executable's CWD.
 */
#include "td5_pilot_trace_pool15_spline.h"

#include "td5_track.h"
#include "td5_types.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define OUT_PATH "log/port/pool15_spline.csv"

extern int td5_trace_current_sim_tick(void);

static FILE *s_fp = NULL;
static int   s_call_idx = 0;

static void open_once(void) {
    if (s_fp) return;
    s_fp = fopen(OUT_PATH, "w");
    if (!s_fp) return;
    fprintf(s_fp,
        "sim_tick,call_idx,which_fn,"
        "span_index,span_type,lane_count_nibble,left_vi,right_vi,"
        "origin_x,origin_z,"
        "in_param2,in_param3,in_param4,"
        "type_offset,end_vi,"
        "v_start_x,v_start_z,v_end_x,v_end_z,"
        "dX,dZ,delta,mag_sq,"
        "out_value,out_x,out_z\n");
    fflush(s_fp);
}

/* Same table as in td5_track.c — declared static there so we recreate
 * the same values here for symmetry with the Frida probe. Two columns:
 *   col 0 (idx 0): SampleTrackTargetPoint (0x00434800)
 *   col 1 (idx 1): ComputeSignedTrackOffset / ComputeTrackSpanProgress
 */
static const int8_t s_type_offsets[12][2] = {
    /* type  0 */ {  0,  0 },
    /* type  1 */ {  0,  0 },
    /* type  2 */ { -1,  0 },
    /* type  3 */ { -1,  0 },
    /* type  4 */ { -2,  0 },
    /* type  5 */ {  0, -1 },
    /* type  6 */ {  0, -1 },
    /* type  7 */ {  0, -2 },
    /* type  8 */ {  0,  0 },
    /* type  9 */ {  0,  0 },
    /* type 10 */ {  0,  0 },
    /* type 11 */ {  0,  0 },
};

void td5_pilot_trace_pool15_emit_signed_offset(int span_index,
                                               int progress,
                                               int route_byte,
                                               int32_t out_value)
{
    open_once();
    if (!s_fp) return;

    TD5_StripSpan *sp = td5_track_get_span(span_index);
    int span_type = -1;
    int lane_count = -1;
    int left_vi = -1, right_vi = -1;
    int32_t origin_x = 0, origin_z = 0;
    int type_off = 0;
    int end_vi = -1, start_vi = -1;
    int vs_x = 0, vs_z = 0;
    int ve_x = 0, ve_z = 0;
    int dX = 0, dZ = 0;
    int delta = progress - route_byte;
    long long mag_sq = 0;

    if (sp) {
        span_type = (int)sp->span_type;
        lane_count = td5_track_get_span_lane_count(span_index);
        left_vi = (int)(uint16_t)sp->left_vertex_index;
        right_vi = (int)(uint16_t)sp->right_vertex_index;
        origin_x = sp->origin_x;
        origin_z = sp->origin_z;
        if (span_type >= 0 && span_type < 12) {
            type_off = (int)s_type_offsets[span_type][1];  /* col 1 */
        }
        start_vi = right_vi;
        end_vi   = start_vi + lane_count + type_off;
        TD5_StripVertex *vs = td5_track_get_vertex(start_vi);
        TD5_StripVertex *ve = td5_track_get_vertex(end_vi);
        if (vs) { vs_x = (int)vs->x; vs_z = (int)vs->z; }
        if (ve) { ve_x = (int)ve->x; ve_z = (int)ve->z; }
        dX = ve_x - vs_x;
        dZ = ve_z - vs_z;
        long long sx = ((long long)dX * delta) >> 8;
        long long sz = ((long long)dZ * delta) >> 8;
        mag_sq = sx*sx + sz*sz;
    }

    fprintf(s_fp,
        "%d,%d,0x00434670,"
        "%d,%d,%d,%d,%d,"
        "%d,%d,"
        "%d,%d,0,"
        "%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%lld,"
        "%d,0,0\n",
        td5_trace_current_sim_tick(), s_call_idx++,
        span_index, span_type, lane_count, left_vi, right_vi,
        (int)origin_x, (int)origin_z,
        progress, route_byte,
        type_off, end_vi,
        vs_x, vs_z, ve_x, ve_z,
        dX, dZ, delta, mag_sq,
        (int)out_value);
    fflush(s_fp);
}

void td5_pilot_trace_pool15_emit_target_point(int span_index,
                                              int route_byte,
                                              int lateral_bias,
                                              int32_t out_x,
                                              int32_t out_z)
{
    open_once();
    if (!s_fp) return;

    TD5_StripSpan *sp = td5_track_get_span(span_index);
    int span_type = -1;
    int lane_count = -1;
    int left_vi = -1, right_vi = -1;
    int32_t origin_x = 0, origin_z = 0;
    int type_off = 0;
    int end_vi = -1, start_vi = -1;
    int vs_x = 0, vs_z = 0;
    int ve_x = 0, ve_z = 0;
    int dX = 0, dZ = 0;
    long long mag_sq = 0;

    if (sp) {
        span_type = (int)sp->span_type;
        lane_count = td5_track_get_span_lane_count(span_index);
        left_vi = (int)(uint16_t)sp->left_vertex_index;
        right_vi = (int)(uint16_t)sp->right_vertex_index;
        origin_x = sp->origin_x;
        origin_z = sp->origin_z;
        if (span_type >= 0 && span_type < 12) {
            type_off = (int)s_type_offsets[span_type][0];  /* col 0 */
        }
        start_vi = left_vi;
        end_vi   = start_vi + lane_count + type_off;
        TD5_StripVertex *vs = td5_track_get_vertex(start_vi);
        TD5_StripVertex *ve = td5_track_get_vertex(end_vi);
        if (vs) { vs_x = (int)vs->x; vs_z = (int)vs->z; }
        if (ve) { ve_x = (int)ve->x; ve_z = (int)ve->z; }
        dX = ve_x - vs_x;
        dZ = ve_z - vs_z;
        mag_sq = (long long)dX*dX + (long long)dZ*dZ;
    }

    fprintf(s_fp,
        "%d,%d,0x00434800,"
        "%d,%d,%d,%d,%d,"
        "%d,%d,"
        "%d,0,%d,"
        "%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,0,%lld,"
        "0,%d,%d\n",
        td5_trace_current_sim_tick(), s_call_idx++,
        span_index, span_type, lane_count, left_vi, right_vi,
        (int)origin_x, (int)origin_z,
        route_byte, lateral_bias,
        type_off, end_vi,
        vs_x, vs_z, ve_x, ve_z,
        dX, dZ, mag_sq,
        (int)out_x, (int)out_z);
    fflush(s_fp);
}
