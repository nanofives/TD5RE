/**
 * td5_roam_groundfix.c -- the ground held at or under the road, shared by the renderer and the scenery.
 * See td5_roam_groundfix.h. The clamp is the G4 code of td5_roam_render.c, moved here unchanged.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_config.h"
#include "td5_roam_groundfix.h"

#define GF_MAX_LANES 16     /* = RR_MAX_LANES of td5_roam_render.c */
#define GF_MARGIN    3      /* cells of lattice kept round the tile */

int td5_roam_gfix_cell_m(void)
{
    int m = td5_env_int("TD5RE_ROAM_RENDER_CELL_M", 16, 8, 64);
    if (64 % m != 0) m = 16;
    return m;
}

int td5_roam_gfix_enabled(void) { return !td5_env_flag_off("TD5RE_ROAM_GROUND_NOFIX"); }

static int gf_span_ok(const RoamTile *t, const TD5_StripSpan *sp, uint32_t *L, uint32_t *R, int *n)
{
    *L = sp->left_vertex_index; *R = sp->right_vertex_index;
    *n = (int)(sp->pad_02[1] & 0x0Fu);
    if (*n < 1 || *n >= GF_MAX_LANES) return 0;
    if (*L + (uint32_t)*n >= t->nvert || *R + (uint32_t)*n >= t->nvert) return 0;
    return 1;
}

/* clip polygon p (n points of u, v, y) to coordinate `ax` >= c (ge) or <= c; returns the count */
static int gf_clip_half(const double (*p)[3], int n, double (*o)[3], int ax, double c, int ge)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const double *a = p[i], *b = p[(i + 1) % n];
        const double da = ge ? a[ax] - c : c - a[ax], db = ge ? b[ax] - c : c - b[ax];
        if (da >= 0.0) { o[m][0] = a[0]; o[m][1] = a[1]; o[m][2] = a[2]; m++; }
        if ((da >= 0.0) != (db >= 0.0)) {
            const double f = da / (da - db);
            o[m][0] = a[0] + f * (b[0] - a[0]); o[m][1] = a[1] + f * (b[1] - a[1]); o[m][2] = a[2] + f * (b[2] - a[2]);
            m++;
        }
    }
    return m;
}

/* lowest road height of triangle q (place x, y, z) inside each ground cell it meets */
static void gf_clamp_tri(const RoamGrid *g, const double q[3][3], double *cm, int I0, int J0, int N, double cs)
{
    double p[3][3], lo[2] = { 1e300, 1e300 }, hi[2] = { -1e300, -1e300 };
    int k, I, J, i0, i1, j0, j1;
    for (k = 0; k < 3; k++) {
        roam_grid_uv(g, q[k][0], q[k][2], &p[k][0], &p[k][1]);
        p[k][0] /= cs; p[k][1] /= cs; p[k][2] = q[k][1];
        if (p[k][0] < lo[0]) lo[0] = p[k][0];
        if (p[k][0] > hi[0]) hi[0] = p[k][0];
        if (p[k][1] < lo[1]) lo[1] = p[k][1];
        if (p[k][1] > hi[1]) hi[1] = p[k][1];
    }
    i0 = (int)floor(lo[0]); i1 = (int)floor(hi[0]); j0 = (int)floor(lo[1]); j1 = (int)floor(hi[1]);
    if (i0 < I0) i0 = I0;
    if (j0 < J0) j0 = J0;
    if (i1 > I0 + N - 1) i1 = I0 + N - 1;
    if (j1 > J0 + N - 1) j1 = J0 + N - 1;
    for (J = j0; J <= j1; J++) for (I = i0; I <= i1; I++) {
        double a[12][3], b[12][3], y = 1e300;
        int n = gf_clip_half((const double (*)[3])p, 3, a, 0, (double)I, 1);
        n = gf_clip_half((const double (*)[3])a, n, b, 0, (double)(I + 1), 0);
        n = gf_clip_half((const double (*)[3])b, n, a, 1, (double)J, 1);
        n = gf_clip_half((const double (*)[3])a, n, b, 1, (double)(J + 1), 0);
        for (k = 0; k < n; k++) if (b[k][2] < y) y = b[k][2];
        if (n > 0 && y < cm[(J - J0) * N + (I - I0)]) cm[(J - J0) * N + (I - I0)] = y;
    }
}

void td5_roam_gfix_cells(const RoamGrid *g, const RoamTile *const nb[9], int I0, int J0, int N, double cs, double *cm)
{
    int k, i;
    for (i = 0; i < N * N; i++) cm[i] = 1e300;
    for (k = 0; k < 9; k++) {
        const RoamTile *n = nb[k];
        uint8_t *bore;
        uint32_t e, q, si;
        if (!n) continue;
        bore = (uint8_t *)calloc(n->nspan ? n->nspan : 1u, 1);
        if (!bore) continue;
        for (e = 0; e < n->nedge; e++)                 /* a tunnel road is under the ground on purpose */
            if (n->edge[e].flags & ROAM_EF_BORE)
                for (q = 0; q < n->edge[e].span_count; q++)
                    if ((uint32_t)n->edge[e].span_first + q < n->nspan) bore[n->edge[e].span_first + q] = 1;
        for (si = 0; si < n->nspan; si++) {
            const TD5_StripSpan *sp = &n->span[si];
            uint32_t L, R; int ln, kk;
            if (bore[si] || !gf_span_ok(n, sp, &L, &R, &ln)) continue;
            for (kk = 0; kk < ln; kk++) {          /* the lane quad as the renderer fans it: 0 1 2, 0 2 3 */
                const TD5_StripVertex *v4[4] = { &n->vert[L + kk], &n->vert[L + kk + 1], &n->vert[R + kk + 1], &n->vert[R + kk] };
                double pq[4][3], tri[3][3];
                int c;
                for (c = 0; c < 4; c++) {
                    pq[c][0] = (double)n->org_x + (double)sp->origin_x + (double)v4[c]->x;
                    pq[c][1] = (double)sp->origin_y + (double)v4[c]->y;
                    pq[c][2] = (double)n->org_z + (double)sp->origin_z + (double)v4[c]->z;
                }
                memcpy(tri[0], pq[0], sizeof tri[0]); memcpy(tri[1], pq[1], sizeof tri[1]); memcpy(tri[2], pq[2], sizeof tri[2]);
                gf_clamp_tri(g, (const double (*)[3])tri, cm, I0, J0, N, cs);
                memcpy(tri[1], pq[2], sizeof tri[1]); memcpy(tri[2], pq[3], sizeof tri[2]);
                gf_clamp_tri(g, (const double (*)[3])tri, cm, I0, J0, N, cs);
            }
        }
        for (si = 0; si < n->ntri; si++) {
            const RoamTri *tr = &n->tri[si];
            double tri[3][3];
            int c;
            if (tr->v[0] >= n->nvert || tr->v[1] >= n->nvert || tr->v[2] >= n->nvert) continue;
            for (c = 0; c < 3; c++) {
                tri[c][0] = (double)n->org_x + (double)tr->ox + (double)n->vert[tr->v[c]].x;
                tri[c][1] = (double)tr->oy + (double)n->vert[tr->v[c]].y;
                tri[c][2] = (double)n->org_z + (double)tr->oz + (double)n->vert[tr->v[c]].z;
            }
            gf_clamp_tri(g, (const double (*)[3])tri, cm, I0, J0, N, cs);
        }
        free(bore);
    }
}

void td5_roam_gfix_point(const RoamGrid *g, int tx, int tz, int i, int j, int G, double *px, double *pz)
{
    const double S = (double)g->tile;
    const double u = ((double)tx + (double)i / (double)G) * S;
    const double v = ((double)tz + (double)j / (double)G) * S;
    *px = g->ox + g->c * u - g->s * v;
    *pz = g->oz + g->s * u + g->c * v;
}

int td5_roam_gfix_build(TD5_RoamGfix *f, const RoamGrid *g, const RoamTile *const nb[9], int tx, int tz, TD5_RoamHeight *hc)
{
    int G, N, W, i, j;
    double *cm;
    long W0;
    memset(f, 0, sizeof *f);
    if (!hc) return 0;
    G = 256 / td5_roam_gfix_cell_m();
    f->grid = *g; f->G = G; f->tx = tx; f->tz = tz;
    f->cs = (double)g->tile / (double)G;
    f->lo = -GF_MARGIN + 1;                 /* lattice points lo .. G + GF_MARGIN - 1 */
    f->np = G + 2 * GF_MARGIN - 1;
    f->h = (double *)malloc((size_t)f->np * (size_t)f->np * sizeof(double));
    if (!f->h) { memset(f, 0, sizeof *f); return 0; }
    W0 = (long)f->np * (long)f->np;
    for (j = 0; j < f->np; j++) for (i = 0; i < f->np; i++) {
        double x, z, h;
        td5_roam_gfix_point(g, tx, tz, f->lo + i, f->lo + j, G, &x, &z);
        f->h[j * f->np + i] = td5_roam_height_sample(hc, x, z, &h, NULL) ? h : NAN;
    }
    if (!td5_roam_gfix_enabled()) { for (i = 0; i < W0; i++) f->h[i] = 0.0; return 1; }
    /* cells tx*G - GF_MARGIN .. tx*G + G + GF_MARGIN - 1 */
    N = G + 2 * GF_MARGIN; W = f->np;
    cm = (double *)malloc((size_t)N * (size_t)N * sizeof(double));
    if (!cm) { for (i = 0; i < W0; i++) f->h[i] = 0.0; return 1; }
    td5_roam_gfix_cells(g, nb, tx * G - GF_MARGIN, tz * G - GF_MARGIN, N, f->cs, cm);
    for (j = 0; j < W; j++) for (i = 0; i < W; i++) {
        /* lattice point lo + i has the cells local i and i + 1 of the N window (cell -GF_MARGIN + k) */
        double cap = cm[j * N + i], *h = &f->h[j * W + i];
        if (cm[j * N + i + 1] < cap) cap = cm[j * N + i + 1];
        if (cm[(j + 1) * N + i] < cap) cap = cm[(j + 1) * N + i];
        if (cm[(j + 1) * N + i + 1] < cap) cap = cm[(j + 1) * N + i + 1];
        *h = (!isnan(*h) && cap < *h) ? *h - cap : 0.0;      /* how far the clamp lowers this point */
    }
    free(cm);
    return 1;
}

void td5_roam_gfix_free(TD5_RoamGfix *f) { free(f->h); memset(f, 0, sizeof *f); }

int td5_roam_gfix_lowering(const TD5_RoamGfix *f, double x, double z, double *y)
{
    double u, v, fu, fv, h00, h10, h11, h01;
    const double S = (double)f->grid.tile;
    int i, j, W = f->np;
    if (!f->h) return 0;
    roam_grid_uv(&f->grid, x, z, &u, &v);
    fu = (u / S - (double)f->tx) * (double)f->G; fv = (v / S - (double)f->tz) * (double)f->G;
    i = (int)floor(fu); j = (int)floor(fv);
    if (i < f->lo || j < f->lo || i + 1 >= f->lo + f->np || j + 1 >= f->lo + f->np) return 0;
    fu -= (double)i; fv -= (double)j;
    i -= f->lo; j -= f->lo;
    h00 = f->h[j * W + i]; h10 = f->h[j * W + i + 1]; h11 = f->h[(j + 1) * W + i + 1]; h01 = f->h[(j + 1) * W + i];
    *y = (fu >= fv) ? h00 + fu * (h10 - h00) + fv * (h11 - h10) : h00 + fv * (h01 - h00) + fu * (h11 - h01);
    return 1;
}
