/*
 * td5_pilot_trace_00404EC0.c -- Port-side CSV emitter for
 * UpdateAIVehicleDynamics pilot (pool12 / 0x00404EC0).
 *
 * Mirrors tools/frida_pool12_00404EC0.js column-for-column.
 */
#include "td5_pilot_trace_00404EC0.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH "log/port/pool12_00404EC0.csv"

/* Actor field offsets */
#define O_TUNING_PTR        0x1BC
#define O_TRACK_SPAN_RAW    0x80
#define O_SUB_LANE_INDEX    0x8C
#define O_EULER_YAW         0x1C4
#define O_LIN_VX            0x1CC
#define O_LIN_VY            0x1D0
#define O_LIN_VZ            0x1D4
#define O_ANG_YAW           0x1F4
#define O_CENTER_POS        0x2CC
#define O_CENTER_VEL        0x2D0
#define O_SUSP_POS          0x2DC
#define O_STEER_CMD         0x30C
#define O_LONG_SPEED        0x314
#define O_LAT_SPEED         0x318
#define O_FRONT_SLIP_EXCESS 0x31C
#define O_REAR_SLIP_EXCESS  0x320
#define O_ENCOUNTER_STEER   0x33E
#define O_SLIP_METRIC       0x33C
#define O_TIRE_SLIP_X       0x340
#define O_TIRE_SLIP_Z       0x342
#define O_GEAR              0x36B
#define O_BRAKE_FLAG        0x36D
#define O_SURFACE_CHASSIS   0x370

/* Cardef field offsets */
#define C_INERTIA      0x20
#define C_HALF_WB      0x24
#define C_FRONT_W      0x28
#define C_REAR_W       0x2A
#define C_TIRE_GRIP    0x2C
#define C_SPRING       0x62
#define C_DAMP_HIGH    0x6A
#define C_DAMP_LOW     0x6C
#define C_BRAKE_FRONT  0x6E
#define C_BRAKE_REAR   0x70
#define C_SPEED_LIMIT  0x74

#define TARGET_SLOT     0

typedef struct {
    char     actor_addr[20];
    char     cardef_ptr[20];
    uint8_t  surface_chassis_in;
    int16_t  track_span_raw;
    uint8_t  sub_lane_index;
    int32_t  euler_yaw_in;
    int32_t  lin_vx_in;
    int32_t  lin_vz_in;
    int32_t  ang_yaw_in;
    int32_t  steer_cmd_in;
    int16_t  encounter_steer_in;
    uint8_t  gear_in;
    uint8_t  brake_flag_in;
    int32_t  center_pos_in;
    int32_t  center_vel_in;
    int32_t  susp_pos_in[4];
    int32_t  inertia;
    int32_t  half_wb;
    int16_t  Wf, Wr, tire_grip, k_spring;
    int16_t  damp_high, damp_low, brake_front, brake_rear, speed_limit;
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
        "actor_addr,cardef_ptr,surface_chassis_in,"
        "track_span_raw,sub_lane_index,"
        "euler_yaw_in,lin_vx_in,lin_vz_in,ang_yaw_in,"
        "steer_cmd_in,encounter_steer_in,gear_in,brake_flag_in,"
        "center_pos_in,center_vel_in,"
        "susp_pos0_in,susp_pos1_in,susp_pos2_in,susp_pos3_in,"
        "inertia,half_wb,Wf,Wr,tire_grip,k_spring,"
        "damp_high,damp_low,brake_front,brake_rear,speed_limit,"
        "lin_vx_out,lin_vz_out,ang_yaw_out,"
        "long_speed_out,lat_speed_out,"
        "front_slip_excess_out,rear_slip_excess_out,slip_metric_out,"
        "tire_slip_x_out,tire_slip_z_out,surface_chassis_out,"
        "center_pos_out,center_vel_out,"
        "susp_pos0_out,susp_pos1_out,susp_pos2_out,susp_pos3_out\n");
}

void td5_pilot_emit_00404EC0_enter(const TD5_Actor *actor, uintptr_t caller_ra) {
    s_active_call = 0;
    int slot = actor->slot_index;
    if (slot != TARGET_SLOT) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    const uint8_t *base = (const uint8_t *)actor;
    const uint8_t *cardef = (const uint8_t *)actor->tuning_data_ptr;

    s_active_call = 1;
    s_caller_ra   = caller_ra;
    s_tick        = (uint32_t)td5_trace_current_sim_tick();
    s_paused      = 0;
    s_slot        = slot;
    s_actor       = actor;

    snprintf(s_inputs.actor_addr, sizeof s_inputs.actor_addr, "0x%p", (const void *)actor);
    snprintf(s_inputs.cardef_ptr, sizeof s_inputs.cardef_ptr, "0x%p", (const void *)cardef);

    s_inputs.surface_chassis_in = rd_u8(base + O_SURFACE_CHASSIS);
    s_inputs.track_span_raw     = rd_i16(base + O_TRACK_SPAN_RAW);
    s_inputs.sub_lane_index     = rd_u8(base + O_SUB_LANE_INDEX);
    s_inputs.euler_yaw_in       = rd_i32(base + O_EULER_YAW);
    s_inputs.lin_vx_in          = rd_i32(base + O_LIN_VX);
    s_inputs.lin_vz_in          = rd_i32(base + O_LIN_VZ);
    s_inputs.ang_yaw_in         = rd_i32(base + O_ANG_YAW);
    s_inputs.steer_cmd_in       = rd_i32(base + O_STEER_CMD);
    s_inputs.encounter_steer_in = rd_i16(base + O_ENCOUNTER_STEER);
    s_inputs.gear_in            = rd_u8(base + O_GEAR);
    s_inputs.brake_flag_in      = rd_u8(base + O_BRAKE_FLAG);
    s_inputs.center_pos_in      = rd_i32(base + O_CENTER_POS);
    s_inputs.center_vel_in      = rd_i32(base + O_CENTER_VEL);
    for (int w = 0; w < 4; w++) {
        s_inputs.susp_pos_in[w] = rd_i32(base + O_SUSP_POS + w * 4);
    }

    if (cardef) {
        s_inputs.inertia     = rd_i32(cardef + C_INERTIA);
        s_inputs.half_wb     = rd_i32(cardef + C_HALF_WB);
        s_inputs.Wf          = rd_i16(cardef + C_FRONT_W);
        s_inputs.Wr          = rd_i16(cardef + C_REAR_W);
        s_inputs.tire_grip   = rd_i16(cardef + C_TIRE_GRIP);
        s_inputs.k_spring    = rd_i16(cardef + C_SPRING);
        s_inputs.damp_high   = rd_i16(cardef + C_DAMP_HIGH);
        s_inputs.damp_low    = rd_i16(cardef + C_DAMP_LOW);
        s_inputs.brake_front = rd_i16(cardef + C_BRAKE_FRONT);
        s_inputs.brake_rear  = rd_i16(cardef + C_BRAKE_REAR);
        s_inputs.speed_limit = rd_i16(cardef + C_SPEED_LIMIT);
    } else {
        s_inputs.inertia = s_inputs.half_wb = 0;
        s_inputs.Wf = s_inputs.Wr = s_inputs.tire_grip = s_inputs.k_spring = 0;
        s_inputs.damp_high = s_inputs.damp_low = 0;
        s_inputs.brake_front = s_inputs.brake_rear = s_inputs.speed_limit = 0;
    }
}

void td5_pilot_emit_00404EC0_leave(const TD5_Actor *actor) {
    if (!s_active_call || !s_fp || actor != s_actor) {
        s_active_call = 0;
        return;
    }
    const uint8_t *base = (const uint8_t *)actor;

    int32_t lin_vx_out  = rd_i32(base + O_LIN_VX);
    int32_t lin_vz_out  = rd_i32(base + O_LIN_VZ);
    int32_t ang_yaw_out = rd_i32(base + O_ANG_YAW);
    int32_t long_speed_out = rd_i32(base + O_LONG_SPEED);
    int32_t lat_speed_out  = rd_i32(base + O_LAT_SPEED);
    int32_t front_slip_excess_out = rd_i32(base + O_FRONT_SLIP_EXCESS);
    int32_t rear_slip_excess_out  = rd_i32(base + O_REAR_SLIP_EXCESS);
    int16_t slip_metric_out = rd_i16(base + O_SLIP_METRIC);
    int16_t tire_slip_x_out = rd_i16(base + O_TIRE_SLIP_X);
    int16_t tire_slip_z_out = rd_i16(base + O_TIRE_SLIP_Z);
    uint8_t surface_chassis_out = rd_u8(base + O_SURFACE_CHASSIS);
    int32_t center_pos_out = rd_i32(base + O_CENTER_POS);
    int32_t center_vel_out = rd_i32(base + O_CENTER_VEL);
    int32_t susp_pos_out[4];
    for (int w = 0; w < 4; w++) {
        susp_pos_out[w] = rd_i32(base + O_SUSP_POS + w * 4);
    }

    fprintf(s_fp,
        "%u,%d,%d,0x%lx,"
        "%s,%s,%u,"
        "%d,%u,"
        "%d,%d,%d,%d,"
        "%d,%d,%u,%u,"
        "%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%u,"
        "%d,%d,"
        "%d,%d,%d,%d\n",
        s_tick, s_paused, s_slot, (unsigned long)s_caller_ra,
        s_inputs.actor_addr, s_inputs.cardef_ptr, s_inputs.surface_chassis_in,
        s_inputs.track_span_raw, s_inputs.sub_lane_index,
        s_inputs.euler_yaw_in, s_inputs.lin_vx_in, s_inputs.lin_vz_in, s_inputs.ang_yaw_in,
        s_inputs.steer_cmd_in, s_inputs.encounter_steer_in, s_inputs.gear_in, s_inputs.brake_flag_in,
        s_inputs.center_pos_in, s_inputs.center_vel_in,
        s_inputs.susp_pos_in[0], s_inputs.susp_pos_in[1], s_inputs.susp_pos_in[2], s_inputs.susp_pos_in[3],
        s_inputs.inertia, s_inputs.half_wb, s_inputs.Wf, s_inputs.Wr, s_inputs.tire_grip, s_inputs.k_spring,
        s_inputs.damp_high, s_inputs.damp_low, s_inputs.brake_front, s_inputs.brake_rear, s_inputs.speed_limit,
        lin_vx_out, lin_vz_out, ang_yaw_out,
        long_speed_out, lat_speed_out,
        front_slip_excess_out, rear_slip_excess_out, (int)slip_metric_out,
        (int)tire_slip_x_out, (int)tire_slip_z_out, surface_chassis_out,
        center_pos_out, center_vel_out,
        susp_pos_out[0], susp_pos_out[1], susp_pos_out[2], susp_pos_out[3]);
    fflush(s_fp);
    s_active_call = 0;
}
