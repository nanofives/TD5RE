/*
 * td5_pilot_trace_trig.c -- Port-side CSV emitter for the precise-port trig
 * pilot. Mirrors tools/frida_pool3_trig.js column-for-column.
 *
 * Path: log/port/pool3_trig.csv (created on first emit).
 * Cap : 5000 rows to match the Frida probe's cap.
 *
 * Both port functions live in td5_render.c; the hook sites call this emitter
 * at the top of CosFloat12bit/SinFloat12bit before returning the float.
 */
#include "td5_pilot_trace_trig.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define OUT_PATH "log/port/pool3_trig.csv"
#define MAX_ROWS 5000
/* Mirror tools/frida_pool3_trig.js: skip early-init/menu-phase trig calls so
 * the diff window is the stable sim region required by the precise-port DoD. */
#define MIN_TICK 5

extern int td5_trace_current_sim_tick(void);

/* Debug stubs retained as no-ops in case earlier diagnostic paths get
 * re-enabled. The byte-faithful LUT path (load embedded dump) no longer
 * needs FPU CW inspection — kept for binary-compat. */
static unsigned short s_dbg_cw_saved = 0;
static unsigned short s_dbg_cw_used  = 0;
static unsigned short s_dbg_cw_post  = 0;
void td5_trig_dbg_set_cw(unsigned short saved, unsigned short used) {
    s_dbg_cw_saved = saved;
    s_dbg_cw_used = used;
}
void td5_trig_dbg_set_cw3(unsigned short saved, unsigned short used, unsigned short post) {
    s_dbg_cw_saved = saved;
    s_dbg_cw_used = used;
    s_dbg_cw_post = post;
}

static FILE   *s_fp = NULL;
static int     s_row_count = 0;
static int     s_disabled  = 0;

/* call_idx tracking per which_fn × tick — matches the Frida script's logic. */
static int     s_cos_tick = -1, s_cos_n = 0;
static int     s_sin_tick = -1, s_sin_n = 0;

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,which_fn,call_idx,"
        "arg_raw,arg_masked,"
        "ret_float_bits,ret_float_val\n");
    /* Side-write FPU CW snapshot so we can verify precision swap took. */
    FILE *dbg = fopen("log/port/pool3_trig.dbg", "w");
    if (dbg) {
        fprintf(dbg, "saved=0x%04x used=0x%04x post=0x%04x\n",
                s_dbg_cw_saved, s_dbg_cw_used, s_dbg_cw_post);
        fclose(dbg);
    }
}

void td5_pilot_trig_emit(const char *which_fn, int32_t arg_raw, uint32_t ret_bits, float ret_val)
{
    if (s_disabled) return;
    if (s_row_count >= MAX_ROWS) return;

    int tick = td5_trace_current_sim_tick();
    if (tick < MIN_TICK) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) { s_disabled = 1; return; }
        emit_header(s_fp);
    }

    int32_t shifted = arg_raw;
    int is_sin = (which_fn[0] == 's');
    if (is_sin) {
        /* +(-0x400) in 32-bit signed -- wraps the same way as
         * EAX + 0xfffffc00 in the original. */
        shifted = (int32_t)((uint32_t)arg_raw + 0xfffffc00u);
    }
    uint32_t masked = (uint32_t)shifted & 0xfffu;

    int call_idx;
    if (is_sin) {
        if (s_sin_tick != tick) { s_sin_tick = tick; s_sin_n = 0; }
        call_idx = s_sin_n++;
    } else {
        if (s_cos_tick != tick) { s_cos_tick = tick; s_cos_n = 0; }
        call_idx = s_cos_n++;
    }

    fprintf(s_fp,
        "%d,%s,%d,%d,%u,0x%08x,%.9f\n",
        tick, which_fn, call_idx,
        arg_raw, masked,
        ret_bits, (double)ret_val);

    s_row_count++;
    if ((s_row_count & 0xff) == 0) fflush(s_fp);
}
