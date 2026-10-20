/**
 * td5_roam_rebase.c -- GEO FREE ROAM: dynamic world-origin rebasing (PORT-ONLY, F9b).
 * Contract and the list of what moves: td5_roam_rebase.h.
 *
 * NO FILE-SCOPE STATE outside s_rb (listeners + counters).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_fp.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_race_state.h"     /* td5_game_get_actor / _total_actor_count */
#include "td5_physics.h"
#include "td5_camera.h"
#include "td5_vfx.h"
#include "td5_sound.h"
#include "td5_light.h"
#include "td5_ai.h"
#include "td5_replay.h"
#include "td5_roam_tile.h"
#include "td5_roam_mode.h"
#include "td5_roam_physics.h"
#include "td5_roam_render.h"
#include "td5_roam_rebase.h"
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RB_MAX_LISTEN   16
#define RB_UPM          430.0
#define RB_HALF_RANGE   4194304      /* 2^22 world units: a + b of two such positions still fits 24.8 */

typedef struct {
    TD5_RoamRebaseFn    fn[RB_MAX_LISTEN];
    int                 nfn;
    int                 tiles;           /* threshold, -1 = not read */
    int                 force_tiles;     /* DEV/selftest override + 1, 0 = the knob */
    RoamGrid            grid;
    int                 grid_ok;
    char                grid_place[64];
    int                 force;
    int                 pending_frame;   /* a rebase ran since the last frame note */
#ifndef TD5RE_RELEASE
    int                 trace_on;        /* TD5RE_ROAM_REBASE_TRACE=1: -1 unread */
    FILE               *trace;           /* <log>/roam_rebase_trace.csv: slot 0 per sim tick, PLACE frame */
    long                trace_tick;
#endif
    TD5_RoamRebaseStats st;
} RoamRebase;

static RoamRebase s_rb = { .tiles = -1
#ifndef TD5RE_RELEASE
    , .trace_on = -1
#endif
};

#ifndef TD5RE_RELEASE
/* DEV: one row per sim tick, after any rebase of that tick: slot 0 in the PLACE frame (exact: world/256 +
 * origin) and the raw world value, so two runs (rebase on / off) compare tick by tick and a jump at a
 * rebase shows as a place-frame step. */
static void rb_trace(int rebased)
{
    TD5_Actor *a;
    int32_t ox, oz;
    const long tick = (long)g_td5.simulation_tick_counter;
    if (s_rb.trace_on < 0) s_rb.trace_on = td5_env_int("TD5RE_ROAM_REBASE_TRACE", 0, 0, 1);
    if (!s_rb.trace_on || tick == s_rb.trace_tick) return;
    a = td5_game_get_actor(0);
    if (!a) return;
    if (!s_rb.trace) {
        char path[512];
        const char *dir = td5_plat_log_dir();
        snprintf(path, sizeof path, "%sroam_rebase_trace.csv", dir ? dir : "");
        s_rb.trace = fopen(path, "w");
        if (!s_rb.trace) { s_rb.trace_on = 0; return; }
        fprintf(s_rb.trace, "tick,origin_x,origin_z,world_x_fp,world_y_fp,world_z_fp,place_x,place_z,vx,vz,yaw,rebased\n");
    }
    s_rb.trace_tick = tick;
    td5_roam_world_origin(&ox, &oz);
    fprintf(s_rb.trace, "%ld,%d,%d,%d,%d,%d,%.4f,%.4f,%d,%d,%d,%d\n", tick, (int)ox, (int)oz, (int)a->world_pos.x,
            (int)a->world_pos.y, (int)a->world_pos.z, (double)a->world_pos.x / 256.0 + ox, (double)a->world_pos.z / 256.0 + oz,
            (int)a->linear_velocity_x, (int)a->linear_velocity_z, (int)(a->euler_accum.yaw >> 8 & 0xFFF), rebased);
    if ((tick % 300) == 0) fflush(s_rb.trace);
}
#endif

void td5_roam_rebase_listen(TD5_RoamRebaseFn fn)
{
    int i;
    if (!fn) return;
    for (i = 0; i < s_rb.nfn; i++) if (s_rb.fn[i] == fn) return;
    if (s_rb.nfn < RB_MAX_LISTEN) s_rb.fn[s_rb.nfn++] = fn;
}

void td5_roam_rebase_session_reset(void)
{
    const int keep_tiles = s_rb.tiles;
    memset(&s_rb.st, 0, sizeof s_rb.st);
    s_rb.st.threshold_tiles = keep_tiles;
    s_rb.force = 0;
    s_rb.pending_frame = 0;
#ifndef TD5RE_RELEASE
    if (s_rb.trace) { fclose(s_rb.trace); s_rb.trace = NULL; }
    s_rb.trace_tick = -1;
#endif
}

void td5_roam_rebase_force(void) { s_rb.force = 1; }
void td5_roam_rebase_force_tiles(int tiles) { s_rb.force_tiles = tiles < 0 ? 0 : tiles + 1; }

void td5_roam_rebase_stats(TD5_RoamRebaseStats *out)
{
    if (!out) return;
    *out = s_rb.st;
    out->threshold_tiles = s_rb.tiles;
}

void td5_roam_rebase_note_frame(double dt_ms)
{
    if (!s_rb.pending_frame) return;
    s_rb.pending_frame = 0;
    if (dt_ms > s_rb.st.frame_worst_ms) s_rb.st.frame_worst_ms = dt_ms;
    TD5_LOG_I(LOG_TAG, "ROAM_REBASE frame: %.1f ms (the frame that held rebase #%ld)", dt_ms, s_rb.st.count);
}

static const RoamGrid *rb_grid(void)
{
    const TD5_RoamSpawn *sp = td5_roam_spawn();
    if (!sp) return NULL;
    if (!s_rb.grid_ok || strcmp(s_rb.grid_place, sp->place) != 0) {
        {
            /* the lattice of the session's place: the render session holds it (ROAMGRID.JSON) */
            const RoamGrid *g = td5_roam_render_grid();
            if (!g) return NULL;
            s_rb.grid = *g;
        }
        snprintf(s_rb.grid_place, sizeof s_rb.grid_place, "%s", sp->place);
        s_rb.grid_ok = 1;
    }
    return &s_rb.grid;
}

/* ---- the actor shift ------------------------------------------------------------ */

static int32_t rb_add_fp(int32_t v, int32_t d)
{
    const int64_t r = (int64_t)v + (int64_t)d;
    if (r > INT32_MAX) return INT32_MAX;
    if (r < INT32_MIN) return INT32_MIN;
    return (int32_t)r;
}

static void rb_vec_fp(TD5_Vec3_Fixed *v, int32_t fx, int32_t fz) { v->x = rb_add_fp(v->x, fx); v->z = rb_add_fp(v->z, fz); }

/* wheel_world_positions_hires has two writers in two units: the wheel-contact refresh stores whole
 * world units (td5_physics_suspension.c), the vehicle-model pass stores 24.8 (td5_physics.c). Tell
 * them apart by which one sits next to the car. */
static void rb_hires(TD5_Actor *a, int32_t sx, int32_t sz)
{
    int i;
    for (i = 0; i < 4; i++) {
        TD5_Vec3_Fixed *w = &a->wheel_world_positions_hires[i];
        const int64_t dwu = llabs((int64_t)w->x - (int64_t)FP_TRUNC(a->world_pos.x)) + llabs((int64_t)w->z - (int64_t)FP_TRUNC(a->world_pos.z));
        const int64_t dfp = llabs((int64_t)w->x - (int64_t)a->world_pos.x) + llabs((int64_t)w->z - (int64_t)a->world_pos.z);
        if (dwu <= dfp) { w->x = rb_add_fp(w->x, sx); w->z = rb_add_fp(w->z, sz); }
        else            { w->x = rb_add_fp(w->x, FP_SCALE(sx)); w->z = rb_add_fp(w->z, FP_SCALE(sz)); }
    }
}

static void rb_actor(TD5_Actor *a, int32_t sx, int32_t sz)
{
    const int32_t fx = FP_SCALE(sx), fz = FP_SCALE(sz);
    int i;
    rb_hires(a, sx, sz);                  /* before world_pos moves: it is judged against it */
    rb_vec_fp(&a->world_pos, fx, fz);
    rb_vec_fp(&a->probe_FL, fx, fz);
    rb_vec_fp(&a->probe_FR, fx, fz);
    rb_vec_fp(&a->probe_RL, fx, fz);
    rb_vec_fp(&a->probe_RR, fx, fz);
    for (i = 0; i < 4; i++) {
        rb_vec_fp(&a->bbox_vertices_upper[i], fx, fz);
        rb_vec_fp(&a->wheel_contact_pos[i], fx, fz);
    }
    a->render_pos.x += (float)sx;
    a->render_pos.z += (float)sz;
    a->saved_render_pos.x += (float)sx;
    a->saved_render_pos.z += (float)sz;
    if (llabs((int64_t)FP_TRUNC(a->world_pos.x)) > RB_HALF_RANGE || llabs((int64_t)FP_TRUNC(a->world_pos.z)) > RB_HALF_RANGE)
        s_rb.st.far_actors++;
}

/* ---- the rebase ------------------------------------------------------------------ */

static void rb_apply(int32_t new_ox, int32_t new_oz, int ptx, int ptz)
{
    int32_t ox, oz, sx, sz;
    int i, n;
    const uint64_t t0 = td5_plat_time_us();
    double ms;
    td5_roam_world_origin(&ox, &oz);
    sx = ox - new_ox;
    sz = oz - new_oz;
    if (!sx && !sz) return;
    /* 1. the authority */
    td5_roam_set_world_origin(new_ox, new_oz);
    /* 2. the roam contact provider (resident set origin + slot memories) */
    td5_roam_physics_world_shift(sx, sz, new_ox, new_oz);
    /* 3. every actor slot */
    n = td5_game_get_total_actor_count();
    for (i = 0; i < n; i++) {
        TD5_Actor *a = td5_game_get_actor(i);
        if (a) rb_actor(a, sx, sz);
    }
    /* 4. the engine's own world-frame memories */
    td5_physics_world_shift(sx, sz);
    td5_camera_world_shift(sx, sz);
    td5_vfx_world_shift(sx, sz);
    td5_sound_world_shift(sx, sz);
    td5_light_world_shift(sx, sz);
    td5_ai_world_shift(sx, sz);
    td5_replay_world_shift(sx, sz);
    /* 5. the roam render (resident tiles now, later tiles at install) */
    td5_roam_render_world_shift(sx, sz);
    /* 6. listeners */
    for (i = 0; i < s_rb.nfn; i++) s_rb.fn[i](sx, sz);

    ms = (double)(td5_plat_time_us() - t0) / 1000.0;
    s_rb.st.count++;
    s_rb.st.last_tick = (long)g_td5.simulation_tick_counter;
    s_rb.st.total_x += -sx;
    s_rb.st.total_z += -sz;
    if (ms > s_rb.st.op_worst_ms) s_rb.st.op_worst_ms = ms;
    s_rb.pending_frame = 1;
    TD5_LOG_I(LOG_TAG, "ROAM_REBASE #%ld tick=%ld origin (%d,%d) -> (%d,%d) = tile %d,%d, shift world (%+d,%+d) units "
              "(%+.0f,%+.0f m), %d actor(s), %.3f ms", s_rb.st.count, s_rb.st.last_tick, (int)ox, (int)oz, (int)new_ox,
              (int)new_oz, ptx, ptz, (int)sx, (int)sz, sx / RB_UPM, sz / RB_UPM, n, ms);
}

#ifndef TD5RE_RELEASE
#define RB_TRACE(r) rb_trace(r)
#else
#define RB_TRACE(r) ((void)0)
#endif

void td5_roam_rebase_tick(void)
{
    const RoamGrid *g;
    TD5_Actor *a;
    int32_t ox, oz, nox, noz;
    int otx, otz, ptx, ptz, d;
    double px, pz;
    if (!td5_roam_active()) return;
    if (s_rb.tiles < 0) {
        s_rb.tiles = td5_env_int("TD5RE_ROAM_REBASE_TILES", 4, 0, 64);
        TD5_LOG_I(LOG_TAG, "ROAM_REBASE: threshold %d tile(s) (%s)", s_rb.tiles, s_rb.tiles ? "on" : "off: fixed origin");
    }
    {
        const int thr = s_rb.force_tiles ? s_rb.force_tiles - 1 : s_rb.tiles;
        if ((!thr && !s_rb.force) || td5_roam_physics_rebase_inhibited()) { RB_TRACE(0); return; }
    }
    a = td5_game_get_actor(0);
    g = rb_grid();
    if (!a || !g) { RB_TRACE(0); return; }
    td5_roam_world_origin(&ox, &oz);
    px = (double)FP_TRUNC(a->world_pos.x) + (double)ox;
    pz = (double)FP_TRUNC(a->world_pos.z) + (double)oz;
    {
        const double wm = fabs((double)FP_TRUNC(a->world_pos.x)) > fabs((double)FP_TRUNC(a->world_pos.z))
                              ? fabs((double)FP_TRUNC(a->world_pos.x)) : fabs((double)FP_TRUNC(a->world_pos.z));
        if (wm / RB_UPM > s_rb.st.max_world_m) s_rb.st.max_world_m = wm / RB_UPM;
    }
    roam_grid_tile_of(g, px, pz, &ptx, &ptz);
    /* the origin is a lattice corner: its own tile is the one whose (0,0) corner it is. [roam M] The world
     * origin is the SOURCE-frame (0,0) corner, which the reflected lattice makes the (0,0) corner of tile
     * tx + 1 (td5_roam_mode.c): its own tile is the one to the -u side */
    roam_grid_tile_of(g, (double)ox + 8.0 * (g->c - g->s), (double)oz + 8.0 * (g->s + g->c), &otx, &otz);
    otx -= 1;
    d = abs(ptx - otx) > abs(ptz - otz) ? abs(ptx - otx) : abs(ptz - otz);
    if (!s_rb.force && d <= (s_rb.force_tiles ? s_rb.force_tiles - 1 : s_rb.tiles)) { RB_TRACE(0); return; }
    s_rb.force = 0;
    roam_grid_origin(g, ptx + 1, ptz, &nox, &noz);
    rb_apply(nox, noz, ptx, ptz);
    RB_TRACE(1);
}
