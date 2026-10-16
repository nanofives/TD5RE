/**
 * td5_roam_render.c -- GEO FREE ROAM plain road + ground render (PORT-ONLY, F2-C).
 * Contract: td5_roam_render.h. Design: docs/plans/GEO_FREE_ROAM_DESIGN.md 3.6.
 *
 * BAKE. A ROAMTILE is turned into up to 16 ordinary MODELS.DAT-format meshes, one
 * per 64 m sub-block of the tile's own lattice (4 x 4 of S/4). Each mesh is ONE
 * allocation: TD5_MeshHeader, up to three 16-byte commands, then the de-indexed
 * 44-byte vertices, origin 0 and absolute positions in the render frame (strip
 * units, 430 per metre), exactly how the generator writes level geometry
 * (tg_write_quad_mesh). vertex_data_ptr is 0 everywhere: the renderer's own
 * sequential cursor walks the vertex array, tris before quads inside a command.
 *   cmd 0  road     ribbon lane quads + junction patch triangles, the asphalt page
 *   cmd 1  ground   a heightfield cell per ROAM_RENDER_CELL_M, the ground page
 *   cmd 2  skirts   a grey vertical face down every BEDG (kerb, deck side)
 *
 * FRAME. place -> render is `place - (ox, oz)` in x/z and `y - dy` in height (the
 * contract of td5_roam_mode.h). DEV route mode swaps x/z for the place -> route
 * chain of td5_roam_view.c and dy for the strip datum, so the tiles can be laid
 * over a loaded geo level (an eye that is "g_cameraPos" lives in the same units:
 * strip units = 24.8 / 256, never multiplied again, see td5_roam_view.c).
 *
 * WINDING / LIGHTING. Tile triangles come out of triangle_height with a DOWNWARD
 * normal (the locator flips it). This renderer submits pre-projected polygons and
 * has no back-face cull, so the winding cannot hide a face; it only matters for
 * the baked light. The shade here is taken from the face normal FLIPPED to point
 * up (|ny|), so a flat road reads full-bright, a slope a little darker, and no
 * face is ever lit as if it faced the ground. The meshes carry no normals, so the
 * renderer keeps these baked ARGB values untouched (alpha 0xFF skips the colour
 * LUT, the same as every generated mesh).
 *
 * GROUND. A (256 m / cell) grid per tile on the tile's own lattice, heights from
 * the SOURCE HEIGHT.R16 through td5_roam_height (bilinear x exaggeration, minus
 * water depth), dropped ROAM_RENDER_GROUND_DROP_CM so the road, which is laid on
 * the same raster, never z-fights it. Adjacent tiles share their border points
 * exactly, so there is no seam. Cells under a tunnel bore are left out.
 *
 * RESIDENCY. td5_roam_render_add_tile / _remove_tile are the interface for the
 * resident set (sibling B). The DEV loader at the bottom reads ROAMTILE files
 * itself so this task can be proven alone; it is the part to REPLACE.
 *
 * State: one session struct (s_rr), no other file-scope mutable state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_render.h"
#include "td5_camera.h"
#include "td5_page_map.h"
#include "td5_roam_tile.h"
#include "td5_roam_height.h"
#include "td5_roam_ribbon.h"
#include "td5_roam_view.h"
#include "td5_roam_render.h"

#if defined(__has_include)
#  if __has_include("td5_roam_mode.h")
#    include "td5_roam_mode.h"
#    define RR_HAVE_MODE 1
#  endif
#endif

#define LOG_TAG "roam"

#define RR_SUB            4                  /* sub-blocks per tile side (64 m)     */
#define RR_MAXMESH        (RR_SUB * RR_SUB)
#define RR_UPM            430.0              /* units per metre                     */
#define RR_MAX_LANES      16
/* Pages of the loaded GENERATED level (td5_trackgen_internal.h TD5_TG_PAGE_*):
 * 0 = ROAD (asphalt, lane line at the u edge), 5 = GROUND. The kerb skirt uses the
 * 1x1 white page of the HUD (TD5 page map) with a grey vertex colour. Override
 * with TD5RE_ROAM_RENDER_PAGE_ROAD / _GROUND / _KERB for another level. */
#define RR_PAGE_ROAD      0
#define RR_PAGE_GROUND    5
#define RR_PAGE_KERB      HUD_WHITE_TEX_PAGE

typedef struct {
    int             tx, tz;
    int             nmesh;
    TD5_MeshHeader *mesh[RR_MAXMESH];
    float           sph[RR_MAXMESH][4];      /* x y z radius, render frame          */
    TD5_MeshHeader *vis[RR_MAXMESH];         /* this frame's list (stable storage)  */
    TD5_SpanDisplayList dl;
    float           cx, cy, cz, r;           /* whole-tile sphere                   */
    uint32_t        nroad, ntri, nground, nskirt;
    size_t          bytes;
} RRTile;

typedef struct {
    int       began;
    char      place[64];
    TD5_RoamRenderFrame f;
    RoamGrid  grid;
    int       grid_ok;
    TD5_RoamHeight *hc;                      /* height source, heap (large)         */
    int       hc_ok;
    RRTile  **tile;
    int       ntile, captile;
    /* knobs, read once at begin */
    int       page_road, page_ground, page_kerb;
    int       use_ground;
    double    lift;                          /* units, added to every y             */
    double    ground_drop;                   /* units                               */
    int       cell_m;                        /* ground cell, metres (divides 64)    */
    double    radius;                        /* draw radius, units                  */
    /* DEV loader */
    int       dev_mode;                      /* 0 off, 1 near, 2 all                */
    int       dev_loaded_all;
    int       dev_reach;
    int       dev_overlay;                   /* 0 unread, 1 off, 2 on (env, read once)  */
    int       dev_hide;                      /* same encoding, HIDE_STRIP               */
    int      *tried;                         /* (tx,tz) pairs that failed to read   */
    int       ntried, captried;
    /* stats */
    uint32_t  st_frames;
    double    st_sum_ms, st_max_ms, st_last_ms;
    int       st_drawn_tiles, st_drawn_meshes;
    uint32_t  st_drawn_quads;
} RRSession;

static RRSession s_rr;

/* ------------------------------------------------------------------ frame ---- */

static void rr_pt(double px, double py, double pz, float o[3])
{
    double x = px - (double)s_rr.f.ox;
    double z = pz - (double)s_rr.f.oz;
#ifndef TD5RE_RELEASE
    if (s_rr.f.route) {
        double xr, zr;
        if (td5_roam_view_frame(s_rr.place, px, pz, &xr, &zr)) { x = xr; z = zr; }
    }
#endif
    o[0] = (float)x;
    o[1] = (float)(py - s_rr.f.dy + s_rr.lift);
    o[2] = (float)z;
}

/* the tile's vertex `v` as owned by the primitive whose origin is (ox, oy, oz) */
static void rr_vert(const RoamTile *t, int32_t ox, int32_t oy, int32_t oz,
                    const TD5_StripVertex *v, float o[3])
{
    rr_pt((double)t->org_x + (double)ox + (double)v->x,
          (double)oy + (double)v->y,
          (double)t->org_z + (double)oz + (double)v->z, o);
}

/* ------------------------------------------------------------ shading -------- */

/* Sun from above and a little to one side, y UP (the strip's height axis). A flat
 * face reads 1.0; the normal is flipped to point up first (see header). */
static uint32_t rr_shade(const float a[3], const float b[3], const float c[3])
{
    const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    float nx = uy * vz - uz * vy;
    float ny = uz * vx - ux * vz;
    float nz = ux * vy - uy * vx;
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    float nd, sh;
    unsigned g;
    if (len < 1e-6f) return 0xFFFFFFFFu;
    if (ny < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }     /* downward normal -> up */
    nd = (nx * 0.30f + ny * 0.80f + nz * 0.52f) / len;
    sh = 0.55f + 0.45f * (nd / 0.80f);
    if (sh < 0.35f) sh = 0.35f;
    if (sh > 1.00f) sh = 1.00f;
    g = (unsigned)(sh * 255.0f + 0.5f);
    return 0xFF000000u | (g << 16) | (g << 8) | g;
}

/* ------------------------------------------------------------- mesh baker ---- */

typedef struct {
    int      nrt, nrq, ngq, nsq;             /* counts: road tris/quads, ground, skirt */
    TD5_MeshHeader *m;
    TD5_MeshVertex *v;
    int      ct, cq, cg, cs;                 /* write cursors (in primitives)         */
    float    lo[3], hi[3];
} RRSb;

static void rr_put(RRSb *b, TD5_MeshVertex *dst, const float p[3], uint32_t col,
                   float u, float w)
{
    int k;
    memset(dst, 0, sizeof *dst);
    dst->pos_x = p[0]; dst->pos_y = p[1]; dst->pos_z = p[2];
    dst->lighting = col;
    dst->tex_u = u; dst->tex_v = w;
    for (k = 0; k < 3; k++) {
        if (p[k] < b->lo[k]) b->lo[k] = p[k];
        if (p[k] > b->hi[k]) b->hi[k] = p[k];
    }
}

/* sub-block index (0..15) of a place-frame point on this tile's lattice */
static int rr_sb_of(const RoamTile *t, double px, double pz)
{
    double u, v, q = (double)s_rr.grid.tile / (double)RR_SUB;
    int a, b;
    roam_grid_uv(&s_rr.grid, px, pz, &u, &v);
    a = (int)floor((u - (double)t->tx * (double)s_rr.grid.tile) / q);
    b = (int)floor((v - (double)t->tz * (double)s_rr.grid.tile) / q);
    if (a < 0) a = 0; if (a >= RR_SUB) a = RR_SUB - 1;
    if (b < 0) b = 0; if (b >= RR_SUB) b = RR_SUB - 1;
    return b * RR_SUB + a;
}

static int rr_span_ok(const RoamTile *t, const TD5_StripSpan *sp, uint32_t *L, uint32_t *R, int *n)
{
    *L = sp->left_vertex_index; *R = sp->right_vertex_index;
    *n = (int)(sp->pad_02[1] & 0x0Fu);
    if (*n < 1 || *n >= RR_MAX_LANES) return 0;
    if (*L + (uint32_t)*n >= t->nvert || *R + (uint32_t)*n >= t->nvert) return 0;
    return 1;
}

/* centre of a span's quad in place frame (for the sub-block pick) */
static void rr_span_centre(const RoamTile *t, const TD5_StripSpan *sp, uint32_t L, uint32_t R,
                           int n, double *px, double *pz)
{
    const TD5_StripVertex *a = &t->vert[L], *b = &t->vert[R + (uint32_t)n];
    *px = (double)t->org_x + (double)sp->origin_x + 0.5 * ((double)a->x + (double)b->x);
    *pz = (double)t->org_z + (double)sp->origin_z + 0.5 * ((double)a->z + (double)b->z);
}

static void rr_tri_centre(const RoamTile *t, const RoamTri *tr, double *px, double *pz)
{
    double sx = 0.0, sz = 0.0;
    int k;
    for (k = 0; k < 3; k++) { sx += t->vert[tr->v[k]].x; sz += t->vert[tr->v[k]].z; }
    *px = (double)t->org_x + (double)tr->ox + sx / 3.0;
    *pz = (double)t->org_z + (double)tr->oz + sz / 3.0;
}

static int rr_bedge_prim(const RoamTile *t, const RoamBEdge *be, int32_t *ox, int32_t *oy, int32_t *oz)
{
    const uint32_t p = (uint32_t)(be->prim & ~ROAM_PRIM_TRI);
    if (be->v[0] >= t->nvert || be->v[1] >= t->nvert) return 0;
    if (be->prim & ROAM_PRIM_TRI) {
        if (p >= t->ntri) return 0;
        *ox = t->tri[p].ox; *oy = t->tri[p].oy; *oz = t->tri[p].oz;
    } else {
        if (p >= t->nspan) return 0;
        *ox = t->span[p].origin_x; *oy = t->span[p].origin_y; *oz = t->span[p].origin_z;
    }
    return 1;
}

static uint32_t rr_skirt_col(int kind)
{
    return kind == ROAM_BEDGE_DECK ? 0xFF6E6E6Au : 0xFF9A9A94u;
}

/* ground: heights on the tile's lattice, (G+1) x (G+1) points */
typedef struct { int G; double *h; unsigned char *hole; } RRGround;

static void rr_ground_point(const RoamTile *t, int i, int j, int G, double *px, double *pz)
{
    const double S = (double)s_rr.grid.tile;
    const double u = ((double)t->tx + (double)i / (double)G) * S;
    const double v = ((double)t->tz + (double)j / (double)G) * S;
    *px = s_rr.grid.ox + s_rr.grid.c * u - s_rr.grid.s * v;
    *pz = s_rr.grid.oz + s_rr.grid.s * u + s_rr.grid.c * v;
}

static int rr_ground_build(const RoamTile *t, RRGround *g)
{
    const int G = (int)(256 / s_rr.cell_m);
    double minx = 1e18, minz = 1e18, maxx = -1e18, maxz = -1e18, x, z;
    int i, j, c, ok = 0;
    uint32_t e, q;
    const double S = (double)s_rr.grid.tile;
    (void)S;
    memset(g, 0, sizeof *g);
    g->G = G;
    if (!s_rr.use_ground || !s_rr.hc_ok) return 0;
    g->h = (double *)malloc((size_t)(G + 1) * (size_t)(G + 1) * sizeof(double));
    g->hole = (unsigned char *)calloc((size_t)G * (size_t)G, 1);
    if (!g->h || !g->hole) { free(g->h); free(g->hole); memset(g, 0, sizeof *g); return 0; }
    for (c = 0; c < 4; c++) {
        rr_ground_point(t, (c & 1) ? G : 0, (c & 2) ? G : 0, G, &x, &z);
        if (x < minx) minx = x; if (x > maxx) maxx = x;
        if (z < minz) minz = z; if (z > maxz) maxz = z;
    }
    if (!td5_roam_height_prefetch(s_rr.hc, minx - 2000.0, minz - 2000.0, maxx + 2000.0, maxz + 2000.0)) {
        free(g->h); free(g->hole); memset(g, 0, sizeof *g);
        return 0;
    }
    for (j = 0; j <= G; j++) {
        for (i = 0; i <= G; i++) {
            double h;
            rr_ground_point(t, i, j, G, &x, &z);
            if (td5_roam_height_sample(s_rr.hc, x, z, &h, NULL)) { g->h[j * (G + 1) + i] = h; ok++; }
            else g->h[j * (G + 1) + i] = NAN;
        }
    }
    /* cells under a tunnel bore are left out: the road runs below the ground there */
    for (e = 0; e < t->nedge; e++) {
        const RoamEdgeRec *er = &t->edge[e];
        if (!(er->flags & ROAM_EF_BORE)) continue;
        for (q = 0; q < er->span_count; q++) {
            const uint32_t si = (uint32_t)er->span_first + q;
            uint32_t L, R; int n; double px, pz, u, v;
            int a, b, da, db;
            if (si >= t->nspan || !rr_span_ok(t, &t->span[si], &L, &R, &n)) continue;
            rr_span_centre(t, &t->span[si], L, R, n, &px, &pz);
            roam_grid_uv(&s_rr.grid, px, pz, &u, &v);
            a = (int)floor((u / (double)s_rr.grid.tile - (double)t->tx) * (double)G);
            b = (int)floor((v / (double)s_rr.grid.tile - (double)t->tz) * (double)G);
            for (db = -1; db <= 1; db++) for (da = -1; da <= 1; da++) {
                const int ca = a + da, cb = b + db;
                if (ca >= 0 && ca < G && cb >= 0 && cb < G) g->hole[cb * G + ca] = 1;
            }
        }
    }
    return ok > 0;
}

static void rr_ground_free(RRGround *g) { free(g->h); free(g->hole); memset(g, 0, sizeof *g); }

static int rr_ground_cell_ok(const RRGround *g, int i, int j)
{
    const int W = g->G + 1;
    if (!g->h || g->hole[j * g->G + i]) return 0;
    return !isnan(g->h[j * W + i]) && !isnan(g->h[j * W + i + 1]) &&
           !isnan(g->h[(j + 1) * W + i]) && !isnan(g->h[(j + 1) * W + i + 1]);
}

/* ground cell (i, j) -> sub-block index */
static int rr_cell_sb(const RRGround *g, int i, int j)
{
    const int per = g->G / RR_SUB;
    return (j / per) * RR_SUB + (i / per);
}

static void rr_free_tile(RRTile *rt)
{
    int k;
    if (!rt) return;
    for (k = 0; k < rt->nmesh; k++) free(rt->mesh[k]);
    free(rt);
}

/* Bake `t`. Returns the tile or NULL (nothing to draw / out of memory). */
static RRTile *rr_bake(const RoamTile *t)
{
    RRSb sb[RR_MAXMESH];
    RRGround gr;
    RRTile *rt;
    uint8_t *spsb = NULL, *trsb = NULL, *besb = NULL;
    uint32_t i;
    int k, ok = 1;
    const float kerb_h = (float)(0.45 * RR_UPM), deck_h = (float)(1.2 * RR_UPM);

    memset(sb, 0, sizeof sb);
    for (k = 0; k < RR_MAXMESH; k++) {
        sb[k].lo[0] = sb[k].lo[1] = sb[k].lo[2] = 1e30f;
        sb[k].hi[0] = sb[k].hi[1] = sb[k].hi[2] = -1e30f;
    }
    rt = (RRTile *)calloc(1, sizeof *rt);
    spsb = (uint8_t *)calloc(t->nspan ? t->nspan : 1u, 1);
    trsb = (uint8_t *)calloc(t->ntri ? t->ntri : 1u, 1);
    besb = (uint8_t *)calloc(t->nbedge ? t->nbedge : 1u, 1);
    if (!rt || !spsb || !trsb || !besb) { ok = 0; goto done_counts; }
    rt->tx = t->tx; rt->tz = t->tz;

    /* ---- pass 1: count per sub-block ---- */
    for (i = 0; i < t->nspan; i++) {
        uint32_t L, R; int n; double px, pz;
        spsb[i] = 0xFF;
        if (!rr_span_ok(t, &t->span[i], &L, &R, &n)) continue;
        rr_span_centre(t, &t->span[i], L, R, n, &px, &pz);
        spsb[i] = (uint8_t)rr_sb_of(t, px, pz);
        sb[spsb[i]].nrq += n;
    }
    for (i = 0; i < t->ntri; i++) {
        const RoamTri *tr = &t->tri[i];
        double px, pz;
        trsb[i] = 0xFF;
        if (tr->v[0] >= t->nvert || tr->v[1] >= t->nvert || tr->v[2] >= t->nvert) continue;
        rr_tri_centre(t, tr, &px, &pz);
        trsb[i] = (uint8_t)rr_sb_of(t, px, pz);
        sb[trsb[i]].nrt++;
    }
    for (i = 0; i < t->nbedge; i++) {
        const RoamBEdge *be = &t->bedge[i];
        int32_t ox, oy, oz;
        const TD5_StripVertex *a, *b;
        besb[i] = 0xFF;
        if (!rr_bedge_prim(t, be, &ox, &oy, &oz)) continue;
        a = &t->vert[be->v[0]]; b = &t->vert[be->v[1]];
        besb[i] = (uint8_t)rr_sb_of(t, (double)t->org_x + ox + 0.5 * ((double)a->x + b->x),
                                       (double)t->org_z + oz + 0.5 * ((double)a->z + b->z));
        sb[besb[i]].nsq++;
    }
    rr_ground_build(t, &gr);
    if (gr.h) {
        int a, b;
        for (b = 0; b < gr.G; b++) for (a = 0; a < gr.G; a++)
            if (rr_ground_cell_ok(&gr, a, b)) sb[rr_cell_sb(&gr, a, b)].ngq++;
    }

    /* ---- allocate one block per non-empty sub-block ---- */
    for (k = 0; k < RR_MAXMESH; k++) {
        const int nv = sb[k].nrt * 3 + (sb[k].nrq + sb[k].ngq + sb[k].nsq) * 4;
        size_t hdr = (sizeof(TD5_MeshHeader) + 15u) & ~(size_t)15u;
        size_t bytes;
        uint8_t *blk;
        if (nv <= 0) continue;
        bytes = hdr + 3u * sizeof(TD5_PrimitiveCmd) + (size_t)nv * sizeof(TD5_MeshVertex);
        blk = (uint8_t *)calloc(1, bytes);
        if (!blk) { ok = 0; break; }
        sb[k].m = (TD5_MeshHeader *)blk;
        sb[k].m->commands = (TD5_PrimitiveCmd *)(blk + hdr);
        sb[k].v = (TD5_MeshVertex *)(blk + hdr + 3u * sizeof(TD5_PrimitiveCmd));
        sb[k].m->vertices = sb[k].v;
        sb[k].m->normals = NULL;
        sb[k].m->render_type = 259;
        sb[k].m->texture_page_id = 0;           /* not a billboard tag */
        sb[k].m->total_vertex_count = nv;
        rt->bytes += bytes;
    }
    if (!ok) goto done_free;

    /* ---- pass 2: fill. vertex order per mesh: road tris, road quads, ground, skirts ---- */
    for (i = 0; i < t->nspan; i++) {
        const TD5_StripSpan *sp = &t->span[i];
        uint32_t L, R; int n, kk;
        float pl[RR_MAX_LANES + 1][3], pr[RR_MAX_LANES + 1][3];
        RRSb *b;
        if (spsb[i] == 0xFF) continue;
        rr_span_ok(t, sp, &L, &R, &n);
        for (kk = 0; kk <= n; kk++) {
            rr_vert(t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[L + (uint32_t)kk], pl[kk]);
            rr_vert(t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[R + (uint32_t)kk], pr[kk]);
        }
        b = &sb[spsb[i]];
        for (kk = 0; kk < n; kk++) {
            TD5_MeshVertex *q = b->v + b->nrt * 3 + b->cq * 4;
            const uint32_t col = rr_shade(pl[kk], pl[kk + 1], pr[kk + 1]);
            rr_put(b, q + 0, pl[kk],     col, (float)kk,       0.0f);
            rr_put(b, q + 1, pl[kk + 1], col, (float)(kk + 1), 0.0f);
            rr_put(b, q + 2, pr[kk + 1], col, (float)(kk + 1), 1.0f);
            rr_put(b, q + 3, pr[kk],     col, (float)kk,       1.0f);
            b->cq++;
            rt->nroad++;
        }
    }
    for (i = 0; i < t->ntri; i++) {
        const RoamTri *tr = &t->tri[i];
        float p[3][3];
        uint32_t col;
        TD5_MeshVertex *q;
        RRSb *b;
        int kk;
        if (trsb[i] == 0xFF) continue;
        for (kk = 0; kk < 3; kk++) rr_vert(t, tr->ox, tr->oy, tr->oz, &t->vert[tr->v[kk]], p[kk]);
        col = rr_shade(p[0], p[1], p[2]);
        b = &sb[trsb[i]];
        q = b->v + b->ct * 3;
        /* a plain asphalt patch of the road page: UVs stay inside the stripe-free
         * middle of the page (the lane line sits on the u = 0 edge) */
        for (kk = 0; kk < 3; kk++)
            rr_put(b, q + kk, p[kk], col,
                   0.5f + (p[kk][0] - p[0][0]) / 80000.0f,
                   0.5f + (p[kk][2] - p[0][2]) / 80000.0f);
        b->ct++;
        rt->ntri++;
    }
    if (gr.h) {
        int a, bq;
        const int W = gr.G + 1;
        for (bq = 0; bq < gr.G; bq++) for (a = 0; a < gr.G; a++) {
            RRSb *b;
            float p[4][3];
            uint32_t col;
            TD5_MeshVertex *q;
            double x, z;
            static const int di[4] = { 0, 1, 1, 0 }, dj[4] = { 0, 0, 1, 1 };
            int kk;
            if (!rr_ground_cell_ok(&gr, a, bq)) continue;
            for (kk = 0; kk < 4; kk++) {
                rr_ground_point(t, a + di[kk], bq + dj[kk], gr.G, &x, &z);
                rr_pt(x, gr.h[(bq + dj[kk]) * W + a + di[kk]] - s_rr.ground_drop, z, p[kk]);
            }
            col = rr_shade(p[0], p[1], p[2]);
            b = &sb[rr_cell_sb(&gr, a, bq)];
            q = b->v + b->nrt * 3 + b->nrq * 4 + b->cg * 4;
            for (kk = 0; kk < 4; kk++)
                rr_put(b, q + kk, p[kk], col, (float)di[kk], (float)dj[kk]);
            b->cg++;
            rt->nground++;
        }
    }
    for (i = 0; i < t->nbedge; i++) {
        const RoamBEdge *be = &t->bedge[i];
        int32_t ox, oy, oz;
        float a[3], b2[3], al[3], bl[3];
        RRSb *b;
        TD5_MeshVertex *q;
        uint32_t col;
        const float h = (be->kind == ROAM_BEDGE_DECK) ? deck_h : kerb_h;
        if (besb[i] == 0xFF) continue;
        rr_bedge_prim(t, be, &ox, &oy, &oz);
        rr_vert(t, ox, oy, oz, &t->vert[be->v[0]], a);
        rr_vert(t, ox, oy, oz, &t->vert[be->v[1]], b2);
        al[0] = a[0];  al[1] = a[1] - h;  al[2] = a[2];
        bl[0] = b2[0]; bl[1] = b2[1] - h; bl[2] = b2[2];
        col = rr_skirt_col(be->kind);
        b = &sb[besb[i]];
        q = b->v + b->nrt * 3 + (b->nrq + b->ngq) * 4 + b->cs * 4;
        rr_put(b, q + 0, a,  col, 0.5f, 0.5f);
        rr_put(b, q + 1, b2, col, 0.5f, 0.5f);
        rr_put(b, q + 2, bl, col, 0.5f, 0.5f);
        rr_put(b, q + 3, al, col, 0.5f, 0.5f);
        b->cs++;
        rt->nskirt++;
    }

    /* ---- headers, commands, bounding spheres ---- */
    for (k = 0; k < RR_MAXMESH; k++) {
        TD5_MeshHeader *m = sb[k].m;
        TD5_PrimitiveCmd *c;
        int nc = 0;
        float cx, cy, cz, r, dx, dy, dz;
        if (!m) continue;
        c = m->commands;
        if (sb[k].nrt || sb[k].nrq) {
            c[nc].dispatch_type = 0; c[nc].texture_page_id = (int16_t)s_rr.page_road;
            c[nc].triangle_count = (uint16_t)sb[k].nrt; c[nc].quad_count = (uint16_t)sb[k].nrq;
            nc++;
        }
        if (sb[k].ngq) {
            c[nc].dispatch_type = 0; c[nc].texture_page_id = (int16_t)s_rr.page_ground;
            c[nc].triangle_count = 0; c[nc].quad_count = (uint16_t)sb[k].ngq;
            nc++;
        }
        if (sb[k].nsq) {
            c[nc].dispatch_type = 0; c[nc].texture_page_id = (int16_t)s_rr.page_kerb;
            c[nc].triangle_count = 0; c[nc].quad_count = (uint16_t)sb[k].nsq;
            nc++;
        }
        m->command_count = nc;
        cx = 0.5f * (sb[k].lo[0] + sb[k].hi[0]);
        cy = 0.5f * (sb[k].lo[1] + sb[k].hi[1]);
        cz = 0.5f * (sb[k].lo[2] + sb[k].hi[2]);
        dx = sb[k].hi[0] - cx; dy = sb[k].hi[1] - cy; dz = sb[k].hi[2] - cz;
        r = sqrtf(dx * dx + dy * dy + dz * dz) + 1.0f;
        m->bounding_center_x = cx; m->bounding_center_y = cy; m->bounding_center_z = cz;
        m->bounding_radius = r;
        rt->mesh[rt->nmesh] = m;
        rt->sph[rt->nmesh][0] = cx; rt->sph[rt->nmesh][1] = cy;
        rt->sph[rt->nmesh][2] = cz; rt->sph[rt->nmesh][3] = r;
        rt->nmesh++;
        sb[k].m = NULL;                        /* owned by the tile now */
    }
    if (rt->nmesh > 0) {
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        float dx, dy, dz;
        for (k = 0; k < rt->nmesh; k++) {
            const float *s = rt->sph[k];
            int a;
            for (a = 0; a < 3; a++) {
                if (s[a] - s[3] < lo[a]) lo[a] = s[a] - s[3];
                if (s[a] + s[3] > hi[a]) hi[a] = s[a] + s[3];
            }
        }
        rt->cx = 0.5f * (lo[0] + hi[0]); rt->cy = 0.5f * (lo[1] + hi[1]); rt->cz = 0.5f * (lo[2] + hi[2]);
        dx = hi[0] - rt->cx; dy = hi[1] - rt->cy; dz = hi[2] - rt->cz;
        rt->r = sqrtf(dx * dx + dy * dy + dz * dz);
        rt->dl.count = 0;
        rt->dl.meshes = rt->vis;
    }
    rr_ground_free(&gr);
    free(spsb); free(trsb); free(besb);
    if (rt->nmesh == 0) { free(rt); return NULL; }
    return rt;

done_free:
    rr_ground_free(&gr);
    for (k = 0; k < RR_MAXMESH; k++) free(sb[k].m);
done_counts:
    free(spsb); free(trsb); free(besb);
    free(rt);
    return NULL;
}

/* ------------------------------------------------------------- tile table ---- */

static int rr_find(int tx, int tz)
{
    int i;
    for (i = 0; i < s_rr.ntile; i++)
        if (s_rr.tile[i]->tx == tx && s_rr.tile[i]->tz == tz) return i;
    return -1;
}

void td5_roam_render_remove_tile(int tx, int tz)
{
    const int i = rr_find(tx, tz);
    if (i < 0) return;
    rr_free_tile(s_rr.tile[i]);
    s_rr.tile[i] = s_rr.tile[--s_rr.ntile];
}

int td5_roam_render_add_tile(const RoamTile *t)
{
    RRTile *rt;
    uint32_t t0;
    if (!s_rr.began || !t) return 0;
    td5_roam_render_remove_tile(t->tx, t->tz);
    t0 = td5_plat_time_ms();
    rt = rr_bake(t);
    if (!rt) return 0;
    if (s_rr.ntile == s_rr.captile) {
        const int nc = s_rr.captile ? s_rr.captile * 2 : 64;
        RRTile **nt = (RRTile **)realloc(s_rr.tile, (size_t)nc * sizeof *nt);
        if (!nt) { rr_free_tile(rt); return 0; }
        s_rr.tile = nt; s_rr.captile = nc;
    }
    s_rr.tile[s_rr.ntile++] = rt;
    TD5_LOG_D(LOG_TAG, "ROAM_RENDER_TILE %d,%d meshes=%d road_quads=%u patch_tris=%u ground_quads=%u skirts=%u "
              "bytes=%.2f MB bake_ms=%u", rt->tx, rt->tz, rt->nmesh, rt->nroad, rt->ntri, rt->nground,
              rt->nskirt, (double)rt->bytes / 1048576.0, (unsigned)(td5_plat_time_ms() - t0));
    return 1;
}

/* ---------------------------------------------------------------- session ---- */

int td5_roam_render_began(void) { return s_rr.began; }

int td5_roam_render_begin(const char *place, const TD5_RoamRenderFrame *frame)
{
    char path[512], wpath[512], json_path[512];
    TD5_File *f;
    int64_t n;
    char *buf;
    if (s_rr.began && place && strcmp(place, s_rr.place) == 0) return 1;
    if (s_rr.began) td5_roam_render_end();
    memset(&s_rr, 0, sizeof s_rr);
    if (!place || !place[0] || !frame) return 0;
    snprintf(s_rr.place, sizeof s_rr.place, "%s", place);
    s_rr.f = *frame;

    td5_geo_source_path(json_path, sizeof json_path, place, "roam/ROAMGRID.JSON");
    f = td5_plat_file_open(json_path, "rb");
    if (f) {
        n = td5_plat_file_size(f);
        buf = (n > 0 && n < 65536) ? (char *)malloc((size_t)n + 1u) : NULL;
        if (buf && td5_plat_file_read(f, buf, (size_t)n) == (size_t)n) {
            buf[n] = '\0';
            s_rr.grid_ok = roam_grid_parse(buf, &s_rr.grid);
        }
        free(buf);
        td5_plat_file_close(f);
    }
    if (!s_rr.grid_ok) roam_grid_default(place, &s_rr.grid);

    s_rr.page_road   = td5_env_int("TD5RE_ROAM_RENDER_PAGE_ROAD",   RR_PAGE_ROAD,   0, 1023);
    s_rr.page_ground = td5_env_int("TD5RE_ROAM_RENDER_PAGE_GROUND", RR_PAGE_GROUND, 0, 1023);
    s_rr.page_kerb   = td5_env_int("TD5RE_ROAM_RENDER_PAGE_KERB",   RR_PAGE_KERB,   0, 1023);
    s_rr.use_ground  = td5_env_int("TD5RE_ROAM_RENDER_GROUND", 1, 0, 1);
    s_rr.cell_m      = td5_env_int("TD5RE_ROAM_RENDER_CELL_M", 16, 8, 64);
    if (64 % s_rr.cell_m != 0) s_rr.cell_m = 16;            /* must divide a sub-block */
    s_rr.lift        = td5_env_int("TD5RE_ROAM_RENDER_LIFT_CM", 0, -1000, 1000) * (RR_UPM / 100.0);
    s_rr.ground_drop = td5_env_int("TD5RE_ROAM_RENDER_GROUND_DROP_CM", 40, 0, 500) * (RR_UPM / 100.0);
    s_rr.radius      = td5_env_int("TD5RE_ROAM_RENDER_R", 500, 50, 5000) * RR_UPM;

    if (s_rr.use_ground) {
        s_rr.hc = (TD5_RoamHeight *)calloc(1, sizeof *s_rr.hc);
        if (s_rr.hc) {
            const int ex100 = td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400);
            td5_geo_source_path(path, sizeof path, place, "HEIGHT.R16");
            td5_geo_source_path(wpath, sizeof wpath, place, "WATER.R8");
            s_rr.hc_ok = td5_roam_height_open(s_rr.hc, path, wpath, (double)ex100 / 100.0);
            if (!s_rr.hc_ok) TD5_LOG_W(LOG_TAG, "roam render: no height source at %s, ground off", path);
        }
    }
    s_rr.began = 1;
    TD5_LOG_I(LOG_TAG, "roam render: begin place=%s frame origin=(%d,%d) dy=%.0f route=%d pages road=%d ground=%d "
              "kerb=%d ground=%s cell=%d m drop=%.0f units lift=%.0f R=%.0f m",
              place, (int)s_rr.f.ox, (int)s_rr.f.oz, s_rr.f.dy, s_rr.f.route, s_rr.page_road, s_rr.page_ground,
              s_rr.page_kerb, (s_rr.use_ground && s_rr.hc_ok) ? "on" : "off", s_rr.cell_m,
              s_rr.ground_drop, s_rr.lift, s_rr.radius / RR_UPM);
    return 1;
}

void td5_roam_render_end(void)
{
    int i;
    for (i = 0; i < s_rr.ntile; i++) rr_free_tile(s_rr.tile[i]);
    free(s_rr.tile);
    if (s_rr.hc) { td5_roam_height_close(s_rr.hc); free(s_rr.hc); }
    free(s_rr.tried);
    memset(&s_rr, 0, sizeof s_rr);
}

void td5_roam_render_stats(TD5_RoamRenderStats *o)
{
    int i;
    memset(o, 0, sizeof *o);
    o->tiles = s_rr.ntile;
    for (i = 0; i < s_rr.ntile; i++) {
        const RRTile *t = s_rr.tile[i];
        o->meshes += t->nmesh;
        o->road_quads += t->nroad; o->patch_tris += t->ntri;
        o->ground_quads += t->nground; o->skirt_quads += t->nskirt;
        o->bytes_mb += (double)t->bytes / 1048576.0;
    }
    o->drawn_tiles = s_rr.st_drawn_tiles; o->drawn_meshes = s_rr.st_drawn_meshes;
    o->drawn_quads = s_rr.st_drawn_quads;
    o->last_ms = s_rr.st_last_ms;
    o->avg_ms = s_rr.st_frames ? s_rr.st_sum_ms / (double)s_rr.st_frames : 0.0;
    o->max_ms = s_rr.st_max_ms;
    o->frames = s_rr.st_frames;
}

/* ------------------------------------------------------------- DEV loader ---- */
/* REPLACE with sibling B's resident set: B calls td5_roam_render_add_tile /
 * _remove_tile itself. This reads ROAMTILE files straight from disk so the render
 * can be proven alone. near = the tiles within TD5RE_ROAM_RENDER_REACH (default 2)
 * of the eye's tile, two bakes per frame; all = every casco tile at once. */

static int rr_tried(int tx, int tz)
{
    int i;
    for (i = 0; i < s_rr.ntried; i++)
        if (s_rr.tried[2 * i] == tx && s_rr.tried[2 * i + 1] == tz) return 1;
    return 0;
}

static void rr_mark_tried(int tx, int tz)
{
    if (s_rr.ntried == s_rr.captried) {
        const int nc = s_rr.captried ? s_rr.captried * 2 : 256;
        int *n = (int *)realloc(s_rr.tried, (size_t)nc * 2u * sizeof(int));
        if (!n) return;
        s_rr.tried = n; s_rr.captried = nc;
    }
    s_rr.tried[2 * s_rr.ntried] = tx; s_rr.tried[2 * s_rr.ntried + 1] = tz;
    s_rr.ntried++;
}

static int rr_dev_load(int tx, int tz)
{
    RoamTile t;
    char path[512], err[160];
    int ok = 0;
    if (rr_find(tx, tz) >= 0 || rr_tried(tx, tz)) return 0;
    td5_roam_tile_path(path, sizeof path, NULL, s_rr.place, tx, tz);
    memset(&t, 0, sizeof t);
    if (td5_roam_tile_read(path, &t, 0, err, sizeof err)) {
        ok = td5_roam_render_add_tile(&t);
        td5_roam_tile_free(&t);
    }
    if (!ok) rr_mark_tried(tx, tz);
    return ok;
}

static void rr_eye_place(double *px, double *pz)
{
    const double ex = (double)g_cameraPos[0], ez = (double)g_cameraPos[2];
    *px = ex + (double)s_rr.f.ox; *pz = ez + (double)s_rr.f.oz;
#ifndef TD5RE_RELEASE
    if (s_rr.f.route) td5_roam_view_frame_inv(s_rr.place, ex, ez, px, pz);
#endif
}

static void rr_dev_stream(void)
{
    double px, pz;
    int ctx, ctz, dx, dz, budget = 2, i;
    if (s_rr.dev_mode == 2) {
        if (!s_rr.dev_loaded_all) {
            int (*list)[2] = (int (*)[2])malloc(2048u * sizeof *list);
            int n, k, loaded = 0;
            uint32_t t0 = td5_plat_time_ms();
            s_rr.dev_loaded_all = 1;
            if (!list) return;
            n = td5_roam_casco_tiles(&s_rr.grid, list, 2048);
            for (k = 0; k < n; k++) loaded += rr_dev_load(list[k][0], list[k][1]);
            free(list);
            TD5_LOG_I(LOG_TAG, "roam render: dev loaded %d of %d casco tiles in %u ms", loaded, n,
                      (unsigned)(td5_plat_time_ms() - t0));
        }
        return;
    }
    rr_eye_place(&px, &pz);
    roam_grid_tile_of(&s_rr.grid, px, pz, &ctx, &ctz);
    for (dz = -s_rr.dev_reach; dz <= s_rr.dev_reach && budget > 0; dz++)
        for (dx = -s_rr.dev_reach; dx <= s_rr.dev_reach && budget > 0; dx++)
            if (rr_dev_load(ctx + dx, ctz + dz)) budget--;
    for (i = s_rr.ntile - 1; i >= 0; i--) {                    /* drop what the eye left */
        const RRTile *t = s_rr.tile[i];
        if (abs(t->tx - ctx) > s_rr.dev_reach + 2 || abs(t->tz - ctz) > s_rr.dev_reach + 2)
            td5_roam_render_remove_tile(t->tx, t->tz);
    }
}

#ifndef TD5RE_RELEASE
static int rr_dev_overlay_on(void)
{
    if (!s_rr.dev_overlay) s_rr.dev_overlay = td5_env_int("TD5RE_ROAM_RENDER", 0, 0, 1) ? 2 : 1;
    return s_rr.dev_overlay == 2;
}
int td5_roam_render_hides_strip(void)
{
    if (!rr_dev_overlay_on()) return 0;
    if (!s_rr.dev_hide) s_rr.dev_hide = td5_env_int("TD5RE_ROAM_RENDER_HIDE_STRIP", 0, 0, 1) ? 2 : 1;
    return s_rr.dev_hide == 2;
}
#else
static int rr_dev_overlay_on(void) { return 0; }
#endif

static int rr_roam_active(void)
{
#ifdef RR_HAVE_MODE
    return td5_roam_active();
#else
    return 0;
#endif
}

int td5_roam_render_wanted(void)
{
    return s_rr.began || rr_roam_active() || rr_dev_overlay_on();
}

/* Lazy session start for the two entry points that exist before the frontend /
 * resident-set plumbing does: a real roam session (frame from the mode), and the
 * DEV overlay (route frame on a loaded geo level). */
static void rr_autobegin(void)
{
    TD5_RoamRenderFrame f;
    const char *slug = td5_geo_place_slug();
    const char *mode;
    memset(&f, 0, sizeof f);
    if (!slug || !slug[0]) slug = "la_plata_partido";
    if (rr_roam_active()) {
#ifdef RR_HAVE_MODE
        td5_roam_world_origin(&f.ox, &f.oz);
#endif
        f.dy = (double)td5_env_int("TD5RE_ROAM_RENDER_DY", 0, -100000000, 100000000);
    } else if (rr_dev_overlay_on()) {
#ifndef TD5RE_RELEASE
        f.route = 1;
        f.dy = td5_roam_view_datum_units(slug);
#endif
    } else {
        return;
    }
    if (!td5_roam_render_begin(slug, &f)) return;
    mode = getenv("TD5RE_ROAM_RENDER_TILES");
    s_rr.dev_mode = (mode && strcmp(mode, "all") == 0) ? 2 : 1;
    s_rr.dev_reach = td5_env_int("TD5RE_ROAM_RENDER_REACH", 2, 1, 6);
}

#ifndef TD5RE_RELEASE
/* DEV pose generator. TD5RE_ROAM_RENDER_POSES=<file> with lines
 *   name:lat,lon,h_above_ground_m,yaw_deg,pitch_deg      (yaw 0 = north, as seen on the map)
 * writes <file>.tour.txt in the free-cam tour format (route-frame WORLD units = strip
 * units x 256, y from the place height minus the strip datum) and returns 1 (the caller
 * exits). Only meaningful with a built geo level whose _route/ frame is the target of the
 * tour (verify/geo_tour.ps1 with TD5RE_ROAM_RENDER=1 TD5RE_ROAM_RENDER_HIDE_STRIP=1). */
int td5_roam_render_dev_poses(void)
{
    const char *in = getenv("TD5RE_ROAM_RENDER_POSES");
    const char *slug = td5_geo_place_slug();
    char *buf = NULL, outp[600], path[512], wpath[512], hp[512];
    TD5_File *f;
    FILE *o;
    int64_t n;
    char *line, *save = NULL;
    double lat0 = 0, lon0 = 0, mdlat = 0, mdlon = 0, datum;
    TD5_RoamHeight *hc;
    int cnt = 0;
    if (!in || !in[0]) return 0;
    if (!slug || !slug[0]) slug = "la_plata_partido";
    td5_geo_source_path(path, sizeof path, slug, "PLACE.JSON");
    {   /* place projection: lat0/lon0/m_per_deg from PLACE.JSON, scanned as text */
        char *js; TD5_File *pf = td5_plat_file_open(path, "rb"); int64_t pn;
        const char *k;
        if (!pf) { fprintf(stderr, "roam poses: no %s
", path); return 1; }
        pn = td5_plat_file_size(pf);
        js = (char *)malloc((size_t)pn + 1u);
        if (!js || td5_plat_file_read(pf, js, (size_t)pn) != (size_t)pn) { td5_plat_file_close(pf); free(js); return 1; }
        js[pn] = '\0'; td5_plat_file_close(pf);
        k = strstr(js, "\"projection\"");
        if (k) {
            const char *a;
            if ((a = strstr(k, "\"lat0\"")))          lat0  = atof(strchr(a, ':') + 1);
            if ((a = strstr(k, "\"lon0\"")))          lon0  = atof(strchr(a, ':') + 1);
            if ((a = strstr(k, "\"m_per_deg_lat\""))) mdlat = atof(strchr(a, ':') + 1);
            if ((a = strstr(k, "\"m_per_deg_lon\""))) mdlon = atof(strchr(a, ':') + 1);
        }
        free(js);
        if (mdlat <= 0.0 || mdlon <= 0.0) { fprintf(stderr, "roam poses: no projection
"); return 1; }
    }
    f = td5_plat_file_open(in, "rb");
    if (!f) { fprintf(stderr, "roam poses: cannot open %s
", in); return 1; }
    n = td5_plat_file_size(f);
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf || td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) { td5_plat_file_close(f); free(buf); return 1; }
    buf[n] = '\0'; td5_plat_file_close(f);
    hc = (TD5_RoamHeight *)calloc(1, sizeof *hc);
    td5_geo_source_path(hp, sizeof hp, slug, "HEIGHT.R16");
    td5_geo_source_path(wpath, sizeof wpath, slug, "WATER.R8");
    if (!hc || !td5_roam_height_open(hc, hp, wpath,
            (double)td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400) / 100.0)) {
        fprintf(stderr, "roam poses: no height source
"); free(buf); free(hc); return 1;
    }
    datum = td5_roam_view_datum_units(slug);
    snprintf(outp, sizeof outp, "%s.tour.txt", in);
    o = fopen(outp, "wb");
    for (line = strtok_s(buf, "
;", &save); line && o; line = strtok_s(NULL, "
;", &save)) {
        char nm[48];
        double lat, lon, hm, yaw, pit, px, pz, xr, zr, xn, zn, g = 0.0, yr;
        const char *c = strchr(line, ':');
        size_t l;
        while (*line == ' ') line++;
        if (!c || *line == '#') continue;
        l = (size_t)(c - line); if (l >= sizeof nm) l = sizeof nm - 1;
        memcpy(nm, line, l); nm[l] = '\0';
        if (sscanf(c + 1, "%lf,%lf,%lf,%lf,%lf", &lat, &lon, &hm, &yaw, &pit) != 5) continue;
        px = (lon - lon0) * mdlon * 430.0;
        pz = (lat - lat0) * mdlat * 430.0;
        if (!td5_roam_height_prefetch(hc, px - 2000.0, pz - 2000.0, px + 2000.0, pz + 2000.0) ||
            !td5_roam_height_sample(hc, px, pz, &g, NULL)) { fprintf(stderr, "roam poses: no height at %s
", nm); continue; }
        if (!td5_roam_view_frame(slug, px, pz, &xr, &zr) || !td5_roam_view_frame(slug, px, pz + 430.0, &xn, &zn)) continue;
        yr = yaw * 3.14159265358979 / 180.0 + atan2(xn - xr, zn - zr);   /* map north -> route +Z-relative */
        fprintf(o, "%s:%.0f,%.0f,%.0f,%.3f,%.3f
", nm, xr * 256.0, (g + hm * 430.0 - datum) * 256.0, zr * 256.0,
                yr * 180.0 / 3.14159265358979, pit);
        cnt++;
    }
    if (o) fclose(o);
    td5_roam_height_close(hc); free(hc); free(buf);
    fprintf(stderr, "roam poses: %d pose(s) -> %s (datum %.0f units)
", cnt, outp, datum);
    return 1;
}
#endif

/* ------------------------------------------------------------------- draw ---- */

void td5_roam_render_tiles(int view)
{
    uint32_t t0;
    int i, k;
    int tiles = 0, meshes = 0;
    uint32_t quads = 0;
    double ms;
    const float ex = g_cameraPos[0], ey = g_cameraPos[1], ez = g_cameraPos[2];
    const float R = (float)s_rr.radius;
    (void)view;

    if (!s_rr.began) rr_autobegin();
    if (!s_rr.began) return;
    t0 = td5_plat_time_us() ? (uint32_t)(td5_plat_time_us() & 0xFFFFFFFFu) : 0u;
    if (s_rr.dev_mode) rr_dev_stream();

    for (i = 0; i < s_rr.ntile; i++) {
        RRTile *t = s_rr.tile[i];
        float dx = t->cx - ex, dy = t->cy - ey, dz = t->cz - ez;
        int n = 0;
        if (sqrtf(dx * dx + dy * dy + dz * dz) - t->r > R) continue;
        if (!td5_render_is_sphere_visible(t->cx, t->cy, t->cz, t->r)) continue;
        for (k = 0; k < t->nmesh; k++) {
            const float *s = t->sph[k];
            dx = s[0] - ex; dy = s[1] - ey; dz = s[2] - ez;
            if (sqrtf(dx * dx + dy * dy + dz * dz) - s[3] > R) continue;
            t->vis[n++] = t->mesh[k];
            quads += (uint32_t)(t->mesh[k]->total_vertex_count / 4);
        }
        if (n == 0) continue;
        t->dl.count = (uint32_t)n;
        t->dl.meshes = t->vis;
        td5_render_span_display_list(&t->dl);
        tiles++; meshes += n;
    }

    {
        const uint32_t t1 = (uint32_t)(td5_plat_time_us() & 0xFFFFFFFFu);
        ms = (double)(uint32_t)(t1 - t0) / 1000.0;
    }
    s_rr.st_frames++;
    s_rr.st_sum_ms += ms; s_rr.st_last_ms = ms;
    if (ms > s_rr.st_max_ms) s_rr.st_max_ms = ms;
    s_rr.st_drawn_tiles = tiles; s_rr.st_drawn_meshes = meshes; s_rr.st_drawn_quads = quads;
    if ((s_rr.st_frames % 240u) == 1u) {
        TD5_RoamRenderStats st;
        td5_roam_render_stats(&st);
        TD5_LOG_I(LOG_TAG, "ROAM_RENDER resident=%d tiles meshes=%d (road %u quads, %u patch tris, ground %u, "
                  "skirts %u) %.1f MB cpu | drawn %d tiles %d meshes %u quads | submit last %.2f avg %.2f max %.2f ms "
                  "(%u frames)", st.tiles, st.meshes, st.road_quads, st.patch_tris, st.ground_quads,
                  st.skirt_quads, st.bytes_mb, st.drawn_tiles, st.drawn_meshes, st.drawn_quads,
                  st.last_ms, st.avg_ms, st.max_ms, st.frames);
    }
}
