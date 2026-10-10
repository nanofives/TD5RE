/**
 * td5_tg_plazafork.c -- GEO TRACK: PLAZA FORKS, free corridor round a ring (PORT-ONLY).
 * A drivable corridor round the far side of a ring plaza.
 *
 * [ROUND 1015 E] Mariano, round 1015 item 9 (pick: level091 e53 s14 `road p0:ROAD`,
 * route node 216, 10.9 m beside the route -- the scenery far carriageway of Diagonal 73,
 * which ends at Plaza Miguel de Azcuenaga): "if a road passes through a plaza by default
 * i should be able to drive to both sides of the plaza".
 *
 * WHAT WAS MISSING. A fork corridor is tied 1:1 to the MAIN node it rides: corridor
 * step k sits on node F+1+k at a lateral shift, and the strip rows, the road mesh,
 * the AI heading table, the carriageway authority and the scenery all key off that
 * node. A ring plaza's far side is not a function of the route's nodes (Azcuenaga:
 * a 91 m ring, the other side up to 182 m from the route, against a 65 m lateral
 * cap), so rounds 1013 and 1014 laid it as scenery ribbons (not drivable) and left
 * the design in docs/plans/GEO_REAL_FORKS.md.
 *
 * WHAT THIS DOES. The corridor keeps the engine's fork SPAN structure (type 8 at F,
 * 9 / 1 / 10 corridor spans appended after the ring, type 11 at R, the jump table,
 * lanes(F) = lanes(F+1) + lanes(B0)) and changes only where its geometry comes from:
 *
 *   rows 0..k1          CLASSIC: the main node's own centre line plus a lateral. The
 *                       corridor runs beside the road it split from, the median
 *                       opening at the rate every real fork uses and held at the real
 *                       gap the avenue sidecar gives -- these rows ARE the avenue's far
 *                       carriageway, and the avenue code treats them as an avenue fork's;
 *   rows k1..len-kx     FREE: a chain of nodes of its own. The real ring way's far arc,
 *                       entered along the street's axis and left onto the exit street,
 *                       rounded (Laplacian) so no corner is tighter than a real
 *                       junction, resampled so a row is a span long;
 *   rows len-kx..len    CLASSIC again, mirrored: it converges on the main road.
 *
 * A plaza fork may begin on the span after an avenue fork's rejoin and end on the one
 * before the next avenue fork's split (rf_compat): the far carriageway of the avenue
 * then runs on, through the cross street, round the ring and out the other side.
 *
 * The corridor has exactly R-F-1 spans like every other fork; on the free rows they are
 * far arc / free rows long (3.5 m on Azcuenaga). Everything that needs the corridor's
 * position reads one view, tg_pf_view(): a node list whose v[F+1+k] is corridor row k,
 * so the emitters that already take (nl, mb) take (view, mb) with shift 0.
 *
 * THE MAIN ROAD over a plaza fork keeps the route's own carriageway and lanes. Only the
 * classic rows' main spans and TG_PF_WEDGE spans beyond them carry fork geometry
 * (tg_fork_of_main); between them the ring road is untouched, so the plaza arc's lanes,
 * sidewalks, lawn, ribbons and streets are exactly what they were.
 *
 * NOT FAKED: nothing is invented. No lawn, ribbon or scenery is moved to fake a road; the
 * corridor is spans the car drives, built over the real OSM ring way, and every consumer
 * of the old lateral (reach, paint, routes, preview, far-band apron, avenue scenery) is
 * taught the free geometry in its own place.
 *
 * KNOBS. TD5RE_GEO_FORK_PLAZA=0 turns plaza forks off (master's behaviour; it was the
 * report knob before). TD5RE_GEO_PLAZA_STRETCH_MAX (1.6) refuses a plaza whose far arc
 * would need spans longer than that many route spans (Plaza Dardo Rocha: 2.2x, measured: pile-ups).
 * TD5RE_GEO_PLAZA_SMOOTH (300) is the Laplacian pass count. TD5RE_GEO_FORK_DIAG=1 logs
 * every refusal, TD5RE_GEO_PLAZA_DUMP=1 writes log/pf_chain_<n>.csv.
 *
 * BYTE-IDENTICAL SYNTHETIC BUILDS. Every entry point returns "nothing" with no geo
 * place loaded, and no tg_rand / tg_frand / tg_range is called here.
 */
#include "td5_trackgen_internal.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_geo_sidewalk.h"
#include "td5_geo_avenues.h"      /* the real gap between the two carriageways of an avenue */

#define PF_UPM          430.0
#define PF_MAXPLAN      96
#define PF_MAXRING      12
#define PF_RING_MAX     1024
#define PF_NODES        (TD5_TG_MAX_SPANS + 8)
#define PF_ROWS_MAX     (TD5_TG_MAX_SPANS + 8)
#define PF_TOL_M        12.0        /* a route node this close to the ring runs along it */
#define PF_RUN_MIN      8           /* nodes that make a run                              */
#define PF_PIN          7           /* chain points pinned at each end (10 m)             */
#define PF_STEP_M       1.5         /* densified chain spacing                            */
#define PF_RATE         0.12        /* classic median opening, span per span              */
#define PF_MINR_M       8.0         /* tightest corner a chain may keep                   */

typedef struct {
    int    n;
    double x[PF_RING_MAX], z[PF_RING_MAX], s[PF_RING_MAX];
    double len;
    char   name[64];
    int    lanes, namek;
} PfRing;

typedef struct {
    int    used, committed;
    int    ring;
    int    F, R, len, la, lb;
    int    k1, kx;                  /* classic rows at the entry (0..k1) and the exit (len-kx..len) */
    int    K2;                      /* first classic MAIN STEP at the exit = len - kx */
    int    clen;                    /* [1016 K] corridor spans: k1 + nfree2 + kx (len = the MAIN spans bypassed) */
    int    nfree2;                  /* free rows the arc needs at one span a row            */
    int    K2c;                     /* first classic ROW at the exit = k1 + nfree2          */
    double pitch;                   /* a free row's length / a main span's                  */
    double *med;                    /* classic median opening per row, world units (len+1)     */
    double *lat;                    /* signed lateral of the corridor row from its main node   */
    int    ie, ix;                  /* the route run along the ring  */
    double stretch;
    double ring_m, near_m, far_m;
    char   name[64];
    /* built */
    int    fi;                      /* index into s_forks            */
    TG_NodeList view;               /* v[F+1+j]: the corridor beside MAIN node F+1+j (throats, wedges) */
    TG_NodeList rview;              /* [1016 K] v[F+1+k]: corridor ROW k, k = 0..clen */
    double chain_m, min_r_m, max_dev_m;
    int    built;
    int    tin, tout;               /* [1017 R] the lane ramps in front of F / behind R actually fitted */
    int    merged;                  /* [1017 R] absorbed the avenue fork(s) next to it (classic rows run 0..k1 over the avenue) */
} PfPlan;

static PfRing  s_ring[PF_MAXRING];
static int     s_nring;
static PfPlan  s_plan[PF_MAXPLAN];
static int     s_nplan;
static double  s_rx[PF_NODES], s_rz[PF_NODES];
static int     s_rl[PF_NODES];
static int     s_rn;
static int     s_diag;
static int     s_rings_built;
static int     s_runs;                 /* ring plazas the route runs along, this build */
static char    s_run_names[8][64];

static const double kPi = 3.14159265358979323846;

static double pf_lw(void) { return (double)TD5_TG_LANE_WIDTH; }

void tg_pf_reset(void)
{
    int i;
    for (i = 0; i < s_nplan; i++) {
        free(s_plan[i].view.v);
        free(s_plan[i].rview.v);
        free(s_plan[i].med);
        free(s_plan[i].lat);
        s_plan[i].view.v = NULL;
        s_plan[i].rview.v = NULL;
    }
    s_nplan = 0;
    s_nring = 0;
    s_rn = 0;
    s_rings_built = 0;
    s_runs = 0;
    memset(s_plan, 0, sizeof s_plan);
}

static void pf_note(const char *name, int F, int R, const char *why)
{
    if (!s_diag) return;
    TD5_LOG_I(LOG_TAG, "trackgen: [PLAZA FORK] REJECT \"%s\" F=%d R=%d: %s",
              name ? name : "", F, R, why);
}

/* ------------------------------------------------------------ route helpers */

static double pf_heading(int i)
{
    const int a = (i > 0) ? i - 1 : 0;
    const int b = (i + 1 < s_rn) ? i + 1 : s_rn - 1;
    return atan2(s_rx[b] - s_rx[a], s_rz[b] - s_rz[a]);
}

static double pf_angdiff(double a, double b)
{
    double d = a - b;
    while (d >  kPi) d -= 2.0 * kPi;
    while (d < -kPi) d += 2.0 * kPi;
    return d;
}

static void pf_tangent(int i, double *tx, double *tz)
{
    const int a = (i > 0) ? i - 1 : 0;
    const int b = (i + 1 < s_rn) ? i + 1 : s_rn - 1;
    double dx = s_rx[b] - s_rx[a], dz = s_rz[b] - s_rz[a];
    const double l = sqrt(dx * dx + dz * dz);
    if (l < 1e-9) { *tx = 0.0; *tz = 1.0; return; }
    *tx = dx / l; *tz = dz / l;
}

/* Is the route straight (heading within `tol` degrees of node i's) over nodes lo..hi? */
static int pf_straight(int lo, int hi, int i, double tol_deg)
{
    int j;
    const double h0 = pf_heading(i), tol = tol_deg * kPi / 180.0;
    for (j = lo; j <= hi; j++)
        if (j >= 0 && j < s_rn && fabs(pf_angdiff(pf_heading(j), h0)) > tol) return 0;
    return 1;
}

/* ------------------------------------------------------------- ring reader */

static int pf_near_pt(double ax, double az, double bx, double bz)
{
    const double dx = ax - bx, dz = az - bz;
    return dx * dx + dz * dz < 60.0 * 60.0;     /* 0.14 m: the same OSM vertex */
}

static void pf_build_rings(void)
{
    const int nw = td5_geo_roads_count();
    unsigned char *used;
    int *idx, ni = 0, w, i;

    s_nring = 0;
    s_rings_built = 1;
    if (nw < 1) return;
    used = (unsigned char *)calloc((size_t)nw, 1);
    idx  = (int *)malloc((size_t)nw * sizeof(int));
    if (!used || !idx) { free(used); free(idx); return; }
    for (w = 0; w < nw; w++) {
        const TD5_GeoRoad *r = td5_geo_roads_get(w);
        if (r && r->roundabout && !r->bridge && !r->tunnel && r->layer == 0 && r->count >= 2)
            idx[ni++] = w;
    }
    for (i = 0; i < ni && s_nring < PF_MAXRING; i++) {
        PfRing *R;
        int cur = idx[i], added, k;
        int lane_hist[16];
        const TD5_GeoRoad *r0;
        if (used[cur]) continue;
        r0 = td5_geo_roads_get(cur);
        R = &s_ring[s_nring];
        memset(R, 0, sizeof *R);
        used[cur] = 1;
        for (k = 0; k < r0->count && R->n < PF_RING_MAX; k++) {
            double x, z;
            td5_geo_roads_point(r0, k, &x, &z);
            R->x[R->n] = x; R->z[R->n] = z; R->n++;
        }
        R->lanes = r0->lanes; R->namek = r0->namek;
        lane_hist[0] = 0; memset(lane_hist, 0, sizeof lane_hist);
        if (r0->lanes >= 0 && r0->lanes < 16) lane_hist[r0->lanes] += r0->count;
        if (r0->name_id >= 0)
            snprintf(R->name, sizeof R->name, "%s", td5_geo_roads_name_by_id(r0->name_id));
        do {
            int j;
            added = 0;
            for (j = 0; j < ni && R->n < PF_RING_MAX - 64; j++) {
                const TD5_GeoRoad *r;
                double fx, fz, lx, lz;
                const int wj = idx[j];
                if (used[wj]) continue;
                r = td5_geo_roads_get(wj);
                td5_geo_roads_point(r, 0, &fx, &fz);
                td5_geo_roads_point(r, r->count - 1, &lx, &lz);
                if (pf_near_pt(fx, fz, R->x[R->n - 1], R->z[R->n - 1])) {
                    for (k = 1; k < r->count; k++) {
                        double x, z;
                        td5_geo_roads_point(r, k, &x, &z);
                        R->x[R->n] = x; R->z[R->n] = z; R->n++;
                    }
                } else if (pf_near_pt(lx, lz, R->x[R->n - 1], R->z[R->n - 1])) {
                    for (k = r->count - 2; k >= 0; k--) {
                        double x, z;
                        td5_geo_roads_point(r, k, &x, &z);
                        R->x[R->n] = x; R->z[R->n] = z; R->n++;
                    }
                } else continue;
                used[wj] = 1;
                if (r->lanes >= 0 && r->lanes < 16) lane_hist[r->lanes] += r->count;
                if (R->name[0] == 0 && r->name_id >= 0)
                    snprintf(R->name, sizeof R->name, "%s", td5_geo_roads_name_by_id(r->name_id));
                if (r->namek) R->namek = r->namek;
                added = 1;
            }
        } while (added);
        {   /* the ring's lane count: the one most of its length has (Azcuenaga is 2 lanes
             * on 9 of its 10 ways and 3 on one) */
            int q, best = 0;
            for (q = 1; q < 16; q++) if (lane_hist[q] > lane_hist[best]) best = q;
            if (lane_hist[best] > 0) R->lanes = best;
        }
        /* a ring is a closed loop: refuse a chain that did not come back to its start */
        if (R->n < 6 || !pf_near_pt(R->x[0], R->z[0], R->x[R->n - 1], R->z[R->n - 1]))
            continue;
        R->x[R->n - 1] = R->x[0]; R->z[R->n - 1] = R->z[0];
        R->s[0] = 0.0;
        for (k = 1; k < R->n; k++)
            R->s[k] = R->s[k - 1] + sqrt((R->x[k] - R->x[k - 1]) * (R->x[k] - R->x[k - 1])
                                       + (R->z[k] - R->z[k - 1]) * (R->z[k] - R->z[k - 1]));
        R->len = R->s[R->n - 1];
        if (R->len < 40.0 * PF_UPM) continue;        /* not a plaza                    */
        s_nring++;
    }
    free(used); free(idx);
}

/* Nearest point of ring r to (px,pz): distance and arc parameter. */
static double pf_ring_dist(const PfRing *r, double px, double pz, double *sp)
{
    double best = 1e300, bs = 0.0;
    int i;
    for (i = 0; i + 1 < r->n; i++) {
        const double ax = r->x[i], az = r->z[i], bx = r->x[i + 1], bz = r->z[i + 1];
        const double dx = bx - ax, dz = bz - az;
        const double l2 = dx * dx + dz * dz;
        double t = (l2 > 1e-9) ? ((px - ax) * dx + (pz - az) * dz) / l2 : 0.0, qx, qz, d;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        qx = ax + dx * t - px; qz = az + dz * t - pz;
        d = sqrt(qx * qx + qz * qz);
        if (d < best) { best = d; bs = r->s[i] + t * (r->s[i + 1] - r->s[i]); }
    }
    if (sp) *sp = bs;
    return best;
}

static void pf_ring_pos(const PfRing *r, double sp, double *x, double *z)
{
    int i;
    sp = fmod(sp, r->len); if (sp < 0.0) sp += r->len;
    for (i = 0; i + 1 < r->n; i++)
        if (sp <= r->s[i + 1] || i + 2 == r->n) {
            const double l = r->s[i + 1] - r->s[i];
            const double t = (l > 1e-9) ? (sp - r->s[i]) / l : 0.0;
            *x = r->x[i] + (r->x[i + 1] - r->x[i]) * t;
            *z = r->z[i] + (r->z[i + 1] - r->z[i]) * t;
            return;
        }
    *x = r->x[0]; *z = r->z[0];
}

/* First crossing of the ray (px,pz)+t(dx,dz), 0 < t < maxd, with the ring. */
static int pf_ray_ring(const PfRing *r, double px, double pz, double dx, double dz,
                       double maxd, double *sp, double *hx, double *hz)
{
    double bt = 1e300; int i, hit = 0;
    for (i = 0; i + 1 < r->n; i++) {
        const double ax = r->x[i], az = r->z[i];
        const double ex = r->x[i + 1] - ax, ez = r->z[i + 1] - az;
        const double den = dx * ez - dz * ex;
        double t, u;
        if (fabs(den) < 1e-9) continue;
        t = ((ax - px) * ez - (az - pz) * ex) / den;
        u = ((ax - px) * dz - (az - pz) * dx) / den;
        if (t > 0.0 && t < maxd && u >= 0.0 && u <= 1.0 && t < bt) {
            bt = t; hit = 1;
            *sp = r->s[i] + u * (r->s[i + 1] - r->s[i]);
            *hx = px + dx * t; *hz = pz + dz * t;
        }
    }
    return hit;
}

/* ------------------------------------------------------------- the chain */

/* The free chain from Ps (heading ts) to Pe (heading te) round ring r: entered along
 * the street's own heading, rounded, resampled to `nsteps` equal steps. Outputs the
 * nsteps+1 points. Returns 0 (with *why set) when the geometry does not allow it. */
static int pf_free_chain(const PfRing *r, double psx, double psz, double tsx, double tsz,
                         double pex, double pez, double tex, double tez, int nsteps,
                         int run_lo, int run_hi, int smooth_it,
                         double *cx, double *cz, double *len_out, double *minr_out,
                         double *far_out, const char **why)
{
    double sa, sb, jax, jaz, jbx, jbz, lf, lb, dmf = 1e300, dmb = 1e300, mx, mz;
    double *px = NULL, *pz = NULL, *dx = NULL, *dz = NULL, *tx = NULL, *tz = NULL;
    int np = 0, cap, i, it, ok = 0, dir, nd;
    double tot = 0.0, h = PF_STEP_M * PF_UPM;
    const double ex = PF_UPM * 10.0;

    *why = "";
    if (!pf_ray_ring(r, psx, psz, tsx, tsz, 150.0 * PF_UPM, &sa, &jax, &jaz)) {
        *why = "the street's axis never meets the ring"; return 0;
    }
    if (!pf_ray_ring(r, pex, pez, -tex, -tez, 150.0 * PF_UPM, &sb, &jbx, &jbz)) {
        *why = "the exit street's axis never meets the ring"; return 0;
    }
    if (sqrt((jax - psx) * (jax - psx) + (jaz - psz) * (jaz - psz)) < 14.0 * PF_UPM ||
        sqrt((jbx - pex) * (jbx - pex) + (jbz - pez) * (jbz - pez)) < 14.0 * PF_UPM) {
        *why = "the free chain starts too close to the ring to turn onto it"; return 0;
    }
    /* which way round: the arc whose middle is FARTHER from the route's own run */
    lf = fmod(sb - sa, r->len); if (lf < 0.0) lf += r->len;
    lb = r->len - lf;
    pf_ring_pos(r, sa + lf * 0.5, &mx, &mz);
    for (i = run_lo; i <= run_hi; i++) {
        const double d = sqrt((s_rx[i] - mx) * (s_rx[i] - mx) + (s_rz[i] - mz) * (s_rz[i] - mz));
        if (d < dmf) dmf = d;
    }
    pf_ring_pos(r, sa - lb * 0.5, &mx, &mz);
    for (i = run_lo; i <= run_hi; i++) {
        const double d = sqrt((s_rx[i] - mx) * (s_rx[i] - mx) + (s_rz[i] - mz) * (s_rz[i] - mz));
        if (d < dmb) dmb = d;
    }
    dir = (dmf >= dmb) ? 1 : -1;
    {
        const double arc = (dir > 0) ? lf : lb;
        const int na = (int)(arc / (3.0 * PF_UPM)) + 2;
        cap = na + 8;
        px = (double *)malloc((size_t)cap * sizeof(double));
        pz = (double *)malloc((size_t)cap * sizeof(double));
        if (!px || !pz) { free(px); free(pz); *why = "out of memory"; return 0; }
        px[np] = psx; pz[np] = psz; np++;
        px[np] = psx + tsx * ex; pz[np] = psz + tsz * ex; np++;
        for (i = 0; i <= na && np < cap - 3; i++) {
            double x, z;
            pf_ring_pos(r, sa + (double)dir * arc * (double)i / (double)na, &x, &z);
            px[np] = x; pz[np] = z; np++;
        }
        px[np] = pex - tex * ex; pz[np] = pez - tez * ex; np++;
        px[np] = pex; pz[np] = pez; np++;
        *far_out = arc;
    }
    for (i = 1; i < np; i++)
        tot += sqrt((px[i] - px[i - 1]) * (px[i] - px[i - 1]) + (pz[i] - pz[i - 1]) * (pz[i] - pz[i - 1]));
    nd = (int)(tot / h); if (nd < 16) nd = 16;
    dx = (double *)malloc((size_t)(nd + 1) * sizeof(double));
    dz = (double *)malloc((size_t)(nd + 1) * sizeof(double));
    tx = (double *)malloc((size_t)(nd + 1) * sizeof(double));
    tz = (double *)malloc((size_t)(nd + 1) * sizeof(double));
    if (!dx || !dz || !tx || !tz) { *why = "out of memory"; goto out; }
    {   /* arc-length resample of the polyline to nd+1 points */
        double acc = 0.0, seg = 0.0; int j = 0;
        for (i = 0; i <= nd; i++) {
            const double want = tot * (double)i / (double)nd;
            while (j + 1 < np - 1) {
                seg = sqrt((px[j + 1] - px[j]) * (px[j + 1] - px[j]) + (pz[j + 1] - pz[j]) * (pz[j + 1] - pz[j]));
                if (acc + seg >= want) break;
                acc += seg; j++;
            }
            seg = sqrt((px[j + 1] - px[j]) * (px[j + 1] - px[j]) + (pz[j + 1] - pz[j]) * (pz[j + 1] - pz[j]));
            {
                const double u = (seg > 1e-9) ? (want - acc) / seg : 0.0;
                dx[i] = px[j] + (px[j + 1] - px[j]) * u;
                dz[i] = pz[j] + (pz[j + 1] - pz[j]) * u;
            }
        }
    }
    for (it = 0; it < smooth_it; it++) {
        for (i = 0; i <= nd; i++) { tx[i] = dx[i]; tz[i] = dz[i]; }
        for (i = PF_PIN; i <= nd - PF_PIN; i++) {
            dx[i] = (tx[i - 1] + 2.0 * tx[i] + tx[i + 1]) * 0.25;
            dz[i] = (tz[i - 1] + 2.0 * tz[i] + tz[i + 1]) * 0.25;
        }
    }
    /* resample to nsteps equal steps */
    {
        double *ls = tx;                       /* reuse: cumulative length */
        double acc = 0.0, L2; int j = 0;
        ls[0] = 0.0;
        for (i = 1; i <= nd; i++)
            ls[i] = ls[i - 1] + sqrt((dx[i] - dx[i - 1]) * (dx[i] - dx[i - 1]) + (dz[i] - dz[i - 1]) * (dz[i] - dz[i - 1]));
        L2 = ls[nd];
        (void)acc;
        for (i = 0; i <= nsteps; i++) {
            const double want = L2 * (double)i / (double)nsteps;
            double u;
            while (j + 1 < nd && ls[j + 1] < want) j++;
            u = (ls[j + 1] - ls[j] > 1e-9) ? (want - ls[j]) / (ls[j + 1] - ls[j]) : 0.0;
            cx[i] = dx[j] + (dx[j + 1] - dx[j]) * u;
            cz[i] = dz[j] + (dz[j + 1] - dz[j]) * u;
        }
        *len_out = L2;
    }
    {   /* tightest corner (circumradius of consecutive chain triples) */
        double minr = 1e300;
        for (i = 1; i < nsteps; i++) {
            const double ax = cx[i] - cx[i - 1], az = cz[i] - cz[i - 1];
            const double bx = cx[i + 1] - cx[i], bz = cz[i + 1] - cz[i];
            const double la = sqrt(ax * ax + az * az), lb2 = sqrt(bx * bx + bz * bz);
            const double cr = fabs(ax * bz - az * bx);
            const double th = atan2(cr, ax * bx + az * bz);
            if (th > 1e-3) {
                const double rr = 0.5 * (la + lb2) / th;
                if (rr < minr) minr = rr;
            }
        }
        *minr_out = minr / PF_UPM;
    }
    ok = 1;
out:
    free(px); free(pz); free(dx); free(dz); free(tx); free(tz);
    return ok;
}

/* ------------------------------------------------------------- the planner */

static int pf_ring_lanes(const PfRing *r)
{
    /* The ring way's OWN lane count (OSM lanes=2 on Azcuenaga): the corridor is that
     * road. The route's plaza carriageway is wider (the 10.47 m plaza floor), but a
     * corridor of 2 lanes keeps lanes(A)+lanes(B) equal to the avenue forks' 2+2, which
     * is what lets a plaza fork start right where an avenue fork ends. */
    return r->lanes < 1 ? 1 : r->lanes;       /* the MODE over the ring's ways, see pf_build_rings */
}

static void pf_classic_xz(const double *Ox, const double *Oz, double tx, double tz,
                          int la, int lb, double med, double *x, double *z)
{
    const double off = (double)(la + lb) * pf_lw() * 0.5 + med;
    *x = *Ox + (-tz) * off;               /* right of travel = (-tz, tx) */
    *z = *Oz + ( tx) * off;
}

/* The real gap between the two carriageways at span si (world units), from the avenue
 * sidecar: |offset| less the half of both carriageways. 1e30 = no sidecar there, so only
 * the opening rate limits the median. The far carriageway must be on the corridor's
 * (right, negative) side, or there is no real gap to follow. */
static double pf_gap_cap(int si, double half)
{
    double off = 0.0;
    if (!td5_geo_avenue_at(si, &off, NULL, NULL)) return 1e30;
    if (!(off < 0.0)) return 1e30;
    off = -off - half;
    return off < 0.0 ? 0.0 : off;
}

/* The classic rows' median opening, per row: 0 at both mouths, `rate` per row away
 * from them, held at the real gap where the avenue sidecar gives one. Entry rows
 * 0..k1 and exit rows len-kx..len; the free interior holds the entry value (unused). */
static void pf_med_profile(PfPlan *P)
{
    const double half = (double)(P->la + P->lb) * pf_lw() * 0.5;
    const double rate = PF_RATE * (double)TD5_TG_SPAN_LENGTH;
    int k;
    for (k = 0; k <= P->len; k++) {
        double m;
        if (k <= P->k1) {
            m = rate * (double)k;
            { const double cap = pf_gap_cap(P->F + 1 + k, half); if (cap < m) m = cap; }
            P->med[k] = m;
        } else if (k >= P->K2) {
            m = rate * (double)(P->len - k);
            { const double cap = pf_gap_cap(P->F + 1 + k, half); if (cap < m) m = cap; }
            P->med[k] = m;
        } else {
            P->med[k] = P->med[P->k1];
        }
    }
}

double tg_pf_med(int plan, int k)
{
    const PfPlan *P;
    if (plan < 0 || plan >= s_nplan) return 0.0;
    P = &s_plan[plan];
    if (!P->med) return 0.0;
    if (k < 0) k = 0;
    if (k > P->len) k = P->len;
    return P->med[k];
}

int tg_pf_candidates(const double *rx, const double *rz, const int *rl, int rn,
                     int win_hi, const int *avF, const int *avR, int nav,
                     TG_PfCand *out, int max_out)
{
    int r, nout = 0;
    const int taper = td5_env_int("TD5RE_GEO_FORK_TAPER", 16, 2, 48);
    const double stretch_max = (double)td5_env_float("TD5RE_GEO_PLAZA_STRETCH_MAX", 3.0f, 1.0f, 8.0f);
    const int smooth_it = td5_env_int("TD5RE_GEO_PLAZA_SMOOTH", 300, 0, 800);
    /* How many nodes before the street's bend the free chain leaves the avenue's line, and
     * how many after the exit bend it rejoins: the longer, the wider the corner it takes. */
    const int lead_in  = td5_env_int("TD5RE_GEO_PLAZA_LEAD_IN", 14, 0, 40);
    const int lead_out = td5_env_int("TD5RE_GEO_PLAZA_LEAD_OUT", 10, 0, 40);

    s_diag = td5_env_flag_off("TD5RE_GEO_FORK_DIAG");
    if (!td5_env_flag_on("TD5RE_GEO_FORK_PLAZA")) return 0;
    if (!td5_geo_loaded() || rn < 40) return 0;
    if (!td5_geo_roads_sync(td5_geo_place_slug())) return 0;
    if (rn > PF_NODES) rn = PF_NODES;
    memcpy(s_rx, rx, (size_t)rn * sizeof(double));
    memcpy(s_rz, rz, (size_t)rn * sizeof(double));
    memcpy(s_rl, rl, (size_t)rn * sizeof(int));
    s_rn = rn;
    td5_geo_sw_place(td5_geo_place_slug());
    pf_build_rings();

    for (r = 0; r < s_nring && nout < max_out; r++) {
        const PfRing *R = &s_ring[r];
        int i, ie = -1, ix = -1;
        double sp_dummy;
        /* runs of route nodes that run ALONG the ring */
        for (i = 0; i < s_rn; i++) {
            const int on = pf_ring_dist(R, s_rx[i], s_rz[i], &sp_dummy) < PF_TOL_M * PF_UPM;
            if (on && ie < 0) ie = i;
            if (on) ix = i;
            if ((!on || i == s_rn - 1) && ie >= 0) {
                if (ix - ie + 1 >= PF_RUN_MIN) {
                    /* ---- one run: plan it ---- */
                    int b_in = -1, b_out = -1, v, w;
                    if (s_runs < 8)
                        snprintf(s_run_names[s_runs], sizeof s_run_names[0], "%s",
                                 R->name[0] ? R->name : "(unnamed ring)");
                    s_runs++;
                    int Fopt[16], Ropt[16], Fthr[16], Rthr[16], nF = 0, nR = 0;
                    const int merge_on = td5_env_flag_on("TD5RE_GEO_FORK_MERGE_ADJ");   /* [1017 R step 1] */
                    char label[64];
                    snprintf(label, sizeof label, "%s",
                             R->name[0] ? R->name : "(unnamed ring)");
                    for (v = ie - 1; v >= 12; v--)
                        if (pf_straight(v - 8, v, v, 1.0)) { b_in = v; break; }
                    for (v = ix + 1; v + 8 < s_rn; v++)
                        if (pf_straight(v, v + 8, v, 1.0)) { b_out = v; break; }
                    if (b_in < 0 || b_out < 0) {
                        pf_note(label, ie, ix, "no straight street before the entry or after the exit");
                    } else {
                        /* ENTRY options: F ten nodes before the street's bend (a short
                         * classic zone), and right after each avenue fork that ends on
                         * this street before the bend (the far carriageway runs on). */
                        Fopt[nF] = b_in - 10; Fthr[nF] = Fopt[nF]; nF++;
                        for (w = 0; w < nav && nF < 7; w++)
                            if (avR[w] + 1 <= b_in - 10 && avR[w] + 1 >= b_in - 70) {
                                int dup = 0, q;
                                for (q = 0; q < nF; q++) if (Fopt[q] == avR[w] + 1) dup = 1;
                                if (!dup) { Fopt[nF] = avR[w] + 1; Fthr[nF] = Fopt[nF]; nF++; }
                            }
                        /* [ROUND 1017 R step 1] MERGE WITH THE AVENUE FORK THAT ENDS AT THE THROAT:
                         * one more option per such fork, the fork's OWN split span. The plaza fork
                         * then starts there and the avenue's far carriageway is its classic entry
                         * rows (k1 grows by the avenue's length), so the median is the avenue's real
                         * gap straight into the plaza chain: no rejoin and no second split at the
                         * shared span, no pin to zero, no 13.7 degree weave. */
                        if (merge_on) {
                            /* the LONGEST such fork only: every shorter one is a prefix of it, and the
                             * plan build (a free chain, twice) is the expensive part */
                            int best = -1;
                            for (w = 0; w < nav; w++)
                                if (avR[w] + 1 <= b_in - 10 && avR[w] + 1 >= b_in - 70 && avF[w] < avR[w] + 1 - 24 &&
                                    (best < 0 || avF[w] < avF[best])) best = w;
                            if (best >= 0 && nF < 15) { Fopt[nF] = avF[best]; Fthr[nF] = avR[best] + 1; nF++; }
                        }
                        /* EXIT options: R five nodes after the straight resumes, and just
                         * before each avenue fork that starts on the exit street. */
                        Ropt[nR] = b_out + TG_PF_K1MIN; Rthr[nR] = Ropt[nR]; nR++;
                        for (w = 0; w < nav && nR < 7; w++)
                            if (avF[w] - 1 >= b_out + TG_PF_K1MIN && avF[w] - 1 <= b_out + 70) {
                                int dup = 0, q;
                                for (q = 0; q < nR; q++) if (Ropt[q] == avF[w] - 1) dup = 1;
                                if (!dup) { Ropt[nR] = avF[w] - 1; Rthr[nR] = Ropt[nR]; nR++; }
                            }
                        if (merge_on) {
                            int best = -1;
                            for (w = 0; w < nav; w++)
                                if (avF[w] - 1 >= b_out + TG_PF_K1MIN && avF[w] - 1 <= b_out + 70 && avR[w] > avF[w] + 24 &&
                                    (best < 0 || avR[w] > avR[best])) best = w;
                            if (best >= 0 && nR < 15) { Ropt[nR] = avR[best]; Rthr[nR] = avF[best] - 1; nR++; }
                        }
                        if (s_diag) {
                            char ob[160]; int q, n = 0;
                            n += snprintf(ob + n, sizeof ob - n, "straight street nodes ..%d / %d.., avenue forks on offer %d; F options", b_in, b_out, nav);
                            for (q = 0; q < nF; q++) n += snprintf(ob + n, sizeof ob - n, " %d", Fopt[q]);
                            n += snprintf(ob + n, sizeof ob - n, "; R options");
                            for (q = 0; q < nR; q++) n += snprintf(ob + n, sizeof ob - n, " %d", Ropt[q]);
                            pf_note(label, ie, ix, ob);
                        }
                        for (v = 0; v < nF && nout < max_out && s_nplan < PF_MAXPLAN; v++)
                            for (w = 0; w < nR && nout < max_out && s_nplan < PF_MAXPLAN; w++) {
                                const int F = Fopt[v], Rr = Ropt[w];
                                const int merged = (Fthr[v] != F) || (Rthr[w] != Rr);   /* [1017 R] */
                                const int L = Rr - F - 1;
                                const int li = (lead_in < b_in - (F + 1) - TG_PF_K1MIN) ? lead_in : b_in - (F + 1) - TG_PF_K1MIN;
                                const int lo = (lead_out < Rr - b_out - 3) ? lead_out : Rr - b_out - 3;
                                const int k1 = (b_in - li) - (F + 1), kx = Rr - (b_out + lo);
                                int tin = taper, tout = taper;
                                const int nfree = L - k1 - kx;
                                int nfree2;
                                double tsx, tsz, tex, tez, psx, psz, pex, pez, len_m = 0, minr = 0, far_m = 0;
                                double *cx, *cz;
                                const char *why = "";
                                int q, la, lb, bad = 0, a, b;
                                PfPlan *P;
                                /* [1017 R] a merged fork's far ends are avenue ends: the lane ramp is
                                 * fitted to the room there is (as rf_fit_window does), at least TAPER_MIN */
                                if (merged) {
                                    const int room_in  = F - (TD5_TG_BRANCH_WIDEN + 2) - (TD5_TG_GRID_SPAN + 2);
                                    const int room_out = win_hi - (Rr + 2);
                                    if (tin > room_in)   tin = room_in;
                                    if (tout > room_out) tout = room_out;
                                    if (tin < 6 || tout < 6) { pf_note(label, F, Rr, "merged: no room for the lane ramp (start grid or finish)"); continue; }
                                }
                                if (F - TD5_TG_BRANCH_WIDEN - 2 - tin < TD5_TG_GRID_SPAN + 2) { pf_note(label, F, Rr, "inside the start grid"); continue; }
                                if (Rr + 2 + tout > win_hi) { pf_note(label, F, Rr, "past the finish line (or inside the ring tail)"); continue; }
                                if (L < 40 || L > (merged ? 640 : 470) || nfree < 12 || k1 < TG_PF_K1MIN || kx < 3) { pf_note(label, F, Rr, "window length or classic rows out of range"); continue; }
                                la = s_rl[F + 1]; lb = pf_ring_lanes(R);
                                /* The corridor is never wider than the road it splits from: the reader
                                 * gives every plaza-named ring way the plaza floor (3 lanes, 10.47 m),
                                 * the street here is 2, and 2+2 is what the avenue forks carry, which
                                 * is what lets a plaza fork start where an avenue fork ends. */
                                if (lb > la) lb = la;
                                if (la < 1 || la + lb > 8) { pf_note(label, F, Rr, "lanes(A)+lanes(B) over the 8-lane rail range"); continue; }
                                /* the route's own lane count is constant over the whole entry
                                 * (taper .. wedge) and the whole exit (wedge .. taper) */
                                /* [1017 R] a merged fork's avenue part was checked by the avenue fork's own
                                 * window (rf_fit_window); only the plaza's throat is checked here */
                                for (q = (merged ? Fthr[v] - (TD5_TG_BRANCH_WIDEN + 2) - taper : F - TD5_TG_BRANCH_WIDEN - 2 - tin) - 1;
                                     q <= F + 2 + k1 + TG_PF_WEDGE && !bad; q++)
                                    if (q >= 0 && q < s_rn && s_rl[q] != la) bad = 1;
                                for (q = Rr - 2 - kx - TG_PF_WEDGE; q <= (merged ? Rthr[w] : Rr) + 3 + taper && !bad; q++)
                                    if (q >= 0 && q < s_rn && s_rl[q] != la) bad = 1;
                                if (bad) { pf_note(label, F, Rr, "the route's own lane count changes inside a throat"); continue; }
                                if (!pf_straight((merged ? Fthr[v] : F) - 8, b_in, b_in, 3.0) || !pf_straight(b_out, (merged ? Rthr[w] : Rr) + 8, b_out, 3.0)) {
                                    pf_note(label, F, Rr, "the street is not straight through the throat"); continue; }
                                P = &s_plan[s_nplan];
                                memset(P, 0, sizeof *P);
                                P->F = F; P->R = Rr; P->len = L; P->la = la; P->lb = lb;
                                P->k1 = k1; P->kx = kx; P->K2 = L - kx;
                                P->tin = tin; P->tout = tout; P->merged = merged;
                                P->med = (double *)calloc((size_t)(L + 2), sizeof(double));
                                P->lat = (double *)calloc((size_t)(L + 2), sizeof(double));
                                if (!P->med || !P->lat) { free(P->med); free(P->lat); P->med = P->lat = NULL; continue; }
                                pf_med_profile(P);
                                /* poses of the free chain's two ends (raw route as the centre line) */
                                a = F + 1 + k1; b = Rr - kx;
                                pf_tangent(a, &tsx, &tsz); pf_tangent(b, &tex, &tez);
                                pf_classic_xz(&s_rx[a], &s_rz[a], tsx, tsz, la, lb, P->med[k1], &psx, &psz);
                                pf_classic_xz(&s_rx[b], &s_rz[b], tex, tez, la, lb, P->med[L - kx], &pex, &pez);
                                cx = (double *)malloc((size_t)(nfree * 4 + 16) * sizeof(double));
                                cz = (double *)malloc((size_t)(nfree * 4 + 16) * sizeof(double));
                                if (!cx || !cz) { free(cx); free(cz); free(P->med); free(P->lat); P->med = P->lat = NULL; continue; }
                                if (!pf_free_chain(R, psx, psz, tsx, tsz, pex, pez, tex, tez, nfree, ie, ix,
                                                   smooth_it, cx, cz, &len_m, &minr, &far_m, &why)) {
                                    pf_note(label, F, Rr, why); free(cx); free(cz);
                                    free(P->med); free(P->lat); P->med = P->lat = NULL; continue; }
                                /* [ROUND 1016 K] The corridor has the spans its ARC needs: one a
                                 * TD5_TG_SPAN_LENGTH, whatever number of main spans it bypasses (a
                                 * 395 m far arc over 142 m of route was spans 7.8 m long). The same
                                 * chain is built again at that row count to validate the corner. */
                                nfree2 = (int)(len_m / (double)TD5_TG_SPAN_LENGTH + 0.5);
                                if (!td5_env_flag_on("TD5RE_GEO_PLAZA_VARLEN")) nfree2 = nfree;
                                if (nfree2 < 12) nfree2 = 12;
                                if (nfree2 > nfree * 4) nfree2 = nfree * 4;
                                if (nfree2 != nfree &&
                                    !pf_free_chain(R, psx, psz, tsx, tsz, pex, pez, tex, tez, nfree2, ie, ix,
                                                   smooth_it, cx, cz, &len_m, &minr, &far_m, &why)) {
                                    pf_note(label, F, Rr, why); free(cx); free(cz);
                                    free(P->med); free(P->lat); P->med = P->lat = NULL; continue; }
                                free(cx); free(cz);
                                {
                                    double main_m = 0.0; int j;
                                    for (j = a; j < b; j++)
                                        main_m += sqrt((s_rx[j + 1] - s_rx[j]) * (s_rx[j + 1] - s_rx[j])
                                                     + (s_rz[j + 1] - s_rz[j]) * (s_rz[j + 1] - s_rz[j]));
                                    P->stretch = len_m / main_m;                     /* arc / route over the free rows */
                                    P->pitch   = (len_m / (double)nfree2) / (main_m / (double)(b - a));
                                    P->nfree2  = nfree2;
                                    P->K2c     = k1 + nfree2;
                                    P->clen    = k1 + nfree2 + kx;
                                }
                                if (P->stretch > stretch_max || P->stretch < 0.5) {
                                    char w2[256];
                                    snprintf(w2, sizeof w2, "the far arc is %.2fx the route over the same stretch (limit %.2f): "
                                             "the corridor would be %.0f m of road over %d spans",
                                             P->stretch, stretch_max, len_m / PF_UPM, nfree2);
                                    pf_note(label, F, Rr, w2);
                                    free(P->med); free(P->lat); P->med = P->lat = NULL; continue;
                                }
                                if (minr < PF_MINR_M) {
                                    char w2[256];
                                    snprintf(w2, sizeof w2, "the chain keeps a %.1f m corner (limit %.1f)", minr, PF_MINR_M);
                                    pf_note(label, F, Rr, w2);
                                    free(P->med); free(P->lat); P->med = P->lat = NULL; continue;
                                }
                                P->used = 1; P->ring = r;
                                P->ie = ie; P->ix = ix;
                                P->ring_m = R->len / PF_UPM; P->far_m = far_m / PF_UPM;
                                P->near_m = R->len / PF_UPM - far_m / PF_UPM;
                                P->chain_m = len_m / PF_UPM; P->min_r_m = minr;
                                snprintf(P->name, sizeof P->name, "%s", label);
                                {
                                    TG_PfCand *c0 = &out[nout];
                                    memset(c0, 0, sizeof *c0);
                                    c0->F = F; c0->R = Rr; c0->len = L; c0->lanes_a = la; c0->lanes_b = lb;
                                    c0->k1 = k1; c0->kx = kx;
                                    c0->tin = tin; c0->tout = tout; c0->merged = merged;
                                    c0->plan = s_nplan; c0->stretch = P->stretch; c0->clen = P->clen;
                                    c0->ring_m = P->ring_m; c0->near_m = P->near_m; c0->far_m = P->far_m;
                                    snprintf(c0->name, sizeof c0->name, "%s", label);
                                }
                                s_nplan++; nout++;
                            }
                    }
                }
                ie = -1; ix = -1;
            }
        }
    }
    if (nout)
        TD5_LOG_I(LOG_TAG, "trackgen: [PLAZA FORK] %d ring(s) read, %d plan(s) from the route "
                  "(knob TD5RE_GEO_FORK_PLAZA)", s_nring, nout);
    return nout;
}

void tg_pf_commit(int plan)
{
    if (plan >= 0 && plan < s_nplan) s_plan[plan].committed = 1;
}

/* ------------------------------------------------------ node window profile */

/* The corridor lanes carried by route node `node`: full over the classic entry rows
 * (F-8 .. F+1+k1) and the classic exit rows (R-kx .. R+2), ramped out over the TG_PF_WEDGE
 * nodes beyond each, 0 on the ring road in between. */
double tg_pf_node_extra(int plan, int node)
{
    const PfPlan *P;
    double b, e = 0.0;
    int F, R, T, To, i0, w_end, k1, kx;
    if (plan < 0 || plan >= s_nplan) return 0.0;
    P = &s_plan[plan];
    b = (double)P->lb; F = P->F; R = P->R; k1 = P->k1; kx = P->kx;
    T = P->tin > 0 ? P->tin : td5_env_int("TD5RE_GEO_FORK_TAPER", 16, 2, 48);
    To = P->tout > 0 ? P->tout : T;
    i0 = F - (TD5_TG_BRANCH_WIDEN + 2) - T;
    w_end = R + 2 + To;
    if (node < i0 || node > w_end) return 0.0;
    if (node < F - (TD5_TG_BRANCH_WIDEN + 2))             /* ramp in */
        return b * (double)(node - i0) / (double)T;
    if (node <= F + 1 + k1) return b;                       /* full, entry classic zone */
    if (node <= F + 1 + k1 + TG_PF_WEDGE) {                 /* ramp out of the entry wedge */
        e = b * (1.0 - (double)(node - (F + 1 + k1)) / (double)TG_PF_WEDGE);
        return e < 0.0 ? 0.0 : e;
    }
    if (node < R - kx - TG_PF_WEDGE) return 0.0;            /* the ring road, untouched */
    if (node < R - kx) {                                    /* ramp into the exit wedge */
        e = b * (1.0 - (double)((R - kx) - node) / (double)TG_PF_WEDGE);
        return e < 0.0 ? 0.0 : e;
    }
    if (node <= R + 2) return b;
    return b * (1.0 - (double)(node - (R + 2)) / (double)To);
}

/* ------------------------------------------------------------- the build */

int tg_fork_is_free(int fi)
{
    return fi >= 0 && fi < s_fork_count && s_forks[fi].freec > 0;
}

static PfPlan *pf_of(int fi)
{
    if (!tg_fork_is_free(fi)) return NULL;
    {
        const int p = s_forks[fi].freec - 1;
        return (p >= 0 && p < s_nplan) ? &s_plan[p] : NULL;
    }
}

const TG_NodeList *tg_pf_view(int fi)
{
    PfPlan *P = pf_of(fi);
    return (P && P->built) ? &P->view : NULL;
}

const TG_NodeList *tg_pf_rview(int fi)
{
    PfPlan *P = pf_of(fi);
    return (P && P->built) ? &P->rview : NULL;
}

int tg_pf_plan_clen(int plan)
{
    if (plan < 0 || plan >= s_nplan || !s_plan[plan].used) return 0;
    return s_plan[plan].clen;
}

/* The main node index corridor row k stands beside: one per row on the classic entry and
 * exit rows, and the free rows spread over the nodes between (nfree main nodes carry
 * nfree2 rows). -1 when fi is not a plaza fork. */
int tg_pf_row_node(int fi, int k)
{
    const PfPlan *P = pf_of(fi);
    if (!P) return -1;
    if (k <= P->k1) return P->F + 1 + (k < 0 ? 0 : k);
    if (k >= P->K2c) return P->F + 1 + P->K2 + (k - P->K2c);
    return P->F + 1 + P->k1 + (int)(((long)(k - P->k1) * (long)(P->K2 - P->k1)) / (long)P->nfree2);
}

static void pf_set_node(TG_Node *n, double x, double y, double z, double tx, double tz,
                        double width, int lanes)
{
    memset(n, 0, sizeof *n);
    n->x = x; n->y = y; n->z = z;
    n->tx = tx; n->tz = tz;
    n->width = width; n->lanes = lanes;
    n->lane_base = TD5_TG_HEIGHT_NIBBLE;
}

/* Position of the free chain at a fractional row t (0..nfree2), linearly between rows. */
static void pf_chain_at(const double *fx, const double *fy, const double *fz, int n, double t,
                        double *x, double *y, double *z)
{
    int i = (int)t;
    double u;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    u = t - (double)i;
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;
    *x = fx[i] + (fx[i + 1] - fx[i]) * u;
    *y = fy[i] + (fy[i + 1] - fy[i]) * u;
    *z = fz[i] + (fz[i + 1] - fz[i]) * u;
}

/* [ROUND 1016 K] THE TWO VIEWS.
 *   rview  v[F+1+k], k = 0..clen: corridor ROW k. Rows 0..k1 are the classic entry rows
 *          (one per main node), rows k1..K2c the free chain at one row a span, rows
 *          K2c..clen the classic exit rows (one per main node again).
 *   view   v[F+1+j], j = 0..len: the corridor beside MAIN node F+1+j. The throat/wedge
 *          code (reach, wedge quad, scenery clash) pairs a main span with the corridor
 *          next to it; on the classic rows that is the row itself, in between it is the
 *          chain at the matching fraction. With clen == len the two are the same array. */
static void pf_build_one(PfPlan *P, const TG_NodeList *nl)
{
    const int F = P->F, R = P->R, L = P->len, k1 = P->k1, k2 = P->K2;
    const int la = P->la, lb = P->lb, nfree = k2 - k1;
    const int nfree2 = P->nfree2, K2c = P->K2c, clen = P->clen;
    const double lw = pf_lw();
    const double wfull = (double)(la + lb) * lw;
    const PfRing *ring = &s_ring[P->ring];
    double *ex, *ez, *ey, *fx, *fz, *fy;
    int k, j, ok;
    double len_m = 0.0, minr = 0.0, far_m = 0.0;
    double psx = 0, psz = 0, pex = 0, pez = 0, tsx = 0, tsz = 0, tex = 0, tez = 0;
    const char *why = "";
    const int smooth_it = td5_env_int("TD5RE_GEO_PLAZA_SMOOTH", 300, 0, 800);

    P->built = 0;
    free(P->view.v); P->view.v = NULL; P->view.count = 0; P->view.cap = 0;
    free(P->rview.v); P->rview.v = NULL; P->rview.count = 0; P->rview.cap = 0;
    if (!nl || R + 1 >= nl->count || F < 1 || nfree2 < 2 || clen < 4) return;
    P->view.v = (TG_Node *)calloc((size_t)nl->count, sizeof(TG_Node));
    {
        const int rcount = (nl->count > F + clen + 4) ? nl->count : F + clen + 4;
        P->rview.v = (TG_Node *)calloc((size_t)rcount, sizeof(TG_Node));
        P->rview.count = rcount; P->rview.cap = rcount;
    }
    if (!P->view.v || !P->rview.v) { free(P->view.v); free(P->rview.v); P->view.v = P->rview.v = NULL; return; }
    P->view.count = nl->count; P->view.cap = nl->count;
    ex = (double *)calloc((size_t)(L + 2), sizeof(double));
    ez = (double *)calloc((size_t)(L + 2), sizeof(double));
    ey = (double *)calloc((size_t)(L + 2), sizeof(double));
    fx = (double *)calloc((size_t)(nfree2 + 2), sizeof(double));
    fz = (double *)calloc((size_t)(nfree2 + 2), sizeof(double));
    fy = (double *)calloc((size_t)(nfree2 + 2), sizeof(double));
    if (!ex || !ez || !ey || !fx || !fz || !fy) {
        free(ex); free(ez); free(ey); free(fx); free(fz); free(fy);
        free(P->view.v); free(P->rview.v); P->view.v = P->rview.v = NULL;
        return;
    }

    /* classic rows (entry steps 0..k1, exit steps k2..L): the ORIGINAL route centre line
     * (node minus its jog) plus the corridor's lateral, so they sit exactly where the
     * avenue forks put theirs. Indexed by MAIN step j. */
    for (j = 0; j <= L; j++) {
        const TG_Node *n;
        double Ox, Oz, med;
        if (j > k1 && j < k2) continue;
        n = &nl->v[F + 1 + j];
        Ox = n->x - n->jx; Oz = n->z - n->jz;
        med = P->med[j];
        pf_classic_xz(&Ox, &Oz, n->tx, n->tz, la, lb, med, &ex[j], &ez[j]);
        ey[j] = n->y;
    }
    {   /* the two poses the free chain is pinned to */
        const TG_Node *a2 = &nl->v[F + 1 + k1], *b2 = &nl->v[F + 1 + k2];
        psx = ex[k1]; psz = ez[k1]; tsx = a2->tx; tsz = a2->tz;
        pex = ex[k2]; pez = ez[k2]; tex = b2->tx; tez = b2->tz;
    }
    ok = pf_free_chain(ring, psx, psz, tsx, tsz, pex, pez, tex, tez, nfree2, P->ie, P->ix,
                       smooth_it, fx, fz, &len_m, &minr, &far_m, &why);
    if (!ok) {
        /* Never a hole in the strip: the fork is already in the table, so the corridor
         * stays a classic one (beside the road, median opening and closing at the
         * classic rate, capped at 4 m). It is logged loudly: the planner ran the
         * same chain on the raw route and accepted it. */
        TD5_LOG_W(LOG_TAG, "trackgen: [PLAZA FORK] \"%s\" F=%d: the free chain failed at build (%s); "
                  "the corridor falls back to a classic one beside the road", P->name, F, why);
        for (k = 0; k <= nfree2; k++) {
            const double jj = (double)k1 + (double)k * (double)nfree / (double)nfree2;
            int j0 = (int)jj;
            const TG_Node *n;
            double med = P->med[k1] + (P->med[k2] - P->med[k1]) * (double)k / (double)nfree2;
            double Ox, Oz;
            if (j0 < k1) j0 = k1;
            if (j0 > k2) j0 = k2;
            n = &nl->v[F + 1 + j0];
            Ox = n->x - n->jx; Oz = n->z - n->jz;
            if (med > 4.0 * PF_UPM) med = 4.0 * PF_UPM;
            pf_classic_xz(&Ox, &Oz, n->tx, n->tz, la, lb, med, &fx[k], &fz[k]);
            fy[k] = n->y;
        }
        len_m = 0.0; minr = 0.0;
    } else
    /* heights: terrain-following between the two route poses */
    {
        const double ys = nl->v[F + 1 + k1].y, ye = nl->v[F + 1 + k2].y;
        const double ds = ys - tg_world_h(fx[0], fz[0]);
        const double de = ye - tg_world_h(fx[nfree2], fz[nfree2]);
        for (k = 0; k <= nfree2; k++) {
            const double u = (double)k / (double)nfree2;
            fy[k] = tg_world_h(fx[k], fz[k]) + ds + (de - ds) * u;
        }
        fy[0] = ys; fy[nfree2] = ye;
        TD5_LOG_I(LOG_TAG, "trackgen: [PLAZA FORK] heights: route y %.0f at row %d, %.0f at row %d; "
                  "terrain under them %.0f / %.0f (offsets %.0f / %.0f); mid-chain terrain %.0f road %.0f",
                  ys, k1, ye, K2c, ys - ds, ye - de, ds, de,
                  tg_world_h(fx[nfree2 / 2], fz[nfree2 / 2]), fy[nfree2 / 2]);
    }
    /* ---- rview: corridor rows ---- */
    for (k = 0; k <= clen; k++) {
        double x, y, z, tx, tz, l;
        const TG_Node *mn;
        int classic = 0;
        if (k <= k1) { x = ex[k]; y = ey[k]; z = ez[k]; classic = 1; mn = &nl->v[F + 1 + k]; }
        else if (k >= K2c) { const int jj = k2 + (k - K2c); x = ex[jj]; y = ey[jj]; z = ez[jj]; classic = 1; mn = &nl->v[F + 1 + jj]; }
        else { const int t = k - k1; x = fx[t]; y = fy[t]; z = fz[t]; mn = &nl->v[tg_pf_row_node(P->fi, k)]; }
        if (classic) { tx = mn->tx; tz = mn->tz; }
        else {
            const int t = k - k1;
            const int ta = (t > 0) ? t - 1 : 0, tb = (t < nfree2) ? t + 1 : nfree2;
            tx = fx[tb] - fx[ta]; tz = fz[tb] - fz[ta];
            l = sqrt(tx * tx + tz * tz);
            if (l < 1e-9) { tx = mn->tx; tz = mn->tz; } else { tx /= l; tz /= l; }
        }
        pf_set_node(&P->rview.v[F + 1 + k], x, y, z, tx, tz, wfull, lb);
    }
    /* ---- view: the corridor beside each main node ---- */
    for (j = 0; j <= L; j++) {
        double x, y, z, tx, tz, l;
        const TG_Node *mn = &nl->v[F + 1 + j];
        if (j <= k1 || j >= k2) { x = ex[j]; y = ey[j]; z = ez[j]; tx = mn->tx; tz = mn->tz; }
        else {
            const double t = (double)(j - k1) * (double)nfree2 / (double)nfree;
            const int ti = (int)(t + 0.5);                 /* the nearest row: with one row a node this is j - k1 */
            const int ta = (ti > 0) ? ti - 1 : 0, tb = (ti + 1 < nfree2) ? ti + 1 : nfree2;
            pf_chain_at(fx, fy, fz, nfree2, t, &x, &y, &z);
            tx = fx[tb] - fx[ta]; tz = fz[tb] - fz[ta];
            l = sqrt(tx * tx + tz * tz);
            if (l < 1e-9) { tx = mn->tx; tz = mn->tz; } else { tx /= l; tz /= l; }
        }
        pf_set_node(&P->view.v[F + 1 + j], x, y, z, tx, tz, wfull, lb);
        /* lateral of the corridor centre from its (jogged) main node, + = left of travel */
        P->lat[j] = (x - mn->x) * mn->tz - (z - mn->z) * mn->tx;
    }
    P->chain_m = len_m / PF_UPM; P->min_r_m = minr;
    {   /* how far the rounded chain strays from the real ring way where it follows it */
        double dev = 0.0, sp = 0.0; int q;
        for (q = 30; q <= nfree2 - 30; q++) {
            const double d = pf_ring_dist(ring, fx[q], fz[q], &sp);
            if (d < 20.0 * PF_UPM && d > dev) dev = d;
        }
        P->max_dev_m = dev / PF_UPM;
    }
    P->built = 1;
    free(ex); free(ez); free(ey); free(fx); free(fz); free(fy);
}

void tg_pf_finalize(const TG_NodeList *nl)
{
    int fi, built = 0, any = 0;
    for (fi = 0; fi < s_fork_count; fi++) {
        PfPlan *P = pf_of(fi);
        if (!P) continue;
        P->fi = fi;
        any = 1;
        pf_build_one(P, nl);
        if (P->built) {
            built++;
            TD5_LOG_I(LOG_TAG, "trackgen: [PLAZA FORK] %d: \"%s\" F=%d len=%d R=%d (classic rows %d in / %d out) lanes %d+%d, ring "
                      "%.0f m (near %.0f far %.0f), free chain %.0f m over %d spans (%d main spans bypassed, %.2fx the route, "
                      "span %.2fx), corridor %d spans, tightest corner %.1f m, strays %.1f m from the ring way",
                      fi, P->name, P->F, P->len, P->R, P->k1, P->kx, P->la, P->lb,
                      P->ring_m, P->near_m, P->far_m, P->chain_m, P->nfree2, P->K2 - P->k1,
                      P->stretch, P->pitch, P->clen, P->min_r_m, P->max_dev_m);
            if (td5_env_flag_off("TD5RE_GEO_PLAZA_DUMP")) {
                char path[96]; FILE *fp;
                snprintf(path, sizeof path, "log/pf_chain_%d.csv", fi);
                fp = fopen(path, "w");
                if (fp) {
                    int k;
                    fprintf(fp, "row,x,y,z,tx,tz\n");
                    for (k = 0; k <= P->clen; k++) {
                        const TG_Node *n = &P->rview.v[P->F + 1 + k];
                        fprintf(fp, "%d,%.1f,%.1f,%.1f,%.4f,%.4f\n", k, n->x, n->y, n->z, n->tx, n->tz);
                    }
                    fclose(fp);
                }
            }
        }
    }
    if (s_runs > 0 || any) {
        int q;
        TD5_LOG_I(LOG_TAG, "trackgen: [PLAZA FORK] summary: the route runs along %d ring plaza(s); "
                  "%d plaza fork(s) built", s_runs, built);
        for (q = 0; q < s_runs && q < 8; q++) {
            int got = 0, i2;
            for (i2 = 0; i2 < s_nplan; i2++)
                if (s_plan[i2].committed && s_plan[i2].built &&
                    strcmp(s_plan[i2].name, s_run_names[q]) == 0) got = 1;
            TD5_LOG_I(LOG_TAG, "trackgen: [PLAZA FORK]   \"%s\": %s", s_run_names[q],
                      got ? "FORK BUILT" : "no fork (see TD5RE_GEO_FORK_DIAG=1 for the reason)");
        }
    }
}

/* ------------------------------------------------------ spans and geometry */

int tg_pf_throat_span(int fi, int si)
{
    const PfPlan *P = pf_of(fi);
    int k;
    if (!P || !P->built) return 0;
    k = si - P->F - 1;
    if (k < 0 || k >= P->len) return 0;
    return k < P->k1 + TG_PF_WEDGE || k >= P->len - P->kx - TG_PF_WEDGE;
}

int tg_pf_wedge_span(int fi, int si)
{
    const PfPlan *P = pf_of(fi);
    int k;
    if (!P || !P->built) return 0;
    k = si - P->F - 1;
    if (k < 0 || k >= P->len) return 0;
    if (k < P->k1 + TG_PF_WEDGE) return k >= P->k1;
    if (k >= P->len - P->kx - TG_PF_WEDGE) return k < P->K2;
    return 0;
}

int tg_pf_clear_span(int fi, int si)
{
    const PfPlan *P = pf_of(fi);
    if (!P || !P->built) return 0;
    return (si >= P->F - TD5_TG_BRANCH_WIDEN - 2 && si <= P->F + 1 + P->k1 + TG_PF_WEDGE + 1)
        || (si >= P->R - P->kx - TG_PF_WEDGE - 1 && si <= P->R + 2);
}

double tg_pf_main_wscale(int fi, double w)
{
    const PfPlan *P = pf_of(fi);
    if (!P || w < 1.0) return 0.5;
    return (double)P->la * pf_lw() / w;
}

/* Outermost corridor edge beside span si, as a positive distance from the main
 * node: the larger of the two span ends. 0 outside the throats. */
double tg_pf_reach(const TG_NodeList *nl, int fi, int si, double side)
{
    const PfPlan *P = pf_of(fi);
    double best = 0.0;
    int e;
    if (!P || !P->built || !nl || !tg_pf_throat_span(fi, si)) return 0.0;
    /* the wedge spans answer 0: the corridor leaves the road there, the skirt starts at the
     * road's own edge and the paved wedge lies over it (the crotch of a mouth is ground, not
     * a slot to the sky) */
    if (tg_pf_wedge_span(fi, si)) return 0.0;
    if (side * (double)s_forks[fi].side < 0.0) return 0.0;
    for (e = 0; e <= 1; e++) {
        const int i = si + e;
        const TG_Node *m, *c;
        double dx, dz, d;
        if (i >= nl->count) continue;
        m = &nl->v[i]; c = &P->view.v[i];
        dx = c->x - m->x; dz = c->z - m->z;
        d = sqrt(dx * dx + dz * dz) + (double)P->lb * pf_lw() * 0.5;
        if (d > best) best = d;
    }
    return best;
}

/* The lateral of the corridor centre from the (jogged) main node of row k, + = left of
 * travel (the corridor is on the right, so negative): measured at build from the final
 * nodes, so it is exact on the classic rows and the projection on the free ones. */
double tg_pf_br_shift(int fi, int k, double w)
{
    const PfPlan *P = pf_of(fi);
    (void)w;
    if (!P || !P->lat) return 0.0;
    if (k < 0) k = 0;
    if (k > P->len) k = P->len;
    return P->lat[k];
}

int tg_pf_far_over(int fi, int g0, int g1)
{
    const PfPlan *P = pf_of(fi);
    if (!P || !P->built) return 0;
    if (g1 >= P->F - TD5_TG_BRANCH_WIDEN - 2 && g0 <= P->F + 1 + P->k1 + TG_PF_WEDGE + TD5_TG_FAR_FORK_PAD) return 1;
    if (g1 >= P->R - P->kx - TG_PF_WEDGE - 1 && g0 <= P->R + TD5_TG_FAR_FORK_PAD) return 1;
    return 0;
}

int tg_pf_row_point(int fi, int row, double *x, double *z)
{
    const PfPlan *P = pf_of(fi);
    if (!P || !P->built || row < 0 || row > P->clen) return 0;
    *x = P->rview.v[P->F + 1 + row].x;
    *z = P->rview.v[P->F + 1 + row].z;
    return 1;
}

/* ------------------------------------------------------ wedge, paint */

/* The paved wedge between the main half's right edge and the corridor's left edge
 * while the two diverge (the classic gore cannot: it is a lateral strip, and the
 * corridor here is not beside the road). One quad per span. */
int tg_pf_emit_wedge(const TG_NodeList *nl, int fi, int si, TG_Buf *blk, size_t *moff, int *nmesh)
{
    const PfPlan *P = pf_of(fi);
    const double lw = pf_lw();
    double px[4], py[4], pz[4], uu[4], vv[4], gap0, gap1;
    int seg_page, seg_nq = 1;
    const TG_Node *m0, *m1, *c0, *c1;
    double t0, t1;
    if (!P || !P->built || si + 1 >= nl->count) return 1;
    m0 = &nl->v[si]; m1 = &nl->v[si + 1];
    c0 = &P->view.v[si]; c1 = &P->view.v[si + 1];
    /* main half's right edge: the node's left edge is at +w/2, the half is la lanes */
    t0 = m0->width * 0.5 - (double)P->la * lw;
    t1 = m1->width * 0.5 - (double)P->la * lw;
    px[0] = m0->x + m0->tz * t0; py[0] = m0->y; pz[0] = m0->z - m0->tx * t0;
    px[3] = m1->x + m1->tz * t1; py[3] = m1->y; pz[3] = m1->z - m1->tx * t1;
    /* the corridor's left edge */
    px[1] = c0->x + c0->tz * (double)P->lb * lw * 0.5; py[1] = c0->y; pz[1] = c0->z - c0->tx * (double)P->lb * lw * 0.5;
    px[2] = c1->x + c1->tz * (double)P->lb * lw * 0.5; py[2] = c1->y; pz[2] = c1->z - c1->tx * (double)P->lb * lw * 0.5;
    gap0 = sqrt((px[1] - px[0]) * (px[1] - px[0]) + (pz[1] - pz[0]) * (pz[1] - pz[0]));
    gap1 = sqrt((px[2] - px[3]) * (px[2] - px[3]) + (pz[2] - pz[3]) * (pz[2] - pz[3]));
    if (gap0 < 1.0 && gap1 < 1.0) return 1;                 /* edges meet: nothing to fill */
    uu[0] = 0.0;           vv[0] = (double)si;
    uu[1] = gap0 / lw;     vv[1] = (double)si;
    uu[2] = gap1 / lw;     vv[2] = (double)si + 1.0;
    uu[3] = 0.0;           vv[3] = (double)si + 1.0;
    seg_page = tg_road_page(si);
    moff[(*nmesh)++] = blk->len;
    {
        const size_t p0 = blk->len;
        const int r = tg_write_quad_mesh(blk, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1);
        tg_guard_mark(p0, blk->len, TG_GK_BRANCHROAD, si);
        return r;
    }
}

/* Paint the corridor into the world raster and bed the ground under it, so no street
 * lands on it and the terrain meets the road: the free counterpart of the lateral
 * loop in tg_network_build. */
void tg_pf_paint_network(int fi)
{
    const PfPlan *P = pf_of(fi);
    double w;
    int k;
    if (!P || !P->built) return;
    w = (double)P->lb * pf_lw();
    for (k = 0; k <= P->clen; k++) {
        const TG_Node *c = &P->rview.v[P->F + 1 + k];
        tg_world_occ_disc(c->x, c->z, w * 0.5 + 600.0, TG_WO_DRIVABLE);
        if (k < P->clen) {
            const TG_Node *d = &P->rview.v[P->F + 2 + k];
            tg_world_conform_seg(c->x, c->z, c->y, d->x, d->z, d->y,
                                 w * 0.5 + TD5_TG_ROAD_BED_VERGE, 3500.0);
        }
    }
}

/* [ROUND 1015 E] The first place the ray (ox,oz)+t(ux,uz), tmin < t < tmax, crosses a
 * built plaza corridor's FREE rows; t and the corridor's height there. The far-band
 * apron (td5_tg_terrain.c) pins a ring on that crossing so the ground meets the road
 * instead of lying over it. 0 when no plaza fork is built, which is every other build. */
int tg_pf_ray_hit(double ox, double oz, double ux, double uz, double tmin, double tmax,
                  double *t_out, double *y_out)
{
    int p, any = 0;
    double bt = 1e300, by = 0.0;
    for (p = 0; p < s_nplan; p++) {
        const PfPlan *P = &s_plan[p];
        int k;
        if (!P->used || !P->committed || !P->built) continue;
        for (k = P->k1; k < P->K2c; k++) {
            const TG_Node *a = &P->rview.v[P->F + 1 + k], *b = &P->rview.v[P->F + 2 + k];
            const double ex = b->x - a->x, ez = b->z - a->z;
            const double den = ux * ez - uz * ex;
            double t, u;
            if (fabs(den) < 1e-9) continue;
            t = ((a->x - ox) * ez - (a->z - oz) * ex) / den;
            u = ((a->x - ox) * uz - (a->z - oz) * ux) / den;
            if (t > tmin && t < tmax && u >= 0.0 && u <= 1.0 && t < bt) {
                bt = t; by = a->y + (b->y - a->y) * u; any = 1;
            }
        }
    }
    if (any) { *t_out = bt; *y_out = by; }
    return any;
}

/* The V scale of a free corridor's road texture: its span stretch, clamped. */
double tg_pf_vscale(int fi)
{
    const PfPlan *P = pf_of(fi);
    double v;
    if (!P) return 1.0;
    v = P->pitch > 0.0 ? P->pitch : P->stretch;
    if (v < 0.5) v = 0.5;
    if (v > 4.0) v = 4.0;
    return v;
}

/* Corridor span ck runs beside the road on both its rows (a classic span). */
int tg_pf_classic_span(int fi, int ck)
{
    const PfPlan *P = pf_of(fi);
    if (!P || !P->built) return 0;
    return ck < P->k1 || ck >= P->K2c;
}

/* [ROUND 1015 E] Would the scenery carriageway of an avenue stand ON a plaza corridor?
 * `lat` is that scenery road's centre lateral at node si (the sidecar's, + = left). The
 * corridor leaves the avenue's line to turn onto the ring over the last spans of the
 * run, past the spans the fork owns, so two roads would stack there: ask the corridor's
 * own rows. 1 = a corridor row lies within three lanes of the scenery centre line. */
int tg_pf_scenery_clash(const TG_NodeList *nl, int si, double lat)
{
    int p;
    double x, z;
    if (!nl || si < 0 || si >= nl->count) return 0;
    x = nl->v[si].x + nl->v[si].tz * lat;
    z = nl->v[si].z - nl->v[si].tx * lat;
    for (p = 0; p < s_nplan; p++) {
        const PfPlan *P = &s_plan[p];
        int k;
        if (!P->used || !P->committed || !P->built) continue;
        if (si < P->F + 1 || si > P->F + P->len) continue;
        for (k = P->k1; k <= P->K2c; k++) {
            const TG_Node *c = &P->rview.v[P->F + 1 + k];
            const double dx = c->x - x, dz = c->z - z;
            if (dx * dx + dz * dz < 3.0 * pf_lw() * 3.0 * pf_lw()) return 1;
        }
    }
    return 0;
}
