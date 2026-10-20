/**
 * td5_roam_drive.c -- GEO FREE ROAM: scripted drive + verdict for the `roam` selftest rows
 * (PORT-ONLY, DEV-ONLY, F2 task D). Contract: td5_roam_drive.h.
 *
 * The steering / pure-pursuit / corner-picker logic is F2-B's dev harness (td5_roam_physics.c,
 * TD5RE_ROAM_DRIVE=street|corners) re-expressed over PUBLIC APIs only (the resident set from
 * td5_roam_resident(), the locator, td5_roam_physics_stats), so the selftest does not depend on
 * that file's statics. The drive goes through the same overlay as a keyboard
 * (td5_input.c ORs the bits in), i.e. through the real steering ramp, physics, contact provider
 * and wall code.
 *
 * ONE file-scope struct (s_rv) holds the state. Compiled out of RELEASE.
 */
#ifndef TD5RE_RELEASE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_fp.h"
#include "td5_geo.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_track.h"
#include "td5_physics.h"
#include "td5_roam_tile.h"
#include "td5_roam_locate.h"
#include "td5_roam_mode.h"
#include "td5_roam_physics.h"
#include "td5_roam_drive.h"
#include "td5_race_state.h"       /* [F8] the other cars, for the traffic-aware drive */
#include "td5_roam_traffic.h"
#include "td5_roam_route.h"        /* [F8] td5_roam_route_snap_xz: the soak move */
#include "td5_roam_stream.h"     /* [F3] pin the casco for the corner matrix; miss counters */
#include "td5_roam_rebase.h"     /* [F9b] the route tour's world-frame memory follows a rebase */
#include "td5_geo_roads.h"      /* TD5_GEO_RC_* */
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RV_PI         3.14159265358979323846
#define RV_M          430.0                       /* units per metre */
#define RV_UPT_KMH    (430.0 / 30.0 / 3.6)        /* units per tick at 1 km/h (30 Hz sim) */
#define RV_NP         9
#define RV_PMAX       64                          /* [F2-E] a routed corner path has up to this many points */
#define RV_MAXC       320
#define RV_MAXKEYS    128
#define RV_UNIT_CM    (256.0 * 4.30)              /* 24.8 world units per cm */

#define RV_ABOVE_REST_MAX_CM   5.0                /* wheel bottom above the ground at rest */
#define RV_WHEEL_BELOW_MAX_CM  30.0               /* wheel bottom under the ground (tyre / suspension) */
#define RV_STUCK_TICKS         90                 /* 3 s */
#define RV_STUCK_KMH           5.0
/* [F2-E] corner matrix passes per corner: 0 held 40 km/h, 1 held 80 km/h, 2 BRAKING driver from 80 km/h */
#define RV_PASS_MAX            3
#define RV_BRAKE_ALAT          6.0                /* m/s^2: bend speed sqrt(a_lat * R) */
#define RV_BRAKE_DECEL         7.0                /* m/s^2: braking towards that speed */
#define RV_BRAKE_MIN_KMH       8.0

typedef struct {
    int kind;
    double cx, cz, inx, inz, outx, outz, defl;
    uint64_t cl, ein, eout;
    int np;                           /* path points: RV_NP for the Bezier, more once routed */
    int route;                        /* [F2-E] 0 not checked, 1 on the road, 2 routed, 3 off the road, no route */
    double off_m;                     /* metres of the Bezier off the drivable surface */
    double px[RV_PMAX], pz[RV_PMAX];
} RvCorner;

static const char *const k_rv_kind[4] = { "one-lane/service", "calle x calle", "avenida x calle", "diagonal x avenida" };
static const char *const k_rv_pass[RV_PASS_MAX] = { "held 40", "held 80", "brake 80" };

typedef struct {
    int      mode;
    int      armed_tick_set;
    int      env_checked, env_driven, env_reported;
    long     t0, tick_last;
    int      phase;                 /* route: 0 tour, 1 brake, 2 rest, 3 done ; corners: 0 idle, 1 run, 2 done */
    int      done, aborted;
    char     why[96];

    /* knobs */
    double   kmh;
    int      want_junctions, max_secs, per_kind, gap_m, max_arms;

    /* measured every sim tick (both modes) */
    long     samples, below_n, off_n;
    int32_t  above_min;             /* 24.8, chassis centre above the ground at the chassis */
    int      above_seen;
    uint32_t ch_hint;
    TD5_RoamHit ch;
    int      ch_ok;
    long     stuck_t;               /* consecutive ticks throttle held and < 5 km/h */
    long     stuck_events;
    long     slow_rt;               /* consecutive ticks throttle held and < 3 km/h (retry trigger) */
    int      rev_t, recoveries, retries_total;

    /* route */
    double   dir_x, dir_z, off0, dist_m;
    int      have_dir, have_off, have_last;
    int32_t  lx, lz;
    double   sp_x, sp_z;            /* spawn, for homing */
    /* [F3] TOUR (TD5RE_ROAM_SELFTEST_TOUR_M > 0): instead of homing past 1.4 km, drive round a
     * square of that side centred on the spawn (corners on the lattice axes) for
     * TD5RE_ROAM_SELFTEST_TOUR_SECS, then brake to rest. The streaming soak test. */
    double   tour_m;
    int      tour_secs, wp;
    int      traffic;               /* [F8] ambient traffic is on: shoves are reported, not judged */
    long     traffic_waits;         /* [F8] ticks the drive held back for a car ahead          */
    int      soak_moves;            /* [F8] wedges the soak got out of by moving the car on    */
    long     wp_t;
    double   wp_x[4], wp_z[4];
    uint64_t cl_key;
    double   cl_x, cl_z, arm_x, arm_z, tgt_x, tgt_z;
    int      crossings, distinct;
    uint64_t keys[RV_MAXKEYS];
    long     rest_t, log_t;
    double   kmh_max;
    int      on_path;               /* following the planned path through a junction */
    long     path_t0;
    double   pth_x[RV_NP], pth_z[RV_NP], pth_ox, pth_oz, pth_defl;

    /* corners */
    RvCorner c[RV_MAXC];
    int      nc, ci, si;
    long     ct, cstuck_t, slow_t, rev_ct, stalls;
    int      retries;
    long     hits0, pinch0, resc0;
    double   vmin, vmax;
    long     runs, ok, stuck, pinch, hits, resc;
    long     b_runs, b_ok, b_stuck;     /* [F2-E] braking-driver runs: the stuck / exit verdict */
    int      npass;                     /* 3 with the braking pass (TD5RE_ROAM_SELFTEST_CORNER_BRAKE), else 2 */
    long     k_runs[4][RV_PASS_MAX], k_ok[4][RV_PASS_MAX], k_stuck[4][RV_PASS_MAX], k_pinch[4][RV_PASS_MAX],
             k_hits[4][RV_PASS_MAX], k_resc[4][RV_PASS_MAX];
    int      kind_n[4];
    int      cand_n[4];             /* junctions with a valid candidate before the spacing / cap filter */
    FILE    *rep;
} RvState;

static RvState s_rv;

/* ------------------------------------------------------------ helpers -------- */

static double rv_speed_upt(const TD5_Actor *a)
{
    return sqrt((double)a->linear_velocity_x * a->linear_velocity_x +
                (double)a->linear_velocity_z * a->linear_velocity_z) / 256.0;
}

static void rv_heading(const TD5_Actor *a, double *hx, double *hz)
{
    const double sp = rv_speed_upt(a);
    if (sp > 20.0) {
        *hx = a->linear_velocity_x / 256.0 / sp;
        *hz = a->linear_velocity_z / 256.0 / sp;
    } else {
        const double yaw = (double)(a->display_angles.yaw & 0xFFF) * (2.0 * RV_PI / 4096.0);
        *hx = sin(yaw);
        *hz = cos(yaw);
    }
}

/* STEER_LEFT turns the heading toward +x: a positive atan2(x, z) error is a LEFT turn (F2-B, measured). */
static uint32_t rv_steer_to(const TD5_Actor *a, double tx, double tz, double kmh_target)
{
    double hx, hz, err;
    const double px = (double)FP_TRUNC(a->world_pos.x), pz = (double)FP_TRUNC(a->world_pos.z);
    const double kmh = rv_speed_upt(a) / RV_UPT_KMH;
    uint32_t bits = 0;
    rv_heading(a, &hx, &hz);
    err = atan2(tx - px, tz - pz) - atan2(hx, hz);
    while (err > RV_PI) err -= 2.0 * RV_PI;
    while (err < -RV_PI) err += 2.0 * RV_PI;
    if (err > 0.03) bits |= (uint32_t)TD5_INPUT_STEER_LEFT;
    else if (err < -0.03) bits |= (uint32_t)TD5_INPUT_STEER_RIGHT;
    if (kmh < kmh_target) bits |= (uint32_t)TD5_INPUT_THROTTLE;
    else if (kmh > kmh_target * 1.10) bits |= (uint32_t)TD5_INPUT_BRAKE;
    return bits;
}

static const char *rv_name(const RoamTile *t, unsigned id)
{
    const char *p = t->names, *e;
    unsigned k = 0;
    if (!p || id == 0xFFFFu) return "";
    e = p + t->names_bytes;
    while (p < e && k < id) { p += strlen(p) + 1; k++; }
    return p < e ? p : "";
}

static int rv_is(const char *n, const char *pfx) { return strncmp(n, pfx, strlen(pfx)) == 0; }

/* centre line of the chassis hit's ribbon quad, world units: point nearest the car + unit axis */
static int rv_street_axis(const TD5_RoamResident *rs, const TD5_RoamHit *h, double px, double pz,
                          double *cx, double *cz, double *ax, double *az)
{
    const uint32_t slot = ROAM_PRIM_SLOT(h->prim), idx = ROAM_PRIM_INDEX(h->prim);
    const RoamTile *t;
    const TD5_StripSpan *sp;
    int vl0, vl1, vr0, vr1, L;
    double bx, bz, c0x, c0z, c1x, c1z, dx, dz, len, tt;
    if (h->prim == ROAM_PRIM_NONE || ROAM_PRIM_IS_TRI(h->prim) || slot >= (uint32_t)rs->nslot) return 0;
    if (!(t = rs->tile[slot]) || idx >= t->nspan) return 0;
    sp = &t->span[idx];
    L = sp->pad_02[1] & 0x0F;
    if (L < 1) L = 1;
    td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
    bx = (double)t->org_x - rs->org_x + sp->origin_x;
    bz = (double)t->org_z - rs->org_z + sp->origin_z;
    c0x = bx + 0.5 * (t->vert[vl0].x + t->vert[vl0 + L].x);
    c0z = bz + 0.5 * (t->vert[vl0].z + t->vert[vl0 + L].z);
    c1x = bx + 0.5 * (t->vert[vr0].x + t->vert[vr0 + L].x);
    c1z = bz + 0.5 * (t->vert[vr0].z + t->vert[vr0 + L].z);
    dx = c1x - c0x; dz = c1z - c0z;
    len = sqrt(dx * dx + dz * dz);
    if (len < 1.0) return 0;
    *ax = dx / len; *az = dz / len;
    tt = (px - c0x) * *ax + (pz - c0z) * *az;
    *cx = c0x + *ax * tt;
    *cz = c0z + *az * tt;
    return 1;
}

/* The end row of street piece `key` nearest to (cx, cz), world units: its centre and the unit
 * direction along the ribbon pointing AWAY from that end into the street. 0 when not resident. */
static int rv_edge_end_near(const TD5_RoamResident *rs, uint64_t key, double cx, double cz,
                            double *ex, double *ez, double *ox, double *oz)
{
    double best = 1e30;
    int ti, found = 0, btx = 0, btz = 0;
    for (ti = 0; ti < rs->nslot; ti++) {
        const RoamTile *t = rs->tile[ti];
        uint32_t k;
        if (!t) continue;
        for (k = 0; k < t->nedge; k++) {
            const RoamEdgeRec *e = &t->edge[k];
            int end;
            if (e->key != key || e->span_count == 0) continue;
            for (end = 0; end < 2; end++) {
                const uint32_t si = end ? (uint32_t)e->span_first + e->span_count - 1 : e->span_first;
                const TD5_StripSpan *sp;
                int vl0, vl1, vr0, vr1, L;
                double bx, bz, nx0, nz0, fx0, fz0, qx, qz, rx, rz, d2, len;
                if (si >= t->nspan) continue;
                sp = &t->span[si];
                L = sp->pad_02[1] & 0x0F;
                if (L < 1) L = 1;
                td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
                bx = (double)t->org_x - rs->org_x + sp->origin_x;
                bz = (double)t->org_z - rs->org_z + sp->origin_z;
                nx0 = bx + 0.5 * (t->vert[vl0].x + t->vert[vl0 + L].x);
                nz0 = bz + 0.5 * (t->vert[vl0].z + t->vert[vl0 + L].z);
                fx0 = bx + 0.5 * (t->vert[vr0].x + t->vert[vr0 + L].x);
                fz0 = bz + 0.5 * (t->vert[vr0].z + t->vert[vr0 + L].z);
                if (end) { qx = fx0; qz = fz0; rx = nx0 - fx0; rz = nz0 - fz0; }   /* far row, inward = back */
                else     { qx = nx0; qz = nz0; rx = fx0 - nx0; rz = fz0 - nz0; }
                d2 = (qx - cx) * (qx - cx) + (qz - cz) * (qz - cz);
                len = sqrt(rx * rx + rz * rz);
                /* [F3] an exact tie (the border row two tiles' records share) goes to the lower
                 * (tx, tz), not to the lower resident slot: same pick whatever the load order */
                if (len < 1.0 || d2 > best ||
                    (d2 == best && (t->tx > btx || (t->tx == btx && t->tz >= btz)))) continue;
                best = d2; *ex = qx; *ez = qz; *ox = rx / len; *oz = rz / len; found = 1;
                btx = t->tx; btz = t->tz;
            }
        }
    }
    return found;
}

static const RoamEdgeRec *rv_edge(const TD5_RoamResident *rs, uint64_t key, const RoamTile **tout)
{
    /* [F3] the record of the lowest (tx, tz) tile that holds the edge: the same whatever the
     * resident slot order (streaming fills slots in arrival order) */
    const RoamEdgeRec *best = NULL;
    int i;
    uint32_t k;
    for (i = 0; i < rs->nslot; i++) {
        const RoamTile *t = rs->tile[i];
        if (!t || (best && (t->tx > (*tout)->tx || (t->tx == (*tout)->tx && t->tz > (*tout)->tz)))) continue;
        for (k = 0; k < t->nedge; k++)
            if (t->edge[k].key == key) { *tout = t; best = &t->edge[k]; break; }
    }
    return best;
}

/* way of a world point: the locator's edge key >> 16 (way id), 0 = none / patch */
static uint64_t rv_way_at(const TD5_RoamResident *rs, double x, double z)
{
    TD5_RoamHit h;
    if (!td5_roam_locate(rs, (int32_t)(x * 256.0), (int32_t)(z * 256.0), 0, 0, ROAM_PRIM_NONE, &h) ||
        h.prim == ROAM_PRIM_NONE || !h.inside || h.is_patch)
        return 0;
    return h.edge_key >> 16;
}

/* Pure pursuit on a RV_NP-point polyline whose last point is followed by a straight run along
 * (outx, outz): the arc length of the car's nearest point, aim `look` further. along_out > 0 once the
 * nearest point is past (total - 40 m), i.e. on the exit ribbon; lat_out = distance to the polyline. */
static void rv_pursue(const double *pxs, const double *pzs, double outx, double outz, double px, double pz,
                      double look, double *tx, double *tz, double *along_out, double *lat_out)
{
    double best = 1e30, cum = 0.0, total = 0.0, s_car = 0.0, want;
    int k;
    for (k = 0; k < RV_NP - 1; k++) {
        const double sx = pxs[k + 1] - pxs[k], sz = pzs[k + 1] - pzs[k];
        const double len = sqrt(sx * sx + sz * sz);
        double tt = len > 0.0 ? ((px - pxs[k]) * sx + (pz - pzs[k]) * sz) / (len * len) : 0.0, qx, qz, dd;
        if (tt < 0.0) tt = 0.0;
        if (tt > 1.0) tt = 1.0;
        qx = pxs[k] + sx * tt; qz = pzs[k] + sz * tt;
        dd = (px - qx) * (px - qx) + (pz - qz) * (pz - qz);
        if (dd < best) { best = dd; s_car = cum + tt * len; }
        cum += len;
    }
    total = cum;
    want = s_car + look;
    if (want > total) want = total + look;           /* past the end: keep going straight out */
    cum = 0.0;
    *tx = pxs[RV_NP - 1] + outx * (want - total);
    *tz = pzs[RV_NP - 1] + outz * (want - total);
    for (k = 0; k < RV_NP - 1; k++) {
        const double sx = pxs[k + 1] - pxs[k], sz = pzs[k + 1] - pzs[k];
        const double len = sqrt(sx * sx + sz * sz);
        if (want <= cum + len && len > 0.0) {
            *tx = pxs[k] + sx * (want - cum) / len;
            *tz = pzs[k] + sz * (want - cum) / len;
            break;
        }
        cum += len;
    }
    *along_out = s_car - (total - 40.0 * RV_M);
    *lat_out = sqrt(best);
}

/* The planned path through a junction: [0] 45 m up the in-ribbon, [1..RV_NP-2] a quadratic Bezier from the
 * in-ribbon end row through the point where the two street centre lines meet to the out-ribbon end row,
 * [RV_NP-1] 40 m out along it. (ein, eout) are end-row centres, (inx, inz) / (outx, outz) the unit
 * directions along each ribbon pointing AWAY from the junction. */
static void rv_plan_path(double *pxs, double *pzs, double eix, double eiz, double inx, double inz,
                         double eox, double eoz, double outx, double outz)
{
    const double ax_ = -inx, az_ = -inz, bx_ = -outx, bz_ = -outz;
    const double det = ax_ * (-bz_) - az_ * (-bx_);
    double qx = 0.5 * (eix + eox), qz = 0.5 * (eiz + eoz);
    int q;
    if (fabs(det) > 1e-6) {
        const double tt = ((eox - eix) * (-bz_) - (eoz - eiz) * (-bx_)) / det;
        if (tt > 0.0 && tt < 60.0 * RV_M) { qx = eix + ax_ * tt; qz = eiz + az_ * tt; }
    }
    pxs[0] = eix + inx * 45.0 * RV_M; pzs[0] = eiz + inz * 45.0 * RV_M;
    for (q = 0; q <= RV_NP - 3; q++) {
        const double u = (double)q / (double)(RV_NP - 3);
        const double w0 = (1 - u) * (1 - u), w1 = 2 * u * (1 - u), w2 = u * u;
        pxs[1 + q] = w0 * eix + w1 * qx + w2 * eox;
        pzs[1 + q] = w0 * eiz + w1 * qz + w2 * eoz;
    }
    pxs[RV_NP - 1] = eox + outx * 40.0 * RV_M;
    pzs[RV_NP - 1] = eoz + outz * 40.0 * RV_M;
}

static void rv_report_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void rv_report_line(const char *fmt, ...)
{
    va_list ap;
    if (!s_rv.rep) return;
    va_start(ap, fmt);
    vfprintf(s_rv.rep, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------ per-tick measurement -- */

/* The chassis against the road under it. A car under the ground (centre below the locator's
 * surface height) or centred outside every surface is a violation; both are rare enough that a
 * single occurrence fails the row. */
static void rv_measure(TD5_Actor *a, const TD5_RoamResident *rs)
{
    TD5_RoamHit h;
    s_rv.ch_ok = 0;
    s_rv.samples++;
    if (!td5_roam_locate(rs, a->world_pos.x, a->world_pos.z, a->world_pos.y, 1, s_rv.ch_hint, &h) ||
        h.prim == ROAM_PRIM_NONE) {
        s_rv.off_n++;
        return;
    }
    s_rv.ch = h;
    s_rv.ch_ok = 1;
    s_rv.ch_hint = h.prim;
    if (!h.inside) { s_rv.off_n++; return; }
    {
        const int32_t above = a->world_pos.y - h.ground_y;
        if (!s_rv.above_seen || above < s_rv.above_min) { s_rv.above_min = above; s_rv.above_seen = 1; }
        if (above < 0) s_rv.below_n++;
    }
}

/* ------------------------------------------------------------ route ---------- */

static void rv_finish(const char *why, int aborted)
{
    s_rv.done = 1;
    s_rv.aborted = aborted;
    snprintf(s_rv.why, sizeof s_rv.why, "%s", why);
    TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] drive finished: %s", why);
}

/* turn script: 0 straight, +1 / -1 a 90 degree turn */
static int rv_turn_for(int n)
{
    static const int k_script[8] = { 0, 1, 0, -1, 0, 1, 0, -1 };
    return k_script[n & 7];
}

static int rv_key_seen(uint64_t k)
{
    int i;
    for (i = 0; i < s_rv.distinct && i < RV_MAXKEYS; i++) if (s_rv.keys[i] == k) return 1;
    return 0;
}

static uint32_t rv_route(TD5_Actor *a, const TD5_RoamResident *rs, long tick)
{
    const double px = (double)FP_TRUNC(a->world_pos.x), pz = (double)FP_TRUNC(a->world_pos.z);
    const double kmh = rv_speed_upt(a) / RV_UPT_KMH;
    const int on_patch = s_rv.ch_ok && s_rv.ch.is_patch;
    double cx, cz, ax, az, hx, hz, look, tx = px, tz = pz;
    uint32_t bits = 0;

    if (s_rv.phase == 3) return 0;
    if (s_rv.have_last)
        s_rv.dist_m += sqrt((px - s_rv.lx) * (px - s_rv.lx) + (pz - s_rv.lz) * (pz - s_rv.lz)) / RV_M;
    s_rv.lx = (int32_t)px; s_rv.lz = (int32_t)pz; s_rv.have_last = 1;
    if (kmh > s_rv.kmh_max) s_rv.kmh_max = kmh;
    rv_heading(a, &hx, &hz);
    if (!s_rv.have_dir) {
        s_rv.dir_x = hx; s_rv.dir_z = hz; s_rv.have_dir = 1;
        s_rv.sp_x = px; s_rv.sp_z = pz;
        if (s_rv.tour_m > 0.0) {
            /* [roam M] the first sign is negated: the game-frame lattice is the source one reflected
             * (u' = -mirror(u), v' = mirror(v)), so this is the same square on the ground, corners in the
             * same order, as before the reflection */
            static const int sg[4][2] = { { -1, 1 }, { -1, -1 }, { 1, -1 }, { 1, 1 } };
            const double h = 0.5 * s_rv.tour_m * RV_M;
            const double ux = rs->grid.c, uz = rs->grid.s, vx = -rs->grid.s, vz = rs->grid.c;
            int k;
            for (k = 0; k < 4; k++) {
                s_rv.wp_x[k] = px + h * (sg[k][0] * ux + sg[k][1] * vx);
                s_rv.wp_z[k] = pz + h * (sg[k][0] * uz + sg[k][1] * vz);
            }
            s_rv.wp = 0;
            s_rv.wp_t = tick;
            TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] tour: square %.0f m round the spawn (%.0f,%.0f), %d s, first corner (%.0f,%.0f)",
                      s_rv.tour_m, px, pz, s_rv.tour_secs, s_rv.wp_x[0], s_rv.wp_z[0]);
        }
    }
    look = 14.0 * RV_M + rv_speed_upt(a) * 20.0;

    if (s_rv.phase == 0) {
        double vt = s_rv.kmh;
        if (on_patch && !s_rv.on_path) {
            const uint32_t slot = ROAM_PRIM_SLOT(s_rv.ch.prim), idx = ROAM_PRIM_INDEX(s_rv.ch.prim);
            const RoamTile *t = slot < (uint32_t)rs->nslot ? rs->tile[slot] : NULL;
            if (t && idx < t->ntri && t->tri[idx].patch < t->nnode &&
                (uint64_t)t->node[t->tri[idx].patch].cluster_key != s_rv.cl_key) {
                /* a new junction: the arm we came in on is the one most opposite to the heading; the exit arm
                 * is picked from the turn script (or homes back to the spawn past 1.4 km), preferring a
                 * junction not seen yet, never the way we came. The car then follows a planned path in-end ->
                 * out-end (the corner matrix path) instead of aiming straight at the exit (islands). */
                const RoamNodeRec *nd = &t->node[t->tri[idx].patch];
                const double hd = hypot(px - s_rv.sp_x, pz - s_rv.sp_z);
                double wx, wz, bd = -1e9;
                int k, bk = -1, ik = 0, turn = rv_turn_for(s_rv.crossings);
                double idot = 2.0;
                for (k = 0; k < nd->narm; k++) {
                    const double b = (double)t->arm[nd->arm_first + k].bearing12 * (2.0 * RV_PI / 4096.0);
                    const double d = cos(b) * s_rv.dir_x + sin(b) * s_rv.dir_z;
                    if (d < idot) { idot = d; ik = k; }
                }
                if (s_rv.tour_m > 0.0) {
                    /* [F3] tour: head for the current corner; next corner within 150 m (or after 4 min) */
                    if (hypot(px - s_rv.wp_x[s_rv.wp], pz - s_rv.wp_z[s_rv.wp]) < 150.0 * RV_M ||
                        tick - s_rv.wp_t > 240L * 30L) {
                        s_rv.wp = (s_rv.wp + 1) & 3;
                        s_rv.wp_t = tick;
                        TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] tour: corner reached, next (%.0f,%.0f), %.0f m driven",
                                  s_rv.wp_x[s_rv.wp], s_rv.wp_z[s_rv.wp], s_rv.dist_m);
                    }
                    wx = s_rv.wp_x[s_rv.wp] - px; wz = s_rv.wp_z[s_rv.wp] - pz;
                    { const double m = hypot(wx, wz); if (m > 1.0) { wx /= m; wz /= m; } }
                    turn = 99;
                } else if (hd > 1400.0 * RV_M) {
                    wx = s_rv.sp_x - px; wz = s_rv.sp_z - pz;
                    { const double m = hypot(wx, wz); if (m > 1.0) { wx /= m; wz /= m; } }
                    turn = 99;
                } else if (turn == 0) {
                    wx = s_rv.dir_x; wz = s_rv.dir_z;
                } else {
                    /* [roam M] turn +1 was the engine's right = the REAL left before the geographic -> game
                     * reflection (td5_geo_frame.h); the sign keeps the tour on the same streets as before */
                    wx = s_rv.dir_z * turn; wz = -s_rv.dir_x * turn;
                }
                s_rv.cl_key = (uint64_t)nd->cluster_key;
                s_rv.cl_x = (double)t->org_x - rs->org_x + nd->x;
                s_rv.cl_z = (double)t->org_z - rs->org_z + nd->z;
                s_rv.crossings++;
                if (!rv_key_seen(s_rv.cl_key) && s_rv.distinct < RV_MAXKEYS) s_rv.keys[s_rv.distinct++] = s_rv.cl_key;
                for (k = 0; k < nd->narm; k++) {
                    const RoamArmRec *ar = &t->arm[nd->arm_first + k];
                    const double b = (double)ar->bearing12 * (2.0 * RV_PI / 4096.0);
                    double score = cos(b) * wx + sin(b) * wz;
                    const RoamTile *te = NULL;
                    const RoamEdgeRec *er;
                    if (k == ik && nd->narm > 1) continue;
                    if ((er = rv_edge(rs, ar->edge_key, &te)) != NULL) {
                        const int64_t far_key = (er->end_key[0] != nd->cluster_key) ? er->end_key[0] : er->end_key[1];
                        if (far_key != nd->cluster_key && rv_key_seen((uint64_t)far_key)) score -= 1.2;
                    }
                    if (score > bd) { bd = score; bk = k; }
                }
                if (bk < 0) bk = (ik + 1) % (nd->narm > 0 ? nd->narm : 1);
                {
                    const RoamArmRec *ao = &t->arm[nd->arm_first + bk], *ai = &t->arm[nd->arm_first + ik];
                    const double bo = (double)ao->bearing12 * (2.0 * RV_PI / 4096.0);
                    const double bi = (double)ai->bearing12 * (2.0 * RV_PI / 4096.0);
                    double inx = cos(bi), inz = sin(bi), outx = cos(bo), outz = sin(bo);
                    double eix = 0, eiz = 0, eox = 0, eoz = 0;
                    const int have_in = rv_edge_end_near(rs, ai->edge_key, s_rv.cl_x, s_rv.cl_z, &eix, &eiz, &inx, &inz);
                    const int have_out = rv_edge_end_near(rs, ao->edge_key, s_rv.cl_x, s_rv.cl_z, &eox, &eoz, &outx, &outz);
                    if (have_in && have_out) {
                        rv_plan_path(s_rv.pth_x, s_rv.pth_z, eix, eiz, inx, inz, eox, eoz, outx, outz);
                    } else {
                        /* no ribbon end rows: straight from the car to 25 m past the node along the exit arm */
                        int q;
                        const double gx = s_rv.cl_x + outx * 25.0 * RV_M, gz = s_rv.cl_z + outz * 25.0 * RV_M;
                        for (q = 0; q < RV_NP; q++) {
                            s_rv.pth_x[q] = px + (gx - px) * q / (RV_NP - 1);
                            s_rv.pth_z[q] = pz + (gz - pz) * q / (RV_NP - 1);
                        }
                    }
                    s_rv.pth_ox = outx; s_rv.pth_oz = outz;
                    s_rv.pth_defl = acos(fmax(-1.0, fmin(1.0, -(inx * outx + inz * outz)))) * 180.0 / RV_PI;
                    s_rv.on_path = 1;
                    s_rv.path_t0 = tick;
                    TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] junction #%d (distinct %d) %016llx (%d arms) at (%.0f,%.0f): turn %d, in-arm %d, out-arm %d, "
                              "deflection %.0f deg, heading (%.2f,%.2f), %.0f m driven",
                              s_rv.crossings, s_rv.distinct, (unsigned long long)nd->cluster_key, (int)nd->narm, s_rv.cl_x, s_rv.cl_z,
                              turn, ik, bk, s_rv.pth_defl, s_rv.dir_x, s_rv.dir_z, s_rv.dist_m);
                }
            }
        }
        if (s_rv.on_path) {
            double along, lat;
            rv_pursue(s_rv.pth_x, s_rv.pth_z, s_rv.pth_ox, s_rv.pth_oz, px, pz,
                      (s_rv.pth_defl > 30.0 ? 10.0 : 14.0) * RV_M, &tx, &tz, &along, &lat);
            if (s_rv.pth_defl > 30.0) vt = s_rv.kmh * 0.6 > 15.0 ? s_rv.kmh * 0.6 : 15.0;
            if ((along > 12.0 * RV_M && !on_patch) || tick - s_rv.path_t0 > 20 * 30) {
                s_rv.on_path = 0;
                s_rv.dir_x = s_rv.pth_ox; s_rv.dir_z = s_rv.pth_oz;
                s_rv.have_off = 0;
            }
        } else if (s_rv.ch_ok && !on_patch && rv_street_axis(rs, &s_rv.ch, px, pz, &cx, &cz, &ax, &az)) {
            if (ax * s_rv.dir_x + az * s_rv.dir_z < 0.0) { ax = -ax; az = -az; }
            if (ax * s_rv.dir_x + az * s_rv.dir_z < 0.82) {     /* > 35 deg off: a crossing street, keep going straight */
                ax = s_rv.dir_x; az = s_rv.dir_z;
                cx = px; cz = pz;
            }
            s_rv.dir_x = ax; s_rv.dir_z = az;
            tx = cx + s_rv.dir_x * look;                         /* the carriageway centre line */
            tz = cz + s_rv.dir_z * look;
        } else {                                                /* nothing under the chassis: hold the direction */
            tx = px + s_rv.dir_x * look;
            tz = pz + s_rv.dir_z * look;
        }
        bits = rv_steer_to(a, tx, tz, vt);
        if ((s_rv.tour_m > 0.0 ? tick - s_rv.t0 >= (long)s_rv.tour_secs * 30L : s_rv.distinct >= s_rv.want_junctions) &&
            !on_patch && !s_rv.on_path) {
            s_rv.phase = 1;
            TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] %d junctions (%d distinct), %.0f m: braking to rest",
                      s_rv.crossings, s_rv.distinct, s_rv.dist_m);
        } else if (tick - s_rv.t0 > (long)s_rv.max_secs * 30) {
            rv_finish("timeout before the junction count", 1);
            return 0;
        }
    } else if (s_rv.phase == 1) {
        /* brake down to 3 km/h, then coast: brake held at a standstill is REVERSE in TD5 */
        tx = px + s_rv.dir_x * look; tz = pz + s_rv.dir_z * look;
        bits = rv_steer_to(a, tx, tz, 0.0) & ~(uint32_t)(TD5_INPUT_THROTTLE | TD5_INPUT_BRAKE);
        if (kmh > 3.0) bits |= (uint32_t)TD5_INPUT_BRAKE;
        if (kmh < 0.3) { s_rv.phase = 2; s_rv.rest_t = tick; }
        else if (tick - s_rv.t0 > (long)(s_rv.max_secs + 30) * 30) { rv_finish("could not come to rest", 1); return 0; }
    } else if (s_rv.phase == 2) {
        bits = 0;
        if (tick - s_rv.rest_t >= 90) { s_rv.phase = 3; rv_finish("route done", 0); return 0; }
    }

    /* [F8] with ambient traffic: a car ahead in the way -> lift off and brake (a queue at a red light is not a wedge) */
    if (s_rv.traffic && s_rv.phase == 0 && s_rv.rev_t == 0 && (bits & TD5_INPUT_THROTTLE)) {
        const double v_ms = kmh / 3.6;
        int j, total = td5_game_get_total_actor_count(), blocked = 0;
        for (j = 1; j < total && !blocked; j++) {
            const TD5_Actor *b;
            double f, l, bx, bz;
            if (j < 6 && td5_game_get_slot_state(j) == 3) continue;
            if (td5_roam_traffic_parked(j) == 1) continue;
            b = td5_game_get_actor(j);
            if (!b) continue;
            bx = (double)FP_TRUNC(b->world_pos.x) - px; bz = (double)FP_TRUNC(b->world_pos.z) - pz;
            f = (bx * hx + bz * hz) / RV_M;
            l = (bx * hz - bz * hx) / RV_M;
            if (f > 0.5 && f < 7.0 + v_ms * 1.4 && fabs(l) < 2.2) blocked = 1;
        }
        if (blocked) {
            bits &= ~(uint32_t)TD5_INPUT_THROTTLE;
            if (kmh > 3.0) bits |= (uint32_t)TD5_INPUT_BRAKE;
            s_rv.traffic_waits++;
        }
    }

    /* A wedge: throttle held and < 3 km/h for 1.5 s -> reverse with the wheel turned away for 1.3 s, as a
     * driver would (counted as a retry, reported). STUCK is the spec metric: throttle held and < 5 km/h
     * for more than 3 s in a row, i.e. a wedge the retry did not free. */
    if (s_rv.rev_t > 0) {
        s_rv.rev_t--;
        bits = (uint32_t)TD5_INPUT_BRAKE | ((bits & TD5_INPUT_STEER_LEFT) ? (uint32_t)TD5_INPUT_STEER_RIGHT
                                                                           : (bits & TD5_INPUT_STEER_RIGHT) ? (uint32_t)TD5_INPUT_STEER_LEFT : 0u);
        s_rv.stuck_t = 0;
        s_rv.slow_rt = 0;
    } else if (s_rv.phase == 0) {
        if ((bits & TD5_INPUT_THROTTLE) && kmh < RV_STUCK_KMH) {
            if (++s_rv.stuck_t == RV_STUCK_TICKS + 1) {
                s_rv.stuck_events++;
                TD5_LOG_W(LOG_TAG, "[ROAMSELFTEST] STUCK #%ld at world (%.0f,%.0f) after %d junctions: %.1f km/h for > 3 s with throttle",
                          s_rv.stuck_events, px, pz, s_rv.crossings, kmh);
            }
        } else {
            s_rv.stuck_t = 0;
        }
        if ((bits & TD5_INPUT_THROTTLE) && kmh < 3.0) {
            if (++s_rv.slow_rt > 45) {
                s_rv.slow_rt = 0;
                s_rv.retries_total++;
                TD5_LOG_W(LOG_TAG, "[ROAMSELFTEST] wedge #%d at world (%.0f,%.0f) edge %016llx patch=%d after %d junctions: reversing",
                          s_rv.retries_total, px, pz, (unsigned long long)s_rv.ch.edge_key, (int)s_rv.ch.is_patch, s_rv.crossings);
                if ((s_rv.retries_total > 8 || s_rv.stuck_events > 2) && s_rv.traffic && s_rv.tour_m > 0.0) {
                    /* [F8] the traffic soak keeps its anchor moving: a wedge the retries did not free (the tour wedges
                     * at the same corners without traffic too) puts the car 30 m on toward the tour corner, counted */
                    const double wx = s_rv.wp_x[s_rv.wp & 3] - px, wz = s_rv.wp_z[s_rv.wp & 3] - pz;
                    const double wl = sqrt(wx * wx + wz * wz);
                    int32_t ox, oz;
                    double sx, sz;
                    td5_roam_world_origin(&ox, &oz);
                    if (wl > 1.0 && td5_roam_route_snap_xz(px + ox + wx / wl * 30.0 * RV_M, pz + oz + wz / wl * 30.0 * RV_M, 80.0,
                                                           &sx, &sz, NULL, NULL)) {
                        const int yaw = (int)floor(atan2(wx, wz) * (4096.0 / (2.0 * RV_PI)) + 0.5) & 0xFFF;
                        td5_roam_place_actor(0, a, (int32_t)floor(sx + 0.5), (int32_t)floor(sz + 0.5), yaw);
                        s_rv.soak_moves++;
                        TD5_LOG_W(LOG_TAG, "[ROAMSELFTEST] soak: wedged at (%.0f,%.0f), car put 30 m on (move #%d)", px, pz, s_rv.soak_moves);
                        s_rv.retries_total = 0; s_rv.stuck_events = 0; s_rv.rev_t = 0; s_rv.slow_rt = 0; s_rv.stuck_t = 0;
                        s_rv.have_last = 0; s_rv.on_path = 0;
                        return 0;
                    }
                }
                if (s_rv.retries_total > 8 || s_rv.stuck_events > 2) { rv_finish("wedged, retries exhausted", 1); return 0; }
                s_rv.rev_t = 40;
            }
        } else {
            s_rv.slow_rt = 0;
        }
    }
    if (tick - s_rv.log_t >= 150 && !s_rv.done) {
        s_rv.log_t = tick;
        TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] t=%ld pos=(%d,%d,%d) kmh=%.1f junctions=%d/%d edge=%016llx patch=%d inside=%d dist=%.0f m",
                  tick - s_rv.t0, (int)px, (int)FP_TRUNC(a->world_pos.y), (int)pz, kmh, s_rv.crossings, s_rv.distinct,
                  (unsigned long long)s_rv.ch.edge_key, (int)s_rv.ch.is_patch, (int)s_rv.ch.inside, s_rv.dist_m);
    }
    return bits;
}

/* ------------------------------------------------------------ corners -------- */

static int rv_kind_of(const RoamEdgeRec *ea, const RoamEdgeRec *eb, const char *na, const char *nb, double defl)
{
    const int av_a = rv_is(na, "Avenida"), av_b = rv_is(nb, "Avenida");
    const int dg_a = rv_is(na, "Diagonal"), dg_b = rv_is(nb, "Diagonal");
    if ((dg_a && av_b) || (av_a && dg_b)) return (defl >= 45.0 && defl <= 135.0) ? 3 : -1;
    if (dg_a || dg_b) return -1;
    if (av_a && av_b) return -1;
    if (av_a != av_b) return fabs(defl - 90.0) <= 15.0 ? 2 : -1;
    if ((ea->klass == TD5_GEO_RC_SERVICE || ea->lanes == 1) && (eb->klass == TD5_GEO_RC_SERVICE || eb->lanes == 1))
        return fabs(defl - 90.0) <= 20.0 ? 0 : -1;
    if (ea->klass >= TD5_GEO_RC_LIVING && ea->klass <= TD5_GEO_RC_TERTIARY &&
        eb->klass >= TD5_GEO_RC_LIVING && eb->klass <= TD5_GEO_RC_TERTIARY)
        return fabs(defl - 90.0) <= 15.0 ? 1 : -1;
    return -1;
}

/* One pass over every junction. gapk[kind] = minimum spacing between two picks of that kind, units. */
static void rv_pick_pass(const TD5_RoamResident *rs, const double *gapk)
{
    int ti;
    uint32_t ni;
    int ord[ROAM_RES_MAX], nord = 0, oi;
    s_rv.nc = 0;
    memset(s_rv.kind_n, 0, sizeof s_rv.kind_n);
    memset(s_rv.cand_n, 0, sizeof s_rv.cand_n);
    /* [F3] tiles in (tx, tz) order, not resident-slot order: a streamed set fills its slots
     * in arrival order, and the spacing filter below keeps the FIRST pick of a kind */
    for (ti = 0; ti < rs->nslot; ti++) {
        const RoamTile *t = rs->tile[ti];
        int j;
        if (!t) continue;
        for (j = nord; j > 0; j--) {
            const RoamTile *u = rs->tile[ord[j - 1]];
            if (u->tx < t->tx || (u->tx == t->tx && u->tz < t->tz)) break;
            ord[j] = ord[j - 1];
        }
        ord[j] = ti;
        nord++;
    }
    for (oi = 0; oi < nord; oi++) {
        const RoamTile *t = rs->tile[ord[oi]];
        for (ni = 0; ni < t->nnode; ni++) {
            const RoamNodeRec *nd = &t->node[ni];
            int ai, ao, kind;
            double best[4] = { 1e9, 1e9, 1e9, 1e9 };
            RvCorner cand[4];
            memset(cand, 0, sizeof cand);
            if (nd->narm < 3 || nd->narm > s_rv.max_arms || (nd->flags & 0x04)) continue;
            for (ai = 0; ai < nd->narm; ai++) {
                for (ao = 0; ao < nd->narm; ao++) {
                    const RoamArmRec *A = &t->arm[nd->arm_first + ai], *B = &t->arm[nd->arm_first + ao];
                    const RoamTile *ta = NULL, *tb = NULL;
                    const RoamEdgeRec *ea, *eb;
                    double ia, oa, inx, inz, outx, outz, defl, cx, cz, eix, eiz, eox, eoz;
                    if (ai == ao) continue;
                    if (!(ea = rv_edge(rs, A->edge_key, &ta)) || !(eb = rv_edge(rs, B->edge_key, &tb))) continue;
                    if (ea->layer != 0 || eb->layer != 0) continue;
                    ia = (double)A->bearing12 * (2.0 * RV_PI / 4096.0);
                    oa = (double)B->bearing12 * (2.0 * RV_PI / 4096.0);
                    inx = cos(ia); inz = sin(ia); outx = cos(oa); outz = sin(oa);
                    defl = acos(fmax(-1.0, fmin(1.0, -(inx * outx + inz * outz)))) * 180.0 / RV_PI;
                    kind = rv_kind_of(ea, eb, rv_name(ta, ea->name_id), rv_name(tb, eb->name_id), defl);
                    if (kind < 0 || fabs(defl - 90.0) >= best[kind]) continue;
                    cx = (double)t->org_x + nd->x - rs->org_x;
                    cz = (double)t->org_z + nd->z - rs->org_z;
                    /* the path runs on the ribbons' own centre lines: in-ribbon end row and axis, out-ribbon likewise */
                    if (!rv_edge_end_near(rs, A->edge_key, cx, cz, &eix, &eiz, &inx, &inz)) continue;
                    if (!rv_edge_end_near(rs, B->edge_key, cx, cz, &eox, &eoz, &outx, &outz)) continue;
                    defl = acos(fmax(-1.0, fmin(1.0, -(inx * outx + inz * outz)))) * 180.0 / RV_PI;
                    if (rv_kind_of(ea, eb, rv_name(ta, ea->name_id), rv_name(tb, eb->name_id), defl) != kind) continue;
                    if (fabs(defl - 90.0) >= best[kind]) continue;
                    /* the approach and the exit must be on the arms' own ways */
                    if (rv_way_at(rs, eix + inx * 45.0 * RV_M, eiz + inz * 45.0 * RV_M) != (A->edge_key >> 16)) continue;
                    if (rv_way_at(rs, eox + outx * 40.0 * RV_M, eoz + outz * 40.0 * RV_M) != (B->edge_key >> 16)) continue;
                    {
                        /* control point: E_in + t*(-in) meets E_out + u*(-out) */
                        const double ax_ = -inx, az_ = -inz, bx_ = -outx, bz_ = -outz;
                        const double det = ax_ * (-bz_) - az_ * (-bx_);
                        double qx = 0.5 * (eix + eox), qz = 0.5 * (eiz + eoz);
                        int q;
                        if (fabs(det) > 1e-6) {
                            const double tt = ((eox - eix) * (-bz_) - (eoz - eiz) * (-bx_)) / det;
                            if (tt > 0.0 && tt < 60.0 * RV_M) { qx = eix + ax_ * tt; qz = eiz + az_ * tt; }
                        }
                        cand[kind].px[0] = eix + inx * 45.0 * RV_M; cand[kind].pz[0] = eiz + inz * 45.0 * RV_M;
                        for (q = 0; q <= RV_NP - 3; q++) {
                            const double u = (double)q / (double)(RV_NP - 3);
                            const double w0 = (1 - u) * (1 - u), w1 = 2 * u * (1 - u), w2 = u * u;
                            cand[kind].px[1 + q] = w0 * eix + w1 * qx + w2 * eox;
                            cand[kind].pz[1 + q] = w0 * eiz + w1 * qz + w2 * eoz;
                        }
                        cand[kind].px[RV_NP - 1] = eox + outx * 40.0 * RV_M;
                        cand[kind].pz[RV_NP - 1] = eoz + outz * 40.0 * RV_M;
                        cand[kind].np = RV_NP;
                        cand[kind].route = 0;
                    }
                    best[kind] = fabs(defl - 90.0);
                    cand[kind].kind = kind; cand[kind].cx = cx; cand[kind].cz = cz;
                    cand[kind].inx = inx; cand[kind].inz = inz; cand[kind].outx = outx; cand[kind].outz = outz;
                    cand[kind].defl = defl; cand[kind].cl = (uint64_t)nd->cluster_key;
                    cand[kind].ein = A->edge_key; cand[kind].eout = B->edge_key;
                }
            }
            for (kind = 0; kind < 4; kind++) {
                int j, far_ok = 1;
                if (best[kind] < 1e8) s_rv.cand_n[kind]++;
                if (best[kind] > 1e8 || s_rv.kind_n[kind] >= s_rv.per_kind || s_rv.nc >= RV_MAXC) continue;
                for (j = 0; j < s_rv.nc; j++)
                    if (s_rv.c[j].kind == kind && hypot(s_rv.c[j].cx - cand[kind].cx, s_rv.c[j].cz - cand[kind].cz) < gapk[kind]) far_ok = 0;
                if (!far_ok) continue;
                s_rv.c[s_rv.nc++] = cand[kind];
                s_rv.kind_n[kind]++;
            }
        }
    }
}

/* Pick the matrix. A kind with no more candidate junctions than the per-kind target is taken whole (no spacing
 * filter: the casco simply does not hold more); a kind with plenty is thinned to >= gap_m between picks so the
 * matrix spreads over the city. */
static void rv_pick_corners(const TD5_RoamResident *rs)
{
    double gapk[4] = { 0.0, 0.0, 0.0, 0.0 };
    int k;
    const uint32_t t_start = td5_plat_time_ms();
    rv_pick_pass(rs, gapk);                       /* counts the candidates per kind (cand_n) */
    for (k = 0; k < 4; k++) gapk[k] = (s_rv.cand_n[k] > s_rv.per_kind) ? (double)s_rv.gap_m * RV_M : 0.0;
    rv_pick_pass(rs, gapk);
    TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] corners picked %d (want %d per kind, >= %d m apart) in %u ms: one-lane/service %d, "
              "calle x calle %d, avenida x calle %d, diagonal x avenida %d; junctions with a candidate before the "
              "spacing / cap filter: %d / %d / %d / %d",
              s_rv.nc, s_rv.per_kind, s_rv.gap_m, td5_plat_time_ms() - t_start,
              s_rv.kind_n[0], s_rv.kind_n[1], s_rv.kind_n[2], s_rv.kind_n[3],
              s_rv.cand_n[0], s_rv.cand_n[1], s_rv.cand_n[2], s_rv.cand_n[3]);
}

static void rv_corner_start(TD5_Actor *a, long tick)
{
    RvCorner *c = &s_rv.c[s_rv.ci];
    const double v = (s_rv.si ? 80.0 : 40.0) * RV_UPT_KMH;   /* the braking pass arrives at 80 km/h too */
    const double tx = -c->inx, tz = -c->inz;
    TD5_RoamPhysStats st;
    if (c->route == 0) {
        /* [F2-E] a Bezier that cuts through a walled island or a median (00064c21000864e3: 17 m of
         * it off the road) is not a fair path for any driver: route it on the paved mask, once per
         * corner. Knob TD5RE_ROAM_SELFTEST_CORNER_ROUTE (1). */
        int routed = 0;
        c->off_m = 0.0;
        if (td5_env_int("TD5RE_ROAM_SELFTEST_CORNER_ROUTE", 1, 0, 1))
            c->off_m = td5_roam_dev_route_path(c->px, c->pz, &c->np, RV_PMAX, c->cx, c->cz, c->outx, c->outz,
                                               40.0 * RV_M, &routed);
        c->route = routed == 1 ? 2 : routed == 2 ? 3 : 1;
        if (c->off_m > 0.0)
            TD5_LOG_I(LOG_TAG, "[ROAMCORNER] #%d cluster=%016llx: Bezier %.1f m off the road -> %s (%d points)", s_rv.ci,
                      (unsigned long long)c->cl, c->off_m, routed == 1 ? "ROUTED on the paved mask" : "NO ROUTE, kept",
                      c->np);
    }
    a->world_pos.x = (int32_t)(c->px[0] * 256.0);
    a->world_pos.z = (int32_t)(c->pz[0] * 256.0);
    a->euler_accum.yaw = FP_SCALE((int32_t)(atan2(tx, tz) * 4096.0 / (2.0 * RV_PI)) & 0xFFF);
    td5_physics_reset_actor_state(a);
    a->linear_velocity_x = (int32_t)(tx * v * 256.0);
    a->linear_velocity_z = (int32_t)(tz * v * 256.0);
    s_rv.ch_hint = ROAM_PRIM_NONE;
    s_rv.ct = tick; s_rv.cstuck_t = 0; s_rv.vmin = 1e9; s_rv.vmax = 0.0;
    s_rv.slow_t = 0; s_rv.rev_ct = 0; s_rv.stalls = 0; s_rv.retries = 0;
    td5_roam_physics_stats(&st);
    s_rv.hits0 = st.wall_hits; s_rv.pinch0 = st.pinches;
    s_rv.resc0 = st.rescues + st.rescues_y + st.teleports;
    s_rv.phase = 1;
    {
        /* [F2-E] the planned path, so a run can be plotted over the tiles */
        char buf[400];
        size_t l = 0;
        int k;
        for (k = 0; k < c->np && l < sizeof buf - 24; k++)
            l += (size_t)snprintf(buf + l, sizeof buf - l, " %.0f,%.0f", c->px[k], c->pz[k]);
        TD5_LOG_I(LOG_TAG, "[ROAMCORNER] start #%d cluster=%016llx v=%d km/h (%s) path%s", s_rv.ci,
                  (unsigned long long)c->cl, s_rv.si ? 80 : 40, k_rv_pass[s_rv.si], buf);
    }
}

static void rv_corner_end(long tick, int ok, int stuck, const char *why)
{
    const RvCorner *c = &s_rv.c[s_rv.ci];
    TD5_RoamPhysStats st;
    long hits, pin, resc;
    const int sp = s_rv.si;
    td5_roam_physics_stats(&st);
    hits = st.wall_hits - s_rv.hits0;
    pin = st.pinches - s_rv.pinch0;
    resc = st.rescues + st.rescues_y + st.teleports - s_rv.resc0;
    s_rv.runs++;
    s_rv.k_runs[c->kind][sp]++;
    s_rv.hits += hits; s_rv.k_hits[c->kind][sp] += hits;
    s_rv.ok += ok; s_rv.stuck += stuck; s_rv.pinch += pin; s_rv.resc += resc;
    s_rv.k_ok[c->kind][sp] += ok; s_rv.k_stuck[c->kind][sp] += stuck;
    s_rv.k_pinch[c->kind][sp] += pin; s_rv.k_resc[c->kind][sp] += resc;
    if (sp == 2) { s_rv.b_runs++; s_rv.b_ok += ok; s_rv.b_stuck += stuck; }
    TD5_LOG_I(LOG_TAG, "[ROAMCORNER] #%d %s cluster=%016llx defl=%.0f v=%d km/h %s: %s (%s) ticks=%ld wall_hits=%ld pinches=%ld "
              "stuck=%d stalls=%ld retries=%d rescues=%ld speed min/max near C %.0f/%.0f km/h",
              s_rv.ci, k_rv_kind[c->kind], (unsigned long long)c->cl, c->defl, sp ? 80 : 40, k_rv_pass[sp],
              ok ? "EXIT" : "NO EXIT", why, tick - s_rv.ct, hits, pin, stuck, s_rv.stalls, s_rv.retries, resc, s_rv.vmin > 1e8 ? 0.0 : s_rv.vmin, s_rv.vmax);
    rv_report_line("%d\t%s\t%016llx\t%.0f\t%d\t%s\t%s\t%s\t%ld\t%ld\t%ld\t%d\t%ld\t%d\t%ld\t%.0f\t%.0f\n",
                   s_rv.ci, k_rv_kind[c->kind], (unsigned long long)c->cl, c->defl, sp ? 80 : 40, sp == 2 ? "brake" : "held",
                   ok ? "EXIT" : "NO_EXIT", why, tick - s_rv.ct, hits, pin, stuck, s_rv.stalls,
                   s_rv.retries, resc, s_rv.vmin > 1e8 ? 0.0 : s_rv.vmin, s_rv.vmax);
    s_rv.phase = 0;
    /* every pass at one corner back to back: the later runs need no new tile meshes around the car */
    if (++s_rv.si >= s_rv.npass) { s_rv.si = 0; s_rv.ci++; }
}

/* [F2-E] The braking driver's speed (km/h): the highest from which every path vertex ahead of arc length
 * s_car can still be taken at sqrt(RV_BRAKE_ALAT * R), R from the turn at that vertex over its two half
 * segments, braking at RV_BRAKE_DECEL. */
static double rv_bend_kmh(const RvCorner *c, double s_car)
{
    double best = 1e9, cum = 0.0;
    int k;
    for (k = 1; k + 1 < c->np; k++) {
        const double ax = c->px[k] - c->px[k - 1], az = c->pz[k] - c->pz[k - 1];
        const double bx = c->px[k + 1] - c->px[k], bz = c->pz[k + 1] - c->pz[k];
        const double la = sqrt(ax * ax + az * az), lb = sqrt(bx * bx + bz * bz);
        double th, r_m, v_k, ahead_m, v_ok;
        cum += la;
        if (la < 1.0 || lb < 1.0 || cum < s_car) continue;
        th = atan2(fabs(ax * bz - az * bx), ax * bx + az * bz);
        if (th < 0.05) continue;
        r_m = 0.5 * (la + lb) / RV_M / th;
        v_k = sqrt(RV_BRAKE_ALAT * r_m);
        ahead_m = (cum - s_car) / RV_M;
        v_ok = sqrt(v_k * v_k + 2.0 * RV_BRAKE_DECEL * ahead_m) * 3.6;
        if (v_ok < best) best = v_ok;
    }
    return best;
}

static uint32_t rv_corners(TD5_Actor *a, const TD5_RoamResident *rs, long tick)
{
    const RvCorner *c;
    double px, pz, kmh, look, rx, rz, along_out, lat_out, tx, tz, s_car = 0.0, vset;
    uint32_t bits;
    if (s_rv.phase == 0) {
        if (s_rv.ci >= s_rv.nc) {
            int k, sp;
            TD5_LOG_I(LOG_TAG, "[ROAMCORNER] SUMMARY runs=%ld exit=%ld stuck=%ld pinches=%ld wall_hits=%ld rescues=%ld",
                      s_rv.runs, s_rv.ok, s_rv.stuck, s_rv.pinch, s_rv.hits, s_rv.resc);
            rv_report_line("\nSUMMARY runs=%ld exit=%ld stuck=%ld pinches=%ld wall_hits=%ld rescues=%ld\n",
                           s_rv.runs, s_rv.ok, s_rv.stuck, s_rv.pinch, s_rv.hits, s_rv.resc);
            for (k = 0; k < 4; k++)
                for (sp = 0; sp < s_rv.npass; sp++)
                    rv_report_line("kind %-20s %-8s: runs %ld exit %ld stuck %ld pinches %ld wall_hits %ld rescues %ld\n",
                                   k_rv_kind[k], k_rv_pass[sp], s_rv.k_runs[k][sp], s_rv.k_ok[k][sp], s_rv.k_stuck[k][sp],
                                   s_rv.k_pinch[k][sp], s_rv.k_hits[k][sp], s_rv.k_resc[k][sp]);
            if (s_rv.rep) { fclose(s_rv.rep); s_rv.rep = NULL; }
            rv_finish("corner matrix done", 0);
            return 0;
        }
        rv_corner_start(a, tick);
        return (uint32_t)TD5_INPUT_THROTTLE;
    }
    c = &s_rv.c[s_rv.ci];
    px = (double)FP_TRUNC(a->world_pos.x);
    pz = (double)FP_TRUNC(a->world_pos.z);
    kmh = rv_speed_upt(a) / RV_UPT_KMH;
    rx = px - c->cx; rz = pz - c->cz;
    if (hypot(rx, rz) < 20.0 * RV_M) {
        if (kmh < s_rv.vmin) s_rv.vmin = kmh;
        if (kmh > s_rv.vmax) s_rv.vmax = kmh;
    }
    /* pure pursuit on the polyline: arc length of the car's nearest point, aim `look` further */
    look = (s_rv.si ? 16.0 : 10.0) * RV_M;
    if (s_rv.si == 2) {
        /* [F2-E] the braking driver looks as far ahead as its speed needs: 10 m at 40 km/h (the held-40
         * pass), 16 m from 64 km/h; a 16 m look at bend speed cut the inside of the turn into the kerb */
        double lm = 0.25 * kmh;
        if (lm < 6.0) lm = 6.0;
        if (lm > 16.0) lm = 16.0;
        look = lm * RV_M;
    }
    {
        double best = 1e30, cum = 0.0, total = 0.0, want;
        int k;
        for (k = 0; k < c->np - 1; k++) {
            const double sx = c->px[k + 1] - c->px[k], sz = c->pz[k + 1] - c->pz[k];
            const double len = sqrt(sx * sx + sz * sz);
            double tt = len > 0.0 ? ((px - c->px[k]) * sx + (pz - c->pz[k]) * sz) / (len * len) : 0.0, qx, qz, dd;
            if (tt < 0.0) tt = 0.0;
            if (tt > 1.0) tt = 1.0;
            qx = c->px[k] + sx * tt; qz = c->pz[k] + sz * tt;
            dd = (px - qx) * (px - qx) + (pz - qz) * (pz - qz);
            if (dd < best) { best = dd; s_car = cum + tt * len; }
            cum += len;
        }
        total = cum;
        want = s_car + look;
        if (want > total) want = total + look;           /* past the end: keep going straight out */
        cum = 0.0;
        tx = c->px[c->np - 1] + c->outx * (want - total);
        tz = c->pz[c->np - 1] + c->outz * (want - total);
        for (k = 0; k < c->np - 1; k++) {
            const double sx = c->px[k + 1] - c->px[k], sz = c->pz[k + 1] - c->pz[k];
            const double len = sqrt(sx * sx + sz * sz);
            if (want <= cum + len && len > 0.0) {
                tx = c->px[k] + sx * (want - cum) / len;
                tz = c->pz[k] + sz * (want - cum) / len;
                break;
            }
            cum += len;
        }
        along_out = s_car - (total - 40.0 * RV_M);       /* > 0 once on the exit ribbon past its end row */
        lat_out = sqrt(best);
    }
    (void)rs;
    vset = s_rv.si ? 80.0 : 40.0;
    if (s_rv.si == 2) {
        /* [F2-E] the braking driver: arrives at 80 km/h, slows for the bend ahead (a held-speed
         * 135 degree city corner is not a fair stuck test) */
        const double vb = rv_bend_kmh(c, s_car);
        if (vb < vset) vset = vb < RV_BRAKE_MIN_KMH ? RV_BRAKE_MIN_KMH : vb;
    }
    bits = rv_steer_to(a, tx, tz, vset);
    if (s_rv.rev_ct > 0) {
        /* reverse-and-retry, as a driver would: back off with the wheel turned away */
        const uint32_t steer = bits & (uint32_t)(TD5_INPUT_STEER_LEFT | TD5_INPUT_STEER_RIGHT);
        s_rv.rev_ct--;
        bits = (uint32_t)TD5_INPUT_BRAKE;
        if (steer & TD5_INPUT_STEER_LEFT) bits |= (uint32_t)TD5_INPUT_STEER_RIGHT;
        else if (steer & TD5_INPUT_STEER_RIGHT) bits |= (uint32_t)TD5_INPUT_STEER_LEFT;
        s_rv.cstuck_t = 0;
        s_rv.slow_t = 0;
    } else {
        /* the stall metric: < 1 km/h for > 2 s with the throttle on (counted once per stall) */
        if ((bits & TD5_INPUT_THROTTLE) && kmh < 1.0) {
            if (++s_rv.cstuck_t == 61) s_rv.stalls++;
        } else s_rv.cstuck_t = 0;
        if ((bits & TD5_INPUT_THROTTLE) && kmh < 3.0) s_rv.slow_t++;
        else s_rv.slow_t = 0;
        if (s_rv.slow_t > 45 && s_rv.retries < 2) { s_rv.retries++; s_rv.rev_ct = 40; s_rv.slow_t = 0; }
    }
    if (along_out > 30.0 * RV_M && lat_out < 6.0 * RV_M) rv_corner_end(tick, 1, 0, "exit reached");
    else if (s_rv.retries >= 2 && s_rv.slow_t > 60) rv_corner_end(tick, 0, 1, "stuck after 2 retries");
    else if (tick - s_rv.ct > 20 * 30) rv_corner_end(tick, 0, 0, "timeout");
    return bits;
}

/* ------------------------------------------------------------ public ---------- */

/* [F9b] World-origin rebase: the route tour's spawn, square, junction, target, path and last position
 * are world units (the corner matrix inhibits rebasing instead: td5_roam_physics_rebase_inhibited). */
static void rv_world_shift(int32_t sx, int32_t sz)
{
    int k;
    if (s_rv.mode != TD5_RDRV_ROUTE) return;
    s_rv.lx += sx; s_rv.lz += sz;
    s_rv.sp_x += sx; s_rv.sp_z += sz;
    for (k = 0; k < 4; k++) { s_rv.wp_x[k] += sx; s_rv.wp_z[k] += sz; }
    s_rv.cl_x += sx; s_rv.cl_z += sz;
    if (s_rv.tgt_x != 0.0 || s_rv.tgt_z != 0.0) { s_rv.tgt_x += sx; s_rv.tgt_z += sz; }
    for (k = 0; k < RV_NP; k++) { s_rv.pth_x[k] += sx; s_rv.pth_z[k] += sz; }
}

void td5_roam_drive_arm(int mode)
{
    td5_roam_rebase_listen(rv_world_shift);
    const int ec = s_rv.env_checked, ed = s_rv.env_driven;
    memset(&s_rv, 0, sizeof s_rv);
    s_rv.env_checked = ec; s_rv.env_driven = ed;
    s_rv.mode = mode;
    s_rv.kmh = (double)td5_env_int("TD5RE_ROAM_SELFTEST_KMH", 40, 10, 120);
    s_rv.want_junctions = td5_env_int("TD5RE_ROAM_SELFTEST_JUNCTIONS", 24, 1, 100);
    s_rv.max_secs = td5_env_int("TD5RE_ROAM_SELFTEST_MAX_SECS", 420, 30, 3000);
    s_rv.per_kind = td5_env_int("TD5RE_ROAM_SELFTEST_CORNERS_PER_KIND", 20, 1, 80);
    s_rv.gap_m = td5_env_int("TD5RE_ROAM_SELFTEST_CORNER_GAP_M", 120, 0, 1000);
    s_rv.max_arms = td5_env_int("TD5RE_ROAM_SELFTEST_CORNER_MAX_ARMS", 4, 3, 8);
    s_rv.npass = td5_env_int("TD5RE_ROAM_SELFTEST_CORNER_BRAKE", 1, 0, 1) ? 3 : 2;
    s_rv.tour_m = (double)td5_env_int("TD5RE_ROAM_SELFTEST_TOUR_M", 0, 0, 20000);
    s_rv.tour_secs = td5_env_int("TD5RE_ROAM_SELFTEST_TOUR_SECS", 600, 30, 7200);
    if (s_rv.tour_m > 0.0 && s_rv.max_secs < s_rv.tour_secs + 120) s_rv.max_secs = s_rv.tour_secs + 120;
    s_rv.ch_hint = ROAM_PRIM_NONE;
}

/* [F8] the TOUR mode without the env knobs (the roam-traffic soak rows) */
void td5_roam_drive_set_traffic(int on) { s_rv.traffic = on ? 1 : 0; }
int  td5_roam_drive_soak_moves(void) { return s_rv.soak_moves; }

void td5_roam_drive_set_tour(double side_m, int secs)
{
    s_rv.tour_m = side_m > 0.0 ? side_m : 0.0;
    s_rv.tour_secs = secs > 30 ? secs : 30;
    if (s_rv.tour_m > 0.0 && s_rv.max_secs < s_rv.tour_secs + 120) s_rv.max_secs = s_rv.tour_secs + 120;
}

void td5_roam_drive_disarm(void)
{
    if (s_rv.rep) fclose(s_rv.rep);
    memset(&s_rv, 0, sizeof s_rv);
}

int td5_roam_drive_mode(void) { return s_rv.mode; }
int td5_roam_drive_done(void) { return s_rv.mode != TD5_RDRV_OFF && s_rv.done; }

uint32_t td5_roam_drive_bits(TD5_Actor *a)
{
    const TD5_RoamResident *rs;
    long tick;
    uint32_t bits;
    if (s_rv.mode == TD5_RDRV_OFF && !s_rv.env_checked) {
        /* standalone dev run: TD5RE_ROAM_SELFTEST_DRIVE=route|corners with --Roam=1 --RoamSpawn=... drives the
         * same script without the director, logs the verdict and quits */
        const char *m = getenv("TD5RE_ROAM_SELFTEST_DRIVE");
        s_rv.env_checked = 1;
        if (m && (!strcmp(m, "route") || !strcmp(m, "corners"))) {
            td5_roam_drive_arm(!strcmp(m, "route") ? TD5_RDRV_ROUTE : TD5_RDRV_CORNERS);
            s_rv.env_checked = 1;
            s_rv.env_driven = 1;
        }
    }
    if (s_rv.env_driven && s_rv.done) {
        char note[128];
        if (!s_rv.env_reported) {
            s_rv.env_reported = 1;
            td5_roam_drive_verdict(note, sizeof note);
            TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] standalone verdict: %s", note);
            g_td5.quit_requested = 1;
        }
        return 0;
    }
    if (s_rv.mode == TD5_RDRV_OFF || s_rv.done || !a || !td5_roam_active()) return 0;
    rs = td5_roam_resident();
    if (!rs) return 0;
    tick = g_td5.simulation_tick_counter;
    if (!s_rv.armed_tick_set) {
        s_rv.armed_tick_set = 1;
        s_rv.t0 = tick;
        s_rv.log_t = tick;
        TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] %s drive armed (kmh=%.0f junctions=%d per_kind=%d gap=%d m)",
                  s_rv.mode == TD5_RDRV_ROUTE ? "route" : "corners", s_rv.kmh, s_rv.want_junctions, s_rv.per_kind, s_rv.gap_m);
        if (s_rv.mode == TD5_RDRV_CORNERS) {
            char path[512];
            const char *dir = td5_plat_log_dir();
            td5_roam_stream_pin_all();       /* [F3] the matrix picks from (and teleports across) the whole casco */
            rs = td5_roam_resident();
            rv_pick_corners(rs);
            snprintf(path, sizeof path, "%sroam_corner_test.txt", dir ? dir : "");
            s_rv.rep = fopen(path, "w");
            rv_report_line("idx\tkind\tcluster\tdefl\tkmh\tdriver\tresult\twhy\tticks\twall_hits\tpinches\tstuck\tstalls\tretries\trescues\tvmin\tvmax\n");
        }
    }
    if (tick == s_rv.tick_last) return 0;      /* one decision per sim tick */
    s_rv.tick_last = tick;
    if (tick - s_rv.t0 < 15) return 0;         /* let the spawn settle */
    if (s_rv.mode == TD5_RDRV_ROUTE || s_rv.phase != 0) rv_measure(a, rs);
    bits = (s_rv.mode == TD5_RDRV_ROUTE) ? rv_route(a, rs, tick) : rv_corners(a, rs, tick);
    return bits;
}

/* ------------------------------------------------------------ verdict -------- */

int td5_roam_drive_verdict(char *note, size_t n)
{
    TD5_RoamPhysStats st;
    TD5_RoamStreamStats sst;
    int status = 0;
    td5_roam_physics_stats(&st);
    td5_roam_stream_stats(&sst);     /* [F3] zeros when the session is not streamed */
    if (s_rv.mode == TD5_RDRV_ROUTE) {
        const double rest_max = st.rest_n ? st.rest_max / RV_UNIT_CM : 0.0;
        const double gap_min = st.gap_n ? st.gap_min / RV_UNIT_CM : 0.0;
        const double above_min = s_rv.above_seen ? s_rv.above_min / RV_UNIT_CM : 0.0;
        const long resc = st.rescues + st.rescues_y;
        char bad[96] = "";
        size_t bl = 0;
#define RV_BAD(...) do { if (bl < sizeof bad - 1) bl += (size_t)snprintf(bad + bl, sizeof bad - bl, __VA_ARGS__); } while (0)
        if (!s_rv.done || s_rv.aborted) RV_BAD("%s; ", s_rv.why[0] ? s_rv.why : "drive never finished");
        if (s_rv.tour_m <= 0.0 && s_rv.distinct < s_rv.want_junctions)
            RV_BAD("%d distinct junctions < %d; ", s_rv.distinct, s_rv.want_junctions);
        if (sst.misses) RV_BAD("stream miss %ld; ", sst.misses);
        if (s_rv.below_n) RV_BAD("below surface %ld; ", s_rv.below_n);
        if (gap_min < -RV_WHEEL_BELOW_MAX_CM) RV_BAD("wheel %.1f cm under; ", -gap_min);
        if (st.rest_n == 0) RV_BAD("no rest sample; ");
        else if (rest_max > RV_ABOVE_REST_MAX_CM && !s_rv.traffic) RV_BAD("rest %.1f cm above; ", rest_max);
        /* [F8] with ambient traffic the scripted car (which ignores traffic) is shoved by it: being stuck behind /
         * against a car, pushed off the surface (and rescued), or resting on a car's bumper are reported, not judged */
        if (s_rv.stuck_events && !s_rv.traffic) RV_BAD("stuck %ld; ", s_rv.stuck_events);
        if (s_rv.off_n && !s_rv.traffic) RV_BAD("off surface %ld ticks; ", s_rv.off_n);
        if (resc && !s_rv.traffic) RV_BAD("rescues %ld; ", resc);
        if (st.pinches) RV_BAD("pinches %ld; ", st.pinches);
        if (st.teleports) RV_BAD("teleports %ld; ", st.teleports);
#undef RV_BAD
        status = bad[0] ? 2 : 0;
        if (status)
            snprintf(note, n, "roam FAIL: %.100s", bad);
        else if (s_rv.traffic)
            snprintf(note, n, "drive %d jn %.1f km, %d moves, waits %ld, rest %+.1f cm stuck %ld off %ld resc %ld",
                     s_rv.crossings, s_rv.dist_m / 1000.0, s_rv.soak_moves, s_rv.traffic_waits, rest_max, s_rv.stuck_events,
                     s_rv.off_n, resc);
        else
            snprintf(note, n, "roam %d jn (%d distinct) %.1f km, snap %.0f m, rest %+.1f cm, below 0 stuck 0 resc 0 pinch 0 wedge %d",
                     s_rv.crossings, s_rv.distinct, s_rv.dist_m / 1000.0, td5_roam_spawn_snap_m(), rest_max, s_rv.retries_total);
        TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] ROUTE VERDICT %s: junctions=%d distinct=%d dist=%.0f m vmax=%.0f km/h samples=%ld "
                  "chassis_above_min=%.1f cm below_n=%ld off_n=%ld wheel_gap_min=%.2f cm rest_n=%ld rest_gap_max=%.2f cm "
                  "stuck=%ld wedge_retries=%d rescues=%ld pinches=%ld teleports=%ld wall_hits=%ld spawn_snap=%.1f m "
                  "| stream loaded=%ld built=%ld miss=%ld hold=%ld road %d..%d scenery %d..%d %s",
                  status ? "FAIL" : "PASS", s_rv.crossings, s_rv.distinct, s_rv.dist_m, s_rv.kmh_max, s_rv.samples, above_min,
                  s_rv.below_n, s_rv.off_n, gap_min, st.rest_n, rest_max, s_rv.stuck_events, s_rv.retries_total, resc, st.pinches, st.teleports,
                  st.wall_hits, td5_roam_spawn_snap_m(), sst.loaded, sst.built, sst.misses, sst.holds, sst.road_min,
                  sst.road_max, sst.scen_min, sst.scen_max, bad);
    } else if (s_rv.mode == TD5_RDRV_CORNERS) {
        if (s_rv.npass == 3) {
            /* [F2-E] pinches, rescues and wall wedges are judged on EVERY run (held 40, held 80 and the
             * braking driver); exit and stuck only on the braking driver's runs (orchestrator, 2026-10-17).
             * The held runs' exits and stuck events are reported, not judged. */
            const long held = s_rv.runs - s_rv.b_runs, held_ok = s_rv.ok - s_rv.b_ok;
            if (!s_rv.done || s_rv.aborted || s_rv.b_runs == 0 || s_rv.b_ok < s_rv.b_runs || s_rv.b_stuck ||
                s_rv.pinch || s_rv.resc)
                status = 2;
            snprintf(note, n, "%s corners: %ld runs (%d/%d/%d/%d), brake exit %ld/%ld stuck %ld, held exit %ld/%ld "
                     "stuck %ld, pinch %ld, resc %ld",
                     status ? "FAIL" : "ok", s_rv.runs, s_rv.kind_n[0], s_rv.kind_n[1], s_rv.kind_n[2], s_rv.kind_n[3],
                     s_rv.b_ok, s_rv.b_runs, s_rv.b_stuck, held_ok, held, s_rv.stuck - s_rv.b_stuck, s_rv.pinch,
                     s_rv.resc);
        } else {
            const long fails = s_rv.runs - s_rv.ok;
            if (!s_rv.done || s_rv.aborted || s_rv.runs == 0 || fails > 0 || s_rv.pinch || s_rv.stuck || s_rv.resc)
                status = 2;
            snprintf(note, n, "%s corners: %ld runs (%d/%d/%d/%d per kind), exit %ld, pinch %ld, stuck %ld, resc %ld",
                     status ? "FAIL" : "ok", s_rv.runs, s_rv.kind_n[0], s_rv.kind_n[1], s_rv.kind_n[2], s_rv.kind_n[3],
                     s_rv.ok, s_rv.pinch, s_rv.stuck, s_rv.resc);
        }
    } else {
        snprintf(note, n, "roam drive not armed");
        status = 2;
    }
    return status;
}

#endif /* !TD5RE_RELEASE */
