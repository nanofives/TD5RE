/* ========================================================================
 * td5_geo_route.h -- GEOSPATIAL TRACK GENERATOR: the in-game router contract
 *                    (PORT-ONLY). See docs/plans/GEO_TRACK_OSM_PLAN.md and
 *                    _archive/GEO_GENERATOR_ROUND_2026-10-07_PLAN.md.
 *
 * The C port of what re/tools/geo_route.py (A* over ROADS.JSON) and
 * geo_condition.py (condition_route, self-crossing detection, grade
 * separations) do offline, plus the route-frame raster rebuild the browser
 * selector performs on SEND TO GAME. With this in place the RELEASE exe can
 * take a start, a finish and any number of middle points placed on the map
 * screen (td5_fe_geo.c) and turn them into a raceable track with no Python.
 *
 * OWNERSHIP. This header is the L2 <-> L3 seam of round 1007. L3 owns the
 * real implementation (td5_geo_route.c); L2 owns the screen that calls it and
 * ships a stub (td5_geo_route_stub.c) so the UI is testable before the router
 * lands. The field order below is part of the contract: extend the struct at
 * the END, never rename or reorder what is already here.
 *
 * THREADING. td5_geo_route_build is synchronous and must finish well under a
 * second on La Plata. The screen calls it on a worker thread when a rebuild
 * would miss the 16 ms frame budget, so the implementation must not touch
 * frontend state, g_td5, or anything else the main thread owns -- it reads the
 * place cache off disk and writes only its own buffers.
 * ======================================================================== */
#ifndef TD5_GEO_ROUTE_H
#define TD5_GEO_ROUTE_H

typedef struct { double lat, lon; } TD5_GeoLatLon;

typedef enum {
    TD5_GEO_ROUTE_OK = 0,        /* drivable, conditioned                    */
    TD5_GEO_ROUTE_NO_DATA,       /* a point lies outside every downloaded place */
    TD5_GEO_ROUTE_NO_PATH,       /* no road path between two points          */
    TD5_GEO_ROUTE_TOO_LONG,      /* over the 3000-span cap                   */
    TD5_GEO_ROUTE_TOO_SHORT,     /* "not enough road here"                   */
    TD5_GEO_ROUTE_ERROR
} TD5_GeoRouteVerdict;

typedef struct {
    TD5_GeoRouteVerdict verdict;
    char  place_slug[64];        /* which cached place the route lives in    */
    char  reason[160];           /* human text for the screen (English, TR()-able) */
    int   spans;                 /* conditioned span count                   */
    float length_m;
    int   n_path;                /* routed polyline, for drawing on the map  */
    TD5_GeoLatLon *path;         /* owned by the module, valid until next call */
    int   n_crossings;           /* self-crossings (red markers)             */
    TD5_GeoLatLon *crossings;
} TD5_GeoRouteResult;

/* The conditioned-span ceiling the engine cannot exceed: s_struct[] / s_rn[]
 * in td5_tg_road.c are indexed by node with no bounds check past ~3008, so the
 * conditioner enforces the cap rather than leaving it to the build. Mirrors
 * TD5_TG_MAX_SPANS in td5_trackgen_internal.h, repeated here so the screen can
 * show "N / 3000" without pulling in the generator's private header. */
#define TD5_GEO_ROUTE_MAX_SPANS 3000

/* Route + condition through start, waypoints..., finish. `n_pts` >= 2.
 * Returns 0 on success (out->verdict then says whether the route is raceable),
 * non-zero only when the call itself could not be made (bad arguments).
 * `out->path` / `out->crossings` point into module-owned storage that stays
 * valid until the next td5_geo_route_build call. */
int  td5_geo_route_build(const TD5_GeoLatLon *pts, int n_pts,
                         TD5_GeoRouteResult *out);

/* Write ROUTE_RAW.JSON + ROUTE.JSON for the last OK build, rebuild the rasters
 * in the route frame, and set that place as selected. Returns 0 on success. */
int  td5_geo_route_commit(void);

/* Bounds of every downloaded place, so the screen can shade where routing
 * works. Fills up to `max` rows; bbox[i] is {west, south, east, north} in
 * degrees. Returns the number of rows written. */
int  td5_geo_route_places(char slugs[][64], double bbox[][4], int max);

/* 1 while a real router is linked, 0 while the L2 stub is standing in for it.
 * The screen uses this to label its status line honestly rather than claiming
 * a straight line between two pins is a drivable route. */
int  td5_geo_route_is_stub(void);

#endif /* TD5_GEO_ROUTE_H */
