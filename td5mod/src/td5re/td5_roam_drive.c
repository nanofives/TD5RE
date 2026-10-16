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
#include "td5_geo_roads.h"      /* TD5_GEO_RC_* */
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RV_PI         3.14159265358979323846
#define RV_M          430.0                       /* units per metre */
#define RV_UPT_KMH    (430.0 / 30.0 / 3.6)        /* units per tick at 1 km/h (30 Hz sim) */
#define RV_NP         9
#define RV_MAXC       160
#define RV_MAXKEYS    128
#define RV_UNIT_CM    (256.0 * 4.30)              /* 24.8 world units per cm */

#define RV_ABOVE_REST_MAX_CM   5.0                /* wheel bottom above the ground at rest */
#define RV_WHEEL_BELOW_MAX_CM  25.0               /* wheel bottom under the ground (tyre / suspension) */
#define RV_STUCK_TICKS         90                 /* 3 s */
#define RV_STUCK_KMH           5.0

/* the diagonal x avenida cluster whose 80 km/h run the sibling task F2-E fixes (acute-junction wedge) */
#define RV_KNOWN_CLUSTER   0x00064c21000864e3ULL
#define RV_KNOWN_KMH       80

typedef struct {
    int kind;
    double cx, cz, inx, inz, outx, outz, defl;
    uint64_t cl, ein, eout;
    double px[RV_NP], pz[RV_NP];
} RvCorner;

static const char *const k_rv_kind[4] = { "one-lane/service", "calle x calle", "avenida x calle", "diagonal x avenida" };

typedef struct {
    int      mode;
    int      armed_tick_set;
    long     t0, tick_last;
    int      phase;                 /* route: 0 tour, 1 brake, 2 rest, 3 done ; corners: 0 idle, 1 run, 2 done */
    int      done, aborted;
    char     why[96];

    /* knobs */
    double   kmh;
    int      want_junctions, max_secs, per_kind, gap_m;

    /* measured every sim tick (both modes) */
    long     samples, below_n, off_n;
    int32_t  above_min;             /* 24.8, chassis centre above the ground at the chassis */
    int      above_seen;
    uint32_t ch_hint;
    TD5_RoamHit ch;
    int      ch_ok;
    long     stuck_t;               /* consecutive ticks throttle held and < 5 km/h */
    long     stuck_events;
    int      rev_t, recoveries;

    /* route */
    double   dir_x, dir_z, off0, dist_m;
    int      have_dir, have_off, have_last;
    int32_t  lx, lz;
    double   sp_x, sp_z;            /* spawn, for homing */
    uint64_t cl_key;
    double   cl_x, cl_z, arm_x, arm_z, tgt_x, tgt_z;
    int      crossings, distinct;
    uint64_t keys[RV_MAXKEYS];
    long     rest_t, log_t;
    double   kmh_max;

    /* corners */
    RvCorner c[RV_MAXC];
    int      nc, ci, si;
    long     ct, cstuck_t, slow_t, rev_ct, stalls;
    int      retries;
    long     hits0, pinch0, resc0;
    double   vmin, vmax;
    long     runs, ok, stuck, pinch, hits, resc, known_fail;
    long     k_runs[4][2], k_ok[4][2], k_stuck[4][2], k_pinch[4][2], k_hits[4][2], k_resc[4][2], k_known[4][2];
    int      kind_n[4];
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
    int ti, found = 0;
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
                if (len < 1.0 || d2 >= best) continue;
                best = d2; *ex = qx; *ez = qz; *ox = rx / len; *oz = rz / len; found = 1;
            }
        }
    }
    return found;
}

static const RoamEdgeRec *rv_edge(const TD5_RoamResident *rs, uint64_t key, const RoamTile **tout)
{
    int i;
    uint32_t k;
    for (i = 0; i < rs->nslot; i++) {
        const RoamTile *t = rs->tile[i];
        if (!t) continue;
        for (k = 0; k < t->nedge; k++)
            if (t->edge[k].key == key) { *tout = t; return &t->edge[k]; }
    }
    return NULL;
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
    double cx, cz, ax, az, hx, hz, look, tx, tz;
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
    }
    look = 14.0 * RV_M + rv_speed_upt(a) * 20.0;

    if (s_rv.phase == 0) {
        if (s_rv.ch_ok && !s_rv.ch.is_patch && rv_street_axis(rs, &s_rv.ch, px, pz, &cx, &cz, &ax, &az)) {
            double qx, qz, off;
            if (ax * s_rv.dir_x + az * s_rv.dir_z < 0.0) { ax = -ax; az = -az; }
            if (ax * s_rv.dir_x + az * s_rv.dir_z < 0.82) {     /* > 35 deg off: a crossing street, keep going straight */
                ax = s_rv.dir_x; az = s_rv.dir_z;
                cx = px; cz = pz;
            }
            s_rv.dir_x = ax; s_rv.dir_z = az;
            qx = s_rv.dir_z; qz = -s_rv.dir_x;
            off = (px - cx) * qx + (pz - cz) * qz;
            if (!s_rv.have_off) { s_rv.off0 = off; s_rv.have_off = 1; }
            tx = cx + qx * s_rv.off0 + s_rv.dir_x * look;
            tz = cz + qz * s_rv.off0 + s_rv.dir_z * look;
        } else if (s_rv.ch_ok && s_rv.ch.is_patch) {
            const uint32_t slot = ROAM_PRIM_SLOT(s_rv.ch.prim), idx = ROAM_PRIM_INDEX(s_rv.ch.prim);
            const RoamTile *t = slot < (uint32_t)rs->nslot ? rs->tile[slot] : NULL;
            if (t && idx < t->ntri && t->tri[idx].patch < t->nnode) {
                const RoamNodeRec *nd = &t->node[t->tri[idx].patch];
                if ((uint64_t)nd->cluster_key != s_rv.cl_key) {
                    /* a new junction: pick the exit arm from the turn script (or home back to the
                     * spawn past 1.4 km), never the way we came */
                    const double hd = hypot(px - s_rv.sp_x, pz - s_rv.sp_z);
                    double wx, wz;
                    int k, bk = -1, bk_any = -1;
                    double bd = -2.0, bd_any = -2.0;
                    int turn = rv_turn_for(s_rv.crossings);
                    if (hd > 1400.0 * RV_M) {
                        wx = s_rv.sp_x - px; wz = s_rv.sp_z - pz;
                        { const double m = hypot(wx, wz); if (m > 1.0) { wx /= m; wz /= m; } }
                        turn = 99;
                    } else if (turn == 0) {
                        wx = s_rv.dir_x; wz = s_rv.dir_z;
                    } else {
                        wx = -s_rv.dir_z * turn; wz = s_rv.dir_x * turn;
                    }
                    for (k = 0; k < nd->narm; k++) {
                        const double b = (double)t->arm[nd->arm_first + k].bearing12 * (2.0 * RV_PI / 4096.0);
                        const double abx = cos(b), abz = sin(b);
                        const double want = abx * wx + abz * wz;
                        const double back = abx * s_rv.dir_x + abz * s_rv.dir_z;
                        if (want > bd_any) { bd_any = want; bk_any = k; }
                        if (back > -0.5 && want > bd) { bd = want; bk = k; }
                    }
                    if (bk < 0) bk = bk_any;
                    s_rv.cl_key = (uint64_t)nd->cluster_key;
                    s_rv.cl_x = (double)t->org_x - rs->org_x + nd->x;
                    s_rv.cl_z = (double)t->org_z - rs->org_z + nd->z;
                    s_rv.tgt_x = s_rv.tgt_z = 0.0;
                    s_rv.crossings++;
                    if (!rv_key_seen(s_rv.cl_key) && s_rv.distinct < RV_MAXKEYS) s_rv.keys[s_rv.distinct++] = s_rv.cl_key;
                    if (bk >= 0) {
                        const RoamArmRec *ar = &t->arm[nd->arm_first + bk];
                        const double b = (double)ar->bearing12 * (2.0 * RV_PI / 4096.0);
                        double ex = 0.0, ez = 0.0, ox = 0.0, oz = 0.0;
                        s_rv.arm_x = cos(b); s_rv.arm_z = sin(b);
                        if (rv_edge_end_near(rs, ar->edge_key, s_rv.cl_x, s_rv.cl_z, &ex, &ez, &ox, &oz)) {
                            s_rv.arm_x = ox; s_rv.arm_z = oz;
                            s_rv.tgt_x = ex + ox * 15.0 * RV_M; s_rv.tgt_z = ez + oz * 15.0 * RV_M;
                        }
                    } else {
                        s_rv.arm_x = s_rv.dir_x; s_rv.arm_z = s_rv.dir_z;
                    }
                    if (s_rv.tgt_x == 0.0 && s_rv.tgt_z == 0.0) {
                        s_rv.tgt_x = s_rv.cl_x + s_rv.arm_x * 25.0 * RV_M; s_rv.tgt_z = s_rv.cl_z + s_rv.arm_z * 25.0 * RV_M;
                    }
                    s_rv.have_off = 0;
                    TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] junction #%d %016llx (%d arms) at (%.0f,%.0f): turn %d, heading (%.2f,%.2f) -> "
                              "arm %d (%.2f,%.2f), %.0f m driven",
                              s_rv.crossings, (unsigned long long)nd->cluster_key, (int)nd->narm, s_rv.cl_x, s_rv.cl_z,
                              turn, s_rv.dir_x, s_rv.dir_z, bk, s_rv.arm_x, s_rv.arm_z, s_rv.dist_m);
                }
                tx = s_rv.tgt_x;
                tz = s_rv.tgt_z;
                s_rv.dir_x = s_rv.arm_x; s_rv.dir_z = s_rv.arm_z;
            } else {
                tx = px + s_rv.dir_x * look;
                tz = pz + s_rv.dir_z * look;
            }
        } else {                                        /* nothing under the chassis: hold the direction */
            tx = px + s_rv.dir_x * look;
            tz = pz + s_rv.dir_z * look;
        }
        {
            /* slow down for a turn: the target speed scales with how far the aim is off the heading */
            double e = atan2(tx - px, tz - pz) - atan2(hx, hz), vt = s_rv.kmh;
            while (e > RV_PI) e -= 2.0 * RV_PI;
            while (e < -RV_PI) e += 2.0 * RV_PI;
            if (fabs(e) > 0.25) vt = s_rv.kmh * 0.4 > 15.0 ? s_rv.kmh * 0.4 : 15.0;
            bits = rv_steer_to(a, tx, tz, vt);
        }
        if (s_rv.crossings >= s_rv.want_junctions && s_rv.distinct >= s_rv.want_junctions && !s_rv.ch.is_patch) {
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

    /* stuck: throttle held and < 5 km/h for more than 3 s. Counted once per event; a reverse
     * recovery then frees the car so the tour can go on (and the row still fails). */
    if (s_rv.rev_t > 0) {
        s_rv.rev_t--;
        bits = (uint32_t)TD5_INPUT_BRAKE | ((bits & TD5_INPUT_STEER_LEFT) ? (uint32_t)TD5_INPUT_STEER_RIGHT
                                                                           : (bits & TD5_INPUT_STEER_RIGHT) ? (uint32_t)TD5_INPUT_STEER_LEFT : 0u);
        s_rv.stuck_t = 0;
    } else if (s_rv.phase == 0) {
        if ((bits & TD5_INPUT_THROTTLE) && kmh < RV_STUCK_KMH) {
            if (++s_rv.stuck_t == RV_STUCK_TICKS + 1) {
                s_rv.stuck_events++;
                TD5_LOG_W(LOG_TAG, "[ROAMSELFTEST] STUCK #%ld at world (%.0f,%.0f) after %d junctions: %.1f km/h for > 3 s with throttle",
                          s_rv.stuck_events, px, pz, s_rv.crossings, kmh);
                if (++s_rv.recoveries > 3) { rv_finish("stuck, recoveries exhausted", 1); return 0; }
                s_rv.rev_t = 40;
            }
        } else {
            s_rv.stuck_t = 0;
        }
    }
    if (tick - s_rv.log_t >= 90 && !s_rv.done) {
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

static void rv_pick_corners(const TD5_RoamResident *rs)
{
    int ti;
    uint32_t ni;
    const double gap = (double)s_rv.gap_m * RV_M;
    uint32_t t_start = td5_plat_time_ms();
    s_rv.nc = 0;
    memset(s_rv.kind_n, 0, sizeof s_rv.kind_n);
    for (ti = 0; ti < rs->nslot; ti++) {
        const RoamTile *t = rs->tile[ti];
        if (!t) continue;
        for (ni = 0; ni < t->nnode; ni++) {
            const RoamNodeRec *nd = &t->node[ni];
            int ai, ao, kind;
            double best[4] = { 1e9, 1e9, 1e9, 1e9 };
            RvCorner cand[4];
            memset(cand, 0, sizeof cand);
            if (nd->narm < 3 || nd->narm > 4 || (nd->flags & 0x04)) continue;
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
                if (best[kind] > 1e8 || s_rv.kind_n[kind] >= s_rv.per_kind || s_rv.nc >= RV_MAXC) continue;
                for (j = 0; j < s_rv.nc; j++)
                    if (s_rv.c[j].kind == kind && hypot(s_rv.c[j].cx - cand[kind].cx, s_rv.c[j].cz - cand[kind].cz) < gap) far_ok = 0;
                if (!far_ok) continue;
                s_rv.c[s_rv.nc++] = cand[kind];
                s_rv.kind_n[kind]++;
            }
        }
    }
    TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] corners picked %d (want %d per kind, >= %d m apart) in %u ms: one-lane/service %d, "
              "calle x calle %d, avenida x calle %d, diagonal x avenida %d",
              s_rv.nc, s_rv.per_kind, s_rv.gap_m, td5_plat_time_ms() - t_start,
              s_rv.kind_n[0], s_rv.kind_n[1], s_rv.kind_n[2], s_rv.kind_n[3]);
}

static void rv_corner_start(TD5_Actor *a, long tick)
{
    const RvCorner *c = &s_rv.c[s_rv.ci];
    const double v = (s_rv.si ? 80.0 : 40.0) * RV_UPT_KMH;
    const double tx = -c->inx, tz = -c->inz;
    TD5_RoamPhysStats st;
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
}

static void rv_corner_end(long tick, int ok, int stuck, const char *why)
{
    const RvCorner *c = &s_rv.c[s_rv.ci];
    TD5_RoamPhysStats st;
    long hits, pin, resc;
    const int sp = s_rv.si;
    const int known = (c->cl == RV_KNOWN_CLUSTER && (sp ? 80 : 40) == RV_KNOWN_KMH);
    td5_roam_physics_stats(&st);
    hits = st.wall_hits - s_rv.hits0;
    pin = st.pinches - s_rv.pinch0;
    resc = st.rescues + st.rescues_y + st.teleports - s_rv.resc0;
    s_rv.runs++;
    s_rv.k_runs[c->kind][sp]++;
    s_rv.hits += hits; s_rv.k_hits[c->kind][sp] += hits;
    if (known && (!ok || stuck || pin || resc)) {
        /* the sibling task's wedge: reported, not failed */
        s_rv.known_fail++; s_rv.k_known[c->kind][sp]++;
    } else {
        s_rv.ok += ok; s_rv.stuck += stuck; s_rv.pinch += pin; s_rv.resc += resc;
        s_rv.k_ok[c->kind][sp] += ok; s_rv.k_stuck[c->kind][sp] += stuck;
        s_rv.k_pinch[c->kind][sp] += pin; s_rv.k_resc[c->kind][sp] += resc;
    }
    TD5_LOG_I(LOG_TAG, "[ROAMCORNER] #%d %s cluster=%016llx defl=%.0f v=%d km/h: %s (%s)%s ticks=%ld wall_hits=%ld pinches=%ld "
              "stuck=%d stalls=%ld retries=%d rescues=%ld speed min/max near C %.0f/%.0f km/h",
              s_rv.ci, k_rv_kind[c->kind], (unsigned long long)c->cl, c->defl, sp ? 80 : 40, ok ? "EXIT" : "NO EXIT", why,
              known && (!ok || stuck || pin || resc) ? " KNOWN(F2-E)" : "",
              tick - s_rv.ct, hits, pin, stuck, s_rv.stalls, s_rv.retries, resc, s_rv.vmin > 1e8 ? 0.0 : s_rv.vmin, s_rv.vmax);
    rv_report_line("%d\t%s\t%016llx\t%.0f\t%d\t%s\t%s\t%s\t%ld\t%ld\t%ld\t%d\t%ld\t%d\t%ld\t%.0f\t%.0f\n",
                   s_rv.ci, k_rv_kind[c->kind], (unsigned long long)c->cl, c->defl, sp ? 80 : 40, ok ? "EXIT" : "NO_EXIT", why,
                   known && (!ok || stuck || pin || resc) ? "KNOWN" : "-", tick - s_rv.ct, hits, pin, stuck, s_rv.stalls,
                   s_rv.retries, resc, s_rv.vmin > 1e8 ? 0.0 : s_rv.vmin, s_rv.vmax);
    s_rv.phase = 0;
    if (++s_rv.ci >= s_rv.nc) { s_rv.ci = 0; s_rv.si++; }
}

static uint32_t rv_corners(TD5_Actor *a, const TD5_RoamResident *rs, long tick)
{
    const RvCorner *c;
    double px, pz, kmh, look, rx, rz, along_out, lat_out, tx, tz;
    uint32_t bits;
    if (s_rv.phase == 0) {
        if (s_rv.si >= 2 || s_rv.nc == 0) {
            int k, sp;
            TD5_LOG_I(LOG_TAG, "[ROAMCORNER] SUMMARY runs=%ld exit=%ld stuck=%ld pinches=%ld wall_hits=%ld rescues=%ld known=%ld",
                      s_rv.runs, s_rv.ok, s_rv.stuck, s_rv.pinch, s_rv.hits, s_rv.resc, s_rv.known_fail);
            rv_report_line("\nSUMMARY runs=%ld exit=%ld stuck=%ld pinches=%ld wall_hits=%ld rescues=%ld known=%ld\n",
                           s_rv.runs, s_rv.ok, s_rv.stuck, s_rv.pinch, s_rv.hits, s_rv.resc, s_rv.known_fail);
            for (k = 0; k < 4; k++)
                for (sp = 0; sp < 2; sp++)
                    rv_report_line("kind %-20s %2d km/h: runs %ld exit %ld stuck %ld pinches %ld wall_hits %ld rescues %ld known %ld\n",
                                   k_rv_kind[k], sp ? 80 : 40, s_rv.k_runs[k][sp], s_rv.k_ok[k][sp], s_rv.k_stuck[k][sp],
                                   s_rv.k_pinch[k][sp], s_rv.k_hits[k][sp], s_rv.k_resc[k][sp], s_rv.k_known[k][sp]);
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
    {
        double best = 1e30, cum = 0.0, total = 0.0, s_car = 0.0, want;
        int k;
        for (k = 0; k < RV_NP - 1; k++) {
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
        tx = c->px[RV_NP - 1] + c->outx * (want - total);
        tz = c->pz[RV_NP - 1] + c->outz * (want - total);
        for (k = 0; k < RV_NP - 1; k++) {
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
    bits = rv_steer_to(a, tx, tz, s_rv.si ? 80.0 : 40.0);
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

void td5_roam_drive_arm(int mode)
{
    memset(&s_rv, 0, sizeof s_rv);
    s_rv.mode = mode;
    s_rv.kmh = (double)td5_env_int("TD5RE_ROAM_SELFTEST_KMH", 36, 10, 120);
    s_rv.want_junctions = td5_env_int("TD5RE_ROAM_SELFTEST_JUNCTIONS", 24, 1, 100);
    s_rv.max_secs = td5_env_int("TD5RE_ROAM_SELFTEST_MAX_SECS", 300, 30, 3000);
    s_rv.per_kind = td5_env_int("TD5RE_ROAM_SELFTEST_CORNERS_PER_KIND", 20, 1, 40);
    s_rv.gap_m = td5_env_int("TD5RE_ROAM_SELFTEST_CORNER_GAP_M", 120, 0, 1000);
    s_rv.ch_hint = ROAM_PRIM_NONE;
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
            rv_pick_corners(rs);
            snprintf(path, sizeof path, "%sroam_corner_test.txt", dir ? dir : "");
            s_rv.rep = fopen(path, "w");
            rv_report_line("idx\tkind\tcluster\tdefl\tkmh\tresult\twhy\tknown\tticks\twall_hits\tpinches\tstuck\tstalls\tretries\trescues\tvmin\tvmax\n");
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

int td5_roam_drive_verdict(char *note, size_t n, int *known)
{
    TD5_RoamPhysStats st;
    int status = 0;
    td5_roam_physics_stats(&st);
    if (known) *known = (int)s_rv.known_fail;
    if (s_rv.mode == TD5_RDRV_ROUTE) {
        const double rest_max = st.rest_n ? st.rest_max / RV_UNIT_CM : 0.0;
        const double gap_min = st.gap_n ? st.gap_min / RV_UNIT_CM : 0.0;
        const double above_min = s_rv.above_seen ? s_rv.above_min / RV_UNIT_CM : 0.0;
        const long resc = st.rescues + st.rescues_y;
        char bad[96] = "";
        size_t bl = 0;
#define RV_BAD(...) do { if (bl < sizeof bad - 1) bl += (size_t)snprintf(bad + bl, sizeof bad - bl, __VA_ARGS__); } while (0)
        if (!s_rv.done || s_rv.aborted) RV_BAD("%s; ", s_rv.why[0] ? s_rv.why : "drive never finished");
        if (s_rv.distinct < s_rv.want_junctions) RV_BAD("%d distinct junctions < %d; ", s_rv.distinct, s_rv.want_junctions);
        if (s_rv.below_n) RV_BAD("below surface %ld; ", s_rv.below_n);
        if (gap_min < -RV_WHEEL_BELOW_MAX_CM) RV_BAD("wheel %.1f cm under; ", -gap_min);
        if (st.rest_n == 0) RV_BAD("no rest sample; ");
        else if (rest_max > RV_ABOVE_REST_MAX_CM) RV_BAD("rest %.1f cm above; ", rest_max);
        if (s_rv.stuck_events) RV_BAD("stuck %ld; ", s_rv.stuck_events);
        if (s_rv.off_n) RV_BAD("off surface %ld ticks; ", s_rv.off_n);
        if (resc) RV_BAD("rescues %ld; ", resc);
        if (st.pinches) RV_BAD("pinches %ld; ", st.pinches);
        if (st.teleports) RV_BAD("teleports %ld; ", st.teleports);
#undef RV_BAD
        status = bad[0] ? 2 : 0;
        if (status)
            snprintf(note, n, "roam FAIL: %.100s", bad);
        else
            snprintf(note, n, "roam %d jn (%d distinct) %.1f km, snap %.0f m, rest %+.1f cm, below 0, stuck 0, resc 0, pinch 0",
                     s_rv.crossings, s_rv.distinct, s_rv.dist_m / 1000.0, td5_roam_spawn_snap_m(), rest_max);
        TD5_LOG_I(LOG_TAG, "[ROAMSELFTEST] ROUTE VERDICT %s: junctions=%d distinct=%d dist=%.0f m vmax=%.0f km/h samples=%ld "
                  "chassis_above_min=%.1f cm below_n=%ld off_n=%ld wheel_gap_min=%.2f cm rest_n=%ld rest_gap_max=%.2f cm "
                  "stuck=%ld rescues=%ld pinches=%ld teleports=%ld wall_hits=%ld spawn_snap=%.1f m %s",
                  status ? "FAIL" : "PASS", s_rv.crossings, s_rv.distinct, s_rv.dist_m, s_rv.kmh_max, s_rv.samples, above_min,
                  s_rv.below_n, s_rv.off_n, gap_min, st.rest_n, rest_max, s_rv.stuck_events, resc, st.pinches, st.teleports,
                  st.wall_hits, td5_roam_spawn_snap_m(), bad);
    } else if (s_rv.mode == TD5_RDRV_CORNERS) {
        const long fails = (s_rv.runs - s_rv.known_fail) - s_rv.ok;
        if (!s_rv.done || s_rv.aborted || s_rv.runs == 0 || fails > 0 || s_rv.pinch || s_rv.stuck || s_rv.resc)
            status = 2;
        snprintf(note, n, "%s corners: %ld runs (%d/%d/%d/%d per kind), exit %ld, pinch %ld, stuck %ld, resc %ld",
                 status ? "FAIL" : "ok", s_rv.runs, s_rv.kind_n[0], s_rv.kind_n[1], s_rv.kind_n[2], s_rv.kind_n[3],
                 s_rv.ok, s_rv.pinch, s_rv.stuck, s_rv.resc);
        if (s_rv.known_fail) {
            size_t l = strlen(note);
            snprintf(note + l, n - l, " KNOWN(F2-E) %ld", s_rv.known_fail);
        }
    } else {
        snprintf(note, n, "roam drive not armed");
        status = 2;
    }
    return status;
}

#endif /* !TD5RE_RELEASE */
