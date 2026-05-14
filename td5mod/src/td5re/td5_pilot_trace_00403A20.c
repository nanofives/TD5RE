/*
 * td5_pilot_trace_00403A20.c -- Port-side CSV emitter for the
 * IntegrateWheelSuspensionTravel pilot (pool5 / 0x00403A20).
 *
 * Mirrors tools/frida_pool5_00403A20.js column-for-column. Uses raw byte
 * offsets (not struct field access) so the schema is insulated from port-
 * side struct drift and matches what Frida reads from TD5_d3d.exe.
 */
#include "td5_pilot_trace_00403A20.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool5_00403A20.csv"

/* Field offsets — matches frida_pool5_00403A20.js exactly. */
#define O_WORLD_POS_X   0x1FC
#define O_WORLD_POS_Z   0x204
#define O_HIRES         0x298
#define O_CENTER_POS    0x2CC
#define O_CENTER_VEL    0x2D0
#define O_SUSP_POS      0x2DC
#define O_SPRING_DV     0x2EC
#define O_LOAD_ACCUM    0x2FC

#define C_POS_DAMP      0x5E
#define C_VEL_DAMP      0x60
#define C_SPRING        0x62
#define C_TRAVEL_LIM    0x64
#define C_LOAD_SCALE    0x66

#define TARGET_SLOT     0

typedef struct {
    int32_t hires_x, hires_y, hires_z;
    int32_t susp_pos_in;
    int32_t spring_dv_in;
    int32_t load_accum;
} WheelInputSnap;

typedef struct {
    char     actor_addr[20];
    char     cardef_ptr[20];
    int16_t  k_pos_damp;
    int16_t  k_vel_damp;
    int16_t  k_spring;
    int16_t  k_travel_lim;
    int16_t  k_load_scale;
    int32_t  accel_x;
    int32_t  accel_z;
    int32_t  world_pos_x;
    int32_t  world_pos_z;
    int32_t  center_pos_in;
    int32_t  center_vel_in;
    WheelInputSnap wheels[4];
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
static inline uintptr_t rd_ptr(const uint8_t *p) { uintptr_t v; memcpy(&v, p, sizeof v); return v; }

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,paused,slot,wheel,caller_ra,"
        "actor_addr,cardef_ptr,"
        "k_pos_damp,k_vel_damp,k_spring,k_travel_lim,k_load_scale,"
        "accel_x,accel_z,"
        "world_pos_x,world_pos_z,"
        "center_pos_in,center_vel_in,"
        "hires_x,hires_y,hires_z,"
        "susp_pos_in,spring_dv_in,load_accum,"
        "susp_pos_out,spring_dv_out,"
        "center_pos_out,center_vel_out\n");
}

static int actor_slot_index(const TD5_Actor *actor) {
    return actor->slot_index;
}

void td5_pilot_emit_00403A20_enter(const TD5_Actor *actor, int32_t accel_x, int32_t accel_z, uintptr_t caller_ra) {
    s_active_call = 0;
    int slot = actor_slot_index(actor);
    if (slot != TARGET_SLOT) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    const uint8_t *base = (const uint8_t *)actor;
    /* tuning_data_ptr lives at actor offset matching the original's
     * param_2 in IntegrateWheelSuspensionTravel — the port's get_phys()
     * returns this same pointer (td5_physics.c:438). */
    const uint8_t *cardef = (const uint8_t *)actor->tuning_data_ptr;

    s_active_call = 1;
    s_caller_ra   = caller_ra;
    s_tick        = (uint32_t)td5_trace_current_sim_tick();
    s_paused      = 0;
    s_slot        = slot;
    s_actor       = actor;

    snprintf(s_inputs.actor_addr, sizeof s_inputs.actor_addr, "0x%p", (const void *)actor);
    snprintf(s_inputs.cardef_ptr, sizeof s_inputs.cardef_ptr, "0x%p", (const void *)cardef);

    s_inputs.k_pos_damp    = cardef ? rd_i16(cardef + C_POS_DAMP)   : 0;
    s_inputs.k_vel_damp    = cardef ? rd_i16(cardef + C_VEL_DAMP)   : 0;
    s_inputs.k_spring      = cardef ? rd_i16(cardef + C_SPRING)     : 0;
    s_inputs.k_travel_lim  = cardef ? rd_i16(cardef + C_TRAVEL_LIM) : 0;
    s_inputs.k_load_scale  = cardef ? rd_i16(cardef + C_LOAD_SCALE) : 0;

    s_inputs.accel_x = accel_x;
    s_inputs.accel_z = accel_z;

    s_inputs.world_pos_x   = rd_i32(base + O_WORLD_POS_X);
    s_inputs.world_pos_z   = rd_i32(base + O_WORLD_POS_Z);
    s_inputs.center_pos_in = rd_i32(base + O_CENTER_POS);
    s_inputs.center_vel_in = rd_i32(base + O_CENTER_VEL);

    for (int w = 0; w < 4; w++) {
        const uint8_t *hbase = base + O_HIRES + w * 0xC;
        s_inputs.wheels[w].hires_x      = rd_i32(hbase + 0);
        s_inputs.wheels[w].hires_y      = rd_i32(hbase + 4);
        s_inputs.wheels[w].hires_z      = rd_i32(hbase + 8);
        s_inputs.wheels[w].susp_pos_in  = rd_i32(base + O_SUSP_POS  + w * 4);
        s_inputs.wheels[w].spring_dv_in = rd_i32(base + O_SPRING_DV + w * 4);
        s_inputs.wheels[w].load_accum   = rd_i32(base + O_LOAD_ACCUM + w * 4);
    }
}

void td5_pilot_emit_00403A20_leave(const TD5_Actor *actor) {
    if (!s_active_call || !s_fp || actor != s_actor) {
        s_active_call = 0;
        return;
    }
    const uint8_t *base = (const uint8_t *)actor;
    int32_t center_pos_out = rd_i32(base + O_CENTER_POS);
    int32_t center_vel_out = rd_i32(base + O_CENTER_VEL);
    for (int w = 0; w < 4; w++) {
        int32_t susp_pos_out  = rd_i32(base + O_SUSP_POS  + w * 4);
        int32_t spring_dv_out = rd_i32(base + O_SPRING_DV + w * 4);
        const WheelInputSnap *wi = &s_inputs.wheels[w];

        fprintf(s_fp,
            "%u,%d,%d,%d,0x%lx,"
            "%s,%s,"
            "%d,%d,%d,%d,%d,"
            "%d,%d,"
            "%d,%d,"
            "%d,%d,"
            "%d,%d,%d,"
            "%d,%d,%d,"
            "%d,%d,"
            "%d,%d\n",
            s_tick, s_paused, s_slot, w, (unsigned long)s_caller_ra,
            s_inputs.actor_addr, s_inputs.cardef_ptr,
            s_inputs.k_pos_damp, s_inputs.k_vel_damp, s_inputs.k_spring,
            s_inputs.k_travel_lim, s_inputs.k_load_scale,
            s_inputs.accel_x, s_inputs.accel_z,
            s_inputs.world_pos_x, s_inputs.world_pos_z,
            s_inputs.center_pos_in, s_inputs.center_vel_in,
            wi->hires_x, wi->hires_y, wi->hires_z,
            wi->susp_pos_in, wi->spring_dv_in, wi->load_accum,
            susp_pos_out, spring_dv_out,
            center_pos_out, center_vel_out);
    }
    fflush(s_fp);
    s_active_call = 0;
}
