/* ========================================================================
 * td5_geo_route_stub.c -- PLACEHOLDER router behind td5_geo_route.h.
 *
 * [GEO GENERATOR 2026-10-07, round 1007 group L2] The GEOSPATIAL TRACK
 * GENERATOR screen (td5_fe_geo.c) is built against the L2 <-> L3 contract in
 * td5_geo_route.h. Group L3 is porting the real A* + conditioner
 * (re/tools/geo_route.py + geo_condition.py) to C in td5_geo_route.c; until
 * that lands this file answers the same three calls so the screen can be
 * built, run and framedumped.
 *
 * WHAT IT ACTUALLY DOES, so nobody mistakes it for the real thing:
 *   - build:  a STRAIGHT great-circle-ish polyline through the given points.
 *             No roads are consulted. The span count is the straight-line
 *             length over the span length, and the only verdicts it can
 *             return are OK, TOO_SHORT and TOO_LONG.
 *   - places: the real place list (td5_geo_places_*) with each PLACE.JSON's
 *             bbox -- this part IS correct and L3 can keep it.
 *   - commit: fails, loudly. Writing ROUTE.JSON for a line that ignores the
 *             road graph would hand the generator an unraceable track.
 *
 * td5_geo_route_is_stub() returns 1 here, and the screen labels its status
 * line accordingly, so a framedump of the stub can never be read as proof
 * that routing works.
 *
 * REMOVING IT: delete this file, drop its line from srcs.txt, add
 * td5_geo_route.c. Nothing else references it by name. The
 * TD5_GEO_ROUTE_REAL guard below is a second belt: if both files are ever in
 * the build at once, the real one wins instead of the link failing.
 * ======================================================================== */

#ifndef TD5_GEO_ROUTE_REAL

#include "td5_geo_route.h"
#include "td5_geo.h"
#include "td5_platform.h"
#include "deps/cjson/cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "asset"

/* ~3.5 m of road per span (TD5_TG_SPAN_LENGTH 1500 units at ~430 units/m --
 * see GEO_TRACK_OSM_PLAN.md section 2). Repeated as a literal rather than
 * pulled from td5_trackgen_internal.h: this TU has no business including the
 * generator's private header, and L3's real conditioner computes the figure
 * properly from the cache's own cell_m. */
#define STUB_SPAN_M 3.4883720930232558

#define STUB_MAX_PTS  64
#define STUB_PATH_MAX (STUB_MAX_PTS * 64)

/* How many cached places the stub will consider when deciding which one the
 * points fall in. Local to the stub; the screen has its own cap. */
#define TD5_GEO_STUB_PLACE_MAX 32

static TD5_GeoLatLon s_path[STUB_PATH_MAX];

/* Metres per degree of latitude, and of longitude at `lat`. Equirectangular,
 * which is accurate to a fraction of a percent over a city-sized bbox. */
static void stub_metres_per_deg(double lat, double *m_lat, double *m_lon)
{
    const double rad = lat * 3.14159265358979323846 / 180.0;
    *m_lat = 111132.0;
    *m_lon = 111320.0 * cos(rad);
    if (*m_lon < 1.0) *m_lon = 1.0;   /* at the poles, keep the division sane */
}

static double stub_seg_metres(TD5_GeoLatLon a, TD5_GeoLatLon b)
{
    double m_lat, m_lon, dx, dy;
    stub_metres_per_deg((a.lat + b.lat) * 0.5, &m_lat, &m_lon);
    dy = (b.lat - a.lat) * m_lat;
    dx = (b.lon - a.lon) * m_lon;
    return sqrt(dx * dx + dy * dy);
}

/* Read a whole file into a malloc'd NUL-terminated buffer, for cJSON. Same
 * shape as td5_geo.c's own reader; duplicated rather than exported because
 * this file is scaffolding that gets deleted. */
static char *stub_slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long  n;
    char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n <= 0 || n > (64L << 20)) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int td5_geo_route_is_stub(void)
{
    return 1;
}

int td5_geo_route_build(const TD5_GeoLatLon *pts, int n_pts,
                        TD5_GeoRouteResult *out)
{
    double total_m = 0.0;
    int    i, n_out = 0;

    if (!pts || !out || n_pts < 2) return 1;
    if (n_pts > STUB_MAX_PTS) n_pts = STUB_MAX_PTS;

    memset(out, 0, sizeof(*out));
    out->path       = s_path;
    out->crossings  = NULL;
    out->n_crossings = 0;

    /* Densify each leg so the drawn polyline has the same shape a real routed
     * one would (the screen draws whatever n_path it is handed). */
    for (i = 0; i < n_pts - 1; i++) {
        const double leg_m = stub_seg_metres(pts[i], pts[i + 1]);
        int steps = (int)(leg_m / 50.0);
        int s;
        if (steps < 1)  steps = 1;
        if (steps > 63) steps = 63;
        total_m += leg_m;
        for (s = 0; s < steps && n_out < STUB_PATH_MAX; s++) {
            const double t = (double)s / (double)steps;
            s_path[n_out].lat = pts[i].lat + (pts[i + 1].lat - pts[i].lat) * t;
            s_path[n_out].lon = pts[i].lon + (pts[i + 1].lon - pts[i].lon) * t;
            n_out++;
        }
    }
    if (n_out < STUB_PATH_MAX) s_path[n_out++] = pts[n_pts - 1];
    out->n_path   = n_out;
    out->length_m = (float)total_m;
    out->spans    = (int)(total_m / STUB_SPAN_M);

    /* Which cached place the points fall in -- this much the stub can answer
     * honestly, and the screen uses it to grey BUILD outside a downloaded
     * area. */
    {
        char   slugs[TD5_GEO_STUB_PLACE_MAX][64];
        double bbox[TD5_GEO_STUB_PLACE_MAX][4];
        const int np = td5_geo_route_places(slugs, bbox, TD5_GEO_STUB_PLACE_MAX);
        int p, found = -1;
        for (p = 0; p < np && found < 0; p++) {
            int all_in = 1, k;
            for (k = 0; k < n_pts && all_in; k++)
                if (pts[k].lon < bbox[p][0] || pts[k].lon > bbox[p][2] ||
                    pts[k].lat < bbox[p][1] || pts[k].lat > bbox[p][3])
                    all_in = 0;
            if (all_in) found = p;
        }
        if (found < 0) {
            out->verdict = TD5_GEO_ROUTE_NO_DATA;
            snprintf(out->reason, sizeof(out->reason),
                     "NO MAP DATA HERE - PICK POINTS INSIDE A DOWNLOADED AREA");
            return 0;
        }
        snprintf(out->place_slug, sizeof(out->place_slug), "%s", slugs[found]);
    }

    if (out->spans < 64) {
        out->verdict = TD5_GEO_ROUTE_TOO_SHORT;
        snprintf(out->reason, sizeof(out->reason),
                 "NOT ENOUGH ROAD HERE - MOVE THE POINTS FURTHER APART");
    } else if (out->spans > TD5_GEO_ROUTE_MAX_SPANS) {
        out->verdict = TD5_GEO_ROUTE_TOO_LONG;
        snprintf(out->reason, sizeof(out->reason),
                 "ROUTE TOO LONG - OVER THE %d SPAN LIMIT",
                 TD5_GEO_ROUTE_MAX_SPANS);
    } else {
        out->verdict = TD5_GEO_ROUTE_OK;
        snprintf(out->reason, sizeof(out->reason), "STRAIGHT LINE - ROUTER NOT LINKED YET");
    }
    return 0;
}

int td5_geo_route_commit(void)
{
    TD5_LOG_W(LOG_TAG, "geo route: COMMIT REFUSED -- this build links the L2 "
              "stub router, which ignores the road graph. Writing its straight "
              "line as ROUTE.JSON would produce an unraceable track. Link "
              "td5_geo_route.c (group L3) first.");
    return 1;
}

int td5_geo_route_places(char slugs[][64], double bbox[][4], int max)
{
    int n, i, out = 0;

    if (!slugs || !bbox || max <= 0) return 0;

    td5_geo_places_rescan();
    n = td5_geo_places_count();
    for (i = 0; i < n && out < max; i++) {
        const char *slug = td5_geo_places_slug(i);
        char  path[512];
        char *json;
        cJSON *root, *bb;
        const cJSON *w, *s, *e, *nn;

        if (!slug || !slug[0]) continue;
        snprintf(path, sizeof(path), "re/assets/geo/%s/PLACE.JSON", slug);
        json = stub_slurp(path);
        if (!json) continue;
        root = cJSON_Parse(json);
        free(json);
        if (!root) continue;

        /* The selector pins the routing area in route_graph_bbox on the first
         * SEND TO GAME (see GEO_TRACK_OSM_PLAN.md 6f); that is the area a
         * route is actually guaranteed to find roads in, so prefer it and
         * fall back to the fetched bbox. */
        bb = cJSON_GetObjectItem(root, "route_graph_bbox");
        if (!bb || !cJSON_IsObject(bb)) bb = cJSON_GetObjectItem(root, "bbox");
        if (bb && cJSON_IsObject(bb)) {
            w  = cJSON_GetObjectItem(bb, "west");
            s  = cJSON_GetObjectItem(bb, "south");
            e  = cJSON_GetObjectItem(bb, "east");
            nn = cJSON_GetObjectItem(bb, "north");
            if (cJSON_IsNumber(w) && cJSON_IsNumber(s) &&
                cJSON_IsNumber(e) && cJSON_IsNumber(nn)) {
                snprintf(slugs[out], 64, "%s", slug);
                bbox[out][0] = w->valuedouble;
                bbox[out][1] = s->valuedouble;
                bbox[out][2] = e->valuedouble;
                bbox[out][3] = nn->valuedouble;
                out++;
            }
        }
        cJSON_Delete(root);
    }
    return out;
}

#endif /* !TD5_GEO_ROUTE_REAL */
