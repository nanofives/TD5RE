/**
 * td5_roam_cops.c -- GEO FREE ROAM: police on the street graph (PORT-ONLY, F10).
 * Contract and design: td5_roam_cops.h. The DEV self-test driver is td5_roam_cops_test.c.
 *
 * Sections: knobs + helpers / pool + assets / path (A* + the Driver Model's profile) / spawn + despawn / the driver
 * / the player's offences / pursuit, escape, bust / tick / answers + HUD / stats + DEV.
 *
 * FRAMES. The graph and the paths are in the PLACE frame (430 units per metre, doubles); an actor's place position is
 * world / 256 + the world origin. Speeds are m/s inside this file; the actor's linear velocity is 24.8 world units
 * per sim tick (30 Hz).
 *
 * DETERMINISM. Every random draw comes from the session LCG (or a cop's own, seeded from slot + spawn count); no wall
 * clock, no pointer order.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_fp.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_race_state.h"
#include "td5_asset.h"
#include "td5_ai.h"             /* td5_ai_roam_mark_cop, the wreck flag */
#include "td5_physics.h"
#include "td5_render.h"         /* td5_render_set_cop_mesh, g_render_width_f */
#include "td5_hud.h"            /* td5_hud_prim_tris */
#include "td5_vectorui.h"
#include "td5_geo_signals.h"
#include "td5_roam_mode.h"
#include "td5_roam_locate.h"
#include "td5_roam_physics.h"
#include "td5_roam_route.h"
#include "td5_roam_route_ui.h"
#include "td5_roam_race.h"
#include "td5_roam_traffic.h"   /* td5_roam_traffic_pool (the slot layout), td5_roam_signal_group */
#include "td5_roam_ai.h"        /* td5_roam_ai_count: the racer field when the session re-sets it */
#include "td5_roam_stream.h"    /* td5_roam_stream_point_missing: a leg into a tile not streamed in yet */
#include "td5_roam_cops.h"
#include "td5_roam_rebase.h"    /* [F9b] parked cops are held where they were parked */
#include "td5_police.h"         /* [round H HH] the shared pursuit HUD drawing */
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RC_M           430.0                     /* place units per metre               */
#define RC_HZ          30.0
#define RC_PI          3.14159265358979323846
#define RC_DS          2.0                       /* path resample step, metres          */
#define RC_PTS         1400                      /* path points (2.8 km)                */
#define RC_STEER_CLAMP 0x18000
#define RC_FADE_TICKS  20
#define RC_GLOW        0x3000                    /* = td5_ai_cop_glow_intensity's default */
#define RC_STOP_BACK_M 2.8
#define RC_OFF_TICKS   30                        /* 1 s off the surface                 */
#define RC_STUCK_TICKS 150                       /* 5 s still (a stuck episode)         */
#define RC_REPLAN      20                        /* pursuit re-plan every 0.67 s        */
#define RC_LOS_EVERY   6

enum { RC_PARKED = 0, RC_PATROL = 1, RC_PURSUIT = 2, RC_BLOCK = 3 };   /* RC_BLOCK: [G1b] a roadblock car, held */
#define RC_BLK_MAX  2
#define RC_BLK_CARS 3
#define RC_BLK_CAR_W 4.4                         /* lateral cover of one car parked across the road, metres */
enum { RC_WHY_FAR = 1, RC_WHY_OFF, RC_WHY_STUCK, RC_WHY_UNRES, RC_WHY_LEVEL, RC_WHY_WRECK, RC_WHY_BUST, RC_WHY_NOPATH, RC_WHY_BLOCK };

typedef struct {
    int      slot, state;
    int      alpha, fade, why_out;
    int      hold;                               /* DEV: parked at its spot until a pursuit */
    int      role;                               /* pursuit: 0 behind, 1 ahead, 2 beside      */
    uint32_t rng;
    /* path */
    int      np, pi;                             /* points, projection hint                    */
    double   px[RC_PTS], pz[RC_PTS], pv[RC_PTS];
    double   s, lat, off;                        /* metres along, metres off the path (+ right), lane offset */
    double   pass_off; int pass_dwell;
    long     plan_t;                             /* tick of the last plan                      */
    int      plan_fail;
    /* control */
    int32_t  steer;
    double   v_t;
    int      rec, rec_t, rec_n; double rec_x, rec_z;
    long     still_t, off_t, born, last_contact, wreck_t, wall_t;
    int      stuck_counted;
    int      sees; long los_t;                   /* line of sight to the player (cached)       */
    double   d_player;
    int      sig_hold;                           /* patrol: holding at a red stop line         */
    int      turning;                            /* pursuit: the last leg leaves backwards (turn round) */
    int      blk;                                /* [G1b] roadblock index + 1 (RC_BLOCK cars)            */
} RcCop;

/* [G1b] a roadblock: PLACE frame only (no graph index, so a rebase and a graph swap need nothing) */
typedef struct {
    int    on, ncar, heat;
    int    slot[RC_BLK_CARS];
    double cx, cz, tx, tz;                       /* centre, the player's travel tangent there            */
    double gx, gz, gap_w;                        /* the gap's centre (gap_w 0 = closed)                  */
    long   t0;
    double min_d, prev_f;
    int    have_prev, crossed, contact, was_near;
} RcBlock;

static struct {
    int     on, pool, base, loaded, model;
    int     ready;
    RcCop   cop[TD5_ROAM_COPS_POOL_MAX];
    uint32_t rng;
    long    spawn_count, tick_last;
    /* knobs */
    int     knobs;
    int     dens_free, dens_race;
    double  vmax, v_patrol, limit_kmh, notice_m, notice_red_m, see_m, lose_s, escape_m, bust_s;
    double  r_spawn_min, r_spawn_max, r_despawn, r_despawn_pursuit;
    double  wb, a_lat, a_brake;
    int     log_every, offpath_dumped;
    int     force_dens, force_vmax;              /* DEV overrides (+1 encoded, 0 = knob) */
    /* the race corridor */
    int     race_on;                             /* a race with COPS on runs (the corridor applies) */
    uint8_t *mask; int mask_n; uint32_t mask_hash;
    uint8_t *bad; int bad_n;                     /* graph edges a cop leg must not use (no road under the line), session */
    long    gen;                                 /* [F9b] planner graph generation the edge-indexed state is in */
    uint8_t *use; int use_n, use_dirty, use_race; /* the mask a leg is planned with: corridor (race) AND not bad */
    /* pursuit */
    int     pursuit, heat, offences, cause;
    long    p_t0, lost_t, spawn_cd, grace_until, last_offence;
    double  bust_meter;
    long    busted_t;                            /* > 0: the BUSTED hold runs                  */
    /* the player's red-lamp watch */
    int     pl_e, pl_d, pl_node, pl_red_node; double pl_rem;
    double  pl_vprev;
    /* toasts */
    char    toast[96]; uint32_t toast_col; long toast_until;
    /* roadblocks [G1b] */
    RcBlock blk[RC_BLK_MAX];
    int     block_heat, block_max, block_fail_run;
    int     dev_race_pursuit;                    /* DEV: a race with COPS on starts a heat 5 pursuit once (TD5RE_ROAM_COPS_DEV_RACE_PURSUIT) */
    double  block_min_m, block_max_m;
    long    block_cd;
    TD5_RoamCopsStats st;
    long    st_regraphs;                         /* [F9b] planner graph swaps carried over */
} C;

/* ============================================================ helpers ============ */

static double rc_clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int rc_ang12(int a) { a &= 0xFFF; if (a > 0x800) a -= 0x1000; return a; }
static int rc_angle_of(double dx, double dz)
{
    if (dx == 0.0 && dz == 0.0) return 0;
    return (int)floor(atan2(dx, dz) * (4096.0 / (2.0 * RC_PI)) + 0.5) & 0xFFF;
}
static uint32_t rc_lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
static double rc_unit(uint32_t *s) { return (double)(rc_lcg(s) & 0xFFFFu) / 65536.0; }
static long rc_tick(void) { return (long)g_td5.simulation_tick_counter; }

static void rc_read_knobs(void)
{
    if (C.knobs) return;
    C.knobs = 1;
    C.dens_free   = td5_env_int("TD5RE_ROAM_COPS", 2, 0, TD5_ROAM_COPS_POOL_MAX);
    C.dens_race   = td5_env_int("TD5RE_ROAM_COPS_RACE", 2, 0, TD5_ROAM_COPS_POOL_MAX);
    C.vmax        = td5_env_int("TD5RE_ROAM_COPS_VMAX", 125, 30, 250) / 3.6;
    C.v_patrol    = td5_env_int("TD5RE_ROAM_COPS_PATROL_KMH", 36, 10, 80) / 3.6;
    C.limit_kmh   = td5_env_int("TD5RE_ROAM_COPS_LIMIT_KMH", 60, 20, 200);
    C.notice_m    = td5_env_int("TD5RE_ROAM_COPS_NOTICE_M", 60, 10, 300);
    C.notice_red_m = td5_env_int("TD5RE_ROAM_COPS_NOTICE_RED_M", 80, 10, 300);
    C.see_m       = td5_env_int("TD5RE_ROAM_COPS_SEE_M", 150, 30, 600);
    C.lose_s      = td5_env_int("TD5RE_ROAM_COPS_LOSE_S", 6, 1, 120);
    C.escape_m    = td5_env_int("TD5RE_ROAM_COPS_ESCAPE_M", 320, 80, 2000);
    C.bust_s      = td5_env_int("TD5RE_ROAM_COPS_BUST_DS", 20, 3, 200) / 10.0;
    C.log_every   = td5_env_int("TD5RE_ROAM_COPS_LOG", 0, 0, 60);     /* DEV: a pursuit line every N s */
    C.r_spawn_min = td5_env_int("TD5RE_ROAM_COPS_SPAWN_MIN_M", 120, 30, 400);
    C.r_spawn_max = td5_env_int("TD5RE_ROAM_COPS_SPAWN_MAX_M", 220, 60, 500);
    C.r_despawn   = td5_env_int("TD5RE_ROAM_COPS_DESPAWN_M", 330, 80, 800);
    C.r_despawn_pursuit = td5_env_int("TD5RE_ROAM_COPS_DESPAWN_PURSUIT_M", 450, 100, 2000);
    C.block_heat  = td5_env_int("TD5RE_ROAM_COPS_BLOCK_HEAT", 3, 0, 5);
    C.dev_race_pursuit = td5_env_int("TD5RE_ROAM_COPS_DEV_RACE_PURSUIT", 0, 0, 1);
    C.block_max   = td5_env_int("TD5RE_ROAM_COPS_BLOCKS", 2, 1, RC_BLK_MAX);
    C.block_min_m = td5_env_int("TD5RE_ROAM_COPS_BLOCK_MIN_M", 200, 100, 500);
    C.block_max_m = td5_env_int("TD5RE_ROAM_COPS_BLOCK_MAX_M", 400, 150, 800);
    if (C.block_max_m < C.block_min_m + 50.0) C.block_max_m = C.block_min_m + 50.0;
    C.wb          = td5_env_int("TD5RE_ROAM_COPS_WB", 26, 10, 60) / 10.0;
    C.a_lat       = td5_env_int("TD5RE_ROAM_COPS_ALAT", 45, 10, 150) / 10.0;
    C.a_brake     = td5_env_int("TD5RE_ROAM_COPS_BRAKE", 55, 10, 150) / 10.0;
    if (C.r_spawn_max < C.r_spawn_min + 20.0) C.r_spawn_max = C.r_spawn_min + 20.0;
    if (C.r_despawn < C.r_spawn_max + 40.0) C.r_despawn = C.r_spawn_max + 40.0;
}

static double rc_vmax(void) { return C.force_vmax ? (C.force_vmax - 1) / 3.6 : C.vmax; }

static int rc_place_of(const TD5_Actor *a, double *px, double *pz)
{
    int32_t ox, oz;
    if (!a) return 0;
    td5_roam_world_origin(&ox, &oz);
    *px = (double)a->world_pos.x / 256.0 + (double)ox;
    *pz = (double)a->world_pos.z / 256.0 + (double)oz;
    return 1;
}

/* locator at a PLACE point: 1 when a resident tile covers it; *inside = on the drivable surface */
static int rc_locate(double px, double pz, int32_t y, int has_y, int *inside, int32_t *gy)
{
    const TD5_RoamResident *rs = td5_roam_resident();
    int32_t ox, oz;
    TD5_RoamHit h;
    if (!rs) return 0;
    td5_roam_world_origin(&ox, &oz);
    if (!td5_roam_locate(rs, (int32_t)floor((px - ox) * 256.0 + 0.5), (int32_t)floor((pz - oz) * 256.0 + 0.5), y, has_y,
                         ROAM_PRIM_NONE, &h) || h.prim == ROAM_PRIM_NONE)
        return 0;
    if (inside) *inside = h.inside;
    if (gy) *gy = h.ground_y;
    return 1;
}

static int rc_inside(double px, double pz)
{
    int in = 0;
    return rc_locate(px, pz, 0, 0, &in, NULL) && in;
}

static double rc_speed_ms(const TD5_Actor *a)
{
    return sqrt((double)a->linear_velocity_x * a->linear_velocity_x + (double)a->linear_velocity_z * a->linear_velocity_z) /
           256.0 * RC_HZ / RC_M;
}

static void rc_heading(const TD5_Actor *a, double *fx, double *fz)
{
    const double hy = (double)((((int)a->euler_accum.yaw) >> 8) & 0xFFF) * (2.0 * RC_PI / 4096.0);
    *fx = sin(hy); *fz = cos(hy);
}

/* line of sight: the straight line stays on the drivable surface (a building block between hides the player) */
static int rc_los(double ax, double az, double bx, double bz, double max_m)
{
    const double d = hypot(bx - ax, bz - az) / RC_M;
    int n, i;
    if (d > max_m) return 0;
    n = (int)(d / 2.5);
    for (i = 1; i < n; i++) {
        const double t = (double)i / (double)n;
        if (!rc_inside(ax + (bx - ax) * t, az + (bz - az) * t)) return 0;
    }
    return 1;
}

static double rc_class_kmh(int klass)
{
    switch (klass) {
    case 9: return 60.0; case 8: return 52.0; case 7: return 46.0; case 6: return 42.0; case 5: return 38.0;
    case 4: case 3: return 32.0;
    default: return 20.0;
    }
}

static void rc_toast(const char *msg, uint32_t col, double secs)
{
    snprintf(C.toast, sizeof C.toast, "%s", msg);
    C.toast_col = col;
    C.toast_until = rc_tick() + (long)(secs * RC_HZ);
    TD5_LOG_I(LOG_TAG, "ROAM_COPS: \"%s\"", msg);
}

/* ============================================================ pool + assets ======= */

int td5_roam_cops_pool(void)
{
    int pool, base;
    if (!td5_roam_active()) return 0;
    pool = td5_env_int("TD5RE_ROAM_COPS_POOL", 8, 0, TD5_ROAM_COPS_POOL_MAX);   /* [G1b] 6 -> 8: slots 30..37 */
    base = g_traffic_slot_base + td5_roam_traffic_pool();
    if (base + pool > 40) pool = 40 - base;                 /* the roam per-slot tables hold 40 */
    return pool < 0 ? 0 : pool;
}

int td5_roam_cops_base(void) { return g_traffic_slot_base + td5_roam_traffic_pool(); }

void td5_roam_cops_load_assets(void)
{
    const int pool = td5_roam_cops_pool();
    const int base = td5_roam_cops_base();
    int model, i, loaded = 0;
    C.loaded = 0;
    if (pool <= 0) return;
    /* pool slot 3 of the host level's traffic row is its police model (td5_game.c step 5b); else track 0's; else 36 */
    model = td5_asset_resolve_traffic_model_index(g_td5.track_index, 0, 3);
    if (model < 0) model = td5_asset_resolve_traffic_model_index(0, 0, 3);
    if (model < 0) model = 36;
    for (i = 0; i < pool; i++) {
        const int slot = base + i;
        if (td5_asset_load_traffic_model(model, slot)) {
            td5_physics_seed_traffic_cardef_from_player(slot);
            loaded++;
        } else {
            TD5_LOG_W(LOG_TAG, "ROAM_COPS: slot %d police model %d failed to load", slot, model);
        }
    }
    td5_render_set_cop_mesh(td5_asset_load_cop_mesh(model));   /* the police mesh over every g_cop_is_cop slot */
    C.loaded = loaded;
    C.model = model;
    TD5_LOG_I(LOG_TAG, "ROAM_COPS: assets: police model %d into %d/%d slots (base %d)", model, loaded, pool, base);
}

static void rc_park(RcCop *c)
{
    TD5_Actor *a = td5_game_get_actor(c->slot);
    c->state = RC_PARKED;
    c->alpha = 0; c->fade = 0; c->hold = 0;
    c->np = 0;
    td5_ai_roam_mark_cop(c->slot, 0);
    if (a) {
        a->linear_velocity_x = a->linear_velocity_z = a->linear_velocity_y = 0;
        a->angular_velocity_yaw = 0;
        a->steering_command = 0;
        a->encounter_steering_cmd = 0;
        a->brake_flag = 1;
    }
}

static void rc_world_shift(int32_t sx, int32_t sz);

void td5_roam_cops_session_init(void)
{
    int i;
    const int keep_loaded = C.loaded, keep_model = C.model, keep_fd = C.force_dens, keep_fv = C.force_vmax;
    free(C.mask); free(C.bad); free(C.use);
    memset(&C, 0, sizeof C);
    C.force_dens = keep_fd; C.force_vmax = keep_fv;
    if (!td5_roam_active()) return;
    C.loaded = keep_loaded; C.model = keep_model;
    rc_read_knobs();
    C.base = td5_roam_cops_base();
    C.pool = td5_roam_cops_pool();
    if (C.pool > 0 && C.loaded <= 0) {
        TD5_LOG_W(LOG_TAG, "ROAM_COPS: no police car loaded: cops off");
        C.pool = 0;
    }
    C.on = C.pool > 0;
    td5_roam_rebase_listen(rc_world_shift);   /* [F9b] */
    C.rng = 0x5EED0F10u;
    C.pl_e = -1; C.pl_node = -1; C.pl_red_node = -1;
    for (i = 0; i < C.pool; i++) {
        C.cop[i].slot = C.base + i;
        rc_park(&C.cop[i]);
    }
    C.st.pool = C.pool;
    td5_game_roam_set_racer_field(td5_roam_ai_count());   /* the field now spans the cop slots too (td5_roam_cops_field_total) */
    TD5_LOG_I(LOG_TAG, "ROAM_COPS: session: pool %d (slots %d..%d), patrol %d per anchor free roam / %d in a race with COPS on, "
              "pursuit top %.0f km/h, speeding over max(%.0f km/h, 1.5 x street), notice %.0f m (red %.0f m), see %.0f m, "
              "lose %.0f s + 2 s/heat, escape %.0f m, bust %.1f s", C.pool, C.base, C.base + C.pool - 1, C.dens_free, C.dens_race,
              C.vmax * 3.6, C.limit_kmh, C.notice_m, C.notice_red_m, C.see_m, C.lose_s, C.escape_m, C.bust_s);
}

void td5_roam_cops_session_end(void)
{
    int i;
    for (i = 0; i < C.pool; i++) td5_ai_roam_mark_cop(C.cop[i].slot, 0);
    C.on = 0;
    free(C.mask); C.mask = NULL; C.mask_n = 0;
    free(C.bad); C.bad = NULL; C.bad_n = 0; free(C.use); C.use = NULL; C.use_n = 0;
}

/* [F9b] The planner graph is rebuilt off-thread as its region follows the car and swapped in: edge and node INDICES
 * change, keys do not. The cops' paths are PLACE-frame point lists (re-planned every 0.67 s in a pursuit) and need
 * nothing; what is indexed by edge or node is carried over: the learned "no road under the line" set through the
 * keys, the race corridor rebuilt from the route, the player's red-lamp watch remapped (or restarted). */
static void rc_regraph(void)
{
    const long gen = td5_roam_route_graph_gen();
    if (gen == C.gen) return;
    if (C.bad) {
        const int n = td5_roam_route_graph_nedge();
        uint8_t *nb = n > 0 ? (uint8_t *)calloc((size_t)n, 1) : NULL;
        int b, kept = 0, lost = 0;
        for (b = 0; nb && b < C.bad_n; b++) {
            int e;
            if (!C.bad[b]) continue;
            e = td5_roam_route_graph_remap_edge(C.gen, b);
            if (e >= 0 && e < n) { nb[e] = 1; kept++; } else lost++;
        }
        free(C.bad);
        C.bad = nb; C.bad_n = nb ? n : 0;
        if (kept || lost) TD5_LOG_I(LOG_TAG, "ROAM_COPS: planner graph gen %ld -> %ld: %d bad edge(s) carried, %d left the graph",
                                    C.gen, gen, kept, lost);
    }
    free(C.mask); C.mask = NULL; C.mask_n = 0; C.mask_hash = 0;     /* the corridor is rebuilt from the route */
    C.use_dirty = 1;
    if (C.pl_e >= 0) {
        const int e = td5_roam_route_graph_remap_edge(C.gen, C.pl_e);
        C.pl_e = e >= 0 ? e : -1;
    }
    if (C.pl_red_node >= 0) {
        const int nn = td5_roam_route_graph_remap_node(C.gen, C.pl_red_node);
        C.pl_red_node = nn >= 0 ? nn : -1;
    }
    C.gen = gen;
    C.st_regraphs++;
}

/* [F9b] World-origin rebase: a live cop is an actor the rebase core moved, its path is PLACE frame. A PARKED cop keeps
 * its last world position until a spawn re-places it: held there (the shift undone), so it is never carried far off. */
static void rc_world_shift(int32_t sx, int32_t sz)
{
    int i;
    for (i = 0; i < C.pool; i++) {
        TD5_Actor *a;
        if (C.cop[i].state != RC_PARKED) continue;
        a = td5_game_get_actor(C.cop[i].slot);
        if (!a) continue;
        a->world_pos.x -= FP_SCALE(sx); a->world_pos.z -= FP_SCALE(sz);
        a->render_pos.x -= (float)sx;   a->render_pos.z -= (float)sz;
    }
}

/* the leg mask: the race corridor (a race with COPS on) AND not a bad edge; NULL = every edge */
static const uint8_t *rc_use_mask(void)
{
    const int n = td5_roam_route_graph_nedge();
    int e;
    if (n <= 0) return NULL;
    if (!C.bad) { C.bad = (uint8_t *)calloc((size_t)n, 1); C.bad_n = C.bad ? n : 0; C.use_dirty = 1; }
    if (!C.use || C.use_n != n) { free(C.use); C.use = (uint8_t *)malloc((size_t)n); C.use_n = n; C.use_dirty = 1; }
    if (!C.bad || !C.use) return C.race_on ? C.mask : NULL;
    if (C.use_race != C.race_on) { C.use_race = C.race_on; C.use_dirty = 1; }
    if (C.use_dirty) {
        for (e = 0; e < n; e++) C.use[e] = (uint8_t)(!C.bad[e] && (!C.race_on || (C.mask && e < C.mask_n && C.mask[e])));
        C.use_dirty = 0;
    }
    return C.use;
}

int td5_roam_cops_field_total(void)
{
    if (!C.on || C.pool <= 0) return 0;
    return C.base + C.pool;
}

static RcCop *rc_cop_of(int slot)
{
    if (!C.on || slot < C.base || slot >= C.base + C.pool || !td5_roam_active()) return NULL;
    return &C.cop[slot - C.base];
}

int td5_roam_cops_parked(int slot)
{
    const RcCop *c = rc_cop_of(slot);
    if (!c) return -1;
    return c->state == RC_PARKED;
}

int td5_roam_cops_alpha(int slot)
{
    const RcCop *c = rc_cop_of(slot);
    if (!c) return -1;
    return c->state == RC_PARKED ? 0 : c->alpha;
}

/* ============================================================ race corridor ======= */

/* the race's COPS row and corridor: in a race only the route's own edges (built once per route) */
static int rc_race_level(int *in_race)
{
    const int rs = td5_roam_race_state();
    const int racing = rs == TD5_RRACE_COUNTDOWN || rs == TD5_RRACE_RACING || rs == TD5_RRACE_FINISHED;
    if (in_race) *in_race = racing;
    if (!racing) return C.force_dens ? C.force_dens - 1 : C.dens_free;
    if (!td5_roam_race_cops()) return 0;
    return C.force_dens ? C.force_dens - 1 : C.dens_race;
}

static void rc_corridor_update(int in_race)
{
    const TD5_RouteResult *r;
    int i, n;
    if (!in_race || !td5_roam_race_cops()) {
        C.race_on = 0;
        return;
    }
    r = td5_roam_route_result();
    C.race_on = 1;
    if (C.mask && r && r->hash == C.mask_hash) return;
    C.use_dirty = 1;
    n = td5_roam_route_graph_nedge();
    free(C.mask);
    C.mask = n > 0 ? (uint8_t *)calloc((size_t)n, 1) : NULL;
    C.mask_n = n;
    C.mask_hash = r ? r->hash : 0;
    if (!C.mask || !r) return;
    {
        int on = 0;
        for (i = 0; i < r->nedge; i++) {
            const int e = td5_roam_route_graph_edge_index(r->edge[i].key);
            if (e >= 0 && e < n && !C.mask[e]) { C.mask[e] = 1; on++; }
        }
        TD5_LOG_I(LOG_TAG, "ROAM_COPS: race corridor: %d of %d route edges on the planner graph (hash %08X)", on, r->nedge,
                  (unsigned)r->hash);
    }
}

/* a route polyline point `along` metres in (planner's route = the race's), PLACE frame + travel tangent */
static int rc_route_point(double along, double *x, double *z, double *tx, double *tz)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    int i = 0;
    double t, l, dx, dz;
    if (!r || r->npt < 2 || !r->dist_m) return 0;
    if (along < 0.0) along = 0.0;
    if (along > r->length_m) along = r->length_m;
    while (i + 2 < r->npt && r->dist_m[i + 1] < along) i++;
    l = r->dist_m[i + 1] - r->dist_m[i];
    t = l > 0.0 ? (along - r->dist_m[i]) / l : 0.0;
    dx = r->px[i + 1] - r->px[i]; dz = r->pz[i + 1] - r->pz[i];
    *x = r->px[i] + dx * t; *z = r->pz[i] + dz * t;
    l = sqrt(dx * dx + dz * dz);
    if (tx) { *tx = l > 0.0 ? dx / l : 0.0; *tz = l > 0.0 ? dz / l : 1.0; }
    return 1;
}

/* ============================================================ path ================ */

/* The Driver Model's path on a planned polyline (td5_roam_ai.c's shape): resampled every RC_DS, lightly smoothed,
 * a corner speed sqrt(a_lat R) over a +-8 m window capped at v_cap, then a backward braking pass to end_v. */
static int rc_set_path(RcCop *c, const double *qx, const double *qz, int nq, double v_cap, double end_v)
{
    double *cum, total, rx[RC_PTS], rz[RC_PTS];
    int i, seg = 0, np;
    if (nq < 2) return 0;
    cum = (double *)malloc((size_t)nq * sizeof(double));
    if (!cum) return 0;
    cum[0] = 0.0;
    for (i = 1; i < nq; i++) cum[i] = cum[i - 1] + hypot(qx[i] - qx[i - 1], qz[i] - qz[i - 1]) / RC_M;
    total = cum[nq - 1];
    np = (int)(total / RC_DS) + 1;
    if (np > RC_PTS) np = RC_PTS;
    if (np < 3) { free(cum); return 0; }
    for (i = 0; i < np; i++) {
        const double s = i * RC_DS;
        double t;
        while (seg + 2 < nq && cum[seg + 1] < s) seg++;
        t = cum[seg + 1] > cum[seg] ? (s - cum[seg]) / (cum[seg + 1] - cum[seg]) : 0.0;
        t = rc_clamp(t, 0.0, 1.0);
        rx[i] = qx[seg] + (qx[seg + 1] - qx[seg]) * t;
        rz[i] = qz[seg] + (qz[seg + 1] - qz[seg]) * t;
    }
    free(cum);
    for (i = 0; i < np; i++) {                         /* +-2 samples (4 m), ends kept */
        int j;
        double sx = 0.0, sz = 0.0;
        if (i < 2 || i + 2 >= np) { c->px[i] = rx[i]; c->pz[i] = rz[i]; continue; }
        for (j = i - 2; j <= i + 2; j++) { sx += rx[j]; sz += rz[j]; }
        c->px[i] = sx / 5.0; c->pz[i] = sz / 5.0;
    }
    {
        /* a sample off the drivable surface (a turn through a junction centre that sits in a median, a cut corner):
         * moved onto the road along the locator's push vector, 1 m inside the edge; then smoothed once more (+-1) */
        const TD5_RoamResident *rs = td5_roam_resident();
        int32_t ox, oz, moved = 0;
        td5_roam_world_origin(&ox, &oz);
        for (i = 1; rs && i < np; i++) {
            TD5_RoamHit h;
            if (!td5_roam_locate(rs, (int32_t)floor((c->px[i] - ox) * 256.0 + 0.5), (int32_t)floor((c->pz[i] - oz) * 256.0 + 0.5), 0, 0,
                                 ROAM_PRIM_NONE, &h) || h.prim == ROAM_PRIM_NONE || h.inside)
                continue;
            {
                const double l = sqrt((double)h.push_x * h.push_x + (double)h.push_z * h.push_z);
                if (l <= 0.0 || h.push_dist > 12.0 * RC_M) continue;            /* not a sliver: leave it */
                c->px[i] += h.push_x / l * (h.push_dist + 1.0 * RC_M);
                c->pz[i] += h.push_z / l * (h.push_dist + 1.0 * RC_M);
                moved++;
            }
        }
        if (moved) {
            for (i = 0; i < np; i++) { rx[i] = c->px[i]; rz[i] = c->pz[i]; }
            for (i = 1; i + 1 < np; i++) { c->px[i] = (rx[i - 1] + rx[i] + rx[i + 1]) / 3.0; c->pz[i] = (rz[i - 1] + rz[i] + rz[i + 1]) / 3.0; }
            C.st.path_fixes += moved;
        }
    }
    for (i = 0; i < np; i++) {
        const int a0 = i - 4 < 0 ? 0 : i - 4, a1 = i + 4 >= np ? np - 1 : i + 4;
        const int b0 = a0 + 1 < np ? a0 + 1 : a0, b1 = a1 - 1 > 0 ? a1 - 1 : a1;
        const double h0 = atan2(c->px[b0] - c->px[a0], c->pz[b0] - c->pz[a0]);
        const double h1 = atan2(c->px[a1] - c->px[b1], c->pz[a1] - c->pz[b1]);
        double dh = h1 - h0, R;
        while (dh > RC_PI) dh -= 2.0 * RC_PI;
        while (dh < -RC_PI) dh += 2.0 * RC_PI;
        dh = fabs(dh);
        R = dh > 1e-3 ? ((a1 - a0) * RC_DS) / dh : 1e9;
        c->pv[i] = rc_clamp(sqrt(C.a_lat * R), 3.0, v_cap);
    }
    if (c->pv[np - 1] > end_v) c->pv[np - 1] = end_v;
    for (i = np - 2; i >= 0; i--) {
        const double lim = sqrt(c->pv[i + 1] * c->pv[i + 1] + 2.0 * C.a_brake * RC_DS);
        if (lim < c->pv[i]) c->pv[i] = lim;
    }
    c->np = np;
    c->pi = 0;
    return 1;
}

static double rc_path_proj(RcCop *c, double x, double z, double *lat)
{
    int i, best = -1, lo = c->pi - 8, hi = c->pi + 40;
    double bd = 1e300, bt = 0.0;
    if (c->np < 2) { *lat = 0.0; return 0.0; }
    if (lo < 0) lo = 0;
    if (hi > c->np - 2) hi = c->np - 2;
    for (i = lo; i <= hi; i++) {
        const double sx = c->px[i + 1] - c->px[i], sz = c->pz[i + 1] - c->pz[i], l2 = sx * sx + sz * sz;
        double t = l2 > 0.0 ? ((x - c->px[i]) * sx + (z - c->pz[i]) * sz) / l2 : 0.0, qx, qz, d;
        t = rc_clamp(t, 0.0, 1.0);
        qx = c->px[i] + sx * t; qz = c->pz[i] + sz * t;
        d = (qx - x) * (qx - x) + (qz - z) * (qz - z);
        if (d < bd) { bd = d; best = i; bt = t; }
    }
    if (best < 0) { *lat = 0.0; return c->pi * RC_DS; }
    c->pi = best;
    {
        const double sx = c->px[best + 1] - c->px[best], sz = c->pz[best + 1] - c->pz[best], l = sqrt(sx * sx + sz * sz);
        *lat = l > 0.0 ? ((x - c->px[best]) * (sz / l) - (z - c->pz[best]) * (sx / l)) / RC_M : 0.0;   /* + = right */
    }
    return (best + bt) * RC_DS;
}

/* path point at s metres, offset `off` metres to the right of travel */
static void rc_path_point(const RcCop *c, double s, double off, double *x, double *z, double *tx, double *tz)
{
    int i = (int)floor(s / RC_DS);
    double t, sx, sz, l;
    if (i < 0) i = 0;
    if (i > c->np - 2) i = c->np - 2;
    t = rc_clamp(s / RC_DS - i, 0.0, 1.0);
    sx = c->px[i + 1] - c->px[i]; sz = c->pz[i + 1] - c->pz[i];
    l = sqrt(sx * sx + sz * sz);
    if (l <= 0.0) { sx = 0.0; sz = 1.0; l = 1.0; }
    sx /= l; sz /= l;
    *x = c->px[i] + (c->px[i + 1] - c->px[i]) * t + sz * off * RC_M;
    *z = c->pz[i] + (c->pz[i + 1] - c->pz[i]) * t - sx * off * RC_M;
    if (tx) { *tx = sx; *tz = sz; }
}

static double rc_path_len(const RcCop *c) { return c->np > 1 ? (c->np - 1) * RC_DS : 0.0; }

/* plan from the cop (its heading) to a PLACE point; 1 = a new path */
static int rc_plan(RcCop *c, const TD5_Actor *a, double gx, double gz, double v_cap, double end_v)
{
    double x, z, fx, fz, *qx = NULL, *qz = NULL, len = 0.0;
    int nq = 0, ok;
    rc_place_of(a, &x, &z);
    rc_heading(a, &fx, &fz);
    C.st.plans++;
    c->plan_t = rc_tick();
    {
        /* plan from a point AHEAD of the car on its heading (4..12 m, 0.6 s): at a junction the street under the car
         * can snap to the crossing street, and a leg starting there threw the wheel to lock (F10 smoke run 2) */
        const double la = rc_clamp(rc_speed_ms(a) * 0.6, 4.0, 12.0);
        double sx = x + fx * la * RC_M, sz = z + fz * la * RC_M, hx = fx, hz = fz;
        /* the player BEHIND the cop and close (it overshot a car that braked): leave either way from the cop itself
         * (it turns round) instead of round the block (F10 smoke: a 691 m leg for a car 51 m behind on the same street) */
        c->turning = c->state == RC_PURSUIT && (gx - x) * fx + (gz - z) * fz < 0.0 && hypot(gx - x, gz - z) < 150.0 * RC_M;
        /* a pursuer within 150 m of the player may take a one-way against the flow (siren on): 30 m back down a one-way
         * street, not a 700 m legal loop round the block */
        const int close_ow = c->state == RC_PURSUIT && hypot(gx - x, gz - z) < 150.0 * RC_M;
        if (c->turning) { sx = x; sz = z; hx = 0.0; hz = 0.0; }
        if (!rc_inside(sx, sz)) { sx = x; sz = z; }
        ok = td5_roam_route_plan_path(sx, sz, hx, hz, gx, gz, 60.0, rc_use_mask(), close_ow, &qx, &qz, &nq, &len);
#ifndef TD5RE_RELEASE
        if (ok && C.log_every) {
            const double straight = hypot(gx - sx, gz - sz) / RC_M;
            if (len > 2.5 * straight + 60.0 && C.st.plans % 4 == 0) {
                uint64_t ks = 0, kt = 0;
                double d1, d2, a, b;
                td5_roam_route_snap_xz(sx, sz, 60.0, &a, &b, &ks, &d1);
                td5_roam_route_snap_xz(gx, gz, 60.0, &a, &b, &kt, &d2);
                TD5_LOG_I(LOG_TAG, "ROAM_COPS_LONGPLAN slot %d %s: leg %.0f m for %.0f m straight, from (%.0f,%.0f) heading (%.2f,%.2f) edge %016llx "
                          "(%.1f m off) to (%.0f,%.0f) edge %016llx (%.1f m off), %d points", c->slot, c->state == RC_PURSUIT ? "pursuit" : "patrol",
                          len, straight, sx, sz, fx, fz, (unsigned long long)ks, d1, gx, gz, (unsigned long long)kt, d2, nq);
            }
        }
#endif
        if (ok && nq >= 2) {
            /* the car, then the planned line */
            double *bx = (double *)malloc((size_t)(nq + 1) * sizeof(double)), *bz = (double *)malloc((size_t)(nq + 1) * sizeof(double));
            if (bx && bz) {
                bx[0] = x; bz[0] = z;
                memcpy(bx + 1, qx, (size_t)nq * sizeof(double));
                memcpy(bz + 1, qz, (size_t)nq * sizeof(double));
                ok = rc_set_path(c, bx, bz, nq + 1, v_cap, end_v);
            } else ok = 0;
            free(bx); free(bz);
        } else ok = 0;
    }
    free(qx); free(qz);
    if (ok) {
        /* the first 80 m of the leg on the road (the sliver fix done): a graph line with no road under it (seen at the
         * casco's south-west edge, F10 smoke run 12: 60 m of an edge over tile area with no ribbon) refuses the leg
         * and bars that edge for the session's cop legs, so the next plan goes round it */
        const TD5_RoamResident *rs = td5_roam_resident();
        int32_t ox, oz;
        int i, lim = (int)(80.0 / RC_DS);
        td5_roam_world_origin(&ox, &oz);
        if (lim > c->np) lim = c->np;
        for (i = 1; rs && i < lim; i++) {
            TD5_RoamHit h;
            const int fnd = td5_roam_locate(rs, (int32_t)floor((c->px[i] - ox) * 256.0 + 0.5), (int32_t)floor((c->pz[i] - oz) * 256.0 + 0.5),
                                            0, 0, ROAM_PRIM_NONE, &h) && h.prim != ROAM_PRIM_NONE;
            if (fnd && (h.inside || h.push_dist <= 1.5 * RC_M)) continue;
            if (!fnd && td5_roam_stream_point_missing((int32_t)floor((c->px[i] - ox) * 256.0 + 0.5),
                                                      (int32_t)floor((c->pz[i] - oz) * 256.0 + 0.5))) break;   /* not streamed in yet */
            {
                int be;
                double along, dm;
                if (td5_roam_route_graph_snap_edge(c->px[i], c->pz[i], 15.0, &be, &along, &dm) && C.bad && be >= 0 &&
                    be < td5_roam_route_graph_nedge() && !C.bad[be]) {
                    C.bad[be] = 1; C.use_dirty = 1; C.st.bad_edges++;
                    TD5_LOG_I(LOG_TAG, "ROAM_COPS: slot %d leg leaves the road %.0f m ahead at place (%.0f,%.0f) (%s): edge %d barred for cop legs",
                              c->slot, i * RC_DS, c->px[i], c->pz[i], fnd ? "off the surface" : "no road", be);
                }
            }
            c->np = 0;
            ok = 0;
            C.st.plan_offroad++;
            break;
        }
    }
    if (!ok) { C.st.plan_fails++; c->plan_fail++; return 0; }
    c->plan_fail = 0;
    c->s = 0.0; c->pi = 0;
    return 1;
}

/* a random patrol goal 250..600 m away (free roam) or 250 m further along the route (a race) */
static int rc_patrol_goal(RcCop *c, const TD5_Actor *a, double *gx, double *gz)
{
    double x, z;
    rc_place_of(a, &x, &z);
    if (C.race_on) {
        double along = 0.0;
        const TD5_RouteResult *r = td5_roam_route_result();
        int i, bi = 0;
        double bd = 1e300;
        if (!r || r->npt < 2 || !r->dist_m) return 0;
        for (i = 0; i < r->npt; i++) {
            const double d = (r->px[i] - x) * (r->px[i] - x) + (r->pz[i] - z) * (r->pz[i] - z);
            if (d < bd) { bd = d; bi = i; }
        }
        along = r->dist_m[bi] + 250.0;
        if (along > r->length_m - 5.0) along = r->length_m - 5.0;
        return rc_route_point(along, gx, gz, NULL, NULL);
    }
    {
        const double th = rc_unit(&c->rng) * 2.0 * RC_PI, r = (250.0 + rc_unit(&c->rng) * 350.0) * RC_M;
        double sx, sz, dm;
        uint64_t key;
        if (!td5_roam_route_snap_xz(x + cos(th) * r, z + sin(th) * r, 120.0, &sx, &sz, &key, &dm)) return 0;
        if (!rc_inside(sx, sz)) return 0;          /* a goal on a resident street */
        *gx = sx; *gz = sz;
        return 1;
    }
}

/* ============================================================ spawn ================ */

static int rc_clear_of_actors(double px, double pz, double r_m, int self)
{
    int j, total = td5_game_get_total_actor_count();
    for (j = 0; j < total; j++) {
        const TD5_Actor *b;
        double bx, bz;
        if (j == self) continue;
        if (j < g_traffic_slot_base && td5_game_get_slot_state(j) == 3) continue;
        if (j >= g_traffic_slot_base && td5_roam_traffic_parked(j) == 1) continue;
        b = td5_game_get_actor(j);
        if (!b || !rc_place_of(b, &bx, &bz)) continue;
        if (hypot(bx - px, bz - pz) < r_m * RC_M) return 0;
    }
    return 1;
}

/* td5_roam_traffic.c's traffic placement (dynamics zeroed, the traffic pose sets y and the angles next tick) */
static void rc_place_actor(RcCop *c, TD5_Actor *a, double px, double pz, int yaw12, double v)
{
    int32_t ox, oz;
    const double y = (double)(yaw12 & 0xFFF) * (2.0 * RC_PI / 4096.0);
    const double vu = v * RC_M / RC_HZ * 256.0;
    int q, in = 0;
    int32_t gy = 0;
    td5_roam_world_origin(&ox, &oz);
    a->world_pos.x = FP_SCALE((int32_t)floor(px - ox + 0.5));
    a->world_pos.y = (int32_t)0xC0000000;
    a->world_pos.z = FP_SCALE((int32_t)floor(pz - oz + 0.5));
    a->slot_index = (uint8_t)c->slot;
    memset(&a->tire_track_emitter_FL, 0xFF, 4);
    a->euler_accum.yaw = FP_SCALE(yaw12 & 0xFFF);
    td5_roam_physics_forget_slot(c->slot);
    td5_ai_clear_actor_broken_down(c->slot);
    a->airborne_frame_counter = 0;
    a->surface_contact_flags = 0;
    a->vehicle_mode = 0;
    a->angular_velocity_roll = a->angular_velocity_yaw = a->angular_velocity_pitch = 0;
    a->linear_velocity_x = a->linear_velocity_y = a->linear_velocity_z = 0;
    a->frame_counter = 0;
    a->wheel_contact_bitmask = 0;
    for (q = 0; q < 4; q++) { a->wheel_suspension_pos[q] = 0; a->wheel_spring_dv[q] = 0; a->wheel_load_accum[q] = 0; }
    if (rc_locate(px, pz, 0, 0, &in, &gy)) a->world_pos.y = gy;
    a->display_angles.roll = 0; a->euler_accum.roll = 0;
    a->display_angles.pitch = 0; a->euler_accum.pitch = 0;
    a->display_angles.yaw = (int16_t)FP_TRUNC(a->euler_accum.yaw);
    a->render_pos.x = (float)a->world_pos.x * (1.0f / 256.0f);
    a->render_pos.y = (float)a->world_pos.y * (1.0f / 256.0f);
    a->render_pos.z = (float)a->world_pos.z * (1.0f / 256.0f);
    a->track_span_raw = -1;
    a->track_span_normalized = -1;
    a->track_span_accumulated = -1;
    a->track_span_high_water = -1;
    a->track_sub_lane_index = 0;
    td5_roam_set_slot_edge(c->slot, 0);
    a->linear_velocity_x = (int32_t)(sin(y) * vu);
    a->linear_velocity_z = (int32_t)(cos(y) * vu);
    a->steering_command = 0;
    a->encounter_steering_cmd = 0;
    a->brake_flag = 0; a->handbrake_flag = 0;
}

/* centre + 4 body corners on the drivable surface, one road surface (not under or on a deck) */
static int rc_footprint_ok(double x, double z, double tx, double tz)
{
    const double rx = tz, rz = -tx;
    int q;
    if (!rc_inside(x, z)) return 0;
    for (q = 0; q < 4; q++) {
        const double l = (q & 1) ? 2.3 : -2.3, w = (q & 2) ? 1.0 : -1.0;
        if (!rc_inside(x + (tx * l + rx * w) * RC_M, z + (tz * l + rz * w) * RC_M)) return 0;
    }
    {
        TD5_RoamCand cand[4];
        int32_t ox, oz;
        const TD5_RoamResident *rs = td5_roam_resident();
        int nc;
        if (!rs) return 0;
        td5_roam_world_origin(&ox, &oz);
        nc = td5_roam_collect(rs, (int32_t)floor((x - ox) * 256.0 + 0.5), (int32_t)floor((z - oz) * 256.0 + 0.5), cand, 4);
        for (q = 1; q < nc; q++)
            if (abs(cand[q].h - cand[0].h) > 2 * 430 * 256) return 0;
    }
    return 1;
}

static RcCop *rc_free_cop(void)
{
    int i;
    for (i = 0; i < C.pool; i++) if (C.cop[i].state == RC_PARKED) return &C.cop[i];
    return NULL;
}

/* put cop c on the road at (x, z) facing (tx, tz); state + plan */
static int rc_spawn_at(RcCop *c, double x, double z, double tx, double tz, int state, int hold)
{
    TD5_Actor *a = td5_game_get_actor(c->slot);
    const int slot = c->slot;
    if (!a) return 0;
    memset(c, 0, sizeof *c);
    c->slot = slot;
    c->rng = 0x9E3779B9u ^ ((uint32_t)slot * 2654435761u) ^ (uint32_t)(C.spawn_count * 40503u);
    rc_lcg(&c->rng);
    c->born = rc_tick();
    c->last_contact = -100000;
    c->hold = hold;
    c->state = state;
    c->alpha = 0; c->fade = 1;
    c->role = 0;
    rc_place_actor(c, a, x, z, rc_angle_of(tx, tz), hold ? 0.0 : (state == RC_PURSUIT ? 8.0 : C.v_patrol * 0.7));
    td5_ai_roam_mark_cop(slot, 1);
    C.spawn_count++;
    C.st.spawns++;
    return 1;
}

/* one try to spawn a cop round (ax, az); `ahead` = 1: a reinforcement, from BEHIND or beside the player (heading (fx, fz)),
 * so it joins the chase instead of meeting the car head-on (an escape stays possible). 1 when placed. */
static int rc_try_spawn(double ax, double az, double fx, double fz, int ahead, int state)
{
    RcCop *c = rc_free_cop();
    TD5_RouteGEdge E;
    double x, z, tx, tz, along, dm;
    int e, d;
    if (!c) return 0;
    if (C.race_on) {
        /* the route corridor ahead of the player */
        double pa = 0.0, len = 0.0, lat = 0.0, s;
        td5_roam_race_progress(&pa, &len, &lat);
        s = pa + C.r_spawn_min + rc_unit(&C.rng) * (C.r_spawn_max - C.r_spawn_min);
        if (s > len - 20.0) return 0;
        if (!rc_route_point(s, &x, &z, &tx, &tz)) return 0;
        {   /* right lane of the route direction when it fits */
            const double ox = x + tz * 1.6 * RC_M, oz = z - tx * 1.6 * RC_M;
            if (rc_footprint_ok(ox, oz, tx, tz)) { x = ox; z = oz; }
        }
    } else {
        double th = rc_unit(&C.rng) * 2.0 * RC_PI;
        const double r = (C.r_spawn_min + rc_unit(&C.rng) * (C.r_spawn_max - C.r_spawn_min)) * RC_M;
        double px, pz;
        if (ahead) th = atan2(-fz, -fx) + (rc_unit(&C.rng) - 0.5) * 3.4;  /* behind / beside the player (+-97 deg round its back) */
        px = ax + cos(th) * r; pz = az + sin(th) * r;
        if (!td5_roam_route_graph_snap_edge(px, pz, 60.0, &e, &along, &dm)) return 0;
        if (!td5_roam_route_graph_edge_view(e, &E) || E.npt < 2 || E.plen < 30.0 * RC_M) return 0;
        if (E.flags & 0x03) return 0;                          /* not on a deck or in a bore */
        if (E.klass <= 2) return 0;                             /* not on a service / living street */
        if (along < 8.0 * RC_M || along > E.plen - 8.0 * RC_M) return 0;
        d = E.oneway > 0 ? 0 : (E.oneway < 0 ? 1 : (int)(rc_lcg(&C.rng) & 1));
        {
            int lo = 0, hi = E.npt - 2;
            double l, ux, uz, off;
            while (lo < hi) { const int mid = (lo + hi + 1) / 2; if (E.cum[mid] <= along) lo = mid; else hi = mid - 1; }
            ux = (double)E.pt[2 * lo + 2] - E.pt[2 * lo]; uz = (double)E.pt[2 * lo + 3] - E.pt[2 * lo + 1];
            l = sqrt(ux * ux + uz * uz);
            if (l <= 0.0) return 0;
            ux /= l; uz /= l;
            {
                const double t = (along - E.cum[lo]) / (E.cum[lo + 1] - E.cum[lo] > 0.0 ? E.cum[lo + 1] - E.cum[lo] : 1.0);
                x = E.pt[2 * lo] + t * (E.pt[2 * lo + 2] - E.pt[2 * lo]);
                z = E.pt[2 * lo + 1] + t * (E.pt[2 * lo + 3] - E.pt[2 * lo + 1]);
            }
            if (d) { ux = -ux; uz = -uz; }
            tx = ux; tz = uz;
            off = E.oneway ? 0.0 : (E.lanes >= 2 ? 1.6 : 0.5);   /* the right lane */
            x += tz * off * RC_M; z -= tx * off * RC_M;
        }
    }
    if (hypot(x - ax, z - az) < C.r_spawn_min * 0.75 * RC_M) return 0;
    if (!rc_footprint_ok(x, z, tx, tz)) return 0;
    if (!rc_clear_of_actors(x, z, 14.0, c->slot)) return 0;
    if (!rc_spawn_at(c, x, z, tx, tz, state, 0)) return 0;
    return 1;
}

static void rc_despawn(RcCop *c, int why)
{
    if (c->state == RC_PARKED || c->fade < 0) return;
    c->fade = -1;
    c->why_out = why;
    C.st.despawns++;
}

/* ============================================================ the driver ========== */

/* cars ahead of the cop: the speed it may drive (m/s); pass request in *pass_side (0 none) */
static double rc_gap_cap(RcCop *c, const TD5_Actor *a, double x, double z, double v, int ignore_player, double *pass_l)
{
    double fx, fz, rx, rz, cap = 1e9;
    int j, total = td5_game_get_total_actor_count();
    rc_heading(a, &fx, &fz);
    rx = fz; rz = -fx;
    *pass_l = 0.0;
    for (j = 0; j < total; j++) {
        const TD5_Actor *b;
        double bx, bz, f, l, vb;
        if (j == c->slot || (ignore_player && j == 0)) continue;
        if (j < g_traffic_slot_base && td5_game_get_slot_state(j) == 3) continue;
        if (j >= g_traffic_slot_base && td5_roam_traffic_parked(j) == 1) continue;
        b = td5_game_get_actor(j);
        if (!b || !rc_place_of(b, &bx, &bz)) continue;
        f = ((bx - x) * fx + (bz - z) * fz) / RC_M;
        if (f <= 0.3 || f > 10.0 + v * 1.6) continue;
        l = ((bx - x) * rx + (bz - z) * rz) / RC_M;
        if (fabs(l) > 1.9) continue;
        vb = ((double)b->linear_velocity_x * fx + (double)b->linear_velocity_z * fz) / 256.0 * RC_HZ / RC_M;
        {
            const double gap = f - (7.0 + 0.5 * (vb > 0.0 ? vb : 0.0));
            const double cv = (vb > 0.0 ? vb : 0.0) + (gap > 0.0 ? gap * 0.5 : gap * 1.2);
            if (cv < cap) { cap = cv; *pass_l = l; }
        }
    }
    return cap;
}

/* lane offset that keeps the aim point and the body beside it on the road: off, off/2, 0 */
static double rc_lane_fit(const RcCop *c, double s, double off)
{
    int tries;
    for (tries = 0; tries < 3; tries++) {
        double x, z, tx, tz;
        const double side = off > 0.0 ? 1.0 : -1.0;
        if (fabs(off) < 0.05) return 0.0;
        rc_path_point(c, s, off, &x, &z, &tx, &tz);
        if (rc_inside(x, z) && rc_inside(x + tz * side * 1.1 * RC_M, z - tx * side * 1.1 * RC_M)) return off;
        off *= 0.5;
    }
    return 0.0;
}

/* The signal group an approach along (E, d) obeys at its end node (td5_roam_traffic.c's rule), -1 = no signal. */
static int rc_sig_group(const TD5_RouteGEdge *E, int d, int *node)
{
    TD5_RouteGNode N;
    int16_t bear[16];
    uint64_t ek[16];
    int na, a, arm = -1, best = 0x7FFF, ob, nd;
    double ux, uz;
    nd = E->node[d == 0 ? 1 : 0];
    if (node) *node = nd;
    if (!td5_roam_route_graph_node_view(nd, &N) || N.ninc < 2) return -1;
    na = td5_roam_route_graph_signal(N.key, bear, ek, 16);
    if (na <= 0) return -1;
    if (d == 0) { ux = (double)E->pt[2 * (E->npt - 1)] - E->pt[2 * (E->npt - 2)]; uz = (double)E->pt[2 * (E->npt - 1) + 1] - E->pt[2 * (E->npt - 2) + 1]; }
    else        { ux = (double)E->pt[0] - E->pt[2]; uz = (double)E->pt[1] - E->pt[3]; }
    ob = (int)floor(atan2(-uz, -ux) * (4096.0 / (2.0 * RC_PI)) + 0.5) & 0xFFF;   /* outward, atan2(dz,dx) */
    for (a = 0; a < na; a++) if (ek[a] == E->key) { arm = a; break; }
    if (arm < 0)
        for (a = 0; a < na; a++) {
            const int dd = abs(rc_ang12((int)bear[a] - ob));
            if (dd < best) { best = dd; arm = a; }
        }
    return td5_roam_signal_group(bear, na, arm < 0 ? 0 : arm);
}

/* where a car at (x, z) heading (fx, fz) stands on the graph: edge, travel dir, metres to the end, 1 on success */
static int rc_on_graph(double x, double z, double fx, double fz, int *e, int *d, double *rem_m, TD5_RouteGEdge *E)
{
    double along, dm;
    int lo, hi;
    double ux, uz;
    if (!td5_roam_route_graph_snap_edge(x, z, 12.0, e, &along, &dm)) return 0;
    if (!td5_roam_route_graph_edge_view(*e, E) || E->npt < 2) return 0;
    lo = 0; hi = E->npt - 2;
    while (lo < hi) { const int mid = (lo + hi + 1) / 2; if (E->cum[mid] <= along) lo = mid; else hi = mid - 1; }
    ux = (double)E->pt[2 * lo + 2] - E->pt[2 * lo]; uz = (double)E->pt[2 * lo + 3] - E->pt[2 * lo + 1];
    *d = (ux * fx + uz * fz) >= 0.0 ? 0 : 1;
    *rem_m = (*d == 0 ? E->plen - along : along) / RC_M;
    return 1;
}

/* patrol: the speed a red lamp ahead allows (m/s), 1e9 = no limit */
static double rc_signal_cap(RcCop *c, const TD5_Actor *a, double x, double z, double v, double *a_req)
{
    TD5_RouteGEdge E;
    double fx, fz, rem, d_stop;
    int e, d, g, node;
    c->sig_hold = 0;
    rc_heading(a, &fx, &fz);
    if (!rc_on_graph(x, z, fx, fz, &e, &d, &rem, &E)) return 1e9;
    if (rem > 70.0) return 1e9;
    g = rc_sig_group(&E, d, &node);
    if (g < 0) return 1e9;
    d_stop = rem - RC_STOP_BACK_M;
    {
        const double ss = (double)rc_tick() / RC_HZ;
        const int ph = td5_geo_signal_phase_at(ss, g);
        /* red, or amber it can still stop for: stop at the line; a stop past 6 m/s2 is not tried */
        if ((ph == 0 || ph == 1) && d_stop > v * v / (2.0 * 6.0) - 0.3 && d_stop > -0.5) {
            c->sig_hold = 1;
            if (v > 0.2) { const double ar = d_stop > 0.15 ? -v * v / (2.0 * d_stop) : -6.5; if (ar < *a_req) *a_req = ar; }
            return d_stop > 0.2 ? sqrt(2.0 * C.a_brake * d_stop) : 0.0;
        }
    }
    return 1e9;
}

static void rc_drive(RcCop *c, TD5_Actor *a, const TD5_Actor *pl, double plx, double plz)
{
    const long tick = rc_tick();
    double x, z, v, vt, L, ax, az, dx, dz, dl, alpha, delta, fx, fz, a_req = 1e9, acc_hi, acc_lo;
    int32_t steer_t;
    int herr, heading, brake = 0, direct = 0;
    double thr;

    rc_place_of(a, &x, &z);
    v = rc_speed_ms(a);
    if (v * 3.6 > C.st.max_kmh) C.st.max_kmh = v * 3.6;
    rc_heading(a, &fx, &fz);
    c->d_player = pl ? hypot(plx - x, plz - z) / RC_M : 1e9;
    if (td5_ai_actor_is_broken_down(c->slot)) {
        /* a wreck: stands, then taken away after 8 s (the traffic rule) */
        a->steering_command = 0; a->encounter_steering_cmd = 0; a->brake_flag = 1;
        if (++c->wreck_t == 1) C.st.wrecks++;
        if (c->wreck_t > 240) rc_despawn(c, RC_WHY_WRECK);
        c->still_t = 0; c->off_t = 0;
        return;
    }
    if (c->d_player < 4.5) c->last_contact = tick;
    /* line of sight to the player, every RC_LOS_EVERY ticks */
    if (pl && tick - c->los_t >= RC_LOS_EVERY) {
        c->los_t = tick;
        c->sees = rc_los(x, z, plx, plz, c->state == RC_PURSUIT ? C.see_m : C.notice_red_m);
    }

    /* ---- the path ---- */
    if (c->state == RC_PURSUIT && pl) {
        if (tick - c->plan_t >= RC_REPLAN || c->np < 2 || fabs(c->lat) > 6.0) {
            /* the player's spot half a second ahead (velocity: 24.8 units per tick, x 15 ticks), on the street */
            const double gx = plx + (double)pl->linear_velocity_x / 256.0 * 15.0;
            const double gz = plz + (double)pl->linear_velocity_z / 256.0 * 15.0;
            const double vpl = rc_speed_ms(pl);
            if (tick - c->plan_t >= 5) rc_plan(c, a, gx, gz, rc_vmax(), vpl > 3.0 ? vpl : 3.0);
        }
    } else if (c->state == RC_PATROL && !c->hold) {
        if ((c->np < 2 || c->s > rc_path_len(c) - 25.0) && tick - c->plan_t >= (c->plan_fail ? 30 : 1)) {
            double gx, gz;
            if (!rc_patrol_goal(c, a, &gx, &gz) || !rc_plan(c, a, gx, gz, C.v_patrol, C.v_patrol)) {
                c->plan_t = tick;
                if (c->plan_fail > 6) rc_despawn(c, RC_WHY_NOPATH);
            }
        }
    }
    if (c->np >= 2) c->s = rc_path_proj(c, x, z, &c->lat);

    /* ---- target speed ---- */
    if (c->hold) vt = 0.0;
    else if (c->np >= 2) {
        const double look = v * 0.3 > 2.0 ? v * 0.3 : 2.0;
        int i = (int)floor((c->s + look) / RC_DS);
        if (i > c->np - 1) i = c->np - 1;
        vt = c->pv[i < 0 ? 0 : i];
    } else vt = 0.0;
    if (c->state == RC_PATROL) {
        double pl_l, cap;
        if (vt > C.v_patrol) vt = C.v_patrol;
        cap = rc_signal_cap(c, a, x, z, v, &a_req);
        if (cap < vt) vt = cap;
        cap = rc_gap_cap(c, a, x, z, v, 0, &pl_l);
        if (cap < vt) vt = cap;
    } else if (c->state == RC_PURSUIT && pl) {
        double pl_l, cap;
        const double vpl = rc_speed_ms(pl);
        cap = rc_gap_cap(c, a, x, z, v, 1, &pl_l);
        if (cap < vt - 1.0 && c->pass_dwell <= 0) {
            /* a slower car ahead: pass on the free side (kept on the road by the lane fit) */
            c->pass_off = pl_l > 0.0 ? -2.4 : 2.4;
            c->pass_dwell = 60;
        }
        if (cap < vt && c->pass_dwell <= 0) vt = cap;
        else if (cap < vt) vt = 0.5 * (vt + cap);
        if (c->d_player < 45.0) {
            /* closing in: no faster than the player plus the gap to a box point 6 m off it */
            const double vc = vpl + (c->d_player - 6.0) * 0.6;
            if (vc < vt) vt = vc;
        }
        if (vt < 0.0) vt = 0.0;
        direct = c->sees && (c->d_player < 30.0 || (c->np < 2 && c->d_player < 80.0));   /* no leg: straight at it while in sight */
    }
    c->v_t = vt;

    /* ---- lane ---- */
    {
        double want = c->state == RC_PATROL ? 1.3 : 0.0;
        if (c->pass_dwell > 0) { want = c->pass_off; c->pass_dwell--; }
        L = rc_clamp(4.0 + 0.4 * v, 5.0, 20.0);
        if (c->np >= 2) {
            /* a bend ahead (heading change over the next 15 m past 30 deg): the short look-ahead of td5_roam_traffic's
             * turns, a long one cuts the corner over the kerb */
            double x0, z0, x1, z1, tx0, tz0, tx1, tz1;
            rc_path_point(c, c->s, 0.0, &x0, &z0, &tx0, &tz0);
            rc_path_point(c, c->s + 15.0, 0.0, &x1, &z1, &tx1, &tz1);
            if (tx0 * tx1 + tz0 * tz1 < 0.866) L = rc_clamp(2.5 + 0.3 * v, 3.5, 8.0);
            want = rc_lane_fit(c, c->s + L, want);
        }
        if (want > c->off + 0.05) c->off += 0.05;
        else if (want < c->off - 0.05) c->off -= 0.05;
        else c->off = want;
    }

    /* ---- steering: pure pursuit on the path, or at the box point beside the player ---- */
    heading = ((int)a->euler_accum.yaw >> 8) & 0xFFF;
    if (direct) {
        double pfx, pfz, bx, bz;
        rc_heading(pl, &pfx, &pfz);
        /* role 0: behind the player, 1: ahead of it, 2: beside it (the box) */
        if (c->role == 1) { bx = plx + pfx * 7.0 * RC_M; bz = plz + pfz * 7.0 * RC_M; }
        else if (c->role == 2) { bx = plx + pfz * 3.2 * RC_M; bz = plz - pfx * 3.2 * RC_M; }
        else { bx = plx - pfx * 6.0 * RC_M; bz = plz - pfz * 6.0 * RC_M; }
        if (rc_inside(bx, bz) && rc_los(x, z, bx, bz, 40.0)) { ax = bx; az = bz; }
        else { ax = plx; az = plz; }
        {
            const double db = hypot(ax - x, az - z) / RC_M;
            const double vb = rc_speed_ms(pl) + (db - 1.5) * 0.7;
            if (vb < vt) vt = vb > 0.0 ? vb : 0.0;
            if (db < 2.0) vt = rc_speed_ms(pl) * 0.8;
        }
    } else if (c->np >= 2) {
        /* the aim point and the line to it on the drivable surface, else a shorter look-ahead */
        int k;
        for (k = 0; k < 3; k++) {
            rc_path_point(c, c->s + L, c->off, &ax, &az, NULL, NULL);
            if (rc_inside(ax, az) && rc_inside(0.5 * (ax + x), 0.5 * (az + z))) break;
            L *= 0.6;
        }
    } else {
        ax = x + fx * L * RC_M; az = z + fz * L * RC_M;
    }
    dx = ax - x; dz = az - z; dl = sqrt(dx * dx + dz * dz) / RC_M;
    herr = rc_ang12(rc_angle_of(dx, dz) - heading);
    if (c->state == RC_PURSUIT && abs(herr) > 0x300 && vt > 4.0) vt = 4.0;   /* the aim is behind: turn round slowly */
    alpha = (double)herr * (2.0 * RC_PI / 4096.0);
    delta = atan2(2.0 * C.wb * sin(alpha), dl > 1.0 ? dl : 1.0);
    steer_t = (int32_t)(delta * (4096.0 / (2.0 * RC_PI)) * 256.0);

    /* ---- recovery: still while it wants to move (and not at the player): reverse, wheel away ---- */
    if (c->rec == 0 && !c->hold && !c->sig_hold && vt > 1.5 && v < (tick - c->wall_t < 4 ? 1.0 : 0.3) && c->d_player > 9.0) {
        /* still while it wants to move: reverse out after 2 s, or after 0.5 s when the road edge holds it (nosed in) */
        if (++c->rec_t > (tick - c->wall_t < 4 ? 15 : 60)) {
            c->rec = 1; c->rec_t = 0;
            C.st.recoveries++;
            c->rec_n = (hypot(x - c->rec_x, z - c->rec_z) < 6.0 * RC_M) ? c->rec_n + 1 : 1;
            c->rec_x = x; c->rec_z = z;
        }
    } else if (c->rec == 0) c->rec_t = 0;
    if (c->rec) {
        const double v24 = (double)a->longitudinal_speed, vs = v24 / 256.0 * RC_HZ / RC_M;
        const double k122 = RC_M * 256.0 / (RC_HZ * RC_HZ);
        const double accr = rc_clamp(0.9 * (-3.0 - vs), -3.0, 3.0);
        if (++c->rec_t > 40) { c->rec = 0; c->rec_t = 0; c->plan_t = -100000; }
        a->steering_command = herr > 0 ? -RC_STEER_CLAMP : RC_STEER_CLAMP;
        c->steer = a->steering_command;
        a->encounter_steering_cmd = (int16_t)floor(rc_clamp(v24 / 256.0 + accr * k122, -1024.0, 1024.0) / 4.0 + 0.5);
        a->brake_flag = 0; a->handbrake_flag = 0; a->throttle_state = 0;
        if (c->rec_n >= 3) rc_despawn(c, RC_WHY_STUCK);
        goto status;
    }
    {
        long dd = (long)steer_t - (long)c->steer;
        const long rate = c->state == RC_PURSUIT ? 0x3000 : 0x2400;
        if (dd > rate) dd = rate;
        if (dd < -rate) dd = -rate;
        c->steer = (int32_t)rc_clamp((double)((long)c->steer + dd), -RC_STEER_CLAMP, RC_STEER_CLAMP);
    }

    /* ---- throttle: td5_roam_traffic.c's law (the traffic integrator adds cmd * 4 to the 24.8 speed per tick and
     * drags 1/256 of it, so cmd = (speed / 256 + accel) / 4 holds a speed exactly) ---- */
    acc_hi = c->state == RC_PURSUIT ? 3.6 : 2.2;
    acc_lo = c->state == RC_PURSUIT ? -7.5 : -6.5;
    {
        const double v24 = (double)a->longitudinal_speed;
        const double k122 = RC_M * 256.0 / (RC_HZ * RC_HZ);
        const double vs = v24 / 256.0 * RC_HZ / RC_M;
        double acc = 0.9 * (vt - vs);
        if (a_req < acc && vs > 0.0) acc = a_req;
        acc = rc_clamp(acc, acc_lo, acc_hi);
        if (vt < 0.3 && fabs(vs) < 0.8) { thr = -v24 * (255.0 / 256.0); brake = 1; }
        else { thr = v24 / 256.0 + acc * k122; if (acc < -0.4) brake = 1; }
        thr = rc_clamp(thr, -1024.0, 1024.0);
        a->encounter_steering_cmd = (int16_t)floor(thr / 4.0 + 0.5);
    }
    a->steering_command = c->steer;
    a->brake_flag = (uint8_t)brake;
    a->handbrake_flag = 0;
    a->throttle_state = (uint8_t)(brake ? 1 : 0);

status:
    /* ---- stuck episodes (not held by a lamp, not at the player, not parked on purpose) ---- */
    if (v < 1.0 / 3.6 && !c->hold && !c->sig_hold && c->d_player > 9.0 && vt > 1.0) {
        if (++c->still_t > RC_STUCK_TICKS && !c->stuck_counted) {
            c->stuck_counted = 1;
            C.st.stuck_episodes++;
            TD5_LOG_W(LOG_TAG, "ROAM_COPS: slot %d STUCK > 5 s at place (%.0f,%.0f) %s, %.0f m from the player, path %d pts s %.1f",
                      c->slot, x, z, c->state == RC_PURSUIT ? "pursuing" : "patrolling", c->d_player, c->np, c->s);
        }
        if (c->still_t > 3 * RC_STUCK_TICKS) { C.st.stuck_removed++; rc_despawn(c, RC_WHY_STUCK); }
    } else { c->still_t = 0; c->stuck_counted = 0; }

    /* ---- off the surface: counted; 1 s off -> back on its path (pursuit) or removed (patrol) ---- */
    {
        int inside = 0;
        int32_t gy = 0;
        if (!rc_locate(x, z, a->world_pos.y, 1, &inside, &gy)) { rc_despawn(c, RC_WHY_UNRES); c->fade = -1; c->alpha = 0; return; }
        if (!inside) {
            if (c->off_t == 0 && C.st.off_ticks < 200) {
                double qx = x, qz = z;
                if (c->np >= 2) rc_path_point(c, c->s, 0.0, &qx, &qz, NULL, NULL);
#ifndef TD5RE_RELEASE
                if (C.log_every && !C.offpath_dumped && c->np >= 2 && !rc_inside(qx, qz)) {
                    /* DEV: the path that crosses non-road, every point with the locator's answer */
                    char path[512];
                    const char *dir = td5_plat_log_dir();
                    FILE *f;
                    int i;
                    C.offpath_dumped = 1;
                    snprintf(path, sizeof path, "%sroam_cops_offpath.csv", dir ? dir : "");
                    f = fopen(path, "w");
                    if (f) {
                        const TD5_RoamResident *rs = td5_roam_resident();
                        int32_t ox, oz;
                        td5_roam_world_origin(&ox, &oz);
                        fprintf(f, "i,x,z,v,found,inside,patch,edge,push_dist_m\n");
                        for (i = 0; i < c->np; i++) {
                            TD5_RoamHit h;
                            int fnd = rs && td5_roam_locate(rs, (int32_t)floor((c->px[i] - ox) * 256.0 + 0.5), (int32_t)floor((c->pz[i] - oz) * 256.0 + 0.5),
                                                            0, 0, ROAM_PRIM_NONE, &h) && h.prim != ROAM_PRIM_NONE;
                            fprintf(f, "%d,%.0f,%.0f,%.1f,%d,%d,%d,%016llx,%.2f\n", i, c->px[i], c->pz[i], c->pv[i] * 3.6, fnd, fnd ? h.inside : 0,
                                    fnd ? h.is_patch : 0, fnd ? (unsigned long long)h.edge_key : 0ull, fnd ? h.push_dist / RC_M : 0.0);
                        }
                        fclose(f);
                        TD5_LOG_I(LOG_TAG, "ROAM_COPS: DEV off-road path of slot %d -> %s", c->slot, path);
                    }
                }
#endif
                TD5_LOG_W(LOG_TAG, "ROAM_COPS_OFF slot %d at place (%.0f,%.0f) %s v=%.1f km/h vt=%.1f lat=%.2f m s=%.1f/%.1f path point "
                          "(%.0f,%.0f) %s, steer %d, contact %ld ticks ago | top surface here %s, car y %d ground %d", c->slot, x, z,
                          c->state == RC_PURSUIT ? "pursuit" : "patrol", v * 3.6, c->v_t * 3.6, c->lat, c->s, rc_path_len(c), qx, qz,
                          rc_inside(qx, qz) ? "on the road" : "OFF the road", (int)c->steer, tick - c->last_contact,
                          rc_inside(x, z) ? "ON the road" : "off the road", (int)(a->world_pos.y >> 8), (int)(gy >> 8));
            }
            C.st.off_ticks++;
            if (tick - c->last_contact < 60) C.st.off_ticks_contact++;
            if (++c->off_t > RC_OFF_TICKS) {
                C.st.off_events++;
                c->off_t = 0;
                if (c->state == RC_PURSUIT) {
                    /* back on the nearest street centre, facing along its path (or the car's heading) */
                    double rx, rz, dm, rtx = fx, rtz = fz;
                    uint64_t key;
                    int ok = 0;
                    if (td5_roam_route_snap_xz(x, z, 30.0, &rx, &rz, &key, &dm)) {
                        if (c->np >= 2) { double qx, qz; rc_path_point(c, c->s + 4.0, 0.0, &qx, &qz, &rtx, &rtz); }
                        ok = rc_footprint_ok(rx, rz, rtx, rtz) && rc_clear_of_actors(rx, rz, 5.0, c->slot);
                    }
                    if (ok) {
                        rc_place_actor(c, a, rx, rz, rc_angle_of(rtx, rtz), v * 0.5);
                        c->np = 0; c->plan_t = -100000;
                        C.st.off_rescues++;
                        TD5_LOG_I(LOG_TAG, "ROAM_COPS: slot %d put back on the street %.1f m away", c->slot, dm);
                    } else rc_despawn(c, RC_WHY_OFF);
                } else rc_despawn(c, RC_WHY_OFF);
            }
        } else c->off_t = 0;
    }
}

/* Barriers are walls for cops (racer-like): the 4 body corners against the race's mouth barriers, the push applied
 * to the position and the velocity into the panel removed. */
static void rc_barriers(RcCop *c, TD5_Actor *a)
{
    double fx, fz;
    int q, best = 0;
    int32_t bpx = 0, bpz = 0, bpd = 0;
    if (!td5_roam_race_barriers_live()) return;
    rc_heading(a, &fx, &fz);
    for (q = 0; q < 4; q++) {
        const double l = (q & 1) ? 2.2 : -2.2, w = (q & 2) ? 0.95 : -0.95;
        const int32_t qx = a->world_pos.x + (int32_t)((fx * l + fz * w) * RC_M * 256.0);
        const int32_t qz = a->world_pos.z + (int32_t)((fz * l - fx * w) * RC_M * 256.0);
        int32_t px, pz, pd;
        if (td5_roam_race_wall_probe_cop(qx, qz, a->world_pos.y, &px, &pz, &pd) && pd > bpd) { bpx = px; bpz = pz; bpd = pd; best = 1; }
    }
    if (!best) return;
    {
        const double l = sqrt((double)bpx * bpx + (double)bpz * bpz);
        a->world_pos.x += bpx * 256;
        a->world_pos.z += bpz * 256;
        if (l > 0.0) {
            const double nx = bpx / l, nz = bpz / l;
            const double vn = a->linear_velocity_x * nx + a->linear_velocity_z * nz;
            if (vn < 0.0) { a->linear_velocity_x -= (int32_t)(vn * nx); a->linear_velocity_z -= (int32_t)(vn * nz); }
        }
        C.st.barrier_hits++;
    }
    (void)c;
}

/* The ROAD EDGE is a wall for cops (racer-like; traffic slots skip the roam wall resolver): the 4 body corners against
 * the drivable surface at the car's level, a corner a kerb overhang (0.35 m, the racers' ROAM_KERB_OVERHANG) past the
 * edge pushes the car back along the locator's push vector, ONE push per tick, the velocity into the edge removed.
 * The deepest corner's push, or with corners pushed opposite ways (a turn: the front outer corner and the rear inner
 * one) the VECTOR SUM of every corner's push, so a narrow pinch nets out and a corner still moves the car in. */
static void rc_road_walls(RcCop *c, TD5_Actor *a)
{
    const TD5_RoamResident *rs = td5_roam_resident();
    const double tol = 0.35 * RC_M;
    double fx, fz, bpx = 0.0, bpz = 0.0, bpen = 0.0, ox0 = 0.0, oz0 = 0.0, sx = 0.0, sz = 0.0;
    int q, n = 0, pinch = 0;
    /* probes: 4 body corners and the 2 side midpoints (kerb overhang allowed), the chassis centre (none: a car
     * cutting a convex corner has every corner on the road and its centre over the kerb) */
    static const double k_l[7] = { -2.2, 2.2, -2.2, 2.2, 0.0, 0.0, 0.0 };
    static const double k_w[7] = { -0.95, -0.95, 0.95, 0.95, -0.95, 0.95, 0.0 };
    if (!rs) return;
    rc_heading(a, &fx, &fz);
    for (q = 0; q < 7; q++) {
        const double l = k_l[q], w = k_w[q];
        const int32_t qx = a->world_pos.x + (int32_t)((fx * l + fz * w) * RC_M * 256.0);
        const int32_t qz = a->world_pos.z + (int32_t)((fz * l - fx * w) * RC_M * 256.0);
        TD5_RoamHit h;
        double pd;
        const double tq = q == 6 ? -0.2 * RC_M : tol;       /* the centre: pushed 0.2 m inside the edge */
        if (!td5_roam_locate(rs, qx, qz, a->world_pos.y, 1, ROAM_PRIM_NONE, &h) || h.prim == ROAM_PRIM_NONE || h.inside) continue;
        pd = (double)h.push_dist;
        if (pd <= tq) continue;
        if (n > 0 && (h.push_x * ox0 + h.push_z * oz0) < 0.0) pinch = 1;
        if (n == 0) { ox0 = h.push_x; oz0 = h.push_z; }
        n++;
        {
            const double l2 = sqrt((double)h.push_x * h.push_x + (double)h.push_z * h.push_z);
            const double px = l2 > 0.0 ? h.push_x / l2 * (pd - tq) : 0.0, pz = l2 > 0.0 ? h.push_z / l2 * (pd - tq) : 0.0;
            sx += px; sz += pz;
            if (pd - tq > bpen) { bpen = pd - tq; bpx = px; bpz = pz; }
        }
    }
    if (n == 0 || bpen <= 0.0) return;
    C.st.road_pushes++;
    c->wall_t = rc_tick();
    if (pinch) { bpx = sx; bpz = sz; C.st.road_pinches++; }
    a->world_pos.x += (int32_t)floor(bpx * 256.0 + 0.5);
    a->world_pos.z += (int32_t)floor(bpz * 256.0 + 0.5);
    {
        const double l = sqrt(bpx * bpx + bpz * bpz);
        if (l > 0.0) {
            const double nx = bpx / l, nz = bpz / l;
            const double vn = a->linear_velocity_x * nx + a->linear_velocity_z * nz;
            if (vn < 0.0) { a->linear_velocity_x -= (int32_t)(vn * nx); a->linear_velocity_z -= (int32_t)(vn * nz); }
        }
    }
}

/* ============================================================ roadblocks [G1b] ===== */

/* a roadblock car holds its place: braked, wheels straight (a traffic-type actor, so the V2V contact is the normal one) */
static void rc_block_drive(RcCop *c, TD5_Actor *a, double plx, double plz)
{
    const double v24 = (double)a->longitudinal_speed;
    double x, z;
    rc_place_of(a, &x, &z);
    c->d_player = hypot(plx - x, plz - z) / RC_M;
    if (td5_ai_actor_is_broken_down(c->slot)) {
        if (++c->wreck_t == 1) C.st.wrecks++;
        if (c->wreck_t > 240) rc_despawn(c, RC_WHY_WRECK);
    }
    a->steering_command = 0;
    a->encounter_steering_cmd = (int16_t)floor(rc_clamp(-v24 * (255.0 / 256.0), -1024.0, 1024.0) / 4.0 + 0.5);
    a->brake_flag = 1; a->handbrake_flag = 0;
    a->throttle_state = 1;
}

static int rc_free_count(void)
{
    int i, n = 0;
    for (i = 0; i < C.pool; i++) if (C.cop[i].state == RC_PARKED) n++;
    return n;
}

static int rc_block_count(void)
{
    int i, n = 0;
    for (i = 0; i < RC_BLK_MAX; i++) if (C.blk[i].on) n++;
    return n;
}

/* the carriageway's lateral extent at (cx, cz) across the travel tangent (tx, tz): metres left (<0) / right (>0) of the
 * centre while the locator stays on the drivable surface (a median ends it) */
static int rc_road_extent(double cx, double cz, double tx, double tz, double *lmin, double *lmax)
{
    const double rx = tz, rz = -tx;
    double l;
    if (!rc_inside(cx, cz)) return 0;
    for (l = 0.5; l <= 14.0; l += 0.5) if (!rc_inside(cx + rx * l * RC_M, cz + rz * l * RC_M)) break;
    *lmax = l - 0.5;
    for (l = 0.5; l <= 14.0; l += 0.5) if (!rc_inside(cx - rx * l * RC_M, cz - rz * l * RC_M)) break;
    *lmin = -(l - 0.5);
    return 1;
}

static int rc_side_cars(double len)
{
    return len >= 2.4 ? (int)ceil(len / RC_BLK_CAR_W - 0.15) : 0;
}

/* the cars of a block across [lmin, lmax]: lateral centres + a metre along the road; the gap by heat. Returns the car count */
static int rc_block_layout(int heat, double lmin, double lmax, int ncap, double *lat, double *aoff, double *gap_l, double *gap_w)
{
    const double W = lmax - lmin;
    double gw = heat >= 5 ? 0.0 : (heat >= 4 ? 3.2 : 4.2), gl;
    double a0, a1, b0, b1;
    int n = 0, ka, kb, j;
    if (W < 5.5) return 0;
    if (C.race_on && gw < 3.6) gw = 3.6;
    if (gw > W - 2.4) gw = W - 2.4;
    if (gw < 3.0) gw = 0.0;
    if (gw > 0.0) gl = lmin + (0.15 + 0.7 * rc_unit(&C.rng)) * (W - gw);
    else gl = lmax;
    a0 = lmin; a1 = gl; b0 = gl + gw; b1 = lmax;
    ka = rc_side_cars(a1 - a0);
    kb = rc_side_cars(b1 - b0);
    if (ka + kb > ncap) {            /* too few cars: the left side first, the rest is gap */
        if (ka > ncap) ka = ncap;
        if (kb > ncap - ka) kb = ncap - ka;
    }
    for (j = 0; j < ka && n < RC_BLK_CARS; j++) {
        double c = a0 + (a1 - a0) * (j + 0.5) / ka;
        if (a1 - a0 >= RC_BLK_CAR_W) { if (c < a0 + 2.2) c = a0 + 2.2; if (c > a1 - 2.2) c = a1 - 2.2; }
        lat[n] = c; aoff[n] = 0.0; n++;
    }
    for (j = 0; j < kb && n < RC_BLK_CARS; j++) {
        double c = b0 + (b1 - b0) * (j + 0.5) / kb;
        if (b1 - b0 >= RC_BLK_CAR_W) { if (c < b0 + 2.2) c = b0 + 2.2; if (c > b1 - 2.2) c = b1 - 2.2; }
        lat[n] = c; aoff[n] = 0.0; n++;
    }
    if (n == 1 && ncap >= 2) {       /* a narrow street: the second car parks behind the first, same side */
        lat[1] = lat[0]; aoff[1] = -5.2; n = 2;
    }
    *gap_l = gl; *gap_w = gw;
    return n;
}

/* a point `s` metres along a polyline (PLACE frame, cumulative metres in cum) and its tangent over +-4 m */
static int rc_poly_at(const double *qx, const double *qz, const double *cum, int nq, double s, double *x, double *z, double *tx, double *tz)
{
    double dx, dz, l, p[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
    int q;
    *x = *z = 0.0;
    if (nq < 2 || s < 0.0 || s > cum[nq - 1]) return 0;
    for (q = 0; q < 3; q++) {
        const double ss = q == 0 ? s : (q == 1 ? (s - 4.0 < 0.0 ? 0.0 : s - 4.0) : (s + 4.0 > cum[nq - 1] ? cum[nq - 1] : s + 4.0));
        int i = 0;
        double t, px, pz;
        while (i + 2 < nq && cum[i + 1] < ss) i++;
        l = cum[i + 1] - cum[i];
        t = l > 0.0 ? (ss - cum[i]) / l : 0.0;
        px = qx[i] + (qx[i + 1] - qx[i]) * t; pz = qz[i] + (qz[i + 1] - qz[i]) * t;
        if (q == 0) { *x = px; *z = pz; } else { p[q - 1][0] = px; p[q - 1][1] = pz; }
    }
    dx = p[1][0] - p[0][0]; dz = p[1][1] - p[0][1];
    l = sqrt(dx * dx + dz * dz);
    if (l <= 0.0) return 0;
    *tx = dx / l; *tz = dz / l;
    return 1;
}

/* try to set a roadblock up ahead of the player on the predicted street path; 1 = placed */
static int rc_block_place(const TD5_Actor *pl, double plx, double plz, double fx, double fz)
{
    static const double k_rot[3] = { 0.0, 0.45, -0.45 };
    double hx = fx, hz = fz;
    const double vpl = rc_speed_ms(pl);
    int k, pass, bi;
    if (vpl > 2.0) {
        const double vx = (double)pl->linear_velocity_x, vz = (double)pl->linear_velocity_z, l = sqrt(vx * vx + vz * vz);
        if (l > 0.0) { hx = vx / l; hz = vz / l; }
    }
    for (bi = 0; bi < RC_BLK_MAX && C.blk[bi].on; bi++) {}
    if (bi >= RC_BLK_MAX) return 0;
    for (k = 0; k < 3; k++) {
        const double cr = cos(k_rot[k]), sr = sin(k_rot[k]);
        const double dx = hx * cr - hz * sr, dz = hx * sr + hz * cr;
        double gx = plx + dx * 450.0 * RC_M, gz = plz + dz * 450.0 * RC_M, sx = plx + hx * 6.0 * RC_M, sz = plz + hz * 6.0 * RC_M;
        double *qx = NULL, *qz = NULL, len = 0.0, *cum;
        int nq = 0, i;
        {   /* the goal on a street */
            double ax, az, dm;
            uint64_t key;
            if (td5_roam_route_snap_xz(gx, gz, 150.0, &ax, &az, &key, &dm)) { gx = ax; gz = az; }
        }
        if (!rc_inside(sx, sz)) { sx = plx; sz = plz; }
        if (!td5_roam_route_plan_path(sx, sz, hx, hz, gx, gz, 60.0, rc_use_mask(), 0, &qx, &qz, &nq, &len) || nq < 2) { free(qx); free(qz); continue; }
        cum = (double *)malloc((size_t)nq * sizeof(double));
        if (!cum) { free(qx); free(qz); return 0; }
        cum[0] = 0.0;
        for (i = 1; i < nq; i++) cum[i] = cum[i - 1] + hypot(qx[i] - qx[i - 1], qz[i] - qz[i - 1]) / RC_M;
        if (cum[nq - 1] < C.block_min_m - 20.0) { free(cum); free(qx); free(qz); continue; }
        for (pass = 0; pass < 2; pass++) {
            const double s_lo = C.block_min_m, s_hi = C.block_max_m < cum[nq - 1] - 10.0 ? C.block_max_m : cum[nq - 1] - 10.0;
            const int ncand = 14;
            const double s_start = s_lo + rc_unit(&C.rng) * (s_hi > s_lo ? s_hi - s_lo : 0.0);
            int ic;
            for (ic = 0; ic < ncand && s_hi >= s_lo; ic++) {
                double s = s_start + 20.0 * ic, x, z, tx, tz, lmin, lmax, lat[RC_BLK_CARS], aoff[RC_BLK_CARS], gl, gw, along, dm;
                double px[RC_BLK_CARS], pz[RC_BLK_CARS], dxc[RC_BLK_CARS], dzc[RC_BLK_CARS];
                int e, n, j, ok = 1, nfree;
                TD5_RouteGEdge E;
                if (s > s_hi) s = s_lo + (s - s_hi);
                if (s > s_hi) continue;
                if (!rc_poly_at(qx, qz, cum, nq, s, &x, &z, &tx, &tz)) continue;
                {
                    const double de = hypot(x - plx, z - plz) / RC_M;
                    if (de < C.block_min_m * 0.7 || de > C.block_max_m + 20.0) continue;
                }
                if (pass == 0 && rc_los(plx, plz, x, z, 600.0)) continue;          /* out of sight first */
                if (!td5_roam_route_graph_snap_edge(x, z, 12.0, &e, &along, &dm) || !td5_roam_route_graph_edge_view(e, &E)) continue;
                if ((E.flags & 0x03) || E.klass <= 2 || along < 16.0 * RC_M || along > E.plen - 16.0 * RC_M) continue;
                for (j = 0; j < RC_BLK_MAX; j++)
                    if (C.blk[j].on && hypot(x - C.blk[j].cx, z - C.blk[j].cz) < 120.0 * RC_M) ok = 0;
                if (!ok) continue;
                if (!rc_road_extent(x, z, tx, tz, &lmin, &lmax)) continue;
                nfree = rc_free_count();
                n = rc_block_layout(C.heat, lmin, lmax, nfree < RC_BLK_CARS ? nfree : (C.heat >= 4 ? 3 : 2), lat, aoff, &gl, &gw);
                if (n < 2 || n > nfree) continue;
                {
                    const double rx = tz, rz = -tx;
                    for (j = 0; j < n && ok; j++) {
                        /* the car's long axis across the road, a little off square, alternating */
                        const double jit = (j & 1 ? 1.0 : -1.0) * (0.12 + 0.1 * rc_unit(&C.rng));
                        const double sgn = (j & 1) ? 1.0 : -1.0;
                        const double ux = rx * sgn * cos(jit) + tx * sin(jit), uz = rz * sgn * cos(jit) + tz * sin(jit);
                        px[j] = x + rx * lat[j] * RC_M + tx * aoff[j] * RC_M;
                        pz[j] = z + rz * lat[j] * RC_M + tz * aoff[j] * RC_M;
                        dxc[j] = ux; dzc[j] = uz;
                        if (!rc_footprint_ok(px[j], pz[j], ux, uz) || !rc_clear_of_actors(px[j], pz[j], 3.0, -1)) ok = 0;
                    }
                }
                if (!ok) continue;
                {   /* place it */
                    RcBlock *b = &C.blk[bi];
                    const double rx = tz, rz = -tx;
                    int placed = 0;
                    memset(b, 0, sizeof *b);
                    for (j = 0; j < n; j++) {
                        RcCop *c = rc_free_cop();
                        if (!c || !rc_spawn_at(c, px[j], pz[j], dxc[j], dzc[j], RC_BLOCK, 1)) continue;
                        c->blk = bi + 1;
                        b->slot[placed++] = c->slot;
                    }
                    if (placed < 2) {     /* a slot vanished under us: take it back */
                        for (j = 0; j < placed; j++) { RcCop *c = rc_cop_of(b->slot[j]); if (c) rc_despawn(c, RC_WHY_BLOCK); }
                        memset(b, 0, sizeof *b);
                        continue;
                    }
                    b->on = 1; b->ncar = placed; b->heat = C.heat;
                    b->cx = x; b->cz = z; b->tx = tx; b->tz = tz;
                    b->gap_w = gw;
                    b->gx = x + rx * (gl + gw * 0.5) * RC_M; b->gz = z + rz * (gl + gw * 0.5) * RC_M;
                    b->t0 = rc_tick();
                    b->min_d = hypot(x - plx, z - plz) / RC_M;
                    C.st.blocks_placed++; C.st.block_cars += placed;
                    TD5_LOG_I(LOG_TAG, "ROAM_COPS: ROADBLOCK %ld placed (heat %d): %d cars across %.1f m of carriageway (%.1f..%.1f) at place "
                              "(%.0f,%.0f), %.0f m from the player, %.0f m along the predicted path (try %d, %s), gap %.1f m",
                              C.st.blocks_placed, C.heat, placed, lmax - lmin, lmin, lmax, x, z, b->min_d, s, k, pass ? "in sight" : "out of sight", gw);
                    rc_toast("ROADBLOCK AHEAD", 0xFFFFA040, 2.5);
                    free(cum); free(qx); free(qz);
                    return 1;
                }
            }
        }
        free(cum); free(qx); free(qz);
    }
    return 0;
}

static void rc_block_end(RcBlock *b, const char *why)
{
    int i;
    for (i = 0; i < b->ncar; i++) {
        RcCop *c = rc_cop_of(b->slot[i]);
        if (c && c->state == RC_BLOCK && c->blk) rc_despawn(c, RC_WHY_BLOCK);
    }
    TD5_LOG_I(LOG_TAG, "ROAM_COPS: roadblock at place (%.0f,%.0f) removed (%s) after %.1f s, closest %.0f m, %s", b->cx, b->cz, why,
              (rc_tick() - b->t0) / RC_HZ, b->min_d, b->crossed ? (b->contact ? "crossed through the cars" : "crossed through the gap") : "not crossed");
    b->on = 0;
    C.st.blocks_despawned++;
}

/* per sim tick after the pursuit rules: place, track and remove the roadblocks */
static void rc_blocks_tick(const TD5_Actor *pl, double plx, double plz, double fx, double fz, int in_race)
{
    const long tick = rc_tick();
    int i, active = 0;
    if (C.block_heat <= 0) return;
    for (i = 0; i < RC_BLK_MAX; i++) {
        RcBlock *b = &C.blk[i];
        double d, f, l, rx, rz;
        int alive = 0, j;
        if (!b->on) continue;
        for (j = 0; j < b->ncar; j++) { const RcCop *c = rc_cop_of(b->slot[j]); if (c && c->state == RC_BLOCK && c->fade >= 0) alive = 1; }
        if (!alive) { b->on = 0; C.st.blocks_despawned++; continue; }
        if (!C.pursuit) { rc_block_end(b, "pursuit over"); continue; }
        d = hypot(plx - b->cx, plz - b->cz) / RC_M;
        if (d < b->min_d) b->min_d = d;
        f = ((plx - b->cx) * b->tx + (plz - b->cz) * b->tz) / RC_M;
        rx = b->tz; rz = -b->tx;
        l = ((plx - b->cx) * rx + (plz - b->cz) * rz) / RC_M;
        if (d < 40.0) b->was_near = 1;
        for (j = 0; j < b->ncar; j++) {       /* touched a car */
            const RcCop *c = rc_cop_of(b->slot[j]);
            if (c && c->state == RC_BLOCK && c->d_player < 3.4) {
                if (!b->contact) TD5_LOG_I(LOG_TAG, "ROAM_COPS: player touches a roadblock car (slot %d) at %.0f km/h", c->slot, rc_speed_ms(pl) * 3.6);
                b->contact = 1; C.st.block_hits++;
                break;
            }
        }
        if (b->have_prev && b->prev_f < 0.0 && f >= 0.0 && fabs(l) < 20.0 && !b->crossed) {
            b->crossed = 1;
            if (b->contact) C.st.blocks_rammed++; else C.st.blocks_gap++;
            TD5_LOG_I(LOG_TAG, "ROAM_COPS: player crossed the roadblock line at lateral %.1f m, %.0f km/h: %s", l, rc_speed_ms(pl) * 3.6,
                      b->contact ? "RAMMED THROUGH" : "through the gap");
        }
        b->prev_f = f; b->have_prev = 1;
        if (b->crossed && f > 35.0) { rc_block_end(b, "passed"); continue; }
        if (!b->crossed && C.busted_t <= 0 && d > b->min_d + 80.0 && !b->was_near) {
            C.st.blocks_detoured++; rc_block_end(b, "player turned away"); continue;
        }
        if (d > 560.0) { rc_block_end(b, "out of range"); continue; }
        active++;
    }
    if (!C.pursuit || C.busted_t > 0 || C.heat < C.block_heat || (in_race && !C.race_on)) { C.st.blocks_active = active; return; }
    if (active < C.block_max && tick >= C.block_cd) {
        if (rc_free_count() < 2) {
            /* the pool is out: the farthest patrols give up their cars (they fade out, the next ticks retry) */
            int k, need = 2 - rc_free_count();
            for (k = 0; k < need; k++) {
                RcCop *farc = NULL;
                int q;
                for (q = 0; q < C.pool; q++) {
                    RcCop *c = &C.cop[q];
                    if (c->state != RC_PATROL || c->fade < 0 || c->d_player < 60.0) continue;
                    if (!farc || c->d_player > farc->d_player) farc = c;
                }
                if (farc) rc_despawn(farc, RC_WHY_LEVEL);
            }
            C.block_cd = tick + 15;
        } else if (rc_block_place(pl, plx, plz, fx, fz)) {
            C.block_cd = tick + 20 * 30;
            C.block_fail_run = 0;
            active++;
        } else {
            C.st.block_fails++;
            C.block_fail_run++;
            C.block_cd = tick + 45;
        }
    }
    C.st.blocks_active = active;
}

/* pool cars a pending roadblock holds back from the reinforcements (none once placing keeps failing) */
static int rc_block_reserve(void)
{
    if (C.block_heat <= 0 || C.heat < C.block_heat || C.block_fail_run >= 4 || rc_block_count() >= C.block_max) return 0;
    return 2;
}

/* ============================================================ the player's offences */

/* 1 when the player crossed a stop line on red this tick (the lamp its arm obeys) */
static int rc_player_red(const TD5_Actor *pl, double x, double z)
{
    TD5_RouteGEdge E;
    double fx, fz, rem;
    int e, d, g, node = -1;
    const double v = rc_speed_ms(pl);
    rc_heading(pl, &fx, &fz);
    if (!rc_on_graph(x, z, fx, fz, &e, &d, &rem, &E)) { C.pl_e = -1; return 0; }
    if (e != C.pl_e || d != C.pl_d) { C.pl_e = e; C.pl_d = d; C.pl_rem = rem; return 0; }
    {
        const double prev = C.pl_rem;
        C.pl_rem = rem;
        if (!(prev > 1.0 && rem <= 1.0) || v < 3.0) return 0;
    }
    g = rc_sig_group(&E, d, &node);
    if (g < 0 || node == C.pl_red_node) return 0;
    if (td5_geo_signal_phase_at((double)rc_tick() / RC_HZ, g) != 0) return 0;
    C.pl_red_node = node;
    C.st.red_runs++;
    TD5_LOG_I(LOG_TAG, "ROAM_COPS: the player ran a red at node %d (group %d) at %.0f km/h", node, g, v * 3.6);
    return 1;
}

/* the speeding limit for the street under the player, km/h */
static double rc_limit_here(double x, double z)
{
    int e;
    double along, dm;
    TD5_RouteGEdge E;
    double lim = C.limit_kmh;
    if (td5_roam_route_graph_snap_edge(x, z, 15.0, &e, &along, &dm) && td5_roam_route_graph_edge_view(e, &E)) {
        const double k = rc_class_kmh(E.klass) * 1.5;
        if (k > lim) lim = k;
    }
    return lim;
}

/* ============================================================ pursuit, escape, bust */

static int rc_count(int state)
{
    int i, n = 0;
    for (i = 0; i < C.pool; i++) if (C.cop[i].state == state && C.cop[i].fade >= 0) n++;
    return n;
}

static void rc_assign_roles(void)
{
    int i, k = 0;
    for (i = 0; i < C.pool; i++)
        if (C.cop[i].state == RC_PURSUIT) C.cop[i].role = (k++) % 3;
}

static void rc_join(RcCop *c)
{
    c->state = RC_PURSUIT;
    c->hold = 0;
    c->np = 0;
    c->plan_t = -100000;
    rc_assign_roles();
}

static const char *const k_cause[4] = { "", "SPEEDING", "RAN A RED LIGHT", "RAMMED A POLICE CAR" };

static void rc_start_pursuit(RcCop *by, int cause, double kmh)
{
    char msg[96];
    C.pursuit = 1;
    C.heat = 1;
    C.offences = 1;
    C.cause = cause;
    C.p_t0 = rc_tick();
    C.lost_t = 0;
    C.bust_meter = 0.0;
    C.spawn_cd = rc_tick() + 60;
    C.st.pursuits++;
    if (cause == 1) C.st.notice_speed++;
    else if (cause == 2) C.st.notice_red++;
    else C.st.notice_ram++;
    rc_join(by);
    if (cause == 1) snprintf(msg, sizeof msg, "POLICE PURSUIT: SPEEDING %.0f KM/H", kmh);
    else snprintf(msg, sizeof msg, "POLICE PURSUIT: %s", k_cause[cause]);
    rc_toast(msg, 0xFFFF4040, 3.0);
    TD5_LOG_I(LOG_TAG, "ROAM_COPS: PURSUIT %ld starts: slot %d, %s (%.0f km/h), %.0f m from the player", C.st.pursuits, by->slot,
              k_cause[cause], kmh, by->d_player);
}

static void rc_offence(int cause)
{
    C.offences++;
    TD5_LOG_I(LOG_TAG, "ROAM_COPS: offence in the pursuit: %s (offences %d)", k_cause[cause], C.offences);
}

static void rc_end_pursuit(int escaped, int dist)
{
    int i;
    C.pursuit = 0;
    C.heat = 0;
    C.bust_meter = 0.0;
    for (i = 0; i < C.pool; i++) {
        RcCop *c = &C.cop[i];
        if (c->state != RC_PURSUIT) continue;
        c->state = RC_PATROL;
        c->np = 0;
        c->role = 0;
    }
    if (escaped) {
        C.st.escapes++;
        if (dist) C.st.escapes_dist++; else C.st.escapes_los++;
        C.grace_until = rc_tick() + 5 * 30;
        rc_toast(dist ? "ESCAPED: THE COPS ARE FAR BEHIND" : "ESCAPED: THE COPS LOST SIGHT OF YOU", 0xFF50E070, 3.0);
        TD5_LOG_I(LOG_TAG, "ROAM_COPS: ESCAPE %ld (%s) after %.1f s", C.st.escapes, dist ? "distance" : "line of sight",
                  (rc_tick() - C.p_t0) / RC_HZ);
    }
}

static void rc_bust_release(TD5_Actor *pl, double plx, double plz)
{
    int i;
    char msg[96];
    const int race = C.race_on;
    for (i = 0; i < C.pool; i++) {
        RcCop *c = &C.cop[i];
        if (c->state != RC_PURSUIT) continue;
        if (race) { c->state = RC_PATROL; c->np = 0; }
        else rc_despawn(c, RC_WHY_BUST);
    }
    C.pursuit = 0;
    C.heat = 0;
    C.bust_meter = 0.0;
    if (race) {
        C.grace_until = rc_tick() + 15 * 30;
        rc_toast("RELEASED: BACK IN THE RACE", 0xFFFFE070, 2.5);
        return;
    }
    C.grace_until = rc_tick() + 10 * 30;
    snprintf(msg, sizeof msg, "BACK ON THE STREET   %ld BUST%s", C.st.busts, C.st.busts == 1 ? "" : "S");
    rc_toast(msg, 0xFFFFE070, 3.5);
    {   /* back on the nearest street centre, facing along it the way the car pointed */
        double sx, sz, dm, fx, fz;
        uint64_t key;
        int e, d, on;
        double rem;
        TD5_RouteGEdge E;
        rc_heading(pl, &fx, &fz);
        if (td5_roam_route_snap_xz(plx, plz, 80.0, &sx, &sz, &key, &dm) && rc_on_graph(sx, sz, fx, fz, &e, &d, &rem, &E)) {
            double along, dm2, ux, uz, l;
            int lo = 0, hi;
            td5_roam_route_graph_snap_edge(sx, sz, 5.0, &e, &along, &dm2);
            hi = E.npt - 2;
            while (lo < hi) { const int mid = (lo + hi + 1) / 2; if (E.cum[mid] <= along) lo = mid; else hi = mid - 1; }
            ux = (double)E.pt[2 * lo + 2] - E.pt[2 * lo]; uz = (double)E.pt[2 * lo + 3] - E.pt[2 * lo + 1];
            l = sqrt(ux * ux + uz * uz);
            if (l > 0.0) { ux /= l; uz /= l; }
            if (d) { ux = -ux; uz = -uz; }
            td5_roam_place_actor(0, pl, (int32_t)floor(sx + 0.5), (int32_t)floor(sz + 0.5), rc_angle_of(ux, uz));
            C.st.player_respawns++;
            on = rc_inside(sx, sz);
            C.st.respawn_on_road = on;
            TD5_LOG_I(LOG_TAG, "ROAM_COPS: player put back on the street at place (%.0f,%.0f) (%.1f m from the bust spot, %s)", sx, sz,
                      dm, on ? "on the road" : "OFF THE ROAD");
        } else {
            C.st.respawn_on_road = rc_inside(plx, plz);
            TD5_LOG_W(LOG_TAG, "ROAM_COPS: no street within 80 m of the bust spot: the car stays");
        }
    }
}

/* the pursuit's per-tick rules: heat, reinforcements, joins, escape, bust */
static void rc_pursuit_tick(TD5_Actor *pl, double plx, double plz)
{
    const long tick = rc_tick();
    const double vpl = rc_speed_ms(pl);
    double dmin = 1e300, bdmin = 1e300, fx, fz;
    int i, seen = 0, npur = 0, wanted;
    rc_heading(pl, &fx, &fz);
    C.st.pursuit_ticks++;
    for (i = 0; i < C.pool; i++) {
        const RcCop *c = &C.cop[i];
        if (c->state == RC_BLOCK && c->fade >= 0 && c->alpha >= 255 && c->d_player < bdmin) bdmin = c->d_player;   /* [G1b] */
        if (c->state != RC_PURSUIT || c->fade < 0) continue;
        npur++;
        if (c->d_player < dmin) dmin = c->d_player;
        if (c->sees && c->d_player < C.see_m) seen = 1;
    }
    /* heat: +1 per further offence, +1 per 25 s */
    {
        int h = C.offences + (int)((tick - C.p_t0) / (25 * 30));
        if (h > 5) h = 5;
        if (h > C.heat) {
            C.heat = h;
            TD5_LOG_I(LOG_TAG, "ROAM_COPS: heat %d", C.heat);
        }
        if (C.heat > C.st.max_heat) C.st.max_heat = C.heat;
    }
    if (npur > C.st.max_pursuers) C.st.max_pursuers = npur;
#ifndef TD5RE_RELEASE
    if (C.log_every && (tick - C.p_t0) % (C.log_every * 30L) == 0) {
        char ln[400];
        size_t q = 0;
        for (i = 0; i < C.pool && q < sizeof ln - 60; i++) {
            const RcCop *c = &C.cop[i];
            const TD5_Actor *ca = td5_game_get_actor(c->slot);
            if (c->state != RC_PURSUIT || !ca) continue;
            q += (size_t)snprintf(ln + q, sizeof ln - q, " [%d %.0fm %.0fkmh vt%.0f %s s%.0f/%.0f r%d]", c->slot, c->d_player, rc_speed_ms(ca) * 3.6,
                                  c->v_t * 3.6, c->sees ? "sees" : "-", c->s, rc_path_len(c), c->role);
        }
        TD5_LOG_I(LOG_TAG, "ROAM_COPS_P t=%.0f s heat %d player %.0f km/h meter %.2f lost %.1f s:%s", (tick - C.p_t0) / RC_HZ, C.heat,
                  vpl * 3.6, C.bust_meter, C.lost_t / RC_HZ, ln);
    }
#endif
    /* bust: the player (nearly) stopped with a pursuer within 10 m. A boxing cop nudges the car, so "stopped" is
     * under 10 km/h, and the meter drains at the rate it fills */
    if (vpl < 10.0 / 3.6 && (dmin < 10.0 || bdmin < 10.0)) C.bust_meter += 1.0 / (C.bust_s * RC_HZ);
    else C.bust_meter -= 1.0 / (C.bust_s * RC_HZ);
    if (C.bust_meter < 0.0) C.bust_meter = 0.0;
    if (C.bust_meter >= 1.0) {
        C.bust_meter = 1.0;
        C.busted_t = 90;
        C.st.busts++;
        if (bdmin < 12.0) C.st.blocks_bust++;      /* [G1b] busted with a roadblock car within 12 m (centre to centre) */
        rc_toast("BUSTED", 0xFFFF3B3B, 3.0);
        TD5_LOG_I(LOG_TAG, "ROAM_COPS: BUST %ld after %.1f s of pursuit, heat %d, %d pursuer(s), nearest %.1f m%s", C.st.busts,
                  (tick - C.p_t0) / RC_HZ, C.heat, npur, dmin, C.race_on ? " (race: held 3 s)" : " (free roam: back on the street)");
        return;
    }
    /* escape */
    if (seen) C.lost_t = 0; else C.lost_t++;
    if (npur == 0 || dmin > C.escape_m) { rc_end_pursuit(1, 1); return; }
    if (C.lost_t > (long)((C.lose_s + 2.0 * C.heat) * RC_HZ)) { rc_end_pursuit(1, 0); return; }
    /* reinforcements: 1 + heat pursuers; patrols within 200 m join from heat 2 */
    wanted = 1 + C.heat;
    if (C.heat >= 2)
        for (i = 0; i < C.pool && npur < wanted; i++) {
            RcCop *c = &C.cop[i];
            if (c->state == RC_PATROL && c->fade >= 0 && c->alpha > 0 && c->d_player < 200.0) {
                rc_join(c); npur++; C.st.joins++;
                TD5_LOG_I(LOG_TAG, "ROAM_COPS: slot %d joins the pursuit (%.0f m away)", c->slot, c->d_player);
            }
        }
    if (npur < wanted && tick >= C.spawn_cd && rc_free_count() > rc_block_reserve()) {
        C.spawn_cd = tick + 90;
        if (rc_try_spawn(plx, plz, fx, fz, 1, RC_PURSUIT) || rc_try_spawn(plx, plz, fx, fz, 0, RC_PURSUIT)) {
            C.st.reinforcements++;
            rc_assign_roles();
            TD5_LOG_I(LOG_TAG, "ROAM_COPS: reinforcement %ld (heat %d, %d pursuer(s) now)", C.st.reinforcements, C.heat, npur + 1);
        }
    }
}

/* ============================================================ tick ================ */

void td5_roam_cops_tick(void)
{
    const long tick = rc_tick();
    TD5_Actor *pl;
    double plx = 0.0, plz = 0.0, fx = 0.0, fz = 1.0, vpl = 0.0;
    int i, in_race = 0, per, live = 0, red = 0, ram = -1;
    if (!C.on || !td5_roam_active()) return;
    if (tick == C.tick_last) return;
    C.tick_last = tick;
    if (!td5_roam_route_graph_ready()) return;         /* td5_roam_traffic_tick starts the async build */
    if (!C.ready) {
        C.ready = 1;
        C.gen = td5_roam_route_graph_gen();
        TD5_LOG_I(LOG_TAG, "ROAM_COPS: planner graph ready: %d edges", td5_roam_route_graph_nedge());
    }
    rc_regraph();              /* [F9b] a planner graph swap renumbered the edges */
    pl = td5_game_get_actor(0);
    if (!pl) return;
    rc_place_of(pl, &plx, &plz);
    rc_heading(pl, &fx, &fz);
    vpl = rc_speed_ms(pl);
    per = rc_race_level(&in_race);
    {
        const int was = C.race_on;
        rc_corridor_update(in_race);
        if (was != C.race_on || (in_race && !C.race_on && C.pursuit)) {
            /* a race with COPS on starts or ends (or one with COPS off starts): every cop goes, the corridor's own spawn */
            if (C.pursuit) rc_end_pursuit(0, 0);
            for (i = 0; i < C.pool; i++) rc_despawn(&C.cop[i], RC_WHY_LEVEL);
            C.busted_t = 0;
            TD5_LOG_I(LOG_TAG, "ROAM_COPS: %s: cops cleared", C.race_on ? "race with COPS on" : (in_race ? "race with COPS off" : "race over"));
        }
    }
    if (in_race && td5_roam_race_state() == TD5_RRACE_FINISHED && C.pursuit) rc_end_pursuit(0, 0);

    /* the BUSTED hold */
    if (C.busted_t > 0) {
        if (--C.busted_t == 0) rc_bust_release(pl, plx, plz);
    }

    /* the player's red lamps (every tick: the stop-line crossing is a one-tick event) */
    red = rc_player_red(pl, plx, plz);

    /* drive every live cop; far ones fade out */
    for (i = 0; i < C.pool; i++) {
        RcCop *c = &C.cop[i];
        TD5_Actor *a;
        if (c->state == RC_PARKED) continue;
        a = td5_game_get_actor(c->slot);
        if (!a) { rc_park(c); continue; }
        if (c->state == RC_BLOCK) rc_block_drive(c, a, plx, plz);       /* [G1b] */
        else rc_drive(c, a, pl, plx, plz);
        if (c->state != RC_PARKED) { rc_road_walls(c, a); rc_barriers(c, a); }
        if (c->state != RC_BLOCK && c->d_player > (c->state == RC_PURSUIT ? C.r_despawn_pursuit : C.r_despawn)) rc_despawn(c, RC_WHY_FAR);
        if (per == 0 && !C.pursuit && c->fade >= 0 && c->state == RC_PATROL && !c->hold) rc_despawn(c, RC_WHY_LEVEL);
        /* a ram: the player into a cop, closing faster than 2.5 m/s */
        if (c->d_player < 4.6 && c->alpha >= 255) {
            const double rvx = (double)(pl->linear_velocity_x - a->linear_velocity_x), rvz = (double)(pl->linear_velocity_z - a->linear_velocity_z);
            double cx, cz;
            rc_place_of(a, &cx, &cz);
            {
                /* the PLAYER drives into the cop: closing faster than 2.5 m/s, and the player's own speed towards the
                 * cop is the larger share (a cop boxing the car in is not a ram by the player) */
                const double dxp = cx - plx, dzp = cz - plz, dl2 = sqrt(dxp * dxp + dzp * dzp);
                const double closing = dl2 > 0.0 ? (rvx * dxp + rvz * dzp) / dl2 / 256.0 * RC_HZ / RC_M : 0.0;
                const double v_pl = dl2 > 0.0 ? ((double)pl->linear_velocity_x * dxp + (double)pl->linear_velocity_z * dzp) / dl2 / 256.0 * RC_HZ / RC_M : 0.0;
                const double v_cop = dl2 > 0.0 ? -((double)a->linear_velocity_x * dxp + (double)a->linear_velocity_z * dzp) / dl2 / 256.0 * RC_HZ / RC_M : 0.0;
                if (closing > 2.5 && v_pl > v_cop) ram = i;
            }
        }
        if (c->fade > 0) { c->alpha += 255 / RC_FADE_TICKS; if (c->alpha >= 255) { c->alpha = 255; c->fade = 0; } }
        else if (c->fade < 0) { c->alpha -= 255 / RC_FADE_TICKS; if (c->alpha <= 0) rc_park(c); }
        if (c->state != RC_PARKED) live++;
    }

    /* notice + pursuit (not in the BUSTED hold, not in the grace after a bust / escape) */
    if (C.busted_t <= 0 && tick >= C.grace_until && td5_roam_race_state() != TD5_RRACE_FINISHED &&
        !(in_race && td5_roam_race_state() == TD5_RRACE_COUNTDOWN)) {
        const double kmh = vpl * 3.6;
        const int speeding = kmh > rc_limit_here(plx, plz);
        if (!C.pursuit) {
            RcCop *by = NULL;
            int cause = 0;
            for (i = 0; i < C.pool && !by; i++) {
                RcCop *c = &C.cop[i];
                if (c->state != RC_PATROL || c->fade < 0 || c->alpha < 128) continue;
                if (ram == i) { by = c; cause = 3; }
                else if (c->sees && red && c->d_player < C.notice_red_m) { by = c; cause = 2; }
                else if (c->sees && speeding && c->d_player < C.notice_m) { by = c; cause = 1; }
            }
            if (by) rc_start_pursuit(by, cause, kmh);
        } else {
            int seen_by = 0;
            for (i = 0; i < C.pool; i++) if (C.cop[i].state == RC_PURSUIT && C.cop[i].sees) seen_by = 1;
            if (ram >= 0 && C.cop[ram].state == RC_PURSUIT && tick - C.last_offence > 60) { rc_offence(3); C.last_offence = tick; }
            else if (red && seen_by) { rc_offence(2); C.last_offence = tick; }
            rc_pursuit_tick(pl, plx, plz);
        }
    } else if (C.pursuit && C.busted_t <= 0) {
        rc_pursuit_tick(pl, plx, plz);
    }

#ifndef TD5RE_RELEASE
    if (C.dev_race_pursuit == 1 && C.race_on && !C.pursuit && td5_roam_race_state() == TD5_RRACE_RACING && tick >= C.grace_until) {
        /* DEV (G1b): the race-with-COPS-on check of the roadblock's gap rule: a patrol cop 'notices' the player, heat 5 */
        if (rc_count(RC_PATROL) == 0 && rc_try_spawn(plx, plz, fx, fz, 0, RC_PATROL)) live++;   /* the selftest rows force 0 patrols */
        for (i = 0; i < C.pool; i++) {
            RcCop *c = &C.cop[i];
            if (c->state == RC_PATROL && c->fade >= 0) {
                C.dev_race_pursuit = 2;
                rc_start_pursuit(c, 1, vpl * 3.6);
                C.offences = 5; C.heat = 5; C.st.max_heat = 5;
                TD5_LOG_I(LOG_TAG, "ROAM_COPS: DEV race pursuit at heat 5 started by slot %d", c->slot);
                break;
            }
        }
    }
#endif
    rc_blocks_tick(pl, plx, plz, fx, fz, in_race);           /* [G1b] */

    /* spawn patrols round the player: one try per tick */
    {
        const int npat = rc_count(RC_PATROL);
        if (per > 0 && npat < per && live < C.pool)
            if (rc_try_spawn(plx, plz, fx, fz, 0, RC_PATROL)) live++;
    }
    C.st.live = live;
    C.st.patrol = rc_count(RC_PATROL);
    C.st.pursuing = rc_count(RC_PURSUIT);
}

/* ============================================================ answers ============= */

int td5_roam_cops_glow(int slot)
{
    const RcCop *c = rc_cop_of(slot);
    if (!c || (c->state != RC_PURSUIT && c->state != RC_BLOCK) || c->alpha <= 0) return 0;   /* [G1b] block cars: lights on, no siren */
    return RC_GLOW;
}

/* [G1a] live cops for the roam minimap: place-frame position, pursuing or patrolling. Read-only. */
int td5_roam_cops_marks(TD5_RoamCopMark *out, int max)
{
    int i, n = 0;
    int32_t ox, oz;
    if (!out || max <= 0 || !C.on || !td5_roam_active()) return 0;
    td5_roam_world_origin(&ox, &oz);
    for (i = 0; i < C.pool && n < max; i++) {
        const RcCop *c = &C.cop[i];
        const TD5_Actor *a;
        if (c->state == RC_PARKED || c->alpha <= 0) continue;
        a = td5_game_get_actor(c->slot);
        if (!a) continue;
        out[n].px = (double)FP_TRUNC(a->world_pos.x) + (double)ox;
        out[n].pz = (double)FP_TRUNC(a->world_pos.z) + (double)oz;
        out[n].pursuing = c->state == RC_PURSUIT;
        n++;
    }
    return n;
}

int td5_roam_cops_siren_slot(int32_t wx, int32_t wz)
{
    int i, best = -1;
    double bd = 1e300;
    if (!C.on || !td5_roam_active()) return -1;
    for (i = 0; i < C.pool; i++) {
        const RcCop *c = &C.cop[i];
        const TD5_Actor *a;
        double d;
        if (c->state != RC_PURSUIT || c->alpha <= 0) continue;
        a = td5_game_get_actor(c->slot);
        if (!a) continue;
        d = hypot((double)(a->world_pos.x - wx), (double)(a->world_pos.z - wz));
        if (d < bd) { bd = d; best = c->slot; }
    }
    return best;
}

/* [roam G3] the nearest `max` pursuers (lit, visible), nearest first; returns how many */
int td5_roam_cops_siren_slots(int32_t wx, int32_t wz, int max, int *out)
{
    double bd[8];
    int i, j, n = 0;
    if (!C.on || !td5_roam_active() || max <= 0 || max > 8 || !out) return 0;
    for (i = 0; i < C.pool; i++) {
        const RcCop *c = &C.cop[i];
        const TD5_Actor *a;
        double d;
        if (c->state != RC_PURSUIT || c->alpha <= 0) continue;
        a = td5_game_get_actor(c->slot);
        if (!a) continue;
        d = hypot((double)(a->world_pos.x - wx), (double)(a->world_pos.z - wz));
        if (n == max && d >= bd[n - 1]) continue;
        j = (n < max) ? n++ : n - 1;
        for (; j > 0 && bd[j - 1] > d; j--) { bd[j] = bd[j - 1]; out[j] = out[j - 1]; }
        bd[j] = d; out[j] = c->slot;
    }
    return n;
}

uint32_t td5_roam_cops_filter_bits(int slot, uint32_t bits)
{
    const TD5_Actor *a;
    if (slot != 0 || !C.on || C.busted_t <= 0 || !td5_roam_active()) return bits;
    /* brake to a stop, then the handbrake (a brake held at a standstill engages reverse) */
    a = td5_game_get_actor(0);
    bits &= ~(uint32_t)(TD5_INPUT_THROTTLE | TD5_INPUT_RECOVER | TD5_INPUT_BRAKE);
    return bits | (uint32_t)((a && rc_speed_ms(a) > 1.0) ? TD5_INPUT_BRAKE : TD5_INPUT_HANDBRAKE);
}

int td5_roam_cops_heat(void) { return C.pursuit ? C.heat : 0; }

void td5_roam_cops_stats(TD5_RoamCopsStats *out)
{
    if (out) *out = C.st;
}

/* ============================================================ HUD ================= */

/* [round H HH] The drawing itself is td5_police_hud_draw (td5_police.c), shared with the normal races' pursuit panel. */
void td5_roam_cops_draw_hud(void)
{
    const float W = g_render_width_f, H = g_render_height_f;
    const long tick = rc_tick();
    char line[96];
    TD5_PoliceHud h;
    if (!C.on || !td5_roam_active() || W < 2.0f) return;
    if (td5_roam_route_ui_map_open() || td5_game_is_pause_menu_active()) return;
    if (!C.pursuit && C.busted_t <= 0 && tick >= C.toast_until) return;
    memset(&h, 0, sizeof h);
    h.tick = tick;
    h.pursuit = C.pursuit;
    h.heat = C.heat;
    h.cops = rc_count(RC_PURSUIT);
    h.evade = (float)rc_clamp((double)C.lost_t / ((C.lose_s + 2.0 * C.heat) * RC_HZ), 0.0, 1.0);
    h.bust = (float)C.bust_meter;
    h.busted = C.busted_t > 0;
    if (h.busted) {
        /* no fines and no money anywhere (Mariano, 2026-10-19) */
        if (C.race_on) snprintf(line, sizeof line, "HELD BY THE POLICE   %d", (int)(C.busted_t / 30) + 1);
        else snprintf(line, sizeof line, "HEAT %d   %d OFFENCE%s", C.heat, C.offences, C.offences == 1 ? "" : "S");
        h.busted_line = line;
    }
    if (tick < C.toast_until && C.toast[0]) { h.toast = C.toast; h.toast_col = C.toast_col; }
    td5_police_hud_draw(&h, 0.0f, 0.0f, W, H);
}

/* ============================================================ DEV ================= */

#ifndef TD5RE_RELEASE
int td5_roam_cops_dev_place(double px, double pz, int yaw12, int hold)
{
    RcCop *c = rc_free_cop();
    const double y = (double)(yaw12 & 0xFFF) * (2.0 * RC_PI / 4096.0);
    if (!C.on || !c) return -1;
    if (!rc_spawn_at(c, px, pz, sin(y), cos(y), RC_PATROL, hold)) return -1;
    c->alpha = 255; c->fade = 0;
    TD5_LOG_I(LOG_TAG, "ROAM_COPS: DEV cop slot %d placed at place (%.0f,%.0f) yaw %d%s", c->slot, px, pz, yaw12 & 0xFFF,
              hold ? " (parked)" : "");
    return c->slot;
}

void td5_roam_cops_dev_force(int patrol_per_anchor, int vmax_kmh)
{
    C.force_dens = patrol_per_anchor < 0 ? 0 : patrol_per_anchor + 1;
    C.force_vmax = vmax_kmh < 0 ? 0 : vmax_kmh + 1;
}

int td5_roam_cops_dev_busted(void) { return C.busted_t > 0; }

/* [G1b] */
void td5_roam_cops_dev_set_heat(int h)
{
    if (!C.pursuit || h < 1) return;
    if (h > 5) h = 5;
    if (C.offences < h) C.offences = h;
    if (C.heat < h) { C.heat = h; if (C.heat > C.st.max_heat) C.st.max_heat = C.heat; }
}

int td5_roam_cops_dev_block(int i, double *cx, double *cz, double *tx, double *tz, double *gx, double *gz, double *gap_w, int *ncar)
{
    const RcBlock *b;
    if (i < 0 || i >= RC_BLK_MAX || !C.blk[i].on) return 0;
    b = &C.blk[i];
    *cx = b->cx; *cz = b->cz; *tx = b->tx; *tz = b->tz; *gx = b->gx; *gz = b->gz; *gap_w = b->gap_w; *ncar = b->ncar;
    return 1;
}

int td5_roam_cops_dev_pursuer_behind(double min_m, double max_m)
{
    const TD5_Actor *pl = td5_game_get_actor(0);
    double plx, plz, fx, fz;
    int i;
    if (!pl || !C.on) return 0;
    rc_place_of(pl, &plx, &plz);
    rc_heading(pl, &fx, &fz);
    for (i = 0; i < C.pool; i++) {
        const RcCop *c = &C.cop[i];
        const TD5_Actor *a;
        double x, z, f;
        if (c->state != RC_PURSUIT || c->alpha < 255) continue;
        a = td5_game_get_actor(c->slot);
        if (!a) continue;
        rc_place_of(a, &x, &z);
        f = ((x - plx) * fx + (z - plz) * fz) / RC_M;
        if (-f >= min_m && -f <= max_m && c->sees) return 1;
    }
    return 0;
}
#endif
