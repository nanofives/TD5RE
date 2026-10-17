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
 * RESIDENCY. [F3] In a streamed session (td5_roam_stream.c) the stream worker bakes
 * each tile (td5_roam_render_bake, its own height source) and the main thread installs
 * it between frames (td5_roam_render_install); a dropped tile is retired for 3 ticks
 * before it is freed. td5_roam_render_add_tile / _remove_tile stay for the F2 path
 * (TD5RE_ROAM_STREAM=0), where the DEV loader at the bottom bakes near the eye on the
 * main thread.
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
#include "td5_roam_scenery.h"   /* [F4a] the tile's SCENERY layer */
#include "td5_light2.h"         /* [F4a] td5_light2_active: derived normals */

#if defined(__has_include)
#  if __has_include("td5_roam_mode.h")
#    include "td5_roam_mode.h"
#    include "td5_roam_locate.h"   /* [F2-B] TD5_RoamResident: tiles come from the physics resident set */
#    define RR_HAVE_MODE 1
#  endif
#endif

#define LOG_TAG "roam"

#define RR_SUB            4                  /* sub-blocks per tile side (64 m)     */
#define RR_MAXMESH        (RR_SUB * RR_SUB)
#define RR_UPM            430.0              /* units per metre                     */
#define RR_MAX_LANES      16
/* Pages of the HOST level (td5_roam_mode.h). F2-F: a roam session asks the mode for them
 * (td5_roam_host_pages: the place's own geo level, TD5_TG_PAGE_ROAD / _GREEN / _SIDEWALK); the
 * numbers below are the SHIPPED-LEVEL FALLBACK (level023 "Moscow", 475 pages) and the dev overlay
 * defaults. Plain look: 91 is
 * a featureless dark-grey asphalt (92..95 the same, 96/97 carry an edge line, 98/99 a
 * crossing), 9 is a plain green lawn. Looked up on a contact sheet of
 * re/assets/levels/level023/textures.src/pages. Another host needs other numbers:
 * TD5RE_ROAM_RENDER_PAGE_ROAD / _GROUND / _KERB override. The kerb skirt uses the 1x1
 * white page of the HUD (TD5 page map) with a grey vertex colour. */
#define RR_PAGE_ROAD      91
#define RR_PAGE_GROUND    9
#define RR_PAGE_KERB      HUD_WHITE_TEX_PAGE
/* Junction patch UVs: units of road per page. 80000 (186 m) magnified the geo asphalt's grain into
 * blotches; 24000 (56 m, +-22 m inside the stripe-free middle) keeps a patch uniform. */
#define RR_PATCH_UV_UNITS 24000.0f

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
    /* [F4a] the SCENERY layer (td5_roam_scenery.c): runtime headers over the
     * layer's MODELS records, one display list per 64 m sub-block */
    TD5_RoamScenery *scn;
    TD5_MeshHeader  *smesh;                  /* scn->nrec runtime headers           */
    TD5_VertexNormal *snorm;                 /* derived flat normals (light2 only)  */
    TD5_MeshHeader **sptr;                   /* pointers to them, sub-block order   */
    float            ssph[TD5_ROAM_SCN_NSB][4];
    TD5_SpanDisplayList sdl[TD5_ROAM_SCN_NSB];
    uint32_t         nscn;
} RRTile;

typedef struct { RRTile *t; uint32_t tick; } RRRetired;
#define RR_RETIRE_TICKS   3u     /* D3D12_FRAME_COUNT (2) + 1, design 3.5 */

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
    /* [F3] streamed: tiles come from td5_roam_stream.c (no DEV loader); retired tiles
     * wait RR_RETIRE_TICKS main-thread ticks before they are freed */
    int       streamed;
    int       fog_done;
    TD5_RoamScnSource *scn;                  /* [F4a] main-thread scenery source (F2 path only) */
    int       scn_failed;
    RRRetired *retired;
    int       nretired, capretired;
    uint32_t  retire_clock;
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
    if (a < 0) a = 0;
    if (a >= RR_SUB) a = RR_SUB - 1;
    if (b < 0) b = 0;
    if (b >= RR_SUB) b = RR_SUB - 1;
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

static int rr_ground_build(const RoamTile *t, RRGround *g, TD5_RoamHeight *hc)
{
    const int G = (int)(256 / s_rr.cell_m);
    double minx = 1e18, minz = 1e18, maxx = -1e18, maxz = -1e18, x, z;
    int i, j, c, ok = 0;
    uint32_t e, q;
    const double S = (double)s_rr.grid.tile;
    (void)S;
    memset(g, 0, sizeof *g);
    g->G = G;
    if (!s_rr.use_ground || !hc) return 0;
    g->h = (double *)malloc((size_t)(G + 1) * (size_t)(G + 1) * sizeof(double));
    g->hole = (unsigned char *)calloc((size_t)G * (size_t)G, 1);
    if (!g->h || !g->hole) { free(g->h); free(g->hole); memset(g, 0, sizeof *g); return 0; }
    for (c = 0; c < 4; c++) {
        rr_ground_point(t, (c & 1) ? G : 0, (c & 2) ? G : 0, G, &x, &z);
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (z < minz) minz = z;
        if (z > maxz) maxz = z;
    }
    if (!td5_roam_height_prefetch(hc, minx - 2000.0, minz - 2000.0, maxx + 2000.0, maxz + 2000.0)) {
        free(g->h); free(g->hole); memset(g, 0, sizeof *g);
        return 0;
    }
    for (j = 0; j <= G; j++) {
        for (i = 0; i <= G; i++) {
            double h;
            rr_ground_point(t, i, j, G, &x, &z);
            if (td5_roam_height_sample(hc, x, z, &h, NULL)) { g->h[j * (G + 1) + i] = h; ok++; }
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
    free(rt->smesh);
    free(rt->snorm);
    free(rt->sptr);
    td5_roam_scn_free(rt->scn);
    free(rt);
}

/* [F4a] Flat face normals for the scenery meshes, the same rule as
 * td5_track_derive_missing_normals (track_face_normal_up, sequential cursor,
 * billboards and opcode 4 left without) -- the strip derives them for its
 * generated scenery when light2 is active, so a roam building lights the same. */
static void rr_face_normal_up(const TD5_MeshVertex *a, const TD5_MeshVertex *b, const TD5_MeshVertex *c, float out[3])
{
    float ux = b->pos_x - a->pos_x, uy = b->pos_y - a->pos_y, uz = b->pos_z - a->pos_z;
    float vx = c->pos_x - a->pos_x, vy = c->pos_y - a->pos_y, vz = c->pos_z - a->pos_z;
    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const float len2 = nx * nx + ny * ny + nz * nz;
    float inv;
    if (len2 < 1e-12f) { out[0] = out[1] = out[2] = 0.0f; return; }
    inv = 1.0f / sqrtf(len2);
    nx *= inv; ny *= inv; nz *= inv;
    if (ny < -0.01f) { nx = -nx; ny = -ny; nz = -nz; }
    out[0] = nx; out[1] = ny; out[2] = nz;
}

static void rr_scenery_normals(RRTile *rt)
{
    uint32_t i, total = 0, at = 0;
    if (!td5_light2_active()) return;
    for (i = 0; i < rt->nscn; i++) total += (uint32_t)rt->smesh[i].total_vertex_count;
    if (!total) return;
    rt->snorm = (TD5_VertexNormal *)calloc(total, sizeof(TD5_VertexNormal));
    if (!rt->snorm) return;
    for (i = 0; i < rt->nscn; i++) {
        TD5_MeshHeader *m = &rt->smesh[i];
        TD5_VertexNormal *nn = rt->snorm + at;
        int ci, cursor = 0;
        at += (uint32_t)m->total_vertex_count;
        if (m->texture_page_id == 1 || m->texture_page_id == 2 || !m->vertices || !m->commands) continue;
        for (ci = 0; ci < m->command_count; ci++) {
            const TD5_PrimitiveCmd *cmd = &m->commands[ci];
            const int needed = cmd->triangle_count * 3 + cmd->quad_count * 4;
            int t, k;
            float fn[3];
            if (cursor + needed > m->total_vertex_count) break;
            if (cmd->dispatch_type == 4) { cursor += needed; continue; }
            for (t = 0; t < cmd->triangle_count; t++) {
                const TD5_MeshVertex *v = m->vertices + cursor + t * 3;
                rr_face_normal_up(&v[0], &v[1], &v[2], fn);
                for (k = 0; k < 3; k++) { nn[cursor + t * 3 + k].nx = fn[0]; nn[cursor + t * 3 + k].ny = fn[1]; nn[cursor + t * 3 + k].nz = fn[2]; }
            }
            for (t = 0; t < cmd->quad_count; t++) {
                const int q0 = cursor + cmd->triangle_count * 3 + t * 4;
                const TD5_MeshVertex *v = m->vertices + q0;
                rr_face_normal_up(&v[0], &v[1], &v[2], fn);
                for (k = 0; k < 4; k++) { nn[q0 + k].nx = fn[0]; nn[q0 + k].ny = fn[1]; nn[q0 + k].nz = fn[2]; }
            }
            cursor += needed;
        }
        m->normals = nn;
    }
    rt->bytes += (size_t)total * sizeof(TD5_VertexNormal);
}

/* [F4a] Runtime headers over the scenery records (td5_track_runtime_mesh_for's
 * relocation: header bytes up to the links, links rebased off the record), moved
 * from the TILE-LOCAL frame they were written in to the render frame: the vertices
 * stay tile-local and mesh->origin (24.8, the renderer's own convention) carries
 * the tile origin; bounding centres are render-float. Takes ownership of sc. */
static int rr_attach_scenery(RRTile *rt, TD5_RoamScenery *sc)
{
    uint32_t i;
    int k;
    const double offx = (double)sc->org_x - (double)s_rr.f.ox;
    const double offz = (double)sc->org_z - (double)s_rr.f.oz;
    const double offy = -s_rr.f.dy + s_rr.lift;
    if (!sc->nrec) { rt->scn = sc; return 1; }
    rt->smesh = (TD5_MeshHeader *)calloc(sc->nrec, sizeof(TD5_MeshHeader));
    rt->sptr = (TD5_MeshHeader **)calloc(sc->nrec, sizeof(TD5_MeshHeader *));
    if (!rt->smesh || !rt->sptr) { free(rt->smesh); free(rt->sptr); rt->smesh = NULL; rt->sptr = NULL; td5_roam_scn_free(sc); return 0; }
    for (i = 0; i < sc->nrec; i++) {
        const uint8_t *rec = sc->blob + sc->rec_off[i];
        TD5_MeshHeader *m = &rt->smesh[i];
        uint32_t co, vo;
        memcpy(m, rec, TD5_MESH_DISK_OFF_COMMANDS);
        memcpy(&co, rec + TD5_MESH_DISK_OFF_COMMANDS, 4);
        memcpy(&vo, rec + TD5_MESH_DISK_OFF_VERTICES, 4);
        m->commands = co ? (TD5_PrimitiveCmd *)(void *)(rec + co) : NULL;
        m->vertices = vo ? (TD5_MeshVertex *)(void *)(rec + vo) : NULL;
        m->normals = NULL;
        m->runtime_flags = 0;
        m->origin_x = (float)((double)m->origin_x + offx * 256.0);
        m->origin_y = (float)((double)m->origin_y + offy * 256.0);
        m->origin_z = (float)((double)m->origin_z + offz * 256.0);
        m->bounding_center_x = (float)((double)m->bounding_center_x + offx);
        m->bounding_center_y = (float)((double)m->bounding_center_y + offy);
        m->bounding_center_z = (float)((double)m->bounding_center_z + offz);
        rt->sptr[i] = m;
    }
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) {
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        uint32_t j;
        rt->sdl[k].count = sc->sb_count[k];
        rt->sdl[k].meshes = rt->sptr + sc->sb_first[k];
        rt->ssph[k][3] = 0.0f;
        for (j = 0; j < sc->sb_count[k]; j++) {
            const TD5_MeshHeader *m = rt->sptr[sc->sb_first[k] + j];
            const float c3[3] = { m->bounding_center_x, m->bounding_center_y, m->bounding_center_z };
            int a;
            for (a = 0; a < 3; a++) {
                if (c3[a] - m->bounding_radius < lo[a]) lo[a] = c3[a] - m->bounding_radius;
                if (c3[a] + m->bounding_radius > hi[a]) hi[a] = c3[a] + m->bounding_radius;
            }
        }
        if (sc->sb_count[k]) {
            const float dx = 0.5f * (hi[0] - lo[0]), dy = 0.5f * (hi[1] - lo[1]), dz = 0.5f * (hi[2] - lo[2]);
            rt->ssph[k][0] = 0.5f * (lo[0] + hi[0]); rt->ssph[k][1] = 0.5f * (lo[1] + hi[1]);
            rt->ssph[k][2] = 0.5f * (lo[2] + hi[2]);
            rt->ssph[k][3] = sqrtf(dx * dx + dy * dy + dz * dz);
        }
    }
    rt->scn = sc;
    rt->nscn = sc->nrec;
    rt->bytes += sc->blob_bytes + (size_t)sc->nrec * (sizeof(TD5_MeshHeader) + sizeof(void *) + 4u);
    rr_scenery_normals(rt);
    return 1;
}

/* Bake `t` with the ground from `hc` (NULL = no ground). Returns the tile or NULL
 * (nothing to draw / out of memory). [F3] Thread-safe for a caller-owned `hc`: it
 * reads only the session's fixed configuration (frame, grid, pages, knobs), never
 * the tile table, so the stream worker bakes while the main thread draws. */
static RRTile *rr_bake(const RoamTile *t, TD5_RoamHeight *hc)
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
    rr_ground_build(t, &gr, hc);
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
                   0.5f + (p[kk][0] - p[0][0]) / RR_PATCH_UV_UNITS,
                   0.5f + (p[kk][2] - p[0][2]) / RR_PATCH_UV_UNITS);
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
        int32_t ox = 0, oy = 0, oz = 0;
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

/* [F4a] road + ground bake, then the SCENERY layer when a source is given (the
 * caller's: one per baking thread). The tile sphere grows to hold both. */
static RRTile *rr_bake_full(const RoamTile *t, TD5_RoamHeight *hc, TD5_RoamScnSource *src)
{
    RRTile *rt = rr_bake(t, hc);
    TD5_RoamScenery *sc;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    int k, a, any = 0;
    if (!src || s_rr.f.route || !td5_roam_scn_enabled()) return rt;
    sc = td5_roam_scn_build(src, t, hc);
    if (!sc) return rt;
    if (!rt) {
        rt = (RRTile *)calloc(1, sizeof *rt);
        if (!rt) { td5_roam_scn_free(sc); return NULL; }
        rt->tx = t->tx; rt->tz = t->tz;
        rt->dl.meshes = rt->vis;
    }
    if (!rr_attach_scenery(rt, sc)) return rt;
    TD5_LOG_I(LOG_TAG, "ROAM_SCENERY tile %d,%d meshes=%d bytes=%.2f MB build_ms=%u (src %u emit %u) buildings=%d/%d "
              "set_pieces=%d aprons=%d dropped_road=%d footpath_pieces=%d lawn_tris=%d trees=%d guard=%d hash=%08x",
              t->tx, t->tz, sc->st.meshes, (double)sc->st.bytes / 1048576.0, (unsigned)sc->st.ms, (unsigned)sc->st.ms_src,
              (unsigned)sc->st.ms_emit, sc->st.bld_emitted, sc->st.bld_seen, sc->st.bld_prefab, sc->st.bld_apron,
              sc->st.bld_drop_road, sc->st.fp_pieces - sc->st.fp_cut_road - sc->st.fp_merged, sc->st.lawn_tris,
              sc->st.trees, sc->st.guard_overlaps, sc->st.hash);
    for (k = 0; k < rt->nmesh; k++)
        for (a = 0; a < 3; a++) {
            if (rt->sph[k][a] - rt->sph[k][3] < lo[a]) lo[a] = rt->sph[k][a] - rt->sph[k][3];
            if (rt->sph[k][a] + rt->sph[k][3] > hi[a]) hi[a] = rt->sph[k][a] + rt->sph[k][3];
            any = 1;
        }
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) {
        if (!rt->sdl[k].count) continue;
        for (a = 0; a < 3; a++) {
            if (rt->ssph[k][a] - rt->ssph[k][3] < lo[a]) lo[a] = rt->ssph[k][a] - rt->ssph[k][3];
            if (rt->ssph[k][a] + rt->ssph[k][3] > hi[a]) hi[a] = rt->ssph[k][a] + rt->ssph[k][3];
            any = 1;
        }
    }
    if (any) {
        const float dx = 0.5f * (hi[0] - lo[0]), dy = 0.5f * (hi[1] - lo[1]), dz = 0.5f * (hi[2] - lo[2]);
        rt->cx = 0.5f * (lo[0] + hi[0]); rt->cy = 0.5f * (lo[1] + hi[1]); rt->cz = 0.5f * (lo[2] + hi[2]);
        rt->r = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    return rt;
}

/* ------------------------------------------------------------- tile table ---- */

static int rr_find(int tx, int tz)
{
    int i;
    for (i = 0; i < s_rr.ntile; i++)
        if (s_rr.tile[i]->tx == tx && s_rr.tile[i]->tz == tz) return i;
    return -1;
}

/* [F3] A dropped tile is RETIRED, not freed: the frame that is recording may still
 * read its CPU vertices (design 3.5). td5_roam_render_retire_tick frees it after
 * RR_RETIRE_TICKS more main-thread ticks. */
static void rr_retire(RRTile *rt)
{
    if (!rt) return;
    if (s_rr.nretired == s_rr.capretired) {
        const int nc = s_rr.capretired ? s_rr.capretired * 2 : 32;
        RRRetired *n = (RRRetired *)realloc(s_rr.retired, (size_t)nc * sizeof *n);
        if (!n) { rr_free_tile(rt); return; }          /* no list: free now (the F2 behaviour) */
        s_rr.retired = n; s_rr.capretired = nc;
    }
    s_rr.retired[s_rr.nretired].t = rt;
    s_rr.retired[s_rr.nretired].tick = s_rr.retire_clock;
    s_rr.nretired++;
}

void td5_roam_render_retire_tick(void)
{
    int i, n = 0;
    s_rr.retire_clock++;
    for (i = 0; i < s_rr.nretired; i++) {
        if (s_rr.retire_clock - s_rr.retired[i].tick > RR_RETIRE_TICKS) rr_free_tile(s_rr.retired[i].t);
        else s_rr.retired[n++] = s_rr.retired[i];
    }
    s_rr.nretired = n;
}

void td5_roam_render_remove_tile(int tx, int tz)
{
    const int i = rr_find(tx, tz);
    if (i < 0) return;
    rr_retire(s_rr.tile[i]);
    s_rr.tile[i] = s_rr.tile[--s_rr.ntile];
}

static int rr_insert(RRTile *rt)
{
    td5_roam_render_remove_tile(rt->tx, rt->tz);
    if (s_rr.ntile == s_rr.captile) {
        const int nc = s_rr.captile ? s_rr.captile * 2 : 64;
        RRTile **nt = (RRTile **)realloc(s_rr.tile, (size_t)nc * sizeof *nt);
        if (!nt) return 0;
        s_rr.tile = nt; s_rr.captile = nc;
    }
    s_rr.tile[s_rr.ntile++] = rt;
    return 1;
}

int td5_roam_render_add_tile(const RoamTile *t)
{
    RRTile *rt;
    uint32_t t0;
    if (!s_rr.began || !t) return 0;
    t0 = td5_plat_time_ms();
    /* [F4a] the F2 path bakes on the main thread with the session's own scenery source */
    if (!s_rr.scn && !s_rr.scn_failed && !s_rr.f.route && td5_roam_scn_enabled()) {
        s_rr.scn = td5_roam_scn_open(s_rr.place, &s_rr.grid, NULL, NULL);
        if (!s_rr.scn) s_rr.scn_failed = 1;
    }
    rt = rr_bake_full(t, s_rr.hc_ok ? s_rr.hc : NULL, s_rr.scn);
    if (!rt) { td5_roam_render_remove_tile(t->tx, t->tz); return 0; }
    if (!rr_insert(rt)) { rr_free_tile(rt); return 0; }
    TD5_LOG_D(LOG_TAG, "ROAM_RENDER_TILE %d,%d meshes=%d road_quads=%u patch_tris=%u ground_quads=%u skirts=%u "
              "bytes=%.2f MB bake_ms=%u", rt->tx, rt->tz, rt->nmesh, rt->nroad, rt->ntri, rt->nground,
              rt->nskirt, (double)rt->bytes / 1048576.0, (unsigned)(td5_plat_time_ms() - t0));
    return 1;
}

/* ---- [F3] worker bake + main-thread install (td5_roam_stream.c) ---- */

TD5_RoamBaked *td5_roam_render_bake(const RoamTile *t, TD5_RoamHeight *hc)
{
    if (!s_rr.began || !t) return NULL;
    return (TD5_RoamBaked *)rr_bake(t, hc);
}

TD5_RoamBaked *td5_roam_render_bake_ex(const RoamTile *t, TD5_RoamHeight *hc, TD5_RoamScnSource *scn)
{
    if (!s_rr.began || !t) return NULL;
    return (TD5_RoamBaked *)rr_bake_full(t, hc, scn);
}

const RoamGrid *td5_roam_render_grid(void) { return s_rr.began ? &s_rr.grid : NULL; }
const char *td5_roam_render_place(void) { return s_rr.began ? s_rr.place : NULL; }

int td5_roam_render_install(TD5_RoamBaked *b)
{
    RRTile *rt = (RRTile *)b;
    if (!rt) return 0;
    if (!s_rr.began || !rr_insert(rt)) { rr_free_tile(rt); return 0; }
    return 1;
}

void   td5_roam_render_free_baked(TD5_RoamBaked *b) { rr_free_tile((RRTile *)b); }
size_t td5_roam_render_baked_bytes(const TD5_RoamBaked *b) { return b ? ((const RRTile *)b)->bytes : 0u; }
int    td5_roam_render_has_tile(int tx, int tz) { return rr_find(tx, tz) >= 0; }

int td5_roam_render_open_height(TD5_RoamHeight *hc)
{
    char path[512], wpath[512];
    int ex100;
    if (!s_rr.began || !hc || !s_rr.use_ground) return 0;
    memset(hc, 0, sizeof *hc);
    ex100 = td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400);
    td5_geo_source_path(path, sizeof path, s_rr.place, "HEIGHT.R16");
    td5_geo_source_path(wpath, sizeof wpath, s_rr.place, "WATER.R8");
    return td5_roam_height_open(hc, path, wpath, (double)ex100 / 100.0);
}

void td5_roam_render_set_streamed(int on)
{
    s_rr.streamed = on ? 1 : 0;
    if (on) s_rr.dev_mode = 0;
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

    {   /* the host level's own road / lawn / kerb pages (the knobs override) */
        int hr = RR_PAGE_ROAD, hg = RR_PAGE_GROUND, hk = RR_PAGE_KERB;
#ifdef RR_HAVE_MODE
        if (td5_roam_active()) {
            td5_roam_host_pages(&hr, &hg, &hk);
            if (hk < 0) hk = RR_PAGE_KERB;
        }
#endif
        s_rr.page_road   = td5_env_int("TD5RE_ROAM_RENDER_PAGE_ROAD",   hr, 0, 1023);
        s_rr.page_ground = td5_env_int("TD5RE_ROAM_RENDER_PAGE_GROUND", hg, 0, 1023);
        s_rr.page_kerb   = td5_env_int("TD5RE_ROAM_RENDER_PAGE_KERB",   hk, 0, 1023);
    }
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
    for (i = 0; i < s_rr.nretired; i++) rr_free_tile(s_rr.retired[i].t);
    free(s_rr.retired);
    if (s_rr.hc) { td5_roam_height_close(s_rr.hc); free(s_rr.hc); }
    td5_roam_scn_close(s_rr.scn);
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
        o->meshes += t->nmesh + (int)t->nscn;
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
#ifdef RR_HAVE_MODE
    /* [F2-B] the physics resident set already holds every casco tile in memory: bake from
     * it instead of re-reading the file (the near-eye streaming policy stays here) */
    {
        const TD5_RoamResident *rs = td5_roam_resident();
        const int slot = rs ? td5_roam_resident_find(rs, tx, tz) : -1;
        if (slot >= 0 && rs->tile[slot]) {
            ok = td5_roam_render_add_tile(rs->tile[slot]);
            if (!ok) rr_mark_tried(tx, tz);
            return ok;
        }
    }
#endif
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

/* The roam fog, applied on the first draw (after the host level's LEVELINF set its own):
 * the host level's own fog is black (Moscow LEVELINF), which draws the far edge of the
 * ground (the renderer's fixed 195000-unit far plane, ~450 m) as a black seam under the
 * sky. A roam world is lit by the sky above it: fade the far ground to its horizon. */
static void rr_first_draw_fog(void)
{
    if (s_rr.fog_done) return;
    s_rr.fog_done = 1;
    if (rr_roam_active()) {
        const int rgb = td5_env_int("TD5RE_ROAM_RENDER_FOG_RGB", 0x5A5E66, 0, 0xFFFFFF);
        const int on  = td5_env_int("TD5RE_ROAM_RENDER_FOG", 1, 0, 1);
        if (on) td5_render_configure_fog((uint32_t)rgb, 1);
    }
}

int td5_roam_render_ensure_begun(void)
{
    if (!s_rr.began) rr_autobegin();
    return s_rr.began;
}

#ifndef TD5RE_RELEASE
/* DEV pose generator. TD5RE_ROAM_RENDER_POSES=<file> with lines
 *   name:lat,lon,h_above_ground_m,yaw_deg,pitch_deg      (yaw 0 = north, as seen on the map)
 * writes <file>.tour.txt in the free-cam tour format (STRIP units, the numbers the POS line prints;
 * the tour multiplies by 256 itself, y from the place height minus the strip datum) and returns 1 (the caller
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
    int cnt = 0, rf = 0;
    int32_t rf_ox = 0, rf_oz = 0;
    if (!in || !in[0]) return 0;
    if (!slug || !slug[0]) slug = "la_plata_partido";
    td5_geo_source_path(path, sizeof path, slug, "PLACE.JSON");
    {   /* place projection: lat0/lon0/m_per_deg from PLACE.JSON, scanned as text */
        char *js; TD5_File *pf = td5_plat_file_open(path, "rb"); int64_t pn;
        const char *k;
        if (!pf) { fprintf(stderr, "roam poses: no %s\n", path); return 1; }
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
        if (mdlat <= 0.0 || mdlon <= 0.0) { fprintf(stderr, "roam poses: no projection\n"); return 1; }
    }
    f = td5_plat_file_open(in, "rb");
    if (!f) { fprintf(stderr, "roam poses: cannot open %s\n", in); return 1; }
    n = td5_plat_file_size(f);
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf || td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) { td5_plat_file_close(f); free(buf); return 1; }
    buf[n] = '\0'; td5_plat_file_close(f);
    hc = (TD5_RoamHeight *)calloc(1, sizeof *hc);
    td5_geo_source_path(hp, sizeof hp, slug, "HEIGHT.R16");
    td5_geo_source_path(wpath, sizeof wpath, slug, "WATER.R8");
    if (!hc || !td5_roam_height_open(hc, hp, wpath,
            (double)td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400) / 100.0)) {
        fprintf(stderr, "roam poses: no height source\n"); free(buf); free(hc); return 1;
    }
    datum = td5_roam_view_datum_units(slug);
    {   /* roam frame: TD5RE_ROAM_RENDER_POSES_SPAWN=lat,lon -> origin = the spawn tile's lattice corner */
        const char *sp = getenv("TD5RE_ROAM_RENDER_POSES_SPAWN");
        double slat, slon;
        if (sp && sscanf(sp, "%lf,%lf", &slat, &slon) == 2) {
            char gp[512];
            TD5_File *gf;
            RoamGrid grd;
            int stx, stz, okg = 0;
            double spx = (slon - lon0) * mdlon * 430.0, spz = (slat - lat0) * mdlat * 430.0;
            td5_geo_source_path(gp, sizeof gp, slug, "roam/ROAMGRID.JSON");
            gf = td5_plat_file_open(gp, "rb");
            if (gf) {
                const int64_t gn = td5_plat_file_size(gf);
                char *gb = (gn > 0 && gn < 65536) ? (char *)malloc((size_t)gn + 1u) : NULL;
                if (gb && td5_plat_file_read(gf, gb, (size_t)gn) == (size_t)gn) { gb[gn] = 0; okg = roam_grid_parse(gb, &grd); }
                free(gb); td5_plat_file_close(gf);
            }
            if (!okg) { fprintf(stderr, "roam poses: no ROAMGRID\n"); td5_roam_height_close(hc); free(hc); free(buf); return 1; }
            roam_grid_tile_of(&grd, spx, spz, &stx, &stz);
            roam_grid_origin(&grd, stx, stz, &rf_ox, &rf_oz);
            rf = 1;
            fprintf(stderr, "roam poses: roam frame, spawn tile %d,%d origin %d,%d\n", stx, stz, (int)rf_ox, (int)rf_oz);
        }
    }
    snprintf(outp, sizeof outp, "%s.tour.txt", in);
    o = fopen(outp, "wb");
    for (line = strtok_s(buf, "\r\n;", &save); line && o; line = strtok_s(NULL, "\r\n;", &save)) {
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
            !td5_roam_height_sample(hc, px, pz, &g, NULL)) { fprintf(stderr, "roam poses: no height at %s\n", nm); continue; }
        if (rf) {   /* roam frame: world = place - origin, y = place height, yaw 0 = +Z (north) */
            fprintf(o, "%s:%.0f,%.0f,%.0f,%.3f,%.3f\n", nm, (px - (double)rf_ox), (g + hm * 430.0),
                    (pz - (double)rf_oz), yaw, pit);
            cnt++;
            continue;
        }
        if (!td5_roam_view_frame(slug, px, pz, &xr, &zr) || !td5_roam_view_frame(slug, px, pz + 430.0, &xn, &zn)) continue;
        yr = yaw * 3.14159265358979 / 180.0 + atan2(xn - xr, zn - zr);   /* map north -> route +Z-relative */
        fprintf(o, "%s:%.0f,%.0f,%.0f,%.3f,%.3f\n", nm, xr, (g + hm * 430.0 - datum), zr,
                yr * 180.0 / 3.14159265358979, pit);
        cnt++;
    }
    if (o) fclose(o);
    td5_roam_height_close(hc); free(hc); free(buf);
    fprintf(stderr, "roam poses: %d pose(s) -> %s (datum %.0f units)\n", cnt, outp, datum);
    return 1;
}
#endif

/* ------------------------------------------------------------------- draw ---- */

/* one line when a single display list took > 30 ms to submit (a hitch hunt) */
static void rr_note_slow(const RRTile *t, int sb, int n, uint64_t us)
{
    if (us < 30000u) return;
    TD5_LOG_W(LOG_TAG, "ROAM_RENDER_SLOW tile %d,%d %s %d meshes: %.1f ms", t->tx, t->tz,
              sb < 0 ? "road" : "scenery sub-block", n, (double)us / 1000.0);
    (void)sb;
}

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
    rr_first_draw_fog();
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
        if (n > 0) {
            const uint64_t q0 = td5_plat_time_us();
            t->dl.count = (uint32_t)n;
            t->dl.meshes = t->vis;
            td5_render_span_display_list(&t->dl);
            meshes += n;
            rr_note_slow(t, -1, n, td5_plat_time_us() - q0);
        }
        /* [F4a] the scenery layer, one display list per sub-block (the renderer culls each mesh) */
        if (t->nscn) {
            for (k = 0; k < TD5_ROAM_SCN_NSB; k++) {
                const float *s = t->ssph[k];
                if (!t->sdl[k].count) continue;
                dx = s[0] - ex; dy = s[1] - ey; dz = s[2] - ez;
                if (sqrtf(dx * dx + dy * dy + dz * dz) - s[3] > R) continue;
                if (!td5_render_is_sphere_visible(s[0], s[1], s[2], s[3])) continue;
                {
                    const uint64_t q0 = td5_plat_time_us();
                    td5_render_span_display_list(&t->sdl[k]);
                    rr_note_slow(t, k, (int)t->sdl[k].count, td5_plat_time_us() - q0);
                }
                meshes += (int)t->sdl[k].count;
                n++;
            }
        }
        if (n > 0) tiles++;
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
