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
    ok = gr_raster_read_window(path, x0, z0, x1, z1, TD5_ROAM_HEIGHT_PAD, &s->r);
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

/* ---------------------------------------------------------------- dev test -- */

#ifndef TD5RE_RELEASE

/* The projection of one PLACE.JSON, as td5_geo_route.c's GeoProj holds it.
 * Re-derived here (its gr_proj_* helpers are static to that module); the test's
 * own height differences are what checks the mapping. */
typedef struct {
    double lat0, lon0, upm, mdlat, mdlon, cs, sn, offx, offz;
} RoamTestProj;

#define ROAM_TEST_WGS_A  6378137.0
#define ROAM_TEST_WGS_F  (1.0 / 298.257223563)
#define ROAM_TEST_RAD    0.017453292519943295
#define ROAM_TEST_N      1000

static char *roam_test_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)8 * 1024 * 1024) { td5_plat_file_close(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { td5_plat_file_close(f); return NULL; }
    if (td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) {
        free(buf); td5_plat_file_close(f); return NULL;
    }
    buf[n] = '\0';
    td5_plat_file_close(f);
    return buf;
}

static int roam_test_proj(const char *path, RoamTestProj *p)
{
    char *json = roam_test_slurp(path);
    cJSON *root, *pr, *a, *o, *u, *r, *ox, *oz;
    double e2, s, w, phi, rot;
    int ok = 0;
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    pr = cJSON_GetObjectItem(root, "projection");
    if (pr && cJSON_IsObject(pr)) {
        a  = cJSON_GetObjectItem(pr, "lat0");
        o  = cJSON_GetObjectItem(pr, "lon0");
        u  = cJSON_GetObjectItem(pr, "units_per_metre");
        r  = cJSON_GetObjectItem(pr, "rotation_rad");
        ox = cJSON_GetObjectItem(pr, "offset_x");
        oz = cJSON_GetObjectItem(pr, "offset_z");
        if (cJSON_IsNumber(a) && cJSON_IsNumber(o)) {
            p->lat0 = a->valuedouble;
            p->lon0 = o->valuedouble;
            p->upm  = cJSON_IsNumber(u) ? u->valuedouble : 430.0;
            rot     = cJSON_IsNumber(r) ? r->valuedouble : 0.0;
            p->cs   = cos(rot);
            p->sn   = sin(rot);
            p->offx = cJSON_IsNumber(ox) ? ox->valuedouble : 0.0;
            p->offz = cJSON_IsNumber(oz) ? oz->valuedouble : 0.0;
            /* gr_proj_init, same constants and order. */
            e2  = ROAM_TEST_WGS_F * (2.0 - ROAM_TEST_WGS_F);
            phi = p->lat0 * ROAM_TEST_RAD;
            s   = sin(phi);
            w   = 1.0 - e2 * s * s;
            p->mdlat = ROAM_TEST_RAD * ROAM_TEST_WGS_A * (1.0 - e2) / pow(w, 1.5);
            p->mdlon = ROAM_TEST_RAD * ROAM_TEST_WGS_A * cos(phi) / sqrt(w);
            ok = 1;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* The route-frame point (xr, zr) as the place frame sees it: the same chain as
 * gr_affine_old_from_new, route -> lat/lon -> place. */
static void roam_test_route_to_place(const RoamTestProj *np, const RoamTestProj *op,
                                     double xr, double zr, double *xs, double *zs)
{
    const double xm = (xr - np->offx) / np->upm;
    const double zm = (zr - np->offz) / np->upm;
    const double e  = xm * np->cs + zm * np->sn;
    const double n  = -xm * np->sn + zm * np->cs;
    const double lat = np->lat0 + n / np->mdlat;
    const double lon = np->lon0 + e / np->mdlon;
    const double e2 = (lon - op->lon0) * op->mdlon;
    const double n2 = (lat - op->lat0) * op->mdlat;
    *xs = (e2 * op->cs - n2 * op->sn) * op->upm + op->offx;
    *zs = (e2 * op->sn + n2 * op->cs) * op->upm + op->offz;
}

typedef struct {
    double xr, zr;          /* a point of the derived route window, route frame */
    double xs, zs;          /* the same point in the source place frame */
    double xc, zc;          /* snapped to the nearest derived cell centre */
    double xsc, zsc;        /* the snapped point in the source frame */
    long   tx, tz;          /* source tile key (256 m), the sort key */
    double hg, hr;          /* td5_geo_height / source reader, the point */
    double hgc, hrc;        /* the same two at the snapped point */
    double hrs[4];          /* source height at the snapped point nudged half a cell: +x -x +z -z */
    int    wg, wr;          /* wet by td5_geo_is_water / by the source window */
    int    wgc, wrc;        /* the same two at the snapped point */
    int    got, gotc;       /* 1 when the source window covered the point */
    int    gots[4];         /* the four nudged samples covered */
} RoamTestPt;

static int roam_test_cmp(const void *a, const void *b)
{
    const RoamTestPt *p = (const RoamTestPt *)a;
    const RoamTestPt *q = (const RoamTestPt *)b;
    if (p->tz != q->tz) return p->tz < q->tz ? -1 : 1;
    if (p->tx != q->tx) return p->tx < q->tx ? -1 : 1;
    return 0;
}

/* Header of a derived or source raster, for its extent. */
static int roam_test_header(const char *path, GrRaster *h)
{
    return roam_peek(path, ROAM_KIND_I16, h);
}

int td5_roam_height_selftest(void)
{
    char sh[512], sw[512], dh[512], sj[512], dj[512], stamp[512];
    const char *slug;
    RoamTestProj np, op;
    GrRaster dhdr, shdr;
    RoamTestPt *pts;
    TD5_RoamHeight c, c2;
    double tile_u, halo_u, ox, oz, cell, exag, max_d = -1.0, max_dc = -1.0;
    double sum_d = 0.0, sum_d2 = 0.0, sum_dc = 0.0, max_at_x = 0.0, max_at_z = 0.0;
    double tile_ms, bound_m;
    uint32_t lcg = 0x5EED0A11u;
    uint64_t prefetch_t0, prefetch_t1;
    int i, k, m, n_ok = 0, n_okc = 0, n_uncov = 0, n_uncovc = 0, n_exact = 0, n_exactc = 0;
    int n_dry = 0, n_is_floor = 0, n_is_round = 0;
    double sum_sgn_c = 0.0, sum_nudge[4] = { 0.0, 0.0, 0.0, 0.0 };
    int n_nudge[4] = { 0, 0, 0, 0 };
    const char *const nudge_name[4] = { "+x", "-x", "+z", "-z" };
    /* (n_uncovc counts snapped points the window did not cover.) */
    int n_wet_mis = 0, n_wet_misc = 0, groups = 0, n_pref_fail = 0, max_at = -1;
    int ex100;
    unsigned reads0;

    td5_geo_sync();
    if (!td5_geo_loaded()) {
        printf("ROAMH: no geo place loaded; nothing to test\n");
        return 1;
    }
    slug = td5_geo_place_slug();
    td5_geo_source_path(sh, sizeof sh, slug, "HEIGHT.R16");
    td5_geo_source_path(sw, sizeof sw, slug, "WATER.R8");
    td5_geo_source_path(sj, sizeof sj, slug, "PLACE.JSON");
    td5_geo_place_path(dh, sizeof dh, slug, "HEIGHT.R16");
    td5_geo_place_path(dj, sizeof dj, slug, "PLACE.JSON");
    td5_geo_derived_path(stamp, sizeof stamp, slug, TD5_GEO_DERIVED_STAMP);

    ex100 = td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400);
    exag = (double)ex100 / 100.0;

    printf("ROAMH place=%s derived=%s (stamp %s)\n", slug, dh,
           td5_plat_file_exists(stamp) ? "present" : "absent");
    printf("ROAMH src height=%s\n", sh);
    printf("ROAMH src water =%s\n", sw);

    if (!roam_test_proj(sj, &op) || !roam_test_proj(dj, &np)) {
        printf("ROAMH FAIL: a PLACE.JSON projection could not be read\n");
        return 1;
    }
    if (!roam_test_header(dh, &dhdr) || !roam_test_header(sh, &shdr)) {
        printf("ROAMH FAIL: a HEIGHT.R16 header could not be read\n");
        return 1;
    }
    ox = dhdr.origin_x; oz = dhdr.origin_z; cell = dhdr.cell;
    printf("ROAMH derived grid %dx%d cell=%.3f origin=(%.1f,%.1f) rot=%.6f\n",
           dhdr.w, dhdr.h, cell, ox, oz, dhdr.rotation);
    printf("ROAMH source  grid %dx%d cell=%.3f origin=(%.1f,%.1f) scale=%.6f\n",
           shdr.w, shdr.h, shdr.cell, shdr.origin_x, shdr.origin_z, shdr.scale);
    printf("ROAMH exaggeration=%.2f  upm=%.1f\n", exag, op.upm);

    /* Tiles: 256 m, with a 1 km halo, in place-frame world units. */
    tile_u = 256.0 * op.upm;
    halo_u = 1000.0 * op.upm;

    pts = (RoamTestPt *)malloc(sizeof(RoamTestPt) * ROAM_TEST_N);
    if (!pts) { printf("ROAMH FAIL: out of memory\n"); return 1; }

    /* 1000 points, uniform over the derived route window. Its own LCG, not the
     * game's RNG. Each point also gets its snapped twin: the nearest derived
     * cell centre, which td5_geo_height reads exactly. */
    for (i = 0; i < ROAM_TEST_N; i++) {
        RoamTestPt *p = &pts[i];
        double u, v, fx, fz;
        int cix, ciz;
        lcg = lcg * 1664525u + 1013904223u; u = (double)(lcg >> 8) / 16777216.0;
        lcg = lcg * 1664525u + 1013904223u; v = (double)(lcg >> 8) / 16777216.0;
        p->xr = ox + u * (double)(dhdr.w - 1) * cell;
        p->zr = oz + v * (double)(dhdr.h - 1) * cell;
        p->hg = td5_geo_height(p->xr, p->zr);
        p->wg = td5_geo_is_water(p->xr, p->zr);
        roam_test_route_to_place(&np, &op, p->xr, p->zr, &p->xs, &p->zs);
        p->tx = (long)floor(p->xs / tile_u);
        p->tz = (long)floor(p->zs / tile_u);

        fx = (p->xr - ox) / cell; fz = (p->zr - oz) / cell;
        cix = (int)floor(fx + 0.5); ciz = (int)floor(fz + 0.5);
        if (cix < 0) cix = 0;
        if (cix > dhdr.w - 1) cix = dhdr.w - 1;
        if (ciz < 0) ciz = 0;
        if (ciz > dhdr.h - 1) ciz = dhdr.h - 1;
        p->xc = ox + (double)cix * cell;
        p->zc = oz + (double)ciz * cell;
        p->hgc = td5_geo_height(p->xc, p->zc);
        p->wgc = td5_geo_is_water(p->xc, p->zc);
        roam_test_route_to_place(&np, &op, p->xc, p->zc, &p->xsc, &p->zsc);
    }
    qsort(pts, ROAM_TEST_N, sizeof(RoamTestPt), roam_test_cmp);

    if (!td5_roam_height_open(&c, sh, sw, exag)) {
        printf("ROAMH FAIL: source HEIGHT.R16 would not open\n");
        free(pts);
        return 1;
    }
    reads0 = c.reads;
    prefetch_t0 = td5_plat_time_us();

    /* Walk the points tile by tile. One prefetch per tile, its window being the
     * 256 m tile plus the 1 km halo, so each tile is read once. */
    for (i = 0; i < ROAM_TEST_N; ) {
        int j = i;
        double x0, z0, x1, z1;
        while (j < ROAM_TEST_N && pts[j].tx == pts[i].tx && pts[j].tz == pts[i].tz) j++;
        x0 = (double)pts[i].tx * tile_u - halo_u;
        z0 = (double)pts[i].tz * tile_u - halo_u;
        x1 = (double)(pts[i].tx + 1) * tile_u + halo_u;
        z1 = (double)(pts[i].tz + 1) * tile_u + halo_u;
        if (!td5_roam_height_prefetch(&c, x0, z0, x1, z1)) n_pref_fail++;
        groups++;
        for (k = i; k < j; k++) {
            RoamTestPt *p = &pts[k];
            int wr = 0, wrc = 0;
            double hr = 0.0, hrc = 0.0;
            p->got  = td5_roam_height_sample(&c, p->xs, p->zs, &hr, &wr);
            p->gotc = td5_roam_height_sample(&c, p->xsc, p->zsc, &hrc, &wrc);
            p->hr = hr; p->wr = wr;
            p->hrc = hrc; p->wrc = wrc;
            /* Diagnostic: the same snapped point, source sampled half a source cell
             * away in each direction. If the mapping is right, no nudge beats zero. */
            {
                const double hcell = 0.5 * shdr.cell;
                const double dxs[4] = { hcell, -hcell, 0.0, 0.0 };
                const double dzs[4] = { 0.0, 0.0, hcell, -hcell };
                for (m = 0; m < 4; m++) {
                    double hn = 0.0;
                    int wn = 0;
                    p->gots[m] = td5_roam_height_sample(&c, p->xsc + dxs[m], p->zsc + dzs[m], &hn, &wn);
                    p->hrs[m] = hn;
                }
            }
        }
        i = j;
    }
    prefetch_t1 = td5_plat_time_us();

    for (i = 0; i < ROAM_TEST_N; i++) {
        const RoamTestPt *p = &pts[i];
        double d, dc;
        if (!p->got) {
            n_uncov++;
        } else {
            d = fabs(p->hg - p->hr);
            n_ok++;
            sum_d += d; sum_d2 += d * d;
            if (d == 0.0) n_exact++;
            if (d > max_d) { max_d = d; max_at = i; max_at_x = p->xr; max_at_z = p->zr; }
            if (p->wg != p->wr) n_wet_mis++;
        }
        if (!p->gotc) {
            n_uncovc++;
        } else {
            dc = fabs(p->hgc - p->hrc);
            n_okc++;
            sum_dc += dc;
            sum_sgn_c += p->hgc - p->hrc;
            /* Dry points only (a wet bed is lowered by the same constant on both
             * sides, but the raw conversion below would be off by it). Derived raw
             * against the source raw, unrounded: is it the floor or the nearest? */
            if (!p->wgc) {
                const double rd = (p->hgc / exag - shdr.bias) / shdr.scale;
                const double rs = (p->hrc / exag - shdr.bias) / shdr.scale;
                n_dry++;
                if (fabs(rd - floor(rs)) < 1e-3) n_is_floor++;
                if (fabs(rd - floor(rs + 0.5)) < 1e-3) n_is_round++;
            }
            for (m = 0; m < 4; m++) {
                if (!p->gots[m]) continue;
                n_nudge[m]++;
                sum_nudge[m] += fabs(p->hgc - p->hrs[m]);
            }
            if (dc == 0.0) n_exactc++;
            if (dc > max_dc) max_dc = dc;
            if (p->wgc != p->wrc) n_wet_misc++;
        }
    }

    printf("ROAMH points=%d tiles=%d prefetch_fail=%d uncovered=%d (snapped %d)\n",
           ROAM_TEST_N, groups, n_pref_fail, n_uncov, n_uncovc);
    printf("ROAMH raw route-frame point: max|dh|=%.6f units at (%.1f,%.1f) mean=%.6f rms=%.6f exact=%d/%d\n",
           max_d, max_at_x, max_at_z,
           n_ok ? sum_d / n_ok : 0.0,
           n_ok ? sqrt(sum_d2 / n_ok) : 0.0,
           n_exact, n_ok);
    printf("ROAMH raw route-frame point: water-flag mismatches=%d (index %d)\n", n_wet_mis, max_at);
    printf("ROAMH snapped to derived cell centre: max|dh|=%.6f units mean=%.6f exact=%d/%d water-flag mismatches=%d\n",
           max_dc, n_okc ? sum_dc / n_okc : 0.0, n_exactc, n_okc, n_wet_misc);

    /* A derived cell is the source bilinear rounded to a raw int16 step, so its
     * error against the source is at most half a raw step, times the scale and
     * the exaggeration (before any water lowering). */
    bound_m = 0.5 * shdr.scale * exag;
    printf("ROAMH rounding bound: 0.5 * scale(%.6f) * exag(%.2f) = %.6f units\n",
           shdr.scale, exag, bound_m);
    printf("ROAMH snapped signed mean (td5_geo_height - source) = %+.6f units\n",
           n_okc ? sum_sgn_c / n_okc : 0.0);
    printf("ROAMH derived raw == floor(source raw): %d/%d dry points; == nearest integer: %d/%d\n",
           n_is_floor, n_dry, n_is_round, n_dry);
    for (m = 0; m < 4; m++) {
        printf("ROAMH snapped, source nudged %s by half a cell: mean|dh|=%.6f units (n=%d)\n",
               nudge_name[m], n_nudge[m] ? sum_nudge[m] / n_nudge[m] : 0.0, n_nudge[m]);
    }
    printf("ROAMH window reads: %u total for the 1000-point walk; prefetch wall %.1f ms\n",
           c.reads - reads0, (double)(prefetch_t1 - prefetch_t0) / 1000.0);
    printf("ROAMH read time by layer (walk): height %.1f ms, water %.1f ms\n",
           (double)c.read_us[TD5_ROAM_HEIGHT_LAYER_HEIGHT] / 1000.0,
           (double)c.read_us[TD5_ROAM_HEIGHT_LAYER_WATER] / 1000.0);

    /* Timing: one 256 m tile plus a 1 km halo on each side, both layers, on a
     * fresh context so nothing is resident. */
    if (ROAM_TEST_N > 0) {
        const double x0 = (double)pts[0].tx * tile_u - halo_u;
        const double z0 = (double)pts[0].tz * tile_u - halo_u;
        const double x1 = (double)(pts[0].tx + 1) * tile_u + halo_u;
        const double z1 = (double)(pts[0].tz + 1) * tile_u + halo_u;
        uint64_t t0, t1;
        if (td5_roam_height_open(&c2, sh, sw, exag)) {
            t0 = td5_plat_time_us();
            td5_roam_height_prefetch(&c2, x0, z0, x1, z1);
            t1 = td5_plat_time_us();
            tile_ms = (double)(t1 - t0) / 1000.0;
            printf("ROAMH TIMING one 256 m tile + 1 km halo (%.0f x %.0f m), cold: %.2f ms total "
                   "(height %.2f ms, water %.2f ms, %u reads)\n",
                   (x1 - x0) / op.upm, (z1 - z0) / op.upm, tile_ms,
                   (double)c2.read_us[TD5_ROAM_HEIGHT_LAYER_HEIGHT] / 1000.0,
                   (double)c2.read_us[TD5_ROAM_HEIGHT_LAYER_WATER] / 1000.0, c2.reads);
            td5_roam_height_close(&c2);
        }
    }

    td5_roam_height_close(&c);
    free(pts);
    printf("ROAMH done\n");
    return 0;
}

#endif /* TD5RE_RELEASE */
