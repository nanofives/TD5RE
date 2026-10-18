/**
 * td5_roam_ram_test.c -- GEO FREE ROAM: the `roam-ram` self-test (PORT-ONLY, DEV-ONLY, F8e).
 *
 * Rams a stand-still traffic car (td5_roam_traffic_dev_place) at 20 / 35 / 50 / 70 / 90 km/h, once REAR-END (the car
 * parked in the player's lane along the heading) and once SIDE-ON (the car across the lane), in one free-roam session
 * through the real input path, physics and V2V contact. Per ram it logs the contact magnitude, the response kind (light =
 * impulse only, heavy = v2v_heavy_scatter, with the roam damping %), and the player's peak lift, peak spin and speed loss:
 *   [ROAMRAM] ROW kind=rear v_target=50 v_hit=49.3 mag=212345 resp=heavy pct=62 lift_ms=2.1 yaw=9800 roll=.. pitch=.. loss_kmh=31.2
 * and the same rows to log/roam_ram.csv. TD5RE_CRASH_FX etc. are untouched. The crash threshold under test is
 * TD5RE_ROAM_CRASH_HEAVY (0 = the faithful gates = the "before" table).
 *
 * The row passes when every ram made contact. ONE file-scope struct. Compiled out of RELEASE.
 */
#ifndef TD5RE_RELEASE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_physics.h"
#include "td5_race_state.h"
#include "td5_roam_mode.h"
#include "td5_roam_locate.h"
#include "td5_roam_route.h"
#include "td5_roam_traffic.h"
#include "td5_roam_ram_test.h"
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RR_M    430.0
#define RR_PI   3.14159265358979323846
#define RR_HZ   30.0
#define RR_DS   2.0
#define RR_PTS  1600
#define RR_N    10
#define RR_WIN  45

enum { RR_WAIT = 0, RR_PLAN, RR_RUN, RR_HIT, RR_SETTLE, RR_END };

static const int k_speed[5] = { 20, 35, 50, 70, 90 };

static struct {
    int    armed, phase, done, failed;
    char   why[80];
    long   tick_last, phase_t0, t_start;
    uint32_t bits_last;
    double px[RR_PTS], pz[RR_PTS], pv[RR_PTS];
    int    np, pi;
    double s, kmh_cap;
    int    n;                          /* scenario index 0..RR_N-1 */
    int    slot;
    double car_s;
    long   seq0;
    double kmh_prev;
    /* the window after the hit */
    long   hit_t;
    double v_hit;
    int    heavy, pct_min, impulses;
    int32_t mag_max;
    double lift_max, loss_at;
    int32_t yaw_max, roll_max, pitch_max;
    int    contacts, misses;
    FILE  *csv;
} T;

void td5_roam_ram_test_arm(void)
{
    memset(&T, 0, sizeof T);
    T.armed = 1;
    T.slot = -1;
    TD5_LOG_I(LOG_TAG, "[ROAMRAM] armed (TD5RE_ROAM_CRASH_HEAVY=%d)", td5_env_int("TD5RE_ROAM_CRASH_HEAVY", 70000, 0, 2000000));
}

void td5_roam_ram_test_disarm(void)
{
    if (T.armed) td5_roam_traffic_dev_clear();
    if (T.csv) { fclose(T.csv); T.csv = NULL; }
    T.armed = 0;
}

int td5_roam_ram_test_done(void) { return T.armed && T.done; }

static void rr_finish(const char *why, int fail)
{
    if (T.done) return;
    T.done = 1;
    T.failed = fail;
    snprintf(T.why, sizeof T.why, "%s", why);
    td5_roam_traffic_dev_clear();
    TD5_LOG_I(LOG_TAG, "[ROAMRAM] %s: %s", fail ? "ABORT" : "done", why);
}

static void rr_place_of(const TD5_Actor *a, double *x, double *z)
{
    int32_t ox, oz;
    td5_roam_world_origin(&ox, &oz);
    *x = a->world_pos.x / 256.0 + ox;
    *z = a->world_pos.z / 256.0 + oz;
}

static double rr_kmh(const TD5_Actor *a)
{
    return sqrt((double)a->linear_velocity_x * a->linear_velocity_x + (double)a->linear_velocity_z * a->linear_velocity_z) /
           256.0 * RR_HZ / RR_M * 3.6;
}

static int rr_inside(double px, double pz)
{
    const TD5_RoamResident *rs = td5_roam_resident();
    int32_t ox, oz;
    TD5_RoamHit h;
    if (!rs) return 0;
    td5_roam_world_origin(&ox, &oz);
    return td5_roam_locate(rs, (int32_t)floor((px - ox) * 256.0 + 0.5), (int32_t)floor((pz - oz) * 256.0 + 0.5), 0, 0,
                           ROAM_PRIM_NONE, &h) && h.prim != ROAM_PRIM_NONE && h.inside;
}

/* resample the polyline every RR_DS; the speed profile is the straight-line cap (the rams are on straights) */
static int rr_set_path(const double *qx, const double *qz, int nq, double cap)
{
    double cum[4096];
    int i, seg = 0, np;
    if (nq < 2 || nq > 4096) return 0;
    cum[0] = 0.0;
    for (i = 1; i < nq; i++) cum[i] = cum[i - 1] + hypot(qx[i] - qx[i - 1], qz[i] - qz[i - 1]) / RR_M;
    np = (int)(cum[nq - 1] / RR_DS) + 1;
    if (np > RR_PTS) np = RR_PTS;
    if (np < 10) return 0;
    for (i = 0; i < np; i++) {
        const double s = i * RR_DS;
        double t;
        while (seg + 2 < nq && cum[seg + 1] < s) seg++;
        t = cum[seg + 1] > cum[seg] ? (s - cum[seg]) / (cum[seg + 1] - cum[seg]) : 0.0;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        T.px[i] = qx[seg] + (qx[seg + 1] - qx[seg]) * t;
        T.pz[i] = qz[seg] + (qz[seg + 1] - qz[seg]) * t;
        T.pv[i] = cap;
    }
    T.np = np; T.pi = 0; T.s = 0.0;
    return 1;
}

/* plan a leg of len_m ahead (rotated tries when the street runs out); the leg must be a clear straight for car_at_m */
static int rr_plan(const TD5_Actor *a, double len_m, double min_m, double cap)
{
    static const double k_rot[] = { 0.0, 0.5, -0.5, 1.0, -1.0, 1.5, -1.5, 2.0, -2.0, 3.0 };
    double x, z, hx, hz;
    int k;
    rr_place_of(a, &x, &z);
    {
        const double y = (double)((((int)a->euler_accum.yaw) >> 8) & 0xFFF) * (2.0 * RR_PI / 4096.0);
        hx = sin(y); hz = cos(y);
    }
    for (k = 0; k < (int)(sizeof k_rot / sizeof k_rot[0]); k++) {
        const double c = cos(k_rot[k]), s = sin(k_rot[k]);
        const double dx = hx * c - hz * s, dz = hx * s + hz * c;
        double *qx = NULL, *qz = NULL, len = 0.0;
        int nq = 0;
        if (td5_roam_route_plan_path(x, z, hx, hz, x + dx * len_m * RR_M, z + dz * len_m * RR_M, 200.0, NULL, 0, &qx, &qz, &nq, &len) &&
            len >= min_m && rr_set_path(qx, qz, nq, cap)) {
            free(qx); free(qz);
            return 1;
        }
        free(qx); free(qz);
    }
    return 0;
}

static void rr_point(double s, double off, double *x, double *z, double *tx, double *tz)
{
    int i = (int)floor(s / RR_DS);
    double t, sx, sz, l;
    if (i < 0) i = 0;
    if (i > T.np - 2) i = T.np - 2;
    t = s / RR_DS - i;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    sx = T.px[i + 1] - T.px[i]; sz = T.pz[i + 1] - T.pz[i];
    l = sqrt(sx * sx + sz * sz);
    if (l <= 0.0) { sx = 0.0; sz = 1.0; l = 1.0; }
    sx /= l; sz /= l;
    *x = T.px[i] + (T.px[i + 1] - T.px[i]) * t + sz * off * RR_M;
    *z = T.pz[i] + (T.pz[i + 1] - T.pz[i]) * t - sx * off * RR_M;
    if (tx) { *tx = sx; *tz = sz; }
}

static void rr_track(double x, double z)
{
    int i, lo = T.pi - 5, hi = T.pi + 40, best = T.pi;
    double bd = 1e300, bt = 0.0;
    if (lo < 0) lo = 0;
    if (hi > T.np - 2) hi = T.np - 2;
    for (i = lo; i <= hi; i++) {
        const double sx = T.px[i + 1] - T.px[i], sz = T.pz[i + 1] - T.pz[i], l2 = sx * sx + sz * sz;
        double t = l2 > 0.0 ? ((x - T.px[i]) * sx + (z - T.pz[i]) * sz) / l2 : 0.0, d;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        d = (T.px[i] + sx * t - x) * (T.px[i] + sx * t - x) + (T.pz[i] + sz * t - z) * (T.pz[i] + sz * t - z);
        if (d < bd) { bd = d; best = i; bt = t; }
    }
    T.pi = best;
    T.s = (best + bt) * RR_DS;
}

/* drive the path in the right lane at the target speed (no corner profile: straights only) */
static uint32_t rr_drive(const TD5_Actor *a, double kmh_t)
{
    double x, z, ax, az, hx, hz, err, kmh, look;
    uint32_t bits = 0;
    rr_place_of(a, &x, &z);
    rr_track(x, z);
    kmh = rr_kmh(a);
    look = 6.0 + kmh / 3.6 * 0.45;
    if (look > 22.0) look = 22.0;
    rr_point(T.s + look, 1.6, &ax, &az, NULL, NULL);
    if (kmh > 5.0) { hx = a->linear_velocity_x; hz = a->linear_velocity_z; }
    else { const double y = (double)(a->display_angles.yaw & 0xFFF) * (2.0 * RR_PI / 4096.0); hx = sin(y); hz = cos(y); }
    err = atan2(ax - x, az - z) - atan2(hx, hz);
    while (err > RR_PI) err -= 2.0 * RR_PI;
    while (err < -RR_PI) err += 2.0 * RR_PI;
    if (err > 0.03) bits |= (uint32_t)TD5_INPUT_STEER_LEFT;
    else if (err < -0.03) bits |= (uint32_t)TD5_INPUT_STEER_RIGHT;
    if (kmh < kmh_t) bits |= (uint32_t)TD5_INPUT_THROTTLE;
    else if (kmh > kmh_t * 1.05 + 1.0) bits |= (uint32_t)TD5_INPUT_BRAKE;
    return bits;
}

/* brake while rolling, the handbrake once nearly still: a brake held at a standstill engages reverse */
static uint32_t rr_stop_bits(const TD5_Actor *a)
{
    return (uint32_t)(rr_kmh(a) > 12.0 ? TD5_INPUT_BRAKE : TD5_INPUT_HANDBRAKE);
}

static void rr_row(void)
{
    const char *kind = (T.n & 1) ? "side" : "rear";
    const int vt = k_speed[T.n >> 1];
    const char *resp = T.impulses == 0 ? "none" : (T.heavy ? "heavy" : "light");
    const double lift_ms = T.lift_max;
    TD5_LOG_I(LOG_TAG, "[ROAMRAM] ROW kind=%s v_target=%d v_hit=%.1f mag=%d resp=%s pct=%d impulses=%d lift_ms=%.2f yaw=%d roll=%d pitch=%d loss_kmh=%.1f",
              kind, vt, T.v_hit, (int)T.mag_max, resp, T.heavy ? T.pct_min : 100, T.impulses, lift_ms, (int)T.yaw_max, (int)T.roll_max,
              (int)T.pitch_max, T.loss_at);
    if (!T.csv) {
        char path[512];
        const char *dir = td5_plat_log_dir();
        snprintf(path, sizeof path, "%sroam_ram.csv", dir ? dir : "");
        T.csv = fopen(path, "w");
        if (T.csv) fprintf(T.csv, "kind,v_target,v_hit,mag,resp,pct,impulses,lift_ms,yaw,roll,pitch,loss_kmh\n");
    }
    if (T.csv) {
        fprintf(T.csv, "%s,%d,%.1f,%d,%s,%d,%d,%.2f,%d,%d,%d,%.1f\n", kind, vt, T.v_hit, (int)T.mag_max, resp, T.heavy ? T.pct_min : 100,
                T.impulses, lift_ms, (int)T.yaw_max, (int)T.roll_max, (int)T.pitch_max, T.loss_at);
        fflush(T.csv);
    }
}

static uint32_t rr_bits(TD5_Actor *a, long tick);

uint32_t td5_roam_ram_test_bits(TD5_Actor *a)
{
    const long tick = (long)g_td5.simulation_tick_counter;
    if (!T.armed || T.done || !a || !td5_roam_active()) return 0;
    if (tick == T.tick_last) return T.bits_last;
    T.tick_last = tick;
    T.bits_last = rr_bits(a, tick);
    return T.bits_last;
}

static int rr_ahead_ok(void)   /* the leg runs straight and on the road for the whole run-up */
{
    return T.np > 40;
}

static uint32_t rr_bits(TD5_Actor *a, long tick)
{
    const int kmh_t = k_speed[T.n >> 1], side = T.n & 1;
    long seq;
    int ia, ib, heavy, pct;
    int32_t mag;
    if (T.phase != RR_WAIT && tick - T.t_start > 12000L) { rr_finish("timeout", 1); return 0; }
    switch (T.phase) {
    case RR_WAIT:
        if (tick < 90 || !td5_roam_route_graph_ready()) return 0;
        T.t_start = tick;
        T.phase = RR_PLAN;
        return (uint32_t)TD5_INPUT_BRAKE;
    case RR_PLAN: {
        /* run-up 25 m + v^2 / 7 (3.5 m/s2), the car parked 15 m past it */
        const double v = kmh_t / 3.6, runup = 25.0 + v * v / 7.0, need = runup + 40.0;
        double x, z, tx, tz;
        int yaw;
        if (!rr_plan(a, need + 200.0, need, kmh_t) || !rr_ahead_ok()) { rr_finish("no straight leg for the run-up", 1); return 0; }
        T.car_s = runup;
        rr_point(T.car_s, 1.6, &x, &z, &tx, &tz);
        yaw = (int)floor(atan2(tx, tz) * (4096.0 / (2.0 * RR_PI)) + 0.5) & 0xFFF;
        if (side) yaw = (yaw + 1024) & 0xFFF;
        T.slot = td5_roam_traffic_dev_place(x, z, yaw);
        if (T.slot < 0) { rr_finish("no free traffic-pool car", 1); return 0; }
        td5_physics_dev_last_v2v(&T.seq0, &ia, &ib, &mag, &heavy, &pct);
        T.phase = RR_RUN; T.phase_t0 = tick; T.kmh_prev = rr_kmh(a);
        TD5_LOG_I(LOG_TAG, "[ROAMRAM] ram %d/%d: %s at %d km/h, traffic slot %d parked %.0f m along (run-up %.0f m)", T.n + 1, RR_N,
                  side ? "SIDE" : "REAR", kmh_t, T.slot, T.car_s, runup);
        return 0;
    }
    case RR_RUN: {
        td5_physics_dev_last_v2v(&seq, &ia, &ib, &mag, &heavy, &pct);
        if (seq != T.seq0 && ((ia == 0 && ib == T.slot) || (ib == 0 && ia == T.slot))) {
            T.phase = RR_HIT; T.hit_t = tick; T.v_hit = T.kmh_prev;
            T.heavy = 0; T.pct_min = 100; T.impulses = 0; T.mag_max = 0;
            T.lift_max = 0; T.yaw_max = T.roll_max = T.pitch_max = 0; T.loss_at = 0;
            T.seq0 = seq - 1;       /* count this impulse too */
            td5_roam_traffic_dev_release();
            return (uint32_t)TD5_INPUT_BRAKE;
        }
        if (seq != T.seq0) T.seq0 = seq;   /* some other pair's contact */
        if (T.s > T.car_s + 12.0) {
            TD5_LOG_I(LOG_TAG, "[ROAMRAM] ram %d: MISSED (passed the car at %.0f km/h)", T.n + 1, rr_kmh(a));
            T.misses++;
            T.phase = RR_SETTLE; T.phase_t0 = tick;
            td5_roam_traffic_dev_clear();
            return (uint32_t)TD5_INPUT_BRAKE;
        }
        T.kmh_prev = rr_kmh(a);
        return rr_drive(a, kmh_t);
    }
    case RR_HIT: {
        const double vy = (double)a->linear_velocity_y / 256.0 * RR_HZ / RR_M;
        const int32_t yw = abs(a->angular_velocity_yaw), rl = abs(a->angular_velocity_roll), pt = abs(a->angular_velocity_pitch);
        td5_physics_dev_last_v2v(&seq, &ia, &ib, &mag, &heavy, &pct);
        if (seq != T.seq0 && ((ia == 0 && ib == T.slot) || (ib == 0 && ia == T.slot))) {
            T.impulses += (int)(seq - T.seq0);
            if (mag > T.mag_max) T.mag_max = mag;
            if (heavy) { T.heavy = 1; if (pct < T.pct_min) T.pct_min = pct; }
            T.seq0 = seq;
        } else if (seq != T.seq0) T.seq0 = seq;
        if (vy > T.lift_max) T.lift_max = vy;
        if (yw > T.yaw_max) T.yaw_max = yw;
        if (rl > T.roll_max) T.roll_max = rl;
        if (pt > T.pitch_max) T.pitch_max = pt;
        if (tick - T.hit_t == 10) T.loss_at = T.v_hit - rr_kmh(a);
        if (tick - T.hit_t >= RR_WIN) {
            rr_row();
            T.contacts++;
            T.phase = RR_SETTLE; T.phase_t0 = tick;
            td5_roam_traffic_dev_clear();
        }
        return rr_stop_bits(a);
    }
    case RR_SETTLE:
        if (rr_kmh(a) < 3.0 && tick - T.phase_t0 > 45) {
            if (++T.n >= RR_N) { T.phase = RR_END; T.phase_t0 = tick; return 0; }
            T.phase = RR_PLAN;
            return 0;
        }
        if (tick - T.phase_t0 > 600) { rr_finish("the car never settled", 1); return 0; }
        return rr_stop_bits(a);
    case RR_END:
        if (tick - T.phase_t0 > 30) rr_finish("all rams done", 0);
        return (uint32_t)TD5_INPUT_HANDBRAKE;
    default:
        return 0;
    }
}

int td5_roam_ram_test_verdict(char *note, size_t n)
{
    int status = 0;
    if (!T.done || T.failed) status = 2;
    else if (T.misses > 0 || T.contacts < RR_N) status = 2;
    snprintf(note, n, "%sram contacts %d/%d, misses %d%s%.60s", status == 2 ? "FAIL " : "", T.contacts, RR_N, T.misses,
             (T.failed && T.why[0]) ? ": " : "", T.failed ? T.why : "");
    TD5_LOG_I(LOG_TAG, "[ROAMRAM] VERDICT %s: %s", status == 2 ? "FAIL" : "PASS", note);
    return status;
}

#endif /* TD5RE_RELEASE */
