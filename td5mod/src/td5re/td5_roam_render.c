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
 *                   ([roam LC] one road command per surface class: smooth, cobble, loose page)
 *   cmd 1  ground   a heightfield cell per ROAM_RENDER_CELL_M, the ground page
 *   cmd 2  skirts   a grey vertical face down every BEDG (kerb, deck side)
 *   cmd 3  water    [roam LE] a flat-ish water quad per ground cell that touches a wet lattice point
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
 * [G4] The road is NOT the raster: grade smoothing, end blends and cuts put it up to
 * 4.7 m under it (G4 census: 5830 of the 5832 cells within 20 cm of the road), so a
 * 16 m cell crossed above the road surface and grass flickered through it in stripes.
 * Every lattice point is now held at or under the lowest road height in its four
 * cells (each drivable primitive clipped to each cell, bore spans left out), so the
 * drop is a true minimum gap. The 3x3 ROAD tiles feed it and the clip runs in GLOBAL
 * lattice coordinates, so both tiles of a seam compute the same bits.
 * TD5RE_ROAM_GROUND_NOFIX=1 turns the clamp off (before / after checks).
 * [G5] Beside a steep road the 4-cell minimum sat metres under the road edge, so each point now
 * comes down only as far as the road triangles next to it need (td5_roam_gfix_lower: exact, and the
 * same lowering on both sides of a seam); TD5RE_ROAM_GROUND_G4=1 = the G4 cell clamp.
 *
 * WATER. [roam LE] The ground at a wet lattice point sits TD5_ROAM_WATER_DEPTH (3.3 m) under the raster: until LE
 * that was a dry hole (the Bosque lake, the quarry lakes, the arroyos). Each ground cell with a wet corner now gets
 * a quad on the host's water page at the SURFACE LEVEL of its corners: the lowest of the 3 x 3 lattice points round
 * each corner (the raster's surface where wet, the DRAWN ground where dry: raster - G5 lowering - the drop) minus
 * RR_WATER_BELOW. So a shore corner is always under its own bank and the next dry points (the ground occludes the
 * plane there, a clean waterline), the surface follows a river's fall, and it is a function of global lattice
 * points only (both tiles of a seam compute the same level). A cell the road crosses keeps its water only when the
 * lowest road height in the cell (td5_roam_gfix_cells, the 3x3 ROAD tiles) stands RR_WATER_ROAD_GAP over the
 * plane: a deck over a stream keeps its clearance (round I IE lifts it 2.5 m over the surface), a road at the
 * water's level drops the cell. Cells under a bore are left out with the ground. TD5RE_ROAM_WATER=0 = the old dry
 * holes; TD5RE_ROAM_WATER_BELOW_CM moves the level; census: TD5RE_ROAM_WATER_CENSUS=<tile list>.
 *
 * SHORES. [roam PC] The wet test above is one raster sample per 16 m lattice point, so the drawn waterline (where the
 * cell's ground slope meets the plane) ran in 16 m steps across the rotated lattice. The WATER raster is 3.49 m, so a
 * cell whose 2 m sub-samples (RR_SHORE_N x RR_SHORE_N, the raster's bilinear wetness, td5_roam_height_wetfrac) are
 * MIXED is a SHORE cell: its ground is drawn as RR_SHORE_N^2 sub-quads instead of one, an open-water sub-sample sitting
 * in the pit (at least RR_SHORE_BED under the plane), a dry one on the ground as if no water were near (at least
 * RR_SHORE_BANK over the plane), and one within a raster cell of the edge graded by its wetness between the two, so the
 * plane meets the ground on the raster's 0.5 contour (piecewise linear on the 2 m sub-grid). That band is the BANK
 * strip: the ground page tinted to mud at every sub-vertex within a raster cell of the edge (graded between vertices).
 * A cell with no wet lattice corner but wet sub-samples gets its water quad too (same level and road rules). Points on a cell edge shared with a cell that is not drawn as a shore keep
 * that cell's straight edge, and the four cell corners always keep the lattice value, so no crack opens at a seam.
 * TD5RE_ROAM_SHORE=0 = round L's water. Census: TD5RE_ROAM_SHORE_CENSUS=1 with the water census (crossings of the
 * real 0.5 contour on a 0.5 m grid, and those where the drawn edge is more than 2 m off it).
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
#include "td5_geo_frame.h"      /* [roam M] the geographic -> game reflection */
#include "td5_render.h"
#include "td5_camera.h"
#include "td5_page_map.h"
#include "td5_roam_tile.h"
#include "td5_roam_height.h"
#include "td5_roam_groundfix.h"   /* [G4b] the clamp, shared with the scenery */
#include "td5_roam_cut.h"         /* [roam NC] cuttings: hole cells, the refill */
#include "td5_roam_ribbon.h"
#include "td5_roam_junction.h"   /* [roam HC] TD5_ROAM_CL_TAPER / _BEND: which clusters get stop lines */
#include "td5_roam_view.h"
#include "td5_roam_render.h"
#include "td5_roam_scenery.h"   /* [F4a] the tile's SCENERY layer */
#include "td5_roam_shops.h"     /* [roam PE] td5_roam_shops_session_begin */
#ifndef TD5RE_RELEASE
#include "td5_roam_pavecensus.h"   /* [G4b] pavement census */
#endif
#include "td5_light2.h"         /* [F4a] td5_light2_active: derived normals */

#if defined(__has_include)
#  if __has_include("td5_roam_mode.h")
#    include "td5_roam_mode.h"
#    include "td5_trackgen.h"      /* [roam LC] td5_trackgen_roam_surface_pages */
#    include "td5_roam_locate.h"   /* [F2-B] TD5_RoamResident: tiles come from the physics resident set */
#    define RR_HAVE_MODE 1
#  endif
#endif

#define LOG_TAG "roam"

#define RR_SUB            4                  /* sub-blocks per tile side (64 m)     */
#define RR_MAXMESH        (RR_SUB * RR_SUB)
#define RR_UPM            430.0              /* units per metre                     */
#define RR_MAX_LANES      16
#define RR_NCLS           3                  /* [roam LC] road surface classes: smooth, cobble, loose */
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
 * blotches; 24000 (56 m, +-22 m inside the stripe-free middle) keeps a patch uniform.
 * [roam HC] That was 16x the ribbon's texel (87 cm against 5.5 cm: the blur at every junction), and a
 * triangle wider than 12000 units still reached the lane-line column (x <= 1) of the page, which drew a
 * 1.7 m blurred white bar across the box. RR_PATCH_UV_UNITS is now the OLD mapping (TD5RE_ROAM_PATCH_UV=0).
 * The new one cuts every patch triangle into slabs of RR_PATCH_TEX units along the lattice u axis (world
 * anchored, the same in every tile) and maps each slab to columns RR_PATCH_U0..U1 of the page, which hold
 * no stripe even with bilinear filtering; v runs on along the lattice v axis and wraps (the stripe is a
 * column, every row is clean). Texel: 1500 / 60 = 25 units (5.8 cm), the ribbon's 23.4. */
#define RR_PATCH_UV_UNITS 24000.0f
#define RR_PATCH_TEX      1500.0
#define RR_PATCH_U0       (3.0 / 64.0)
#define RR_PATCH_U1       (63.0 / 64.0)
/* [roam HC] Stop lines: a white bar across the inbound lanes of every junction mouth that needs one, set
 * back from the patch's cut row into the ribbon's last quad. Drawn on the road page's own lane-line column
 * (texels x 0..1, rows 0..15 are one solid dash), so it is part of the road command: no new page. */
#define RR_STOP_S0        (0.30 * RR_UPM)   /* bar starts this far back from the cut row */
#define RR_STOP_S1        (0.80 * RR_UPM)   /* and ends here (a 50 cm bar)               */
#define RR_STOP_LIFT      13.0              /* 3 cm over the ribbon: no z-fight          */
#define RR_STOP_U0        (0.5 / 64.0)
#define RR_STOP_U1        (1.5 / 64.0)
#define RR_STOP_V0        (1.5 / 64.0)
#define RR_STOP_V1        (14.5 / 64.0)

typedef struct {
    int             tx, tz;
    int             nmesh;
    TD5_MeshHeader *mesh[RR_MAXMESH];
    float           sph[RR_MAXMESH][4];      /* x y z radius, render frame          */
    TD5_MeshHeader *vis[RR_MAXMESH];         /* this frame's list (stable storage)  */
    TD5_SpanDisplayList dl;
    float           cx, cy, cz, r;           /* whole-tile sphere                   */
    uint32_t        nroad, ntri, nground, nskirt, nfill;   /* nfill: [roam NC] refill sub-cells */
    uint32_t        ncls[RR_NCLS];           /* [roam LC] road quads + patch tris by surface class (smooth, cobble, loose) */
    uint32_t        npiece, nstop;           /* [roam HC] patch slab triangles, stop-line bars */
    uint32_t        nwater, nwet, nwater_road, nwater_deck;   /* [roam LE] water quads, wet lattice points, cells
                                                               * dropped for a road at the water, cells under a deck */
    double          water_clear;             /* [roam LE] lowest road - water over a kept cell, units (1e300 none) */
    uint32_t        nshore, nshore_ext, nbank;   /* [roam PC] shore cells, water cells added by sub-samples, bank sub-vertices */
    uint32_t        sh_cross, sh_step, sh_edge;  /* [roam PC] census: real-shore crossings, > 2 m off, too near the tile edge */
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
    float           *lamp;                   /* [F4c] lantern positions, RENDER frame (x, y, z), nlamp of them */
    uint32_t        nlamp;
    int32_t          sdx, sdz;               /* [F9b] world shift already applied (bake frame -> world) */
    uint8_t         *aud;                    /* [roam IA] DEV cull audit: last frame's verdict per mesh  */
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
    int       surf_pages, page_cls[3];    /* [roam LC] per-surface road pages (0 = the old single page) */
    int       use_ground;
    double    lift;                          /* units, added to every y             */
    double    ground_drop;                   /* units                               */
    int       ground_clamp;                  /* [G4] hold the ground under the road */
    int       keep_raw;                      /* [G5] census: keep the unclamped lattice (RRGround.h0) */
    int       patch_uv;                      /* [roam HC] 1 = slab mapping at the ribbon's texel, 0 = old */
    int       stoplines;                     /* [roam HC] 1 = stop lines at junction mouths            */
    int       kerb_line;                     /* [roam HC] 1 = the old dashed line down one kerb        */
    int       water;                         /* [roam LE] 1 = water quads over wet cells (needs page_water) */
    int       page_water;
    double    water_below;                   /* [roam LE] units under the lowest bank / surface point  */
    int       shore, shore_census;           /* [roam PC] TD5RE_ROAM_SHORE / TD5RE_ROAM_SHORE_CENSUS               */
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
    /* [F9b] world-origin rebase: tiles are BAKED in the session's first frame (f.ox/oz, fixed, so a
     * worker bake never races a rebase) and drawn shifted by (draw_dx, draw_dz) world units */
    int32_t   draw_dx, draw_dz;
    long      rebases;
    /* stats */
    uint32_t  st_frames;
    double    st_sum_ms, st_max_ms, st_last_ms;
    int       st_drawn_tiles, st_drawn_meshes;
    /* [roam RA] per pane (split screen): draws of the last frame, running sums for the averages */
    int       v_tiles[TD5_ROAM_RENDER_VIEWS], v_meshes[TD5_ROAM_RENDER_VIEWS];
    uint32_t  v_frames[TD5_ROAM_RENDER_VIEWS];
    double    v_sum_meshes[TD5_ROAM_RENDER_VIEWS], v_sum_ms[TD5_ROAM_RENDER_VIEWS];
    uint32_t  st_drawn_quads;
    /* [roam IA] DEV cull audit (TD5RE_ROAM_CULL_AUDIT=1): a mesh the draw loop rejected while one
     * of its vertices is in view within the draw radius is a FALSE cull; one that was drawn last
     * frame is a POP. Counted per stage x class (0 road, 1 pavement page, 2 other scenery). */
    int       audit;
    uint64_t  au_frames, au_tested[3], au_drawn[3], au_false[8][3], au_pop[8][3], au_far[3], au_proj;
    int       au_logged;
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

/* [roam LC] road surface classes: one road command each (smooth = the host's road page, cobble, loose) */
typedef struct {
    int      nrt, nrq, ngq, nsq, nwq;        /* counts: road tris/quads (all classes), ground, skirt, [roam LE] water */
    int      cnt_t[RR_NCLS], cnt_q[RR_NCLS]; /* [roam LC] road tris / quads per class                */
    int      cur_t[RR_NCLS], cur_q[RR_NCLS]; /* [roam LC] their write cursors                        */
    TD5_MeshHeader *m;
    TD5_MeshVertex *v;
    int      ct, cq, cg, cs, cw;             /* write cursors (in primitives)         */
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
typedef struct { int G; double *h, *h0; unsigned char *hole; int nclamp; double max_low;
                 uint64_t *fill; int nfill; } RRGround;   /* h0: [G5] census only; fill: [roam NC] sub-cell masks */

/* [G4] the 3x3 ROAD tiles round a bake, [4] = the tile itself; own[] = read here, freed by rr_nb_free */
#define RR_NB 9
typedef struct { const RoamTile *t[RR_NB]; RoamTile *own[RR_NB]; } RRNb;

static void rr_ground_point(const RoamTile *t, int i, int j, int G, double *px, double *pz)
{
    const double S = (double)s_rr.grid.tile;
    const double u = ((double)t->tx + (double)i / (double)G) * S;
    const double v = ((double)t->tz + (double)j / (double)G) * S;
    *px = s_rr.grid.ox + s_rr.grid.c * u - s_rr.grid.s * v;
    *pz = s_rr.grid.oz + s_rr.grid.s * u + s_rr.grid.c * v;
}

/* [G4] hold every lattice point of g at or under the lowest road height of its four cells.
 * The cells reach one ring past the tile, fed by the 3x3 ROAD tiles: a border point sees the
 * same primitives and the same arithmetic from both tiles, so the seam stays exact. */
static void rr_ground_clamp(const RoamTile *t, RRGround *g, const RRNb *nb, TD5_RoamHeight *hc)
{
    const int G = g->G, N = G + 2, W = G + 1, I0 = t->tx * G - 1, J0 = t->tz * G - 1;
    const double cs = (double)s_rr.grid.tile / (double)G;
    double *cm = (double *)malloc((size_t)N * (size_t)N * sizeof(double));
    int i, j;
    if (!cm) return;
    if (!td5_roam_gfix_g4()) {                  /* [G5] each point down as far as the road next to it needs */
        td5_roam_gfix_lower(&s_rr.grid, nb->t, hc, t->tx, t->tz, G, 0, W, cm);
        for (i = 0; i < W * W; i++)
            if (cm[i] > 0.0 && !isnan(g->h[i])) {
                if (cm[i] > g->max_low) g->max_low = cm[i];
                g->h[i] -= cm[i];
                g->nclamp++;
            }
        free(cm);
        return;
    }
    td5_roam_gfix_cells(&s_rr.grid, nb->t, I0, J0, N, cs, cm);
    for (j = 0; j <= G; j++) for (i = 0; i <= G; i++) {
        /* point (i, j) = global (I0 + 1 + i, J0 + 1 + j); its cells are local (i..i+1, j..j+1) */
        double cap = cm[j * N + i], *h = &g->h[j * W + i];
        if (cm[j * N + i + 1] < cap) cap = cm[j * N + i + 1];
        if (cm[(j + 1) * N + i] < cap) cap = cm[(j + 1) * N + i];
        if (cm[(j + 1) * N + i + 1] < cap) cap = cm[(j + 1) * N + i + 1];
        if (cap < *h) {
            if (*h - cap > g->max_low) g->max_low = *h - cap;
            *h = cap;
            g->nclamp++;
        }
    }
    free(cm);
}

/* [roam NC] the place-frame position of lattice point (i + fi, j + fj) of tile t (fractional, G cells per side) */
static void rr_ground_point_f(const RoamTile *t, double fi, double fj, int G, double *px, double *pz)
{
    const double S = (double)s_rr.grid.tile;
    const double u = ((double)t->tx + fi / (double)G) * S;
    const double v = ((double)t->tz + fj / (double)G) * S;
    *px = s_rr.grid.ox + s_rr.grid.c * u - s_rr.grid.s * v;
    *pz = s_rr.grid.oz + s_rr.grid.s * u + s_rr.grid.c * v;
}

/* [roam NC] THE REFILL of the hole cells (td5_roam_cut.h): each hole cell keeps the TD5_ROAM_CUT_SUB^2 sub-cells
 * with no corner inside a cutting band, on the cell's own surface (rr_bake_fill). Bit (sj * SUB + si) of fill[cell]. */
static void rr_ground_fill(const RoamTile *t, RRGround *g, const RoamTile *const n9[9], TD5_RoamHeight *hc)
{
    const int G = g->G, N = TD5_ROAM_CUT_SUB, W = G + 1;
    TD5_RoamCutBands bands;
    double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300, x, z;
    int c, i, j, a, b, any = 0;
    for (i = 0; i < G * G; i++) any |= g->hole[i];
    if (!any) return;
    g->fill = (uint64_t *)calloc((size_t)G * (size_t)G, sizeof(uint64_t));
    if (!g->fill) return;
    for (c = 0; c < 4; c++) {
        rr_ground_point(t, (c & 1) ? G : 0, (c & 2) ? G : 0, G, &x, &z);
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (z < z0) z0 = z;
        if (z > z1) z1 = z;
    }
    if (!td5_roam_cut_bands(&bands, n9, hc, x0, z0, x1, z1)) { free(g->fill); g->fill = NULL; return; }
    for (b = 0; b < G; b++) for (a = 0; a < G; a++) {
        unsigned char in[(TD5_ROAM_CUT_SUB + 1) * (TD5_ROAM_CUT_SUB + 1)];
        uint64_t m = 0;
        if (!g->hole[b * G + a]) continue;
        if (isnan(g->h[b * W + a]) || isnan(g->h[b * W + a + 1]) || isnan(g->h[(b + 1) * W + a]) ||
            isnan(g->h[(b + 1) * W + a + 1])) continue;
        for (j = 0; j <= N; j++) for (i = 0; i <= N; i++) {
            rr_ground_point_f(t, (double)a + (double)i / N, (double)b + (double)j / N, G, &x, &z);
            in[j * (N + 1) + i] = (unsigned char)td5_roam_cut_inside(&bands, x, z);
        }
        for (j = 0; j < N; j++) for (i = 0; i < N; i++)
            if (!in[j * (N + 1) + i] && !in[j * (N + 1) + i + 1] && !in[(j + 1) * (N + 1) + i] && !in[(j + 1) * (N + 1) + i + 1]) {
                m |= (uint64_t)1 << (j * N + i);
                g->nfill++;
            }
        g->fill[b * G + a] = m;
    }
    td5_roam_cut_bands_free(&bands);
}

static int rr_ground_build(const RoamTile *t, RRGround *g, TD5_RoamHeight *hc, const RRNb *nb)
{
    const int G = (int)(256 / s_rr.cell_m);
    double minx = 1e18, minz = 1e18, maxx = -1e18, maxz = -1e18, x, z;
    int i, j, c, ok = 0;
    uint32_t e, q;
    const double S = (double)s_rr.grid.tile;
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
    /* [G5] the clamp reads the raster of the lattice ring one cell outside the tile (up to 1.41 cells out in x / z).
     * [roam LE] the water level reads one more ring (lattice -2..G+2: up to 2.83 cells out), so the window grows
     * when the water is on; a sample's value does not depend on the window (bilinear inside the rect + PAD). */
    {
        double m = 2000.0 + (s_rr.water ? 4.0 : 2.0) * S / G;
        if (td5_roam_cut_on() && m < 0.5 * S) m = 0.5 * S;    /* [roam NC] the 3x3's cutting spans read the raster */
        if (!td5_roam_height_prefetch(hc, minx - m, minz - m, maxx + m, maxz + m)) {
            free(g->h); free(g->hole); memset(g, 0, sizeof *g);
            return 0;
        }
    }
    for (j = 0; j <= G; j++) {
        for (i = 0; i <= G; i++) {
            double h;
            rr_ground_point(t, i, j, G, &x, &z);
            if (td5_roam_height_sample(hc, x, z, &h, NULL)) { g->h[j * (G + 1) + i] = h; ok++; }
            else g->h[j * (G + 1) + i] = NAN;
        }
    }
    if (s_rr.keep_raw && (g->h0 = (double *)malloc((size_t)(G + 1) * (size_t)(G + 1) * sizeof(double))) != NULL)
        memcpy(g->h0, g->h, (size_t)(G + 1) * (size_t)(G + 1) * sizeof(double));
    if (s_rr.ground_clamp && nb) rr_ground_clamp(t, g, nb, hc);
    /* cells under a tunnel bore ([roam NC] and round a cutting span of the 3x3) are left out: the road runs below
     * the ground there */
    {
        const RoamTile *n9[9];
        int k;
        for (k = 0; k < 9; k++) n9[k] = nb ? nb->t[k] : NULL;
        n9[4] = t;
        td5_roam_cut_holes(&s_rr.grid, n9, hc, t->tx, t->tz, G, g->hole);
        if (td5_roam_cut_on()) rr_ground_fill(t, g, n9, hc);
    }
    (void)e; (void)q;
    return ok > 0;
}

static void rr_ground_free(RRGround *g) { free(g->h); free(g->h0); free(g->hole); free(g->fill); memset(g, 0, sizeof *g); }

static int rr_ground_cell_ok(const RRGround *g, int i, int j)
{
    const int W = g->G + 1;
    if (!g->h || g->hole[j * g->G + i]) return 0;
    return !isnan(g->h[j * W + i]) && !isnan(g->h[j * W + i + 1]) &&
           !isnan(g->h[(j + 1) * W + i]) && !isnan(g->h[(j + 1) * W + i + 1]);
}

/* ------------------------------------------------------------ [roam LE] water ---- */

#define RR_WATER_BELOW_CM   25               /* default: the plane this far under the lowest bank point    */
#define RR_WATER_ROAD_GAP   (0.10 * RR_UPM)  /* a road over a water cell must stand this far over the plane */
#define RR_WATER_TILE       6000.0           /* world units per water page repeat (TD5_TG_WATER_TILE)      */

#define RR_SHORE_N      8                    /* [roam PC] sub-samples per cell side (2 m)              */
#define RR_SHORE_FW     (RR_SHORE_N + 1)
#define RR_SHORE_BANK   (0.30 * RR_UPM)      /* a dry sub-sample stands at least this over the plane     */
#define RR_SHORE_BED    (0.30 * RR_UPM)      /* a wet one at least this under it                         */

typedef struct {
    int            G;
    double        *L;              /* (G+1)^2 surface level per lattice point (NAN = none) */
    unsigned char *cell;           /* G*G: 1 = a water quad                               */
    int            ncell, nwet, nroad, ndeck;
    double         min_clear;      /* lowest road - plane over a kept cell, units           */
    /* [roam PC] shores */
    unsigned char *shr;            /* (G+2)^2 cells -1..G: 1 = mixed sub-sample wetness (a shore by the raster)   */
    double        *fh;             /* G*G*FW*FW: the sub-mesh heights of a drawn shore cell (drop included)       */
    unsigned char *fm;             /* G*G*FW*FW: the bank's mud weight per sub-vertex (255 = within a raster cell of the edge) */
    unsigned char *sc;             /* G*G: 1 = drawn as a shore sub-mesh                                          */
    int            nshore, next, nbank;
} RRWater;

static void rr_water_free(RRWater *w)
{
    free(w->L); free(w->cell); free(w->shr); free(w->fh); free(w->fm); free(w->sc);
    memset(w, 0, sizeof *w);
}

/* [roam PC] the raster's wetness at a fractional lattice point of tile t (0 where no window covers it) */
static double rr_wetfrac(const RoamTile *t, TD5_RoamHeight *hc, double fi, double fj, int G)
{
    double x, z, f = 0.0;
    rr_ground_point_f(t, fi, fj, G, &x, &z);
    if (!td5_roam_height_wetfrac(hc, x, z, &f)) f = 0.0;
    return f;
}

/* [roam PC] triangle interpolation over a cell's 4 corner values, the pipeline's own split (0 1 2 / 0 2 3) */
static double rr_tri(const double *h, double fu, double fv)
{
    return (fu >= fv) ? h[0] + fu * (h[1] - h[0]) + fv * (h[2] - h[1])
                      : h[0] + fv * (h[3] - h[0]) + fu * (h[2] - h[3]);
}

/* The water lattice of one tile (see the WATER paragraph of the header). Reads the lattice ring -1..G+1 of the
 * raster (prefetched by rr_ground_build) and the 3x3 ROAD tiles. 0 = no water quad in the tile. */
static int rr_water_build(const RoamTile *t, const RRGround *gr, TD5_RoamHeight *hc, const RRNb *nb, RRWater *w)
{
    const int G = gr->G, W = G + 1, R = G + 3;
    const double cs = (double)s_rr.grid.tile / (double)G;
    double *S = NULL, *low = NULL, *cm = NULL;
    unsigned char *wet = NULL;
    int i, j, k, any = 0;
    memset(w, 0, sizeof *w);
    w->G = G;
    w->min_clear = 1e300;
    if (!s_rr.water || !gr->h || !hc) return 0;
    S = (double *)malloc((size_t)R * (size_t)R * sizeof(double));
    wet = (unsigned char *)calloc((size_t)R * (size_t)R, 1);
    if (!S || !wet) goto fail;
    for (j = 0; j < R; j++) for (i = 0; i < R; i++) {
        double x, z, h;
        int wt = 0;
        td5_roam_gfix_point(&s_rr.grid, t->tx, t->tz, i - 1, j - 1, G, &x, &z);
        if (td5_roam_height_sample(hc, x, z, &h, &wt)) {
            S[j * R + i] = wt ? h + TD5_ROAM_WATER_DEPTH : h;      /* the raster's own surface where wet */
            wet[j * R + i] = (unsigned char)wt;
            any |= wt;
        } else S[j * R + i] = NAN;
    }
    if (!any) goto fail;
    /* dry points: the DRAWN ground (the G5 lowering of rr_ground_clamp, then the drop) */
    if (s_rr.ground_clamp && nb && !td5_roam_gfix_g4()) {
        low = (double *)malloc((size_t)R * (size_t)R * sizeof(double));
        if (!low) goto fail;
        td5_roam_gfix_lower(&s_rr.grid, nb->t, hc, t->tx, t->tz, G, -1, R, low);
    }
    for (k = 0; k < R * R; k++)
        if (!wet[k] && !isnan(S[k])) S[k] -= (low ? low[k] : 0.0) + s_rr.ground_drop;
    w->L = (double *)malloc((size_t)W * (size_t)W * sizeof(double));
    w->cell = (unsigned char *)calloc((size_t)G * (size_t)G, 1);
    if (!w->L || !w->cell) goto fail;
    for (j = 0; j <= G; j++) for (i = 0; i <= G; i++) {
        double m = 1e300;
        int ok = 1, di, dj;
        for (dj = 0; dj < 3; dj++) for (di = 0; di < 3; di++) {
            const double v = S[(j + dj) * R + i + di];
            if (isnan(v)) ok = 0;
            else if (v < m) m = v;
        }
        w->L[j * W + i] = ok ? m - s_rr.water_below : NAN;
        if (i < G && j < G && wet[(j + 1) * R + i + 1]) w->nwet++;        /* a seam point is its lower tile's */
    }
    if (nb) {
        cm = (double *)malloc((size_t)G * (size_t)G * sizeof(double));
        if (!cm) goto fail;
        td5_roam_gfix_cells(&s_rr.grid, nb->t, t->tx * G, t->tz * G, G, cs, cm);
    }
    if (s_rr.shore) {   /* [roam PC] which cells of -1..G have mixed sub-sample wetness (wet corners count as wet) */
        const int E = G + 2, N = RR_SHORE_N;
        w->shr = (unsigned char *)calloc((size_t)E * (size_t)E, 1);
        if (!w->shr) goto fail;
        for (j = -1; j <= G; j++) for (i = -1; i <= G; i++) {
            int a, b, anyw = 0, allw = 1;
            for (b = 0; b <= 1; b++) for (a = 0; a <= 1; a++) {
                const int wv = wet[(j + b + 1) * R + i + a + 1];
                anyw |= wv; allw &= wv;
            }
            for (b = 0; b <= N; b++) for (a = 0; a <= N; a++) {
                const int fw = rr_wetfrac(t, hc, (double)i + (double)a / N, (double)j + (double)b / N, G) >= 0.5;
                anyw |= fw; allw &= fw;
            }
            w->shr[(j + 1) * E + i + 1] = (unsigned char)(anyw && !allw);
        }
    }
    for (j = 0; j < G; j++) for (i = 0; i < G; i++) {
        static const int di[4] = { 0, 1, 1, 0 }, dj[4] = { 0, 0, 1, 1 };
        double mx = -1e300;
        int kk, wc = 0, ok = 1, ext = 0;
        if (gr->hole[j * G + i]) continue;
        for (kk = 0; kk < 4; kk++) {
            const double l = w->L[(j + dj[kk]) * W + i + di[kk]];
            wc |= wet[(j + dj[kk] + 1) * R + i + di[kk] + 1];
            if (isnan(l)) ok = 0;
            else if (l > mx) mx = l;
        }
        if (!wc && w->shr && w->shr[(j + 1) * (G + 2) + i + 1]) ext = 1;   /* [roam PC] wet sub-samples only */
        if ((!wc && !ext) || !ok) continue;
        if (cm && cm[j * G + i] < 1e299) {
            if (mx > cm[j * G + i] - RR_WATER_ROAD_GAP) { w->nroad++; continue; }
            w->ndeck++;
            if (cm[j * G + i] - mx < w->min_clear) w->min_clear = cm[j * G + i] - mx;
        }
        w->cell[j * G + i] = 1;
        w->ncell++;
        if (ext) w->next++;
    }
    if (w->shr && w->ncell > 0) {   /* [roam PC] the sub-mesh of every drawn shore cell */
        const int E = G + 2, N = RR_SHORE_N, FW = RR_SHORE_FW;
        w->sc = (unsigned char *)calloc((size_t)G * (size_t)G, 1);
        w->fh = (double *)malloc((size_t)G * (size_t)G * FW * FW * sizeof(double));
        w->fm = (unsigned char *)calloc((size_t)G * (size_t)G * FW * FW, 1);
        if (!w->sc || !w->fh || !w->fm) goto fail;
        for (j = 0; j < G; j++) for (i = 0; i < G; i++) {
            static const int di[4] = { 0, 1, 1, 0 }, dj[4] = { 0, 0, 1, 1 };
            double D[4], Lc[4], C[4];
            double *fh = &w->fh[(size_t)(j * G + i) * FW * FW];
            unsigned char *fm = &w->fm[(size_t)(j * G + i) * FW * FW];
            int kk, a, b, pin[4], mud = 0;
            if (!w->cell[j * G + i] || !w->shr[(j + 1) * E + i + 1]) continue;
            for (kk = 0; kk < 4; kk++) {
                const int li = (j + dj[kk]) * W + i + di[kk], ri = (j + dj[kk] + 1) * R + i + di[kk] + 1;
                /* the drawn ground as if dry: a wet point's raster surface minus its lowering and the drop */
                D[kk] = wet[ri] ? S[ri] - (low ? low[ri] : 0.0) - s_rr.ground_drop : S[ri];
                Lc[kk] = w->L[li];
                C[kk] = gr->h[li] - s_rr.ground_drop;
            }
            /* edges shared with a cell not drawn as a shore keep the coarse straight edge: 0 bottom, 1 right, 2 top, 3 left */
            {
                static const int ni[4] = { 0, 1, 0, -1 }, nj[4] = { -1, 0, 1, 0 };
                for (kk = 0; kk < 4; kk++) {
                    const int a2 = i + ni[kk], b2 = j + nj[kk];
                    const int inside = a2 >= 0 && a2 < G && b2 >= 0 && b2 < G;
                    const int sh = w->shr[(b2 + 1) * E + a2 + 1];
                    pin[kk] = inside ? !(sh && w->cell[b2 * G + a2]) : !sh;
                }
            }
            for (b = 0; b <= N; b++) for (a = 0; a <= N; a++) {
                const double fu = (double)a / N, fv = (double)b / N;
                const int corner = (a == 0 || a == N) && (b == 0 || b == N);
                const int pinned = corner || (b == 0 && pin[0]) || (a == N && pin[1]) || (b == N && pin[2]) || (a == 0 && pin[3]);
                const double f = rr_wetfrac(t, hc, (double)i + fu, (double)j + fv, G);
                double y;
                fm[b * FW + a] = (unsigned char)((f > 1e-9 && f < 1.0 - 1e-9) ? 255 : 0);   /* the bank strip: mud, graded by vertex */
                mud += f > 1e-9 && f < 1.0 - 1e-9;
                if (pinned) y = rr_tri(C, fu, fv);
                else {
                    const double Dp = rr_tri(D, fu, fv), Lp = rr_tri(Lc, fu, fv);
                    if (f >= 1.0 - 1e-9) {                         /* open water: the pit, under the plane */
                        y = Dp - TD5_ROAM_WATER_DEPTH;
                        if (y > Lp - RR_SHORE_BED) y = Lp - RR_SHORE_BED;
                    } else if (f <= 1e-9) {                        /* dry land, over the plane */
                        y = Dp;
                        if (y < Lp + RR_SHORE_BANK) y = Lp + RR_SHORE_BANK;
                    } else {
                        /* within one raster cell of the edge: graded by the wetness, so the plane crosses each
                         * sub-quad edge exactly where the bilinear wetness is 0.5 (RR_SHORE_BANK == RR_SHORE_BED) */
                        y = Lp + (1.0 - 2.0 * f) * RR_SHORE_BANK;
                    }
                }
                fh[b * FW + a] = y;
            }
            w->nbank += mud;
            w->sc[j * G + i] = 1;
            w->nshore++;
        }
    }
    free(S); free(wet); free(low); free(cm);
    return w->ncell > 0;
fail:
    free(S); free(wet); free(low); free(cm);
    { const int nwet = w->nwet; rr_water_free(w); w->G = G; w->nwet = nwet; w->min_clear = 1e300; }
    return 0;
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
    free(rt->lamp);
    free(rt->aud);
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
    if (sc->nlamp) {      /* [F4c] the lanterns the light registry is fed from, in the render frame */
        rt->lamp = (float *)malloc((size_t)sc->nlamp * 3u * sizeof(float));
        if (rt->lamp) {
            for (i = 0; i < sc->nlamp; i++) {
                rt->lamp[i * 3 + 0] = (float)((double)sc->lamp[i * 3 + 0] + offx);
                rt->lamp[i * 3 + 1] = (float)((double)sc->lamp[i * 3 + 1] + offy);
                rt->lamp[i * 3 + 2] = (float)((double)sc->lamp[i * 3 + 2] + offz);
            }
            rt->nlamp = sc->nlamp;
        }
    }
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

/* [G4] the 3x3 ROAD tiles round `self` for the ground clamp: through the scenery source when
 * there is one (its LRU, the file, or the stream's builder: what the scenery layer itself reads),
 * else the physics resident set (main thread only), else the tile files. */
static void rr_nb_get(RRNb *nb, const RoamTile *self, TD5_RoamScnSource *scn, int main_thread)
{
    int dx, dz;
    memset(nb, 0, sizeof *nb);
    nb->t[4] = self;
    /* [roam HC] the stop lines read the 3x3 too (a mouth's cluster may live in a neighbour) */
    if ((!s_rr.ground_clamp || !s_rr.use_ground) && !s_rr.stoplines) return;
    for (dz = -1; dz <= 1; dz++) for (dx = -1; dx <= 1; dx++) {
        const int k = (dz + 1) * 3 + (dx + 1), tx = self->tx + dx, tz = self->tz + dz;
        char path[512], err[160];
        if (!dx && !dz) { nb->t[k] = self; continue; }
        if (scn) { nb->t[k] = td5_roam_scn_road(scn, tx, tz); continue; }
#ifdef RR_HAVE_MODE
        if (main_thread) {
            const TD5_RoamResident *rs = td5_roam_resident();
            const int slot = rs ? td5_roam_resident_find(rs, tx, tz) : -1;
            if (slot >= 0 && rs->tile[slot]) { nb->t[k] = rs->tile[slot]; continue; }
        }
#else
        (void)main_thread;
#endif
        nb->own[k] = (RoamTile *)calloc(1, sizeof(RoamTile));
        if (!nb->own[k]) continue;
        td5_roam_tile_path(path, sizeof path, NULL, s_rr.place, tx, tz);
        if (td5_roam_tile_read(path, nb->own[k], 0, err, sizeof err)) nb->t[k] = nb->own[k];
        else { free(nb->own[k]); nb->own[k] = NULL; }
    }
}

static void rr_nb_free(RRNb *nb)
{
    int k;
    for (k = 0; k < RR_NB; k++) if (nb->own[k]) { td5_roam_tile_free(nb->own[k]); free(nb->own[k]); }
    memset(nb, 0, sizeof *nb);
}

/* ------------------------------------------- [roam HC] junction road surface ---- */

/* The extra triangles of the road command: junction patch slabs and stop-line bars, render frame,
 * built before the count pass so both passes read one list. */
typedef struct { float p[3][3]; float uv[3][2]; uint32_t col; uint8_t sb, cls; } RRXTri;
typedef struct { RRXTri *t; int n, cap, oom; } RRXList;

/* [roam LC] TD5_GEO_SURF_* (edge) or a patch's 0x10|grip byte -> road page class (0 smooth, 1 cobble, 2 loose) */
static int rr_cls_of_geo(int geo)
{
    if (!s_rr.surf_pages) return 0;
    return geo == TD5_GEO_SURF_COBBLE ? 1 : (geo == TD5_GEO_SURF_LOOSE ? 2 : 0);
}
static int rr_cls_of_grip(int surf)
{
    if (!s_rr.surf_pages) return 0;
    switch (surf & 0x0F) { case 4: return 1; case 2: return 2; default: return 0; }
}

static RRXTri *rr_x_add(RRXList *L)
{
    if (L->n == L->cap) {
        const int nc = L->cap ? L->cap * 2 : 256;
        RRXTri *nt = (RRXTri *)realloc(L->t, (size_t)nc * sizeof *nt);
        if (!nt) { L->oom = 1; return NULL; }
        L->t = nt; L->cap = nc;
    }
    return &L->t[L->n++];
}

/* a patch-slab vertex: place frame + the lattice texture coordinates (a, b) in RR_PATCH_TEX units */
typedef struct { double x, y, z, a, b; } RRPv;

static RRPv rr_pv_lerp(const RRPv *p, const RRPv *q, double t)
{
    RRPv r;
    r.x = p->x + (q->x - p->x) * t; r.y = p->y + (q->y - p->y) * t; r.z = p->z + (q->z - p->z) * t;
    r.a = p->a + (q->a - p->a) * t; r.b = p->b + (q->b - p->b) * t;
    return r;
}

/* keep the part of polygon in[] with sgn * (a - k) >= 0 (axis 0) or sgn * (b - k) >= 0 (axis 1) */
static int rr_pv_clip(const RRPv *in, int n, int axis, double k, double sgn, RRPv *out)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const RRPv *p = &in[i], *q = &in[(i + 1) % n];
        const double dp = sgn * ((axis ? p->b : p->a) - k), dq = sgn * ((axis ? q->b : q->a) - k);
        if (dp >= 0.0) out[m++] = *p;
        if ((dp >= 0.0) != (dq >= 0.0)) out[m++] = rr_pv_lerp(p, q, dp / (dp - dq));
    }
    return m;
}

/* One junction patch triangle -> its cell pieces (TD5RE_ROAM_PATCH_UV=1) or itself with the old UVs.
 * Cells are RR_PATCH_TEX square on the lattice axes, the size of a ribbon lane quad: the texture needs
 * the cut only in a (the slab), the cut in b is for the depth. A whole patch triangle (fans reach 30 m)
 * lost to the ground 40 cm under it near a low camera, while the ribbon's lane quads beside it did not
 * (roam HC: small0 at -34.905171,-57.944409, eye 1.7 m over the patch; old triangles + the same exe =
 * grass, cells = road). */
static void rr_patch_tri(const RoamTile *t, const RoamTri *tr, uint8_t sbi, RRXList *L, uint32_t *npiece)
{
    RRPv v[3];
    float p[3][3];
    uint32_t col;
    double amin, amax, bmin, bmax, k, kb;
    int i, j;
    for (i = 0; i < 3; i++) {
        const TD5_StripVertex *sv = &t->vert[tr->v[i]];
        double u, w;
        v[i].x = (double)t->org_x + (double)tr->ox + (double)sv->x;
        v[i].y = (double)tr->oy + (double)sv->y;
        v[i].z = (double)t->org_z + (double)tr->oz + (double)sv->z;
        roam_grid_uv(&s_rr.grid, v[i].x, v[i].z, &u, &w);
        v[i].a = u / RR_PATCH_TEX; v[i].b = w / RR_PATCH_TEX;
        rr_pt(v[i].x, v[i].y, v[i].z, p[i]);
    }
    col = rr_shade(p[0], p[1], p[2]);
    amin = fmin(v[0].a, fmin(v[1].a, v[2].a)); amax = fmax(v[0].a, fmax(v[1].a, v[2].a));
    bmax = fmax(v[0].b, fmax(v[1].b, v[2].b));
    bmin = floor(fmin(v[0].b, fmin(v[1].b, v[2].b)));
    if (!s_rr.patch_uv || amax - amin > 64.0 || bmax - bmin > 64.0) {
        /* the old mapping: a plain asphalt patch, UVs from the triangle's first vertex */
        RRXTri *x = rr_x_add(L);
        if (!x) return;
        memcpy(x->p, p, sizeof p);
        for (i = 0; i < 3; i++) {
            x->uv[i][0] = 0.5f + (p[i][0] - p[0][0]) / RR_PATCH_UV_UNITS;
            x->uv[i][1] = 0.5f + (p[i][2] - p[0][2]) / RR_PATCH_UV_UNITS;
        }
        x->col = col; x->sb = sbi; x->cls = (uint8_t)rr_cls_of_grip(tr->surface);
        return;
    }
    for (k = floor(amin); k < amax; k += 1.0) for (kb = bmin; kb < bmax; kb += 1.0) {
        RRPv s1[5], s2[7], s3[9], s4[11];
        int n1 = rr_pv_clip(v, 3, 0, k, 1.0, s1), n2;
        if (n1 < 3) continue;
        n2 = rr_pv_clip(s1, n1, 0, k + 1.0, -1.0, s2);
        if (n2 < 3) continue;
        n2 = rr_pv_clip(s2, n2, 1, kb, 1.0, s3);
        if (n2 < 3) continue;
        n2 = rr_pv_clip(s3, n2, 1, kb + 1.0, -1.0, s4);
        for (j = 1; j + 1 < n2; j++) {
            const RRPv *q[3] = { &s4[0], &s4[j], &s4[j + 1] };
            const double ar = (q[1]->x - q[0]->x) * (q[2]->z - q[0]->z) - (q[1]->z - q[0]->z) * (q[2]->x - q[0]->x);
            RRXTri *x;
            if (fabs(ar) < 1.0) continue;               /* a sliver of the cut: nothing to draw */
            x = rr_x_add(L);
            if (!x) return;
            for (i = 0; i < 3; i++) {
                double fa = q[i]->a - k;
                if (fa < 0.0) fa = 0.0;
                if (fa > 1.0) fa = 1.0;
                rr_pt(q[i]->x, q[i]->y, q[i]->z, x->p[i]);
                x->uv[i][0] = (float)(RR_PATCH_U0 + fa * (RR_PATCH_U1 - RR_PATCH_U0));
                x->uv[i][1] = (float)(q[i]->b - bmin);
            }
            x->col = col; x->sb = sbi; x->cls = (uint8_t)rr_cls_of_grip(tr->surface);
            (*npiece)++;
        }
    }
}

/* A cut vertex of a junction patch that can take a stop line: place-frame x/z + its cluster. */
typedef struct { int64_t x, z; int cl; } RRCutV;
typedef struct { int64_t key; int signal, minl, maxl; } RRClus;

static int rr_cutv_cmp(const void *pa, const void *pb)
{
    const RRCutV *a = (const RRCutV *)pa, *b = (const RRCutV *)pb;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    if (a->z != b->z) return a->z < b->z ? -1 : 1;
    return a->cl - b->cl;
}

static int rr_cutv_find(const RRCutV *v, int n, int64_t x, int64_t z)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (v[mid].x < x || (v[mid].x == x && v[mid].z < z)) lo = mid + 1;
        else if (v[mid].x == x && v[mid].z == z) return v[mid].cl;
        else hi = mid - 1;
    }
    return -1;
}

/* point at fractional vertex index f along a row of n+1 vertices (place frame, doubles) */
static void rr_row_at(const double (*r)[3], int n, double f, double o[3])
{
    int i = (int)floor(f);
    double t;
    if (i < 0) i = 0;
    if (i > n - 1) i = n - 1;
    t = f - (double)i;
    o[0] = r[i][0] + (r[i + 1][0] - r[i][0]) * t;
    o[1] = r[i][1] + (r[i + 1][1] - r[i][1]) * t;
    o[2] = r[i][2] + (r[i + 1][2] - r[i][2]) * t;
}

/* Stop lines of tile t. A ribbon row whose every vertex is a vertex of ONE junction patch of the 3x3 is a
 * mouth (the cut rows are shared bit for bit, T3/T5), so a mouth whose cluster another tile owns is found
 * too. Taken: clusters of 3+ arms that are not a taper / bend; signalised clusters on every inbound arm,
 * others on the minor arms (fewer lanes than the cluster's widest), or on every arm when all are equal.
 * Not taken: roundabout rings, loops. Inbound: a one-way street flowing into the junction (the whole
 * width), a two-way street's right half (Argentina drives on the right). The driver's right of a heading
 * (hx, hz) is (-hz, hx), the engine's convention (td5_roam_furniture.c side -1 = right kerb); [roam M] the
 * place frame is the GAME frame (td5_geo_frame.h: x = -east), so that is the real right as well. */
static void rr_stop_lines(const RoamTile *t, const RRNb *nb, const uint8_t *spsb, RRXList *L, uint32_t *nstop)
{
    RRCutV *cv = NULL;
    RRClus *cl = NULL;
    int ncv = 0, ccv = 0, ncl = 0, ccl = 0, k;
    int32_t *sedge = NULL;
    uint32_t i, e;
    if (!nb) return;
    for (k = 0; k < RR_NB; k++) {
        const RoamTile *nt = nb->t[k];
        uint32_t ni;
        if (!nt) continue;
        for (ni = 0; ni < nt->nnode; ni++) {
            const RoamNodeRec *nr = &nt->node[ni];
            RRClus c;
            uint32_t a, q, s;
            if (nr->narm < 3 || (nr->flags & (TD5_ROAM_CL_TAPER | TD5_ROAM_CL_BEND))) continue;
            if ((uint32_t)nr->arm_first + nr->narm > nt->narm || (uint32_t)nr->tri_first + nr->tri_count > nt->ntri) continue;
            c.key = nr->cluster_key; c.signal = 0; c.minl = 99; c.maxl = 0;
            for (a = 0; a < nr->narm; a++) {
                const int ln = nt->arm[nr->arm_first + a].lanes;
                if (ln < c.minl) c.minl = ln;
                if (ln > c.maxl) c.maxl = ln;
            }
            for (s = 0; s < nt->nsign; s++) if (nt->sign[s].cluster_key == nr->cluster_key) { c.signal = 1; break; }
            if (ncl == ccl) {
                const int nc = ccl ? ccl * 2 : 64;
                RRClus *n2 = (RRClus *)realloc(cl, (size_t)nc * sizeof *n2);
                if (!n2) goto done;
                cl = n2; ccl = nc;
            }
            cl[ncl] = c;
            for (q = 0; q < nr->tri_count; q++) {
                const RoamTri *tr = &nt->tri[nr->tri_first + q];
                int j;
                for (j = 0; j < 3; j++) {
                    const TD5_StripVertex *sv;
                    if (tr->v[j] >= nt->nvert) continue;
                    sv = &nt->vert[tr->v[j]];
                    if (ncv == ccv) {
                        const int nc = ccv ? ccv * 2 : 1024;
                        RRCutV *n2 = (RRCutV *)realloc(cv, (size_t)nc * sizeof *n2);
                        if (!n2) goto done;
                        cv = n2; ccv = nc;
                    }
                    cv[ncv].x = (int64_t)nt->org_x + tr->ox + sv->x;
                    cv[ncv].z = (int64_t)nt->org_z + tr->oz + sv->z;
                    cv[ncv].cl = ncl;
                    ncv++;
                }
            }
            ncl++;
        }
    }
    if (!ncv) goto done;
    qsort(cv, (size_t)ncv, sizeof *cv, rr_cutv_cmp);
    sedge = (int32_t *)malloc((t->nspan ? t->nspan : 1u) * sizeof(int32_t));
    if (!sedge) goto done;
    for (i = 0; i < t->nspan; i++) sedge[i] = -1;
    for (e = 0; e < t->nedge; e++)
        for (i = t->edge[e].span_first; i < (uint32_t)t->edge[e].span_first + t->edge[e].span_count && i < t->nspan; i++)
            sedge[i] = (int32_t)e;
    for (i = 0; i < t->nspan; i++) {
        const TD5_StripSpan *sp = &t->span[i];
        uint32_t Lv, Rv;
        int n, side, kk;
        double row[2][RR_MAX_LANES + 1][3];
        if (spsb[i] == 0xFF || sedge[i] < 0 || !rr_span_ok(t, sp, &Lv, &Rv, &n)) continue;
        for (kk = 0; kk <= n; kk++) {
            const TD5_StripVertex *a = &t->vert[Lv + (uint32_t)kk], *b = &t->vert[Rv + (uint32_t)kk];
            row[0][kk][0] = (double)t->org_x + sp->origin_x + a->x; row[0][kk][1] = (double)sp->origin_y + a->y;
            row[0][kk][2] = (double)t->org_z + sp->origin_z + a->z;
            row[1][kk][0] = (double)t->org_x + sp->origin_x + b->x; row[1][kk][1] = (double)sp->origin_y + b->y;
            row[1][kk][2] = (double)t->org_z + sp->origin_z + b->z;
        }
        for (side = 0; side < 2; side++) {
            const RoamEdgeRec *er = &t->edge[sedge[i]];
            const double (*m)[3] = (const double (*)[3])row[side], (*f)[3] = (const double (*)[3])row[side ^ 1];
            int c = -1, inb, end;
            double mc[3], fc[3], hx, hz, hl, f0, f1, fstep, ff;
            for (kk = 0; kk <= n; kk++) {
                const int ck = rr_cutv_find(cv, ncv, (int64_t)m[kk][0], (int64_t)m[kk][2]);
                if (ck < 0 || (c >= 0 && ck != c)) { c = -2; break; }
                c = ck;
            }
            if (c < 0) continue;
            if (er->flags & ROAM_EF_ROUNDABOUT) continue;
            if (rr_cls_of_geo(er->surface)) continue;                          /* [roam LC] no paint on cobble / dirt */
            if (er->end_key[0] == er->end_key[1]) continue;                   /* a loop: which end is this? */
            if (er->end_key[0] == cl[c].key) end = 0;
            else if (er->end_key[1] == cl[c].key) end = 1;
            else continue;
            if (!cl[c].signal && cl[c].minl != cl[c].maxl && er->lanes >= cl[c].maxl) continue;   /* the major road */
            rr_row_at(m, n, 0.5 * n, mc);
            rr_row_at(f, n, 0.5 * n, fc);
            hx = mc[0] - fc[0]; hz = mc[2] - fc[2]; hl = hypot(hx, hz);          /* heading INTO the junction */
            if (hl < RR_STOP_S1 * 1.25) continue;                                /* a stub quad: no room */
            hx /= hl; hz /= hl;
            if (er->oneway) {
                inb = (er->oneway > 0) ? (end == 1) : (end == 0);
                if (!inb) continue;
                f0 = 0.0; f1 = (double)n;
            } else {
                /* the right of travel is (-hz, hx): which end of the row lies there */
                const double lat0 = -(m[0][0] - mc[0]) * hz + (m[0][2] - mc[2]) * hx;
                if (lat0 > 0.0) { f0 = 0.0; f1 = 0.5 * n; } else { f0 = 0.5 * n; f1 = (double)n; }
            }
            /* the bar, one quad per piece between row vertices */
            for (ff = f0; ff < f1 - 1e-9; ff = fstep) {
                double a0[3], a1[3], b0[3], b1[3], pa[3], pb[3];
                double q[4][3];
                int z;
                fstep = floor(ff + 1e-9) + 1.0;
                if (fstep > f1) fstep = f1;
                rr_row_at(m, n, ff, a0); rr_row_at(f, n, ff, b0);
                rr_row_at(m, n, fstep, a1); rr_row_at(f, n, fstep, b1);
                for (z = 0; z < 2; z++) {
                    const double *pm = z ? a1 : a0, *pf = z ? b1 : b0;
                    const double dl = hypot(pf[0] - pm[0], pf[2] - pm[2]);
                    const double t0 = dl > 1.0 ? RR_STOP_S0 / dl : 0.0, t1 = dl > 1.0 ? RR_STOP_S1 / dl : 0.0;
                    for (kk = 0; kk < 3; kk++) {
                        pa[kk] = pm[kk] + (pf[kk] - pm[kk]) * t0;
                        pb[kk] = pm[kk] + (pf[kk] - pm[kk]) * t1;
                    }
                    memcpy(q[z ? 1 : 0], pa, sizeof pa);
                    memcpy(q[z ? 2 : 3], pb, sizeof pb);
                }
                {
                    float r[4][3];
                    static const float uu[4] = { (float)RR_STOP_U0, (float)RR_STOP_U1, (float)RR_STOP_U1, (float)RR_STOP_U0 };
                    static const float vv[4] = { (float)RR_STOP_V0, (float)RR_STOP_V0, (float)RR_STOP_V1, (float)RR_STOP_V1 };
                    static const int tri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
                    uint32_t col;
                    int tt;
                    for (z = 0; z < 4; z++) rr_pt(q[z][0], q[z][1] + RR_STOP_LIFT, q[z][2], r[z]);
                    col = rr_shade(r[0], r[1], r[2]);
                    for (tt = 0; tt < 2; tt++) {
                        RRXTri *x = rr_x_add(L);
                        if (!x) goto done;
                        for (z = 0; z < 3; z++) {
                            memcpy(x->p[z], r[tri[tt][z]], sizeof r[0]);
                            x->uv[z][0] = uu[tri[tt][z]]; x->uv[z][1] = vv[tri[tt][z]];
                        }
                        x->col = col; x->sb = spsb[i]; x->cls = 0;
                    }
                }
            }
            (*nstop)++;
        }
    }
done:
    free(cv); free(cl); free(sedge);
}

/* [roam LC] first vertex of road class `cls` in a sub-block mesh: classes lay out one after another (tris, then quads) */
static int rr_cls_base(const RRSb *b, int cls)
{
    int c, o = 0;
    for (c = 0; c < cls; c++) o += b->cnt_t[c] * 3 + b->cnt_q[c] * 4;
    return o;
}

/* Bake `t` with the ground from `hc` (NULL = no ground). Returns the tile or NULL
 * (nothing to draw / out of memory). [F3] Thread-safe for a caller-owned `hc`: it
 * reads only the session's fixed configuration (frame, grid, pages, knobs), never
 * the tile table, so the stream worker bakes while the main thread draws. */
static int rr_popcount64(uint64_t m) { int n = 0; while (m) { m &= m - 1; n++; } return n; }

/* [roam NC] the refill's sub-cells: the cell's own two fan triangles (0 1 2, 0 2 3), cut into
 * TD5_ROAM_CUT_SUB^2 squares (a square on the diagonal splits along it, so every piece lies in one triangle) */
static void rr_bake_fill(const RoamTile *t, const RRGround *gr, RRSb *sb, RRTile *rt)
{
    const int G = gr->G, W = G + 1, N = TD5_ROAM_CUT_SUB;
    int a, bq;
    for (bq = 0; bq < G; bq++) for (a = 0; a < G; a++) {
        static const int di[4] = { 0, 1, 1, 0 }, dj[4] = { 0, 0, 1, 1 };
        const uint64_t m = gr->fill[bq * G + a];
        double h[4], x, z;
        float c3[3][3];
        uint32_t col;
        RRSb *b;
        int kk, i, j;
        if (!m || rr_ground_cell_ok(gr, a, bq)) continue;
        for (kk = 0; kk < 4; kk++) h[kk] = gr->h[(bq + dj[kk]) * W + a + di[kk]] - s_rr.ground_drop;
        for (kk = 0; kk < 3; kk++) {                   /* the whole cell's shade (rr_bake: its first triangle) */
            rr_ground_point(t, a + di[kk], bq + dj[kk], G, &x, &z);
            rr_pt(x, h[kk], z, c3[kk]);
        }
        col = rr_shade(c3[0], c3[1], c3[2]);
        b = &sb[rr_cell_sb(gr, a, bq)];
        for (j = 0; j < N; j++) for (i = 0; i < N; i++) {
            TD5_MeshVertex *q;
            if (!(m & ((uint64_t)1 << (j * N + i)))) continue;
            q = b->v + b->nrt * 3 + b->nrq * 4 + b->cg * 4;
            for (kk = 0; kk < 4; kk++) {
                const double fu = (double)(i + di[kk]) / N, fv = (double)(j + dj[kk]) / N;
                const double y = (fu >= fv) ? h[0] + fu * (h[1] - h[0]) + fv * (h[2] - h[1])
                                            : h[0] + fv * (h[3] - h[0]) + fu * (h[2] - h[3]);
                float p[3];
                rr_ground_point_f(t, (double)a + fu, (double)bq + fv, G, &x, &z);
                rr_pt(x, y, z, p);
                rr_put(b, q + kk, p, col, (float)fu, (float)fv);
            }
            b->cg++;
            rt->nground++;
            rt->nfill++;
        }
    }
}

#ifndef TD5RE_RELEASE
/* [roam PC] the drawn ground at a fractional lattice point of the tile (sub-mesh in a shore cell), drop included */
static double rr_drawn_at(const RRGround *gr, const RRWater *w, double fi, double fj, int *cell_out)
{
    const int G = gr->G, W = G + 1;
    int a = (int)floor(fi), b = (int)floor(fj);
    double fu, fv, C[4];
    if (a < 0) a = 0;
    if (a > G - 1) a = G - 1;
    if (b < 0) b = 0;
    if (b > G - 1) b = G - 1;
    fu = fi - a; fv = fj - b;
    *cell_out = b * G + a;
    if (w->sc && w->sc[b * G + a]) {
        const int N = RR_SHORE_N, FW = RR_SHORE_FW;
        const double *fh = &w->fh[(size_t)(b * G + a) * FW * FW];
        int si = (int)floor(fu * N), sj = (int)floor(fv * N);
        double H[4];
        if (si > N - 1) si = N - 1;
        if (sj > N - 1) sj = N - 1;
        H[0] = fh[sj * FW + si]; H[1] = fh[sj * FW + si + 1]; H[2] = fh[(sj + 1) * FW + si + 1]; H[3] = fh[(sj + 1) * FW + si];
        return rr_tri(H, fu * N - si, fv * N - sj);
    }
    C[0] = gr->h[b * W + a]; C[1] = gr->h[b * W + a + 1]; C[2] = gr->h[(b + 1) * W + a + 1]; C[3] = gr->h[(b + 1) * W + a];
    return rr_tri(C, fu, fv) - s_rr.ground_drop;
}

/* [roam PC] is water DRAWN at a fractional lattice point (a water quad there, its plane over the drawn ground)? */
static int rr_water_drawn_at(const RRGround *gr, const RRWater *w, double fi, double fj)
{
    const int G = gr->G, W = G + 1;
    int cell, a, b;
    double g, L[4];
    if (!w->cell) return 0;
    g = rr_drawn_at(gr, w, fi, fj, &cell);
    if (!w->cell[cell]) return 0;
    a = cell % G; b = cell / G;
    L[0] = w->L[b * W + a]; L[1] = w->L[b * W + a + 1]; L[2] = w->L[(b + 1) * W + a + 1]; L[3] = w->L[(b + 1) * W + a];
    return rr_tri(L, fi - a, fj - b) > g;
}

/* [roam PC] SHORE CENSUS over the tile's mixed cells (w->shr): on a 0.5 m grid, each pair of neighbours across the
 * raster's 0.5 contour is a CROSSING of the real shore; it is a > 2 m STEP when the point 2 m into the wet side is not
 * drawn as water or the point 2 m into the dry side is. Shore length ~ pi/4 * 0.5 m * crossings (Cauchy-Crofton).
 * Crossings whose test points leave the tile are counted apart (sh_edge). Lots / lawns over the water are not seen. */
static void rr_shore_census(const RoamTile *t, const RRGround *gr, const RRWater *w, TD5_RoamHeight *hc, RRTile *rt)
{
    const int G = gr->G, M = 32, E = G + 2, K = 4;     /* 16 m / 32 = 0.5 m; 2 m = 4 steps */
    const double st = 1.0 / M;
    int a, b, i, j, d;
    unsigned char *sh = NULL;
    if (!s_rr.water) return;
    sh = (unsigned char *)calloc((size_t)G * G, 1);
    if (!sh) return;
    for (b = 0; b < G; b++) for (a = 0; a < G; a++) {
        if (w->shr) { sh[b * G + a] = w->shr[(b + 1) * E + a + 1]; continue; }
        {   /* the knob off: judge the cell the same way (mixed sub-samples) */
            int anyw = 0, allw = 1, x, z;
            for (z = 0; z <= RR_SHORE_N; z++) for (x = 0; x <= RR_SHORE_N; x++) {
                const int fw = rr_wetfrac(t, hc, a + (double)x / RR_SHORE_N, b + (double)z / RR_SHORE_N, G) >= 0.5;
                anyw |= fw; allw &= fw;
            }
            sh[b * G + a] = (unsigned char)(anyw && !allw);
        }
    }
    for (b = 0; b < G; b++) for (a = 0; a < G; a++) {
        if (!sh[b * G + a]) continue;
        for (j = 0; j < M; j++) for (i = 0; i < M; i++) {
            const double fi = a + (i + 0.5) * st, fj = b + (j + 0.5) * st;
            const int r0 = rr_wetfrac(t, hc, fi, fj, G) >= 0.5;
            for (d = 0; d < 2; d++) {                           /* the neighbour +u, then +v */
                const double gi = fi + (d == 0 ? st : 0.0), gj = fj + (d == 1 ? st : 0.0);
                const int r1 = rr_wetfrac(t, hc, gi, gj, G) >= 0.5;
                double wi, wj, di2, dj2;
                if (r0 == r1) continue;
                rt->sh_cross++;
                /* 2 m past the crossing into each side */
                if (r1) { wi = gi + (d == 0 ? K * st : 0.0); wj = gj + (d == 1 ? K * st : 0.0); di2 = fi - (d == 0 ? K * st : 0.0); dj2 = fj - (d == 1 ? K * st : 0.0); }
                else    { wi = fi - (d == 0 ? K * st : 0.0); wj = fj - (d == 1 ? K * st : 0.0); di2 = gi + (d == 0 ? K * st : 0.0); dj2 = gj + (d == 1 ? K * st : 0.0); }
                if (wi < 0.0 || wj < 0.0 || wi > G || wj > G || di2 < 0.0 || dj2 < 0.0 || di2 > G || dj2 > G) { rt->sh_edge++; continue; }
                if ((rr_wetfrac(t, hc, wi, wj, G) >= 0.5 && !rr_water_drawn_at(gr, w, wi, wj)) ||
                    (rr_wetfrac(t, hc, di2, dj2, G) < 0.5 && rr_water_drawn_at(gr, w, di2, dj2)))
                    rt->sh_step++;
            }
        }
    }
    free(sh);
}
#endif

static RRTile *rr_bake(const RoamTile *t, TD5_RoamHeight *hc, const RRNb *nb)
{
    RRSb sb[RR_MAXMESH];
    RRGround gr;
    RRWater wa;
    RRTile *rt;
    uint8_t *spsb = NULL, *trsb = NULL, *besb = NULL, *spcls = NULL;
    RRXList xl;
    uint32_t i;
    int k, ok = 1;
    const float kerb_h = (float)(0.45 * RR_UPM), deck_h = (float)(1.2 * RR_UPM);

    memset(sb, 0, sizeof sb);
    memset(&xl, 0, sizeof xl);
    memset(&gr, 0, sizeof gr);
    memset(&wa, 0, sizeof wa);
    for (k = 0; k < RR_MAXMESH; k++) {
        sb[k].lo[0] = sb[k].lo[1] = sb[k].lo[2] = 1e30f;
        sb[k].hi[0] = sb[k].hi[1] = sb[k].hi[2] = -1e30f;
    }
    rt = (RRTile *)calloc(1, sizeof *rt);
    spsb = (uint8_t *)calloc(t->nspan ? t->nspan : 1u, 1);
    trsb = (uint8_t *)calloc(t->ntri ? t->ntri : 1u, 1);
    besb = (uint8_t *)calloc(t->nbedge ? t->nbedge : 1u, 1);
    spcls = (uint8_t *)calloc(t->nspan ? t->nspan : 1u, 1);
    if (!rt || !spsb || !trsb || !besb || !spcls) { ok = 0; goto done_counts; }
    if (s_rr.surf_pages) {                           /* [roam LC] a span takes its edge's surface class */
        uint32_t e, si;
        for (e = 0; e < t->nedge; e++) {
            const int cl = rr_cls_of_geo(t->edge[e].surface);
            for (si = t->edge[e].span_first; cl && si < (uint32_t)t->edge[e].span_first + t->edge[e].span_count && si < t->nspan; si++)
                spcls[si] = (uint8_t)cl;
        }
    }
    rt->tx = t->tx; rt->tz = t->tz;

    /* ---- pass 1: count per sub-block ---- */
    for (i = 0; i < t->nspan; i++) {
        uint32_t L, R; int n; double px, pz;
        spsb[i] = 0xFF;
        if (!rr_span_ok(t, &t->span[i], &L, &R, &n)) continue;
        rr_span_centre(t, &t->span[i], L, R, n, &px, &pz);
        spsb[i] = (uint8_t)rr_sb_of(t, px, pz);
        sb[spsb[i]].nrq += n;
        sb[spsb[i]].cnt_q[spcls[i]] += n;
    }
    for (i = 0; i < t->ntri; i++) {
        const RoamTri *tr = &t->tri[i];
        double px, pz;
        trsb[i] = 0xFF;
        if (tr->v[0] >= t->nvert || tr->v[1] >= t->nvert || tr->v[2] >= t->nvert) continue;
        rr_tri_centre(t, tr, &px, &pz);
        trsb[i] = (uint8_t)rr_sb_of(t, px, pz);
    }
    /* [roam HC] the road command's triangles: patch slabs (or the old patch triangles) + stop lines */
    for (i = 0; i < t->ntri; i++)
        if (trsb[i] != 0xFF) { rr_patch_tri(t, &t->tri[i], trsb[i], &xl, &rt->npiece); rt->ntri++; }
    if (s_rr.stoplines) rr_stop_lines(t, nb, spsb, &xl, &rt->nstop);
    if (xl.oom) { ok = 0; goto done_counts; }
    for (k = 0; k < xl.n; k++) { sb[xl.t[k].sb].nrt++; sb[xl.t[k].sb].cnt_t[xl.t[k].cls]++; }
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
    rr_ground_build(t, &gr, hc, nb);
    if (gr.h) {
        int a, b;
        for (b = 0; b < gr.G; b++) for (a = 0; a < gr.G; a++) {
            if (rr_ground_cell_ok(&gr, a, b)) sb[rr_cell_sb(&gr, a, b)].ngq++;
            else if (gr.fill && gr.fill[b * gr.G + a])                  /* [roam NC] the refill */
                sb[rr_cell_sb(&gr, a, b)].ngq += rr_popcount64(gr.fill[b * gr.G + a]);
        }
        if (rr_water_build(t, &gr, hc, nb, &wa))                       /* [roam LE] */
            for (b = 0; b < gr.G; b++) for (a = 0; a < gr.G; a++) {
                if (wa.cell[b * gr.G + a]) sb[rr_cell_sb(&gr, a, b)].nwq++;
                if (wa.sc && wa.sc[b * gr.G + a]) sb[rr_cell_sb(&gr, a, b)].ngq += RR_SHORE_N * RR_SHORE_N - 1;   /* [roam PC] */
            }
    }
    rt->nwet = (uint32_t)wa.nwet; rt->nwater_road = (uint32_t)wa.nroad; rt->nwater_deck = (uint32_t)wa.ndeck;
    rt->nshore = (uint32_t)wa.nshore; rt->nshore_ext = (uint32_t)wa.next; rt->nbank = (uint32_t)wa.nbank;   /* [roam PC] */
#ifndef TD5RE_RELEASE
    if (s_rr.shore_census && gr.h) rr_shore_census(t, &gr, &wa, hc, rt);
#endif
    rt->water_clear = wa.min_clear;

    /* ---- allocate one block per non-empty sub-block ---- */
    for (k = 0; k < RR_MAXMESH; k++) {
        const int nv = sb[k].nrt * 3 + (sb[k].nrq + sb[k].ngq + sb[k].nsq + sb[k].nwq) * 4;
        size_t hdr = (sizeof(TD5_MeshHeader) + 15u) & ~(size_t)15u;
        size_t bytes;
        uint8_t *blk;
        if (nv <= 0) continue;
        bytes = hdr + (3u + RR_NCLS) * sizeof(TD5_PrimitiveCmd) + (size_t)nv * sizeof(TD5_MeshVertex);
        blk = (uint8_t *)calloc(1, bytes);
        if (!blk) { ok = 0; break; }
        sb[k].m = (TD5_MeshHeader *)blk;
        sb[k].m->commands = (TD5_PrimitiveCmd *)(blk + hdr);
        sb[k].v = (TD5_MeshVertex *)(blk + hdr + (3u + RR_NCLS) * sizeof(TD5_PrimitiveCmd));
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
            TD5_MeshVertex *q = b->v + rr_cls_base(b, spcls[i]) + b->cnt_t[spcls[i]] * 3 + b->cur_q[spcls[i]] * 4;
            const uint32_t col = rr_shade(pl[kk], pl[kk + 1], pr[kk + 1]);
            /* [roam HC] the page's lane line sits on u = 0 of every lane, so lane 0 drew a dashed line down
             * ONE kerb (whichever the ribbon stores first) and the last lane's u = n edge half a line down
             * the other; only the dividers between lanes keep it now (TD5RE_ROAM_KERB_LINE=1: the old look) */
            const float u0 = (float)kk + ((kk == 0 && !s_rr.kerb_line) ? (float)RR_PATCH_U0 : 0.0f);
            const float u1 = (float)(kk + 1) - ((kk == n - 1 && !s_rr.kerb_line) ? (float)(1.0 - RR_PATCH_U1) : 0.0f);
            rr_put(b, q + 0, pl[kk],     col, u0, 0.0f);
            rr_put(b, q + 1, pl[kk + 1], col, u1, 0.0f);
            rr_put(b, q + 2, pr[kk + 1], col, u1, 1.0f);
            rr_put(b, q + 3, pr[kk],     col, u0, 1.0f);
            b->cur_q[spcls[i]]++; b->cq++; rt->ncls[spcls[i]]++;
            rt->nroad++;
        }
    }
    for (k = 0; k < xl.n; k++) {
        const RRXTri *x = &xl.t[k];
        RRSb *b = &sb[x->sb];
        TD5_MeshVertex *q = b->v + rr_cls_base(b, x->cls) + b->cur_t[x->cls] * 3;
        int kk;
        for (kk = 0; kk < 3; kk++) rr_put(b, q + kk, x->p[kk], x->col, x->uv[kk][0], x->uv[kk][1]);
        b->cur_t[x->cls]++; b->ct++; rt->ncls[x->cls]++;
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
            if (wa.sc && wa.sc[bq * gr.G + a]) {                         /* [roam PC] a shore: the sub-mesh */
                const int N = RR_SHORE_N, FW = RR_SHORE_FW;
                const double *fh = &wa.fh[(size_t)(bq * gr.G + a) * FW * FW];
                const unsigned char *fm = &wa.fm[(size_t)(bq * gr.G + a) * FW * FW];
                int si, sj;
                b = &sb[rr_cell_sb(&gr, a, bq)];
                for (sj = 0; sj < N; sj++) for (si = 0; si < N; si++) {
                    for (kk = 0; kk < 4; kk++) {
                        rr_ground_point_f(t, (double)a + (double)(si + di[kk]) / N, (double)bq + (double)(sj + dj[kk]) / N, gr.G, &x, &z);
                        rr_pt(x, fh[(sj + dj[kk]) * FW + si + di[kk]], z, p[kk]);
                    }
                    col = rr_shade(p[0], p[1], p[2]);
                    q = b->v + b->nrt * 3 + b->nrq * 4 + b->cg * 4;
                    for (kk = 0; kk < 4; kk++) {                         /* the bank: mud where the vertex is in the band */
                        const unsigned g = col & 0xFFu, m = fm[(sj + dj[kk]) * FW + si + di[kk]];
                        const unsigned r = g - (g * 50u / 255u) * m / 255u, gg = g - (g * 77u / 255u) * m / 255u, bb = g - (g * 127u / 255u) * m / 255u;
                        rr_put(b, q + kk, p[kk], 0xFF000000u | (r << 16) | (gg << 8) | bb, (float)(si + di[kk]) / N, (float)(sj + dj[kk]) / N);
                    }
                    b->cg++;
                    rt->nground++;
                }
                continue;
            }
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
        if (gr.fill) rr_bake_fill(t, &gr, sb, rt);                       /* [roam NC] after the whole cells */
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
    if (wa.cell) {                                                      /* [roam LE] water quads, after the skirts */
        int a, bq;
        const int W = gr.G + 1;
        for (bq = 0; bq < gr.G; bq++) for (a = 0; a < gr.G; a++) {
            static const int di[4] = { 0, 1, 1, 0 }, dj[4] = { 0, 0, 1, 1 };
            float p[4][3], uv[4][2];
            RRSb *b;
            TD5_MeshVertex *q;
            uint32_t col;
            int kk;
            if (!wa.cell[bq * gr.G + a]) continue;
            for (kk = 0; kk < 4; kk++) {
                double x, z;
                rr_ground_point(t, a + di[kk], bq + dj[kk], gr.G, &x, &z);
                rr_pt(x, wa.L[(bq + dj[kk]) * W + a + di[kk]], z, p[kk]);
                uv[kk][0] = (float)(x / RR_WATER_TILE);                 /* place frame: rebase-proof, seam-free */
                uv[kk][1] = (float)(z / RR_WATER_TILE);
            }
            col = rr_shade(p[0], p[1], p[2]);
            b = &sb[rr_cell_sb(&gr, a, bq)];
            q = b->v + b->nrt * 3 + (b->nrq + b->ngq + b->nsq) * 4 + b->cw * 4;
            for (kk = 0; kk < 4; kk++) rr_put(b, q + kk, p[kk], col, uv[kk][0], uv[kk][1]);
            b->cw++;
            rt->nwater++;
        }
    }

    /* ---- headers, commands, bounding spheres ---- */
    for (k = 0; k < RR_MAXMESH; k++) {
        TD5_MeshHeader *m = sb[k].m;
        TD5_PrimitiveCmd *c;
        int nc = 0, cls;
        float cx, cy, cz, r, dx, dy, dz;
        if (!m) continue;
        c = m->commands;
        for (cls = 0; cls < RR_NCLS; cls++) {
            if (!sb[k].cnt_t[cls] && !sb[k].cnt_q[cls]) continue;
            c[nc].dispatch_type = 0; c[nc].texture_page_id = (int16_t)s_rr.page_cls[cls];
            c[nc].triangle_count = (uint16_t)sb[k].cnt_t[cls]; c[nc].quad_count = (uint16_t)sb[k].cnt_q[cls];
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
        if (sb[k].nwq) {                                                /* [roam LE] */
            c[nc].dispatch_type = 0; c[nc].texture_page_id = (int16_t)s_rr.page_water;
            c[nc].triangle_count = 0; c[nc].quad_count = (uint16_t)sb[k].nwq;
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
    rr_water_free(&wa);
    free(spsb); free(trsb); free(besb); free(spcls); free(xl.t);
    if (rt->nwater || rt->nwater_road)
        TD5_LOG_D(LOG_TAG, "ROAM_WATER tile %d,%d quads=%u wet_points=%u road_cells_dropped=%u deck_cells=%u "
                  "min_clear=%.2f m", t->tx, t->tz, rt->nwater, rt->nwet, rt->nwater_road, rt->nwater_deck,
                  rt->water_clear < 1e299 ? rt->water_clear / RR_UPM : -1.0);
    TD5_LOG_D(LOG_TAG, "ROAM_RENDER_HC tile %d,%d patch_tris=%u pieces=%u stop_lines=%u lane_quads=%u",
              t->tx, t->tz, rt->ntri, rt->npiece, rt->nstop, rt->nroad);
    if (rt->nmesh == 0) { free(rt); return NULL; }
    return rt;

done_free:
    rr_ground_free(&gr);
    rr_water_free(&wa);
    for (k = 0; k < RR_MAXMESH; k++) free(sb[k].m);
done_counts:
    free(spsb); free(trsb); free(besb); free(spcls); free(xl.t);
    free(rt);
    return NULL;
}

/* [F4a] road + ground bake, then the SCENERY layer when a source is given (the
 * caller's: one per baking thread). The tile sphere grows to hold both. */
static RRTile *rr_bake_full(const RoamTile *t, TD5_RoamHeight *hc, TD5_RoamScnSource *src, int main_thread)
{
    RRTile *rt;
    TD5_RoamScenery *sc;
    RRNb nb;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    int k, a, any = 0;
    rr_nb_get(&nb, t, src, main_thread);
    rt = rr_bake(t, hc, &nb);
    rr_nb_free(&nb);
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

/* [F9b] Move a tile's meshes, spheres and lanterns by (dx, dz) world units (render-float frame; mesh
 * origins are 24.8 floats). Main thread, between frames. */
static void rr_tile_shift(RRTile *rt, int32_t dx, int32_t dz)
{
    const double fx = (double)dx, fz = (double)dz;
    int k;
    uint32_t i;
    if (!dx && !dz) return;
    for (k = 0; k < rt->nmesh; k++) {
        TD5_MeshHeader *m = rt->mesh[k];
        m->origin_x = (float)((double)m->origin_x + fx * 256.0);
        m->origin_z = (float)((double)m->origin_z + fz * 256.0);
        m->bounding_center_x = (float)((double)m->bounding_center_x + fx);
        m->bounding_center_z = (float)((double)m->bounding_center_z + fz);
        rt->sph[k][0] = (float)((double)rt->sph[k][0] + fx);
        rt->sph[k][2] = (float)((double)rt->sph[k][2] + fz);
    }
    for (i = 0; i < rt->nscn; i++) {
        TD5_MeshHeader *m = &rt->smesh[i];
        m->origin_x = (float)((double)m->origin_x + fx * 256.0);
        m->origin_z = (float)((double)m->origin_z + fz * 256.0);
        m->bounding_center_x = (float)((double)m->bounding_center_x + fx);
        m->bounding_center_z = (float)((double)m->bounding_center_z + fz);
    }
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) {
        rt->ssph[k][0] = (float)((double)rt->ssph[k][0] + fx);
        rt->ssph[k][2] = (float)((double)rt->ssph[k][2] + fz);
    }
    for (i = 0; i < rt->nlamp; i++) {
        rt->lamp[i * 3 + 0] = (float)((double)rt->lamp[i * 3 + 0] + fx);
        rt->lamp[i * 3 + 2] = (float)((double)rt->lamp[i * 3 + 2] + fz);
    }
    rt->cx = (float)((double)rt->cx + fx);
    rt->cz = (float)((double)rt->cz + fz);
    rt->sdx += dx; rt->sdz += dz;
}

/* [F9b] The world origin moved: every stored world position + (sx, sz). Resident tiles move now, a
 * tile baked later is moved when it is installed (rr_insert). */
void td5_roam_render_world_shift(int32_t sx, int32_t sz)
{
    int i;
    if (!s_rr.began) return;
    s_rr.draw_dx += sx; s_rr.draw_dz += sz;
    s_rr.rebases++;
    for (i = 0; i < s_rr.ntile; i++) rr_tile_shift(s_rr.tile[i], sx, sz);
}

static int rr_insert(RRTile *rt)
{
    rr_tile_shift(rt, s_rr.draw_dx - rt->sdx, s_rr.draw_dz - rt->sdz);   /* [F9b] baked in the session frame */
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
    rt = rr_bake_full(t, s_rr.hc_ok ? s_rr.hc : NULL, s_rr.scn, 1);
    if (!rt) { td5_roam_render_remove_tile(t->tx, t->tz); return 0; }
    if (!rr_insert(rt)) { rr_free_tile(rt); return 0; }
    TD5_LOG_D(LOG_TAG, "ROAM_RENDER_TILE %d,%d meshes=%d road_quads=%u patch_tris=%u ground_quads=%u skirts=%u "
              "bytes=%.2f MB bake_ms=%u cls(smooth/cobble/loose)=%u/%u/%u", rt->tx, rt->tz, rt->nmesh, rt->nroad, rt->ntri, rt->nground,
              rt->nskirt, (double)rt->bytes / 1048576.0, (unsigned)(td5_plat_time_ms() - t0), rt->ncls[0], rt->ncls[1], rt->ncls[2]);
    return 1;
}

/* ---- [F3] worker bake + main-thread install (td5_roam_stream.c) ---- */

TD5_RoamBaked *td5_roam_render_bake(const RoamTile *t, TD5_RoamHeight *hc)
{
    RRNb nb;
    RRTile *rt;
    if (!s_rr.began || !t) return NULL;
    rr_nb_get(&nb, t, NULL, 0);
    rt = rr_bake(t, hc, &nb);
    rr_nb_free(&nb);
    return (TD5_RoamBaked *)rt;
}

TD5_RoamBaked *td5_roam_render_bake_ex(const RoamTile *t, TD5_RoamHeight *hc, TD5_RoamScnSource *scn)
{
    if (!s_rr.began || !t) return NULL;
    return (TD5_RoamBaked *)rr_bake_full(t, hc, scn, 0);
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

/* [F4c] The resident street lanterns within r (render-frame float units) of (cx, cz), up to cap, for the
 * light registry. Main thread only (the tile table is the main thread's). */
int td5_roam_render_lamps(float cx, float cz, float r, float (*out)[3], int cap)
{
    const float r2 = r * r;
    int i, n = 0;
    uint32_t k;
    for (i = 0; i < s_rr.ntile && n < cap; i++) {
        const RRTile *rt = s_rr.tile[i];
        if (!rt->nlamp) continue;
        for (k = 0; k < rt->nlamp && n < cap; k++) {
            const float dx = rt->lamp[k * 3 + 0] - cx, dz = rt->lamp[k * 3 + 2] - cz;
            if (dx * dx + dz * dz > r2) continue;
            out[n][0] = rt->lamp[k * 3 + 0]; out[n][1] = rt->lamp[k * 3 + 1]; out[n][2] = rt->lamp[k * 3 + 2];
            n++;
        }
    }
    return n;
}

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
        /* [roam LC] cobble / loose road pages of a geo host (TD5RE_ROAM_SURF_PAGES=0: one page for every road) */
        s_rr.page_cls[0] = s_rr.page_road; s_rr.page_cls[1] = s_rr.page_road; s_rr.page_cls[2] = s_rr.page_road;
        s_rr.surf_pages = 0;
#ifdef RR_HAVE_MODE
        if (td5_roam_active() && td5_roam_host_is_geo() && td5_env_int("TD5RE_ROAM_SURF_PAGES", 1, 0, 1)) {
            td5_trackgen_roam_surface_pages(&s_rr.page_cls[1], &s_rr.page_cls[2]);
            s_rr.surf_pages = 1;
        }
#endif
    }
    s_rr.use_ground  = td5_env_int("TD5RE_ROAM_RENDER_GROUND", 1, 0, 1);
    s_rr.cell_m      = td5_env_int("TD5RE_ROAM_RENDER_CELL_M", 16, 8, 64);
    if (64 % s_rr.cell_m != 0) s_rr.cell_m = 16;            /* must divide a sub-block */
    s_rr.lift        = td5_env_int("TD5RE_ROAM_RENDER_LIFT_CM", 0, -1000, 1000) * (RR_UPM / 100.0);
    s_rr.ground_drop = td5_env_int("TD5RE_ROAM_RENDER_GROUND_DROP_CM", 40, 0, 500) * (RR_UPM / 100.0);
    s_rr.ground_clamp = !td5_env_flag_off("TD5RE_ROAM_GROUND_NOFIX");
    s_rr.patch_uv    = td5_env_int("TD5RE_ROAM_PATCH_UV", 1, 0, 1);      /* [roam HC] 0 = the old patch mapping */
    s_rr.stoplines   = td5_env_int("TD5RE_ROAM_STOPLINES", 1, 0, 1);     /* [roam HC] 0 = no stop lines         */
    s_rr.kerb_line   = td5_env_int("TD5RE_ROAM_KERB_LINE", 0, 0, 1);     /* [roam HC] 1 = lane 0's kerb stripe  */
    s_rr.radius      = td5_env_int("TD5RE_ROAM_RENDER_R", 500, 50, 5000) * RR_UPM;
    {   /* [roam LE] the water plane: on when the host has a water page (or the knob names one) */
        int hw = -1;
#ifdef RR_HAVE_MODE
        if (td5_roam_active()) hw = td5_roam_host_water_page();
#endif
        s_rr.page_water  = td5_env_int("TD5RE_ROAM_RENDER_PAGE_WATER", hw, -1, 1023);
        s_rr.water       = td5_env_int("TD5RE_ROAM_WATER", 1, 0, 1) && s_rr.page_water >= 0;
        s_rr.water_below = td5_env_int("TD5RE_ROAM_WATER_BELOW_CM", RR_WATER_BELOW_CM, 0, 300) * (RR_UPM / 100.0);
        s_rr.shore       = td5_env_int("TD5RE_ROAM_SHORE", 1, 0, 1);                          /* [roam PC] */
    }
#ifndef TD5RE_RELEASE
    s_rr.audit       = td5_env_int("TD5RE_ROAM_CULL_AUDIT", 0, 0, 1);   /* [roam IA] */
#endif

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
    /* [F4c] the street-name plate atlas + its GPU pages, BEFORE any worker bakes a tile (main thread) */
    if (!s_rr.f.route && td5_roam_scn_enabled()) td5_roam_furn_session_begin(place, &s_rr.grid);
    if (!s_rr.f.route && td5_roam_scn_enabled()) td5_roam_shops_session_begin(place);   /* [roam PE] shop-name bank */
    s_rr.began = 1;
    TD5_LOG_I(LOG_TAG, "roam render: begin place=%s frame origin=(%d,%d) dy=%.0f route=%d pages road=%d ground=%d "
              "kerb=%d ground=%s cell=%d m drop=%.0f units clamp=%d lift=%.0f R=%.0f m water=%d page=%d below=%.0f units",
              place, (int)s_rr.f.ox, (int)s_rr.f.oz, s_rr.f.dy, s_rr.f.route, s_rr.page_road, s_rr.page_ground,
              s_rr.page_kerb, (s_rr.use_ground && s_rr.hc_ok) ? "on" : "off", s_rr.cell_m,
              s_rr.ground_drop, s_rr.ground_clamp, s_rr.lift, s_rr.radius / RR_UPM, s_rr.water, s_rr.page_water,
              s_rr.water_below);
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
    td5_roam_furn_session_end();
    free(s_rr.tried);
    memset(&s_rr, 0, sizeof s_rr);
}

int td5_roam_render_view_stats(int view, TD5_RoamRenderViewStats *o)
{
    if (!o) return 0;
    memset(o, 0, sizeof *o);
    if (view < 0 || view >= TD5_ROAM_RENDER_VIEWS || !s_rr.v_frames[view]) return 0;
    o->tiles = s_rr.v_tiles[view]; o->meshes = s_rr.v_meshes[view];
    o->frames = s_rr.v_frames[view];
    o->avg_meshes = s_rr.v_sum_meshes[view] / (double)s_rr.v_frames[view];
    o->avg_ms = s_rr.v_sum_ms[view] / (double)s_rr.v_frames[view];
    return 1;
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
    *px = ex - (double)s_rr.draw_dx + (double)s_rr.f.ox; *pz = ez - (double)s_rr.draw_dz + (double)s_rr.f.oz;
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
 *   name:lat,lon,h_above_ground_m,yaw_deg,pitch_deg      (yaw 0 = north, as seen on the map;
 *   [roam M] yaw is a compass bearing, clockwise: both game frames are reflected (td5_geo_frame.h),
 *   so a clockwise bearing is a NEGATIVE engine yaw)
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
            double spx = td5_geo_game_x((slon - lon0) * mdlon * 430.0), spz = (slat - lat0) * mdlat * 430.0;
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
            /* [integ-roamI] the session's corner (td5_roam_mode.c, roam M): the reflected lattice makes the
             * world origin the (0,0) corner of tile tx + 1. Corner (stx, stz) put every pose one tile
             * diagonal (~256 m) off and its height on the wrong ground. */
            roam_grid_origin(&grd, stx + 1, stz, &rf_ox, &rf_oz);
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
        px = td5_geo_game_x((lon - lon0) * mdlon * 430.0);     /* [roam M] game frame */
        pz = (lat - lat0) * mdlat * 430.0;
        if (!td5_roam_height_prefetch(hc, px - 2000.0, pz - 2000.0, px + 2000.0, pz + 2000.0) ||
            !td5_roam_height_sample(hc, px, pz, &g, NULL)) { fprintf(stderr, "roam poses: no height at %s\n", nm); continue; }
        if (rf) {   /* roam frame: world = place - origin, y = place height, yaw 0 = +Z (north) */
            fprintf(o, "%s:%.0f,%.0f,%.0f,%.3f,%.3f\n", nm, (px - (double)rf_ox), (g + hm * 430.0),
                    (pz - (double)rf_oz), -yaw, pit);
            cnt++;
            continue;
        }
        if (!td5_roam_view_frame(slug, px, pz, &xr, &zr) || !td5_roam_view_frame(slug, px, pz + 430.0, &xn, &zn)) continue;
        yr = -yaw * 3.14159265358979 / 180.0 + atan2(xn - xr, zn - zr);  /* map north -> route +Z-relative */
        fprintf(o, "%s:%.0f,%.0f,%.0f,%.3f,%.3f\n", nm, xr, (g + hm * 430.0 - datum), zr,
                yr * 180.0 / 3.14159265358979, pit);
        cnt++;
    }
    if (o) fclose(o);
    td5_roam_height_close(hc); free(hc); free(buf);
    fprintf(stderr, "roam poses: %d pose(s) -> %s (datum %.0f units)\n", cnt, outp, datum);
    return 1;
}

/* ---------------------------------------------------- DEV ground census (G4) ---- */
/* TD5RE_ROAM_GROUND_CENSUS=1: every casco ROAMTILE (TD5RE_ROAM_GROUND_CENSUS_DIR, default the
 * place's roam/ cache) gets its ground built the way rr_bake builds it (rr_ground_build: same
 * lattice, same height source, same holes), then every road primitive (each lane quad as the two
 * triangles the renderer fans from vertex 0, each junction patch triangle) is sampled on a
 * barycentric grid every TD5RE_ROAM_GROUND_CENSUS_STEP_CM (50). At each sample:
 *   d = ground as drawn (the cell's fan triangle, minus the drop) - road as drawn
 *   A = raster - road                 (the road sits below the raster: grade smoothing, cuts)
 *   B = cell interpolation - raster   (the 16 m cell misses the raster's shape)
 * so d = A + B - drop when the cell is not clamped. The ground is meant to sit `drop` under the
 * road (d = -drop); d near 0 z-fights, d > 0 covers the road. Per ground cell under the road the
 * worst d is kept. Output: <log>/roam_ground_census.txt (summary + 60 worst cells) and .csv
 * (cells with worst d > -drop/2). One GZCENSUS: line in the log. */
typedef struct { float d, a, b; double x, z, ry; uint8_t kind; } RRGzCell;   /* kind 0 ribbon, 1 patch, 2 deck */

static int rr_gz_cmp(const void *pa, const void *pb)
{
    const RRGzCell *x = *(const RRGzCell *const *)pa, *y = *(const RRGzCell *const *)pb;
    if (x->d != y->d) return x->d > y->d ? -1 : 1;
    if (x->x != y->x) return x->x < y->x ? -1 : 1;
    return x->z < y->z ? -1 : (x->z > y->z ? 1 : 0);
}

typedef struct {
    int       ntile, tx0, tz0, tw, th;
    int      *slot;                 /* (tz - tz0) * tw + (tx - tx0) -> tile index, -1 none */
    RoamTile *tile;
    RRGround *gr;
    RRGzCell **cell;                /* per tile G*G, d = -1e30 untouched */
    double  **cmin;                 /* [G5] per tile G*G, the lowest road height in the cell (1e300 none) */
    TD5_RoamHeight *hc;
    long      ns, nnog, hist[10];
    double    worst;
    /* [roam HC] ground beside the road: [0 patch, 1 ribbon][offset 1/2/4/8 m] */
    long      bs_n[2][4], bs_hist[2][4][5];
    double    bs_worst[2][4], bs_wx[2][4], bs_wz[2][4];
} RRGz;

static const double k_gz_thr_cm[10] = { -40, -30, -20, -10, -5, -2, 0, 10, 50, 100 };

/* ground as drawn at a place point: the owning tile's cell, fan (0,1,2) / (0,2,3); raw = [G5] the
 * unclamped lattice (RRGround.h0) */
static int rr_gz_ground_h(const RRGz *z, double x, double zz, double *out, int *ti, int *ci, int raw)
{
    double u, v, fu, fv, h00, h10, h11, h01;
    const double S = (double)s_rr.grid.tile;
    int tx, tz, k, G, i, j, W;
    const RRGround *g;
    roam_grid_uv(&s_rr.grid, x, zz, &u, &v);
    tx = (int)floor(u / S); tz = (int)floor(v / S);
    if (tx < z->tx0 || tz < z->tz0 || tx >= z->tx0 + z->tw || tz >= z->tz0 + z->th) return 0;
    k = z->slot[(tz - z->tz0) * z->tw + (tx - z->tx0)];
    if (k < 0) return 0;
    g = &z->gr[k];
    if (!g->h || (raw && !g->h0)) return 0;
    G = g->G; W = G + 1;
    fu = (u / S - (double)tx) * (double)G; fv = (v / S - (double)tz) * (double)G;
    i = (int)floor(fu); j = (int)floor(fv);
    if (i < 0) i = 0;
    if (i >= G) i = G - 1;
    if (j < 0) j = 0;
    if (j >= G) j = G - 1;
    if (!rr_ground_cell_ok(g, i, j)) return 0;
    fu -= (double)i; fv -= (double)j;
    {
        const double *hh = raw ? g->h0 : g->h;
        h00 = hh[j * W + i]; h10 = hh[j * W + i + 1]; h11 = hh[(j + 1) * W + i + 1]; h01 = hh[(j + 1) * W + i];
    }
    if (fu >= fv) *out = h00 + fu * (h10 - h00) + fv * (h11 - h10);
    else          *out = h00 + fv * (h01 - h00) + fu * (h11 - h01);
    *out -= s_rr.ground_drop;
    *ti = k; *ci = j * G + i;
    return 1;
}

static int rr_gz_ground(const RRGz *z, double x, double zz, double *out, int *ti, int *ci)
{
    return rr_gz_ground_h(z, x, zz, out, ti, ci, 0);
}

/* [G5] the ground beside the road: every KERB boundary edge (bedge; deck sides stand over the ground on
 * purpose) sampled every `step`. At each sample, with y the road edge, g the drawn ground there, r the
 * drawn ground WITHOUT the clamp:
 *   hang = y - drop - g                  how far the ground hangs under its nominal place (y - drop)
 *   over = min(r, y - drop) - g          the part of it the clamp added beyond what keeping the ground
 *                                        under the road needs (0 = the clamp lowered just enough)
 * A road on a fill hangs on its own (r < y - drop) and is not `over`. A sample whose 3x3 ground cells hold a
 * road more than 2 m under it (an underpass, a ramp over a street) is booked apart: there the ground has to
 * go under the lower road and a 16 m lattice cannot follow both. */
typedef struct { float over, hang, lvl; double x, z, y; } RRGzEdge;   /* lvl: edge - lowest road of the 3x3 cells */

static const double k_gz_edge_cm[8] = { 2, 5, 10, 25, 50, 100, 200, 400 };

static int rr_gz_edge_cmp(const void *pa, const void *pb)
{
    const RRGzEdge *x = (const RRGzEdge *)pa, *y = (const RRGzEdge *)pb;
    if (x->over != y->over) return x->over > y->over ? -1 : 1;
    if (x->x != y->x) return x->x < y->x ? -1 : 1;
    return x->z < y->z ? -1 : (x->z > y->z ? 1 : 0);
}

static void rr_gz_tri(RRGz *z, const double p[3][3], double step, uint8_t kind)
{
    double L = 0.0;
    int a, b, n, e;
    for (e = 0; e < 3; e++) {
        const double *p0 = p[e], *p1 = p[(e + 1) % 3];
        const double l = hypot(p1[0] - p0[0], p1[2] - p0[2]);
        if (l > L) L = l;
    }
    n = (int)ceil(L / step);
    if (n < 1) n = 1;
    if (n > 4000) n = 4000;
    for (a = 0; a <= n; a++) for (b = 0; a + b <= n; b++) {
        const double wa = (double)a / n, wb = (double)b / n, wc = 1.0 - wa - wb;
        const double x  = wa * p[0][0] + wb * p[1][0] + wc * p[2][0];
        const double y  = wa * p[0][1] + wb * p[1][1] + wc * p[2][1];
        const double zz = wa * p[0][2] + wb * p[1][2] + wc * p[2][2];
        double g, r = NAN, d;
        int ti, ci, h;
        RRGzCell *c;
        if (!rr_gz_ground(z, x, zz, &g, &ti, &ci)) { z->nnog++; continue; }
        z->ns++;
        d = g - y;
        if (!td5_roam_height_sample(z->hc, x, zz, &r, NULL)) r = NAN;
        for (h = 0; h < 10; h++) if (d > k_gz_thr_cm[h] * (RR_UPM / 100.0)) z->hist[h]++;
        if (d > z->worst) z->worst = d;
        c = &z->cell[ti][ci];
        if (d > c->d) {
            c->d = (float)d;
            c->a = isnan(r) ? 0.0f : (float)(r - y);
            c->b = isnan(r) ? 0.0f : (float)(g + s_rr.ground_drop - r);
            c->x = x; c->z = zz; c->ry = y; c->kind = kind;
        }
    }
}

static int rr_gz_ground_cb(void *u, double x, double zz, double *out)
{
    int ti, ci;
    return rr_gz_ground((const RRGz *)u, x, zz, out, &ti, &ci);
}

int td5_roam_render_ground_census_dev_test(void)
{
    static const char place[] = "la_plata_partido";
    const char *dir = getenv("TD5RE_ROAM_GROUND_CENSUS_DIR");
    char path[600], wpath[512], err[160], gtxt[4096];
    int (*list)[2];
    int nlist, i, k, G, tx1 = -100000, tz1 = -100000;
    RRGz z;
    TD5_File *f;
    FILE *o, *csv;
    double step, lat0 = 0, lon0 = 0, mdlat = 0, mdlon = 0;
    const char *ld;
    long cells = 0, cell_hist[10] = { 0 }, cause_a = 0, cause_b = 0, cause_kind[4] = { 0 };
    long nclamp = 0, seam_pts = 0, seam_bad = 0, seam_badf = 0;
    double max_low = 0.0, seam_max = 0.0;
    RRGzCell **all = NULL;
    uint32_t t0;
    /* [G5] the road-edge pass */
    RRGzEdge *ed_keep = NULL;
    long ed_n = 0, ed_nog = 0, ed_over[8] = { 0 }, ed_hang[8] = { 0 }, ed_nkeep = 0, ed_cap = 0;
    double ed_worst_over = -1e30, ed_worst_hang = -1e30, ed_min_over = 1e30, ed_sum_over = 0.0;
    long ed_nc[2] = { 0 }, ed_overc[2][8] = { { 0 } };
    double ed_sumc[2] = { 0.0, 0.0 }, ed_worstc[2] = { -1e30, -1e30 };

    if (!td5_env_flag_off("TD5RE_ROAM_GROUND_CENSUS")) return 0;
    t0 = td5_plat_time_ms();
    memset(&z, 0, sizeof z);
    z.worst = -1e30;
    for (i = 0; i < 8; i++) z.bs_worst[i / 4][i % 4] = -1e30;
    memset(&s_rr, 0, sizeof s_rr);
    snprintf(s_rr.place, sizeof s_rr.place, "%s", place);
    /* the lattice: the tile dir's ROAMGRID.JSON, else the place's, else the default (as begin) */
    if (dir && dir[0]) snprintf(path, sizeof path, "%s/ROAMGRID.JSON", dir);
    else td5_geo_source_path(path, sizeof path, place, "roam/ROAMGRID.JSON");
    f = td5_plat_file_open(path, "rb");
    if (!f && dir && dir[0]) { td5_geo_source_path(path, sizeof path, place, "roam/ROAMGRID.JSON"); f = td5_plat_file_open(path, "rb"); }
    if (f) {
        const int64_t n = td5_plat_file_size(f);
        if (n > 0 && n < (int64_t)sizeof gtxt && td5_plat_file_read(f, gtxt, (size_t)n) == (size_t)n) {
            gtxt[n] = 0; s_rr.grid_ok = roam_grid_parse(gtxt, &s_rr.grid);
        }
        td5_plat_file_close(f);
    }
    if (!s_rr.grid_ok) roam_grid_default(place, &s_rr.grid);
    /* the knobs exactly as td5_roam_render_begin reads them */
    s_rr.use_ground  = 1;
    s_rr.cell_m      = td5_env_int("TD5RE_ROAM_RENDER_CELL_M", 16, 8, 64);
    if (64 % s_rr.cell_m != 0) s_rr.cell_m = 16;
    s_rr.ground_drop = td5_env_int("TD5RE_ROAM_RENDER_GROUND_DROP_CM", 40, 0, 500) * (RR_UPM / 100.0);
    s_rr.ground_clamp = !td5_env_flag_off("TD5RE_ROAM_GROUND_NOFIX");
    s_rr.keep_raw = 1;
    step = td5_env_int("TD5RE_ROAM_GROUND_CENSUS_STEP_CM", 50, 5, 500) * (RR_UPM / 100.0);
    z.hc = (TD5_RoamHeight *)calloc(1, sizeof *z.hc);
    td5_geo_source_path(path, sizeof path, place, "HEIGHT.R16");
    td5_geo_source_path(wpath, sizeof wpath, place, "WATER.R8");
    if (!z.hc || !td5_roam_height_open(z.hc, path, wpath,
            (double)td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400) / 100.0)) {
        TD5_LOG_E(LOG_TAG, "GZCENSUS: no height source at %s", path); return 1;
    }
    td5_geo_source_path(path, sizeof path, place, "PLACE.JSON");
    if ((f = td5_plat_file_open(path, "rb")) != NULL) {   /* projection, for the lat/lon of a spot */
        const int64_t pn = td5_plat_file_size(f);
        char *js = (char *)malloc((size_t)pn + 1u);
        if (js && td5_plat_file_read(f, js, (size_t)pn) == (size_t)pn) {
            const char *kk, *a;
            js[pn] = 0;
            if ((kk = strstr(js, "\"projection\"")) != NULL) {
                if ((a = strstr(kk, "\"lat0\"")))          lat0  = atof(strchr(a, ':') + 1);
                if ((a = strstr(kk, "\"lon0\"")))          lon0  = atof(strchr(a, ':') + 1);
                if ((a = strstr(kk, "\"m_per_deg_lat\""))) mdlat = atof(strchr(a, ':') + 1);
                if ((a = strstr(kk, "\"m_per_deg_lon\""))) mdlon = atof(strchr(a, ':') + 1);
            }
        }
        free(js);
        td5_plat_file_close(f);
    }

    list = (int (*)[2])malloc(2048u * sizeof *list);
    z.tile = (RoamTile *)calloc(2048u, sizeof *z.tile);
    z.gr = (RRGround *)calloc(2048u, sizeof *z.gr);
    if (!list || !z.tile || !z.gr) return 1;
    nlist = td5_roam_casco_tiles(&s_rr.grid, list, 2048);
    z.tx0 = z.tz0 = 100000;
    for (i = 0; i < nlist; i++) {
        td5_roam_tile_path(path, sizeof path, dir, place, list[i][0], list[i][1]);
        if (!td5_roam_tile_read(path, &z.tile[z.ntile], 0, err, sizeof err)) continue;
        if (list[i][0] < z.tx0) z.tx0 = list[i][0];
        if (list[i][1] < z.tz0) z.tz0 = list[i][1];
        if (list[i][0] > tx1) tx1 = list[i][0];
        if (list[i][1] > tz1) tz1 = list[i][1];
        z.ntile++;
    }
    if (z.ntile == 0) { TD5_LOG_E(LOG_TAG, "GZCENSUS: no ROAMTILE read of %d (run --RoamBuildTiles=casco first)", nlist); return 1; }
    z.tw = tx1 - z.tx0 + 1; z.th = tz1 - z.tz0 + 1;
    z.slot = (int *)malloc((size_t)z.tw * (size_t)z.th * sizeof(int));
    z.cell = (RRGzCell **)calloc((size_t)z.ntile, sizeof *z.cell);
    z.cmin = (double **)calloc((size_t)z.ntile, sizeof *z.cmin);
    if (!z.slot || !z.cell || !z.cmin) return 1;
    for (i = 0; i < z.tw * z.th; i++) z.slot[i] = -1;
    G = 256 / s_rr.cell_m;
    for (k = 0; k < z.ntile; k++) {
        const RoamTile *t = &z.tile[k];
        z.slot[(t->tz - z.tz0) * z.tw + (t->tx - z.tx0)] = k;
    }
    for (k = 0; k < z.ntile; k++) {
        const RoamTile *t = &z.tile[k];
        RRNb nb;
        int dx, dz;
        memset(&nb, 0, sizeof nb);                     /* the 3x3 from the loaded set (the bake reads the same files) */
        for (dz = -1; dz <= 1; dz++) for (dx = -1; dx <= 1; dx++) {
            const int ax = t->tx + dx, az = t->tz + dz;
            if (ax >= z.tx0 && az >= z.tz0 && ax < z.tx0 + z.tw && az < z.tz0 + z.th &&
                z.slot[(az - z.tz0) * z.tw + (ax - z.tx0)] >= 0)
                nb.t[(dz + 1) * 3 + (dx + 1)] = &z.tile[z.slot[(az - z.tz0) * z.tw + (ax - z.tx0)]];
        }
        rr_ground_build(t, &z.gr[k], z.hc, &nb);
        nclamp += z.gr[k].nclamp;
        if (z.gr[k].max_low > max_low) max_low = z.gr[k].max_low;
        z.cell[k] = (RRGzCell *)malloc((size_t)G * (size_t)G * sizeof(RRGzCell));
        if (!z.cell[k]) return 1;
        for (i = 0; i < G * G; i++) { memset(&z.cell[k][i], 0, sizeof(RRGzCell)); z.cell[k][i].d = -1e30f; }
        if ((z.cmin[k] = (double *)malloc((size_t)G * (size_t)G * sizeof(double))) != NULL)
            td5_roam_gfix_cells(&s_rr.grid, nb.t, t->tx * G, t->tz * G, G, (double)s_rr.grid.tile / (double)G, z.cmin[k]);
    }
    /* [G4b] TD5RE_ROAM_PAVE_CENSUS=1: the pavement of every tile against this drawn ground, then done */
    if (td5_env_flag_off("TD5RE_ROAM_PAVE_CENSUS")) {
        TD5_RoamScnSource *scn = td5_roam_scn_open(place, &s_rr.grid, NULL, NULL);
        const double proj[4] = { lat0, mdlat, lon0, mdlon };
        td5_roam_pave_census_set_grid(&s_rr.grid);            /* [roam IA] + the median census */
        td5_roam_pave_census(z.tile, z.ntile, scn, z.hc, rr_gz_ground_cb, &z, s_rr.ground_drop, proj, NULL);
        if (scn) td5_roam_scn_close(scn);
        for (k = 0; k < z.ntile; k++) { rr_ground_free(&z.gr[k]); free(z.cell[k]); td5_roam_tile_free(&z.tile[k]); }
        for (k = 0; k < z.ntile; k++) free(z.cmin[k]);
        free(z.cmin); free(z.cell); free(z.slot); free(z.gr); free(z.tile); free(list);
        td5_roam_height_close(z.hc); free(z.hc);
        memset(&s_rr, 0, sizeof s_rr);
        return 1;
    }
    /* the road, tile by tile (raster window over the tile + halo for the attribution) */
    for (k = 0; k < z.ntile; k++) {
        const RoamTile *t = &z.tile[k];
        uint8_t *deck = (uint8_t *)calloc(t->nspan ? t->nspan : 1u, 1);
        double cr[4][2], x0 = 1e18, z0 = 1e18, x1 = -1e18, z1 = -1e18;
        uint32_t e, q, s;
        roam_grid_corners(&s_rr.grid, t->tx, t->tz, cr);
        for (i = 0; i < 4; i++) {
            if (cr[i][0] < x0) x0 = cr[i][0];
            if (cr[i][0] > x1) x1 = cr[i][0];
            if (cr[i][1] < z0) z0 = cr[i][1];
            if (cr[i][1] > z1) z1 = cr[i][1];
        }
        td5_roam_height_prefetch(z.hc, x0 - 20000.0, z0 - 20000.0, x1 + 20000.0, z1 + 20000.0);
        for (e = 0; deck && e < t->nedge; e++)
            if (t->edge[e].flags & ROAM_EF_DECK)
                for (q = 0; q < t->edge[e].span_count; q++)
                    if ((uint32_t)t->edge[e].span_first + q < t->nspan) deck[t->edge[e].span_first + q] = 1;
        for (s = 0; s < t->nspan; s++) {
            const TD5_StripSpan *sp = &t->span[s];
            uint32_t L, R; int n, kk;
            if (!rr_span_ok(t, sp, &L, &R, &n)) continue;
            for (kk = 0; kk < n; kk++) {
                const TD5_StripVertex *v4[4] = { &t->vert[L + kk], &t->vert[L + kk + 1], &t->vert[R + kk + 1], &t->vert[R + kk] };
                double p[4][3], tri[3][3];
                int c;
                for (c = 0; c < 4; c++) {
                    p[c][0] = (double)t->org_x + sp->origin_x + v4[c]->x;
                    p[c][1] = (double)sp->origin_y + v4[c]->y;
                    p[c][2] = (double)t->org_z + sp->origin_z + v4[c]->z;
                }
                memcpy(tri[0], p[0], sizeof tri[0]); memcpy(tri[1], p[1], sizeof tri[1]); memcpy(tri[2], p[2], sizeof tri[2]);
                rr_gz_tri(&z, (const double (*)[3])tri, step, (uint8_t)(deck && deck[s] ? 2 : 0));
                memcpy(tri[1], p[2], sizeof tri[1]); memcpy(tri[2], p[3], sizeof tri[2]);
                rr_gz_tri(&z, (const double (*)[3])tri, step, (uint8_t)(deck && deck[s] ? 2 : 0));
            }
        }
        for (s = 0; s < t->ntri; s++) {
            const RoamTri *tr = &t->tri[s];
            double tri[3][3];
            int c;
            if (tr->v[0] >= t->nvert || tr->v[1] >= t->nvert || tr->v[2] >= t->nvert) continue;
            for (c = 0; c < 3; c++) {
                tri[c][0] = (double)t->org_x + tr->ox + t->vert[tr->v[c]].x;
                tri[c][1] = (double)tr->oy + t->vert[tr->v[c]].y;
                tri[c][2] = (double)t->org_z + tr->oz + t->vert[tr->v[c]].z;
            }
            rr_gz_tri(&z, (const double (*)[3])tri, step, 1);
        }
        free(deck);
    }
    /* [G5] the ground beside the road (see RRGzEdge) */
    for (k = 0; k < z.ntile; k++) {
        const RoamTile *t = &z.tile[k];
        uint32_t s;
        for (s = 0; s < t->nbedge; s++) {
            const RoamBEdge *be = &t->bedge[s];
            int32_t ox = 0, oy = 0, oz = 0;
            double a[3], b[3], L;
            int n, q;
            if (be->kind != ROAM_BEDGE_KERB || !rr_bedge_prim(t, be, &ox, &oy, &oz)) continue;
            a[0] = (double)t->org_x + ox + t->vert[be->v[0]].x; a[1] = (double)oy + t->vert[be->v[0]].y; a[2] = (double)t->org_z + oz + t->vert[be->v[0]].z;
            b[0] = (double)t->org_x + ox + t->vert[be->v[1]].x; b[1] = (double)oy + t->vert[be->v[1]].y; b[2] = (double)t->org_z + oz + t->vert[be->v[1]].z;
            L = hypot(b[0] - a[0], b[2] - a[2]);
            n = (int)ceil(L / step);
            if (n < 1) n = 1;
            if (n > 4000) n = 4000;
            for (q = 0; q <= n; q++) {
                const double f = (double)q / n;
                const double x = a[0] + f * (b[0] - a[0]), y = a[1] + f * (b[1] - a[1]), zz = a[2] + f * (b[2] - a[2]);
                double g, r, bound, over, hang, lvlmin;
                int ti, ci, h, cls;
                if (!rr_gz_ground(&z, x, zz, &g, &ti, &ci) || !rr_gz_ground_h(&z, x, zz, &r, &ti, &ci, 1)) { ed_nog++; continue; }
                bound = (r < y - s_rr.ground_drop) ? r : y - s_rr.ground_drop;
                over = bound - g; hang = y - s_rr.ground_drop - g;
                {   /* the lowest road of the 3x3 cells round the sample (this tile's cells) */
                    const int ca = ci % G, cb = ci / G;
                    int da, db;
                    lvlmin = 1e300;
                    for (db = -1; db <= 1; db++) for (da = -1; da <= 1; da++) {
                        const int xa = ca + da, xb = cb + db;
                        if (z.cmin[ti] && xa >= 0 && xb >= 0 && xa < G && xb < G && z.cmin[ti][xb * G + xa] < lvlmin) lvlmin = z.cmin[ti][xb * G + xa];
                    }
                    cls = (lvlmin < y - 200.0 * (RR_UPM / 100.0)) ? 1 : 0;
                }
                ed_nc[cls]++;
                ed_sumc[cls] += over > 0.0 ? over : 0.0;
                if (over > ed_worstc[cls]) ed_worstc[cls] = over;
                for (h = 0; h < 8; h++) if (over > k_gz_edge_cm[h] * (RR_UPM / 100.0)) ed_overc[cls][h]++;
                ed_n++;
                ed_sum_over += over > 0.0 ? over : 0.0;
                if (over > ed_worst_over) ed_worst_over = over;
                if (hang > ed_worst_hang) ed_worst_hang = hang;
                if (over < ed_min_over) ed_min_over = over;
                for (h = 0; h < 8; h++) {
                    if (over > k_gz_edge_cm[h] * (RR_UPM / 100.0)) ed_over[h]++;
                    if (hang > k_gz_edge_cm[h] * (RR_UPM / 100.0)) ed_hang[h]++;
                }
                if (over > 25.0 * (RR_UPM / 100.0)) {
                    if (ed_nkeep == ed_cap) {
                        RRGzEdge *nk;
                        ed_cap = ed_cap ? ed_cap * 2 : 4096;
                        nk = (RRGzEdge *)realloc(ed_keep, (size_t)ed_cap * sizeof *ed_keep);
                        if (!nk) return 1;
                        ed_keep = nk;
                    }
                    ed_keep[ed_nkeep].over = (float)over; ed_keep[ed_nkeep].hang = (float)hang;
                    ed_keep[ed_nkeep].lvl = (float)(lvlmin < 1e299 ? y - lvlmin : 0.0);
                    ed_keep[ed_nkeep].x = x; ed_keep[ed_nkeep].z = zz; ed_keep[ed_nkeep].y = y;
                    ed_nkeep++;
                }
            }
        }
    }
    /* [roam HC] the ground BESIDE the road: drawn ground at 1 / 2 / 4 / 8 m outside every kerb edge minus the
     * road edge's height (rise > 0 = the grass stands over the road next to it and hides it from a low eye),
     * junction patch kerbs and ribbon kerbs apart. Outward = away from the edge's own primitive. */
    {
        static const double off_m[4] = { 1.0, 2.0, 4.0, 8.0 };
        static const double thr_cm[5] = { 0.0, 25.0, 50.0, 100.0, 200.0 };
        for (k = 0; k < z.ntile; k++) {
            const RoamTile *t = &z.tile[k];
            uint32_t s;
            for (s = 0; s < t->nbedge; s++) {
                const RoamBEdge *be = &t->bedge[s];
                const int cls = (be->prim & ROAM_PRIM_TRI) ? 0 : 1;
                int32_t ox = 0, oy = 0, oz = 0;
                double a[3], b[3], in[2], nx, nz, l, L;
                int n, q, o2, c;
                if (be->kind != ROAM_BEDGE_KERB || !rr_bedge_prim(t, be, &ox, &oy, &oz)) continue;
                if (cls == 0) {
                    const RoamTri *tr = &t->tri[be->prim & ~ROAM_PRIM_TRI];
                    in[0] = in[1] = 0.0;
                    for (c = 0; c < 3; c++) { in[0] += t->vert[tr->v[c]].x; in[1] += t->vert[tr->v[c]].z; }
                    in[0] = (double)t->org_x + ox + in[0] / 3.0; in[1] = (double)t->org_z + oz + in[1] / 3.0;
                } else {
                    uint32_t Lv, Rv; int ln;
                    if (!rr_span_ok(t, &t->span[be->prim], &Lv, &Rv, &ln)) continue;
                    in[0] = (double)t->org_x + ox + 0.25 * ((double)t->vert[Lv].x + t->vert[Lv + ln].x + t->vert[Rv].x + t->vert[Rv + ln].x);
                    in[1] = (double)t->org_z + oz + 0.25 * ((double)t->vert[Lv].z + t->vert[Lv + ln].z + t->vert[Rv].z + t->vert[Rv + ln].z);
                }
                a[0] = (double)t->org_x + ox + t->vert[be->v[0]].x; a[1] = (double)oy + t->vert[be->v[0]].y; a[2] = (double)t->org_z + oz + t->vert[be->v[0]].z;
                b[0] = (double)t->org_x + ox + t->vert[be->v[1]].x; b[1] = (double)oy + t->vert[be->v[1]].y; b[2] = (double)t->org_z + oz + t->vert[be->v[1]].z;
                nx = b[2] - a[2]; nz = -(b[0] - a[0]); l = hypot(nx, nz);
                if (l < 1.0) continue;
                nx /= l; nz /= l;
                if ((0.5 * (a[0] + b[0]) - in[0]) * nx + (0.5 * (a[2] + b[2]) - in[1]) * nz < 0.0) { nx = -nx; nz = -nz; }
                L = l;
                n = (int)ceil(L / step);
                if (n < 1) n = 1;
                if (n > 4000) n = 4000;
                for (q = 0; q <= n; q++) {
                    const double f = (double)q / n;
                    const double x = a[0] + f * (b[0] - a[0]), y = a[1] + f * (b[1] - a[1]), zz = a[2] + f * (b[2] - a[2]);
                    for (o2 = 0; o2 < 4; o2++) {
                        const double sx = x + nx * off_m[o2] * RR_UPM, sz = zz + nz * off_m[o2] * RR_UPM;
                        double g, rise;
                        int ti, ci, h;
                        if (!rr_gz_ground(&z, sx, sz, &g, &ti, &ci)) continue;
                        rise = g - y;
                        z.bs_n[cls][o2]++;
                        if (rise > z.bs_worst[cls][o2]) {
                            z.bs_worst[cls][o2] = rise;
                            z.bs_wx[cls][o2] = sx; z.bs_wz[cls][o2] = sz;
                        }
                        for (h = 0; h < 5; h++) if (rise > thr_cm[h] * (RR_UPM / 100.0)) z.bs_hist[cls][o2][h]++;
                    }
                }
            }
        }
    }
    if (ed_nkeep > 1) qsort(ed_keep, (size_t)ed_nkeep, sizeof *ed_keep, rr_gz_edge_cmp);
    /* seams: every border lattice point must carry the same height in both tiles (bit for bit) */
    for (k = 0; k < z.ntile; k++) {
        const RoamTile *t = &z.tile[k];
        const int W = G + 1;
        int side;
        for (side = 0; side < 2; side++) {
            const int ax = t->tx + (side == 0), az = t->tz + (side == 1);
            int n2;
            if (ax >= z.tx0 + z.tw || az >= z.tz0 + z.th) continue;
            n2 = z.slot[(az - z.tz0) * z.tw + (ax - z.tx0)];
            if (n2 < 0 || !z.gr[k].h || !z.gr[n2].h) continue;
            for (i = 0; i <= G; i++) {
                const double a = side == 0 ? z.gr[k].h[i * W + G] : z.gr[k].h[G * W + i];
                const double b = side == 0 ? z.gr[n2].h[i * W + 0] : z.gr[n2].h[0 * W + i];
                seam_pts++;
                if (!(a == b || (isnan(a) && isnan(b)))) {
                    seam_bad++;
                    if (!isnan(a) && !isnan(b) && fabs(a - b) > seam_max) seam_max = fabs(a - b);
                    if ((float)(a - s_rr.ground_drop) != (float)(b - s_rr.ground_drop)) seam_badf++;   /* as the vertex holds it */
                }
            }
        }
    }
    /* per cell */
    all = (RRGzCell **)malloc((size_t)z.ntile * (size_t)G * (size_t)G * sizeof *all);
    if (!all) return 1;
    for (k = 0; k < z.ntile; k++) for (i = 0; i < G * G; i++) {
        RRGzCell *c = &z.cell[k][i];
        int h;
        if (c->d < -1e29f) continue;
        all[cells++] = c;
        for (h = 0; h < 10; h++) if (c->d > k_gz_thr_cm[h] * (RR_UPM / 100.0)) cell_hist[h]++;
        if (c->d > -0.5 * s_rr.ground_drop) {
            if (c->a >= c->b) cause_a++; else cause_b++;
            cause_kind[c->kind & 3]++;
        }
    }
    qsort(all, (size_t)cells, sizeof *all, rr_gz_cmp);
    ld = td5_plat_log_dir();
    snprintf(path, sizeof path, "%sroam_ground_census.txt", ld ? ld : "");
    o = fopen(path, "w");
    snprintf(wpath, sizeof wpath, "%sroam_ground_census.csv", ld ? ld : "");
    csv = fopen(wpath, "w");
    if (o) {
        int h;
        fprintf(o, "roam ground census (G4): %d tiles of %d casco, cell %d m, drop %.1f cm, step %.1f cm, %ld samples "
                   "(%ld over no ground: hole / no tile), %ld ground cells under the road\n",
                z.ntile, nlist, s_rr.cell_m, s_rr.ground_drop / (RR_UPM / 100.0), step / (RR_UPM / 100.0), z.ns, z.nnog, cells);
        fprintf(o, "d = ground as drawn - road as drawn (cm). Intended: d = -drop. d near 0 z-fights, d > 0 covers the road.\n");
        fprintf(o, "clamp %s: %ld lattice points lowered under the road, deepest %.1f cm; seams: %ld shared border points, "
                   "%ld differ in double (largest %.3g units), %ld in the float vertex\n",
                s_rr.ground_clamp ? "ON" : "OFF (TD5RE_ROAM_GROUND_NOFIX=1)", nclamp,
                max_low / (RR_UPM / 100.0), seam_pts, seam_bad, seam_max, seam_badf);
        fprintf(o, "worst d %.1f cm\n", z.worst / (RR_UPM / 100.0));
        fprintf(o, "threshold_cm  samples_d_above  cells_worst_d_above\n");
        for (h = 0; h < 10; h++) fprintf(o, "%+5.0f  %10ld  %8ld\n", k_gz_thr_cm[h], z.hist[h], cell_hist[h]);
        fprintf(o, "cells with worst d > -drop/2: %ld; cause at the worst sample: road below raster (A >= B) %ld, cell "
                   "interpolation (B > A) %ld; on ribbon %ld, patch %ld, deck ribbon %ld\n",
                cause_a + cause_b, cause_a, cause_b, cause_kind[0], cause_kind[1], cause_kind[2]);
        fprintf(o, "\nROAD EDGE (G5): clamp %s, %ld kerb-edge samples (%ld over no ground); hang = road edge - drop - ground, "
                   "over = the part of the hang the clamp added beyond keeping the ground under the road\n",
                s_rr.ground_clamp ? (!td5_roam_gfix_g4() ? "per-triangle lowering (G5)" : "4-cell minimum (G4, TD5RE_ROAM_GROUND_G4=1)") : "OFF",
                ed_n, ed_nog);
        fprintf(o, "edge over: worst %.1f cm, mean %.2f cm, lowest %.2f cm; edge hang: worst %.1f cm\n",
                ed_worst_over / (RR_UPM / 100.0), ed_n ? ed_sum_over / ed_n / (RR_UPM / 100.0) : 0.0,
                ed_min_over / (RR_UPM / 100.0), ed_worst_hang / (RR_UPM / 100.0));
        fprintf(o, "edge over, ONE road level round the sample: %ld samples, worst %.1f cm, mean %.2f cm; another road > 2 m "
                   "under it in the 3x3 cells (underpass / ramp): %ld samples, worst %.1f cm, mean %.2f cm\n",
                ed_nc[0], ed_worstc[0] / (RR_UPM / 100.0), ed_nc[0] ? ed_sumc[0] / ed_nc[0] / (RR_UPM / 100.0) : 0.0,
                ed_nc[1], ed_worstc[1] / (RR_UPM / 100.0), ed_nc[1] ? ed_sumc[1] / ed_nc[1] / (RR_UPM / 100.0) : 0.0);
        fprintf(o, "edge_threshold_cm  samples_over_above  samples_hang_above  over_one_level  over_multi_level\n");
        for (h = 0; h < 8; h++)
            fprintf(o, "%5.0f  %10ld  %10ld  %10ld  %10ld\n", k_gz_edge_cm[h], ed_over[h], ed_hang[h], ed_overc[0][h], ed_overc[1][h]);
        {   /* [roam HC] */
            static const char *const cn[2] = { "junction patch", "ribbon" };
            static const double off_m[4] = { 1.0, 2.0, 4.0, 8.0 };
            int c2, o2;
            fprintf(o, "\nBESIDE THE ROAD (roam HC): verge %s; rise = ground as drawn at D m outside a kerb edge - the road "
                       "edge height (> 0: the grass stands over the road beside it)\n", td5_roam_gfix_verge() ? "ON" : "OFF");
            fprintf(o, "kerb_of          D_m   samples   rise>0   >25cm   >50cm  >100cm  >200cm  worst_cm  worst_lat worst_lon\n");
            for (c2 = 0; c2 < 2; c2++) for (o2 = 0; o2 < 4; o2++)
                fprintf(o, "%-15s %4.0f %9ld %8ld %7ld %7ld %7ld %7ld %9.1f  %.6f %.6f\n", cn[c2], off_m[o2], z.bs_n[c2][o2],
                        z.bs_hist[c2][o2][0], z.bs_hist[c2][o2][1], z.bs_hist[c2][o2][2], z.bs_hist[c2][o2][3],
                        z.bs_hist[c2][o2][4], z.bs_worst[c2][o2] / (RR_UPM / 100.0),
                        mdlat > 0 ? lat0 + z.bs_wz[c2][o2] / (mdlat * RR_UPM) : 0.0,
                        mdlon > 0 ? lon0 + td5_geo_game_x(z.bs_wx[c2][o2]) / (mdlon * RR_UPM) : 0.0);
            fprintf(o, "\n");
        }
        fprintf(o, "worst 40 edge samples: over_cm hang_cm level_gap_cm place_x place_z road_y lat lon\n");
        for (i = 0; i < ed_nkeep && i < 40; i++) {
            const RRGzEdge *c = &ed_keep[i];
            fprintf(o, "%7.1f %7.1f %7.1f %.0f %.0f %.0f %.6f %.6f\n", c->over / (RR_UPM / 100.0), c->hang / (RR_UPM / 100.0),
                    c->lvl / (RR_UPM / 100.0), c->x, c->z, c->y,
                    mdlat > 0 ? lat0 + c->z / (mdlat * RR_UPM) : 0.0, mdlon > 0 ? lon0 + td5_geo_game_x(c->x) / (mdlon * RR_UPM) : 0.0);
        }
        {   /* every kept edge sample (over > 25 cm) */
            FILE *ec;
            snprintf(wpath, sizeof wpath, "%sroam_ground_edge.csv", ld ? ld : "");
            if ((ec = fopen(wpath, "w")) != NULL) {
                fprintf(ec, "over_cm,hang_cm,level_gap_cm,place_x,place_z,road_y,lat,lon\n");
                for (i = 0; i < ed_nkeep; i++) {
                    const RRGzEdge *c = &ed_keep[i];
                    fprintf(ec, "%.1f,%.1f,%.1f,%.0f,%.0f,%.0f,%.6f,%.6f\n", c->over / (RR_UPM / 100.0), c->hang / (RR_UPM / 100.0),
                            c->lvl / (RR_UPM / 100.0), c->x, c->z, c->y,
                            mdlat > 0 ? lat0 + c->z / (mdlat * RR_UPM) : 0.0, mdlon > 0 ? lon0 + td5_geo_game_x(c->x) / (mdlon * RR_UPM) : 0.0);
                }
                fclose(ec);
            }
        }
        fprintf(o, "\nworst 60 cells: d_cm A_cm(raster-road) B_cm(cell-raster) kind place_x place_z road_y lat lon\n");
        for (i = 0; i < cells && i < 60; i++) {
            const RRGzCell *c = all[i];
            fprintf(o, "%7.1f %7.1f %7.1f %s %.0f %.0f %.0f %.6f %.6f\n", c->d / (RR_UPM / 100.0),
                    c->a / (RR_UPM / 100.0), c->b / (RR_UPM / 100.0),
                    c->kind == 1 ? "patch" : (c->kind & 2) ? "deck" : "ribbon", c->x, c->z, c->ry,
                    mdlat > 0 ? lat0 + c->z / (mdlat * RR_UPM) : 0.0, mdlon > 0 ? lon0 + td5_geo_game_x(c->x) / (mdlon * RR_UPM) : 0.0);
        }
        fclose(o);
    }
    if (csv) {
        fprintf(csv, "d_cm,a_cm,b_cm,kind,place_x,place_z,road_y,lat,lon\n");
        for (i = 0; i < cells && all[i]->d > -0.5 * s_rr.ground_drop; i++) {
            const RRGzCell *c = all[i];
            fprintf(csv, "%.1f,%.1f,%.1f,%d,%.0f,%.0f,%.0f,%.6f,%.6f\n", c->d / (RR_UPM / 100.0),
                    c->a / (RR_UPM / 100.0), c->b / (RR_UPM / 100.0), c->kind, c->x, c->z, c->ry,
                    mdlat > 0 ? lat0 + c->z / (mdlat * RR_UPM) : 0.0, mdlon > 0 ? lon0 + td5_geo_game_x(c->x) / (mdlon * RR_UPM) : 0.0);
        }
        fclose(csv);
    }
    TD5_LOG_I(LOG_TAG, "GZCENSUS: clamp=%d lowered=%ld seam_bad=%ld tiles=%d cells=%ld samples=%ld worst_cm=%.1f "
              "cells_d>-20cm=%ld cells_d>-5cm=%ld cells_d>0=%ld samples_d>-5cm=%ld samples_d>0=%ld ms=%u -> %s",
              s_rr.ground_clamp, nclamp, seam_bad, z.ntile, cells, z.ns,
              z.worst / (RR_UPM / 100.0), cell_hist[2], cell_hist[4], cell_hist[6], z.hist[4], z.hist[6],
              (unsigned)(td5_plat_time_ms() - t0), path);
    fprintf(stderr, "GZCENSUS: worst %.1f cm, cells d>-20cm %ld, d>-5cm %ld, d>0 %ld of %ld -> %s\n",
            z.worst / (RR_UPM / 100.0), cell_hist[2], cell_hist[4], cell_hist[6], cells, path);
    fprintf(stderr, "GZCENSUS EDGE: %ld samples, over worst %.1f cm mean %.2f cm, >10cm %ld >50cm %ld >100cm %ld; hang worst %.1f cm\n",
            ed_n, ed_worst_over / (RR_UPM / 100.0), ed_n ? ed_sum_over / ed_n / (RR_UPM / 100.0) : 0.0, ed_over[2], ed_over[4], ed_over[5],
            ed_worst_hang / (RR_UPM / 100.0));
    free(ed_keep);
    for (k = 0; k < z.ntile; k++) { rr_ground_free(&z.gr[k]); free(z.cell[k]); td5_roam_tile_free(&z.tile[k]); }
    for (k = 0; k < z.ntile; k++) free(z.cmin[k]);
    free(all); free(z.cmin); free(z.cell); free(z.slot); free(z.gr); free(z.tile); free(list);
    td5_roam_height_close(z.hc); free(z.hc);
    memset(&s_rr, 0, sizeof s_rr);
    return 1;
}

/* [roam LE] DEV: TD5RE_ROAM_WATER_CENSUS=<tile list> ("tx tz" per line; _archive/roam/LE/wet_tiles.py writes the
 * wet ones) bakes every listed tile through rr_bake with the session knobs (TD5RE_ROAM_WATER=0 for the before
 * column) and writes <log>/roam_water_census.txt + .csv: water quads and m2, wet lattice points, cells dropped for
 * a road at the water's level, cells under a deck and their lowest clearance, water vertex MB, and an FNV hash of
 * every GROUND quad (equal with the water on and off = the water changed nothing else). Tiles come from the place's
 * roam/ dir (build them with --RoamBuildTiles=list:<file> first). 1 = handled, exit. */
static uint32_t rr_fnv(uint32_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

int td5_roam_render_water_census_dev_test(void)
{
    static const char place[] = "la_plata_partido";
    const char *lp = getenv("TD5RE_ROAM_WATER_CENSUS");
    char path[600], wpath[512], err[160], gtxt[4096];
    int (*list)[2] = NULL;
    int nlist, i, nread = 0, nmiss = 0, nwt = 0;
    long q_tot = 0, wet_tot = 0, road_tot = 0, deck_tot = 0, cells_ground = 0;
    long sh_cells = 0, sh_ext = 0, sh_bank = 0, sh_cross = 0, sh_step = 0, sh_edge = 0;   /* [roam PC] */
    double clear_min = 1e300, cell_m2;
    uint32_t ghash = 2166136261u, whash = 2166136261u, t0;
    TD5_RoamHeight *hc;
    TD5_File *f;
    FILE *o, *csv;
    const char *ld;
    if (!lp || !lp[0] || !strcmp(lp, "0")) return 0;
    t0 = td5_plat_time_ms();
    memset(&s_rr, 0, sizeof s_rr);
    snprintf(s_rr.place, sizeof s_rr.place, "%s", place);
    td5_geo_source_path(path, sizeof path, place, "roam/ROAMGRID.JSON");
    if ((f = td5_plat_file_open(path, "rb")) != NULL) {
        const int64_t n = td5_plat_file_size(f);
        if (n > 0 && n < (int64_t)sizeof gtxt && td5_plat_file_read(f, gtxt, (size_t)n) == (size_t)n) {
            gtxt[n] = 0; s_rr.grid_ok = roam_grid_parse(gtxt, &s_rr.grid);
        }
        td5_plat_file_close(f);
    }
    if (!s_rr.grid_ok) roam_grid_default(place, &s_rr.grid);
    /* the knobs as td5_roam_render_begin reads them; pages are census tags (ground = 2, water = 4) */
    s_rr.page_road = 1; s_rr.page_ground = 2; s_rr.page_kerb = 3; s_rr.page_water = 4;
    s_rr.page_cls[0] = s_rr.page_cls[1] = s_rr.page_cls[2] = s_rr.page_road;   /* [integ L] LC's per-class road pages */
    s_rr.use_ground   = 1;
    s_rr.cell_m       = td5_env_int("TD5RE_ROAM_RENDER_CELL_M", 16, 8, 64);
    if (64 % s_rr.cell_m != 0) s_rr.cell_m = 16;
    s_rr.ground_drop  = td5_env_int("TD5RE_ROAM_RENDER_GROUND_DROP_CM", 40, 0, 500) * (RR_UPM / 100.0);
    s_rr.ground_clamp = !td5_env_flag_off("TD5RE_ROAM_GROUND_NOFIX");
    s_rr.patch_uv     = td5_env_int("TD5RE_ROAM_PATCH_UV", 1, 0, 1);
    s_rr.stoplines    = td5_env_int("TD5RE_ROAM_STOPLINES", 1, 0, 1);
    s_rr.kerb_line    = td5_env_int("TD5RE_ROAM_KERB_LINE", 0, 0, 1);
    s_rr.water        = td5_env_int("TD5RE_ROAM_WATER", 1, 0, 1);
    s_rr.water_below  = td5_env_int("TD5RE_ROAM_WATER_BELOW_CM", RR_WATER_BELOW_CM, 0, 300) * (RR_UPM / 100.0);
    s_rr.shore        = td5_env_int("TD5RE_ROAM_SHORE", 1, 0, 1);              /* [roam PC] */
    s_rr.shore_census = td5_env_int("TD5RE_ROAM_SHORE_CENSUS", 0, 0, 1);
    cell_m2 = (double)s_rr.cell_m * (double)s_rr.cell_m;
    hc = (TD5_RoamHeight *)calloc(1, sizeof *hc);
    td5_geo_source_path(path, sizeof path, place, "HEIGHT.R16");
    td5_geo_source_path(wpath, sizeof wpath, place, "WATER.R8");
    if (!hc || !td5_roam_height_open(hc, path, wpath,
            (double)td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400) / 100.0)) {
        TD5_LOG_E(LOG_TAG, "WATERCENSUS: no height source at %s", path); free(hc); return 1;
    }
    nlist = td5_roam_read_tile_list(lp, &list);
    if (nlist <= 0) { TD5_LOG_E(LOG_TAG, "WATERCENSUS: no tiles in %s", lp); td5_roam_height_close(hc); free(hc); return 1; }
    ld = td5_plat_log_dir();
    snprintf(path, sizeof path, "%sroam_water_census.csv", ld ? ld : "");
    csv = fopen(path, "w");
    if (csv) fprintf(csv, "tx,tz,water_quads,wet_points,road_cells_dropped,deck_cells,min_clear_m,ground_quads\n");
    for (i = 0; i < nlist; i++) {
        RoamTile t;
        RRNb nb;
        RRTile *rt;
        memset(&t, 0, sizeof t);
        td5_roam_tile_path(path, sizeof path, NULL, place, list[i][0], list[i][1]);
        if (!td5_roam_tile_read(path, &t, 0, err, sizeof err)) { nmiss++; continue; }
        nread++;
        rr_nb_get(&nb, &t, NULL, 0);
        rt = rr_bake(&t, hc, &nb);
        rr_nb_free(&nb);
        if (rt) {
            int k, ci;
            for (k = 0; k < rt->nmesh; k++) {           /* hash the ground quads + the water quads, by command */
                const TD5_MeshHeader *m = rt->mesh[k];
                const TD5_MeshVertex *v = m->vertices;
                for (ci = 0; ci < m->command_count; ci++) {
                    const TD5_PrimitiveCmd *c = &m->commands[ci];
                    const int nv = c->triangle_count * 3 + c->quad_count * 4;
                    int e;
                    for (e = 0; e < nv; e++) {
                        const float pp[3] = { v[e].pos_x, v[e].pos_y, v[e].pos_z };
                        if (c->texture_page_id == s_rr.page_ground) ghash = rr_fnv(ghash, pp, sizeof pp);
                        if (c->texture_page_id == s_rr.page_water)  whash = rr_fnv(whash, pp, sizeof pp);
                    }
                    v += nv;
                }
            }
            q_tot += rt->nwater; wet_tot += rt->nwet; road_tot += rt->nwater_road; deck_tot += rt->nwater_deck;
            cells_ground += rt->nground;
            sh_cells += rt->nshore; sh_ext += rt->nshore_ext; sh_bank += rt->nbank;          /* [roam PC] */
            sh_cross += rt->sh_cross; sh_step += rt->sh_step; sh_edge += rt->sh_edge;
            if (rt->water_clear < clear_min) clear_min = rt->water_clear;
            if (rt->nwater) nwt++;
            if (csv) fprintf(csv, "%d,%d,%u,%u,%u,%u,%.2f,%u\n", t.tx, t.tz, rt->nwater, rt->nwet, rt->nwater_road,
                             rt->nwater_deck, rt->water_clear < 1e299 ? rt->water_clear / RR_UPM : -1.0, rt->nground);
            rr_free_tile(rt);
        }
        td5_roam_tile_free(&t);
    }
    if (csv) fclose(csv);
    snprintf(path, sizeof path, "%sroam_water_census.txt", ld ? ld : "");
    if ((o = fopen(path, "w")) != NULL) {
        fprintf(o, "roam water census (roam LE), place %s, list %s\n", place, lp);
        fprintf(o, "knobs: TD5RE_ROAM_WATER=%d below=%.0f cm cell=%d m drop=%.0f cm clamp=%d\n", s_rr.water,
                s_rr.water_below / (RR_UPM / 100.0), s_rr.cell_m, s_rr.ground_drop / (RR_UPM / 100.0), s_rr.ground_clamp);
        fprintf(o, "tiles listed %d, read %d, missing ROAMTILE %d, with water %d\n", nlist, nread, nmiss, nwt);
        fprintf(o, "water quads %ld (%.3f km2), wet lattice points %ld (%.3f km2)\n", q_tot, q_tot * cell_m2 / 1e6,
                wet_tot, wet_tot * cell_m2 / 1e6);
        fprintf(o, "water on road: cells dropped (road under the plane + %.0f cm) %ld; kept cells under a road (deck) %ld, "
                "lowest clearance %.2f m\n", RR_WATER_ROAD_GAP / (RR_UPM / 100.0), road_tot, deck_tot,
                clear_min < 1e299 ? clear_min / RR_UPM : -1.0);
        fprintf(o, "water vertex data %.2f MB\n", (double)q_tot * 4.0 * sizeof(TD5_MeshVertex) / 1048576.0);
        fprintf(o, "ground quads %ld hash %08x ; water hash %08x\n", cells_ground, ghash, whash);
        fprintf(o, "[roam PC] TD5RE_ROAM_SHORE=%d: shore cells drawn %ld, water cells added by sub-samples %ld, bank (mud) "
                "sub-vertices %ld\n", s_rr.shore, sh_cells, sh_ext, sh_bank);
        if (s_rr.shore_census)
            fprintf(o, "[roam PC] shore census: real shore %.2f km (%ld crossings at 0.5 m), drawn > 2 m off it %.2f km "
                    "(%ld, %.1f %%), not judged (tile edge) %ld\n", 0.7853981634 * 0.5 * sh_cross / 1000.0, sh_cross,
                    0.7853981634 * 0.5 * sh_step / 1000.0, sh_step, sh_cross ? 100.0 * sh_step / sh_cross : 0.0, sh_edge);
        fprintf(o, "ms %u\n", (unsigned)(td5_plat_time_ms() - t0));
        fclose(o);
    }
    fprintf(stderr, "WATERCENSUS shore=%d: cells %ld ext %ld bank %ld | crossings %ld step2m %ld edge %ld\n", s_rr.shore,
            sh_cells, sh_ext, sh_bank, sh_cross, sh_step, sh_edge);
    fprintf(stderr, "WATERCENSUS: %d tiles read (%d missing), water quads %ld (%.3f km2), road-dropped %ld, deck %ld "
            "min_clear %.2f m, ground hash %08x water hash %08x -> %s\n", nread, nmiss, q_tot, q_tot * cell_m2 / 1e6,
            road_tot, deck_tot, clear_min < 1e299 ? clear_min / RR_UPM : -1.0, ghash, whash, path);
    TD5_LOG_I(LOG_TAG, "WATERCENSUS: %d tiles read (%d missing), water quads %ld, road-dropped %ld, deck %ld, ground hash "
              "%08x water hash %08x", nread, nmiss, q_tot, road_tot, deck_tot, ghash, whash);
    free(list);
    td5_roam_height_close(hc); free(hc);
    memset(&s_rr, 0, sizeof s_rr);
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

#ifndef TD5RE_RELEASE
/* [roam IA] DEV cull audit, run after the frame's draw loop with the same camera. It re-takes the
 * loop's decisions stage by stage (1 tile radius, 2 tile frustum, 3 mesh / sub-block radius,
 * 4 sub-block frustum, 5 the renderer's 256-mesh list cap, 6 the renderer's per-mesh frustum) and
 * holds each REJECTED mesh against the truth: a vertex in view within the draw radius. In view =
 * the cull planes' own point test OR the projection (td5_render_project_world inside the pane
 * rectangle centre +- centre), so cull planes narrower than the drawn picture show up too.
 * Class: 0 road layer, 1 a mesh with a pavement-page command, 2 other scenery. */
static int rr_au_class(const TD5_MeshHeader *m, int scen)
{
    int c;
    if (!scen) return 0;
    for (c = 0; c < m->command_count; c++)
        if (m->commands[c].texture_page_id == s_rr.page_kerb) return 1;
    return 2;
}

static int rr_au_on_screen(float x, float y, float z)
{
    const float cx = td5_render_get_center_x(), cy = td5_render_get_center_y();
    float sx, sy, rhw;
    /* view z past 195000 (DEFAULT_FAR_CULL = DEFAULT_FAR_CLIP, td5_render_internal.h) is never drawn */
    if (!td5_render_project_world(x, y, z, &sx, &sy, &rhw) || !(rhw >= 1.0f / 195000.0f)) return 0;
    return sx >= 0.0f && sx <= 2.0f * cx && sy >= 0.0f && sy <= 2.0f * cy;
}

/* 1 = a vertex is in view inside R, 2 = in view but only beyond R, 0 = not in view */
static int rr_au_truth(const TD5_MeshHeader *m, float ex, float ey, float ez, float R)
{
    const float ox = m->origin_x / 256.0f, oy = m->origin_y / 256.0f, oz = m->origin_z / 256.0f;
    int i, far_hit = 0;
    for (i = 0; i < m->total_vertex_count; i++) {
        const TD5_MeshVertex *v = &m->vertices[i];
        const float x = ox + v->pos_x, y = oy + v->pos_y, z = oz + v->pos_z;
        const float dx = x - ex, dy = y - ey, dz = z - ez;
        const int planes = td5_render_is_sphere_visible(x, y, z, 0.0f), proj = rr_au_on_screen(x, y, z);
        if (proj && !planes) s_rr.au_proj++;
        if (!planes && !proj) continue;
        if (dx * dx + dy * dy + dz * dz <= R * R) return 1;
        far_hit = 1;
    }
    return far_hit ? 2 : 0;
}

static void rr_au_one(RRTile *t, int slot, const TD5_MeshHeader *m, int scen, int stage,
                      float ex, float ey, float ez, float R)
{
    const int cl = rr_au_class(m, scen);
    const uint8_t prev = t->aud[slot];
    s_rr.au_tested[cl]++;
    if (!stage) { s_rr.au_drawn[cl]++; t->aud[slot] = 1; return; }
    t->aud[slot] = 0;
    {
        const int tr = rr_au_truth(m, ex, ey, ez, R);
        if (tr == 2) s_rr.au_far[cl]++;
        if (tr != 1) return;
    }
    s_rr.au_false[stage][cl]++;
    if (prev) s_rr.au_pop[stage][cl]++;
    if (s_rr.au_logged < 40) {
        s_rr.au_logged++;
        TD5_LOG_I(LOG_TAG, "ROAM_CULL_FALSE frame %llu tile %d,%d stage %d class %d mesh c(%.0f,%.0f,%.0f) r %.0f "
                  "verts %d eye (%.0f,%.0f,%.0f)%s", (unsigned long long)s_rr.au_frames, t->tx, t->tz, stage, cl,
                  m->bounding_center_x, m->bounding_center_y, m->bounding_center_z, m->bounding_radius,
                  m->total_vertex_count, ex, ey, ez, prev ? " POP" : "");
    }
}

static void rr_audit_frame(float ex, float ey, float ez, float R)
{
    int i, k;
    uint32_t j;
    s_rr.au_frames++;
    for (i = 0; i < s_rr.ntile; i++) {
        RRTile *t = s_rr.tile[i];
        float dx = t->cx - ex, dy = t->cy - ey, dz = t->cz - ez;
        int tstage = 0;
        if (!t->aud) {
            t->aud = (uint8_t *)calloc((size_t)t->nmesh + t->nscn + 1u, 1);
            if (!t->aud) continue;
        }
        if (sqrtf(dx * dx + dy * dy + dz * dz) - t->r > R) tstage = 1;
        else if (!td5_render_is_sphere_visible(t->cx, t->cy, t->cz, t->r)) tstage = 2;
        for (k = 0; k < t->nmesh; k++) {
            const TD5_MeshHeader *m = t->mesh[k];
            const float *s = t->sph[k];
            int st = tstage;
            if (!st) {
                dx = s[0] - ex; dy = s[1] - ey; dz = s[2] - ez;
                if (sqrtf(dx * dx + dy * dy + dz * dz) - s[3] > R) st = 3;
                else if (!td5_render_is_sphere_visible(m->bounding_center_x, m->bounding_center_y,
                                                       m->bounding_center_z, m->bounding_radius)) st = 6;
            }
            rr_au_one(t, k, m, 0, st, ex, ey, ez, R);
        }
        for (k = 0; k < TD5_ROAM_SCN_NSB; k++) {
            const float *s = t->ssph[k];
            int sst = tstage;
            if (!t->sdl[k].count) continue;
            if (!sst) {
                dx = s[0] - ex; dy = s[1] - ey; dz = s[2] - ez;
                if (sqrtf(dx * dx + dy * dy + dz * dz) - s[3] > R) sst = 3;
                else if (!td5_render_is_sphere_visible(s[0], s[1], s[2], s[3])) sst = 4;
                else if (t->sdl[k].count > 256) sst = 5;
            }
            for (j = 0; j < t->sdl[k].count; j++) {
                const TD5_MeshHeader *m = t->sdl[k].meshes[j];
                const int slot = t->nmesh + (int)(t->sdl[k].meshes + j - t->sptr);
                int st = sst;
                if (!st && !td5_render_is_sphere_visible(m->bounding_center_x, m->bounding_center_y,
                                                         m->bounding_center_z, m->bounding_radius)) st = 6;
                rr_au_one(t, slot, m, 1, st, ex, ey, ez, R);
            }
        }
    }
    if ((s_rr.au_frames % 240u) == 1u) {
        int c;
        TD5_LOG_I(LOG_TAG, "ROAM_CULL_AUDIT frames %llu centre %.1f,%.1f projection-only vertices %llu",
                  (unsigned long long)s_rr.au_frames, td5_render_get_center_x(), td5_render_get_center_y(),
                  (unsigned long long)s_rr.au_proj);
        for (c = 0; c < 3; c++)
            TD5_LOG_I(LOG_TAG, "ROAM_CULL_AUDIT frames %llu class %d tested %llu drawn %llu false t-r %llu t-fr %llu "
                      "m-r %llu sb-fr %llu cap %llu m-fr %llu pops %llu far %llu", (unsigned long long)s_rr.au_frames, c,
                      (unsigned long long)s_rr.au_tested[c], (unsigned long long)s_rr.au_drawn[c],
                      (unsigned long long)s_rr.au_false[1][c], (unsigned long long)s_rr.au_false[2][c],
                      (unsigned long long)s_rr.au_false[3][c], (unsigned long long)s_rr.au_false[4][c],
                      (unsigned long long)s_rr.au_false[5][c], (unsigned long long)s_rr.au_false[6][c],
                      (unsigned long long)(s_rr.au_pop[1][c] + s_rr.au_pop[2][c] + s_rr.au_pop[3][c] +
                                           s_rr.au_pop[4][c] + s_rr.au_pop[5][c] + s_rr.au_pop[6][c]),
                      (unsigned long long)s_rr.au_far[c]);
    }
}
#endif

/* [roam QB] td5_render_span_display_list draws NOTHING for a list of more than 256 meshes (its "sanity" return, from
 * the original's 0x431270): a scenery sub-block past 256 meshes vanished whole, lawn, pavement, trees and all (the
 * exact lawn cut took the Bosque-side park sub-block of tile 26,47 to 330 lawn meshes). Long lists go in slices. */
#define RR_DL_SLICE 256
static void rr_draw_dl(const TD5_SpanDisplayList *dl)
{
    uint32_t off;
    if (dl->count <= RR_DL_SLICE) { td5_render_span_display_list(dl); return; }
    for (off = 0; off < dl->count; off += RR_DL_SLICE) {
        TD5_SpanDisplayList s;
        s.count = dl->count - off < RR_DL_SLICE ? dl->count - off : RR_DL_SLICE;
        s.meshes = dl->meshes + off;
        td5_render_span_display_list(&s);
    }
}

/* [roam RA] SPLIT-SCREEN DRAW DISTANCE. The tile + scenery submission is the bulk of a roam frame (~1.4 us per mesh,
 * ~3,300 meshes a pane at 500 m): 4 panes measured 26 ms median per frame at min graphics. With 3+ local humans each
 * pane draws to TD5RE_ROAM_SPLIT_DRAW_PCT % of the radius (default 70); 1 or 2 humans keep the full radius. */
static float rr_split_scale(void)
{
    static int pct = -1;
    if (td5_roam_humans() < 3) return 1.0f;
    if (pct < 0) pct = td5_env_int("TD5RE_ROAM_SPLIT_DRAW_PCT", 70, 30, 100);
    return (float)pct / 100.0f;
}

void td5_roam_render_tiles(int view)
{
    uint32_t t0;
    int i, k;
    int tiles = 0, meshes = 0;
    uint32_t quads = 0;
    double ms;
    const float ex = g_cameraPos[0], ey = g_cameraPos[1], ez = g_cameraPos[2];
    const float R = (float)s_rr.radius * rr_split_scale();

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
            rr_draw_dl(&t->dl);
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
                    rr_draw_dl(&t->sdl[k]);
                    rr_note_slow(t, k, (int)t->sdl[k].count, td5_plat_time_us() - q0);
                }
                meshes += (int)t->sdl[k].count;
                n++;
            }
        }
        if (n > 0) tiles++;
    }
#ifndef TD5RE_RELEASE
    if (s_rr.audit) rr_audit_frame(ex, ey, ez, R);
#endif

    {
        const uint32_t t1 = (uint32_t)(td5_plat_time_us() & 0xFFFFFFFFu);
        ms = (double)(uint32_t)(t1 - t0) / 1000.0;
    }
    if (view >= 0 && view < TD5_ROAM_RENDER_VIEWS) {
        s_rr.v_tiles[view] = tiles; s_rr.v_meshes[view] = meshes;
        s_rr.v_frames[view]++;
        s_rr.v_sum_meshes[view] += (double)meshes; s_rr.v_sum_ms[view] += ms;
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
