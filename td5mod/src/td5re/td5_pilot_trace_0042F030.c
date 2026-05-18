/*
 * td5_pilot_trace_0042F030.c -- Port-side CSV emitter for the
 * ComputeDriveTorqueFromGearCurve pilot (pool13 / 0x0042F030).
 *
 * Mirrors tools/frida_pool13_0042F030.js column-for-column. Uses raw byte
 * offsets (not struct field access) so the schema is insulated from port-
 * side struct drift and matches what Frida reads from TD5_d3d.exe.
 *
 * Function reads (no writes — pure leaf):
 *   actor + 0x310  engine_speed_accum  int32
 *   actor + 0x33E  encounter_steering  int16 (signed throttle)
 *   actor + 0x36B  current_gear        uint8
 *   tuning + 0x00..0x1F  LUT[16] int16 (torque curve, per-512-rpm samples)
 *   tuning + 0x2E + gear*2  gear_ratio[gear] int16
 *   tuning + 0x68  torque_mult int16
 *   tuning + 0x72  redline     int16
 */
#include "td5_pilot_trace_0042F030.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool13_0042F030.csv"

/* Field offsets — matches frida_pool13_0042F030.js exactly. */
#define O_ENGINE_SPEED   0x310
#define O_THROTTLE_CMD   0x33E
#define O_CURRENT_GEAR   0x36B

#define T_TORQUE_MULT    0x68
#define T_REDLINE        0x72
#define T_GEAR_RATIO_BASE 0x2E

#define LUT_COUNT        16

#define TARGET_SLOT      0

typedef struct {
    char     actor_addr[20];
    char     tuning_ptr[20];
    int32_t  engine_speed_accum;
    int16_t  throttle_cmd;
    uint8_t  current_gear;
    int16_t  torque_mult;
    int16_t  redline;
    int16_t  gear_ratio_curr;
    int16_t  lut[LUT_COUNT];
} PilotInputs;

static FILE          *s_fp = NULL;
static int            s_active_call = 0;
static uintptr_t      s_caller_ra = 0;
static uint32_t       s_tick = 0;
static int            s_paused = 0;
static int            s_slot = 0;
static const TD5_Actor *s_actor = NULL;
static PilotInputs    s_inputs;

extern int td5_trace_current_sim_tick(void);

static inline int16_t  rd_i16(const uint8_t *p) { int16_t v; memcpy(&v, p, sizeof v); return v; }
static inline int32_t  rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static inline uint8_t  rd_u8 (const uint8_t *p) { return *p; }

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,caller_ra,"
        "actor_addr,tuning_ptr,"
        "engine_speed_accum,throttle_cmd,current_gear,"
        "torque_mult,redline,gear_ratio_curr,"
        "lut0,lut1,lut2,lut3,lut4,lut5,lut6,lut7,"
        "lut8,lut9,lut10,lut11,lut12,lut13,lut14,lut15,"
        "lut_index_used,lut_frac_used,"
        "return_value\n");
}

static int actor_slot_index(const TD5_Actor *actor) {
    return actor->slot_index;
}

void td5_pilot_emit_0042F030_enter(const TD5_Actor *actor, uintptr_t caller_ra) {
    s_active_call = 0;
    if (!actor) return;
    int slot = actor_slot_index(actor);
    if (slot != TARGET_SLOT) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    const uint8_t *base = (const uint8_t *)actor;
    const uint8_t *tuning = (const uint8_t *)actor->tuning_data_ptr;
    if (!tuning) return;

    s_active_call = 1;
    s_caller_ra   = caller_ra;
    s_tick        = (uint32_t)td5_trace_current_sim_tick();
    s_paused      = 0;
    s_slot        = slot;
    s_actor       = actor;

    snprintf(s_inputs.actor_addr, sizeof s_inputs.actor_addr, "0x%p", (const void *)actor);
    snprintf(s_inputs.tuning_ptr, sizeof s_inputs.tuning_ptr, "0x%p", (const void *)tuning);

    s_inputs.engine_speed_accum = rd_i32(base + O_ENGINE_SPEED);
    s_inputs.throttle_cmd       = rd_i16(base + O_THROTTLE_CMD);
    s_inputs.current_gear       = rd_u8 (base + O_CURRENT_GEAR);

    s_inputs.torque_mult        = rd_i16(tuning + T_TORQUE_MULT);
    s_inputs.redline            = rd_i16(tuning + T_REDLINE);
    s_inputs.gear_ratio_curr    = rd_i16(tuning + T_GEAR_RATIO_BASE + (uint8_t)s_inputs.current_gear * 2);

    for (int i = 0; i < LUT_COUNT; i++) {
        s_inputs.lut[i] = rd_i16(tuning + i * 2);
    }
}

void td5_pilot_emit_0042F030_leave(const TD5_Actor *actor, int32_t return_value,
                                   int32_t lut_index_used, int32_t lut_frac_used) {
    if (!s_active_call || !s_fp || actor != s_actor) {
        s_active_call = 0;
        return;
    }

    fprintf(s_fp,
        "%u,%d,%d,0x%lx,"
        "%s,%s,"
        "%d,%d,%u,"
        "%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,%d,%d,"
        "%d,%d,"
        "%d\n",
        s_tick, s_paused, s_slot, (unsigned long)s_caller_ra,
        s_inputs.actor_addr, s_inputs.tuning_ptr,
        s_inputs.engine_speed_accum, (int)s_inputs.throttle_cmd, (unsigned)s_inputs.current_gear,
        (int)s_inputs.torque_mult, (int)s_inputs.redline, (int)s_inputs.gear_ratio_curr,
        (int)s_inputs.lut[0],  (int)s_inputs.lut[1],  (int)s_inputs.lut[2],  (int)s_inputs.lut[3],
        (int)s_inputs.lut[4],  (int)s_inputs.lut[5],  (int)s_inputs.lut[6],  (int)s_inputs.lut[7],
        (int)s_inputs.lut[8],  (int)s_inputs.lut[9],  (int)s_inputs.lut[10], (int)s_inputs.lut[11],
        (int)s_inputs.lut[12], (int)s_inputs.lut[13], (int)s_inputs.lut[14], (int)s_inputs.lut[15],
        lut_index_used, lut_frac_used,
        return_value);
    fflush(s_fp);
    s_active_call = 0;
}
