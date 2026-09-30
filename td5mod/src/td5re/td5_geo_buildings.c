/**
 * td5_geo_buildings.c -- GEO TRACK: real OSM building footprints and area
 *                        polygons (PORT-ONLY). Contract in td5_geo_buildings.h.
 *
 * Pure reader plus polygon geometry. It writes no meshes and knows nothing
 * about trackgen: the emitters in td5_tg_city.c and td5_tg_streets.c ask it for
 * rings, heights and provenance and do all the world-building themselves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_geo_buildings.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "geo"

/* Fallback when PLACE.JSON carries no projection.units_per_metre. Four shipped
 * constants agree on this factor (the plan's section 2); it is only a fallback
 * because the cache states its own and a future place may rescale. */
#define GEOB_UNITS_PER_M_DEFAULT 430.0

/* Refusal caps. A dense city centre holds ten thousand ways in a 2 km box (the
 * plan's section 9), and the whole point of these is that an oversized cache
 * degrades to "fewer real buildings" rather than to a failed build. */
#define GEOB_MAX_BUILDINGS 8192
#define GEOB_MAX_AREAS     2048
#define GEOB_MAX_POINTS    262144
/* BUILDINGS.JSON is 1.5 MB at La Plata; 48 MB covers a far denser place and
 * still refuses a file that is not what we think it is. */
#define GEOB_MAX_JSON      (48 * 1024 * 1024)

static struct {
    int    loaded;
    char   slug[64];
    double units_per_m;
    double storey_m;

    TD5_GeoBuilding *b;
    int    nb;
    TD5_GeoArea     *a;
    int    na;

    double *px, *pz;          /* shared ring point pool */
    int    npt;

    int   *b_head, *a_head;   /* per-span chain heads, bound_spans entries */
    int   *b_next, *a_next;
    int    bound_spans;       /* route nodes the chains cover, 0 = unbound  */
    int    b_bound, b_far, a_bound, a_far;

    int    dec_polys, dec_points;   /* decimation cost */
} s_gb;

/* ----------------------------------------------------------------- io ------ */

static char *geob_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)GEOB_MAX_JSON) {
        TD5_LOG_W(LOG_TAG, "geob: %s is %lld bytes, outside 1..%d; skipped",
                  path, (long long)n, GEOB_MAX_JSON);
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

/* --------------------------------------------------------------- geometry -- */

double td5_geob_ring_area(const double *x, const double *z, int n)
{
    double a = 0.0;
    int i, j;
    if (n < 3) return 0.0;
    for (i = 0, j = n - 1; i < n; j = i++)
        a += x[j] * z[i] - x[i] * z[j];
    return a * 0.5;
}

int td5_geob_point_in_ring(const double *x, const double *z, int n,
                           double px, double pz)
{
    int i, j, in = 0;
    if (n < 3) return 0;
    for (i = 0, j = n - 1; i < n; j = i++) {
        if (((z[i] > pz) != (z[j] > pz))
            && px < (x[j] - x[i]) * (pz - z[i]) / (z[j] - z[i]) + x[i])
            in = !in;
    }
    return in;
}

/* Is p strictly inside triangle (a,b,c), for a CCW triangle? */
static int geob_in_tri(double ax, double az, double bx, double bz,
                       double cx, double cz, double px, double pz)
{
    const double d1 = (bx - ax) * (pz - az) - (bz - az) * (px - ax);
    const double d2 = (cx - bx) * (pz - bz) - (cz - bz) * (px - bx);
    const double d3 = (ax - cx) * (pz - cz) - (az - cz) * (px - cx);
    return d1 >= 0.0 && d2 >= 0.0 && d3 >= 0.0;
}

/* Ear clipping. O(n^2) on rings of at most TD5_GEOB_RING_MAX points, so the
 * worst case here is 48*48 -- cheaper than the mesh bytes it produces.
 *
 * WHY NOT A CENTROID FAN. A fan is two lines of code and is wrong for exactly
 * the footprints that matter: an L-shaped block or a cruciform church has a
 * centroid OUTSIDE its own outline, so fan triangles spill past the walls and
 * the roof reads as a slab hanging over the street. Ear clipping keeps every
 * triangle inside the ring by construction. */
int td5_geob_triangulate(const double *x, const double *z, int n,
                         int *tri, int maxtri)
{
    int idx[TD5_GEOB_RING_MAX];
    int m = n, ntri = 0, guard, i;

    if (n < 3 || n > TD5_GEOB_RING_MAX || maxtri < n - 2) return 0;
    /* Work on a CCW copy: the ear test below assumes positive orientation. */
    if (td5_geob_ring_area(x, z, n) < 0.0)
        for (i = 0; i < n; i++) idx[i] = n - 1 - i;
    else
        for (i = 0; i < n; i++) idx[i] = i;

    /* Each pass must remove at least one ear; `guard` bounds the loop so a
     * self-intersecting ring (OSM has them) stops rather than spins. */
    guard = 2 * n;
    while (m > 3 && guard-- > 0) {
        int clipped = 0;
        for (i = 0; i < m; i++) {
            const int ia = idx[(i + m - 1) % m], ib = idx[i], ic = idx[(i + 1) % m];
            const double ax = x[ia], az = z[ia];
            const double bx = x[ib], bz = z[ib];
            const double cx = x[ic], cz = z[ic];
            const double cross = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
            int k, ok = 1;
            if (cross <= 0.0) continue;            /* reflex or degenerate */
            for (k = 0; k < m && ok; k++) {
                const int ip = idx[k];
                if (ip == ia || ip == ib || ip == ic) continue;
                if (geob_in_tri(ax, az, bx, bz, cx, cz, x[ip], z[ip])) ok = 0;
            }
            if (!ok) continue;
            tri[ntri * 3 + 0] = ia;
            tri[ntri * 3 + 1] = ib;
            tri[ntri * 3 + 2] = ic;
            ntri++;
            for (k = i; k < m - 1; k++) idx[k] = idx[k + 1];
            m--;
            clipped = 1;
            break;
        }
        if (!clipped) break;    /* no ear found: ring is not simple */
    }
    if (m == 3 && ntri + 1 <= maxtri) {
        tri[ntri * 3 + 0] = idx[0];
        tri[ntri * 3 + 1] = idx[1];
        tri[ntri * 3 + 2] = idx[2];
        ntri++;
    }
    return ntri;
}

/* ------------------------------------------------------------------ parse -- */

static unsigned int geob_id_hash(double id)
{
    /* The way id arrives as a JSON number, so it can exceed int range. Fold it
     * through a double->unsigned pair rather than truncating, so two ids that
     * differ only above 2^32 still pick different pages. */
    const double a = floor(fabs(id));
    unsigned int lo = (unsigned int)fmod(a, 4294967296.0);
    unsigned int hi = (unsigned int)fmod(floor(a / 4294967296.0), 4294967296.0);
    unsigned int h = lo * 2654435761u ^ (hi + 0x9E3779B9u);
    h ^= h >> 15;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h;
}

static int geob_roof_of(const char *s)
{
    if (!s || !s[0]) return TD5_GEOB_ROOF_NONE;
    if (!strcmp(s, "flat")) return TD5_GEOB_ROOF_FLAT;
    if (!strcmp(s, "mansard") || !strcmp(s, "gambrel")) return TD5_GEOB_ROOF_MANSARD;
    /* Everything else OSM tags is some kind of slope: gabled, hipped,
     * half-hipped, pyramidal, skillion, round, dome, onion, gabled_with_... */
    return TD5_GEOB_ROOF_PITCHED;
}

static int geob_area_kind_of(const char *leisure, const char *landuse)
{
    if (leisure) {
        if (!strcmp(leisure, "park"))       return TD5_GEOA_KIND_PARK;
        if (!strcmp(leisure, "common"))     return TD5_GEOA_KIND_PARK;
        if (!strcmp(leisure, "pitch"))      return TD5_GEOA_KIND_PITCH;
        if (!strcmp(leisure, "playground")) return TD5_GEOA_KIND_PLAY;
        if (!strcmp(leisure, "garden"))     return TD5_GEOA_KIND_PARK;
    }
    if (landuse) {
        if (!strcmp(landuse, "grass"))         return TD5_GEOA_KIND_GRASS;
        if (!strcmp(landuse, "village_green")) return TD5_GEOA_KIND_PARK;
        if (!strcmp(landuse, "forest"))        return TD5_GEOA_KIND_FOREST;
        if (!strcmp(landuse, "meadow"))        return TD5_GEOA_KIND_GRASS;
    }
    return TD5_GEOA_KIND_OTHER;
}

static const char *geob_str(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static double geob_num(const cJSON *o, const char *key, double def)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : def;
}

static int geob_true(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return v && cJSON_IsTrue(v);
}

/* Copy one `points` array into the shared pool as an OPEN, CCW ring.
 * Returns the number of pool points written (0 = rejected). */
static int geob_push_ring(const cJSON *pts, int *out_first,
                          double *out_cx, double *out_cz, double *out_r)
{
    double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX];
    int raw, n = 0, i, stride;
    double cx = 0.0, cz = 0.0, r = 0.0;

    if (!pts || !cJSON_IsArray(pts)) return 0;
    raw = cJSON_GetArraySize(pts);
    /* geo_fetch closes every ring, so the last point repeats the first. */
    if (raw >= 2) {
        const cJSON *p0 = cJSON_GetArrayItem(pts, 0);
        const cJSON *pl = cJSON_GetArrayItem(pts, raw - 1);
        if (geob_num(p0, "x", 0.0) == geob_num(pl, "x", 1.0)
            && geob_num(p0, "z", 0.0) == geob_num(pl, "z", 1.0))
            raw--;
    }
    if (raw < 3) return 0;

    /* DECIMATE rather than truncate: a uniform stride keeps the silhouette and
     * only loses detail, where cutting the tail would leave an open ring that
     * closes across the middle of the building. */
    stride = (raw + TD5_GEOB_RING_MAX - 1) / TD5_GEOB_RING_MAX;
    if (stride < 1) stride = 1;
    if (stride > 1) { s_gb.dec_polys++; s_gb.dec_points += raw - raw / stride; }
    for (i = 0; i < raw && n < TD5_GEOB_RING_MAX; i += stride) {
        const cJSON *p = cJSON_GetArrayItem(pts, i);
        const cJSON *vx = p ? cJSON_GetObjectItem(p, "x") : NULL;
        const cJSON *vz = p ? cJSON_GetObjectItem(p, "z") : NULL;
        if (!vx || !vz || !cJSON_IsNumber(vx) || !cJSON_IsNumber(vz)) return 0;
        rx[n] = vx->valuedouble;
        rz[n] = vz->valuedouble;
        n++;
    }
    if (n < 3) return 0;
    if (s_gb.npt + n > GEOB_MAX_POINTS) return 0;

    /* One winding for every ring, so the triangulator and the wall loop can
     * both assume it instead of each testing. */
    if (td5_geob_ring_area(rx, rz, n) < 0.0) {
        for (i = 0; i < n / 2; i++) {
            double t = rx[i]; rx[i] = rx[n - 1 - i]; rx[n - 1 - i] = t;
            t = rz[i]; rz[i] = rz[n - 1 - i]; rz[n - 1 - i] = t;
        }
    }
    for (i = 0; i < n; i++) { cx += rx[i]; cz += rz[i]; }
    cx /= (double)n; cz /= (double)n;
    for (i = 0; i < n; i++) {
        const double d = hypot(rx[i] - cx, rz[i] - cz);
        if (d > r) r = d;
    }

    *out_first = s_gb.npt;
    for (i = 0; i < n; i++) {
        s_gb.px[s_gb.npt] = rx[i];
        s_gb.pz[s_gb.npt] = rz[i];
        s_gb.npt++;
    }
    *out_cx = cx; *out_cz = cz; *out_r = r;
    return n;
}

static int geob_load_place_scale(const char *slug)
{
    char path[512];
    char *json;
    cJSON *root;

    s_gb.units_per_m = GEOB_UNITS_PER_M_DEFAULT;
    snprintf(path, sizeof(path), "re/assets/geo/%s/PLACE.JSON", slug);
    json = geob_slurp(path);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    {
        const cJSON *pr = cJSON_GetObjectItem(root, "projection");
        const double u = geob_num(pr, "units_per_metre", 0.0);
        if (u > 1.0) s_gb.units_per_m = u;
    }
    cJSON_Delete(root);
    return 1;
}

static int geob_load_buildings(const char *slug)
{
    char path[512];
    char *json;
    cJSON *root, *arr;
    int n, i;

    snprintf(path, sizeof(path), "re/assets/geo/%s/BUILDINGS.JSON", slug);
    json = geob_slurp(path);
    if (!json) {
        TD5_LOG_W(LOG_TAG, "geob: %s has no readable BUILDINGS.JSON; the "
                  "procedural facades stand everywhere", slug);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_E(LOG_TAG, "geob: BUILDINGS.JSON is not valid JSON");
        return 0;
    }
    s_gb.storey_m = geob_num(root, "storey_height_m", 3.0);
    arr = cJSON_GetObjectItem(root, "buildings");
    if (!arr || !cJSON_IsArray(arr)) {
        TD5_LOG_E(LOG_TAG, "geob: BUILDINGS.JSON has no buildings[]");
        cJSON_Delete(root);
        return 0;
    }
    n = cJSON_GetArraySize(arr);
    if (n > GEOB_MAX_BUILDINGS) {
        TD5_LOG_W(LOG_TAG, "geob: %d buildings, capped at %d (the rest are "
                  "dropped; the procedural filler covers them)",
                  n, GEOB_MAX_BUILDINGS);
        n = GEOB_MAX_BUILDINGS;
    }
    s_gb.b = (TD5_GeoBuilding *)calloc((size_t)(n > 0 ? n : 1),
                                       sizeof(TD5_GeoBuilding));
    if (!s_gb.b) { cJSON_Delete(root); return 0; }

    for (i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(arr, i);
        TD5_GeoBuilding *b = &s_gb.b[s_gb.nb];
        const char *hs;
        double h_m, mh_m;
        int first = 0, rn;

        rn = geob_push_ring(e ? cJSON_GetObjectItem(e, "points") : NULL,
                            &first, &b->cx, &b->cz, &b->radius);
        if (rn == 0) continue;
        b->first = first;
        b->n     = rn;

        h_m  = geob_num(e, "height_m", 0.0);
        mh_m = geob_num(e, "min_height_m", 0.0);
        /* A footprint with no usable height is not a building we can extrude;
         * one storey is the floor rather than dropping it, since the footprint
         * itself is the fact worth keeping. */
        if (!(h_m > 0.0)) h_m = s_gb.storey_m;
        b->height     = h_m  * s_gb.units_per_m;
        b->min_height = (mh_m > 0.0) ? mh_m * s_gb.units_per_m : 0.0;
        b->area_m2    = geob_num(e, "area_m2", 0.0);
        b->id_hash    = geob_id_hash(geob_num(e, "id", (double)i));
        b->roof       = (unsigned char)geob_roof_of(geob_str(e, "roof_shape"));
        b->landmark   = (unsigned char)(geob_true(e, "landmark") ? 1 : 0);
        b->part       = (unsigned char)(geob_true(e, "part") ? 1 : 0);
        b->host_span  = -1;
        b->host_side  = 0;

        hs = geob_str(e, "height_src");
        if (hs && !strcmp(hs, "osm_height"))      b->hsrc = TD5_GEOB_HSRC_OSM_HEIGHT;
        else if (hs && !strcmp(hs, "osm_levels")) b->hsrc = TD5_GEOB_HSRC_OSM_LEVELS;
        else                                      b->hsrc = TD5_GEOB_HSRC_ESTIMATED;

        s_gb.nb++;
    }
    cJSON_Delete(root);
    return s_gb.nb > 0;
}

static int geob_load_areas(const char *slug)
{
    char path[512];
    char *json;
    cJSON *root, *arr;
    int n, i;

    snprintf(path, sizeof(path), "re/assets/geo/%s/AREAS.JSON", slug);
    json = geob_slurp(path);
    if (!json) {
        TD5_LOG_W(LOG_TAG, "geob: %s has no readable AREAS.JSON; plazas stay "
                  "procedural", slug);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    arr = cJSON_GetObjectItem(root, "areas");
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(root); return 0; }
    n = cJSON_GetArraySize(arr);
    if (n > GEOB_MAX_AREAS) n = GEOB_MAX_AREAS;
    s_gb.a = (TD5_GeoArea *)calloc((size_t)(n > 0 ? n : 1), sizeof(TD5_GeoArea));
    if (!s_gb.a) { cJSON_Delete(root); return 0; }

    for (i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(arr, i);
        TD5_GeoArea *a = &s_gb.a[s_gb.na];
        int first = 0, rn;
        const char *nm;

        /* An unclosed way is a fence or a hedge line, not an area. */
        if (!geob_true(e, "closed")) continue;
        rn = geob_push_ring(e ? cJSON_GetObjectItem(e, "points") : NULL,
                            &first, &a->cx, &a->cz, &a->radius);
        if (rn == 0) continue;
        a->first = first;
        a->n     = rn;
        a->kind  = (unsigned char)geob_area_kind_of(geob_str(e, "leisure"),
                                                    geob_str(e, "landuse"));
        a->id_hash = geob_id_hash(geob_num(e, "id", (double)i));
        nm = geob_str(e, "name");
        a->named = (unsigned char)((nm && nm[0]) ? 1 : 0);
        a->host_span = -1;
        a->host_side = 0;
        s_gb.na++;
    }
    cJSON_Delete(root);
    return s_gb.na > 0;
}

/* ------------------------------------------------------------- lifecycle --- */

/* Defined with the rest of the binding below; the loader is the only caller. */
static void geob_bind_all(void);

void td5_geob_unload(void)
{
    free(s_gb.b);
    free(s_gb.a);
    free(s_gb.px);
    free(s_gb.pz);
    free(s_gb.b_head);
    free(s_gb.a_head);
    free(s_gb.b_next);
    free(s_gb.a_next);
    memset(&s_gb, 0, sizeof(s_gb));
}

int td5_geob_sync(void)
{
    const char *slug = td5_geo_place_slug();

    if (!slug || !slug[0]) {
        if (s_gb.loaded) td5_geob_unload();
        return 0;
    }
    if (s_gb.loaded && !strcmp(slug, s_gb.slug)) return 1;

    td5_geob_unload();
    snprintf(s_gb.slug, sizeof(s_gb.slug), "%s", slug);
    s_gb.storey_m = 3.0;
    s_gb.px = (double *)malloc((size_t)GEOB_MAX_POINTS * sizeof(double));
    s_gb.pz = (double *)malloc((size_t)GEOB_MAX_POINTS * sizeof(double));
    if (!s_gb.px || !s_gb.pz) {
        TD5_LOG_E(LOG_TAG, "geob: out of memory for the ring pool");
        td5_geob_unload();
        return 0;
    }
    geob_load_place_scale(slug);
    geob_load_buildings(slug);
    geob_load_areas(slug);

    if (s_gb.nb <= 0 && s_gb.na <= 0) {
        TD5_LOG_W(LOG_TAG, "geob: %s has no usable polygons; everything stays "
                  "procedural", slug);
        td5_geob_unload();
        return 0;
    }
    s_gb.loaded = 1;
    geob_bind_all();
    {
        int meas = 0, est = 0, lm = 0, roof = 0, plaza = 0, i;
        for (i = 0; i < s_gb.nb; i++) {
            if (s_gb.b[i].hsrc == TD5_GEOB_HSRC_ESTIMATED) est++; else meas++;
            if (s_gb.b[i].landmark) lm++;
            if (s_gb.b[i].roof != TD5_GEOB_ROOF_NONE) roof++;
        }
        for (i = 0; i < s_gb.na; i++)
            if (s_gb.a[i].kind == TD5_GEOA_KIND_PARK
                || s_gb.a[i].kind == TD5_GEOA_KIND_PLAY) plaza++;
        TD5_LOG_I(LOG_TAG, "geob: %s loaded %d building(s) (%d measured, %d "
                  "estimated, %d landmark, %d with roof:shape) and %d area(s) "
                  "(%d plaza/park); %d ring point(s), %.1f units/m, storey "
                  "%.1f m; decimated %d polygon(s) / %d point(s)",
                  slug, s_gb.nb, meas, est, lm, roof, s_gb.na, plaza,
                  s_gb.npt, s_gb.units_per_m, s_gb.storey_m,
                  s_gb.dec_polys, s_gb.dec_points);
        TD5_LOG_I(LOG_TAG, "geob: bound to %d route span(s): %d building(s) "
                  "on the route / %d past %.0f units, %d area(s) on the route "
                  "/ %d past %.0f units",
                  s_gb.bound_spans, s_gb.b_bound, s_gb.b_far,
                  TD5_GEOB_BIND_MAX_B, s_gb.a_bound, s_gb.a_far,
                  TD5_GEOB_BIND_MAX_A);
    }
    return 1;
}

int td5_geob_loaded(void) { return s_gb.loaded; }

/* ---------------------------------------------------------------- access --- */

int td5_geob_building_count(void) { return s_gb.loaded ? s_gb.nb : 0; }
int td5_geob_area_count(void)     { return s_gb.loaded ? s_gb.na : 0; }

const TD5_GeoBuilding *td5_geob_building(int i)
{
    if (!s_gb.loaded || i < 0 || i >= s_gb.nb) return NULL;
    return &s_gb.b[i];
}

const TD5_GeoArea *td5_geob_area(int i)
{
    if (!s_gb.loaded || i < 0 || i >= s_gb.na) return NULL;
    return &s_gb.a[i];
}

void td5_geob_ring(int first, int k, double *x, double *z)
{
    const int i = first + k;
    if (!s_gb.loaded || first < 0 || k < 0 || i >= s_gb.npt) {
        if (x) *x = 0.0;
        if (z) *z = 0.0;
        return;
    }
    if (x) *x = s_gb.px[i];
    if (z) *z = s_gb.pz[i];
}

/* ------------------------------------------------------------- host bind --- */

/* Nearest route node to a ring, plus the side and lateral distance in the
 * generator's own convention: lateral = (dx*tz - dz*tx), POSITIVE = LEFT of
 * travel (see the CARRIAGEWAY QUERY block in td5_trackgen_internal.h), and the
 * outward unit for side sg is (tz, -tx) * sg.
 *
 * Chosen on the nearest RING VERTEX, not the centroid: a 100 m block whose
 * centroid is 60 m off the road still has a near wall on the street, and that
 * wall is the thing the driver sees. The lateral that comes back is the
 * CENTROID's, because that is where the building's base sits. */
static int geob_bind_ring(int first, int n, double cx, double cz,
                          double max_near, int *out_span, int *out_side,
                          double *out_lat)
{
    const int nr = td5_geo_route_count();
    double best_d2 = -1.0;
    int best = -1, k, i;

    if (nr < 2) return 0;
    for (k = 0; k < n; k++) {
        const double vx = s_gb.px[first + k], vz = s_gb.pz[first + k];
        for (i = 0; i < nr; i++) {
            double rx, rz, dx, dz, d2;
            if (!td5_geo_route_node(i, &rx, &rz, NULL)) break;
            dx = vx - rx; dz = vz - rz;
            d2 = dx * dx + dz * dz;
            if (best_d2 < 0.0 || d2 < best_d2) { best_d2 = d2; best = i; }
        }
    }
    if (best < 0 || sqrt(best_d2) > max_near) return 0;

    {   /* Tangent from the forward chord, which is what the engine integrates
         * (x += sin(h)*span; z += cos(h)*span), so it matches the node's own
         * heading to within the resampler's tolerance. */
        double ax, az, bx, bz, tx, tz, len, lat;
        const int j = (best + 1 < nr) ? best + 1 : best;
        const int p = (best > 0) ? best - 1 : best;
        if (!td5_geo_route_node(p, &ax, &az, NULL)
            || !td5_geo_route_node(j, &bx, &bz, NULL)) return 0;
        tx = bx - ax; tz = bz - az;
        len = hypot(tx, tz);
        if (!(len > 0.0)) return 0;
        tx /= len; tz /= len;
        if (!td5_geo_route_node(best, &ax, &az, NULL)) return 0;
        lat = (cx - ax) * tz - (cz - az) * tx;
        *out_span = best;
        *out_side = (lat >= 0.0) ? 1 : -1;
        *out_lat  = fabs(lat);
    }
    return 1;
}

/* Bind every polygon and chain it into its span bucket. Runs once per place
 * load, single-threaded, and allocates nothing afterwards -- see the header on
 * why a lazy bind from inside the scenery loop would be a race. */
static void geob_bind_all(void)
{
    const int nr = td5_geo_route_count();
    int i;

    s_gb.bound_spans = 0;
    s_gb.b_bound = s_gb.b_far = s_gb.a_bound = s_gb.a_far = 0;
    if (nr < 2) {
        TD5_LOG_W(LOG_TAG, "geob: no conditioned route loaded; real buildings "
                  "and plazas have no span to stand beside");
        return;
    }

    free(s_gb.b_head); free(s_gb.a_head);
    free(s_gb.b_next); free(s_gb.a_next);
    s_gb.b_head = (int *)malloc((size_t)nr * sizeof(int));
    s_gb.a_head = (int *)malloc((size_t)nr * sizeof(int));
    s_gb.b_next = (int *)malloc((size_t)(s_gb.nb > 0 ? s_gb.nb : 1) * sizeof(int));
    s_gb.a_next = (int *)malloc((size_t)(s_gb.na > 0 ? s_gb.na : 1) * sizeof(int));
    if (!s_gb.b_head || !s_gb.a_head || !s_gb.b_next || !s_gb.a_next) {
        free(s_gb.b_head); free(s_gb.a_head);
        free(s_gb.b_next); free(s_gb.a_next);
        s_gb.b_head = s_gb.a_head = s_gb.b_next = s_gb.a_next = NULL;
        TD5_LOG_E(LOG_TAG, "geob: out of memory for the per-span chains");
        return;
    }
    for (i = 0; i < nr; i++) { s_gb.b_head[i] = -1; s_gb.a_head[i] = -1; }

    for (i = 0; i < s_gb.nb; i++) {
        TD5_GeoBuilding *b = &s_gb.b[i];
        int sp = -1, sd = 0;
        double lat = 0.0;
        b->host_span = -1; b->host_side = 0; b->host_lat = 0.0;
        if (geob_bind_ring(b->first, b->n, b->cx, b->cz,
                           TD5_GEOB_BIND_MAX_B, &sp, &sd, &lat)) {
            b->host_span = sp; b->host_side = sd; b->host_lat = lat;
            s_gb.b_bound++;
        } else {
            s_gb.b_far++;
        }
    }
    for (i = 0; i < s_gb.na; i++) {
        TD5_GeoArea *a = &s_gb.a[i];
        int sp = -1, sd = 0;
        double lat = 0.0;
        a->host_span = -1; a->host_side = 0; a->host_lat = 0.0;
        if (geob_bind_ring(a->first, a->n, a->cx, a->cz,
                           TD5_GEOB_BIND_MAX_A, &sp, &sd, &lat)) {
            a->host_span = sp; a->host_side = sd; a->host_lat = lat;
            s_gb.a_bound++;
        } else {
            s_gb.a_far++;
        }
    }

    /* Walk backwards so each chain ends up in ASCENDING index order, which
     * makes the emitted mesh order a pure function of the cache file. */
    for (i = s_gb.nb - 1; i >= 0; i--) {
        const int sp = s_gb.b[i].host_span;
        if (sp < 0 || sp >= nr) { s_gb.b_next[i] = -1; continue; }
        s_gb.b_next[i] = s_gb.b_head[sp];
        s_gb.b_head[sp] = i;
    }
    for (i = s_gb.na - 1; i >= 0; i--) {
        const int sp = s_gb.a[i].host_span;
        if (sp < 0 || sp >= nr) { s_gb.a_next[i] = -1; continue; }
        s_gb.a_next[i] = s_gb.a_head[sp];
        s_gb.a_head[sp] = i;
    }
    s_gb.bound_spans = nr;
}

int td5_geob_bound_spans(void) { return s_gb.loaded ? s_gb.bound_spans : 0; }

void td5_geob_bind_stats(int *buildings_bound, int *buildings_far,
                         int *areas_bound, int *areas_far)
{
    if (buildings_bound) *buildings_bound = s_gb.b_bound;
    if (buildings_far)   *buildings_far   = s_gb.b_far;
    if (areas_bound)     *areas_bound     = s_gb.a_bound;
    if (areas_far)       *areas_far       = s_gb.a_far;
}

int td5_geob_span_building(int span)
{
    if (!s_gb.loaded || !s_gb.b_head || span < 0 || span >= s_gb.bound_spans)
        return -1;
    return s_gb.b_head[span];
}

int td5_geob_next_building(int i)
{
    if (!s_gb.loaded || !s_gb.b_next || i < 0 || i >= s_gb.nb) return -1;
    return s_gb.b_next[i];
}

int td5_geob_span_area(int span)
{
    if (!s_gb.loaded || !s_gb.a_head || span < 0 || span >= s_gb.bound_spans)
        return -1;
    return s_gb.a_head[span];
}

int td5_geob_next_area(int i)
{
    if (!s_gb.loaded || !s_gb.a_next || i < 0 || i >= s_gb.na) return -1;
    return s_gb.a_next[i];
}

int td5_geob_area_is_plaza(const TD5_GeoArea *a)
{
    if (!a) return 0;
    return a->kind == TD5_GEOA_KIND_PARK || a->kind == TD5_GEOA_KIND_GRASS
        || a->kind == TD5_GEOA_KIND_PITCH || a->kind == TD5_GEOA_KIND_PLAY;
}

/* Shared body of the two point-set tests. The polygon's bounding circle rejects
 * first, so the crossing-number walk only runs where a point could actually be
 * inside; and the whole point set is tested per polygon, so one walk over the
 * span window answers for the caller's entire depth band. */
static int geob_points_hit(int span, const double *px, const double *pz, int np,
                           int win, int areas)
{
    const int lo = (span - win > 0) ? span - win : 0;
    const int hi = (span + win < s_gb.bound_spans - 1)
                 ? span + win : s_gb.bound_spans - 1;
    int s, k;

    if (!s_gb.loaded || s_gb.bound_spans <= 0 || np <= 0) return 0;
    for (s = lo; s <= hi; s++) {
        int i = areas ? td5_geob_span_area(s) : td5_geob_span_building(s);
        for (; i >= 0; i = areas ? td5_geob_next_area(i)
                                 : td5_geob_next_building(i)) {
            double cx, cz, r;
            int first, n;
            if (areas) {
                const TD5_GeoArea *a = &s_gb.a[i];
                if (!td5_geob_area_is_plaza(a)) continue;
                cx = a->cx; cz = a->cz; r = a->radius;
                first = a->first; n = a->n;
            } else {
                const TD5_GeoBuilding *b = &s_gb.b[i];
                cx = b->cx; cz = b->cz; r = b->radius;
                first = b->first; n = b->n;
            }
            for (k = 0; k < np; k++) {
                if (hypot(px[k] - cx, pz[k] - cz) > r) continue;
                if (td5_geob_point_in_ring(&s_gb.px[first], &s_gb.pz[first],
                                           n, px[k], pz[k]))
                    return 1;
            }
        }
    }
    return 0;
}

int td5_geob_points_in_building(int span, const double *px, const double *pz,
                                int np, int win)
{
    return geob_points_hit(span, px, pz, np, win, 0);
}

int td5_geob_points_in_plaza(int span, const double *px, const double *pz,
                             int np, int win)
{
    return geob_points_hit(span, px, pz, np, win, 1);
}

/* ---------------------------------------------------------------- census --- */

void td5_geob_census(int *buildings, int *measured, int *estimated,
                     int *landmarks, int *roofs, int *areas, int *plazas)
{
    int meas = 0, est = 0, lm = 0, roof = 0, plaza = 0, i;
    for (i = 0; i < s_gb.nb; i++) {
        if (s_gb.b[i].hsrc == TD5_GEOB_HSRC_ESTIMATED) est++; else meas++;
        if (s_gb.b[i].landmark) lm++;
        if (s_gb.b[i].roof != TD5_GEOB_ROOF_NONE) roof++;
    }
    for (i = 0; i < s_gb.na; i++)
        if (s_gb.a[i].kind == TD5_GEOA_KIND_PARK
            || s_gb.a[i].kind == TD5_GEOA_KIND_PLAY) plaza++;
    if (buildings) *buildings = s_gb.nb;
    if (measured)  *measured  = meas;
    if (estimated) *estimated = est;
    if (landmarks) *landmarks = lm;
    if (roofs)     *roofs     = roof;
    if (areas)     *areas     = s_gb.na;
    if (plazas)    *plazas    = plaza;
}

void td5_geob_decimation(int *polys, int *points_dropped)
{
    if (polys)          *polys          = s_gb.dec_polys;
    if (points_dropped) *points_dropped = s_gb.dec_points;
}
