/*
 * td5_pilot_trace_004440F0.c -- Port-side CSV emitter for the
 * UpdateActorTrackPosition pilot (pool10 / 0x004440F0).
 *
 * Mirrors tools/frida_pool10_004440F0.js column-for-column. Slot is
 * resolved by the probe pointer's offset within the actor table
 * (port: g_actor_table_base + slot * TD5_ACTOR_STRIDE). Only TARGET_SLOT
 * emits rows. Probes that don't live inside an actor block (camera/
 * traffic stack-locals) are skipped on both sides.
 */
#include "td5_pilot_trace_004440F0.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool10_004440F0.csv"
#define TARGET_SLOT  0
#define MAX_CALLS    12000

/* Externs from td5_game.c */
extern uint8_t *g_actor_table_base;
extern int      td5_trace_current_sim_tick(void);

#ifndef TD5_ACTOR_STRIDE
#define TD5_ACTOR_STRIDE 0x388
#endif
#define ACTOR_COUNT 6

typedef struct {
    int16_t span_index;
    int16_t word_1;
    int16_t accum;
    int16_t high_water;
    int16_t word_4;
    int16_t word_5;
    int8_t  sub_lane;
    uint8_t flag_13;
} ProbeSnap;

static FILE       *s_fp = NULL;
static int         s_last_tick = -1;
static int         s_call_idx = 0;
static int         s_total = 0;
static int         s_active = 0;
static int         s_slot = -1;
static int32_t     s_probe_off = -1;
static int32_t     s_wpos_off  = -1;
static uintptr_t   s_caller_ra = 0;
static int32_t     s_world_x = 0;
static int32_t     s_world_z = 0;
static const void *s_probe = NULL;
static const void *s_wpos  = NULL;
static int         s_tick = 0;
static int         s_paused = 0;
static ProbeSnap   s_in;

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,call_idx,caller_ra,caller_tag,"
        "probe_off,wpos_off,"
        "world_x_in,world_z_in,"
        "span_in,accum_in,hi_in,sub_lane_in,"
        "word_1_in,word_4_in,word_5_in,flag_13_in,"
        "span_out,accum_out,hi_out,sub_lane_out,"
        "word_1_out,word_4_out,word_5_out,flag_13_out,"
        "retval\n");
}

/* Returns slot (0..ACTOR_COUNT-1) if probe is within probe range
 * (offset 0x00..0x8F inside an actor), else -1. Also writes the
 * offset within that actor block to *off_out. */
static int resolve_slot(const void *probe, int32_t *off_out) {
    if (!g_actor_table_base || !probe) return -1;
    intptr_t rel = (intptr_t)((const uint8_t *)probe - g_actor_table_base);
    if (rel < 0) return -1;
    int slot = (int)(rel / (intptr_t)TD5_ACTOR_STRIDE);
    int32_t off = (int32_t)(rel - (intptr_t)slot * (intptr_t)TD5_ACTOR_STRIDE);
    if (slot < 0 || slot >= ACTOR_COUNT) return -1;
    if (off < 0x00 || off > 0x8F) return -1;
    if (off_out) *off_out = off;
    return slot;
}

static int32_t actor_relative_off(int slot, const void *p) {
    if (!g_actor_table_base || slot < 0) return -1;
    intptr_t rel = (intptr_t)((const uint8_t *)p
                              - (g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE));
    if (rel < 0 || rel >= (intptr_t)TD5_ACTOR_STRIDE) return -1;
    return (int32_t)rel;
}

static void snapshot_probe(const void *probe, ProbeSnap *out) {
    const uint8_t *p = (const uint8_t *)probe;
    memcpy(&out->span_index, p + 0,  2);
    memcpy(&out->word_1,     p + 2,  2);
    memcpy(&out->accum,      p + 4,  2);
    memcpy(&out->high_water, p + 6,  2);
    memcpy(&out->word_4,     p + 8,  2);
    memcpy(&out->word_5,     p + 10, 2);
    out->sub_lane = (int8_t)p[12];
    out->flag_13  = p[13];
}

void td5_pilot_emit_004440F0_enter(const void *probe,
                                   const int32_t *world_pos,
                                   uintptr_t caller_ra) {
    s_active = 0;
    if (!probe || !world_pos) return;
    if (s_total >= MAX_CALLS) return;

    int32_t off = -1;
    int slot = resolve_slot(probe, &off);
    if (slot != TARGET_SLOT) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    int tick = td5_trace_current_sim_tick();
    if (tick != s_last_tick) {
        s_last_tick = tick;
        s_call_idx = 0;
    }

    s_active     = 1;
    s_probe      = probe;
    s_wpos       = world_pos;
    s_tick       = tick;
    s_paused     = 0;
    s_slot       = slot;
    s_probe_off  = off;
    s_wpos_off   = actor_relative_off(slot, (const void *)world_pos);
    s_caller_ra  = caller_ra;
    s_world_x    = world_pos[0];
    s_world_z    = world_pos[2];
    snapshot_probe(probe, &s_in);
    s_total++;
}

void td5_pilot_emit_004440F0_leave(const void *probe, int retval) {
    if (!s_active || !s_fp || probe != s_probe) { s_active = 0; return; }

    ProbeSnap out;
    snapshot_probe(probe, &out);
    int call_idx = s_call_idx++;

    fprintf(s_fp,
        "%d,%d,%d,%d,"
        "0x%lx,PORT,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%u,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%u,"
        "%d\n",
        s_tick, s_paused, s_slot, call_idx,
        (unsigned long)s_caller_ra,
        s_probe_off, s_wpos_off,
        s_world_x, s_world_z,
        s_in.span_index, s_in.accum, s_in.high_water, (int)s_in.sub_lane,
        s_in.word_1, s_in.word_4, s_in.word_5, (unsigned)s_in.flag_13,
        out.span_index, out.accum, out.high_water, (int)out.sub_lane,
        out.word_1, out.word_4, out.word_5, (unsigned)out.flag_13,
        retval & 0xFF);
    if ((s_total & 0x1F) == 0) fflush(s_fp);

    s_active = 0;
}
