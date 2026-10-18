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
#include "td5_roam_mode.h"     /* [GEO FREE ROAM F2-A] the FREE ROAM button */
#include "td5_roam_route.h"    /* [F11a] TD5_ROUTE_MAX_WP / TD5_ROUTE_SPRINT */
#include "td5_roam_handoff.h"  /* [F11a] RACE ROUTE: the pins raced in the city (free roam) */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "frontend"

/* ========================================================================
 * SECTION: layout (640x480 design space)
 * Left margin 112 and the y=460 content floor are the standard bands.
 * ======================================================================== */

/* [ROUND 1030 B item 3] "options for generation on the geo generator should
 * be on the right of the map, make the map smaller and put the buttons on the
 * right with pages and sections". The map gave up the right 244 px of its old
 * 416 to the OPTIONS panel (392..628); the status box follows the map's width
 * and grew a line, because its old one-line readout no longer fits 272 px. */
#define GEO_MAP_X        112.0f
#define GEO_MAP_Y         46.0f
#define GEO_MAP_W        272.0f
#define GEO_MAP_H        286.0f
#define GEO_STATUS_X     112.0f
#define GEO_STATUS_Y     338.0f
#define GEO_STATUS_W     272.0f
#define GEO_STATUS_H      60.0f
#define GEO_BTN_Y        404
#define GEO_BTN_H         32
#define GEO_BTN_CLEAR_X  112
#define GEO_BTN_CLEAR_W   96
#define GEO_BTN_BUILD_X  214
#define GEO_BTN_BUILD_W  140
#define GEO_BTN_BACK_X   360
#define GEO_BTN_BACK_W    96
/* [GEO FREE ROAM F2-A] FREE ROAM sits right of BACK, under the options panel's edge (628). */
#define GEO_BTN_ROAM_X   462
#define GEO_BTN_ROAM_W   166

/* The OPTIONS panel: a section selector, up to GEO_OPT_VIS rows of the
 * current section, a help box for the focused row, and DEFAULTS. */
#define GEO_PANEL_X      392
#define GEO_PANEL_W      236
#define GEO_SECT_Y        46
#define GEO_SECT_H        28
#define GEO_ROW_Y0        80
#define GEO_ROW_STEP      28
#define GEO_ROW_H         24
#define GEO_OPT_VIS        6
#define GEO_HELP_Y       250.0f
#define GEO_HELP_H        82.0f
#define GEO_BTN_DEF_Y    338
#define GEO_BTN_DEF_H     28

/* Button indices. The first three keep their old numbers; the panel's come
 * after them, 1 + GEO_OPT_VIS + 1 more, well inside FE_MAX_BUTTONS (18). */
enum {
    GEO_BTN_CLEAR = 0, GEO_BTN_BUILD, GEO_BTN_BACK,
    GEO_BTN_SECT,
    GEO_BTN_ROW0,
    GEO_BTN_DEFAULTS = GEO_BTN_ROW0 + GEO_OPT_VIS,
    GEO_BTN_ROAM,                  /* [GEO FREE ROAM F2-A] created last, after DEFAULTS */
    GEO_BTN_COUNT
};

/* How many map pixels the pane shows across its width. Fixed rather than
 * "one map pixel per screen pixel" so the tile count stays bounded (5 x 4 at
 * every resolution) instead of growing to 77 tiles at 4K -- which would be a
 * burst of requests at OSM for no visible gain.
 * [ROUND 1030 B] Scaled with the pane: 1024 across the old 416 px pane, so
 * the narrower pane shows LESS ground at the SAME metres per design pixel
 * rather than squeezing the same ground into fewer pixels. */
#define GEO_VIEW_MAP_PX  (1024.0 * (double)GEO_MAP_W / 416.0)

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
/* [ROUND 1030 B item 4] When each point was PLACED, parallel to s_pts. Route
 * order and click order differ once middles exist (a middle is inserted
 * before the finish), and RIGHT CLICK = "go back" undoes the newest PLACE,
 * not the last point along the route. */
static unsigned s_pt_seq[GEO_MAX_PTS];
static unsigned s_seq_next;

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
static int    s_n_path_user;               /* [1030 B item 5] s_path up to FINISH */
static TD5_GeoLatLon s_xmark[GEO_XMARK_MAX];
static int    s_n_xmark;
static int    s_have_route;

/* Routable-area boxes, read once per screen entry. */
static char   s_place_slug[GEO_PLACE_MAX][64];
static double s_place_bbox[GEO_PLACE_MAX][4];   /* W, S, E, N */
static int    s_n_places;
/* [ROUND 1015 F] A place fetched for an administrative area carries its
 * boundary polygon. Drawn as an outline INSTEAD of the bbox wash: a partido is
 * ~40% of its bbox, and shading the corners would promise routing where the
 * place has no data. First ring only (the mainland); lat/lon pairs. */
#define GEO_RING_MAX 400
static double s_place_ring[GEO_PLACE_MAX][GEO_RING_MAX][2];
static int    s_place_ring_n[GEO_PLACE_MAX];

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
static int           s_bg_n_path_user;

/* Where BACK goes, and where BUILD hands off to. Both are SELECT TRACK today;
 * kept as a variable so a future Quick Race entry point can set it like the
 * AUTO TRACK STUDIO chip does (frontend_autotrack_parent_screen). */
static int    s_parent_screen = TD5_SCREEN_TRACK_SELECTION;

/* [GEO FREE ROAM F11a, F11b] 1 = the STRIP geo BUILD is on, read at screen entry: local multiplayer
 * (2+ humans, the only context the strip race is built in) or TD5RE_GEO_STRIP_BUILD=1 (dev). BUILD
 * TRACK and the generation OPTIONS panel as before. 0 (single player) = the button is RACE ROUTE: the
 * same pins raced inside the free-roam city, and the panel, whose rows only shape a strip build,
 * gives way to a short note. docs/plans/geo_free_roam/F11_PARITY.md. */
static int    s_strip;
static int    geo_roam_spawn(TD5_RoamSpawn *out);
static int    geo_race_city_ready(const char **why);

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
                             const TD5_GeoLatLon *path, int n_path, int n_user,
                             const TD5_GeoLatLon *xm, int n_xm)
{
    s_verdict  = r->verdict;
    s_spans    = r->spans;
    s_length_m = r->length_m;
    snprintf(s_reason, sizeof(s_reason), "%s", r->reason);
    snprintf(s_place,  sizeof(s_place),  "%s", r->place_slug);
    s_n_path  = n_path;
    s_n_path_user = n_user;
    s_n_xmark = n_xm;
    if (n_path > 0) memcpy(s_path,  path, (size_t)n_path * sizeof(s_path[0]));
    if (n_xm   > 0) memcpy(s_xmark, xm,   (size_t)n_xm   * sizeof(s_xmark[0]));
    s_have_route = 1;
}

static void geo_clear_result(void)
{
    s_have_route = 0;
    s_n_path = s_n_path_user = s_n_xmark = 0;
    s_spans = 0;
    s_length_m = 0.0f;
    s_place[0] = '\0';
    s_verdict = TD5_GEO_ROUTE_NO_DATA;
    s_reason[0] = '\0';
}

/* Copy a router result into caller-owned arrays. The contract says path /
 * crossings stay valid only until the next call, so nothing may hold them. */
static void geo_snapshot(const TD5_GeoRouteResult *r,
                         TD5_GeoLatLon *path, int *n_path, int *n_user,
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
    *n_user = (r->n_path_user > 0 && r->n_path_user < n) ? r->n_path_user : n;
    *n_xm   = m;
}

static void geo_bg_worker(void *arg)
{
    (void)arg;
    if (td5_geo_route_build(s_bg_in, s_bg_n_in, &s_bg_out) == 0)
        geo_snapshot(&s_bg_out, s_bg_path, &s_bg_n_path, &s_bg_n_path_user,
                     s_bg_xmark, &s_bg_n_xmark);
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
        int np = 0, nu = 0, nx = 0;
        geo_snapshot(&r, path, &np, &nu, xm, &nx);
        geo_adopt_result(&r, path, np, nu, xm, nx);
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
    geo_adopt_result(&s_bg_out, s_bg_path, s_bg_n_path, s_bg_n_path_user,
                     s_bg_xmark, s_bg_n_xmark);
    InterlockedExchange(&s_bg_busy, 0);
    if (s_rebuild_wanted) geo_rebuild_route();
}

/* ========================================================================
 * SECTION: point editing
 * ======================================================================== */

static void geo_mark_dirty(void) { s_rebuild_wanted = 1; }

/* [ROUND 1030 B item 5] "red line on the preview extends past the finish line
 * selected". It did: since round 1013 F3 the router walks a RUN-OFF of
 * TD5RE_GEO_RUNOFF_SPANS (104) spans past the finish so the car has road to
 * stop on, and hands back ONE polyline
 * with that tail on the end. The red line now stops where the player's route
 * ends (TD5_GeoRouteResult.n_path_user); the run-off still exists in the
 * built track, it is just not drawn as if it were part of the route.
 * TD5RE_GEO_MAP_TRIM_RUNOFF=0 draws the whole polyline again. */
static int geo_route_draw_n(void)
{
    if (!td5_env_flag_on("TD5RE_GEO_MAP_TRIM_RUNOFF")) return s_n_path;
    return (s_n_path_user > 0 && s_n_path_user <= s_n_path) ? s_n_path_user : s_n_path;
}

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

static int geo_insert_point(int at, double lat, double lon)
{
    int i;
    if (s_n_pts >= GEO_MAX_PTS) { frontend_play_sfx(10); return 0; }
    if (at < 0) at = 0;
    if (at > s_n_pts) at = s_n_pts;
    for (i = s_n_pts; i > at; i--) {
        s_pts[i] = s_pts[i - 1];
        s_pt_seq[i] = s_pt_seq[i - 1];
    }
    s_pts[at].lat = lat;
    s_pts[at].lon = lon;
    s_pt_seq[at] = s_seq_next++;
    s_n_pts++;
    geo_mark_dirty();
    return 1;
}

static void geo_remove_point(int idx)
{
    int i;
    if (idx < 0 || idx >= s_n_pts) return;
    for (i = idx; i < s_n_pts - 1; i++) {
        s_pts[i] = s_pts[i + 1];
        s_pt_seq[i] = s_pt_seq[i + 1];
    }
    s_n_pts--;
    geo_mark_dirty();
}

static void geo_clear_points(void)
{
    s_n_pts = 0;
    s_seq_next = 0;
}

/* [ROUND 1030 B item 4] "first click should be the start, second click should
 * be the finish, third click and so on should be intermediate points, right
 * click should go back". TD5RE_GEO_CLICK_ORDER (default ON); =0 restores the
 * round-1007 rules: a click off the route APPENDED a point past the finish
 * (so every click moved the finish) and right click deleted the marker under
 * the cursor. */
static int geo_click_order_on(void) { return td5_env_flag_on("TD5RE_GEO_CLICK_ORDER"); }

/* Where a new MIDDLE point goes: the leg whose detour it lengthens least
 * (cheapest insertion), measured in map pixels at the current zoom, which is
 * planar to well under a percent over a city. Returns an index in
 * 1 .. s_n_pts-1, so a middle can never land before START or after FINISH. */
static int geo_cheapest_leg(double lat, double lon)
{
    const double px = geo_lon_to_mx(lon, s_zoom), py = geo_lat_to_my(lat, s_zoom);
    double best = -1.0;
    int i, at = s_n_pts - 1;
    for (i = 0; i + 1 < s_n_pts; i++) {
        const double ax = geo_lon_to_mx(s_pts[i].lon, s_zoom);
        const double ay = geo_lat_to_my(s_pts[i].lat, s_zoom);
        const double bx = geo_lon_to_mx(s_pts[i + 1].lon, s_zoom);
        const double by = geo_lat_to_my(s_pts[i + 1].lat, s_zoom);
        const double c = hypot(px - ax, py - ay) + hypot(bx - px, by - py)
                       - hypot(bx - ax, by - ay);
        if (best < 0.0 || c < best) { best = c; at = i + 1; }
    }
    return at;
}

/* A click that PLACES a point (not a drag, not on a marker). `leg` is what
 * geo_hit_route_leg said for the click, -1 when it was off the route. Returns
 * the route index the point went to, -1 when it was refused. */
static int geo_place_click(double lat, double lon, int leg)
{
    int at;
    if (!geo_click_order_on()) {
        /* round-1007 rules, kept byte for byte behind the knob */
        if (leg >= 0 && s_n_pts >= 2) at = leg;
        else                          at = s_n_pts;
    } else if (s_n_pts < 2) {
        at = s_n_pts;                                   /* START, then FINISH */
    } else {
        /* a middle: on the leg the player clicked, else the cheapest one --
         * never past the finish, which stays where the second click put it */
        at = (leg >= 1 && leg <= s_n_pts - 1) ? leg : geo_cheapest_leg(lat, lon);
    }
    return geo_insert_point(at, lat, lon) ? at : -1;
}

/* RIGHT CLICK = go back: drop the most recently PLACED point. Undoing in
 * placement order means a middle goes before the finish, and the finish
 * before the start, which is exactly the reverse of how they were put down.
 * Returns the route index it removed, -1 when there was nothing to undo. */
static int geo_undo_last(void)
{
    int i, newest = -1;
    for (i = 0; i < s_n_pts; i++)
        if (newest < 0 || s_pt_seq[i] > s_pt_seq[newest]) newest = i;
    if (newest < 0) return -1;
    geo_remove_point(newest);
    if (s_n_pts == 0) s_seq_next = 0;
    return newest;
}

#ifndef TD5RE_RELEASE
/* Stamp a route that arrived all at once (the dev seeds) in the order a player
 * would have clicked it: START, FINISH, then the middles along the route. */
static void geo_stamp_route_order(void)
{
    int i;
    s_seq_next = 0;
    if (s_n_pts <= 0) return;
    s_pt_seq[0] = s_seq_next++;
    if (s_n_pts >= 2) s_pt_seq[s_n_pts - 1] = s_seq_next++;
    for (i = 1; i < s_n_pts - 1; i++) s_pt_seq[i] = s_seq_next++;
}
#endif

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
    if (s_have_route && geo_route_draw_n() > 1) {
        float hit_d2 = GEO_INSERT_R * GEO_INSERT_R;
        int   hit = -1;
        const int n_drawn = geo_route_draw_n();   /* not the undrawn run-off */
        for (i = 0; i < n_drawn; i++) {
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

    /* Right click: go back one placement ([ROUND 1030 B item 4]); with
     * TD5RE_GEO_CLICK_ORDER=0, remove the marker under the cursor. Not while
     * a marker is being dragged: undoing the point under the hand would leave
     * s_drag_pt pointing at a different one. */
    if (r_edge && over_map && s_drag_pt < 0) {
        const int hit = geo_click_order_on() ? geo_undo_last()
                                             : geo_pick_point(dx, dy, sx, sy);
        if (hit >= 0) {
            if (!geo_click_order_on()) geo_remove_point(hit);
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
            geo_design_to_latlon(dx, dy, sx, sy, &lat, &lon);
            if (geo_place_click(lat, lon, geo_hit_route_leg(dx, dy, sx, sy)) >= 0)
                frontend_play_sfx(3);
        }
        (void)was_drag;
        s_moved_while_down = 0;
    }
}

/* ========================================================================
 * SECTION: generation options ([ROUND 1030 B item 3])
 *
 * Every row is an EXISTING TD5RE_* knob the geo build already reads; nothing
 * here adds generation behaviour. Same mechanism as the AUTO TRACK STUDIO
 * (k_at_rows, td5_fe_race.c): the row writes the knob with _putenv_s, the
 * generator reads it with getenv when the race launches, and tg_env_hash
 * folds every TD5RE_* variable into the level's GENSTAMP, so a changed option
 * is a stamp miss and level09x is REBUILT rather than reused.
 *
 * A row set back to its default REMOVES the variable instead of writing the
 * default's digits, so an untouched panel leaves the environment exactly as
 * it found it: same env hash, same reused level, same bytes as master.
 *
 * Two rows (AVENUES, RUN-OFF) are read by the ROUTER rather than the track
 * builder -- td5_geo_route_build / commit -- so changing them re-routes the
 * pins at once and the status box shows the new verdict. The rest act when
 * the track is built.
 *
 * NOT persisted (same call as the studio): per-session generator settings,
 * and an env var set on the command line for an A/B must still win.
 * ======================================================================== */

typedef enum { GEO_OPT_INT = 0, GEO_OPT_FLAG_ON } GeoOptKind;

typedef struct {
    const char *label;
    const char *knob;
    const int  *values;
    const char *const *names;
    int         n;
    int         def;        /* what the reader does when the knob is unset    */
    unsigned char kind;     /* GeoOptKind: how an existing value is parsed    */
    unsigned char route;    /* 1 = read by the router: re-route on change     */
    signed char parent;     /* row that must be ON for this one to matter     */
    const char *help1, *help2;
} GeoOpt;

static const int k_go_off_on_v[] = { 0, 1 };
static const char *const k_go_off_on_n[] = { "OFF", "ON" };
/* TD5RE_AUTOTRACK_NIGHT: 2 is the reader's own "derive from the seed", which is
 * also what unset means (tg_decide_night), so AUTO is the default entry. */
static const int k_go_night_v[] = { 2, 0, 1 };
static const char *const k_go_night_n[] = { "AUTO", "DAY", "NIGHT" };
/* TD5RE_GEO_EXAGGERATION x100, clamp 100..400, default 150 (td5_geo.h). */
static const int k_go_hills_v[] = { 100, 150, 200, 300 };
static const char *const k_go_hills_n[] = { "REAL", "STANDARD", "HIGH", "EXTREME" };
/* TD5RE_GEO_RUNOFF_SPANS, clamp 0..400, default 104 (GR_RO_SPANS_DEFAULT). */
static const int k_go_runoff_v[] = { 0, 50, 104, 200 };
static const char *const k_go_runoff_n[] = { "NONE", "SHORT", "STANDARD", "LONG" };
/* TD5RE_GEO_BLOCKLOOP_MAX, clamp 0..BL_MAXPLAN, default 6. */
static const int k_go_loops_v[] = { 1, 2, 3, 4, 6 };
static const char *const k_go_loops_n[] = { "1", "2", "3", "4", "6" };
/* TD5RE_GEO_PARK_TREES: ON = the big per-park cap (td5_tg_streets.c). */
static const char *const k_go_trees_n[] = { "SPARSE", "DENSE" };
/* TD5RE_GEO_LAMP_SPACING_M, clamp 8..200, default 30. */
static const int k_go_lamps_v[] = { 15, 30, 60, 120 };
static const char *const k_go_lamps_n[] = { "DENSE", "STANDARD", "SPARSE", "RARE" };

/* Every `def` checked against its reader on master 78a6b25f:
 * td5_env_flag_on -> ON, td5_env_int -> its stated default. */
static const GeoOpt k_geo_opts[] = {
    /* ---------------------------------------------------------- ROUTE --- */
    { "TIME OF DAY",    "TD5RE_AUTOTRACK_NIGHT",     k_go_night_v,  k_go_night_n,  3, 2,   GEO_OPT_INT,     0, -1,
      "Day or night race. AUTO picks",   "from the track seed." },
    { "HILLS",          "TD5RE_GEO_EXAGGERATION",    k_go_hills_v,  k_go_hills_n,  4, 150, GEO_OPT_INT,     0, -1,
      "How much the real relief is",     "exaggerated. REAL = true heights." },
    { "AVENUES",        "TD5RE_GEO_AVENUES",         k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 1, -1,
      "Build divided avenues with the",  "real median where the map has one." },
    { "RUN-OFF",        "TD5RE_GEO_RUNOFF_SPANS",    k_go_runoff_v, k_go_runoff_n, 4, 104, GEO_OPT_INT,     1, -1,
      "Road kept past the finish line",  "to slow down on." },
    /* ---------------------------------------------------------- FORKS --- */
#define GO_ROW_FORKS 4
    { "FORKS",          "TD5RE_GEO_FORKS",           k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, -1,
      "Driveable forks where the real",  "map has a second road beside you." },
#define GO_ROW_LOOPS 5
    { "BLOCK LOOPS",    "TD5RE_GEO_FORK_BLOCKLOOP",  k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, GO_ROW_FORKS,
      "Alternate roads: turn off and",   "drive round a city block." },
    { "MAX LOOPS",      "TD5RE_GEO_BLOCKLOOP_MAX",   k_go_loops_v,  k_go_loops_n,  5, 6,   GEO_OPT_INT,     0, GO_ROW_LOOPS,
      "Most block loops one track",      "may have." },
    { "LEFT LOOPS",     "TD5RE_GEO_BLOCKLOOP_LEFT",  k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, GO_ROW_LOOPS,
      "Allow loops that leave the route", "to the left as well." },
    { "PLAZA FORKS",    "TD5RE_GEO_FORK_PLAZA",      k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, GO_ROW_FORKS,
      "A second way round a plaza",      "where the street circles one." },
    { "LEFT FORKS",     "TD5RE_GEO_FORK_LEFT",       k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, GO_ROW_FORKS,
      "Offer forks on the left side",    "of the road too." },
    /* -------------------------------------------------------- STREETS --- */
#define GO_ROW_SIGNALS 10
    { "TRAFFIC LIGHTS", "TD5RE_GEO_SIGNALS",         k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, -1,
      "Traffic lights at the junctions", "the map marks as signalled." },
    { "MAST ARMS",      "TD5RE_GEO_MASTARM",         k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, GO_ROW_SIGNALS,
      "Lights hung over the road on an", "arm, with the street name plate." },
    { "LIVE SIGNALS",   "TD5RE_GEO_SIGNAL_LIVE",     k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, GO_ROW_SIGNALS,
      "Signal heads count down and",     "change. OFF = a fixed display." },
    { "CROSSWALKS",     "TD5RE_GEO_XINGS",           k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, -1,
      "Zebra crossings and stop lines",  "from the map." },
    { "BUS STOPS",      "TD5RE_GEO_BUSSTOPS",        k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, -1,
      "Shelters, benches and bins at",   "the map's bus stops." },
    { "FOOTPATHS",      "TD5RE_GEO_FOOTPATHS",       k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, -1,
      "Pedestrian paths from the map,",  "in parks and plazas." },
    /* ----------------------------------------------------------- CITY --- */
#define GO_ROW_CITY 16
    { "CITY",           "TD5RE_GEO_CITY",            k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, -1,
      "Real city scenery from the map.", "BUILDINGS and PLAZAS need it." },
    { "BUILDINGS",      "TD5RE_GEO_BUILDINGS",       k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, GO_ROW_CITY,
      "Real building footprints and",    "heights." },
    { "PLAZAS",         "TD5RE_GEO_PLAZAS",          k_go_off_on_v, k_go_off_on_n, 2, 1,   GEO_OPT_FLAG_ON, 0, GO_ROW_CITY,
      "Parks and plazas from the map,",  "with lawns and paths." },
    { "PARK TREES",     "TD5RE_GEO_PARK_TREES",      k_go_off_on_v, k_go_trees_n,  2, 1,   GEO_OPT_FLAG_ON, 0, GO_ROW_CITY,
      "How many trees a park may",       "carry." },
    { "STREET LAMPS",   "TD5RE_GEO_LAMP_SPACING_M",  k_go_lamps_v,  k_go_lamps_n,  4, 30,  GEO_OPT_INT,     0, -1,
      "Distance between lamp posts on",  "lit streets (15/30/60/120 m)." },
};
#define GEO_OPTS ((int)(sizeof(k_geo_opts) / sizeof(k_geo_opts[0])))

typedef struct { const char *name; int first, count; } GeoOptSect;
static const GeoOptSect k_geo_sects[] = {
    { "ROUTE",   0,  4 },
    { "FORKS",   4,  6 },
    { "STREETS", 10, 6 },
    { "CITY",    16, 5 },
};
#define GEO_SECTS ((int)(sizeof(k_geo_sects) / sizeof(k_geo_sects[0])))

static int s_opt_sect;                      /* current section */

/* The value the reader would use right now: the env var parsed the way the
 * reader parses it, or `def` when unset. */
static int geo_opt_value(int row)
{
    const GeoOpt *o = &k_geo_opts[row];
    const char *e = getenv(o->knob);
    if (!e || !e[0]) return o->def;
    if (o->kind == GEO_OPT_FLAG_ON) return (e[0] == '0') ? 0 : 1;
    return atoi(e);
}

/* Index of the current value in the row's list; -1 = a value set outside the
 * screen (command line) that the list does not carry, shown as its number. */
static int geo_opt_index(int row)
{
    const GeoOpt *o = &k_geo_opts[row];
    const int v = geo_opt_value(row);
    int i;
    for (i = 0; i < o->n; i++) if (o->values[i] == v) return i;
    return -1;
}

static void geo_opt_write(int row, int value)
{
    const GeoOpt *o = &k_geo_opts[row];
    char buf[24];
    if (value == o->def) {
        _putenv_s(o->knob, "");             /* removes it: default = unset */
    } else {
        snprintf(buf, sizeof buf, "%d", value);
        _putenv_s(o->knob, buf);
    }
    TD5_LOG_I(LOG_TAG, "GEO GENERATOR: option %s -> %d (%s=%s)", o->label, value,
              o->knob, getenv(o->knob) ? getenv(o->knob) : "<unset>");
    if (o->route && s_n_pts >= 2) geo_mark_dirty();
}

/* Step a row one value along its list, wrapping. */
static void geo_opt_step(int row, int d)
{
    const GeoOpt *o = &k_geo_opts[row];
    int i = geo_opt_index(row);
    if (i < 0) i = (d > 0) ? -1 : 0;        /* off-list value: enter the list */
    i = (i + d + o->n) % o->n;
    geo_opt_write(row, o->values[i]);
}

/* 1 when the row's parent is switched off, so the row has no effect. */
static int geo_opt_inactive(int row)
{
    const int p = k_geo_opts[row].parent;
    return (p >= 0 && geo_opt_value(p) == 0) ? 1 : 0;
}

static int geo_opt_row_at(int slot)
{
    const GeoOptSect *s = &k_geo_sects[s_opt_sect];
    return (slot >= 0 && slot < s->count) ? s->first + slot : -1;
}

/* Show the current section's rows on the GEO_OPT_VIS row buttons. Fixed
 * buttons hidden past the section's end, so a page flip never rebuilds the
 * button table and the focus stays where the player left it. */
static void geo_opt_layout_rows(void)
{
    int k;
    for (k = 0; k < GEO_OPT_VIS; k++) {
        FE_Button *b = &s_buttons[GEO_BTN_ROW0 + k];
        /* [F11a] without the strip build only slot 0 of the ROUTE page (TIME OF DAY, which the roam
         * session reads too: td5_trackgen_roam_is_night) stays; the other rows only shape a strip build */
        const int on = (s_strip || k == 0) && (geo_opt_row_at(k) >= 0);
        b->hidden   = on ? 0 : 1;
        b->disabled = on ? 0 : 1;
    }
    if (s_selected_button >= GEO_BTN_ROW0 && s_selected_button < GEO_BTN_ROW0 + GEO_OPT_VIS
        && s_buttons[s_selected_button].disabled)
        s_selected_button = GEO_BTN_SECT;
}

static void geo_opt_set_sect(int sect)
{
    s_opt_sect = s_strip ? ((sect % GEO_SECTS) + GEO_SECTS) % GEO_SECTS : 0;   /* [F11a] city: ROUTE page only */
    geo_opt_layout_rows();
}

static void geo_opt_defaults(void)
{
    int i;
    for (i = 0; i < GEO_OPTS; i++)
        if (getenv(k_geo_opts[i].knob)) geo_opt_write(i, k_geo_opts[i].def);
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
        if (s_place_ring_n[i] >= 4) {
            /* the polygon, not the box -- see s_place_ring */
            int k, budget = 4000;
            float px = 0.0f, py = 0.0f;
            for (k = 0; k < s_place_ring_n[i] && budget > 0; k++) {
                float qx, qy;
                geo_latlon_to_design(s_place_ring[i][k][0], s_place_ring[i][k][1],
                                     sx, sy, &qx, &qy);
                if (k > 0) {
                    const int out_l = (px < GEO_MAP_X && qx < GEO_MAP_X);
                    const int out_r = (px > GEO_MAP_X + GEO_MAP_W && qx > GEO_MAP_X + GEO_MAP_W);
                    const int out_t = (py < GEO_MAP_Y && qy < GEO_MAP_Y);
                    const int out_b = (py > GEO_MAP_Y + GEO_MAP_H && qy > GEO_MAP_Y + GEO_MAP_H);
                    if (!(out_l || out_r || out_t || out_b))
                        budget -= geo_stamp_line(px, py, qx, qy, 2.0f,
                                                 GEO_COL_SHADE_ED, sx, sy, budget);
                }
                px = qx; py = qy;
            }
            continue;
        }
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
    if (s_have_route && geo_route_draw_n() > 1) {
        float px = 0.0f, py = 0.0f;
        int first = 1;
        const int n_drawn = geo_route_draw_n();   /* [1030 B item 5] up to FINISH */
        for (i = 0; i < n_drawn && budget > 0; i++) {
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

/* Small text centred on a design-space x. fe_measure_small_text is in glyph
 * units, drawn at min(sx, sy) (see mp_pos_small_centered, td5_fe_race.c). */
static void geo_small_centered(float cx, float y, const char *t, uint32_t col,
                               float sx, float sy)
{
    const float gsx = (sx < sy) ? sx : sy;
    fe_draw_small_text(cx * sx - fe_measure_small_text(t) * 0.5f * gsx, y * sy,
                       t, col, sx, sy);
}

/* Greedy word wrap of small text to `maxw` DESIGN px; returns the line count.
 * Needed because the glyph width follows min(sx, sy): a line that fits at
 * 16:9 is a third wider at 4:3, and the boxes here are only 272 / 236 wide. */
#define GEO_WRAP_LEN 96
static int geo_wrap_small(const char *s, float maxw, float sx, float sy,
                          char out[][GEO_WRAP_LEN], int max_lines)
{
    const float k = ((sx < sy) ? sx : sy) / sx;   /* glyph units -> design px */
    int n = 0;
    size_t cl = 0;                                /* length of out[n] so far */
    while (*s && n < max_lines) {
        char trial[GEO_WRAP_LEN];
        size_t w = 0, tl;
        const char *wd;
        while (*s == ' ') s++;
        wd = s;
        while (s[w] && s[w] != ' ') w++;
        if (!w) break;
        s += w;
        if (w > GEO_WRAP_LEN / 2) w = GEO_WRAP_LEN / 2;   /* clip a runaway word */
        /* the line with this word added, if it still fits the buffer */
        tl = cl + (cl ? 1 : 0) + w;
        if (cl && tl < GEO_WRAP_LEN) {
            memcpy(trial, out[n], cl);
            trial[cl] = ' ';
            memcpy(trial + cl + 1, wd, w);
            trial[tl] = '\0';
        }
        if (cl && (tl >= GEO_WRAP_LEN || fe_measure_small_text(trial) * k > maxw)) {
            if (++n >= max_lines) break;           /* out[n-1] is complete */
            cl = 0;
        }
        if (cl) { out[n][cl] = ' '; cl++; }
        memcpy(out[n] + cl, wd, w);
        cl += w;
        out[n][cl] = '\0';
    }
    if (cl && n < max_lines) n++;
    return n;
}

/* [ROUND 1030 B item 3] The OPTIONS panel right of the map. The frames come
 * from the shared button render (the section selector and the rows are
 * is_selector buttons, so they get the standard ◄► and LEFT/RIGHT cycling);
 * this draws what is written on them and the help box under them. */
/* [GEO FREE ROAM F11a] Where the strip OPTIONS panel was: what RACE ROUTE and FREE ROAM do now. */
static void geo_draw_city_note(float sx, float sy)
{
    static const char *k_note[] = {
        "RACE ROUTE races these pins inside the free-roam city.",
        "Next: pick a car, then opponents, traffic and cops on the route map.",
        "Sprint or circuit and laps: on the same panel.",
        "FREE ROAM drives from START with no race.",
    };
    const float px = (float)GEO_PANEL_X, pw = (float)GEO_PANEL_W;
    const char *why = NULL;
    const int ready = geo_race_city_ready(&why);
    float y = (float)(GEO_ROW_Y0 + GEO_ROW_STEP) + 10.0f;
    int i, j;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    geo_fill(px, (float)GEO_SECT_Y, pw, 286.0f, 0xC0101018u, sx, sy);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    geo_frame_rect(px, (float)GEO_SECT_Y, pw, 286.0f, GEO_COL_FRAME, sx, sy);
    fe_draw_text_centered((px + pw * 0.5f) * sx, ((float)GEO_SECT_Y + 6.0f) * sy,
                          TR("RACE IN THE CITY"), 0xFFE3D708u, sx, sy);
    {   /* TIME OF DAY (row 0, the one option the city race reads), drawn like a panel row */
        const int row = geo_opt_row_at(0);
        if (row >= 0) {
            const GeoOpt *o = &k_geo_opts[row];
            const int idx = geo_opt_index(row);
            char line[32];
            const float ry = (float)GEO_ROW_Y0;
            fe_draw_option_arrows(GEO_BTN_ROW0, sx, sy);
            fe_draw_small_text((px + 22.0f) * sx, (ry + 7.0f) * sy, TR(o->label), 0xFFE6ECF4u, sx, sy);
            if (idx >= 0) snprintf(line, sizeof line, "%s", TR(o->names[idx]));
            else          snprintf(line, sizeof line, "%d", geo_opt_value(row));
            geo_small_centered(px + 176.0f, ry + 7.0f, line,
                               geo_opt_value(row) != o->def ? 0xFF7CE07Cu : 0xFFFFFFFFu, sx, sy);
        }
    }
    for (i = 0; i < (int)(sizeof k_note / sizeof k_note[0]); i++) {
        char wl[3][GEO_WRAP_LEN];
        const int n = geo_wrap_small(TR(k_note[i]), pw - 12.0f, sx, sy, wl, 3);
        for (j = 0; j < n; j++, y += 13.0f)
            fe_draw_small_text((px + 6.0f) * sx, y * sy, wl[j], 0xFFE6ECF4u, sx, sy);
        y += 8.0f;
    }
    {
        char wl[2][GEO_WRAP_LEN];
        const int n = geo_wrap_small(ready ? TR("READY: PRESS RACE ROUTE") : TR(why ? why : ""),
                                     pw - 12.0f, sx, sy, wl, 2);
        y = (float)GEO_SECT_Y + 286.0f - 32.0f;
        for (j = 0; j < n; j++, y += 13.0f)
            fe_draw_small_text((px + 6.0f) * sx, y * sy, wl[j], ready ? GEO_COL_OK : GEO_COL_BAD, sx, sy);
    }
}

static void geo_draw_panel(float sx, float sy)
{
    const float px = (float)GEO_PANEL_X, pw = (float)GEO_PANEL_W;
    const GeoOptSect *sec = &k_geo_sects[s_opt_sect];
    const int focus = s_selected_button - GEO_BTN_ROW0;
    const int frow = (focus >= 0 && focus < GEO_OPT_VIS) ? geo_opt_row_at(focus) : -1;
    char line[64];
    int k;

    if (!s_strip) { geo_draw_city_note(sx, sy); return; }   /* [F11a] */

    /* section selector: name centred, page count by the right arrow */
    fe_draw_option_arrows(GEO_BTN_SECT, sx, sy);
    fe_draw_text_centered((px + pw * 0.5f) * sx,
                          ((float)GEO_SECT_Y + (float)GEO_SECT_H * 0.5f - FE_TEXT_CAP_MID) * sy,
                          TR(sec->name), 0xFFE3D708u, sx, sy);
    snprintf(line, sizeof line, "%d/%d", s_opt_sect + 1, GEO_SECTS);
    fe_draw_small_text((px + pw - 46.0f) * sx, ((float)GEO_SECT_Y + 10.0f) * sy,
                       line, GEO_COL_DIM, sx, sy);

    for (k = 0; k < GEO_OPT_VIS; k++) {
        const int row = geo_opt_row_at(k);
        const float ry = (float)(GEO_ROW_Y0 + k * GEO_ROW_STEP);
        const GeoOpt *o;
        int idx, off;
        if (row < 0) continue;
        o = &k_geo_opts[row];
        idx = geo_opt_index(row);
        off = geo_opt_inactive(row);
        fe_draw_option_arrows(GEO_BTN_ROW0 + k, sx, sy);
        fe_draw_small_text((px + 22.0f) * sx, (ry + 7.0f) * sy, TR(o->label),
                           off ? 0xFF6A7280u : 0xFFE6ECF4u, sx, sy);
        if (idx >= 0) snprintf(line, sizeof line, "%s", TR(o->names[idx]));
        else          snprintf(line, sizeof line, "%d", geo_opt_value(row));
        /* A value moved off its default is drawn green, so a glance at a page
         * says what this build will do differently from the stock one. */
        geo_small_centered(px + 176.0f, ry + 7.0f, line,
                           off ? 0xFF6A7280u
                               : (geo_opt_value(row) != o->def ? 0xFF7CE07Cu : 0xFFFFFFFFu),
                           sx, sy);
    }

    /* help box for the focused row */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    geo_fill(px, GEO_HELP_Y, pw, GEO_HELP_H, 0xC0101018u, sx, sy);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    geo_frame_rect(px, GEO_HELP_Y, pw, GEO_HELP_H, GEO_COL_FRAME, sx, sy);
    if (frow >= 0) {
        const GeoOpt *o = &k_geo_opts[frow];
        char help[2 * GEO_WRAP_LEN], wl[3][GEO_WRAP_LEN];
        float y = GEO_HELP_Y + 5.0f;
        int nl, j;
        fe_draw_small_text((px + 6.0f) * sx, y * sy, TR(o->label), 0xFFE3D708u, sx, sy);
        y += 14.0f;
        snprintf(help, sizeof help, "%s %s", TR(o->help1), TR(o->help2));
        nl = geo_wrap_small(help, pw - 12.0f, sx, sy, wl, 3);
        for (j = 0; j < nl; j++, y += 12.0f)
            fe_draw_small_text((px + 6.0f) * sx, y * sy, wl[j], 0xFFE6ECF4u, sx, sy);
        y = GEO_HELP_Y + GEO_HELP_H - 16.0f;
        if (geo_opt_inactive(frow)) {
            snprintf(line, sizeof line, "%s %s", TR("NEEDS"), TR(k_geo_opts[o->parent].label));
            fe_draw_small_text((px + 6.0f) * sx, y * sy, line, GEO_COL_BAD, sx, sy);
        } else {
            fe_draw_small_text((px + 6.0f) * sx, y * sy,
                               o->route ? TR("CHANGES THE ROUTE NOW")
                                        : TR("USED WHEN THE TRACK IS BUILT"),
                               GEO_COL_DIM, sx, sy);
        }
    } else {
        fe_draw_small_text((px + 6.0f) * sx, (GEO_HELP_Y + 5.0f) * sy,
                           TR("GENERATION OPTIONS"), 0xFFE3D708u, sx, sy);
        char wl[3][GEO_WRAP_LEN];
        int nl, j;
        nl = geo_wrap_small(TR("LEFT / RIGHT CHANGES A VALUE. CHANGED VALUES SHOW GREEN."),
                            pw - 12.0f, sx, sy, wl, 3);
        for (j = 0; j < nl; j++)
            fe_draw_small_text((px + 6.0f) * sx, (GEO_HELP_Y + 19.0f + 12.0f * (float)j) * sy,
                               wl[j], 0xFFE6ECF4u, sx, sy);
        fe_draw_small_text((px + 6.0f) * sx, (GEO_HELP_Y + GEO_HELP_H - 16.0f) * sy,
                           TR("USED WHEN THE TRACK IS BUILT"), GEO_COL_DIM, sx, sy);
    }
}

void frontend_geo_generator_render(float sx, float sy)
{
    char line[160];

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

    /* [ROUND 1030 B item 6] "remove indicator on the top right for ZOOM
     * POINTS METERS". The ZOOM / POINTS / metres readout that sat right of
     * the map is gone (the panel lives there now). The tile state it also
     * carried is kept, because a map that is still loading or offline must
     * say so: a small tag at the map's TOP-LEFT corner, on its own plate,
     * only while there is something to say. (Not on the credit plate: at
     * 4:3 the credit runs two thirds of the 272 px pane and the two met.) */
    {
        const int pend = td5_geo_tiles_pending();
        const int fail = td5_geo_tiles_failed();
        const float gk = ((sx < sy) ? sx : sy) / sx;
        if (pend > 0) snprintf(line, sizeof(line), "%s %d", TR("LOADING"), pend);
        else if (fail > 0) snprintf(line, sizeof(line), "%s", TR("OFFLINE"));
        else line[0] = '\0';
        if (line[0]) {
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            geo_fill(GEO_MAP_X + 1.0f, GEO_MAP_Y + 1.0f,
                     fe_measure_small_text(line) * gk + 8.0f, 14.0f, 0x99101018u, sx, sy);
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            fe_draw_small_text((GEO_MAP_X + 4.0f) * sx, (GEO_MAP_Y + 2.0f) * sy, line,
                               pend > 0 ? 0xFFE3D708u : GEO_COL_BAD, sx, sy);
        }
    }

    /* ---- status ----
     * [ROUND 1030 B] Three short lines now: the box is 272 px wide, and the
     * old one-line readout (verdict + spans + km + place side by side) was
     * sized for 516. */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    geo_fill(GEO_STATUS_X, GEO_STATUS_Y, GEO_STATUS_W, GEO_STATUS_H,
             0xC0101018u, sx, sy);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    geo_frame_rect(GEO_STATUS_X, GEO_STATUS_Y, GEO_STATUS_W, GEO_STATUS_H,
                   GEO_COL_FRAME, sx, sy);
    {
        const float lx = GEO_STATUS_X + 6.0f;
        const int order = geo_click_order_on();
        /* every hint <= ~27 characters: the width that fits 260 px at 4:3 */
        if (s_n_pts < 2) {
            fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 5.0f) * sy,
                               s_n_pts == 0 ? TR("CLICK TO PLACE THE START")
                                            : TR("CLICK TO PLACE THE FINISH"),
                               0xFFE8ECF0u, sx, sy);
            fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 19.0f) * sy,
                               order ? TR("MORE CLICKS ADD MIDDLES")
                                     : TR("CLICK LINE: MIDDLE POINT"),
                               GEO_COL_DIM, sx, sy);
            fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 33.0f) * sy,
                               order ? TR("RIGHT CLICK: UNDO")
                                     : TR("RIGHT CLICK: REMOVE"),
                               GEO_COL_DIM, sx, sy);
            fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 47.0f) * sy,
                               TR("WHEEL ZOOMS, DRAG PANS"),
                               GEO_COL_DIM, sx, sy);
        } else if (!s_strip) {
            /* [F11a] the race is planned again in the city: the strip router's verdict (spans, self
             * crossings) does not apply, only whether the pins can be raced there */
            const char *why = NULL;
            const int ready = geo_race_city_ready(&why);
            fe_draw_text(lx * sx, (GEO_STATUS_Y - 3.0f) * sy,
                         ready ? TR("READY TO RACE") : TR("CANNOT RACE"),
                         ready ? GEO_COL_OK : GEO_COL_BAD, sx, sy);
            if (s_have_route && s_length_m > 0.0f)
                snprintf(line, sizeof(line), "%s %.1f KM   %d %s", TR("ABOUT"),
                         (double)s_length_m / 1000.0, s_n_pts, TR("POINTS"));
            else
                snprintf(line, sizeof(line), "%d %s", s_n_pts, TR("POINTS"));
            fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 21.0f) * sy, line, GEO_COL_DIM, sx, sy);
            if (!ready && why)
                fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 34.0f) * sy, TR(why), GEO_COL_BAD, sx, sy);
            if (order)
                fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 47.0f) * sy,
                                   TR("RIGHT CLICK: UNDO"), GEO_COL_DIM, sx, sy);
        } else {
            const uint32_t vc = (s_verdict == TD5_GEO_ROUTE_OK) ? GEO_COL_OK : GEO_COL_BAD;
            fe_draw_text(lx * sx, (GEO_STATUS_Y - 3.0f) * sy, geo_verdict_text(), vc, sx, sy);
            snprintf(line, sizeof(line), "%d / %d %s   %.1f KM",
                     s_spans, TD5_GEO_ROUTE_MAX_SPANS, TR("SPANS"),
                     (double)s_length_m / 1000.0);
            fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 21.0f) * sy,
                               line, GEO_COL_DIM, sx, sy);
            if (s_n_xmark > 0) {
                snprintf(line, sizeof(line), "%d %s", s_n_xmark, TR("SELF CROSSINGS"));
                fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 34.0f) * sy, line,
                                   GEO_COL_XMARK, sx, sy);
            } else if (s_reason[0]) {
                /* router reasons run to ~80 characters: two wrapped lines */
                char wl[2][GEO_WRAP_LEN];
                const int nl = geo_wrap_small(TR(s_reason), GEO_STATUS_W - 12.0f,
                                              sx, sy, wl, 2);
                int j;
                for (j = 0; j < nl; j++)
                    fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 34.0f + 13.0f * (float)j) * sy,
                                       wl[j], GEO_COL_DIM, sx, sy);
            } else {
                snprintf(line, sizeof(line), "%s   %d %s", s_place[0] ? s_place : "-",
                         s_n_pts, TR("POINTS"));
                fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 34.0f) * sy, line,
                                   GEO_COL_DIM, sx, sy);
            }
            if (order && !s_reason[0])
                fe_draw_small_text(lx * sx, (GEO_STATUS_Y + 47.0f) * sy,
                                   TR("RIGHT CLICK: UNDO"), GEO_COL_DIM, sx, sy);
        }
    }

    geo_draw_panel(sx, sy);

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

#ifndef TD5RE_RELEASE
/* [ROUND 1008] The dev seed, now parameterised by VARIANT so one process can
 * build several DIFFERENT routes in a row -- which is the only way to catch a
 * commit that corrupts the place cache, because the first build of a fresh
 * process always looks fine. Each variant is a start / middle / finish triple
 * at a different bearing across the centred place, well inside the La Plata
 * bbox (0.049 deg of latitude by 0.059 of longitude). */
static void geo_seed_dev_points(int variant)
{
    static const double k_var[][6] = {
        /* s_lat   s_lon   m_lat   m_lon   f_lat   f_lon   (x dlat / dlon) */
        { -1.00,  -1.00,   0.30,   0.00,   1.00,   1.00 },
        {  1.00,  -1.00,   0.00,   0.30,  -1.00,   1.00 },
        { -0.70,   0.90,   0.20,  -0.20,   0.90,  -0.70 },
        {  0.00,  -1.00,   0.50,   0.10,   0.00,   1.00 },
    };
    const int n = (int)(sizeof k_var / sizeof k_var[0]);
    const double *v = k_var[(variant % n + n) % n];
    const double dlat = 0.010, dlon = 0.013;
    int i;
    for (i = 0; i < 3; i++) {
        s_pts[i].lat = s_cen_lat + v[i * 2 + 0] * dlat;
        s_pts[i].lon = s_cen_lon + v[i * 2 + 1] * dlon;
    }
    s_n_pts = 3;
    geo_stamp_route_order();
    geo_clear_result();
    geo_mark_dirty();
    TD5_LOG_W(LOG_TAG, "GEO GENERATOR: TD5RE_GEO_SEED - seeded 3 DEV points, "
              "variant %d: (%.5f,%.5f) (%.5f,%.5f) (%.5f,%.5f) "
              "(dev build only; not a user action)", variant,
              s_pts[0].lat, s_pts[0].lon, s_pts[1].lat, s_pts[1].lon,
              s_pts[2].lat, s_pts[2].lon);
}

/* The route order as the letters of the clicks that made each point
 * (A = 1st click, B = 2nd, ...), e.g. "A C D B". */
static void geo_click_order_str(char *out, size_t n)
{
    size_t o = 0;
    int i;
    out[0] = '\0';
    for (i = 0; i < s_n_pts && o + 3 < n; i++) {
        out[o++] = (char)('A' + (int)(s_pt_seq[i] % 26u));
        out[o++] = ' ';
        out[o] = '\0';
    }
}

static void geo_click_selftest(void)
{
    /* place-relative offsets (dlat, dlon units), the seed's variant 0 for the
     * first three, so the final route is one the router is known to accept */
    static const double k_clk[4][2] = {
        { -1.00, -1.00 },   /* A: START  */
        {  1.00,  1.00 },   /* B: FINISH */
        {  0.30,  0.00 },   /* C: middle */
        {  0.60, -0.60 },   /* D: middle, nearer the START->C leg */
    };
    const double dlat = 0.010, dlon = 0.013;
    char buf[80];
    int i, ok = 1;
    geo_clear_points();
    for (i = 0; i < 4; i++) {
        const int at = geo_place_click(s_cen_lat + k_clk[i][0] * dlat,
                                       s_cen_lon + k_clk[i][1] * dlon, -1);
        geo_click_order_str(buf, sizeof buf);
        TD5_LOG_W(LOG_TAG, "GEO CLICK TEST: click %c -> route index %d, route "
                  "order: %s(dev build only)", 'A' + i, at, buf);
        /* the contract: A stays first, B stays last once placed */
        if (s_pt_seq[0] != 0u) ok = 0;
        if (i >= 1 && s_pt_seq[s_n_pts - 1] != 1u) ok = 0;
    }
    i = geo_undo_last();
    geo_click_order_str(buf, sizeof buf);
    TD5_LOG_W(LOG_TAG, "GEO CLICK TEST: right click -> removed route index %d, "
              "route order: %s", i, buf);
    if (strcmp(buf, "A C B ") != 0) ok = 0;
    TD5_LOG_W(LOG_TAG, "GEO CLICK TEST: %s (knob TD5RE_GEO_CLICK_ORDER=%d)",
              ok ? "PASS" : "FAIL", geo_click_order_on());
    geo_clear_result();
    geo_mark_dirty();
}
#endif

static void geo_screen_init(void)
{
    /* [ROUND 1026 L] The studio pins the synthetic world while it is open (see
     * Screen_AutoTrackOptions init); this screen reads the place from its own
     * map, so hand the decision back to the knob the way it was before. */
    td5_geo_force_place(NULL);
    /* [GEO FREE ROAM F2-A] arriving on the map ends any pending roam: BACK / ESC out of the
     * roam car select returns here without a dedicated hook, and a stale ARMED session would
     * turn the next ordinary race into a roam race */
    td5_roam_cancel();
    s_strip = frontend_geo_strip_context();   /* [F11b] the strip BUILD: local multiplayer (or the dev knob) */
    frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
    frontend_reset_buttons();
    frontend_create_button(TR("CLEAR"), GEO_BTN_CLEAR_X, GEO_BTN_Y,
                           GEO_BTN_CLEAR_W, GEO_BTN_H);
    frontend_create_button(s_strip ? TR("BUILD TRACK") : TR("RACE ROUTE"), GEO_BTN_BUILD_X, GEO_BTN_Y,
                           GEO_BTN_BUILD_W, GEO_BTN_H);
    frontend_create_button(TR("BACK"), GEO_BTN_BACK_X, GEO_BTN_Y,
                           GEO_BTN_BACK_W, GEO_BTN_H);
    /* [ROUND 1030 B item 3] the OPTIONS panel: section selector, rows, DEFAULTS.
     * Created in enum order -- frontend_create_button hands out indices
     * sequentially after frontend_reset_buttons. */
    {
        int k, b;
        b = frontend_create_button("", GEO_PANEL_X, GEO_SECT_Y, GEO_PANEL_W, GEO_SECT_H);
        if (b >= 0) s_buttons[b].is_selector = 1;
        for (k = 0; k < GEO_OPT_VIS; k++) {
            b = frontend_create_button("", GEO_PANEL_X, GEO_ROW_Y0 + k * GEO_ROW_STEP,
                                       GEO_PANEL_W, GEO_ROW_H);
            if (b >= 0) s_buttons[b].is_selector = 1;
        }
        b = frontend_create_button(TR("DEFAULTS"), GEO_PANEL_X + 38, GEO_BTN_DEF_Y,
                                   GEO_PANEL_W - 76, GEO_BTN_DEF_H);
        if (b != GEO_BTN_DEFAULTS)
            TD5_LOG_E(LOG_TAG, "GEO GENERATOR: DEFAULTS button got index %d, "
                      "expected %d -- the panel's button map is off", b, GEO_BTN_DEFAULTS);
        /* the section table must tile k_geo_opts with no gap, and no section
         * may hold more rows than there are row buttons */
        for (k = 0, b = 0; k < GEO_SECTS; k++) {
            if (k_geo_sects[k].first != b || k_geo_sects[k].count > GEO_OPT_VIS)
                TD5_LOG_E(LOG_TAG, "GEO GENERATOR: option section %s is malformed "
                          "(first %d, count %d)", k_geo_sects[k].name,
                          k_geo_sects[k].first, k_geo_sects[k].count);
            b = k_geo_sects[k].first + k_geo_sects[k].count;
        }
        if (b != GEO_OPTS)
            TD5_LOG_E(LOG_TAG, "GEO GENERATOR: option sections cover %d of %d rows",
                      b, GEO_OPTS);
        geo_opt_set_sect(s_opt_sect);
        if (!s_strip) {   /* [F11a] the panel's buttons stay (indices) but cannot be seen or focused */
            s_buttons[GEO_BTN_SECT].hidden = s_buttons[GEO_BTN_SECT].disabled = 1;
            s_buttons[GEO_BTN_DEFAULTS].hidden = s_buttons[GEO_BTN_DEFAULTS].disabled = 1;
        }
    }
    {
        /* [GEO FREE ROAM F2-A] FREE ROAM: drive the real city from the START pin (the first click) */
        const int rb = frontend_create_button(TR("FREE ROAM"), GEO_BTN_ROAM_X, GEO_BTN_Y,
                                              GEO_BTN_ROAM_W, GEO_BTN_H);
        if (rb != GEO_BTN_ROAM)
            TD5_LOG_E(LOG_TAG, "GEO GENERATOR: FREE ROAM button got index %d, expected %d", rb, GEO_BTN_ROAM);
    }

    /* Tiles are fetched only between open and close, so the screen is the
     * only thing that can cause a request. */
    td5_geo_tiles_open();

    s_n_places = td5_geo_route_places(s_place_slug, s_place_bbox, GEO_PLACE_MAX);
    {
        int i;
        for (i = 0; i < s_n_places; i++) {
            double ll[2 * GEO_RING_MAX];
            const int n = td5_geo_route_place_boundary(s_place_slug[i], 0, ll, GEO_RING_MAX);
            int k;
            s_place_ring_n[i] = 0;
            if (n >= 4 && n <= GEO_RING_MAX) {
                for (k = 0; k < n; k++) {
                    s_place_ring[i][k][0] = ll[2 * k];
                    s_place_ring[i][k][1] = ll[2 * k + 1];
                }
                s_place_ring_n[i] = n;
            }
        }
    }

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
    if (s_n_pts == 0 && td5_env_int("TD5RE_GEO_SEED", 0, 0, 1))
        geo_seed_dev_points(0);
    /* [ROUND 1010 AVENUES] ...and the same thing for a SPECIFIC route.
     * geo_seed_dev_points' four variants are relative to the place centre, so
     * they can seed A route but never THE route a report is about. Verifying
     * the avenue work needed Mariano's exact two clicks, which are recorded in
     * his _route/ROUTE_RAW.JSON "waypoints", and copying his cache in does not
     * help because the derived frame is what the fix REBUILDS.
     *
     *   TD5RE_GEO_SEED_LATLON="lat,lon;lat,lon[;...]"   (up to GEO_MAX_PTS)
     *
     * Dev-only and compiled out of RELEASE, like the seed above. */
    if (s_n_pts == 0) {
        const char *e = getenv("TD5RE_GEO_SEED_LATLON");
        if (e && e[0]) {
            const char *p = e;
            while (s_n_pts < GEO_MAX_PTS && *p) {
                char *end = NULL;
                const double la = strtod(p, &end);
                if (end == p || *end != ',') break;
                p = end + 1;
                {
                    const double lo = strtod(p, &end);
                    if (end == p) break;
                    s_pts[s_n_pts].lat = la;
                    s_pts[s_n_pts].lon = lo;
                    s_n_pts++;
                    p = end;
                }
                while (*p == ';' || *p == ' ') p++;
            }
            if (s_n_pts > 0) {
                geo_stamp_route_order();
                /* Centre the map on the first point, or the screen opens on the
                 * place centre with the route off the visible map. */
                s_cen_lat = s_pts[0].lat;
                s_cen_lon = s_pts[0].lon;
                geo_clear_result();
                geo_mark_dirty();
                TD5_LOG_W(LOG_TAG, "GEO GENERATOR: TD5RE_GEO_SEED_LATLON - "
                          "seeded %d point(s) from \"%s\" (dev build only; not "
                          "a user action)", s_n_pts, e);
            } else {
                TD5_LOG_E(LOG_TAG, "GEO GENERATOR: TD5RE_GEO_SEED_LATLON=\"%s\" "
                          "parsed to no points; expected \"lat,lon;lat,lon\"", e);
            }
        }
    }
    /* [ROUND 1030 B] Framedump affordances for the OPTIONS panel (inject_key
     * does not reach frontend menus): open on section N, focus button N. */
    {
        /* TD5RE_GEO_VIEW="lat,lon,zoom": where the map opens, so a seeded
         * route can be framed whole rather than centred on its START. */
        const char *v = getenv("TD5RE_GEO_VIEW");
        double la, lo;
        int z;
        if (v && sscanf(v, "%lf,%lf,%d", &la, &lo, &z) == 3) {
            s_cen_lat = la;
            s_cen_lon = lo;
            if (z >= TD5_GEO_TILE_MIN_Z && z <= TD5_GEO_TILE_MAX_Z) s_zoom = z;
        }
    }
    {
        const int sect = td5_env_int("TD5RE_GEO_PANEL_SECT", -1, -1, GEO_SECTS - 1);
        const int foc  = td5_env_int("TD5RE_GEO_PANEL_FOCUS", -1, -1, GEO_BTN_COUNT - 1);
        if (sect >= 0) geo_opt_set_sect(sect);
        if (foc >= 0 && !s_buttons[foc].disabled) s_selected_button = foc;
    }
    /* TD5RE_GEO_PANEL_SET="row:value[,row:value]" sets rows through the SAME
     * geo_opt_write the arrows call, so a headless BUILD can prove a panel
     * choice reaches the track build. Consumed once (cleared after use). */
    {
        const char *ps = getenv("TD5RE_GEO_PANEL_SET");
        if (ps && ps[0]) {
            char buf[128];
            char *tok, *ctx = NULL;
            snprintf(buf, sizeof buf, "%s", ps);
            _putenv_s("TD5RE_GEO_PANEL_SET", "");
            for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
                int row, val;
                if (sscanf(tok, "%d:%d", &row, &val) == 2 && row >= 0 && row < GEO_OPTS)
                    geo_opt_write(row, val);
            }
        }
    }
    /* [ROUND 1030 B item 4] Scripted clicks through the SAME place / undo
     * functions the mouse calls, logged step by step, for a headless check of
     * the click order. Leaves the 3-point route on the map. */
    if (s_n_pts == 0 && td5_env_int("TD5RE_GEO_CLICK_TEST", 0, 0, 1))
        geo_click_selftest();
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
/* [ROUND 1008] The commit half of BUILD, without leaving the screen, so the
 * dev multi-build harness can press it again. Returns the registered track
 * slot, or -1. */
static int geo_commit_and_register(void)
{
    int n, i, slot = -1;

    if (!geo_route_is_buildable()) { frontend_play_sfx(10); return -1; }
    if (td5_geo_route_commit() != 0) {
        /* [ROUND 1008] Say WHY. The commit refuses a degenerate route frame
         * rather than writing one, and "COULD NOT SAVE THE ROUTE" for every
         * cause is how the round-1007 corruption stayed invisible until it had
         * already eaten the place cache. */
        const char *why = td5_geo_route_commit_reason();
        frontend_play_sfx(10);
        snprintf(s_reason, sizeof(s_reason), "%s",
                 (why && why[0]) ? why : "COULD NOT SAVE THE ROUTE");
        s_verdict = TD5_GEO_ROUTE_ERROR;
        return -1;
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
        return -1;
    }

    TD5_LOG_I(LOG_TAG, "GEO GENERATOR: built '%s' -> track slot %d (%d spans)",
              s_place, slot, s_spans);

    /* [1008 N2 item 1] "I can't see the preview after I exit the menu."
     *
     * td5_geo_preview_route() caches the plotted route BY SLUG and returns the
     * cached copy on a name match -- but BUILD has just REWRITTEN that place's
     * ROUTE.JSON, and the slug is of course unchanged. So from the second BUILD
     * of a session onwards the SELECT TRACK panel re-drew the FIRST route and
     * the player's new one was never read off disk. Measured on master
     * 3df2ce24: two BUILDs of la_plata in one session produce exactly ONE
     * "geo: preview route la_plata -> N of M nodes" line in engine.log.
     *
     * Passing "" is that reader's documented invalidate (td5_geo.h: '"" =
     * synthetic / clear'), so the next panel draw re-parses the file. One
     * ~150 KB parse per BUILD, not per frame. */
    td5_geo_preview_route("");

    /* [GEO ROW REMOVAL 2026-10-08, Mariano item 5] The place slots are no
     * longer browsable, so SELECT TRACK would bounce this pick back to track 0
     * on entry. Arm it: the selector accepts exactly this slot until the player
     * cycles away. Must come BEFORE the s_selected_track write for symmetry
     * with the screen's own entry validation, which reads both. */
    frontend_geo_pick_arm(slot);
    s_selected_track = slot;
    frontend_play_sfx(3);
    return slot;
}

/* [GEO FREE ROAM F2-A] The spawn for FREE ROAM: the START pin (the first click), as a point of a
 * place that has built road tiles. 0 (button greyed) with no pin or outside the built tiles. */
static int geo_roam_spawn(TD5_RoamSpawn *out)
{
    /* the lookup parses PLACE.JSON + ROAMGRID.JSON, so it is memoised on the pin (asked every frame) */
    static struct { int valid; double lat, lon; int ok; TD5_RoamSpawn sp; } memo;
    if (s_n_pts < 1) return 0;
    if (!memo.valid || memo.lat != s_pts[0].lat || memo.lon != s_pts[0].lon) {
        memset(&memo.sp, 0, sizeof memo.sp);
        memo.ok = td5_roam_place_for_latlon(s_pts[0].lat, s_pts[0].lon, memo.sp.place,
                                            sizeof memo.sp.place, &memo.sp.spawn_px, &memo.sp.spawn_pz);
        memo.sp.spawn_yaw = -1;           /* along the nearest street */
        memo.sp.car_index = 0;            /* the car-select screen fills it in */
        memo.lat = s_pts[0].lat; memo.lon = s_pts[0].lon; memo.valid = 1;
    }
    if (memo.ok && out) *out = memo.sp;
    return memo.ok;
}

/* FREE ROAM: arm the session, then the NORMAL car select (BACK there returns here, OK launches). */
static void geo_do_roam(void)
{
    TD5_RoamSpawn sp;
    if (!geo_roam_spawn(&sp)) { frontend_play_sfx(10); return; }
    td5_roam_begin(&sp);
    frontend_roam_prepare_car_select();
    frontend_play_sfx(3);
    TD5_LOG_I(LOG_TAG, "GEO GENERATOR: FREE ROAM at %.5f,%.5f (%s) -> car select",
              s_pts[0].lat, s_pts[0].lon, sp.place);
    geo_leave(TD5_SCREEN_CAR_SELECTION);
}

/* [GEO FREE ROAM F11a] 1 when every pin can be raced in the city: 2..TD5_ROUTE_MAX_WP pins, all of
 * them inside the built road tiles of ONE place (the START one). `why` = a short reason when not.
 * Memoised on the pins (asked every frame; each lookup parses PLACE.JSON + ROAMGRID.JSON). */
static int geo_race_city_ready(const char **why)
{
    static struct { int valid; uint32_t key; int ok; const char *why; } memo;
    uint32_t key = 2166136261u;
    int i;
    const unsigned char *b = (const unsigned char *)s_pts;
    for (i = 0; i < (int)(s_n_pts * (int)sizeof s_pts[0]); i++) key = (key ^ b[i]) * 16777619u;
    key ^= (uint32_t)s_n_pts * 2654435761u;
    if (!memo.valid || memo.key != key) {
        TD5_RoamSpawn sp;
        memo.valid = 1; memo.key = key; memo.ok = 0; memo.why = NULL;
        if (s_n_pts < 2) memo.why = "PLACE A START AND A FINISH";
        else if (s_n_pts > TD5_ROUTE_MAX_WP) memo.why = "AT MOST 24 POINTS IN THE CITY";
        else if (!geo_roam_spawn(&sp)) memo.why = "START IS OUTSIDE THE CITY MAP";
        else {
            memo.ok = 1;
            for (i = 1; i < s_n_pts && memo.ok; i++) {
                char slug[64];
                int32_t px, pz;
                if (!td5_roam_place_for_latlon(s_pts[i].lat, s_pts[i].lon, slug, sizeof slug, &px, &pz) ||
                    strcmp(slug, sp.place) != 0) {
                    memo.ok = 0;
                    memo.why = (i == s_n_pts - 1) ? "FINISH IS OUTSIDE THE CITY MAP" : "A POINT IS OUTSIDE THE CITY MAP";
                }
            }
        }
    }
    if (why) *why = memo.why;
    return memo.ok;
}

/* RACE ROUTE: free roam from START with the pins handed to the session; car select first, then the
 * race setup panel opens over the route map once the city's planner has the route (td5_roam_handoff.h). */
static void geo_do_race_city(void)
{
    TD5_RoamSpawn sp;
    double ll[TD5_ROUTE_MAX_WP][2];
    int i;
    if (!geo_race_city_ready(NULL) || !geo_roam_spawn(&sp)) { frontend_play_sfx(10); return; }
    for (i = 0; i < s_n_pts; i++) { ll[i][0] = s_pts[i].lat; ll[i][1] = s_pts[i].lon; }
    td5_roam_begin(&sp);
    if (!td5_roam_handoff_set((const double (*)[2])ll, s_n_pts, TD5_ROUTE_SPRINT, 1, TD5_HANDOFF_SETUP_PANEL, -1, 0, 0)) {
        td5_roam_cancel();
        frontend_play_sfx(10);
        return;
    }
    frontend_roam_prepare_car_select();
    frontend_play_sfx(3);
    TD5_LOG_I(LOG_TAG, "GEO GENERATOR: RACE ROUTE %d point(s) from %.5f,%.5f (%s) -> car select -> race in the city",
              s_n_pts, s_pts[0].lat, s_pts[0].lon, sp.place);
    geo_leave(TD5_SCREEN_CAR_SELECTION);
}

/* [F11a] The pins on the map (START first, FINISH last), for Quick Race's geospatial entry. */
int frontend_geo_generator_points(double (*ll)[2], int max)
{
    int i, n = s_n_pts < max ? s_n_pts : max;
    for (i = 0; i < n; i++) { ll[i][0] = s_pts[i].lat; ll[i][1] = s_pts[i].lon; }
    return n;
}

static void geo_do_build(void)
{
    if (geo_commit_and_register() >= 0) geo_leave(s_parent_screen);
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
     * BUILD, then re-arms AutoRace on the slot it registered -- which is the
     * whole point: it exercises the commit -> re-grid -> reload -> build path
     * WITHOUT a relaunch, the thing a fresh process would paper over.
     *
     * [ROUND 1008] TD5RE_GEO_AUTOBUILD_N=k repeats that k times on k DIFFERENT
     * seeded routes before arming the race. One build per process could never
     * have caught the cache corruption that shipped in round 1007: commit 1
     * looked perfectly healthy and it was commit 2, reading commit 1's output
     * back as if it were the fetched data, that collapsed the place. The whole
     * class of bug only shows up on the SECOND build in a process, so the
     * harness has to be able to press BUILD twice. Compiled out of RELEASE. */
    {
        static int s_autobuild_done;
        const int want = td5_env_int("TD5RE_GEO_AUTOBUILD_N", 1, 1, 4);
        /* [F11b] AUTOBUILD=1 is itself a dev strip request (td5_roam_strip_build_enabled), so s_strip is 1 */
        if (s_strip && td5_env_int("TD5RE_GEO_AUTOBUILD", 0, 0, 1)
            && s_autobuild_done < want && geo_route_is_buildable()) {
            int slot;
            TD5_LOG_W(LOG_TAG, "GEO GENERATOR: TD5RE_GEO_AUTOBUILD - pressing "
                      "BUILD %d of %d on the %d-span route (dev build only)",
                      s_autobuild_done + 1, want, s_spans);
            slot = geo_commit_and_register();   /* commit + invalidate + register */
            s_autobuild_done++;
            if (slot < 0) {
                TD5_LOG_E(LOG_TAG, "GEO GENERATOR: TD5RE_GEO_AUTOBUILD - BUILD "
                          "%d of %d FAILED: %s", s_autobuild_done, want,
                          s_reason[0] ? s_reason : "(no reason given)");
                return;
            }
            TD5_LOG_W(LOG_TAG, "GEO GENERATOR: TD5RE_GEO_AUTOBUILD - BUILD %d of "
                      "%d OK -> slot %d", s_autobuild_done, want, slot);
            if (s_autobuild_done < want) {
                geo_seed_dev_points(s_autobuild_done);   /* a DIFFERENT route */
                return;
            }
            g_td5.ini.default_track = slot;
            g_td5.ini.auto_race     = 1;   /* MENU loop fires auto_race_setup */
            TD5_LOG_W(LOG_TAG, "GEO GENERATOR: TD5RE_GEO_AUTOBUILD - armed "
                      "AutoRace on slot %d after %d build(s)", slot, want);
            return;
        }
    }
#endif

    s_buttons[GEO_BTN_CLEAR].disabled = (s_n_pts > 0) ? 0 : 1;
    s_buttons[GEO_BTN_BUILD].disabled = (s_strip ? geo_route_is_buildable() : geo_race_city_ready(NULL)) ? 0 : 1;
    s_buttons[GEO_BTN_ROAM].disabled = geo_roam_spawn(NULL) ? 0 : 1;

    if (frontend_check_escape()) {
        frontend_play_sfx(5);
        geo_leave(s_parent_screen);
        return;
    }

    /* [ROUND 1030 B item 3] LEFT/RIGHT on the section selector flips the page,
     * on a row steps its value (keyboard, pad, or a click on a row's arrow
     * zone -- the shared nav turns all three into the same arrow bits). */
    {
        const int d = frontend_option_delta();
        const int slot = s_selected_button - GEO_BTN_ROW0;
        if (d && s_selected_button == GEO_BTN_SECT) {
            geo_opt_set_sect(s_opt_sect + d);
            frontend_play_sfx(2);
        } else if (d && slot >= 0 && slot < GEO_OPT_VIS && geo_opt_row_at(slot) >= 0) {
            geo_opt_step(geo_opt_row_at(slot), d);
            frontend_play_sfx(2);
        }
    }

    if (s_input_ready) {
        s_input_ready = 0;
        switch (s_button_index) {
        case GEO_BTN_CLEAR:
            if (s_n_pts > 0) {
                geo_clear_points();
                geo_clear_result();
                frontend_play_sfx(5);
            } else {
                frontend_play_sfx(10);
            }
            break;
        case GEO_BTN_SECT:                      /* confirm = next page */
            geo_opt_set_sect(s_opt_sect + 1);
            break;
        case GEO_BTN_DEFAULTS:
            geo_opt_defaults();
            frontend_play_sfx(5);
            break;
        case GEO_BTN_BUILD:
            if (s_strip) geo_do_build();
            else geo_do_race_city();      /* [F11a] */
            return;
        case GEO_BTN_BACK:
            frontend_play_sfx(5);
            geo_leave(s_parent_screen);
            return;
        case GEO_BTN_ROAM:
            geo_do_roam();
            return;
        default:
            /* confirm on a row (Enter, pad A, double click) = next value */
            if (s_button_index >= GEO_BTN_ROW0 && s_button_index < GEO_BTN_ROW0 + GEO_OPT_VIS
                && geo_opt_row_at(s_button_index - GEO_BTN_ROW0) >= 0)
                geo_opt_step(geo_opt_row_at(s_button_index - GEO_BTN_ROW0), 1);
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
