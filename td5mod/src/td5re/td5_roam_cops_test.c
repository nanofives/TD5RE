/**
 * td5_roam_cops_test.c -- GEO FREE ROAM: the `roam-cops` self-test drive + verdict (PORT-ONLY, DEV-ONLY, F10).
 * Contract: td5_roam_cops.h (DEV section).
 *
 * Two scripted scenarios in one free-roam session, both through the real input path (the bits are ORed in by
 * td5_input.c like the other dev drives), the real cop module, physics and contacts:
 *
 *   A. ESCAPE BY DISTANCE. The player's path is an A* leg ~1.6 km ahead of the spawn (td5_roam_route_plan_path).
 *      A parked patrol cop is put beside it 110 m on. The player drives the path at up to 95 km/h (100 km/h in
 *      the full row), speeds past the cop (pursuit), and keeps going until the module counts an escape. The cops'
 *      pursuit top speed is capped at 70 km/h for this scenario only (td5_roam_cops_dev_force), so the drive can
 *      out-run them on city streets; the real default is TD5RE_ROAM_COPS_VMAX 125.
 *   B. STOP -> BUST. The cap goes, a second parked cop is put 90 m ahead on a new leg, the player passes it at
 *      80 km/h (speeding over 60), brakes to a stop 60 m past it and holds still. The pursuers box it in, the bust
 *      meter fills, BUSTED, the fine, and the car is put back on the street.
 *
 * Random patrols are off during the test (dev force: 0 per anchor) so the two placed cops are the only patrols;
 * reinforcements still come with the heat. The smoke row runs with no ambient traffic, the full row with the knob
 * density.
 *
 * Shots (log dir, TD5RE_D3D12_CAPTURE=1 arms the capture): roam_cops_pursuit_{a,b,c}.png (rear view, a lit pursuer
 * 8..40 m behind, 3 frames 4 ticks apart: the strobe flashes), roam_cops_busted.png (the BUSTED banner),
 * roam_cops_busted_rear_{a,b,c}.png (rear view during the hold).
 *
 * ONE file-scope struct. Compiled out of RELEASE.
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
#include "td5_race_state.h"
#include "td5_roam_mode.h"
#include "td5_roam_locate.h"
#include "td5_roam_route.h"
#include "td5_roam_cops.h"
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define CT_M    430.0
#define CT_PI   3.14159265358979323846
#define CT_HZ   30.0
#define CT_DS   2.0
#define CT_PTS  1600

enum { CT_WAIT = 0, CT_A_PLAN, CT_A_DRIVE, CT_COOL, CT_B_PLAN, CT_B_DRIVE, CT_B_STOP, CT_END };

static struct {
    int    mode, phase, done, failed;
    char   why[80];
    long   tick_last, phase_t0, t_start;
    uint32_t bits_last;
    /* the player's path (PLACE frame), resampled every CT_DS, speed profile */
    double px[CT_PTS], pz[CT_PTS], pv[CT_PTS];
    int    np, pi;
    double s, kmh_cap;
    int    replans;
    int    cop_slot;
    double cop_s;                       /* where the placed cop stands on the path */
    TD5_RoamCopsStats st0;              /* counters at the scenario start */
    long   t_pursuit_a, t_escape, t_pursuit_b, t_bust;
    int    shot_p, shot_b, shot_br, rear;
    long   bust_seen_t;
    int    bust_was;
    int    respawned, respawn_on_road;
    double kmh_max;
    /* [G1b] roadblock scenarios */
    int    sc, cool_planned, shots_n, bn;
    double lane_off;                    /* the drive's lateral offset (right of the path), metres */
    double bcx, bcz, btx, btz, bgx, bgz, bgw, stop_x, stop_z;
    long   cross_t, stuck_t;
    TD5_RoamCopsStats rb_base;
} T;

void td5_roam_cops_test_arm(int mode)
{
    memset(&T, 0, sizeof T);
    T.mode = mode;
    T.cop_slot = -1;
    T.lane_off = 1.6;
    TD5_LOG_I(LOG_TAG, "[ROAMCOPS] armed: %s", mode == TD5_RCOPS_FULL ? "full" : mode == TD5_RCOPS_ROADBLOCK ? "roadblock" : "smoke");
}

void td5_roam_cops_test_disarm(void)
{
    if (T.mode) td5_roam_cops_dev_force(-1, -1);
    T.mode = 0;
}

int td5_roam_cops_test_mode(void) { return T.mode; }
int td5_roam_cops_test_done(void) { return T.mode && T.done; }

static void ct_finish(const char *why, int fail)
{
    if (T.done) return;
    T.done = 1;
    T.failed = fail;
    snprintf(T.why, sizeof T.why, "%s", why);
    td5_roam_cops_dev_force(-1, -1);
    TD5_LOG_I(LOG_TAG, "[ROAMCOPS] %s: %s", fail ? "ABORT" : "done", why);
}

static void ct_place_of(const TD5_Actor *a, double *x, double *z)
{
    int32_t ox, oz;
    td5_roam_world_origin(&ox, &oz);
    *x = a->world_pos.x / 256.0 + ox;
    *z = a->world_pos.z / 256.0 + oz;
}

static double ct_kmh(const TD5_Actor *a)
{
    return sqrt((double)a->linear_velocity_x * a->linear_velocity_x + (double)a->linear_velocity_z * a->linear_velocity_z) /
           256.0 * CT_HZ / CT_M * 3.6;
}

static int ct_inside(double px, double pz)
{
    const TD5_RoamResident *rs = td5_roam_resident();
    int32_t ox, oz;
    TD5_RoamHit h;
    if (!rs) return 0;
    td5_roam_world_origin(&ox, &oz);
    return td5_roam_locate(rs, (int32_t)floor((px - ox) * 256.0 + 0.5), (int32_t)floor((pz - oz) * 256.0 + 0.5), 0, 0,
                           ROAM_PRIM_NONE, &h) && h.prim != ROAM_PRIM_NONE && h.inside;
}

/* the path from a polyline: resample, corner speed sqrt(4.5 R) over +-10 m capped at kmh_cap, braking 4 m/s2 */
static int ct_set_path(const double *qx, const double *qz, int nq)
{
    double cum[4096];
    int i, seg = 0, np;
    if (nq < 2 || nq > 4096) return 0;
    cum[0] = 0.0;
    for (i = 1; i < nq; i++) cum[i] = cum[i - 1] + hypot(qx[i] - qx[i - 1], qz[i] - qz[i - 1]) / CT_M;
    np = (int)(cum[nq - 1] / CT_DS) + 1;
    if (np > CT_PTS) np = CT_PTS;
    if (np < 10) return 0;
    for (i = 0; i < np; i++) {
        const double s = i * CT_DS;
        double t;
        while (seg + 2 < nq && cum[seg + 1] < s) seg++;
        t = cum[seg + 1] > cum[seg] ? (s - cum[seg]) / (cum[seg + 1] - cum[seg]) : 0.0;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        T.px[i] = qx[seg] + (qx[seg + 1] - qx[seg]) * t;
        T.pz[i] = qz[seg] + (qz[seg + 1] - qz[seg]) * t;
    }
    for (i = 0; i < np; i++) {
        const int a0 = i - 5 < 0 ? 0 : i - 5, a1 = i + 5 >= np ? np - 1 : i + 5;
        const int b0 = a0 + 1, b1 = a1 - 1 > 0 ? a1 - 1 : a1;
        const double h0 = atan2(T.px[b0] - T.px[a0], T.pz[b0] - T.pz[a0]);
        const double h1 = atan2(T.px[a1] - T.px[b1], T.pz[a1] - T.pz[b1]);
        double dh = h1 - h0, R, v;
        while (dh > CT_PI) dh -= 2.0 * CT_PI;
        while (dh < -CT_PI) dh += 2.0 * CT_PI;
        dh = fabs(dh);
        R = dh > 1e-3 ? ((a1 - a0) * CT_DS) / dh : 1e9;
        v = sqrt(4.5 * R) * 3.6;
        T.pv[i] = v < 18.0 ? 18.0 : (v > T.kmh_cap ? T.kmh_cap : v);
    }
    for (i = np - 2; i >= 0; i--) {
        const double vn = T.pv[i + 1] / 3.6, lim = sqrt(vn * vn + 2.0 * 4.0 * CT_DS) * 3.6;
        if (lim < T.pv[i]) T.pv[i] = lim;
    }
    T.np = np; T.pi = 0; T.s = 0.0;
    return 1;
}

/* plan the player's leg: `len_m` ahead of the car (rotated tries when the street runs out) */
static int ct_plan(const TD5_Actor *a, double len_m, double min_m)
{
    static const double k_rot[] = { 0.0, 0.5, -0.5, 1.0, -1.0, 1.5, -1.5 };
    double x, z, hx, hz;
    int k;
    ct_place_of(a, &x, &z);
    {
        const double y = (double)((((int)a->euler_accum.yaw) >> 8) & 0xFFF) * (2.0 * CT_PI / 4096.0);
        hx = sin(y); hz = cos(y);
    }
    for (k = 0; k < (int)(sizeof k_rot / sizeof k_rot[0]); k++) {
        const double c = cos(k_rot[k]), s = sin(k_rot[k]);
        const double dx = hx * c - hz * s, dz = hx * s + hz * c;
        double *qx = NULL, *qz = NULL, len = 0.0;
        int nq = 0;
        if (td5_roam_route_plan_path(x, z, hx, hz, x + dx * len_m * CT_M, z + dz * len_m * CT_M, 200.0, NULL, 0, &qx, &qz, &nq, &len) &&
            len >= min_m && ct_set_path(qx, qz, nq)) {
            free(qx); free(qz);
            TD5_LOG_I(LOG_TAG, "[ROAMCOPS] player leg: %.0f m (%d points, try %d)", len, T.np, k);
            return 1;
        }
        free(qx); free(qz);
    }
    return 0;
}

static void ct_point(double s, double off, double *x, double *z, double *tx, double *tz)
{
    int i = (int)floor(s / CT_DS);
    double t, sx, sz, l;
    if (i < 0) i = 0;
    if (i > T.np - 2) i = T.np - 2;
    t = s / CT_DS - i;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    sx = T.px[i + 1] - T.px[i]; sz = T.pz[i + 1] - T.pz[i];
    l = sqrt(sx * sx + sz * sz);
    if (l <= 0.0) { sx = 0.0; sz = 1.0; l = 1.0; }
    sx /= l; sz /= l;
    *x = T.px[i] + (T.px[i + 1] - T.px[i]) * t + sz * off * CT_M;
    *z = T.pz[i] + (T.pz[i + 1] - T.pz[i]) * t - sx * off * CT_M;
    if (tx) { *tx = sx; *tz = sz; }
}

static void ct_track(double x, double z)
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
    T.s = (best + bt) * CT_DS;
}

/* drive the path at its profile (capped), right lane where the road allows */
static uint32_t ct_drive(const TD5_Actor *a, double kmh_cap)
{
    double x, z, ax, az, hx, hz, err, kmh_t, kmh, look;
    uint32_t bits = 0;
    int i;
    ct_place_of(a, &x, &z);
    ct_track(x, z);
    kmh = ct_kmh(a);
    if (kmh > T.kmh_max) T.kmh_max = kmh;
    look = 6.0 + kmh / 3.6 * 0.45;
    if (look > 22.0) look = 22.0;
    {
        double off = T.lane_off, tx, tz;
        int tries;
        for (tries = 0; tries < 3; tries++) {
            ct_point(T.s + look, off, &ax, &az, &tx, &tz);
            if (ct_inside(ax, az) && ct_inside(ax + tz * 1.1 * CT_M, az - tx * 1.1 * CT_M)) break;
            off *= 0.5;
        }
        if (tries == 3) ct_point(T.s + look, 0.0, &ax, &az, NULL, NULL);
    }
    i = (int)floor((T.s + (kmh / 3.6) * 0.4 + 2.0) / CT_DS);
    if (i > T.np - 1) i = T.np - 1;
    kmh_t = T.pv[i < 0 ? 0 : i];
    if (kmh_t > kmh_cap) kmh_t = kmh_cap;
    if (kmh > 5.0) { hx = a->linear_velocity_x; hz = a->linear_velocity_z; }
    else { const double y = (double)(a->display_angles.yaw & 0xFFF) * (2.0 * CT_PI / 4096.0); hx = sin(y); hz = cos(y); }
    err = atan2(ax - x, az - z) - atan2(hx, hz);
    while (err > CT_PI) err -= 2.0 * CT_PI;
    while (err < -CT_PI) err += 2.0 * CT_PI;
    if (err > 0.03) bits |= (uint32_t)TD5_INPUT_STEER_LEFT;
    else if (err < -0.03) bits |= (uint32_t)TD5_INPUT_STEER_RIGHT;
    if (kmh < kmh_t) bits |= (uint32_t)TD5_INPUT_THROTTLE;
    else if (kmh > kmh_t * 1.08 + 2.0) bits |= (uint32_t)TD5_INPUT_BRAKE;
    return bits;
}

/* a parked patrol cop beside the path at s (right of travel, the first offset whose footprint fits) */
static int ct_place_cop(double s)
{
    static const double k_off[] = { 3.2, 2.6, -3.2, -2.6, 4.0 };
    int k;
    for (k = 0; k < (int)(sizeof k_off / sizeof k_off[0]); k++) {
        double x, z, tx, tz;
        int ok = 1, q;
        ct_point(s, k_off[k], &x, &z, &tx, &tz);
        for (q = 0; q < 5 && ok; q++) {
            const double l = q == 4 ? 0.0 : ((q & 1) ? 2.3 : -2.3), w = q == 4 ? 0.0 : ((q & 2) ? 1.0 : -1.0);
            if (!ct_inside(x + (tx * l + tz * w) * CT_M, z + (tz * l - tx * w) * CT_M)) ok = 0;
        }
        if (!ok) continue;
        T.cop_slot = td5_roam_cops_dev_place(x, z, (int)floor(atan2(tx, tz) * (4096.0 / (2.0 * CT_PI)) + 0.5) & 0xFFF, 1);
        if (T.cop_slot >= 0) { T.cop_s = s; return 1; }
    }
    return 0;
}

static void ct_shot(const char *name)
{
    char path[512];
    const char *dir = td5_plat_log_dir();
    snprintf(path, sizeof path, "%sroam_cops_%s.png", dir ? dir : "", name);
    td5_plat_request_frame_dump(path);
    TD5_LOG_I(LOG_TAG, "[ROAMCOPS] shot %s -> %s", name, path);
}

static uint32_t ct_bits(TD5_Actor *a, long tick, int full);
static uint32_t rb_bits(TD5_Actor *a, long tick);

uint32_t td5_roam_cops_test_bits(TD5_Actor *a)
{
    const long tick = (long)g_td5.simulation_tick_counter;
    const int full = T.mode == TD5_RCOPS_FULL;
    if (!T.mode || T.done || !a || !td5_roam_active()) return 0;
    /* the input layer may poll more than once per sim tick: the same bits for the whole tick (a rear-view bit that
     * drops on the second poll switches the rear camera straight back off) */
    if (tick == T.tick_last) return T.bits_last;
    T.tick_last = tick;
    T.bits_last = ct_bits(a, tick, full);
    return T.bits_last;
}

static uint32_t ct_bits(TD5_Actor *a, long tick, int full)
{
    TD5_RoamCopsStats st;
    uint32_t bits = 0;
    if (T.mode == TD5_RCOPS_ROADBLOCK) return rb_bits(a, tick);
    td5_roam_cops_stats(&st);
    if (T.phase != CT_WAIT && tick - T.t_start > (full ? 10800L : 9000L)) { ct_finish("timeout", 1); return 0; }
    switch (T.phase) {
    case CT_WAIT:
        if (tick < 90 || !td5_roam_route_graph_ready() || st.pool <= 0) return 0;
        T.t_start = tick;
        td5_roam_cops_dev_force(0, 70);
        T.kmh_cap = full ? 100.0 : 95.0;
        if (!ct_plan(a, 1600.0, 900.0)) { ct_finish("no 900 m leg ahead of the spawn", 1); return 0; }
        T.phase = CT_A_PLAN;
        return (uint32_t)TD5_INPUT_BRAKE;
    case CT_A_PLAN:
        T.st0 = st;
        if (!ct_place_cop(110.0)) { ct_finish("scenario A: no spot for the patrol cop", 1); return 0; }
        TD5_LOG_I(LOG_TAG, "[ROAMCOPS] scenario A: patrol cop slot %d parked 110 m ahead; drive at up to %.0f km/h", T.cop_slot, T.kmh_cap);
        T.phase = CT_A_DRIVE; T.phase_t0 = tick;
        return 0;
    case CT_A_DRIVE:
        if (!T.t_pursuit_a && st.pursuits > T.st0.pursuits) {
            T.t_pursuit_a = tick;
            TD5_LOG_I(LOG_TAG, "[ROAMCOPS] scenario A: pursuit started at %.1f s, %.0f m along, %.0f km/h", (tick - T.phase_t0) / CT_HZ,
                      T.s, ct_kmh(a));
        }
        if (T.t_pursuit_a && !T.shot_p) {
            if (T.rear > 0) {
                bits |= (uint32_t)TD5_INPUT_REAR_VIEW;
                /* the chase camera swings round over a few ticks; the strobe flashes, so 3 frames 4 ticks apart */
                ++T.rear;
                if (T.rear == 25) ct_shot("pursuit_a");
                if (T.rear == 29) ct_shot("pursuit_b");
                if (T.rear == 33) ct_shot("pursuit_c");
                if (T.rear >= 34) { T.rear = 0; T.shot_p = 1; }
            } else if (td5_roam_cops_dev_pursuer_behind(8.0, 40.0)) T.rear = 1;
        }
        if (st.escapes > T.st0.escapes) {
            T.t_escape = tick;
            TD5_LOG_I(LOG_TAG, "[ROAMCOPS] scenario A: escape at %.1f s (%s), %ld pursuers spawned", (tick - T.phase_t0) / CT_HZ,
                      st.escapes_dist > T.st0.escapes_dist ? "distance" : "line of sight", st.reinforcements - T.st0.reinforcements);
            T.phase = CT_COOL; T.phase_t0 = tick;
            return (uint32_t)TD5_INPUT_BRAKE;
        }
        if (!T.t_pursuit_a && T.s > T.cop_s + 80.0) { ct_finish("scenario A: passed the cop at speed, no pursuit", 1); return 0; }
        if (T.s > (T.np - 1) * CT_DS - 40.0) {
            if (++T.replans > 6 || !ct_plan(a, 1200.0, 300.0)) { ct_finish("scenario A: the road ran out before an escape", 1); return 0; }
        }
        return bits | ct_drive(a, T.kmh_cap);
    case CT_COOL:
        /* the grace after an escape (5 s) runs out at a calm 40 km/h */
        if (T.s > (T.np - 1) * CT_DS - 40.0 && !ct_plan(a, 800.0, 200.0)) { ct_finish("cool-down: no road", 1); return 0; }
        if (tick - T.phase_t0 > 7 * 30) {
            td5_roam_cops_dev_force(0, -1);                    /* scenario B: the real pursuit top speed */
            if (!ct_plan(a, 700.0, 330.0)) { ct_finish("scenario B: no 330 m leg", 1); return 0; }
            T.phase = CT_B_PLAN;
        }
        return ct_drive(a, 40.0);
    case CT_B_PLAN:
        T.st0 = st;
        if (!ct_place_cop(90.0)) { ct_finish("scenario B: no spot for the patrol cop", 1); return 0; }
        TD5_LOG_I(LOG_TAG, "[ROAMCOPS] scenario B: patrol cop slot %d parked 90 m ahead; pass at 80 km/h, stop 60 m past it", T.cop_slot);
        T.phase = CT_B_DRIVE; T.phase_t0 = tick;
        return 0;
    case CT_B_DRIVE:
        if (!T.t_pursuit_b && st.pursuits > T.st0.pursuits) {
            T.t_pursuit_b = tick;
            TD5_LOG_I(LOG_TAG, "[ROAMCOPS] scenario B: pursuit started at %.0f m along, %.0f km/h", T.s, ct_kmh(a));
        }
        if (T.s > T.cop_s + 60.0) {
            if (!T.t_pursuit_b) { ct_finish("scenario B: passed the cop at speed, no pursuit", 1); return 0; }
            T.phase = CT_B_STOP; T.phase_t0 = tick;
            TD5_LOG_I(LOG_TAG, "[ROAMCOPS] scenario B: braking to a stop at %.0f m along", T.s);
        }
        return ct_drive(a, 80.0);
    case CT_B_STOP:
        {
            const int busted = td5_roam_cops_dev_busted();
            if (busted && !T.bust_was) { T.bust_seen_t = tick; T.t_bust = tick; T.bust_was = 1; }
            if (busted) {
                if (!T.shot_b && tick - T.bust_seen_t == 20) { ct_shot("busted"); T.shot_b = 1; }
                if (!T.shot_br && tick - T.bust_seen_t >= 45) {
                    ++T.rear;
                    if (T.rear == 25) ct_shot("busted_rear_a");
                    if (T.rear == 29) ct_shot("busted_rear_b");
                    if (T.rear == 33) ct_shot("busted_rear_c");
                    if (T.rear >= 34) { T.rear = 0; T.shot_br = 1; }
                    return (uint32_t)TD5_INPUT_REAR_VIEW;
                }
            }
            if (T.bust_was && !busted) {
                T.respawned = st.player_respawns > T.st0.player_respawns;
                T.respawn_on_road = st.respawn_on_road;
                TD5_LOG_I(LOG_TAG, "[ROAMCOPS] scenario B: bust at %.1f s after the stop; respawn %s (%s)", (T.t_bust - T.phase_t0) / CT_HZ,
                          T.respawned ? "yes" : "NO", T.respawn_on_road ? "on the road" : "OFF THE ROAD");
                T.phase = CT_END; T.phase_t0 = tick;
            }
            if (tick - T.phase_t0 > 90 * 30) { ct_finish("scenario B: no bust within 90 s of stopping", 1); return 0; }
        }
        /* brake to a stop, then the handbrake: a brake held at a standstill engages reverse */
        return (uint32_t)(ct_kmh(a) > 4.0 ? TD5_INPUT_BRAKE : TD5_INPUT_HANDBRAKE);
    case CT_END:
        if (tick - T.phase_t0 > 60) ct_finish("both scenarios ran", 0);
        return (uint32_t)(ct_kmh(a) > 4.0 ? TD5_INPUT_BRAKE : TD5_INPUT_HANDBRAKE);
    default:
        return 0;
    }
}

static int ct_verdict(char *note, size_t n)
{
    TD5_RoamCopsStats s;
    char bad[80] = "";
    size_t bl = 0;
    int status = 0;
    long off_free;
    td5_roam_cops_stats(&s);
    off_free = s.off_ticks - s.off_ticks_contact;
#define CT_BAD(...) do { if (bl < sizeof bad - 1) bl += (size_t)snprintf(bad + bl, sizeof bad - bl, __VA_ARGS__); } while (0)
    if (!T.done) CT_BAD("not finished (%s); ", T.phase == CT_WAIT ? "never started" : "timeout");
    else if (T.failed) CT_BAD("%s; ", T.why);
    if (s.pursuits < 2) CT_BAD("pursuits %ld < 2; ", s.pursuits);
    if (s.escapes < 1) CT_BAD("no escape; ");
    if (s.busts < 1) CT_BAD("no bust; ");
    if (s.busts >= 1 && (!T.respawned || !T.respawn_on_road)) CT_BAD("respawn %s; ", T.respawned ? "off the road" : "missing");
    if (off_free > 0) CT_BAD("cop off surface %ld ticks; ", off_free);
#undef CT_BAD
    if (bad[0]) status = 2;
    else if (s.stuck_episodes > 0 || s.off_events > 0) status = 1;
    snprintf(note, n, "%spursuit %ld esc %ld (dist %ld) bust %ld fine $%ld heat %d cops %d stuck %ld off %ld+%ldhit %s%.48s",
             status == 2 ? "FAIL " : "", s.pursuits, s.escapes, s.escapes_dist, s.busts, s.fines, s.max_heat, s.max_pursuers,
             s.stuck_episodes, off_free, s.off_ticks_contact, bad[0] ? ": " : "", bad);
    TD5_LOG_I(LOG_TAG, "[ROAMCOPS] VERDICT %s: %s | spawns %ld despawns %ld reinforcements %ld joins %ld | notices speed %ld red %ld ram %ld "
              "| escapes %ld (los %ld dist %ld) busts %ld fines $%ld respawns %ld (on road %d) | red runs %ld | recoveries %ld stuck %ld "
              "removed %ld | off ticks %ld (%ld within 2 s of a contact) events %ld rescues %ld | road pushes %ld pinches %ld | barrier hits %ld | plans %ld fails %ld | "
              "wrecks %ld | pursuit %.1f s, max heat %d, max pursuers %d, cop vmax %.0f km/h | player vmax %.0f km/h | A: pursuit %.1f s "
              "escape %.1f s; B: pursuit %d bust %d",
              status == 2 ? "FAIL" : (status ? "WARN" : "PASS"), note, s.spawns, s.despawns, s.reinforcements, s.joins, s.notice_speed,
              s.notice_red, s.notice_ram, s.escapes, s.escapes_los, s.escapes_dist, s.busts, s.fines, s.player_respawns,
              s.respawn_on_road, s.red_runs, s.recoveries, s.stuck_episodes, s.stuck_removed, s.off_ticks, s.off_ticks_contact,
              s.off_events, s.off_rescues, s.road_pushes, s.road_pinches, s.barrier_hits, s.plans, s.plan_fails, s.wrecks, s.pursuit_ticks / CT_HZ, s.max_heat,
              s.max_pursuers, s.max_kmh, T.kmh_max, T.t_pursuit_a ? (T.t_pursuit_a - T.t_start) / CT_HZ : -1.0,
              T.t_escape ? (T.t_escape - T.t_start) / CT_HZ : -1.0, T.t_pursuit_b != 0, T.t_bust != 0);
    return status;
}

/* ============================================================ roadblock scenarios [G1b] ===== */

enum { RB_WAIT = 0, RB_COP, RB_TRIG, RB_WAITBLK, RB_RUN, RB_STOP, RB_BUSTWAIT, RB_COOL, RB_END };

/* the active roadblock the player is still driving towards (60..480 m before it along its tangent) */
static int rb_find_block(const TD5_Actor *a)
{
    double px, pz;
    int i;
    ct_place_of(a, &px, &pz);
    for (i = 0; i < 2; i++) {
        double cx, cz, tx, tz, gx, gz, gw, f;
        int nc;
        if (!td5_roam_cops_dev_block(i, &cx, &cz, &tx, &tz, &gx, &gz, &gw, &nc)) continue;
        f = ((px - cx) * tx + (pz - cz) * tz) / CT_M;
        if (f < -50.0 && f > -480.0) {
            T.bcx = cx; T.bcz = cz; T.btx = tx; T.btz = tz; T.bgx = gx; T.bgz = gz; T.bgw = gw; T.bn = nc;
            return 1;
        }
    }
    return 0;
}

static int rb_block_alive(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        double cx, cz, tx, tz, gx, gz, gw;
        int nc;
        if (td5_roam_cops_dev_block(i, &cx, &cz, &tx, &tz, &gx, &gz, &gw, &nc) && hypot(cx - T.bcx, cz - T.bcz) < 1.0) return 1;
    }
    return 0;
}

/* plan the player's way to the street point 60 m before the block, then the given waypoints (PLACE frame, metres off the
 * block centre along its tangent / across it): mode 0 = through the gap, 1 = to a stop 7 m short of the block */
static int rb_plan_block(const TD5_Actor *a, int mode)
{
    double x, z, hx, hz, *qx = NULL, *qz = NULL, len = 0.0, gx, gz;
    double *bx, *bz;
    int nq = 0, k = 0, ok = 0;
    ct_place_of(a, &x, &z);
    {
        const double y = (double)((((int)a->euler_accum.yaw) >> 8) & 0xFFF) * (2.0 * CT_PI / 4096.0);
        hx = sin(y); hz = cos(y);
    }
    gx = T.bcx - T.btx * 60.0 * CT_M; gz = T.bcz - T.btz * 60.0 * CT_M;
    if (!td5_roam_route_plan_path(x, z, hx, hz, gx, gz, 200.0, NULL, 0, &qx, &qz, &nq, &len) || nq < 2) { free(qx); free(qz); return 0; }
    bx = (double *)malloc((size_t)(nq + 4) * sizeof(double));
    bz = (double *)malloc((size_t)(nq + 4) * sizeof(double));
    if (bx && bz) {
        memcpy(bx, qx, (size_t)nq * sizeof(double)); memcpy(bz, qz, (size_t)nq * sizeof(double));
        k = nq;
        if (mode == 0) {
            bx[k] = T.bgx - T.btx * 25.0 * CT_M; bz[k] = T.bgz - T.btz * 25.0 * CT_M; k++;
            bx[k] = T.bgx; bz[k] = T.bgz; k++;
            bx[k] = T.bgx + T.btx * 40.0 * CT_M; bz[k] = T.bgz + T.btz * 40.0 * CT_M; k++;
        } else {
            bx[k] = T.bcx - T.btx * 7.0 * CT_M; bz[k] = T.bcz - T.btz * 7.0 * CT_M; k++;
            T.stop_x = bx[k - 1]; T.stop_z = bz[k - 1];
        }
        ok = ct_set_path(bx, bz, k);
    }
    free(bx); free(bz); free(qx); free(qz);
    if (ok) TD5_LOG_I(LOG_TAG, "[ROAMCOPS] roadblock leg (%s): %.0f m to the street point 60 m short, %d points", mode ? "stop" : "gap", len, T.np);
    return ok;
}

/* a leg that leaves the street the roadblock is on: first try whose whole path stays >= 120 m from the block */
static int rb_plan_detour(const TD5_Actor *a)
{
    static const double k_rot[] = { 0.8, -0.8, 1.4, -1.4, 2.0, -2.0, 2.6, -2.6 };
    double x, z, hx, hz;
    int k;
    ct_place_of(a, &x, &z);
    {
        const double y = (double)((((int)a->euler_accum.yaw) >> 8) & 0xFFF) * (2.0 * CT_PI / 4096.0);
        hx = sin(y); hz = cos(y);
    }
    for (k = 0; k < (int)(sizeof k_rot / sizeof k_rot[0]); k++) {
        const double c = cos(k_rot[k]), s = sin(k_rot[k]);
        const double dx = hx * c - hz * s, dz = hx * s + hz * c;
        double *qx = NULL, *qz = NULL, len = 0.0, dmin = 1e300;
        int nq = 0, i;
        if (td5_roam_route_plan_path(x, z, hx, hz, x + dx * 700.0 * CT_M, z + dz * 700.0 * CT_M, 200.0, NULL, 0, &qx, &qz, &nq, &len) &&
            len >= 300.0) {
            for (i = 0; i < nq; i++) {
                const double d = hypot(qx[i] - T.bcx, qz[i] - T.bcz) / CT_M;
                if (d < dmin) dmin = d;
            }
            if (dmin >= 120.0 && ct_set_path(qx, qz, nq)) {
                TD5_LOG_I(LOG_TAG, "[ROAMCOPS] detour leg: %.0f m, closest to the block %.0f m (rot %.1f)", len, dmin, k_rot[k]);
                free(qx); free(qz);
                return 1;
            }
        }
        free(qx); free(qz);
    }
    return 0;
}

static uint32_t rb_hold(const TD5_Actor *a)
{
    return (uint32_t)(ct_kmh(a) > 4.0 ? TD5_INPUT_BRAKE : TD5_INPUT_HANDBRAKE);
}

static double rb_dist_to_block(const TD5_Actor *a)
{
    double px, pz;
    ct_place_of(a, &px, &pz);
    return hypot(px - T.bcx, pz - T.bcz) / CT_M;
}

static uint32_t rb_bits(TD5_Actor *a, long tick)
{
    TD5_RoamCopsStats st;
    uint32_t bits = 0;
    td5_roam_cops_stats(&st);
    if (T.phase != RB_WAIT && tick - T.t_start > 25200L) { ct_finish("timeout", 1); return 0; }
    switch (T.phase) {
    case RB_WAIT:
        if (tick < 90 || !td5_roam_route_graph_ready() || st.pool <= 0) return 0;
        T.t_start = tick; T.sc = 1;
        td5_roam_cops_dev_force(0, 60);
        T.kmh_cap = 95.0; T.lane_off = 1.6;
        if (!ct_plan(a, 1600.0, 900.0)) { ct_finish("no 900 m leg ahead of the spawn", 1); return 0; }
        T.phase = RB_COP;
        return (uint32_t)TD5_INPUT_BRAKE;
    case RB_COP:
        T.st0 = st;
        if (!ct_place_cop(110.0)) { ct_finish("roadblock: no spot for the patrol cop", 1); return 0; }
        TD5_LOG_I(LOG_TAG, "[ROAMCOPS] roadblock scenario %d: patrol cop slot %d parked 110 m ahead; speed past it", T.sc, T.cop_slot);
        T.phase = RB_TRIG; T.phase_t0 = tick;
        return 0;
    case RB_TRIG:
        if (st.pursuits > T.st0.pursuits) {
            td5_roam_cops_dev_set_heat(3);
            T.kmh_cap = 60.0; T.lane_off = 1.6;
            T.phase = RB_WAITBLK; T.phase_t0 = tick;
            TD5_LOG_I(LOG_TAG, "[ROAMCOPS] roadblock scenario %d: pursuit, heat raised to 3 (%.0f km/h)", T.sc, ct_kmh(a));
        } else if (T.s > T.cop_s + 80.0) { ct_finish("roadblock: passed the cop at speed, no pursuit", 1); return 0; }
        if (T.s > (T.np - 1) * CT_DS - 40.0 && !ct_plan(a, 1200.0, 300.0)) { ct_finish("roadblock: road ran out", 1); return 0; }
        return ct_drive(a, T.kmh_cap);
    case RB_WAITBLK:
        if (rb_find_block(a)) {
            const int ok = T.sc == 3 ? rb_plan_detour(a) : rb_plan_block(a, T.sc == 1 ? 0 : 1);
            if (!ok) { ct_finish(T.sc == 3 ? "roadblock: no detour leg" : "roadblock: no leg to the block", 1); return 0; }
            T.lane_off = 0.0;
            T.rb_base = st;
            T.shots_n = 0; T.stuck_t = 0;
            TD5_LOG_I(LOG_TAG, "[ROAMCOPS] roadblock scenario %d: block at place (%.0f,%.0f), %d cars, gap %.1f m, %.0f m ahead", T.sc, T.bcx, T.bcz,
                      T.bn, T.bgw, rb_dist_to_block(a));
            T.phase = T.sc == 2 ? RB_STOP : RB_RUN; T.phase_t0 = tick;
            return ct_drive(a, 45.0);
        }
        if (tick - T.phase_t0 > 30 * 30) { ct_finish("roadblock: none placed within 30 s of heat 3", 1); return 0; }
        if (T.s > (T.np - 1) * CT_DS - 40.0 && !ct_plan(a, 1200.0, 300.0)) { ct_finish("roadblock: road ran out", 1); return 0; }
        return ct_drive(a, T.kmh_cap);
    case RB_RUN: {
        const double d = rb_dist_to_block(a);
        const double cap = T.sc == 3 ? 50.0 : 45.0;
        if (T.sc == 3) {
            if (st.blocks_detoured > T.rb_base.blocks_detoured) {
                TD5_LOG_I(LOG_TAG, "[ROAMCOPS] scenario 3: detour counted at %.0f m from the block", d);
                T.phase = RB_END; T.phase_t0 = tick;
                return ct_drive(a, cap);
            }
            if (tick - T.phase_t0 > 45 * 30) { ct_finish("scenario 3: no detour counted within 45 s", 1); return 0; }
            if (T.s > (T.np - 1) * CT_DS - 30.0) { ct_finish("scenario 3: the detour leg ended before a detour was counted", 1); return 0; }
            return ct_drive(a, cap);
        }
        /* scenario 1: through the gap */
        if (d < 62.0 && T.shots_n == 0) { ct_shot("roadblock_a"); T.shots_n = 1; }
        if (d < 32.0 && T.shots_n == 1) { ct_shot("roadblock_b"); T.shots_n = 2; }
        if (st.blocks_gap + st.blocks_rammed > T.rb_base.blocks_gap + T.rb_base.blocks_rammed) {
            if (!T.cross_t) {
                T.cross_t = tick;
                TD5_LOG_I(LOG_TAG, "[ROAMCOPS] scenario 1: crossed the block (%s), %.0f km/h", st.blocks_rammed > T.rb_base.blocks_rammed ? "rammed" : "gap", ct_kmh(a));
            }
            if (tick - T.cross_t > 4 * 30) {
                T.sc = 2; T.st0 = st; T.kmh_cap = 50.0; T.lane_off = 1.6; T.cross_t = 0;
                if (!ct_plan(a, 1200.0, 300.0)) { ct_finish("scenario 2: no leg after the block", 1); return 0; }
                T.phase = RB_WAITBLK; T.phase_t0 = tick;
                return 0;
            }
        } else {
            if (ct_kmh(a) < 2.0 && d < 25.0) { if (++T.stuck_t > 4 * 30) { ct_finish("scenario 1: stopped at the block, not through", 1); return 0; } }
            else T.stuck_t = 0;
            if (!rb_block_alive()) { ct_finish("scenario 1: the block was removed before the player crossed it", 1); return 0; }
            if (tick - T.phase_t0 > 60 * 30) { ct_finish("scenario 1: no crossing within 60 s", 1); return 0; }
        }
        return ct_drive(a, cap);
    }
    case RB_STOP: {
        double px, pz, dist, cap;
        const int busted = td5_roam_cops_dev_busted();
        ct_place_of(a, &px, &pz);
        dist = hypot(px - T.stop_x, pz - T.stop_z) / CT_M;
        if (busted && !T.bust_was) { T.bust_was = 1; T.t_bust = tick; }
        if (T.bust_was && !busted) {
            T.respawned = st.player_respawns > T.st0.player_respawns;
            T.respawn_on_road = st.respawn_on_road;
            TD5_LOG_I(LOG_TAG, "[ROAMCOPS] scenario 2: bust at the block (blocks_bust %ld), respawn %s", st.blocks_bust, T.respawned ? "yes" : "NO");
            T.sc = 3; T.phase = RB_COOL; T.phase_t0 = tick; T.cool_planned = 0;
            return (uint32_t)TD5_INPUT_BRAKE;
        }
        if (busted) return rb_hold(a);
        if (!rb_block_alive() && !T.bust_was) { ct_finish("scenario 2: the block was removed before the bust", 1); return 0; }
        if (tick - T.phase_t0 > 60 * 30) { ct_finish("scenario 2: no bust at the block within 60 s", 1); return 0; }
        if (dist < 10.0 && T.shots_n == 0) { ct_shot("roadblock_c"); T.shots_n = 1; }
        if (dist < 3.0 || T.s > (T.np - 1) * CT_DS - 4.0) {
            if (T.shots_n == 1 && ct_kmh(a) < 4.0) { ct_shot("roadblock_stop"); T.shots_n = 2; }
            return rb_hold(a);
        }
        cap = sqrt(2.0 * 2.5 * (dist > 2.0 ? dist - 2.0 : 0.0)) * 3.6 + 3.0;
        if (cap > 45.0) cap = 45.0;
        return ct_drive(a, cap);
    }
    case RB_COOL:
        /* the 10 s grace after the bust runs out at a calm 40 km/h on a fresh leg; then scenario 3 starts like 1 */
        if (!T.cool_planned) {
            if (!ct_plan(a, 1600.0, 900.0)) { if (tick - T.phase_t0 > 30) { ct_finish("scenario 3: no leg after the bust", 1); return 0; } return rb_hold(a); }
            T.cool_planned = 1;
        }
        if (tick - T.phase_t0 > 12 * 30) {
            if (!ct_plan(a, 1600.0, 900.0)) { ct_finish("scenario 3: no 900 m leg", 1); return 0; }
            T.kmh_cap = 95.0; T.lane_off = 1.6;
            T.phase = RB_COP;
            return 0;
        }
        return ct_drive(a, 40.0);
    case RB_END:
        if (tick - T.phase_t0 > 90) ct_finish("three roadblock scenarios ran", 0);
        return ct_drive(a, 40.0);
    default:
        return 0;
    }
    (void)bits;
}

static int rb_verdict(char *note, size_t n)
{
    TD5_RoamCopsStats s;
    char bad[120] = "";
    size_t bl = 0;
    int status = 0;
    td5_roam_cops_stats(&s);
#define RB_BAD(...) do { if (bl < sizeof bad - 1) bl += (size_t)snprintf(bad + bl, sizeof bad - bl, __VA_ARGS__); } while (0)
    if (!T.done) RB_BAD("not finished (%s); ", T.phase == RB_WAIT ? "never started" : "timeout");
    else if (T.failed) RB_BAD("%s; ", T.why);
    if (s.blocks_placed < 3) RB_BAD("placed %ld < 3; ", s.blocks_placed);
    if (s.blocks_gap + s.blocks_rammed < 1) RB_BAD("no crossing; ");
    if (s.blocks_bust < 1) RB_BAD("no bust at a block; ");
    if (s.blocks_detoured < 1) RB_BAD("no detour; ");
    if (s.blocks_despawned < s.blocks_placed - 1) RB_BAD("despawned %ld of %ld; ", s.blocks_despawned, s.blocks_placed);
    if (s.blocks_bust >= 1 && (!T.respawned || !T.respawn_on_road)) RB_BAD("respawn %s; ", T.respawned ? "off the road" : "missing");
#undef RB_BAD
    if (bad[0]) status = 2;
    snprintf(note, n, "%sblocks placed %ld cars %ld gap %ld rammed %ld detour %ld bust %ld despawned %ld hits %ld fails %ld%s%.40s",
             status == 2 ? "FAIL " : "", s.blocks_placed, s.block_cars, s.blocks_gap, s.blocks_rammed, s.blocks_detoured, s.blocks_bust,
             s.blocks_despawned, s.block_hits, s.block_fails, bad[0] ? ": " : "", bad);
    TD5_LOG_I(LOG_TAG, "[ROAMCOPS] ROADBLOCK VERDICT %s: %s | pursuits %ld busts %ld fines $%ld max heat %d | spawns %ld despawns %ld | "
              "stuck %ld off ticks %ld events %ld | player vmax %.0f km/h",
              status == 2 ? "FAIL" : "PASS", note, s.pursuits, s.busts, s.fines, s.max_heat, s.spawns, s.despawns, s.stuck_episodes,
              s.off_ticks, s.off_events, T.kmh_max);
    return status;
}

int td5_roam_cops_test_verdict(char *note, size_t n)
{
    if (T.mode == TD5_RCOPS_ROADBLOCK) return rb_verdict(note, n);
    return ct_verdict(note, n);
}

#endif /* TD5RE_RELEASE */
