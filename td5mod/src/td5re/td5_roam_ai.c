/**
 * td5_roam_ai.c -- GEO FREE ROAM: AI racers on the planned route (PORT-ONLY, F7).
 * Contract and design: td5_roam_ai.h. Race flow: td5_roam_race.c. The field (which racer slots
 * exist): td5_game_roam_set_racer_field (td5_race_state.h).
 *
 * FRAMES. The route and the driver's path are in the PLACE frame (430 units per metre, doubles);
 * an actor's place position is FP_TRUNC-free world / 256 + the world origin. Speeds are m/s inside
 * this file; the actor's linear velocity is 24.8 world units per sim tick (30 Hz).
 *
 * ONE file-scope struct (A). Main thread only.
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
#include "td5_race_state.h"
#include "td5_asset.h"          /* td5_asset_get_car_zip_path: is an opponent car index loadable */
#include "td5_ai.h"             /* td5_ai_get_catchup_level: the CATCHUP setting scales the band */
#include "td5_ai_driver.h"      /* td5_ai_driver_persona: the same personalities as span races */
#include "td5_roam_mode.h"
#include "td5_roam_locate.h"
#include "td5_roam_route.h"
#include "td5_roam_stream.h"
#include "td5_roam_race.h"
#include "td5_roam_lap.h"
#include "td5_roam_ai.h"
#include "td5_roam_traffic.h"
#include "td5_roam_physics.h"     /* [F8c] DEV impact ring: the provider side table */
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RA_M           430.0                     /* place units per metre               */
#define RA_HZ          30.0                      /* sim ticks per second                */
#define RA_PI          3.14159265358979323846
#define RA_DS          1.5                       /* path resample step, metres          */
#define RA_BACK_M      60.0                      /* path behind START (the grid)        */
#define RA_FWD_M       90.0                      /* path past FINISH (the run-out)      */
#define RA_STEER_CLAMP 0x18000                   /* physics clamp on steering_command   */
#define RA_THR_FULL    0x100
#define RA_FINISH_EPS  2.0                       /* = the race's RR_FINISH_EPS_M        */
#define RA_CP_RADIUS   25.0                      /* a finish counts only this close     */
#define RA_LOST_M      60.0
#define RA_STILL_MS    (3.0 / 3.6)               /* "still" under 3 km/h                */
#define RA_STUCK_LONG  150                       /* 5 s                                 */

typedef struct {
    int    slot, live;
    int    finished, order;
    long   finish_ticks;
    /* progress on the driver's path and on the race polyline */
    double s;  int di;
    double along; int ri; double lat_race;
    double lat;                                  /* signed offset from the path, metres (+ = right) */
    /* lane + racecraft */
    double lane, off, pass_off;
    int    pass_dwell;
    /* control */
    int32_t steer;
    double thr_i;
    int    rec, rec_t, slow_t, cool_t;
    long   still_t, still_max;
    long   recoveries;
    double rec_along;                            /* [F8b] where the last recovery started     */
    int    rec_same;                             /*       recoveries in a row at that spot    */
    long   respawns;
    double v_t;                                  /* last target speed (trace)            */
    int    braking_bend;                         /* brake on with a bend < 50 km/h within 40 m */
    double bend_d;                               /* metres to that bend                   */
    double stop_s;                               /* where a finished car stops            */
    double v_prev;                               /* [F8b] DEV: last tick's speed (impact log) */
    TD5_LapTrack lt;                             /* [F9a] circuit: total distance + laps  */
    int    laps_done;
    /* persona */
    float  skill, aggr, cons, line_bias;
    int    car_index;
} RaCar;

static struct {
    int     preload, loaded;                     /* opponents asked for / loaded this session */
    int     on, n;
    RaCar   car[TD5_ROAM_AI_MAX];
    /* the race polyline (copy) */
    double *rpx, *rpz, *rd; int rn; double length;       /* length = the race's FINISH distance (a circuit: laps x lap) */
    /* [F9a] circuit: laps asked for the next start (req), laps of this race, the lap, the unrolled path's lap */
    int     circ_req; double circ_req_len;
    int     circ, laps; double lap_len, path_lap_len;
    TD5_RoamLoop loop;
    /* the driver's path */
    double *px, *pz, *ps, *pv, *nx, *nz, *tx, *tz; int np;
    /* grid */
    int32_t start_px, start_pz; int start_yaw;
    double  grid_px[TD5_ROAM_AI_MAX], grid_pz[TD5_ROAM_AI_MAX], grid_lane[TD5_ROAM_AI_MAX];
    int     grid_yaw[TD5_ROAM_AI_MAX];
    int     grid_force;                          /* self-test: the player's spot + 1, 0 = knob / random */
    int     player_spot; double ppx, ppz; int pyaw;   /* the player's grid spot (0 = START) */
    int     nfin;
    /* standings */
    TD5_RoamStanding st[1 + TD5_ROAM_AI_MAX]; int nst; int player_rank;
    double  player_along; int player_finished; long player_fin_ticks; long race_ticks; int racing;
    TD5_RoamAiStats stats;
    /* knobs */
    int    knobs;
    double a_lat, a_brake, v_top, v_min, look0, lookk, look_min, look_max, wb, lane_m;
    int    kd, rate;
    int    rb_on; double rb_behind_m, rb_ahead_m, rb_boost, rb_trim;
    int    see_traffic;                          /* [F8c] TD5RE_ROAM_AI_SEE_TRAFFIC (1; 0 = the pre-F8c blind test) */
    int    brake_prop;                           /* [F8c] TD5RE_ROAM_AI_BRAKE_PROP (1; 0 = the idle -32 brake)      */
    double headway;                              /* [F8c] TD5RE_ROAM_AI_HEADWAY, seconds of follow gap (0 = none)   */
    FILE  *trc;
} A;

/* ============================================================ helpers ============ */

static double ra_clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int ra_ang12(int a) { a &= 0xFFF; if (a > 0x800) a -= 0x1000; return a; }
static int ra_angle_of(double dx, double dz)
{
    if (dx == 0.0 && dz == 0.0) return 0;
    return (int)floor(atan2(dx, dz) * (4096.0 / (2.0 * RA_PI)) + 0.5) & 0xFFF;
}

static void ra_read_knobs(void)
{
    if (A.knobs) return;
    A.knobs = 1;
    A.a_lat    = td5_env_int("TD5RE_ROAM_AI_ALAT", 55, 10, 200) / 10.0;      /* m/s^2 x10 */
    A.a_brake  = td5_env_int("TD5RE_ROAM_AI_BRAKE", 55, 10, 200) / 10.0;     /* m/s^2 x10 */
    A.v_top    = td5_env_int("TD5RE_ROAM_AI_VTOP", 110, 30, 300) / 3.6;      /* km/h      */
    A.v_min    = td5_env_int("TD5RE_ROAM_AI_VMIN", 16, 5, 60) / 3.6;         /* km/h      */
    A.look0    = td5_env_int("TD5RE_ROAM_AI_LOOK0", 40, 10, 300) / 10.0;     /* dm        */
    A.lookk    = td5_env_int("TD5RE_ROAM_AI_LOOKK", 45, 0, 200) / 100.0;     /* cs        */
    A.look_min = td5_env_int("TD5RE_ROAM_AI_LOOKMIN", 5, 2, 30);             /* m         */
    A.look_max = td5_env_int("TD5RE_ROAM_AI_LOOKMAX", 24, 6, 80);            /* m         */
    A.wb       = td5_env_int("TD5RE_ROAM_AI_WB", 260, 150, 500) / 100.0;     /* cm        */
    A.lane_m   = td5_env_int("TD5RE_ROAM_AI_LANE", 140, 0, 300) / 100.0;     /* cm        */
    A.kd       = td5_env_int("TD5RE_ROAM_AI_KD", 24, 0, 400);
    A.rate     = td5_env_int("TD5RE_ROAM_AI_STEER_RATE", 0x9000, 0x800, RA_STEER_CLAMP);
    A.rb_on    = td5_env_int("TD5RE_ROAM_AI_RB", 1, 0, 1);
    A.rb_behind_m = td5_env_int("TD5RE_ROAM_AI_RB_BEHIND_M", 300, 20, 5000);
    A.rb_ahead_m  = td5_env_int("TD5RE_ROAM_AI_RB_AHEAD_M", 150, 20, 5000);
    A.rb_boost    = td5_env_int("TD5RE_ROAM_AI_RB_BOOST", 8, 0, 50) / 100.0;
    A.rb_trim     = td5_env_int("TD5RE_ROAM_AI_RB_TRIM", 8, 0, 50) / 100.0;
    A.see_traffic = td5_env_int("TD5RE_ROAM_AI_SEE_TRAFFIC", 1, 0, 1);
    A.brake_prop  = td5_env_int("TD5RE_ROAM_AI_BRAKE_PROP", 1, 0, 1);
    A.headway     = td5_env_int("TD5RE_ROAM_AI_HEADWAY", 80, 0, 300) / 100.0;   /* cs */
}

int td5_roam_ai_preload(void)
{
    return td5_env_int("TD5RE_ROAM_AI_PRELOAD", TD5_ROAM_AI_MAX, 0, TD5_ROAM_AI_MAX);
}

int td5_roam_ai_default_opponents(void)
{
    int n = td5_env_int("TD5RE_ROAM_AI_OPPONENTS", 3, 0, TD5_ROAM_AI_MAX);
    return n > A.loaded ? A.loaded : n;
}

int td5_roam_ai_available(void) { return td5_roam_active() ? A.loaded : 0; }
int td5_roam_ai_count(void) { return A.on ? A.n : 0; }

int td5_roam_ai_racing(void)
{
    int k;
    if (!A.on || !td5_roam_active()) return 0;
    for (k = 0; k < A.n; k++) if (A.car[k].live && !A.car[k].finished) return 1;
    return 0;
}

/* [F8c] Is slot j a car on the road? A racer slot past the field is disabled (state 3). A traffic slot is
 * the traffic pool's to answer: td5_game_get_slot_state says 3 for EVERY slot >= TD5_MAX_RACER_SLOTS (16)
 * and td5_game_roam_set_racer_field writes 3 into the pool slots 6..15 too, so the old "state 3 = skip"
 * test made every traffic car invisible here. The opponents then never followed, passed or braked for
 * traffic and drove into it at full speed (roam-circuit, F8b: slot 3 into traffic 10 / 9 at 80..100 km/h,
 * the heavy-crash scatter launched it), and the IMPACT log reported no car near. */
static int ra_on_road(int j)
{
    if (j < g_traffic_slot_base || !A.see_traffic) return td5_game_get_slot_state(j) != 3;   /* knob 0: the old test */
    return td5_roam_traffic_parked(j) == 0;
}

static int ra_place_of(const TD5_Actor *a, double *px, double *pz)
{
    int32_t ox, oz;
    if (!a) return 0;
    td5_roam_world_origin(&ox, &oz);
    *px = (double)a->world_pos.x / 256.0 + (double)ox;
    *pz = (double)a->world_pos.z / 256.0 + (double)oz;
    return 1;
}

/* a PLACE point on the drivable surface of the resident tiles (the grid footprint, the lane check) */
static int ra_inside(double px, double pz, int32_t y, int has_y)
{
    const TD5_RoamResident *rs = td5_roam_resident();
    int32_t ox, oz;
    TD5_RoamHit h;
    if (!rs) return 0;
    td5_roam_world_origin(&ox, &oz);
    if (!td5_roam_locate(rs, (int32_t)floor((px - ox) * 256.0 + 0.5), (int32_t)floor((pz - oz) * 256.0 + 0.5), y, has_y,
                         ROAM_PRIM_NONE, &h))
        return 0;
    return h.prim != ROAM_PRIM_NONE && h.inside;
}

/* centre + 4 body corners (car 1.9 x 4.4 m) on the road at yaw */
static int ra_footprint_ok(double px, double pz, int yaw12)
{
    const double y = (double)(yaw12 & 0xFFF) * (2.0 * RA_PI / 4096.0);
    const double fx = sin(y), fz = cos(y), rx = fz, rz = -fx;
    const double hl = 2.3 * RA_M, hw = 1.0 * RA_M;
    int i;
    if (!ra_inside(px, pz, 0, 0)) return 0;
    for (i = 0; i < 4; i++) {
        const double l = (i & 1) ? hl : -hl, w = (i & 2) ? hw : -hw;
        if (!ra_inside(px + fx * l + rx * w, pz + fz * l + rz * w, 0, 0)) return 0;
    }
    return 1;
}

static void ra_free_path(void)
{
    free(A.rpx); free(A.rpz); free(A.rd);
    free(A.px); free(A.pz); free(A.ps); free(A.pv); free(A.nx); free(A.nz); free(A.tx); free(A.tz);
    A.rpx = A.rpz = A.rd = NULL;
    A.px = A.pz = A.ps = A.pv = A.nx = A.nz = A.tx = A.tz = NULL;
    A.rn = A.np = 0;
    td5_roam_loop_free(&A.loop);
}

/* ============================================================ path ============== */

/* Snap a PLACE point onto the street centre within 6 m (the planner graph); 0 = no street there. */
static int ra_snap(double x, double z, double *sx, double *sz)
{
    uint64_t key;
    double d;
    return td5_roam_route_snap_xz(x, z, 6.0, sx, sz, &key, &d);
}

/* Extend from (x, z) in direction (ux, uz) by `len` m along the snapped street, 3 m steps.
 * Writes up to `cap` points (excluding the start). A step whose snap turns more than 35 degrees
 * from the running direction (a cross street) goes straight on instead. */
static int ra_extend(double x, double z, double ux, double uz, double len, double *ox, double *oz, int cap)
{
    int n = 0;
    double d = 0.0;
    while (d < len && n < cap) {
        double qx = x + ux * 3.0 * RA_M, qz = z + uz * 3.0 * RA_M, sx, sz;
        if (ra_snap(qx, qz, &sx, &sz)) {
            const double vx = sx - x, vz = sz - z, l = sqrt(vx * vx + vz * vz);
            if (l > 0.5 * RA_M && (vx * ux + vz * uz) / l > cos(35.0 * RA_PI / 180.0)) {
                qx = sx; qz = sz;
                ux = vx / l; uz = vz / l;
            }
        }
        ox[n] = qx; oz[n] = qz; n++;
        x = qx; z = qz;
        d += 3.0;
    }
    return n;
}

static int ra_build_path(void)
{
    /* 1. the race polyline without the planner's junction-connector jogs (the sprint test's rule:
     *    a point that turns back by more than 90 degrees with a leg under 15 m) */
    double *jx = (double *)malloc((size_t)(A.rn + 1) * sizeof(double));
    double *jz = (double *)malloc((size_t)(A.rn + 1) * sizeof(double));
    double bx[64], bz[64], fx[64], fz[64];
    double *cx = NULL, *cz = NULL, *cs = NULL;
    int nj = 0, nb = 0, nf = 0, nc, i, k, m, dropped = 0;
    double ux, uz, l, total;
    if (!jx || !jz) { free(jx); free(jz); return 0; }
    for (i = 0; i < A.rn; i++) {
        if (i >= 1 && i + 1 < A.rn) {
            const double ax = A.rpx[i] - A.rpx[i - 1], az = A.rpz[i] - A.rpz[i - 1];
            const double qx = A.rpx[i + 1] - A.rpx[i], qz = A.rpz[i + 1] - A.rpz[i];
            const double la = sqrt(ax * ax + az * az), lb = sqrt(qx * qx + qz * qz);
            if (la > 0.0 && lb > 0.0 && (ax * qx + az * qz) / (la * lb) < 0.0 && (la < 15.0 * RA_M || lb < 15.0 * RA_M)) {
                dropped++;
                continue;
            }
        }
        if (nj > 0 && fabs(A.rpx[i] - jx[nj - 1]) < 1.0 && fabs(A.rpz[i] - jz[nj - 1]) < 1.0) continue;
        jx[nj] = A.rpx[i]; jz[nj] = A.rpz[i]; nj++;
    }
    if (nj < 2) { free(jx); free(jz); return 0; }
    /* 1b. LATERAL jogs: the planner routes through a junction cluster's centre, which on a divided
     * avenue sits in the median, 7..14 m off the carriageway, with long legs either side (measured on
     * the F7 full route: 9.7 m off a 56 m chord at 671 m, 14 m off a 110 m chord at 2309 m; the
     * opponents drove into the median nose there). A vertex is dropped when the heading before it
     * (i-2 -> i-1) and after it (i+1 -> i+2) agree within 20 degrees, it lies > 1.5 m off the chord
     * i-1 -> i+1 and that chord is < 150 m. A real turn changes heading and is kept. Repeated until
     * nothing moves. */
    {
        int changed = 1, guard = 0;
        while (changed && guard++ < 16) {
            changed = 0;
            for (i = 2; i + 2 < nj; i++) {
                const double ax = jx[i - 1] - jx[i - 2], az = jz[i - 1] - jz[i - 2];
                const double bx = jx[i + 2] - jx[i + 1], bz = jz[i + 2] - jz[i + 1];
                const double cx0 = jx[i + 1] - jx[i - 1], cz0 = jz[i + 1] - jz[i - 1];
                const double la = sqrt(ax * ax + az * az), lb = sqrt(bx * bx + bz * bz), lc = sqrt(cx0 * cx0 + cz0 * cz0);
                double dev;
                if (la <= 0.0 || lb <= 0.0 || lc <= 0.0 || lc > 150.0 * RA_M) continue;
                if ((ax * bx + az * bz) / (la * lb) < cos(20.0 * RA_PI / 180.0)) continue;
                dev = fabs((jx[i] - jx[i - 1]) * cz0 - (jz[i] - jz[i - 1]) * cx0) / lc;
                if (dev <= 1.5 * RA_M) continue;
                memmove(&jx[i], &jx[i + 1], (size_t)(nj - i - 1) * sizeof(double));
                memmove(&jz[i], &jz[i + 1], (size_t)(nj - i - 1) * sizeof(double));
                nj--;
                dropped++;
                changed = 1;
            }
        }
    }
    if (A.circ) goto ctrl_circuit;
    /* 2. behind START: the direction from START to the point >= 8 m on, reversed, along the street */
    k = 1;
    while (k + 1 < nj && hypot(jx[k] - jx[0], jz[k] - jz[0]) < 8.0 * RA_M) k++;
    ux = jx[0] - jx[k]; uz = jz[0] - jz[k]; l = sqrt(ux * ux + uz * uz);
    if (l <= 0.0) { free(jx); free(jz); return 0; }
    nb = ra_extend(jx[0], jz[0], ux / l, uz / l, RA_BACK_M, bx, bz, 64);
    /* 3. past FINISH */
    k = nj - 2;
    while (k > 0 && hypot(jx[nj - 1] - jx[k], jz[nj - 1] - jz[k]) < 8.0 * RA_M) k--;
    ux = jx[nj - 1] - jx[k]; uz = jz[nj - 1] - jz[k]; l = sqrt(ux * ux + uz * uz);
    if (l <= 0.0) { ux = 0.0; uz = 1.0; l = 1.0; }
    nf = ra_extend(jx[nj - 1], jz[nj - 1], ux / l, uz / l, RA_FWD_M, fx, fz, 64);
    /* 4. the combined control polyline, arc length with START at s = 0 */
    nc = nb + nj + nf;
    cx = (double *)malloc((size_t)nc * sizeof(double));
    cz = (double *)malloc((size_t)nc * sizeof(double));
    cs = (double *)malloc((size_t)nc * sizeof(double));
    if (!cx || !cz || !cs) { free(jx); free(jz); free(cx); free(cz); free(cs); return 0; }
    k = 0;
    for (i = nb - 1; i >= 0; i--) { cx[k] = bx[i]; cz[k] = bz[i]; k++; }
    for (i = 0; i < nj; i++) { cx[k] = jx[i]; cz[k] = jz[i]; k++; }
    for (i = 0; i < nf; i++) { cx[k] = fx[i]; cz[k] = fz[i]; k++; }
    cs[0] = 0.0;
    for (i = 1; i < nc; i++) cs[i] = cs[i - 1] + hypot(cx[i] - cx[i - 1], cz[i] - cz[i - 1]) / RA_M;
    {
        const double s_start = cs[nb];
        for (i = 0; i < nc; i++) cs[i] -= s_start;
    }
    goto ctrl_done;
ctrl_circuit:
    /* [F9a] a CIRCUIT: the lap's control points, closed. The path is the lap unrolled A.laps times, with the
     * lap's TAIL as the run-up behind START (the grid) and the lap's START as the run-out past the line. */
    {
        const int laps = A.laps;
        int kb, kf = 0;
        double acc, Lp = 0.0;
        if (nj > 2 && hypot(jx[0] - jx[nj - 1], jz[0] - jz[nj - 1]) < 3.0 * RA_M) nj--;
        if (nj < 3) { free(jx); free(jz); return 0; }
        for (i = 0; i < nj; i++) {
            const int j = (i + 1) % nj;
            Lp += hypot(jx[j] - jx[i], jz[j] - jz[i]) / RA_M;
        }
        acc = hypot(jx[0] - jx[nj - 1], jz[0] - jz[nj - 1]) / RA_M;
        kb = 1;
        for (i = nj - 1; i > 1 && acc < RA_BACK_M + 8.0; i--) {
            acc += hypot(jx[i] - jx[i - 1], jz[i] - jz[i - 1]) / RA_M;
            kb++;
        }
        acc = 0.0;
        for (i = 1; i < nj && acc < RA_FWD_M + 8.0; i++) {
            acc += hypot(jx[i] - jx[i - 1], jz[i] - jz[i - 1]) / RA_M;
            kf++;
        }
        nb = kb; nf = kf;
        nc = kb + laps * nj + 1 + kf;
        cx = (double *)malloc((size_t)nc * sizeof(double));
        cz = (double *)malloc((size_t)nc * sizeof(double));
        cs = (double *)malloc((size_t)nc * sizeof(double));
        if (!cx || !cz || !cs) { free(jx); free(jz); free(cx); free(cz); free(cs); return 0; }
        k = 0;
        for (i = nj - kb; i < nj; i++) { cx[k] = jx[i]; cz[k] = jz[i]; k++; }
        for (m = 0; m < laps; m++)
            for (i = 0; i < nj; i++) { cx[k] = jx[i]; cz[k] = jz[i]; k++; }
        for (i = 0; i <= kf; i++) { cx[k] = jx[i]; cz[k] = jz[i]; k++; }
        cs[0] = 0.0;
        for (i = 1; i < nc; i++) cs[i] = cs[i - 1] + hypot(cx[i] - cx[i - 1], cz[i] - cz[i - 1]) / RA_M;
        {
            const double s_start = cs[nb];
            for (i = 0; i < nc; i++) cs[i] -= s_start;
        }
        A.path_lap_len = Lp;
        TD5_LOG_I(LOG_TAG, "ROAM_AI: circuit path: lap %.1f m (race lap %.1f m), %d laps unrolled, %d run-up + %d run-out control points",
                  Lp, A.lap_len, laps, kb, kf);
    }
ctrl_done:
    free(jx); free(jz);
    /* 5. resample every RA_DS metres */
    total = cs[nc - 1] - cs[0];
    A.np = (int)(total / RA_DS) + 1;
    if (A.np < 4) { free(cx); free(cz); free(cs); return 0; }
    A.px = (double *)malloc((size_t)A.np * sizeof(double));
    A.pz = (double *)malloc((size_t)A.np * sizeof(double));
    A.ps = (double *)malloc((size_t)A.np * sizeof(double));
    A.pv = (double *)malloc((size_t)A.np * sizeof(double));
    A.nx = (double *)malloc((size_t)A.np * sizeof(double));
    A.nz = (double *)malloc((size_t)A.np * sizeof(double));
    A.tx = (double *)malloc((size_t)A.np * sizeof(double));
    A.tz = (double *)malloc((size_t)A.np * sizeof(double));
    if (!A.px || !A.pz || !A.ps || !A.pv || !A.nx || !A.nz || !A.tx || !A.tz) { free(cx); free(cz); free(cs); return 0; }
    {
        double *rx = (double *)malloc((size_t)A.np * sizeof(double));
        double *rz = (double *)malloc((size_t)A.np * sizeof(double));
        int seg = 0;
        if (!rx || !rz) { free(rx); free(rz); free(cx); free(cz); free(cs); return 0; }
        for (i = 0; i < A.np; i++) {
            const double s = cs[0] + i * RA_DS;
            double t;
            while (seg + 2 < nc && cs[seg + 1] < s) seg++;
            t = cs[seg + 1] > cs[seg] ? (s - cs[seg]) / (cs[seg + 1] - cs[seg]) : 0.0;
            t = ra_clamp(t, 0.0, 1.0);
            rx[i] = cx[seg] + (cx[seg + 1] - cx[seg]) * t;
            rz[i] = cz[seg] + (cz[seg + 1] - cz[seg]) * t;
            A.ps[i] = s;
        }
        /* 6. light smoothing: +-3 samples (4.5 m), ends kept */
        for (i = 0; i < A.np; i++) {
            const int a0 = i - 3 < 0 ? 0 : i - 3, a1 = i + 3 >= A.np ? A.np - 1 : i + 3;
            double sx = 0.0, sz = 0.0;
            int j;
            if (i < 3 || i + 3 >= A.np) { A.px[i] = rx[i]; A.pz[i] = rz[i]; continue; }
            for (j = a0; j <= a1; j++) { sx += rx[j]; sz += rz[j]; }
            A.px[i] = sx / (a1 - a0 + 1); A.pz[i] = sz / (a1 - a0 + 1);
        }
        free(rx); free(rz);
    }
    free(cx); free(cz); free(cs);
    /* 7. tangents + right normals (yaw convention: forward (sin y, cos y), right (cos y, -sin y)) */
    for (i = 0; i < A.np; i++) {
        const int a0 = i > 0 ? i - 1 : 0, a1 = i + 1 < A.np ? i + 1 : A.np - 1;
        double dx = A.px[a1] - A.px[a0], dz = A.pz[a1] - A.pz[a0], dl = sqrt(dx * dx + dz * dz);
        if (dl <= 0.0) { dx = 0.0; dz = 1.0; dl = 1.0; }
        A.tx[i] = dx / dl; A.tz[i] = dz / dl;
        A.nx[i] = A.tz[i]; A.nz[i] = -A.tx[i];
    }
    /* 8. speed profile: corner cap from the heading change over +-6 samples (+-9 m), then braking */
    for (i = 0; i < A.np; i++) {
        const int a0 = i - 6 < 0 ? 0 : i - 6, a1 = i + 6 >= A.np ? A.np - 1 : i + 6;
        const double h0 = atan2(A.tx[a0], A.tz[a0]), h1 = atan2(A.tx[a1], A.tz[a1]);
        double dh = h1 - h0, R;
        while (dh > RA_PI) dh -= 2.0 * RA_PI;
        while (dh < -RA_PI) dh += 2.0 * RA_PI;
        dh = fabs(dh);
        R = dh > 1e-3 ? ((a1 - a0) * RA_DS) / dh : 1e9;
        A.pv[i] = ra_clamp(sqrt(A.a_lat * R), A.v_min, A.v_top);
    }
    for (i = A.np - 2; i >= 0; i--) {
        const double lim = sqrt(A.pv[i + 1] * A.pv[i + 1] + 2.0 * A.a_brake * RA_DS);
        if (lim < A.pv[i]) A.pv[i] = lim;
    }
    {
        int nb90 = 0, nb50 = 0;
        double vmin = 1e9;
        for (i = 1; i < A.np; i++) {
            if (A.pv[i] < 50.0 / 3.6 && A.pv[i - 1] >= 50.0 / 3.6) nb50++;
            if (A.pv[i] < 30.0 / 3.6 && A.pv[i - 1] >= 30.0 / 3.6) nb90++;
            if (A.ps[i] >= 0.0 && A.ps[i] <= A.length && A.pv[i] < vmin) vmin = A.pv[i];
        }
        TD5_LOG_I(LOG_TAG, "ROAM_AI: path %d points (%.1f m, %d connector jogs dropped, %d m grid run-up, %d m run-out), "
                  "%d bends under 50 km/h, %d under 30 km/h, slowest %.0f km/h | a_lat %.1f a_brake %.1f top %.0f km/h",
                  A.np, total, dropped, nb * 3, nf * 3, nb50, nb90, vmin * 3.6, A.a_lat, A.a_brake, A.v_top * 3.6);
    }
    return 1;
}

/* nearest path point to (x, z) in a window round the hint: arc length, signed lateral (m), index */
static double ra_path_proj(double x, double z, int *hint, double *lat)
{
    int i, best = -1, lo = *hint - 20, hi = *hint + 60;
    double bd = 1e300, bs = 0.0, bt = 0.0;
    if (lo < 0) lo = 0;
    if (hi > A.np - 2) hi = A.np - 2;
    for (i = lo; i <= hi; i++) {
        const double sx = A.px[i + 1] - A.px[i], sz = A.pz[i + 1] - A.pz[i], l2 = sx * sx + sz * sz;
        double t = l2 > 0.0 ? ((x - A.px[i]) * sx + (z - A.pz[i]) * sz) / l2 : 0.0, qx, qz, d;
        t = ra_clamp(t, 0.0, 1.0);
        qx = A.px[i] + sx * t; qz = A.pz[i] + sz * t;
        d = (qx - x) * (qx - x) + (qz - z) * (qz - z);
        if (d < bd) { bd = d; best = i; bt = t; }
    }
    if (best < 0) { *lat = 0.0; return A.ps[*hint < 0 ? 0 : *hint]; }
    *hint = best;
    bs = A.ps[best] + bt * RA_DS;
    *lat = ((x - A.px[best]) * A.nx[best] + (z - A.pz[best]) * A.nz[best]) / RA_M;
    return bs;
}

static int ra_path_index(double s)
{
    int i = (int)floor((s - A.ps[0]) / RA_DS);
    return i < 0 ? 0 : (i > A.np - 1 ? A.np - 1 : i);
}

static void ra_path_point(double s, double off_m, double *x, double *z, int *idx)
{
    const int i = ra_path_index(s);
    const int j = i + 1 < A.np ? i + 1 : i;
    const double t = ra_clamp((s - A.ps[i]) / RA_DS, 0.0, 1.0);
    *x = A.px[i] + (A.px[j] - A.px[i]) * t + A.nx[i] * off_m * RA_M;
    *z = A.pz[i] + (A.pz[j] - A.pz[i]) * t + A.nz[i] * off_m * RA_M;
    if (idx) *idx = i;
}

/* distance along the RACE polyline (the player's measure) in a window round the last answer; the
 * first segment extends backwards, so a car on the grid has a negative distance */
static double ra_race_along(RaCar *c, double x, double z, double *lat_m)
{
    double best = 1e300, ba = c->along;
    int i, bs = -1, lo = c->ri, hi = c->ri;
    while (lo > 0 && A.rd[lo] > c->along - 40.0) lo--;
    while (hi + 2 < A.rn && A.rd[hi + 1] < c->along + 150.0) hi++;
    for (i = lo; i <= hi && i + 1 < A.rn; i++) {
        const double sx = A.rpx[i + 1] - A.rpx[i], sz = A.rpz[i + 1] - A.rpz[i], l2 = sx * sx + sz * sz;
        double t = l2 > 0.0 ? ((x - A.rpx[i]) * sx + (z - A.rpz[i]) * sz) / l2 : 0.0, qx, qz, d;
        if (t < 0.0 && i > 0) t = 0.0;      /* i == 0: behind START stays negative */
        if (t > 1.0) t = 1.0;
        qx = A.rpx[i] + sx * t; qz = A.rpz[i] + sz * t;
        d = (qx - x) * (qx - x) + (qz - z) * (qz - z);
        if (d < best) { best = d; bs = i; ba = A.rd[i] + t * (A.rd[i + 1] - A.rd[i]); }
    }
    *lat_m = sqrt(best) / RA_M;
    if (bs < 0 || *lat_m > RA_LOST_M) return c->along;
    c->ri = bs;
    return ba;
}

/* ============================================================ session ============ */

void td5_roam_ai_session_init(void)
{
    int k;
    if (A.trc) { fclose(A.trc); A.trc = NULL; }
    ra_free_path();
    for (k = 0; k < TD5_ROAM_AI_MAX; k++) memset(&A.car[k], 0, sizeof A.car[k]);
    A.on = 0; A.n = 0; A.nst = 0; A.player_rank = 0;
    A.circ = 0; A.circ_req = 0; A.laps = 0;
    A.loaded = 0;
    if (!td5_roam_active()) return;
    {
        /* the cars the race init loaded = the racer slots it spawned past the player */
        const int total = td5_game_get_total_actor_count();
        A.loaded = total - 1;
        if (A.loaded > TD5_ROAM_AI_MAX) A.loaded = TD5_ROAM_AI_MAX;
        if (A.loaded < 0) A.loaded = 0;
    }
    td5_game_roam_set_racer_field(0);
    TD5_LOG_I(LOG_TAG, "ROAM_AI: session: %d opponent car(s) loaded and parked (preload knob %d)", A.loaded,
              td5_roam_ai_preload());
}

/* ============================================================ race ============== */

#ifndef TD5RE_RELEASE
static void ra_ring_reset(void);
#endif

static void ra_place(int k)
{
    RaCar *c = &A.car[k];
    TD5_Actor *a = td5_game_get_actor(c->slot);
    if (!a) return;
    td5_roam_place_actor(c->slot, a, (int32_t)floor(A.grid_px[k] + 0.5), (int32_t)floor(A.grid_pz[k] + 0.5), A.grid_yaw[k]);
    a->steering_command = 0;
    a->encounter_steering_cmd = 0;
    a->brake_flag = 1;
    a->handbrake_flag = 0;
    c->steer = 0; c->thr_i = 0.0;
    c->rec = 0; c->rec_t = 0; c->slow_t = 0; c->cool_t = 0; c->still_t = 0;
    c->di = 0; c->ri = 0; c->along = 0.0; c->v_prev = 0.0;
    c->rec_along = 0.0; c->rec_same = 0; c->respawns = 0;
    td5_roam_lap_reset(&c->lt); c->laps_done = 0;          /* [F9a] circuit: re-found on the next progress call */
    {
        double lat;
        c->s = ra_path_proj(A.grid_px[k], A.grid_pz[k], &c->di, &lat);
        c->lat = lat;
        /* the hint window starts at the grid: walk it there */
        c->di = ra_path_index(c->s);
    }
    c->off = A.grid_lane[k];
}

/* The grid: candidate spots behind START every 7.5 m, two columns at +-1.7 m (then +-1.25 m), single
 * file on the centre where two do not fit. A spot is taken when the car's footprint is on the road
 * and it is >= 5 m from every car already placed (the player at START included). */
static int ra_build_grid(int want)
{
    static const double k_lat[] = { -1.7, 1.7, -1.25, 1.25, 0.0 };
    int row, li, got = 0, single = 0, rows = 0;
    for (row = 1; row <= 8 && got < want; row++) {
        int row_used = 0;
        for (li = 0; li < (int)(sizeof k_lat / sizeof k_lat[0]) && got < want; li++) {
            double s = -7.5 * row, x, z, best = 1e300;
            int yaw, j, idx;
            if (k_lat[li] == 0.0 && row_used) continue;          /* a row with two never takes a third */
            ra_path_point(s, k_lat[li], &x, &z, &idx);
            yaw = ra_angle_of(A.tx[idx], A.tz[idx]);
            if (!ra_footprint_ok(x, z, yaw)) continue;
            best = hypot(x - A.start_px, z - A.start_pz);
            for (j = 0; j < got; j++) { const double d = hypot(x - A.grid_px[j], z - A.grid_pz[j]); if (d < best) best = d; }
            if (best < 5.0 * RA_M) continue;
            A.grid_px[got] = x; A.grid_pz[got] = z; A.grid_yaw[got] = yaw; A.grid_lane[got] = k_lat[li];
            if (k_lat[li] == 0.0) single++;
            got++; row_used++;
            if (row_used >= 2) break;
        }
        if (row_used) rows++;
    }
    A.stats.grid_rows = rows;
    A.stats.grid_single = single;
    TD5_LOG_I(LOG_TAG, "ROAM_AI: grid %d/%d spots behind START in %d rows (%d single file)", got, want, rows, single);
    return got;
}

int td5_roam_ai_race_start(const double *px, const double *pz, const double *dist, int npt, double length_m,
                           int32_t start_px, int32_t start_pz, int start_yaw, int n_ai)
{
    int k, placed;
    uint32_t t0 = td5_plat_time_ms();
    ra_read_knobs();
    td5_roam_ai_race_end();
    if (!td5_roam_active() || A.loaded <= 0 || n_ai <= 0 || npt < 2) return 0;
    if (n_ai > A.loaded) n_ai = A.loaded;
    ra_free_path();
    A.rpx = (double *)malloc((size_t)npt * sizeof(double));
    A.rpz = (double *)malloc((size_t)npt * sizeof(double));
    A.rd  = (double *)malloc((size_t)npt * sizeof(double));
    if (!A.rpx || !A.rpz || !A.rd) { ra_free_path(); return 0; }
    memcpy(A.rpx, px, (size_t)npt * sizeof(double));
    memcpy(A.rpz, pz, (size_t)npt * sizeof(double));
    memcpy(A.rd, dist, (size_t)npt * sizeof(double));
    A.rn = npt; A.length = length_m;
    A.circ = 0; A.laps = 0; A.lap_len = 0.0; A.path_lap_len = 0.0;
    if (A.circ_req > 0) {
        /* [F9a] a circuit: the polyline is ONE closed lap, the race is `laps` of it */
        if (!td5_roam_loop_init(&A.loop, px, pz, dist, npt, RA_M)) {
            TD5_LOG_W(LOG_TAG, "ROAM_AI: the circuit lap is not a usable closed loop: no opponents");
            A.circ_req = 0; ra_free_path(); return 0;
        }
        A.circ = 1; A.laps = A.circ_req; A.lap_len = A.loop.L;
        A.length = A.lap_len * (double)A.laps;
        if (fabs(A.lap_len - A.circ_req_len) > 1.0)
            TD5_LOG_W(LOG_TAG, "ROAM_AI: circuit lap %.1f m differs from the race's %.1f m", A.lap_len, A.circ_req_len);
    }
    A.circ_req = 0;
    A.start_px = start_px; A.start_pz = start_pz; A.start_yaw = start_yaw;
    if (td5_roam_stream_active()) td5_roam_stream_wait_area(start_px, start_pz, -1);
    if (!ra_build_path()) { TD5_LOG_W(LOG_TAG, "ROAM_AI: no driver path (route too short?)"); ra_free_path(); return 0; }
    memset(&A.stats, 0, sizeof A.stats);
    A.stats.circuit_laps = A.circ ? A.laps : 0;
    placed = ra_build_grid(n_ai);
    if (placed <= 0) { TD5_LOG_W(LOG_TAG, "ROAM_AI: no grid spot behind START fits a car: no opponents"); ra_free_path(); return 0; }
    A.n = placed;
    /* The player's spot: START plus the spots behind it form one grid of placed + 1, the player
     * draws one (Mariano: random), the opponents fill the rest in order. Spot 0 = START. */
    {
        double sx[TD5_ROAM_AI_MAX + 1], sz[TD5_ROAM_AI_MAX + 1], sl[TD5_ROAM_AI_MAX + 1];
        int sy[TD5_ROAM_AI_MAX + 1], j, m = 0, p = A.grid_force - 1;
        sx[0] = start_px; sz[0] = start_pz; sl[0] = 0.0; sy[0] = start_yaw & 0xFFF;
        for (k = 0; k < placed; k++) { sx[k + 1] = A.grid_px[k]; sz[k + 1] = A.grid_pz[k]; sl[k + 1] = A.grid_lane[k]; sy[k + 1] = A.grid_yaw[k]; }
        if (p < 0) p = td5_env_int("TD5RE_ROAM_GRID_PLAYER", -1, -1, TD5_ROAM_AI_MAX);
        if (p < 0) {
            /* not the CRT rand(): the sim's random sequence stays untouched */
            uint32_t h = td5_plat_time_ms() * 2654435761u;
            h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 13;
            p = (int)(h % (uint32_t)(placed + 1));
        }
        if (p > placed) p = placed;
        A.player_spot = p;
        A.ppx = sx[p]; A.ppz = sz[p]; A.pyaw = sy[p];
        for (j = 0; j <= placed; j++) {
            if (j == p) continue;
            A.grid_px[m] = sx[j]; A.grid_pz[m] = sz[j]; A.grid_lane[m] = sl[j]; A.grid_yaw[m] = sy[j];
            m++;
        }
        TD5_LOG_I(LOG_TAG, "ROAM_AI: player grid spot %d of %d (%s)", p + 1, placed + 1,
                  A.grid_force > 0 ? "forced by the self-test" : "random, TD5RE_ROAM_GRID_PLAYER to fix it");
    }
    td5_game_roam_set_racer_field(A.n);
    A.nfin = 0;
#ifndef TD5RE_RELEASE
    ra_ring_reset();
#endif
    for (k = 0; k < A.n; k++) {
        RaCar *c = &A.car[k];
        memset(c, 0, sizeof *c);
        c->slot = k + 1;
        c->live = 1;
        c->car_index = td5_game_get_slot_car_index(c->slot);
        td5_ai_driver_persona(c->slot, g_td5.difficulty_tier, &c->skill, &c->aggr, &c->cons, &c->line_bias);
        c->lane = A.grid_lane[k] == 0.0 ? 0.0 : (A.grid_lane[k] < 0.0 ? -A.lane_m : A.lane_m);
        ra_place(k);
    }
    A.on = 1;
    A.stats.n = A.n;
    A.player_along = 0.0; A.player_finished = 0; A.player_fin_ticks = 0; A.race_ticks = 0; A.racing = 0;
#ifndef TD5RE_RELEASE
    if (td5_env_int("TD5RE_ROAM_AI_TRACE", 0, 0, 1)) {
        char path[512];
        const char *dir = td5_plat_log_dir();
        snprintf(path, sizeof path, "%sroam_ai_trace.csv", dir ? dir : "");
        A.trc = fopen(path, "w");
        if (A.trc) fprintf(A.trc, "tick,slot,s,along,lat,kmh,vt_kmh,off,steer,thr,brake,rec,rank\n");
    }
#endif
    for (k = 0; k < A.n; k++)
        TD5_LOG_I(LOG_TAG, "ROAM_AI: slot %d car %d on the grid at place (%.0f,%.0f) yaw %d lane %+.1f m (skill %.2f aggr %.2f)",
                  A.car[k].slot, A.car[k].car_index, A.grid_px[k], A.grid_pz[k], A.grid_yaw[k], A.car[k].lane,
                  (double)A.car[k].skill, (double)A.car[k].aggr);
    TD5_LOG_I(LOG_TAG, "ROAM_AI: race start, %d opponent(s) (asked %d, loaded %d), path + grid %u ms", A.n, n_ai, A.loaded,
              (unsigned)(td5_plat_time_ms() - t0));
    return A.n;
}

void td5_roam_ai_regrid(void)
{
    int k;
    if (!A.on) return;
    for (k = 0; k < A.n; k++) ra_place(k);
}

void td5_roam_ai_race_end(void)
{
    if (A.trc) { fclose(A.trc); A.trc = NULL; }
    if (A.on) {
        TD5_LOG_I(LOG_TAG, "ROAM_AI: race over: %d/%d opponent(s) finished, stuck>5s %ld, recoveries %ld, passes %ld; parked",
                  A.stats.finished, A.n, A.stats.stuck_long, A.stats.recoveries, A.stats.passes);
        td5_game_roam_set_racer_field(0);
    }
    A.on = 0;                 /* A.n, the cars and the counters stay readable for the verdict */
    A.nst = 0;
    A.player_rank = 0;
}

/* ============================================================ driving ========== */

/* lane offset that keeps the aim point and the body beside it on the road: off, off/2, 0 */
static double ra_lane_fit(double s, double off, int32_t y)
{
    int tries;
    for (tries = 0; tries < 3; tries++) {
        double x, z, x2, z2;
        int idx;
        const double side = off > 0.0 ? 1.0 : -1.0;
        if (fabs(off) < 0.05) return 0.0;
        ra_path_point(s, off, &x, &z, &idx);
        x2 = x + A.nx[idx] * side * 1.1 * RA_M; z2 = z + A.nz[idx] * side * 1.1 * RA_M;
        if (ra_inside(x, z, y, 1) && ra_inside(x2, z2, y, 1)) return off;
        off *= 0.5;
        A.stats.lane_shrinks++;
    }
    return 0.0;
}

/* [F8b] a car pinned where reversing does not free it (roam-circuit: thrown off its lane at speed into a side
 * street mouth, held between the race barrier and the street wall, 102 recoveries in place): put it back on its
 * path 12..30 m behind, on its lane where that fits, clear of every car. 1 = placed. */
static int ra_respawn(RaCar *c, TD5_Actor *a)
{
    double back;
    for (back = 12.0; back <= 30.0; back += 6.0) {
        const double s = c->s - back;
        const double off = ra_lane_fit(s, c->lane, a->world_pos.y);
        double x, z, lat;
        int idx, yaw, j, ok = 1;
        const int total = td5_game_get_total_actor_count();
        ra_path_point(s, off, &x, &z, &idx);
        yaw = ra_angle_of(A.tx[idx], A.tz[idx]);
        if (!ra_footprint_ok(x, z, yaw)) continue;
        for (j = 0; j < total && ok; j++) {
            const TD5_Actor *b;
            double bx, bz;
            if (j == c->slot || !ra_on_road(j)) continue;
            b = td5_game_get_actor(j);
            if (b && ra_place_of(b, &bx, &bz) && hypot(bx - x, bz - z) < 6.0 * RA_M) ok = 0;
        }
        if (!ok) continue;
        td5_roam_place_actor(c->slot, a, (int32_t)floor(x + 0.5), (int32_t)floor(z + 0.5), yaw);
        a->steering_command = 0; a->encounter_steering_cmd = 0; a->brake_flag = 0; a->handbrake_flag = 0;
        c->steer = 0; c->thr_i = 0.0;
        c->rec = 0; c->rec_t = 0; c->slow_t = 0; c->cool_t = 0; c->still_t = 0; c->v_prev = 0.0;
        c->rec_same = 0; c->pass_dwell = 0;
        c->s = ra_path_proj(x, z, &c->di, &lat);
        c->lat = lat; c->off = off;
        c->respawns++; A.stats.respawns++;
        TD5_LOG_W(LOG_TAG, "ROAM_AI: slot %d RESPAWN %ld: pinned at %.0f m along after %ld recoveries, put back %.0f m on its path (lane %+.1f m)",
                  c->slot, c->respawns, c->along, c->recoveries, back, off);
        return 1;
    }
    return 0;
}

#ifndef TD5RE_RELEASE
/* [F8c] DEV: an after-the-fact ring of each opponent's state, one entry per driven tick (the actor as
 * physics left it + the provider's side table for that slot), written out only when an IMPACT fires.
 * A memcpy per car per tick, no file I/O until then: the run's timing stays what it was. */
#define RA_RING 16
typedef struct {
    long          race_tick, sim_tick;
    double        s, lat;
    TD5_Actor     a;
    unsigned char phys[TD5_ROAM_PHYS_SLOT_BLOB];
} RaSnap;
static struct { RaSnap e[RA_RING]; int head, n; } s_ring[TD5_ROAM_AI_MAX];

static void ra_ring_reset(void) { memset(s_ring, 0, sizeof s_ring); }

static void ra_ring_push(const RaCar *c, const TD5_Actor *a)
{
    const int k = (int)(c - A.car);
    RaSnap *e;
    if (k < 0 || k >= TD5_ROAM_AI_MAX) return;
    e = &s_ring[k].e[s_ring[k].head];
    e->race_tick = A.race_ticks; e->sim_tick = (long)g_td5.simulation_tick_counter;
    e->s = c->s; e->lat = c->lat;
    memcpy(&e->a, a, sizeof *a);
    if (!td5_roam_physics_dev_slot_copy(c->slot, e->phys, sizeof e->phys)) memset(e->phys, 0, sizeof e->phys);
    s_ring[k].head = (s_ring[k].head + 1) % RA_RING;
    if (s_ring[k].n < RA_RING) s_ring[k].n++;
}

static void ra_ring_print_actor(FILE *f, const TD5_Actor *a)
{
    int w;
    fprintf(f, "  pos (%d,%d,%d) vel (%d,%d,%d) euler r/y/p (%d,%d,%d) angvel r/y/p (%d,%d,%d) mode %d contact 0x%X prev 0x%X "
               "air %d steer %d thr %d brake %d slip f/r %d/%d span %d\n",
            (int)a->world_pos.x, (int)a->world_pos.y, (int)a->world_pos.z, (int)a->linear_velocity_x, (int)a->linear_velocity_y,
            (int)a->linear_velocity_z, (int)a->euler_accum.roll, (int)a->euler_accum.yaw, (int)a->euler_accum.pitch,
            (int)a->angular_velocity_roll, (int)a->angular_velocity_yaw, (int)a->angular_velocity_pitch, (int)a->vehicle_mode,
            (unsigned)a->wheel_contact_bitmask, (unsigned)a->damage_lockout, (int)a->airborne_frame_counter,
            (int)a->steering_command, (int)a->encounter_steering_cmd, (int)a->brake_flag, (int)a->front_axle_slip_excess,
            (int)a->rear_axle_slip_excess, (int)a->track_span_raw);
    for (w = 0; w < 4; w++)
        fprintf(f, "  wheel%d hires (%d,%d,%d) contact (%d,%d,%d) n (%d,%d,%d) susp %d dv %d load %d probe (%d,%d,%d)\n", w,
                (int)a->wheel_world_positions_hires[w].x, (int)a->wheel_world_positions_hires[w].y,
                (int)a->wheel_world_positions_hires[w].z, (int)a->wheel_contact_pos[w].x, (int)a->wheel_contact_pos[w].y,
                (int)a->wheel_contact_pos[w].z, a->wheel_contact_normals[w][0], a->wheel_contact_normals[w][1],
                a->wheel_contact_normals[w][2], (int)a->wheel_suspension_pos[w], (int)a->wheel_spring_dv[w],
                (int)a->wheel_load_accum[w], (int)(&a->probe_FL)[w].x, (int)(&a->probe_FL)[w].y, (int)(&a->probe_FL)[w].z);
}

static void ra_ring_dump(const RaCar *c, const TD5_Actor *now, double v_prev, double v)
{
    const int k = (int)(c - A.car);
    char path[512];
    const char *dir = td5_plat_log_dir();
    FILE *f;
    int i, n;
    if (k < 0 || k >= TD5_ROAM_AI_MAX || (n = s_ring[k].n) <= 0) return;
    snprintf(path, sizeof path, "%sroam_ai_impact_s%d_t%ld.txt", dir ? dir : "", c->slot, A.race_ticks);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "slot %d car %d IMPACT %.1f -> %.1f km/h at race tick %ld sim tick %ld, s %.2f along %.1f lat %.2f; "
               "%d ring entries, oldest first, then NOW (the state after the impact tick's physics)\n",
            c->slot, c->car_index, v_prev * 3.6, v * 3.6, A.race_ticks, (long)g_td5.simulation_tick_counter, c->s, c->along,
            c->lat, n);
    for (i = 0; i < n; i++) {
        const RaSnap *e = &s_ring[k].e[(s_ring[k].head - n + i + RA_RING) % RA_RING];
        fprintf(f, "[ring %d] race tick %ld sim tick %ld s %.2f lat %.2f\n", i - n, e->race_tick, e->sim_tick, e->s, e->lat);
        ra_ring_print_actor(f, &e->a);
        td5_roam_physics_dev_slot_print(f, e->phys);
    }
    {
        unsigned char blob[TD5_ROAM_PHYS_SLOT_BLOB];
        const RaSnap *last = &s_ring[k].e[(s_ring[k].head - 1 + RA_RING) % RA_RING];
        const uint32_t *p0 = (const uint32_t *)&last->a, *p1 = (const uint32_t *)now;
        fprintf(f, "[NOW] race tick %ld sim tick %ld\n", A.race_ticks, (long)g_td5.simulation_tick_counter);
        ra_ring_print_actor(f, now);
        if (td5_roam_physics_dev_slot_copy(c->slot, blob, sizeof blob)) td5_roam_physics_dev_slot_print(f, blob);
        fprintf(f, "[DIFF last ring -> NOW] actor dwords that changed (offset: before -> after)\n");
        for (i = 0; i < (int)(sizeof(TD5_Actor) / 4); i++)
            if (p0[i] != p1[i]) fprintf(f, "  +0x%03X: %d -> %d\n", i * 4, (int)p0[i], (int)p1[i]);
    }
    fclose(f);
    TD5_LOG_I(LOG_TAG, "ROAM_AI: slot %d impact ring -> %s", c->slot, path);
}
#endif

/* the field's speeds: a car ahead in my lane and slower -> follow cap + a pass request */
static double ra_racecraft(RaCar *c, double x, double z, double v, int idx)
{
    double cap = 1e9;
    int j, total = td5_game_get_total_actor_count();
    const double tx = A.tx[idx], tz = A.tz[idx], nx = A.nx[idx], nz = A.nz[idx];
    for (j = 0; j < total; j++) {
        const TD5_Actor *b;
        double bx, bz, f, l, vb;
        if (j == c->slot || !ra_on_road(j)) continue;
        b = td5_game_get_actor(j);
        if (!b || !ra_place_of(b, &bx, &bz)) continue;
        f = ((bx - x) * tx + (bz - z) * tz) / RA_M;
        l = ((bx - x) * nx + (bz - z) * nz) / RA_M + c->lat;
        if (f <= 0.5 || f > 6.0 + v * 1.2) continue;
        if (fabs(l - c->lat) > 2.1) continue;
        vb = ((double)b->linear_velocity_x * tx + (double)b->linear_velocity_z * tz) / 256.0 * RA_HZ / RA_M;
        /* slower than me, or (nearly) stopped: a stopped car ahead would otherwise hold the whole
         * queue behind it at 0 km/h, nobody being faster than it (F7 full run, 682 m) */
        if ((vb < v - 0.5 || vb < 1.5) && c->pass_dwell <= 0) {
            /* pass on the side away from it */
            const double side = (l - c->lat) > 0.0 ? -1.0 : 1.0;
            c->pass_off = ra_clamp(l + side * 2.5, -2.8, 2.8);
            c->pass_dwell = 45 + (int)(c->aggr * 30.0f);
            A.stats.passes++;
        }
        {
            /* [F8c] the gap kept grows with the speed of the car ahead (TD5RE_ROAM_AI_HEADWAY s): 5.5 m centre to
             * centre (1 m between bumpers) was the gap at any speed, so a traffic car braking for its red light
             * was rear-ended (roam-circuit: 64 -> 47 km/h into traffic 6, the heavy-crash scatter) */
            const double gap = f - 5.5 - A.headway * (vb > 0.0 ? vb : 0.0);
            const double cv = (vb > 0.0 ? vb : 0.0) + (gap > 0.0 ? gap * 0.6 : gap * 1.5);
            if (cv < cap) cap = cv;
        }
    }
    if (cap < 1e8) A.stats.follow_ticks++;
    return cap;
}

static void ra_drive(RaCar *c, int hold)
{
    TD5_Actor *a = td5_game_get_actor(c->slot);
    const TD5_RoamResident *rs = td5_roam_resident();
    double x, z, lat, v, vt, L, ax, az, dx, dz, dl, alpha, delta, rb = 1.0, cap;
    int32_t steer_t;
    int heading, herr, idx, thr = 0, brake = 0;
    if (!a || !rs) return;
    ra_place_of(a, &x, &z);
    c->s = ra_path_proj(x, z, &c->di, &lat);
    c->lat = lat;
    v = sqrt((double)a->linear_velocity_x * a->linear_velocity_x + (double)a->linear_velocity_z * a->linear_velocity_z) /
        256.0 * RA_HZ / RA_M;
    if (v * 3.6 > A.stats.vmax_kmh) A.stats.vmax_kmh = v * 3.6;
    heading = ((int)a->euler_accum.yaw >> 8) & 0xFFF;
#ifndef TD5RE_RELEASE
    if (!hold && c->v_prev - v > 3.0) {
        /* [F8b] DEV: an impact (over 11 km/h lost in one tick): what was hit */
        int j, bj = -1, total = td5_game_get_total_actor_count();
        double bd = 1e300, bv = 0.0, bf = 0.0, bl = 0.0;
        for (j = 0; j < total; j++) {
            const TD5_Actor *b;
            double bx, bz, d;
            if (j == c->slot || !ra_on_road(j)) continue;
            b = td5_game_get_actor(j);
            if (!b || !ra_place_of(b, &bx, &bz)) continue;
            d = hypot(bx - x, bz - z) / RA_M;
            if (d < bd) {
                const int i0 = ra_path_index(c->s);
                bd = d; bj = j;
                bv = sqrt((double)b->linear_velocity_x * b->linear_velocity_x + (double)b->linear_velocity_z * b->linear_velocity_z) /
                     256.0 * RA_HZ / RA_M;
                bf = ((bx - x) * A.tx[i0] + (bz - z) * A.tz[i0]) / RA_M;
                bl = ((bx - x) * A.nx[i0] + (bz - z) * A.nz[i0]) / RA_M;
            }
        }
        TD5_LOG_I(LOG_TAG, "ROAM_AI: slot %d IMPACT %.1f -> %.1f km/h at %.0f m along (lat %.2f) | nearest actor slot %d%s at %.1f m "
                  "(%.1f ahead, %.1f across) moving %.1f km/h", c->slot, c->v_prev * 3.6, v * 3.6, c->along, c->lat, bj,
                  bj >= g_traffic_slot_base ? " (traffic)" : "", bd, bf, bl, bv * 3.6);
        ra_ring_dump(c, a, c->v_prev, v);
    }
    c->v_prev = v;
    if (!hold) ra_ring_push(c, a);
#endif

    if (hold) {
        /* grid / 3-2-1: hold the brake, wheel straight */
        a->steering_command = 0; a->encounter_steering_cmd = 0; a->brake_flag = 1; a->handbrake_flag = 0;
        a->throttle_state = 1;
        c->steer = 0; c->thr_i = 0.0; c->still_t = 0;
        return;
    }

    /* ---- target speed: the profile a little ahead of the car, persona, band, racecraft ---- */
    vt = A.pv[ra_path_index(c->s + (v * 0.25 > 2.0 ? v * 0.25 : 2.0))];
    vt *= 0.94 + 0.06 * (double)c->skill;
    if (A.rb_on && !A.player_finished && !c->finished) {
        const int level = td5_ai_get_catchup_level();
        const double gap = A.player_along - c->along;     /* + = I am behind the player */
        if (level > 0) {
            const double pct = level == 1 ? 0.25 : (level == 2 ? 0.50 : 1.0);
            if (gap > 0.0) rb = 1.0 + A.rb_boost * pct * ra_clamp(gap / A.rb_behind_m, 0.0, 1.0);
            else rb = 1.0 - A.rb_trim * (176.0 / 256.0) * ra_clamp(-gap / A.rb_ahead_m, 0.0, 1.0);
        }
    }
    vt *= rb;
    idx = ra_path_index(c->s);
    cap = ra_racecraft(c, x, z, v, idx);
    if (cap < vt) vt = cap;
    if (c->finished) {
        /* run-out: stop at this car's spot past the line */
        const double rem = c->stop_s - c->s;
        const double vs = rem > 0.0 ? sqrt(2.0 * 4.0 * rem) : 0.0;
        if (vs < vt) vt = vs;
    }
    if (vt < 0.0) vt = 0.0;
    c->v_t = vt;

    /* ---- lane: pass offset while committed, else the car's lane; kept on the road ---- */
    {
        double want = c->pass_dwell > 0 ? c->pass_off : c->lane;
        if (c->pass_dwell > 0) c->pass_dwell--;
        L = ra_clamp(A.look0 + A.lookk * v, A.look_min, A.look_max);
        want = ra_lane_fit(c->s + L, want, a->world_pos.y);
        if (want > c->off + 0.06) c->off += 0.06;
        else if (want < c->off - 0.06) c->off -= 0.06;
        else c->off = want;
    }

    /* ---- steering: pure pursuit to the aim point ---- */
    ra_path_point(c->s + L, c->off, &ax, &az, NULL);
    dx = ax - x; dz = az - z; dl = sqrt(dx * dx + dz * dz) / RA_M;
    herr = ra_ang12(ra_angle_of(dx, dz) - heading);
    alpha = (double)herr * (2.0 * RA_PI / 4096.0);
    delta = atan2(2.0 * A.wb * sin(alpha), dl > 1.0 ? dl : 1.0);
    steer_t = (int32_t)(delta * (4096.0 / (2.0 * RA_PI)) * 256.0) - (int32_t)(((long)a->angular_velocity_yaw * A.kd) >> 8);

    /* ---- recovery: wedged (throttle, no motion) or facing away at a crawl -> reverse and retry ---- */
    if (c->cool_t > 0) c->cool_t--;
    if (c->rec == 0 && !c->finished) {
        if (c->thr_i > 0.15 && v < 0.55) c->slow_t++;
        else if (c->slow_t > 0) c->slow_t--;
        if ((c->slow_t > 40 || (abs(herr) > 0x4E0 && v < 2.0 && c->cool_t == 0)) && c->cool_t == 0) {
            /* [F8b] the 4th recovery within 4 m of the last one: reversing does not free it, respawn */
            c->rec_same = (c->recoveries > 0 && fabs(c->along - c->rec_along) < 4.0) ? c->rec_same + 1 : 0;
            c->rec_along = c->along;
            if (c->rec_same >= 3 && ra_respawn(c, a)) return;
            c->rec = 1; c->rec_t = 0; c->slow_t = 0;
            c->recoveries++; A.stats.recoveries++;
            {   /* [F8b] where, and the nearest traffic car (a wedge against traffic or against the street?) */
                double td = -1.0;
                const int ts = td5_roam_traffic_nearest(a->world_pos.x, a->world_pos.z, &td, NULL);
                TD5_LOG_I(LOG_TAG, "ROAM_AI: slot %d recovery %ld at %.0f m along (herr %d, %.1f km/h): reverse | place (%.0f,%.0f) "
                          "nearest traffic slot %d at %.1f m", c->slot, c->recoveries, c->along, herr, v * 3.6, x, z, ts, td);
            }
        }
    }
    if (c->rec == 1) {
        int32_t sc = herr > 0 ? -RA_STEER_CLAMP : RA_STEER_CLAMP;
        if (++c->rec_t > 36) { c->rec = 0; c->cool_t = 45; c->thr_i = 0.3; }
        a->steering_command = sc;
        c->steer = sc;
        a->encounter_steering_cmd = (int16_t)(-0xC0);
        a->brake_flag = 0; a->handbrake_flag = 0; a->throttle_state = 0;
        return;
    }

    {
        long d = (long)steer_t - (long)c->steer;
        if (d > A.rate) d = A.rate;
        if (d < -A.rate) d = -A.rate;
        c->steer = (int32_t)ra_clamp((double)((long)c->steer + d), -RA_STEER_CLAMP, RA_STEER_CLAMP);
    }

    /* ---- throttle / brake: PI on the speed error ---- */
    {
        const double err = vt - v;
        if (err > 0.25) {
            c->thr_i = ra_clamp(c->thr_i + 0.015 * err, 0.0, 1.0);
            thr = (int)(RA_THR_FULL * ra_clamp(0.30 * err + c->thr_i, 0.0, 1.0));
        } else if (err < -1.2 || (err < -0.4 && err < -0.10 * v)) {
            c->thr_i = ra_clamp(c->thr_i - 0.05, 0.0, 1.0);
            thr = 0;
            brake = v > 0.3;
            /* [F8c] brake_flag with a zero command is the drivetrain's IDLE brake (-32 of 256, td5_physics.c AI
             * brake path): the opponents could never shed more than ~4 m/s2. The command now grows with the
             * speed error to the full -256 (what the span AI's stop-and-wait uses), 1.2 m/s over -> the idle
             * brake, 5.2 m/s over or more -> full. TD5RE_ROAM_AI_BRAKE_PROP=0: the old idle brake. */
            if (brake && A.brake_prop)
                thr = -(int)(RA_THR_FULL * ra_clamp(0.125 + 0.875 * (-err - 1.2) / 4.0, 0.125, 1.0));
        } else {
            c->thr_i = ra_clamp(c->thr_i + 0.01 * err, 0.0, 1.0);
            thr = (int)(RA_THR_FULL * ra_clamp(c->thr_i, 0.0, 1.0));
        }
        if (c->finished && vt < 0.3) { thr = 0; brake = 1; }
    }
    /* traction cap (the Driver Model's): back off when the rear slides past the threshold */
    {
        int32_t rs2 = a->rear_axle_slip_excess;
        if (rs2 < 0) rs2 = -rs2;
        if (!brake && thr > 0 && rs2 > 0x600) {
            const double k = ra_clamp(1.0 - (double)(rs2 - 0x600) / (double)0x1200, 0.25, 1.0);
            thr = (int)(thr * k);
        }
    }
    a->steering_command = c->steer;
    a->encounter_steering_cmd = (int16_t)thr;
    a->brake_flag = (uint8_t)brake;
    a->handbrake_flag = 0;
    a->throttle_state = (uint8_t)(brake ? 1 : 0);

    /* braking for a bend ahead (stats, DEV shots) */
    {
        int j, k0 = ra_path_index(c->s), k1 = ra_path_index(c->s + 40.0);
        c->braking_bend = 0;
        if (brake) {
            for (j = k0; j <= k1; j++)
                if (A.pv[j] < 50.0 / 3.6 && A.pv[j] < v - 1.0) { c->braking_bend = 1; c->bend_d = A.ps[j] - c->s; break; }
            if (c->braking_bend) A.stats.brake_ticks++;
        }
    }
}

void td5_roam_ai_tick(void)
{
    int k, rs;
    if (!A.on || !td5_roam_active()) return;
    rs = td5_roam_race_state();
    for (k = 0; k < A.n; k++) {
        RaCar *c = &A.car[k];
        if (!c->live) continue;
        ra_drive(c, !(rs == TD5_RRACE_RACING || rs == TD5_RRACE_FINISHED));
    }
}

/* ============================================================ progress ========= */

static void ra_rank(void)
{
    int i, j, n = 0;
    TD5_RoamStanding t;
    A.st[n].slot = 0; A.st[n].finished = A.player_finished; A.st[n].finish_ticks = A.player_finished ? A.player_fin_ticks : 0;
    A.st[n].along_m = A.player_along; A.st[n].car_index = td5_game_get_slot_car_index(0);
    n++;
    for (i = 0; i < A.n; i++) {
        const RaCar *c = &A.car[i];
        if (!c->live) continue;
        A.st[n].slot = c->slot; A.st[n].finished = c->finished; A.st[n].finish_ticks = c->finish_ticks;
        A.st[n].along_m = c->along; A.st[n].car_index = c->car_index;
        n++;
    }
    if (A.circ) {
        for (i = 0; i < n; i++) {
            int l = A.st[i].finished ? A.laps : (int)floor((A.st[i].along_m > 0.0 ? A.st[i].along_m : 0.0) / A.lap_len);
            A.st[i].laps_done = l > A.laps ? A.laps : l;
        }
    }
    /* insertion sort: finished first by time, then by distance along (a circuit: total, i.e. laps first) */
    for (i = 1; i < n; i++) {
        t = A.st[i];
        j = i - 1;
        while (j >= 0) {
            const TD5_RoamStanding *o = &A.st[j];
            int before;
            if (t.finished != o->finished) before = t.finished;
            else if (t.finished) before = t.finish_ticks < o->finish_ticks;
            else before = t.along_m > o->along_m;
            if (!before) break;
            A.st[j + 1] = A.st[j];
            j--;
        }
        A.st[j + 1] = t;
    }
    A.nst = n;
    for (i = 0; i < n; i++) {
        TD5_Actor *a = td5_game_get_actor(A.st[i].slot);
        if (A.st[i].slot == 0) A.player_rank = i + 1;
        if (a) { a->prev_race_position = a->race_position; a->race_position = (uint8_t)i; }
    }
}

void td5_roam_ai_progress(long race_ticks, int racing, double player_along, int player_finished, long player_finish_ticks)
{
    int k;
    if (!A.on || !td5_roam_active()) return;
    A.race_ticks = race_ticks;
    A.racing = racing;
    A.player_along = player_along;
    if (player_finished && !A.player_finished) {
        A.player_finished = 1;
        A.player_fin_ticks = player_finish_ticks;
    }
    for (k = 0; k < A.n; k++) {
        RaCar *c = &A.car[k];
        TD5_Actor *a = td5_game_get_actor(c->slot);
        double x, z, lat_m, v;
        if (!c->live || !a || !ra_place_of(a, &x, &z)) continue;
        if (A.circ) {
            /* [F9a] the TOTAL distance of td5_roam_lap.h: continuous over the line, never double counted */
            td5_roam_lap_update(&A.loop, &c->lt, x, z, RA_LOST_M);
            c->along = c->lt.total;
            lat_m = c->lt.lat_m;
        } else {
            c->along = ra_race_along(c, x, z, &lat_m);
        }
        c->lat_race = lat_m;
        if (A.circ && c->lt.init) {
            const int now = td5_roam_lap_count(&A.loop, &c->lt, 0.0);
            if (now > c->laps_done + 1) {
                A.stats.lap_jumps++;
                TD5_LOG_W(LOG_TAG, "ROAM_AI: slot %d lap count jumped %d -> %d (total %.1f m)", c->slot, c->laps_done, now, c->lt.total);
            }
            if (now > c->laps_done) c->laps_done = now;
        }
        if (racing && !c->finished && lat_m > A.stats.max_lateral_m) A.stats.max_lateral_m = lat_m;   /* not the run-out */
        v = sqrt((double)a->linear_velocity_x * a->linear_velocity_x + (double)a->linear_velocity_z * a->linear_velocity_z) /
            256.0 * RA_HZ / RA_M;
        if (racing && !c->finished) {
            if (v < RA_STILL_MS) {
                c->still_t++;
                if (c->still_t == RA_STUCK_LONG + 1) {
                    A.stats.stuck_long++;
                    TD5_LOG_W(LOG_TAG, "ROAM_AI: slot %d STUCK > 5 s at %.0f m along (lat %.1f m)", c->slot, c->along, lat_m);
                }
            } else {
                c->still_t = 0;
            }
            if (c->still_t > c->still_max) c->still_max = c->still_t;
            if (c->still_t > A.stats.still_max_ticks) A.stats.still_max_ticks = c->still_t;
            if ((A.circ ? c->lt.max_total : c->along) >= A.length - RA_FINISH_EPS && lat_m < RA_CP_RADIUS) {
                c->finished = 1;
                c->finish_ticks = race_ticks;
                c->order = A.nfin++;
                /* stop 30..70 m past the line, the first finisher farthest. A finished opponent is no
                 * longer racing: the route barriers let it through (td5_roam_ai_slot_finished), the
                 * closing panel past FINISH being ~40 m on, closer than a car stops from 100 km/h */
                c->stop_s = (A.circ ? A.path_lap_len * (double)A.laps : A.length) + 30.0 + 10.0 * (double)(A.n - 1 - c->order);
                A.stats.finished++;
                TD5_LOG_I(LOG_TAG, "ROAM_AI: slot %d FINISH #%d at %ld ticks (%.2f s), recoveries %ld, longest still %.1f s",
                          c->slot, c->order + 1, race_ticks, race_ticks / RA_HZ, c->recoveries, c->still_max / RA_HZ);
            }
        }
    }
    ra_rank();
    if (A.trc && racing) {
        for (k = 0; k < A.n; k++) {
            const RaCar *c = &A.car[k];
            const TD5_Actor *a = td5_game_get_actor(c->slot);
            int r, rank = 0;
            if (!a) continue;
            for (r = 0; r < A.nst; r++) if (A.st[r].slot == c->slot) rank = r + 1;
            fprintf(A.trc, "%ld,%d,%.1f,%.1f,%.2f,%.1f,%.1f,%.2f,%d,%d,%d,%d,%d\n", race_ticks, c->slot, c->s, c->along, c->lat,
                    sqrt((double)a->linear_velocity_x * a->linear_velocity_x + (double)a->linear_velocity_z * a->linear_velocity_z) /
                        256.0 * RA_HZ / RA_M * 3.6,
                    c->v_t * 3.6, c->off, (int)a->steering_command, (int)a->encounter_steering_cmd, (int)a->brake_flag, c->rec, rank);
        }
    }
}

/* ============================================================ queries ========== */

int td5_roam_ai_standings(TD5_RoamStanding *out, int max)
{
    int i;
    if (!A.on || A.nst <= 0) return 0;
    for (i = 0; i < A.nst && i < max; i++) out[i] = A.st[i];
    return i;
}

int td5_roam_ai_player_rank(void) { return A.on ? A.player_rank : 0; }

void td5_roam_ai_stats(TD5_RoamAiStats *out)
{
    if (out) *out = A.stats;
}

int td5_roam_ai_car_info(int k, int *slot, long *finish_ticks, long *still_max, long *recoveries)
{
    if (k < 0 || k >= A.n) return 0;
    if (slot) *slot = A.car[k].slot;
    if (finish_ticks) *finish_ticks = A.car[k].finished ? A.car[k].finish_ticks : 0;
    if (still_max) *still_max = A.car[k].still_max;
    if (recoveries) *recoveries = A.car[k].recoveries;
    return 1;
}

void td5_roam_ai_set_circuit(int laps, double lap_len_m)
{
    A.circ_req = laps > 0 ? laps : 0;
    A.circ_req_len = lap_len_m;
}

int td5_roam_ai_car_laps(int k) { return (A.circ && k >= 0 && k < A.n) ? A.car[k].laps_done : 0; }

void td5_roam_ai_force_player_spot(int spot) { A.grid_force = spot < 0 ? 0 : spot + 1; }

int td5_roam_ai_player_grid(int32_t *px, int32_t *pz, int *yaw12)
{
    if (!A.on) return 0;
    *px = (int32_t)floor(A.ppx + 0.5); *pz = (int32_t)floor(A.ppz + 0.5); *yaw12 = A.pyaw;
    return 1;
}

int td5_roam_ai_slot_finished(int slot)
{
    int k;
    if (!A.on) return 0;
    for (k = 0; k < A.n; k++)
        if (A.car[k].slot == slot) return A.car[k].finished;
    return 0;
}

int td5_roam_ai_braking_for_bend(int slot, double m)
{
    int k;
    if (!A.on) return 0;
    for (k = 0; k < A.n; k++)
        if (A.car[k].slot == slot) return A.car[k].braking_bend && A.car[k].bend_d <= m;
    return 0;
}
