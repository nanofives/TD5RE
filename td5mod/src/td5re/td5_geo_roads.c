/**
 * td5_geo_roads.c -- GEO TRACK: real OSM road graph (ROADS.JSON) reader (PORT-ONLY)
 *
 * Contract and rationale in td5_geo_roads.h. Reads the `roads` array
 * re/tools/geo_fetch.py writes into re/assets/geo/<slug>/ROADS.JSON:
 *
 *   {"roads":[{"class":"secondary","lanes":2,"oneway":true,"bridge":false,
 *              "tunnel":false,"layer":0,"width":null,"median":false,
 *              "points":[{"x":257749.23,"z":722919.4}, ...], ...}, ...]}
 *
 * Only the fields the street network needs are kept. `latlon`, `name`,
 * `maxspeed`, `sidewalk`, `median`, `lanes_src` and the raw `tags` sub-object
 * are deliberately skipped: a 2 MB file parses into roughly 10 MB of cJSON DOM
 * and the pool below is the long-lived copy, so it holds the minimum.
 *
 * `surface` USED TO BE ON THAT SKIP LIST and should not have been -- it is on
 * 2238 of La Plata's 2291 ways and it is the one tag here that changes what the
 * driver sees. It is now reduced to a TD5_GEO_SURF_* class (3 values, one int)
 * rather than held as a string, which costs the pool nothing. Same for
 * `junction`, which geo_fetch dropped entirely until 2026-10-07 through a dead
 * `and False` clause, and the direction half of `oneway`. See gate 4 of
 * docs/plans/GEO_TAG_AUDIT.md.
 *
 * WIDTH. OSM tags `lanes` on 45% of ways and `width` (metres, as a free-text
 * string) on 2%; geo_fetch fills the rest from the highway class. Both are
 * reduced HERE to one lane count, because the engine has
 * TD5_TG_SPAN_LENGTH == TD5_TG_LANE_WIDTH and so a street's frontage run is an
 * integer number of lanes -- there is no way to express a 7.3 m street to the
 * mouth table, and pretending otherwise would put the drawn quad and the
 * registered width out of step. A tagged `width` wins over a lane count
 * because it is a measurement rather than a class default.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"          /* td5_env_flag_on */
#include "td5_trackgen.h"        /* TD5_TG_LANE_WIDTH */
#include "td5_geo_roads.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "geo"

/* Caps. Measured on La Plata (2 200 m radius, a dense regular grid): 2 291
 * roads / 11 739 points, longest way 67 points. Sized for roughly 4x that so a
 * denser centre still loads, and enforced rather than trusted -- an oversized
 * file is truncated with a warning, never allowed to run off the pool. */
#define GEO_ROADS_MAX      8192
#define GEO_ROADS_MAX_PTS  65536
/* A way longer than this is split across records by Overpass anyway; the limit
 * only bounds one walk. */
#define GEO_ROADS_MAX_WAY  512
/* ROADS.JSON for a big city centre is larger than td5_geo.c's 8 MB route/place
 * budget, so this reader carries its own. */
#define GEO_ROADS_MAX_FILE (64 * 1024 * 1024)

static struct {
    int          n, np;
    TD5_GeoRoad *road;
    double      *px, *pz;
    char         slug[64];
    char         source[320];
} s_roads;

/* ------------------------------------------------------------------- io --- */

/* Read a whole file into a malloc'd NUL-terminated buffer, for cJSON. Same
 * shape as td5_geo.c's geo_slurp (which is static there, and carries a smaller
 * cap for the rasters' sidecars). */
static char *geo_roads_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)GEO_ROADS_MAX_FILE) {
        td5_plat_file_close(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) {
        td5_plat_file_close(f);
        return NULL;
    }
    if (td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) {
        free(buf);
        td5_plat_file_close(f);
        return NULL;
    }
    buf[n] = '\0';
    td5_plat_file_close(f);
    return buf;
}

static int geo_roads_class(const char *s)
{
    if (!s || !s[0]) return TD5_GEO_RC_UNKNOWN;
    /* `*_link` ways are ramps onto their parent class and carry its
     * importance, so they collapse onto it rather than onto UNKNOWN. */
    if (!strncmp(s, "motorway", 8))      return TD5_GEO_RC_MOTORWAY;
    if (!strncmp(s, "trunk", 5))         return TD5_GEO_RC_TRUNK;
    if (!strncmp(s, "primary", 7))       return TD5_GEO_RC_PRIMARY;
    if (!strncmp(s, "secondary", 9))     return TD5_GEO_RC_SECONDARY;
    if (!strncmp(s, "tertiary", 8))      return TD5_GEO_RC_TERTIARY;
    if (!strcmp(s, "unclassified"))      return TD5_GEO_RC_UNCLASSIFIED;
    if (!strcmp(s, "residential"))       return TD5_GEO_RC_RESIDENTIAL;
    if (!strcmp(s, "living_street"))     return TD5_GEO_RC_LIVING;
    if (!strcmp(s, "service"))           return TD5_GEO_RC_SERVICE;
    return TD5_GEO_RC_UNKNOWN;
}

/* OSM `surface` -> TD5_GEO_SURF_*. An unrecognised or absent value is SMOOTH,
 * which is the no-op: it takes the span's own biome page, exactly as every
 * street did before this tag was read. Defaulting the OTHER way would turn
 * every untagged street to dirt, which is 299 of La Plata's ways. */
static int geo_roads_surface(const char *s)
{
    static const char *const k_cobble[] = {
        "sett", "cobblestone", "unhewn_cobblestone", "stone", NULL
    };
    static const char *const k_loose[] = {
        "unpaved", "dirt", "ground", "earth", "gravel", "fine_gravel",
        "compacted", "pebblestone", "sand", "mud", "grass", "woodchips", NULL
    };
    int i;
    if (!s || !s[0]) return TD5_GEO_SURF_SMOOTH;
    for (i = 0; k_cobble[i]; i++)
        if (!strcmp(s, k_cobble[i])) return TD5_GEO_SURF_COBBLE;
    for (i = 0; k_loose[i]; i++)
        if (!strcmp(s, k_loose[i])) return TD5_GEO_SURF_LOOSE;
    return TD5_GEO_SURF_SMOOTH;
}

static const char *geo_roads_str(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static int geo_roads_bool(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    if (!v) return 0;
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v) ? 1 : 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0;
    return 0;
}

/* Units per metre of the cache's frame, from PLACE.JSON's cell_units / cell_m.
 * Derived rather than hardcoded at 430 so a cache built at another scale still
 * converts a tagged `width` correctly. 0 means "unknown": the caller then
 * ignores every width tag and uses the lane count, which is exactly the
 * behaviour before this module existed. */
static double geo_roads_units_per_metre(const char *slug)
{
    char path[320];
    char *json;
    double upm = 0.0;
    snprintf(path, sizeof path, "re/assets/geo/%s/PLACE.JSON", slug);
    json = geo_roads_slurp(path);
    if (!json) return 0.0;
    {
        cJSON *root = cJSON_Parse(json);
        const cJSON *cu = root ? cJSON_GetObjectItem(root, "cell_units") : NULL;
        const cJSON *cm = root ? cJSON_GetObjectItem(root, "cell_m") : NULL;
        if (cu && cm && cJSON_IsNumber(cu) && cJSON_IsNumber(cm)
            && cm->valuedouble > 1e-6 && cu->valuedouble > 0.0)
            upm = cu->valuedouble / cm->valuedouble;
        cJSON_Delete(root);
    }
    free(json);
    return upm;
}

/* Lane count for one record: a tagged `width` (metres) when it parses, else the
 * `lanes` field geo_fetch already filled from the tag or the highway class. */
static int geo_roads_lanes(const cJSON *r, double upm)
{
    const cJSON *w = cJSON_GetObjectItem(r, "width");
    const cJSON *l = cJSON_GetObjectItem(r, "lanes");
    int lanes = 2;
    if (l && cJSON_IsNumber(l)) lanes = l->valueint;
    if (upm > 0.0 && w && cJSON_IsString(w) && w->valuestring[0]) {
        /* OSM `width` is free text: "7.5", "6", occasionally "10 m". strtod
         * takes the leading number and ignores the rest; a value that does not
         * start with a number leaves `end == start` and is discarded. */
        char *end = NULL;
        const double m = strtod(w->valuestring, &end);
        if (end != w->valuestring && m > 0.5 && m < 120.0) {
            const int fromw = (int)floor(m * upm / (double)TD5_TG_LANE_WIDTH + 0.5);
            if (fromw >= 1) lanes = fromw;
        }
    }
    if (lanes < 1) lanes = 1;
    if (lanes > TD5_GEO_ROADS_LANES_MAX) lanes = TD5_GEO_ROADS_LANES_MAX;
    return lanes;
}

void td5_geo_roads_unload(void)
{
    free(s_roads.road);
    free(s_roads.px);
    free(s_roads.pz);
    memset(&s_roads, 0, sizeof(s_roads));
}

static int geo_roads_load(const char *slug)
{
    char path[320];
    char *json;
    cJSON *root, *arr;
    const double upm = geo_roads_units_per_metre(slug);
    /* The A/B for the road half of the tag round, same shape as
     * TD5RE_GEO_LM_TAGS and TD5RE_GEO_AREA_TAGS: off, the reader ignores
     * `surface` and `roundabout` and every street takes its biome page, which
     * is what it did before 2026-10-07. */
    const int read_tags = td5_env_flag_on("TD5RE_GEO_ROAD_TAGS");
    int n, i, dropped_short = 0, truncated = 0, roundabouts = 0, n_surf[3];

    td5_geo_roads_unload();
    snprintf(path, sizeof path, "re/assets/geo/%s/ROADS.JSON", slug);
    json = geo_roads_slurp(path);
    if (!json) {
        TD5_LOG_W(LOG_TAG, "geo: no readable %s; the street network stays synthetic",
                  path);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_E(LOG_TAG, "geo: %s is not valid JSON", path);
        return 0;
    }
    arr = cJSON_GetObjectItem(root, "roads");
    if (!arr || !cJSON_IsArray(arr) || (n = cJSON_GetArraySize(arr)) < 1) {
        TD5_LOG_E(LOG_TAG, "geo: %s has no roads[]", path);
        cJSON_Delete(root);
        return 0;
    }
    if (n > GEO_ROADS_MAX) { n = GEO_ROADS_MAX; truncated = 1; }

    s_roads.road = (TD5_GeoRoad *)malloc((size_t)n * sizeof(TD5_GeoRoad));
    s_roads.px   = (double *)malloc((size_t)GEO_ROADS_MAX_PTS * sizeof(double));
    s_roads.pz   = (double *)malloc((size_t)GEO_ROADS_MAX_PTS * sizeof(double));
    if (!s_roads.road || !s_roads.px || !s_roads.pz) {
        TD5_LOG_E(LOG_TAG, "geo: out of memory for %d road(s)", n);
        cJSON_Delete(root);
        td5_geo_roads_unload();
        return 0;
    }

    for (i = 0; i < n; i++) {
        const cJSON *r   = cJSON_GetArrayItem(arr, i);
        const cJSON *pts = r ? cJSON_GetObjectItem(r, "points") : NULL;
        const cJSON *cl  = r ? cJSON_GetObjectItem(r, "class") : NULL;
        const cJSON *ly  = r ? cJSON_GetObjectItem(r, "layer") : NULL;
        TD5_GeoRoad *out;
        int k, npt, kept = 0;

        if (!pts || !cJSON_IsArray(pts)) continue;
        npt = cJSON_GetArraySize(pts);
        if (npt < 2) { dropped_short++; continue; }
        if (npt > GEO_ROADS_MAX_WAY) { npt = GEO_ROADS_MAX_WAY; truncated = 1; }
        if (s_roads.np + npt > GEO_ROADS_MAX_PTS) { truncated = 1; break; }

        out = &s_roads.road[s_roads.n];
        memset(out, 0, sizeof(*out));
        out->first = s_roads.np;
        for (k = 0; k < npt; k++) {
            const cJSON *p  = cJSON_GetArrayItem(pts, k);
            const cJSON *px = p ? cJSON_GetObjectItem(p, "x") : NULL;
            const cJSON *pz = p ? cJSON_GetObjectItem(p, "z") : NULL;
            double x, z;
            if (!px || !pz || !cJSON_IsNumber(px) || !cJSON_IsNumber(pz)) continue;
            x = px->valuedouble; z = pz->valuedouble;
            /* Drop a repeated vertex: a zero-length segment has no bearing, and
             * the network's junction walk would divide by its length. */
            if (kept > 0) {
                const double dx = x - s_roads.px[out->first + kept - 1];
                const double dz = z - s_roads.pz[out->first + kept - 1];
                if (dx * dx + dz * dz < 1.0) continue;
            }
            s_roads.px[out->first + kept] = x;
            s_roads.pz[out->first + kept] = z;
            if (kept == 0) {
                out->minx = out->maxx = x;
                out->minz = out->maxz = z;
            } else {
                if (x < out->minx) out->minx = x;
                if (x > out->maxx) out->maxx = x;
                if (z < out->minz) out->minz = z;
                if (z > out->maxz) out->maxz = z;
            }
            kept++;
        }
        if (kept < 2) { dropped_short++; continue; }

        out->count  = kept;
        out->klass  = geo_roads_class(cl && cJSON_IsString(cl) ? cl->valuestring : NULL);
        out->lanes  = geo_roads_lanes(r, upm);
        out->width  = (double)out->lanes * (double)TD5_TG_LANE_WIDTH;
        out->oneway = geo_roads_bool(r, "oneway");
        out->bridge = geo_roads_bool(r, "bridge");
        out->tunnel = geo_roads_bool(r, "tunnel");
        out->layer  = (ly && cJSON_IsNumber(ly)) ? ly->valueint : 0;
        /* All three absent from a pre-2026-10-07 cache, and all three default
         * to the old behaviour there: SMOOTH, two-way, not a roundabout. */
        out->surface = read_tags
                     ? geo_roads_surface(geo_roads_str(r, "surface")) : 0;
        {
            const cJSON *od = cJSON_GetObjectItem(r, "oneway_dir");
            out->oneway_dir = (od && cJSON_IsNumber(od)) ? od->valueint
                            : (out->oneway ? 1 : 0);
        }
        out->roundabout = read_tags ? geo_roads_bool(r, "roundabout") : 0;
        if (out->roundabout) roundabouts++;
        s_roads.np += kept;
        s_roads.n++;
    }
    cJSON_Delete(root);

    if (s_roads.n < 1) {
        TD5_LOG_E(LOG_TAG, "geo: %s held no usable road", path);
        td5_geo_roads_unload();
        return 0;
    }
    snprintf(s_roads.slug, sizeof s_roads.slug, "%s", slug);
    snprintf(s_roads.source, sizeof s_roads.source, "%s", path);
    TD5_LOG_I(LOG_TAG, "geo: roads %s loaded: %d way(s) %d point(s)%s "
              "(%d too short); scale %.1f units/m",
              path, s_roads.n, s_roads.np, truncated ? " [TRUNCATED at a cap]" : "",
              dropped_short, upm);
    /* The surface census is the attributable half of the surface change: a
     * street that comes out cobbled or loose should be traceable to a count
     * here rather than noticed in a framedump. */
    n_surf[0] = n_surf[1] = n_surf[2] = 0;
    for (i = 0; i < s_roads.n; i++) {
        const int s = s_roads.road[i].surface;
        if (s >= 0 && s < 3) n_surf[s]++;
    }
    TD5_LOG_I(LOG_TAG, "geo: roads surface: %d smooth, %d cobbled, %d loose; "
              "%d roundabout way(s)%s",
              n_surf[0], n_surf[1], n_surf[2], roundabouts,
              read_tags ? "" : " [TD5RE_GEO_ROAD_TAGS=0: tags ignored]");
    return 1;
}

int td5_geo_roads_sync(const char *slug)
{
    if (!slug) slug = "";
    if (!slug[0]) {
        if (s_roads.n) td5_geo_roads_unload();
        return 0;
    }
    if (s_roads.n && !strcmp(slug, s_roads.slug)) return 1;
    return geo_roads_load(slug);
}

int         td5_geo_roads_count(void)  { return s_roads.n; }
int         td5_geo_roads_points(void) { return s_roads.np; }
const char *td5_geo_roads_source(void) { return s_roads.n ? s_roads.source : ""; }

const TD5_GeoRoad *td5_geo_roads_get(int i)
{
    return (i >= 0 && i < s_roads.n) ? &s_roads.road[i] : NULL;
}

int td5_geo_roads_point(const TD5_GeoRoad *r, int k, double *x, double *z)
{
    if (!r || k < 0 || k >= r->count) return 0;
    if (x) *x = s_roads.px[r->first + k];
    if (z) *z = s_roads.pz[r->first + k];
    return 1;
}
