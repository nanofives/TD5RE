/* ========================================================================
 * td5_fe_geo.c -- GEOSPATIAL TRACK GENERATOR: the in-game map + route builder
 *                 (PORT-ONLY, no original counterpart).
 *
 * [GEO GENERATOR 2026-10-07, round 1007 group L2] Screen_GeoGenerator
 * (TD5_SCREEN_GEO_GENERATOR), reached from SELECT TRACK's "GEOSPATIAL TRACK
 * GENERATOR" row. An OSM raster map fills the middle of the screen; the
 * player places START, FINISH and any number of middle points on it with the
 * mouse, and BUILD turns the resulting route into a track.
 *
 * This is the in-game equivalent of the browser selector
 * (re/tools/geo_selector/index.html, GEO_TRACK_OSM_PLAN.md section 6e), which
 * stays as the offline tool. The difference that matters: this one ships in
 * the release exe, so a player never needs Python or a browser.
 *
 * THREE SEAMS, ALL OWNED ELSEWHERE:
 *   td5_geo_tiles.h  the map imagery (WinHTTP, disk cache, atlas page)
 *   td5_geo_route.h  routing + conditioning (group L3; a stub stands in today,
 *                    and td5_geo_route_is_stub() makes the screen say so)
 *   td5_geo.h        the place cache and the track slots a place registers
 *
 * COORDINATES. Three spaces, kept deliberately separate:
 *   lat/lon       what the player picks and what the router speaks
 *   MAP PIXELS    Web-Mercator slippy-map pixels at the current integer zoom,
 *                 256 per tile -- the space tiles are addressed in
 *   DESIGN pixels the frontend's 640x480 canvas, which is where the mouse
 *                 lives (s_mouse_x / s_mouse_y) and where every layout
 *                 constant below is written
 *
 * The map pane always shows GEO_VIEW_MAP_PX map pixels across its WIDTH,
 * whatever the window size, and the same screen-pixels-per-map-pixel factor
 * is used vertically -- so the imagery is never stretched even though the
 * rest of the 640x480 canvas is (sx = w/640, sy = h/480 are not equal at
 * 16:9). A stretched map would misstate distances and directions, which on a
 * tool whose whole job is picking a real-world route is not a cosmetic bug.
 * ======================================================================== */

#include "td5_frontend.h"
#include "td5_asset.h"         /* TD5_ColorKeyMode (td5_frontend_internal.h uses it) */
#include "td5_platform.h"
#include "td5re.h"
#include "td5_geo.h"
#include "td5_geo_tiles.h"
#include "td5_geo_route.h"
#include "td5_trackgen.h"
#include "td5_vectorui.h"
#include "td5_font.h"
#include "td5_i18n.h"
#include "td5_config.h"        /* shared TD5RE_* env-knob accessors */
#include "td5_frontend_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "frontend"

/* ========================================================================
 * SECTION: layout (640x480 design space)
 * Left margin 112 and the y=460 content floor are the standard bands.
 * ======================================================================== */

#define GEO_MAP_X        112.0f
#define GEO_MAP_Y         46.0f
#define GEO_MAP_W        416.0f
#define GEO_MAP_H        300.0f
#define GEO_SIDE_X       534.0f            /* readout column, 534..628        */
#define GEO_SIDE_W        94.0f
#define GEO_STATUS_X     112.0f
#define GEO_STATUS_Y     352.0f
#define GEO_STATUS_W     516.0f
#define GEO_STATUS_H      44.0f
#define GEO_BTN_Y        404
#define GEO_BTN_H         32
#define GEO_BTN_CLEAR_X  112
#define GEO_BTN_CLEAR_W   96
#define GEO_BTN_BUILD_X  214
#define GEO_BTN_BUILD_W  120
#define GEO_BTN_BACK_X   340
#define GEO_BTN_BACK_W    96

enum { GEO_BTN_CLEAR = 0, GEO_BTN_BUILD, GEO_BTN_BACK, GEO_BTN_COUNT };

/* How many map pixels the pane shows across its width. Fixed rather than
 * "one map pixel per screen pixel" so the tile count stays bounded (5 x 4 at
 * every resolution) instead of growing to 77 tiles at 4K -- which would be a
 * burst of requests at OSM for no visible gain. */
#define GEO_VIEW_MAP_PX  1024.0

#define GEO_MAX_PTS       32               /* start + finish + 30 middles     */
#define GEO_PATH_MAX    8192
#define GEO_XMARK_MAX     64
#define GEO_PLACE_MAX     16

/* Hit radius for grabbing a marker, in design pixels. */
#define GEO_GRAB_R         7.0f
/* How close a click has to land to the drawn route to count as "on it". */
#define GEO_INSERT_R       5.0f

/* Colours. Gold is the canonical frontend 0xFFE3D708; the rest are picked to
 * read over map imagery rather than over the menu background. */
#define GEO_COL_ROUTE    0xFFFF3C14u       /* the routed polyline             */
#define GEO_COL_DRAFT    0xFF9AA6B4u       /* the straight lines between pins */
#define GEO_COL_XMARK    0xFFFF2020u       /* self-crossing marker            */
#define GEO_COL_MID      0xFFFFC040u       /* a middle point                  */
#define GEO_COL_FRAME    0xFF2A2A3Cu
#define GEO_COL_PLACEHLD 0xFF3A3A44u       /* tile not here yet               */
#define GEO_COL_SHADE    0x2240FF80u       /* routable area wash              */
#define GEO_COL_SHADE_ED 0x8840FF80u       /* its edge                        */
#define GEO_COL_OK       0xFF60E060u
#define GEO_COL_BAD      0xFFFF8060u
#define GEO_COL_DIM      0xFF8899AAu

/* La Plata, the place the shipped cache covers (re/assets/geo/la_plata) and
 * the default Mariano asked for. */
#define GEO_HOME_LAT   (-34.921)
#define GEO_HOME_LON   (-57.954)
#define GEO_HOME_ZOOM   14

#define GEO_PI 3.14159265358979323846

/* ========================================================================
 * SECTION: state
 * ======================================================================== */

static double s_cen_lat = GEO_HOME_LAT;    /* map centre                      */
static double s_cen_lon = GEO_HOME_LON;
static int    s_zoom    = GEO_HOME_ZOOM;

static TD5_GeoLatLon s_pts[GEO_MAX_PTS];   /* [0] START, [n-1] FINISH         */
static int    s_n_pts;

static int    s_drag_pt = -1;              /* marker being dragged, -1 = none */
static int    s_panning;
static int    s_pan_mx, s_pan_my;          /* design-space anchor of the pan  */
static double s_pan_lat, s_pan_lon;
static int    s_prev_lmb, s_prev_rmb;
static int    s_moved_while_down;          /* a drag is not a click           */

/* Last render's canvas scale, so the input pass (which runs without sx/sy)
 * can use the same design->map mapping the draw used. A resize is one frame
 * late here, which is invisible. */
static float  s_sx = 1.0f, s_sy = 1.0f;

/* Route result, owned by this screen (copied out of the router so a later
 * build cannot pull the arrays out from under the draw). */
static TD5_GeoRouteVerdict s_verdict = TD5_GEO_ROUTE_NO_DATA;
static char   s_reason[160];
static char   s_place[64];
static int    s_spans;
static float  s_length_m;
static TD5_GeoLatLon s_path[GEO_PATH_MAX];
static int    s_n_path;
static TD5_GeoLatLon s_xmark[GEO_XMARK_MAX];
static int    s_n_xmark;
static int    s_have_route;

/* Routable-area boxes, read once per screen entry. */
static char   s_place_slug[GEO_PLACE_MAX][64];
static double s_place_bbox[GEO_PLACE_MAX][4];   /* W, S, E, N */
static int    s_n_places;

/* Off-thread rebuild. The router is synchronous by contract but must finish
 * "well under a second"; we only move it off the main thread once a build has
 * actually been measured over the frame budget, so the common cheap case
 * stays simple and synchronous. */
static int           s_rebuild_wanted;
static unsigned      s_last_build_us;
static volatile LONG s_bg_busy;
static volatile LONG s_bg_done;
static void         *s_bg_thread;
static TD5_GeoLatLon s_bg_in[GEO_MAX_PTS];
static int           s_bg_n_in;
static TD5_GeoRouteResult s_bg_out;
static TD5_GeoLatLon s_bg_path[GEO_PATH_MAX];
static TD5_GeoLatLon s_bg_xmark[GEO_XMARK_MAX];
static int           s_bg_n_path, s_bg_n_xmark;

/* Where BACK goes, and where BUILD hands off to. Both are SELECT TRACK today;
 * kept as a variable so a future Quick Race entry point can set it like the
 * AUTO TRACK STUDIO chip does (frontend_autotrack_parent_screen). */
static int    s_parent_screen = TD5_SCREEN_TRACK_SELECTION;

/* ========================================================================
 * SECTION: Web Mercator (the slippy-map projection)
 *
 * Standard EPSG:3857 tile maths: the world is 256 * 2^z pixels square, x is
 * linear in longitude, y is linear in the Mercator latitude. At z=18 that is
 * 67 108 864 px, well inside a double's exact-integer range.
 * ======================================================================== */

static double geo_world_px(int z) { return 256.0 * (double)(1 << z); }

static double geo_lon_to_mx(double lon, int z)
{
    return (lon + 180.0) / 360.0 * geo_world_px(z);
}

static double geo_lat_to_my(double lat, int z)
{
    double s, y;
    if (lat >  85.05112878) lat =  85.05112878;
    if (lat < -85.05112878) lat = -85.05112878;
    s = sin(lat * GEO_PI / 180.0);
    y = 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * GEO_PI);
    return y * geo_world_px(z);
}

static double geo_mx_to_lon(double mx, int z)
{
    return mx / geo_world_px(z) * 360.0 - 180.0;
}

static double geo_my_to_lat(double my, int z)
{
    const double y = 0.5 - my / geo_world_px(z);
    return 90.0 - 360.0 * atan(exp(-y * 2.0 * GEO_PI)) / GEO_PI;
}

/* Ground metres per map pixel at this latitude and zoom -- used for the
 * scale bar and for the "how far apart are these pins" readout. */
static double geo_m_per_map_px(double lat, int z)
{
    return 156543.03392804097 * cos(lat * GEO_PI / 180.0) / (double)(1 << z);
}

/* ========================================================================
 * SECTION: the view transform
 *
 * k = SCREEN pixels per MAP pixel, chosen so the pane width always shows
 * GEO_VIEW_MAP_PX map pixels. Vertically the same k applies, so the pane
 * shows however many map pixels its height is worth. That is what keeps the
 * imagery undistorted while the surrounding canvas is stretched.
 * ======================================================================== */

static double geo_k(float sx) { return (double)(GEO_MAP_W * sx) / GEO_VIEW_MAP_PX; }

/* DESIGN point -> MAP pixel. */
static void geo_design_to_map(float dx, float dy, float sx, float sy,
                              double *mx, double *my)
{
    const double k = geo_k(sx);
    const double cx = geo_lon_to_mx(s_cen_lon, s_zoom);
    const double cy = geo_lat_to_my(s_cen_lat, s_zoom);
    *mx = cx + (double)((dx - (GEO_MAP_X + GEO_MAP_W * 0.5f)) * sx) / k;
    *my = cy + (double)((dy - (GEO_MAP_Y + GEO_MAP_H * 0.5f)) * sy) / k;
}

/* MAP pixel -> DESIGN point. */
static void geo_map_to_design(double mx, double my, float sx, float sy,
                              float *dx, float *dy)
{
    const double k = geo_k(sx);
    const double cx = geo_lon_to_mx(s_cen_lon, s_zoom);
    const double cy = geo_lat_to_my(s_cen_lat, s_zoom);
    *dx = GEO_MAP_X + GEO_MAP_W * 0.5f + (float)((mx - cx) * k / (double)sx);
    *dy = GEO_MAP_Y + GEO_MAP_H * 0.5f + (float)((my - cy) * k / (double)sy);
}

static void geo_latlon_to_design(double lat, double lon, float sx, float sy,
                                 float *dx, float *dy)
{
    geo_map_to_design(geo_lon_to_mx(lon, s_zoom), geo_lat_to_my(lat, s_zoom),
                      sx, sy, dx, dy);
}

static void geo_design_to_latlon(float dx, float dy, float sx, float sy,
                                 double *lat, double *lon)
{
    double mx, my;
    geo_design_to_map(dx, dy, sx, sy, &mx, &my);
    *lon = geo_mx_to_lon(mx, s_zoom);
    *lat = geo_my_to_lat(my, s_zoom);
}

static int geo_in_map(float dx, float dy)
{
    return dx >= GEO_MAP_X && dx < GEO_MAP_X + GEO_MAP_W &&
           dy >= GEO_MAP_Y && dy < GEO_MAP_Y + GEO_MAP_H;
}

/* ========================================================================
 * SECTION: routing
 * ======================================================================== */

static void geo_adopt_result(const TD5_GeoRouteResult *r,
                             const TD5_GeoLatLon *path, int n_path,
                             const TD5_GeoLatLon *xm, int n_xm)
{
    s_verdict  = r->verdict;
    s_spans    = r->spans;
    s_length_m = r->length_m;
    snprintf(s_reason, sizeof(s_reason), "%s", r->reason);
    snprintf(s_place,  sizeof(s_place),  "%s", r->place_slug);
    s_n_path  = n_path;
    s_n_xmark = n_xm;
    if (n_path > 0) memcpy(s_path,  path, (size_t)n_path * sizeof(s_path[0]));
    if (n_xm   > 0) memcpy(s_xmark, xm,   (size_t)n_xm   * sizeof(s_xmark[0]));
    s_have_route = 1;
}

static void geo_clear_result(void)
{
    s_have_route = 0;
    s_n_path = s_n_xmark = 0;
    s_spans = 0;
    s_length_m = 0.0f;
    s_place[0] = '\0';
    s_verdict = TD5_GEO_ROUTE_NO_DATA;
    s_reason[0] = '\0';
}

/* Copy a router result into caller-owned arrays. The contract says path /
 * crossings stay valid only until the next call, so nothing may hold them. */
static void geo_snapshot(const TD5_GeoRouteResult *r,
                         TD5_GeoLatLon *path, int *n_path,
                         TD5_GeoLatLon *xm, int *n_xm)
{
    int n = r->n_path, m = r->n_crossings;
    if (n > GEO_PATH_MAX)  n = GEO_PATH_MAX;
    if (m > GEO_XMARK_MAX) m = GEO_XMARK_MAX;
    if (n > 0 && r->path)      memcpy(path, r->path, (size_t)n * sizeof(path[0]));
    else n = 0;
    if (m > 0 && r->crossings) memcpy(xm, r->crossings, (size_t)m * sizeof(xm[0]));
    else m = 0;
    *n_path = n;
    *n_xm   = m;
}

static void geo_bg_worker(void *arg)
{
    (void)arg;
    if (td5_geo_route_build(s_bg_in, s_bg_n_in, &s_bg_out) == 0)
        geo_snapshot(&s_bg_out, s_bg_path, &s_bg_n_path, s_bg_xmark, &s_bg_n_xmark);
    else
        s_bg_out.verdict = TD5_GEO_ROUTE_ERROR;
    InterlockedExchange(&s_bg_done, 1);
}

/* Run a build. Synchronous while that is cheap; once a build has been
 * measured over the 16 ms frame budget, later ones go to a worker so dragging
 * a marker keeps animating. */
static void geo_rebuild_route(void)
{
    TD5_GeoRouteResult r;
    uint64_t t0;

    if (s_n_pts < 2) { geo_clear_result(); s_rebuild_wanted = 0; return; }

    if (s_last_build_us > 16000u) {
        if (InterlockedCompareExchange(&s_bg_busy, 1, 0) != 0) {
            s_rebuild_wanted = 1;    /* one is already running; ask again after */
            return;
        }
        memcpy(s_bg_in, s_pts, (size_t)s_n_pts * sizeof(s_pts[0]));
        s_bg_n_in = s_n_pts;
        InterlockedExchange(&s_bg_done, 0);
        s_bg_thread = td5_plat_thread_create(geo_bg_worker, NULL);
        if (!s_bg_thread) {          /* could not spawn: fall back to inline */
            InterlockedExchange(&s_bg_busy, 0);
        } else {
            s_rebuild_wanted = 0;
            return;
        }
    }

    t0 = td5_plat_time_us();
    if (td5_geo_route_build(s_pts, s_n_pts, &r) == 0) {
        TD5_GeoLatLon path[GEO_PATH_MAX], xm[GEO_XMARK_MAX];
        int np = 0, nx = 0;
        geo_snapshot(&r, path, &np, xm, &nx);
        geo_adopt_result(&r, path, np, xm, nx);
    } else {
        geo_clear_result();
        s_verdict = TD5_GEO_ROUTE_ERROR;
        snprintf(s_reason, sizeof(s_reason), "ROUTER REFUSED THE REQUEST");
    }
    s_last_build_us = (unsigned)(td5_plat_time_us() - t0);
    s_rebuild_wanted = 0;
}

static void geo_collect_bg(void)
{
    if (!InterlockedCompareExchange(&s_bg_done, 0, 1)) return;
    if (s_bg_thread) { td5_plat_thread_join(s_bg_thread); s_bg_thread = NULL; }
    geo_adopt_result(&s_bg_out, s_bg_path, s_bg_n_path, s_bg_xmark, s_bg_n_xmark);
    InterlockedExchange(&s_bg_busy, 0);
    if (s_rebuild_wanted) geo_rebuild_route();
}

/* ========================================================================
 * SECTION: point editing
 * ======================================================================== */

static void geo_mark_dirty(void) { s_rebuild_wanted = 1; }

static int geo_pick_point(float dx, float dy, float sx, float sy)
{
    int i, best = -1;
    float best_d2 = GEO_GRAB_R * GEO_GRAB_R;
    for (i = 0; i < s_n_pts; i++) {
        float px, py, ddx, ddy, d2;
        geo_latlon_to_design(s_pts[i].lat, s_pts[i].lon, sx, sy, &px, &py);
        ddx = px - dx; ddy = py - dy;
        d2 = ddx * ddx + ddy * ddy;
        if (d2 <= best_d2) { best_d2 = d2; best = i; }
    }
    return best;
}

static void geo_insert_point(int at, double lat, double lon)
{
    int i;
    if (s_n_pts >= GEO_MAX_PTS) { frontend_play_sfx(10); return; }
    if (at < 0) at = 0;
    if (at > s_n_pts) at = s_n_pts;
    for (i = s_n_pts; i > at; i--) s_pts[i] = s_pts[i - 1];
    s_pts[at].lat = lat;
    s_pts[at].lon = lon;
    s_n_pts++;
    geo_mark_dirty();
}

static void geo_remove_point(int idx)
{
    int i;
    if (idx < 0 || idx >= s_n_pts) return;
    for (i = idx; i < s_n_pts - 1; i++) s_pts[i] = s_pts[i + 1];
    s_n_pts--;
    geo_mark_dirty();
}

/* Which LEG of the drawn route a click landed on, or -1. Returns the index of
 * the point the new middle point should be inserted BEFORE, so clicking the
 * line between pin k and pin k+1 inserts at k+1 -- "between its neighbours",
 * which is what makes the route grow where the player pointed rather than at
 * the end. Measured against the ROUTED polyline when there is one (that is
 * the line actually on screen), falling back to the straight draft lines. */
static int geo_hit_route_leg(float dx, float dy, float sx, float sy)
{
    int i, best_leg = -1;
    float best_d2 = GEO_INSERT_R * GEO_INSERT_R;

    for (i = 0; i + 1 < s_n_pts; i++) {
        /* Walk this leg as a straight segment between its two pins. The
         * routed polyline bends away from it, so for the ROUTED case we test
         * the polyline itself below and only use this as the fallback. */
        float ax, ay, bx, by, vx, vy, wx, wy, t, px, py, ex, ey, d2, len2;
        geo_latlon_to_design(s_pts[i].lat,     s_pts[i].lon,     sx, sy, &ax, &ay);
        geo_latlon_to_design(s_pts[i + 1].lat, s_pts[i + 1].lon, sx, sy, &bx, &by);
        vx = bx - ax; vy = by - ay;
        wx = dx - ax; wy = dy - ay;
        len2 = vx * vx + vy * vy;
        if (len2 < 1e-4f) continue;
        t = (wx * vx + wy * vy) / len2;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        px = ax + vx * t; py = ay + vy * t;
        ex = dx - px; ey = dy - py;
        d2 = ex * ex + ey * ey;
        if (d2 <= best_d2) { best_d2 = d2; best_leg = i + 1; }
    }

    /* With a routed polyline on screen, prefer a hit on IT -- but map the hit
     * back to a leg by nearest pin, because the router hands back one flat
     * polyline with no per-leg boundaries in it. */
    if (s_have_route && s_n_path > 1) {
        float hit_d2 = GEO_INSERT_R * GEO_INSERT_R;
        int   hit = -1;
        for (i = 0; i < s_n_path; i++) {
            float px, py, ex, ey, d2;
            geo_latlon_to_design(s_path[i].lat, s_path[i].lon, sx, sy, &px, &py);
            ex = dx - px; ey = dy - py;
            d2 = ex * ex + ey * ey;
            if (d2 <= hit_d2) { hit_d2 = d2; hit = i; }
        }
        if (hit >= 0) {
            /* Nearest pin to the hit point, then insert after it unless it is
             * the finish. (Not named `near` -- windows.h still #defines that
             * as the 16-bit memory-model keyword, so the declaration does not
             * even parse.) */
            int j, nearest = 0;
            float nd2 = -1.0f;
            for (j = 0; j < s_n_pts; j++) {
                float px, py, ex, ey, d2;
                geo_latlon_to_design(s_pts[j].lat, s_pts[j].lon, sx, sy, &px, &py);
                ex = dx - px; ey = dy - py;
                d2 = ex * ex + ey * ey;
                if (nd2 < 0.0f || d2 < nd2) { nd2 = d2; nearest = j; }
            }
            best_leg = (nearest >= s_n_pts - 1) ? s_n_pts - 1 : nearest + 1;
        }
    }
    return best_leg;
}

/* ========================================================================
 * SECTION: mouse
 *
 * The frontend's own mouse state (s_mouse_x / s_mouse_y, s_mouse_clicked) is
 * built for buttons: one latched left click, no drag, no wheel, no right
 * button. The map needs all four, so this screen tracks the raw buttons
 * itself and leaves the shared latch to the three real buttons.
 *
 * Position stays in the 640x480 DESIGN space the frontend already provides.
 * The raw client-pixel API would be finer, but it breaks at RenderScale < 100
 * (the backbuffer is no longer the client area), and a design pixel is about
 * 19 m of ground at the default zoom -- zooming in is the precision control,
 * which is how every map tool works anyway.
 * ======================================================================== */

static void geo_handle_mouse(void)
{
    const float sx = s_sx, sy = s_sy;
    const float dx = (float)s_mouse_x, dy = (float)s_mouse_y;
    const int   over_map = geo_in_map(dx, dy);
    int lmb, rmb, l_edge, l_rel, r_edge, wheel;

    lmb = td5_plat_input_mouse_left_down();
    rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
    l_edge = (lmb && !s_prev_lmb);
    l_rel  = (!lmb && s_prev_lmb);
    r_edge = (rmb && !s_prev_rmb);
    s_prev_lmb = lmb;
    s_prev_rmb = rmb;

    wheel = td5_plat_input_get_mouse_wheel();
    if (wheel != 0 && over_map) {
        /* Zoom about the cursor, so the ground under the pointer stays put --
         * the behaviour every map has, and the only one that makes placing a
         * point at a known junction practical. */
        double lat_at, lon_at;
        int nz = s_zoom + ((wheel > 0) ? 1 : -1);
        if (nz < TD5_GEO_TILE_MIN_Z) nz = TD5_GEO_TILE_MIN_Z;
        if (nz > TD5_GEO_TILE_MAX_Z) nz = TD5_GEO_TILE_MAX_Z;
        if (nz != s_zoom) {
            double keep_mx, keep_my;
            geo_design_to_latlon(dx, dy, sx, sy, &lat_at, &lon_at);
            s_zoom = nz;
            /* Re-centre so (lat_at, lon_at) lands back under the cursor. */
            keep_mx = geo_lon_to_mx(lon_at, s_zoom);
            keep_my = geo_lat_to_my(lat_at, s_zoom);
            {
                const double k = geo_k(sx);
                const double off_x = (double)((dx - (GEO_MAP_X + GEO_MAP_W * 0.5f)) * sx) / k;
                const double off_y = (double)((dy - (GEO_MAP_Y + GEO_MAP_H * 0.5f)) * sy) / k;
                s_cen_lon = geo_mx_to_lon(keep_mx - off_x, s_zoom);
                s_cen_lat = geo_my_to_lat(keep_my - off_y, s_zoom);
            }
            frontend_play_sfx(2);
        }
    }

    /* Right click: remove the marker under the cursor. */
    if (r_edge && over_map) {
        const int hit = geo_pick_point(dx, dy, sx, sy);
        if (hit >= 0) {
            geo_remove_point(hit);
            frontend_play_sfx(5);
        } else {
            frontend_play_sfx(10);
        }
    }

    if (l_edge && over_map) {
        const int hit = geo_pick_point(dx, dy, sx, sy);
        s_moved_while_down = 0;
        if (hit >= 0) {
            s_drag_pt = hit;                   /* grab a marker */
        } else {
            s_panning  = 1;                    /* grab the map  */
            s_pan_mx   = s_mouse_x;
            s_pan_my   = s_mouse_y;
            s_pan_lat  = s_cen_lat;
            s_pan_lon  = s_cen_lon;
        }
    }

    if (lmb && s_drag_pt >= 0) {
        double lat, lon;
        geo_design_to_latlon(dx, dy, sx, sy, &lat, &lon);
        if (lat != s_pts[s_drag_pt].lat || lon != s_pts[s_drag_pt].lon) {
            s_pts[s_drag_pt].lat = lat;
            s_pts[s_drag_pt].lon = lon;
            s_moved_while_down = 1;
            geo_mark_dirty();
        }
    } else if (lmb && s_panning) {
        const double k = geo_k(sx);
        const double ddx = (double)((s_mouse_x - s_pan_mx) * sx) / k;
        const double ddy = (double)((s_mouse_y - s_pan_my) * sy) / k;
        if (s_mouse_x != s_pan_mx || s_mouse_y != s_pan_my) s_moved_while_down = 1;
        s_cen_lon = geo_mx_to_lon(geo_lon_to_mx(s_pan_lon, s_zoom) - ddx, s_zoom);
        s_cen_lat = geo_my_to_lat(geo_lat_to_my(s_pan_lat, s_zoom) - ddy, s_zoom);
    }

    if (l_rel) {
        const int was_drag = s_drag_pt;
        const int was_pan  = s_panning;
        s_drag_pt = -1;
        s_panning = 0;
        /* A press-and-release that never moved, on empty map, is a PLACE. A
         * drag is not: that was a pan, and ending it must not drop a pin. */
        if (was_pan && !s_moved_while_down && over_map) {
            double lat, lon;
            int leg;
            geo_design_to_latlon(dx, dy, sx, sy, &lat, &lon);
            leg = geo_hit_route_leg(dx, dy, sx, sy);
            if (leg >= 0 && s_n_pts >= 2) {
                geo_insert_point(leg, lat, lon);     /* between its neighbours */
                frontend_play_sfx(3);
            } else if (s_n_pts < 2) {
                geo_insert_point(s_n_pts, lat, lon); /* START, then FINISH     */
                frontend_play_sfx(3);
            } else {
                /* Past the finish: extend the route rather than refuse. */
                geo_insert_point(s_n_pts, lat, lon);
                frontend_play_sfx(3);
            }
        }
        (void)was_drag;
        s_moved_while_down = 0;
    }
}

/* ========================================================================
 * SECTION: drawing
 * ======================================================================== */

static void geo_fill(float x, float y, float w, float h, uint32_t c,
                     float sx, float sy)
{
    fe_draw_quad(x * sx, y * sy, w * sx, h * sy, c, -1, 0, 0, 1, 1);
}

static void geo_frame_rect(float x, float y, float w, float h, uint32_t c,
                           float sx, float sy)
{
    geo_fill(x,         y,         w,   1.0f, c, sx, sy);
    geo_fill(x,         y + h - 1, w,   1.0f, c, sx, sy);
    geo_fill(x,         y,         1.0f, h,   c, sx, sy);
    geo_fill(x + w - 1, y,         1.0f, h,   c, sx, sy);
}

/* Stamp a line as a run of small quads, clipped to the map pane.
 *
 * There is no 2D line primitive in the frontend (verified: nothing named
 * fe_draw_line exists, and td5_plat_render_draw_lines is the depth-tested
 * debug-overlay path, not a UI one). The shipped geo route plot
 * (td5_geo_draw_route, td5_fe_race.c) draws its polyline as a string of dots
 * for the same reason, so this follows that precedent rather than inventing a
 * second way to do it. Stamps are spaced under their own width so the run
 * reads as a continuous line. */
static int geo_stamp_line(float x0, float y0, float x1, float y1,
                          float thick, uint32_t col, float sx, float sy,
                          int budget)
{
    const float ddx = x1 - x0, ddy = y1 - y0;
    const float len = sqrtf(ddx * ddx + ddy * ddy);
    const float step = (thick > 1.0f) ? thick * 0.6f : 0.8f;
    int n, i;

    if (budget <= 0) return 0;
    if (len < 0.01f) n = 1;
    else             n = (int)(len / step) + 1;
    if (n > budget) n = budget;

    for (i = 0; i < n; i++) {
        const float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        const float px = x0 + ddx * t;
        const float py = y0 + ddy * t;
        if (px < GEO_MAP_X || px >= GEO_MAP_X + GEO_MAP_W ||
            py < GEO_MAP_Y || py >= GEO_MAP_Y + GEO_MAP_H)
            continue;              /* clip to the pane, no scissor needed */
        geo_fill(px - thick * 0.5f, py - thick * 0.5f, thick, thick, col, sx, sy);
    }
    return n;
}

static void geo_draw_tiles(float sx, float sy)
{
    const double k   = geo_k(sx);
    const double cx  = geo_lon_to_mx(s_cen_lon, s_zoom);
    const double cy  = geo_lat_to_my(s_cen_lat, s_zoom);
    const double half_w_map = (double)(GEO_MAP_W * 0.5f * sx) / k;
    const double half_h_map = (double)(GEO_MAP_H * 0.5f * sy) / k;
    const double left   = cx - half_w_map;
    const double top    = cy - half_h_map;
    const double right  = cx + half_w_map;
    const double bottom = cy + half_h_map;
    const int    span   = 1 << s_zoom;
    const float  tile_dw = (float)(256.0 * k / (double)sx);   /* design px wide */
    const float  tile_dh = (float)(256.0 * k / (double)sy);
    int tx, ty;
    int tx0 = (int)floor(left   / 256.0), tx1 = (int)floor((right  - 0.001) / 256.0);
    int ty0 = (int)floor(top    / 256.0), ty1 = (int)floor((bottom - 0.001) / 256.0);

    if (ty0 < 0) ty0 = 0;
    if (ty1 > span - 1) ty1 = span - 1;

    for (ty = ty0; ty <= ty1; ty++) {
        for (tx = tx0; tx <= tx1; tx++) {
            /* Longitude wraps; latitude does not (clamped above). */
            int wx = tx % span;
            float ox, oy;
            float u0, v0, u1, v1;
            if (wx < 0) wx += span;
            geo_map_to_design((double)tx * 256.0, (double)ty * 256.0, sx, sy, &ox, &oy);
            if (td5_geo_tiles_lookup(s_zoom, wx, ty, &u0, &v0, &u1, &v1)) {
                fe_draw_quad(ox * sx, oy * sy, tile_dw * sx, tile_dh * sy,
                             0xFFFFFFFFu, TD5_GEO_TILE_ATLAS_PAGE, u0, v0, u1, v1);
            } else {
                geo_fill(ox, oy, tile_dw, tile_dh, GEO_COL_PLACEHLD, sx, sy);
            }
        }
    }
}

/* Wash over every area a route can actually be built in. Clipped to the pane
 * by hand -- the fill is one quad, so there is nothing to scissor. */
static void geo_draw_places(float sx, float sy)
{
    int i;
    if (s_n_places <= 0) return;
    /* The wash is alpha-blended, so it needs the translucent preset. Without
     * it the frontend's default opaque state ignores the alpha byte and
     * 0x22-alpha green came out as FLAT GREEN over the whole map -- which is
     * exactly what the first framedump showed. */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    for (i = 0; i < s_n_places; i++) {
        float x0, y0, x1, y1, t;
        geo_latlon_to_design(s_place_bbox[i][3], s_place_bbox[i][0], sx, sy, &x0, &y0);
        geo_latlon_to_design(s_place_bbox[i][1], s_place_bbox[i][2], sx, sy, &x1, &y1);
        if (x1 < x0) { t = x0; x0 = x1; x1 = t; }
        if (y1 < y0) { t = y0; y0 = y1; y1 = t; }
        if (x0 < GEO_MAP_X) x0 = GEO_MAP_X;
        if (y0 < GEO_MAP_Y) y0 = GEO_MAP_Y;
        if (x1 > GEO_MAP_X + GEO_MAP_W) x1 = GEO_MAP_X + GEO_MAP_W;
        if (y1 > GEO_MAP_Y + GEO_MAP_H) y1 = GEO_MAP_Y + GEO_MAP_H;
        if (x1 <= x0 || y1 <= y0) continue;
        geo_fill(x0, y0, x1 - x0, y1 - y0, GEO_COL_SHADE, sx, sy);
        geo_frame_rect(x0, y0, x1 - x0, y1 - y0, GEO_COL_SHADE_ED, sx, sy);
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

static void geo_draw_route(float sx, float sy)
{
    int budget = 2400;    /* hard cap on stamps, so a long route cannot stall a frame */
    int i;

    /* The straight draft between pins, under everything: it is what the route
     * WOULD be if routing had nothing to say, and it keeps the pins visibly
     * connected while a rebuild is in flight. */
    for (i = 0; i + 1 < s_n_pts && budget > 0; i++) {
        float ax, ay, bx, by;
        geo_latlon_to_design(s_pts[i].lat,     s_pts[i].lon,     sx, sy, &ax, &ay);
        geo_latlon_to_design(s_pts[i + 1].lat, s_pts[i + 1].lon, sx, sy, &bx, &by);
        budget -= geo_stamp_line(ax, ay, bx, by, 1.0f, GEO_COL_DRAFT, sx, sy, budget);
    }

    /* The routed polyline. Decimated to the screen: consecutive nodes closer
     * than a pixel add nothing and cost a draw call each. */
    if (s_have_route && s_n_path > 1) {
        float px = 0.0f, py = 0.0f;
        int first = 1;
        for (i = 0; i < s_n_path && budget > 0; i++) {
            float nx, ny, ex, ey;
            geo_latlon_to_design(s_path[i].lat, s_path[i].lon, sx, sy, &nx, &ny);
            if (first) { px = nx; py = ny; first = 0; continue; }
            ex = nx - px; ey = ny - py;
            if (ex * ex + ey * ey < 1.0f) continue;
            budget -= geo_stamp_line(px, py, nx, ny, 2.0f, GEO_COL_ROUTE, sx, sy, budget);
            px = nx; py = ny;
        }
    }

    /* Self-crossings, on top: these are the reason a route gets rejected. */
    for (i = 0; i < s_n_xmark; i++) {
        float mx, my;
        geo_latlon_to_design(s_xmark[i].lat, s_xmark[i].lon, sx, sy, &mx, &my);
        if (!geo_in_map(mx, my)) continue;
        geo_fill(mx - 3.0f, my - 1.0f, 7.0f, 3.0f, GEO_COL_XMARK, sx, sy);
        geo_fill(mx - 1.0f, my - 3.0f, 3.0f, 7.0f, GEO_COL_XMARK, sx, sy);
    }

    /* Markers last. START and FINISH reuse the shared map markers so this
     * screen reads the same as the SELECT TRACK route preview; middles are a
     * plain amber pip, deliberately smaller so the ends stay findable. */
    for (i = 0; i < s_n_pts; i++) {
        float mx, my;
        geo_latlon_to_design(s_pts[i].lat, s_pts[i].lon, sx, sy, &mx, &my);
        if (!geo_in_map(mx, my)) continue;
        if (i == 0) {
            frontend_draw_marker_dot(mx * sx, my * sy, sx, sy, 0);        /* START  */
        } else if (i == s_n_pts - 1 && s_n_pts >= 2) {
            frontend_draw_marker_dot(mx * sx, my * sy, sx, sy, 1);        /* FINISH */
        } else {
            geo_fill(mx - 3.0f, my - 3.0f, 6.0f, 6.0f, GEO_COL_MID, sx, sy);
            geo_frame_rect(mx - 4.0f, my - 4.0f, 8.0f, 8.0f, 0xFF201808u, sx, sy);
        }
        /* The grabbed marker gets a ring so a drag is visibly attached. */
        if (i == s_drag_pt)
            geo_frame_rect(mx - 7.0f, my - 7.0f, 14.0f, 14.0f, 0xFFE3D708u, sx, sy);
    }
}

static const char *geo_verdict_text(void)
{
    switch (s_verdict) {
    case TD5_GEO_ROUTE_OK:         return TR("ROUTE OK");
    case TD5_GEO_ROUTE_NO_DATA:    return TR("NO MAP DATA");
    case TD5_GEO_ROUTE_NO_PATH:    return TR("NO ROAD PATH");
    case TD5_GEO_ROUTE_TOO_LONG:   return TR("TOO LONG");
    case TD5_GEO_ROUTE_TOO_SHORT:  return TR("TOO SHORT");
    default:                       return TR("ROUTER ERROR");
    }
}

static int geo_route_is_buildable(void)
{
    return s_have_route && s_verdict == TD5_GEO_ROUTE_OK && !td5_geo_route_is_stub();
}

void frontend_geo_generator_render(float sx, float sy)
{
    char line[160];
    const float side_x = GEO_SIDE_X;

    s_sx = sx; s_sy = sy;

    /* Decode whatever the tile workers finished and refresh the atlas page.
     * Before the draw, so a tile that landed this frame is visible this
     * frame rather than next. */
    td5_geo_tiles_frame();

    /* ---- the map ----
     * SCISSORED. A slippy map always draws whole tiles, so the edge ones hang
     * over the pane by up to 256 map pixels; unclipped they covered the title
     * and ran off both sides of the canvas (first framedump, 2026-10-07).
     * The clip is also what lets the tile loop stay a plain whole-tile loop
     * instead of computing partial quads. */
    td5_plat_render_set_clip_rect((int)(GEO_MAP_X * sx), (int)(GEO_MAP_Y * sy),
                                  (int)((GEO_MAP_X + GEO_MAP_W) * sx),
                                  (int)((GEO_MAP_Y + GEO_MAP_H) * sy));
    geo_fill(GEO_MAP_X, GEO_MAP_Y, GEO_MAP_W, GEO_MAP_H, 0xFF101018u, sx, sy);
    geo_draw_tiles(sx, sy);
    geo_draw_places(sx, sy);
    geo_draw_route(sx, sy);
    td5_plat_render_set_clip_rect(0, 0, (int)(640.0f * sx), (int)(480.0f * sy));

    geo_frame_rect(GEO_MAP_X, GEO_MAP_Y, GEO_MAP_W, GEO_MAP_H, GEO_COL_FRAME, sx, sy);

    /* Title AFTER the map. Drawn before it, the overhanging edge tiles painted
     * straight over it.
     *
     * "GEO TRACK GENERATOR", not the full "GEOSPATIAL TRACK GENERATOR" that
     * the SELECT TRACK row carries. frontend_draw_screen_title does NOT fit
     * text: its advance comes from FE_TITLE_CAP_PX * sy while the canvas is
     * 640 * sx wide, so a title is widest in DESIGN terms at 4:3 (sx == sy)
     * and 4/3 narrower at 16:9. Measured on a 1024x768 framedump
     * (2026-10-07): the 26-character string ran clean off the right edge of
     * the canvas, exactly the way "CUSTOM PERFORMANCE" did on screen 54, and
     * a 19-character second attempt ("GEO TRACK GENERATOR") still ended at
     * design x~637 of 640. The title font runs ~27 design px per character
     * from FE_TITLE_LEFT_X (126) at 4:3, so 13 characters is the comfortable
     * budget. "GEO GENERATOR" ends around x=476 and is also exactly what the
     * s_screens[] badge calls this screen, so the header and the dev badge
     * agree. The row the player clicked still says Mariano's full phrase. */
    frontend_draw_screen_title(TR("GEO GENERATOR"),
                               FE_TITLE_LEFT_X * sx, 17.0f * sy,
                               0xFFE3D708u, sx, sy);

    /* [ODbL] The credit travels with the map and is never conditional.
     * LEFT-aligned inside the pane's bottom edge: right-aligning it off
     * fe_measure_small_text clipped the last characters on the first
     * framedump, and a credit that can be truncated is not a credit. A dark
     * plate under it keeps it readable over pale map imagery. */
    {
        const char *credit = TR("(C) OPENSTREETMAP CONTRIBUTORS");
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        geo_fill(GEO_MAP_X + 1.0f, GEO_MAP_Y + GEO_MAP_H - 15.0f,
                 GEO_MAP_W - 2.0f, 14.0f, 0x99101018u, sx, sy);
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        fe_draw_small_text((GEO_MAP_X + 4.0f) * sx,
                           (GEO_MAP_Y + GEO_MAP_H - 14.0f) * sy,
                           credit, 0xFFE8ECF0u, sx, sy);
    }

    /* ---- side readout ---- */
    {
        float y = GEO_MAP_Y + 2.0f;
        const double mpp = geo_m_per_map_px(s_cen_lat, s_zoom);
        snprintf(line, sizeof(line), "%s %d", TR("ZOOM"), s_zoom);
        fe_draw_small_text(side_x * sx, y * sy, line, GEO_COL_DIM, sx, sy);
        y += 13.0f;
        snprintf(line, sizeof(line), "%s %d", TR("POINTS"), s_n_pts);
        fe_draw_small_text(side_x * sx, y * sy, line, GEO_COL_DIM, sx, sy);
        y += 13.0f;
        /* Scale: ground metres across 100 map pixels, the cheapest honest
         * statement of scale that needs no bar graphic. */
        snprintf(line, sizeof(line), "%d M", (int)(mpp * 100.0));
        fe_draw_small_text(side_x * sx, y * sy, line, GEO_COL_DIM, sx, sy);
        y += 20.0f;
        {
            const int pend = td5_geo_tiles_pending();
            const int fail = td5_geo_tiles_failed();
            if (pend > 0) {
                snprintf(line, sizeof(line), "%s %d", TR("LOADING"), pend);
                fe_draw_small_text(side_x * sx, y * sy, line, 0xFFE3D708u, sx, sy);
            } else if (fail > 0) {
                fe_draw_small_text(side_x * sx, y * sy, TR("OFFLINE"),
                                   GEO_COL_BAD, sx, sy);
            }
        }
    }

    /* ---- status ---- */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    geo_fill(GEO_STATUS_X, GEO_STATUS_Y, GEO_STATUS_W, GEO_STATUS_H,
             0xC0101018u, sx, sy);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    geo_frame_rect(GEO_STATUS_X, GEO_STATUS_Y, GEO_STATUS_W, GEO_STATUS_H,
                   GEO_COL_FRAME, sx, sy);
    if (s_n_pts < 2) {
        fe_draw_text((GEO_STATUS_X + 6.0f) * sx, (GEO_STATUS_Y + 5.0f) * sy,
                     TR("CLICK THE MAP TO PLACE START, THEN FINISH"),
                     GEO_COL_DIM, sx, sy);
        fe_draw_small_text((GEO_STATUS_X + 6.0f) * sx, (GEO_STATUS_Y + 27.0f) * sy,
                           TR("DRAG TO PAN - WHEEL TO ZOOM - RIGHT CLICK REMOVES A POINT"),
                           GEO_COL_DIM, sx, sy);
    } else {
        const uint32_t vc = (s_verdict == TD5_GEO_ROUTE_OK) ? GEO_COL_OK : GEO_COL_BAD;
        fe_draw_text((GEO_STATUS_X + 6.0f) * sx, (GEO_STATUS_Y + 5.0f) * sy,
                     geo_verdict_text(), vc, sx, sy);
        snprintf(line, sizeof(line), "%d / %d %s   %.1f KM   %s",
                 s_spans, TD5_GEO_ROUTE_MAX_SPANS, TR("SPANS"),
                 (double)s_length_m / 1000.0,
                 s_place[0] ? s_place : "-");
        fe_draw_small_text((GEO_STATUS_X + 150.0f) * sx, (GEO_STATUS_Y + 8.0f) * sy,
                           line, GEO_COL_DIM, sx, sy);
        if (s_n_xmark > 0) {
            snprintf(line, sizeof(line), "%d %s", s_n_xmark, TR("SELF CROSSINGS"));
            fe_draw_small_text((GEO_STATUS_X + 6.0f) * sx,
                               (GEO_STATUS_Y + 27.0f) * sy, line,
                               GEO_COL_XMARK, sx, sy);
        } else if (s_reason[0]) {
            fe_draw_small_text((GEO_STATUS_X + 6.0f) * sx,
                               (GEO_STATUS_Y + 27.0f) * sy, TR(s_reason),
                               GEO_COL_DIM, sx, sy);
        }
    }

    /* The stub router must never be mistaken for the real one on a
     * screenshot. Said plainly, on the screen, every frame. */
    if (td5_geo_route_is_stub())
        fe_draw_small_text(GEO_STATUS_X * sx, 444.0f * sy,
                           TR("ROUTER NOT LINKED - LINE SHOWN IS NOT A DRIVABLE ROUTE"),
                           GEO_COL_BAD, sx, sy);
}

/* ========================================================================
 * SECTION: the screen
 * ======================================================================== */

static void geo_screen_init(void)
{
    frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
    frontend_reset_buttons();
    frontend_create_button(TR("CLEAR"), GEO_BTN_CLEAR_X, GEO_BTN_Y,
                           GEO_BTN_CLEAR_W, GEO_BTN_H);
    frontend_create_button(TR("BUILD TRACK"), GEO_BTN_BUILD_X, GEO_BTN_Y,
                           GEO_BTN_BUILD_W, GEO_BTN_H);
    frontend_create_button(TR("BACK"), GEO_BTN_BACK_X, GEO_BTN_Y,
                           GEO_BTN_BACK_W, GEO_BTN_H);

    /* Tiles are fetched only between open and close, so the screen is the
     * only thing that can cause a request. */
    td5_geo_tiles_open();

    s_n_places = td5_geo_route_places(s_place_slug, s_place_bbox, GEO_PLACE_MAX);

    /* Open on La Plata unless a cached place says otherwise -- the default
     * Mariano asked for, and the only place the shipped cache can route in. */
    if (s_cen_lat == GEO_HOME_LAT && s_cen_lon == GEO_HOME_LON && s_n_places > 0) {
        int i;
        for (i = 0; i < s_n_places; i++) {
            if (strcmp(s_place_slug[i], "la_plata") == 0) {
                s_cen_lat = (s_place_bbox[i][1] + s_place_bbox[i][3]) * 0.5;
                s_cen_lon = (s_place_bbox[i][0] + s_place_bbox[i][2]) * 0.5;
                break;
            }
        }
    }

#ifndef TD5RE_RELEASE
    /* Dev seed for framedump verification. Control-socket keys do not reach
     * frontend menus and inject_key cannot fabricate a mouse drag, so the only
     * way to see placed points and a drawn route on a screenshot is to put
     * them there. TD5RE_GEO_SEED=1 drops a START, a middle and a FINISH across
     * the centred place. Same device as the CHAOS board's "DEV: FAKE ROSTER"
     * (td5_fe_chaos.c) and compiled out of RELEASE for the same reason. */
    /* td5_env_int, not td5_env_flag_on: an unset knob must mean OFF, and
     * flag_on has bitten this tree before (the R22 water diag that ran on
     * every build). Default 0, range 0..1, no ambiguity. */
    if (s_n_pts == 0 && td5_env_int("TD5RE_GEO_SEED", 0, 0, 1)) {
        const double dlat = 0.010, dlon = 0.013;
        s_pts[0].lat = s_cen_lat - dlat; s_pts[0].lon = s_cen_lon - dlon;
        s_pts[1].lat = s_cen_lat + dlat * 0.3; s_pts[1].lon = s_cen_lon;
        s_pts[2].lat = s_cen_lat + dlat; s_pts[2].lon = s_cen_lon + dlon;
        s_n_pts = 3;
        geo_mark_dirty();
        TD5_LOG_W(LOG_TAG, "GEO GENERATOR: TD5RE_GEO_SEED - seeded 3 DEV points "
                  "(dev build only; not a user action)");
    }
#endif

    s_drag_pt  = -1;
    s_panning  = 0;
    s_prev_lmb = td5_plat_input_mouse_left_down();   /* a click carried in from
                                                        the previous screen must
                                                        not place a point */
    s_prev_rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
    s_moved_while_down = 0;
    (void)td5_plat_input_get_mouse_wheel();          /* drop queued wheel */

    TD5_LOG_I(LOG_TAG, "GEO GENERATOR: open at %.5f,%.5f z%d (%d routable place(s), "
              "router=%s)", s_cen_lat, s_cen_lon, s_zoom, s_n_places,
              td5_geo_route_is_stub() ? "STUB" : "real");

    s_anim_complete = 1;
    s_inner_state   = 1;
}

static void geo_leave(int screen)
{
    td5_geo_tiles_close();

    /* Drain a background build before leaving. Without this the thread handle
     * leaks AND s_bg_busy stays 1 forever, so every later visit to the screen
     * would find the slot occupied, set s_rebuild_wanted and never route
     * again -- a dead screen with no error anywhere. The router is contracted
     * to finish "well under a second", so the wait is bounded; the result is
     * thrown away because the screen is closing. */
    if (s_bg_thread) {
        td5_plat_thread_join(s_bg_thread);
        s_bg_thread = NULL;
    }
    InterlockedExchange(&s_bg_done, 0);
    InterlockedExchange(&s_bg_busy, 0);
    s_rebuild_wanted = 0;

    s_inner_state = 0;
    td5_frontend_set_screen(screen);
}

/* BUILD: commit the route, re-register the place as a track slot, and hand
 * the player back to SELECT TRACK with that slot picked -- the same shape the
 * J8 GEO-PICK place slots use, so nothing downstream learns a new path. */
static void geo_do_build(void)
{
    int n, i, slot = -1;

    if (!geo_route_is_buildable()) { frontend_play_sfx(10); return; }
    if (td5_geo_route_commit() != 0) {
        frontend_play_sfx(10);
        snprintf(s_reason, sizeof(s_reason), "COULD NOT SAVE THE ROUTE");
        s_verdict = TD5_GEO_ROUTE_ERROR;
        return;
    }

    td5_trackgen_register_geo_places();
    n = td5_geo_places_count();
    for (i = 0; i < n; i++) {
        const char *slug = td5_geo_places_slug(i);
        if (slug && s_place[0] && strcmp(slug, s_place) == 0) {
            slot = td5_trackgen_geo_slot_for_index(i);
            break;
        }
    }
    if (slot < 0) {
        TD5_LOG_W(LOG_TAG, "GEO GENERATOR: committed '%s' but it did not register "
                  "as a track slot", s_place);
        frontend_play_sfx(10);
        return;
    }

    TD5_LOG_I(LOG_TAG, "GEO GENERATOR: built '%s' -> track slot %d (%d spans)",
              s_place, slot, s_spans);
    /* SELECT TRACK re-inits on entry (td5_frontend_set_screen zeroes
     * s_inner_state), so setting the shared pick here is all it takes for the
     * column to come up on the new place with the right preview and the right
     * DIRECTION / LAPS rows. */
    s_selected_track = slot;
    frontend_play_sfx(3);
    geo_leave(s_parent_screen);
}

void Screen_GeoGenerator(void)
{
    if (s_inner_state == 0) { geo_screen_init(); return; }

    geo_collect_bg();
    geo_handle_mouse();
    if (s_rebuild_wanted) geo_rebuild_route();

#ifndef TD5RE_RELEASE
    /* Dev-only in-session BUILD -> race, so the one-process path can be
     * verified on a framedump: control-socket keys do not reach frontend menus
     * and inject_key cannot press a vector button. TD5RE_GEO_AUTOBUILD=1 waits
     * for the (seeded, see TD5RE_GEO_SEED) route to condition OK, presses
     * BUILD once, then re-arms AutoRace on the slot it registered -- which is
     * the whole point: it exercises the commit -> re-grid -> reload -> build
     * path WITHOUT a relaunch, the thing a fresh process would paper over.
     * Fires exactly once. Compiled out of RELEASE. */
    {
        static int s_autobuild_fired;
        if (!s_autobuild_fired && td5_env_int("TD5RE_GEO_AUTOBUILD", 0, 0, 1)
            && geo_route_is_buildable()) {
            s_autobuild_fired = 1;
            TD5_LOG_W(LOG_TAG, "GEO GENERATOR: TD5RE_GEO_AUTOBUILD - pressing "
                      "BUILD on the %d-span route (dev build only)", s_spans);
            geo_do_build();                 /* commit + invalidate + register */
            if (s_selected_track >= 0) {
                g_td5.ini.default_track = s_selected_track;
                g_td5.ini.auto_race     = 1;   /* MENU loop fires auto_race_setup */
                TD5_LOG_W(LOG_TAG, "GEO GENERATOR: TD5RE_GEO_AUTOBUILD - armed "
                          "AutoRace on slot %d", s_selected_track);
            }
            return;
        }
    }
#endif

    s_buttons[GEO_BTN_CLEAR].disabled = (s_n_pts > 0) ? 0 : 1;
    s_buttons[GEO_BTN_BUILD].disabled = geo_route_is_buildable() ? 0 : 1;

    if (frontend_check_escape()) {
        frontend_play_sfx(5);
        geo_leave(s_parent_screen);
        return;
    }

    if (s_input_ready) {
        s_input_ready = 0;
        switch (s_button_index) {
        case GEO_BTN_CLEAR:
            if (s_n_pts > 0) {
                s_n_pts = 0;
                geo_clear_result();
                frontend_play_sfx(5);
            } else {
                frontend_play_sfx(10);
            }
            break;
        case GEO_BTN_BUILD:
            geo_do_build();
            return;
        case GEO_BTN_BACK:
            frontend_play_sfx(5);
            geo_leave(s_parent_screen);
            return;
        default:
            break;
        }
    }
}

/* Called by the SELECT TRACK row so BACK and BUILD return where the player
 * came from. Mirrors frontend_autotrack_parent_screen's job for the studio. */
void frontend_geo_generator_set_parent(int screen)
{
    s_parent_screen = screen;
}

int frontend_geo_generator_parent_screen(void)
{
    return s_parent_screen;
}
