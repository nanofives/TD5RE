/*
 * td5_pilot_trace_00406650.c -- Port-side CSV emitter for the precise-port
 * pilot @ 0x00406650 UpdateVehicleActor.
 *
 * Mirrors tools/frida_pool1_00406650.js column-for-column so the diff tool
 * can match rows by (sim_tick, slot, phase) and report which column diverges
 * first.
 *
 * Uses raw byte offsets, not struct field access, so we are insulated from
 * port-side struct layout drift. Offsets match the Frida script exactly.
 */
#include "td5_pilot_trace_00406650.h"
#include "../../../re/include/td5_actor_struct.h"
#include "td5re.h"     /* g_td5 (paused field) */
#include "td5_game.h"  /* g_game_type, g_special_encounter, g_replay_mode */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool1_00406650.csv"

/* Field offsets — exact mirrors of Frida script. */
#define O_STEERING_CMD       0x30C
#define O_ENGINE_SPEED       0x310
#define O_LONG_SPEED         0x314
#define O_FINISH_TIME        0x328
#define O_ACCUM_DIST         0x32C
#define O_PEAK_SPEED         0x330  /* int16 */
#define O_AVG_SPEED          0x332  /* int16 */
#define O_FRAME_COUNTER      0x338  /* int16 */
#define O_ENCOUNTER_STEER    0x33E  /* int16 */
#define O_PENDING_FINISH     0x344  /* uint16 */
#define O_TIMING_COUNTER     0x34C  /* uint16 */
#define O_AIRBORNE_FC        0x360  /* int16 */
#define O_BRAKE_FLAG         0x36D
#define O_SLOT_INDEX         0x375
#define O_SCF                0x376
#define O_VEHICLE_MODE       0x379
#define O_TRACK_CONTACT      0x37B
#define O_WCB                0x37C
#define O_OLD_WCB            0x37D
#define O_GHOST_FLAG         0x37E
#define O_GRIP_REDUCTION     0x380
#define O_PREV_RACE_POS      0x381
#define O_RACE_POSITION      0x383

/* Pilot target slot (Edinburgh PlayerIsAI=1 baseline keeps slot 0 as AI). */
#define TARGET_SLOT  0

/* Globals matching the Frida-side capture. The port's g_td5.paused mirrors
 * gRaceCameraTransitionGate (set during the countdown overlay and cleared at
 * "GO"). g_game_paused is file-local to td5_physics.c; we don't read it here
 * because it's the same value at ENTER/LEAVE — informational only. */
extern int td5_trace_current_sim_tick(void);

static inline int16_t  rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint16_t rd_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t  rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint8_t  rd_u8 (const uint8_t *p) { return *p; }

static FILE *s_fp = NULL;

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,phase,"
        "g_game_type,g_special_encounter,g_replay_mode,g_race_camera_gate,"
        "frame_counter,vehicle_mode,ghost_flag,"
        "long_speed,finish_time,accum_dist,peak_speed,avg_speed,"
        "timing_counter,airborne_fc,wcb,old_wcb,"
        "grip_reduction,race_position,prev_race_position,"
        "engine_speed,steering_cmd,encounter_steer,brake_flag,"
        "scf,track_contact,pending_finish\n");
}

static void emit_row(const TD5_Actor *actor, const char *phase) {
    if (!actor) return;
    int slot = actor->slot_index;
    if (slot != TARGET_SLOT) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    const uint8_t *base = (const uint8_t *)actor;
    uint32_t tick   = (uint32_t)td5_trace_current_sim_tick();
    int      paused = g_td5.paused;

    fprintf(s_fp,
        "%u,%d,%d,%s,"
        "%d,%d,%d,%d,"
        "%d,%u,%u,"
        "%d,%d,%d,%d,%d,"
        "%u,%d,%u,%u,"
        "%u,%u,%u,"
        "%d,%d,%d,%u,"
        "%u,%u,%u\n",
        tick, paused, slot, phase,
        g_game_type, g_special_encounter, g_replay_mode, g_td5.paused,
        (int)rd_i16(base + O_FRAME_COUNTER),
        rd_u8 (base + O_VEHICLE_MODE),
        rd_u8 (base + O_GHOST_FLAG),
        rd_i32(base + O_LONG_SPEED),
        rd_i32(base + O_FINISH_TIME),
        rd_i32(base + O_ACCUM_DIST),
        (int)rd_i16(base + O_PEAK_SPEED),
        (int)rd_i16(base + O_AVG_SPEED),
        rd_u16(base + O_TIMING_COUNTER),
        (int)rd_i16(base + O_AIRBORNE_FC),
        rd_u8 (base + O_WCB),
        rd_u8 (base + O_OLD_WCB),
        rd_u8 (base + O_GRIP_REDUCTION),
        rd_u8 (base + O_RACE_POSITION),
        rd_u8 (base + O_PREV_RACE_POS),
        rd_i32(base + O_ENGINE_SPEED),
        rd_i32(base + O_STEERING_CMD),
        (int)rd_i16(base + O_ENCOUNTER_STEER),
        rd_u8 (base + O_BRAKE_FLAG),
        rd_u8 (base + O_SCF),
        rd_u8 (base + O_TRACK_CONTACT),
        rd_u16(base + O_PENDING_FINISH));

    fflush(s_fp);
}

void td5_pilot_emit_00406650_enter(const TD5_Actor *actor) {
    emit_row(actor, "ENTER");
}

void td5_pilot_emit_00406650_leave(const TD5_Actor *actor) {
    emit_row(actor, "LEAVE");
}
