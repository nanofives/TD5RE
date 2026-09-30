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
              "cover=%s water=%s",
              s_geo.name, s_geo.slug, s_geo.height.w, s_geo.height.h,
              s_geo.height.cell, s_geo.height.origin_x, s_geo.height.origin_z,
              s_geo.height.rotation, s_geo.exaggeration,
              s_geo.cover.data ? "yes" : "no", s_geo.water.data ? "yes" : "no");
    return 1;
}

void td5_geo_unload(void)
{
    geo_raster_free(&s_geo.height);
    geo_raster_free(&s_geo.cover);
    geo_raster_free(&s_geo.water);
    memset(&s_geo, 0, sizeof(s_geo));
}

int td5_geo_init(void)
{
    /* String knob, so getenv rather than td5_env_int -- the same pattern main.c
     * uses for its own string env vars. Unset means the synthetic world and
     * costs exactly this one lookup. */
    const char *slug = getenv("TD5RE_GEO_PLACE");
    if (!slug || !slug[0]) return 1;
    if (!td5_geo_load(slug)) {
        /* Not fatal: fall through to the synthetic world rather than refusing to
         * boot. A missing or malformed cache is a content problem, and the
         * generator has a perfectly good world of its own. */
        TD5_LOG_E(LOG_TAG, "geo: TD5RE_GEO_PLACE=\"%s\" could not be loaded; "
                  "falling back to the synthetic world", slug);
    }
    return 1;
}

void td5_geo_shutdown(void) { td5_geo_unload(); }

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
