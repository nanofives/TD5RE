/**
 * td5_roam_cut.c -- GEO FREE ROAM: CUTTINGS, the walls of a road that runs under the ground (PORT-ONLY, roam NC).
 * Contract and rationale in td5_roam_cut.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_trackgen_internal.h"   /* TG_Buf, tg_write_quad_mesh, TD5_TG_PAGE_*  */
#include "td5_config.h"
#include "td5_platform.h"
#include "td5_roam_scenery.h"
#include "td5_roam_cut.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "roam"

#define CT_UPM        430.0
#define CT_M(m)       ((m) * CT_UPM)
#define CT_MAX_LANES  16
#define CT_SINK       CT_M(0.30)     /* the wall foot under the road                          */
#define CT_SHOULDER   CT_M(0.03)     /* the shoulder strip under the road surface             */
#define CT_PARAPET    CT_M(0.90)     /* parapet over the drawn ground                         */
#define CT_APRON_LIFT CT_M(0.10)     /* the coping apron over the drawn ground                */
#define CT_APRON_W    CT_M(3.00)     /* covers the sub-cell staircase (2 m cells: <= 2.83 m)  */
#define CT_WALL_MIN   CT_M(0.30)     /* no wall where the drawn ground is this near the road  */
#define CT_PIECE      CT_M(2.00)     /* kerb pieces                                           */
#define CT_DECK_OVER  CT_M(2.50)     /* a road this far over the cut road is a deck           */
#define CT_DECK_GAP   CT_M(0.02)     /* the wall top under a deck's surface                   */
#define CT_UV_REPEAT  CT_M(4.00)
#define CT_QMAX       256            /* quads per mesh record                                 */

int td5_roam_cut_on(void) { return td5_env_int("TD5RE_ROAM_CUT", 1, 0, 1); }

/* ------------------------------------------------------------ spans ---- */

static int ct_span_ok(const RoamTile *t, const TD5_StripSpan *sp, uint32_t *L, uint32_t *R, int *n)
{
    *L = sp->left_vertex_index; *R = sp->right_vertex_index;
    *n = (int)(sp->pad_02[1] & 0x0Fu);
    if (*n < 1 || *n >= CT_MAX_LANES) return 0;
    if (*L + (uint32_t)*n >= t->nvert || *R + (uint32_t)*n >= t->nvert) return 0;
    return 1;
}

/* the span quad's four kerb corners, place frame: L, L + n (row a), R + n, R (row b) */
static int ct_corners(const RoamTile *t, uint32_t si, double p[4][3])
{
    const TD5_StripSpan *sp = &t->span[si];
    uint32_t L, R, vi[4];
    int n, k;
    if (!ct_span_ok(t, sp, &L, &R, &n)) return 0;
    vi[0] = L; vi[1] = L + (uint32_t)n; vi[2] = R + (uint32_t)n; vi[3] = R;
    for (k = 0; k < 4; k++) {
        const TD5_StripVertex *v = &t->vert[vi[k]];
        p[k][0] = (double)t->org_x + (double)sp->origin_x + (double)v->x;
        p[k][1] = (double)sp->origin_y + (double)v->y;
        p[k][2] = (double)t->org_z + (double)sp->origin_z + (double)v->z;
    }
    return 1;
}

int td5_roam_cut_spans(const RoamTile *t, TD5_RoamHeight *hc, uint8_t *cut)
{
    const int on = td5_roam_cut_on();
    uint32_t e, q;
    int cnt = 0;
    if (!t) return 0;
    memset(cut, 0, t->nspan ? t->nspan : 1u);
    for (e = 0; e < t->nedge; e++) {
        const RoamEdgeRec *er = &t->edge[e];
        const int bore = (er->flags & ROAM_EF_BORE) != 0;
        if (!bore && !(on && (er->flags & ROAM_EF_UNDER))) continue;
        for (q = 0; q < er->span_count; q++) {
            const uint32_t si = (uint32_t)er->span_first + q;
            double p[4][3], h;
            int k, deep = bore;
            if (si >= t->nspan) continue;
            if (!deep && hc && ct_corners(t, si, p))
                for (k = 0; k < 4 && !deep; k++)
                    if (td5_roam_height_sample(hc, p[k][0], p[k][2], &h, NULL) && h - p[k][1] > TD5_ROAM_CUT_MIN) deep = 1;
            if (deep) { cut[si] = 1; cnt++; }
        }
    }
    return cnt;
}

/* ------------------------------------------------------------ holes ---- */

void td5_roam_cut_holes(const RoamGrid *g, const RoamTile *const nb[9], TD5_RoamHeight *hc, int tx, int tz, int G,
                        uint8_t *hole)
{
    const int on = td5_roam_cut_on();
    const double S = (double)g->tile;
    int k;
    memset(hole, 0, (size_t)G * (size_t)G);
    for (k = 0; k < 9; k++) {
        const RoamTile *t = nb[k];
        uint8_t *cut;
        uint32_t si;
        if (!t || (!on && k != 4)) continue;
        cut = (uint8_t *)calloc(t->nspan ? t->nspan : 1u, 1);
        if (!cut) continue;
        if (on) td5_roam_cut_spans(t, hc, cut);
        else {                                    /* the old rule: the tile's own bore spans */
            uint32_t e, q;
            for (e = 0; e < t->nedge; e++)
                if (t->edge[e].flags & ROAM_EF_BORE)
                    for (q = 0; q < t->edge[e].span_count; q++)
                        if ((uint32_t)t->edge[e].span_first + q < t->nspan) cut[t->edge[e].span_first + q] = 1;
        }
        for (si = 0; si < t->nspan; si++) {
            const TD5_StripSpan *sp = &t->span[si];
            uint32_t L, R; int n, a, b, da, db;
            double X, Z, u, v;
            if (!cut[si] || !ct_span_ok(t, sp, &L, &R, &n)) continue;
            /* the span centre exactly as rr_span_centre / the scenery lot pass took it */
            X = (double)t->org_x + (double)sp->origin_x + 0.5 * ((double)t->vert[L].x + (double)t->vert[R + (uint32_t)n].x);
            Z = (double)t->org_z + (double)sp->origin_z + 0.5 * ((double)t->vert[L].z + (double)t->vert[R + (uint32_t)n].z);
            roam_grid_uv(g, X, Z, &u, &v);
            a = (int)floor((u / S - (double)tx) * (double)G);
            b = (int)floor((v / S - (double)tz) * (double)G);
            for (db = -1; db <= 1; db++) for (da = -1; da <= 1; da++) {
                const int ca = a + da, cb = b + db;
                if (ca >= 0 && ca < G && cb >= 0 && cb < G) hole[cb * G + ca] = 1;
            }
        }
        free(cut);
    }
}

/* ------------------------------------------------------------ bands ---- */

int td5_roam_cut_bands(TD5_RoamCutBands *b, const RoamTile *const nb[9], TD5_RoamHeight *hc,
                       double x0, double z0, double x1, double z1)
{
    int k;
    memset(b, 0, sizeof *b);
    for (k = 0; k < 9; k++) {
        const RoamTile *t = nb[k];
        uint8_t *cut;
        uint32_t si;
        if (!t) continue;
        cut = (uint8_t *)calloc(t->nspan ? t->nspan : 1u, 1);
        if (!cut) return 0;
        td5_roam_cut_spans(t, hc, cut);
        for (si = 0; si < t->nspan; si++) {
            double p[4][3], *q;
            int r;
            if (!cut[si] || !ct_corners(t, si, p)) continue;
            if (b->n >= b->cap) {
                const int nc = b->cap ? b->cap * 2 : 256;
                double *nq = (double *)realloc(b->q, (size_t)nc * 12u * sizeof(double));
                if (!nq) { free(cut); return 0; }
                b->q = nq; b->cap = nc;
            }
            q = &b->q[b->n * 12];
            for (r = 0; r < 2; r++) {             /* rows a (0, 1) and b (3, 2): widen across the row */
                const int i0 = r ? 3 : 0, i1 = r ? 2 : 1;
                double dx = p[i1][0] - p[i0][0], dz = p[i1][2] - p[i0][2];
                const double l = hypot(dx, dz);
                if (l < 1.0) { dx = 0.0; dz = 0.0; } else { dx /= l; dz /= l; }
                q[i0 * 2 + 0] = p[i0][0] - dx * TD5_ROAM_CUT_BAND; q[i0 * 2 + 1] = p[i0][2] - dz * TD5_ROAM_CUT_BAND;
                q[i1 * 2 + 0] = p[i1][0] + dx * TD5_ROAM_CUT_BAND; q[i1 * 2 + 1] = p[i1][2] + dz * TD5_ROAM_CUT_BAND;
            }
            q[8] = q[10] = q[0]; q[9] = q[11] = q[1];
            for (r = 1; r < 4; r++) {
                if (q[r * 2] < q[8]) q[8] = q[r * 2];
                if (q[r * 2] > q[10]) q[10] = q[r * 2];
                if (q[r * 2 + 1] < q[9]) q[9] = q[r * 2 + 1];
                if (q[r * 2 + 1] > q[11]) q[11] = q[r * 2 + 1];
            }
            if (q[10] < x0 || q[8] > x1 || q[11] < z0 || q[9] > z1) continue;
            b->n++;
        }
        free(cut);
    }
    return 1;
}

int td5_roam_cut_inside(const TD5_RoamCutBands *b, double x, double z)
{
    int i;
    for (i = 0; i < b->n; i++) {
        const double *q = &b->q[i * 12];
        int k, pos = 0, neg = 0;
        if (x < q[8] || x > q[10] || z < q[9] || z > q[11]) continue;
        for (k = 0; k < 4; k++) {
            const int m = (k + 1) & 3;
            const double cr = (q[m * 2] - q[k * 2]) * (z - q[k * 2 + 1]) - (q[m * 2 + 1] - q[k * 2 + 1]) * (x - q[k * 2]);
            if (cr > 0.0) pos++;
            else if (cr < 0.0) neg++;
        }
        if (!pos || !neg) return 1;
    }
    return 0;
}

void td5_roam_cut_bands_free(TD5_RoamCutBands *b) { free(b->q); memset(b, 0, sizeof *b); }

/* ------------------------------------------------------------ walls ---- */

typedef struct {
    double x[CT_QMAX * 4], y[CT_QMAX * 4], z[CT_QMAX * 4], u[CT_QMAX * 4], v[CT_QMAX * 4];
    int    nq;
} CtQB;

typedef struct {
    TG_TileCtx       *c;
    TD5_RoamCutStats *st;
    CtQB             *qb;              /* one per sub-block */
    int               oom;
} CtEm;

static int ct_flush(CtEm *em, int sb)
{
    CtQB *q = &em->qb[sb];
    TG_Buf *b;
    int page = TD5_TG_PAGE_R4_PIER, nq;
    if (q->nq <= 0) return 1;
    b = (TG_Buf *)roam_ctx_mesh_begin(em->c, sb);
    if (!b) { em->oom = 1; return 0; }
    nq = q->nq;
    if (!tg_write_quad_mesh(b, q->x, q->y, q->z, q->u, q->v, nq * 4, &page, &nq, 1) || b->oom) { em->oom = 1; return 0; }
    q->nq = 0;
    return 1;
}

/* one quad: p[4] (tile-local x, y, z), u across, v along */
static int ct_quad(CtEm *em, int sb, const double p[4][3], const double uv[4][2])
{
    CtQB *q = &em->qb[sb];
    int k;
    if (q->nq >= CT_QMAX && !ct_flush(em, sb)) return 0;
    for (k = 0; k < 4; k++) {
        const int o = q->nq * 4 + k;
        q->x[o] = p[k][0]; q->y[o] = p[k][1]; q->z[o] = p[k][2];
        q->u[o] = uv[k][0]; q->v[o] = uv[k][1];
    }
    q->nq++;
    return 1;
}

/* the face between lateral offsets o0 -> o1 with heights h0[] -> h1[] at the piece's two ends (absolute y) */
static int ct_face(CtEm *em, int sb, const double a[2], const double b[2], const double n[2],
                   double o0, const double h0[2], double o1, const double h1[2], double u0, double u1, double v0, double v1)
{
    double p[4][3], uv[4][2];
    p[0][0] = a[0] + n[0] * o0; p[0][1] = h0[0]; p[0][2] = a[1] + n[1] * o0;
    p[1][0] = b[0] + n[0] * o0; p[1][1] = h0[1]; p[1][2] = b[1] + n[1] * o0;
    p[2][0] = b[0] + n[0] * o1; p[2][1] = h1[1]; p[2][2] = b[1] + n[1] * o1;
    p[3][0] = a[0] + n[0] * o1; p[3][1] = h1[0]; p[3][2] = a[1] + n[1] * o1;
    uv[0][0] = u0; uv[0][1] = v0; uv[1][0] = u0; uv[1][1] = v1;
    uv[2][0] = u1; uv[2][1] = v1; uv[3][0] = u1; uv[3][1] = v0;
    return ct_quad(em, sb, (const double (*)[3])p, (const double (*)[2])uv);
}

/* the highest road surface over (x, z) and round it (2.5 m along n and t) higher than ylo; 0 = none */
static int ct_upper_y(const TG_TileCtx *c, double x, double z, const double n[2], double ylo, double *out)
{
    static const double k_o[5][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    const double r = CT_M(2.5);
    int k, hit = 0;
    double best = -1e300;
    for (k = 0; k < 5; k++) {
        const double px = x + (n[0] * k_o[k][0] - n[1] * k_o[k][1]) * r;
        const double pz = z + (n[1] * k_o[k][0] + n[0] * k_o[k][1]) * r;
        double y;
        if (roam_ctx_road_y(c, px, pz, ylo + CT_M(100.0), &y) && y > ylo && y > best) { best = y; hit = 1; }
    }
    if (hit) *out = best;
    return hit;
}

/* lateral free width of the apron at wall-line point (x, z): until a cutting band, a road at the ground's level
 * or a deck over it, sampled every 0.5 m out to CT_APRON_W */
static double ct_apron_free(const TG_TileCtx *c, const TD5_RoamCutBands *bands, int32_t ox, int32_t oz,
                            double x, double z, const double n[2], double gy)
{
    double o;
    for (o = TD5_ROAM_CUT_T + CT_M(0.5); o <= TD5_ROAM_CUT_T + CT_APRON_W + 1.0; o += CT_M(0.5)) {
        const double px = x + n[0] * o, pz = z + n[1] * o;
        if (td5_roam_cut_inside(bands, px + (double)ox, pz + (double)oz)) return o - CT_M(0.5);
        if (roam_ctx_on_road_band(c, &px, &pz, 1, gy - CT_M(1.0), gy + CT_M(100.0))) return o - CT_M(0.5);
    }
    return TD5_ROAM_CUT_T + CT_APRON_W;
}

int td5_roam_cut_emit(TG_TileCtx *c, TD5_RoamCutStats *st)
{
    const RoamTile *nb[9];
    const RoamTile *t;
    TD5_RoamHeight *hc = roam_ctx_hc(c);
    TD5_RoamCutBands bands;
    CtEm em;
    uint8_t *cut = NULL;
    int32_t ox, oz;
    int k, ok = 1;
    uint32_t bi;
    if (!td5_roam_cut_on() || !hc) return 1;
    for (k = 0; k < 9; k++) nb[k] = roam_ctx_tile(c, k % 3 - 1, k / 3 - 1);
    t = nb[4];
    if (!t || !t->nspan) return 1;
    roam_ctx_origin(c, NULL, NULL, &ox, &oz);
    cut = (uint8_t *)calloc(t->nspan, 1);
    if (!cut) return 0;
    if (!td5_roam_cut_spans(t, hc, cut)) { free(cut); return 1; }
    for (bi = 0; bi < t->nspan; bi++) st->spans += cut[bi];
    {
        const double m = CT_M(40.0);
        double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
        for (bi = 0; bi < t->nspan; bi++) {
            double p[4][3];
            int q;
            if (!cut[bi] || !ct_corners(t, bi, p)) continue;
            for (q = 0; q < 4; q++) {
                if (p[q][0] < x0) x0 = p[q][0];
                if (p[q][0] > x1) x1 = p[q][0];
                if (p[q][2] < z0) z0 = p[q][2];
                if (p[q][2] > z1) z1 = p[q][2];
            }
        }
        if (!td5_roam_cut_bands(&bands, nb, hc, x0 - m, z0 - m, x1 + m, z1 + m)) { free(cut); return 0; }
    }
    memset(&em, 0, sizeof em);
    em.c = c; em.st = st;
    em.qb = (CtQB *)calloc(TD5_ROAM_SCN_NSB, sizeof(CtQB));
    if (!em.qb) { free(cut); td5_roam_cut_bands_free(&bands); return 0; }

    for (bi = 0; bi < t->nbedge && !em.oom; bi++) {
        const RoamBEdge *be = &t->bedge[bi];
        const uint32_t si = (uint32_t)be->prim;
        const TD5_StripSpan *sp;
        double A[3], B[3], ctr[2], n[2], l, len;
        uint32_t L, R; int ln, np, pi;
        if (be->kind != ROAM_BEDGE_KERB || (be->prim & ROAM_PRIM_TRI) || si >= t->nspan || !cut[si]) continue;
        if (be->v[0] >= t->nvert || be->v[1] >= t->nvert) continue;
        sp = &t->span[si];
        if (!ct_span_ok(t, sp, &L, &R, &ln)) continue;
        A[0] = (double)t->org_x - ox + sp->origin_x + t->vert[be->v[0]].x;
        A[1] = (double)sp->origin_y + t->vert[be->v[0]].y;
        A[2] = (double)t->org_z - oz + sp->origin_z + t->vert[be->v[0]].z;
        B[0] = (double)t->org_x - ox + sp->origin_x + t->vert[be->v[1]].x;
        B[1] = (double)sp->origin_y + t->vert[be->v[1]].y;
        B[2] = (double)t->org_z - oz + sp->origin_z + t->vert[be->v[1]].z;
        ctr[0] = (double)t->org_x - ox + sp->origin_x + 0.25 * ((double)t->vert[L].x + t->vert[L + ln].x + t->vert[R].x + t->vert[R + ln].x);
        ctr[1] = (double)t->org_z - oz + sp->origin_z + 0.25 * ((double)t->vert[L].z + t->vert[L + ln].z + t->vert[R].z + t->vert[R + ln].z);
        len = hypot(B[0] - A[0], B[2] - A[2]);
        if (len < 1.0) continue;
        n[0] = (B[2] - A[2]) / len; n[1] = -(B[0] - A[0]) / len;
        l = (0.5 * (A[0] + B[0]) - ctr[0]) * n[0] + (0.5 * (A[2] + B[2]) - ctr[1]) * n[1];
        if (l < 0.0) { n[0] = -n[0]; n[1] = -n[1]; }      /* away from the road */
        np = (int)ceil(len / CT_PIECE);
        if (np < 1) np = 1;
        for (pi = 0; pi < np && !em.oom; pi++) {
            const double f0 = (double)pi / np, f1 = (double)(pi + 1) / np;
            double a[2], b[2], yk[2], gy[2], top[2], h0[2], h1[2], wa = CT_APRON_W + TD5_ROAM_CUT_T;
            int deck[2], par = 1, e2, sb;
            double fx[4], fz[4];
            a[0] = A[0] + (B[0] - A[0]) * f0; a[1] = A[2] + (B[2] - A[2]) * f0; yk[0] = A[1] + (B[1] - A[1]) * f0;
            b[0] = A[0] + (B[0] - A[0]) * f1; b[1] = A[2] + (B[2] - A[2]) * f1; yk[1] = A[1] + (B[1] - A[1]) * f1;
            for (e2 = 0; e2 < 2; e2++) {
                const double *P = e2 ? b : a;
                const double wx = P[0] + n[0] * TD5_ROAM_CUT_OFF, wz = P[1] + n[1] * TD5_ROAM_CUT_OFF;
                double up;
                gy[e2] = roam_ctx_drawn_ground(c, wx, wz);
                deck[e2] = ct_upper_y(c, wx, wz, n, yk[e2] + CT_DECK_OVER, &up);
                top[e2] = deck[e2] ? up - CT_DECK_GAP : gy[e2] + CT_PARAPET;
            }
            if (gy[0] - yk[0] < CT_WALL_MIN && gy[1] - yk[1] < CT_WALL_MIN && !deck[0] && !deck[1]) { st->low_skip++; continue; }
            /* the wall footprint on a road at the cut road's own level (the other carriageway hard by): no wall */
            fx[0] = a[0] + n[0] * TD5_ROAM_CUT_OFF; fz[0] = a[1] + n[1] * TD5_ROAM_CUT_OFF;
            fx[1] = b[0] + n[0] * TD5_ROAM_CUT_OFF; fz[1] = b[1] + n[1] * TD5_ROAM_CUT_OFF;
            fx[2] = b[0] + n[0] * (TD5_ROAM_CUT_OFF + TD5_ROAM_CUT_T); fz[2] = b[1] + n[1] * (TD5_ROAM_CUT_OFF + TD5_ROAM_CUT_T);
            fx[3] = a[0] + n[0] * (TD5_ROAM_CUT_OFF + TD5_ROAM_CUT_T); fz[3] = a[1] + n[1] * (TD5_ROAM_CUT_OFF + TD5_ROAM_CUT_T);
            if (roam_ctx_on_road_band(c, fx, fz, 4, (yk[0] < yk[1] ? yk[0] : yk[1]) - CT_M(1.0),
                                      (yk[0] > yk[1] ? yk[0] : yk[1]) + CT_M(1.0))) { st->on_road++; continue; }
            if (deck[0] || deck[1]) par = 0;
            /* a parapet standing on a road at the ground's level: the wall stops at the ground there */
            if (par && roam_ctx_on_road_band(c, fx, fz, 4, (gy[0] < gy[1] ? gy[0] : gy[1]) - CT_M(0.5),
                                             (gy[0] > gy[1] ? gy[0] : gy[1]) + CT_M(1.5))) {
                par = 0; top[0] = gy[0]; top[1] = gy[1]; st->on_road++;
            }
            if (!par && !deck[0] && !deck[1]) { top[0] = gy[0]; top[1] = gy[1]; }
            if (!par && (deck[0] != deck[1])) {         /* half under a deck: the open end at the ground */
                if (!deck[0]) top[0] = gy[0];
                if (!deck[1]) top[1] = gy[1];
            }
            sb = roam_ctx_sb_of(c, 0.5 * (a[0] + b[0]), 0.5 * (a[1] + b[1]));
            {
                const double v0 = (double)pi * len / np / CT_UV_REPEAT, v1 = (double)(pi + 1) * len / np / CT_UV_REPEAT;
                const double o = TD5_ROAM_CUT_OFF, T = TD5_ROAM_CUT_T;
                /* shoulder: kerb -> wall foot, under the road surface */
                h0[0] = yk[0] - CT_SHOULDER; h0[1] = yk[1] - CT_SHOULDER;
                if (!ct_face(&em, sb, a, b, n, CT_M(0.02), h0, o, h0, 0.0, 0.15, v0, v1)) break;
                /* face: from under the road to the top */
                h0[0] = yk[0] - CT_SINK; h0[1] = yk[1] - CT_SINK;
                if (!ct_face(&em, sb, a, b, n, o, h0, o, top, 0.15, 0.80, v0, v1)) break;
                /* crown */
                if (!ct_face(&em, sb, a, b, n, o, top, o + T, top, 0.80, 0.90, v0, v1)) break;
                /* back: down to the apron (parapet) or under the ground */
                h1[0] = par ? gy[0] + CT_APRON_LIFT : gy[0] - CT_M(0.50);
                h1[1] = par ? gy[1] + CT_APRON_LIFT : gy[1] - CT_M(0.50);
                if (!deck[0] || !deck[1])
                    if (!ct_face(&em, sb, a, b, n, o + T, top, o + T, h1, 0.90, 1.0, v0, v1)) break;
                st->pieces++;
                st->walls_m += len / np / CT_UPM;
                if (par) st->parapet_m += len / np / CT_UPM;
                for (e2 = 0; e2 < 2; e2++)
                    if ((top[e2] - yk[e2]) / CT_UPM > st->max_h_m) st->max_h_m = (top[e2] - yk[e2]) / CT_UPM;
                if (deck[0] || deck[1]) st->deck_pieces++;
                if (par) {
                    const double ga = ct_apron_free(c, &bands, ox, oz, a[0] + n[0] * o, a[1] + n[1] * o, n, gy[0]);
                    const double gb = ct_apron_free(c, &bands, ox, oz, b[0] + n[0] * o, b[1] + n[1] * o, n, gy[1]);
                    wa = ga < gb ? ga : gb;
                    if (wa < CT_APRON_W + T - 1.0) st->apron_cut++;
                    if (wa > T + CT_M(0.25)) {
                        const double xa = a[0] + n[0] * (o + wa), za = a[1] + n[1] * (o + wa);
                        const double xb = b[0] + n[0] * (o + wa), zb = b[1] + n[1] * (o + wa);
                        h0[0] = gy[0] + CT_APRON_LIFT; h0[1] = gy[1] + CT_APRON_LIFT;
                        h1[0] = roam_ctx_drawn_ground(c, xa, za) + CT_APRON_LIFT;
                        h1[1] = roam_ctx_drawn_ground(c, xb, zb) + CT_APRON_LIFT;
                        if (!ct_face(&em, sb, a, b, n, o + T, h0, o + wa, h1, 0.0, 0.5, v0, v1)) break;
                        st->apron_m += len / np / CT_UPM;
                    }
                }
            }
        }
    }
    for (k = 0; k < TD5_ROAM_SCN_NSB && ok; k++) if (!ct_flush(&em, k)) ok = 0;
    if (em.oom) ok = 0;
    free(em.qb);
    free(cut);
    td5_roam_cut_bands_free(&bands);
    return ok;
}
