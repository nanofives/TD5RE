/**
 * td5_roam_cache.c -- GEO FREE ROAM: the cache ROADS reader (PORT-ONLY).
 *
 * Contract and rationale in td5_roam_cache.h. The record parse is the strip's
 * own (td5_geo_roads_parse_record, td5_geo_roads.c), so a way reads the same in
 * both readers; what is roam-only is the session table, the way id, the node
 * keys, the lane count and the dev census.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"          /* td5_env_flag_on */
#include "td5_trackgen.h"        /* TD5_TG_LANE_WIDTH */
#include "td5_geo.h"             /* td5_geo_source_path */
#include "td5_geo_roads.h"
#include "td5_geo_sidewalk.h"    /* the place pavement table, for the lane floor */
#include "td5_roam_cache.h"
#include "td5_geo_frame.h"       /* [roam M] the geographic -> game reflection */
#include "deps/cjson/cJSON.h"

#define LOG_TAG "roam"

/* First capacities. The casco window holds ~2.2k ways; the whole La Plata cache
 * ~17k. Each array doubles from here. */
#define ROAM_WAYS_FIRST   4096
#define ROAM_PTS_FIRST    65536
#define ROAM_NAMES_FIRST  256

typedef struct {
    int    ix, iz, n;             /* INDEX row                              */
    double x0, z0, x1, z1;        /* extent of everything the tile holds    */
    int    loaded;                /* parsed once per session                */
    int    nways, npts;           /* what that parse put into the session   */
} RoamTile;

/* THE SESSION TABLE. The one file-scope mutable object in this module: it is
 * filled by td5_roam_cache_session_begin / _load_window and freed by _end. */
static struct {
    int          live;
    char         slug[64];
    int          ntiles;
    RoamTile    *tile;
    int          nway, cap_way;
    TD5_RoamWay *way;
    int          np, cap_pt;
    double      *px, *pz;             /* place-frame points                  */
    int64_t     *nk;                  /* 10-unit node key per point          */
    int          nname, cap_name;
    char       (*name)[TD5_GEO_ROADS_NAME_MAX];
    int          tiles_read;
} s_roam;

/* ------------------------------------------------------------ growth -------- */

/* Grows one array to hold `need` elements of `elt` bytes. Returns the block (the
 * old one when it already fits, a moved one after realloc), or NULL on out of
 * memory with the old block still valid and *cap unchanged. */
static void *roam_grow(void *arr, int *cap, int need, int first, size_t elt)
{
    int nc;
    void *nb;
    if (need <= *cap) return arr;
    nc = *cap ? *cap : first;
    while (nc < need) nc *= 2;
    nb = realloc(arr, (size_t)nc * elt);
    if (nb) *cap = nc;
    return nb;
}

/* The three point arrays share one capacity, so they grow together. */
static int roam_reserve_pts(int need)
{
    int nc;
    double *nx;
    double *nz;
    int64_t *nk;
    if (need <= s_roam.cap_pt) return 1;
    nc = s_roam.cap_pt ? s_roam.cap_pt : ROAM_PTS_FIRST;
    while (nc < need) nc *= 2;
    nx = (double *)realloc(s_roam.px, (size_t)nc * sizeof(double));
    if (!nx) return 0;
    s_roam.px = nx;
    nz = (double *)realloc(s_roam.pz, (size_t)nc * sizeof(double));
    if (!nz) return 0;
    s_roam.pz = nz;
    nk = (int64_t *)realloc(s_roam.nk, (size_t)nc * sizeof(int64_t));
    if (!nk) return 0;
    s_roam.nk = nk;
    s_roam.cap_pt = nc;
    return 1;
}

/* ------------------------------------------------------- attributes -------- */

int64_t td5_roam_node_key(double x, double z)
{
    /* Same rule as the router's gr_node_of (td5_geo_route.c, GR_SNAP 10,
     * nearbyint = round half to even). Packed so the key is one int64. */
    const int64_t kx = (int64_t)nearbyint(x / 10.0);
    const int64_t kz = (int64_t)nearbyint(z / 10.0);
    return (int64_t)(((uint64_t)kx << 32) | (uint64_t)(uint32_t)kz);
}

int td5_roam_lane_count(const cJSON *r, double upm, int parsed_lanes, int namek)
{
    const cJSON *wd = cJSON_GetObjectItem(r, "width");
    const cJSON *ls = cJSON_GetObjectItem(r, "lanes_src");
    const int tagged = wd && ((cJSON_IsString(wd) && wd->valuestring[0]
                               && atof(wd->valuestring) > 0.5)
                              || (cJSON_IsNumber(wd) && wd->valuedouble > 0.5));
    const int counted = ls && cJSON_IsString(ls) && ls->valuestring
                     && strcmp(ls->valuestring, "osm_lanes") == 0;
    int lanes = parsed_lanes;
    /* The carriageway floor, the same rule as the strip's (td5_geo_roads.c).
     * A tagged width is a measurement and is never overridden. */
    if (upm > 0.0 && !tagged) {
        const double tm = td5_geo_sw_carriageway_m(TD5_GEO_RC_UNKNOWN, namek);
        if (tm > 0.0) {
            int want = (int)floor(tm * upm / (double)TD5_TG_LANE_WIDTH + 0.5);
            if (counted && want < lanes) want = lanes;
            if (want >= 1 && want <= TD5_GEO_ROADS_LANES_MAX) lanes = want;
        }
    }
    return lanes;
}

/* Index of the way's name in the session pool, interning it when new. -1 for an
 * unnamed way or a full pool. The string is copied, so the DOM may go. */
static int roam_intern(const cJSON *r)
{
    const cJSON *nm = cJSON_GetObjectItem(r, "name");
    const char *s = (nm && cJSON_IsString(nm)) ? nm->valuestring : NULL;
    size_t n;
    int i;
    void *nb;
    if (!s || !s[0]) return -1;
    for (i = 0; i < s_roam.nname; i++)
        if (!strcmp(s_roam.name[i], s)) return i;
    nb = roam_grow(s_roam.name, &s_roam.cap_name, s_roam.nname + 1,
                   ROAM_NAMES_FIRST, sizeof *s_roam.name);
    if (!nb) return -1;
    s_roam.name = (char (*)[TD5_GEO_ROADS_NAME_MAX])nb;
    /* Truncate on a UTF-8 boundary, as the strip's pool does. */
    n = strlen(s);
    if (n >= TD5_GEO_ROADS_NAME_MAX) {
        n = TD5_GEO_ROADS_NAME_MAX - 1u;
        while (n > 0 && ((unsigned char)s[n] & 0xC0u) == 0x80u) n--;
    }
    memcpy(s_roam.name[s_roam.nname], s, n);
    s_roam.name[s_roam.nname][n] = '\0';
    return s_roam.nname++;
}

/* ------------------------------------------------------------ parsing ------ */

static int roam_str_is(const cJSON *o, const char *key, const char *val)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return v && cJSON_IsString(v) && v->valuestring && !strcmp(v->valuestring, val);
}

/* [F1 T3] The raw tag facts behind TD5_ROAM_TAG_* (see the header). */
static uint32_t roam_tag_bits(const cJSON *r)
{
    const cJSON *tags = cJSON_GetObjectItem(r, "tags");
    uint32_t f = 0;
    if (roam_str_is(r, "class", "bus_stop")) f |= TD5_ROAM_TAG_HW_BUS_STOP;
    if (roam_str_is(r, "class", "razed"))    f |= TD5_ROAM_TAG_HW_RAZED;
    if (roam_str_is(r, "class", "services")) f |= TD5_ROAM_TAG_HW_SERVICES;
    if (roam_str_is(r, "class", "raceway"))  f |= TD5_ROAM_TAG_HW_RACEWAY;
    if (roam_str_is(r, "tunnel_kind", "building_passage"))
        f |= TD5_ROAM_TAG_TUNNEL_PASSAGE;
    if (tags && cJSON_IsObject(tags)) {
        if (roam_str_is(tags, "area", "yes"))              f |= TD5_ROAM_TAG_AREA;
        if (roam_str_is(tags, "service", "parking_aisle")) f |= TD5_ROAM_TAG_SVC_PARKING_AISLE;
        if (roam_str_is(tags, "service", "drive-through")) f |= TD5_ROAM_TAG_SVC_DRIVE_THROUGH;
        if (roam_str_is(tags, "service", "driveway"))      f |= TD5_ROAM_TAG_SVC_DRIVEWAY;
        if (roam_str_is(tags, "service", "alley"))         f |= TD5_ROAM_TAG_SVC_ALLEY;
        if (roam_str_is(tags, "access", "private") || roam_str_is(tags, "access", "no"))
            f |= TD5_ROAM_TAG_ACCESS_PRIVATE;
    }
    return f;
}

/* One ROADS record -> one session way. Returns 1 when the way was kept. Nothing
 * is committed to the session until the way is complete, so a failed record
 * leaves no half-written points behind. */
static int roam_add_way(const cJSON *r, double upm, int tile)
{
    const cJSON *pts = cJSON_GetObjectItem(r, "points");
    const cJSON *idj = cJSON_GetObjectItem(r, "id");
    TD5_RoamWay *w;
    double bb[4];
    void *nb;
    int npt, kept, first, k;

    if (!pts || !cJSON_IsArray(pts)) return 0;
    npt = cJSON_GetArraySize(pts);
    if (npt < 2) return 0;
    if (npt > TD5_GEO_ROADS_WAY_POINTS_MAX) npt = TD5_GEO_ROADS_WAY_POINTS_MAX;
    if (!roam_reserve_pts(s_roam.np + npt)) return 0;
    first = s_roam.np;
    kept = td5_geo_roads_parse_points(pts, npt, &s_roam.px[first],
                                      &s_roam.pz[first], bb);
    if (kept < 2) return 0;
    nb = roam_grow(s_roam.way, &s_roam.cap_way, s_roam.nway + 1,
                   ROAM_WAYS_FIRST, sizeof *s_roam.way);
    if (!nb) return 0;
    s_roam.way = (TD5_RoamWay *)nb;

    for (k = 0; k < kept; k++)
        s_roam.nk[first + k] = td5_roam_node_key(s_roam.px[first + k],
                                                 s_roam.pz[first + k]);
    w = &s_roam.way[s_roam.nway];
    memset(w, 0, sizeof *w);
    w->way_id = (idj && cJSON_IsNumber(idj)) ? (int64_t)idj->valuedouble : -1;
    w->tile = tile;
    w->tagf = roam_tag_bits(r);
    w->road.first = first;
    w->road.count = kept;
    w->road.minx = bb[0];
    w->road.minz = bb[1];
    w->road.maxx = bb[2];
    w->road.maxz = bb[3];
    td5_geo_roads_parse_record(r, upm, &w->road);
    w->lanes_osm = w->road.lanes;
    {
        const cJSON *wd = cJSON_GetObjectItem(r, "width");
        const cJSON *ls = cJSON_GetObjectItem(r, "lanes_src");
        w->width_tagged = (uint8_t)(wd && ((cJSON_IsString(wd) && wd->valuestring[0]
                                            && atof(wd->valuestring) > 0.5)
                                           || (cJSON_IsNumber(wd) && wd->valuedouble > 0.5)));
        w->lanes_counted = (uint8_t)(ls && cJSON_IsString(ls) && ls->valuestring
                                     && strcmp(ls->valuestring, "osm_lanes") == 0);
    }
    w->road.lanes = td5_roam_lane_count(r, upm, w->road.lanes, w->road.namek);
    w->road.width = (double)w->road.lanes * (double)TD5_TG_LANE_WIDTH;
    w->road.name_id = roam_intern(r);
    s_roam.np += kept;
    s_roam.nway++;
    return 1;
}

/* Parses one ROADS tile into the session. Marks it loaded first, so a tile that
 * fails to read is not retried on every later window. */
static void roam_parse_tile(int ti, double upm, TD5_RoamCacheCensus *c)
{
    RoamTile *t = &s_roam.tile[ti];
    char rel[64];
    char path[400];
    char *json;
    cJSON *doc, *arr, *rec;
    int pts0 = s_roam.np, ways0 = s_roam.nway, dropped = 0;

    t->loaded = 1;
    s_roam.tiles_read++;
    c->tiles_read++;
    snprintf(rel, sizeof rel, "tiles/ROADS_%d_%d.JSON", t->ix, t->iz);
    td5_geo_source_path(path, sizeof path, s_roam.slug, rel);
    json = td5_geo_roads_slurp(path);
    if (!json) {
        TD5_LOG_W(LOG_TAG, "roam cache: tile %s is missing or unreadable", path);
        return;
    }
    doc = cJSON_Parse(json);
    free(json);
    if (!doc) {
        TD5_LOG_W(LOG_TAG, "roam cache: tile %s is not valid JSON", path);
        return;
    }
    td5_geo_src_json_to_game(doc);       /* [roam M] source frame -> game frame */
    arr = cJSON_GetObjectItem(doc, "roads");
    if (arr && cJSON_IsArray(arr)) {
        /* Child walk, not cJSON_GetArrayItem: that is O(n) per call. */
        for (rec = arr->child; rec; rec = rec->next)
            if (!roam_add_way(rec, upm, ti)) dropped++;
    }
    t->nways = s_roam.nway - ways0;
    t->npts  = s_roam.np - pts0;
    if (dropped)
        TD5_LOG_I(LOG_TAG, "roam cache: tile %s: %d record(s) dropped (too short "
                  "or out of memory)", path, dropped);
    cJSON_Delete(doc);
}

/* ----------------------------------------------------------- session -------- */

void td5_roam_cache_session_end(void)
{
    free(s_roam.tile);
    free(s_roam.way);
    free(s_roam.px);
    free(s_roam.pz);
    free(s_roam.nk);
    free(s_roam.name);
    memset(&s_roam, 0, sizeof s_roam);
}

int td5_roam_cache_session_begin(const char *slug)
{
    char path[400];
    char *json;
    cJSON *root, *layers, *roads, *tl, *t;
    int n = 0;

    td5_roam_cache_session_end();
    if (!slug || !slug[0]) return 0;
    td5_geo_source_path(path, sizeof path, slug, "tiles/INDEX.JSON");
    json = td5_geo_roads_slurp(path);
    if (!json) {
        TD5_LOG_E(LOG_TAG, "roam cache: no readable %s", path);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    layers = root ? cJSON_GetObjectItem(root, "layers") : NULL;
    roads  = layers ? cJSON_GetObjectItem(layers, "ROADS") : NULL;
    tl     = roads ? cJSON_GetObjectItem(roads, "tiles") : NULL;
    if (!tl || !cJSON_IsArray(tl)) {
        TD5_LOG_E(LOG_TAG, "roam cache: %s has no layers.ROADS.tiles[]", path);
        if (root) cJSON_Delete(root);
        return 0;
    }
    s_roam.tile = (RoamTile *)calloc((size_t)cJSON_GetArraySize(tl) + 1u,
                                     sizeof(RoamTile));
    if (!s_roam.tile) {
        cJSON_Delete(root);
        return 0;
    }
    /* Row = [ix, iz, n, minx, minz, maxx, maxz] (geo_region.py build_index). */
    for (t = tl->child; t; t = t->next) {
        const cJSON *a = t->child;
        double v[7];
        int k;
        for (k = 0; k < 7 && a; k++, a = a->next) v[k] = a->valuedouble;
        if (k < 7) continue;
        td5_geo_src_bbox_to_game(&v[3], &v[5]);   /* [roam M] the extent in the game frame */
        s_roam.tile[n].ix = (int)v[0];
        s_roam.tile[n].iz = (int)v[1];
        s_roam.tile[n].n  = (int)v[2];
        s_roam.tile[n].x0 = v[3];
        s_roam.tile[n].z0 = v[4];
        s_roam.tile[n].x1 = v[5];
        s_roam.tile[n].z1 = v[6];
        n++;
    }
    cJSON_Delete(root);
    s_roam.ntiles = n;
    s_roam.live = 1;
    snprintf(s_roam.slug, sizeof s_roam.slug, "%s", slug);
    TD5_LOG_I(LOG_TAG, "roam cache: session %s: %d ROADS tile row(s)", slug, n);
    return n;
}

int td5_roam_cache_load_window(const double win[4], double halo,
                               TD5_RoamCacheCensus *c)
{
    const uint32_t t0 = td5_plat_time_ms();
    double upm;
    int i;
    if (!c) return -1;
    memset(c, 0, sizeof *c);
    if (!s_roam.live || !win) return -1;
    upm = td5_geo_roads_units_per_metre(s_roam.slug);
    /* The lane floor reads the place's pavement table. Idempotent, and the strip
     * selects the same place, so the two readers agree. */
    td5_geo_sw_place(s_roam.slug);
    /* The tile rule of gr_tiled_load (td5_geo_route.c:774): skip a tile whose
     * extent misses the window. */
    for (i = 0; i < s_roam.ntiles; i++) {
        if (s_roam.tile[i].x1 < win[0] - halo || s_roam.tile[i].x0 > win[2] + halo ||
            s_roam.tile[i].z1 < win[1] - halo || s_roam.tile[i].z0 > win[3] + halo)
            continue;
        c->tiles_selected++;
        if (!s_roam.tile[i].loaded) roam_parse_tile(i, upm, c);
        c->ways   += s_roam.tile[i].nways;
        c->points += s_roam.tile[i].npts;
    }
    c->names = s_roam.nname;
    c->ms = td5_plat_time_ms() - t0;
    TD5_LOG_I(LOG_TAG, "roam cache census: window (%.0f,%.0f)-(%.0f,%.0f) halo %.0f: "
              "%d tile(s) selected, %d read, %d way(s), %d point(s), %d name(s), %u ms",
              win[0], win[1], win[2], win[3], halo, c->tiles_selected, c->tiles_read,
              c->ways, c->points, c->names, (unsigned)c->ms);
    return c->ways;
}

int td5_roam_cache_way_count(void)
{
    return s_roam.nway;
}

const TD5_RoamWay *td5_roam_cache_way(int i)
{
    return (i >= 0 && i < s_roam.nway) ? &s_roam.way[i] : NULL;
}

const char *td5_roam_cache_name(int id)
{
    return (s_roam.live && id >= 0 && id < s_roam.nname) ? s_roam.name[id] : NULL;
}

int td5_roam_cache_point(const TD5_RoamWay *w, int k, double *x, double *z,
                         int64_t *node_key)
{
    int at;
    if (!w || k < 0 || k >= w->road.count) return 0;
    at = w->road.first + k;
    if (x) *x = s_roam.px[at];
    if (z) *z = s_roam.pz[at];
    if (node_key) *node_key = s_roam.nk[at];
    return 1;
}

/* -------------------------------------------------------- dev census -------- */

#ifndef TD5RE_RELEASE

/* The casco polygon, F0a's (_archive/roam/F0a/casco_poly.json, world frame): the
 * four avenue corners in ring order. Same quad measure_poly.py clips to. */
static const double k_casco_poly[4][2] = {
    { -1078705.99,        5374175.73 },     /* [roam M] game frame: x = -east */
    { -2582491.695195345, 7019045.535332896 },
    { -4255470.01,        5615132.29 },
    { -2731713.44,        3880755.48 },
};

/* Ray-casting point in polygon, the same test as F0a (measure_poly.py pip). */
static int roam_pip(const double (*poly)[2], int n, double x, double z)
{
    int i, j = n - 1, inside = 0;
    for (i = 0; i < n; i++) {
        const double xi = poly[i][0], zi = poly[i][1];
        const double xj = poly[j][0], zj = poly[j][1];
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi)
            inside = !inside;
        j = i;
    }
    return inside;
}

/* F0a's rule (measure_poly.py measure_poly): a way touches the polygon when one of
 * its vertices, or the midpoint of one of its segments, is inside. */
static int roam_way_touches(const TD5_RoamWay *w)
{
    const int f = w->road.first, n = w->road.count;
    int k;
    for (k = 0; k < n; k++)
        if (roam_pip(k_casco_poly, 4, s_roam.px[f + k], s_roam.pz[f + k])) return 1;
    for (k = 0; k + 1 < n; k++) {
        const double mx = 0.5 * (s_roam.px[f + k] + s_roam.px[f + k + 1]);
        const double mz = 0.5 * (s_roam.pz[f + k] + s_roam.pz[f + k + 1]);
        if (roam_pip(k_casco_poly, 4, mx, mz)) return 1;
    }
    return 0;
}

int td5_roam_cache_dev_census(void)
{
    static const char slug[] = "la_plata_partido";
    TD5_RoamCacheCensus c;
    double win[4] = { k_casco_poly[0][0], k_casco_poly[0][1],
                      k_casco_poly[0][0], k_casco_poly[0][1] };
    char line[512];
    char path[400];
    const char *dir;
    FILE *f;
    int i, touch = 0;

    /* td5_env_flag_on is ON when unset, so the opt-in must be the "1" test. */
    if (!td5_env_flag_off("TD5RE_ROAM_CACHE_TEST")) return 0;
    for (i = 1; i < 4; i++) {
        const double x = k_casco_poly[i][0], z = k_casco_poly[i][1];
        if (x < win[0]) win[0] = x;
        if (z < win[1]) win[1] = z;
        if (x > win[2]) win[2] = x;
        if (z > win[3]) win[3] = z;
    }
    if (td5_roam_cache_session_begin(slug) <= 0) {
        TD5_LOG_E(LOG_TAG, "roam cache test: no ROADS tiles for %s", slug);
        return 1;
    }
    td5_roam_cache_load_window(win, 0.0, &c);
    for (i = 0; i < s_roam.nway; i++)
        if (roam_way_touches(&s_roam.way[i])) touch++;
    snprintf(line, sizeof line,
             "roam cache test: casco polygon (F0a rule): %d way(s) touch the polygon "
             "of %d held in %d selected tile(s) (%d points, %d names, %u ms); "
             "F0a says 2170. Gap = %d way(s) in the selected tiles that miss the polygon",
             touch, c.ways, c.tiles_selected, c.points, c.names, (unsigned)c.ms,
             c.ways - touch);
    TD5_LOG_I(LOG_TAG, "%s", line);
    dir = td5_plat_log_dir();
    snprintf(path, sizeof path, "%sroam_cache_census.txt", dir ? dir : "");
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    td5_roam_cache_session_end();
    return 1;
}

#endif /* !TD5RE_RELEASE */
