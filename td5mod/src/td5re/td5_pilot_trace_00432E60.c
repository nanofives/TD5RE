/*
 * td5_pilot_trace_00432E60.c -- Port-side CSV emitter for the precise-port
 * pilot 0x00432E60 InitializeRaceActorRuntime.
 *
 * Mirrors tools/frida_pool12_00432E60.js column-for-column so the diff tool
 * can match rows by (sim_tick, phase). The function fires once per race, so
 * the capture is exactly two rows (entry + exit) per race.
 */
#include "td5_pilot_trace_00432E60.h"

#include <stdio.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool12_00432E60.csv"

extern int td5_trace_current_sim_tick(void);

static FILE *s_fp = NULL;

static void emit_header_if_needed(void) {
    if (s_fp) return;
    s_fp = fopen(OUT_PATH, "w");
    if (!s_fp) return;
    fprintf(s_fp,
        "sim_tick,phase,"
        "tier_in,is_circuit,has_traffic,wanted_mode,difficulty,time_trial,"
        "tpl_steer,tpl_grip,tpl_brake,tpl_lspd_brake,tpl_top_spd,"
        "rb_behind_scale,rb_behind_range,rb_ahead_scale,rb_ahead_range,"
        "tpl_steer_out,tpl_grip_out,tpl_brake_out,tpl_lspd_brake_out,tpl_top_spd_out,"
        "racer_count\n");
}

void td5_pilot_emit_00432E60_enter(int tier_in,
                                   int is_circuit,
                                   int has_traffic,
                                   int wanted_mode,
                                   int difficulty,
                                   int time_trial,
                                   int16_t tpl_steer,
                                   int16_t tpl_grip,
                                   int16_t tpl_brake,
                                   int16_t tpl_lspd_brake,
                                   int16_t tpl_top_spd) {
    emit_header_if_needed();
    if (!s_fp) return;
    fprintf(s_fp,
        "%d,entry,"
        "%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,"
        ",,,,"        /* rb_* outputs blank on entry */
        ",,,,,"       /* tpl_*_out blank on entry */
        "\n",
        td5_trace_current_sim_tick(),
        tier_in, is_circuit, has_traffic, wanted_mode, difficulty, time_trial,
        (int)tpl_steer, (int)tpl_grip, (int)tpl_brake, (int)tpl_lspd_brake, (int)tpl_top_spd);
    fflush(s_fp);
}

void td5_pilot_emit_00432E60_leave(int32_t rb_behind_scale,
                                   int32_t rb_behind_range,
                                   int32_t rb_ahead_scale,
                                   int32_t rb_ahead_range,
                                   int16_t tpl_steer_out,
                                   int16_t tpl_grip_out,
                                   int16_t tpl_brake_out,
                                   int16_t tpl_lspd_brake_out,
                                   int16_t tpl_top_spd_out,
                                   int     racer_count) {
    emit_header_if_needed();
    if (!s_fp) return;
    fprintf(s_fp,
        "%d,exit,"
        ",,,,,,"   /* tier_in etc blank on exit */
        ",,,,,"    /* tpl_* in-values blank on exit */
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,"
        "%d\n",
        td5_trace_current_sim_tick(),
        (int)rb_behind_scale, (int)rb_behind_range, (int)rb_ahead_scale, (int)rb_ahead_range,
        (int)tpl_steer_out, (int)tpl_grip_out, (int)tpl_brake_out,
        (int)tpl_lspd_brake_out, (int)tpl_top_spd_out,
        racer_count);
    fflush(s_fp);
}
