/*
 * td5_pilot_trace_v2v_contact.h -- Port-side trace emitter for the V2V
 * contact-collection + collision-pair pilot (precise-port pool15).
 *
 * Mirrors tools/frida_pool15_v2v_contact.js column-for-column so the
 * diff tool can join port + original rows by (sim_tick, which_fn,
 * event_idx, slot_a, slot_b).
 *
 * Two row schemas, distinguished by `which_fn`:
 *   which_fn = "contact" — emitted on each obb_corner_test entry/leave.
 *                           Captures 8 corner records + bitmask.
 *   which_fn = "toi"     — emitted on each collision_detect_full call.
 *                           Captures the pre-loop seed, 7 bisection steps,
 *                           and final impactForce + dispatched bitmask.
 *
 * event_idx counts within a single (sim_tick, slot_a, slot_b) tuple so
 * the 8 contact calls (1 seed + 7 bisection) from one toi call are
 * uniquely keyed.
 *
 * Hook points (port side):
 *   - obb_corner_test entry → set inputs, call_idx
 *   - obb_corner_test exit  → emit row (inputs + outputs)
 *   - collision_detect_full entry → reset event_idx counter, capture seed
 *   - collision_detect_full pre-dispatch → emit toi row
 */
#ifndef TD5_PILOT_TRACE_V2V_CONTACT_H
#define TD5_PILOT_TRACE_V2V_CONTACT_H

#include <stdint.h>

/* Per-call snapshot for obb_corner_test.
 * Inputs captured at entry; outputs captured at leave. */
typedef struct {
    /* Inputs */
    uint32_t actor_a_addr;
    uint32_t actor_b_addr;
    int32_t  ax, ay, az;      /* 24.8 FP world coords as passed (a's world_pos) */
    int32_t  bx, by, bz;      /* 24.8 FP world coords as passed (b's world_pos) */
    int32_t  yaw_a_raw;       /* raw euler_accum.yaw (24.8 FP) as passed */
    int32_t  yaw_b_raw;
    int16_t  cardef_a_off04;  /* front_z_a */
    int16_t  cardef_a_off08;  /* half_w_a */
    int16_t  cardef_a_off14;  /* rear_z_a (negative) */
    int16_t  cardef_b_off04;
    int16_t  cardef_b_off08;
    int16_t  cardef_b_off14;
    /* Outputs */
    uint32_t bitmask;
    int16_t  corner_proj_x[8];
    int16_t  corner_proj_z[8];
    int16_t  corner_own_x[8];
    int16_t  corner_own_z[8];
} TD5_PilotV2VContactSnap;

/* Per-pair snapshot for collision_detect_full. */
typedef struct {
    /* Inputs at entry (post broadphase pass) */
    uint32_t actor_a_addr;
    uint32_t actor_b_addr;
    int32_t  ax, az;            /* 24.8 FP world coords */
    int32_t  bx, bz;
    int32_t  yaw_a_raw;
    int32_t  yaw_b_raw;
    int32_t  lin_vel_a_x, lin_vel_a_z;   /* +0x1cc, +0x1d4 */
    int32_t  lin_vel_b_x, lin_vel_b_z;
    int32_t  ang_vel_a_yaw, ang_vel_b_yaw; /* +0x1c4 */
    /* Outputs at pre-dispatch */
    int32_t  impactForce;      /* local_84 - 0x10 */
    uint32_t dispatched_bitmask;
    int32_t  final_yaw_a_disp; /* test_ha — bisected A yaw in 12-bit display units */
    int32_t  final_yaw_b_disp; /* test_hb */
    int32_t  final_ax, final_az; /* bisected positions in display units */
    int32_t  final_bx, final_bz;
} TD5_PilotV2VToiSnap;

void td5_pilot_v2v_contact_emit_enter(const TD5_PilotV2VContactSnap *inputs,
                                      int slot_a, int slot_b,
                                      int call_idx);
void td5_pilot_v2v_contact_emit_leave(const TD5_PilotV2VContactSnap *outputs);

/* call_idx is incremented per obb_corner_test call within one
 * collision_detect_full invocation. Called from collision_detect_full
 * entry to reset the counter and at exit to flush. */
void td5_pilot_v2v_toi_emit(const TD5_PilotV2VToiSnap *snap,
                            int slot_a, int slot_b);

/* Reset call_idx counter at start of each collision_detect_full. */
void td5_pilot_v2v_reset_call_idx(int slot_a, int slot_b);
int  td5_pilot_v2v_next_call_idx(void);

#endif /* TD5_PILOT_TRACE_V2V_CONTACT_H */
