/*
 * td5_pilot_trace_0042EB10.c -- Port-side emitter mirroring frida_pool4_0042EB10.js.
 *
 * Header schema must match Frida script's column order exactly so
 * diff_func_trace.py can pair rows by (sim_tick, call_idx).
 *
 * Only slot 0 is emitted (TARGET_SLOT in the Frida probe). Caps at 5000 rows
 * to keep CSVs tractable.
 */
#include "td5_pilot_trace_0042EB10.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define OUT_PATH       "log/port/pool4_0042EB10.csv"
#define TARGET_SLOT    0
#define MAX_ROWS       5000

static inline uint32_t f32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static FILE   *s_fp        = NULL;
static int     s_rows      = 0;
static uint32_t s_last_tick = (uint32_t)-1;
static int     s_call_idx  = 0;

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,call_idx,slot,kind,wheel,caller_ra,"
        "param1_addr,param2_addr,"
        "p0,p1,p2,"
        "m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,m10,m11,"
        "out0,out1,out2,"
        "mh0,mh1,mh2,mh3,mh4,mh5,mh6,mh7,mh8,mh9,mh10,mh11\n");
}

void td5_pilot_emit_0042EB10(
    uint32_t tick,
    int      slot,
    const char *kind,
    int      wheel,
    uint32_t caller_ra,
    const void *param1,
    const void *param2,
    int16_t p0, int16_t p1, int16_t p2,
    const float matrix[12],
    int32_t out0, int32_t out1, int32_t out2)
{
    if (slot != TARGET_SLOT) return;
    if (s_rows >= MAX_ROWS)  return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    if (tick != s_last_tick) { s_last_tick = tick; s_call_idx = 0; }
    int call_idx = s_call_idx++;

    /* paused column always 0 — informational only on port side. */
    fprintf(s_fp,
        "%u,0,%d,%d,%s,%d,0x%lx,"
        "0x%p,0x%p,"
        "%d,%d,%d,"
        "%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,"
        "%d,%d,%d,"
        "0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x\n",
        tick, call_idx, slot, kind, wheel, (unsigned long)caller_ra,
        param1, param2,
        p0, p1, p2,
        matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5],
        matrix[6], matrix[7], matrix[8], matrix[9], matrix[10], matrix[11],
        out0, out1, out2,
        f32_bits(matrix[0]), f32_bits(matrix[1]), f32_bits(matrix[2]),
        f32_bits(matrix[3]), f32_bits(matrix[4]), f32_bits(matrix[5]),
        f32_bits(matrix[6]), f32_bits(matrix[7]), f32_bits(matrix[8]),
        f32_bits(matrix[9]), f32_bits(matrix[10]), f32_bits(matrix[11]));
    fflush(s_fp);
    s_rows++;
}
