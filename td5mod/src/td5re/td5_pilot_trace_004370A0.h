/*
 * td5_pilot_trace_004370A0.h — port-side capture for the precise-port pilot
 * targeting 0x004370A0 AdvanceActorTrackScript.
 *
 * Captures the input/output state of the script VM each call. One row per
 * call. Keyed by (sim_tick, slot, call_idx). Caller is always
 * UpdateActorTrackBehavior @ 0x00435133 in original; port equivalent is
 * td5_ai_update_track_behavior.
 *
 * Build is gated on TD5_PILOT_TRACE_004370A0 so the production build has zero
 * overhead.
 */
#ifndef TD5_PILOT_TRACE_004370A0_H
#define TD5_PILOT_TRACE_004370A0_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Branch enum mirrors the listing structure. Used to bucket the exit row by
 * which path the function took. */
enum {
    TD5_PT_004370A0_BR_FLAG10        = 1,  /* flag-0x10 path returned early */
    TD5_PT_004370A0_BR_FLAG2_PASS    = 2,  /* flag-0x02 ran but fell through to switch */
    TD5_PT_004370A0_BR_SWITCH_CASE_0_BLOCK    = 10,
    TD5_PT_004370A0_BR_SWITCH_CASE_0_TERM     = 11,
    TD5_PT_004370A0_BR_SWITCH_CASE_1          = 21,
    TD5_PT_004370A0_BR_SWITCH_CASE_2          = 22,
    TD5_PT_004370A0_BR_SWITCH_CASE_3          = 23,
    TD5_PT_004370A0_BR_SWITCH_CASE_4          = 24,
    TD5_PT_004370A0_BR_SWITCH_CASE_5          = 25,
    TD5_PT_004370A0_BR_SWITCH_CASE_6          = 26,
    TD5_PT_004370A0_BR_SWITCH_CASE_7          = 27,
    TD5_PT_004370A0_BR_SWITCH_CASE_8          = 28,
    TD5_PT_004370A0_BR_SWITCH_CASE_9          = 29,
    TD5_PT_004370A0_BR_SWITCH_CASE_10         = 30,
    TD5_PT_004370A0_BR_SWITCH_CASE_11         = 31,
    TD5_PT_004370A0_BR_SWITCH_DEFAULT         = 99
};

/* Captured at function entry (after countdown decrement, before opcode dispatch). */
typedef struct TD5_PilotTrace_004370A0_Entry {
    /* Identity */
    uintptr_t rs_addr;
    int       slot_index;
    /* RS fields read */
    int32_t  route_table_ptr;
    int32_t  script_base_ptr;
    int32_t  script_ip;
    int32_t  script_ip_index;  /* port-side: 0/1/2 etc.; -1 if can't compute */
    int32_t  script_flags;
    int32_t  script_countdown;
    int32_t  script_offset_param;
    int32_t  script_speed_param;
    int32_t  script_field_3e;
    int32_t  script_field_43;
    /* Actor fields read */
    int32_t  actor_yaw_accum;
    int32_t  actor_steering_cmd;
    int32_t  actor_long_speed;
    int32_t  actor_span_norm;
    int32_t  actor_span_raw;
    int32_t  actor_sub_lane;
    int32_t  actor_encounter_steer;
    int32_t  actor_brake_flag;
    int32_t  actor_angular_velocity_yaw;
    /* Derived: route_byte and route_heading at entry (for input parity) */
    int32_t  route_byte_at_entry;
    int32_t  route_heading_at_entry;
} TD5_PilotTrace_004370A0_Entry;

/* Captured at function exit (just before return). */
typedef struct TD5_PilotTrace_004370A0_Exit {
    int32_t  opcode_dispatched;  /* -1 if flag-0x10 short-circuit before dispatch */
    int32_t  branch_taken;        /* enum above */
    int32_t  script_base_ptr_out;
    int32_t  script_ip_out;
    int32_t  script_ip_index_out;
    int32_t  script_flags_out;
    int32_t  script_countdown_out;
    int32_t  script_offset_param_out;
    int32_t  script_speed_param_out;
    int32_t  script_field_3e_out;
    int32_t  script_field_43_out;
    int32_t  rs_left_deviation_out;
    int32_t  rs_right_deviation_out;
    int32_t  actor_steering_cmd_out;
    int32_t  actor_encounter_steer_out;
    int32_t  actor_brake_flag_out;
    int32_t  actor_angular_velocity_yaw_out;
    int32_t  return_value;
} TD5_PilotTrace_004370A0_Exit;

void td5_pilot_trace_004370A0_enter(const TD5_PilotTrace_004370A0_Entry *e);
void td5_pilot_trace_004370A0_exit (const TD5_PilotTrace_004370A0_Exit  *x);

#ifdef __cplusplus
}
#endif

#endif /* TD5_PILOT_TRACE_004370A0_H */
