/*
 * td5_pilot_trace_00436A70.c -- Port-side CSV emitter for the
 * UpdateRaceActors pilot (pool13 / 0x00436A70).
 *
 * Mirrors tools/frida_pool13_00436A70.js column-for-column.
 * One row per racer slot per call. Slots 0..5 always emitted (6 rows/call).
 */
#include "td5_pilot_trace_00436A70.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool13_00436A70.csv"
#define N_SLOTS  PILOT_00436A70_N_SLOTS

/* RS strides + offsets (mirrors td5_ai.c; replicated here so the trace
 * file is self-contained and immune to RS_LEFT_DEVIATION naming drift). */
#define PILOT_RS_STRIDE_DW           0x47   /* dwords */
#define PILOT_RS_SELECTOR_DW         0x03   /* gActorRouteTableSelector */
#define PILOT_RS_ROUTE_TABLE_DW      0x00   /* gActorRouteStateTable    */
#define PILOT_RS_LAT_BIAS_DW         0x09   /* gLateralOffsetBias       */
#define PILOT_RS_LEFT_DEV_DW         0x0E   /* gLeftDeviation  (byte 0x38) */
#define PILOT_RS_RIGHT_DEV_DW        0x0F   /* gRightDeviation (byte 0x3C) */
#define PILOT_RS_ACTIVE_UPPER_DW     0x14   /* gActiveUpperBound (byte 0x50) */
#define PILOT_RS_ACTIVE_LOWER_DW     0x15   /* gActiveLowerBound (byte 0x54) */
#define PILOT_RS_FWD_TRACK_DW        0x18   /* gActorForwardTrackComponent  */
#define PILOT_RS_SPAN_PROGRESS_DW    0x19   /* gActorTrackSpanProgress      */

/* Externs from td5_ai.c -- per-actor state arrays. */
extern int32_t *td5_pilot_00436A70_route_state_ptr(void);
extern char    *td5_pilot_00436A70_actor_base_ptr(void);

/* From td5_game.c / td5_trace.c */
extern int td5_trace_current_sim_tick(void);

static FILE *s_fp = NULL;
static int   s_active_call = 0;
static uint32_t s_tick = 0;

/* DIAG-2026-05-21: extended physics dump for orig comparison.
 * Mirrors tools/_probes/extended_physics_probe.js columns exactly. */
#define EXT_OUT_PATH "log/port/extended_physics.csv"
static FILE *s_ext_fp = NULL;
static int   s_ext_rows = 0;

static void emit_extended_physics(void) {
    if (s_ext_rows >= 2000) return;
    if (!s_ext_fp) {
        s_ext_fp = fopen(EXT_OUT_PATH, "w");
        if (!s_ext_fp) return;
        fputs("sim_tick,world_x,world_y,world_z,lvx,lvy,lvz,"
              "euler_roll,euler_yaw,euler_pitch,ang_vel_yaw,"
              "susp0,susp1,wheel_load_0,wheel_load_1,wheel_load_2,wheel_load_3,"
              "wheel_contact,surface_contact,brake,handbrake,"
              "span_raw,span_norm,long_speed,steer_cmd\n", s_ext_fp);
    }
    char *base = td5_pilot_00436A70_actor_base_ptr();
    if (!base) return;
    /* slot 0 */
    char *a = base;
    int32_t wx = *(int32_t *)(a + 0x1FC);
    int32_t wy = *(int32_t *)(a + 0x200);
    int32_t wz = *(int32_t *)(a + 0x204);
    int32_t lvx = *(int32_t *)(a + 0x1CC);
    int32_t lvy = *(int32_t *)(a + 0x1D0);
    int32_t lvz = *(int32_t *)(a + 0x1D4);
    int32_t er  = *(int32_t *)(a + 0x1F0);
    int32_t ey  = *(int32_t *)(a + 0x1F4);
    int32_t ep  = *(int32_t *)(a + 0x1F8);
    int32_t avy = *(int32_t *)(a + 0x21C);
    int32_t s0  = *(int32_t *)(a + 0x2DC);
    int32_t s1  = *(int32_t *)(a + 0x2E0);
    int16_t wl0 = *(int16_t *)(a + 0x270);
    int16_t wl1 = *(int16_t *)(a + 0x272);
    int16_t wl2 = *(int16_t *)(a + 0x274);
    int16_t wl3 = *(int16_t *)(a + 0x276);
    uint8_t wc  = *(uint8_t *)(a + 0x37C);
    uint8_t sc  = *(uint8_t *)(a + 0x37D);
    uint8_t bk  = *(uint8_t *)(a + 0x36D);
    uint8_t hb  = *(uint8_t *)(a + 0x36E);
    int16_t sr  = *(int16_t *)(a + 0x80);
    int16_t sn  = *(int16_t *)(a + 0x82);
    int32_t ls  = *(int32_t *)(a + 0x314);
    int32_t st  = *(int32_t *)(a + 0x30C);
    fprintf(s_ext_fp,
        "%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%d,%d,%d,%d\n",
        (unsigned)s_tick,
        wx, wy, wz, lvx, lvy, lvz, er, ey, ep, avy,
        s0, s1, (int)wl0, (int)wl1, (int)wl2, (int)wl3,
        (unsigned)wc, (unsigned)sc, (unsigned)bk, (unsigned)hb,
        (int)sr, (int)sn, ls, st);
    s_ext_rows++;
    if ((s_ext_rows & 0x3F) == 0) fflush(s_ext_fp);
}

static PilotSnapshot_00436A70 s_in_snap;

/* Per-slot pre-call output snapshot — many output fields are
 * read-modified-write inside the dispatcher (e.g., gLateralOffsetBias).
 * Capturing pre-call lets the diff distinguish "field unchanged" vs
 * "field changed but ended at same value as port did". */
static int32_t s_pre_route_selector[N_SLOTS];
static int32_t s_pre_lat_bias[N_SLOTS];

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,slot,phase,"
        /* call-wide constants */
        "network_active,racer_count,drag_mode,wanted_mode,"
        "encounter_handle,encounter_enabled,track_total_span_count,"
        /* per-slot inputs */
        "slot_state,span_raw,span_norm,lin_vel_x,lin_vel_z,"
        "world_pos_x,world_pos_z,recovery_stage,wanted_damage,"
        /* per-slot outputs (post-call) */
        "route_selector,route_table_ptr,fwd_track_comp,span_progress,"
        "lat_offset_bias,left_deviation,right_deviation,"
        "active_upper_bound,active_lower_bound,"
        "encounter_steer,brake_flag,"
        "actor_byte_371,actor_byte_372,actor_byte_373,actor_byte_374,"
        "actor_byte_375,actor_byte_376\n");
}

static inline int32_t rs_read(int slot, int dw) {
    int32_t *base = td5_pilot_00436A70_route_state_ptr();
    if (!base) return 0;
    return base[slot * PILOT_RS_STRIDE_DW + dw];
}

static inline int8_t actor_byte(int slot, int off) {
    char *base = td5_pilot_00436A70_actor_base_ptr();
    if (!base) return 0;
    return *((int8_t *)(base + slot * 0x388 + off));
}

static inline int16_t actor_i16(int slot, int off) {
    char *base = td5_pilot_00436A70_actor_base_ptr();
    if (!base) return 0;
    return *((int16_t *)(base + slot * 0x388 + off));
}

void td5_pilot_emit_00436A70_enter(void) {
    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    s_active_call = 1;
    s_tick = (uint32_t)td5_trace_current_sim_tick();

    /* Snapshot pre-call mutable outputs. */
    for (int i = 0; i < N_SLOTS; i++) {
        s_pre_route_selector[i] = rs_read(i, PILOT_RS_SELECTOR_DW);
        s_pre_lat_bias[i]       = rs_read(i, PILOT_RS_LAT_BIAS_DW);
    }

    /* Snapshot per-slot inputs from the port's runtime state. */
    td5_pilot_00436A70_collect(&s_in_snap);
}

void td5_pilot_emit_00436A70_leave(void) {
    if (!s_active_call || !s_fp) {
        s_active_call = 0;
        return;
    }

    for (int slot = 0; slot < N_SLOTS; slot++) {
        int32_t route_selector  = rs_read(slot, PILOT_RS_SELECTOR_DW);
        int32_t route_table_ptr = rs_read(slot, PILOT_RS_ROUTE_TABLE_DW);
        int32_t fwd_track       = rs_read(slot, PILOT_RS_FWD_TRACK_DW);
        int32_t span_progress   = rs_read(slot, PILOT_RS_SPAN_PROGRESS_DW);
        int32_t lat_bias        = rs_read(slot, PILOT_RS_LAT_BIAS_DW);
        int32_t left_dev        = rs_read(slot, PILOT_RS_LEFT_DEV_DW);
        int32_t right_dev       = rs_read(slot, PILOT_RS_RIGHT_DEV_DW);
        int32_t active_upper    = rs_read(slot, PILOT_RS_ACTIVE_UPPER_DW);
        int32_t active_lower    = rs_read(slot, PILOT_RS_ACTIVE_LOWER_DW);
        int16_t encounter_steer = actor_i16(slot, 0x33E);
        int8_t  brake_flag      = actor_byte(slot, 0x36D);
        int8_t  b371            = actor_byte(slot, 0x371);
        int8_t  b372            = actor_byte(slot, 0x372);
        int8_t  b373            = actor_byte(slot, 0x373);
        int8_t  b374            = actor_byte(slot, 0x374);
        int8_t  b375            = actor_byte(slot, 0x375);
        int8_t  b376            = actor_byte(slot, 0x376);

        fprintf(s_fp,
            /* keys */
            "%u,%d,leave,"
            /* call-wide */
            "%d,%d,%d,%d,"
            "%d,%d,%d,"
            /* per-slot inputs */
            "%d,%d,%d,%d,%d,"
            "%d,%d,%d,%d,"
            /* per-slot outputs */
            "%d,0x%08x,%d,%d,"
            "%d,%d,%d,"
            "%d,%d,"
            "%d,%d,"
            "%d,%d,%d,%d,"
            "%d,%d\n",
            s_tick, slot,
            /* call-wide */
            s_in_snap.network_active, s_in_snap.racer_count,
            s_in_snap.drag_mode, s_in_snap.wanted_mode,
            s_in_snap.encounter_handle, s_in_snap.encounter_enabled,
            s_in_snap.track_total_span_count,
            /* per-slot inputs */
            s_in_snap.slot_state[slot], s_in_snap.span_raw[slot],
            s_in_snap.span_norm[slot], s_in_snap.lin_vel_x[slot],
            s_in_snap.lin_vel_z[slot],
            s_in_snap.world_pos_x[slot], s_in_snap.world_pos_z[slot],
            s_in_snap.recovery_stage[slot], s_in_snap.wanted_damage[slot],
            /* per-slot outputs */
            route_selector, (unsigned)route_table_ptr,
            fwd_track, span_progress,
            lat_bias, left_dev, right_dev,
            active_upper, active_lower,
            (int)encounter_steer, (int)brake_flag,
            (int)b371, (int)b372, (int)b373, (int)b374,
            (int)b375, (int)b376);
    }

    fflush(s_fp);
    /* DIAG: also dump extended physics for orig comparison. */
    emit_extended_physics();
    s_active_call = 0;
    /* Silence unused warnings if pre-snapshot is not consumed in this build. */
    (void)s_pre_route_selector;
    (void)s_pre_lat_bias;
}
