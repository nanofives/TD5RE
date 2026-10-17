/**
 * td5_roam_scenery.c -- GEO FREE ROAM: the SCENERY layer of one tile (PORT-ONLY,
 * roam F4a). Contract and rules: td5_roam_scenery.h. Design: GEO_FREE_ROAM_DESIGN.md 4.
 *
 * SECTIONS
 *   geometry       polygon overlap (the guard), point/segment helpers
 *   source         INDEX.JSON, the OSM cache-tile LRU (BUILDINGS / AREAS / FOOTWAYS),
 *                  the neighbour ROAD-tile LRU
 *   tile ctx       TG_TileCtx: drivable primitives + street centrelines of the 3x3
 *                  ROAD tiles on a 16 m grid; roam_nearest_edge / roam_edge_frame
 *   buildings      ownership, landmark clusters, plaza veto, clearance, the shared core
 *   plazas         lawns (refined, cut off the road), trees (world lattice + canopy)
 *   footpaths      mapped paths in pieces, cut off the road, merged into the street band
 *   assemble       the blob + record table + hash, the guard census over the bytes
 *   dev CLI        --RoamBuildScenery
 *
 * Every per-record decision keys off the record itself (way id hash, place-frame
 * position), never off the order tiles are built in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_tg_geo_bld.h"      /* td5_trackgen_internal.h + the extrusion core */
#include "td5_geo.h"
#include "td5_geo_buildings.h"
#include "td5_geo_landmarks.h"   /* td5_geolm_apron (pure hull)                */
#include "td5_geo_footways.h"
#include "td5_geo_roads.h"       /* TD5_GEO_RC_*                                */
#include "td5_geo_route.h"       /* GrRaster, gr_raster_read_window              */
#include "td5_config.h"
#include "td5_platform.h"
#include "deps/cjson/cJSON.h"
#include "td5_roam_tile.h"
#include "td5_roam_height.h"
#include "td5_roam_scenery.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "roam"

#define SC_UPM            430.0
#define SC_ROW            1500.0
/* the strip's numbers, kept equal on purpose (td5_tg_city.c / td5_tg_streets.c /
 * td5_tg_geo_street.c): a change there is a change here */
#define SC_MAX_SHIFT      4500.0      /* TD5_TG_GEO_MAX_SHIFT                       */
#define SC_GEOP_MIN_R     3000.0      /* TD5_TG_GEOP_MIN_R                          */
#define SC_GEOP_LIFT      60.0        /* TD5_TG_GEOP_LIFT                           */
#define SC_GEOP_TIER      45.0        /* TD5_TG_GEOP_TIER                           */
#define SC_LAWN_STEP      8000.0      /* TD5_TG_GEOP_LAWN_STEP                      */
#define SC_LAWN_TRI_MAX   4096        /* TD5_TG_GEOP_LAWN_TRI_MAX                   */
#define SC_LAWN_CHUNK     64          /* TD5_TG_GEOP_LAWN_CHUNK                     */
#define SC_TREE_STEP      5160.0      /* TD5_TG_GEOP_TREE_STEP                      */
#define SC_TREES_BIG      128         /* TD5_TG_GEOP_TREES_BIG                      */
#define SC_CANOPY_M       3           /* TD5_TG_GEOP_CANOPY_M                       */
#define SC_FP_LIFT        16.0        /* TG_GEOFP_LIFT                              */
#define SC_PARK_PATH_W    1300.0      /* TD5_TG_GEOP_PATH_W                         */
#define SC_FP_DEF_W       (1.60 * TD5_TG_INFRA_M)
#define SC_FP_MIN_W       (0.80 * TD5_TG_INFRA_M)
#define SC_FP_MAX_W       (8.00 * TD5_TG_INFRA_M)
#define SC_FP_KIND_MASK   ((1u << TD5_GEO_FW_FOOTWAY) | (1u << TD5_GEO_FW_PATH) | \
                           (1u << TD5_GEO_FW_PEDESTRIAN) | (1u << TD5_GEO_FW_CYCLEWAY))
#define SC_PLAZA_KEEP_M2  60.0        /* GEOB_PLAZA_KEEP_M2                         */
#define SC_LM_SLACK_M     3.0         /* GEOLM_EDGE_SLACK_M                         */
#define SC_APRON_ROAD_M   9.0         /* TD5_TG_GEO_APRON_ROAD_M                    */
/* roam's own */
#define SC_FP_PIECE       (6.0 * SC_UPM)   /* footpath piece, the strip's sub-piece  */
#define SC_FP_MERGE_M     1.5         /* a path this far past the pavement band is the street's own */
#define SC_TREE_CLEAR     1500.0      /* trunk square half size kept off the road   */
#define SC_HOST_MAX_M     60.0        /* a footprint clears the street within this  */
#define SC_HALO_M         400.0       /* records / raster window around the tile    */
#define SC_CELL           6880.0      /* 16 m ctx grid                              */
#define SC_FP_CHUNK       64          /* footpath quads per mesh                    */
#define SC_CACHE_LRU      48          /* slots per layer (array size)              */
static const int k_sc_lru[3] = { 8, 48, 48 };   /* parsed OSM cache tiles kept: BUILDINGS (~2-3 MB each), AREAS, FOOTWAYS (tiny) */
#define SC_ROAD_LRU       24          /* neighbour ROAD tiles kept                  */
#define SC_LAYERS         3
enum { SC_L_BLD = 0, SC_L_AREA = 1, SC_L_FOOT = 2 };
static const char *const k_sc_layer[SC_LAYERS] = { "BUILDINGS", "AREAS", "FOOTWAYS" };
static const char *const k_sc_key[SC_LAYERS]   = { "buildings", "areas", "footways" };

/* ============================================================================
 * SECTION: geometry
 * ========================================================================== */

static double sc_cross(double ax, double az, double bx, double bz, double cx, double cz)
{
    return (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
}

/* proper or touching intersection of segments ab and cd */
static int sc_seg_hit(double ax, double az, double bx, double bz,
                      double cx, double cz, double dx, double dz)
{
    const double d1 = sc_cross(cx, cz, dx, dz, ax, az), d2 = sc_cross(cx, cz, dx, dz, bx, bz);
    const double d3 = sc_cross(ax, az, bx, bz, cx, cz), d4 = sc_cross(ax, az, bx, bz, dx, dz);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return 1;
    return 0;
}

/* point strictly inside a CONVEX polygon of either winding */
static int sc_in_convex(const double *x, const double *z, int n, double px, double pz)
{
    int i, pos = 0, neg = 0;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double c = sc_cross(x[i], z[i], x[j], z[j], px, pz);
        if (c > 1e-6) pos = 1; else if (c < -1e-6) neg = 1; else return 0;
        if (pos && neg) return 0;
    }
    return 1;
}

/* Does polygon A (any simple ring, or a 2-point segment) overlap CONVEX polygon B?
 * Exact: a vertex of one inside the other, or two edges crossing. */
static int sc_poly_overlap(const double *ax, const double *az, int na,
                           const double *bx, const double *bz, int nb)
{
    int i, j;
    for (i = 0; i < na; i++)
        if (sc_in_convex(bx, bz, nb, ax[i], az[i])) return 1;
    if (na >= 3)
        for (j = 0; j < nb; j++)
            if (td5_geob_point_in_ring(ax, az, na, bx[j], bz[j])) return 1;
    for (i = 0; i < na; i++) {
        const int i2 = (na == 2) ? 1 : (i + 1) % na;
        if (na == 2 && i == 1) break;
        for (j = 0; j < nb; j++) {
            const int j2 = (j + 1) % nb;
            if (sc_seg_hit(ax[i], az[i], ax[i2], az[i2], bx[j], bz[j], bx[j2], bz[j2])) return 1;
        }
    }
    return 0;
}

static double sc_seg_dist(double px, double pz, double ax, double az, double bx, double bz, double *t_out)
{
    const double dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
    double t = l2 > 0.0 ? ((px - ax) * dx + (pz - az) * dz) / l2 : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    if (t_out) *t_out = t;
    return hypot(px - (ax + dx * t), pz - (az + dz * t));
}

static uint32_t sc_fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* ============================================================================
 * SECTION: source (OSM cache tiles, neighbour ROAD tiles)
 * ========================================================================== */

typedef struct { int ix, iz; double x0, z0, x1, z1; } ScIdxRow;

typedef struct {
    int first, count, kind, surface, structure;   /* structure: bridge|tunnel|covered */
    double width_m, minx, minz, maxx, maxz, id;
} ScFoot;

typedef struct {
    int       used;
    int       layer, ix, iz;
    uint32_t  clock;
    int       pin;
    int       n;
    TD5_GeoBuilding *b;  TD5_GeoArea *a;  ScFoot *f;
    double   *id;
    int64_t  *own;          /* owner tile key per record (b, a)          */
    int      *order;        /* record indices sorted by (own, id, cx, cz) */
    double   *px, *pz;      /* ring / way points                          */
    int       npt;
    double    storey_m;     /* BUILDINGS storey_height_m of this file     */
    size_t    bytes;
} ScCache;

typedef struct { int tx, tz; RoamTile *t; int absent; uint32_t clock; } ScRoad;

struct TD5_RoamScnSource {
    char      place[64];
    RoamGrid  grid;
    double    upm;
    int       tag_lm, area_wide;
    ScIdxRow *rows[SC_LAYERS];
    int       nrows[SC_LAYERS];
    ScCache   cache[SC_LAYERS][SC_CACHE_LRU];
    ScRoad    road[SC_ROAD_LRU];
    uint32_t  clock;
    TD5_RoamScnBuildFn build;
    void     *build_u;
    char      canopy_path[512];
    /* the scenery's OWN height source, re-windowed per tile with ONE fixed rectangle:
     * td5_roam_height_sample computes cell coordinates from the window origin, so a
     * shared LRU (whichever window happens to be resident) moves the low bits of a
     * height with the build order */
    char      height_path[512], water_path[512];
    double    exag;
    TD5_RoamHeight *hc;
    int       hc_open;
    double    gap;            /* carriageway clearance of a footprint (units)    */
    int       fp_on, plaza_on, bld_on, lm_prefab;
    /* reused per build */
    TG_TileCtx *ctx;
    TD5_RoamFurnSource *furn;   /* [F4c] street furniture (td5_roam_furniture.c), this thread's */
};

static int64_t sc_tkey(int tx, int tz) { return ((int64_t)tx << 32) | (uint32_t)tz; }

static char *sc_slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (192L << 20)) { fclose(f); return NULL; }
    b = (char *)malloc((size_t)n + 1u);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = '\0'; if (len) *len = (size_t)n; }
    return b;
}

static void sc_cache_free(ScCache *c)
{
    free(c->b); free(c->a); free(c->f); free(c->id); free(c->own); free(c->order);
    free(c->px); free(c->pz);
    memset(c, 0, sizeof *c);
}

typedef struct { int64_t own; double id, cx, cz; int idx; } ScOrd;

static int sc_ord_cmp(const void *pa, const void *pb)
{
    const ScOrd *a = (const ScOrd *)pa, *b = (const ScOrd *)pb;
    if (a->own != b->own) return a->own < b->own ? -1 : 1;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    if (a->cx != b->cx) return a->cx < b->cx ? -1 : 1;
    if (a->cz != b->cz) return a->cz < b->cz ? -1 : 1;
    return a->idx < b->idx ? -1 : (a->idx > b->idx);
}

/* records sorted by (owner tile, id, cx, cz): the keys travel with the index, so
 * the comparator needs no context (no file-scope state) */
static void sc_cache_sort(ScCache *c)
{
    ScOrd *o;
    int i;
    c->order = (int *)malloc((size_t)(c->n > 0 ? c->n : 1) * sizeof(int));
    o = (ScOrd *)malloc((size_t)(c->n > 0 ? c->n : 1) * sizeof(ScOrd));
    if (!c->order || !o) { free(o); free(c->order); c->order = NULL; c->n = 0; return; }
    for (i = 0; i < c->n; i++) {
        o[i].own = c->own[i]; o[i].id = c->id[i];
        o[i].cx = c->b ? c->b[i].cx : c->a[i].cx;
        o[i].cz = c->b ? c->b[i].cz : c->a[i].cz;
        o[i].idx = i;
    }
    qsort(o, (size_t)c->n, sizeof(ScOrd), sc_ord_cmp);
    for (i = 0; i < c->n; i++) c->order[i] = o[i].idx;
    free(o);
}

static int sc_pool_push(ScCache *c, int *cap, const double *x, const double *z, int n)
{
    int i;
    if (c->npt + n > *cap) {
        int nc = *cap ? *cap * 2 : 65536;
        double *nx, *nz;
        while (nc < c->npt + n) nc *= 2;
        nx = (double *)realloc(c->px, (size_t)nc * sizeof(double));
        if (!nx) return -1;
        c->px = nx;
        nz = (double *)realloc(c->pz, (size_t)nc * sizeof(double));
        if (!nz) return -1;
        c->pz = nz;
        *cap = nc;
    }
    for (i = 0; i < n; i++) { c->px[c->npt + i] = x[i]; c->pz[c->npt + i] = z[i]; }
    c->npt += n;
    return c->npt - n;
}

/* Parse one OSM cache tile of `layer` into slot c. 1 on success (an empty or
 * missing file is an empty slot, also 1: "nothing there" is a stable answer). */
static int sc_cache_load_body(TD5_RoamScnSource *s, ScCache *c, int layer, int ix, int iz);
static int sc_cache_load(TD5_RoamScnSource *s, ScCache *c, int layer, int ix, int iz)
{
    const uint32_t t0 = td5_plat_time_ms();
    const int r = sc_cache_load_body(s, c, layer, ix, iz);
    TD5_LOG_I(LOG_TAG, "ROAM_SCN_CACHE %s_%d_%d: %d record(s), %.1f MB kept, %u ms (from %u to %u ms)",
              k_sc_layer[layer], ix, iz, c->n, (double)c->bytes / 1048576.0, (unsigned)(td5_plat_time_ms() - t0),
              (unsigned)t0, (unsigned)td5_plat_time_ms());
    return r;
}

static int sc_cache_load_body(TD5_RoamScnSource *s, ScCache *c, int layer, int ix, int iz)
{
    char rel[96], path[512];
    char *json;
    cJSON *root, *arr, *e;
    int cap_rec = 0, cap_pt = 0, idx = 0;
    double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX];
    double storey_m = 3.0;

    sc_cache_free(c);
    c->used = 1; c->layer = layer; c->ix = ix; c->iz = iz;
    snprintf(rel, sizeof rel, "tiles/%s_%d_%d.JSON", k_sc_layer[layer], ix, iz);
    td5_geo_source_path(path, sizeof path, s->place, rel);
    json = sc_slurp(path, NULL);
    if (!json) return 1;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 1;
    if (layer == SC_L_BLD) storey_m = td5_geob_storey_m_of(root);
    c->storey_m = storey_m;
    arr = cJSON_GetObjectItem(root, k_sc_key[layer]);
    cap_rec = arr ? cJSON_GetArraySize(arr) : 0;
    if (cap_rec > 0) {
        if (layer == SC_L_BLD) c->b = (TD5_GeoBuilding *)calloc((size_t)cap_rec, sizeof(TD5_GeoBuilding));
        if (layer == SC_L_AREA) c->a = (TD5_GeoArea *)calloc((size_t)cap_rec, sizeof(TD5_GeoArea));
        if (layer == SC_L_FOOT) c->f = (ScFoot *)calloc((size_t)cap_rec, sizeof(ScFoot));
        c->id = (double *)calloc((size_t)cap_rec, sizeof(double));
        c->own = (int64_t *)calloc((size_t)cap_rec, sizeof(int64_t));
    }
    for (e = arr ? arr->child : NULL; e; e = e->next, idx++) {
        double id = (double)idx;
        int n, first, tx, tz;
        if (!c->id) break;
        if (layer == SC_L_BLD) {
            TD5_GeoBuilding *b = &c->b[c->n];
            n = td5_geob_parse_building(e, idx, s->tag_lm, storey_m, s->upm, b, rx, rz, &id);
            if (n <= 0) continue;
            first = sc_pool_push(c, &cap_pt, rx, rz, n);
            if (first < 0) break;
            b->first = first;
            roam_grid_tile_of(&s->grid, b->cx, b->cz, &tx, &tz);
        } else if (layer == SC_L_AREA) {
            TD5_GeoArea *a = &c->a[c->n];
            n = td5_geob_parse_area(e, idx, s->area_wide, a, rx, rz, &id);
            if (n <= 0) continue;
            first = sc_pool_push(c, &cap_pt, rx, rz, n);
            if (first < 0) break;
            a->first = first;
            roam_grid_tile_of(&s->grid, a->cx, a->cz, &tx, &tz);
        } else {
            const cJSON *pts = cJSON_GetObjectItem(e, "points"), *p, *w, *v;
            ScFoot *f = &c->f[c->n];
            double px[TD5_GEO_FOOTWAYS_MAX_WAY], pz[TD5_GEO_FOOTWAYS_MAX_WAY];
            int kept = 0;
            if (!pts || !cJSON_IsArray(pts)) continue;
            for (p = pts->child; p && kept < TD5_GEO_FOOTWAYS_MAX_WAY; p = p->next) {
                const cJSON *qx = cJSON_GetObjectItem(p, "x"), *qz = cJSON_GetObjectItem(p, "z");
                double x, z;
                if (!qx || !qz || !cJSON_IsNumber(qx) || !cJSON_IsNumber(qz)) continue;
                x = qx->valuedouble; z = qz->valuedouble;
                if (kept > 0 && (x - px[kept - 1]) * (x - px[kept - 1]) + (z - pz[kept - 1]) * (z - pz[kept - 1]) < 1.0)
                    continue;
                px[kept] = x; pz[kept] = z;
                if (kept == 0) { f->minx = f->maxx = x; f->minz = f->maxz = z; }
                else {
                    if (x < f->minx) f->minx = x;
                    if (x > f->maxx) f->maxx = x;
                    if (z < f->minz) f->minz = z;
                    if (z > f->maxz) f->maxz = z;
                }
                kept++;
            }
            if (kept < 2) continue;
            first = sc_pool_push(c, &cap_pt, px, pz, kept);
            if (first < 0) break;
            f->first = first; f->count = kept;
            v = cJSON_GetObjectItem(e, "kind");
            f->kind = td5_geo_footways_kind_of((v && cJSON_IsString(v)) ? v->valuestring : NULL);
            v = cJSON_GetObjectItem(e, "surface");
            f->surface = td5_geo_footways_surface_of((v && cJSON_IsString(v)) ? v->valuestring : NULL);
            f->structure = cJSON_IsTrue(cJSON_GetObjectItem(e, "bridge")) || cJSON_IsTrue(cJSON_GetObjectItem(e, "tunnel"))
                        || cJSON_IsTrue(cJSON_GetObjectItem(e, "covered"));
            w = cJSON_GetObjectItem(e, "width_m");
            f->width_m = (w && cJSON_IsNumber(w) && w->valuedouble > 0.0 && w->valuedouble < 100.0) ? w->valuedouble : 0.0;
            v = cJSON_GetObjectItem(e, "id");
            id = (v && cJSON_IsNumber(v)) ? v->valuedouble : (double)idx;
            f->id = id;
            tx = tz = 0;
        }
        c->id[c->n] = id;
        c->own[c->n] = sc_tkey(tx, tz);
        c->n++;
    }
    cJSON_Delete(root);
    if (layer != SC_L_FOOT) sc_cache_sort(c);
    c->bytes = (size_t)c->npt * 16u + (size_t)c->n * (sizeof(double) + sizeof(int64_t) + sizeof(int) +
               (layer == SC_L_BLD ? sizeof(TD5_GeoBuilding) : layer == SC_L_AREA ? sizeof(TD5_GeoArea) : sizeof(ScFoot)));
    return 1;
}

/* The cache tiles of `layer` whose extent meets [x0,x1]x[z0,z1] (place frame),
 * loaded and pinned. Writes up to cap slot pointers in INDEX order. */
static int sc_cache_window(TD5_RoamScnSource *s, int layer, double x0, double z0, double x1, double z1,
                           ScCache **out, int cap)
{
    int r, n = 0, k;
    for (r = 0; r < s->nrows[layer] && n < cap; r++) {
        const ScIdxRow *row = &s->rows[layer][r];
        ScCache *hit = NULL, *victim = NULL;
        if (row->x1 < x0 || row->x0 > x1 || row->z1 < z0 || row->z0 > z1) continue;
        for (k = 0; k < k_sc_lru[layer]; k++) {
            ScCache *c = &s->cache[layer][k];
            if (c->used && c->ix == row->ix && c->iz == row->iz) { hit = c; break; }
        }
        if (!hit) {
            for (k = 0; k < k_sc_lru[layer]; k++) {
                ScCache *c = &s->cache[layer][k];
                if (c->pin) continue;
                if (!c->used) { victim = c; break; }
                if (!victim || c->clock < victim->clock) victim = c;
            }
            if (!victim) {                      /* every slot pinned: the window needs more than the LRU holds */
                TD5_LOG_W(LOG_TAG, "ROAM_SCN_CACHE_FULL %s_%d_%d not loaded (%d slots pinned)", k_sc_layer[layer],
                          row->ix, row->iz, k_sc_lru[layer]);
                continue;
            }
            sc_cache_load(s, victim, layer, row->ix, row->iz);
            hit = victim;
        }
        hit->clock = ++s->clock;
        hit->pin = 1;
        out[n++] = hit;
    }
    return n;
}

static void sc_cache_unpin(TD5_RoamScnSource *s)
{
    int l, k;
    for (l = 0; l < SC_LAYERS; l++)
        for (k = 0; k < SC_CACHE_LRU; k++) s->cache[l][k].pin = 0;
}

/* the records of cache slot c owned by tile key `own`: [lo, hi) in c->order */
static void sc_owned_range(const ScCache *c, int64_t own, int *lo, int *hi)
{
    int a = 0, b = c->n;
    while (a < b) { const int m = (a + b) / 2; if (c->own[c->order[m]] < own) a = m + 1; else b = m; }
    *lo = a;
    b = c->n;
    while (a < b) { const int m = (a + b) / 2; if (c->own[c->order[m]] <= own) a = m + 1; else b = m; }
    *hi = a;
}

static int sc_index_load(TD5_RoamScnSource *s)
{
    char path[512];
    char *json;
    cJSON *root, *layers;
    int l;
    td5_geo_source_path(path, sizeof path, s->place, "tiles/INDEX.JSON");
    json = sc_slurp(path, NULL);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    layers = root ? cJSON_GetObjectItem(root, "layers") : NULL;
    if (!layers) { cJSON_Delete(root); return 0; }
    for (l = 0; l < SC_LAYERS; l++) {
        const cJSON *ly = cJSON_GetObjectItem(layers, k_sc_layer[l]);
        const cJSON *tl = ly ? cJSON_GetObjectItem(ly, "tiles") : NULL, *t;
        int n = 0;
        if (!tl || !cJSON_IsArray(tl)) continue;
        s->rows[l] = (ScIdxRow *)calloc((size_t)cJSON_GetArraySize(tl) + 1u, sizeof(ScIdxRow));
        if (!s->rows[l]) continue;
        for (t = tl->child; t; t = t->next) {
            const cJSON *a = t->child;
            double v[7];
            int k;
            for (k = 0; k < 7 && a; k++, a = a->next) v[k] = a->valuedouble;
            if (k < 7) continue;
            s->rows[l][n].ix = (int)v[0]; s->rows[l][n].iz = (int)v[1];
            s->rows[l][n].x0 = v[3]; s->rows[l][n].z0 = v[4];
            s->rows[l][n].x1 = v[5]; s->rows[l][n].z1 = v[6];
            n++;
        }
        s->nrows[l] = n;
    }
    cJSON_Delete(root);
    return 1;
}

static double sc_place_upm(const char *place)
{
    char path[512];
    char *json;
    cJSON *root;
    double u = SC_UPM;
    td5_geo_source_path(path, sizeof path, place, "PLACE.JSON");
    json = sc_slurp(path, NULL);
    if (!json) return u;
    root = cJSON_Parse(json);
    free(json);
    if (root) {
        const cJSON *pr = cJSON_GetObjectItem(root, "projection");
        const cJSON *v = pr ? cJSON_GetObjectItem(pr, "units_per_metre") : NULL;
        if (v && cJSON_IsNumber(v) && v->valuedouble > 1.0) u = v->valuedouble;
        cJSON_Delete(root);
    }
    return u;
}

/* A ROAD tile of the 3x3: the caller's own, a cached one, its file, or the builder. */
static const RoamTile *sc_road(TD5_RoamScnSource *s, int tx, int tz)
{
    int k, best = -1;
    ScRoad *r;
    char path[512], err[160];
    for (k = 0; k < SC_ROAD_LRU; k++) {
        if (s->road[k].clock && s->road[k].tx == tx && s->road[k].tz == tz) {
            s->road[k].clock = ++s->clock;
            return s->road[k].absent ? NULL : s->road[k].t;
        }
    }
    for (k = 0; k < SC_ROAD_LRU; k++)
        if (best < 0 || s->road[k].clock < s->road[best].clock) best = k;
    r = &s->road[best];
    if (r->t) { td5_roam_tile_free(r->t); free(r->t); }
    memset(r, 0, sizeof *r);
    r->tx = tx; r->tz = tz; r->clock = ++s->clock;
    r->t = (RoamTile *)calloc(1, sizeof(RoamTile));
    td5_roam_tile_path(path, sizeof path, NULL, s->place, tx, tz);
    if (r->t && td5_roam_tile_read(path, r->t, 0, err, sizeof err)) return r->t;
    free(r->t);
    r->t = s->build ? s->build(s->build_u, tx, tz) : NULL;
    if (!r->t) r->absent = 1;
    return r->t;
}

/* ============================================================================
 * SECTION: tile ctx (the drivable surface + street centrelines of the 3x3)
 * ========================================================================== */

typedef struct { double x[4], z[4]; int n; double x0, z0, x1, z1; int seg; } ScPrim;
typedef struct { double ax, az, bx, bz, ay, by, hw, s0; int edge; } ScSeg;
typedef struct { uint64_t key; int klass, seg_first, seg_count; } ScEdge;

typedef struct {
    TG_Buf  blk;
    size_t *moff;
    int     nmesh, cap;
} ScSb;

struct TG_TileCtx {
    const RoamGrid *grid;
    int      tx, tz;
    int32_t  org_x, org_z;
    const RoamTile *road[9];
    int      nroad;
    ScPrim  *prim;  int nprim, cap_prim;
    ScSeg   *seg;   int nseg,  cap_seg;
    ScEdge  *edge;  int nedge, cap_edge;
    /* 16 m grid over the 3x3, tile-local */
    double   gx0, gz0;
    int      gnx, gnz;
    int     *pstart, *pitem, *sstart, *sitem;
    int     *pmark, *smark;
    int      gen;
    /* sources for the emit */
    TD5_RoamHeight *hc;
    GrRaster canopy;
    int      canopy_ok;
    double   ground_last;
    ScSb     sb[TD5_ROAM_SCN_NSB];
    /* lawn planes of every plaza of the 3x3 that reaches this tile: x/z/y per
     * corner (3 per triangle), base refinement, no road cut -- a path laid on a
     * neighbour's lawn reads the same plane the neighbour draws */
    double  *lx, *lz, *ly;
    int      nlt, cap_lt;
    TD5_RoamScnStats st;
    TG_GeoBldStats   bst;
    /* [F4c] furniture: first mesh of each sub-block that is furniture (meshes from there on are
     * exempt from the F4a census, which judges them with its own overhead-aware one), and the lanterns */
    int      furn_first[TD5_ROAM_SCN_NSB];
    float   *lamp;  uint32_t nlamp;
};

static int sc_grow(void **p, int *cap, int need, size_t elem)
{
    void *n;
    int nc;
    if (need <= *cap) return 1;
    nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    n = realloc(*p, (size_t)nc * elem);
    if (!n) return 0;
    *p = n; *cap = nc;
    return 1;
}

static void sc_ctx_reset(TG_TileCtx *c)
{
    int k;
    free(c->pstart); free(c->pitem); free(c->sstart); free(c->sitem);
    free(c->pmark); free(c->smark);
    c->pstart = c->pitem = c->sstart = c->sitem = c->pmark = c->smark = NULL;
    c->nprim = c->nseg = c->nedge = 0;
    c->nroad = 0;
    c->nlt = 0;
    free(c->lamp); c->lamp = NULL; c->nlamp = 0;
    if (c->canopy.data) free(c->canopy.data);
    memset(&c->canopy, 0, sizeof c->canopy);
    c->canopy_ok = 0;
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) { c->sb[k].blk.len = 0; c->sb[k].blk.oom = 0; c->sb[k].nmesh = 0; c->furn_first[k] = 0x7FFFFFFF; }
    memset(&c->st, 0, sizeof c->st);
    memset(&c->bst, 0, sizeof c->bst);
}

static void sc_ctx_free(TG_TileCtx *c)
{
    int k;
    if (!c) return;
    sc_ctx_reset(c);
    free(c->prim); free(c->seg); free(c->edge);
    free(c->lx); free(c->lz); free(c->ly);
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) { tg_buf_free(&c->sb[k].blk); free(c->sb[k].moff); }
    free(c);
}

static void sc_tvert(const TG_TileCtx *c, const RoamTile *t, int32_t ox, int32_t oy, int32_t oz,
                     const TD5_StripVertex *v, double *x, double *y, double *z)
{
    *x = (double)t->org_x + (double)ox + (double)v->x - (double)c->org_x;
    *y = (double)oy + (double)v->y;
    *z = (double)t->org_z + (double)oz + (double)v->z - (double)c->org_z;
}

static void sc_prim_bbox(ScPrim *p)
{
    int k;
    p->x0 = p->x1 = p->x[0]; p->z0 = p->z1 = p->z[0];
    for (k = 1; k < p->n; k++) {
        if (p->x[k] < p->x0) p->x0 = p->x[k];
        if (p->x[k] > p->x1) p->x1 = p->x[k];
        if (p->z[k] < p->z0) p->z0 = p->z[k];
        if (p->z[k] > p->z1) p->z1 = p->z[k];
    }
}

static int sc_ctx_add_tile(TG_TileCtx *c, const RoamTile *t)
{
    uint32_t e, i;
    for (e = 0; e < t->nedge; e++) {
        const RoamEdgeRec *er = &t->edge[e];
        ScEdge *ed;
        double s0 = (double)er->row0 * SC_ROW;
        if (!sc_grow((void **)&c->edge, &c->cap_edge, c->nedge + 1, sizeof(ScEdge))) return 0;
        ed = &c->edge[c->nedge];
        ed->key = er->key; ed->klass = er->klass; ed->seg_first = c->nseg; ed->seg_count = 0;
        for (i = 0; i < er->span_count; i++) {
            const uint32_t si = (uint32_t)er->span_first + i;
            const TD5_StripSpan *sp;
            uint32_t L, R;
            int n;
            double lx0, ly0, lz0, lxn, lyn, lzn, rx0, ry0, rz0, rxn, ryn, rzn;
            ScSeg *sg;
            ScPrim *pr;
            if (si >= t->nspan) break;
            sp = &t->span[si];
            L = sp->left_vertex_index; R = sp->right_vertex_index;
            n = (int)(sp->pad_02[1] & 0x0Fu);
            if (n < 1 || L + (uint32_t)n >= t->nvert || R + (uint32_t)n >= t->nvert) continue;
            sc_tvert(c, t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[L], &lx0, &ly0, &lz0);
            sc_tvert(c, t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[L + (uint32_t)n], &lxn, &lyn, &lzn);
            sc_tvert(c, t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[R], &rx0, &ry0, &rz0);
            sc_tvert(c, t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[R + (uint32_t)n], &rxn, &ryn, &rzn);
            if (!sc_grow((void **)&c->seg, &c->cap_seg, c->nseg + 1, sizeof(ScSeg))) return 0;
            if (!sc_grow((void **)&c->prim, &c->cap_prim, c->nprim + 1, sizeof(ScPrim))) return 0;
            sg = &c->seg[c->nseg];
            sg->ax = 0.5 * (lx0 + lxn); sg->az = 0.5 * (lz0 + lzn); sg->ay = 0.5 * (ly0 + lyn);
            sg->bx = 0.5 * (rx0 + rxn); sg->bz = 0.5 * (rz0 + rzn); sg->by = 0.5 * (ry0 + ryn);
            sg->hw = 0.25 * (hypot(lxn - lx0, lzn - lz0) + hypot(rxn - rx0, rzn - rz0));
            sg->s0 = s0;
            sg->edge = c->nedge;
            s0 += hypot(sg->bx - sg->ax, sg->bz - sg->az);
            pr = &c->prim[c->nprim];
            pr->n = 4;
            pr->x[0] = lx0; pr->z[0] = lz0; pr->x[1] = lxn; pr->z[1] = lzn;
            pr->x[2] = rxn; pr->z[2] = rzn; pr->x[3] = rx0; pr->z[3] = rz0;
            pr->seg = c->nseg;
            sc_prim_bbox(pr);
            c->nprim++;
            c->nseg++;
            ed->seg_count++;
        }
        c->nedge++;
    }
    for (i = 0; i < t->ntri; i++) {
        const RoamTri *tr = &t->tri[i];
        ScPrim *pr;
        int k;
        double y;
        if (tr->v[0] >= t->nvert || tr->v[1] >= t->nvert || tr->v[2] >= t->nvert) continue;
        if (!sc_grow((void **)&c->prim, &c->cap_prim, c->nprim + 1, sizeof(ScPrim))) return 0;
        pr = &c->prim[c->nprim];
        pr->n = 3;
        for (k = 0; k < 3; k++) sc_tvert(c, t, tr->ox, tr->oy, tr->oz, &t->vert[tr->v[k]], &pr->x[k], &y, &pr->z[k]);
        pr->seg = -1;
        sc_prim_bbox(pr);
        c->nprim++;
    }
    return 1;
}

static int sc_cell_of(const TG_TileCtx *c, double x, double z, int *ix, int *iz)
{
    *ix = (int)floor((x - c->gx0) / SC_CELL);
    *iz = (int)floor((z - c->gz0) / SC_CELL);
    if (*ix < 0) *ix = 0;
    if (*iz < 0) *iz = 0;
    if (*ix >= c->gnx) *ix = c->gnx - 1;
    if (*iz >= c->gnz) *iz = c->gnz - 1;
    return 1;
}

/* CSR buckets: every item whose bbox meets the cell */
static int sc_bucket(TG_TileCtx *c, int nitem, int is_seg, int **start, int **item)
{
    const int ncell = c->gnx * c->gnz;
    int *cnt = (int *)calloc((size_t)ncell + 1u, sizeof(int));
    int pass, i, total = 0;
    if (!cnt) return 0;
    *start = (int *)calloc((size_t)ncell + 1u, sizeof(int));
    if (!*start) { free(cnt); return 0; }
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < nitem; i++) {
            double x0, z0, x1, z1;
            int a0, b0, a1, b1, a, b;
            if (is_seg) {
                const ScSeg *s = &c->seg[i];
                x0 = (s->ax < s->bx ? s->ax : s->bx) - s->hw; x1 = (s->ax > s->bx ? s->ax : s->bx) + s->hw;
                z0 = (s->az < s->bz ? s->az : s->bz) - s->hw; z1 = (s->az > s->bz ? s->az : s->bz) + s->hw;
            } else {
                const ScPrim *p = &c->prim[i];
                x0 = p->x0; z0 = p->z0; x1 = p->x1; z1 = p->z1;
            }
            sc_cell_of(c, x0, z0, &a0, &b0);
            sc_cell_of(c, x1, z1, &a1, &b1);
            for (b = b0; b <= b1; b++)
                for (a = a0; a <= a1; a++) {
                    const int cell = b * c->gnx + a;
                    if (pass == 0) cnt[cell]++;
                    else (*item)[(*start)[cell] + cnt[cell]++] = i;
                }
        }
        if (pass == 0) {
            for (i = 0; i < ncell; i++) { (*start)[i] = total; total += cnt[i]; cnt[i] = 0; }
            (*start)[ncell] = total;
            *item = (int *)malloc((size_t)(total > 0 ? total : 1) * sizeof(int));
            if (!*item) { free(cnt); return 0; }
        }
    }
    free(cnt);
    return 1;
}

static int sc_ctx_build(TD5_RoamScnSource *s, TG_TileCtx *c, const RoamTile *self)
{
    int dx, dz, k;
    double S = (double)s->grid.tile, corner[4][2], x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
    sc_ctx_reset(c);
    c->grid = &s->grid;
    c->tx = self->tx; c->tz = self->tz;
    roam_grid_origin(&s->grid, self->tx, self->tz, &c->org_x, &c->org_z);
    for (dz = -1; dz <= 1; dz++)
        for (dx = -1; dx <= 1; dx++) {
            const RoamTile *t = (dx == 0 && dz == 0) ? self : sc_road(s, self->tx + dx, self->tz + dz);
            c->road[(dz + 1) * 3 + (dx + 1)] = t;
            if (t) { c->nroad++; if (!sc_ctx_add_tile(c, t)) return 0; }
        }
    /* grid over the 3x3 tiles' box (tile-local) */
    roam_grid_corners(&s->grid, self->tx - 1, self->tz - 1, corner);
    for (k = 0; k < 4; k++) {
        double cx = corner[k][0], cz = corner[k][1];
        int a, b;
        for (b = 0; b < 2; b++) for (a = 0; a < 2; a++) {
            double px, pz;
            const double u = (double)(self->tx - 1 + a * 3) * S, v = (double)(self->tz - 1 + b * 3) * S;
            px = s->grid.ox + s->grid.c * u - s->grid.s * v;
            pz = s->grid.oz + s->grid.s * u + s->grid.c * v;
            if (px < x0) x0 = px;
            if (px > x1) x1 = px;
            if (pz < z0) z0 = pz;
            if (pz > z1) z1 = pz;
        }
        (void)cx; (void)cz;
    }
    c->gx0 = x0 - (double)c->org_x - SC_CELL;
    c->gz0 = z0 - (double)c->org_z - SC_CELL;
    c->gnx = (int)ceil((x1 - x0) / SC_CELL) + 2;
    c->gnz = (int)ceil((z1 - z0) / SC_CELL) + 2;
    if (!sc_bucket(c, c->nprim, 0, &c->pstart, &c->pitem)) return 0;
    if (!sc_bucket(c, c->nseg, 1, &c->sstart, &c->sitem)) return 0;
    c->pmark = (int *)calloc((size_t)(c->nprim > 0 ? c->nprim : 1), sizeof(int));
    c->smark = (int *)calloc((size_t)(c->nseg > 0 ? c->nseg : 1), sizeof(int));
    c->gen = 0;
    c->st.road_tiles = c->nroad;
    return c->pmark && c->smark;
}

/* every prim whose bbox meets [x0,x1]x[z0,z1], once: calls fn until it returns 1 */
static int sc_prims_in(TG_TileCtx *c, double x0, double z0, double x1, double z1,
                       int (*fn)(void *u, const ScPrim *p), void *u)
{
    int a0, b0, a1, b1, a, b, k;
    c->gen++;
    sc_cell_of(c, x0, z0, &a0, &b0);
    sc_cell_of(c, x1, z1, &a1, &b1);
    for (b = b0; b <= b1; b++)
        for (a = a0; a <= a1; a++) {
            const int cell = b * c->gnx + a;
            for (k = c->pstart[cell]; k < c->pstart[cell + 1]; k++) {
                const int pi = c->pitem[k];
                const ScPrim *p = &c->prim[pi];
                if (c->pmark[pi] == c->gen) continue;
                c->pmark[pi] = c->gen;
                if (p->x1 < x0 || p->x0 > x1 || p->z1 < z0 || p->z0 > z1) continue;
                if (fn(u, p)) return 1;
            }
        }
    return 0;
}

typedef struct { const double *x, *z; int n; const ScPrim *hit; } ScPolyQ;
static int sc_poly_q(void *u, const ScPrim *p)
{
    ScPolyQ *q = (ScPolyQ *)u;
    if (!sc_poly_overlap(q->x, q->z, q->n, p->x, p->z, p->n)) return 0;
    q->hit = p;
    return 1;
}

/* 1 when polygon (x,z)[n] (tile-local; n = 2 is a segment, 1 a point) overlaps any
 * drivable primitive of the 3x3. *hit = that primitive. */
static int sc_on_road(TG_TileCtx *c, const double *x, const double *z, int n, const ScPrim **hit)
{
    ScPolyQ q;
    double x0 = x[0], x1 = x[0], z0 = z[0], z1 = z[0];
    int k;
    for (k = 1; k < n; k++) {
        if (x[k] < x0) x0 = x[k];
        if (x[k] > x1) x1 = x[k];
        if (z[k] < z0) z0 = z[k];
        if (z[k] > z1) z1 = z[k];
    }
    q.x = x; q.z = z; q.n = n; q.hit = NULL;
    if (n == 1) {
        double sx[4] = { x[0] - 1.0, x[0] + 1.0, x[0] + 1.0, x[0] - 1.0 };
        double sz[4] = { z[0] - 1.0, z[0] - 1.0, z[0] + 1.0, z[0] + 1.0 };
        q.x = sx; q.z = sz; q.n = 4;
        if (sc_prims_in(c, x0 - 1.0, z0 - 1.0, x1 + 1.0, z1 + 1.0, sc_poly_q, &q)) { if (hit) *hit = q.hit; return 1; }
        return 0;
    }
    if (sc_prims_in(c, x0, z0, x1, z1, sc_poly_q, &q)) { if (hit) *hit = q.hit; return 1; }
    return 0;
}

static int sc_square_on_road(TG_TileCtx *c, double x, double z, double h)
{
    const double sx[4] = { x - h, x + h, x + h, x - h }, sz[4] = { z - h, z - h, z + h, z + h };
    return sc_on_road(c, sx, sz, 4, NULL);
}

int roam_nearest_edge(const TG_TileCtx *cc, double x, double z, double max_d,
                      int *edge, int *side, double *along, double *lateral)
{
    TG_TileCtx *c = (TG_TileCtx *)cc;            /* the mark array only */
    int a0, b0, a1, b1, a, b, k, best = -1;
    double bd = max_d, bt = 0.0;
    c->gen++;
    sc_cell_of(c, x - max_d, z - max_d, &a0, &b0);
    sc_cell_of(c, x + max_d, z + max_d, &a1, &b1);
    for (b = b0; b <= b1; b++)
        for (a = a0; a <= a1; a++) {
            const int cell = b * c->gnx + a;
            for (k = c->sstart[cell]; k < c->sstart[cell + 1]; k++) {
                const int si = c->sitem[k];
                const ScSeg *s = &c->seg[si];
                double t, d;
                if (c->smark[si] == c->gen) continue;
                c->smark[si] = c->gen;
                d = sc_seg_dist(x, z, s->ax, s->az, s->bx, s->bz, &t);
                if (d < bd || (d == bd && best >= 0 && si < best)) { bd = d; best = si; bt = t; }
            }
        }
    if (best < 0) return 0;
    {
        const ScSeg *s = &c->seg[best];
        const double dx = s->bx - s->ax, dz = s->bz - s->az, l = hypot(dx, dz);
        double lat = 0.0;
        if (l > 1e-6) lat = ((x - s->ax) * (dz / l) - (z - s->az) * (dx / l));   /* + = the (tz, -tx) side */
        if (edge) *edge = best;                      /* segment index: the frame reads it directly */
        if (side) *side = lat >= 0.0 ? 1 : -1;
        if (along) *along = s->s0 + bt * l;
        if (lateral) *lateral = lat;
    }
    return 1;
}

int roam_edge_frame(const TG_TileCtx *c, int edge, int side, double along, RoamFrame *out)
{
    const ScSeg *s;
    const ScEdge *ed;
    double dx, dz, l, t;
    int k, si = edge;
    if (edge < 0 || edge >= c->nseg) return 0;
    ed = &c->edge[c->seg[edge].edge];
    /* walk the edge's own segments to the one holding `along` */
    for (k = ed->seg_first; k < ed->seg_first + ed->seg_count; k++) {
        const ScSeg *q = &c->seg[k];
        const double ql = hypot(q->bx - q->ax, q->bz - q->az);
        if (along >= q->s0 && along <= q->s0 + ql) { si = k; break; }
    }
    s = &c->seg[si];
    dx = s->bx - s->ax; dz = s->bz - s->az;
    l = hypot(dx, dz);
    if (!(l > 1e-6)) return 0;
    t = (along - s->s0) / l;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    memset(out, 0, sizeof *out);
    out->px = s->ax + dx * t; out->pz = s->az + dz * t; out->py = s->ay + (s->by - s->ay) * t;
    out->tx = dx / l; out->tz = dz / l;
    out->nx = out->tz * (double)side; out->nz = -out->tx * (double)side;
    out->half_carriage = s->hw;
    out->kerb_y = out->py;
    out->edge = si;
    out->key = ed->key;
    out->klass = ed->klass;
    return 1;
}

/* ---- [F4c] the ctx as the furniture emitters see it (declared in td5_roam_scenery.h) ---- */
const RoamTile *roam_ctx_tile(const TG_TileCtx *c, int dx, int dz)
{
    if (dx < -1 || dx > 1 || dz < -1 || dz > 1) return NULL;
    return c->road[(dz + 1) * 3 + (dx + 1)];
}
const RoamGrid *roam_ctx_grid(const TG_TileCtx *c) { return c->grid; }
void roam_ctx_origin(const TG_TileCtx *c, int *tx, int *tz, int32_t *org_x, int32_t *org_z)
{
    if (tx) *tx = c->tx;
    if (tz) *tz = c->tz;
    if (org_x) *org_x = c->org_x;
    if (org_z) *org_z = c->org_z;
}

/* ============================================================================
 * SECTION: sources for the emit (ground, canopy, sub-blocks)
 * ========================================================================== */

static double sc_ground(void *u, double x, double z)
{
    TG_TileCtx *c = (TG_TileCtx *)u;
    double h;
    if (c->hc && td5_roam_height_sample(c->hc, x + (double)c->org_x, z + (double)c->org_z, &h, NULL)) {
        c->ground_last = h;
        return h;
    }
    return c->ground_last;
}

/* canopy height in metres at a tile-local point, -1 = no measurement (td5_geo_canopy_m) */
static int sc_canopy_m(const TG_TileCtx *c, double x, double z)
{
    const GrRaster *r = &c->canopy;
    double fx, fz;
    unsigned v;
    if (!c->canopy_ok || !r->data || r->w <= 0 || r->h <= 0) return -1;
    fx = (x + (double)c->org_x - r->origin_x) / r->cell;
    fz = (z + (double)c->org_z - r->origin_z) / r->cell;
    if (fx < 0.0) fx = 0.0;
    if (fz < 0.0) fz = 0.0;
    if (fx > (double)(r->w - 1)) fx = (double)(r->w - 1);
    if (fz > (double)(r->h - 1)) fz = (double)(r->h - 1);
    v = ((const unsigned char *)r->data)[(size_t)(fz + 0.5) * (size_t)r->w + (size_t)(fx + 0.5)];
    if (r->nodata_raw >= 0 && r->nodata_raw <= 255 && (int)v == r->nodata_raw) return -1;
    return (int)v;
}

static int sc_sb_of(const TG_TileCtx *c, double x, double z)
{
    double u, v;
    const double q = (double)c->grid->tile / (double)TD5_ROAM_SCN_SUB;
    int a, b;
    roam_grid_uv(c->grid, x + (double)c->org_x, z + (double)c->org_z, &u, &v);
    a = (int)floor((u - (double)c->tx * (double)c->grid->tile) / q);
    b = (int)floor((v - (double)c->tz * (double)c->grid->tile) / q);
    if (a < 0) a = 0;
    if (a >= TD5_ROAM_SCN_SUB) a = TD5_ROAM_SCN_SUB - 1;
    if (b < 0) b = 0;
    if (b >= TD5_ROAM_SCN_SUB) b = TD5_ROAM_SCN_SUB - 1;
    return b * TD5_ROAM_SCN_SUB + a;
}

/* room for `more` meshes in sub-block k */
static ScSb *sc_sb(TG_TileCtx *c, int k, int more)
{
    ScSb *b = &c->sb[k];
    if (b->nmesh + more > b->cap) {
        int nc = b->cap ? b->cap : 64;
        size_t *n;
        while (nc < b->nmesh + more) nc *= 2;
        n = (size_t *)realloc(b->moff, (size_t)nc * sizeof(size_t));
        if (!n) return NULL;
        b->moff = n; b->cap = nc;
    }
    return b;
}

/* the tile-local point lies on the 3x3 the guard knows (a record owned here may reach
 * further: past it nothing is drawn, so nothing can stand on a road the ctx never saw) */
static int sc_in_reach(const TG_TileCtx *c, double x, double z)
{
    int tx, tz;
    roam_grid_tile_of(c->grid, x + (double)c->org_x, z + (double)c->org_z, &tx, &tz);
    return tx >= c->tx - 1 && tx <= c->tx + 1 && tz >= c->tz - 1 && tz <= c->tz + 1;
}

static int sc_owned_point(const TG_TileCtx *c, double x, double z)
{
    int tx, tz;
    roam_grid_tile_of(c->grid, x + (double)c->org_x, z + (double)c->org_z, &tx, &tz);
    return tx == c->tx && tz == c->tz;
}

int roam_ctx_on_road(const TG_TileCtx *c, const double *x, const double *z, int n)
{
    return sc_on_road((TG_TileCtx *)c, x, z, n, NULL);
}
int roam_ctx_owned(const TG_TileCtx *c, double x, double z) { return sc_owned_point(c, x, z); }
int roam_ctx_sb_of(const TG_TileCtx *c, double x, double z) { return sc_sb_of(c, x, z); }
double roam_ctx_ground(const TG_TileCtx *c, double x, double z) { return sc_ground((void *)c, x, z); }
void *roam_ctx_mesh_begin(TG_TileCtx *c, int sb)
{
    ScSb *b = sc_sb(c, sb, 1);
    if (!b) return NULL;
    b->moff[b->nmesh++] = b->blk.len;
    return &b->blk;
}

/* ============================================================================
 * SECTION: buildings
 * ========================================================================== */

typedef struct {
    const TD5_GeoBuilding *b;
    const double *px, *pz;     /* place-frame ring (cache pool) */
    double id;
    double storey_m;           /* its file's storey height      */
} ScBld;

typedef struct {
    const TD5_GeoArea *a;
    const double *px, *pz;
    double id;
} ScArea;

typedef struct {
    TG_TileCtx *c;
    int    host, side;
    RoamFrame f;
    double minout, gap;
} ScBldU;

/* the ground callback for the building env: its user pointer is the ScBldU */
static double sc_ground_u(void *u, double x, double z)
{
    return sc_ground(((ScBldU *)u)->c, x, z);
}

static double sc_seg_need(TG_TileCtx *c, double x, double z, double gap, double *dir_x, double *dir_z);
static int sc_bld_corner_ok(void *uv, double x, double z)
{
    ScBldU *u = (ScBldU *)uv;
    double dx, dz;
    if (sc_seg_need(u->c, x, z, u->gap, &dx, &dz) > 1.0) return 0;
    {
        const double px = x, pz = z;
        return !sc_on_road(u->c, &px, &pz, 1, NULL);
    }
}

static int sc_bld_rect_on_road(void *uv, double cx, double cz, double ux, double uz, double hx, double hz)
{
    ScBldU *u = (ScBldU *)uv;
    double x[4], z[4];
    int k;
    for (k = 0; k < 4; k++) {
        const double sx = (k == 1 || k == 2) ? 1.0 : -1.0, sz = (k >= 2) ? 1.0 : -1.0;
        x[k] = cx + ux * hx * sx + (-uz) * hz * sz;
        z[k] = cz + uz * hx * sx + ux * hz * sz;
    }
    return sc_on_road(u->c, x, z, 4, NULL);
}

static int sc_bld_apron_ok(void *uv, double x, double z)
{
    ScBldU *u = (ScBldU *)uv;
    TG_TileCtx *c = u->c;
    const double clear = SC_APRON_ROAD_M * SC_UPM;
    int a0, b0, a1, b1, a, b, k;
    if (sc_on_road(c, &x, &z, 1, NULL)) return 0;
    c->gen++;
    sc_cell_of(c, x - clear, z - clear, &a0, &b0);
    sc_cell_of(c, x + clear, z + clear, &a1, &b1);
    for (b = b0; b <= b1; b++)
        for (a = a0; a <= a1; a++) {
            const int cell = b * c->gnx + a;
            for (k = c->sstart[cell]; k < c->sstart[cell + 1]; k++) {
                const int si = c->sitem[k];
                const ScSeg *s = &c->seg[si];
                if (c->smark[si] == c->gen) continue;
                c->smark[si] = c->gen;
                if (c->edge[s->edge].klass <= TD5_GEO_RC_SERVICE) continue;
                if (sc_seg_dist(x, z, s->ax, s->az, s->bx, s->bz, NULL) < clear) return 0;
            }
        }
    return 1;
}

/* landmark cluster answers for building `self` among the halo set (td5_geo_landmarks.c rules) */
static void sc_lm_info(const ScBld *set, int n, int self, int *kind, int *anchor, int *veto)
{
    const TD5_GeoBuilding *c = set[self].b;
    const double slack = SC_LM_SLACK_M * SC_UPM;
    int k;
    *kind = TD5_GEOB_LMK_NONE; *anchor = 0; *veto = 0;
    if (!td5_env_flag_on("TD5RE_GEO_LM_CLUSTER")) return;
    if (c->landmark && !c->part && c->lmkind != TD5_GEOB_LMK_NONE && c->n >= 3) {
        *kind = c->lmkind; *anchor = 1;
        return;
    }
    for (k = 0; k < n; k++) {
        const TD5_GeoBuilding *a = set[k].b;
        int inside, j;
        double dmin = 1e300;
        if (k == self) continue;
        if (!a->landmark || a->part || a->lmkind == TD5_GEOB_LMK_NONE || a->n < 3) continue;
        if (hypot(c->cx - a->cx, c->cz - a->cz) > a->radius + slack) continue;
        inside = td5_geob_point_in_ring(set[k].px, set[k].pz, a->n, c->cx, c->cz);
        if (!inside)
            for (j = 0; j < a->n; j++) {
                const int j2 = (j + 1) % a->n;
                const double d = sc_seg_dist(c->cx, c->cz, set[k].px[j], set[k].pz[j], set[k].px[j2], set[k].pz[j2], NULL);
                if (d < dmin) dmin = d;
            }
        if (!inside && !(dmin <= slack)) continue;
        if (c->part) *kind = a->lmkind;
        else if (!c->landmark) *veto = 1;
        return;
    }
}

/* MAJOR worship landmarks of the halo set: the convex hull of the outline + every
 * building:part under it (td5_geo_landmarks.c geolm_build_forecourts, same knobs).
 * A foreign footprint within TD5RE_GEO_LM_BUILDING_CLEAR_M of one is not drawn. */
#define SC_FC_MAX     8
#define SC_FC_PTS     512
typedef struct { int n; double x[SC_FC_PTS], z[SC_FC_PTS]; } ScHull;

static double sc_hull_dist(const ScHull *h, double x, double z)
{
    double best = 1e300;
    int i, inside = 1;
    for (i = 0; i < h->n; i++) {
        const int j = (i + 1) % h->n;
        const double d = sc_seg_dist(x, z, h->x[i], h->z[i], h->x[j], h->z[j], NULL);
        if (d < best) best = d;
        if ((h->x[j] - h->x[i]) * (z - h->z[i]) - (h->z[j] - h->z[i]) * (x - h->x[i]) < 0.0) inside = 0;
    }
    return inside ? 0.0 : best;
}

static int sc_part_of(const ScBld *set, int a, int p)
{
    const TD5_GeoBuilding *an = set[a].b, *c = set[p].b;
    const double slack = SC_LM_SLACK_M * SC_UPM;
    double dmin = 1e300;
    int j;
    if (p == a || !c->part) return 0;
    if (hypot(c->cx - an->cx, c->cz - an->cz) > an->radius + slack) return 0;
    if (td5_geob_point_in_ring(set[a].px, set[a].pz, an->n, c->cx, c->cz)) return 1;
    for (j = 0; j < an->n; j++) {
        const int j2 = (j + 1) % an->n;
        const double d = sc_seg_dist(c->cx, c->cz, set[a].px[j], set[a].pz[j], set[a].px[j2], set[a].pz[j2], NULL);
        if (d < dmin) dmin = d;
    }
    return dmin <= slack;
}

typedef struct { double x, z; } ScP2;
static int sc_p2_cmp(const void *pa, const void *pb)
{
    const ScP2 *a = (const ScP2 *)pa, *b = (const ScP2 *)pb;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    return a->z < b->z ? -1 : (a->z > b->z);
}

/* convex hull of a point set (Andrew's monotone chain, CCW), any size up to SC_FC_PTS out */
static int sc_hull_of(const double *px, const double *pz, int n, ScHull *h)
{
    ScP2 *p = (ScP2 *)malloc((size_t)(n > 0 ? n : 1) * sizeof(ScP2));
    ScP2 *q = (ScP2 *)malloc((size_t)(2 * n + 2) * sizeof(ScP2));
    int i, k = 0, t;
    h->n = 0;
    if (!p || !q || n < 3) { free(p); free(q); return 0; }
    for (i = 0; i < n; i++) { p[i].x = px[i]; p[i].z = pz[i]; }
    qsort(p, (size_t)n, sizeof(ScP2), sc_p2_cmp);
    for (i = 0; i < n; i++) {
        while (k >= 2 && sc_cross(q[k - 2].x, q[k - 2].z, q[k - 1].x, q[k - 1].z, p[i].x, p[i].z) <= 0.0) k--;
        q[k++] = p[i];
    }
    for (i = n - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && sc_cross(q[k - 2].x, q[k - 2].z, q[k - 1].x, q[k - 1].z, p[i].x, p[i].z) <= 0.0) k--;
        q[k++] = p[i];
    }
    k--;
    if (k >= 3 && k <= SC_FC_PTS) {
        for (i = 0; i < k; i++) { h->x[i] = q[i].x; h->z[i] = q[i].z; }
        h->n = k;
    }
    free(p); free(q);
    return h->n >= 3;
}

static int sc_forecourts(const ScBld *set, int nset, ScHull *fc)
{
    const int min_parts = td5_env_int("TD5RE_GEO_LM_FORECOURT_PARTS", 3, 1, 64);
    double px[SC_FC_PTS * 2], pz[SC_FC_PTS * 2];
    int a, p, k, nfc = 0;
    if (!td5_env_flag_on("TD5RE_GEO_LM_CLUSTER")) return 0;
    for (a = 0; a < nset && nfc < SC_FC_MAX; a++) {
        const TD5_GeoBuilding *an = set[a].b;
        int parts = 0, np = 0;
        if (!an->landmark || an->part || an->lmkind != TD5_GEOB_LMK_WORSHIP || an->n < 3) continue;
        for (p = 0; p < nset; p++) parts += sc_part_of(set, a, p);
        if (parts < min_parts) continue;
        for (k = 0; k < an->n && np < SC_FC_PTS * 2; k++) { px[np] = set[a].px[k]; pz[np] = set[a].pz[k]; np++; }
        for (p = 0; p < nset; p++) {
            if (!sc_part_of(set, a, p)) continue;
            for (k = 0; k < set[p].b->n && np < SC_FC_PTS * 2; k++) { px[np] = set[p].px[k]; pz[np] = set[p].pz[k]; np++; }
        }
        if (sc_hull_of(px, pz, np, &fc[nfc])) nfc++;
    }
    return nfc;
}

static int sc_in_plaza(const ScArea *ar, int na, double x, double z)
{
    int k;
    for (k = 0; k < na; k++) {
        const TD5_GeoArea *a = ar[k].a;
        if (!td5_geob_area_is_plaza(a)) continue;
        if (hypot(x - a->cx, z - a->cz) > a->radius) continue;
        if (td5_geob_point_in_ring(ar[k].px, ar[k].pz, a->n, x, z)) return 1;
    }
    return 0;
}

static double sc_ring_area_m2(const double *x, const double *z, int n)
{
    return fabs(td5_geob_ring_area(x, z, n)) / (SC_UPM * SC_UPM);
}

/* clear ring (tile-local, in place) of the drivable surface: the host street's
 * carriageway + pavement first (the strip's nudge), then any primitive it still
 * overlaps. 0 = keep, 1 = drop (on a road), 2 = drop (past the shift cap). */
/* How far (x, z) must move to stand kerb + gap clear of every street centreline
 * segment near it, and which way (unit, away from the closest point of the worst
 * one). 0 when it is clear. */
static double sc_seg_need(TG_TileCtx *c, double x, double z, double gap, double *dir_x, double *dir_z)
{
    const double r = 15000.0 + gap;
    int a0, b0, a1, b1, a, b, k;
    double worst = 0.0;
    c->gen++;
    sc_cell_of(c, x - r, z - r, &a0, &b0);
    sc_cell_of(c, x + r, z + r, &a1, &b1);
    for (b = b0; b <= b1; b++)
        for (a = a0; a <= a1; a++) {
            const int cell = b * c->gnx + a;
            for (k = c->sstart[cell]; k < c->sstart[cell + 1]; k++) {
                const int si = c->sitem[k];
                const ScSeg *s = &c->seg[si];
                double t, d, need;
                if (c->smark[si] == c->gen) continue;
                c->smark[si] = c->gen;
                d = sc_seg_dist(x, z, s->ax, s->az, s->bx, s->bz, &t);
                need = s->hw + gap - d;
                if (need > worst) {
                    const double qx = s->ax + (s->bx - s->ax) * t, qz = s->az + (s->bz - s->az) * t;
                    double ux = x - qx, uz = z - qz, l = hypot(ux, uz);
                    if (!(l > 1e-6)) {               /* on the centreline: across the segment */
                        const double sl = hypot(s->bx - s->ax, s->bz - s->az);
                        if (!(sl > 1e-6)) continue;
                        ux = (s->bz - s->az) / sl; uz = -(s->bx - s->ax) / sl; l = 1.0;
                    }
                    worst = need; *dir_x = ux / l; *dir_z = uz / l;
                }
            }
        }
    return worst;
}

static int sc_bld_clear(TG_TileCtx *c, ScBldU *u, double gap, double *rx, double *rz, int n,
                        double *shift_out)
{
    double shift = 0.0, total = 0.0;
    int k, it;
    const ScPrim *hit = NULL;
    *shift_out = 0.0;
    (void)u;
    /* the strip clears a footprint against its HOST span's straight frame; a roam
     * footprint has no host, so every vertex is cleared against the street segments
     * actually near it (kerb + gap from the nearest centreline), deepest first */
    for (it = 0; it < 6; it++) {
        double worst = 0.0, wx = 0.0, wz = 0.0;
        for (k = 0; k < n; k++) {
            double dx, dz;
            const double need = sc_seg_need(c, rx[k], rz[k], gap, &dx, &dz);
            if (need > worst) { worst = need; wx = dx; wz = dz; }
        }
        if (!(worst > 1.0)) break;
        if (total + worst > SC_MAX_SHIFT) return 2;
        for (k = 0; k < n; k++) { rx[k] += wx * worst; rz[k] += wz * worst; }
        total += worst;
    }
    (void)shift;
    for (it = 0; it < 3; it++) {
        double cx = 0.0, cz = 0.0, push = 0.0, nx, nz;
        if (!sc_on_road(c, rx, rz, n, &hit)) { *shift_out = total; return 0; }
        for (k = 0; k < n; k++) { cx += rx[k]; cz += rz[k]; }
        cx /= n; cz /= n;
        if (hit->seg >= 0) {
            /* away from that ribbon's centreline, on the ring's side, to its kerb + gap */
            const ScSeg *s = &c->seg[hit->seg];
            const double dx = s->bx - s->ax, dz = s->bz - s->az, l = hypot(dx, dz);
            double side;
            if (!(l > 1e-6)) return 1;
            nx = dz / l; nz = -dx / l;
            side = ((cx - s->ax) * nx + (cz - s->az) * nz) >= 0.0 ? 1.0 : -1.0;
            nx *= side; nz *= side;
            for (k = 0; k < n; k++) {
                const double o = (rx[k] - s->ax) * nx + (rz[k] - s->az) * nz;
                const double need = s->hw + gap - o;
                if (need > push) push = need;
            }
        } else {
            /* a junction patch: straight out from the patch centre */
            double pcx = (hit->x[0] + hit->x[1] + hit->x[2]) / 3.0, pcz = (hit->z[0] + hit->z[1] + hit->z[2]) / 3.0;
            double l;
            nx = cx - pcx; nz = cz - pcz;
            l = hypot(nx, nz);
            if (!(l > 1e-6)) return 1;
            nx /= l; nz /= l;
            push = 1500.0;
        }
        if (!(push > 0.0) || total + push > SC_MAX_SHIFT) return 1;
        for (k = 0; k < n; k++) { rx[k] += nx * push; rz[k] += nz * push; }
        total += push;
    }
    if (sc_on_road(c, rx, rz, n, NULL)) return 1;
    *shift_out = total;
    return 0;
}

static int sc_emit_buildings(TD5_RoamScnSource *s, TG_TileCtx *c, ScCache **bc, int nbc,
                             const ScArea *ar, int nar)
{
    ScBld *set = NULL;
    ScHull *fc = NULL;
    int nset = 0, cap = 0, i, k, l, dx, dz, nfc = 0;
    const int64_t self = sc_tkey(c->tx, c->tz);
    /* the halo set: every building owned by the 3x3 (landmark anchors may sit next door) */
    for (dz = -1; dz <= 1; dz++)
        for (dx = -1; dx <= 1; dx++) {
            const int64_t key = sc_tkey(c->tx + dx, c->tz + dz);
            for (l = 0; l < nbc; l++) {
                int lo, hi, j;
                sc_owned_range(bc[l], key, &lo, &hi);
                for (j = lo; j < hi; j++) {
                    const int ri = bc[l]->order[j];
                    if (!sc_grow((void **)&set, &cap, nset + 1, sizeof(ScBld))) { free(set); return 0; }
                    set[nset].b = &bc[l]->b[ri];
                    set[nset].px = &bc[l]->px[bc[l]->b[ri].first];
                    set[nset].pz = &bc[l]->pz[bc[l]->b[ri].first];
                    set[nset].id = bc[l]->id[ri];
                    set[nset].storey_m = bc[l]->storey_m;
                    nset++;
                }
            }
        }
    /* sort the whole set by (id, cx, cz): the halo answers and the emit order are
     * then independent of which cache slot a record came from */
    for (i = 1; i < nset; i++) {
        ScBld t = set[i];
        for (k = i - 1; k >= 0; k--) {
            const ScBld *q = &set[k];
            int gt = q->id > t.id || (q->id == t.id && (q->b->cx > t.b->cx || (q->b->cx == t.b->cx && q->b->cz > t.b->cz)));
            if (!gt) break;
            set[k + 1] = set[k];
        }
        set[k + 1] = t;
    }
    fc = (ScHull *)malloc(SC_FC_MAX * sizeof(ScHull));
    nfc = fc ? sc_forecourts(set, nset, fc) : 0;
    for (i = 0; i < nset; i++) {
        const TD5_GeoBuilding *gb = set[i].b;
        double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], cx = 0.0, cz = 0.0, shift = 0.0;
        int tx, tz, n = gb->n, kind, anchor, veto, edge = -1, side = 1, r, sb;
        double along = 0.0, lat = 0.0;
        ScBldU u;
        TG_GeoBldEnv e;
        ScSb *b;
        roam_grid_tile_of(c->grid, gb->cx, gb->cz, &tx, &tz);
        if (sc_tkey(tx, tz) != self) continue;
        c->st.bld_seen++;
        if (n < 3 || n > TD5_GEOB_RING_MAX) { c->st.bld_drop_ring++; continue; }
        /* the plaza veto: most of the outline in a plaza, not a landmark, not a small OSM building */
        if (td5_env_flag_on("TD5RE_GEO_PLAZA_BLD") && nar > 0) {
            int in = 0, np = 0;
            for (k = -1; k < n; k++) {
                np++;
                in += sc_in_plaza(ar, nar, k < 0 ? gb->cx : set[i].px[k], k < 0 ? gb->cz : set[i].pz[k]);
            }
            if (in * 2 > np && !gb->landmark
                && !(gb->fsrc == TD5_GEOB_FSRC_OSM && sc_ring_area_m2(set[i].px, set[i].pz, n) < SC_PLAZA_KEEP_M2)) {
                c->st.bld_veto_plaza++;
                continue;
            }
        }
        sc_lm_info(set, nset, i, &kind, &anchor, &veto);
        if (!veto && nfc > 0 && !gb->landmark && !gb->part && kind == TD5_GEOB_LMK_NONE) {
            /* a foreign footprint on a major landmark's forecourt (geolm_build_forecourts) */
            const double clear = (double)td5_env_float("TD5RE_GEO_LM_BUILDING_CLEAR_M", 12.0f, 0.0f, 100.0f) * SC_UPM;
            int f;
            for (k = 0; k < n && !veto; k++)
                for (f = 0; f < nfc && !veto; f++)
                    if (sc_hull_dist(&fc[f], set[i].px[k], set[i].pz[k]) <= clear) veto = 1;
        }
        if (veto) { c->st.bld_veto_lm++; continue; }
        for (k = 0; k < n; k++) { rx[k] = set[i].px[k] - (double)c->org_x; rz[k] = set[i].pz[k] - (double)c->org_z; }
        if (!td5_geob_ring_simple(rx, rz, n)) { c->st.bld_drop_ring++; continue; }
        for (k = 0; k < n && sc_in_reach(c, rx[k], rz[k]); k++) { }
        if (k < n) { c->st.bld_drop_ring++; continue; }
        for (k = 0; k < n; k++) { cx += rx[k]; cz += rz[k]; }
        cx /= n; cz /= n;
        memset(&u, 0, sizeof u);
        u.c = c;
        u.host = roam_nearest_edge(c, cx, cz, SC_HOST_MAX_M * SC_UPM, &edge, &side, &along, &lat)
              && roam_edge_frame(c, edge, side, along, &u.f);
        u.side = side;
        u.minout = u.host ? u.f.half_carriage + s->gap : 0.0;
        u.gap = s->gap;
        r = sc_bld_clear(c, &u, s->gap, rx, rz, n, &shift);
        if (gb->landmark && !gb->part)       /* one line per landmark outline: what became of it */
            TD5_LOG_I(LOG_TAG, "[ROAM LM] tile %d,%d way_hash=%08x kind=%d anchor=%d host=%d half=%.0f minout=%.0f "
                      "area=%.0fm2 h=%.0f: %s (shift %.0f)", c->tx, c->tz, gb->id_hash, kind, anchor, u.host,
                      u.f.half_carriage, u.minout, gb->area_m2, gb->height,
                      r == 2 ? "DROPPED past the shift cap" : r == 1 ? "DROPPED on a road" : "kept", shift);
        if (r == 2) { c->st.bld_drop_shift++; continue; }
        if (r == 1) { c->st.bld_drop_road++; continue; }
        if (shift > 0.0) { c->st.bld_nudged++; if (shift > c->st.bld_shift_max) c->st.bld_shift_max = shift; }
        cx = 0.0; cz = 0.0;
        for (k = 0; k < n; k++) { cx += rx[k]; cz += rz[k]; }
        cx /= n; cz /= n;
        sb = sc_sb_of(c, cx, cz);
        b = sc_sb(c, sb, 4);
        if (!b) { free(set); free(fc); return 0; }
        memset(&e, 0, sizeof e);
        e.blk = &b->blk; e.moff = b->moff; e.nmesh = &b->nmesh; e.maxmesh = b->cap;
        e.b = &k_biomes[0];                         /* CITY: the geo city biome */
        e.ground = sc_ground_u; e.u = &u;
        e.have_ceiling = u.host;
        e.ceiling_y = u.f.py + tg_city_kerb_h(e.b) + 400.0;
        e.have_pf_ceiling = u.host;
        e.pf_ceiling_y = e.ceiling_y;
        e.upm = s->upm;
        e.storey = set[i].storey_m * s->upm;      /* td5_geob_storey_units() */
        e.lm_kind = kind;
        e.lm_anchor = anchor;
        e.corner_ok = sc_bld_corner_ok;
        e.rect_on_road = sc_bld_rect_on_road;
        e.apron_ok = sc_bld_apron_ok;
        e.si = -1;
        e.acct = 0;
        e.st = &c->bst;
        {
            const long emitted0 = c->bst.emitted, prefab0 = c->bst.lm_prefab, apron0 = c->bst.lm_apron;
            if (!tg_geo_emit_building_core(&e, gb, rx, rz, n, cx, cz, shift) || b->blk.oom) {
                free(set); free(fc); return 0;
            }
            if (c->bst.emitted > emitted0) c->st.bld_emitted++;
            if (c->bst.lm_prefab > prefab0) c->st.bld_prefab++;
            if (c->bst.lm_apron > apron0) c->st.bld_apron++;
            if (kind != TD5_GEOB_LMK_NONE || gb->landmark) c->st.bld_lm++;
        }
    }
    free(set);
    free(fc);
    return 1;
}


/* ============================================================================
 * SECTION: plazas (lawns + trees)
 * ========================================================================== */

typedef struct { double *x, *z; int n, cap; } ScTris;   /* 3 points per triangle */

static int sc_tri_push(ScTris *t, const double *p)
{
    int k;
    if (t->n * 3 + 3 > t->cap) {
        const int nc = t->cap ? t->cap * 2 : 768;
        double *nx = (double *)realloc(t->x, (size_t)nc * sizeof(double)), *nz;
        if (!nx) return 0;
        t->x = nx;
        nz = (double *)realloc(t->z, (size_t)nc * sizeof(double));
        if (!nz) return 0;
        t->z = nz;
        t->cap = nc;
    }
    for (k = 0; k < 3; k++) { t->x[t->n * 3 + k] = p[k * 2]; t->z[t->n * 3 + k] = p[k * 2 + 1]; }
    t->n++;
    return 1;
}

/* red refinement to `lvl`, then: a sub-triangle on the road is refined `extra`
 * more times and only its road-free pieces kept */
static int sc_tri_split(TG_TileCtx *c, ScTris *out, const double *p, int lvl, int extra)
{
    if (lvl <= 0) {
        const double x[3] = { p[0], p[2], p[4] }, z[3] = { p[1], p[3], p[5] };
        if (!sc_in_reach(c, x[0], z[0]) || !sc_in_reach(c, x[1], z[1]) || !sc_in_reach(c, x[2], z[2])) {
            c->st.lawn_cut++;
            return 1;
        }
        if (!sc_on_road(c, x, z, 3, NULL)) return sc_tri_push(out, p);
        if (extra <= 0) { c->st.lawn_cut++; return 1; }
        lvl = 1; extra--;
    }
    {
        const double ab[2] = { 0.5 * (p[0] + p[2]), 0.5 * (p[1] + p[3]) };
        const double bc[2] = { 0.5 * (p[2] + p[4]), 0.5 * (p[3] + p[5]) };
        const double ca[2] = { 0.5 * (p[4] + p[0]), 0.5 * (p[5] + p[1]) };
        const double t0[6] = { p[0], p[1], ab[0], ab[1], ca[0], ca[1] };
        const double t1[6] = { ab[0], ab[1], p[2], p[3], bc[0], bc[1] };
        const double t2[6] = { ca[0], ca[1], bc[0], bc[1], p[4], p[5] };
        const double t3[6] = { ab[0], ab[1], bc[0], bc[1], ca[0], ca[1] };
        return sc_tri_split(c, out, t0, lvl - 1, extra) && sc_tri_split(c, out, t1, lvl - 1, extra)
            && sc_tri_split(c, out, t2, lvl - 1, extra) && sc_tri_split(c, out, t3, lvl - 1, extra);
    }
}

/* the strip's refinement level for an outline (td5_tg_streets.c tg_geop_lawn_build) */
static int sc_lawn_level(const double *rx, const double *rz, const int *tri, int ntri)
{
    double maxe = 0.0;
    int k, lvl = 0;
    for (k = 0; k < ntri; k++) {
        const int i0 = tri[k * 3], i1 = tri[k * 3 + 1], i2 = tri[k * 3 + 2];
        const double e0 = hypot(rx[i1] - rx[i0], rz[i1] - rz[i0]);
        const double e1 = hypot(rx[i2] - rx[i1], rz[i2] - rz[i1]);
        const double e2 = hypot(rx[i0] - rx[i2], rz[i0] - rz[i2]);
        if (e0 > maxe) maxe = e0;
        if (e1 > maxe) maxe = e1;
        if (e2 > maxe) maxe = e2;
    }
    while (lvl < 7 && maxe / (double)(1 << lvl) > SC_LAWN_STEP) lvl++;
    while (lvl > 0 && (long)ntri * (1L << (2 * lvl)) > SC_LAWN_TRI_MAX) lvl--;
    return lvl;
}

static int sc_plane_push(TG_TileCtx *c, const double *p, int lvl)
{
    if (lvl <= 0) {
        int k;
        if (c->nlt * 3 + 3 > c->cap_lt) {
            const int nc = c->cap_lt ? c->cap_lt * 2 : 3072;
            double *a = (double *)realloc(c->lx, (size_t)nc * sizeof(double));
            if (!a) return 0;
            c->lx = a;
            a = (double *)realloc(c->lz, (size_t)nc * sizeof(double));
            if (!a) return 0;
            c->lz = a;
            a = (double *)realloc(c->ly, (size_t)nc * sizeof(double));
            if (!a) return 0;
            c->ly = a;
            c->cap_lt = nc;
        }
        for (k = 0; k < 3; k++) {
            c->lx[c->nlt * 3 + k] = p[k * 2];
            c->lz[c->nlt * 3 + k] = p[k * 2 + 1];
            c->ly[c->nlt * 3 + k] = sc_ground(c, p[k * 2], p[k * 2 + 1]) + SC_GEOP_LIFT;
        }
        c->nlt++;
        return 1;
    }
    {
        const double ab[2] = { 0.5 * (p[0] + p[2]), 0.5 * (p[1] + p[3]) };
        const double bc[2] = { 0.5 * (p[2] + p[4]), 0.5 * (p[3] + p[5]) };
        const double ca[2] = { 0.5 * (p[4] + p[0]), 0.5 * (p[5] + p[1]) };
        const double t0[6] = { p[0], p[1], ab[0], ab[1], ca[0], ca[1] };
        const double t1[6] = { ab[0], ab[1], p[2], p[3], bc[0], bc[1] };
        const double t2[6] = { ca[0], ca[1], bc[0], bc[1], p[4], p[5] };
        const double t3[6] = { ab[0], ab[1], bc[0], bc[1], ca[0], ca[1] };
        return sc_plane_push(c, t0, lvl - 1) && sc_plane_push(c, t1, lvl - 1)
            && sc_plane_push(c, t2, lvl - 1) && sc_plane_push(c, t3, lvl - 1);
    }
}

/* lawn planes of the plazas of the 3x3 whose box meets the tile box (tile-local) */
static int sc_lawn_planes(TG_TileCtx *c, const ScArea *ar, int nar, double x0, double z0, double x1, double z1)
{
    int i, k;
    for (i = 0; i < nar; i++) {
        const TD5_GeoArea *a = ar[i].a;
        double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX];
        int tri[(TD5_GEOB_RING_MAX - 2) * 3], ntri, lvl;
        if (!td5_geob_area_is_plaza(a) || a->n < 3 || a->n > TD5_GEOB_RING_MAX || a->radius < SC_GEOP_MIN_R) continue;
        if (a->cx - (double)c->org_x + a->radius < x0 || a->cx - (double)c->org_x - a->radius > x1 ||
            a->cz - (double)c->org_z + a->radius < z0 || a->cz - (double)c->org_z - a->radius > z1) continue;
        for (k = 0; k < a->n; k++) { rx[k] = ar[i].px[k] - (double)c->org_x; rz[k] = ar[i].pz[k] - (double)c->org_z; }
        ntri = td5_geob_triangulate(rx, rz, a->n, tri, TD5_GEOB_RING_MAX - 2);
        if (ntri <= 0) continue;
        lvl = sc_lawn_level(rx, rz, tri, ntri);
        for (k = 0; k < ntri; k++) {
            const int i0 = tri[k * 3], i1 = tri[k * 3 + 1], i2 = tri[k * 3 + 2];
            const double pp[6] = { rx[i0], rz[i0], rx[i1], rz[i1], rx[i2], rz[i2] };
            if (!sc_plane_push(c, pp, lvl)) return 0;
        }
    }
    return 1;
}

/* the highest lawn plane over (x, z); 0 when no lawn covers it */
static int sc_lawn_y(const TG_TileCtx *c, double x, double z, double *y)
{
    int t, hit = 0;
    for (t = 0; t < c->nlt; t++) {
        const double *X = &c->lx[t * 3], *Z = &c->lz[t * 3], *Y = &c->ly[t * 3];
        const double d = (Z[1] - Z[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Z[0] - Z[2]);
        double w0, w1, w2, h;
        if (x < X[0] && x < X[1] && x < X[2]) continue;
        if (x > X[0] && x > X[1] && x > X[2]) continue;
        if (z < Z[0] && z < Z[1] && z < Z[2]) continue;
        if (z > Z[0] && z > Z[1] && z > Z[2]) continue;
        if (fabs(d) < 1e-9) continue;
        w0 = ((Z[1] - Z[2]) * (x - X[2]) + (X[2] - X[1]) * (z - Z[2])) / d;
        w1 = ((Z[2] - Z[0]) * (x - X[2]) + (X[0] - X[2]) * (z - Z[2])) / d;
        w2 = 1.0 - w0 - w1;
        if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
        h = w0 * Y[0] + w1 * Y[1] + w2 * Y[2];
        if (!hit || h > *y) *y = h;
        hit = 1;
    }
    return hit;
}

static int sc_emit_lawn(TG_TileCtx *c, const TD5_GeoArea *a, const double *rx, const double *rz, int n, int sb)
{
    int tri[(TD5_GEOB_RING_MAX - 2) * 3];
    int ntri, k, lvl = 0, t0 = 0;
    double maxe = 0.0;
    const double inv_tile = td5_env_flag_on("TD5RE_GEO_PLAZA_TILE") ? (1.0 / (double)TD5_TG_SPAN_LENGTH) : (1.0 / 3400.0);
    ScTris tr;
    float v[SC_LAWN_CHUNK * 3 * 5];
    unsigned int light[SC_LAWN_CHUNK * 3];
    (void)a;
    memset(&tr, 0, sizeof tr);
    ntri = td5_geob_triangulate(rx, rz, n, tri, TD5_GEOB_RING_MAX - 2);
    if (ntri <= 0) return 1;
    for (k = 0; k < ntri; k++) {
        const int i0 = tri[k * 3], i1 = tri[k * 3 + 1], i2 = tri[k * 3 + 2];
        const double e0 = hypot(rx[i1] - rx[i0], rz[i1] - rz[i0]);
        const double e1 = hypot(rx[i2] - rx[i1], rz[i2] - rz[i1]);
        const double e2 = hypot(rx[i0] - rx[i2], rz[i0] - rz[i2]);
        if (e0 > maxe) maxe = e0;
        if (e1 > maxe) maxe = e1;
        if (e2 > maxe) maxe = e2;
    }
    while (lvl < 7 && maxe / (double)(1 << lvl) > SC_LAWN_STEP) lvl++;
    while (lvl > 0 && (long)ntri * (1L << (2 * lvl)) > SC_LAWN_TRI_MAX) lvl--;
    for (k = 0; k < ntri; k++) {
        const int i0 = tri[k * 3], i1 = tri[k * 3 + 1], i2 = tri[k * 3 + 2];
        const double p[6] = { rx[i0], rz[i0], rx[i1], rz[i1], rx[i2], rz[i2] };
        if (!sc_tri_split(c, &tr, p, lvl, 2)) { free(tr.x); free(tr.z); return 0; }
    }
    while (t0 < tr.n) {
        const int nt = (tr.n - t0 < SC_LAWN_CHUNK) ? tr.n - t0 : SC_LAWN_CHUNK;
        unsigned short cmd[3];
        ScSb *b = sc_sb(c, sb, 1);
        int i;
        if (!b) { free(tr.x); free(tr.z); return 0; }
        for (i = 0; i < nt * 3; i++) {
            const int q = t0 * 3 + i;
            v[i * 5 + 0] = (float)tr.x[q];
            v[i * 5 + 1] = (float)(sc_ground(c, tr.x[q], tr.z[q]) + SC_GEOP_LIFT);
            v[i * 5 + 2] = (float)tr.z[q];
            v[i * 5 + 3] = (float)(tr.x[q] * inv_tile);
            v[i * 5 + 4] = (float)(tr.z[q] * inv_tile);
            light[i] = 0xFFFFFFFFu;
        }
        cmd[0] = (unsigned short)(TD5_TG_PAGE_R3_BLOCK + 0);
        cmd[1] = (unsigned short)nt;
        cmd[2] = 0;
        b->moff[b->nmesh++] = b->blk.len;
        if (!tg_write_prefab_mesh(&b->blk, v, light, nt * 3, cmd, 1, 0, 0.0, 0.0, 0.0, 1.0, 0.0)) {
            free(tr.x); free(tr.z); return 0;
        }
        c->st.lawn_tris += nt;
        t0 += nt;
    }
    free(tr.x); free(tr.z);
    return 1;
}

static int sc_emit_trees(TG_TileCtx *c, const TD5_GeoArea *a, const double *rx, const double *rz, int n)
{
    const int tv = (int)(a->id_hash % (unsigned)TD5_TG_TREE_VARIANTS);
    const TG_TreePage *tp = &k_tree_pages[tv];
    const double tw = (double)tp->w, th = (double)tp->h;
    const int always = (a->kind == TD5_GEOA_KIND_PARK || a->kind == TD5_GEOA_KIND_PLAY);
    double x0 = rx[0], x1 = rx[0], z0 = rz[0], z1 = rz[0], gx, gz;
    int k, pass, cand = 0, seen = 0, planted = 0;
    const int cap = SC_TREES_BIG;
    for (k = 1; k < n; k++) {
        if (rx[k] < x0) x0 = rx[k];
        if (rx[k] > x1) x1 = rx[k];
        if (rz[k] < z0) z0 = rz[k];
        if (rz[k] > z1) z1 = rz[k];
    }
    /* the WORLD lattice (place frame), so a park's trees do not move with the tile origin */
    {
        const double ox = (double)c->org_x, oz = (double)c->org_z;
        const double lx0 = floor((x0 + ox) / SC_TREE_STEP) * SC_TREE_STEP - ox;
        const double lz0 = floor((z0 + oz) / SC_TREE_STEP) * SC_TREE_STEP - oz;
        for (pass = 0; pass < 2; pass++) {
            for (gz = lz0; gz <= z1 && planted < cap; gz += SC_TREE_STEP) {
                for (gx = lx0; gx <= x1 && planted < cap; gx += SC_TREE_STEP) {
                    int ok;
                    if (!td5_geob_point_in_ring(rx, rz, n, gx, gz)) continue;
                    if (!sc_in_reach(c, gx, gz)) continue;
                    if (sc_canopy_m(c, gx, gz) >= 0) {
                        int ddx, ddz, best = 0;
                        for (ddz = -1; ddz <= 1; ddz++)
                            for (ddx = -1; ddx <= 1; ddx++) {
                                const int cm = sc_canopy_m(c, gx + ddx * SC_TREE_STEP * 0.33, gz + ddz * SC_TREE_STEP * 0.33);
                                if (cm > best) best = cm;
                            }
                        if (best < SC_CANOPY_M) continue;
                    } else if (!always) continue;
                    ok = !sc_square_on_road(c, gx, gz, SC_TREE_CLEAR);
                    if (!ok) { if (pass == 1) c->st.trees_cut++; continue; }
                    if (pass == 0) { cand++; continue; }
                    if (cand > cap) {
                        const int keep = (int)(((long)(seen + 1) * cap) / cand) > (int)(((long)seen * cap) / cand);
                        seen++;
                        if (!keep) continue;
                    }
                    {
                        ScSb *b = sc_sb(c, sc_sb_of(c, gx, gz), 1);
                        if (!b) return 0;
                        b->moff[b->nmesh++] = b->blk.len;
                        if (!tg_emit_billboard_mesh(&b->blk, gx, sc_ground(c, gx, gz) + SC_GEOP_LIFT, gz,
                                                    tw * 0.5, th, tg_tree_slot(tv), 1))
                            return 0;
                    }
                    planted++;
                    c->st.trees++;
                }
            }
        }
    }
    return 1;
}

static int sc_emit_plazas(TG_TileCtx *c, const ScArea *ar, int nar)
{
    int i, k;
    if (!td5_env_flag_on("TD5RE_GEO_PLAZAS")) return 1;
    for (i = 0; i < nar; i++) {
        const TD5_GeoArea *a = ar[i].a;
        double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], cx = 0.0, cz = 0.0;
        const int n = a->n;
        if (!td5_geob_area_is_plaza(a)) continue;
        if (n < 3 || n > TD5_GEOB_RING_MAX || a->radius < SC_GEOP_MIN_R) continue;
        if (!sc_owned_point(c, a->cx - (double)c->org_x, a->cz - (double)c->org_z)) continue;
        for (k = 0; k < n; k++) { rx[k] = ar[i].px[k] - (double)c->org_x; rz[k] = ar[i].pz[k] - (double)c->org_z; cx += rx[k]; cz += rz[k]; }
        cx /= n; cz /= n;
        c->st.areas++;
        if (!sc_emit_lawn(c, a, rx, rz, n, sc_sb_of(c, cx, cz))) return 0;
        if (!sc_emit_trees(c, a, rx, rz, n)) return 0;
    }
    return 1;
}

/* ============================================================================
 * SECTION: footpaths
 * ========================================================================== */

typedef struct { int page; int nq; float v[SC_FP_CHUNK * 4 * 5]; unsigned int light[SC_FP_CHUNK * 4]; } ScFpBatch;

static int sc_fp_flush(TG_TileCtx *c, int sb, ScFpBatch *q)
{
    unsigned short cmd[3];
    ScSb *b;
    if (q->nq <= 0) return 1;
    b = sc_sb(c, sb, 1);
    if (!b) return 0;
    cmd[0] = (unsigned short)q->page; cmd[1] = 0; cmd[2] = (unsigned short)q->nq;
    b->moff[b->nmesh++] = b->blk.len;
    if (!tg_write_prefab_mesh(&b->blk, q->v, q->light, q->nq * 4, cmd, 1, 0, 0.0, 0.0, 0.0, 1.0, 0.0)) return 0;
    q->nq = 0;
    return 1;
}

typedef struct { const ScFoot *f; const double *px, *pz; } ScFw;

static int sc_emit_footpaths(TD5_RoamScnSource *s, TG_TileCtx *c, ScCache **fc, int nfc,
                             double tx0, double tz0, double tx1, double tz1)
{
    ScFw *list = NULL;
    int nl = 0, cap = 0, i, l, k, sb;
    ScFpBatch *bat;
    if (!td5_env_flag_on("TD5RE_GEO_FOOTPATHS")) return 1;
    for (l = 0; l < nfc; l++)
        for (i = 0; i < fc[l]->n; i++) {
            const ScFoot *f = &fc[l]->f[i];
            /* the record bbox is PLACE frame, the window tile-local */
            if (f->maxx - (double)c->org_x < tx0 || f->minx - (double)c->org_x > tx1 ||
                f->maxz - (double)c->org_z < tz0 || f->minz - (double)c->org_z > tz1) continue;
            if (!sc_grow((void **)&list, &cap, nl + 1, sizeof(ScFw))) { free(list); return 0; }
            list[nl].f = f; list[nl].px = &fc[l]->px[f->first]; list[nl].pz = &fc[l]->pz[f->first];
            nl++;
        }
    /* order by OSM id (then first point): independent of the cache slots */
    for (i = 1; i < nl; i++) {
        ScFw t = list[i];
        for (k = i - 1; k >= 0; k--) {
            const ScFw *q = &list[k];
            if (!(q->f->id > t.f->id || (q->f->id == t.f->id && (q->px[0] > t.px[0] || (q->px[0] == t.px[0] && q->pz[0] > t.pz[0])))))
                break;
            list[k + 1] = list[k];
        }
        list[k + 1] = t;
    }
    bat = (ScFpBatch *)calloc((size_t)TD5_ROAM_SCN_NSB * 2u, sizeof(ScFpBatch));
    if (!bat) { free(list); return 0; }
    for (sb = 0; sb < TD5_ROAM_SCN_NSB; sb++) {
        bat[sb * 2 + 0].page = TD5_TG_PAGE_SIDEWALK;
        bat[sb * 2 + 1].page = k_biomes[0].ground_page;
    }
    for (i = 0; i < nl; i++) {
        const ScFoot *f = list[i].f;
        double half, s_along = 0.0;
        int used = 0;
        if (f->kind < 0 || f->kind >= TD5_GEO_FW_KINDS || !(SC_FP_KIND_MASK & (1u << (unsigned)f->kind))) continue;
        if (f->structure) { c->st.fp_struct++; continue; }
        half = (f->width_m > 0.0) ? f->width_m * SC_UPM * 0.5 : SC_FP_DEF_W * 0.5;
        if (half < SC_FP_MIN_W * 0.5) half = SC_FP_MIN_W * 0.5;
        if (half > SC_FP_MAX_W * 0.5) half = SC_FP_MAX_W * 0.5;
        for (k = 0; k + 1 < f->count; k++) {
            const double ax = list[i].px[k] - (double)c->org_x, az = list[i].pz[k] - (double)c->org_z;
            const double bx = list[i].px[k + 1] - (double)c->org_x, bz = list[i].pz[k + 1] - (double)c->org_z;
            const double dx = bx - ax, dz = bz - az, len = hypot(dx, dz);
            int np, j;
            if (!(len > 1.0)) continue;
            np = (int)ceil(len / SC_FP_PIECE);
            if (np < 1) np = 1;
            for (j = 0; j < np; j++) {
                const double t0 = (double)j / np, t1 = (double)(j + 1) / np;
                const double x0 = ax + dx * t0, z0 = az + dz * t0, x1 = ax + dx * t1, z1 = az + dz * t1;
                const double mx = 0.5 * (x0 + x1), mz = 0.5 * (z0 + z1);
                double yl0;
                /* an untagged path on a lawn is the strip's park path: TD5_TG_GEOP_PATH_W wide */
                const double hw = (!(f->width_m > 0.0) && sc_lawn_y(c, mx, mz, &yl0)) ? SC_PARK_PATH_W * 0.5 : half;
                const double nx = dz / len * hw, nz = -dx / len * hw;
                double qx[4], qz[4], lat = 0.0, al;
                int ed, sd, q, pageb;
                ScFpBatch *bt;
                const double s0 = s_along + len * t0, s1 = s_along + len * t1;
                if (!sc_owned_point(c, mx, mz)) continue;
                c->st.fp_pieces++;
                used = 1;
                qx[0] = x0 - nx; qz[0] = z0 - nz; qx[1] = x0 + nx; qz[1] = z0 + nz;
                qx[2] = x1 + nx; qz[2] = z1 + nz; qx[3] = x1 - nx; qz[3] = z1 - nz;
                if (sc_on_road(c, qx, qz, 4, NULL)) { c->st.fp_cut_road++; continue; }
                /* inside the street band: the street's pavement (F4b) draws it */
                if (roam_nearest_edge(c, mx, mz, 30.0 * SC_UPM, &ed, &sd, &al, &lat)
                    && fabs(lat) <= c->seg[ed].hw + s->gap + SC_FP_MERGE_M * SC_UPM) { c->st.fp_merged++; continue; }
                pageb = (f->surface == TD5_GEO_FW_SURF_LOOSE) ? 1 : 0;
                sb = sc_sb_of(c, mx, mz);
                bt = &bat[sb * 2 + pageb];
                if (bt->nq >= SC_FP_CHUNK && !sc_fp_flush(c, sb, bt)) { free(bat); free(list); return 0; }
                for (q = 0; q < 4; q++) {
                    const int o = (bt->nq * 4 + q) * 5;
                    double y = sc_ground(c, qx[q], qz[q]) + SC_FP_LIFT, yl;
                    /* on a lawn: the lawn's own plane + the strip's tier (a path never sinks into the grass) */
                    if (sc_lawn_y(c, qx[q], qz[q], &yl) && yl + SC_GEOP_TIER > y) y = yl + SC_GEOP_TIER;
                    bt->v[o + 0] = (float)qx[q];
                    bt->v[o + 1] = (float)y;
                    bt->v[o + 2] = (float)qz[q];
                    bt->v[o + 3] = (float)((q == 1 || q == 2) ? 1.0 : 0.0);
                    bt->v[o + 4] = (float)(((q >= 2) ? s1 : s0) / (2.0 * TD5_TG_INFRA_M));
                    bt->light[bt->nq * 4 + q] = 0xFFFFFFFFu;
                }
                bt->nq++;
            }
            s_along += len;
        }
        if (used) c->st.fp_ways++;
    }
    for (sb = 0; sb < TD5_ROAM_SCN_NSB; sb++)
        if (!sc_fp_flush(c, sb, &bat[sb * 2]) || !sc_fp_flush(c, sb, &bat[sb * 2 + 1])) { free(bat); free(list); return 0; }
    free(bat);
    free(list);
    return 1;
}

/* ============================================================================
 * SECTION: assemble + the guard census over the bytes
 * ========================================================================== */

static uint32_t sc_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float    sc_rdf(const uint8_t *p)  { float v; memcpy(&v, p, 4); return v; }

/* Decode every primitive of one record and count the ones whose XZ footprint
 * overlaps the drivable surface. Billboards: their base point. */
static int sc_census_record(TG_TileCtx *c, const uint8_t *rec, size_t avail)
{
    const int tag = (int)(int16_t)(sc_rd32(rec) >> 16);
    const uint32_t ncmd = sc_rd32(rec + 4), nv = sc_rd32(rec + 8);
    const uint32_t cmd_off = sc_rd32(rec + TD5_MESH_DISK_OFF_COMMANDS), vtx_off = sc_rd32(rec + TD5_MESH_DISK_OFF_VERTICES);
    const float ox = sc_rdf(rec + 28) / 256.0f, oz = sc_rdf(rec + 36) / 256.0f;
    uint32_t ci, cursor = 0;
    int bad = 0;
    if (cmd_off + ncmd * 16u > avail || vtx_off + nv * 44u > avail) return 0;
    for (ci = 0; ci < ncmd; ci++) {
        const uint8_t *cm = rec + cmd_off + ci * 16u;
        const uint32_t tc = (uint32_t)(sc_rd32(cm + 8) & 0xFFFFu), qc = sc_rd32(cm + 8) >> 16;
        uint32_t p;
        for (p = 0; p < tc + qc; p++) {
            const int nvp = p < tc ? 3 : 4;
            double x[4], z[4];
            int k;
            if (cursor + (uint32_t)nvp > nv) return bad;
            for (k = 0; k < nvp; k++) {
                const uint8_t *v = rec + vtx_off + (cursor + (uint32_t)k) * 44u;
                x[k] = (double)sc_rdf(v) + (tag ? ox : 0.0f);
                z[k] = (double)sc_rdf(v + 8) + (tag ? oz : 0.0f);
            }
            cursor += (uint32_t)nvp;
            if (tag) {
                /* a camera-facing billboard stands on its ORIGIN (the trunk); its corners swing with the view */
                const double bx = (double)ox, bz = (double)oz;
                if (sc_on_road(c, &bx, &bz, 1, NULL)) {
                    bad++;
                    TD5_LOG_W(LOG_TAG, "ROAM_SCN_GUARD tile %d,%d billboard page %d at local (%.0f,%.0f) on the road",
                              c->tx, c->tz, (int)(int16_t)(sc_rd32(cm) >> 16), bx, bz);
                }
                continue;
            }
            {
                /* the XZ footprint of the face: a wall projects onto a segment */
                double fx[4], fz[4];
                int m = 0, a2;
                for (k = 0; k < nvp; k++) {
                    for (a2 = 0; a2 < m; a2++) if (fabs(fx[a2] - x[k]) < 1.0 && fabs(fz[a2] - z[k]) < 1.0) break;
                    if (a2 == m) { fx[m] = x[k]; fz[m] = z[k]; m++; }
                }
                if (m >= 3 && fabs(td5_geob_ring_area(fx, fz, m)) < 4.0) m = 2;   /* collinear: a wall */
                if (m >= 2 && m <= 4 && sc_on_road(c, fx, fz, m, NULL)) {
                    bad++;
                    if (bad <= 3)
                        TD5_LOG_W(LOG_TAG, "ROAM_SCN_GUARD tile %d,%d page %d %s face (%d pts) at local (%.0f,%.0f)-(%.0f,%.0f) "
                                  "on the road", c->tx, c->tz, (int)(int16_t)(sc_rd32(cm) >> 16), m == 2 ? "wall" : "flat",
                                  m, fx[0], fz[0], fx[1], fz[1]);
                }
            }
        }
    }
    return bad;
}

void td5_roam_scn_free(TD5_RoamScenery *sc)
{
    if (!sc) return;
    free(sc->blob);
    free(sc->rec_off);
    free(sc->lamp);
    free(sc);
}

static TD5_RoamScenery *sc_assemble(TG_TileCtx *c)
{
    TD5_RoamScenery *sc = (TD5_RoamScenery *)calloc(1, sizeof *sc);
    size_t total = 0, off = 0;
    int k, m;
    uint32_t nrec = 0, r = 0, h = 2166136261u;
    int *rec_furn = NULL;
    if (!sc) return NULL;
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) { total += c->sb[k].blk.len; nrec += (uint32_t)c->sb[k].nmesh; }
    sc->blob = (uint8_t *)malloc(total ? total : 1u);
    sc->rec_off = (uint32_t *)malloc((nrec ? nrec : 1u) * sizeof(uint32_t));
    rec_furn = (int *)calloc(nrec ? nrec : 1u, sizeof(int));
    if (!sc->blob || !sc->rec_off || !rec_furn) { free(rec_furn); td5_roam_scn_free(sc); return NULL; }
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) {
        const ScSb *b = &c->sb[k];
        if (b->blk.len) memcpy(sc->blob + off, b->blk.b, b->blk.len);
        sc->sb_first[k] = r;
        sc->sb_count[k] = (uint32_t)b->nmesh;
        for (m = 0; m < b->nmesh; m++) {
            rec_furn[r] = (m >= c->furn_first[k]);      /* [F4c] furniture: judged by its own census */
            sc->rec_off[r++] = (uint32_t)(off + b->moff[m]);
        }
        off += b->blk.len;
    }
    sc->lamp = c->lamp; sc->nlamp = c->nlamp;           /* ownership moves to the layer */
    c->lamp = NULL; c->nlamp = 0;
    sc->blob_bytes = total;
    sc->nrec = nrec;
    sc->tx = c->tx; sc->tz = c->tz;
    sc->org_x = c->org_x; sc->org_z = c->org_z;
    {
        const uint32_t rules = TD5_ROAM_SCENERY_RULES;
        h = sc_fnv(h, &rules, 4);
        h = sc_fnv(h, &sc->tx, 4); h = sc_fnv(h, &sc->tz, 4);
        h = sc_fnv(h, sc->sb_count, sizeof sc->sb_count);
        h = sc_fnv(h, sc->rec_off, (size_t)nrec * 4u);
        h = sc_fnv(h, sc->blob, total);
        {   /* [F4c] the furniture rules, its lanterns (the light registry's feed) */
            const uint32_t fr = TD5_ROAM_FURN_RULES;
            h = sc_fnv(h, &fr, 4);
            if (sc->nlamp) h = sc_fnv(h, sc->lamp, (size_t)sc->nlamp * 3u * sizeof(float));
        }
    }
    /* the census, over the bytes that will be drawn */
    for (r = 0; r < nrec; r++) {
        const size_t ro = sc->rec_off[r];
        const size_t end = (r + 1 < nrec) ? sc->rec_off[r + 1] : total;
        if (rec_furn[r]) continue;
        c->st.guard_overlaps += sc_census_record(c, sc->blob + ro, (end > ro && end <= total) ? end - ro : total - ro);
    }
    free(rec_furn);
    c->st.meshes = (int)nrec;
    c->st.bytes = (uint32_t)(total + (size_t)nrec * 4u);
    c->st.hash = h;
    sc->st = c->st;
    return sc;
}

/* ============================================================================
 * SECTION: session + build
 * ========================================================================== */

int td5_roam_scn_enabled(void) { return td5_env_int("TD5RE_ROAM_SCENERY", 1, 0, 1); }

TD5_RoamScnSource *td5_roam_scn_open(const char *place, const RoamGrid *g, TD5_RoamScnBuildFn build, void *u)
{
    TD5_RoamScnSource *s;
    if (!place || !g) return NULL;
    s = (TD5_RoamScnSource *)calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->place, sizeof s->place, "%s", place);
    s->grid = *g;
    s->build = build; s->build_u = u;
    s->upm = sc_place_upm(place);
    /* knob caches, read once here (design 4.1 class d) */
    s->tag_lm = td5_env_flag_on("TD5RE_GEO_LM_TAGS");
    s->area_wide = td5_env_flag_on("TD5RE_GEO_AREA_TAGS");
    s->gap = (double)td5_env_int("TD5RE_ROAM_SCN_GAP", (int)(tg_city_sidewalk_w(&k_biomes[0]) + TD5_TG_CARRIAGEWAY_MARGIN),
                                 0, 20000);
    td5_geo_source_path(s->canopy_path, sizeof s->canopy_path, place, "CANOPY.R8");
    td5_geo_source_path(s->height_path, sizeof s->height_path, place, "HEIGHT.R16");
    td5_geo_source_path(s->water_path, sizeof s->water_path, place, "WATER.R8");
    s->exag = (double)td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400) / 100.0;
    s->hc = (TD5_RoamHeight *)calloc(1, sizeof(TD5_RoamHeight));
    if (!sc_index_load(s)) TD5_LOG_W(LOG_TAG, "roam scenery: %s has no tiles/INDEX.JSON: no buildings", place);
    s->ctx = (TG_TileCtx *)calloc(1, sizeof(TG_TileCtx));
    if (!s->ctx) { td5_roam_scn_close(s); return NULL; }
    s->furn = td5_roam_furn_open(place);      /* [F4c] NULL = no furniture (knob off / no data) */
    TD5_LOG_I(LOG_TAG, "roam scenery: source open place=%s upm=%.1f gap=%.0f units index rows B=%d A=%d F=%d",
              place, s->upm, s->gap, s->nrows[0], s->nrows[1], s->nrows[2]);
    return s;
}

void td5_roam_scn_close(TD5_RoamScnSource *s)
{
    int l, k;
    if (!s) return;
    for (l = 0; l < SC_LAYERS; l++) {
        for (k = 0; k < SC_CACHE_LRU; k++) sc_cache_free(&s->cache[l][k]);
        free(s->rows[l]);
    }
    for (k = 0; k < SC_ROAD_LRU; k++) if (s->road[k].t) { td5_roam_tile_free(s->road[k].t); free(s->road[k].t); }
    sc_ctx_free(s->ctx);
    td5_roam_furn_close(s->furn);
    if (s->hc) { if (s->hc_open) td5_roam_height_close(s->hc); free(s->hc); }
    free(s);
}

TD5_RoamScenery *td5_roam_scn_build(TD5_RoamScnSource *s, const RoamTile *self, TD5_RoamHeight *hc)
{
    TG_TileCtx *c;
    ScCache *bc[SC_CACHE_LRU], *ac[SC_CACHE_LRU], *fc[SC_CACHE_LRU];
    ScArea *ar = NULL;
    int nbc, nac, nfc, nar = 0, cap = 0, l, i, dx, dz;
    double corner[4][2], x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
    const double halo = SC_HALO_M * SC_UPM;
    uint32_t t0 = td5_plat_time_ms(), t1;
    TD5_RoamScenery *sc = NULL;
    if (!s || !self || !td5_roam_scn_enabled()) return NULL;
    c = s->ctx;
    if (!sc_ctx_build(s, c, self)) return NULL;
    c->hc = hc;
    roam_grid_corners(&s->grid, self->tx, self->tz, corner);
    for (i = 0; i < 4; i++) {
        if (corner[i][0] < x0) x0 = corner[i][0];
        if (corner[i][0] > x1) x1 = corner[i][0];
        if (corner[i][1] < z0) z0 = corner[i][1];
        if (corner[i][1] > z1) z1 = corner[i][1];
    }
    /* the caller's hc is not used for the bytes (see TD5_RoamScnSource.hc) */
    (void)hc;
    hc = NULL;
    if (s->hc) {
        if (s->hc_open) td5_roam_height_close(s->hc);
        s->hc_open = td5_roam_height_open(s->hc, s->height_path, s->water_path, s->exag);
        if (s->hc_open && td5_roam_height_prefetch(s->hc, x0 - halo, z0 - halo, x1 + halo, z1 + halo)) hc = s->hc;
    }
    c->hc = hc;
    c->ground_last = 0.0;
    if (hc) {
        double h;
        if (td5_roam_height_sample(hc, 0.5 * (x0 + x1), 0.5 * (z0 + z1), &h, NULL)) c->ground_last = h;
    }
    if (gr_raster_read_window(s->canopy_path, x0 - halo, z0 - halo, x1 + halo, z1 + halo, 2, &c->canopy))
        c->canopy_ok = c->canopy.kind == 2 && c->canopy.data != NULL;
    /* OSM sources: the cache tiles that can hold a record owned by the 3x3 (extents
     * cover whole records, so the tile box + one tile each side is exact) */
    {
        const double S = (double)s->grid.tile * 1.5;
        nbc = sc_cache_window(s, SC_L_BLD, x0 - S, z0 - S, x1 + S, z1 + S, bc, k_sc_lru[SC_L_BLD]);
        nac = sc_cache_window(s, SC_L_AREA, x0 - S, z0 - S, x1 + S, z1 + S, ac, k_sc_lru[SC_L_AREA]);
        nfc = sc_cache_window(s, SC_L_FOOT, x0, z0, x1, z1, fc, k_sc_lru[SC_L_FOOT]);
    }
    /* the areas of the 3x3 (plaza veto, footpath lift) and the tile's own (lawns) */
    for (dz = -1; dz <= 1; dz++)
        for (dx = -1; dx <= 1; dx++) {
            const int64_t key = sc_tkey(self->tx + dx, self->tz + dz);
            for (l = 0; l < nac; l++) {
                int lo, hi, j;
                sc_owned_range(ac[l], key, &lo, &hi);
                for (j = lo; j < hi; j++) {
                    const int ri = ac[l]->order[j];
                    if (!sc_grow((void **)&ar, &cap, nar + 1, sizeof(ScArea))) goto out;
                    ar[nar].a = &ac[l]->a[ri];
                    ar[nar].px = &ac[l]->px[ac[l]->a[ri].first];
                    ar[nar].pz = &ac[l]->pz[ac[l]->a[ri].first];
                    ar[nar].id = ac[l]->id[ri];
                    nar++;
                }
            }
        }
    for (i = 1; i < nar; i++) {               /* (id, cx, cz) order */
        ScArea t = ar[i];
        int k;
        for (k = i - 1; k >= 0; k--) {
            const ScArea *q = &ar[k];
            if (!(q->id > t.id || (q->id == t.id && (q->a->cx > t.a->cx || (q->a->cx == t.a->cx && q->a->cz > t.a->cz)))))
                break;
            ar[k + 1] = ar[k];
        }
        ar[k + 1] = t;
    }
    if (!sc_lawn_planes(c, ar, nar, x0 - (double)c->org_x, z0 - (double)c->org_z,
                        x1 - (double)c->org_x, z1 - (double)c->org_z)) goto out;
    t1 = td5_plat_time_ms();
    c->st.ms_src = t1 - t0;
    if (td5_env_int("TD5RE_ROAM_SCN_PLAZAS", 1, 0, 1) && !sc_emit_plazas(c, ar, nar)) goto out;
    if (td5_env_int("TD5RE_ROAM_SCN_BUILDINGS", 1, 0, 1) && !sc_emit_buildings(s, c, bc, nbc, ar, nar)) goto out;
    if (td5_env_int("TD5RE_ROAM_SCN_FOOTPATHS", 1, 0, 1) &&
        !sc_emit_footpaths(s, c, fc, nfc, x0 - (double)c->org_x, z0 - (double)c->org_z,
                           x1 - (double)c->org_x, z1 - (double)c->org_z)) goto out;
    /* [F4c] street furniture: appended LAST in every sub-block (meshes from furn_first on are its own) */
    if (s->furn) {
        int k;
        for (k = 0; k < TD5_ROAM_SCN_NSB; k++) c->furn_first[k] = c->sb[k].nmesh;
        if (!td5_roam_furn_emit(s->furn, c, &c->st.furn, &c->lamp, &c->nlamp)) goto out;
    }
    c->st.ms_emit = td5_plat_time_ms() - t1;
    sc = sc_assemble(c);
    if (sc) {
        sc->st.ms = td5_plat_time_ms() - t0;
        sc->st.ms_emit = c->st.ms_emit;
        sc->st.ms_src = c->st.ms_src;
    }
out:
    sc_cache_unpin(s);
    free(ar);
    if (c->canopy.data) { free(c->canopy.data); c->canopy.data = NULL; }
    return sc;
}

/* ============================================================================
 * SECTION: dev CLI (--RoamBuildScenery), compiled out of RELEASE
 * ========================================================================== */

#ifndef TD5RE_RELEASE

static int sc_cli_get(const char *cmd, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *m;
    size_t n = 0;
    snprintf(pat, sizeof pat, "--%s=", key);
    m = cmd ? strstr(cmd, pat) : NULL;
    if (!m) return 0;
    m += strlen(pat);
    while (m[n] && m[n] != ' ' && m[n] != '\t' && n + 1 < cap) { out[n] = m[n]; n++; }
    out[n] = '\0';
    return 1;
}

typedef struct {
    char place[64], outdir[400];
    RoamGrid grid;
    int (*list)[2];
    int n, reverse, apply_env;
    TD5_FpEnv env;
    TD5_RoamScnStats *rows;
    int *ok;
} ScCliJob;

static int sc_write_file(const char *path, const TD5_RoamScenery *sc)
{
    FILE *f = fopen(path, "wb");
    uint32_t hdr[8];
    if (!f) return 0;
    memcpy(hdr, "TD5RSCN1", 8);
    hdr[2] = TD5_ROAM_SCENERY_RULES; hdr[3] = (uint32_t)sc->tx; hdr[4] = (uint32_t)sc->tz;
    hdr[5] = (uint32_t)sc->org_x; hdr[6] = (uint32_t)sc->org_z; hdr[7] = sc->nrec;
    fwrite(hdr, 4, 8, f);
    fwrite(sc->sb_count, 4, TD5_ROAM_SCN_NSB, f);
    fwrite(sc->rec_off, 4, sc->nrec, f);
    if (sc->blob_bytes) fwrite(sc->blob, 1, sc->blob_bytes, f);
    fwrite(&sc->st.hash, 4, 1, f);
    fclose(f);
    return 1;
}

static void sc_cli_run(ScCliJob *j)
{
    TD5_RoamScnSource *s = td5_roam_scn_open(j->place, &j->grid, NULL, NULL);
    TD5_RoamHeight *hc = (TD5_RoamHeight *)calloc(1, sizeof *hc);
    char hp[512], wp[512];
    int i, hc_ok = 0;
    if (j->apply_env) td5_plat_fpenv_apply(&j->env);
    if (!s) return;
    td5_geo_source_path(hp, sizeof hp, j->place, "HEIGHT.R16");
    td5_geo_source_path(wp, sizeof wp, j->place, "WATER.R8");
    if (hc) hc_ok = td5_roam_height_open(hc, hp, wp,
            (double)td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400) / 100.0);
    for (i = 0; i < j->n; i++) {
        const int k = j->reverse ? j->n - 1 - i : i;
        RoamTile t;
        char path[512], err[160];
        TD5_RoamScenery *sc;
        memset(&t, 0, sizeof t);
        td5_roam_tile_path(path, sizeof path, NULL, j->place, j->list[k][0], j->list[k][1]);
        if (!td5_roam_tile_read(path, &t, 0, err, sizeof err)) continue;
        sc = td5_roam_scn_build(s, &t, hc_ok ? hc : NULL);
        if (sc) {
            j->rows[k] = sc->st;
            j->ok[k] = 1;
            if (j->outdir[0]) {
                snprintf(path, sizeof path, "%s/SCENERY_%d_%d.BIN", j->outdir, j->list[k][0], j->list[k][1]);
                sc_write_file(path, sc);
            }
            td5_roam_scn_free(sc);
        }
        td5_roam_tile_free(&t);
    }
    if (hc) { if (hc_ok) td5_roam_height_close(hc); free(hc); }
    td5_roam_scn_close(s);
}

static void sc_cli_thread(void *arg) { sc_cli_run((ScCliJob *)arg); }

int td5_roam_scn_dev_cli(const char *cmdline)
{
    char val[256], cens[400] = "", buf[64], gpath[512];
    ScCliJob j;
    int i, threadmode = 0, cap = 2048;
    FILE *f;
    uint32_t t0;
    long tot[24], ft[16];
    double shift_max = 0.0;
    uint32_t ms_max = 0, ms_sum = 0, bytes_max = 0;
    if (!sc_cli_get(cmdline, "RoamBuildScenery", val, sizeof val)) return 0;
    memset(&j, 0, sizeof j);
    snprintf(j.place, sizeof j.place, "la_plata_partido");
    sc_cli_get(cmdline, "RoamPlace", j.place, sizeof j.place);
    sc_cli_get(cmdline, "RoamOut", j.outdir, sizeof j.outdir);
    sc_cli_get(cmdline, "RoamCensus", cens, sizeof cens);
    if (sc_cli_get(cmdline, "RoamOrder", buf, sizeof buf)) j.reverse = (_strnicmp(buf, "rev", 3) == 0);
    if (sc_cli_get(cmdline, "RoamThread", buf, sizeof buf)) threadmode = atoi(buf);
    td5_geo_source_path(gpath, sizeof gpath, j.place, "roam/ROAMGRID.JSON");
    {
        char *g = sc_slurp(gpath, NULL);
        if (!g || !roam_grid_parse(g, &j.grid)) roam_grid_default(j.place, &j.grid);
        free(g);
    }
    j.list = (int (*)[2])malloc((size_t)cap * sizeof(int[2]));
    if (!j.list) return 1;
    if (_strnicmp(val, "casco", 5) == 0) {
        j.n = td5_roam_casco_tiles(&j.grid, j.list, cap);
    } else {
        int x0, z0, x1, z1, tx, tz;
        if (sscanf(val, "%d,%d,%d,%d", &x0, &z0, &x1, &z1) != 4) { free(j.list); return 1; }
        for (tz = z0; tz <= z1; tz++) for (tx = x0; tx <= x1 && j.n < cap; tx++) { j.list[j.n][0] = tx; j.list[j.n][1] = tz; j.n++; }
    }
    j.rows = (TD5_RoamScnStats *)calloc((size_t)(j.n > 0 ? j.n : 1), sizeof(TD5_RoamScnStats));
    j.ok = (int *)calloc((size_t)(j.n > 0 ? j.n : 1), sizeof(int));
    if (!j.rows || !j.ok) return 1;
    if (j.outdir[0]) td5_roam_mkdirs(j.outdir);
    td5_roam_furn_atlas_prepare(j.place, &j.grid);        /* [F4c] main thread, before any worker */
    t0 = td5_plat_time_ms();
    if (threadmode > 0) {
        void *th;
        td5_plat_fpenv_capture(&j.env);
        j.apply_env = (threadmode == 1);
        th = td5_plat_thread_create(sc_cli_thread, &j);
        if (th) td5_plat_thread_join(th);
    } else {
        sc_cli_run(&j);
    }
    memset(tot, 0, sizeof tot);
    memset(ft, 0, sizeof ft);
    if (!cens[0]) {
        const char *d = td5_plat_log_dir();
        snprintf(cens, sizeof cens, "%sroam_scenery_census.txt", d ? d : "");
    }
    f = fopen(cens, "w");
    if (f) fprintf(f, "roam scenery census (F4a), place %s, %d tile(s), order %s, thread %d, rules %u\n"
                      "tx tz ok hash meshes bytes ms ms_src ms_emit road_tiles bld_seen bld_emitted bld_prefab apron "
                      "drop_road drop_shift drop_ring veto_plaza veto_lm nudged shift_max_m fp_ways fp_pieces fp_cut "
                      "fp_merged areas lawn_tris lawn_cut trees trees_cut guard_overlaps furn_meshes sig_clusters sig_arms "
                      "sig_inbound mast_near post_left mast_far far_shared plates plate_one_face plate_none sig_drop_frame "
                      "sig_drop_road sig_drop_dup lamps lamp_glow lamp_drop_road lamp_skip_class stops shelters benches bins "
                      "stop_drop_far stop_drop_road zebras limit_lines xing_unknown xing_drop_far furn_guard\n",
                   j.place, j.n, j.reverse ? "reverse" : "forward", threadmode, TD5_ROAM_SCENERY_RULES);
    for (i = 0; i < j.n; i++) {
        const TD5_RoamScnStats *r = &j.rows[i];
        if (f) fprintf(f, "%d %d %d %08x %d %u %u %u %u %d %d %d %d %d %d %d %d %d %d %d %.2f %d %d %d %d %d %d %d %d %d %d "
                          "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                       j.list[i][0], j.list[i][1], j.ok[i], r->hash, r->meshes, r->bytes, r->ms, r->ms_src, r->ms_emit,
                       r->road_tiles, r->bld_seen, r->bld_emitted, r->bld_prefab, r->bld_apron, r->bld_drop_road,
                       r->bld_drop_shift, r->bld_drop_ring, r->bld_veto_plaza, r->bld_veto_lm, r->bld_nudged,
                       r->bld_shift_max / SC_UPM, r->fp_ways, r->fp_pieces, r->fp_cut_road, r->fp_merged, r->areas,
                       r->lawn_tris, r->lawn_cut, r->trees, r->trees_cut, r->guard_overlaps,
                       r->furn.meshes, r->furn.sig_clusters, r->furn.sig_arms, r->furn.sig_inbound, r->furn.mast_near,
                       r->furn.post_left, r->furn.mast_far, r->furn.far_shared, r->furn.plates, r->furn.plate_one_face,
                       r->furn.plate_none, r->furn.sig_drop_frame, r->furn.sig_drop_road, r->furn.sig_drop_dup, r->furn.lamps,
                       r->furn.lamp_glow, r->furn.lamp_drop_road, r->furn.lamp_skip_class, r->furn.stops, r->furn.shelters,
                       r->furn.benches, r->furn.bins, r->furn.stop_drop_far, r->furn.stop_drop_road, r->furn.zebras,
                       r->furn.limit_lines, r->furn.xing_unknown, r->furn.xing_drop_far, r->furn.guard_hits);
        ft[0] += r->furn.meshes; ft[1] += r->furn.sig_clusters; ft[2] += r->furn.sig_inbound; ft[3] += r->furn.mast_near;
        ft[4] += r->furn.post_left; ft[5] += r->furn.mast_far; ft[6] += r->furn.plates; ft[7] += r->furn.lamps;
        ft[8] += r->furn.stops; ft[9] += r->furn.zebras; ft[10] += r->furn.limit_lines; ft[11] += r->furn.guard_hits;
        ft[12] += r->furn.sig_drop_road + r->furn.sig_drop_frame + r->furn.sig_drop_dup;
        ft[13] += r->furn.lamp_drop_road; ft[14] += r->furn.far_shared; ft[15] += r->furn.plate_none;
        tot[0] += j.ok[i]; tot[1] += r->meshes; tot[2] += r->bytes; tot[3] += r->bld_seen; tot[4] += r->bld_emitted;
        tot[5] += r->bld_prefab; tot[6] += r->bld_drop_road; tot[7] += r->bld_drop_shift; tot[8] += r->bld_veto_plaza;
        tot[9] += r->bld_veto_lm; tot[10] += r->fp_pieces; tot[11] += r->fp_cut_road; tot[12] += r->fp_merged;
        tot[13] += r->areas; tot[14] += r->lawn_tris; tot[15] += r->trees; tot[16] += r->guard_overlaps;
        tot[17] += r->bld_nudged; tot[18] += r->bld_apron; tot[19] += r->lawn_cut; tot[20] += r->trees_cut;
        tot[21] += r->bld_drop_ring;
        if (r->bld_shift_max > shift_max) shift_max = r->bld_shift_max;
        if (r->ms > ms_max) ms_max = r->ms;
        if (r->bytes > bytes_max) bytes_max = r->bytes;
        ms_sum += r->ms;
    }
    {
        char line[1400];
        snprintf(line, sizeof line,
                 "roam scenery: %ld/%d tile(s) in %u ms (build ms mean %.1f max %u), %ld meshes, %.2f MB (max %.2f MB/tile) | "
                 "buildings %ld seen %ld emitted (%ld set pieces, %ld aprons), dropped on a road %ld, past the shift cap "
                 "%ld, bad ring %ld, plaza veto %ld, landmark veto %ld, nudged %ld (max %.1f m) | footpaths %ld pieces, "
                 "%ld cut on a road, %ld merged into the street band | plazas %ld, %ld lawn tris (%ld cut), %ld trees "
                 "(%ld off the road) | GUARD: %ld emitted primitive(s) on the drivable surface",
                 tot[0], j.n, (unsigned)(td5_plat_time_ms() - t0), j.n ? (double)ms_sum / j.n : 0.0, (unsigned)ms_max,
                 tot[1], (double)tot[2] / 1048576.0, (double)bytes_max / 1048576.0, tot[3], tot[4], tot[5], tot[18],
                 tot[6], tot[7], tot[21], tot[8], tot[9], tot[17], shift_max / SC_UPM, tot[10], tot[11], tot[12],
                 tot[13], tot[14], tot[19], tot[15], tot[20], tot[16]);
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam furniture: %ld meshes | signalised clusters %ld, approach arms %ld: near mast arms %ld, post lights %ld, "
                 "far mast arms %ld (far shared with an opposite near arm %ld), plates %ld (arms with no plate cell %ld), "
                 "signal heads dropped (road/frame/dup) %ld | lamps %ld (dropped on a road %ld) | bus stops %ld | zebras %ld, "
                 "limit lines %ld | FURNITURE GUARD: %ld face(s) on the drivable surface",
                 ft[0], ft[1], ft[2], ft[3], ft[4], ft[5], ft[14], ft[6], ft[15], ft[12], ft[7], ft[13], ft[8], ft[9], ft[10],
                 ft[11]);
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        if (f) fclose(f);
    }
    free(j.list); free(j.rows); free(j.ok);
    return 1;
}

#endif /* !TD5RE_RELEASE */
