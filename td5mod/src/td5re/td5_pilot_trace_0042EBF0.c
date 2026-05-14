/*
 * td5_pilot_trace_0042EBF0.c -- Port-side CSV emitter for
 * ComputeVehicleSurfaceNormalAndGravity (0x0042EBF0).
 *
 * Mirrors tools/frida_pool7_0042EBF0.js column-for-column. One row per call;
 * keyed by (sim_tick, slot). Only slot 0 is emitted (TARGET_SLOT). The port
 * pairs an inputs() call at "after v1/v2 computed, before normalize" with an
 * outputs() call at "after writing linear_velocity_xz". The Frida script
 * captures the same pair (entry + exit) on the original side.
 */
#include "td5_pilot_trace_0042EBF0.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define OUT_PATH    "log/port/pool7_0042EBF0.csv"
#define TARGET_SLOT 0

/* Caller-PC distinction is helpful: 0x404030 (Player) vs 0x404EC0 (AI). The
 * port can't easily get the return address inside a regular C function, so
 * we use the actor's role bit instead: actor->is_ai (offset 0x9 per the
 * actor struct, set by AutoRace / PlayerIsAI). Frida side records the real RA. */
extern int td5_trace_current_sim_tick(void);

static FILE *s_fp = NULL;

/* Latched inputs from the inputs() call so outputs() can emit a single row. */
static struct {
    int                 active;
    int                 slot;
    uint32_t            tick;
    const TD5_Actor    *actor;
    /* Probes (24.8 fixed-point world coords) */
    int32_t             fl_x, fl_y, fl_z;
    int32_t             fr_x, fr_y, fr_z;
    int32_t             rl_x, rl_y, rl_z;
    int32_t             rr_x, rr_y, rr_z;
    /* Pre-normalize diagonals (post-Phase-1) */
    int32_t             v1_pre[3];
    int32_t             v2_pre[3];
    /* Pre-update linear velocity */
    int32_t             lin_vx_in;
    int32_t             lin_vz_in;
    /* Gravity */
    int32_t             g;
} s_snap;

/* Forward — g_gravity_constant is file-static in td5_physics.c, but exposed
 * to this module via a tiny accessor. We just read the int at the canonical
 * variable location through a function exported from physics. */
extern int32_t td5_physics_dbg_get_gravity_constant(void);

static void emit_header(FILE *fp) {
    fprintf(fp,
        "sim_tick,slot,actor_addr,is_ai,g_grav,"
        "fl_x,fl_y,fl_z,fr_x,fr_y,fr_z,rl_x,rl_y,rl_z,rr_x,rr_y,rr_z,"
        "lin_vx_in,lin_vz_in,"
        "v1_pre_x,v1_pre_y,v1_pre_z,v2_pre_x,v2_pre_y,v2_pre_z,"
        "v1_post_x,v1_post_y,v1_post_z,v2_post_x,v2_post_y,v2_post_z,"
        "cross_x,cross_z,"
        "lin_vx_out,lin_vz_out,dvel_x,dvel_z\n");
}

void td5_pilot_emit_0042EBF0_inputs(const TD5_Actor *actor,
                                     const int32_t v1[3],
                                     const int32_t v2[3]) {
    s_snap.active = 0;
    int slot = actor ? actor->slot_index : -1;
    if (slot != TARGET_SLOT) return;

    if (!s_fp) {
        s_fp = fopen(OUT_PATH, "w");
        if (!s_fp) return;
        emit_header(s_fp);
    }

    s_snap.active = 1;
    s_snap.slot   = slot;
    s_snap.tick   = (uint32_t)td5_trace_current_sim_tick();
    s_snap.actor  = actor;

    s_snap.fl_x = actor->probe_FL.x; s_snap.fl_y = actor->probe_FL.y; s_snap.fl_z = actor->probe_FL.z;
    s_snap.fr_x = actor->probe_FR.x; s_snap.fr_y = actor->probe_FR.y; s_snap.fr_z = actor->probe_FR.z;
    s_snap.rl_x = actor->probe_RL.x; s_snap.rl_y = actor->probe_RL.y; s_snap.rl_z = actor->probe_RL.z;
    s_snap.rr_x = actor->probe_RR.x; s_snap.rr_y = actor->probe_RR.y; s_snap.rr_z = actor->probe_RR.z;

    s_snap.v1_pre[0] = v1[0]; s_snap.v1_pre[1] = v1[1]; s_snap.v1_pre[2] = v1[2];
    s_snap.v2_pre[0] = v2[0]; s_snap.v2_pre[1] = v2[1]; s_snap.v2_pre[2] = v2[2];

    s_snap.lin_vx_in = actor->linear_velocity_x;
    s_snap.lin_vz_in = actor->linear_velocity_z;
    s_snap.g         = td5_physics_dbg_get_gravity_constant();
}

void td5_pilot_emit_0042EBF0_outputs(const TD5_Actor *actor,
                                      const int32_t v1[3],
                                      const int32_t v2[3],
                                      int32_t cross_x,
                                      int32_t cross_z) {
    if (!s_snap.active || !s_fp || actor != s_snap.actor) {
        s_snap.active = 0;
        return;
    }
    int32_t vx_out = actor->linear_velocity_x;
    int32_t vz_out = actor->linear_velocity_z;
    int is_ai = (int)((const uint8_t *)actor)[0x9];

    fprintf(s_fp,
        "%u,%d,0x%p,%d,%d,"
        "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
        "%d,%d,"
        "%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,"
        "%d,%d,"
        "%d,%d,%d,%d\n",
        s_snap.tick, s_snap.slot, (const void *)actor, is_ai, s_snap.g,
        s_snap.fl_x, s_snap.fl_y, s_snap.fl_z,
        s_snap.fr_x, s_snap.fr_y, s_snap.fr_z,
        s_snap.rl_x, s_snap.rl_y, s_snap.rl_z,
        s_snap.rr_x, s_snap.rr_y, s_snap.rr_z,
        s_snap.lin_vx_in, s_snap.lin_vz_in,
        s_snap.v1_pre[0], s_snap.v1_pre[1], s_snap.v1_pre[2],
        s_snap.v2_pre[0], s_snap.v2_pre[1], s_snap.v2_pre[2],
        v1[0], v1[1], v1[2],
        v2[0], v2[1], v2[2],
        cross_x, cross_z,
        vx_out, vz_out,
        vx_out - s_snap.lin_vx_in, vz_out - s_snap.lin_vz_in);
    fflush(s_fp);
    s_snap.active = 0;
}
