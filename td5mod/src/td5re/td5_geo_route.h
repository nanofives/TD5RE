/**
 * td5_geo_route.h -- GEO TRACK: route + condition, in C (PORT-ONLY).
 *
 * The C port of re/tools/geo_route.py (A* over ROADS.JSON) and
 * re/tools/geo_condition.py (condition_route), plus the part of
 * re/tools/geo_selector.py that SEND TO GAME runs. See
 * docs/plans/GEO_TRACK_OSM_PLAN.md sections 5, 6e and 6j, and
 * _archive/GEO_GENERATOR_ROUND_2026-10-07_PLAN.md for the round that asked
 * for it.
 *
 * WHY. Until now a geo track could only be authored through the Python
 * selector: the browser drew the map, geo_route found the path and
 * geo_condition turned it into a TD5-legal centerline. The release exe shipped
 * neither. This module is the whole of that pipeline with no interpreter in
 * it, so the in-game GEOSPATIAL TRACK GENERATOR screen can route, condition
 * and commit on its own.
 *
 * WHAT IT IS NOT. It does not fetch. Routing works only inside a place that is
 * already in re/assets/geo/ (today: la_plata); nothing here speaks HTTP,
 * Overpass, Terrarium or WorldCover. td5_geo_route_places() exists so the
 * screen can shade where routing will actually work.
 *
 * PARITY IS THE CONTRACT. Every number this module produces is meant to equal
 * the Python tool's on the same input -- the conditioner is what certifies a
 * route the engine then builds, so a C/Python disagreement would be a route
 * the selector blesses and the game breaks on. The fixtures in
 * re/tools/geo_fixtures/ are the reference, and the dev harness
 * (TD5RE_GEO_ROUTE_TEST, below) re-measures against them.
 *
 * BYTE-IDENTITY. Nothing here runs on a synthetic auto-track build. The only
 * entry points are called from the geo screen and from the dev harness; the
 * module draws no tg_rand/tg_frand/tg_range (the standing rule at
 * td5_trackgen_internal.h:1290-1296) and holds no generator state.
 */
#ifndef TD5_GEO_ROUTE_H
#define TD5_GEO_ROUTE_H

typedef struct { double lat, lon; } TD5_GeoLatLon;

typedef enum {
    TD5_GEO_ROUTE_OK = 0,        /* drivable, conditioned */
    TD5_GEO_ROUTE_NO_DATA,       /* a point lies outside every downloaded place */
    TD5_GEO_ROUTE_NO_PATH,       /* no road path between two points */
    TD5_GEO_ROUTE_TOO_LONG,      /* over the 3000-span cap */
    TD5_GEO_ROUTE_TOO_SHORT,     /* "not enough road here" */
    TD5_GEO_ROUTE_ERROR
} TD5_GeoRouteVerdict;

/* Warnings the screen may want to show beside an accepted route (grade-
 * separated crossings, route-byte dead zone, ...). Bounded so the struct is
 * plain data a caller can keep on the stack. */
#define TD5_GEO_ROUTE_MAX_WARN   4
#define TD5_GEO_ROUTE_WARN_LEN 160

typedef struct {
    TD5_GeoRouteVerdict verdict;
    char  place_slug[64];        /* which cached place the route lives in */
    char  reason[160];           /* human text for the screen (English, TR()-able) */
    int   spans;                 /* conditioned span count */
    float length_m;
    int   n_path;                /* routed polyline, for drawing on the map */
    TD5_GeoLatLon *path;         /* owned by the module, valid until next call */
    int   n_crossings;           /* self-crossings (red markers) */
    TD5_GeoLatLon *crossings;

    /* ---------------------------------------------------------------------
     * EXTENSIONS. Everything above is the L2<->L3 contract from the round
     * plan and must keep its name, type and order. Everything below is
     * additive and may grow at the END only. */

    int   n_crossings_level;     /* of n_crossings, how many are NOT separated  */
    int   n_grade_separations;   /* level sites the generator will build a deck */
    int   direction_reversed;    /* the conditioner drove the route backwards   */
    int   span_cap;              /* TD5_TG_MAX_SPANS, so the screen can bar it  */
    float worst_turn_deg;        /* worst stored turn, after smoothing          */
    float limit_turn_deg;        /* the tightest limit it was measured against  */
    float monotone_pct;          /* share of spans advancing along +X           */
    float raw_length_m;          /* the ROUTED polyline, before conditioning    */
    char  streets[320];          /* "Avenida 53 > Calle 30 > ...", truncated  */
    int   n_warnings;
    char  warning[TD5_GEO_ROUTE_MAX_WARN][TD5_GEO_ROUTE_WARN_LEN];
    double build_ms;             /* wall time of td5_geo_route_build            */
} TD5_GeoRouteResult;

/* Route + condition through start, waypoints..., finish. `n_pts` >= 2.
 * Synchronous but must finish well under a second on La Plata; the screen
 * calls it on a worker once a build has been measured over the frame budget,
 * so it must not touch frontend state or g_td5 -- it reads the place cache
 * off disk and writes only its own buffers.
 *
 * RETURNS 0 when the CALL was made, and `out->verdict` then says whether the
 * route is raceable -- every refusal (no data, no path, too long, too short)
 * is a 0 return with a verdict, NOT an error return. Non-zero means the call
 * itself could not be made (a null `out`). `out->path` and `out->crossings`
 * point into module-owned storage valid until the next build. */
int  td5_geo_route_build(const TD5_GeoLatLon *pts, int n_pts, TD5_GeoRouteResult *out);

/* Build the DERIVED route frame of the last OK route and select the place.
 * Returns 0 on success.
 *
 * [ROUND 1008] It READS re/assets/geo/<slug>/ and WRITES only
 * re/assets/geo/<slug>/_route/ (see td5_geo.h). The fetched source is never
 * modified, so pressing BUILD N times on the same route gives N identical
 * results -- before the split each press re-derived from the previous press's
 * output and the place collapsed to a 2x2 grid by the third.
 *
 * It REFUSES rather than writing a degenerate frame (tiny grid, no terrain
 * under the route, a vector layer that survived empty). On a refusal nothing
 * is written, the place keeps racing off whatever it raced off before, and
 * td5_geo_route_commit_reason() is a short line the screen can show. */
int  td5_geo_route_commit(void);

/* Why the last td5_geo_route_commit() refused, in screen-ready English.
 * "" after a successful commit, or before the first one. */
const char *td5_geo_route_commit_reason(void);

/* Bounds of every downloaded place, so the screen can shade where routing works.
 * bbox rows are {west, south, east, north} in degrees -- lon first, the order
 * the screen's own projection takes, and the one td5_fe_geo.c reads. Returns
 * the count written (<= max); pass slugs == NULL to count only. */
int  td5_geo_route_places(char slugs[][64], double bbox[][4], int max);

/* The conditioned-span ceiling the engine cannot exceed: s_struct[] / s_rn[]
 * in td5_tg_road.c are indexed by node with no bounds check, so the
 * conditioner enforces the cap rather than leaving it to the build. Mirrors
 * TD5_TG_MAX_SPANS in td5_trackgen_internal.h, repeated here so the screen can
 * show "N / 3000" without pulling in the generator's private header. */
#define TD5_GEO_ROUTE_MAX_SPANS 3000

/* 1 while the L2 placeholder is standing in for the router, 0 once the real
 * module is linked. The screen gates BUILD TRACK on it and labels its status
 * line with it, so a framedump of the placeholder can never be read as proof
 * that routing works. (The L2 header had this comment the other way round;
 * every call site -- td5_fe_geo.c:823, :957, :1030 -- reads it as written
 * here, and the placeholder returned 1.) */
int  td5_geo_route_is_stub(void);

/* ------------------------------------------------------------ extensions ---
 * Additive helpers. The three calls above are the contract; these exist so the
 * screen does not have to re-implement the projection or the snap test. */

/* Module lifecycle, registered in g_td5re_modules. Init loads nothing. */
int  td5_geo_route_init(void);
void td5_geo_route_shutdown(void);

/* Snap a click to the nearest drivable road of `slug` (NULL = auto-pick the
 * place whose bbox holds it). Writes the snapped point and its distance in
 * metres. Returns 1 when a road was found. The screen uses this to show where
 * a click will actually start, and to refuse one that is in the middle of a
 * field -- the same CLICK_MAX_SNAP_M test the Python selector runs. */
#define TD5_GEO_ROUTE_SNAP_MAX_M 250.0
int  td5_geo_route_snap(const char *slug, TD5_GeoLatLon p,
                        TD5_GeoLatLon *out, double *dist_m);

/* The place a lat/lon falls in ("" when none), by PLACE.JSON bbox. */
const char *td5_geo_route_place_at(TD5_GeoLatLon p);

/* The last build's result, or NULL. Same lifetime as `path` above. */
const TD5_GeoRouteResult *td5_geo_route_last(void);

/* Free the cached road graph (several MB). The screen may call this on exit so
 * a menu visit does not hold the memory for the rest of the session. */
void td5_geo_route_drop_graph(void);

#endif /* TD5_GEO_ROUTE_H */
