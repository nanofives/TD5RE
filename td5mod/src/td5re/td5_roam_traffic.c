/**
 * td5_roam_traffic.c -- GEO FREE ROAM: ambient traffic on the street graph (PORT-ONLY, F8).
 * Contract and design: td5_roam_traffic.h.
 *
 * Sections: knobs + helpers / graph geometry (lanes, connectors) / pool + assets / spawn + despawn /
 * the driver (path, signals, yield, gap, steering, speed) / tick / stats.
 *
 * FRAMES. The graph is in the PLACE frame (430 units per metre, doubles); an actor's place position is
 * world / 256 + the world origin. Speeds are m/s inside this file; the actor's linear velocity is 24.8
 * world units per sim tick (30 Hz).
 *
 * DETERMINISM. Each car draws from its own LCG, seeded from (slot, spawn count); the spawner draws from
 * the session LCG. No wall clock, no pointer order: a run with the same input does the same thing.
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
#include "td5_physics.h"
#include "td5_render.h"
#include "td5_geo_signals.h"
#include "td5_roam_mode.h"
#include "td5_roam_locate.h"
#include "td5_roam_physics.h"
#include "td5_roam_route.h"
#include "td5_roam_race.h"
#include "td5_roam_ai.h"
#include "td5_roam_barrier.h"
#include "td5_roam_traffic.h"
#include "../../../re/include/td5_actor_struct.h"

#if defined(_WIN32) && !defined(TD5RE_RELEASE)
#include <windows.h>
#include <psapi.h>
#endif

#define LOG_TAG "roam"

#define RT_M           430.0                     /* place units per metre               */
#define RT_HZ          30.0
#define RT_PI          3.14159265358979323846
#define RT_LANE_W      1500.0                    /* one lane, units (TD5_ROAM_LANE_W)   */
#define RT_STEER_CLAMP 0x18000
#define RT_FADE_TICKS  20
#define RT_STOP_BACK_M 2.8                       /* car centre this far before the stop line */
#define RT_STILL_MS    (1.0 / 3.6)               /* "still" under 1 km/h                */
#define RT_STUCK_TICKS 300                       /* 10 s                                */
#define RT_HOLD_TICKS  1200                      /* 40 s holding with a reason          */
#define RT_OFF_TICKS   30                        /* 1 s off the surface                 */
#define RT_WARMUP      300                       /* live-count stats start after 10 s   */

enum { RT_PARKED = 0, RT_LIVE = 1 };
enum { RT_SEG_EDGE = 0, RT_SEG_CONN = 1 };
enum { RT_WHY_FAR = 1, RT_WHY_DEAD, RT_WHY_STUCK, RT_WHY_HOLD, RT_WHY_OFF, RT_WHY_UNRES, RT_WHY_LEVEL, RT_WHY_LOST };

typedef struct {
    int      slot, state;
    int      alpha, fade;                        /* fade +1 in, -1 out, 0 steady           */
    int      why_out;
    uint32_t rng;
    /* the path */
    int      seg;                                /* RT_SEG_*                               */
    int      e, d, k;                            /* edge, travel dir (0 = node0->node1), lane */
    double   off, off_t;                         /* lane offset now / target, units (+ right) */
    double   s;                                  /* units along the segment, travel order  */
    double   lat_m;                              /* offset from the lane line, metres      */
    int      ne, nd, nk;                         /* the next edge (-1 = dead end)          */
    int      node;                               /* node at the end of the current edge    */
    int      turn;                               /* -1 left, 0 straight, +1 right          */
    double   bz[4][2], clen, v_conn;             /* the connector to the next edge         */
    /* the junction ahead */
    int      sig_n, sig_g;                       /* signalised: group (0/1), -1 = no signal */
    int      node_deg, priority;                 /* arms at the node; 1 = my street outranks the rest */
    int      stop_req;                           /* decided to stop for this phase         */
    int      committed;                          /* passed the decision point              */
    int      yield_node; long wait_since; int waiting;
    long     commit_tick; int commit_ph, commit_pa; double commit_tarr, commit_dstop, commit_v;   /* DEV log */
    long     sr_tick; double sr_d, sr_v, sr_vt; int sr_ph;                                   /* DEV log: stop decided */
    /* control */
    double   v_class;                            /* cruise, m/s                            */
    int32_t  steer;
    int      holding;                            /* holding with a reason this tick        */
    long     still_t, hold_t, off_t_ticks, born, last_contact, lost_t;
    int      lane_chk;
    int      bar_k, bar_side;                    /* the barrier the car is beside, and its side */
} RtCar;

static struct {
    int     on;                                  /* a roam session with a pool             */
    int     pool, base, loaded;
    int     ready;                               /* planner graph seen ready               */
    RtCar   car[TD5_ROAM_TRAFFIC_POOL_MAX];
    uint32_t rng;
    long    spawn_count, tick_last;
    int     force_level, force_density;
    /* knobs */
    int     knobs;
    int     dens_free, dens_low, dens_high, max_live;
    double  r_spawn_min, r_spawn_max, r_count, r_despawn;
    double  wb, a_lat, a_brake, speed_pct;
    int     log_every;
    TD5_RoamTrafficStats st;
    double  live_sum;
    long    st_t0;                               /* tick of the last stats reset      */
    int     bar_valid;
    int     shots, shot_done[3]; long shot_wait; char shot_name[24];   /* DEV shots */
    int     trace_slot; FILE *trc;                                  /* DEV: TD5RE_ROAM_TRAFFIC_TRACE=<slot> */
    /* DEV soak measures: private bytes after the warm-up / now / max, the tick's own cost */
    double  priv0_mb, priv_mb, priv_max_mb; int priv_n;
    double  tick_us_sum, tick_us_max; long tick_n;
} T;

/* ============================================================ helpers ============ */

static double rt_clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int rt_ang12(int a) { a &= 0xFFF; if (a > 0x800) a -= 0x1000; return a; }
static int rt_angle_of(double dx, double dz)
{
    if (dx == 0.0 && dz == 0.0) return 0;
    return (int)floor(atan2(dx, dz) * (4096.0 / (2.0 * RT_PI)) + 0.5) & 0xFFF;
}
static uint32_t rt_lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
static double rt_unit(uint32_t *s) { return (double)(rt_lcg(s) & 0xFFFFu) / 65536.0; }

static void rt_read_knobs(void)
{
    if (T.knobs) return;
    T.knobs = 1;
    T.dens_free   = td5_env_int("TD5RE_ROAM_TRAFFIC", 14, 0, TD5_ROAM_TRAFFIC_POOL_MAX);
    T.dens_low    = td5_env_int("TD5RE_ROAM_TRAFFIC_LOW", 4, 0, TD5_ROAM_TRAFFIC_POOL_MAX);
    T.dens_high   = td5_env_int("TD5RE_ROAM_TRAFFIC_HIGH", 8, 0, TD5_ROAM_TRAFFIC_POOL_MAX);
    T.max_live    = td5_env_int("TD5RE_ROAM_TRAFFIC_MAX", TD5_ROAM_TRAFFIC_POOL_MAX, 0, TD5_ROAM_TRAFFIC_POOL_MAX);
    T.r_spawn_min = td5_env_int("TD5RE_ROAM_TRAFFIC_SPAWN_MIN_M", 110, 20, 400);
    T.r_spawn_max = td5_env_int("TD5RE_ROAM_TRAFFIC_SPAWN_MAX_M", 210, 40, 500);
    T.r_count     = td5_env_int("TD5RE_ROAM_TRAFFIC_COUNT_M", 250, 50, 600);
    T.r_despawn   = td5_env_int("TD5RE_ROAM_TRAFFIC_DESPAWN_M", 280, 60, 700);
    T.wb          = td5_env_int("TD5RE_ROAM_TRAFFIC_WB", 26, 10, 60) / 10.0;          /* m x10   */
    T.a_lat       = td5_env_int("TD5RE_ROAM_TRAFFIC_ALAT", 28, 5, 100) / 10.0;        /* m/s2 x10 */
    T.a_brake     = td5_env_int("TD5RE_ROAM_TRAFFIC_BRAKE", 30, 5, 100) / 10.0;       /* m/s2 x10 */
    T.speed_pct   = td5_env_int("TD5RE_ROAM_TRAFFIC_SPEED", 100, 20, 300) / 100.0;
    T.log_every   = td5_env_int("TD5RE_ROAM_TRAFFIC_LOG", 0, 0, 3600);
#ifndef TD5RE_RELEASE
    T.shots       = td5_env_int("TD5RE_ROAM_TRAFFIC_SHOTS", 0, 0, 1);
    T.trace_slot  = td5_env_int("TD5RE_ROAM_TRAFFIC_TRACE", -1, -1, 31);
    {   /* DEV: force the race level (the sprint test harness has no setup panel) */
        const int fl = td5_env_int("TD5RE_ROAM_TRAFFIC_RACE", -1, -1, 2);
        if (fl >= 0 && !T.force_level) T.force_level = fl + 1;
    }
#endif
    if (T.r_spawn_max < T.r_spawn_min + 20.0) T.r_spawn_max = T.r_spawn_min + 20.0;
    if (T.r_despawn < T.r_spawn_max + 30.0) T.r_despawn = T.r_spawn_max + 30.0;
}

static int rt_place_of(const TD5_Actor *a, double *px, double *pz)
{
    int32_t ox, oz;
    if (!a) return 0;
    td5_roam_world_origin(&ox, &oz);
    *px = (double)a->world_pos.x / 256.0 + (double)ox;
    *pz = (double)a->world_pos.z / 256.0 + (double)oz;
    return 1;
}

/* locator at a PLACE point: 1 when a resident tile covers it; *inside = on the drivable surface */
static int rt_locate(double px, double pz, int32_t y, int has_y, int *inside, int32_t *gy)
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

static double rt_speed_ms(const TD5_Actor *a)
{
    return sqrt((double)a->linear_velocity_x * a->linear_velocity_x + (double)a->linear_velocity_z * a->linear_velocity_z) /
           256.0 * RT_HZ / RT_M;
}

/* ============================================================ the signal group ==== */

int td5_roam_signal_group(const int16_t *bear, int n, int a)
{
    int d;
    if (!bear || n <= 0 || a <= 0 || a >= n) return 0;
    d = ((int)bear[a] - (int)bear[0]) & 0x7FF;      /* the axis: modulo half a turn */
    if (d > 0x400) d = 0x800 - d;
    return d > 0x200 ? 1 : 0;                        /* more than 45 deg off arm 0's axis */
}

/* ============================================================ graph geometry ====== */

static int rt_edge(int e, TD5_RouteGEdge *E) { return td5_roam_route_graph_edge_view(e, E) && E->npt >= 2 && E->plen > 1.0; }

static int rt_allowed(const TD5_RouteGEdge *E, int d)
{
    if (E->oneway > 0) return d == 0;
    if (E->oneway < 0) return d == 1;
    return 1;
}

/* lanes in the travel direction */
static int rt_dir_lanes(const TD5_RouteGEdge *E)
{
    const int L = E->lanes < 1 ? 1 : E->lanes;
    if (E->oneway) return L;
    return L >= 2 ? L / 2 : 1;
}

/* the lateral offset of lane k (0 = innermost / leftmost of the direction), units, + = right of travel */
static double rt_lane_off(const TD5_RouteGEdge *E, int k)
{
    const int L = E->lanes < 1 ? 1 : E->lanes;
    const int n = rt_dir_lanes(E);
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    if (E->oneway) return ((double)k + 0.5 - (double)L * 0.5) * RT_LANE_W;
    if (L < 2) return 0.3 * RT_LANE_W;
    return ((L & 1) ? 0.5 * RT_LANE_W : 0.0) + ((double)k + 0.5) * RT_LANE_W;
}

/* polyline point at arc a (node0 order) + the node0->node1 unit tangent of its segment */
static void rt_poly_at(const TD5_RouteGEdge *E, double a, double *x, double *z, double *tx, double *tz, int *seg)
{
    int lo = 0, hi = E->npt - 2;
    double l, t, dx, dz;
    if (a < 0.0) a = 0.0;
    if (a > E->plen) a = E->plen;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (E->cum[mid] <= a) lo = mid; else hi = mid - 1;
    }
    dx = (double)E->pt[2 * lo + 2] - E->pt[2 * lo];
    dz = (double)E->pt[2 * lo + 3] - E->pt[2 * lo + 1];
    l = E->cum[lo + 1] - E->cum[lo];
    t = l > 0.0 ? (a - E->cum[lo]) / l : 0.0;
    t = rt_clamp(t, 0.0, 1.0);
    *x = E->pt[2 * lo] + t * dx;
    *z = E->pt[2 * lo + 1] + t * dz;
    l = sqrt(dx * dx + dz * dz);
    if (l > 0.0) { *tx = dx / l; *tz = dz / l; } else { *tx = 0.0; *tz = 1.0; }
    if (seg) *seg = lo;
}

/* the point s units along TRAVEL on edge E, offset `off` units to the right of travel */
static void rt_edge_pt(const TD5_RouteGEdge *E, int d, double s, double off, double *x, double *z, double *tx, double *tz)
{
    double px, pz, ux, uz;
    rt_poly_at(E, d == 0 ? s : E->plen - s, &px, &pz, &ux, &uz, NULL);
    if (d) { ux = -ux; uz = -uz; }
    *x = px + uz * off;
    *z = pz - ux * off;
    if (tx) { *tx = ux; *tz = uz; }
}

/* project a PLACE point onto edge E near travel-s `hint`: travel s, signed lateral (units, + right) */
static double rt_edge_proj(const TD5_RouteGEdge *E, int d, double hint, double px, double pz, double *lat)
{
    double hx, hz, ux, uz, best = 1e300, bs = hint, blat = 0.0;
    int seg, j, j0, j1;
    rt_poly_at(E, d == 0 ? hint : E->plen - hint, &hx, &hz, &ux, &uz, &seg);
    j0 = seg - 4 < 0 ? 0 : seg - 4;
    j1 = seg + 4 > E->npt - 2 ? E->npt - 2 : seg + 4;
    for (j = j0; j <= j1; j++) {
        const double ax = E->pt[2 * j], az = E->pt[2 * j + 1];
        const double dx = (double)E->pt[2 * j + 2] - ax, dz = (double)E->pt[2 * j + 3] - az;
        const double l2 = dx * dx + dz * dz;
        double t = l2 > 0.0 ? ((px - ax) * dx + (pz - az) * dz) / l2 : 0.0, qx, qz, dd;
        t = rt_clamp(t, 0.0, 1.0);
        qx = ax + t * dx; qz = az + t * dz;
        dd = (px - qx) * (px - qx) + (pz - qz) * (pz - qz);
        if (dd < best) {
            const double l = sqrt(l2 > 0.0 ? l2 : 1.0);
            const double a = E->cum[j] + t * (E->cum[j + 1] - E->cum[j]);
            double tx = dx / l, tz = dz / l;
            best = dd;
            bs = d == 0 ? a : E->plen - a;
            if (d) { tx = -tx; tz = -tz; }
            blat = (px - qx) * tz - (pz - qz) * tx;      /* right = (tz, -tx) */
        }
    }
    if (lat) *lat = blat;
    return bs;
}

static void rt_bez(const double (*b)[2], double t, double *x, double *z, double *tx, double *tz)
{
    const double u = 1.0 - t;
    const double w0 = u * u * u, w1 = 3.0 * u * u * t, w2 = 3.0 * u * t * t, w3 = t * t * t;
    *x = w0 * b[0][0] + w1 * b[1][0] + w2 * b[2][0] + w3 * b[3][0];
    *z = w0 * b[0][1] + w1 * b[1][1] + w2 * b[2][1] + w3 * b[3][1];
    if (tx) {
        double dx = 3.0 * u * u * (b[1][0] - b[0][0]) + 6.0 * u * t * (b[2][0] - b[1][0]) + 3.0 * t * t * (b[3][0] - b[2][0]);
        double dz = 3.0 * u * u * (b[1][1] - b[0][1]) + 6.0 * u * t * (b[2][1] - b[1][1]) + 3.0 * t * t * (b[3][1] - b[2][1]);
        const double l = sqrt(dx * dx + dz * dz);
        if (l > 1e-9) { dx /= l; dz /= l; } else { dx = 0.0; dz = 1.0; }
        *tx = dx; *tz = dz;
    }
}

static double rt_class_speed(int klass);

/* ============================================================ the next edge ======= */

/* node at the travel end of (e, d) */
static int rt_end_node(const TD5_RouteGEdge *E, int d) { return E->node[d == 0 ? 1 : 0]; }

/* the lane a car takes on the next edge for a turn */
static int rt_next_lane(const TD5_RouteGEdge *E, const TD5_RouteGEdge *F, int k, int turn)
{
    const int n2 = rt_dir_lanes(F), n1 = rt_dir_lanes(E);
    int nk;
    if (turn > 0) nk = n2 - 1;
    else if (turn < 0) nk = 0;
    else nk = (int)floor((double)k * (double)n2 / (double)(n1 > 0 ? n1 : 1) + 0.5);
    if (nk >= n2) nk = n2 - 1;
    if (nk < 0) nk = 0;
    return nk;
}

/* The connector from the end of lane `off0` of (E, d) to lane `nk` of (F, nd): a cubic, its length and
 * speed. Returns the samples (of 9, body edges +-0.8 m included) found off the drivable surface. */
static int rt_build_conn(RtCar *c, const TD5_RouteGEdge *E, int d, double off0, const TD5_RouteGEdge *F, int nd, int nk,
                         double v_class)
{
    double p0x, p0z, t0x, t0z, p3x, p3z, t3x, t3z, ch, h, th, lx, lz;
    int j, off = 0;
    rt_edge_pt(E, d, E->plen, off0, &p0x, &p0z, &t0x, &t0z);
    rt_edge_pt(F, nd, 0.0, rt_lane_off(F, nk), &p3x, &p3z, &t3x, &t3z);
    ch = hypot(p3x - p0x, p3z - p0z);
    h = ch * 0.38;
    c->bz[0][0] = p0x; c->bz[0][1] = p0z;
    c->bz[1][0] = p0x + t0x * h; c->bz[1][1] = p0z + t0z * h;
    c->bz[2][0] = p3x - t3x * h; c->bz[2][1] = p3z - t3z * h;
    c->bz[3][0] = p3x; c->bz[3][1] = p3z;
    c->clen = 0.0;
    lx = p0x; lz = p0z;
    for (j = 1; j <= 12; j++) {
        double x, z;
        rt_bez((const double (*)[2])c->bz, (double)j / 12.0, &x, &z, NULL, NULL);
        c->clen += hypot(x - lx, z - lz);
        lx = x; lz = z;
    }
    th = fabs(atan2(t0x * t3z - t0z * t3x, t0x * t3x + t0z * t3z));
    if (th > 0.17 && ch > 1.0) {
        const double R = (ch / RT_M) / (2.0 * sin(th * 0.5));
        c->v_conn = rt_clamp(sqrt(T.a_lat * R), 3.5, v_class);
    } else c->v_conn = v_class;
    if (c->clen > 1.0 * RT_M)
    {
        /* one sample every ~2.5 m (at least 9) */
        int ns = (int)(c->clen / (2.5 * RT_M));
        if (ns < 9) ns = 9;
        if (ns > 40) ns = 40;
        for (j = 1; j <= ns; j++) {
            double x, z, tx, tz;
            int in0 = 0, in1 = 0, in2 = 0;
            rt_bez((const double (*)[2])c->bz, (double)j / (double)(ns + 1), &x, &z, &tx, &tz);
            if (!rt_locate(x, z, 0, 0, &in0, NULL) || !in0) { off++; continue; }
            rt_locate(x + tz * 0.8 * RT_M, z - tx * 0.8 * RT_M, 0, 0, &in1, NULL);
            rt_locate(x - tz * 0.8 * RT_M, z + tx * 0.8 * RT_M, 0, 0, &in2, NULL);
            if (!in1 || !in2) off++;
        }
    }
    return off;
}

/* Pick the next edge at the travel end of the car's edge: straight preferred, one-way respected, no
 * U-turn (a dead end returns -1), and only a turn whose connector stays on the paved patch (when none
 * does, the one that leaves it least). Builds the connector and the junction-ahead state. */
static void rt_plan_next(RtCar *c)
{
    TD5_RouteGEdge E, F;
    TD5_RouteGNode N;
    double ex, ez, etx, etz, w[16], wsum = 0.0, r;
    int cand_e[16], cand_d[16], cand_turn[16], cand_off[16], nc = 0, i, pick = -1, min_off = 1 << 30;
    c->ne = -1; c->nd = 0; c->nk = 0; c->turn = 0; c->clen = 0.0; c->v_conn = c->v_class;
    c->sig_n = -1; c->sig_g = -1; c->stop_req = 0; c->committed = 0; c->yield_node = -1; c->waiting = 0;
    if (!rt_edge(c->e, &E)) return;
    c->node = rt_end_node(&E, c->d);
    if (!td5_roam_route_graph_node_view(c->node, &N)) return;
    c->node_deg = N.ninc;
    c->priority = 0;
    rt_edge_pt(&E, c->d, E.plen, 0.0, &ex, &ez, &etx, &etz);
    {   /* priority: my street's class above every other street at the node (secondary and up) */
        int mx = 0;
        for (i = 0; i < N.ninc; i++) {
            const int e2 = N.inc[i] >> 1;
            if (e2 != c->e && rt_edge(e2, &F) && F.klass > mx) mx = F.klass;
        }
        c->priority = E.klass >= 6 && E.klass > mx;
    }
    for (i = 0; i < N.ninc && nc < 16; i++) {
        const int inc = N.inc[i];
        const int e2 = inc >> 1, end = inc & 1;
        const int d2 = end == 0 ? 0 : 1;         /* leaving from node0 = along node0->node1 */
        double sx, sz, tx, tz, cr, dt, ang;
        if (e2 == c->e) continue;                /* no U-turn on the same street piece */
        if (!rt_edge(e2, &F) || !rt_allowed(&F, d2)) continue;
        rt_edge_pt(&F, d2, 0.0, 0.0, &sx, &sz, &tx, &tz);
        cr = etx * tz - etz * tx;
        dt = etx * tx + etz * tz;
        ang = atan2(cr, dt);                     /* < 0 = a right turn (x east, z north) */
        cand_e[nc] = e2; cand_d[nc] = d2;
        cand_turn[nc] = fabs(ang) < 0.55 ? 0 : (ang < 0.0 ? 1 : -1);
        w[nc] = cand_turn[nc] == 0 ? 4.0 : (cand_turn[nc] > 0 ? 2.0 : 1.5);
        if (fabs(ang) > 2.4) w[nc] = 0.25;       /* a hairpin: rare */
        if (F.klass <= 2) w[nc] *= 0.35;         /* service / living streets: rarely */
        cand_off[nc] = rt_build_conn(c, &E, c->d, c->off_t, &F, d2, rt_next_lane(&E, &F, c->k, cand_turn[nc]),
                                     rt_class_speed(F.klass));
        if (cand_off[nc] < min_off) min_off = cand_off[nc];
        nc++;
    }
    if (nc == 0) return;                         /* dead end */
    if (min_off > 0) {
        /* every way on leaves the paved patch: a dead end for traffic (the car is removed before it) */
        T.st.conn_off_plans++;
        return;
    }
    for (i = 0; i < nc; i++) { if (cand_off[i] != min_off) w[i] = 0.0; wsum += w[i]; }
    r = rt_unit(&c->rng) * wsum;
    for (i = 0; i < nc; i++) { if (w[i] <= 0.0) continue; r -= w[i]; if (r <= 0.0) { pick = i; break; } }
    if (pick < 0) for (i = nc - 1; i >= 0; i--) if (cand_off[i] == min_off) { pick = i; break; }
    c->ne = cand_e[pick]; c->nd = cand_d[pick]; c->turn = cand_turn[pick];
    rt_edge(c->ne, &F);
    c->nk = rt_next_lane(&E, &F, c->k, c->turn);
    rt_build_conn(c, &E, c->d, c->off_t, &F, c->nd, c->nk, c->v_class);
    /* the junction ahead: signal group of my arm */
    {
        int16_t bear[16];
        uint64_t ek[16];
        const int na = td5_roam_route_graph_signal(N.key, bear, ek, 16);
        if (na > 0 && N.ninc >= 2) {
            int a, arm = -1, best = 0x7FFF;
            const int ob = (int)floor(atan2(-etz, -etx) * (4096.0 / (2.0 * RT_PI)) + 0.5) & 0xFFF;   /* outward, atan2(dz,dx) */
            for (a = 0; a < na; a++) if (ek[a] == E.key) { arm = a; break; }
            if (arm < 0)
                for (a = 0; a < na; a++) {
                    const int dd = abs(rt_ang12((int)bear[a] - ob));
                    if (dd < best) { best = dd; arm = a; }
                }
            c->sig_n = c->node;
            c->sig_g = td5_roam_signal_group(bear, na, arm < 0 ? 0 : arm);
        }
    }
}

/* the point L units ahead of the car's path position (connector + next edge included) */
static void rt_ahead(RtCar *c, double L, double *x, double *z, double *tx, double *tz)
{
    TD5_RouteGEdge E, F;
    double rem = L;
    if (c->seg == RT_SEG_EDGE) {
        if (!rt_edge(c->e, &E)) { *x = *z = 0.0; *tx = 0.0; *tz = 1.0; return; }
        if (c->s + rem <= E.plen || c->ne < 0) { rt_edge_pt(&E, c->d, c->s + rem, c->off, x, z, tx, tz); return; }
        rem -= E.plen - c->s;
        if (rem <= c->clen && c->clen > 0.0) { rt_bez((const double (*)[2])c->bz, rem / c->clen, x, z, tx, tz); return; }
        rem -= c->clen;
    } else {
        if (c->s + rem <= c->clen) { rt_bez((const double (*)[2])c->bz, (c->s + rem) / c->clen, x, z, tx, tz); return; }
        rem -= c->clen - c->s;
    }
    if (c->ne < 0 || !rt_edge(c->ne, &F)) { rt_bez((const double (*)[2])c->bz, 1.0, x, z, tx, tz); return; }
    rt_edge_pt(&F, c->nd, rem, rt_lane_off(&F, c->nk), x, z, tx, tz);
}

/* ============================================================ pool + assets ======= */

int td5_roam_traffic_pool(void)
{
    if (!td5_roam_active()) return 0;
    return td5_env_int("TD5RE_ROAM_TRAFFIC_POOL", TD5_ROAM_TRAFFIC_POOL_MAX, 0, TD5_ROAM_TRAFFIC_POOL_MAX);
}

void td5_roam_traffic_load_assets(void)
{
    static const int k_models[5] = { 0, 1, 2, 4, 5 };   /* pool slot 3 is the track's police car */
    const int pool = td5_roam_traffic_pool();
    const int host = g_td5.track_index;
    int ti, loaded = 0, from_host = 0;
    td5_render_set_cop_mesh(NULL);
    T.loaded = 0;
    for (ti = 0; ti < pool; ti++) {
        const int slot = g_traffic_slot_base + ti;
        const int ms = k_models[ti % 5];
        int model = td5_asset_resolve_traffic_model_index(host, 0, ms);
        if (model >= 0) from_host++;
        else model = td5_asset_resolve_traffic_model_index(0, 0, ms);   /* the host has no row: track 0's set */
        if (model < 0) continue;
        if (td5_asset_load_traffic_model(model, slot)) {
            td5_physics_seed_traffic_cardef_from_player(slot);
            loaded++;
        } else {
            TD5_LOG_W(LOG_TAG, "ROAM_TRAFFIC: slot %d model %d failed to load", slot, model);
        }
    }
    T.loaded = loaded;
    TD5_LOG_I(LOG_TAG, "ROAM_TRAFFIC: assets: %d/%d pool slots loaded (base %d, %d from the host level's row, rest track 0's)",
              loaded, pool, g_traffic_slot_base, from_host);
}

int td5_roam_traffic_field_total(void)
{
    if (!T.on || T.pool <= 0) return 0;
    return T.base + T.pool;
}

static void rt_park(RtCar *c)
{
    TD5_Actor *a = td5_game_get_actor(c->slot);
    c->state = RT_PARKED;
    c->alpha = 0; c->fade = 0;
    if (a) {
        a->linear_velocity_x = a->linear_velocity_z = a->linear_velocity_y = 0;
        a->angular_velocity_yaw = 0;
        a->steering_command = 0;
        a->encounter_steering_cmd = 0;
        a->brake_flag = 1;
    }
}

void td5_roam_traffic_session_init(void)
{
    int i;
    const int keep_force_l = T.force_level, keep_force_d = T.force_density, keep_loaded = T.loaded;
    memset(&T, 0, sizeof T);   /* knobs are re-read below (T.knobs cleared) */
    T.force_level = keep_force_l; T.force_density = keep_force_d;
    T.loaded = td5_roam_active() ? keep_loaded : 0;   /* td5_roam_traffic_load_assets ran this race init */
    if (!td5_roam_active()) return;
    rt_read_knobs();
    T.base = g_traffic_slot_base;
    T.pool = td5_roam_traffic_pool();
    if (T.pool > TD5_ROAM_TRAFFIC_POOL_MAX) T.pool = TD5_ROAM_TRAFFIC_POOL_MAX;
    if (T.base + T.pool > 32) T.pool = 32 - T.base;        /* the roam physics side table */
    if (T.pool > 0 && T.loaded <= 0) {
        TD5_LOG_W(LOG_TAG, "ROAM_TRAFFIC: no traffic car loaded: traffic off");
        T.pool = 0;
    }
    T.on = T.pool > 0;
    T.rng = 0x5EED0F08u;
    for (i = 0; i < T.pool; i++) {
        T.car[i].slot = T.base + i;
        rt_park(&T.car[i]);
    }
    T.st.pool = T.pool;
    T.st.live_min = 1 << 30;
    td5_game_roam_set_racer_field(td5_roam_ai_count());      /* the field now spans the pool */
    TD5_LOG_I(LOG_TAG, "ROAM_TRAFFIC: session: pool %d (slots %d..%d), density free %d / race low %d high %d per anchor, max %d, "
              "ring %.0f..%.0f m, count %.0f m, despawn %.0f m", T.pool, T.base, T.base + T.pool - 1, T.dens_free, T.dens_low,
              T.dens_high, T.max_live, T.r_spawn_min, T.r_spawn_max, T.r_count, T.r_despawn);
}

void td5_roam_traffic_session_end(void)
{
    T.on = 0;
    if (T.trc) { fclose(T.trc); T.trc = NULL; }
}

static RtCar *rt_car_of(int slot)
{
    if (!T.on || slot < T.base || slot >= T.base + T.pool || !td5_roam_active()) return NULL;
    return &T.car[slot - T.base];
}

int td5_roam_traffic_parked(int slot)
{
    const RtCar *c = rt_car_of(slot);
    if (!c) return -1;
    return c->state == RT_PARKED;
}

int td5_roam_traffic_alpha(int slot)
{
    const RtCar *c = rt_car_of(slot);
    if (!c) return -1;
    return c->state == RT_PARKED ? 0 : c->alpha;
}

void td5_roam_traffic_force_race_level(int level) { T.force_level = level < 0 ? 0 : level + 1; }
void td5_roam_traffic_force_density(int cars) { T.force_density = cars < 0 ? 0 : cars + 1; }

/* ============================================================ anchors + density === */

static int rt_level_now(int *racing)
{
    const int rs = td5_roam_race_state();
    const int in_race = rs == TD5_RRACE_COUNTDOWN || rs == TD5_RRACE_RACING || rs == TD5_RRACE_FINISHED;
    int per;
    if (racing) *racing = in_race;
    if (in_race) {
        int lvl = T.force_level ? T.force_level - 1 : td5_roam_race_traffic_level();
        per = lvl <= 0 ? 0 : (lvl == 1 ? T.dens_low : T.dens_high);
    } else {
        per = T.force_density ? T.force_density - 1 : T.dens_free;
    }
    return per;
}

/* the anchors: slot 0, and every racer slot of the field in a race */
static int rt_anchors(int racing, double *ax, double *az, int cap)
{
    int n = 0, s;
    for (s = 0; s < 6 && n < cap; s++) {
        const TD5_Actor *a;
        if (s > 0 && (!racing || td5_game_get_slot_state(s) == 3)) continue;
        a = td5_game_get_actor(s);
        if (!a || !rt_place_of(a, &ax[n], &az[n])) continue;
        n++;
    }
    return n;
}

/* ============================================================ spawn =============== */

static int rt_clear_of_actors(double px, double pz, double r_m, int self)
{
    int j, total = td5_game_get_total_actor_count();
    for (j = 0; j < total; j++) {
        const TD5_Actor *b;
        double bx, bz;
        if (j == self) continue;
        if (j < T.base && td5_game_get_slot_state(j) == 3) continue;
        if (j >= T.base && td5_roam_traffic_parked(j) == 1) continue;
        b = td5_game_get_actor(j);
        if (!b || !rt_place_of(b, &bx, &bz)) continue;
        if (hypot(bx - px, bz - pz) < r_m * RT_M) return 0;
    }
    return 1;
}

static double rt_class_speed(int klass)
{
    double kmh;
    switch (klass) {
    case 9: kmh = 60.0; break;          /* motorway    */
    case 8: kmh = 52.0; break;          /* trunk       */
    case 7: kmh = 46.0; break;          /* primary     */
    case 6: kmh = 42.0; break;          /* secondary   */
    case 5: kmh = 38.0; break;          /* tertiary    */
    case 4: case 3: kmh = 32.0; break;  /* unclassified / residential */
    default: kmh = 20.0; break;         /* living / service / unknown */
    }
    return kmh / 3.6 * T.speed_pct;
}

static void rt_place_actor(RtCar *c, TD5_Actor *a, double px, double pz, int yaw12, double v)
{
    int32_t ox, oz;
    const double y = (double)(yaw12 & 0xFFF) * (2.0 * RT_PI / 4096.0);
    const double vu = v * RT_M / RT_HZ * 256.0;
    td5_roam_world_origin(&ox, &oz);
    a->world_pos.x = FP_SCALE((int32_t)floor(px - ox + 0.5));
    a->world_pos.y = (int32_t)0xC0000000;
    a->world_pos.z = FP_SCALE((int32_t)floor(pz - oz + 0.5));
    a->slot_index = (uint8_t)c->slot;
    memset(&a->tire_track_emitter_FL, 0xFF, 4);
    a->euler_accum.yaw = FP_SCALE(yaw12 & 0xFFF);
    td5_roam_physics_forget_slot(c->slot);
    a->airborne_frame_counter = 0;
    td5_physics_reset_actor_state(a);
    a->track_span_raw = -1;
    a->track_span_normalized = -1;
    a->track_span_accumulated = -1;
    a->track_span_high_water = -1;
    a->track_sub_lane_index = 0;
    a->vehicle_mode = 0;
    td5_roam_set_slot_edge(c->slot, 0);
    td5_roam_physics_spawn_snap(a);
    a->linear_velocity_x = (int32_t)(sin(y) * vu);
    a->linear_velocity_z = (int32_t)(cos(y) * vu);
    a->steering_command = 0;
    a->encounter_steering_cmd = 0;
    a->brake_flag = 0; a->handbrake_flag = 0;
}

/* Try once to put a parked car on the road round anchor (ax, az). 1 when placed. */
static int rt_try_spawn(double ax, double az, const double *anx, const double *anz, int nanch)
{
    RtCar *c = NULL;
    TD5_Actor *a;
    TD5_RouteGEdge E;
    double th, r, px, pz, along, dm, s, x, z, tx, tz, v;
    int i, e, d, k, inside = 0, ok;
    for (i = 0; i < T.pool; i++) if (T.car[i].state == RT_PARKED) { c = &T.car[i]; break; }
    if (!c) return 0;
    th = rt_unit(&T.rng) * 2.0 * RT_PI;
    r = (T.r_spawn_min + rt_unit(&T.rng) * (T.r_spawn_max - T.r_spawn_min)) * RT_M;
    px = ax + cos(th) * r; pz = az + sin(th) * r;
    if (!td5_roam_route_graph_snap_edge(px, pz, 60.0, &e, &along, &dm)) return 0;
    if (!rt_edge(e, &E) || E.plen < 42.0 * RT_M) return 0;
    if (E.klass <= 2 && rt_unit(&T.rng) < 0.8) return 0;      /* service / living: few cars */
    if (E.flags & 0x03) return 0;                              /* not on a deck or in a bore */
    if (E.oneway > 0) d = 0;
    else if (E.oneway < 0) d = 1;
    else d = (rt_lcg(&T.rng) & 1) ? 1 : 0;
    s = d == 0 ? along : E.plen - along;
    if (s < 6.0 * RT_M || s > E.plen - 35.0 * RT_M) return 0;  /* clear of the junctions (and of a stop line) */
    k = (int)(rt_lcg(&T.rng) % (uint32_t)rt_dir_lanes(&E));
    rt_edge_pt(&E, d, s, rt_lane_off(&E, k), &x, &z, &tx, &tz);
    /* far enough from EVERY anchor (no car pops up next to a racer) */
    for (i = 0; i < nanch; i++)
        if (hypot(x - anx[i], z - anz[i]) < T.r_spawn_min * 0.8 * RT_M) return 0;
    ok = rt_locate(x, z, 0, 0, &inside, NULL);
    if (!ok || !inside) return 0;
    {   /* the body corners on the road too */
        const double rx = tz, rz = -tx;
        int q;
        for (q = 0; q < 4; q++) {
            const double l = (q & 1) ? 2.2 : -2.2, w = (q & 2) ? 0.95 : -0.95;
            if (!rt_locate(x + (tx * l + rx * w) * RT_M, z + (tz * l + rz * w) * RT_M, 0, 0, &inside, NULL) || !inside) return 0;
        }
    }
    if (!rt_clear_of_actors(x, z, 14.0, c->slot)) return 0;
    a = td5_game_get_actor(c->slot);
    if (!a) return 0;
    memset(&c->e, 0, sizeof *c - offsetof(RtCar, e));
    c->rng = 0x9E3779B9u ^ ((uint32_t)c->slot * 2654435761u) ^ (uint32_t)(T.spawn_count * 40503u);
    rt_lcg(&c->rng);
    c->seg = RT_SEG_EDGE;
    c->e = e; c->d = d; c->k = k; c->s = s;
    c->off = c->off_t = rt_lane_off(&E, k);
    c->v_class = rt_class_speed(E.klass) * (0.9 + 0.2 * rt_unit(&c->rng));
    c->born = (long)g_td5.simulation_tick_counter;
    c->last_contact = -100000;
    rt_plan_next(c);
    v = c->v_class * 0.8;
    rt_place_actor(c, a, x, z, rt_angle_of(tx, tz), v);
    c->state = RT_LIVE;
    c->alpha = 0; c->fade = 1;
    T.spawn_count++;
    T.st.spawns++;
    return 1;
}

static void rt_despawn(RtCar *c, int why)
{
    if (c->state != RT_LIVE || c->fade < 0) return;
    c->fade = -1;
    c->why_out = why;
    T.st.despawns++;
    switch (why) {
    case RT_WHY_FAR: T.st.despawn_far++; break;
    case RT_WHY_DEAD: T.st.despawn_dead_end++; break;
    case RT_WHY_STUCK: T.st.despawn_stuck++; break;
    case RT_WHY_HOLD: T.st.despawn_hold++; break;
    case RT_WHY_OFF: T.st.despawn_off++; break;
    case RT_WHY_UNRES: T.st.despawn_unres++; break;
    case RT_WHY_LOST: T.st.despawn_lost++; break;
    default: break;
    }
}

/* ============================================================ the driver ========== */

/* move the path position to the car: projection on an edge, distance travelled on a connector */
static void rt_track(RtCar *c, const TD5_Actor *a, double x, double z)
{
    TD5_RouteGEdge E;
    int guard;
    for (guard = 0; guard < 4; guard++) {
        if (c->seg == RT_SEG_EDGE) {
            double lat;
            if (!rt_edge(c->e, &E)) return;
            c->s = rt_edge_proj(&E, c->d, c->s, x, z, &lat);
            c->lat_m = (lat - c->off) / RT_M;
            if (c->s < E.plen - 0.25 * RT_M) return;
            if (c->ne < 0) return;               /* dead end: the caller despawns */
            /* the stop line is crossed: the junction is entered */
            if (c->sig_g >= 0) {
                const double ss = (double)g_td5.simulation_tick_counter / RT_HZ;
                const int ph = td5_geo_signal_phase_at(ss, c->sig_g);
                T.st.sig_cross++;
                if (ph == 0) {
                    T.st.red_violations++;
                    if (T.st.red_violations <= 20)
                        TD5_LOG_W(LOG_TAG, "ROAM_TRAFFIC_RED slot %d crossed the stop line on red at node %d (group %d, %.1f km/h) "
                                  "committed=%d %ld ticks ago (phase %d, at arrival %d, t_arr %.2f s, d_stop %.2f m, %.1f km/h) stop_req=%d "
                                  "decided %ld ticks ago at d_stop %.2f m %.1f km/h phase %d",
                                  c->slot, c->node, c->sig_g, rt_speed_ms(a) * 3.6, c->committed,
                                  (long)g_td5.simulation_tick_counter - c->commit_tick, c->commit_ph, c->commit_pa, c->commit_tarr,
                                  c->commit_dstop, c->commit_v * 3.6, c->stop_req, (long)g_td5.simulation_tick_counter - c->sr_tick,
                                  c->sr_d, c->sr_v * 3.6, c->sr_ph);
                }
            }
            T.st.turns[c->turn + 1]++;
            c->seg = RT_SEG_CONN;
            c->s = 0.0;
            if (c->clen < 0.3 * RT_M) c->s = c->clen;   /* a JOIN: no connector */
        }
        if (c->seg == RT_SEG_CONN) {
            double bx, bz, tx, tz, f;
            if (c->clen > 0.0 && c->s < c->clen) {
                rt_bez((const double (*)[2])c->bz, c->s / c->clen, &bx, &bz, &tx, &tz);
                f = (x - bx) * tx + (z - bz) * tz;          /* along the connector */
                c->s = rt_clamp(c->s + f, c->s, c->clen);
                c->lat_m = ((x - bx) * tz - (z - bz) * tx) / RT_M;
                if (c->s < c->clen - 0.25 * RT_M) return;
            }
            /* onto the next edge */
            c->seg = RT_SEG_EDGE;
            c->e = c->ne; c->d = c->nd; c->k = c->nk;
            if (!rt_edge(c->e, &E)) return;
            c->off_t = rt_lane_off(&E, c->k);
            c->off = c->off_t;
            c->s = 0.0;
            c->v_class = rt_class_speed(E.klass) * (0.9 + 0.2 * rt_unit(&c->rng));
            rt_plan_next(c);
            c->s = rt_edge_proj(&E, c->d, 0.0, x, z, NULL);
            if (c->s > E.plen - 0.25 * RT_M && c->ne >= 0) continue;   /* a very short piece */
            return;
        }
    }
}

/* cars ahead: the speed this car may drive (m/s); *obstacle = something in the way, close */
static double rt_gap_cap(const RtCar *c, const TD5_Actor *a, double x, double z, double v, int *obstacle)
{
    const int heading = ((int)a->euler_accum.yaw >> 8) & 0xFFF;
    const double hy = (double)heading * (2.0 * RT_PI / 4096.0);
    const double fx = sin(hy), fz = cos(hy), rx = fz, rz = -fx;
    double cap = 1e9;
    int j, total = td5_game_get_total_actor_count();
    *obstacle = 0;
    for (j = 0; j < total; j++) {
        const TD5_Actor *b;
        double bx, bz, f, l, vb;
        int bh, dh;
        if (j == c->slot) continue;
        if (j < T.base && td5_game_get_slot_state(j) == 3) continue;
        if (j >= T.base && td5_roam_traffic_parked(j) == 1) continue;
        b = td5_game_get_actor(j);
        if (!b || !rt_place_of(b, &bx, &bz)) continue;
        f = ((bx - x) * fx + (bz - z) * fz) / RT_M;
        if (f <= 0.3 || f > 10.0 + v * 1.8) continue;
        l = ((bx - x) * rx + (bz - z) * rz) / RT_M;
        if (fabs(l) > 2.0) continue;
        vb = ((double)b->linear_velocity_x * fx + (double)b->linear_velocity_z * fz) / 256.0 * RT_HZ / RT_M;
        bh = ((int)b->euler_accum.yaw >> 8) & 0xFFF;
        dh = abs(rt_ang12(bh - heading));
        if (dh < 0x300 && fabs(l) < 1.7) {
            /* following: hold ~7.5 m centre to centre at rest, more with speed */
            const double gap = f - (7.0 + 0.6 * (vb > 0.0 ? vb : 0.0));
            const double cv = (vb > 0.0 ? vb : 0.0) + (gap > 0.0 ? gap * 0.5 : gap * 1.2);
            if (cv < cap) cap = cv;
        } else if (fabs(l) < 1.6 && f < 5.5 + v * 1.0) {
            /* crossing / oncoming in my lane, close: stop short of it */
            const double cv = (f - 5.0) * 0.8;
            if (cv < cap) cap = cv;
            *obstacle = 1;
        }
    }
    return cap;
}

/* crude yield at an unsignalised junction: wait while another car is inside the junction, or while a
 * car that arrived first waits at another arm. 1 = may go. */
static int rt_may_enter(RtCar *c, double x, double z, double d_stop_m)
{
    TD5_RouteGNode N;
    double rj;
    int j, total = td5_game_get_total_actor_count();
    const long tick = (long)g_td5.simulation_tick_counter;
    if (!td5_roam_route_graph_node_view(c->node, &N) || N.ninc < 3) return 1;
    rj = hypot(N.x - x, N.z - z) / RT_M - d_stop_m;      /* the stop line's distance from the centre */
    if (rj < 8.0) rj = 8.0;
    if (rj > 25.0) rj = 25.0;
    for (j = 0; j < total; j++) {
        const TD5_Actor *b;
        double bx, bz, vb;
        const RtCar *o;
        if (j == c->slot) continue;
        if (j < T.base && td5_game_get_slot_state(j) == 3) continue;
        if (j >= T.base && td5_roam_traffic_parked(j) == 1) continue;
        b = td5_game_get_actor(j);
        if (!b || !rt_place_of(b, &bx, &bz)) continue;
        if (hypot(bx - N.x, bz - N.z) / RT_M > rj + 1.0) continue;
        o = rt_car_of(j);
        vb = rt_speed_ms(b);
        if (o && o->seg == RT_SEG_EDGE && o->node == c->node && o->waiting) {
            /* another arm's car waits at the line: first come first served (ties by slot) */
            if (o->e == c->e) continue;
            if (o->wait_since < c->wait_since || (o->wait_since == c->wait_since && o->slot < c->slot)) {
                if (tick - c->wait_since < 150) return 0;   /* 5 s: then only the lowest slot holds */
                if (o->slot < c->slot) return 0;
            }
            continue;
        }
        if (o && o->seg == RT_SEG_EDGE && o->e == c->e && o->d == c->d) continue;   /* the queue behind / in front of me */
        if (vb > 0.5 || !o) {
            /* something moving inside the junction (or a racer standing in it) */
            if (tick - c->wait_since > 240 && vb <= 0.5) continue;              /* 8 s: a parked racer does not hold forever */
            return 0;
        }
    }
    return 1;
}

static void rt_drive(RtCar *c, TD5_Actor *a)
{
    TD5_RouteGEdge E;
    const long tick = (long)g_td5.simulation_tick_counter;
    double x, z, v, vt, L, ax, az, atx, atz, dx, dz, dl, alpha, delta, cap;
    double a_req = 1e9;          /* the deceleration a constraint ahead needs NOW (feed-forward), m/s2 */
    double d_end_now = 1e9;
    int32_t steer_t;
    int heading, herr, obstacle = 0, brake = 0;
    double thr;

    rt_place_of(a, &x, &z);
    v = rt_speed_ms(a);
    if (v * 3.6 > T.st.max_kmh) T.st.max_kmh = v * 3.6;
    rt_track(c, a, x, z);
    if (!rt_edge(c->seg == RT_SEG_EDGE ? c->e : (c->ne >= 0 ? c->ne : c->e), &E)) { rt_despawn(c, RT_WHY_DEAD); return; }
    c->holding = 0;

    /* ---- target speed ---- */
    vt = c->v_class;
    if (c->seg == RT_SEG_EDGE) {
        TD5_RouteGEdge C;
        const double d_end = rt_edge(c->e, &C) ? (C.plen - c->s) / RT_M : 0.0;
        d_end_now = d_end;
        if (c->ne < 0) {
            /* a dead end: slow down, then the car is removed at the end */
            vt = rt_clamp(sqrt(2.0 * T.a_brake * (d_end > 0.0 ? d_end : 0.0)), 0.0, vt);
            if (v > 0.2) { const double ar = -v * v / (2.0 * (d_end > 2.3 ? d_end - 2.0 : 0.3)); if (ar < a_req) a_req = ar; }
            if (d_end < 4.0) rt_despawn(c, RT_WHY_DEAD);
        } else {
            const double d_stop = d_end - RT_STOP_BACK_M;
            int must_stop = 0;
            /* the connector's own speed */
            {
                const double vc = sqrt(c->v_conn * c->v_conn + 2.0 * T.a_brake * (d_end > 0.0 ? d_end : 0.0));
                if (vc < vt) vt = vc;
                if (v > c->v_conn) {
                    const double ar = (c->v_conn * c->v_conn - v * v) / (2.0 * (d_end > 0.5 ? d_end : 0.5));
                    if (ar < a_req) a_req = ar;
                }
            }
            if (!c->committed) {
                if (c->sig_g >= 0 && d_end < 70.0) {
                    /* the lamp when the car reaches the line: go only on green, or on the amber that follows
                     * green; stop for red and for the amber before green. A stop it cannot make any more
                     * (past 6 m/s2) is not tried: it goes on. */
                    const double ss = (double)tick / RT_HZ;
                    const double dl = d_stop + RT_STOP_BACK_M;
                    /* the speed it will cross at: it slows for the turn first */
                    const double v_eff = c->v_conn < v ? 0.5 * (v + c->v_conn) : v;
                    const double t_arr = dl > 0.0 ? dl / (v_eff > 1.0 ? v_eff : 1.0) : 0.0;
                    const int ph = td5_geo_signal_phase_at(ss, c->sig_g);
                    /* amber after green (the head was green 1.6 s ago) vs amber after red */
                    const int amber_out = ph == 1 && td5_geo_signal_phase_at(ss - 1.6, c->sig_g) == 2;
                    int pa = 2, go;
                    double tq;
                    /* red anywhere between now and half a second after the car reaches the line: no go
                     * (a far car's "arrival" can fall in the NEXT green, past a whole red) */
                    for (tq = 0.0; tq <= t_arr + 0.5; tq += 0.25)
                        if (td5_geo_signal_phase_at(ss + tq, c->sig_g) == 0) { pa = 0; break; }
                    go = (ph == 2 || amber_out) && pa != 0;
                    if (!go && (c->stop_req || ph == 0 || d_stop > v * v / (2.0 * 6.5) - 0.3)) {
                        if (!c->stop_req) { T.st.sig_stops++; c->sr_tick = tick; c->sr_d = d_stop; c->sr_v = v; c->sr_ph = ph; }
                        c->stop_req = 1;
                    } else c->stop_req = 0;
                    must_stop = c->stop_req;
                    c->commit_ph = ph; c->commit_pa = pa; c->commit_tarr = t_arr; c->commit_dstop = d_stop; c->commit_v = v;
                } else if (c->sig_g < 0 && c->node_deg >= 3 && !c->priority && d_end < 22.0) {
                    if (d_stop < 10.0 && !c->waiting) { c->waiting = 1; c->wait_since = tick; T.st.yield_waits++; }
                    if (c->waiting && !rt_may_enter(c, x, z, d_end)) must_stop = 1;
                    else if (!c->waiting && d_stop < 14.0) {
                        /* rolling up: slow to a crawl before the line, then decide */
                        const double vy = sqrt(16.0 + 2.0 * T.a_brake * (d_stop > 0.0 ? d_stop : 0.0));
                        if (vy < vt) vt = vy;
                    }
                }
                if (must_stop) {
                    const double vs = d_stop > 0.2 ? sqrt(2.0 * T.a_brake * d_stop) : 0.0;
                    if (vs < vt) vt = vs;
                    if (v > 0.2) { const double ar = d_stop > 0.15 ? -v * v / (2.0 * d_stop) : -6.5; if (ar < a_req) a_req = ar; }
                    c->holding = 1;
                } else if (d_stop < 1.5 && (c->sig_g >= 0 || c->waiting)) {
                    c->committed = 1;
                    c->commit_tick = tick;
                }
            }
        }
    } else if (c->v_conn < vt) vt = c->v_conn;
    cap = rt_gap_cap(c, a, x, z, v, &obstacle);
    if (cap < vt) { vt = cap; if (vt < 0.5) c->holding = 1; }
    if (vt < 0.0) vt = 0.0;

    /* ---- lane: keep the target lane on the road (the carriageway may be narrower than the nominal lanes) ---- */
    if (c->seg == RT_SEG_EDGE) {
        if (--c->lane_chk <= 0) {
            double lx, lz, ltx, ltz;
            int in1 = 0, in2 = 0;
            c->lane_chk = 8;
            if (fabs(c->off_t) > 50.0) {
                const double side = c->off_t > 0.0 ? 1.0 : -1.0;
                rt_edge_pt(&E, c->d, c->s + 8.0 * RT_M, c->off_t, &lx, &lz, &ltx, &ltz);
                if (rt_locate(lx, lz, 0, 0, &in1, NULL) && rt_locate(lx + ltz * side * 1.1 * RT_M, lz - ltx * side * 1.1 * RT_M, 0, 0, &in2, NULL) &&
                    !(in1 && in2))
                    c->off_t *= 0.6;
            }
        }
        if (c->off < c->off_t - 20.0) c->off += 20.0;
        else if (c->off > c->off_t + 20.0) c->off -= 20.0;
        else c->off = c->off_t;
        if (fabs(c->lat_m) > T.st.max_lat_m) T.st.max_lat_m = fabs(c->lat_m);
    }

    /* ---- steering: pure pursuit to a point L ahead on the path ---- */
    heading = ((int)a->euler_accum.yaw >> 8) & 0xFFF;
    /* a short look-ahead through a turn (a long one cuts the corner over the kerb) */
    if (c->seg == RT_SEG_CONN || (c->turn != 0 && d_end_now < 8.0)) L = rt_clamp(2.5 + 0.3 * v, 3.5, 8.0) * RT_M;
    else L = rt_clamp(4.0 + 0.45 * v, 5.0, 15.0) * RT_M;
    rt_ahead(c, L, &ax, &az, &atx, &atz);
    dx = ax - x; dz = az - z; dl = sqrt(dx * dx + dz * dz) / RT_M;
    herr = rt_ang12(rt_angle_of(dx, dz) - heading);
    alpha = (double)herr * (2.0 * RT_PI / 4096.0);
    delta = atan2(2.0 * T.wb * sin(alpha), dl > 1.0 ? dl : 1.0);
    steer_t = (int32_t)(delta * (4096.0 / (2.0 * RT_PI)) * 256.0);
    {
        long dd = (long)steer_t - (long)c->steer;
        if (dd > 0x2400) dd = 0x2400;
        if (dd < -0x2400) dd = -0x2400;
        c->steer = (int32_t)rt_clamp((double)((long)c->steer + dd), -RT_STEER_CLAMP, RT_STEER_CLAMP);
    }

    /* ---- throttle: the traffic integrator adds `throttle` (= cmd * 4) to the 24.8 speed per tick
     * and drags 1/256 of it, so cmd = (speed/256 + accel) / 4 holds a speed exactly ---- */
    {
        const double v24 = (double)a->longitudinal_speed;                  /* 24.8 units / tick, signed */
        const double k122 = RT_M * 256.0 / (RT_HZ * RT_HZ);               /* 24.8 per tick per (m/s^2) */
        const double vs = v24 / 256.0 * RT_HZ / RT_M;                       /* signed forward speed, m/s */
        double acc = 0.9 * (vt - vs);
        if (a_req < acc && vs > 0.0) acc = a_req;
        acc = rt_clamp(acc, -6.5, 2.2);
        if (vt < 0.3 && fabs(vs) < 0.8) {
            thr = -v24 * (255.0 / 256.0);                                   /* stand still */
            brake = 1;
        } else {
            thr = v24 / 256.0 + acc * k122;
            if (acc < -0.4) brake = 1;
        }
        thr = rt_clamp(thr, -1024.0, 1024.0);
        a->encounter_steering_cmd = (int16_t)floor(thr / 4.0 + 0.5);
    }
    a->steering_command = c->steer;
#ifndef TD5RE_RELEASE
    if (T.trace_slot == c->slot) {
        if (!T.trc) {
            char path[512];
            const char *dir = td5_plat_log_dir();
            snprintf(path, sizeof path, "%sroam_traffic_trace.csv", dir ? dir : "");
            T.trc = fopen(path, "w");
            if (T.trc) fprintf(T.trc, "tick,x,z,seg,e,d,k,s_m,lat_m,off_m,offt_m,v_kmh,vt_kmh,steer,herr,cmd,brake,hold,node,turn,clen_m\n");
        }
        if (T.trc)
            fprintf(T.trc, "%ld,%.0f,%.0f,%d,%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%d,%d,%d,%d,%d,%d,%d,%.1f\n", tick, x, z, c->seg, c->e,
                    c->d, c->k, c->s / RT_M, c->lat_m, c->off / RT_M, c->off_t / RT_M, v * 3.6, vt * 3.6, (int)c->steer, herr,
                    (int)a->encounter_steering_cmd, brake, c->holding, c->node, c->turn, c->clen / RT_M);
    }
#endif
    a->brake_flag = (uint8_t)brake;
    a->handbrake_flag = 0;
    a->throttle_state = (uint8_t)(brake ? 1 : 0);

    /* ---- lost: far off the lane line it follows (a shove, a missed connector) ---- */
    if (fabs(c->lat_m) > 3.5) {
        if (++c->lost_t == 1 && T.st.despawn_lost < 20)
            TD5_LOG_W(LOG_TAG, "ROAM_TRAFFIC_LOST slot %d lat %.1f m seg=%s edge=%d d=%d s=%.1f m (conn %.1f m) turn=%d v=%.1f km/h contact %ld "
                      "ticks ago", c->slot, c->lat_m, c->seg == RT_SEG_EDGE ? "edge" : "conn", c->e, c->d, c->s / RT_M, c->clen / RT_M,
                      c->turn, v * 3.6, tick - c->last_contact);
        if (c->lost_t > 20 && c->fade >= 0) rt_despawn(c, RT_WHY_LOST);
    } else c->lost_t = 0;

    /* ---- stuck / holding / off the surface ---- */
    if (v < RT_STILL_MS) {
        c->still_t++;
        if (c->holding) c->hold_t++;
    } else { c->still_t = 0; c->hold_t = 0; }
    if (c->still_t > RT_STUCK_TICKS && !c->holding && c->fade >= 0) rt_despawn(c, RT_WHY_STUCK);
    if (c->hold_t > RT_HOLD_TICKS && c->fade >= 0) rt_despawn(c, RT_WHY_HOLD);
    if (c->still_t > RT_STUCK_TICKS + 30 && c->fade >= 0 && c->state == RT_LIVE && !c->holding) T.st.stuck_live++;
    {
        int inside = 0;
        int32_t gy = 0;
        if (!rt_locate(x, z, a->world_pos.y, 1, &inside, &gy)) { rt_despawn(c, RT_WHY_UNRES); c->fade = -1; c->alpha = 0; return; }
        if (!inside) {
            if (c->off_t_ticks == 0 && T.st.off_ticks < 400)
                TD5_LOG_W(LOG_TAG, "ROAM_TRAFFIC_OFF slot %d at place (%.0f,%.0f) seg=%s edge=%d d=%d k=%d s=%.1f/%.1f m lat=%.2f m off=%.2f m "
                          "turn=%d v=%.1f km/h contact=%ld ticks ago", c->slot, x, z, c->seg == RT_SEG_EDGE ? "edge" : "conn", c->e, c->d,
                          c->k, c->s / RT_M, c->seg == RT_SEG_EDGE ? 0.0 : c->clen / RT_M, c->lat_m, c->off / RT_M, c->turn, v * 3.6,
                          tick - c->last_contact);
            T.st.off_ticks++;
            if (tick - c->last_contact < 60) T.st.off_ticks_contact++;
            if (++c->off_t_ticks > RT_OFF_TICKS && c->fade >= 0) { T.st.off_events++; rt_despawn(c, RT_WHY_OFF); }
        } else c->off_t_ticks = 0;
        if (inside) {
            /* how far the traffic pose (XZ ground: the highest deck) is from the ground under the car */
            const double dy = fabs((double)(a->world_pos.y - gy)) / 256.0 / RT_M;
            if (dy < 5.0 && dy > T.st.max_ground_dy_m) T.st.max_ground_dy_m = dy;
        }
    }
}

/* ============================================================ barriers + DEV shots */

/* A traffic car crossing a race barrier segment (barriers are racer-only): counted for the proof. */
static void rt_barrier_passes(void)
{
    int nb = 0, i, k;
    const TD5_RoamBarrier *b;
    if (!td5_roam_race_barriers_live()) { T.bar_valid = 0; return; }
    b = td5_roam_barriers(&nb);
    if (!b || nb <= 0) return;
    for (i = 0; i < T.pool; i++) {
        RtCar *c = &T.car[i];
        const TD5_Actor *a;
        double x, z;
        if (c->state != RT_LIVE || c->alpha < 255) { c->bar_side = 0; continue; }
        a = td5_game_get_actor(c->slot);
        if (!a) continue;
        x = (double)a->world_pos.x / 256.0; z = (double)a->world_pos.z / 256.0;     /* WORLD frame, units */
        for (k = 0; k < nb; k++) {
            const double x0 = b[k].x0 / 256.0, z0 = b[k].z0 / 256.0, x1 = b[k].x1 / 256.0, z1 = b[k].z1 / 256.0;
            const double dx = x1 - x0, dz = z1 - z0, l2 = dx * dx + dz * dz;
            double t, sd;
            int side;
            if (b[k].kind != TD5_ROAM_BARRIER_MOUTH || l2 <= 1.0) continue;
            t = ((x - x0) * dx + (z - z0) * dz) / l2;
            if (t < -0.05 || t > 1.05) continue;
            sd = ((x - x0) * dz - (z - z0) * dx) / sqrt(l2);
            if (fabs(sd) > 3.0 * RT_M) continue;
            side = sd > 0.0 ? 1 : -1;
            if (c->bar_k == k && c->bar_side != 0 && side != c->bar_side && T.bar_valid) {
                T.st.barrier_passes++;
#ifndef TD5RE_RELEASE
                if (T.shots && !T.shot_done[2]) {
                    const TD5_Actor *p = td5_game_get_actor(0);
                    if (p) {
                        const double px = p->world_pos.x / 256.0, pz = p->world_pos.z / 256.0;
                        const double hy = (double)(((int)p->euler_accum.yaw >> 8) & 0xFFF) * (2.0 * RT_PI / 4096.0);
                        const double rx = x - px, rz = z - pz, d = sqrt(rx * rx + rz * rz);
                        if (d < 70.0 * RT_M && d > 6.0 * RT_M && (rx * sin(hy) + rz * cos(hy)) > 0.6 * d) {
                            char path[512];
                            const char *dir = td5_plat_log_dir();
                            snprintf(path, sizeof path, "%sroam_traffic_barrier.png", dir ? dir : "");
                            td5_plat_request_frame_dump(path);
                            T.shot_done[2] = 1;
                            TD5_LOG_I(LOG_TAG, "[ROAMTRAFFIC] shot barrier: slot %d crossed barrier %d (cluster %llx) %.0f m ahead "
                                      "of the player -> %s", c->slot, k, (unsigned long long)b[k].cluster_key, d / RT_M, path);
                        }
                    }
                }
#endif
            }
            c->bar_k = k; c->bar_side = side;
            break;
        }
    }
    T.bar_valid = 1;
}

#ifndef TD5RE_RELEASE
/* DEV shots (TD5RE_ROAM_TRAFFIC_SHOTS=1, free roam only): put the player behind a car held at a red
 * stop line, then behind a car on a busy avenue, and dump a frame each time. */
static void rt_shots(void)
{
    const long tick = (long)g_td5.simulation_tick_counter;
    TD5_Actor *p;
    int i;
    if (!T.shots || td5_roam_race_running()) return;
    p = td5_game_get_actor(0);
    if (!p) return;
    if (T.shot_wait > 0) {
        if (tick >= T.shot_wait) {
            char path[512];
            const char *dir = td5_plat_log_dir();
            snprintf(path, sizeof path, "%sroam_traffic_%s.png", dir ? dir : "", T.shot_name);
            td5_plat_request_frame_dump(path);
            TD5_LOG_I(LOG_TAG, "[ROAMTRAFFIC] shot %s -> %s (live %d)", T.shot_name, path, T.st.live);
            T.shot_wait = 0;
        }
        return;
    }
    if (tick < 450) return;
    for (i = 0; i < T.pool; i++) {
        RtCar *c = &T.car[i];
        TD5_RouteGEdge E;
        double x, z, tx, tz, back;
        int want = -1, j, nnear = 0;
        const TD5_Actor *a;
        if (c->state != RT_LIVE || c->seg != RT_SEG_EDGE || c->alpha < 255 || !rt_edge(c->e, &E)) continue;
        a = td5_game_get_actor(c->slot);
        if (!a) continue;
        if (!T.shot_done[0] && c->sig_g >= 0 && c->stop_req && c->holding && rt_speed_ms(a) < 0.3 &&
            td5_geo_signal_phase_at((double)tick / RT_HZ + 2.0, c->sig_g) == 0) want = 0;     /* red for 2 s more */
        else if (T.shot_done[0] && !T.shot_done[1] && E.klass >= 6 && c->s > 40.0 * RT_M) {
            double cx, cz;
            rt_place_of(a, &cx, &cz);
            for (j = 0; j < T.pool; j++) {
                const TD5_Actor *b;
                double bx, bz;
                if (j == i || T.car[j].state != RT_LIVE) continue;
                b = td5_game_get_actor(T.car[j].slot);
                if (b && rt_place_of(b, &bx, &bz) && hypot(bx - cx, bz - cz) < 90.0 * RT_M) nnear++;
            }
            if (nnear >= 3) want = 1;
        }
        if (want < 0) continue;
        back = want == 0 ? 16.0 : 22.0;
        if (c->s < back * RT_M + 2.0 * RT_M) continue;
        rt_edge_pt(&E, c->d, c->s - back * RT_M, c->off, &x, &z, &tx, &tz);
        if (!rt_clear_of_actors(x, z, 5.0, 0)) continue;
        td5_roam_place_actor(0, p, (int32_t)floor(x + 0.5), (int32_t)floor(z + 0.5), rt_angle_of(tx, tz));
        T.shot_done[want] = 1;
        T.shot_wait = tick + 20;
        snprintf(T.shot_name, sizeof T.shot_name, "%s", want == 0 ? "red_stop" : "avenue");
        TD5_LOG_I(LOG_TAG, "[ROAMTRAFFIC] shot %s: player put %.0f m behind slot %d (edge %d klass %d lanes %d, node %d group %d, "
                  "%d cars within 90 m)", T.shot_name, back, c->slot, c->e, E.klass, E.lanes, c->node, c->sig_g, nnear);
        return;
    }
}
#endif

/* ============================================================ tick ================ */

static void rt_tick_body(void);

void td5_roam_traffic_tick(void)
{
#ifndef TD5RE_RELEASE
    const uint64_t t0 = td5_plat_time_us();
    if (!T.on || !td5_roam_active()) return;
    rt_tick_body();
    {
        const double us = (double)(td5_plat_time_us() - t0);
        const long tick = (long)g_td5.simulation_tick_counter;
        T.tick_us_sum += us; T.tick_n++;
        if (us > T.tick_us_max) T.tick_us_max = us;
#ifdef _WIN32
        if (tick - T.st_t0 > RT_WARMUP && tick % 30 == 0) {
            PROCESS_MEMORY_COUNTERS_EX pmc;
            memset(&pmc, 0, sizeof pmc);
            if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc)) {
                const double mb = (double)pmc.PrivateUsage / (1024.0 * 1024.0);
                if (T.priv_n == 0) T.priv0_mb = mb;
                T.priv_mb = mb;
                if (mb > T.priv_max_mb) T.priv_max_mb = mb;
                T.priv_n++;
            }
        }
#endif
    }
#else
    rt_tick_body();
#endif
}

static void rt_tick_body(void)
{
    double ax[8], az[8];
    int nanch, racing = 0, per, i, live = 0, target;
    const long tick = (long)g_td5.simulation_tick_counter;
    if (!T.on || !td5_roam_active()) return;
    if (tick == T.tick_last) return;
    T.tick_last = tick;
    if (!td5_roam_route_graph_ready()) {
        if (!td5_roam_route_graph_building()) td5_roam_route_graph_begin_async(td5_roam_route_place());
        return;
    }
    if (!T.ready) {
        T.ready = 1;
        TD5_LOG_I(LOG_TAG, "ROAM_TRAFFIC: planner graph ready: %d edges, %d nodes, %d signalised clusters",
                  td5_roam_route_graph_nedge(), td5_roam_route_graph_nnode(), td5_roam_route_graph_nsignal());
    }
    per = rt_level_now(&racing);
    nanch = rt_anchors(racing, ax, az, 8);
    target = per * nanch;
    if (target > T.max_live) target = T.max_live;
    if (target > T.pool) target = T.pool;
    T.st.target = target;

    /* drive every live car; far ones fade out */
    for (i = 0; i < T.pool; i++) {
        RtCar *c = &T.car[i];
        TD5_Actor *a;
        double x, z, dmin = 1e300;
        int k;
        if (c->state != RT_LIVE) continue;
        a = td5_game_get_actor(c->slot);
        if (!a) { rt_park(c); continue; }
        rt_place_of(a, &x, &z);
        for (k = 0; k < nanch; k++) {
            const double dd = hypot(x - ax[k], z - az[k]);
            if (dd < dmin) dmin = dd;
            if (dd < 4.5 * RT_M) { c->last_contact = tick; T.st.racer_near++; }
        }
        if (dmin > T.r_despawn * RT_M) rt_despawn(c, RT_WHY_FAR);
        if (per == 0 && c->fade >= 0) rt_despawn(c, RT_WHY_LEVEL);
        rt_drive(c, a);
        if (c->fade > 0) { c->alpha += 255 / RT_FADE_TICKS; if (c->alpha >= 255) { c->alpha = 255; c->fade = 0; } }
        else if (c->fade < 0) { c->alpha -= 255 / RT_FADE_TICKS; if (c->alpha <= 0) rt_park(c); }
        if (c->state == RT_LIVE) live++;
    }

    /* spawn: the anchor with the fewest cars round it, one try per tick */
    if (nanch > 0 && live < target) {
        int best = -1, bn = 1 << 30, k;
        for (k = 0; k < nanch; k++) {
            int n = 0;
            for (i = 0; i < T.pool; i++) {
                const RtCar *c = &T.car[i];
                const TD5_Actor *a;
                double x, z;
                if (c->state != RT_LIVE || c->fade < 0) continue;
                a = td5_game_get_actor(c->slot);
                if (!a || !rt_place_of(a, &x, &z)) continue;
                if (hypot(x - ax[k], z - az[k]) < T.r_count * RT_M) n++;
            }
            if (n < per && n < bn) { bn = n; best = k; }
        }
        if (best >= 0 && rt_try_spawn(ax[best], az[best], ax, az, nanch)) live++;
    }

    rt_barrier_passes();
#ifndef TD5RE_RELEASE
    rt_shots();
#endif
    T.st.live = live;
    if (tick - T.st_t0 > RT_WARMUP) {
        T.st.samples++;
        T.live_sum += live;
        if (live < T.st.live_min) T.st.live_min = live;
        if (live > T.st.live_max) T.st.live_max = live;
        T.st.live_mean = T.live_sum / (double)T.st.samples;
    }
    if (T.log_every > 0 && tick % (T.log_every * 30L) == 0)
        TD5_LOG_I(LOG_TAG, "ROAM_TRAFFIC tick %ld: live %d target %d (per anchor %d x %d) spawns %ld despawns %ld (far %ld dead %ld "
                  "stuck %ld hold %ld off %ld unres %ld lost %ld) red %ld/%ld off_ticks %ld yield %ld turns L%ld S%ld R%ld vmax %.0f lat %.2f m",
                  tick, live, target, per, nanch, T.st.spawns, T.st.despawns, T.st.despawn_far, T.st.despawn_dead_end,
                  T.st.despawn_stuck, T.st.despawn_hold, T.st.despawn_off, T.st.despawn_unres, T.st.despawn_lost, T.st.red_violations, T.st.sig_cross,
                  T.st.off_ticks, T.st.yield_waits, T.st.turns[0], T.st.turns[1], T.st.turns[2], T.st.max_kmh, T.st.max_lat_m);
}

/* ============================================================ stats =============== */

void td5_roam_traffic_stats(TD5_RoamTrafficStats *out)
{
    if (!out) return;
    *out = T.st;
    if (out->samples == 0) out->live_min = 0;
}

#ifndef TD5RE_RELEASE
int td5_roam_traffic_verdict(char *note, size_t n, int race_row)
{
    const TD5_RoamTrafficStats *s = &T.st;
    const long off_free = s->off_ticks - s->off_ticks_contact;
    const double dpriv = T.priv_n ? T.priv_mb - T.priv0_mb : 0.0;
    const double tick_ms = T.tick_n ? T.tick_us_sum / (double)T.tick_n / 1000.0 : 0.0;
    char bad[96] = "";
    size_t bl = 0;
    int status = 0;
#define RT_BAD(...) do { if (bl < sizeof bad - 1) bl += (size_t)snprintf(bad + bl, sizeof bad - bl, __VA_ARGS__); } while (0)
    if (!T.on) RT_BAD("traffic off; ");
    else if (s->samples == 0) RT_BAD("no samples; ");
    else {
        if (!race_row && (s->live_min == 0 || s->live_mean < 0.6 * (double)s->target))
            RT_BAD("density min %d mean %.1f < 0.6 x %d; ", s->live_min, s->live_mean, s->target);
        if (race_row && s->live_max == 0) RT_BAD("no traffic in the race; ");
    }
    if (off_free > 0) RT_BAD("off surface %ld ticks; ", off_free);
    if (s->stuck_live > 0) RT_BAD("stuck>10s live %ld; ", s->stuck_live);
    if (dpriv > 32.0) RT_BAD("private +%.0f MB; ", dpriv);
#undef RT_BAD
    if (bad[0]) status = 2;
    else if (s->red_violations > 0) status = 1;
    snprintf(note, n, "%straffic %d/%.1f/%d (target %d) red %ld/%ld off %ld(+%ld hit) stuck 0 priv %+.1f MB tick %.2f ms%s%.40s",
             status == 2 ? "FAIL " : "", s->live_min, s->live_mean, s->live_max, s->target, s->red_violations, s->sig_cross, off_free,
             s->off_ticks_contact, dpriv, tick_ms, bad[0] ? ": " : "", bad);
    TD5_LOG_I(LOG_TAG, "[ROAMTRAFFIC] VERDICT %s: live min %d mean %.2f max %d target %d samples %ld | spawns %ld despawns %ld "
              "(far %ld dead-end %ld stuck %ld hold %ld off %ld unresident %ld lost %ld) | off-surface ticks %ld (%ld within 2 s of a racer "
              "contact, events %ld) | red violations %ld of %ld stop-line crossings at signals, red stops %ld | stuck>10s live %ld | "
              "yield waits %ld | turns L%ld S%ld R%ld | connector off-patch plans %ld | barrier passes %ld | racer-near ticks %ld | "
              "max lane offset %.2f m, vmax %.0f km/h, pose-vs-ground %.2f m | private %.1f -> %.1f MB (max %.1f, %d samples) | "
              "traffic tick mean %.3f ms max %.2f ms %s",
              status == 2 ? "FAIL" : (status ? "WARN" : "PASS"), s->live_min, s->live_mean, s->live_max, s->target, s->samples,
              s->spawns, s->despawns, s->despawn_far, s->despawn_dead_end, s->despawn_stuck, s->despawn_hold, s->despawn_off,
              s->despawn_unres, s->despawn_lost, s->off_ticks, s->off_ticks_contact, s->off_events, s->red_violations, s->sig_cross, s->sig_stops,
              s->stuck_live, s->yield_waits, s->turns[0], s->turns[1], s->turns[2], s->conn_off_plans, s->barrier_passes,
              s->racer_near, s->max_lat_m, s->max_kmh, s->max_ground_dy_m, T.priv0_mb, T.priv_mb, T.priv_max_mb, T.priv_n,
              tick_ms, T.tick_us_max / 1000.0, bad);
    return status;
}
#endif

void td5_roam_traffic_stats_reset(void)
{
    const int pool = T.st.pool;
    memset(&T.st, 0, sizeof T.st);
    T.st.pool = pool;
    T.st.live_min = 1 << 30;
    T.live_sum = 0.0;
    T.st_t0 = (long)g_td5.simulation_tick_counter;
}

int td5_roam_traffic_find_red_stop(int *slot, double *cx, double *cz)
{
    int i;
    for (i = 0; i < T.pool; i++) {
        const RtCar *c = &T.car[i];
        TD5_RouteGNode N;
        if (c->state != RT_LIVE || c->seg != RT_SEG_EDGE || c->sig_g < 0 || !c->stop_req || !c->holding) continue;
        if (!td5_roam_route_graph_node_view(c->node, &N)) continue;
        if (slot) *slot = c->slot;
        if (cx) *cx = N.x;
        if (cz) *cz = N.z;
        return 1;
    }
    return 0;
}
