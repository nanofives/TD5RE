/**
 * td5_roam_height.c -- GEO FREE ROAM: height from the SOURCE rasters, by window
 * (PORT-ONLY, F1 task T2). Contract and rationale in td5_roam_height.h.
 *
 * The sampler below is td5_geo.c's, transcribed: geo_cell (clamp to the grid),
 * geo_i16_at (a nodata cell reads as the bias), geo_sample_bilinear (the same
 * four-corner expression in the same order), geo_sample_u8 (nearest, for the
 * water mask) and td5_geo_is_water. Keep it in step with those if they change.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_roam_height.h"
#include "td5_geo_frame.h"       /* [roam M] the geographic -> game reflection */
#include "deps/cjson/cJSON.h"

#define LOG_TAG "roam"

#define ROAM_MAGIC        "TD5GEOR1"
#define ROAM_HEADER_BYTES 72
#define ROAM_KIND_I16     1
#define ROAM_KIND_U8      2

/* ----------------------------------------------------------------- headers -- */

static double roam_rd_f64(const unsigned char *p)
{
    double v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int roam_rd_i32(const unsigned char *p)
{
    int v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* The 72-byte header of a TD5GEOR1 file, with no samples read. Fills the
 * fields of `out` that the sampler needs and leaves data NULL. */
static int roam_peek(const char *path, int want_kind, GrRaster *out)
{
    unsigned char h[ROAM_HEADER_BYTES];
    TD5_File *f;
    memset(out, 0, sizeof *out);
    f = td5_plat_file_open(path, "rb");
    if (!f) return 0;
    if (td5_plat_file_read(f, h, sizeof h) != sizeof h
        || memcmp(h, ROAM_MAGIC, 8) != 0) {
        td5_plat_file_close(f);
        return 0;
    }
    td5_plat_file_close(f);
    out->kind       = roam_rd_i32(h + 8);
    out->w          = roam_rd_i32(h + 12);
    out->h          = roam_rd_i32(h + 16);
    out->origin_x   = roam_rd_f64(h + 20);
    out->origin_z   = roam_rd_f64(h + 28);
    out->cell       = roam_rd_f64(h + 36);
    out->scale      = roam_rd_f64(h + 44);
    out->bias       = roam_rd_f64(h + 52);
    out->nodata_raw = roam_rd_i32(h + 60);
    out->rotation   = roam_rd_f64(h + 64);
    return out->kind == want_kind && out->w >= 1 && out->h >= 1 && out->cell > 0.0;
}

/* ----------------------------------------------------------------- sampling -- */

/* geo_cell: the cell coordinate of a world point, clamped to the grid. */
static void roam_cell(const GrRaster *r, double x, double z, double *fx, double *fz)
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

/* geo_i16_at: a nodata cell reads as the bias ("treat a hole as the floor"). */
static double roam_i16_at(const GrRaster *r, int ix, int iz)
{
    const int16_t *d = (const int16_t *)r->data;
    const int16_t v = d[(size_t)iz * (size_t)r->w + (size_t)ix];
    if ((int)v == r->nodata_raw) return r->bias;
    return (double)v * r->scale + r->bias;
}

/* geo_sample_bilinear, on the window's own cells. */
static double roam_bilinear(const GrRaster *r, double x, double z)
{
    double fx, fz, tx, tz, a, b, c, d;
    int ix, iz, jx, jz;
    roam_cell(r, x, z, &fx, &fz);
    ix = (int)fx;
    iz = (int)fz;
    jx = (ix + 1 < r->w) ? ix + 1 : ix;
    jz = (iz + 1 < r->h) ? iz + 1 : iz;
    tx = fx - (double)ix;
    tz = fz - (double)iz;
    a = roam_i16_at(r, ix, iz);
    b = roam_i16_at(r, jx, iz);
    c = roam_i16_at(r, ix, jz);
    d = roam_i16_at(r, jx, jz);
    return (a * (1.0 - tx) + b * tx) * (1.0 - tz)
         + (c * (1.0 - tx) + d * tx) * tz;
}

/* geo_sample_u8 + td5_geo_is_water: nearest cell, and a non-zero non-nodata
 * byte is wet. */
static int roam_is_wet(const GrRaster *r, double x, double z)
{
    double fx, fz;
    const unsigned char *d = (const unsigned char *)r->data;
    unsigned v;
    int nodata;
    roam_cell(r, x, z, &fx, &fz);
    v = d[(size_t)(fz + 0.5) * (size_t)r->w + (size_t)(fx + 0.5)];
    nodata = r->nodata_raw >= 0 && r->nodata_raw <= 255 && (int)v == r->nodata_raw;
    return v != 0u && !nodata;
}

/* ------------------------------------------------------------- LRU windows -- */

/* A resident window that was read for this layer and covers (x, z). Touches
 * its LRU stamp. Only a window with cells is returned: a rectangle that missed
 * the raster is a 0x0 grid and covers nothing. */
static TD5_RoamHeightWin *roam_find(TD5_RoamHeight *c, int layer, double x, double z)
{
    int i;
    for (i = 0; i < TD5_ROAM_HEIGHT_WINDOWS; i++) {
        TD5_RoamHeightWin *w = &c->win[i];
        if (!w->used || w->layer != layer) continue;
        if (w->r.w < 1 || w->r.h < 1) continue;
        if (x < w->x0 || x > w->x1 || z < w->z0 || z > w->z1) continue;
        w->last_use = ++c->clock;
        return w;
    }
    return NULL;
}

/* Make one layer's window for the rectangle resident. A rectangle already
 * resident for this layer is a hit. Otherwise the window goes into an empty
 * slot, or else replaces the least recently used one (the first such slot on a
 * tie, so the choice is deterministic). */
static int roam_load(TD5_RoamHeight *c, int layer,
                     double x0, double z0, double x1, double z1)
{
    TD5_RoamHeightWin *s = NULL;
    const char *path = (layer == TD5_ROAM_HEIGHT_LAYER_HEIGHT) ? c->height_path : c->water_path;
    uint64_t t0;
    int i, ok;

    for (i = 0; i < TD5_ROAM_HEIGHT_WINDOWS; i++) {
        TD5_RoamHeightWin *w = &c->win[i];
        if (w->used && w->layer == layer
            && w->x0 == x0 && w->z0 == z0 && w->x1 == x1 && w->z1 == z1) {
            w->last_use = ++c->clock;
            return 1;
        }
    }
    for (i = 0; i < TD5_ROAM_HEIGHT_WINDOWS; i++) {
        TD5_RoamHeightWin *w = &c->win[i];
        if (!w->used) { s = w; break; }
        if (!s || w->last_use < s->last_use) s = w;
    }

    if (s->r.data) free(s->r.data);
    memset(s, 0, sizeof *s);

    t0 = td5_plat_time_us();
    ok = gr_raster_read_window_game(path, x0, z0, x1, z1, TD5_ROAM_HEIGHT_PAD, &s->r);   /* [roam M] game frame */
    c->read_us[layer] += td5_plat_time_us() - t0;
    c->reads++;
    if (!ok) {
        if (s->r.data) free(s->r.data);
        memset(s, 0, sizeof *s);
        return 0;
    }
    s->used = 1;
    s->layer = layer;
    s->x0 = x0; s->z0 = z0; s->x1 = x1; s->z1 = z1;
    s->last_use = ++c->clock;
    return 1;
}

/* ---------------------------------------------------------------- public -- */

int td5_roam_height_open(TD5_RoamHeight *c, const char *height_path,
                         const char *water_path, double exaggeration)
{
    GrRaster hdr;
    memset(c, 0, sizeof *c);
    snprintf(c->height_path, sizeof c->height_path, "%s", height_path);
    c->exaggeration = exaggeration;
    if (!roam_peek(height_path, ROAM_KIND_I16, &hdr)) {
        TD5_LOG_W(LOG_TAG, "roam: %s is not a usable int16 TD5GEOR1 raster", height_path);
        return 0;
    }
    if (water_path && roam_peek(water_path, ROAM_KIND_U8, &hdr)) {
        snprintf(c->water_path, sizeof c->water_path, "%s", water_path);
        c->has_water = 1;
    }
    return 1;
}

void td5_roam_height_close(TD5_RoamHeight *c)
{
    int i;
    for (i = 0; i < TD5_ROAM_HEIGHT_WINDOWS; i++) {
        if (c->win[i].r.data) free(c->win[i].r.data);
    }
    memset(c, 0, sizeof *c);
}

int td5_roam_height_prefetch(TD5_RoamHeight *c, double x0, double z0,
                             double x1, double z1)
{
    if (!roam_load(c, TD5_ROAM_HEIGHT_LAYER_HEIGHT, x0, z0, x1, z1)) return 0;
    if (c->has_water && !roam_load(c, TD5_ROAM_HEIGHT_LAYER_WATER, x0, z0, x1, z1)) return 0;
    return 1;
}

int td5_roam_height_sample(TD5_RoamHeight *c, double x, double z,
                           double *out_h, int *out_water)
{
    TD5_RoamHeightWin *hw, *ww = NULL;
    double h;
    int wet = 0;

    hw = roam_find(c, TD5_ROAM_HEIGHT_LAYER_HEIGHT, x, z);
    if (!hw) return 0;
    if (c->has_water) {
        ww = roam_find(c, TD5_ROAM_HEIGHT_LAYER_WATER, x, z);
        if (!ww) return 0;
    }
    /* td5_geo_height: exaggerate about sea level, then lower a wet bed. */
    h = roam_bilinear(&hw->r, x, z) * c->exaggeration;
    if (ww) wet = roam_is_wet(&ww->r, x, z);
    if (wet) h -= TD5_ROAM_WATER_DEPTH;
    *out_h = h;
    if (out_water) *out_water = wet;
    return 1;
}

/* [roam PC] the WATER raster's wetness at a place-frame point, bilinear over the 4 cells round it (0 dry .. 1 wet):
 * its 0.5 contour is the shoreline roam_is_wet's nearest-cell rule draws at the raster's own resolution. */
int td5_roam_height_wetfrac(TD5_RoamHeight *c, double x, double z, double *out_frac)
{
    TD5_RoamHeightWin *ww;
    const GrRaster *r;
    const unsigned char *d;
    double fx, fz, tx, tz, w[4];
    int ix, iz, jx, jz, k;
    if (!c->has_water) { *out_frac = 0.0; return 1; }
    ww = roam_find(c, TD5_ROAM_HEIGHT_LAYER_WATER, x, z);
    if (!ww) return 0;
    r = &ww->r;
    d = (const unsigned char *)r->data;
    roam_cell(r, x, z, &fx, &fz);
    ix = (int)fx; iz = (int)fz;
    jx = (ix + 1 < r->w) ? ix + 1 : ix;
    jz = (iz + 1 < r->h) ? iz + 1 : iz;
    tx = fx - (double)ix; tz = fz - (double)iz;
    for (k = 0; k < 4; k++) {
        const unsigned v = d[(size_t)(k < 2 ? iz : jz) * (size_t)r->w + (size_t)((k & 1) ? jx : ix)];
        const int nodata = r->nodata_raw >= 0 && r->nodata_raw <= 255 && (int)v == r->nodata_raw;
        w[k] = (v != 0u && !nodata) ? 1.0 : 0.0;
    }
    *out_frac = (w[0] * (1.0 - tx) + w[1] * tx) * (1.0 - tz) + (w[2] * (1.0 - tx) + w[3] * tx) * tz;
    return 1;
}
