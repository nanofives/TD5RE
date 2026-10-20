/**
 * td5_roam_view.c -- GEO FREE ROAM dev viewer (PORT-ONLY, DEV-ONLY, F1 task T8).
 * Contract: td5_roam_view.h. Tile layout: td5_roam_tile.h (ROAMTILE, design 1.7).
 *
 * FRAME CHAIN. ROAMTILE geometry is in the PLACE frame (re/assets/geo/<slug>/
 * PLACE.JSON projection, rotation 0). The game world, the strip and the collision
 * overlay are in the ROUTE frame (re/assets/geo/<slug>/_route/PLACE.JSON
 * projection). The two are linked through lat/lon exactly as
 * td5_geo_route.c gr_affine_old_from_new does (route -> lat/lon -> place), and
 * td5_roam_height.c roam_test_route_to_place is the same chain. This file adds the
 * inverse (place -> route) so a tile vertex lands where the level091 road is.
 * Coordinates are strip units (render-float, = 24.8 world / 256), the space
 * td5_render_debug_line_world projects; the eye arrives in the same units.
 *
 * HEIGHT DATUM. ROAMTILE heights are about sea level (the source DEM). The
 * strip's are about h0 = the ground height at the route origin
 * (td5_tg_world.c: strip y = tg_w_raw - h0). F1-T8b measured the difference on
 * level091: +18.4 .. +19.5 m at 12 spans, against h0 = 7969 units (18.5 m) in
 * that build's [WORLD] log line. [F1-T10] The viewer subtracts that h0 itself
 * (tg_world_geo_h0) when this process built the geo world. The DEM height at
 * route (0,0), the fallback, leaves out the detail octave tg_w_raw adds there:
 * on level091 (-Seed 2054677019, relief 23400) h0 = 8050 against a DEM 7906,
 * 0.33 m, which is the whole "+0.30 m median" T8b's coverage.py measured with
 * the DEM datum (F1-T10: median -0.03 m against h0). TD5RE_ROAM_VIEW_DATUM=<units>
 * overrides both.
 * The lines are depth-tested against the track, so roam geometry a few cm under
 * the strip tarmac would vanish: the draw adds TD5RE_ROAM_VIEW_LIFT_CM (default
 * 50) on top. Display only; the real height difference is measured offline
 * (_archive/roam/F1/T8b/coverage.py).
 *
 * DRAW. Tiles within RV_REACH of the camera's tile (5 x 5 tiles, 256 m each) are
 * read once and kept in a small LRU (a missing file is cached as "not ok" so it
 * is not re-read every frame). Per primitive: a segment is skipped when both ends
 * are beyond TD5RE_ROAM_VIEW_R of the eye. Cyan = ribbon quad and patch triangle
 * edges, white = BEDG boundary edges (drawn last so they win on a shared kerb).
 *
 * DEV ONLY. The dev viewer keeps its own file-scope state (the tile LRU and the
 * projection); it is not a builder module, so the F3 worker rule does not apply.
 * Compiled out of RELEASE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_geo_frame.h"    /* [roam M] the geographic -> game reflection */
#include "td5_render.h"
#include "td5_roam_tile.h"
#include "td5_roam_height.h"
#include "td5_roam_view.h"
#include "td5_tg_world.h"     /* tg_world_geo_h0: the strip's own datum */
#include "deps/cjson/cJSON.h"

#define LOG_TAG "roam"

#ifndef TD5RE_RELEASE

#define RV_SLOTS        32             /* >= the 5 x 5 window, so it does not thrash */
#define RV_REACH        2              /* tiles around the camera tile: 5 x 5     */
#define RV_WHITE        0xFFFFFFFFu    /* boundary / wall edges                   */
#define RV_CYAN         0xFF00FFFFu    /* ribbon quad and patch triangle edges    */
#define RV_DEFAULT_R_M  400            /* distance cull, metres                   */
#define RV_UPM          430.0          /* units per metre, both frames            */
#define RV_WGS_A        6378137.0
#define RV_WGS_F        (1.0 / 298.257223563)
#define RV_RAD          0.017453292519943295

/* [roam M] reflect: a route frame whose projection carries "reflect_x" (east reflected before the
 * rotation). The ROAM place frame is the SOURCE projection followed by the reflection
 * (td5_geo_frame.h), which rv_place_to_route / rv_route_to_place apply on the place side. */
typedef struct { double lat0, lon0, upm, mdlat, mdlon, cs, sn, offx, offz; int reflect; } RvProj;

typedef struct {
    int       used, ok;
    int       tx, tz;
    uint32_t  last;                    /* s_tick of the last use, for LRU        */
    RoamTile  t;
} RvSlot;

typedef struct { uint32_t spans, tris, bedges, lines; } RvStats;

static RvProj   s_np, s_op;            /* route frame (np), place frame (op)     */
static RoamGrid s_grid;
static char     s_slug[64];
static int      s_ready;               /* 1 when the projections and grid loaded */
static double   s_datum;               /* place height at the strip's y = 0      */
static double   s_lift;                /* display lift, units                    */
static RvSlot   s_slot[RV_SLOTS];
static uint32_t s_tick;
static int      s_log_tx = 0x7fffffff, s_log_tz = 0x7fffffff;

/* ----------------------------------------------------------- file helpers ---- */

static char *rv_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)8 * 1024 * 1024) { td5_plat_file_close(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { td5_plat_file_close(f); return NULL; }
    if (td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) {
        free(buf); td5_plat_file_close(f); return NULL;
    }
    buf[n] = '\0';
    td5_plat_file_close(f);
    return buf;
}

/* The "projection" block of a PLACE.JSON, with the metres-per-degree pair
 * recomputed from lat0 exactly as td5_geo_route.c gr_proj_init does. */
static int rv_proj_load(const char *path, RvProj *p)
{
    char *json = rv_slurp(path);
    cJSON *root, *pr, *a, *o, *u, *r, *ox, *oz;
    double e2, s, w, phi, rot;
    int ok = 0;
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    pr = cJSON_GetObjectItem(root, "projection");
    if (pr && cJSON_IsObject(pr)) {
        a  = cJSON_GetObjectItem(pr, "lat0");
        o  = cJSON_GetObjectItem(pr, "lon0");
        u  = cJSON_GetObjectItem(pr, "units_per_metre");
        r  = cJSON_GetObjectItem(pr, "rotation_rad");
        ox = cJSON_GetObjectItem(pr, "offset_x");
        oz = cJSON_GetObjectItem(pr, "offset_z");
        if (cJSON_IsNumber(a) && cJSON_IsNumber(o)) {
            p->lat0 = a->valuedouble;
            p->lon0 = o->valuedouble;
            p->upm  = cJSON_IsNumber(u) ? u->valuedouble : RV_UPM;
            rot     = cJSON_IsNumber(r) ? r->valuedouble : 0.0;
            p->cs   = cos(rot);
            p->sn   = sin(rot);
            p->offx = cJSON_IsNumber(ox) ? ox->valuedouble : 0.0;
            p->offz = cJSON_IsNumber(oz) ? oz->valuedouble : 0.0;
            {
                const cJSON *rf = cJSON_GetObjectItem(pr, TD5_GEO_PROJ_REFLECT_KEY);
                p->reflect = cJSON_IsNumber(rf) && rf->valueint != 0;
            }
            e2  = RV_WGS_F * (2.0 - RV_WGS_F);
            phi = p->lat0 * RV_RAD;
            s   = sin(phi);
            w   = 1.0 - e2 * s * s;
            p->mdlat = RV_RAD * RV_WGS_A * (1.0 - e2) / pow(w, 1.5);
            p->mdlon = RV_RAD * RV_WGS_A * cos(phi) / sqrt(w);
            ok = 1;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* ------------------------------------------------------------- frame chain ---- */

/* place (xs, zs) -> route (xr, zr): the inverse of rv_route_to_place. */
static void rv_place_to_route(const RvProj *np, const RvProj *op,
                              double xs, double zs, double *xr, double *zr)
{
    const double dx = td5_geo_game_x(xs) - op->offx, dz = zs - op->offz;
    const double e2 = (dx * op->cs + dz * op->sn) / op->upm;
    const double n2 = (-dx * op->sn + dz * op->cs) / op->upm;
    const double lon = op->lon0 + e2 / op->mdlon;
    const double lat = op->lat0 + n2 / op->mdlat;
    const double e0 = (lon - np->lon0) * np->mdlon;
    const double e = np->reflect ? td5_geo_game_x(e0) : e0;
    const double n = (lat - np->lat0) * np->mdlat;
    const double xm = e * np->cs - n * np->sn;
    const double zm = e * np->sn + n * np->cs;
    *xr = xm * np->upm + np->offx;
    *zr = zm * np->upm + np->offz;
}

/* route (xr, zr) -> place (xs, zs): gr_affine_old_from_new, point form. */
static void rv_route_to_place(const RvProj *np, const RvProj *op,
                              double xr, double zr, double *xs, double *zs)
{
    const double xm = (xr - np->offx) / np->upm;
    const double zm = (zr - np->offz) / np->upm;
    const double er = xm * np->cs + zm * np->sn;
    const double e  = np->reflect ? td5_geo_game_x(er) : er;
    const double n  = -xm * np->sn + zm * np->cs;
    const double lat = np->lat0 + n / np->mdlat;
    const double lon = np->lon0 + e / np->mdlon;
    const double e2 = (lon - op->lon0) * op->mdlon;
    const double n2 = (lat - op->lat0) * op->mdlat;
    *xs = td5_geo_game_x((e2 * op->cs - n2 * op->sn) * op->upm + op->offx);
    *zs = (e2 * op->sn + n2 * op->cs) * op->upm + op->offz;
}

/* One world (strip units, route frame) position of a tile vertex. ox/oz are
 * tile-local, oy absolute, exactly as the strip stores them. */
static void rv_world(const RoamTile *t, int32_t ox, int32_t oy, int32_t oz,
                     const TD5_StripVertex *v, double w[3])
{
    const double px = (double)t->org_x + (double)ox + (double)v->x;
    const double pz = (double)t->org_z + (double)oz + (double)v->z;
    rv_place_to_route(&s_np, &s_op, px, pz, &w[0], &w[2]);
    w[1] = (double)oy + (double)v->y - s_datum + s_lift;
}

/* ------------------------------------------------------------ setup / tiles ---- */

/* The strip's height datum: the DEM height at the route origin, from the loaded
 * geo rasters when there are any, else from the place's source HEIGHT.R16
 * through the T2 window reader. 0 when neither can be read. */
static double rv_datum(const char *slug)
{
    const char *o = getenv("TD5RE_ROAM_VIEW_DATUM");
    double h0;
    if (o && o[0]) return atof(o);
    if (tg_world_geo_h0(&h0)) return h0;
    if (td5_geo_loaded()) return td5_geo_height(0.0, 0.0);
    {
        static TD5_RoamHeight hc;     /* large (window table); used once per place */
        char hp[512], wp[512];
        double xs, zs, h = 0.0;
        const int ex100 = td5_env_int("TD5RE_GEO_EXAGGERATION",
                                      TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400);
        int ok = 0;
        td5_geo_source_path(hp, sizeof hp, slug, "HEIGHT.R16");
        td5_geo_source_path(wp, sizeof wp, slug, "WATER.R8");
        rv_route_to_place(&s_np, &s_op, 0.0, 0.0, &xs, &zs);
        if (td5_roam_height_open(&hc, hp, wp, (double)ex100 / 100.0)) {
            ok = td5_roam_height_prefetch(&hc, xs - 4300.0, zs - 4300.0, xs + 4300.0, zs + 4300.0) &&
                 td5_roam_height_sample(&hc, xs, zs, &h, NULL);
            td5_roam_height_close(&hc);
        }
        return ok ? h : 0.0;
    }
}

static int rv_setup(const char *slug)
{
    char path[512], err[64];
    char *json;
    if (strcmp(s_slug, slug) == 0 && s_ready != -1) return s_ready;
    /* a new place: drop the tile cache, then load the frames and the grid */
    {
        int i;
        for (i = 0; i < RV_SLOTS; i++) {
            if (s_slot[i].used && s_slot[i].ok) td5_roam_tile_free(&s_slot[i].t);
            memset(&s_slot[i], 0, sizeof s_slot[i]);
        }
    }
    snprintf(s_slug, sizeof s_slug, "%s", slug);
    s_ready = 0;
    td5_geo_source_path(path, sizeof path, slug, "PLACE.JSON");
    if (!rv_proj_load(path, &s_op)) { TD5_LOG_W(LOG_TAG, "roam view: no place projection in %s", path); return 0; }
    td5_geo_derived_path(path, sizeof path, slug, "PLACE.JSON");
    if (!rv_proj_load(path, &s_np)) { TD5_LOG_W(LOG_TAG, "roam view: no route projection in %s", path); return 0; }
    td5_geo_source_path(path, sizeof path, slug, "roam/ROAMGRID.JSON");
    json = rv_slurp(path);
    if (!json || !roam_grid_parse(json, &s_grid)) {
        free(json);
        TD5_LOG_W(LOG_TAG, "roam view: no ROAMGRID.JSON at %s (build tiles with --RoamBuildTiles)", path);
        return 0;
    }
    free(json);
    (void)err;
    s_datum = rv_datum(slug);
    s_lift = td5_env_int("TD5RE_ROAM_VIEW_LIFT_CM", 50, -1000, 1000) * (RV_UPM / 100.0);
    s_ready = 1;
    TD5_LOG_I(LOG_TAG, "roam view: place %s, route frame loaded, lattice %d units, "
              "height datum %.0f units (%s; geo rasters %s), display lift %.0f units",
              slug, (int)s_grid.tile, s_datum,
              (getenv("TD5RE_ROAM_VIEW_DATUM") && getenv("TD5RE_ROAM_VIEW_DATUM")[0]) ? "TD5RE_ROAM_VIEW_DATUM"
              : tg_world_geo_h0(NULL) ? "the strip's h0" : "DEM at the route origin",
              td5_geo_loaded() ? "loaded" : "not loaded", s_lift);
    return 1;
}

/* The slot for tile (tx, tz), read on first use. A tile that cannot be read stays
 * cached as not ok so it is not re-read every frame. */
static RvSlot *rv_tile(int tx, int tz)
{
    int i, victim = 0;
    char path[512], err[160];
    for (i = 0; i < RV_SLOTS; i++) {
        if (s_slot[i].used && s_slot[i].tx == tx && s_slot[i].tz == tz) {
            s_slot[i].last = s_tick;
            return &s_slot[i];
        }
    }
    for (i = 0; i < RV_SLOTS; i++) {          /* prefer a free slot, else the LRU */
        if (!s_slot[i].used) { victim = i; break; }
        if (s_slot[i].last < s_slot[victim].last) victim = i;
    }
    if (s_slot[victim].used && s_slot[victim].ok) td5_roam_tile_free(&s_slot[victim].t);
    memset(&s_slot[victim], 0, sizeof s_slot[victim]);
    s_slot[victim].used = 1;
    s_slot[victim].tx = tx;
    s_slot[victim].tz = tz;
    s_slot[victim].last = s_tick;
    td5_roam_tile_path(path, sizeof path, NULL, s_slug, tx, tz);
    s_slot[victim].ok = td5_roam_tile_read(path, &s_slot[victim].t, 0, err, sizeof err);
    if (s_slot[victim].ok) {
        TD5_LOG_I(LOG_TAG, "roam view: tile %d,%d read: %u spans %u verts %u tris %u bedges",
                  tx, tz, (unsigned)s_slot[victim].t.nspan, (unsigned)s_slot[victim].t.nvert,
                  (unsigned)s_slot[victim].t.ntri, (unsigned)s_slot[victim].t.nbedge);
    } else {
        TD5_LOG_W(LOG_TAG, "roam view: tile %d,%d not read (%s)", tx, tz, err);
    }
    return &s_slot[victim];
}

/* ------------------------------------------------------------------ draw ---- */

static int rv_radius_raw(void)
{
    int m = td5_env_int("TD5RE_ROAM_VIEW_R", RV_DEFAULT_R_M, 50, 3000);
    return (int)(m * RV_UPM);
}

static void rv_seg(const double a[3], const double b[3], uint32_t col,
                   double cx, double cz, double r2, RvStats *st)
{
    const double da = (a[0] - cx) * (a[0] - cx) + (a[2] - cz) * (a[2] - cz);
    const double db = (b[0] - cx) * (b[0] - cx) + (b[2] - cz) * (b[2] - cz);
    if (da > r2 && db > r2) return;
    td5_render_debug_line_world((float)a[0], (float)a[1], (float)a[2],
                                (float)b[0], (float)b[1], (float)b[2], col);
    st->lines++;
}

static void rv_draw_tile(const RoamTile *t, double cx, double cz, double r2, RvStats *st)
{
    uint32_t i, k;
    double w[4][3], wt[3][3];

    /* ribbon quads, cyan: the WHOLE carriageway of each span. As in STRIP.DAT the
     * left_vertex_index row runs ACROSS the road at the near end (lanes + 1
     * vertices) and the right_vertex_index row is the far end, so L .. L+n is the
     * near rung, L -> R the left rail and L+n -> R+n the right rail. */
    for (i = 0; i < t->nspan; i++) {
        const TD5_StripSpan *sp = &t->span[i];
        const uint32_t L = sp->left_vertex_index, R = sp->right_vertex_index;
        const uint32_t n = sp->pad_02[1] & 0x0Fu;          /* lane count */
        if (n < 1 || L + n >= t->nvert || R + n >= t->nvert) continue;
        rv_world(t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[L],     w[0]);
        rv_world(t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[L + n], w[1]);
        rv_world(t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[R],     w[2]);
        rv_world(t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[R + n], w[3]);
        rv_seg(w[0], w[2], RV_CYAN, cx, cz, r2, st);     /* left rail  */
        rv_seg(w[1], w[3], RV_CYAN, cx, cz, r2, st);     /* right rail */
        rv_seg(w[0], w[1], RV_CYAN, cx, cz, r2, st);     /* near rung  */
        rv_seg(w[2], w[3], RV_CYAN, cx, cz, r2, st);     /* far rung   */
        rv_seg(w[0], w[3], RV_CYAN, cx, cz, r2, st);     /* diagonal   */
        st->spans++;
    }

    /* junction patch triangles, cyan */
    for (i = 0; i < t->ntri; i++) {
        const RoamTri *tr = &t->tri[i];
        if (tr->v[0] >= t->nvert || tr->v[1] >= t->nvert || tr->v[2] >= t->nvert) continue;
        for (k = 0; k < 3; k++)
            rv_world(t, tr->ox, tr->oy, tr->oz, &t->vert[tr->v[k]], wt[k]);
        rv_seg(wt[0], wt[1], RV_CYAN, cx, cz, r2, st);
        rv_seg(wt[1], wt[2], RV_CYAN, cx, cz, r2, st);
        rv_seg(wt[2], wt[0], RV_CYAN, cx, cz, r2, st);
        st->tris++;
    }

    /* boundary / wall edges, white, drawn last so they win on a shared kerb */
    for (i = 0; i < t->nbedge; i++) {
        const RoamBEdge *be = &t->bedge[i];
        const uint32_t p = be->prim & ~ROAM_PRIM_TRI;
        int32_t ox, oy, oz;
        if (be->v[0] >= t->nvert || be->v[1] >= t->nvert) continue;
        if (be->prim & ROAM_PRIM_TRI) {
            if (p >= t->ntri) continue;
            ox = t->tri[p].ox; oy = t->tri[p].oy; oz = t->tri[p].oz;
        } else {
            if (p >= t->nspan) continue;
            ox = t->span[p].origin_x; oy = t->span[p].origin_y; oz = t->span[p].origin_z;
        }
        rv_world(t, ox, oy, oz, &t->vert[be->v[0]], wt[0]);
        rv_world(t, ox, oy, oz, &t->vert[be->v[1]], wt[1]);
        rv_seg(wt[0], wt[1], RV_WHITE, cx, cz, r2, st);
        st->bedges++;
    }
}

/* [roam F2-C] The place -> route frame chain and the strip height datum, for the
 * plain roam renderer's dev overlay (td5_roam_render.c, route mode). Same
 * projections, same datum as the line viewer draws with. 0 when the place frames
 * are not available. */
int td5_roam_view_frame(const char *slug, double xs, double zs, double *xr, double *zr)
{
    if (!slug || !slug[0] || !rv_setup(slug)) return 0;
    rv_place_to_route(&s_np, &s_op, xs, zs, xr, zr);
    return 1;
}

int td5_roam_view_frame_inv(const char *slug, double xr, double zr, double *xs, double *zs)
{
    if (!slug || !slug[0] || !rv_setup(slug)) return 0;
    rv_route_to_place(&s_np, &s_op, xr, zr, xs, zs);
    return 1;
}

double td5_roam_view_datum_units(const char *slug)
{
    return (slug && slug[0] && rv_setup(slug)) ? s_datum : 0.0;
}

int td5_roam_view_wanted(void)
{
    return td5_env_int("TD5RE_ROAM_VIEW", 0, 0, 1) != 0;
}

void td5_roam_view_draw(int32_t cam_x, int32_t cam_y, int32_t cam_z)
{
    const char *slug = td5_geo_place_slug();
    double cxs, czs;
    double r2;
    int ctx_, ctz_, dx, dz, rr;
    RvStats st;
    (void)cam_y;

    if (!slug || !slug[0]) return;
    if (!rv_setup(slug)) return;

    /* the eye in place frame picks the tile the camera stands over */
    rv_route_to_place(&s_np, &s_op, (double)cam_x, (double)cam_z, &cxs, &czs);
    roam_grid_tile_of(&s_grid, cxs, czs, &ctx_, &ctz_);
    rr = rv_radius_raw();
    r2 = (double)rr * (double)rr;
    memset(&st, 0, sizeof st);
    s_tick++;

    for (dz = -RV_REACH; dz <= RV_REACH; dz++) {
        for (dx = -RV_REACH; dx <= RV_REACH; dx++) {
            RvSlot *sl = rv_tile(ctx_ + dx, ctz_ + dz);
            if (sl->ok) rv_draw_tile(&sl->t, (double)cam_x, (double)cam_z, r2, &st);
        }
    }

    if ((s_tick % 120u) == 1u)
        TD5_LOG_I(LOG_TAG, "roam view: eye raw (%ld,%ld) place (%.0f,%.0f) tile %d,%d",
                  (long)cam_x, (long)cam_z, cxs, czs, ctx_, ctz_);

    if (ctx_ != s_log_tx || ctz_ != s_log_tz) {
        s_log_tx = ctx_; s_log_tz = ctz_;
        TD5_LOG_I(LOG_TAG, "roam view: eye tile %d,%d r=%d raw: spans %u tris %u bedges %u lines %u",
                  ctx_, ctz_, rr, (unsigned)st.spans, (unsigned)st.tris,
                  (unsigned)st.bedges, (unsigned)st.lines);
    }
}

#endif /* TD5RE_RELEASE */
