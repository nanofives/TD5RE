/*
 * td5_pilot_trace_v2v_contact.c -- Port-side V2V contact + TOI trace emitter.
 * Mirrors tools/frida_pool15_v2v_contact.js exactly.
 *
 * Output: log/port/pool15_v2v_contact.csv (combined contact + toi rows
 *         distinguished by which_fn).
 *
 * Pairing key: (sim_tick, which_fn, slot_a, slot_b, event_idx).
 *  - For which_fn="contact", event_idx = call sequence within one
 *    collision_detect_full invocation (1 = seed test, 2..8 = bisection iters).
 *  - For which_fn="toi", event_idx is always 0.
 */
#include "td5_pilot_trace_v2v_contact.h"
#include "td5_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_PATH "log/port/pool15_v2v_contact.csv"

static FILE *s_fp = NULL;
static int   s_call_idx = 0;
static int   s_pending_slot_a = -1;
static int   s_pending_slot_b = -1;
static int   s_pending_call_idx = 0;
static TD5_PilotV2VContactSnap s_pending;

static void open_and_header(void) {
    if (s_fp) return;
    s_fp = fopen(OUT_PATH, "w");
    if (!s_fp) return;
    fprintf(s_fp,
        "sim_tick,which_fn,slot_a,slot_b,event_idx,"
        "actor_a_addr,actor_b_addr,"
        "ax,ay,az,bx,by,bz,"
        "yaw_a_raw,yaw_b_raw,"
        "cardef_a_off04,cardef_a_off08,cardef_a_off14,"
        "cardef_b_off04,cardef_b_off08,cardef_b_off14,"
        "bitmask,"
        /* contact: 8 corners × 4 fields = 32 columns */
        "c0_px,c0_pz,c0_ox,c0_oz,"
        "c1_px,c1_pz,c1_ox,c1_oz,"
        "c2_px,c2_pz,c2_ox,c2_oz,"
        "c3_px,c3_pz,c3_ox,c3_oz,"
        "c4_px,c4_pz,c4_ox,c4_oz,"
        "c5_px,c5_pz,c5_ox,c5_oz,"
        "c6_px,c6_pz,c6_ox,c6_oz,"
        "c7_px,c7_pz,c7_ox,c7_oz,"
        /* toi-only fields (zero for contact rows) */
        "lin_vel_a_x,lin_vel_a_z,lin_vel_b_x,lin_vel_b_z,"
        "ang_vel_a_yaw,ang_vel_b_yaw,"
        "impactForce,dispatched_bitmask,"
        "final_yaw_a_disp,final_yaw_b_disp,"
        "final_ax,final_az,final_bx,final_bz"
        "\n");
    fflush(s_fp);
}

void td5_pilot_v2v_reset_call_idx(int slot_a, int slot_b) {
    (void)slot_a; (void)slot_b;
    s_call_idx = 0;
}

int td5_pilot_v2v_next_call_idx(void) {
    s_call_idx += 1;
    return s_call_idx;
}

void td5_pilot_v2v_contact_emit_enter(const TD5_PilotV2VContactSnap *inputs,
                                      int slot_a, int slot_b,
                                      int call_idx)
{
    open_and_header();
    if (!s_fp) return;
    s_pending = *inputs;
    s_pending_slot_a = slot_a;
    s_pending_slot_b = slot_b;
    s_pending_call_idx = call_idx;
}

void td5_pilot_v2v_contact_emit_leave(const TD5_PilotV2VContactSnap *outputs) {
    open_and_header();
    if (!s_fp) return;
    if (s_pending_slot_a < 0) return;

    /* Merge outputs into s_pending — outputs has bitmask + corner arrays. */
    s_pending.bitmask = outputs->bitmask;
    memcpy(s_pending.corner_proj_x, outputs->corner_proj_x, sizeof(s_pending.corner_proj_x));
    memcpy(s_pending.corner_proj_z, outputs->corner_proj_z, sizeof(s_pending.corner_proj_z));
    memcpy(s_pending.corner_own_x,  outputs->corner_own_x,  sizeof(s_pending.corner_own_x));
    memcpy(s_pending.corner_own_z,  outputs->corner_own_z,  sizeof(s_pending.corner_own_z));

    uint32_t tick = (uint32_t)td5_trace_current_sim_tick();
    fprintf(s_fp,
        "%u,contact,%d,%d,%d,"
        "0x%x,0x%x,"
        "%d,%d,%d,%d,%d,%d,"
        "%d,%d,"
        "%d,%d,%d,%d,%d,%d,"
        "0x%x,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,"
        "%d,%d,%d,%d,"
        /* toi-only zeros */
        "0,0,0,0,"
        "0,0,"
        "0,0,"
        "0,0,"
        "0,0,0,0"
        "\n",
        tick, s_pending_slot_a, s_pending_slot_b, s_pending_call_idx,
        s_pending.actor_a_addr, s_pending.actor_b_addr,
        s_pending.ax, s_pending.ay, s_pending.az,
        s_pending.bx, s_pending.by, s_pending.bz,
        s_pending.yaw_a_raw, s_pending.yaw_b_raw,
        s_pending.cardef_a_off04, s_pending.cardef_a_off08, s_pending.cardef_a_off14,
        s_pending.cardef_b_off04, s_pending.cardef_b_off08, s_pending.cardef_b_off14,
        s_pending.bitmask,
        s_pending.corner_proj_x[0], s_pending.corner_proj_z[0], s_pending.corner_own_x[0], s_pending.corner_own_z[0],
        s_pending.corner_proj_x[1], s_pending.corner_proj_z[1], s_pending.corner_own_x[1], s_pending.corner_own_z[1],
        s_pending.corner_proj_x[2], s_pending.corner_proj_z[2], s_pending.corner_own_x[2], s_pending.corner_own_z[2],
        s_pending.corner_proj_x[3], s_pending.corner_proj_z[3], s_pending.corner_own_x[3], s_pending.corner_own_z[3],
        s_pending.corner_proj_x[4], s_pending.corner_proj_z[4], s_pending.corner_own_x[4], s_pending.corner_own_z[4],
        s_pending.corner_proj_x[5], s_pending.corner_proj_z[5], s_pending.corner_own_x[5], s_pending.corner_own_z[5],
        s_pending.corner_proj_x[6], s_pending.corner_proj_z[6], s_pending.corner_own_x[6], s_pending.corner_own_z[6],
        s_pending.corner_proj_x[7], s_pending.corner_proj_z[7], s_pending.corner_own_x[7], s_pending.corner_own_z[7]);
    fflush(s_fp);

    s_pending_slot_a = -1;
    s_pending_slot_b = -1;
}

void td5_pilot_v2v_toi_emit(const TD5_PilotV2VToiSnap *snap,
                            int slot_a, int slot_b)
{
    open_and_header();
    if (!s_fp) return;

    uint32_t tick = (uint32_t)td5_trace_current_sim_tick();
    fprintf(s_fp,
        "%u,toi,%d,%d,0,"
        "0x%x,0x%x,"
        "%d,0,%d,%d,0,%d,"   /* ax,0,az,bx,0,bz — y always 0 for toi */
        "%d,%d,"
        "0,0,0,0,0,0,"        /* cardef cols zero for toi */
        "0,"                  /* bitmask for contact field */
        /* 8 corner records all zero */
        "0,0,0,0,"
        "0,0,0,0,"
        "0,0,0,0,"
        "0,0,0,0,"
        "0,0,0,0,"
        "0,0,0,0,"
        "0,0,0,0,"
        "0,0,0,0,"
        /* toi outputs */
        "%d,%d,%d,%d,"
        "%d,%d,"
        "%d,0x%x,"
        "%d,%d,"
        "%d,%d,%d,%d"
        "\n",
        tick, slot_a, slot_b,
        snap->actor_a_addr, snap->actor_b_addr,
        snap->ax, snap->az, snap->bx, snap->bz,
        snap->yaw_a_raw, snap->yaw_b_raw,
        snap->lin_vel_a_x, snap->lin_vel_a_z, snap->lin_vel_b_x, snap->lin_vel_b_z,
        snap->ang_vel_a_yaw, snap->ang_vel_b_yaw,
        snap->impactForce, snap->dispatched_bitmask,
        snap->final_yaw_a_disp, snap->final_yaw_b_disp,
        snap->final_ax, snap->final_az, snap->final_bx, snap->final_bz);
    fflush(s_fp);
}
