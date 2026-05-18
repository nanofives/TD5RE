/*
 * td5_pilot_trace_004370A0.c — port-side emitter for the AdvanceActorTrackScript
 * pilot.
 *
 * Writes one entry+exit row per call. Output CSV at log/port/pool11_004370A0.csv.
 * Mirrors tools/frida_pool11_004370A0.js.
 */
#include "td5_pilot_trace_004370A0.h"

#include <stdio.h>
#include <stdlib.h>

#define OUT_PATH "log/port/pool11_004370A0.csv"

extern int td5_trace_current_sim_tick(void);

static FILE *s_fp = NULL;
static unsigned long s_call_idx = 0;
/* Buffer entry data between enter/exit so we emit one row per call (joined). */
static TD5_PilotTrace_004370A0_Entry s_pending;
static int s_have_pending = 0;

static void emit_header(FILE *fp) {
    /* Two-row split would double the row count; instead emit a single combined
     * row with `_in` and `_out` suffixes for paired fields. */
    fprintf(fp,
        "sim_tick,call_idx,slot,"
        "rs_addr,"
        "route_table_ptr,"
        "script_base_ptr_in,script_ip_in,script_ip_index_in,"
        "script_flags_in,script_countdown_in,"
        "script_offset_param_in,script_speed_param_in,"
        "script_field_3e_in,script_field_43_in,"
        "actor_yaw_accum_in,actor_steering_cmd_in,actor_long_speed_in,"
        "actor_span_norm,actor_span_raw,actor_sub_lane,"
        "actor_encounter_steer_in,actor_brake_flag_in,"
        "actor_ang_vel_yaw_in,"
        "route_byte,route_heading,"
        "opcode_dispatched,branch_taken,"
        "script_base_ptr_out,script_ip_out,script_ip_index_out,"
        "script_flags_out,script_countdown_out,"
        "script_offset_param_out,script_speed_param_out,"
        "script_field_3e_out,script_field_43_out,"
        "rs_left_deviation_out,rs_right_deviation_out,"
        "actor_steering_cmd_out,actor_encounter_steer_out,actor_brake_flag_out,"
        "actor_ang_vel_yaw_out,"
        "return_value"
        "\n");
}

void td5_pilot_trace_004370A0_enter(const TD5_PilotTrace_004370A0_Entry *e) {
    if (!e) return;
    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }
    s_pending = *e;
    s_have_pending = 1;
}

void td5_pilot_trace_004370A0_exit(const TD5_PilotTrace_004370A0_Exit *x) {
    if (!s_fp || !s_have_pending || !x) {
        s_have_pending = 0;
        return;
    }
    const TD5_PilotTrace_004370A0_Entry *e = &s_pending;
    fprintf(s_fp,
        "%d,%lu,%d,"
        "0x%lx,"
        "0x%lx,0x%lx,0x%lx,%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,"
        "%d,"
        "%d,%d,"
        "%d,%d,"
        "0x%lx,0x%lx,%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,"
        "%d,%d,%d,"
        "%d,"
        "%d"
        "\n",
        td5_trace_current_sim_tick(),
        s_call_idx++,
        e->slot_index,
        (unsigned long)e->rs_addr,
        (unsigned long)(uint32_t)e->route_table_ptr,
        (unsigned long)(uint32_t)e->script_base_ptr,
        (unsigned long)(uint32_t)e->script_ip,
        e->script_ip_index,
        e->script_flags, e->script_countdown,
        e->script_offset_param, e->script_speed_param,
        e->script_field_3e, e->script_field_43,
        e->actor_yaw_accum, e->actor_steering_cmd, e->actor_long_speed,
        e->actor_span_norm, e->actor_span_raw, e->actor_sub_lane,
        e->actor_encounter_steer, e->actor_brake_flag,
        e->actor_angular_velocity_yaw,
        e->route_byte_at_entry, e->route_heading_at_entry,
        x->opcode_dispatched, x->branch_taken,
        (unsigned long)(uint32_t)x->script_base_ptr_out,
        (unsigned long)(uint32_t)x->script_ip_out,
        x->script_ip_index_out,
        x->script_flags_out, x->script_countdown_out,
        x->script_offset_param_out, x->script_speed_param_out,
        x->script_field_3e_out, x->script_field_43_out,
        x->rs_left_deviation_out, x->rs_right_deviation_out,
        x->actor_steering_cmd_out, x->actor_encounter_steer_out,
        x->actor_brake_flag_out,
        x->actor_angular_velocity_yaw_out,
        x->return_value
    );
    if ((s_call_idx & 0x7F) == 0) fflush(s_fp);
    s_have_pending = 0;
}
