/**
 * td5_roam_lap.c -- GEO FREE ROAM: lap counting on a closed route polyline (PORT-ONLY, F9a).
 * Contract: td5_roam_lap.h.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_roam_lap.h"

#define LAP_BACK_M   40.0     /* search window behind the last answer, along the route */
#define LAP_AHEAD_M  150.0

static double lap_seglen(const TD5_RoamLoop *lp, int i)
{
    return i + 1 < lp->n ? lp->d[i + 1] - lp->d[i] : lp->L - lp->d[lp->n - 1];
}

int td5_roam_loop_init(TD5_RoamLoop *lp, const double *px, const double *pz, const double *dist_m, int npt, double upm)
{
    int i;
    memset(lp, 0, sizeof *lp);
    if (!px || !pz || npt < 3 || upm <= 0.0) return 0;
    lp->px = (double *)malloc((size_t)npt * sizeof(double));
    lp->pz = (double *)malloc((size_t)npt * sizeof(double));
    lp->d = (double *)malloc((size_t)npt * sizeof(double));
    if (!lp->px || !lp->pz || !lp->d) { td5_roam_loop_free(lp); return 0; }
    memcpy(lp->px, px, (size_t)npt * sizeof(double));
    memcpy(lp->pz, pz, (size_t)npt * sizeof(double));
    lp->n = npt;
    lp->upm = upm;
    lp->d[0] = 0.0;
    for (i = 1; i < npt; i++)
        lp->d[i] = lp->d[i - 1] + hypot(px[i] - px[i - 1], pz[i] - pz[i - 1]) / upm;
    (void)dist_m;                       /* always re-measured: the closing seam must add up exactly */
    lp->L = lp->d[npt - 1] + hypot(px[0] - px[npt - 1], pz[0] - pz[npt - 1]) / upm;
    if (lp->L < 20.0) { td5_roam_loop_free(lp); return 0; }
    return 1;
}

void td5_roam_loop_free(TD5_RoamLoop *lp)
{
    free(lp->px); free(lp->pz); free(lp->d);
    memset(lp, 0, sizeof *lp);
}

void td5_roam_loop_dir(const TD5_RoamLoop *lp, int seg, double *ux, double *uz)
{
    const int j = (seg + 1) % lp->n;
    const double sx = lp->px[j] - lp->px[seg], sz = lp->pz[j] - lp->pz[seg], l = sqrt(sx * sx + sz * sz);
    *ux = l > 0.0 ? sx / l : 0.0;
    *uz = l > 0.0 ? sz / l : 1.0;
}

void td5_roam_lap_reset(TD5_LapTrack *t) { memset(t, 0, sizeof *t); }

/* nearest point of segment i to (x, z): squared distance (units), wrapped arc length (m) */
static double lap_probe(const TD5_RoamLoop *lp, int i, double x, double z, double *arc)
{
    const int j = (i + 1) % lp->n;
    const double sx = lp->px[j] - lp->px[i], sz = lp->pz[j] - lp->pz[i], l2 = sx * sx + sz * sz;
    double t = l2 > 0.0 ? ((x - lp->px[i]) * sx + (z - lp->pz[i]) * sz) / l2 : 0.0, qx, qz;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    qx = lp->px[i] + sx * t; qz = lp->pz[i] + sz * t;
    *arc = lp->d[i] + t * lap_seglen(lp, i);
    return (qx - x) * (qx - x) + (qz - z) * (qz - z);
}

int td5_roam_lap_update(const TD5_RoamLoop *lp, TD5_LapTrack *t, double x, double z, double lost_m)
{
    double best = 1e300, arc = 0.0;
    int i, bi = -1;
    if (!lp || !lp->n) return 0;
    if (!t->init) {
        for (i = 0; i < lp->n; i++) {
            double a, d2 = lap_probe(lp, i, x, z, &a);
            if (d2 < best) { best = d2; bi = i; arc = a; }
        }
        t->lat_m = sqrt(best) / lp->upm;
        t->seg = bi < 0 ? 0 : bi;
        t->wrapped = arc;
        t->total = arc > 0.5 * lp->L ? arc - lp->L : arc;       /* behind the line = the grid */
        t->max_total = t->total;
        t->init = 1;
        t->lost = 0;
        return 1;
    }
    {
        /* walk back to LAP_BACK_M behind the last answer's segment, then forward over the window */
        int s0 = t->seg, k;
        double back = 0.0, span;
        for (k = 0; k < lp->n - 1 && back < LAP_BACK_M; k++) {
            const int p = (s0 - 1 + lp->n) % lp->n;
            back += lap_seglen(lp, p);
            s0 = p;
        }
        span = back + LAP_AHEAD_M + lap_seglen(lp, t->seg);
        {
            double acc = 0.0;
            for (k = 0, i = s0; k < lp->n && acc <= span; k++, i = (i + 1) % lp->n) {
                double a, d2 = lap_probe(lp, i, x, z, &a);
                if (d2 < best) { best = d2; bi = i; arc = a; }
                acc += lap_seglen(lp, i);
            }
        }
    }
    t->lat_m = sqrt(best) / lp->upm;
    if (bi < 0 || t->lat_m > lost_m) { t->lost = 1; return 0; }
    t->lost = 0;
    {
        double delta = arc - t->wrapped;
        if (delta > 0.5 * lp->L) delta -= lp->L;
        else if (delta < -0.5 * lp->L) delta += lp->L;
        t->total += delta;
        t->wrapped = arc;
        t->seg = bi;
        if (t->total > t->max_total) t->max_total = t->total;
    }
    return 1;
}

int td5_roam_lap_count(const TD5_RoamLoop *lp, const TD5_LapTrack *t, double eps_m)
{
    const double v = t->max_total + eps_m;
    if (!lp || !lp->n || v < lp->L) return 0;
    return (int)floor(v / lp->L);
}
