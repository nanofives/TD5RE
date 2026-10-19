/**
 * td5_roam_ribbon.c -- GEO FREE ROAM: street ribbons + heights (PORT-ONLY,
 * F1 task T4). Contract and rationale in td5_roam_ribbon.h.
 *
 * ORDER OF WORK for one edge (roam_ribbon_build):
 *   1. orient: row 0 at the end with the smaller node key
 *   2. trim the centreline to T3's cut distances
 *   3. resample with gr_resample (chord, 1500 units), pin the far end exactly
 *   4. raster profile G_k, triangular smoothing S_k, cosine end blend toward
 *      the cluster heights, bridge / bore lift L_k
 *   5. per 1 km chunk: roam_grade_solve_pin (the three-pass rule), ends pinned
 *   6. quantise ONCE (floor(v + 0.5)) into int16 rows under per-block origins,
 *      emit type-1 spans
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_geo_route.h"
#include "td5_roam_cache.h"
#include "td5_roam_ribbon.h"

#define LOG_TAG "roam"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------- rounding ----- */

/* The one rounding of every stored coordinate. Explicit: it does not depend on
 * the CRT rounding mode. */
int32_t roam_round(double v)
{
    return (int32_t)floor(v + 0.5);
}

/* ------------------------------------------------------- grade solver ------- */

/* The three-pass rule of tg_road_solve (td5_tg_road.c:374), copied statement for
 * statement, over arrays instead of the s_rn[] statics. cap/curv come from
 * either a per-row array (cap_v/curv_v, index = row) or a scalar. With uniform
 * spacing this is the strip's solve. */
#define RB_CAP(i)  (cap_v  ? cap_v[(i)]  : cap_s)
#define RB_CURV(i) (curv_v ? curv_v[(i)] : curv_s)

static int rib_solve_core(double *y, const double *t, int n,
                          const double *cap_v, double cap_s,
                          const double *curv_v, double curv_s, int pin)
{
    int i, ok = 1;
    if (n < 1) return 1;
    y[0] = t[0];
    if (n < 2) return 1;
    for (i = 1; i < n; i++) {
        const double lo = y[i - 1] - RB_CAP(i), hi = y[i - 1] + RB_CAP(i);
        double v = t[i];
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        if (RB_CURV(i) > 0.0 && i >= 2) {
            const double pg = y[i - 1] - y[i - 2];
            const double clo = y[i - 1] + pg - RB_CURV(i), chi = y[i - 1] + pg + RB_CURV(i);
            if (v < clo) v = clo;
            if (v > chi) v = chi;
            if (v < lo) v = lo;
            if (v > hi) v = hi;
        }
        y[i] = v;
    }
    if (pin) y[n - 1] = t[n - 1];
    for (i = n - 2; i > 0; i--) {
        const double lo = y[i + 1] - RB_CAP(i + 1), hi = y[i + 1] + RB_CAP(i + 1);
        double v = y[i];
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        if (RB_CURV(i + 1) > 0.0 && i + 2 < n) {
            const double ng = y[i + 1] - y[i + 2];
            const double clo = y[i + 1] + ng - RB_CURV(i + 1), chi = y[i + 1] + ng + RB_CURV(i + 1);
            if (v < clo) v = clo;
            if (v > chi) v = chi;
            if (v < lo) v = lo;
            if (v > hi) v = hi;
        }
        y[i] = v;
    }
    for (i = 1; i < n; i++) {
        const double lo = y[i - 1] - RB_CAP(i), hi = y[i - 1] + RB_CAP(i);
        double v = y[i];
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        if (RB_CURV(i) > 0.0 && i >= 2) {
            const double pg = y[i - 1] - y[i - 2];
            const double clo = y[i - 1] + pg - RB_CURV(i), chi = y[i - 1] + pg + RB_CURV(i);
            if (v < clo) v = clo;
            if (v > chi) v = chi;
            if (v < lo) v = lo;
            if (v > hi) v = hi;
        }
        y[i] = v;
    }
    if (pin && fabs(y[n - 1] - t[n - 1]) > 1e-6) {
        ok = 0;                 /* the pin was infeasible under the cap */
        y[n - 1] = t[n - 1];    /* the join row stays exact; the segment pays */
    }
    return ok;
}

int roam_grade_solve(double *y, const double *t, int n, double cap_per_row,
                     double curv_cap)
{
    return rib_solve_core(y, t, n, NULL, cap_per_row, NULL, curv_cap, 0);
}

int roam_grade_solve_pin(double *y, const double *t, int n, double cap_per_row,
                         double curv_cap)
{
    return rib_solve_core(y, t, n, NULL, cap_per_row, NULL, curv_cap, 1);
}

/* True when every segment of y[0..n) is within its cap (and the pin holds). */
static int rib_slopes_ok(const double *y, int n, const double *cap_v)
{
    int i;
    for (i = 1; i < n; i++)
        if (fabs(y[i] - y[i - 1]) > cap_v[i] * 1.0005 + 1e-6) return 0;
    return 1;
}

/* One chunk with BOTH ends pinned (rows 0 and n-1 hold t[0], t[n-1] exactly).
 *   1. If the two pin heights are further apart than the cap allows over the
 *      chunk, the cap of THIS chunk is raised just enough to join them (a uniform
 *      grade beats a plateau and a cliff). *raised = 1.
 *   2. Targets are clamped into the double cone of the two pins, so a feasible
 *      profile exists.
 *   3. The strip's three passes (roam_grade_solve's rule), curvature on. If the
 *      result breaks a cap, again with the curvature clamp off (*repair |= 1),
 *      and if that still does, a Lipschitz open-close of the result (*repair |= 2).
 * capw / tw / mw are scratch of n doubles. Returns 1 when the cap was not raised. */
static int rib_solve_chunk(double *y, const double *t, int n, const double *cap_v,
                           const double *curv_v, double *capw, double *tw,
                           double *mw, int *repair)
{
    double total = 0.0, need, scale = 1.0, cum = 0.0;
    int i, raised = 0;
    if (n < 2) { if (n == 1) y[0] = t[0]; return 1; }
    for (i = 1; i < n; i++) total += cap_v[i];
    need = fabs(t[n - 1] - t[0]);
    if (need > total) { scale = need / total * (1.0 + 1e-9); raised = 1; }
    capw[0] = 0.0;
    for (i = 1; i < n; i++) capw[i] = cap_v[i] * scale;
    total *= scale;
    tw[0] = t[0]; tw[n - 1] = t[n - 1];
    for (i = 1; i < n - 1; i++) {
        double lo, hi;
        cum += capw[i];
        lo = t[0] - cum; hi = t[0] + cum;
        if (t[n - 1] - (total - cum) > lo) lo = t[n - 1] - (total - cum);
        if (t[n - 1] + (total - cum) < hi) hi = t[n - 1] + (total - cum);
        tw[i] = t[i] < lo ? lo : (t[i] > hi ? hi : t[i]);
    }
    rib_solve_core(y, tw, n, capw, 0.0, curv_v, 0.0, 1);
    if (rib_slopes_ok(y, n, capw)) return !raised;
    *repair |= 1;
    rib_solve_core(y, tw, n, capw, 0.0, NULL, 0.0, 1);
    if (rib_slopes_ok(y, n, capw)) return !raised;
    *repair |= 2;
    /* open-close with the pins kept: the largest capw-Lipschitz minorant, then the
     * smallest Lipschitz majorant of that. Pins are preserved because tw sits
     * inside both cones. */
    for (i = 0; i < n; i++) mw[i] = tw[i];
    for (i = 1; i < n; i++) if (mw[i] > mw[i - 1] + capw[i]) mw[i] = mw[i - 1] + capw[i];
    for (i = n - 2; i >= 0; i--) if (mw[i] > mw[i + 1] + capw[i + 1]) mw[i] = mw[i + 1] + capw[i + 1];
    for (i = 1; i < n; i++) if (mw[i] < mw[i - 1] - capw[i]) mw[i] = mw[i - 1] - capw[i];
    for (i = n - 2; i >= 0; i--) if (mw[i] < mw[i + 1] - capw[i + 1]) mw[i] = mw[i + 1] - capw[i + 1];
    for (i = 0; i < n; i++) y[i] = mw[i];
    y[0] = t[0]; y[n - 1] = t[n - 1];
    return !raised;
}

/* ----------------------------------------------------------- helpers -------- */

void roam_ribbon_free(RoamRibbon *r)
{
    if (!r) return;
    free(r->span); free(r->vert); free(r->meta);
    free(r->cx); free(r->cz); free(r->ground); free(r->target); free(r->y); free(r->hw); free(r->tilt);
    memset(r, 0, sizeof *r);
}

static int rib_push(GrPts *p, double x, double z)
{
    if (p->n >= p->cap) {
        const int c = p->cap ? p->cap * 2 : 64;
        double *nx = (double *)realloc(p->x, (size_t)c * sizeof(double));
        double *nz;
        if (nx) p->x = nx;
        nz = (double *)realloc(p->z, (size_t)c * sizeof(double));
        if (nz) p->z = nz;
        if (!nx || !nz) return 0;
        p->cap = c;
    }
    p->x[p->n] = x; p->z[p->n] = z; p->n++;
    return 1;
}

/* Point at arc distance s along (px,pz) with cumulative lengths cum[]. */
static void rib_point_at(const double *px, const double *pz, const double *cum,
                         int n, double s, double *ox, double *oz)
{
    int i = 0;
    while (i < n - 2 && cum[i + 1] < s) i++;
    {
        const double seg = cum[i + 1] - cum[i];
        const double u = seg > 1e-12 ? (s - cum[i]) / seg : 0.0;
        *ox = px[i] + (px[i + 1] - px[i]) * u;
        *oz = pz[i] + (pz[i + 1] - pz[i]) * u;
    }
}

/* Nearest point of a polyline (cx,cz)[n] to (x,z): segment index, parameter and
 * squared distance. */
static void rib_nearest(const double *cx, const double *cz, int n, double x,
                        double z, int *seg, double *u, double *d2)
{
    int i, bi = 0;
    double bu = 0.0, bd = 1e300;
    for (i = 0; i + 1 < n; i++) {
        const double ax = cx[i], az = cz[i];
        const double dx = cx[i + 1] - ax, dz = cz[i + 1] - az;
        const double l2 = dx * dx + dz * dz;
        double t = l2 > 1e-12 ? ((x - ax) * dx + (z - az) * dz) / l2 : 0.0;
        double qx, qz, d;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        qx = ax + dx * t - x; qz = az + dz * t - z;
        d = qx * qx + qz * qz;
        if (d < bd) { bd = d; bi = i; bu = t; }
    }
    *seg = bi; *u = bu; *d2 = bd;
}

/* One ground sample: the raster height with the water rule undone for a road (a
 * road over water is a deck on the surface, not a dip to the bed). Prefetches the
 * edge box once on a miss. Returns 0 when no window covers the point. */
static int rib_ground(TD5_RoamHeight *h, double x, double z, const double box[4],
                      int *prefetched, double *g, int *wet)
{
    double hg;
    int w = 0, ok = td5_roam_height_sample(h, x, z, &hg, &w);
    if (!ok && !*prefetched) {
        *prefetched = 1;
        if (td5_roam_height_prefetch(h, box[0], box[1], box[2], box[3]))
            ok = td5_roam_height_sample(h, x, z, &hg, &w);
    }
    if (!ok) return 0;
    *g = w ? hg + TD5_ROAM_WATER_DEPTH : hg;
    *wet = w ? 1 : 0;
    return 1;
}

int roam_cluster_height(TD5_RoamHeight *h, const double (*pts)[2], int n,
                        double *out)
{
    double sum = 0.0;
    int i;
    if (!h || n < 1 || !pts) return 0;
    for (i = 0; i < n; i++) {
        double g;
        int w = 0, pf = 0;
        const double box[4] = { pts[i][0] - 3000.0, pts[i][1] - 3000.0,
                                pts[i][0] + 3000.0, pts[i][1] + 3000.0 };
        if (!rib_ground(h, pts[i][0], pts[i][1], box, &pf, &g, &w)) return 0;
        sum += g;
    }
    *out = sum / (double)n;
    return 1;
}

static int rib_grip(int surface)
{
    /* k_road_surf[].grip_class of td5_tg_terrain.c: tarmac 0, dirt 2, cobble 4. */
    switch (surface) {
    case TD5_GEO_SURF_COBBLE: return 4;
    case TD5_GEO_SURF_LOOSE:  return 2;
    default:                  return 0;
    }
}

/* Row 0 is at the end with the smaller node key. A loop (both ends one node) is
 * oriented by its second point against its second-to-last, so the input
 * direction does not matter either. */
static int rib_is_reversed(const RoamEdgeIn *in)
{
    if (in->node_a == in->node_b && in->n >= 3) {
        const double ax = in->px[1], az = in->pz[1];
        const double bx = in->px[in->n - 2], bz = in->pz[in->n - 2];
        return (ax > bx) || (ax == bx && az > bz);
    }
    return in->node_a > in->node_b;
}

/* ------------------------------------------------------ coupling (T10) ----- */

/* Height of a finished ribbon's surface at (x, z), when the point lies on it:
 * the solved centre profile at the point's projection on the centre polyline,
 * plus the row's cross slope (tilt, coupled rows only) times the signed lateral
 * offset. A point before row 0 or past the last row, or further out than the
 * row half width, is not on it. A margin of `inset` units is taken off the half
 * width (negative: the surface is extended sideways by that much). */
static int rib_surface_at(const RoamRibbon *r, double x, double z, double inset,
                          double *out_y)
{
    int i, best = -1;
    double bd = 1e300, bu = 0.0;
    if (!r || r->nrows < 2 || !r->cx || !r->y || !r->hw) return 0;
    for (i = 0; i + 1 < r->nrows; i++) {
        const double ax = r->cx[i], az = r->cz[i];
        const double dx = r->cx[i + 1] - ax, dz = r->cz[i + 1] - az;
        const double l2 = dx * dx + dz * dz;
        double t, qx, qz, d;
        if (l2 <= 1e-12) continue;
        t = ((x - ax) * dx + (z - az) * dz) / l2;
        if ((i == 0 && t < 0.0) || (i + 2 == r->nrows && t > 1.0)) continue;   /* past an end */
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        qx = ax + dx * t - x; qz = az + dz * t - z;
        d = qx * qx + qz * qz;
        if (d < bd) { bd = d; best = i; bu = t; }
    }
    if (best < 0) return 0;
    {
        const double hw = r->hw[best] + bu * (r->hw[best + 1] - r->hw[best]) - inset;
        if (hw <= 0.0 || bd > hw * hw) return 0;
    }
    *out_y = r->y[best] + bu * (r->y[best + 1] - r->y[best]);
    if (r->tilt) {
        const double dx = r->cx[best + 1] - r->cx[best], dz = r->cz[best + 1] - r->cz[best];
        const double l = hypot(dx, dz);
        if (l > 1e-9) {
            const double qx = r->cx[best] + dx * bu, qz = r->cz[best] + dz * bu;
            const double lat = ((x - qx) * dz - (z - qz) * dx) / l;      /* along (dz, -dx) */
            *out_y += (r->tilt[best] + bu * (r->tilt[best + 1] - r->tilt[best])) * lat;
        }
    }
    return 1;
}

/* Height of a patch triangle list at (x, z): the first triangle that holds the
 * point (edges inclusive), its plane. Triangles are 9 doubles (x,y,z x3). */
static int rib_patch_at(const double *tri, int ntri, double x, double z, double *out_y)
{
    int t;
    for (t = 0; t < ntri; t++) {
        const double *v = tri + 9 * t;
        const double ax = v[0], ay = v[1], az = v[2], bx = v[3], by = v[4], bz = v[5];
        const double cx = v[6], cy = v[7], cz = v[8];
        const double den = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
        double l1, l2;
        if (fabs(den) < 1e-9) continue;
        l1 = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / den;
        l2 = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / den;
        if (l1 < -1e-9 || l2 < -1e-9 || l1 + l2 > 1.0 + 1e-9) continue;
        *out_y = l1 * ay + l2 * by + (1.0 - l1 - l2) * cy;
        return 1;
    }
    return 0;
}

/* Step 5b. Rows whose footprint lies on a leader take the leader's height there
 * (the mean over the row's lane boundary and lane centre points that are on it;
 * ribbon leaders first, in priority order, then foreign patches; the first that
 * covers any point wins). A row point up to ROAM_COUPLE_REACH outside a leader
 * ribbon counts: a follower that only grazes a kerb meets it at the same height.
 *
 * NO NEW GRADE ABOVE THE CAP. The pinned end rows are shared vertices and never
 * move. The anchors are first held inside each pin's cap cone, then projected
 * onto the cap-Lipschitz set (largest minorant, then smallest majorant, over the
 * anchor rows: the pins, being inside every other anchor's cone, stay put). Every
 * other row is clamped between the cap envelopes of those anchors: min/max of
 * cap-Lipschitz functions is cap-Lipschitz, so a quad that was within the cap
 * stays within it, and rows beyond the envelopes' reach keep their free profile.
 * Where the leader itself is steeper than the cap allows along this ribbon the
 * follower departs from it (counted in couple_pin_clamped). Returns 0 on OOM. */
#define ROAM_COUPLE_REACH 1500.0    /* units (3.5 m): the leader is laterally flat, so its surface extends sideways exactly */
static int rib_couple(const RoamEdgeIn *in, const double *rx, const double *rz,
                      const double *arc, const double *seg, const double *HW, int n,
                      int has_pa, int has_pb, double *Y, double *TILT, RoamRibbon *out)
{
    double *A = (double *)malloc((size_t)n * sizeof(double));
    double *U = (double *)malloc((size_t)n * sizeof(double));
    double *L = (double *)malloc((size_t)n * sizeof(double));
    unsigned char *isa = (unsigned char *)calloc((size_t)n, 1);
    double *sk = (double *)malloc((size_t)n * sizeof(double));   /* shortest run into row i */
    double *ak = (double *)malloc((size_t)n * sizeof(double));   /* its running sum          */
    unsigned leadmask = 0;
    int i, j, nanch = 0;
    double s_end;
    if (!A || !U || !L || !isa || !sk || !ak) { free(A); free(U); free(L); free(isa); free(sk); free(ak); return 0; }
    /* Every cap constraint below is measured over the SHORTEST of the centre and
     * the two kerb runs between two rows: on a bend the inner kerb is shorter, so
     * a centre profile exactly at the cap would put that kerb over it. */
    for (i = 0; i < n; i++) {
        const int a = i > 0 ? i - 1 : 0, b = i < n - 1 ? i + 1 : n - 1;
        double dx = rx[b] - rx[a], dz = rz[b] - rz[a];
        const double l = hypot(dx, dz);
        if (l > 1e-9) { dx /= l; dz /= l; } else { dx = 1.0; dz = 0.0; }
        L[i] = dz;
        A[i] = -dx;
    }
    sk[0] = 0.0;
    ak[0] = 0.0;
    for (i = 1; i < n; i++) {
        double m = seg[i];
        int sd;
        for (sd = -1; sd <= 1; sd += 2) {
            const double ax = rx[i - 1] + L[i - 1] * HW[i - 1] * sd, az = rz[i - 1] + A[i - 1] * HW[i - 1] * sd;
            const double bx = rx[i] + L[i] * HW[i] * sd, bz = rz[i] + A[i] * HW[i] * sd;
            const double run = hypot(bx - ax, bz - az);
            if (run < m) m = run;
        }
        sk[i] = m;
        ak[i] = ak[i - 1] + m;
    }
    s_end = ak[n - 1];
    (void)arc;
    for (i = 0; i < n; i++) {
        const int a = i > 0 ? i - 1 : 0, b = i < n - 1 ? i + 1 : n - 1;
        double dx = rx[b] - rx[a], dz = rz[b] - rz[a];
        const double l = hypot(dx, dz);
        const int ns = 2 * out->lanes + 1;
        double lx, lz;
        int q;
        if ((i == 0 && has_pa) || (i == n - 1 && has_pb)) continue;
        if (l <= 1e-9) continue;
        dx /= l; dz /= l;
        lx = dz; lz = -dx;
        for (q = -1; q < in->nlead; q++) {             /* -1: the patches first */
            double sum = 0.0, st = 0.0, stt = 0.0, sty = 0.0;
            int hits = 0, k;
            if (q < 0 && (!in->ltri || in->nltri <= 0)) continue;
            for (k = 0; k < ns; k++) {
                const double t = -HW[i] + (double)k * (2.0 * HW[i] / (double)(ns - 1));
                const double px = rx[i] + lx * t, pz = rz[i] + lz * t;
                double yl;
                const int on = q >= 0 ? rib_surface_at(in->lead[q], px, pz, -ROAM_COUPLE_REACH, &yl)
                                      : rib_patch_at(in->ltri, in->nltri, px, pz, &yl);
                if (on) { sum += yl; st += t; stt += t * t; sty += t * yl; hits++; }
            }
            if (hits > 0) {
                /* the row follows the leader ACROSS too: a least-squares line
                 * over the points on it (a flat row cannot match a leader it
                 * crosses at an angle on a grade); the cross slope is capped */
                const double mt = st / (double)hits, my = sum / (double)hits;
                const double var = stt / (double)hits - mt * mt;
                double tl = 0.0;
                if (hits >= 2 && var > 1e3) tl = (sty / (double)hits - mt * my) / var;
                if (tl > ROAM_GRADE_CAP) tl = ROAM_GRADE_CAP;
                if (tl < -ROAM_GRADE_CAP) tl = -ROAM_GRADE_CAP;
                TILT[i] = tl;
                A[i] = my - tl * mt;
                isa[i] = 1;
                nanch++;
                if (q < 0) out->couple_patch_rows++;
                else if (q < 32) leadmask |= 1u << q;
                break;
            }
        }
    }
    if (nanch > 0) {
        const double cap = ROAM_GRADE_CAP * 0.97;   /* margin: the emitted end rows are T3 cut rows */
        /* an anchor inside the cap cone of each pinned end row */
        for (i = 0; i < n; i++) {
            double lo = -1e300, hi = 1e300, v;
            if (isa[i] != 1) continue;
            if (has_pa) { lo = Y[0] - cap * ak[i]; hi = Y[0] + cap * ak[i]; }
            if (has_pb) {
                const double r = cap * (s_end - ak[i]);
                if (Y[n - 1] - r > lo) lo = Y[n - 1] - r;
                if (Y[n - 1] + r < hi) hi = Y[n - 1] + r;
            }
            if (lo > hi) {
                /* the two pins are further apart than the cap allows (a PIN_MISS
                 * chunk, solved at a raised cap): no anchor can do better than the
                 * solver's uniform grade there */
                isa[i] = 0;
                TILT[i] = 0.0;
                nanch--;
                out->couple_pin_clamped++;
                continue;
            }
            v = A[i] < lo ? lo : (A[i] > hi ? hi : A[i]);
            if (v != A[i]) { out->couple_pin_clamped++; A[i] = v; }
        }
    }
    if (nanch > 0) {
        const double cap = ROAM_GRADE_CAP * 0.97;   /* margin: the emitted end rows are T3 cut rows */
        int prev;
        /* the pinned end rows are anchors too */
        if (has_pa) { A[0] = Y[0]; isa[0] = 2; }
        if (has_pb) { A[n - 1] = Y[n - 1]; isa[n - 1] = 2; }
        /* anchors onto the cap-Lipschitz set: min-plus forward/backward (the largest
         * minorant), then max-plus forward/backward (the smallest majorant of it) */
        prev = -1;
        for (i = 0; i < n; i++) {
            if (!isa[i]) continue;
            if (prev >= 0 && isa[i] == 1 && A[prev] + cap * (ak[i] - ak[prev]) < A[i]) A[i] = A[prev] + cap * (ak[i] - ak[prev]);
            prev = i;
        }
        prev = -1;
        for (i = n - 1; i >= 0; i--) {
            if (!isa[i]) continue;
            if (prev >= 0 && isa[i] == 1 && A[prev] + cap * (ak[prev] - ak[i]) < A[i]) A[i] = A[prev] + cap * (ak[prev] - ak[i]);
            prev = i;
        }
        prev = -1;
        for (i = 0; i < n; i++) {
            if (!isa[i]) continue;
            if (prev >= 0 && isa[i] == 1 && A[prev] - cap * (ak[i] - ak[prev]) > A[i]) A[i] = A[prev] - cap * (ak[i] - ak[prev]);
            prev = i;
        }
        prev = -1;
        for (i = n - 1; i >= 0; i--) {
            if (!isa[i]) continue;
            if (prev >= 0 && isa[i] == 1 && A[prev] - cap * (ak[prev] - ak[i]) > A[i]) A[i] = A[prev] - cap * (ak[prev] - ak[i]);
            prev = i;
        }
        for (i = 0; i < n; i++) { U[i] = isa[i] ? A[i] : 1e300; L[i] = isa[i] ? A[i] : -1e300; }
        for (i = 1; i < n; i++) {
            if (U[i - 1] + cap * sk[i] < U[i]) U[i] = U[i - 1] + cap * sk[i];
            if (L[i - 1] - cap * sk[i] > L[i]) L[i] = L[i - 1] - cap * sk[i];
        }
        for (i = n - 2; i >= 0; i--) {
            if (U[i + 1] + cap * sk[i + 1] < U[i]) U[i] = U[i + 1] + cap * sk[i + 1];
            if (L[i + 1] - cap * sk[i + 1] > L[i]) L[i] = L[i + 1] - cap * sk[i + 1];
        }
        for (i = 0; i < n; i++) {
            double v = Y[i];
            if (isa[i] == 2) continue;                  /* a pin stays exact */
            if (isa[i] == 1) v = A[i];
            else {
                if (v < L[i]) v = L[i];
                if (v > U[i]) v = U[i];
            }
            if (fabs(v - Y[i]) > out->couple_max_dy) out->couple_max_dy = fabs(v - Y[i]);
            Y[i] = v;
        }
        for (j = 0; j < n; j++) if (isa[j] == 1) out->couple_rows++;
        for (j = 0; j < 32; j++) if (leadmask & (1u << j)) out->couple_lead++;
        /* The cross slope may only change as fast as the kerb lines allow: a kerb
         * vertex rises |dY| + |dtilt| * hw between two rows, which must stay within
         * the cap over THAT kerb's run (shorter on the inside of a bend). Budgets
         * b_i = min over both kerbs of (cap * run - |Y_i - Y_i-1|) / hw; the tilts
         * are projected onto that Lipschitz set (min-plus, then max-plus, both
         * ways), pinned rows held flat. */
        for (i = 0; i < n; i++) {
            const int a = i > 0 ? i - 1 : 0, b = i < n - 1 ? i + 1 : n - 1;
            double dx = rx[b] - rx[a], dz = rz[b] - rz[a];
            const double l = hypot(dx, dz);
            if (l > 1e-9) { dx /= l; dz /= l; } else { dx = 1.0; dz = 0.0; }
            L[i] = dz;                                    /* L reused: row left axis x */
            A[i] = -dx;                                   /* A reused: row left axis z */
        }
        for (i = 1; i < n; i++) {
            const double hwm = HW[i] > HW[i - 1] ? HW[i] : HW[i - 1];
            const double dy = fabs(Y[i] - Y[i - 1]);
            double bud = 1e300;
            int sd;
            for (sd = -1; sd <= 1; sd += 2) {
                const double ax = rx[i - 1] + L[i - 1] * HW[i - 1] * sd, az = rz[i - 1] + A[i - 1] * HW[i - 1] * sd;
                const double bx = rx[i] + L[i] * HW[i] * sd, bz = rz[i] + A[i] * HW[i] * sd;
                const double run = hypot(bx - ax, bz - az);
                if (cap * run - dy < bud) bud = cap * run - dy;
            }
            if (bud < 0.0) bud = 0.0;
            U[i] = hwm > 1e-9 ? bud / hwm : 0.0;          /* U reused: budget into row i */
        }
        U[0] = 0.0;
        for (i = 0; i < n; i++) if (isa[i] == 2) TILT[i] = 0.0;
        for (i = 1; i < n; i++) if (isa[i] != 2 && TILT[i] > TILT[i - 1] + U[i]) TILT[i] = TILT[i - 1] + U[i];
        for (i = n - 2; i >= 0; i--) if (isa[i] != 2 && TILT[i] > TILT[i + 1] + U[i + 1]) TILT[i] = TILT[i + 1] + U[i + 1];
        for (i = 1; i < n; i++) if (isa[i] != 2 && TILT[i] < TILT[i - 1] - U[i]) TILT[i] = TILT[i - 1] - U[i];
        for (i = n - 2; i >= 0; i--) if (isa[i] != 2 && TILT[i] < TILT[i + 1] - U[i + 1]) TILT[i] = TILT[i + 1] - U[i + 1];
    }
    free(A); free(U); free(L); free(isa); free(sk); free(ak);
    return 1;
}

/* [F1-T10] FOLDED QUADS. A lane quad must be a positively oriented quadrilateral
 * (both of its triangles counter-clockwise in the stored frame). On a sharp turn
 * next to a given end row (a mitred JOIN row after a 1.7 m dog-leg, service way
 * 1322710577) the row before it crosses it and the quad folds into a bow-tie the
 * locator cannot hit (T6b's 'degenerate span'). The non-end row of a folded END
 * quad is dropped, repeatedly, until it does not fold or only the two end rows
 * remain (interior folds on a hairpin are left alone: dropping rows there would
 * cut the corner).
 * Heights of the kept rows are unchanged, so a merged segment's grade is the mean
 * of the capped ones. The row positions are formed exactly as step 6 forms them.
 * Returns the new row count; arrays are compacted in place. */
static void rib_row_vertex(const double *rx, const double *rz, int n, int row, int j, int L, double lane_w,
                           int has_rf, int has_rl, const int32_t (*rowf)[2], const int32_t (*rowl)[2],
                           double *vx, double *vz)
{
    const int a = row > 0 ? row - 1 : 0, b = row < n - 1 ? row + 1 : n - 1;
    double dx = rx[b] - rx[a], dz = rz[b] - rz[a];
    const double l = hypot(dx, dz);
    const double t = ((double)L * 0.5 - (double)j) * lane_w;
    if (row == 0 && has_rf) { *vx = rowf[j][0]; *vz = rowf[j][1]; return; }
    if (row == n - 1 && has_rl) { *vx = rowl[j][0]; *vz = rowl[j][1]; return; }
    if (l > 1e-9) { dx /= l; dz /= l; } else { dx = 0.0; dz = 1.0; }
    *vx = rx[row] + dz * t;
    *vz = rz[row] - dx * t;
}

static int rib_untwist(double *rx, double *rz, double *Y, double *HW, double *TILT, double *G, double *TS,
                       int n, int L, double lane_w, int has_rf, int has_rl,
                       const int32_t (*rowf)[2], const int32_t (*rowl)[2], unsigned *fl)
{
    int guard;
    for (guard = 0; guard < 64 && n > 2; guard++) {
        int i, j, drop = -1;
        for (i = 0; i + 1 < n && drop < 0; i++) {
            if (i != 0 && i + 2 != n) continue;      /* only the quads at a given end row */
            for (j = 0; j < L && drop < 0; j++) {
                double ax, az, bx, bz, cx, cz, dx, dz, c1, c2;
                rib_row_vertex(rx, rz, n, i, j, L, lane_w, has_rf, has_rl, rowf, rowl, &ax, &az);
                rib_row_vertex(rx, rz, n, i + 1, j, L, lane_w, has_rf, has_rl, rowf, rowl, &bx, &bz);
                rib_row_vertex(rx, rz, n, i + 1, j + 1, L, lane_w, has_rf, has_rl, rowf, rowl, &cx, &cz);
                rib_row_vertex(rx, rz, n, i, j + 1, L, lane_w, has_rf, has_rl, rowf, rowl, &dx, &dz);
                c1 = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
                c2 = (cx - ax) * (dz - az) - (cz - az) * (dx - ax);
                if (c1 <= 0.0 || c2 <= 0.0) drop = (i + 1 < n - 1) ? i + 1 : i;
            }
        }
        if (drop <= 0 || drop >= n - 1) break;
        for (i = drop; i + 1 < n; i++) {
            rx[i] = rx[i + 1]; rz[i] = rz[i + 1]; Y[i] = Y[i + 1]; HW[i] = HW[i + 1];
            G[i] = G[i + 1]; TS[i] = TS[i + 1];
            if (TILT) TILT[i] = TILT[i + 1];
        }
        n--;
        *fl |= ROAM_RIBF_UNTWIST;
    }
    return n;
}

/* -------------------------------------------------------------- build ------- */

int roam_ribbon_build(const RoamEdgeIn *in, TD5_RoamHeight *h, RoamRibbon *out)
{
    double *ox = NULL, *oz = NULL, *cum = NULL;     /* oriented raw polyline */
    GrPts trim, rs;
    double *rx = NULL, *rz = NULL, *seg = NULL, *G = NULL, *S = NULL, *T = NULL;
    double *TS = NULL, *Y = NULL, *capv = NULL, *curvv = NULL, *HW = NULL, *TILT = NULL;
    double *capw = NULL, *tw = NULL, *mw = NULL;    /* solve scratch */
    int32_t rowf[ROAM_RIB_MAX_LANES + 1][2], rowl[ROAM_RIB_MAX_LANES + 1][2];
    int      has_rf = 0, has_rl = 0;                /* T3 end rows given */
    double *cs = NULL, *cbase = NULL, *cyu = NULL, *cu = NULL, *arc = NULL;
    int    *csg = NULL;                             /* per crossing on this ribbon */
    int    *wetv = NULL;
    int      n = 0, i, rev, lanes, nq, b;
    double   ltot, ca, cb, pa, pb, s_tot;
    int      has_pa, has_pb, attempt, nch = 1, ncr = 0, prefetched = 0;
    int      st = ROAM_RIB_BAD_INPUT;
    unsigned fl = 0;
    double   box[4], lane_w;
    int      chunk_b[64];
    int     *chunk_big = NULL;                      /* when > 63 chunks */
    int      cbn = 0;
    int     *cb_ptr;

    memset(out, 0, sizeof *out);
    memset(&trim, 0, sizeof trim);
    memset(&rs, 0, sizeof rs);
    if (!in || !h || !in->px || !in->pz || in->n < 2) return out->status = st;
    out->key = in->key;
    lanes = in->lanes < 1 ? 1 : (in->lanes > ROAM_RIB_MAX_LANES ? ROAM_RIB_MAX_LANES : in->lanes);
    out->lanes = lanes;
    lane_w = in->lane_w > 0.0 ? in->lane_w : ROAM_RIB_LANE_W;

    /* 1. orient */
    ox = (double *)malloc((size_t)in->n * sizeof(double));
    oz = (double *)malloc((size_t)in->n * sizeof(double));
    cum = (double *)malloc((size_t)in->n * sizeof(double));
    if (!ox || !oz || !cum) goto done;
    rev = rib_is_reversed(in);
    for (i = 0; i < in->n; i++) {
        const int k = rev ? in->n - 1 - i : i;
        ox[i] = in->px[k]; oz[i] = in->pz[k];
    }
    if (rev) { ca = in->cut_b; cb = in->cut_a; pa = in->pin_b; pb = in->pin_a; }
    else     { ca = in->cut_a; cb = in->cut_b; pa = in->pin_a; pb = in->pin_b; }
    /* T3 end rows, in the oriented direction: reversing the traffic direction turns
     * right into left, so a reversed ribbon swaps the ends and flips each row. */
    if (in->rowq_a || in->rowq_b) {
        int j;
        const int L = lanes;
        for (j = 0; j <= L; j++) {
            const int32_t (*sf)[2] = rev ? in->rowq_b : in->rowq_a;
            const int32_t (*sl)[2] = rev ? in->rowq_a : in->rowq_b;
            const int k = rev ? L - j : j;
            if (sf) { rowf[j][0] = sf[k][0]; rowf[j][1] = sf[k][1]; }
            if (sl) { rowl[j][0] = sl[k][0]; rowl[j][1] = sl[k][1]; }
        }
        has_rf = (rev ? in->rowq_b : in->rowq_a) != NULL;
        has_rl = (rev ? in->rowq_a : in->rowq_b) != NULL;
    }
    if (in->node_a == in->node_b) fl |= ROAM_RIBF_LOOP;
    if (ca < 0.0) ca = 0.0;
    if (cb < 0.0) cb = 0.0;
    has_pa = !isnan(pa);
    has_pb = !isnan(pb);

    /* 2. trim to the cut distances */
    cum[0] = 0.0;
    for (i = 1; i < in->n; i++)
        cum[i] = cum[i - 1] + hypot(ox[i] - ox[i - 1], oz[i] - oz[i - 1]);
    ltot = cum[in->n - 1];
    if (ltot - ca - cb < 1.0) { st = ROAM_RIB_ABSORBED; goto done; }
    {
        double x, z;
        rib_point_at(ox, oz, cum, in->n, ca, &x, &z);
        if (!rib_push(&trim, x, z)) goto done;
        for (i = 1; i < in->n - 1; i++)
            if (cum[i] > ca + 1e-6 && cum[i] < ltot - cb - 1e-6)
                if (!rib_push(&trim, ox[i], oz[i])) goto done;
        rib_point_at(ox, oz, cum, in->n, ltot - cb, &x, &z);
        if (!rib_push(&trim, x, z)) goto done;
    }

    /* 3. resample (chord) and pin the far end */
    if (!gr_resample(&trim, ROAM_RIB_STEP, &rs)) goto done;
    {
        const double ex = trim.x[trim.n - 1], ez = trim.z[trim.n - 1];
        if (rs.n < 2) {
            rs.n = 0;
            if (!rib_push(&rs, trim.x[0], trim.z[0]) || !rib_push(&rs, ex, ez)) goto done;
        } else {
            const double d = hypot(ex - rs.x[rs.n - 1], ez - rs.z[rs.n - 1]);
            if (d >= 1e-3) {
                if (d < 0.5 * ROAM_RIB_STEP) { rs.x[rs.n - 1] = ex; rs.z[rs.n - 1] = ez; }
                else if (!rib_push(&rs, ex, ez)) goto done;
            }
        }
    }
    n = rs.n;
    nq = n - 1;
    rx = rs.x; rz = rs.z;
    box[0] = box[2] = rx[0]; box[1] = box[3] = rz[0];
    for (i = 1; i < n; i++) {
        if (rx[i] < box[0]) box[0] = rx[i];
        if (rx[i] > box[2]) box[2] = rx[i];
        if (rz[i] < box[1]) box[1] = rz[i];
        if (rz[i] > box[3]) box[3] = rz[i];
    }
    box[0] -= 3000.0; box[1] -= 3000.0; box[2] += 3000.0; box[3] += 3000.0;

    seg   = (double *)malloc((size_t)n * sizeof(double));
    G     = (double *)malloc((size_t)n * sizeof(double));
    S     = (double *)malloc((size_t)n * sizeof(double));
    T     = (double *)malloc((size_t)n * sizeof(double));
    TS    = (double *)malloc((size_t)n * sizeof(double));
    Y     = (double *)malloc((size_t)n * sizeof(double));
    capv  = (double *)malloc((size_t)n * sizeof(double));
    curvv = (double *)malloc((size_t)n * sizeof(double));
    wetv  = (int *)malloc((size_t)n * sizeof(int));
    arc   = (double *)malloc((size_t)n * sizeof(double));
    capw  = (double *)malloc((size_t)n * sizeof(double));
    tw    = (double *)malloc((size_t)n * sizeof(double));
    mw    = (double *)malloc((size_t)n * sizeof(double));
    if (!seg || !G || !S || !T || !TS || !Y || !capv || !curvv || !wetv || !arc ||
        !capw || !tw || !mw) goto done;
    seg[0] = 0.0;
    arc[0] = 0.0;
    s_tot = 0.0;
    for (i = 1; i < n; i++) {
        seg[i] = hypot(rx[i] - rx[i - 1], rz[i] - rz[i - 1]);
        s_tot += seg[i];
        arc[i] = s_tot;
    }
    out->length = s_tot;

    /* 4. ground profile, smoothing, end blend */
    for (i = 0; i < n; i++) {
        int w = 0;
        if (!rib_ground(h, rx[i], rz[i], box, &prefetched, &G[i], &w)) {
            st = ROAM_RIB_HEIGHT;
            goto done;
        }
        wetv[i] = w;
        if (w) out->wet_rows++;
    }
    for (i = 0; i < n; i++) {
        double sw = 0.0, sg = 0.0;
        int j;
        for (j = -ROAM_RIB_SMOOTH; j <= ROAM_RIB_SMOOTH; j++) {
            const int k = i + j;
            const double wgt = (double)(ROAM_RIB_SMOOTH + 1 - (j < 0 ? -j : j));
            if (k < 0 || k >= n) continue;
            sw += wgt; sg += wgt * G[k];
        }
        S[i] = sg / sw;
        T[i] = S[i];
    }
    const int blend_both = has_pa && has_pb && n - 1 < 2 * ROAM_RIB_BLEND_ROWS - 1;
    if (blend_both) {
        /* [F1 T9] The two end blends overlap (a short piece, e.g. a 10 m stub
         * from a JOIN to a junction). Applied one after the other, the far end's
         * blend reached row 0 and moved it off its pin, so the JOIN row stood up
         * to 0.6 m away from the same row of the partner ribbon (304 open lane
         * segments in the casco audit). Blended together instead, with the
         * straight line between the pins where both weigh in: row 0 is pa and
         * row n-1 is pb exactly, and the weights still sum to one. */
        for (i = 0; i < n; i++) {
            const int ia = i, ib = n - 1 - i;
            const double wa = ia < ROAM_RIB_BLEND_ROWS
                ? 0.5 * (1.0 + cos(M_PI * (double)ia / (double)ROAM_RIB_BLEND_ROWS)) : 0.0;
            const double wb = ib < ROAM_RIB_BLEND_ROWS
                ? 0.5 * (1.0 + cos(M_PI * (double)ib / (double)ROAM_RIB_BLEND_ROWS)) : 0.0;
            const double f = n > 1 ? (double)i / (double)(n - 1) : 0.0;
            const double mid = pa + (pb - pa) * f;
            T[i] = (1.0 - wa) * (1.0 - wb) * T[i] + wa * (1.0 - wb) * pa
                 + wb * (1.0 - wa) * pb + wa * wb * mid;
        }
        T[0] = pa;
        T[n - 1] = pb;
    } else if (has_pa) {
        for (i = 0; i < ROAM_RIB_BLEND_ROWS && i < n; i++) {
            const double w = 0.5 * (1.0 + cos(M_PI * (double)i / (double)ROAM_RIB_BLEND_ROWS));
            T[i] = T[i] * (1.0 - w) + pa * w;
        }
    }
    if (has_pb && !blend_both) {
        for (i = 0; i < ROAM_RIB_BLEND_ROWS && i < n; i++) {
            const int k = n - 1 - i;
            const double w = 0.5 * (1.0 + cos(M_PI * (double)i / (double)ROAM_RIB_BLEND_ROWS));
            T[k] = T[k] * (1.0 - w) + pb * w;
        }
    }

    /* crossings under a bridge deck: where they sit on the ribbon, and the base
     * target there */
    if ((in->bridge || in->tunnel == 1) && in->ncross > 0 && in->cross) {
        cs    = (double *)malloc((size_t)in->ncross * sizeof(double));
        cbase = (double *)malloc((size_t)in->ncross * sizeof(double));
        cyu   = (double *)malloc((size_t)in->ncross * sizeof(double));
        csg   = (int *)malloc((size_t)in->ncross * sizeof(int));
        cu    = (double *)malloc((size_t)in->ncross * sizeof(double));
        if (!cs || !cbase || !cyu || !csg || !cu) goto done;
        for (i = 0; i < in->ncross; i++) {
            int sg;
            double u, d2;
            rib_nearest(rx, rz, n, in->cross[i].x, in->cross[i].z, &sg, &u, &d2);
            if (d2 > ROAM_RIB_STEP * ROAM_RIB_STEP) continue;   /* not on this ribbon */
            cs[ncr]    = arc[sg] + u * seg[sg + 1];
            cbase[ncr] = T[sg] + u * (T[sg + 1] - T[sg]);
            cyu[ncr]   = in->cross[i].y_under;
            csg[ncr]   = sg;
            cu[ncr]    = u;
            ncr++;
        }
    }

    /* 5. lift + solve per chunk. Two attempts: the strip's cap, then 0.20. */
    out->flags = 0;
    {
        /* chunk boundaries: multiples of ROAM_RIB_CHUNK_ROWS, a short tail merges */
        int nchunk = nq / ROAM_RIB_CHUNK_ROWS;
        const int rem = nq - nchunk * ROAM_RIB_CHUNK_ROWS;
        int c;
        if (nchunk < 1) nchunk = 1;
        else if (rem >= ROAM_RIB_CHUNK_TAIL) nchunk++;
        nch = nchunk;
        cb_ptr = chunk_b;
        if (nch + 1 > 64) {
            chunk_big = (int *)malloc((size_t)(nch + 1) * sizeof(int));
            if (!chunk_big) goto done;
            cb_ptr = chunk_big;
        }
        for (c = 0; c < nch; c++) cb_ptr[c] = c * ROAM_RIB_CHUNK_ROWS;
        cb_ptr[nch] = n - 1;
        cbn = nch;
        if (nch > 1) fl |= ROAM_RIBF_CHUNKED;
    }
    (void)cbn;
    if (in->bridge) fl |= ROAM_RIBF_BRIDGE;
    if (in->tunnel == 1) fl |= ROAM_RIBF_BORE;

    for (attempt = 0; attempt < 2; attempt++) {
        const double cap = attempt ? ROAM_GRADE_ABSMAX : ROAM_GRADE_CAP;
        const double rg = ROAM_XSEP_RAMP_FRAC * cap;     /* ramp grade */
        int c, k, miss = 0, clear_ok = 1;

        for (i = 1; i < n; i++) {
            capv[i]  = cap * seg[i];
            curvv[i] = ROAM_CURV_CAP * seg[i];
        }
        capv[0] = curvv[0] = 0.0;
        for (i = 0; i < n; i++) TS[i] = T[i];

        if (in->bridge && ncr > 0) {
            /* lift every interior row: max over crossings of the plateau minus
             * the ramp. The end rows stay at the cluster height (design 1.5). */
            for (k = 0; k < ncr; k++) {
                const double req = cyu[k] + ROAM_XSEP_LIFT - cbase[k];
                if (req <= 0.0) continue;
                for (i = 1; i < n - 1; i++) {
                    /* a flat top, so the curvature clamp rounds the knees and
                     * not the crossing itself */
                    double dd = fabs(arc[i] - cs[k]) - ROAM_RIB_DECK_FLAT * ROAM_RIB_STEP;
                    double v;
                    if (dd < 0.0) dd = 0.0;
                    v = req - rg * dd;
                    if (v > TS[i] - T[i]) TS[i] = T[i] + v;
                }
            }
        } else if (in->tunnel == 1 && ncr > 0) {
            /* [F1-T10] a bore under the roads in cross[]: every interior row
             * at most (road above - lift) at the crossing, rising at the ramp
             * grade away from a flat floor. The end rows are the caller's
             * lowered pins (the chain through the JOIN nodes, td5_roam_tile.c). */
            for (k = 0; k < ncr; k++) {
                const double top = cyu[k] - ROAM_XSEP_LIFT;
                for (i = 1; i < n - 1; i++) {
                    double dd = fabs(arc[i] - cs[k]) - ROAM_RIB_DECK_FLAT * ROAM_RIB_STEP;
                    if (dd < 0.0) dd = 0.0;
                    if (top + rg * dd < TS[i]) TS[i] = top + rg * dd;
                }
            }
        } else if (in->tunnel == 1) {
            for (i = 1; i < n - 1; i++) {
                double d = arc[i] < s_tot - arc[i] ? arc[i] : s_tot - arc[i];
                d *= rg;
                if (d > ROAM_XSEP_LIFT) d = ROAM_XSEP_LIFT;
                TS[i] = T[i] - d;
            }
        }

        for (c = 0; c < nch; c++) {
            const int c0 = cb_ptr[c], c1 = cb_ptr[c + 1];
            int rep = 0;
            if (!rib_solve_chunk(Y + c0, TS + c0, c1 - c0 + 1, capv + c0, curvv + c0,
                                 capw + c0, tw + c0, mw + c0, &rep))
                miss = 1;
            if (rep & 1) fl |= ROAM_RIBF_NOCURV;
            if (rep & 2) fl |= ROAM_RIBF_REPAIR;
        }

        out->clear_n = ncr;
        for (k = 0; k < ncr; k++) {
            const double ys = Y[csg[k]] + cu[k] * (Y[csg[k] + 1] - Y[csg[k]]);
            const double cl = in->bridge ? ys - cyu[k] : cyu[k] - ys;
            if (k == 0 || cl < out->clear_min) out->clear_min = cl;
            if (cl < ROAM_XSEP_LIFT - 1.0) clear_ok = 0;
        }
        if (miss) fl |= ROAM_RIBF_PIN_MISS;
        if (clear_ok) break;
        if (attempt == 0) fl |= ROAM_RIBF_RAMP_SHORT;
        else              fl |= ROAM_RIBF_RAMP_FAIL;
    }

    /* 5b. [F1-T10] height coupling to the leaders (RoamEdgeIn.lead) */
    HW = (double *)malloc((size_t)n * sizeof(double));
    if (!HW) goto done;
    for (i = 0; i < n; i++) HW[i] = (double)lanes * lane_w * 0.5;
    if (((in->nlead > 0 && in->lead) || (in->nltri > 0 && in->ltri)) && !in->bridge && in->tunnel != 1) {
        TILT = (double *)calloc((size_t)n, sizeof(double));
        if (!TILT) goto done;
        if (!rib_couple(in, rx, rz, arc, seg, HW, n, has_pa, has_pb, Y, TILT, out)) goto done;
    }

    /* 5c. [F1-T10] no folded lane quad */
    {
        const int n0 = n;
        n = rib_untwist(rx, rz, Y, HW, TILT, G, TS, n, lanes, lane_w, has_rf, has_rl,
                        (const int32_t (*)[2])rowf, (const int32_t (*)[2])rowl, &fl);
        if (n != n0) {
            nq = n - 1;
            for (i = 1; i < n; i++) seg[i] = hypot(rx[i] - rx[i - 1], rz[i] - rz[i - 1]);
        }
    }

    /* 6. quantise once, emit */
    {
        const int L = lanes;
        const int nblock = (nq + ROAM_RIB_BLOCK - 1) / ROAM_RIB_BLOCK;
        const int per_row = L + 1;
        int *Xc, *Yc, *Zc;
        double *lxv, *lzv;
        int nvert = 0;

        nvert = (nq + nblock) * per_row;   /* every block repeats its closing row */
        if (nvert > 65535) { st = ROAM_RIB_BAD_INPUT; goto done; }
        out->span  = (TD5_StripSpan *)calloc((size_t)nq, sizeof(TD5_StripSpan));
        out->vert  = (TD5_StripVertex *)calloc((size_t)nvert, sizeof(TD5_StripVertex));
        out->meta  = (RoamQuadMeta *)calloc((size_t)nq, sizeof(RoamQuadMeta));
        Xc = (int *)malloc((size_t)n * sizeof(int));
        Yc = (int *)malloc((size_t)n * sizeof(int));
        Zc = (int *)malloc((size_t)n * sizeof(int));
        lxv = (double *)malloc((size_t)n * sizeof(double));
        lzv = (double *)malloc((size_t)n * sizeof(double));
        if (!out->span || !out->vert || !out->meta || !Xc || !Yc || !Zc || !lxv || !lzv) {
            free(Xc); free(Yc); free(Zc); free(lxv); free(lzv);
            goto done;
        }
        for (i = 0; i < n; i++) {
            const int a = i > 0 ? i - 1 : 0, bb = i < n - 1 ? i + 1 : n - 1;
            double dx = rx[bb] - rx[a], dz = rz[bb] - rz[a];
            const double l = hypot(dx, dz);
            if (l > 1e-9) { dx /= l; dz /= l; }
            else if (i > 0) { dx = -lzv[i - 1]; dz = lxv[i - 1]; }  /* keep the last direction */
            else { dx = 0.0; dz = 1.0; }
            /* the strip's row lateral: lx = tz, lz = -tx */
            lxv[i] = dz; lzv[i] = -dx;
            Xc[i] = roam_round(rx[i]);
            Yc[i] = roam_round(Y[i]);
            Zc[i] = roam_round(rz[i]);
        }
        {
            int vbase = 0, k;
            for (b = 0; b < nblock; b++) {
                const int r0 = b * ROAM_RIB_BLOCK;
                const int ns = (r0 + ROAM_RIB_BLOCK <= nq) ? ROAM_RIB_BLOCK : nq - r0;
                const int ox0 = Xc[r0], oy0 = Yc[r0], oz0 = Zc[r0];
                int j;
                for (k = 0; k <= ns; k++) {
                    const int row = r0 + k;
                    for (j = 0; j < per_row; j++) {
                        const double t = ((double)L * 0.5 - (double)j) * lane_w;
                        int vx = roam_round(rx[row] + lxv[row] * t) - ox0;
                        const int vy = (TILT ? roam_round(Y[row] + TILT[row] * t) : Yc[row]) - oy0;
                        int vz = roam_round(rz[row] + lzv[row] * t) - oz0;
                        if (row == 0 && has_rf) { vx = rowf[j][0] - ox0; vz = rowf[j][1] - oz0; }
                        if (row == n - 1 && has_rl) { vx = rowl[j][0] - ox0; vz = rowl[j][1] - oz0; }
                        TD5_StripVertex *v = &out->vert[vbase + k * per_row + j];
                        if (vx < -32768 || vx > 32767 || vy < -32768 || vy > 32767 ||
                            vz < -32768 || vz > 32767) fl |= ROAM_RIBF_I16;
                        v->x = (int16_t)vx; v->y = (int16_t)vy; v->z = (int16_t)vz;
                    }
                }
                for (k = 0; k < ns; k++) {
                    TD5_StripSpan *sp = &out->span[r0 + k];
                    sp->span_type         = 1;
                    sp->surface_attribute = (uint8_t)(0x10 | (rib_grip(in->surface) & 0x0F));
                    sp->pad_02[0]         = 0;
                    sp->pad_02[1]         = (uint8_t)(L & 0x0F);
                    sp->left_vertex_index  = (uint16_t)(vbase + k * per_row);
                    sp->right_vertex_index = (uint16_t)(vbase + (k + 1) * per_row);
                    sp->link_next = (int16_t)((r0 + k + 1 < nq) ? r0 + k + 1 : -1);
                    sp->link_prev = (int16_t)((r0 + k > 0) ? r0 + k - 1 : -1);
                    sp->origin_x = ox0; sp->origin_y = oy0; sp->origin_z = oz0;
                    out->meta[r0 + k].mx = roam_round(0.5 * (rx[r0 + k] + rx[r0 + k + 1]));
                    out->meta[r0 + k].mz = roam_round(0.5 * (rz[r0 + k] + rz[r0 + k + 1]));
                }
                vbase += (ns + 1) * per_row;
            }
            out->nvert = vbase;
        }
        free(Xc); free(Yc); free(Zc); free(lxv); free(lzv);
    }

    /* keep the profile arrays for the census and the plots */
    {
        double mg = 0.0;
        for (i = 0; i < nq; i++) {
            const double g = fabs(Y[i + 1] - Y[i]) / (seg[i + 1] > 1e-9 ? seg[i + 1] : 1e-9);
            if (g > mg) mg = g;
        }
        out->max_grade = mg;
    }
    out->y_first = Y[0];       out->y_last = Y[n - 1];
    out->yq_first = roam_round(Y[0]); out->yq_last = roam_round(Y[n - 1]);
    out->nrows = n;
    out->nquad = nq;
    out->nchunks = nch;
    out->flags = fl;
    out->cx = rx; out->cz = rz; out->ground = G; out->target = TS; out->y = Y; out->hw = HW; out->tilt = TILT;
    rx = rz = G = TS = Y = HW = TILT = NULL;
    rs.x = rs.z = NULL;
    st = ROAM_RIB_OK;

done:
    out->status = st;
    if (st != ROAM_RIB_OK) {
        free(out->span); free(out->vert); free(out->meta);
        out->span = NULL; out->vert = NULL; out->meta = NULL;
        out->nrows = out->nquad = out->nvert = 0;
    }
    free(ox); free(oz); free(cum);
    free(trim.x); free(trim.z);
    free(rs.x); free(rs.z);
    free(seg); free(S); free(T); free(capv); free(curvv); free(wetv);
    free(capw); free(tw); free(mw);
    free(cs); free(cbase); free(cyu); free(cu); free(csg); free(arc); free(chunk_big);
    free(G); free(TS); free(Y); free(HW); free(TILT);
    return st;
}

/* ---------------------------------------------------------- queries --------- */

int roam_ribbon_y_at(const RoamRibbon *r, double x, double z, double *out_y,
                     double *out_dist)
{
    int sg;
    double u, d2;
    if (!r || r->nrows < 2 || !r->cx) return 0;
    rib_nearest(r->cx, r->cz, r->nrows, x, z, &sg, &u, &d2);
    if (out_y) *out_y = r->y[sg] + u * (r->y[sg + 1] - r->y[sg]);
    if (out_dist) *out_dist = sqrt(d2);
    return 1;
}

int roam_ribbon_row_world(const RoamRibbon *r, int row, int32_t (*out)[3])
{
    int b, j, k, per_row;
    const TD5_StripSpan *sp;
    if (!r || !r->span || row < 0 || row >= r->nrows) return 0;
    per_row = r->lanes + 1;
    b = row / ROAM_RIB_BLOCK;
    if (b * ROAM_RIB_BLOCK >= r->nquad) b = (r->nquad - 1) / ROAM_RIB_BLOCK;
    k = row - b * ROAM_RIB_BLOCK;
    sp = &r->span[b * ROAM_RIB_BLOCK];
    for (j = 0; j < per_row; j++) {
        const TD5_StripVertex *v = &r->vert[b * (ROAM_RIB_BLOCK + 1) * per_row + k * per_row + j];
        out[j][0] = sp->origin_x + v->x;
        out[j][1] = sp->origin_y + v->y;
        out[j][2] = sp->origin_z + v->z;
    }
    return per_row;
}

/* ======================================================================== *
 * SECTION: dev test (TD5RE_ROAM_RIBBON_TEST=1), compiled out of RELEASE
 * ======================================================================== */

#ifndef TD5RE_RELEASE

/* F0a's casco polygon (same four corners as td5_roam_cache.c k_casco_poly). */
static const double k_tpoly[4][2] = {
    { 1078705.99,        5374175.73 },
    { 2582491.695195345, 7019045.535332896 },
    { 4255470.01,        5615132.29 },
    { 2731713.44,        3880755.48 },
};

static int t_pip(double x, double z)
{
    int i, j = 3, inside = 0;
    for (i = 0; i < 4; i++) {
        const double xi = k_tpoly[i][0], zi = k_tpoly[i][1];
        const double xj = k_tpoly[j][0], zj = k_tpoly[j][1];
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi)
            inside = !inside;
        j = i;
    }
    return inside;
}

typedef struct { int64_t key; int arms; } TNode;
typedef struct {
    int      way, i0, i1;     /* way index in the session table, point range   */
    uint64_t key;
    int64_t  na, nb;
    int      off;             /* start in the polyline arrays                  */
    double   minx, minz, maxx, maxz;
    int      layer, bridge;
} TPiece;
typedef struct {
    TPiece  *pc; int np;
    double  *px, *pz;
    TNode   *nodes; int nn;
    RoamCross **cross; int *ncross;
} TCtx;

typedef struct {
    int      edges, absorbed, bad, height_fail;
    long     quads, rows, verts;
    double   max_grade;
    int      grade_over, bridges, bores, ramp_short, ramp_fail, pin_miss, i16, chunked,
             loops, wet_rows, crossings, bridges_with_cross, nocurv, repair, over_absmax;
    uint32_t ms;
} TStats;

static int t_cmp_node(const void *a, const void *b)
{
    const int64_t x = ((const TNode *)a)->key, y = ((const TNode *)b)->key;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int t_cmp_piece(const void *a, const void *b)
{
    const uint64_t x = ((const TPiece *)a)->key, y = ((const TPiece *)b)->key;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int t_arms(const TCtx *c, int64_t key)
{
    int lo = 0, hi = c->nn - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (c->nodes[mid].key == key) return c->nodes[mid].arms;
        if (c->nodes[mid].key < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static double t_pin(const TCtx *c, TD5_RoamHeight *h, int64_t key)
{
    /* The cluster height of a node: ground at the node's snapped centre. Arms == 1
     * (a dead end) has no cluster, its end stays free. */
    double g;
    int w = 0, pf = 0;
    double x, z, box[4];
    if (t_arms(c, key) < 2) return NAN;
    x = (double)(int32_t)(key >> 32) * 10.0;
    z = (double)(int32_t)(uint32_t)key * 10.0;
    box[0] = x - 3000.0; box[1] = z - 3000.0; box[2] = x + 3000.0; box[3] = z + 3000.0;
    if (!rib_ground(h, x, z, box, &pf, &g, &w)) return NAN;
    return g;
}

static uint64_t t_fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static uint64_t t_hash_ribbon(const RoamRibbon *r)
{
    uint64_t h = 1469598103934665603ULL;
    const int32_t hd[6] = { r->status, (int32_t)r->flags, r->lanes, r->nrows,
                            r->nquad, r->nvert };
    h = t_fnv(h, &r->key, sizeof r->key);
    h = t_fnv(h, hd, sizeof hd);
    if (r->span) h = t_fnv(h, r->span, (size_t)r->nquad * sizeof(TD5_StripSpan));
    if (r->vert) h = t_fnv(h, r->vert, (size_t)r->nvert * sizeof(TD5_StripVertex));
    if (r->meta) h = t_fnv(h, r->meta, (size_t)r->nquad * sizeof(RoamQuadMeta));
    return h;
}

/* Build every piece, in index order or reversed, from the stored or the
 * reversed input. Fills hv[] (one hash per piece), keeps the ribbons when
 * `keep` is non-NULL, accumulates the census. */
static void t_run(const TCtx *c, const TD5_RoamWay *const *ways, TD5_RoamHeight *h,
                  int reverse_order, int flip, int use_cross, const RoamRibbon *rows_from,
                  RoamRibbon *keep, uint64_t *hv, TStats *st)
{
    int32_t rq_a[ROAM_RIB_MAX_LANES + 1][2], rq_b[ROAM_RIB_MAX_LANES + 1][2];
    double *fx = NULL, *fz = NULL;
    int ii, maxn = 0;
    const uint32_t t0 = td5_plat_time_ms();
    memset(st, 0, sizeof *st);
    for (ii = 0; ii < c->np; ii++) {
        const int n = c->pc[ii].i1 - c->pc[ii].i0 + 1;
        if (n > maxn) maxn = n;
    }
    fx = (double *)malloc((size_t)maxn * sizeof(double));
    fz = (double *)malloc((size_t)maxn * sizeof(double));
    if (!fx || !fz) { free(fx); free(fz); return; }
    for (ii = 0; ii < c->np; ii++) {
        const int idx = reverse_order ? c->np - 1 - ii : ii;
        const TPiece *p = &c->pc[idx];
        const TD5_RoamWay *w = ways[p->way];
        const int n = p->i1 - p->i0 + 1;
        RoamEdgeIn e;
        RoamRibbon tmp, *r = keep ? &keep[idx] : &tmp;
        int k;

        memset(&e, 0, sizeof e);
        e.key = p->key;
        e.n = n;
        e.lanes = w->road.lanes;
        e.surface = w->road.surface;
        e.layer = w->road.layer;
        e.bridge = p->bridge;
        /* The cache keeps tunnel as a bool; the KIND (yes vs building_passage) sits in
         * tags{} and is not read by T1. Stand-in: a tunnel at layer < 0 that is at
         * least 40 m long is a bore, anything shorter is a building passage. */
        e.tunnel = 0;
        if (w->road.tunnel) {
            double len = 0.0;
            for (k = 1; k < n; k++)
                len += hypot(c->px[p->off + k] - c->px[p->off + k - 1],
                             c->pz[p->off + k] - c->pz[p->off + k - 1]);
            e.tunnel = (w->road.layer < 0 && len >= 40.0 * 430.0) ? 1 : 2;
        }
        e.cut_a = e.cut_b = 0.0;                 /* ADAPTER POINT: T3 cut rows */
        e.pin_a = t_pin(c, h, p->na);
        e.pin_b = t_pin(c, h, p->nb);
        if (use_cross && c->cross[idx]) { e.cross = c->cross[idx]; e.ncross = c->ncross[idx]; }
        if (flip) {
            for (k = 0; k < n; k++) {
                fx[k] = c->px[p->off + n - 1 - k];
                fz[k] = c->pz[p->off + n - 1 - k];
            }
            e.px = fx; e.pz = fz;
            e.node_a = p->nb; e.node_b = p->na;
            { const double t = e.cut_a; e.cut_a = e.cut_b; e.cut_b = t; }
            { const double t = e.pin_a; e.pin_a = e.pin_b; e.pin_b = t; }
        } else {
            e.px = c->px + p->off; e.pz = c->pz + p->off;
            e.node_a = p->na; e.node_b = p->nb;
        }
        if (rows_from && rows_from[idx].status == ROAM_RIB_OK) {
            /* T3 adapter rehearsal: hand the builder its own end rows back, in the
             * input frame, as T3's rowq would. The result must not change. */
            int32_t r0[ROAM_RIB_MAX_LANES + 1][3], rl[ROAM_RIB_MAX_LANES + 1][3];
            const int L = rows_from[idx].lanes;
            const int rv = rib_is_reversed(&e);
            int j;
            roam_ribbon_row_world(&rows_from[idx], 0, r0);
            roam_ribbon_row_world(&rows_from[idx], rows_from[idx].nrows - 1, rl);
            for (j = 0; j <= L; j++) {
                rq_a[j][0] = rv ? rl[L - j][0] : r0[j][0];
                rq_a[j][1] = rv ? rl[L - j][2] : r0[j][2];
                rq_b[j][0] = rv ? r0[L - j][0] : rl[j][0];
                rq_b[j][1] = rv ? r0[L - j][2] : rl[j][2];
            }
            e.rowq_a = rq_a; e.rowq_b = rq_b;
        }
        roam_ribbon_build(&e, h, r);
        hv[idx] = t_hash_ribbon(r);
        st->edges++;
        if (r->status == ROAM_RIB_ABSORBED) st->absorbed++;
        else if (r->status == ROAM_RIB_HEIGHT) st->height_fail++;
        else if (r->status != ROAM_RIB_OK) st->bad++;
        else {
            st->quads += r->nquad; st->rows += r->nrows; st->verts += r->nvert;
            if (r->max_grade > st->max_grade) st->max_grade = r->max_grade;
            if (r->max_grade > ROAM_GRADE_CAP * 1.001) st->grade_over++;
            if (r->flags & ROAM_RIBF_BRIDGE) st->bridges++;
            if (r->flags & ROAM_RIBF_BORE) st->bores++;
            if (r->flags & ROAM_RIBF_RAMP_SHORT) st->ramp_short++;
            if (r->flags & ROAM_RIBF_RAMP_FAIL) st->ramp_fail++;
            if (r->flags & ROAM_RIBF_PIN_MISS) st->pin_miss++;
            if (r->flags & ROAM_RIBF_NOCURV) st->nocurv++;
            if (r->flags & ROAM_RIBF_REPAIR) st->repair++;
            if (r->max_grade > ROAM_GRADE_ABSMAX * 1.001) st->over_absmax++;
            if (r->flags & ROAM_RIBF_I16) st->i16++;
            if (r->flags & ROAM_RIBF_CHUNKED) st->chunked++;
            if (r->flags & ROAM_RIBF_LOOP) st->loops++;
            st->wet_rows += r->wet_rows;
        }
        if (!keep) roam_ribbon_free(r);
    }
    free(fx); free(fz);
    st->ms = td5_plat_time_ms() - t0;
}

static uint64_t t_combine(const uint64_t *hv, int n)
{
    uint64_t h = 1469598103934665603ULL;
    int i;
    for (i = 0; i < n; i++) h = t_fnv(h, &hv[i], sizeof hv[i]);
    return h;
}

/* Segment (a,b) x (c,d) intersection point; returns 1 and the point. */
static int t_seg_x(double ax, double az, double bx, double bz,
                   double cx, double cz, double dx, double dz,
                   double *ox, double *oz)
{
    const double r_x = bx - ax, r_z = bz - az, s_x = dx - cx, s_z = dz - cz;
    const double den = r_x * s_z - r_z * s_x;
    double t, u;
    if (fabs(den) < 1e-9) return 0;
    t = ((cx - ax) * s_z - (cz - az) * s_x) / den;
    u = ((cx - ax) * r_z - (cz - az) * r_x) / den;
    if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) return 0;
    *ox = ax + t * r_x; *oz = az + t * r_z;
    return 1;
}

static void t_dump_profile(const RoamRibbon *r, double upm, const char *path,
                           const char *why)
{
    FILE *f = fopen(path, "w");
    double s = 0.0;
    int i;
    if (!f || !r || !r->y) { if (f) fclose(f); return; }
    fprintf(f, "# key=%llu why=%s rows=%d lanes=%d flags=0x%02x length_m=%.1f max_grade=%.4f\n",
            (unsigned long long)r->key, why, r->nrows, r->lanes, r->flags,
            r->length / upm, r->max_grade);
    fprintf(f, "row,s_m,ground_m,target_m,y_m\n");
    for (i = 0; i < r->nrows; i++) {
        if (i > 0) s += hypot(r->cx[i] - r->cx[i - 1], r->cz[i] - r->cz[i - 1]);
        fprintf(f, "%d,%.3f,%.4f,%.4f,%.4f\n", i, s / upm, r->ground[i] / upm,
                r->target[i] / upm, r->y[i] / upm);
    }
    fclose(f);
}

int td5_roam_ribbon_dev_test(void)
{
    static const char slug[] = "la_plata_partido";
    TD5_RoamCacheCensus cc;
    TCtx c;
    TD5_RoamHeight h;
    TStats s1, s1r, s1f, sf, sfr, sff, sfo, sfoi;
    uint64_t *hv = NULL, *hv2 = NULL;
    RoamRibbon *ribs = NULL, *ribs2 = NULL;
    const TD5_RoamWay **ways = NULL;
    char sh[400], sw[400], line[1024], path[400], pj[400];
    const char *dir;
    double win[4], upm;
    int nway, i, j, k, ntouch = 0, ncross_total = 0;
    int *sel = NULL;
    size_t npts_total = 0;
    int ex100;
    uint64_t H1, H1r, H1f, Hf, Hfr, Hff, Hfo, Hfoi;
    TNode *tmp = NULL;
    int ntmp = 0, np_alloc = 0;
    char synth[3][1024] = { "", "", "" };
    FILE *fp;

    if (!td5_env_flag_off("TD5RE_ROAM_RIBBON_TEST")) return 0;
    memset(&c, 0, sizeof c);
    memset(&h, 0, sizeof h);
    dir = td5_plat_log_dir();

    win[0] = win[2] = k_tpoly[0][0]; win[1] = win[3] = k_tpoly[0][1];
    for (i = 1; i < 4; i++) {
        if (k_tpoly[i][0] < win[0]) win[0] = k_tpoly[i][0];
        if (k_tpoly[i][1] < win[1]) win[1] = k_tpoly[i][1];
        if (k_tpoly[i][0] > win[2]) win[2] = k_tpoly[i][0];
        if (k_tpoly[i][1] > win[3]) win[3] = k_tpoly[i][1];
    }
    if (td5_roam_cache_session_begin(slug) <= 0) {
        TD5_LOG_E(LOG_TAG, "roam ribbon test: no ROADS tiles for %s", slug);
        return 1;
    }
    td5_roam_cache_load_window(win, 0.0, &cc);
    upm = td5_geo_roads_units_per_metre(slug);
    if (upm <= 0.0) upm = 430.0;
    nway = td5_roam_cache_way_count();

    /* the ways that touch the polygon (F0a's rule: a vertex or a segment midpoint) */
    sel = (int *)calloc((size_t)nway + 1u, sizeof(int));
    ways = (const TD5_RoamWay **)calloc((size_t)nway + 1u, sizeof(*ways));
    if (!sel || !ways) goto fail;
    for (i = 0; i < nway; i++) {
        const TD5_RoamWay *w = td5_roam_cache_way(i);
        double x, z, x2, z2;
        ways[i] = w;
        for (k = 0; k < w->road.count && !sel[i]; k++) {
            td5_roam_cache_point(w, k, &x, &z, NULL);
            if (t_pip(x, z)) sel[i] = 1;
            else if (k + 1 < w->road.count) {
                td5_roam_cache_point(w, k + 1, &x2, &z2, NULL);
                if (t_pip(0.5 * (x + x2), 0.5 * (z + z2))) sel[i] = 1;
            }
        }
        if (sel[i]) { ntouch++; npts_total += (size_t)w->road.count; }
    }

    /* node arms over the selected ways: an interior vertex is 2 arms, an end 1 */
    tmp = (TNode *)malloc((npts_total + 1u) * sizeof(TNode));
    if (!tmp) goto fail;
    for (i = 0; i < nway; i++) {
        const TD5_RoamWay *w = ways[i];
        if (!sel[i]) continue;
        for (k = 0; k < w->road.count; k++) {
            int64_t nk;
            td5_roam_cache_point(w, k, NULL, NULL, &nk);
            tmp[ntmp].key = nk;
            tmp[ntmp].arms = (k == 0 || k == w->road.count - 1) ? 1 : 2;
            ntmp++;
        }
    }
    qsort(tmp, (size_t)ntmp, sizeof(TNode), t_cmp_node);
    c.nodes = (TNode *)malloc((size_t)ntmp * sizeof(TNode) + 1u);
    if (!c.nodes) goto fail;
    for (i = 0; i < ntmp; i++) {
        if (c.nn > 0 && c.nodes[c.nn - 1].key == tmp[i].key) c.nodes[c.nn - 1].arms += tmp[i].arms;
        else c.nodes[c.nn++] = tmp[i];
    }

    /* edge pieces: a way is cut at its ends and at every vertex with 3+ arms */
    np_alloc = ntouch * 4 + 16;
    c.pc = (TPiece *)malloc((size_t)np_alloc * sizeof(TPiece));
    c.px = (double *)malloc((npts_total * 2u + 16u) * sizeof(double));
    c.pz = (double *)malloc((npts_total * 2u + 16u) * sizeof(double));
    if (!c.pc || !c.px || !c.pz) goto fail;
    {
        int off = 0;
        for (i = 0; i < nway; i++) {
            const TD5_RoamWay *w = ways[i];
            int start = 0, ord = 0;
            if (!sel[i]) continue;
            for (k = 1; k < w->road.count; k++) {
                int64_t nk;
                int brk = (k == w->road.count - 1);
                td5_roam_cache_point(w, k, NULL, NULL, &nk);
                if (!brk && t_arms(&c, nk) >= 3) brk = 1;
                if (!brk) continue;
                if (c.np >= np_alloc) {
                    TPiece *np2;
                    np_alloc *= 2;
                    np2 = (TPiece *)realloc(c.pc, (size_t)np_alloc * sizeof(TPiece));
                    if (!np2) goto fail;
                    c.pc = np2;
                }
                {
                    TPiece *p = &c.pc[c.np++];
                    int64_t ka, kb;
                    td5_roam_cache_point(w, start, NULL, NULL, &ka);
                    kb = nk;
                    memset(p, 0, sizeof *p);
                    p->way = i; p->i0 = start; p->i1 = k;
                    p->key = ((uint64_t)(w->way_id < 0 ? 0 : w->way_id) << 16) | (uint64_t)ord++;
                    p->na = ka; p->nb = kb;
                    p->off = off;
                    p->layer = w->road.layer;
                    p->bridge = (w->road.bridge && w->road.layer >= 1) ? 1 : 0;
                    p->minx = p->minz = 1e300; p->maxx = p->maxz = -1e300;
                    for (j = start; j <= k; j++) {
                        double x, z;
                        td5_roam_cache_point(w, j, &x, &z, NULL);
                        c.px[off] = x; c.pz[off] = z; off++;
                        if (x < p->minx) p->minx = x;
                        if (x > p->maxx) p->maxx = x;
                        if (z < p->minz) p->minz = z;
                        if (z > p->maxz) p->maxz = z;
                    }
                }
                start = k;
            }
        }
    }
    qsort(c.pc, (size_t)c.np, sizeof(TPiece), t_cmp_piece);

    /* heights: the SOURCE rasters, one window over the casco + 2 km */
    td5_geo_source_path(sh, sizeof sh, slug, "HEIGHT.R16");
    td5_geo_source_path(sw, sizeof sw, slug, "WATER.R8");
    ex100 = td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400);
    if (!td5_roam_height_open(&h, sh, sw, (double)ex100 / 100.0)) {
        TD5_LOG_E(LOG_TAG, "roam ribbon test: HEIGHT.R16 would not open (%s)", sh);
        goto fail;
    }
    td5_roam_height_prefetch(&h, win[0] - 860000.0, win[1] - 860000.0,
                             win[2] + 860000.0, win[3] + 860000.0);

    hv   = (uint64_t *)calloc((size_t)c.np + 1u, sizeof(uint64_t));
    hv2  = (uint64_t *)calloc((size_t)c.np + 1u, sizeof(uint64_t));
    ribs = (RoamRibbon *)calloc((size_t)c.np + 1u, sizeof(RoamRibbon));
    ribs2 = (RoamRibbon *)calloc((size_t)c.np + 1u, sizeof(RoamRibbon));
    c.cross  = (RoamCross **)calloc((size_t)c.np + 1u, sizeof(RoamCross *));
    c.ncross = (int *)calloc((size_t)c.np + 1u, sizeof(int));
    if (!hv || !hv2 || !ribs || !ribs2 || !c.cross || !c.ncross) goto fail;

    /* PASS 1: no crossings. Forward, reversed order, reversed input. */
    t_run(&c, ways, &h, 0, 0, 0, NULL, ribs, hv, &s1);
    H1 = t_combine(hv, c.np);
    t_run(&c, ways, &h, 1, 0, 0, NULL, NULL, hv2, &s1r);
    H1r = t_combine(hv2, c.np);
    t_run(&c, ways, &h, 0, 1, 0, NULL, NULL, hv2, &s1f);
    H1f = t_combine(hv2, c.np);

    /* crossings: a bridge over a lower-layer piece of ANOTHER way, away from the
     * ends (a shared node is a connection, not a crossing). y_under = pass 1. */
    for (i = 0; i < c.np; i++) {
        const TPiece *p = &c.pc[i];
        const int pn = p->i1 - p->i0 + 1;
        RoamCross *list = NULL;
        int nl = 0, cap = 0;
        if (!p->bridge) continue;
        for (j = 0; j < c.np; j++) {
            const TPiece *q = &c.pc[j];
            const int qn = q->i1 - q->i0 + 1;
            int a, b2;
            if (j == i || q->way == p->way || q->layer >= p->layer) continue;
            if (q->maxx < p->minx || q->minx > p->maxx || q->maxz < p->minz || q->minz > p->maxz) continue;
            if (ribs[j].status != ROAM_RIB_OK) continue;
            for (a = 0; a + 1 < pn; a++) {
                for (b2 = 0; b2 + 1 < qn; b2++) {
                    double xx, zz, yu, dist;
                    if (!t_seg_x(c.px[p->off + a], c.pz[p->off + a], c.px[p->off + a + 1], c.pz[p->off + a + 1],
                                 c.px[q->off + b2], c.pz[q->off + b2], c.px[q->off + b2 + 1], c.pz[q->off + b2 + 1],
                                 &xx, &zz)) continue;
                    /* ends: 60 units = 14 cm; a node-shared connection is at the node */
                    if (hypot(xx - c.px[p->off], zz - c.pz[p->off]) < 60.0 ||
                        hypot(xx - c.px[p->off + pn - 1], zz - c.pz[p->off + pn - 1]) < 60.0 ||
                        hypot(xx - c.px[q->off], zz - c.pz[q->off]) < 60.0 ||
                        hypot(xx - c.px[q->off + qn - 1], zz - c.pz[q->off + qn - 1]) < 60.0) continue;
                    if (!roam_ribbon_y_at(&ribs[j], xx, zz, &yu, &dist)) continue;
                    if (nl >= cap) {
                        RoamCross *nlst;
                        cap = cap ? cap * 2 : 4;
                        nlst = (RoamCross *)realloc(list, (size_t)cap * sizeof(RoamCross));
                        if (!nlst) { cap = nl; continue; }
                        list = nlst;
                    }
                    list[nl].x = xx; list[nl].z = zz; list[nl].y_under = yu;
                    nl++;
                }
            }
        }
        c.cross[i] = list; c.ncross[i] = nl;
        ncross_total += nl;
    }

    /* FINAL pass: crossings in. Forward (kept), reversed order, reversed input. */
    for (i = 0; i < c.np; i++) roam_ribbon_free(&ribs2[i]);
    t_run(&c, ways, &h, 0, 0, 1, NULL, ribs2, hv, &sf);
    Hf = t_combine(hv, c.np);
    t_run(&c, ways, &h, 1, 0, 1, NULL, NULL, hv2, &sfr);
    Hfr = t_combine(hv2, c.np);
    t_run(&c, ways, &h, 0, 1, 1, NULL, NULL, hv2, &sff);
    Hff = t_combine(hv2, c.np);
    /* T3 adapter rehearsal: the end rows handed back (forward, and reversed input) */
    t_run(&c, ways, &h, 0, 0, 1, ribs2, NULL, hv2, &sfo);
    Hfo = t_combine(hv2, c.np);
    t_run(&c, ways, &h, 0, 1, 1, ribs2, NULL, hv2, &sfoi);
    Hfoi = t_combine(hv2, c.np);
    for (i = 0; i < c.np; i++) if (c.ncross[i] > 0) sf.bridges_with_cross++;
    sf.crossings = ncross_total;

    snprintf(line, sizeof line,
             "roam ribbon test: casco polygon: %d way(s) -> %d edge(s), %ld quad(s), %ld row(s), "
             "%ld vert(s) (%d absorbed, %d bad, %d height-fail); max grade %.4f (cap %.3f), "
             "%d edge(s) over cap (pins further apart than the cap), %d over %.2f; bridges %d "
             "(%d with %d crossing(s)), bores %d, RAMP_SHORT %d, RAMP_FAIL %d, pin-miss %d, "
             "no-curv %d, repair %d, i16 %d, chunked %d, loops %d, wet rows %d; "
             "build %u ms (pass 1 %u ms)",
             ntouch, sf.edges, sf.quads, sf.rows, sf.verts, sf.absorbed, sf.bad, sf.height_fail,
             sf.max_grade, ROAM_GRADE_CAP, sf.grade_over, sf.over_absmax, ROAM_GRADE_ABSMAX,
             sf.bridges, sf.bridges_with_cross, sf.crossings, sf.bores, sf.ramp_short,
             sf.ramp_fail, sf.pin_miss, sf.nocurv, sf.repair, sf.i16, sf.chunked, sf.loops,
             sf.wet_rows, (unsigned)sf.ms, (unsigned)s1.ms);
    TD5_LOG_I(LOG_TAG, "%s", line);
    printf("%s\n", line);
    snprintf(path, sizeof path, "%sroam_ribbon_census.txt", dir ? dir : "");
    fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%s\n", line);
        {
            /* what the tags say: ways, then pieces */
            int wb = 0, wt = 0, wl_pos = 0, wl_neg = 0, wb_l0 = 0, wt_l0 = 0, wb_lpos = 0;
            for (i = 0; i < nway; i++) {
                const TD5_RoamWay *w = ways[i];
                if (!sel[i]) continue;
                if (w->road.bridge) { wb++; if (w->road.layer == 0) wb_l0++; if (w->road.layer >= 1) wb_lpos++; }
                if (w->road.tunnel) { wt++; if (w->road.layer == 0) wt_l0++; }
                if (w->road.layer > 0) wl_pos++;
                if (w->road.layer < 0) wl_neg++;
            }
            fprintf(fp, "tags over the %d way(s): bridge=yes %d (layer 0: %d, layer>=1: %d), tunnel %d (layer 0: %d), layer>0 %d, layer<0 %d\n",
                    ntouch, wb, wb_l0, wb_lpos, wt, wt_l0, wl_pos, wl_neg);
        }
        {
            /* the five steepest solved edges, with their length: a short edge
             * between two pinned nodes is the expected offender */
            int top[5] = { -1, -1, -1, -1, -1 }, q, m;
            for (i = 0; i < c.np; i++) {
                const RoamRibbon *r = &ribs2[i];
                if (r->status != ROAM_RIB_OK) continue;
                for (q = 0; q < 5; q++) {
                    if (top[q] < 0 || r->max_grade > ribs2[top[q]].max_grade) {
                        for (m = 4; m > q; m--) top[m] = top[m - 1];
                        top[q] = i;
                        break;
                    }
                }
            }
            for (q = 0; q < 5; q++) {
                const RoamRibbon *r;
                if (top[q] < 0) break;
                r = &ribs2[top[q]];
                fprintf(fp, "worst grade #%d: key %llu rows %d length %.1f m max grade %.4f flags 0x%02x\n",
                        q + 1, (unsigned long long)r->key, r->nrows, r->length / upm,
                        r->max_grade, r->flags);
            }
        }
        fprintf(fp, "determinism pass 1 (no crossings): forward %016llx | reversed order %016llx | reversed input %016llx -> %s\n",
                (unsigned long long)H1, (unsigned long long)H1r, (unsigned long long)H1f,
                (H1 == H1r && H1 == H1f) ? "IDENTICAL" : "DIFFERENT");
        fprintf(fp, "determinism final pass (crossings): forward %016llx | reversed order %016llx | reversed input %016llx -> %s\n",
                (unsigned long long)Hf, (unsigned long long)Hfr, (unsigned long long)Hff,
                (Hf == Hfr && Hf == Hff) ? "IDENTICAL" : "DIFFERENT");
        fprintf(fp, "T3 end-row override (own end rows handed back as rowq): forward %016llx | reversed input %016llx vs plain %016llx -> %s\n",
                (unsigned long long)Hfo, (unsigned long long)Hfoi, (unsigned long long)Hf,
                (Hfo == Hf && Hfoi == Hf) ? "IDENTICAL" : "DIFFERENT");
        fprintf(fp, "ms: pass1 fwd %u rev %u flip %u | final fwd %u rev %u flip %u\n",
                (unsigned)s1.ms, (unsigned)s1r.ms, (unsigned)s1f.ms,
                (unsigned)sf.ms, (unsigned)sfr.ms, (unsigned)sff.ms);
        fclose(fp);
    }
    printf("roam ribbon test: determinism pass 1 %s (%016llx), final %s (%016llx)\n",
           (H1 == H1r && H1 == H1f) ? "IDENTICAL" : "DIFFERENT", (unsigned long long)H1,
           (Hf == Hfr && Hf == Hff) ? "IDENTICAL" : "DIFFERENT", (unsigned long long)Hf);

    /* profile dumps: the longest edge, the steepest raw ground, the biggest lift */
    {
        int bl = -1, bs = -1, bb = -1;
        double sl = 0.0, ss = 0.0, sb = 0.0;
        for (i = 0; i < c.np; i++) {
            const RoamRibbon *r = &ribs2[i];
            double rise = 0.0, lift = 0.0;
            if (r->status != ROAM_RIB_OK || r->nrows < 3) continue;
            if (r->length > sl) { sl = r->length; bl = i; }
            for (k = 0; k + 1 < r->nrows; k++) {
                const double sg = hypot(r->cx[k + 1] - r->cx[k], r->cz[k + 1] - r->cz[k]);
                const double gg = fabs(r->ground[k + 1] - r->ground[k]) / (sg > 1e-9 ? sg : 1e-9);
                if (gg > rise && r->nrows >= 20) rise = gg;
            }
            if (rise > ss) { ss = rise; bs = i; }
            for (k = 0; k < r->nrows; k++) {
                const double d = r->y[k] - r->ground[k];
                if (fabs(d) > lift) lift = fabs(d);
            }
            if ((r->flags & (ROAM_RIBF_BRIDGE | ROAM_RIBF_BORE)) && lift > sb) { sb = lift; bb = i; }
        }
        if (bl >= 0) {
            snprintf(pj, sizeof pj, "%sroam_ribbon_profile_0_longest.csv", dir ? dir : "");
            t_dump_profile(&ribs2[bl], upm, pj, "longest edge");
        }
        if (bs >= 0) {
            snprintf(pj, sizeof pj, "%sroam_ribbon_profile_1_steepest.csv", dir ? dir : "");
            t_dump_profile(&ribs2[bs], upm, pj, "steepest raster");
        }
        if (bb >= 0) {
            snprintf(pj, sizeof pj, "%sroam_ribbon_profile_2_bridge.csv", dir ? dir : "");
            t_dump_profile(&ribs2[bb], upm, pj, "bridge/bore lift");
        }
        printf("roam ribbon test: profiles longest=%d steepest=%d lifted=%d (piece index)\n", bl, bs, bb);

        /* SYNTHETIC BRIDGES over the longest real edge: the casco polygon holds 1
         * bridge and it crosses nothing, so the lift path is exercised here. A
         * 200 m deck must ramp inside itself; a 30 m deck cannot (RAMP_SHORT). */
        if (bl >= 0 && ribs2[bl].nrows >= 6) {
            const RoamRibbon *u = &ribs2[bl];
            const int m = u->nrows / 2;
            const double xc = u->cx[m], zc = u->cz[m];
            double dx = u->cx[m + 1] - u->cx[m - 1], dz = u->cz[m + 1] - u->cz[m - 1];
            const double dl = hypot(dx, dz);
            double yu = 0.0, dist = 0.0;
            int variant;
            if (dl > 1e-9 && roam_ribbon_y_at(u, xc, zc, &yu, &dist)) {
                const double px_ = -dz / dl, pz_ = dx / dl;
                for (variant = 0; variant < 2; variant++) {
                    const double half = (variant ? 15.0 : 100.0) * upm;
                    double bx[3], bz[3], yd = 0.0, gd = 0.0;
                    RoamCross xs;
                    RoamEdgeIn e;
                    RoamRibbon br;
                    bx[0] = xc - px_ * half; bz[0] = zc - pz_ * half;
                    bx[1] = xc;              bz[1] = zc;
                    bx[2] = xc + px_ * half; bz[2] = zc + pz_ * half;
                    xs.x = xc; xs.z = zc; xs.y_under = yu;
                    memset(&e, 0, sizeof e);
                    e.key = 0xB1D6E0ull + (uint64_t)variant;
                    e.px = bx; e.pz = bz; e.n = 3;
                    e.node_a = 1; e.node_b = 2;
                    e.lanes = 2; e.layer = 1; e.bridge = 1;
                    e.pin_a = e.pin_b = NAN;
                    e.cross = &xs; e.ncross = 1;
                    roam_ribbon_build(&e, &h, &br);
                    if (br.status == ROAM_RIB_OK) {
                        roam_ribbon_y_at(&br, xc, zc, &yd, &gd);
                        snprintf(line, sizeof line,
                                 "synthetic bridge %s: %.0f m long, %d rows, deck - road at the crossing %.2f m "
                                 "(need %.2f), max grade %.4f, flags 0x%03x%s%s",
                                 variant ? "short" : "long", br.length / upm, br.nrows,
                                 (yd - yu) / upm, ROAM_XSEP_LIFT / upm, br.max_grade, br.flags,
                                 (br.flags & ROAM_RIBF_RAMP_SHORT) ? " RAMP_SHORT" : "",
                                 (br.flags & ROAM_RIBF_RAMP_FAIL) ? " RAMP_FAIL" : "");
                        printf("roam ribbon test: %s\n", line);
                        TD5_LOG_I(LOG_TAG, "%s", line);
                        snprintf(synth[variant], sizeof synth[variant], "%s", line);
                        if (variant == 0) {
                            snprintf(pj, sizeof pj, "%sroam_ribbon_profile_3_synth_bridge.csv", dir ? dir : "");
                            t_dump_profile(&br, upm, pj, "synthetic 200 m bridge over the longest edge");
                        }
                    }
                    roam_ribbon_free(&br);
                }
            }
        }

        /* SYNTHETIC LONG EDGE (3.2 km straight, free ends): the 1 km split. The
         * rows at the chunk joins must be the targets exactly. */
        if (bl >= 0 && ribs2[bl].nrows >= 6) {
            const RoamRibbon *u = &ribs2[bl];
            const double xc = u->cx[u->nrows / 2], zc = u->cz[u->nrows / 2];
            double bx[5], bz[5], jmax = 0.0;
            RoamEdgeIn e;
            RoamRibbon lr;
            int q;
            for (q = 0; q < 5; q++) { bx[q] = xc + ((double)q - 2.0) * 800.0 * upm; bz[q] = zc; }
            memset(&e, 0, sizeof e);
            e.key = 0x10E6ull;
            e.px = bx; e.pz = bz; e.n = 5;
            e.node_a = 1; e.node_b = 2;
            e.lanes = 2;
            e.pin_a = e.pin_b = NAN;
            roam_ribbon_build(&e, &h, &lr);
            if (lr.status == ROAM_RIB_OK) {
                for (q = ROAM_RIB_CHUNK_ROWS; q < lr.nrows - 1; q += ROAM_RIB_CHUNK_ROWS) {
                    const double d = fabs(lr.y[q] - lr.target[q]);
                    if (d > jmax) jmax = d;
                }
                snprintf(line, sizeof line,
                         "synthetic long edge: %.0f m, %d rows, %d chunk(s) (flag %s), max grade %.4f, "
                         "|y - target| at the chunk joins %.6f units, flags 0x%03x",
                         lr.length / upm, lr.nrows, lr.nchunks,
                         (lr.flags & ROAM_RIBF_CHUNKED) ? "set" : "clear", lr.max_grade, jmax, lr.flags);
                printf("roam ribbon test: %s\n", line);
                TD5_LOG_I(LOG_TAG, "%s", line);
                snprintf(synth[2], sizeof synth[2], "%s", line);
            }
            roam_ribbon_free(&lr);
        }
    }

    /* one ribbon's seam rows through the public row accessor (sanity: the first
     * row's centre vertex equals the stored origin + offset) */
    for (i = 0; i < c.np; i++) {
        int32_t row[ROAM_RIB_MAX_LANES + 1][3];
        if (ribs2[i].status != ROAM_RIB_OK) continue;
        if (roam_ribbon_row_world(&ribs2[i], 0, row) == ribs2[i].lanes + 1) {
            printf("roam ribbon test: sample piece %llu row 0 vertex 0 = (%d,%d,%d)\n",
                   (unsigned long long)ribs2[i].key, row[0][0], row[0][1], row[0][2]);
        }
        break;
    }

    fp = fopen(path, "a");
    if (fp) {
        for (i = 0; i < 3; i++) if (synth[i][0]) fprintf(fp, "%s\n", synth[i]);
        fclose(fp);
    }

fail:
    for (i = 0; ribs && i < c.np; i++) roam_ribbon_free(&ribs[i]);
    for (i = 0; ribs2 && i < c.np; i++) roam_ribbon_free(&ribs2[i]);
    for (i = 0; c.cross && i < c.np; i++) free(c.cross[i]);
    free(ribs); free(ribs2); free(hv); free(hv2); free(c.cross); free(c.ncross);
    free(c.pc); free(c.px); free(c.pz); free(c.nodes); free(tmp);
    free(sel); free(ways);
    td5_roam_height_close(&h);
    td5_roam_cache_session_end();
    return 1;
}

#endif /* !TD5RE_RELEASE */
