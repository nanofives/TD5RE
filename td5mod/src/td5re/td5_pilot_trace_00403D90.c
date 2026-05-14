/*
 * td5_pilot_trace_00403D90.c -- Port-side CSV emitter for the
 * UpdateVehicleState0fDamping pilot (pool6 / 0x00403D90).
 *
 * Mirrors tools/frida_pool6_00403D90.js column-for-column. Uses raw byte
 * offsets so the schema is insulated from port-side struct drift.
 */
#include "td5_pilot_trace_00403D90.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool6_00403D90.csv"

/* Field offsets — matches frida_pool6_00403D90.js exactly. */
#define O_ANGVEL_ROLL    0x1C0
#define O_ANGVEL_PITCH   0x1C8
#define O_LINVEL_X       0x1CC
#define O_LINVEL_Z       0x1D4
#define O_EULER_ROLL     0x1F0
#define O_EULER_YAW      0x1F4
#define O_DISP_ROLL      0x208
#define O_DISP_YAW       0x20A
#define O_DISP_PITCH     0x20C
#define O_LONG_SPEED     0x314
#define O_LAT_SPEED      0x318
#define O_FRONT_SLIP     0x31C
#define O_REAR_SLIP      0x320
#define O_SLIP_ACC_X     0x340
#define O_SLIP_ACC_Z     0x342
#define O_SURFACE_FLAGS  0x376

#define TARGET_SLOT      0

typedef struct {
    char     actor_addr[20];
    int32_t  angvel_roll, angvel_pitch;
    int32_t  linvel_x, linvel_z;
    int32_t  euler_roll, euler_yaw;
    int16_t  disp_roll, disp_yaw, disp_pitch;
    int32_t  long_speed, lat_speed;
    int32_t  front_slip, rear_slip;
    int16_t  slip_acc_x, slip_acc_z;
    uint8_t  surface_flags;
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
        "actor_addr,"
        "in_angvel_roll,in_angvel_pitch,"
        "in_linvel_x,in_linvel_z,"
        "in_euler_roll,in_euler_yaw,"
        "in_disp_roll,in_disp_yaw,in_disp_pitch,"
        "in_long_speed,in_lat_speed,"
        "in_front_slip,in_rear_slip,"
        "in_slip_acc_x,in_slip_acc_z,"
        "in_surface_flags,"
        "out_angvel_roll,out_angvel_pitch,"
        "out_front_slip,out_rear_slip,"
        "out_slip_acc_x,out_slip_acc_z,"
        "out_surface_flags\n");
}

static int actor_slot_index(const TD5_Actor *actor) {
    return actor->slot_index;
}

void td5_pilot_emit_00403D90_enter(const TD5_Actor *actor, uintptr_t caller_ra) {
    s_active_call = 0;
    int slot = actor_slot_index(actor);
    if (slot != TARGET_SLOT) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    const uint8_t *base = (const uint8_t *)actor;

    s_active_call = 1;
    s_caller_ra   = caller_ra;
    s_tick        = (uint32_t)td5_trace_current_sim_tick();
    s_paused      = 0;
    s_slot        = slot;
    s_actor       = actor;

    snprintf(s_inputs.actor_addr, sizeof s_inputs.actor_addr, "0x%p", (const void *)actor);
    s_inputs.angvel_roll   = rd_i32(base + O_ANGVEL_ROLL);
    s_inputs.angvel_pitch  = rd_i32(base + O_ANGVEL_PITCH);
    s_inputs.linvel_x      = rd_i32(base + O_LINVEL_X);
    s_inputs.linvel_z      = rd_i32(base + O_LINVEL_Z);
    s_inputs.euler_roll    = rd_i32(base + O_EULER_ROLL);
    s_inputs.euler_yaw     = rd_i32(base + O_EULER_YAW);
    s_inputs.disp_roll     = rd_i16(base + O_DISP_ROLL);
    s_inputs.disp_yaw      = rd_i16(base + O_DISP_YAW);
    s_inputs.disp_pitch    = rd_i16(base + O_DISP_PITCH);
    s_inputs.long_speed    = rd_i32(base + O_LONG_SPEED);
    s_inputs.lat_speed     = rd_i32(base + O_LAT_SPEED);
    s_inputs.front_slip    = rd_i32(base + O_FRONT_SLIP);
    s_inputs.rear_slip     = rd_i32(base + O_REAR_SLIP);
    s_inputs.slip_acc_x    = rd_i16(base + O_SLIP_ACC_X);
    s_inputs.slip_acc_z    = rd_i16(base + O_SLIP_ACC_Z);
    s_inputs.surface_flags = rd_u8 (base + O_SURFACE_FLAGS);
}

void td5_pilot_emit_00403D90_leave(const TD5_Actor *actor) {
    if (!s_active_call || !s_fp || actor != s_actor) {
        s_active_call = 0;
        return;
    }
    const uint8_t *base = (const uint8_t *)actor;
    int32_t out_angvel_roll  = rd_i32(base + O_ANGVEL_ROLL);
    int32_t out_angvel_pitch = rd_i32(base + O_ANGVEL_PITCH);
    int32_t out_front_slip   = rd_i32(base + O_FRONT_SLIP);
    int32_t out_rear_slip    = rd_i32(base + O_REAR_SLIP);
    int16_t out_slip_acc_x   = rd_i16(base + O_SLIP_ACC_X);
    int16_t out_slip_acc_z   = rd_i16(base + O_SLIP_ACC_Z);
    uint8_t out_surface_flags= rd_u8 (base + O_SURFACE_FLAGS);

    fprintf(s_fp,
        "%u,%d,%d,0x%lx,"
        "%s,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%u,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%u\n",
        s_tick, s_paused, s_slot, (unsigned long)s_caller_ra,
        s_inputs.actor_addr,
        s_inputs.angvel_roll, s_inputs.angvel_pitch,
        s_inputs.linvel_x, s_inputs.linvel_z,
        s_inputs.euler_roll, s_inputs.euler_yaw,
        s_inputs.disp_roll, s_inputs.disp_yaw, s_inputs.disp_pitch,
        s_inputs.long_speed, s_inputs.lat_speed,
        s_inputs.front_slip, s_inputs.rear_slip,
        s_inputs.slip_acc_x, s_inputs.slip_acc_z,
        s_inputs.surface_flags,
        out_angvel_roll, out_angvel_pitch,
        out_front_slip, out_rear_slip,
        out_slip_acc_x, out_slip_acc_z,
        out_surface_flags);
    fflush(s_fp);
    s_active_call = 0;
}
