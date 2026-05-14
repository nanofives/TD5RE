/*
 * td5_pilot_trace_0040A720.c — port-side emitter for the AngleFromVector12 pilot.
 *
 * Mirrors tools/frida_pool2_0040A720.js. Pure leaf math function: one row per
 * call, keyed by (p1, p2). caller_ra is informational so the diff can spot a
 * particular call site producing diverging results.
 */
#include "td5_pilot_trace_0040A720.h"

#include <stdio.h>
#include <stdlib.h>

#define OUT_PATH "log/port/pool2_0040A720.csv"

extern int td5_trace_current_sim_tick(void);

static FILE *s_fp = NULL;
static unsigned long s_call_idx = 0;

static void emit_header(FILE *fp) {
    fprintf(fp, "sim_tick,call_idx,caller_ra,p1,p2,ret\n");
}

void td5_pilot_trace_0040A720_call(int p1, int p2, int ret) {
    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }
    /* __builtin_return_address(0) is the immediate caller (the function we
     * were called from — the spot where AngleFromVector12 itself was called).
     * Useful for spotting which call site in the port produces divergence. */
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    fprintf(s_fp, "%d,%lu,0x%lx,%d,%d,%d\n",
            td5_trace_current_sim_tick(),
            s_call_idx++,
            (unsigned long)caller,
            p1, p2, ret);
    if ((s_call_idx & 0x3FF) == 0)
        fflush(s_fp);
}
