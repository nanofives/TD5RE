/**
 * td5_roam_groundfix.c -- the ground held at or under the road, shared by the renderer and the scenery.
 * See td5_roam_groundfix.h. The G4 cell clamp is the G4 code of td5_roam_render.c, moved here unchanged;
 * [G5] the per-triangle lowering (td5_roam_gfix_lower) replaces it by default.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "td5_config.h"
#include "td5_roam_groundfix.h"
#include "td5_roam_cut.h"

#define GF_MAX_LANES 16     /* = RR_MAX_LANES of td5_roam_render.c */
#define GF_MARGIN    3      /* cells of lattice kept round the tile */

int td5_roam_gfix_cell_m(void)
{
    int m = td5_env_int("TD5RE_ROAM_RENDER_CELL_M", 16, 8, 64);
    if (64 % m != 0) m = 16;
    return m;
}

int td5_roam_gfix_enabled(void) { return !td5_env_flag_off("TD5RE_ROAM_GROUND_NOFIX"); }

int td5_roam_gfix_g4(void) { return td5_env_flag_off("TD5RE_ROAM_GROUND_G4"); }

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

/* clip polygon p to (u - v) >= c (ge) or <= c */
static int gf_clip_diag(const double (*p)[3], int n, double (*o)[3], double c, int ge)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const double *a = p[i], *b = p[(i + 1) % n];
        const double da = ge ? (a[0] - a[1]) - c : c - (a[0] - a[1]), db = ge ? (b[0] - b[1]) - c : c - (b[0] - b[1]);
        if (da >= 0.0) { o[m][0] = a[0]; o[m][1] = a[1]; o[m][2] = a[2]; m++; }
        if ((da >= 0.0) != (db >= 0.0)) {
            const double f = da / (da - db);
            o[m][0] = a[0] + f * (b[0] - a[0]); o[m][1] = a[1] + f * (b[1] - a[1]); o[m][2] = a[2] + f * (b[2] - a[2]);
            m++;
        }
    }
    return m;
}

/* [G5] One road triangle q (place x, y, z) against every ground triangle it meets. The drawn ground cell
 * (I, J) is the fan (I,J) (I+1,J) (I+1,J+1) where u - I >= v - J, else (I,J) (I+1,J+1) (I,J+1) (the
 * renderer's quad 0 1 2, 0 2 3). Over the piece C of the road triangle inside a ground triangle D both the
 * road and the raster ground are linear, so the ground is at or under the road on C when it is at each
 * corner q of C. At a corner where the raster ground stands v = sum w_k r_k - y_q > 0 over the road (w the
 * barycentric weights of q in D, r the raster at D's corners), lowering corner k by
 *     d_k = v w_k / (w . w)
 * gives sum w_k d_k = v: the ground meets the road there. Each corner keeps the LARGEST d any (D, q) asks
 * of it, and lowering a corner never lifts the ground anywhere, so every constraint holds at once. The
 * lowering stays where the road is (d_k <= 1.37 v, and ~0 at a corner far from q): beside a steep road a
 * point comes down to the road level next to it, not to the lowest road height of the 32 m round it
 * (G4), and in a cut the bank beside the road keeps its height. Every term is a function of the triangle,
 * the raster at the GLOBAL lattice points and those points, so both tiles of a seam compute the same
 * lowering (to the raster's own sampling, which the census shows is the same in the float vertex). */
typedef struct { const RoamGrid *g; const double *r; double *low; int I0, J0, W; double cs; double dbg[3];
                 double vcap; } GfLowU;                 /* [roam HC] vcap: the most one constraint may ask */

static void gf_low_tri(const GfLowU *cu, const double q[3][3])
{
    const RoamGrid *g = cu->g;
    const int I0 = cu->I0, J0 = cu->J0, W = cu->W, RW = W + 2;
    const double cs = cu->cs;
    double p[3][3], lo[2] = { 1e300, 1e300 }, hi[2] = { -1e300, -1e300 };
    int k, I, J, i0, i1, j0, j1, side;
#define GF_R(PI, PJ) cu->r[((PJ) - (J0 - 1)) * RW + ((PI) - (I0 - 1))]
    for (k = 0; k < 3; k++) {
        roam_grid_uv(g, q[k][0], q[k][2], &p[k][0], &p[k][1]);
        p[k][0] /= cs; p[k][1] /= cs; p[k][2] = q[k][1];
        if (p[k][0] < lo[0]) lo[0] = p[k][0];
        if (p[k][0] > hi[0]) hi[0] = p[k][0];
        if (p[k][1] < lo[1]) lo[1] = p[k][1];
        if (p[k][1] > hi[1]) hi[1] = p[k][1];
    }
    i0 = (int)floor(lo[0]); i1 = (int)floor(hi[0]); j0 = (int)floor(lo[1]); j1 = (int)floor(hi[1]);
    if (i0 < I0 - 1) i0 = I0 - 1;                 /* the cells round lattice points I0 .. I0 + W - 1 */
    if (j0 < J0 - 1) j0 = J0 - 1;
    if (i1 > I0 + W - 1) i1 = I0 + W - 1;
    if (j1 > J0 + W - 1) j1 = J0 + W - 1;
    for (J = j0; J <= j1; J++) for (I = i0; I <= i1; I++) {
        const double r00 = GF_R(I, J), r10 = GF_R(I + 1, J), r11 = GF_R(I + 1, J + 1), r01 = GF_R(I, J + 1);
        if (isnan(r00) || isnan(r10) || isnan(r11) || isnan(r01)) continue;     /* a cell the renderer leaves out */
        for (side = 0; side < 2; side++) {
            static const int du[2][3] = { { 0, 1, 1 }, { 0, 1, 0 } }, dv[2][3] = { { 0, 0, 1 }, { 0, 1, 1 } };
            const double rc[3] = { r00, side == 0 ? r10 : r11, side == 0 ? r11 : r01 };
            double a[12][3], b[12][3];
            int n = gf_clip_half((const double (*)[3])p, 3, a, 0, (double)I, 1), c;
            n = gf_clip_half((const double (*)[3])a, n, b, 0, (double)(I + 1), 0);
            n = gf_clip_half((const double (*)[3])b, n, a, 1, (double)J, 1);
            n = gf_clip_half((const double (*)[3])a, n, b, 1, (double)(J + 1), 0);
            n = gf_clip_diag((const double (*)[3])b, n, a, (double)(I - J), side == 0);
            for (k = 0; k < n; k++) {
                const double fu = a[k][0] - (double)I, fv = a[k][1] - (double)J;
                double w[3], v, ww;
                if (side == 0) { w[0] = 1.0 - fu; w[1] = fu - fv; w[2] = fv; }
                else           { w[0] = 1.0 - fv; w[1] = fu;      w[2] = fv - fu; }
                for (c = 0; c < 3; c++) if (w[c] < 0.0) w[c] = 0.0;
                v = w[0] * rc[0] + w[1] * rc[1] + w[2] * rc[2] - a[k][2];
                ww = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
                if (!(v > 0.0) || !(ww > 0.0)) continue;
                if (v > cu->vcap) v = cu->vcap;
                for (c = 0; c < 3; c++) {
                    const int PI = I + du[side][c], PJ = J + dv[side][c];
                    double d, *L;
                    if (PI < I0 || PJ < J0 || PI >= I0 + W || PJ >= J0 + W) continue;
                    d = v * w[c] / ww;
                    L = &cu->low[(PJ - J0) * W + (PI - I0)];
                    if (d > *L) {
#ifndef TD5RE_RELEASE
                        if (cu->dbg[2] > 0.0) {
                            const double px = g->ox + g->c * (PI * cs) - g->s * (PJ * cs);
                            const double pz = g->oz + g->s * (PI * cs) + g->c * (PJ * cs);
                            if (hypot(px - cu->dbg[0], pz - cu->dbg[1]) < cu->dbg[2])
                                fprintf(stderr, "GFIXDBG pt %d,%d (%.0f %.0f) cell %d,%d side %d low %.0f -> %.0f  v %.0f w %.2f %.2f %.2f q (%.0f %.0f y%.0f) tri (%.0f %.0f %.0f)(%.0f %.0f %.0f)(%.0f %.0f %.0f)\n",
                                        PI, PJ, px, pz, I, J, side, *L, d, v, w[0], w[1], w[2], a[k][0], a[k][1], a[k][2],
                                        q[0][0], q[0][1], q[0][2], q[1][0], q[1][1], q[1][2], q[2][0], q[2][1], q[2][2]);
                        }
#endif
                        *L = d;
                    }
                }
            }
        }
    }
#undef GF_R
}

/* the drivable triangles of the 3x3 ROAD tiles (lane quads fanned 0 1 2, 0 2 3; junction patch triangles;
 * tunnel bores left out), each handed to fn */
typedef void (*GfTriFn)(void *u, const double tri[3][3]);

static void gf_road_tris(const RoamTile *const nb[9], TD5_RoamHeight *hc, GfTriFn fn, void *u)
{
    int k;
    for (k = 0; k < 9; k++) {
        const RoamTile *n = nb[k];
        uint8_t *bore;
        uint32_t e, q, si;
        if (!n) continue;
        bore = (uint8_t *)calloc(n->nspan ? n->nspan : 1u, 1);
        if (!bore) continue;
        /* a tunnel road (and [roam NC] a road in a cutting: its walls hold the ground) is under the ground on purpose */
        td5_roam_cut_spans(n, hc, bore);
        (void)e; (void)q;
        for (si = 0; si < n->nspan; si++) {
            const TD5_StripSpan *sp = &n->span[si];
            uint32_t L, R; int ln, kk;
            if (bore[si] || !gf_span_ok(n, sp, &L, &R, &ln)) continue;
            for (kk = 0; kk < ln; kk++) {
                const TD5_StripVertex *v4[4] = { &n->vert[L + kk], &n->vert[L + kk + 1], &n->vert[R + kk + 1], &n->vert[R + kk] };
                double pq[4][3], tri[3][3];
                int c;
                for (c = 0; c < 4; c++) {
                    pq[c][0] = (double)n->org_x + (double)sp->origin_x + (double)v4[c]->x;
                    pq[c][1] = (double)sp->origin_y + (double)v4[c]->y;
                    pq[c][2] = (double)n->org_z + (double)sp->origin_z + (double)v4[c]->z;
                }
                memcpy(tri[0], pq[0], sizeof tri[0]); memcpy(tri[1], pq[1], sizeof tri[1]); memcpy(tri[2], pq[2], sizeof tri[2]);
                fn(u, (const double (*)[3])tri);
                memcpy(tri[1], pq[2], sizeof tri[1]); memcpy(tri[2], pq[3], sizeof tri[2]);
                fn(u, (const double (*)[3])tri);
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
            fn(u, (const double (*)[3])tri);
        }
        free(bore);
    }
}

/* [roam HC] THE VERGE. Holding the ground under the road is not enough: the road sits up to 4.7 m under the
 * raster (grade smoothing, end blends, a junction patch's single cluster height), the lowering stops at the
 * kerb, and the 16 m ground cell climbs straight back to the raster beside it, so at a junction the lawn and
 * the ground next to a patch stood over the road and hid it from a driver's eye ("grass overlaps with the
 * road"). A band GF_VERGE_W wide outside every kerb edge is now one more constraint of the same lowering:
 * there the ground may rise at most GF_VERGE_GRADE above the road edge's height, measured from the kerb
 * (two triangles per edge, y = edge y + grade x distance, the same linear-target rule as a road triangle).
 * Bores (the road is under the ground on purpose) and bridge-deck sides (the deck is far over the ground)
 * get no verge. TD5RE_ROAM_GROUND_VERGE=0 turns it off (before / after). */
#define GF_VERGE_W      (8.0 * 430.0)
#define GF_VERGE_GRADE  0.08
/* The verge brings the ground down by at most this much: where two road levels meet (a road in a cut beside
 * a higher one) an unbounded verge of the low road pulled the ground metres under the high road's kerb
 * (census: kerb hang > 4 m went from 18 to 299 samples). The rise it exists to remove was <= 1.75 m. */
#define GF_VERGE_VCAP   (2.0 * 430.0)

int td5_roam_gfix_verge(void) { return td5_env_int("TD5RE_ROAM_GROUND_VERGE", 1, 0, 1); }

static void gf_verge_tris(const RoamTile *const nb[9], TD5_RoamHeight *hc, GfTriFn fn, void *u)
{
    int k;
    for (k = 0; k < 9; k++) {
        const RoamTile *n = nb[k];
        uint8_t *bore;
        uint32_t e, q, bi;
        if (!n) continue;
        bore = (uint8_t *)calloc(n->nspan ? n->nspan : 1u, 1);
        if (!bore) continue;
        td5_roam_cut_spans(n, hc, bore);        /* [roam NC] bores + cuttings: no verge */
        (void)e; (void)q;
        for (bi = 0; bi < n->nbedge; bi++) {
            const RoamBEdge *be = &n->bedge[bi];
            const uint32_t p = (uint32_t)(be->prim & ~ROAM_PRIM_TRI);
            double a[3], b[3], in[2], ox, oy, oz, nx, nz, l, s, tri[3][3];
            int c;
            if (be->kind != ROAM_BEDGE_KERB || be->v[0] >= n->nvert || be->v[1] >= n->nvert) continue;
            if (be->prim & ROAM_PRIM_TRI) {                    /* inside = the triangle's third vertex */
                const RoamTri *tr;
                if (p >= n->ntri) continue;
                tr = &n->tri[p];
                ox = (double)n->org_x + tr->ox; oy = (double)tr->oy; oz = (double)n->org_z + tr->oz;
                in[0] = in[1] = 0.0;
                for (c = 0; c < 3; c++) { in[0] += n->vert[tr->v[c]].x; in[1] += n->vert[tr->v[c]].z; }
                in[0] = ox + in[0] / 3.0; in[1] = oz + in[1] / 3.0;
            } else {                                           /* inside = the span quad's centre */
                const TD5_StripSpan *sp;
                uint32_t L, R; int ln;
                if (p >= n->nspan || bore[p]) continue;
                sp = &n->span[p];
                if (!gf_span_ok(n, sp, &L, &R, &ln)) continue;
                ox = (double)n->org_x + sp->origin_x; oy = (double)sp->origin_y; oz = (double)n->org_z + sp->origin_z;
                in[0] = ox + 0.25 * ((double)n->vert[L].x + n->vert[L + ln].x + n->vert[R].x + n->vert[R + ln].x);
                in[1] = oz + 0.25 * ((double)n->vert[L].z + n->vert[L + ln].z + n->vert[R].z + n->vert[R + ln].z);
            }
            a[0] = ox + n->vert[be->v[0]].x; a[1] = oy + n->vert[be->v[0]].y; a[2] = oz + n->vert[be->v[0]].z;
            b[0] = ox + n->vert[be->v[1]].x; b[1] = oy + n->vert[be->v[1]].y; b[2] = oz + n->vert[be->v[1]].z;
            nx = b[2] - a[2]; nz = -(b[0] - a[0]); l = hypot(nx, nz);
            if (l < 1.0) continue;
            nx /= l; nz /= l;
            s = (0.5 * (a[0] + b[0]) - in[0]) * nx + (0.5 * (a[2] + b[2]) - in[1]) * nz;
            if (s < 0.0) { nx = -nx; nz = -nz; }               /* point away from the road */
            {
                const double rise = GF_VERGE_W * GF_VERGE_GRADE;
                const double A[3] = { a[0], a[1], a[2] }, B[3] = { b[0], b[1], b[2] };
                const double Bo[3] = { b[0] + nx * GF_VERGE_W, b[1] + rise, b[2] + nz * GF_VERGE_W };
                const double Ao[3] = { a[0] + nx * GF_VERGE_W, a[1] + rise, a[2] + nz * GF_VERGE_W };
                memcpy(tri[0], A, sizeof A); memcpy(tri[1], B, sizeof B); memcpy(tri[2], Bo, sizeof Bo);
                fn(u, (const double (*)[3])tri);
                memcpy(tri[1], Bo, sizeof Bo); memcpy(tri[2], Ao, sizeof Ao);
                fn(u, (const double (*)[3])tri);
            }
        }
        free(bore);
    }
}

static void gf_low_cb(void *u, const double tri[3][3]) { gf_low_tri((const GfLowU *)u, tri); }

void td5_roam_gfix_lower(const RoamGrid *g, const RoamTile *const nb[9], TD5_RoamHeight *hc, int tx, int tz, int G,
                         int lo, int W, double *low)
{
    GfLowU u;
    const int RW = W + 2;
    double *r = (double *)malloc((size_t)RW * (size_t)RW * sizeof(double));
    int i, j;
    for (i = 0; i < W * W; i++) low[i] = 0.0;
    if (!r) return;
    for (j = 0; j < RW; j++) for (i = 0; i < RW; i++) {      /* the raster one point round the window */
        double x, z, h;
        td5_roam_gfix_point(g, tx, tz, lo - 1 + i, lo - 1 + j, G, &x, &z);
        r[j * RW + i] = (hc && td5_roam_height_sample(hc, x, z, &h, NULL)) ? h : NAN;
    }
    u.g = g; u.r = r; u.low = low; u.I0 = tx * G + lo; u.J0 = tz * G + lo; u.W = W;
    u.cs = (double)g->tile / (double)G;
    u.dbg[0] = u.dbg[1] = u.dbg[2] = 0.0;
    u.vcap = 1e300;
#ifndef TD5RE_RELEASE
    { const char *e = getenv("TD5RE_GFIX_DBG"); if (e) sscanf(e, "%lf,%lf,%lf", &u.dbg[0], &u.dbg[1], &u.dbg[2]); }
#endif
    gf_road_tris(nb, hc, gf_low_cb, &u);
    if (td5_roam_gfix_verge()) {                                     /* [roam HC] */
        u.vcap = GF_VERGE_VCAP;
        gf_verge_tris(nb, hc, gf_low_cb, &u);
    }
    free(r);
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
    int G, N, W, M, i, j;
    double *cm;
    long W0;
    memset(f, 0, sizeof *f);
    if (!hc) return 0;
    G = 256 / td5_roam_gfix_cell_m();
    f->grid = *g; f->G = G; f->tx = tx; f->tz = tz;
    f->cs = (double)g->tile / (double)G;
    /* [G5] the whole reach of the tile's scenery (sc_in_reach: the 3x3), so a lawn, a tree or a path the tile lays
     * in a neighbour stands on the ground the renderer draws there, not on the raster (G4b kept GF_MARGIN cells) */
    M = td5_roam_gfix_g4() ? GF_MARGIN : G + 1;
    f->lo = -M + 1;                         /* lattice points lo .. G + M - 1 */
    f->np = G + 2 * M - 1;
    f->h = (double *)malloc((size_t)f->np * (size_t)f->np * sizeof(double));
    if (!f->h) { memset(f, 0, sizeof *f); return 0; }
    W0 = (long)f->np * (long)f->np;
    for (j = 0; j < f->np; j++) for (i = 0; i < f->np; i++) {
        double x, z, h;
        td5_roam_gfix_point(g, tx, tz, f->lo + i, f->lo + j, G, &x, &z);
        f->h[j * f->np + i] = td5_roam_height_sample(hc, x, z, &h, NULL) ? h : NAN;
    }
    if (!td5_roam_gfix_enabled()) { for (i = 0; i < W0; i++) f->h[i] = 0.0; return 1; }
    W = f->np;
    if (!td5_roam_gfix_g4()) {                  /* [G5] how far each lattice point must come down */
        td5_roam_gfix_lower(g, nb, hc, tx, tz, G, f->lo, W, f->h);
        return 1;
    }
    /* cells tx*G - GF_MARGIN .. tx*G + G + GF_MARGIN - 1 */
    N = G + 2 * M;
    cm = (double *)malloc((size_t)N * (size_t)N * sizeof(double));
    if (!cm) { for (i = 0; i < W0; i++) f->h[i] = 0.0; return 1; }
    td5_roam_gfix_cells(g, nb, tx * G - M, tz * G - M, N, f->cs, cm);
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
