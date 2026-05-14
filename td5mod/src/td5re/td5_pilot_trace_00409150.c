/*
 * td5_pilot_trace_00409150.c — port-side emitter for the ResolveVehicleContacts
 * (V2V iteration driver) pilot.
 *
 * Mirrors tools/frida_pool9_00409150.js column-for-column. Two outputs:
 *   - phase1 csv:  one row per slot per call (AABB build)
 *   - pair csv:    one row per dispatched pair (Phase 2 inner-loop hit)
 *
 * The diff key for cross-port matching is (sim_tick, slot_a, slot_b) in the
 * pair CSV. The phase1 CSV is the upstream-state baseline so we can verify
 * the AABB inputs were identical when pairs match.
 */
#include "td5_pilot_trace_00409150.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_PATH_PHASE1 "log/port/pool9_00409150_phase1.csv"
#define OUT_PATH_PAIR   "log/port/pool9_00409150_pair.csv"

extern int td5_trace_current_sim_tick(void);

static FILE *s_fp_phase1 = NULL;
static FILE *s_fp_pair   = NULL;
static unsigned long s_call_idx = 0;

static void emit_header_phase1(FILE *fp) {
    fprintf(fp,
            "sim_tick,call_idx,racer_count,"
            "slot,world_x_sar8,world_z_sar8,cardef_radius,"
            "span,bucket,prev_head,"
            "xmin,zmin,xmax,zmax\n");
}

static void emit_header_pair(FILE *fp) {
    fprintf(fp,
            "sim_tick,call_idx,pair_idx,"
            "slot_a,slot_b,bucket_off,walk_iter,"
            "dispatch,mode_a,wcb_a,mode_b,wcb_b\n");
}

/* Stash for Phase 1 rows that need racer_count which the enter() reports. */
static int s_racer_count = 0;

void td5_pilot_trace_00409150_enter(int racer_count) {
    s_racer_count = racer_count;
    s_call_idx++;
    /* lazy-open both files */
    if (!s_fp_phase1) {
        s_fp_phase1 = fopen(OUT_PATH_PHASE1, "w");
        if (s_fp_phase1) emit_header_phase1(s_fp_phase1);
    }
    if (!s_fp_pair) {
        s_fp_pair = fopen(OUT_PATH_PAIR, "w");
        if (s_fp_pair) emit_header_pair(s_fp_pair);
    }
}

void td5_pilot_trace_00409150_phase1(int slot,
                                     int32_t world_x_sar8, int32_t world_z_sar8,
                                     int32_t cardef_radius,
                                     int32_t span, int32_t bucket,
                                     int32_t prev_head,
                                     int32_t xmin, int32_t zmin,
                                     int32_t xmax, int32_t zmax) {
    if (!s_fp_phase1) return;
    fprintf(s_fp_phase1,
            "%d,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
            td5_trace_current_sim_tick(),
            s_call_idx,
            s_racer_count,
            slot,
            (int)world_x_sar8, (int)world_z_sar8,
            (int)cardef_radius,
            (int)span, (int)bucket, (int)prev_head,
            (int)xmin, (int)zmin, (int)xmax, (int)zmax);
}

void td5_pilot_trace_00409150_pair(int slot_a, int slot_b,
                                   int bucket_off, int walk_iter,
                                   int dispatch,
                                   uint8_t mode_a, uint8_t wcb_a,
                                   uint8_t mode_b, uint8_t wcb_b,
                                   int pair_idx) {
    if (!s_fp_pair) return;
    fprintf(s_fp_pair,
            "%d,%lu,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u\n",
            td5_trace_current_sim_tick(),
            s_call_idx,
            pair_idx,
            slot_a, slot_b,
            bucket_off, walk_iter,
            dispatch,
            (unsigned)mode_a, (unsigned)wcb_a,
            (unsigned)mode_b, (unsigned)wcb_b);
}

void td5_pilot_trace_00409150_leave(int total_pairs) {
    (void)total_pairs;
    /* Flush both CSVs every few hundred calls to bound data loss on crash. */
    if ((s_call_idx & 0x3F) == 0) {
        if (s_fp_phase1) fflush(s_fp_phase1);
        if (s_fp_pair)   fflush(s_fp_pair);
    }
}
