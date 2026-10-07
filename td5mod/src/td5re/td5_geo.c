/**
 * td5_geo.c -- GEO TRACK: real-world terrain source for the auto-track
 *              (PORT-ONLY). Contract and rationale in td5_geo.h.
 *
 * Reads the place cache re/tools/geo_fetch.py writes:
 *
 *   re/assets/geo/<slug>/PLACE.JSON    name, frame, per-layer provenance
 *                        HEIGHT.R16    DEM on the world cell grid, int16
 *                        COVER.R8      land-cover class per cell, uint8
 *                        WATER.R8      water mask per cell, uint8
 *
 * The three rasters share one self-describing container (re/tools/geo_raster.py
 * documents the layout). They are read here rather than in td5_tg_world.c so
 * that module keeps knowing nothing about file formats -- see td5_geo.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "geo"

/* Bed depth below a water surface, world units. Matches the synthetic path's
 * TG_W_RIVER_DEPTH (td5_tg_world.c) so a geo river reads like a synthetic one to
 * every downstream emitter. A DEM sees the water SURFACE, not the bed, so
 * without this a water cell's ground would sit exactly at its own surface and
 * `h < water_y` would be false everywhere. */
#define TD5_GEO_WATER_DEPTH 1400.0

/* Raster container, mirroring re/tools/geo_raster.py. Fields are pulled from the
 * file at fixed byte offsets rather than by reading a struct, so C padding and
 * member order can never silently disagree with the writer. */
#define GEO_R_MAGIC   "TD5GEOR1"
#define GEO_R_HEADER  72
#define GEO_R_KIND_I16 1
#define GEO_R_KIND_U8  2

typedef struct {
    int    kind;
    int    w, h;
    double origin_x, origin_z, cell;
    double scale, bias;
    int    nodata;
    double rotation;
    void  *data;            /* int16_t[] or uint8_t[], w*h, row-major */
} GeoRaster;

static struct {
    int       loaded;
    char      slug[64];
    char      name[96];
    double    exaggeration;
    GeoRaster height, cover, water;
    GeoRaster canopy;       /* [GEO item 6] CANOPY.R8, metres; optional */
} s_geo;

/* ----------------------------------------------------------------- io ------ */

static double geo_rd_f64(const unsigned char *p)
{
    double v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int geo_rd_i32(const unsigned char *p)
{
    int v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void geo_raster_free(GeoRaster *r)
{
    if (r->data) free(r->data);
    memset(r, 0, sizeof(*r));
}

static int geo_raster_load(const char *path, int want_kind, GeoRaster *out)
{
    unsigned char head[GEO_R_HEADER];
    TD5_File *f;
    size_t px, esz, bytes;

    memset(out, 0, sizeof(*out));
    f = td5_plat_file_open(path, "rb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "geo: cannot open %s", path);
        return 0;
    }
    if (td5_plat_file_read(f, head, GEO_R_HEADER) != GEO_R_HEADER
        || memcmp(head, GEO_R_MAGIC, 8) != 0) {
        TD5_LOG_E(LOG_TAG, "geo: %s is not a TD5GEOR1 raster", path);
        td5_plat_file_close(f);
        return 0;
    }
    out->kind     = geo_rd_i32(head + 8);
    out->w        = geo_rd_i32(head + 12);
    out->h        = geo_rd_i32(head + 16);
    out->origin_x = geo_rd_f64(head + 20);
    out->origin_z = geo_rd_f64(head + 28);
    out->cell     = geo_rd_f64(head + 36);
    out->scale    = geo_rd_f64(head + 44);
    out->bias     = geo_rd_f64(head + 52);
    out->nodata   = geo_rd_i32(head + 60);
    out->rotation = geo_rd_f64(head + 64);

    if (out->kind != want_kind || out->w <= 1 || out->h <= 1
        || out->cell <= 0.0) {
        TD5_LOG_E(LOG_TAG, "geo: %s has kind=%d %dx%d cell=%.1f, expected "
                  "kind=%d and a positive grid", path, out->kind, out->w,
                  out->h, out->cell, want_kind);
        td5_plat_file_close(f);
        return 0;
    }

    esz   = (out->kind == GEO_R_KIND_I16) ? 2u : 1u;
    px    = (size_t)out->w * (size_t)out->h;
    bytes = px * esz;
    out->data = malloc(bytes);
    if (!out->data) {
        TD5_LOG_E(LOG_TAG, "geo: out of memory for %s (%u bytes)", path,
                  (unsigned)bytes);
        td5_plat_file_close(f);
        return 0;
    }
    if (td5_plat_file_read(f, out->data, bytes) != bytes) {
        TD5_LOG_E(LOG_TAG, "geo: %s is short (wanted %u sample bytes)", path,
                  (unsigned)bytes);
        geo_raster_free(out);
        td5_plat_file_close(f);
        return 0;
    }
    td5_plat_file_close(f);
    return 1;
}

/* Read a whole file into a malloc'd NUL-terminated buffer, for cJSON. */
static char *geo_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)8 * 1024 * 1024) {
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

/* --------------------------------------------------------------- sample ---- */

/* Cell index for a world coordinate, clamped to the grid. Clamping rather than
 * failing keeps the terrain continuous past the cache edge: the generator's
 * scenery passes sample well outside the road box, and a hard zero there would
 * put a cliff around the whole place. */
static void geo_cell(const GeoRaster *r, double x, double z,
                     double *fx, double *fz)
{
    double cx = (x - r->origin_x) / r->cell;
    double cz = (z - r->origin_z) / r->cell;
    if (cx < 0.0) cx = 0.0;
    if (cz < 0.0) cz = 0.0;
    if (cx > (double)(r->w - 1)) cx = (double)(r->w - 1);
    if (cz > (double)(r->h - 1)) cz = (double)(r->h - 1);
    *fx = cx;
    *fz = cz;
}

static double geo_i16_at(const GeoRaster *r, int ix, int iz)
{
    const int16_t *d = (const int16_t *)r->data;
    const int16_t v = d[(size_t)iz * (size_t)r->w + (size_t)ix];
    if ((int)v == r->nodata) return r->bias;    /* treat a hole as the floor */
    return (double)v * r->scale + r->bias;
}

static double geo_sample_bilinear(const GeoRaster *r, double x, double z)
{
    double fx, fz, tx, tz, a, b, c, d;
    int ix, iz, jx, jz;
    geo_cell(r, x, z, &fx, &fz);
    ix = (int)fx;
    iz = (int)fz;
    jx = (ix + 1 < r->w) ? ix + 1 : ix;
    jz = (iz + 1 < r->h) ? iz + 1 : iz;
    tx = fx - (double)ix;
    tz = fz - (double)iz;
    a = geo_i16_at(r, ix, iz);
    b = geo_i16_at(r, jx, iz);
    c = geo_i16_at(r, ix, jz);
    d = geo_i16_at(r, jx, jz);
    return (a * (1.0 - tx) + b * tx) * (1.0 - tz)
         + (c * (1.0 - tx) + d * tx) * tz;
}

static unsigned geo_sample_u8(const GeoRaster *r, double x, double z)
{
    double fx, fz;
    const unsigned char *d = (const unsigned char *)r->data;
    if (!r->data) return 0u;
    geo_cell(r, x, z, &fx, &fz);
    /* Nearest, not bilinear: these are CLASS ids and a mask, so interpolating
     * them would invent classes that are not in the vocabulary. */
    return d[(size_t)(fz + 0.5) * (size_t)r->w + (size_t)(fx + 0.5)];
}

/* ------------------------------------------------------------- lifecycle --- */

int td5_geo_load(const char *slug)
{
    char path[512];
    char *json;
    int ex;

    td5_geo_unload();
    if (!slug || !slug[0]) return 0;

    snprintf(s_geo.slug, sizeof(s_geo.slug), "%s", slug);

    snprintf(path, sizeof(path), "re/assets/geo/%s/HEIGHT.R16", slug);
    if (!geo_raster_load(path, GEO_R_KIND_I16, &s_geo.height)) {
        td5_geo_unload();
        return 0;
    }
    /* COVER and WATER are optional: a place is usable with terrain alone, and
     * failing the whole load over a missing mask would be worse than running
     * without it. Both sample as 0 when absent. */
    snprintf(path, sizeof(path), "re/assets/geo/%s/COVER.R8", slug);
    if (!geo_raster_load(path, GEO_R_KIND_U8, &s_geo.cover))
        TD5_LOG_W(LOG_TAG, "geo: %s has no COVER.R8; land cover unavailable", slug);
    snprintf(path, sizeof(path), "re/assets/geo/%s/WATER.R8", slug);
    if (!geo_raster_load(path, GEO_R_KIND_U8, &s_geo.water))
        TD5_LOG_W(LOG_TAG, "geo: %s has no WATER.R8; nothing will be water", slug);

    /* [GEO item 6, 2026-09-30] CANOPY.R8: Meta/WRI canopy height in metres on
     * the same grid. Optional and newer than the rest of the contract, so its
     * absence is silent: a cache built before it simply plants plaza trees the
     * old way. */
    snprintf(path, sizeof(path), "re/assets/geo/%s/CANOPY.R8", slug);
    if (td5_plat_file_exists(path)
        && !geo_raster_load(path, GEO_R_KIND_U8, &s_geo.canopy))
        TD5_LOG_W(LOG_TAG, "geo: %s CANOPY.R8 unreadable; canopy unavailable", slug);
    if (s_geo.canopy.data
        && (s_geo.canopy.w != s_geo.height.w || s_geo.canopy.h != s_geo.height.h
            || s_geo.canopy.cell != s_geo.height.cell
            || s_geo.canopy.origin_x != s_geo.height.origin_x
            || s_geo.canopy.origin_z != s_geo.height.origin_z)) {
        TD5_LOG_E(LOG_TAG, "geo: CANOPY.R8 grid differs from HEIGHT.R16; dropped");
        geo_raster_free(&s_geo.canopy);
    }

    /* A mask on a different grid than the terrain it masks is worse than no
     * mask, so drop any that disagrees rather than sampling it anyway. */
    if (s_geo.cover.data
        && (s_geo.cover.w != s_geo.height.w || s_geo.cover.h != s_geo.height.h
            || s_geo.cover.cell != s_geo.height.cell
            || s_geo.cover.origin_x != s_geo.height.origin_x
            || s_geo.cover.origin_z != s_geo.height.origin_z)) {
        TD5_LOG_E(LOG_TAG, "geo: COVER.R8 grid differs from HEIGHT.R16; dropped");
        geo_raster_free(&s_geo.cover);
    }
    if (s_geo.water.data
        && (s_geo.water.w != s_geo.height.w || s_geo.water.h != s_geo.height.h
            || s_geo.water.cell != s_geo.height.cell
            || s_geo.water.origin_x != s_geo.height.origin_x
            || s_geo.water.origin_z != s_geo.height.origin_z)) {
        TD5_LOG_E(LOG_TAG, "geo: WATER.R8 grid differs from HEIGHT.R16; dropped");
        geo_raster_free(&s_geo.water);
    }

    snprintf(s_geo.name, sizeof(s_geo.name), "%s", slug);
    snprintf(path, sizeof(path), "re/assets/geo/%s/PLACE.JSON", slug);
    json = geo_slurp(path);
    if (json) {
        cJSON *root = cJSON_Parse(json);
        if (root) {
            const cJSON *nm = cJSON_GetObjectItem(root, "name");
            if (nm && cJSON_IsString(nm) && nm->valuestring[0])
                snprintf(s_geo.name, sizeof(s_geo.name), "%s", nm->valuestring);
            cJSON_Delete(root);
        }
        free(json);
    } else {
        TD5_LOG_W(LOG_TAG, "geo: %s has no readable PLACE.JSON; using the slug "
                  "as the name", slug);
    }

    ex = td5_env_int("TD5RE_GEO_EXAGGERATION",
                     TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400);
    s_geo.exaggeration = (double)ex / 100.0;
    s_geo.loaded = 1;

    TD5_LOG_I(LOG_TAG, "geo: loaded \"%s\" (%s) grid=%dx%d cell=%.0f "
              "origin=(%.0f,%.0f) rot=%.6frad exaggeration=%.2fx "
              "cover=%s water=%s canopy=%s",
              s_geo.name, s_geo.slug, s_geo.height.w, s_geo.height.h,
              s_geo.height.cell, s_geo.height.origin_x, s_geo.height.origin_z,
              s_geo.height.rotation, s_geo.exaggeration,
              s_geo.cover.data ? "yes" : "no", s_geo.water.data ? "yes" : "no",
              s_geo.canopy.data ? "yes" : "no");
    return 1;
}

void td5_geo_unload(void)
{
    geo_raster_free(&s_geo.height);
    geo_raster_free(&s_geo.cover);
    geo_raster_free(&s_geo.water);
    geo_raster_free(&s_geo.canopy);
    memset(&s_geo, 0, sizeof(s_geo));
}

/* ------------------------------------------------------------------ route --- */

/* [GEO PHASE 3 2026-09-30] See td5_geo.h. Kept apart from s_geo because a route
 * is usable with no place loaded (the offline fixture over the synthetic world),
 * and td5_geo_unload must not throw away a route that came from
 * TD5RE_GEO_ROUTE. */
static struct {
    int     n;
    double *x, *z;
    int    *lanes;
    char    source[512];
    /* [OPTION B 2026-09-30] Grade separations planned by geo_condition.py. A
     * fixed array rather than a malloc: a route is capped at 3000 spans and a
     * crossing site eats ~75 of them once its ramps are counted, so the file
     * cannot legitimately hold many, and a cap that refuses the surplus is
     * better than a growth path nothing exercises. */
    int          xsep_n;
    TD5_GeoXSep  xsep[TD5_GEO_XSEP_MAX];
} s_route;

/* The engine's span length. Duplicated rather than pulling td5_trackgen.h into
 * this reader; the loader asserts the file agrees with it, which is the check
 * that matters. Keep in step with TD5_TG_SPAN_LENGTH. */
#define GEO_ROUTE_SPAN_LENGTH 1500.0
/* Chord tolerance. geo_condition resamples by chord to 1e-6; the file rounds
 * coordinates to 3 decimals, so anything past a unit is a broken file. */
#define GEO_ROUTE_CHORD_TOL   1.0
/* Hard cap: s_struct[]/s_rn[] are indexed by node with no bounds check
 * (TD5_TG_MAX_SPANS = 3000), so an oversized route must be refused here. */
#define GEO_ROUTE_MAX_NODES   3001

static void geo_route_free(void)
{
    free(s_route.x);
    free(s_route.z);
    free(s_route.lanes);
    memset(&s_route, 0, sizeof(s_route));
}

/* [OPTION B 2026-09-30] Read the crossing plan out of a ROUTE.JSON.
 *
 * STRICTLY ADDITIVE, and the contract says so in td5_geo.h: a pre-Option-B file
 * simply has no `grade_separations` key and this leaves the table empty, which
 * every consumer reads as "this route has no grade separation" -- the behaviour
 * before the key existed. So an old ROUTE.JSON keeps building exactly the track
 * it built before, which is what the La Plata byte-identity gate measures.
 *
 * A MALFORMED entry is DROPPED, not fatal. The rest of the route is a valid
 * track (this is the difference from the chord/origin checks above, where a bad
 * value means the geometry itself is wrong and the engine would mis-measure it
 * silently); a crossing plan that cannot be trusted just means the crossing
 * stays level, and the localiser still keeps the cars apart. Every drop is
 * logged with its reason so a stale file is visible rather than mysterious. */
static void geo_xsep_load(const cJSON *root, const char *path, int n_nodes)
{
    const cJSON *arr = cJSON_GetObjectItem(root, "grade_separations");
    int i, count, dropped = 0;

    s_route.xsep_n = 0;
    if (!arr || !cJSON_IsArray(arr)) return;
    count = cJSON_GetArraySize(arr);
    for (i = 0; i < count; i++) {
        const cJSON *e = cJSON_GetArrayItem(arr, i);
        const cJSON *ol, *oh, *ul, *uh, *rs, *cl, *bu;
        TD5_GeoXSep x;
        if (!e || !cJSON_IsObject(e)) { dropped++; continue; }
        ol = cJSON_GetObjectItem(e, "over_lo");
        oh = cJSON_GetObjectItem(e, "over_hi");
        ul = cJSON_GetObjectItem(e, "under_lo");
        uh = cJSON_GetObjectItem(e, "under_hi");
        rs = cJSON_GetObjectItem(e, "ramp_spans");
        cl = cJSON_GetObjectItem(e, "clearance_units");
        bu = cJSON_GetObjectItem(e, "buildable");
        if (!cJSON_IsNumber(ol) || !cJSON_IsNumber(oh) ||
            !cJSON_IsNumber(ul) || !cJSON_IsNumber(uh)) {
            TD5_LOG_W(LOG_TAG, "geo: route %s grade_separations[%d] lacks "
                      "over/under span range; dropped", path, i);
            dropped++;
            continue;
        }
        /* The conditioner marks a site it could not fit ramps into; honour that
         * rather than re-deriving it, so the tool's verdict and the build agree
         * (the tool is what the selector shows the user). */
        if (bu && cJSON_IsBool(bu) && !cJSON_IsTrue(bu)) {
            TD5_LOG_I(LOG_TAG, "geo: route %s grade_separations[%d] is marked "
                      "not buildable (no ramp room); left LEVEL", path, i);
            continue;
        }
        x.over_lo  = ol->valueint;  x.over_hi  = oh->valueint;
        x.under_lo = ul->valueint;  x.under_hi = uh->valueint;
        x.ramp_spans = cJSON_IsNumber(rs) ? rs->valueint : 0;
        x.clearance_units = cJSON_IsNumber(cl) ? cl->valuedouble
                                               : TD5_GEO_XSEP_LIFT_DEFAULT;
        if (x.over_lo < 0 || x.over_hi < x.over_lo || x.over_hi >= n_nodes ||
            x.under_lo < 0 || x.under_hi < x.under_lo || x.under_hi >= n_nodes) {
            TD5_LOG_W(LOG_TAG, "geo: route %s grade_separations[%d] spans "
                      "%d..%d over %d..%d are outside 0..%d; dropped", path, i,
                      x.over_lo, x.over_hi, x.under_lo, x.under_hi, n_nodes - 1);
            dropped++;
            continue;
        }
        /* The two legs must not be the same stretch of road: lifting a leg over
         * ITSELF is not a grade separation, it is a broken plan, and it would
         * put the ramp target and the ground target on the same nodes. */
        if (x.over_lo <= x.under_hi && x.under_lo <= x.over_hi) {
            TD5_LOG_W(LOG_TAG, "geo: route %s grade_separations[%d] legs "
                      "%d..%d and %d..%d OVERLAP in span index; dropped", path, i,
                      x.over_lo, x.over_hi, x.under_lo, x.under_hi);
            dropped++;
            continue;
        }
        if (x.clearance_units < 1.0) x.clearance_units = TD5_GEO_XSEP_LIFT_DEFAULT;
        if (x.ramp_spans < 0) x.ramp_spans = 0;
        if (s_route.xsep_n >= TD5_GEO_XSEP_MAX) {
            TD5_LOG_W(LOG_TAG, "geo: route %s has more than %d grade "
                      "separation(s); the rest are ignored", path,
                      TD5_GEO_XSEP_MAX);
            break;
        }
        s_route.xsep[s_route.xsep_n++] = x;
    }
    if (s_route.xsep_n || dropped)
        TD5_LOG_I(LOG_TAG, "geo: route %s carries %d grade separation(s) "
                  "(%d entry(ies) dropped)", path, s_route.xsep_n, dropped);
    for (i = 0; i < s_route.xsep_n; i++) {
        const TD5_GeoXSep *x = &s_route.xsep[i];
        TD5_LOG_I(LOG_TAG, "geo:   grade sep %d: spans %d..%d OVER %d..%d, "
                  "%.0f units of clearance, ramp hint %d span(s)", i,
                  x->over_lo, x->over_hi, x->under_lo, x->under_hi,
                  x->clearance_units, x->ramp_spans);
    }
}

static int geo_route_load(const char *path)
{
    char *json;
    cJSON *root, *pts, *sl;
    int n, i, ok = 0;

    geo_route_free();
    json = geo_slurp(path);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_E(LOG_TAG, "geo: route %s is not valid JSON", path);
        return 0;
    }

    sl  = cJSON_GetObjectItem(root, "span_length");
    pts = cJSON_GetObjectItem(root, "points");
    if (sl && cJSON_IsNumber(sl) && fabs(sl->valuedouble - GEO_ROUTE_SPAN_LENGTH) > 1e-6) {
        TD5_LOG_E(LOG_TAG, "geo: route %s was conditioned for span_length %.1f, "
                  "the engine uses %.1f; rejected", path, sl->valuedouble,
                  GEO_ROUTE_SPAN_LENGTH);
        goto done;
    }
    if (!pts || !cJSON_IsArray(pts) || (n = cJSON_GetArraySize(pts)) < 2) {
        TD5_LOG_E(LOG_TAG, "geo: route %s has no points[]", path);
        goto done;
    }
    if (n > GEO_ROUTE_MAX_NODES) {
        TD5_LOG_E(LOG_TAG, "geo: route %s has %d nodes, over the %d cap; rejected",
                  path, n, GEO_ROUTE_MAX_NODES);
        goto done;
    }

    s_route.x     = (double *)malloc((size_t)n * sizeof(double));
    s_route.z     = (double *)malloc((size_t)n * sizeof(double));
    s_route.lanes = (int *)malloc((size_t)n * sizeof(int));
    if (!s_route.x || !s_route.z || !s_route.lanes) goto done;

    for (i = 0; i < n; i++) {
        const cJSON *p  = cJSON_GetArrayItem(pts, i);
        const cJSON *px = p ? cJSON_GetObjectItem(p, "x") : NULL;
        const cJSON *pz = p ? cJSON_GetObjectItem(p, "z") : NULL;
        const cJSON *pl = p ? cJSON_GetObjectItem(p, "lanes") : NULL;
        if (!px || !pz || !cJSON_IsNumber(px) || !cJSON_IsNumber(pz)) {
            TD5_LOG_E(LOG_TAG, "geo: route %s point %d lacks x/z", path, i);
            goto done;
        }
        s_route.x[i] = px->valuedouble;
        s_route.z[i] = pz->valuedouble;
        s_route.lanes[i] = (pl && cJSON_IsNumber(pl)) ? pl->valueint : 2;
        if (s_route.lanes[i] < 1)  s_route.lanes[i] = 1;
        if (s_route.lanes[i] > 12) s_route.lanes[i] = 12;
        if (i > 0) {
            const double c = hypot(s_route.x[i] - s_route.x[i - 1],
                                   s_route.z[i] - s_route.z[i - 1]);
            if (fabs(c - GEO_ROUTE_SPAN_LENGTH) > GEO_ROUTE_CHORD_TOL) {
                TD5_LOG_E(LOG_TAG, "geo: route %s chord %d->%d is %.2f, not %.1f "
                          "(re-run geo_condition); rejected", path, i - 1, i, c,
                          GEO_ROUTE_SPAN_LENGTH);
                goto done;
            }
        }
    }
    if (fabs(s_route.x[0]) > GEO_ROUTE_CHORD_TOL || fabs(s_route.z[0]) > GEO_ROUTE_CHORD_TOL) {
        TD5_LOG_E(LOG_TAG, "geo: route %s node 0 is (%.1f,%.1f), not the origin; "
                  "rejected", path, s_route.x[0], s_route.z[0]);
        goto done;
    }
    s_route.n = n;
    snprintf(s_route.source, sizeof(s_route.source), "%s", path);
    ok = 1;
    TD5_LOG_I(LOG_TAG, "geo: route %s loaded: %d nodes (%d spans, %.2f km at "
              "the conditioner's scale)", path, n, n - 1,
              (double)(n - 1) * GEO_ROUTE_SPAN_LENGTH / 430.0 / 1000.0);
    geo_xsep_load(root, path, n);

done:
    cJSON_Delete(root);
    if (!ok) geo_route_free();
    return ok;
}

int td5_geo_route_count(void) { return s_route.n; }
const char *td5_geo_route_source(void) { return s_route.n ? s_route.source : ""; }

int td5_geo_route_node(int i, double *x, double *z, int *lanes)
{
    if (i < 0 || i >= s_route.n) return 0;
    if (x) *x = s_route.x[i];
    if (z) *z = s_route.z[i];
    if (lanes) *lanes = s_route.lanes[i];
    return 1;
}

int td5_geo_xsep_count(void) { return s_route.n ? s_route.xsep_n : 0; }

const TD5_GeoXSep *td5_geo_xsep(int i)
{
    if (!s_route.n || i < 0 || i >= s_route.xsep_n) return NULL;
    return &s_route.xsep[i];
}

/* ------------------------------------------------------- place selection --- */

#define GEO_SELECTED_PATH "re/assets/geo/SELECTED.TXT"
#define GEO_PLACES_MAX    64

static struct {
    int  n;
    char slug[GEO_PLACES_MAX][64];
    char name[GEO_PLACES_MAX][96];
} s_places;

/* [J8 GEO-PICK] Place dirs that exist but are not raceable, with the reason, so
 * the selector can explain the gap instead of dropping them silently. */
static struct {
    int  n;
    char slug[GEO_PLACES_MAX][64];
    char reason[GEO_PLACES_MAX][48];
} s_places_bad;

/* [J8 GEO-PICK] Slot-driven place override. Empty = not active, and then the
 * env knob decides exactly as before. */
static char s_force_slug[64];
static int  s_force_active;

void td5_geo_force_place(const char *slug)
{
    if (!slug || !slug[0]) {
        if (s_force_active)
            TD5_LOG_I(LOG_TAG, "geo: place override cleared");
        s_force_active = 0;
        s_force_slug[0] = '\0';
        return;
    }
    s_force_active = 1;
    snprintf(s_force_slug, sizeof(s_force_slug), "%s", slug);
    TD5_LOG_I(LOG_TAG, "geo: place override -> %s (env knob untouched)", s_force_slug);
}

const char *td5_geo_wanted_slug(void)
{
    const char *env;
    if (s_force_active) return s_force_slug;
    env = getenv("TD5RE_GEO_PLACE");
    return env ? env : "";
}

/* Which route file the loaded route came from, so sync can tell "already
 * loaded" from "needs loading" without re-parsing JSON every build. */
static char s_route_want[512];

static void geo_route_sync(void)
{
    const char *env = getenv("TD5RE_GEO_ROUTE");
    char want[512];

    if (env && env[0])
        snprintf(want, sizeof(want), "%s", env);
    else if (s_geo.loaded)
        snprintf(want, sizeof(want), "re/assets/geo/%s/ROUTE.JSON", s_geo.slug);
    else
        want[0] = '\0';

    if (!strcmp(want, s_route_want)) return;          /* nothing changed */
    snprintf(s_route_want, sizeof(s_route_want), "%s", want);
    geo_route_free();
    if (!want[0]) return;
    if (!geo_route_load(want) && env && env[0])
        TD5_LOG_E(LOG_TAG, "geo: TD5RE_GEO_ROUTE=\"%s\" could not be loaded; "
                  "the generator walks its own road", want);
}

void td5_geo_sync(void)
{
    /* [J8 GEO-PICK] Was getenv("TD5RE_GEO_PLACE"). A SELECT TRACK geo slot sets
     * the override instead of the env, so the studio's LOCATION row keeps its
     * own value; with no override this reads the knob exactly as before. */
    const char *slug = td5_geo_wanted_slug();
    if (!slug) slug = "";

    if (strcmp(slug, s_geo.loaded ? s_geo.slug : "")) {
        if (!slug[0]) {
            td5_geo_unload();
            TD5_LOG_I(LOG_TAG, "geo: synthetic world selected");
        } else if (!td5_geo_load(slug)) {
            /* Not fatal: fall through to the synthetic world rather than
             * refusing to build. A missing or malformed cache is a content
             * problem, and the generator has a perfectly good world of its
             * own. */
            TD5_LOG_E(LOG_TAG, "geo: TD5RE_GEO_PLACE=\"%s\" could not be loaded; "
                      "falling back to the synthetic world", slug);
        }
    }
    geo_route_sync();
}

/* [GEO ROUTE 2026-10-07] Drop every in-memory copy of the place so the NEXT
 * td5_geo_sync() reloads it from disk. td5_geo_route_commit calls this after it
 * rewrites ROUTE.JSON and re-grids the rasters IN PLACE: without it an
 * in-session BUILD -> race would keep racing the pre-commit track, because
 * td5_geo_sync skips a reload when the slug is unchanged and geo_route_sync
 * skips one when the ROUTE.JSON PATH string is unchanged (both are true after a
 * commit of the already-selected place). Clearing s_geo and the route-want
 * string forces both reloads. A plain unload is not enough on its own: the
 * route is cached separately and keys on the path, not the file's contents. */
void td5_geo_invalidate(void)
{
    td5_geo_unload();            /* place + rasters: next sync sees a slug change */
    geo_route_free();            /* parsed route nodes + grade separations       */
    s_route_want[0] = '\0';      /* so geo_route_sync reloads the same path       */
}

void td5_geo_select(const char *slug)
{
    TD5_File *f;
    if (!slug) slug = "";
    _putenv_s("TD5RE_GEO_PLACE", slug);      /* "" removes it */
    /* Persist, so the choice survives a relaunch and the browser selector sees
     * what the game is set to. Best effort: a read-only install still works for
     * this session. */
    f = td5_plat_file_open(GEO_SELECTED_PATH, "wb");
    if (f) {
        td5_plat_file_write(f, slug, strlen(slug));
        td5_plat_file_close(f);
    }
    TD5_LOG_I(LOG_TAG, "geo: LOCATION -> %s", slug[0] ? slug : "SYNTHETIC");
}

/* [J8 GEO-PICK] Record a place dir that exists but cannot be raced, so the
 * selector can name it and say why. */
static void geo_place_note_incomplete(const char *slug, const char *reason)
{
    if (s_places_bad.n >= GEO_PLACES_MAX) return;
    snprintf(s_places_bad.slug[s_places_bad.n], sizeof(s_places_bad.slug[0]), "%s", slug);
    snprintf(s_places_bad.reason[s_places_bad.n], sizeof(s_places_bad.reason[0]), "%s", reason);
    s_places_bad.n++;
    TD5_LOG_W(LOG_TAG, "geo: place '%s' is not raceable: %s", slug, reason);
}

int td5_geo_places_rescan(void)
{
    DIR *d = opendir("re/assets/geo");
    struct dirent *e;
    s_places.n = 0;
    s_places_bad.n = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL && s_places.n < GEO_PLACES_MAX) {
        char path[384];
        char *json;
        TD5_File *f;
        const size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || e->d_name[0] == '_' || len >= 64) continue;
        /* A dir with neither file is not a place at all (stray folder), so look
         * for the terrain first and only report dirs that tried to be one. */
        snprintf(path, sizeof(path), "re/assets/geo/%s/HEIGHT.R16", e->d_name);
        f = td5_plat_file_open(path, "rb");
        if (!f) {
            snprintf(path, sizeof(path), "re/assets/geo/%s/ROUTE.JSON", e->d_name);
            f = td5_plat_file_open(path, "rb");
            if (f) { td5_plat_file_close(f); geo_place_note_incomplete(e->d_name, "NO TERRAIN DATA"); }
            continue;                        /* no DEM: td5_geo_load would fail */
        }
        td5_plat_file_close(f);
        snprintf(path, sizeof(path), "re/assets/geo/%s/ROUTE.JSON", e->d_name);
        f = td5_plat_file_open(path, "rb");
        if (!f) {                            /* no route: not raceable yet */
            geo_place_note_incomplete(e->d_name, "NO ROUTE -- DRAW ONE IN geo_selector.py");
            continue;
        }
        td5_plat_file_close(f);
        memcpy(s_places.slug[s_places.n], e->d_name, len + 1);
        snprintf(s_places.name[s_places.n], sizeof(s_places.name[0]), "%s", e->d_name);
        snprintf(path, sizeof(path), "re/assets/geo/%s/PLACE.JSON", e->d_name);
        json = geo_slurp(path);
        if (json) {
            cJSON *root = cJSON_Parse(json);
            const cJSON *nm = root ? cJSON_GetObjectItem(root, "name") : NULL;
            if (nm && cJSON_IsString(nm) && nm->valuestring[0])
                snprintf(s_places.name[s_places.n], sizeof(s_places.name[0]),
                         "%s", nm->valuestring);
            cJSON_Delete(root);
            free(json);
        }
        s_places.n++;
    }
    closedir(d);
    return s_places.n;
}

int         td5_geo_places_count(void) { return s_places.n; }
const char *td5_geo_places_slug(int i) { return (i >= 0 && i < s_places.n) ? s_places.slug[i] : ""; }
const char *td5_geo_places_name(int i) { return (i >= 0 && i < s_places.n) ? s_places.name[i] : ""; }

int td5_geo_places_incomplete_count(void) { return s_places_bad.n; }
const char *td5_geo_places_incomplete_slug(int i)
{ return (i >= 0 && i < s_places_bad.n) ? s_places_bad.slug[i] : ""; }
const char *td5_geo_places_incomplete_reason(int i)
{ return (i >= 0 && i < s_places_bad.n) ? s_places_bad.reason[i] : ""; }

/* ------------------------------------------- [J8 GEO-PICK] preview route ---
 * The SELECT TRACK panel needs the SHAPE of a place's route before anything is
 * built or loaded. Parsing ROUTE.JSON for x/z alone is cheap (~150 KB) and,
 * unlike td5_geo_load, touches none of the module's live state -- so the panel
 * can show Valparaiso while La Plata is the loaded place, with no interaction
 * between the two.
 *
 * Deliberately NOT validating chord spacing / origin the way geo_route_load
 * does: those checks decide whether the ENGINE may build from a route, and a
 * route that fails them still draws a perfectly truthful map. Rejecting one
 * here would leave the panel blank for a place the user can see on disk. */
static struct {
    char   slug[64];
    int    n;
    double x[TD5_GEO_PREVIEW_MAX];
    double z[TD5_GEO_PREVIEW_MAX];
} s_preview;

int td5_geo_preview_route(const char *slug)
{
    char path[384];
    char *json;
    cJSON *root, *pts;
    int total, i, stride;

    if (!slug || !slug[0]) { s_preview.slug[0] = '\0'; s_preview.n = 0; return 0; }
    if (!strcmp(slug, s_preview.slug)) return s_preview.n;   /* already shown */

    snprintf(s_preview.slug, sizeof(s_preview.slug), "%s", slug);
    s_preview.n = 0;

    snprintf(path, sizeof(path), "re/assets/geo/%s/ROUTE.JSON", slug);
    json = geo_slurp(path);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_W(LOG_TAG, "geo: preview route %s is not valid JSON", path);
        return 0;
    }
    pts = cJSON_GetObjectItem(root, "points");
    if (!pts || !cJSON_IsArray(pts) || (total = cJSON_GetArraySize(pts)) < 2) {
        cJSON_Delete(root);
        return 0;
    }
    /* Keep the LAST node whatever the stride, so the finish marker lands on the
     * real end of the route rather than wherever the decimation happened to stop. */
    stride = total / TD5_GEO_PREVIEW_MAX;
    if (stride < 1) stride = 1;
    for (i = 0; i < total && s_preview.n < TD5_GEO_PREVIEW_MAX; i += stride) {
        const cJSON *p  = cJSON_GetArrayItem(pts, i);
        const cJSON *px = p ? cJSON_GetObjectItem(p, "x") : NULL;
        const cJSON *pz = p ? cJSON_GetObjectItem(p, "z") : NULL;
        if (!px || !pz || !cJSON_IsNumber(px) || !cJSON_IsNumber(pz)) continue;
        s_preview.x[s_preview.n] = px->valuedouble;
        s_preview.z[s_preview.n] = pz->valuedouble;
        s_preview.n++;
    }
    if (s_preview.n > 1 && stride > 1 && s_preview.n < TD5_GEO_PREVIEW_MAX) {
        const cJSON *p  = cJSON_GetArrayItem(pts, total - 1);
        const cJSON *px = p ? cJSON_GetObjectItem(p, "x") : NULL;
        const cJSON *pz = p ? cJSON_GetObjectItem(p, "z") : NULL;
        if (px && pz && cJSON_IsNumber(px) && cJSON_IsNumber(pz)) {
            s_preview.x[s_preview.n] = px->valuedouble;
            s_preview.z[s_preview.n] = pz->valuedouble;
            s_preview.n++;
        }
    }
    cJSON_Delete(root);
    TD5_LOG_I(LOG_TAG, "geo: preview route %s -> %d of %d nodes (stride %d)",
              slug, s_preview.n, total, stride);
    return s_preview.n;
}

int td5_geo_preview_count(void) { return s_preview.n; }

int td5_geo_preview_node(int i, double *x, double *z)
{
    if (i < 0 || i >= s_preview.n) return 0;
    if (x) *x = s_preview.x[i];
    if (z) *z = s_preview.z[i];
    return 1;
}

int td5_geo_init(void)
{
    /* String knobs, so getenv rather than td5_env_int. An unset
     * TD5RE_GEO_PLACE is filled from SELECTED.TXT (what the selector or the
     * LOCATION row last chose); an env value always wins over the file. Unset
     * and no file means the synthetic world, unchanged. */
    const char *slug = getenv("TD5RE_GEO_PLACE");
    if (!slug || !slug[0]) {
        char *sel = geo_slurp(GEO_SELECTED_PATH);
        if (sel) {
            char *q = sel + strlen(sel);
            while (q > sel && (q[-1] == '\n' || q[-1] == '\r' || q[-1] == ' ')) *--q = '\0';
            if (sel[0] && strlen(sel) < 64) {
                _putenv_s("TD5RE_GEO_PLACE", sel);
                TD5_LOG_I(LOG_TAG, "geo: LOCATION %s (from %s)", sel, GEO_SELECTED_PATH);
            }
            free(sel);
        }
    }
    td5_geo_places_rescan();
    td5_geo_sync();
    return 1;
}

void td5_geo_shutdown(void) { td5_geo_unload(); geo_route_free(); s_route_want[0] = '\0'; }

int         td5_geo_loaded(void)     { return s_geo.loaded; }
const char *td5_geo_place_name(void) { return s_geo.loaded ? s_geo.name : ""; }
const char *td5_geo_place_slug(void) { return s_geo.loaded ? s_geo.slug : ""; }

/* ---------------------------------------------------------------- queries -- */

double td5_geo_height_raw_m_units(double x, double z)
{
    if (!s_geo.loaded) return 0.0;
    return geo_sample_bilinear(&s_geo.height, x, z);
}

int td5_geo_is_water(double x, double z)
{
    if (!s_geo.loaded || !s_geo.water.data) return 0;
    return geo_sample_u8(&s_geo.water, x, z) != 0u;
}

double td5_geo_height(double x, double z)
{
    double h;
    if (!s_geo.loaded) return 0.0;
    /* Exaggerate about real sea level, which the cache encodes as 0, so the
     * shoreline stays put while the relief grows. */
    h = geo_sample_bilinear(&s_geo.height, x, z) * s_geo.exaggeration;
    if (td5_geo_is_water(x, z)) h -= TD5_GEO_WATER_DEPTH;
    return h;
}

double td5_geo_water_y(double x, double z)
{
    if (!s_geo.loaded) return -1e9;
    if (!td5_geo_is_water(x, z)) {
        /* Far below any terrain, so `h < water_y` is false without the caller
         * needing a separate mask lookup. */
        return -1e9;
    }
    /* The DEM sees the water SURFACE, and td5_geo_height lowered the bed below
     * it, so the un-lowered value IS the surface. */
    return geo_sample_bilinear(&s_geo.height, x, z) * s_geo.exaggeration;
}

int td5_geo_canopy_m(double x, double z)
{
    if (!s_geo.loaded || !s_geo.canopy.data) return -1;
    return (int)geo_sample_u8(&s_geo.canopy, x, z);
}

int td5_geo_cover(double x, double z)
{
    if (!s_geo.loaded || !s_geo.cover.data) return TD5_GEO_COVER_NONE;
    return (int)geo_sample_u8(&s_geo.cover, x, z);
}

double td5_geo_sea_y_raw(void)
{
    /* The cache stores real elevation in world units with 0 at real sea level,
     * and the exaggeration is about that same zero, so sea level is 0 either
     * way. Spelled out as a function because tg_world_build reads it and the
     * reasoning is not obvious from a literal 0.0 at the call site. */
    return 0.0;
}

int td5_geo_in_bounds(double x, double z)
{
    const GeoRaster *r = &s_geo.height;
    if (!s_geo.loaded) return 0;
    return x >= r->origin_x && z >= r->origin_z
        && x <= r->origin_x + (double)(r->w - 1) * r->cell
        && z <= r->origin_z + (double)(r->h - 1) * r->cell;
}

void td5_geo_grid(int *out_w, int *out_h, double *out_cell,
                  double *out_origin_x, double *out_origin_z,
                  double *out_rotation_rad)
{
    const GeoRaster *r = &s_geo.height;
    if (out_w)        *out_w = s_geo.loaded ? r->w : 0;
    if (out_h)        *out_h = s_geo.loaded ? r->h : 0;
    if (out_cell)     *out_cell = s_geo.loaded ? r->cell : 0.0;
    if (out_origin_x) *out_origin_x = s_geo.loaded ? r->origin_x : 0.0;
    if (out_origin_z) *out_origin_z = s_geo.loaded ? r->origin_z : 0.0;
    if (out_rotation_rad) *out_rotation_rad = s_geo.loaded ? r->rotation : 0.0;
}
