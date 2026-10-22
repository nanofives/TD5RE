/**
 * td5_roam_greenhouse.c -- GEO FREE ROAM: arched greenhouses on the horticulture belt's footprints (PORT-ONLY,
 * roam PC). Contract and rules: td5_roam_greenhouse.h.
 *
 * SECTIONS
 *   fit      the minimum-area rectangle of a ring over its own edge directions
 *   writer   per sub-block quad batches -> one MODELS.DAT prefab record each
 *   emit     spans, gable ends, side walls
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_trackgen_internal.h"   /* TG_Buf, tg_write_prefab_mesh, TD5_TG_PAGE_* */
#include "td5_config.h"
#include "td5_platform.h"
#include "td5_roam_scenery.h"
#include "td5_roam_greenhouse.h"

#define GH_UPM        430.0
#define GH_M(m)       ((m) * GH_UPM)
#define GH_CHUNK      128
#define GH_PIECE_M    32.0            /* roof / wall pieces along the tunnel at most this long        */
#define GH_EAVE_MIN   1.8
#define GH_SINK       0.25            /* the base this far under the lowest ground sample             */
#define GH_UV_M       4.0             /* one page repeat per 4 m                                      */
#define GH_PAGE       TD5_TG_PAGE_RS_ICE      /* pale blue-white, a light grain (the snow road page) */
#define GH_R          232.0           /* plastic, pale blue-white (before the sun shade)              */
#define GH_G          240.0
#define GH_B          240.0

typedef struct { int nq; float v[GH_CHUNK * 4 * 5]; unsigned int light[GH_CHUNK * 4]; } GhBatch;

struct TD5_RoamGh {
    GhBatch bat[TD5_ROAM_SCN_NSB];
    int oom;
};

int td5_roam_gh_enabled(void) { return td5_env_int("TD5RE_ROAM_GREENHOUSE", 1, 0, 1); }

TD5_RoamGh *td5_roam_gh_begin(void) { return (TD5_RoamGh *)calloc(1, sizeof(TD5_RoamGh)); }
void td5_roam_gh_end(TD5_RoamGh *g) { free(g); }

/* ============================================================================
 * SECTION: fit
 * ========================================================================== */

/* the min-area rectangle: centre (cx, cz), unit long axis (ux, uz), half long / short sides; returns its area */
static double gh_rect(const double *x, const double *z, int n, double *cx, double *cz, double *ux, double *uz,
                      double *hl, double *hs)
{
    double best = 1e300;
    int i, k;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double dx = x[j] - x[i], dz = z[j] - z[i], l = hypot(dx, dz);
        double ax, az, a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300, A;
        if (l < 1e-6) continue;
        ax = dx / l; az = dz / l;
        for (k = 0; k < n; k++) {
            const double a = x[k] * ax + z[k] * az, b = -x[k] * az + z[k] * ax;
            if (a < a0) a0 = a;
            if (a > a1) a1 = a;
            if (b < b0) b0 = b;
            if (b > b1) b1 = b;
        }
        A = (a1 - a0) * (b1 - b0);
        if (A < best) {
            const double ma = 0.5 * (a0 + a1), mb = 0.5 * (b0 + b1);
            best = A;
            *cx = ma * ax - mb * az; *cz = ma * az + mb * ax;
            if (a1 - a0 >= b1 - b0) { *ux = ax; *uz = az; *hl = 0.5 * (a1 - a0); *hs = 0.5 * (b1 - b0); }
            else { *ux = -az; *uz = ax; *hl = 0.5 * (b1 - b0); *hs = 0.5 * (a1 - a0); }
        }
    }
    return best;
}

static double gh_ring_area(const double *x, const double *z, int n)
{
    double a = 0.0;
    int i;
    for (i = 0; i < n; i++) { const int j = (i + 1) % n; a += x[i] * z[j] - x[j] * z[i]; }
    return fabs(a) * 0.5;
}

/* ============================================================================
 * SECTION: writer
 * ========================================================================== */

static int gh_flush_sb(TD5_RoamGh *g, TG_TileCtx *c, int sb, TD5_RoamGhStats *st)
{
    GhBatch *q = &g->bat[sb];
    unsigned short cmd[3];
    TG_Buf *b;
    if (q->nq <= 0) return 1;
    b = (TG_Buf *)roam_ctx_mesh_begin(c, sb);
    if (!b) { g->oom = 1; return 0; }
    cmd[0] = (unsigned short)GH_PAGE; cmd[1] = 0; cmd[2] = (unsigned short)q->nq;
    if (!tg_write_prefab_mesh(b, q->v, q->light, q->nq * 4, cmd, 1, 0, 0.0, 0.0, 0.0, 1.0, 0.0) || b->oom) { g->oom = 1; return 0; }
    st->meshes++;
    q->nq = 0;
    return 1;
}

/* the plastic's colour on a face of unit normal (nx, ny, nz): a fixed sun from the north-west, high */
static unsigned int gh_col(double nx, double ny, double nz)
{
    static const double L[3] = { 0.33, 0.86, 0.39 };
    double d = nx * L[0] + ny * L[1] + nz * L[2], s;
    if (d < 0.0) d = 0.0;
    s = 0.62 + 0.38 * d;
    return 0xFF000000u | ((unsigned)(GH_R * s) << 16) | ((unsigned)(GH_G * s) << 8) | (unsigned)(GH_B * s);
}

/* one quad (tile-local corners in drawing order, uv per corner) into sub-block sb */
static int gh_put(TD5_RoamGh *g, TG_TileCtx *c, int sb, const double *x, const double *y, const double *z,
                  const double *u, const double *v, unsigned int col, TD5_RoamGhStats *st)
{
    GhBatch *q = &g->bat[sb];
    int k;
    if (q->nq >= GH_CHUNK && !gh_flush_sb(g, c, sb, st)) return 0;
    for (k = 0; k < 4; k++) {
        const int o = (q->nq * 4 + k) * 5;
        q->v[o + 0] = (float)x[k]; q->v[o + 1] = (float)y[k]; q->v[o + 2] = (float)z[k];
        q->v[o + 3] = (float)u[k]; q->v[o + 4] = (float)v[k];
        q->light[q->nq * 4 + k] = col;
    }
    q->nq++;
    st->quads++;
    return 1;
}

int td5_roam_gh_flush(TD5_RoamGh *g, TG_TileCtx *c, TD5_RoamGhStats *st)
{
    int sb;
    if (!g) return 1;
    for (sb = 0; sb < TD5_ROAM_SCN_NSB; sb++) if (!gh_flush_sb(g, c, sb, st)) return 0;
    return !g->oom;
}

/* ============================================================================
 * SECTION: emit
 * ========================================================================== */

int td5_roam_gh_try(TD5_RoamGh *g, TG_TileCtx *c, const double *rx, const double *rz, int n,
                    double area_m2, double height_m, int osm_gh, int in_area, TD5_RoamGhStats *st)
{
    double cx = 0.0, cz = 0.0, ux = 1.0, uz = 0.0, hl = 0.0, hs = 0.0, rect, gmin = 1e300, gmax = -1e300, ridge, rise, eave, y0, sw, wx, wz;
    int ns, s, k, p, np, sb;
    if (!g || !(osm_gh || in_area)) return 0;
    if (osm_gh) st->cand_osm++; else st->cand_area++;
    if (!osm_gh && area_m2 < TD5_ROAM_GH_MIN_M2) { st->small++; return 0; }
    if (!osm_gh && height_m > TD5_ROAM_GH_MAX_H) { st->tall++; return 0; }
    if (n < 3) { st->irregular++; return 0; }
    rect = gh_rect(rx, rz, n, &cx, &cz, &ux, &uz, &hl, &hs);
    if (!(rect > 0.0) || gh_ring_area(rx, rz, n) < TD5_ROAM_GH_FILL * rect || hs < GH_M(1.5)) { st->irregular++; return 0; }
    wx = -uz; wz = ux;
    {   /* the rectangle off the drivable surface, and its ground */
        double qx[4], qz[4];
        static const int sa[4] = { -1, 1, 1, -1 }, sw4[4] = { -1, -1, 1, 1 };
        for (k = 0; k < 4; k++) {
            qx[k] = cx + ux * sa[k] * hl + wx * sw4[k] * hs; qz[k] = cz + uz * sa[k] * hl + wz * sw4[k] * hs;
        }
        if (roam_ctx_on_road(c, qx, qz, 4)) { st->road++; return 0; }
        for (k = 0; k < 5; k++) {
            const double gy = k < 4 ? roam_ctx_ground(c, qx[k], qz[k]) : roam_ctx_ground(c, cx, cz);
            if (gy < gmin) gmin = gy;
            if (gy > gmax) gmax = gy;
        }
    }
    ns = (int)floor(2.0 * hs / GH_M(TD5_ROAM_GH_SPAN_M) + 0.5);
    if (ns < 1) ns = 1;
    sw = 2.0 * hs / ns;                                   /* span width, units */
    ridge = (height_m >= 3.0 && height_m <= TD5_ROAM_GH_MAX_H) ? height_m : 4.2;
    rise = ridge - 2.2;
    if (rise > 0.4 * sw / GH_UPM) rise = 0.4 * sw / GH_UPM;
    if (rise < 0.6) rise = 0.6;
    eave = ridge - rise;
    if (eave < GH_EAVE_MIN) eave = GH_EAVE_MIN;
    y0 = gmin - GH_M(GH_SINK);
    np = (int)ceil(2.0 * hl / GH_M(GH_PIECE_M));
    if (np < 1) np = 1;
    sb = roam_ctx_sb_of(c, cx, cz);
    for (s = 0; s < ns; s++) {
        const double wc = -hs + (s + 0.5) * sw;           /* span centre, across */
        double aw[TD5_ROAM_GH_SEG + 1], ay[TD5_ROAM_GH_SEG + 1];
        for (k = 0; k <= TD5_ROAM_GH_SEG; k++) {
            const double th = 3.14159265358979 * k / TD5_ROAM_GH_SEG;
            aw[k] = wc - 0.5 * sw * cos(th);
            ay[k] = gmax + GH_M(eave + rise * sin(th));
        }
        for (k = 0; k < TD5_ROAM_GH_SEG; k++) {
            /* roof strip k, along the tunnel in np pieces; normal in the (w, y) plane */
            const double th = 3.14159265358979 * (k + 0.5) / TD5_ROAM_GH_SEG;
            double nw = -rise * cos(th) * GH_UPM, ny = 0.5 * sw * sin(th), l = hypot(nw, ny);
            unsigned int col;
            nw /= l; ny /= l;
            col = gh_col(wx * nw, ny, wz * nw);
            for (p = 0; p < np; p++) {
                const double a0 = -hl + 2.0 * hl * p / np, a1 = -hl + 2.0 * hl * (p + 1) / np;
                const double x[4] = { cx + ux * a0 + wx * aw[k], cx + ux * a0 + wx * aw[k + 1], cx + ux * a1 + wx * aw[k + 1], cx + ux * a1 + wx * aw[k] };
                const double z[4] = { cz + uz * a0 + wz * aw[k], cz + uz * a0 + wz * aw[k + 1], cz + uz * a1 + wz * aw[k + 1], cz + uz * a1 + wz * aw[k] };
                const double y[4] = { ay[k], ay[k + 1], ay[k + 1], ay[k] };
                const double u[4] = { (double)k, (double)(k + 1), (double)(k + 1), (double)k };
                const double v[4] = { a0 / GH_M(GH_UV_M), a0 / GH_M(GH_UV_M), a1 / GH_M(GH_UV_M), a1 / GH_M(GH_UV_M) };
                if (!gh_put(g, c, sb, x, y, z, u, v, col, st)) return -1;
            }
            {   /* the gable strips under it, both ends */
                int end;
                for (end = -1; end <= 1; end += 2) {
                    const double a = end * hl;
                    const double x[4] = { cx + ux * a + wx * aw[k], cx + ux * a + wx * aw[k + 1], cx + ux * a + wx * aw[k + 1], cx + ux * a + wx * aw[k] };
                    const double z[4] = { cz + uz * a + wz * aw[k], cz + uz * a + wz * aw[k + 1], cz + uz * a + wz * aw[k + 1], cz + uz * a + wz * aw[k] };
                    const double y[4] = { ay[k], ay[k + 1], y0, y0 };
                    const double u[4] = { aw[k] / GH_M(GH_UV_M), aw[k + 1] / GH_M(GH_UV_M), aw[k + 1] / GH_M(GH_UV_M), aw[k] / GH_M(GH_UV_M) };
                    const double v[4] = { (ay[k] - y0) / GH_M(GH_UV_M), (ay[k + 1] - y0) / GH_M(GH_UV_M), 0.0, 0.0 };
                    if (!gh_put(g, c, sb, x, y, z, u, v, gh_col(ux * end, 0.0, uz * end), st)) return -1;
                }
            }
        }
    }
    {   /* the two long side walls, base to eave */
        int side;
        for (side = -1; side <= 1; side += 2)
            for (p = 0; p < np; p++) {
                const double a0 = -hl + 2.0 * hl * p / np, a1 = -hl + 2.0 * hl * (p + 1) / np, w = side * hs;
                const double ye = gmax + GH_M(eave);
                const double x[4] = { cx + ux * a0 + wx * w, cx + ux * a1 + wx * w, cx + ux * a1 + wx * w, cx + ux * a0 + wx * w };
                const double z[4] = { cz + uz * a0 + wz * w, cz + uz * a1 + wz * w, cz + uz * a1 + wz * w, cz + uz * a0 + wz * w };
                const double y[4] = { y0, y0, ye, ye };
                const double u[4] = { a0 / GH_M(GH_UV_M), a1 / GH_M(GH_UV_M), a1 / GH_M(GH_UV_M), a0 / GH_M(GH_UV_M) };
                const double v[4] = { 0.0, 0.0, (ye - y0) / GH_M(GH_UV_M), (ye - y0) / GH_M(GH_UV_M) };
                if (!gh_put(g, c, sb, x, y, z, u, v, gh_col(wx * side, 0.0, wz * side), st)) return -1;
            }
    }
    st->drawn++;
    st->spans += ns;
    st->m2 += rect / (GH_UPM * GH_UPM);
    return 1;
}
