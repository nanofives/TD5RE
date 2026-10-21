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
 *   pavement       [F4b] slab + kerb + back face + fence along every kerb of the drivable
 *                  surface, junction corners on the patch outline, medians and islands
 *   lots          [HD] the city's open ground paved (render-lattice quads), ring plazas, the infill
 *                  frontage lots on each block's building line
 *   assemble      the blob + record table + hash, the guard census over the bytes
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
#include "td5_geo_route.h"       /* GrRaster, gr_raster_read_window_game         */
#include "td5_geo_frame.h"       /* [roam M] the geographic -> game reflection    */
#include "td5_geo_sidewalk.h"    /* [F4b] td5_geo_sw_resolve                     */
#include "td5_config.h"
#include "td5_platform.h"
#include "deps/cjson/cJSON.h"
#include "td5_roam_tile.h"
#include "td5_roam_height.h"
#include "td5_roam_groundfix.h"   /* [G4b] the ground lowered under the road */
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
    unsigned char *lot;     /* [HD] AREAS: SC_LC_* per record (the raw OSM kind) */
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
    char      cover_path[512];    /* [HD] COVER.R8: the built-up test of the city lots */
    int64_t  *casco;              /* [HD] the casco's tile keys, sorted (td5_roam_casco_tiles) */
    int       ncasco;
    int       lots_on, lot_cover, infill_on;   /* [HD] TD5RE_ROAM_LOTS / _LOT_COVER / TD5RE_ROAM_INFILL */
    int       lot_page;                       /* [HD] TD5RE_ROAM_LOT_PAGE: the lots' host page */
    int       infill_log;                     /* [HD] TD5RE_ROAM_INFILL_LOG: one log line per built lot */
    int       bld_log;                        /* [IC] TD5RE_ROAM_BLD_LOG: one log line per footprint (its fate) */
    int       cover_green;                    /* [IC] TD5RE_ROAM_COVER_GREEN: unmapped vegetated casco loops are green */
    int       ic_despike, ic_drops, ic_infill;  /* [IC] TD5RE_ROAM_AREA_DESPIKE / _BLD_DROPS / _IC_INFILL (A/B: all IC
                                                 * knobs 0 = the round M scenery, byte for byte) */
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
    int       lm_wins;        /* [F4b] TD5RE_ROAM_LM_WINS, see sc_bld_clear       */
    struct ScPvSrc *pv;       /* [F4b] the pavement's ROADS facts (SECTION pavement) */
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
    free(c->b); free(c->a); free(c->f); free(c->id); free(c->own); free(c->order); free(c->lot);
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
static int sc_lot_class(const char *kind);
static int sc_cache_load(TD5_RoamScnSource *s, ScCache *c, int layer, int ix, int iz)
{
    const uint32_t t0 = td5_plat_time_ms();
    const int r = sc_cache_load_body(s, c, layer, ix, iz);
    TD5_LOG_I(LOG_TAG, "ROAM_SCN_CACHE %s_%d_%d: %d record(s), %.1f MB kept, %u ms (from %u to %u ms)",
              k_sc_layer[layer], ix, iz, c->n, (double)c->bytes / 1048576.0, (unsigned)(td5_plat_time_ms() - t0),
              (unsigned)t0, (unsigned)td5_plat_time_ms());
    return r;
}

/* [IC] SPIKES. One OSM ring can carry a stray vertex kilometres off (park 558641937, a 6-point plaza ring with one
 * node 4.6 km away at a 3 deg tip): a thin wedge of "park" laid over ordinary blocks, the lots under it kept green,
 * the plaza veto taking the buildings in it. A vertex whose interior angle is under SC_SPIKE_DEG with BOTH edges
 * longer than SC_SPIKE_M and SC_SPIKE_RATIO x the ring's median edge is no outline corner: it is dropped (and the
 * centre / radius recomputed the way td5_geob_parse_area makes them, vertex mean and farthest vertex). Measured on
 * the La Plata partido: 1 of 3200 closed rings (_archive/roam/IC/spike_census.py). 1 when a vertex went. */
#define SC_SPIKE_DEG    8.0
#define SC_SPIKE_M      150.0
#define SC_SPIKE_RATIO  4.0
static int sc_area_despike(double *rx, double *rz, int *np, TD5_GeoArea *a)
{
    double len[TD5_GEOB_RING_MAX], srt[TD5_GEOB_RING_MAX], med, lim, cx = 0.0, cz = 0.0, r = 0.0;
    int n = *np, i, j, k, drop = 0;
    if (n < 5 || n > TD5_GEOB_RING_MAX) return 0;
    for (i = 0; i < n; i++) {
        j = (i + 1) % n;
        len[i] = srt[i] = hypot(rx[j] - rx[i], rz[j] - rz[i]);
    }
    for (i = 1; i < n; i++) {                      /* insertion sort: n <= RING_MAX */
        const double t = srt[i];
        for (k = i - 1; k >= 0 && srt[k] > t; k--) srt[k + 1] = srt[k];
        srt[k + 1] = t;
    }
    med = srt[n / 2];
    lim = SC_SPIKE_M * SC_UPM > SC_SPIKE_RATIO * med ? SC_SPIKE_M * SC_UPM : SC_SPIKE_RATIO * med;
    for (i = 0, k = 0; i < n; i++) {
        const int p = (i + n - 1) % n, q = (i + 1) % n;
        const double ax = rx[p] - rx[i], az = rz[p] - rz[i], bx = rx[q] - rx[i], bz = rz[q] - rz[i];
        const double ang = fabs(atan2(ax * bz - az * bx, ax * bx + az * bz)) * 57.29577951308232;
        if (ang < SC_SPIKE_DEG && len[p] > lim && len[i] > lim) { drop++; continue; }
        srt[k] = rx[i]; len[k] = rz[i]; k++;      /* reuse the two arrays as the kept points */
    }
    if (!drop || k < 3) return 0;
    for (i = 0; i < k; i++) { rx[i] = srt[i]; rz[i] = len[i]; cx += rx[i]; cz += rz[i]; }
    cx /= (double)k; cz /= (double)k;
    for (i = 0; i < k; i++) { const double d = hypot(rx[i] - cx, rz[i] - cz); if (d > r) r = d; }
    a->cx = cx; a->cz = cz; a->radius = r; a->n = k;
    *np = k;
    return 1;
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
    td5_geo_src_json_to_game(root);          /* [roam M] source frame -> game frame */
    if (layer == SC_L_BLD) storey_m = td5_geob_storey_m_of(root);
    c->storey_m = storey_m;
    arr = cJSON_GetObjectItem(root, k_sc_key[layer]);
    cap_rec = arr ? cJSON_GetArraySize(arr) : 0;
    if (cap_rec > 0) {
        if (layer == SC_L_BLD) c->b = (TD5_GeoBuilding *)calloc((size_t)cap_rec, sizeof(TD5_GeoBuilding));
        if (layer == SC_L_AREA) c->a = (TD5_GeoArea *)calloc((size_t)cap_rec, sizeof(TD5_GeoArea));
        if (layer == SC_L_AREA) c->lot = (unsigned char *)calloc((size_t)cap_rec, 1);
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
            if (s->ic_despike && sc_area_despike(rx, rz, &n, a) && s->bld_log)
                TD5_LOG_I(LOG_TAG, "ROAM_AREA_SPIKE %s_%d_%d id %.0f: spike vertex dropped, %d point(s) left",
                          k_sc_layer[layer], ix, iz, id, n);
            first = sc_pool_push(c, &cap_pt, rx, rz, n);
            if (first < 0) break;
            a->first = first;
            if (c->lot) {
                const cJSON *kv = cJSON_GetObjectItem(e, "kind");
                c->lot[c->n] = (unsigned char)sc_lot_class((kv && cJSON_IsString(kv)) ? kv->valuestring : NULL);
            }
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
            td5_geo_src_bbox_to_game(&v[3], &v[5]);   /* [roam M] the extent in the game frame */
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

/* [F4b] cap_row: the quad closes a dead end on its row 0 (x/z[0..1]) or row 1
 * (x/z[2..3]), else -1; lm_ok: a landmark was let stand on this dead-end quad
 * (sc_lm_stub_ok), so the census books its overlap apart (st.guard_lm) */
typedef struct { double x[4], z[4]; int n; double x0, z0, x1, z1; int seg; int cap_row, lm_ok; } ScPrim;
typedef struct { double ax, az, bx, bz, ay, by, hw, s0; int edge; } ScSeg;
typedef struct { uint64_t key; int klass, seg_first, seg_count; } ScEdge;

typedef struct {
    TG_Buf  blk;
    size_t *moff;
    int     nmesh, cap;
} ScSb;

/* [HD] what a lot under an OSM polygon becomes (sc_lot_class), and a polygon the lots / infill ask about (place frame) */
enum { SC_LC_BUILD = 0, SC_LC_GREEN = 1, SC_LC_OPEN = 2 };
/* plaza: [IC] a plaza-kind AREAS polygon (td5_geob_area_is_plaza), a ring plaza or an avenue median: no footprint the
 * map does not draw itself (Overture's) stands on it (sc_bld_green_veto) */
typedef struct ScLotPoly { const double *px, *pz; int n, cls; double x0, z0, x1, z1; int plaza; } ScLotPoly;
typedef struct InfRec { double x[TD5_GEOB_RING_MAX], z[TD5_GEOB_RING_MAX]; int n; double x0, z0, x1, z1; } InfRec;

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
    int      skip_lm;           /* [F4b] queries ignore ScPrim.lm_ok primitives */
    /* sources for the emit */
    TD5_RoamHeight *hc;
    GrRaster canopy;
    int      canopy_ok;
    GrRaster cover;             /* [HD] COVER.R8 window of the tile (lots) */
    int      cover_ok;
    GrRaster cover3;            /* [IC] COVER.R8 window of the 3x3 (pv_open_loops' vegetation test) */
    int      cover3_ok;
    ScLotPoly *anyp;            /* [IC] EVERY AREAS polygon reaching the 3x3, any class (landuse=residential too) */
    int      nanyp;
    ScLotPoly *lotp;            /* [HD] the AREAS polygons that take no infill / keep their green (sc_lot_polys) */
    int      nlotp;             /*      + the ring plazas + medians (pv_open_loops), points in ringpool   */
    double  *ringpool;
    void    *pv_ways, *pv_B;        /* [IC] the pavement's ways + kerb pieces, gathered BEFORE the buildings (sc_pv_prepare) */
    int      pv_nways, pv_nb, pv_ready;
    InfRec  *infr;              /* [HD] the infill lots accepted so far (any owner): no pavement fence in front of them */
    int      ninfr, cap_infr;
    double   ground_last;
    TD5_RoamGfix gf;            /* [G4b] how far the drawn ground is lowered under the road (render clamp) */
    ScSb     sb[TD5_ROAM_SCN_NSB];
    /* lawn planes of every plaza of the 3x3 that reaches this tile: x/z/y per
     * corner (3 per triangle), base refinement, no road cut -- a path laid on a
     * neighbour's lawn reads the same plane the neighbour draws */
    double  *lx, *lz, *ly;
    int      nlt, cap_lt;
    /* [G5] every pavement slab / island quad this tile emitted (x y z per corner, tile-local), so a footpath
     * laid after the pavement stands on top of one it crosses */
    double  *pvq;
    int      npvq, cap_pvq;
    TD5_RoamScnStats st;
    TG_GeoBldStats   bst;
    /* [F4c] furniture: first mesh of each sub-block that is furniture (meshes from there on are
     * exempt from the F4a census, which judges them with its own overhead-aware one), and the lanterns */
    int      furn_first[TD5_ROAM_SCN_NSB];
    int      lot_first[TD5_ROAM_SCN_NSB];   /* [HD] first mesh of each sub-block that is a city lot (ground: no footprint census) */
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
    c->skip_lm = 0;
    c->nlt = 0;
    c->npvq = 0;
    c->ninfr = 0;
    free(c->lamp); c->lamp = NULL; c->nlamp = 0;
    if (c->canopy.data) free(c->canopy.data);
    memset(&c->canopy, 0, sizeof c->canopy);
    c->canopy_ok = 0;
    if (c->cover.data) free(c->cover.data);
    memset(&c->cover, 0, sizeof c->cover);
    c->cover_ok = 0;
    if (c->cover3.data) free(c->cover3.data);
    memset(&c->cover3, 0, sizeof c->cover3);
    c->cover3_ok = 0;
    free(c->anyp); c->anyp = NULL; c->nanyp = 0;
    td5_roam_gfix_free(&c->gf);
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) { c->sb[k].blk.len = 0; c->sb[k].blk.oom = 0; c->sb[k].nmesh = 0; c->furn_first[k] = 0x7FFFFFFF; c->lot_first[k] = 0x7FFFFFFF; }
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
    free(c->pvq);
    free(c->infr);
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
    signed char *caprow = (signed char *)malloc(t->nspan > 0 ? t->nspan : 1u);
    if (!caprow) return 0;
    memset(caprow, -1, t->nspan > 0 ? t->nspan : 1u);
    for (i = 0; i < t->nbedge; i++) {               /* [F4b] dead-end caps: a BEDG across one row */
        const RoamBEdge *be = &t->bedge[i];
        if (be->prim & ROAM_PRIM_TRI || be->prim >= t->nspan) continue;
        {
            const TD5_StripSpan *sp = &t->span[be->prim];
            const uint32_t n = (uint32_t)(sp->pad_02[1] & 0x0Fu);
            if (n >= 1 && (uint32_t)be->v[1] == (uint32_t)be->v[0] + n)
                caprow[be->prim] = (signed char)(be->v[0] == sp->left_vertex_index ? 0 : 1);
        }
    }
    for (e = 0; e < t->nedge; e++) {
        const RoamEdgeRec *er = &t->edge[e];
        ScEdge *ed;
        double s0 = (double)er->row0 * SC_ROW;
        if (!sc_grow((void **)&c->edge, &c->cap_edge, c->nedge + 1, sizeof(ScEdge))) { free(caprow); return 0; }
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
            if (!sc_grow((void **)&c->prim, &c->cap_prim, c->nprim + 1, sizeof(ScPrim))) { free(caprow); return 0; }
            c->prim[c->nprim].cap_row = caprow[si];
            c->prim[c->nprim].lm_ok = 0;
            sc_tvert(c, t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[L], &lx0, &ly0, &lz0);
            sc_tvert(c, t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[L + (uint32_t)n], &lxn, &lyn, &lzn);
            sc_tvert(c, t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[R], &rx0, &ry0, &rz0);
            sc_tvert(c, t, sp->origin_x, sp->origin_y, sp->origin_z, &t->vert[R + (uint32_t)n], &rxn, &ryn, &rzn);
            if (!sc_grow((void **)&c->seg, &c->cap_seg, c->nseg + 1, sizeof(ScSeg))) { free(caprow); return 0; }
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
    free(caprow);
    for (i = 0; i < t->ntri; i++) {
        const RoamTri *tr = &t->tri[i];
        ScPrim *pr;
        int k;
        double y;
        if (tr->v[0] >= t->nvert || tr->v[1] >= t->nvert || tr->v[2] >= t->nvert) continue;
        if (!sc_grow((void **)&c->prim, &c->cap_prim, c->nprim + 1, sizeof(ScPrim))) return 0;
        pr = &c->prim[c->nprim];
        pr->cap_row = -1; pr->lm_ok = 0;
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

typedef struct { const double *x, *z; int n; const ScPrim *hit; int skip_lm; } ScPolyQ;
static int sc_poly_q(void *u, const ScPrim *p)
{
    ScPolyQ *q = (ScPolyQ *)u;
    if (q->skip_lm && p->lm_ok) return 0;
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
    q.x = x; q.z = z; q.n = n; q.hit = NULL; q.skip_lm = c->skip_lm;
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
        double low = 0.0;
        /* [G4b] the render holds the drawn ground under the road (G4): follow it, so pavement beside a road in a cut
         * stands at the road edge and not on the old raster height. 0 outside a clamped cell: bytes unchanged. */
        if (c->gf.h && td5_roam_gfix_lowering(&c->gf, x + (double)c->org_x, z + (double)c->org_z, &low)) h -= low;
        c->ground_last = h;
        return h;
    }
    return c->ground_last;
}

/* [G4b] how far the drawn ground is lowered under the road at a tile-local point (0 where the clamp does not act) */
static double sc_lowering(const TG_TileCtx *c, double x, double z)
{
    double low = 0.0;
    if (c->gf.h && !td5_roam_gfix_lowering(&c->gf, x + (double)c->org_x, z + (double)c->org_z, &low)) low = 0.0;
    return low;
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

/* [F4b] an apron piece clear of every drivable primitive (the apron's long hull edges
 * otherwise lay the band across a street between two cleared vertices) */
static int sc_bld_apron_quad_ok(void *uv, const double *qx, const double *qz)
{
    return !sc_on_road(((ScBldU *)uv)->c, qx, qz, 4, NULL);
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

/* [F4b] lm_wins: a landmark set (anchor outline + its building:parts) skips the
 * kerb + gap nudge and is only cleared off the drivable surface itself (the loop
 * after it). Plaza Moreno: the cathedral (way 119950663) stands 5.6 m from the
 * centreline of private driveway 252476493, 2.1 m from its kerb, never on it; the
 * 2.8 m pavement gap pushed the whole outline into the streets round the block and
 * past the shift cap, so the cathedral, its parts' base and its apron were dropped. */
#define SC_LM_STUB_M  3.5      /* how far into a landmark a dead-end stub may run: its last row */

/* [F4b] May landmark ring (rx, rz)[n] stand on drivable primitive p? Only the LAST
 * quad of a dead end of a minor street (service / living / unclassified-or-less),
 * and only within SC_LM_STUB_M of its cap row: a forecourt access that OSM draws to
 * the wall (Plaza Moreno: service way 255917952 runs 35 m off Avenida 51 and ends ON
 * the cathedral outline; the outline's recessed portal flanks reach 2.49 m back along
 * its 7 m ribbon, inside the last 3.5 m row). The road layer keeps the stub whole (no
 * hole); the cathedral keeps its outline. */
static int sc_lm_stub_ok(const TG_TileCtx *c, const ScPrim *p, const double *rx, const double *rz, int n)
{
    const double lim = SC_LM_STUB_M * SC_UPM;
    double ax, az, bx, bz, ox, oz, cl, dx, dz, deep = 0.0;
    int i, j, klass;
    if (!p || p->seg < 0 || p->n != 4) return 0;
    klass = c->edge[c->seg[p->seg].edge].klass;
    if (p->cap_row < 0 || klass > TD5_GEO_RC_LIVING) {
        TD5_LOG_I(LOG_TAG, "[ROAM LM] tile %d,%d the quad of way %lld under a landmark is no minor dead end "
                  "(cap row %d, class %d): not let through", c->tx, c->tz,
                  (long long)(c->edge[c->seg[p->seg].edge].key >> 16), p->cap_row, klass);
        return 0;
    }
    /* the cap row a-b and the opposite row's midpoint (the stub's way in) */
    ax = p->x[p->cap_row ? 2 : 0]; az = p->z[p->cap_row ? 2 : 0];
    bx = p->x[p->cap_row ? 3 : 1]; bz = p->z[p->cap_row ? 3 : 1];
    ox = 0.5 * (p->x[p->cap_row ? 0 : 2] + p->x[p->cap_row ? 1 : 3]);
    oz = 0.5 * (p->z[p->cap_row ? 0 : 2] + p->z[p->cap_row ? 1 : 3]);
    dx = bx - ax; dz = bz - az;
    cl = hypot(dx, dz);
    if (!(cl > 1.0)) return 0;
    /* every sample of the quad inside the ring lies within lim of the cap row */
    for (i = 0; i <= 16; i++)
        for (j = 0; j <= 16; j++) {
            const double s = (double)i / 16.0, t = (double)j / 16.0;
            const double cx = ax + dx * s, cz = az + dz * s;
            const double x = cx + (ox - 0.5 * (ax + bx)) * t, z = cz + (oz - 0.5 * (az + bz)) * t;
            double d;
            if (!td5_geob_point_in_ring(rx, rz, n, x, z)) continue;
            d = fabs((x - ax) * dz - (z - az) * dx) / cl;
            if (d > deep) deep = d;
        }
    if (deep > lim)
        TD5_LOG_I(LOG_TAG, "[ROAM LM] tile %d,%d a landmark reaches %.2f m into the dead-end quad of way %lld "
                  "(more than %.1f m): not let through", c->tx, c->tz, deep / SC_UPM,
                  (long long)(c->edge[c->seg[p->seg].edge].key >> 16), SC_LM_STUB_M);
    return deep <= lim;
}

static int sc_bld_clear(TG_TileCtx *c, ScBldU *u, double gap, double *rx, double *rz, int n,
                        double *shift_out, int lm_wins)
{
    double shift = 0.0, total = 0.0;
    int k, it;
    const ScPrim *hit = NULL;
    *shift_out = 0.0;
    (void)u;
    /* the strip clears a footprint against its HOST span's straight frame; a roam
     * footprint has no host, so every vertex is cleared against the street segments
     * actually near it (kerb + gap from the nearest centreline), deepest first */
    for (it = 0; it < 6 && !lm_wins; it++) {
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
    if (lm_wins) {
        /* a landmark stands where OSM puts it unless it truly covers a street: the
         * dead-end stubs drawn to its wall are let through (sc_lm_stub_ok) */
        ScPrim *marked[8];
        int nm = 0, g, clear;
        c->skip_lm = 1;
        for (g = 0; g < 8; g++) {
            if (!sc_on_road(c, rx, rz, n, &hit)) break;
            if (!sc_lm_stub_ok(c, hit, rx, rz, n)) break;
            marked[nm] = (ScPrim *)hit;
            marked[nm++]->lm_ok = 1;
        }
        clear = !sc_on_road(c, rx, rz, n, NULL);
        c->skip_lm = 0;
        if (clear) {
            for (g = 0; g < nm; g++)
                TD5_LOG_I(LOG_TAG, "[ROAM LM] tile %d,%d landmark kept over the dead-end quad of way %lld (within %.1f m "
                          "of its cap): the road layer keeps the stub", c->tx, c->tz,
                          (long long)(c->edge[c->seg[marked[g]->seg].edge].key >> 16), SC_LM_STUB_M);
            c->st.bld_lm_stub += nm;
            return 0;
        }
        for (g = 0; g < nm; g++) marked[g]->lm_ok = 0;
        gap = 40.0;                          /* off the surface itself, not off a pavement */
    }
    for (it = 0; it < 3; it++) {
        double cx = 0.0, cz = 0.0, push = 0.0, nx, nz;
        if (!sc_on_road(c, rx, rz, n, &hit)) { *shift_out = total; return 0; }
        if (lm_wins) {
            if (hit->seg >= 0)
                TD5_LOG_I(LOG_TAG, "[ROAM LM] tile %d,%d landmark outline meets ribbon of way %lld (edge key %llx, half %.0f)",
                          c->tx, c->tz, (long long)(c->edge[c->seg[hit->seg].edge].key >> 16),
                          (unsigned long long)c->edge[c->seg[hit->seg].edge].key, c->seg[hit->seg].hw);
            else
                TD5_LOG_I(LOG_TAG, "[ROAM LM] tile %d,%d landmark outline meets a junction patch triangle at local (%.0f,%.0f)",
                          c->tx, c->tz, (hit->x[0] + hit->x[1] + hit->x[2]) / 3.0, (hit->z[0] + hit->z[1] + hit->z[2]) / 3.0);
        }
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

/* the plaza veto: most of the outline in a plaza (ar: the areas the 3x3 owns), not a landmark, not a small OSM
 * building (the strip's rule, td5_geo_buildings.c geob_veto_plaza) */
static int sc_bld_plaza_veto(const ScArea *ar, int nar, const TD5_GeoBuilding *gb, const double *px, const double *pz)
{
    int in = 0, np = 0, k;
    if (!td5_env_flag_on("TD5RE_GEO_PLAZA_BLD") || nar <= 0 || gb->landmark) return 0;
    for (k = -1; k < gb->n; k++) {
        np++;
        in += sc_in_plaza(ar, nar, k < 0 ? gb->cx : px[k], k < 0 ? gb->cz : pz[k]);
    }
    return in * 2 > np && !(gb->fsrc == TD5_GEOB_FSRC_OSM && sc_ring_area_m2(px, pz, gb->n) < SC_PLAZA_KEEP_M2);
}

/* [IC] THE GREEN VETO. Mariano, 2026-10-20: "these buildings are in a park/plaza, they should not be there". The
 * plaza veto above only reads the areas OWNED by the 3x3 (a big park whose centre lies farther, Paseo del Bosque, is
 * not in ar) and knows nothing of the plazas OSM maps only as their ring road (Plaza Italia) or of the avenue
 * medians (pv_open_loops). A footprint the map does not draw itself (Overture's: ML detections, mostly tree crowns
 * and kiosk-sized blobs in a park) whose outline stands mostly (centre + vertices) on a plaza polygon of c->lotp is
 * not built. OSM's own footprints stand (a real building in a park: the census lists them). */
static int sc_bld_green_veto(const TG_TileCtx *c, const TD5_GeoBuilding *gb, const double *px, const double *pz)
{
    int in = 0, np = 0, k, i;
    if (gb->fsrc == TD5_GEOB_FSRC_OSM || gb->landmark || gb->part || !td5_env_int("TD5RE_ROAM_GREEN_VETO", 1, 0, 1))
        return 0;
    for (k = -1; k < gb->n; k++) {
        const double x = k < 0 ? gb->cx : px[k], z = k < 0 ? gb->cz : pz[k];
        np++;
        for (i = 0; i < c->nlotp; i++) {
            const ScLotPoly *P = &c->lotp[i];
            if (!P->plaza || x < P->x0 || x > P->x1 || z < P->z0 || z > P->z1) continue;
            if (td5_geob_point_in_ring(P->px, P->pz, P->n, x, z)) { in++; break; }
        }
    }
    return in * 2 > np;
}

/* [IC] Would sc_emit_buildings draw this footprint (place-frame ring px, pz)? The pavement and the infill read the
 * footprints as "the frontage is taken" (pv_bld_build): one the emitter drops (past the shift cap, on a road, a
 * veto) must not be. Mariano, 2026-10-20, Av 53 / calle 16: "these blocks are practically empty". A 6714 m2
 * Overture blob covering the block was dropped past the shift cap, yet the infill refused every lot on it ("real
 * building") and the pavement stopped at its facade: neither stood. Same tests in the same order as the emitter
 * (landmarks and parts kept: their own clearance runs elsewhere). 1 = it does not stand. */
static int sc_bld_drops(TD5_RoamScnSource *s, TG_TileCtx *c, const TD5_GeoBuilding *gb, const double *px,
                        const double *pz, const ScArea *ar, int nar)
{
    double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], shift = 0.0;
    ScBldU u;
    int k;
    const int n = gb->n;
    if (gb->landmark || gb->part) return 0;
    if (n < 3 || n > TD5_GEOB_RING_MAX) return 1;
    if (sc_bld_plaza_veto(ar, nar, gb, px, pz) || sc_bld_green_veto(c, gb, px, pz)) return 1;
    for (k = 0; k < n; k++) { rx[k] = px[k] - (double)c->org_x; rz[k] = pz[k] - (double)c->org_z; }
    if (!td5_geob_ring_simple(rx, rz, n)) return 1;
    for (k = 0; k < n; k++) if (!sc_in_reach(c, rx[k], rz[k])) return 0;   /* beyond the 3x3: not ours to judge */
    memset(&u, 0, sizeof u);
    u.c = c;
    return sc_bld_clear(c, &u, s->gap, rx, rz, n, &shift, 0) != 0;
}

/* [IC] TD5RE_ROAM_BLD_LOG=1: what became of one footprint (place frame centroid), for the offline census */
static void sc_bld_note(const TD5_RoamScnSource *s, const TG_TileCtx *c, const ScBld *b, const char *fate)
{
    if (!s->bld_log) return;
    TD5_LOG_I(LOG_TAG, "ROAM_BLD tile %d,%d id %.0f src %d c (%.0f,%.0f) area %.0f h %.1f: %s", c->tx, c->tz, b->id,
              (int)b->b->fsrc, b->b->cx, b->b->cz, b->b->area_m2, b->b->height / SC_UPM, fate);
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
        if (n < 3 || n > TD5_GEOB_RING_MAX) { c->st.bld_drop_ring++; sc_bld_note(s, c, &set[i], "bad ring"); continue; }
        /* the plaza veto: most of the outline in a plaza, not a landmark, not a small OSM building */
        if (sc_bld_plaza_veto(ar, nar, gb, set[i].px, set[i].pz)) {
            c->st.bld_veto_plaza++;
            sc_bld_note(s, c, &set[i], "plaza veto");
            continue;
        }
        if (sc_bld_green_veto(c, gb, set[i].px, set[i].pz)) {          /* [IC] park / ring plaza / avenue median */
            c->st.bld_veto_green++;
            sc_bld_note(s, c, &set[i], "green veto");
            continue;
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
        if (veto) { c->st.bld_veto_lm++; sc_bld_note(s, c, &set[i], "landmark veto"); continue; }
        for (k = 0; k < n; k++) { rx[k] = set[i].px[k] - (double)c->org_x; rz[k] = set[i].pz[k] - (double)c->org_z; }
        if (!td5_geob_ring_simple(rx, rz, n)) { c->st.bld_drop_ring++; sc_bld_note(s, c, &set[i], "ring not simple"); continue; }
        for (k = 0; k < n && sc_in_reach(c, rx[k], rz[k]); k++) { }
        if (k < n) { c->st.bld_drop_ring++; sc_bld_note(s, c, &set[i], "out of reach"); continue; }
        for (k = 0; k < n; k++) { cx += rx[k]; cz += rz[k]; }
        cx /= n; cz /= n;
        memset(&u, 0, sizeof u);
        u.c = c;
        u.host = roam_nearest_edge(c, cx, cz, SC_HOST_MAX_M * SC_UPM, &edge, &side, &along, &lat)
              && roam_edge_frame(c, edge, side, along, &u.f);
        u.side = side;
        u.minout = u.host ? u.f.half_carriage + s->gap : 0.0;
        u.gap = s->gap;
        r = sc_bld_clear(c, &u, s->gap, rx, rz, n, &shift,
                         s->lm_wins && (gb->landmark || kind != TD5_GEOB_LMK_NONE));
        if (gb->landmark && !gb->part)       /* one line per landmark outline: what became of it */
            TD5_LOG_I(LOG_TAG, "[ROAM LM] tile %d,%d way_hash=%08x kind=%d anchor=%d host=%d half=%.0f minout=%.0f "
                      "area=%.0fm2 h=%.0f: %s (shift %.0f)", c->tx, c->tz, gb->id_hash, kind, anchor, u.host,
                      u.f.half_carriage, u.minout, gb->area_m2, gb->height,
                      r == 2 ? "DROPPED past the shift cap" : r == 1 ? "DROPPED on a road" : "kept", shift);
        if (r == 2) { c->st.bld_drop_shift++; sc_bld_note(s, c, &set[i], "past the shift cap"); continue; }
        if (r == 1) { c->st.bld_drop_road++; sc_bld_note(s, c, &set[i], "on a road"); continue; }
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
        e.apron_quad_ok = sc_bld_apron_quad_ok;
        e.si = -1;
        e.acct = 0;
        e.st = &c->bst;
        {
            const long emitted0 = c->bst.emitted, prefab0 = c->bst.lm_prefab, apron0 = c->bst.lm_apron;
            if (!tg_geo_emit_building_core(&e, gb, rx, rz, n, cx, cz, shift) || b->blk.oom) {
                free(set); free(fc); return 0;
            }
            if (c->bst.emitted > emitted0) c->st.bld_emitted++;
            sc_bld_note(s, c, &set[i], c->bst.emitted > emitted0 ? "emitted" : "core refused");
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

static int sc_plane_push(TG_TileCtx *c, const double *p, int lvl, int extra)
{
    if (lvl <= 0) {
        /* [G5] refined where sc_tri_split refines the drawn lawn (a piece on the road, `extra` more times), so the
         * plane a path reads is the lawn surface as drawn: next to a road the ground is lowered and a coarser
         * plane missed the drawn lawn by up to a metre (the road pieces are kept: a plane, never drawn) */
        const double x[3] = { p[0], p[2], p[4] }, z[3] = { p[1], p[3], p[5] };
        if (extra > 0 && sc_in_reach(c, x[0], z[0]) && sc_in_reach(c, x[1], z[1]) && sc_in_reach(c, x[2], z[2]) &&
            sc_on_road(c, x, z, 3, NULL)) {
            lvl = 1; extra--;
        }
    }
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
        return sc_plane_push(c, t0, lvl - 1, extra) && sc_plane_push(c, t1, lvl - 1, extra)
            && sc_plane_push(c, t2, lvl - 1, extra) && sc_plane_push(c, t3, lvl - 1, extra);
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
            if (!sc_plane_push(c, pp, lvl, 2)) return 0;      /* 2 = sc_emit_lawn's sc_tri_split extra */
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

/* [G5] the highest pavement slab / island surface over (x, z) (the quad as drawn: 0 1 2, 0 2 3); 0 when none */
static int sc_pv_y(const TG_TileCtx *c, double x, double z, double *y)
{
    int q, t, hit = 0;
    for (q = 0; q < c->npvq; q++) {
        const double *P = &c->pvq[q * 12];
        if (x < P[0] && x < P[3] && x < P[6] && x < P[9]) continue;
        if (x > P[0] && x > P[3] && x > P[6] && x > P[9]) continue;
        if (z < P[2] && z < P[5] && z < P[8] && z < P[11]) continue;
        if (z > P[2] && z > P[5] && z > P[8] && z > P[11]) continue;
        for (t = 0; t < 2; t++) {
            const double *A = P, *B = P + 3 * (1 + t), *C = P + 3 * (2 + t);
            const double d = (B[2] - C[2]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[2] - C[2]);
            double w0, w1, w2, h;
            if (fabs(d) < 1e-9) continue;
            w0 = ((B[2] - C[2]) * (x - C[0]) + (C[0] - B[0]) * (z - C[2])) / d;
            w1 = ((C[2] - A[2]) * (x - C[0]) + (A[0] - C[0]) * (z - C[2])) / d;
            w2 = 1.0 - w0 - w1;
            if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
            h = w0 * A[1] + w1 * B[1] + w2 * C[1];
            if (!hit || h > *y) *y = h;
            hit = 1;
        }
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
                    /* [G5] on a pavement slab or a median / traffic island: on top of it, the same tier */
                    if (sc_pv_y(c, qx[q], qz[q], &yl) && yl + SC_GEOP_TIER > y) { y = yl + SC_GEOP_TIER; c->st.fp_on_pv++; }                    bt->v[o + 0] = (float)qx[q];
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
 * SECTION: pavement (F4b)
 *
 * WHAT. Along every kerb of the drivable surface this tile owns (a ribbon quad's
 * two kerbs, a junction patch's kerb outline with its fillets, a dead-end cap): a
 * pavement SLAB at kerb height, the KERB FACE down to the road, a BACK FACE down
 * under the drawn ground, and a FENCE where nothing stands behind it (a railing on
 * a bridge deck). One rule for all of them: the slab is the band between the kerb
 * line and its offset by the local pavement width, so a street's slab turns the
 * junction corner along the patch outline and meets the next street's slab.
 *
 * THE BOUNDARY CHAIN. A tile's BEDG records (td5_roam_tile.h) are exactly the
 * outline pieces of its drivable surface. The pieces of the 3x3 ROAD tiles are
 * chained head to tail on their WORLD integer vertices (shared bit for bit across
 * tiles, and between a ribbon's cut row and its patch), oriented with the road on
 * the left; a piece that lies inside another drivable primitive is not a kerb and
 * is left out. A chain is offset as a whole: each piece's outer line is its kerb
 * line moved out by the width at its two ends, and consecutive outer lines meet at
 * their mitre. A JUNCTION CORNER is a run of short outline pieces (kerb, fillet
 * arc, kerb) round a block corner; where the pavement is wider than the fillet the
 * arc's outer lines REVERSE, those pieces collapse, and the two streets' outer
 * lines meet in one point, the building-line corner (pv_offset_chain). So the
 * corner piece is the patch outline's own slab, emitted by the tile that owns the
 * cluster. Pieces a neighbour tile owns are inputs only (each piece is emitted by
 * its owner), and every result is a function of the chain round the piece, so two
 * tiles agree on a shared vertex.
 *
 * WIDTH per (edge, side): td5_geo_sw_resolve over the way's OSM facts (class, name
 * kind, sidewalk=* presence and measured per-side width, the built half
 * carriageway), clamped like the strip (0.75 .. 6 m). OSM's left of a way is its
 * (-dz, +dx) normal (td5_tg_city.c tg_geo_sw_sides). A private service way, a
 * driveway or an alley without a sidewalk tag gets the kerb strip; a motorway gets
 * nothing. Patch outline pieces take the widths of the ribbons round them (by arc
 * length). Then per vertex: never past HALF the free distance to another drivable
 * surface, so the two sides of a median, a traffic island or a sliver between two
 * roads meet in the middle (the island is filled and kerbed on both sides), and
 * never under a mapped building front (clipped there, not closer to the kerb than
 * the buildings' own clearance gap). A median between the two one-way carriageways
 * of ONE named street is a lawn island; any other island is paved.
 *
 * GUARD. The slab's kerb edge stands PV_KERB_IN off the kerb line (1 cm), so no
 * face lies on the drivable surface by construction; every piece is re-checked
 * exactly, cut back, then dropped if it still meets one, and the census over the
 * bytes is the judge (st.guard_overlaps).
 * ========================================================================== */

#define PV_LRU          24
#define PV_KERB_IN      4.0          /* slab kerb edge off the kerb line, units (1 cm)    */
#define PV_MITRE_MAX    2.5          /* outer-corner reach cap, multiples of the width   */
#define PV_COLLAPSE_SEG 1.25         /* a piece longer than this many widths never folds */
#define PV_COLLAPSE_RUN 3.5          /* a folded corner run is at most this many widths  */
#define PV_FALL         0.25         /* TG_NET_FW_FALL: steepest cross-fall of a slab    */
#define PV_FALL_LOW     0.06         /* [G4b] steepest fall away from the kerb where the drawn ground is
                                      * lowered under the road (a cut): a street camber, the back face
                                      * (down to the lowered ground) covers the rest                  */
#define PV_BACK_SINK    344.0        /* back face reaches 0.8 m under the raster ground:
                                      * the drawn ground is dropped 0.4 m under it (roam
                                      * render GROUND_DROP) and interpolated per 16 m   */
#define PV_W_MIN        320.0        /* TD5_TG_GEO_SW_MIN (0.75 m)                       */
#define PV_W_MAX        2600.0       /* TD5_TG_GEO_SW_MAX (6 m)                          */
#define PV_W_DEFAULT    860.0        /* 2 m: an outline no ribbon width reaches          */
#define PV_W_SKIP       200.0        /* narrower than this (0.47 m, a sliver between two roads): none */
#define PV_MEDIAN_M     25.0         /* an opposite carriageway this near: a median      */
#define PV_LAWN_MIN_M   2.5          /* a median narrower than this is paved             */
#define PV_RAY_STEP     200.0
#define PV_FACE_STEP    215.0        /* 0.5 m facade march                               */
#define PV_INTERP_M     150.0        /* patch widths reach this far for a ribbon width   */
#define PV_MERGE_M      30.0         /* longest merged straight piece                    */
#define PV_MERGE_TOL    6.0          /* units off the straight line a merge may hide     */
#define PV_DECK_FASCIA  400.0
#define PV_FENCE_PROBE  (1.5 * SC_UPM)
#define PV_CHAIN_MAX    8192
#define PV_UV           (1.0 / 1500.0)   /* paving: one repeat per span length            */
#define PV_UV_LAWN      (1.0 / 3400.0)   /* the F4a lawn's repeat                          */

#define PV_WF_PRIVATE   0x1u         /* access = private | no                            */
#define PV_WF_NOWALK    0x2u         /* service = driveway | alley | parking_aisle | drive-through */
#define PV_WF_RING      0x4u         /* [HD] junction = roundabout | circular: its inner kerb loop is a plaza */

/* vertex flags */
#define PV_VF_DECK      0x01u
#define PV_VF_FLAT      0x02u        /* bore / building passage: no ground follow        */
#define PV_VF_ISLAND    0x04u        /* filled to the middle of a gap: no back, no fence  */
#define PV_VF_MEDIAN    0x08u        /* ... and it is a named avenue's median: lawn       */
#define PV_VF_FACADE    0x10u        /* clipped at a building front: no fence             */
#define PV_VF_COLLAPSE  0x20u        /* merged into a corner point                        */

typedef struct {
    int64_t   id;
    int       klass, namek, sidewalk, oneway;
    unsigned  flags;                 /* PV_WF_*                                           */
    uint32_t  name_h;                /* FNV of the name, 0 = unnamed                      */
    double    tag_l, tag_r, half_carriage_m;
    int       first, count;
    const double *px, *pz;           /* place frame, set once the file is loaded          */
} PvWay;

typedef struct {
    int       used, ix, iz, pin;
    uint32_t  clock;
    PvWay    *w;
    int       n;
    double   *px, *pz;
    int       npt, cap;
} PvFile;

struct ScPvSrc {
    ScIdxRow *rows;
    int       nrows;
    PvFile    f[PV_LRU];
    uint32_t  clock;
    int       on, fence, facade, merge, median;
};

static void pv_file_free(PvFile *f)
{
    free(f->w); free(f->px); free(f->pz);
    memset(f, 0, sizeof *f);
}

static struct ScPvSrc *sc_pv_open(TD5_RoamScnSource *s)
{
    struct ScPvSrc *p = (struct ScPvSrc *)calloc(1, sizeof *p);
    char path[512];
    char *json;
    cJSON *root;
    const cJSON *tl, *t;
    if (!p) return NULL;
    p->on     = td5_env_int("TD5RE_ROAM_SCN_PAVEMENT", 1, 0, 1);
    p->fence  = td5_env_int("TD5RE_ROAM_PV_FENCE", 1, 0, 1);
    p->facade = td5_env_int("TD5RE_ROAM_PV_FACADE", 1, 0, 1);
    p->merge  = td5_env_int("TD5RE_ROAM_PV_MERGE", 1, 0, 1);
    p->median = td5_env_int("TD5RE_ROAM_PV_MEDIAN", 1, 0, 1);
    /* the place's building-line table (td5_geo_sidewalk.c), selected on THIS thread
     * before the first resolve, so its lazy generic default is never taken */
    if (strcmp(td5_geo_sw_place_name(), s->place) != 0) td5_geo_sw_place(s->place);
    td5_geo_source_path(path, sizeof path, s->place, "tiles/INDEX.JSON");
    json = sc_slurp(path, NULL);
    root = json ? cJSON_Parse(json) : NULL;
    free(json);
    tl = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "layers"), "ROADS"), "tiles");
    if (tl && cJSON_IsArray(tl)) {
        p->rows = (ScIdxRow *)calloc((size_t)cJSON_GetArraySize(tl) + 1u, sizeof(ScIdxRow));
        for (t = tl->child; t && p->rows; t = t->next) {
            const cJSON *a = t->child;
            double v[7];
            int k;
            for (k = 0; k < 7 && a; k++, a = a->next) v[k] = a->valuedouble;
            if (k < 7) continue;
            td5_geo_src_bbox_to_game(&v[3], &v[5]);   /* [roam M] the extent in the game frame */
            p->rows[p->nrows].ix = (int)v[0]; p->rows[p->nrows].iz = (int)v[1];
            p->rows[p->nrows].x0 = v[3]; p->rows[p->nrows].z0 = v[4];
            p->rows[p->nrows].x1 = v[5]; p->rows[p->nrows].z1 = v[6];
            p->nrows++;
        }
    }
    cJSON_Delete(root);
    TD5_LOG_I(LOG_TAG, "roam pavement: %s, %d ROADS index row(s), fence=%d facade=%d merge=%d median=%d, "
              "building-line table '%s'", p->on ? "on" : "OFF", p->nrows, p->fence, p->facade, p->merge,
              p->median, td5_geo_sw_place_name());
    return p;
}

static void sc_pv_close(struct ScPvSrc *p)
{
    int k;
    if (!p) return;
    for (k = 0; k < PV_LRU; k++) pv_file_free(&p->f[k]);
    free(p->rows);
    free(p);
}

static const char *pv_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static uint32_t pv_name_hash(const char *nm)
{
    uint32_t h;
    if (!nm || !nm[0]) return 0;
    h = sc_fnv(2166136261u, nm, strlen(nm));
    return h ? h : 1u;
}

/* One ROADS cache tile: the facts the width needs, per way, plus its polyline. */
static void pv_file_load(TD5_RoamScnSource *s, PvFile *f, int ix, int iz)
{
    char rel[96], path[512];
    char *json;
    cJSON *root;
    const cJSON *arr, *e;
    int cap, k;
    double *px, *pz;
    pv_file_free(f);
    f->used = 1; f->ix = ix; f->iz = iz;
    snprintf(rel, sizeof rel, "tiles/ROADS_%d_%d.JSON", ix, iz);
    td5_geo_source_path(path, sizeof path, s->place, rel);
    json = sc_slurp(path, NULL);
    if (!json) return;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return;
    td5_geo_src_json_to_game(root);          /* [roam M] source frame -> game frame */
    arr = cJSON_GetObjectItem(root, "roads");
    cap = arr ? cJSON_GetArraySize(arr) : 0;
    px = (double *)malloc(TD5_GEO_ROADS_WAY_POINTS_MAX * 2 * sizeof(double));
    pz = px ? px + TD5_GEO_ROADS_WAY_POINTS_MAX : NULL;
    if (cap > 0 && px) f->w = (PvWay *)calloc((size_t)cap, sizeof(PvWay));
    for (e = arr ? arr->child : NULL; e && f->w; e = e->next) {
        const cJSON *v = cJSON_GetObjectItem(e, "id"), *tg = cJSON_GetObjectItem(e, "tags");
        const char *acc = pv_str(tg, "access"), *svc = pv_str(tg, "service");
        double bb[4];
        TD5_GeoRoad r;
        PvWay *w = &f->w[f->n];
        int np;
        if (!v || !cJSON_IsNumber(v)) continue;
        np = td5_geo_roads_parse_points(cJSON_GetObjectItem(e, "points"), TD5_GEO_ROADS_WAY_POINTS_MAX, px, pz, bb);
        if (np < 2) continue;
        memset(&r, 0, sizeof r);
        if (!td5_geo_roads_parse_record(e, s->upm, &r)) continue;
        if (f->npt + np > f->cap) {
            int nc = f->cap ? f->cap * 2 : 8192;
            double *nx, *nz;
            while (nc < f->npt + np) nc *= 2;
            nx = (double *)realloc(f->px, (size_t)nc * sizeof(double));
            if (!nx) break;
            f->px = nx;
            nz = (double *)realloc(f->pz, (size_t)nc * sizeof(double));
            if (!nz) break;
            f->pz = nz;
            f->cap = nc;
        }
        memcpy(f->px + f->npt, px, (size_t)np * sizeof(double));
        memcpy(f->pz + f->npt, pz, (size_t)np * sizeof(double));
        w->id = (int64_t)v->valuedouble;
        w->klass = r.klass; w->namek = r.namek; w->sidewalk = r.sidewalk; w->oneway = r.oneway;
        w->tag_l = r.sw_tag_l_m; w->tag_r = r.sw_tag_r_m;
        w->half_carriage_m = (r.tag_width_m > 0.0) ? r.tag_width_m * 0.5
                                                   : (double)r.lanes * TD5_GEO_ROADS_LANE_M * 0.5;
        w->flags = 0;
        if (acc && (!strcmp(acc, "private") || !strcmp(acc, "no"))) w->flags |= PV_WF_PRIVATE;
        if (svc && (!strcmp(svc, "driveway") || !strcmp(svc, "alley") || !strcmp(svc, "parking_aisle") ||
                    !strcmp(svc, "drive-through"))) w->flags |= PV_WF_NOWALK;
        w->name_h = pv_name_hash(pv_str(e, "name"));
        {
            const char *jn = pv_str(e, "junction");
            if (cJSON_IsTrue(cJSON_GetObjectItem(e, "roundabout")) ||
                (jn && (!strcmp(jn, "roundabout") || !strcmp(jn, "circular")))) w->flags |= PV_WF_RING;
        }
        w->first = f->npt; w->count = np;
        f->npt += np;
        f->n++;
    }
    free(px);
    cJSON_Delete(root);
    for (k = 0; k < f->n; k++) { f->w[k].px = f->px + f->w[k].first; f->w[k].pz = f->pz + f->w[k].first; }
}

static int pv_way_cmp(const void *pa, const void *pb)
{
    const PvWay *a = *(const PvWay *const *)pa, *b = *(const PvWay *const *)pb;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    if (a->px[0] != b->px[0]) return a->px[0] < b->px[0] ? -1 : 1;
    return a->pz[0] < b->pz[0] ? -1 : (a->pz[0] > b->pz[0]);
}

/* Every way of the ROADS cache tiles that meet [x0,x1]x[z0,z1] (place frame; a way
 * is stored whole in one tile whose INDEX extent covers it), sorted by id. */
static PvWay **pv_window(TD5_RoamScnSource *s, double x0, double z0, double x1, double z1, int *nout)
{
    struct ScPvSrc *p = s->pv;
    PvWay **list = NULL;
    int r, k, n = 0, cap = 0;
    *nout = 0;
    for (k = 0; k < PV_LRU; k++) p->f[k].pin = 0;
    for (r = 0; r < p->nrows; r++) {
        const ScIdxRow *row = &p->rows[r];
        PvFile *hit = NULL, *victim = NULL;
        if (row->x1 < x0 || row->x0 > x1 || row->z1 < z0 || row->z0 > z1) continue;
        for (k = 0; k < PV_LRU; k++)
            if (p->f[k].used && p->f[k].ix == row->ix && p->f[k].iz == row->iz) { hit = &p->f[k]; break; }
        if (!hit) {
            for (k = 0; k < PV_LRU; k++) {
                PvFile *f = &p->f[k];
                if (f->pin) continue;
                if (!f->used) { victim = f; break; }
                if (!victim || f->clock < victim->clock) victim = f;
            }
            if (!victim) {
                TD5_LOG_W(LOG_TAG, "ROAM_PV_CACHE_FULL ROADS_%d_%d not loaded (%d slots pinned)", row->ix, row->iz, PV_LRU);
                continue;
            }
            pv_file_load(s, victim, row->ix, row->iz);
            hit = victim;
        }
        hit->clock = ++p->clock;
        hit->pin = 1;
        if (!sc_grow((void **)&list, &cap, n + hit->n + 1, sizeof(PvWay *))) { free(list); return NULL; }
        for (k = 0; k < hit->n; k++) list[n++] = &hit->w[k];
    }
    if (n > 1) qsort(list, (size_t)n, sizeof(PvWay *), pv_way_cmp);
    *nout = n;
    return list;
}

static const PvWay *pv_find(PvWay *const *list, int n, int64_t id)
{
    int a = 0, b = n;
    while (a < b) { const int m = (a + b) / 2; if (list[m]->id < id) a = m + 1; else b = m; }
    return (a < n && list[a]->id == id) ? list[a] : NULL;
}

/* the way's unit direction at its segment nearest (x, z), place frame */
static void pv_way_dir(const PvWay *w, double x, double z, double *dx, double *dz)
{
    int k, best = 0;
    double bd = 1e300, l;
    for (k = 0; k + 1 < w->count; k++) {
        const double d = sc_seg_dist(x, z, w->px[k], w->pz[k], w->px[k + 1], w->pz[k + 1], NULL);
        if (d < bd) { bd = d; best = k; }
    }
    *dx = w->px[best + 1] - w->px[best];
    *dz = w->pz[best + 1] - w->pz[best];
    l = hypot(*dx, *dz);
    if (l > 1e-9) { *dx /= l; *dz /= l; } else { *dx = 1.0; *dz = 0.0; }
}

/* The pavement width of one (way, OSM side), units; 0 = no pavement at all. */
static double pv_width(const TD5_RoamScnSource *s, const PvWay *w, const RoamEdgeRec *er, int osm_left, int *src)
{
    TD5_GeoSwIn in;
    double m;
    memset(&in, 0, sizeof in);
    in.klass = w ? w->klass : er->klass;
    *src = TD5_GEO_SWSRC_NONE;
    if (in.klass == TD5_GEO_RC_MOTORWAY) return 0.0;
    in.namek = w ? w->namek : TD5_GEO_NAMEK_UNKNOWN;
    in.half_carriage_m = w ? w->half_carriage_m : (double)er->lanes * TD5_GEO_ROADS_LANE_M * 0.5;
    in.half_road_m = (double)er->lanes * SC_ROW * 0.5 / s->upm;
    in.present = 1;
    if (w) {
        switch (w->sidewalk) {
        case TD5_GEO_SW_NONE:  in.present = 0; break;
        case TD5_GEO_SW_LEFT:  in.present = osm_left; break;
        case TD5_GEO_SW_RIGHT: in.present = !osm_left; break;
        case TD5_GEO_SW_BOTH:  in.present = 1; break;
        default:               in.present = !(w->flags & (PV_WF_PRIVATE | PV_WF_NOWALK)); break;
        }
        in.tag_m = osm_left ? w->tag_l : w->tag_r;
    }
    m = td5_geo_sw_resolve(&in, src) * s->upm;
    if (m < PV_W_MIN) m = PV_W_MIN;
    if (m > PV_W_MAX) m = PV_W_MAX;
    return floor(m / 10.0 + 0.5) * 10.0;
}

/* ---- the boundary pieces of the 3x3 ---- */

enum { PV_K_RIB = 1, PV_K_CAP = 2, PV_K_PATCH = 3 };

typedef struct {
    double    ax, az, ay, bx, bz, by;   /* ctx tile-local; the road on the LEFT of a->b    */
    int64_t   ka, kb;                   /* world (x, z) vertex keys                        */
    double    nx, nz, len;              /* outward unit normal (right of a->b)             */
    int       kind, owned, deck, flat, klass;
    double    w;                        /* RIB: resolved width (units), else -1            */
    const PvWay *way;                   /* RIB: its way (NULL when the cache lacks it)     */
    uint64_t  ekey;                     /* RIB: the edge                                   */
    int       next, prev, done;
} PvB;

typedef struct { int64_t k; int i; } PvKI;

static int pv_ki_cmp(const void *pa, const void *pb)
{
    const PvKI *a = (const PvKI *)pa, *b = (const PvKI *)pb;
    if (a->k != b->k) return a->k < b->k ? -1 : 1;
    return a->i < b->i ? -1 : (a->i > b->i);
}

static int64_t pv_vkey(int64_t X, int64_t Z) { return (int64_t)(((uint64_t)(uint32_t)(int32_t)X << 32) | (uint32_t)(int32_t)Z); }

/* The kerb pieces of the 3x3 ROAD tiles (interior ones left out), linked head to tail. */
static int pv_gather(TD5_RoamScnSource *s, TG_TileCtx *c, PvWay *const *ways, int nways, PvB **out, int *nout)
{
    PvB *B = NULL;
    PvKI *ki = NULL;
    int nb = 0, cap = 0, ti, i;
    *out = NULL; *nout = 0;
    for (ti = 0; ti < 9; ti++) {
        const RoamTile *t = c->road[ti];
        int *ser;
        uint32_t e, bi;
        if (!t) continue;
        ser = (int *)malloc((size_t)(t->nspan > 0 ? t->nspan : 1) * sizeof(int));
        if (!ser) { free(B); return 0; }
        for (bi = 0; bi < t->nspan; bi++) ser[bi] = -1;
        for (e = 0; e < t->nedge; e++)
            for (bi = 0; bi < t->edge[e].span_count; bi++)
                if ((uint32_t)t->edge[e].span_first + bi < t->nspan) ser[t->edge[e].span_first + bi] = (int)e;
        for (bi = 0; bi < t->nbedge; bi++) {
            const RoamBEdge *be = &t->bedge[bi];
            const RoamEdgeRec *er = NULL;
            int32_t ox, oy, oz;
            int kind, k;
            double rx = 0.0, rz = 0.0, x[2], y[2], z[2], cr;
            int64_t X[2], Z[2];
            PvB *b;
            if (be->kind != ROAM_BEDGE_KERB && be->kind != ROAM_BEDGE_DECK) continue;
            if (be->v[0] >= t->nvert || be->v[1] >= t->nvert) continue;
            if (be->prim & ROAM_PRIM_TRI) {
                const uint32_t ix = (uint32_t)(be->prim & 0x7FFFu);
                const RoamTri *tr;
                if (ix >= t->ntri) continue;
                tr = &t->tri[ix];
                if (tr->v[0] >= t->nvert || tr->v[1] >= t->nvert || tr->v[2] >= t->nvert) continue;
                ox = tr->ox; oy = tr->oy; oz = tr->oz;
                for (k = 0; k < 3; k++) {
                    rx += (double)t->org_x + ox + t->vert[tr->v[k]].x - c->org_x;
                    rz += (double)t->org_z + oz + t->vert[tr->v[k]].z - c->org_z;
                }
                rx /= 3.0; rz /= 3.0;
                kind = PV_K_PATCH;
            } else {
                const TD5_StripSpan *sp;
                uint32_t L, R, n;
                if (be->prim >= t->nspan) continue;
                sp = &t->span[be->prim];
                n = (uint32_t)(sp->pad_02[1] & 0x0Fu);
                L = sp->left_vertex_index; R = sp->right_vertex_index;
                if (n < 1 || L + n >= t->nvert || R + n >= t->nvert) continue;
                ox = sp->origin_x; oy = sp->origin_y; oz = sp->origin_z;
                rx = 0.25 * ((double)t->vert[L].x + t->vert[L + n].x + t->vert[R].x + t->vert[R + n].x)
                   + (double)t->org_x + ox - c->org_x;
                rz = 0.25 * ((double)t->vert[L].z + t->vert[L + n].z + t->vert[R].z + t->vert[R + n].z)
                   + (double)t->org_z + oz - c->org_z;
                kind = ((uint32_t)be->v[1] == (uint32_t)be->v[0] + n) ? PV_K_CAP : PV_K_RIB;
                if (ser[be->prim] >= 0) er = &t->edge[ser[be->prim]];
            }
            for (k = 0; k < 2; k++) {
                const TD5_StripVertex *v = &t->vert[be->v[k]];
                X[k] = (int64_t)t->org_x + ox + v->x;
                Z[k] = (int64_t)t->org_z + oz + v->z;
                x[k] = (double)(X[k] - c->org_x);
                z[k] = (double)(Z[k] - c->org_z);
                y[k] = (double)oy + v->y;
            }
            if (!sc_grow((void **)&B, &cap, nb + 1, sizeof(PvB))) { free(ser); free(B); return 0; }
            b = &B[nb];
            memset(b, 0, sizeof *b);
            cr = (x[1] - x[0]) * (rz - z[0]) - (z[1] - z[0]) * (rx - x[0]);
            k = cr >= 0.0 ? 0 : 1;               /* the road on the left of a -> b */
            b->ax = x[k]; b->az = z[k]; b->ay = y[k]; b->ka = pv_vkey(X[k], Z[k]);
            b->bx = x[1 - k]; b->bz = z[1 - k]; b->by = y[1 - k]; b->kb = pv_vkey(X[1 - k], Z[1 - k]);
            b->len = hypot(b->bx - b->ax, b->bz - b->az);
            if (!(b->len > 1.0)) continue;
            b->nx = (b->bz - b->az) / b->len; b->nz = -(b->bx - b->ax) / b->len;
            {   /* a piece inside another drivable primitive is no kerb */
                const double qx = 0.5 * (b->ax + b->bx) + b->nx * 40.0, qz = 0.5 * (b->az + b->bz) + b->nz * 40.0;
                if (sc_on_road(c, &qx, &qz, 1, NULL)) { if (ti == 4) c->st.pv_interior++; continue; }
            }
            b->kind = kind;
            b->owned = (ti == 4);
            b->deck = (be->kind == ROAM_BEDGE_DECK) || (er && (er->flags & ROAM_EF_DECK));
            b->flat = er && (er->flags & (ROAM_EF_BORE | ROAM_EF_PASSAGE));
            b->klass = er ? er->klass : -1;
            b->w = -1.0;
            b->next = b->prev = -1;
            if (kind == PV_K_RIB && er) {
                const PvWay *w = pv_find(ways, nways, (int64_t)(er->key >> 16));
                double dx = 1.0, dz = 0.0;
                int src = TD5_GEO_SWSRC_NONE, left;
                if (w) pv_way_dir(w, rx + (double)c->org_x, rz + (double)c->org_z, &dx, &dz);
                left = ((0.5 * (b->ax + b->bx) - rx) * (-dz) + (0.5 * (b->az + b->bz) - rz) * dx) > 0.0;
                b->w = pv_width(s, w, er, left, &src);
                b->way = w;
                b->ekey = er->key;
                if (b->owned) {
                    if (src >= 0 && src < 7) c->st.pv_src[src]++;
                    if (!w) c->st.pv_noway++;
                }
            }
            if (b->owned) c->st.pv_bedges++;
            nb++;
        }
        free(ser);
    }
    /* link: the unique piece that starts where this one ends (same level) */
    ki = (PvKI *)malloc((size_t)(nb > 0 ? nb : 1) * sizeof(PvKI));
    if (!ki) { free(B); return 0; }
    for (i = 0; i < nb; i++) { ki[i].k = B[i].ka; ki[i].i = i; }
    qsort(ki, (size_t)nb, sizeof(PvKI), pv_ki_cmp);
    for (i = 0; i < nb; i++) {
        PvB *b = &B[i];
        int a = 0, z = nb, cand = -1, ncand = 0, j;
        while (a < z) { const int m = (a + z) / 2; if (ki[m].k < b->kb) a = m + 1; else z = m; }
        for (j = a; j < nb && ki[j].k == b->kb; j++) {
            const PvB *o = &B[ki[j].i];
            if (ki[j].i == i || o->kb == b->ka) continue;      /* itself, or its twin */
            if (fabs(o->ay - b->by) > 50.0) continue;
            cand = ki[j].i; ncand++;
        }
        if (ncand == 1 && B[cand].prev < 0) { b->next = cand; B[cand].prev = i; }
    }
    free(ki);
    *out = B; *nout = nb;
    return 1;
}

/* ---- offsetting one chain ---- */

static int pv_isect(double ax, double az, double bx, double bz, double cx, double cz, double dx, double dz,
                    double *x, double *z)
{
    const double rx = bx - ax, rz = bz - az, sx = dx - cx, sz = dz - cz;
    const double den = rx * sz - rz * sx, nr = hypot(rx, rz), ns = hypot(sx, sz);
    double t;
    if (!(fabs(den) > 1e-3 * nr * ns)) return 0;          /* within ~0.06 deg of parallel */
    t = ((cx - ax) * sz - (cz - az) * sx) / den;
    *x = ax + rx * t; *z = az + rz * t;
    return 1;
}

typedef struct {
    int      nv, ns, closed;
    double  *px, *pz, *py, *w;           /* per vertex                                   */
    double  *nx, *nz;                    /* per segment, outward                         */
    double  *ox, *oz;                    /* per vertex: outer point                      */
    double  *ux, *uz, *mf;               /* per vertex: unit bisector, mitre factor      */
    unsigned char *vf;                   /* per vertex: PV_VF_*                          */
} PvChain;

/* The junction of the outer lines of alive segments a and b (a before b), the
 * vertex between them being v. Clamped to PV_MITRE_MAX widths from it. */
static void pv_join(const PvChain *ch, const double *Ax, const double *Az, const double *Bx, const double *Bz,
                    int a, int b, int v, double *x, double *z)
{
    double lim, d;
    if (!pv_isect(Ax[a], Az[a], Bx[a], Bz[a], Ax[b], Az[b], Bx[b], Bz[b], x, z)) {
        *x = 0.5 * (Bx[a] + Ax[b]);
        *z = 0.5 * (Bz[a] + Az[b]);
    }
    lim = PV_MITRE_MAX * (ch->w[v] > 1.0 ? ch->w[v] : 1.0);
    d = hypot(*x - ch->px[v], *z - ch->pz[v]);
    if (d > lim) {
        *x = ch->px[v] + (*x - ch->px[v]) * lim / d;
        *z = ch->pz[v] + (*z - ch->pz[v]) * lim / d;
    }
}

/* Outer points of every vertex (see the section header): each segment's outer line
 * runs from P_k + w_k n_k to P_k+1 + w_k+1 n_k; alive neighbours meet at their mitre;
 * a segment whose outer piece runs BACKWARDS collapses and its neighbours meet
 * instead, until none does. A closed chain with fewer than 3 segments left is an
 * island small enough to fill: every outer point is its centroid. */
/* the reach of alive segment k's outer piece along its own direction (< 0 = it runs
 * backwards); pa / na = its alive neighbours or -1 */
static double pv_seg_dir(const PvChain *ch, const double *Ax, const double *Az, const double *Bx, const double *Bz,
                         int k, int pa, int na)
{
    const int k1 = (k + 1) % ch->nv;
    double sx, sz, ex, ez;
    const double dx = ch->px[k1] - ch->px[k], dz = ch->pz[k1] - ch->pz[k];
    if (pa >= 0) pv_join(ch, Ax, Az, Bx, Bz, pa, k, k, &sx, &sz); else { sx = Ax[k]; sz = Az[k]; }
    if (na >= 0) pv_join(ch, Ax, Az, Bx, Bz, k, na, k1, &ex, &ez); else { ex = Bx[k]; ez = Bz[k]; }
    return ((ex - sx) * dx + (ez - sz) * dz) / (hypot(dx, dz) + 1e-9);
}

static int pv_offset_chain(PvChain *ch)
{
    const int ns = ch->ns, nv = ch->nv;
    double *Ax = (double *)malloc((size_t)ns * 4u * sizeof(double)), *Az, *Bx, *Bz;
    int *pa = (int *)malloc((size_t)ns * 3u * sizeof(int)), *na, *alive;
    int k, v, nalive = ns, collapsed = 0;
    if (!Ax || !pa) { free(Ax); free(pa); return -1; }
    Az = Ax + ns; Bx = Az + ns; Bz = Bx + ns;
    na = pa + ns; alive = na + ns;
    for (k = 0; k < ns; k++) {
        const int k1 = (k + 1) % nv;
        Ax[k] = ch->px[k] + ch->w[k] * ch->nx[k];   Az[k] = ch->pz[k] + ch->w[k] * ch->nz[k];
        Bx[k] = ch->px[k1] + ch->w[k1] * ch->nx[k]; Bz[k] = ch->pz[k1] + ch->w[k1] * ch->nz[k];
        alive[k] = 1;
        pa[k] = (k > 0) ? k - 1 : (ch->closed ? ns - 1 : -1);
        na[k] = (k + 1 < ns) ? k + 1 : (ch->closed ? 0 : -1);
    }
    if (ch->closed && ns < 3) nalive = 0;
    /* collapse, most-reversed first, until no outer piece runs backwards. Only a corner
     * may collapse: a piece shorter than PV_COLLAPSE_SEG widths, into a run of collapsed
     * pieces no longer than PV_COLLAPSE_RUN widths (a fillet arc and its tangents); a
     * straight kerb never folds into a far point */
    while (nalive > 0) {
        int worst = -1;
        double wd = -1e-6;
        for (k = 0; k < ns; k++) {
            double dd, run, wm;
            int j, q;
            if (!alive[k]) continue;
            dd = pv_seg_dir(ch, Ax, Az, Bx, Bz, k, pa[k], na[k]);
            if (!(dd < wd)) continue;
            wm = ch->w[k] > ch->w[(k + 1) % nv] ? ch->w[k] : ch->w[(k + 1) % nv];
            run = hypot(ch->px[(k + 1) % nv] - ch->px[k], ch->pz[(k + 1) % nv] - ch->pz[k]);
            if (!(wm > 1.0) || run > PV_COLLAPSE_SEG * wm) continue;
            for (j = 1; j < ns; j++) {            /* the dead run this piece would join */
                q = k - j;
                if (q < 0) { if (!ch->closed) break; q += ns; }
                if (alive[q]) break;
                run += hypot(ch->px[(q + 1) % nv] - ch->px[q], ch->pz[(q + 1) % nv] - ch->pz[q]);
                if (ch->w[q] > wm) wm = ch->w[q];
            }
            for (j = 1; j < ns; j++) {
                q = k + j;
                if (q >= ns) { if (!ch->closed) break; q -= ns; }
                if (alive[q]) break;
                run += hypot(ch->px[(q + 1) % nv] - ch->px[q], ch->pz[(q + 1) % nv] - ch->pz[q]);
                if (ch->w[(q + 1) % nv] > wm) wm = ch->w[(q + 1) % nv];
            }
            if (run > PV_COLLAPSE_RUN * wm) continue;
            worst = k; wd = dd;
        }
        if (worst < 0) break;
        if (nalive <= (ch->closed ? 3 : 1)) { if (ch->closed) nalive = 0; break; }
        alive[worst] = 0;
        if (pa[worst] >= 0) na[pa[worst]] = na[worst];
        if (na[worst] >= 0) pa[na[worst]] = pa[worst];
        nalive--;
        collapsed++;
    }
    if (ch->closed && nalive < 3) {
        /* an island smaller than its pavement: filled, every outer point its centroid */
        double cx = 0.0, cz = 0.0;
        for (v = 0; v < nv; v++) { cx += ch->px[v]; cz += ch->pz[v]; }
        cx /= nv; cz /= nv;
        for (v = 0; v < nv; v++) { ch->ox[v] = cx; ch->oz[v] = cz; ch->vf[v] |= PV_VF_COLLAPSE | PV_VF_ISLAND; }
        free(Ax); free(pa);
        return ns;
    }
    for (v = 0; v < nv; v++) {
        /* the alive segment at or after vertex v (b) and the alive one before it (a) */
        int a = -1, b = -1, j;
        for (j = 0; j < ns; j++) {
            const int q = v + j;
            if (q >= ns && !ch->closed) break;
            if (alive[q % ns]) { b = q % ns; break; }
        }
        if (b >= 0) a = pa[b];
        else
            for (j = 1; j <= ns; j++) {
                const int q = v - j;
                if (q < 0) break;
                if (alive[q]) { a = q; break; }
            }
        if (a >= 0 && b >= 0 && a != b) {
            pv_join(ch, Ax, Az, Bx, Bz, a, b, (a + 1) % nv, &ch->ox[v], &ch->oz[v]);
            if (v != (a + 1) % nv) ch->vf[v] |= PV_VF_COLLAPSE;
        } else if (b >= 0) {
            ch->ox[v] = Ax[b]; ch->oz[v] = Az[b];
            if (b != v) ch->vf[v] |= PV_VF_COLLAPSE;
        } else if (a >= 0) {
            ch->ox[v] = Bx[a]; ch->oz[v] = Bz[a];
            if ((a + 1) % nv != v) ch->vf[v] |= PV_VF_COLLAPSE;
        } else {
            ch->ox[v] = ch->px[v]; ch->oz[v] = ch->pz[v];
        }
    }
    free(Ax); free(pa);
    return collapsed;
}

/* ---- buildings for the facade clip and the fence ---- */

typedef struct { const double *px, *pz; int n; double x0, z0, x1, z1; int lm; } PvRing;   /* place frame; lm: [HD] a landmark */
typedef struct {
    PvRing *r; int nr;
    int    *start, *item;            /* CSR over a 64 m grid of the ctx box */
    double  gx0, gz0, cell;
    int     gnx, gnz;
} PvBld;

static void pv_bld_free(PvBld *b) { free(b->r); free(b->start); free(b->item); memset(b, 0, sizeof *b); }

/* [IC] how far past the tile's own square a footprint is judged by sc_bld_drops (farther ones are kept as they are:
 * only this tile's chains and lots read them, and the roads within reach of one this near are all in the 3x3) */
#define PV_BLD_JUDGE_M  80.0
static int pv_bld_build(TD5_RoamScnSource *s, TG_TileCtx *c, ScCache **bc, int nbc, const ScArea *ar, int nar, PvBld *pb,
                        int judge)
{
    int dx, dz, l, j, pass, i, total = 0, cap = 0;
    int *cnt;
    const double T = (double)c->grid->tile, J = PV_BLD_JUDGE_M * SC_UPM;
    memset(pb, 0, sizeof *pb);
    for (dz = -1; dz <= 1; dz++)
        for (dx = -1; dx <= 1; dx++) {
            const int64_t key = sc_tkey(c->tx + dx, c->tz + dz);
            for (l = 0; l < nbc; l++) {
                int lo, hi;
                sc_owned_range(bc[l], key, &lo, &hi);
                for (j = lo; j < hi; j++) {
                    const TD5_GeoBuilding *gb = &bc[l]->b[bc[l]->order[j]];
                    PvRing *r;
                    int k;
                    double u, v;
                    if (gb->n < 3 || gb->part) continue;
                    roam_grid_uv(c->grid, gb->cx, gb->cz, &u, &v);
                    if (judge && s->ic_drops && u > (double)c->tx * T - J - gb->radius && u < (double)(c->tx + 1) * T + J + gb->radius &&
                        v > (double)c->tz * T - J - gb->radius && v < (double)(c->tz + 1) * T + J + gb->radius &&
                        sc_bld_drops(s, c, gb, &bc[l]->px[gb->first], &bc[l]->pz[gb->first], ar, nar)) {
                        c->st.pv_bld_gone++;
                        continue;
                    }
                    if (!sc_grow((void **)&pb->r, &cap, pb->nr + 1, sizeof(PvRing))) return 0;
                    r = &pb->r[pb->nr++];
                    r->px = &bc[l]->px[gb->first]; r->pz = &bc[l]->pz[gb->first]; r->n = gb->n;
                    r->lm = gb->landmark;
                    r->x0 = r->x1 = r->px[0]; r->z0 = r->z1 = r->pz[0];
                    for (k = 1; k < r->n; k++) {
                        if (r->px[k] < r->x0) r->x0 = r->px[k];
                        if (r->px[k] > r->x1) r->x1 = r->px[k];
                        if (r->pz[k] < r->z0) r->z0 = r->pz[k];
                        if (r->pz[k] > r->z1) r->z1 = r->pz[k];
                    }
                }
            }
        }
    pb->cell = SC_CELL * 4.0;
    pb->gx0 = c->gx0 + (double)c->org_x; pb->gz0 = c->gz0 + (double)c->org_z;
    pb->gnx = (int)ceil((double)c->gnx * SC_CELL / pb->cell) + 1;
    pb->gnz = (int)ceil((double)c->gnz * SC_CELL / pb->cell) + 1;
    cnt = (int *)calloc((size_t)pb->gnx * pb->gnz + 1u, sizeof(int));
    pb->start = (int *)calloc((size_t)pb->gnx * pb->gnz + 1u, sizeof(int));
    if (!cnt || !pb->start) { free(cnt); return 0; }
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < pb->nr; i++) {
            const PvRing *r = &pb->r[i];
            int a0 = (int)floor((r->x0 - pb->gx0) / pb->cell), a1 = (int)floor((r->x1 - pb->gx0) / pb->cell);
            int b0 = (int)floor((r->z0 - pb->gz0) / pb->cell), b1 = (int)floor((r->z1 - pb->gz0) / pb->cell), a, b;
            if (a0 < 0) a0 = 0;
            if (b0 < 0) b0 = 0;
            if (a1 >= pb->gnx) a1 = pb->gnx - 1;
            if (b1 >= pb->gnz) b1 = pb->gnz - 1;
            for (b = b0; b <= b1; b++)
                for (a = a0; a <= a1; a++) {
                    const int cell = b * pb->gnx + a;
                    if (pass == 0) cnt[cell]++;
                    else pb->item[pb->start[cell] + cnt[cell]++] = i;
                }
        }
        if (pass == 0) {
            const int nc = pb->gnx * pb->gnz;
            for (i = 0; i < nc; i++) { pb->start[i] = total; total += cnt[i]; cnt[i] = 0; }
            pb->start[nc] = total;
            pb->item = (int *)malloc((size_t)(total > 0 ? total : 1) * sizeof(int));
            if (!pb->item) { free(cnt); return 0; }
        }
    }
    free(cnt);
    return 1;
}

/* a building footprint holds the tile-local point */
static int pv_in_building(const TG_TileCtx *c, const PvBld *pb, double x, double z)
{
    const double X = x + (double)c->org_x, Z = z + (double)c->org_z;
    const int a = (int)floor((X - pb->gx0) / pb->cell), b = (int)floor((Z - pb->gz0) / pb->cell);
    int k;
    if (!pb->start || a < 0 || b < 0 || a >= pb->gnx || b >= pb->gnz) return 0;
    for (k = pb->start[b * pb->gnx + a]; k < pb->start[b * pb->gnx + a + 1]; k++) {
        const PvRing *r = &pb->r[pb->item[k]];
        if (X < r->x0 || X > r->x1 || Z < r->z0 || Z > r->z1) continue;
        if (td5_geob_point_in_ring(r->px, r->pz, r->n, X, Z)) return 1;
    }
    return 0;
}

/* a plaza, park or playground holds the tile-local point (no fence there) */
static int pv_in_green(const TG_TileCtx *c, const ScArea *ar, int nar, double x, double z)
{
    const double X = x + (double)c->org_x, Z = z + (double)c->org_z;
    int k;
    for (k = 0; k < nar; k++) {
        const TD5_GeoArea *a = ar[k].a;
        if (!td5_geob_area_is_plaza(a) && a->kind != TD5_GEOA_KIND_PARK && a->kind != TD5_GEOA_KIND_PLAY &&
            a->kind != TD5_GEOA_KIND_GRASS && a->kind != TD5_GEOA_KIND_PITCH) continue;
        if (hypot(X - a->cx, Z - a->cz) > a->radius) continue;
        if (td5_geob_point_in_ring(ar[k].px, ar[k].pz, a->n, X, Z)) return 1;
    }
    return 0;
}

/* ---- emission ---- */

static int inf_in_lot(const TG_TileCtx *c, double x, double z);   /* [HD] SECTION lots */

typedef struct {
    TD5_RoamScnSource *s;
    TG_TileCtx  *c;
    PvWay *const *ways; int nways;
    PvBld       *bld;
    const ScArea *ar; int nar;
    ScFpBatch   *bat;                 /* 3 per sub-block: paving, lawn, fence */
} PvEmit;

enum { PV_B_PAVE = 0, PV_B_LAWN = 1, PV_B_FENCE = 2, PV_NB = 3 };

static int pv_quad(PvEmit *e, int sb, int which, const double *x, const double *y, const double *z,
                   const double *u, const double *v, unsigned int light)
{
    ScFpBatch *bt = &e->bat[sb * PV_NB + which];
    int q;
    if (bt->nq >= SC_FP_CHUNK && !sc_fp_flush(e->c, sb, bt)) return 0;
    for (q = 0; q < 4; q++) {
        const int o = (bt->nq * 4 + q) * 5;
        bt->v[o + 0] = (float)x[q];
        bt->v[o + 1] = (float)y[q];
        bt->v[o + 2] = (float)z[q];
        bt->v[o + 3] = (float)u[q];
        bt->v[o + 4] = (float)v[q];
        bt->light[bt->nq * 4 + q] = light;
    }
    bt->nq++;
    return 1;
}

/* u along a vertical face: the segment's dominant axis, so the texture runs on */
static double pv_along(double x, double z, double dx, double dz)
{
    return (fabs(dx) >= fabs(dz) ? x : z) * PV_UV;
}

/* the heights of every vertex group that shares one outer point: one y per point */
static void pv_heights(PvEmit *e, PvChain *ch, double *yo, double *gr)
{
    const int nv = ch->nv;
    int v0 = 0, done = 0;
    /* a closed chain starts its groups where the outer point changes (a corner run may wrap) */
    if (ch->closed)
        for (v0 = 0; v0 < nv; v0++) {
            const int p = (v0 + nv - 1) % nv;
            if (ch->ox[p] != ch->ox[v0] || ch->oz[p] != ch->oz[v0]) break;
        }
    if (v0 >= nv) v0 = 0;
    while (done < nv) {
        int n = 1, j;
        double yk = 0.0, g, d = 0.0, low;
        unsigned vf = 0;
        const int v = (v0 + done) % nv;
        while (done + n < nv) {
            const int q = (v + n) % nv;
            if (ch->ox[q] != ch->ox[v] || ch->oz[q] != ch->oz[v]) break;
            n++;
        }
        for (j = 0; j < n; j++) {
            const int k = (v + j) % nv;
            const double dk = hypot(ch->ox[k] - ch->px[k], ch->oz[k] - ch->pz[k]);
            yk += ch->py[k] + TD5_TG_KERB_H;
            vf |= ch->vf[k];
            if (dk > d) d = dk;
        }
        yk /= (double)n;
        g = sc_ground(e->c, ch->ox[v], ch->oz[v]);
        low = sc_lowering(e->c, ch->ox[v], ch->oz[v]);
        for (j = 0; j < n; j++) {
            const int k = (v + j) % nv;
            if (vf & (PV_VF_DECK | PV_VF_FLAT)) {
                yo[k] = yk; gr[k] = yk - PV_DECK_FASCIA;
            } else {
                double y = g + TD5_TG_KERB_H;
                if (y < yk - d * PV_FALL) y = yk - d * PV_FALL;
                if (y > yk + d * PV_FALL) y = yk + d * PV_FALL;
                if (low > 0.0 && y < yk - d * PV_FALL_LOW) y = yk - d * PV_FALL_LOW;
                yo[k] = y; gr[k] = g - PV_BACK_SINK;
            }
        }
        done += n;
    }
}

/* one piece (vertices i0 .. i1 of the chain, i1 > i0 in chain order, wrapped) */
static int pv_piece(PvEmit *e, PvChain *ch, int i0, int i1, const double *kx, const double *kz,
                    const double *yo, const double *gr, int klass, int corner)
{
    TG_TileCtx *c = e->c;
    const int a = i0 % ch->nv, b = i1 % ch->nv;
    const unsigned fa = ch->vf[a], fb = ch->vf[b];
    const int island = ((fa | fb) & PV_VF_ISLAND) != 0, lawn = (fa & PV_VF_MEDIAN) && (fb & PV_VF_MEDIAN);
    const int deck = (fa & PV_VF_DECK) && (fb & PV_VF_DECK), flat = ((fa | fb) & PV_VF_FLAT) != 0;
    double ox[2] = { ch->ox[a], ch->ox[b] }, oz[2] = { ch->oz[a], ch->oz[b] };
    double fx[4], fz[4], x[4], y[4], z[4], u[4], v[4];
    const double ddx = kx[b] - kx[a], ddz = kz[b] - kz[a];
    int sb, k, cut = 0;
    if (ch->w[a] < PV_W_SKIP && ch->w[b] < PV_W_SKIP) return 1;
    if ((ox[1] - ox[0]) * ddx + (oz[1] - oz[0]) * ddz < 0.0) {
        /* an outer piece still running backwards (a reversal no corner rule may fold):
         * a triangle to its midpoint, never a bow-tie */
        ox[0] = ox[1] = 0.5 * (ox[0] + ox[1]);
        oz[0] = oz[1] = 0.5 * (oz[0] + oz[1]);
    }
    sb = sc_sb_of(c, 0.5 * (kx[a] + kx[b]), 0.5 * (kz[a] + kz[b]));
    /* the guard: the slab footprint off every drivable primitive, cut back, else dropped */
    for (;;) {
        /* the footprint as the census reads it: coincident corners merged (a collapsed
         * corner piece is a triangle; a repeated vertex hides it from sc_in_convex) */
        double qx[4] = { kx[a], kx[b], ox[1], ox[0] }, qz[4] = { kz[a], kz[b], oz[1], oz[0] };
        int m = 0, j, d;
        for (j = 0; j < 4; j++) {
            for (d = 0; d < m; d++) if (fabs(fx[d] - qx[j]) < 1.0 && fabs(fz[d] - qz[j]) < 1.0) break;
            if (d == m) { fx[m] = (double)(float)qx[j]; fz[m] = (double)(float)qz[j]; m++; }   /* as the census reads it */
        }
        if (m >= 3 && fabs(td5_geob_ring_area(fx, fz, m)) < 4.0) m = 2;    /* the census's collinear rule */
        if (m < 2 || !sc_on_road(c, fx, fz, m, NULL)) break;
        if (++cut > 2) { c->st.pv_drop++; return 1; }
        for (k = 0; k < 2; k++) {
            const double bx = k ? kx[b] : kx[a], bz = k ? kz[b] : kz[a];
            ox[k] = bx + (ox[k] - bx) * 0.5; oz[k] = bz + (oz[k] - bz) * 0.5;
        }
    }
    if (cut) c->st.pv_cut++;
    c->st.pv_pieces++;
    if (corner) c->st.pv_corner++;
    if (island) c->st.pv_island++;
    if (lawn) c->st.pv_median++;
    /* slab */
    x[0] = kx[a]; z[0] = kz[a]; y[0] = ch->py[a] + TD5_TG_KERB_H;
    x[1] = kx[b]; z[1] = kz[b]; y[1] = ch->py[b] + TD5_TG_KERB_H;
    x[2] = ox[1]; z[2] = oz[1]; y[2] = cut ? 0.5 * (y[1] + yo[b]) : yo[b];
    x[3] = ox[0]; z[3] = oz[0]; y[3] = cut ? 0.5 * (y[0] + yo[a]) : yo[a];
    for (k = 0; k < 4; k++) {
        u[k] = x[k] * (lawn ? PV_UV_LAWN : PV_UV);
        v[k] = z[k] * (lawn ? PV_UV_LAWN : PV_UV);
    }
    if (!pv_quad(e, sb, lawn ? PV_B_LAWN : PV_B_PAVE, x, y, z, u, v, 0xFFFFFFFFu)) return 0;
    if (sc_grow((void **)&c->pvq, &c->cap_pvq, (c->npvq + 1) * 12, sizeof(double))) {   /* [G5] for the footpaths */
        double *P = &c->pvq[c->npvq++ * 12];
        for (k = 0; k < 4; k++) { P[k * 3] = (double)(float)x[k]; P[k * 3 + 1] = (double)(float)y[k]; P[k * 3 + 2] = (double)(float)z[k]; }
    }
    /* kerb face: the road up to the slab */
    {
        const double ka[2] = { kx[a], kz[a] }, kb[2] = { kx[b], kz[b] };
        if (!sc_on_road(c, (const double[2]){ (double)(float)ka[0], (double)(float)kb[0] },
                        (const double[2]){ (double)(float)ka[1], (double)(float)kb[1] }, 2, NULL)) {
            x[0] = ka[0]; z[0] = ka[1]; y[0] = ch->py[a];
            x[1] = kb[0]; z[1] = kb[1]; y[1] = ch->py[b];
            x[2] = kb[0]; z[2] = kb[1]; y[2] = ch->py[b] + TD5_TG_KERB_H;
            x[3] = ka[0]; z[3] = ka[1]; y[3] = ch->py[a] + TD5_TG_KERB_H;
            u[0] = u[3] = pv_along(ka[0], ka[1], ddx, ddz); u[1] = u[2] = pv_along(kb[0], kb[1], ddx, ddz);
            v[0] = v[1] = 0.0; v[2] = v[3] = TD5_TG_KERB_H * PV_UV;
            if (!pv_quad(e, sb, PV_B_PAVE, x, y, z, u, v, 0xFFC8C8C8u)) return 0;
            c->st.pv_kerb++;
        } else {
            c->st.pv_kerb_cut++;
        }
    }
    if (island || cut || flat) return 1;
    /* back face: the slab's outer edge down under the drawn ground (a fascia on a deck) */
    x[0] = ox[1]; z[0] = oz[1]; y[0] = yo[b];
    x[1] = ox[0]; z[1] = oz[0]; y[1] = yo[a];
    x[2] = ox[0]; z[2] = oz[0]; y[2] = gr[a];
    x[3] = ox[1]; z[3] = oz[1]; y[3] = gr[b];
    u[0] = u[3] = pv_along(ox[1], oz[1], ddx, ddz); u[1] = u[2] = pv_along(ox[0], oz[0], ddx, ddz);
    v[0] = v[1] = 0.0; v[2] = (yo[a] - gr[a]) * PV_UV; v[3] = (yo[b] - gr[b]) * PV_UV;
    if (!pv_quad(e, sb, PV_B_PAVE, x, y, z, u, v, 0xFFA8A8A8u)) return 0;
    /* fence: a railing on a deck; elsewhere where nothing stands behind the slab */
    {
        int fence = deck;
        /* not on a junction outline: a block corner holds a building or a plaza, and the
         * outline has no street class of its own */
        if (!fence && e->s->pv->fence && !corner && !((fa | fb) & PV_VF_FACADE) && klass >= TD5_GEO_RC_LIVING &&
            klass <= TD5_GEO_RC_SECONDARY && hypot(ox[1] - ox[0], oz[1] - oz[0]) > 200.0) {
            const double mx = 0.5 * (ox[0] + ox[1]), mz = 0.5 * (oz[0] + oz[1]);
            const double ex = oz[1] - oz[0], ez = -(ox[1] - ox[0]), el = hypot(ex, ez);
            const double px2 = mx + ex / el * PV_FENCE_PROBE, pz2 = mz + ez / el * PV_FENCE_PROBE;
            fence = !pv_in_building(c, e->bld, px2, pz2) && !pv_in_building(c, e->bld, mx, mz) &&
                    !pv_in_green(c, e->ar, e->nar, px2, pz2) && !inf_in_lot(c, px2, pz2);
        }
        if (fence) {
            x[0] = ox[0]; z[0] = oz[0]; y[0] = yo[a];
            x[1] = ox[1]; z[1] = oz[1]; y[1] = yo[b];
            x[2] = ox[1]; z[2] = oz[1]; y[2] = yo[b] + TD5_TG_FENCE_H;
            x[3] = ox[0]; z[3] = oz[0]; y[3] = yo[a] + TD5_TG_FENCE_H;
            u[0] = u[3] = pv_along(ox[0], oz[0], ddx, ddz); u[1] = u[2] = pv_along(ox[1], oz[1], ddx, ddz);
            v[0] = v[1] = 1.0; v[2] = v[3] = 0.0;
            if (!pv_quad(e, sb, PV_B_FENCE, x, y, z, u, v, 0xFFFFFFFFu)) return 0;
            if (deck) c->st.pv_rail++; else c->st.pv_fence++;
        }
    }
    return 1;
}

/* may vertex m be skipped inside a merged straight piece a .. b? */
static int pv_mergeable(const PvChain *ch, const double *kx, const double *kz, const double *yo, int a, int m, int b)
{
    const int A = a % ch->nv, M = m % ch->nv, Bv = b % ch->nv;
    const double dx = kx[Bv] - kx[A], dz = kz[Bv] - kz[A], L = hypot(dx, dz);
    double t;
    if (!(L > 1.0) || L > PV_MERGE_M * SC_UPM) return 0;
    if (ch->vf[M] != ch->vf[A] || ch->vf[M] != ch->vf[Bv] || (ch->vf[M] & (PV_VF_COLLAPSE | PV_VF_ISLAND))) return 0;
    t = ((kx[M] - kx[A]) * dx + (kz[M] - kz[A]) * dz) / (L * L);
    if (t <= 0.0 || t >= 1.0) return 0;
    if (fabs((kx[M] - kx[A]) * dz - (kz[M] - kz[A]) * dx) / L > PV_MERGE_TOL) return 0;
    if (hypot(ch->ox[M] - (ch->ox[A] + (ch->ox[Bv] - ch->ox[A]) * t),
              ch->oz[M] - (ch->oz[A] + (ch->oz[Bv] - ch->oz[A]) * t)) > PV_MERGE_TOL) return 0;
    if (fabs(ch->py[M] - (ch->py[A] + (ch->py[Bv] - ch->py[A]) * t)) > PV_MERGE_TOL) return 0;
    if (fabs(yo[M] - (yo[A] + (yo[Bv] - yo[A]) * t)) > 2.0 * PV_MERGE_TOL) return 0;
    return 1;
}

static int pv_infill(PvEmit *e, const PvChain *ch);   /* [HD] SECTION lots, after the city lots */

static int pv_chain_emit(PvEmit *e, PvB *B, int nb, int start)
{
    TG_TileCtx *c = e->c;
    int *seq = NULL, m = 0, cap = 0, s, closed = 0, ok = 1, k, v;
    PvChain ch;
    double *kx = NULL, *kz = NULL, *yo = NULL, *gr = NULL, *blk;
    /* back to the chain's first piece (or round a closed one), then forward */
    s = start;
    for (k = 0; k < PV_CHAIN_MAX && B[s].prev >= 0 && B[s].prev != start; k++) s = B[s].prev;
    if (B[s].prev == start) { s = start; closed = 1; }
    for (k = s; k >= 0 && m < PV_CHAIN_MAX;) {
        if (!sc_grow((void **)&seq, &cap, m + 1, sizeof(int))) { free(seq); return 0; }
        seq[m++] = k;
        B[k].done = 1;
        k = B[k].next;
        if (k == s) { closed = 1; break; }
    }
    if (m >= PV_CHAIN_MAX) closed = 0;
    (void)nb;
    memset(&ch, 0, sizeof ch);
    ch.ns = m;
    ch.nv = closed ? m : m + 1;
    ch.closed = closed;
    blk = (double *)calloc((size_t)ch.nv * 14u + (size_t)m * 2u, sizeof(double));
    ch.vf = (unsigned char *)calloc((size_t)ch.nv, 1);
    if (!blk || !ch.vf) { free(blk); free(ch.vf); free(seq); return 0; }
    ch.px = blk; ch.pz = ch.px + ch.nv; ch.py = ch.pz + ch.nv; ch.w = ch.py + ch.nv;
    ch.ox = ch.w + ch.nv; ch.oz = ch.ox + ch.nv; ch.ux = ch.oz + ch.nv; ch.uz = ch.ux + ch.nv; ch.mf = ch.uz + ch.nv;
    kx = ch.mf + ch.nv; kz = kx + ch.nv; yo = kz + ch.nv; gr = yo + ch.nv;
    ch.nx = gr + ch.nv; ch.nz = ch.nx + m;
    c->st.pv_chains++;
    /* vertices, segment normals, vertex flags and the ribbon widths */
    for (k = 0; k < m; k++) {
        const PvB *b = &B[seq[k]];
        ch.px[k] = b->ax; ch.pz[k] = b->az; ch.py[k] = b->ay;
        ch.nx[k] = b->nx; ch.nz[k] = b->nz;
        ch.w[k] = -1.0;
    }
    if (!closed) { const PvB *b = &B[seq[m - 1]]; ch.px[m] = b->bx; ch.pz[m] = b->bz; ch.py[m] = b->by; ch.w[m] = -1.0; }
    for (v = 0; v < ch.nv; v++) {
        const int kin = (v > 0) ? v - 1 : (closed ? m - 1 : -1), kout = (v < m) ? v : -1;
        double wsum = 0.0;
        int wn = 0;
        if (kin >= 0) {
            const PvB *b = &B[seq[kin]];
            if (b->deck) ch.vf[v] |= PV_VF_DECK;
            if (b->flat) ch.vf[v] |= PV_VF_FLAT;
            if (b->w >= 0.0) { wsum += b->w; wn++; }
        }
        if (kout >= 0) {
            const PvB *b = &B[seq[kout]];
            if (b->deck) ch.vf[v] |= PV_VF_DECK;
            if (b->flat) ch.vf[v] |= PV_VF_FLAT;
            if (b->w >= 0.0) { wsum += b->w; wn++; }
        }
        if (wn) ch.w[v] = wsum / wn;
        /* bisector + mitre factor */
        {
            double ux = 0.0, uz = 0.0, l, cs = 1.0;
            if (kin >= 0) { ux += ch.nx[kin]; uz += ch.nz[kin]; }
            if (kout >= 0) { ux += ch.nx[kout]; uz += ch.nz[kout]; }
            l = hypot(ux, uz);
            if (l > 1e-9) { ux /= l; uz /= l; } else if (kout >= 0) { ux = ch.nx[kout]; uz = ch.nz[kout]; }
            if (kout >= 0) cs = ux * ch.nx[kout] + uz * ch.nz[kout];
            else if (kin >= 0) cs = ux * ch.nx[kin] + uz * ch.nz[kin];
            ch.ux[v] = ux; ch.uz[v] = uz;
            ch.mf[v] = 1.0 / (cs > 0.4 ? cs : 0.4);
        }
    }
    /* outline vertices with no ribbon on either side: the ribbon widths round them, by arc length */
    {
        double *wk = (double *)malloc((size_t)ch.nv * sizeof(double));
        if (!wk) { ok = 0; goto done; }
        for (v = 0; v < ch.nv; v++) wk[v] = ch.w[v];
        for (v = 0; v < ch.nv; v++) {
            double da = 0.0, db = 0.0, wa = -1.0, wb = -1.0;
            int j, q;
            if (wk[v] >= 0.0) continue;
            for (j = 1, q = v; j < ch.nv; j++) {          /* backward */
                const int p = q - 1;
                if (p < 0 && !closed) break;
                q = (p + ch.nv) % ch.nv;
                da += hypot(ch.px[(q + 1) % ch.nv] - ch.px[q], ch.pz[(q + 1) % ch.nv] - ch.pz[q]);
                if (da > PV_INTERP_M * SC_UPM) break;
                if (wk[q] >= 0.0) { wa = wk[q]; break; }
            }
            for (j = 1, q = v; j < ch.nv; j++) {          /* forward */
                const int p = q + 1;
                if (p >= ch.nv && !closed) break;
                db += hypot(ch.px[p % ch.nv] - ch.px[q], ch.pz[p % ch.nv] - ch.pz[q]);
                q = p % ch.nv;
                if (db > PV_INTERP_M * SC_UPM) break;
                if (wk[q] >= 0.0) { wb = wk[q]; break; }
            }
            if (wa >= 0.0 && wb >= 0.0) ch.w[v] = (wa * db + wb * da) / (da + db > 1e-9 ? da + db : 1.0);
            else if (wa >= 0.0) ch.w[v] = wa;
            else if (wb >= 0.0) ch.w[v] = wb;
            else ch.w[v] = PV_W_DEFAULT;
        }
        free(wk);
    }
    /* per vertex: half the free distance to another drivable surface, the building front */
    for (v = 0; v < ch.nv; v++) {
        const double w0 = ch.w[v];
        const double dmax = 2.0 * w0 + 2.0 * PV_KERB_IN + 400.0;
        const double dmed = (e->s->pv->median ? PV_MEDIAN_M * SC_UPM : 0.0);
        const double lim = dmax > dmed ? dmax : dmed;
        const ScPrim *hp = NULL;
        double d, freed = -1.0;
        if (!(w0 > 0.0)) continue;
        for (d = PV_RAY_STEP; d <= lim; d += PV_RAY_STEP) {
            const double qx = ch.px[v] + ch.ux[v] * (PV_KERB_IN + d), qz = ch.pz[v] + ch.uz[v] * (PV_KERB_IN + d);
            if (sc_on_road(c, &qx, &qz, 1, &hp)) {
                double lo = d - PV_RAY_STEP, hi = d;
                int it;
                for (it = 0; it < 5; it++) {
                    const double mid = 0.5 * (lo + hi);
                    const double rx = ch.px[v] + ch.ux[v] * (PV_KERB_IN + mid), rz = ch.pz[v] + ch.uz[v] * (PV_KERB_IN + mid);
                    if (sc_on_road(c, &rx, &rz, 1, NULL)) hi = mid; else lo = mid;
                }
                freed = PV_KERB_IN + lo;
                break;
            }
        }
        if (freed >= 0.0) {
            int median = 0;
            if (hp && hp->seg >= 0 && freed <= PV_MEDIAN_M * SC_UPM && e->s->pv->median) {
                const int kin = (v > 0) ? v - 1 : (closed ? m - 1 : -1), kout = (v < m) ? v : -1;
                const PvWay *me = (kout >= 0 && B[seq[kout]].way) ? B[seq[kout]].way
                                : (kin >= 0 ? B[seq[kin]].way : NULL);
                const PvWay *ot = pv_find(e->ways, e->nways, (int64_t)(c->edge[c->seg[hp->seg].edge].key >> 16));
                median = me && ot && me != ot && me->name_h && me->name_h == ot->name_h && me->oneway && ot->oneway;
            }
            if (median || ch.w[v] > 0.5 * freed) {
                ch.w[v] = 0.5 * freed;
                ch.vf[v] |= PV_VF_ISLAND;
                if (median && freed > PV_LAWN_MIN_M * SC_UPM) ch.vf[v] |= PV_VF_MEDIAN;
                if (B[seq[v < m ? v : m - 1]].owned) c->st.pv_free_clamp++;
            }
        }
        if (ch.w[v] < PV_KERB_IN + PV_W_SKIP) { ch.w[v] = 0.0; continue; }
        /* a mapped building front behind the kerb: the slab stops there (not inside the clearance gap) */
        if (e->s->pv->facade && !(ch.vf[v] & (PV_VF_ISLAND | PV_VF_DECK | PV_VF_FLAT)) && ch.w[v] > e->s->gap) {
            for (d = e->s->gap; d < ch.w[v]; d += PV_FACE_STEP) {
                const double qx = ch.px[v] + ch.ux[v] * d, qz = ch.pz[v] + ch.uz[v] * d;
                if (pv_in_building(c, e->bld, qx, qz)) {
                    ch.w[v] = d;
                    ch.vf[v] |= PV_VF_FACADE;
                    break;
                }
            }
        }
    }
    {
        const int col = pv_offset_chain(&ch);
        if (col < 0) { ok = 0; goto done; }
    }
    for (v = 0; v < ch.nv; v++) {
        kx[v] = ch.px[v] + ch.ux[v] * PV_KERB_IN * ch.mf[v];
        kz[v] = ch.pz[v] + ch.uz[v] * PV_KERB_IN * ch.mf[v];
    }
    pv_heights(e, &ch, yo, gr);
    /* [HD] this block's frontage lots, before its pieces (a fence never stands in front of one) */
    if (!pv_infill(e, &ch)) { ok = 0; goto done; }
    /* the owned pieces, straight runs merged */
    for (k = 0; k < m;) {
        const PvB *b = &B[seq[k]];
        int k1 = k + 1;
        if (!b->owned) { k++; continue; }
        if (ch.vf[k] & PV_VF_COLLAPSE) c->st.pv_collapsed++;
        if (e->s->pv->merge && b->kind == PV_K_RIB)
            while (k1 < m && B[seq[k1]].owned && B[seq[k1]].kind == PV_K_RIB &&
                   pv_mergeable(&ch, kx, kz, yo, k, k1, k1 + 1)) {
                int j, all = 1;
                for (j = k + 1; j < k1 && all; j++) all = pv_mergeable(&ch, kx, kz, yo, k, j, k1 + 1);
                if (!all) break;
                k1++;
            }
        if (!pv_piece(e, &ch, k, k1, kx, kz, yo, gr, b->klass >= 0 ? b->klass : TD5_GEO_RC_RESIDENTIAL,
                      b->kind == PV_K_PATCH)) { ok = 0; goto done; }
        k = k1;
    }
done:
    free(blk);
    free(ch.vf);
    free(seq);
    return ok;
}

/* [HD] Every closed kerb loop of the 3x3 that runs (by RIB length, PV_RING_SHARE) along roundabout / circular ways
 * with the road outside it: the island of a ring, a plaza mapped only as its ring road (Plaza Italia). Its kerb
 * polygon joins c->lotp as GREEN, so its ground keeps the lawn and no infill lot stands in it.
 *
 * [IC] AVENUE MEDIANS. Mariano, 2026-10-20: "the boulevard (rambla) of the Circunvalacion has buildings, there are no
 * buildings there". A median wider than PV_MEDIAN_M is no island (pv_chain_emit fills only narrower gaps), so its
 * kerb loop closed on its own and read as a city block: the lots paved it, the infill lined it, Overture's
 * detections in it stood. A loop is a MEDIAN when at least PV_MEDIAN_SHARE of its ribbon kerb length belongs to
 * one-way ways whose name stands on two OPPOSITE sides of it (that name's kerb pieces, weighted by length, run both
 * ways round the loop: |sum of their vectors| under PV_MEDIAN_OPPOSE of their length; one carriageway split into two
 * OSM ways along one side of a block runs one way and does not count). The names add up: the rambla island where two
 * boulevards cross (Av 72 x Av 19) is bounded by both pairs, 0.55-0.58 of it each. No width test: measured over the
 * casco (_archive/roam/IC), every such loop wider than a block-like 50 m was a rambla (Av 31 at 54 m, Av 72,
 * Boulevard 84) or a plaza whose ring streets carry the plaza's name (Plaza Sarmiento, Plaza Peron); a city block
 * has different streets on its opposite sides. Same treatment as a ring plaza: GREEN, a plaza for the building veto
 * (sc_bld_green_veto). Dev: TD5RE_ROAM_MEDIAN_LOOPS=0 turns it off,
 * TD5RE_ROAM_BLD_LOG=1 logs every closed block loop with its share and width (ROAM_LOOP). */
/* [IC] UNMAPPED GREEN. Mariano's pick 2 (Av 51 / calle 27, beside Parque Juan Vucetich): a triangle between the city
 * and the park that OSM leaves without ANY area (not even landuse=residential), where ESA WorldCover reads trees and
 * grass, and Google Open Buildings detections (tree crowns) stood as houses. A casco loop that no AREAS polygon covers
 * (at most PV_CG_MAPPED of its 6 m samples) is open green (no infill, lawn, no Overture footprint, sc_bld_green_veto)
 * when its cover is mostly vegetation (tree / shrub / grass, PV_CG_VEG). Measured over the casco's 112 unmapped loops
 * (_archive/roam/IC): 10 read 60-100 % vegetation, the rest under 50 %. Pick 2 itself reads 44 % vegetation with 18 % of
 * it under footprints and a built block on Av 31 / calle 40 reads 40 % / 24 %: neither signal separates them, so pick 2
 * is NOT covered by this rule (open). Casco only: outside it unmapped blocks under tree canopy hold real houses
 * (quintas). TD5RE_ROAM_COVER_GREEN=0 turns it off. */
#define PV_CG_STEP      (6.0 * SC_UPM)
#define PV_CG_MIN_N     20
#define PV_CG_MAPPED    0.10
#define PV_CG_VEG       0.60
static int pv_poly_has(const ScLotPoly *P, int n, double X, double Z)
{
    int i;
    for (i = 0; i < n; i++) {
        if (X < P[i].x0 || X > P[i].x1 || Z < P[i].z0 || Z > P[i].z1) continue;
        if (td5_geob_point_in_ring(P[i].px, P[i].pz, P[i].n, X, Z)) return 1;
    }
    return 0;
}

static int sc_casco_has(const TD5_RoamScnSource *s, int tx, int tz);   /* SECTION lots */
static int pv_loop_cover_green(TD5_RoamScnSource *s, TG_TileCtx *c, const double *lx, const double *lz, int m)
{
    const GrRaster *r = &c->cover3;
    double x0 = lx[0], x1 = lx[0], z0 = lz[0], z1 = lz[0], cx = 0.0, cz = 0.0, x, z;
    int k, n = 0, mapped = 0, veg = 0, tx, tz;
    if (!c->cover3_ok || !r->data || r->w <= 0 || r->h <= 0) return 0;
    for (k = 0; k < m; k++) {
        if (lx[k] < x0) x0 = lx[k];
        if (lx[k] > x1) x1 = lx[k];
        if (lz[k] < z0) z0 = lz[k];
        if (lz[k] > z1) z1 = lz[k];
        cx += lx[k]; cz += lz[k];
    }
    roam_grid_tile_of(c->grid, cx / m + (double)c->org_x, cz / m + (double)c->org_z, &tx, &tz);
    if (!sc_casco_has(s, tx, tz)) return 0;
    /* a lattice in the place frame (6 m), so every tile samples a loop at the same points */
    for (z = floor((z0 + (double)c->org_z) / PV_CG_STEP) * PV_CG_STEP + 0.5 * PV_CG_STEP; z <= z1 + (double)c->org_z; z += PV_CG_STEP)
        for (x = floor((x0 + (double)c->org_x) / PV_CG_STEP) * PV_CG_STEP + 0.5 * PV_CG_STEP; x <= x1 + (double)c->org_x; x += PV_CG_STEP) {
            int i, j, cl;
            if (!td5_geob_point_in_ring(lx, lz, m, x - (double)c->org_x, z - (double)c->org_z)) continue;
            n++;
            if (pv_poly_has(c->anyp, c->nanyp, x, z) || pv_poly_has(c->lotp, c->nlotp, x, z)) { mapped++; continue; }
            i = (int)floor((x - r->origin_x) / r->cell); j = (int)floor((z - r->origin_z) / r->cell);
            if (i < 0 || j < 0 || i >= r->w || j >= r->h) continue;
            cl = ((const unsigned char *)r->data)[(size_t)j * (size_t)r->w + (size_t)i];
            veg += cl == TD5_GEO_COVER_TREE || cl == TD5_GEO_COVER_SHRUB || cl == TD5_GEO_COVER_GRASS;
        }
    k = n >= PV_CG_MIN_N && mapped <= PV_CG_MAPPED * n && veg >= PV_CG_VEG * n;
    if (s->bld_log)
        TD5_LOG_I(LOG_TAG, "ROAM_COVERGREEN tile %d,%d loop at (%.0f,%.0f) samples %d mapped %d vegetation %d: %s",
                  c->tx, c->tz, cx / m + (double)c->org_x, cz / m + (double)c->org_z, n, mapped, veg, k ? "GREEN" : "no");
    return k;
}

#define PV_RING_SHARE   0.6
#define PV_MEDIAN_SHARE 0.6
#define PV_MEDIAN_OPPOSE 0.5
#define PV_MEDIAN_NAMES 8
static int pv_open_loops(TD5_RoamScnSource *s, TG_TileCtx *c, PvB *B, int nb)
{
    unsigned char *seen = (unsigned char *)calloc((size_t)(nb > 0 ? nb : 1), 1);
    int *seq = NULL, cap = 0, i, k, ok = 0, npts = 0, cap_pts = 0, nr = 0;
    double *pts = NULL, *lx = NULL, *lz = NULL;
    int *rf = NULL, *rn = NULL, cap_rf = 0, cap_rn = 0, cap_lx = 0, cap_lz = 0;
    unsigned char *rk = NULL;
    int cap_rk = 0;
    const int med_on = td5_env_int("TD5RE_ROAM_MEDIAN_LOOPS", 1, 0, 1);
    if (!seen) return 0;
    for (i = 0; i < nb; i++) {
        int m = 0, closed = 0, nn = 0, q, kind;
        double rib = 0.0, ring = 0.0, best = 0.0, area, per = 0.0, wid;
        uint32_t nh[PV_MEDIAN_NAMES];
        double nl[PV_MEDIAN_NAMES], nx[PV_MEDIAN_NAMES], nz[PV_MEDIAN_NAMES];
        if (seen[i]) continue;
        for (k = i; k >= 0 && m < PV_CHAIN_MAX && !seen[k]; k = B[k].next) {
            if (!sc_grow((void **)&seq, &cap, m + 1, sizeof(int))) goto out;
            seq[m++] = k;
            seen[k] = 1;
            if (B[k].next == i) { closed = 1; break; }
        }
        if (!closed || m < 3) continue;
        if (!sc_grow((void **)&lx, &cap_lx, m, sizeof(double)) || !sc_grow((void **)&lz, &cap_lz, m, sizeof(double))) goto out;
        for (k = 0; k < m; k++) {
            const PvB *b = &B[seq[k]];
            lx[k] = b->ax; lz[k] = b->az;
            per += b->len;
            if (b->kind != PV_K_RIB || !b->way) continue;
            rib += b->len;
            if (b->way->flags & PV_WF_RING) { ring += b->len; continue; }
            /* the one-way ways of each name round the loop: their kerb length and summed kerb vector */
            if (!b->way->oneway || !b->way->name_h) continue;
            for (q = 0; q < nn && nh[q] != b->way->name_h; q++) { }
            if (q == nn) {
                if (nn == PV_MEDIAN_NAMES) continue;
                nh[nn] = b->way->name_h; nl[nn] = nx[nn] = nz[nn] = 0.0; nn++;
            }
            nl[q] += b->len;
            nx[q] += b->bx - b->ax; nz[q] += b->bz - b->az;
        }
        for (q = 0; q < nn; q++) if (hypot(nx[q], nz[q]) < PV_MEDIAN_OPPOSE * nl[q]) best += nl[q];   /* two opposite sides */
        area = td5_geob_ring_area(lx, lz, m);
        if (!(rib > 0.0) || !(area < 0.0)) continue;     /* the road on the left: land inside */
        wid = per > 0.0 ? 2.0 * -area / per / SC_UPM : 0.0;
        if (s->bld_log && -area > 400.0 * SC_UPM * SC_UPM)
            TD5_LOG_I(LOG_TAG, "ROAM_LOOP tile %d,%d at (%.0f,%.0f) area %.0f m2 width %.1f m rib %.0f m ring %.2f "
                      "opposed-names %.2f", c->tx, c->tz, lx[0] + (double)c->org_x, lz[0] + (double)c->org_z,
                      -area / (SC_UPM * SC_UPM), wid, rib / SC_UPM, ring / rib, best / rib);
        if (ring >= PV_RING_SHARE * rib) kind = 1;
        else if (med_on && best >= PV_MEDIAN_SHARE * rib) kind = 2;
        else if (s->cover_green && pv_loop_cover_green(s, c, lx, lz, m)) kind = 3;
        else continue;
        if (!sc_grow((void **)&pts, &cap_pts, (npts + m) * 2, sizeof(double)) ||
            !sc_grow((void **)&rf, &cap_rf, nr + 1, sizeof(int)) || !sc_grow((void **)&rn, &cap_rn, nr + 1, sizeof(int)) ||
            !sc_grow((void **)&rk, &cap_rk, nr + 1, 1)) goto out;
        for (k = 0; k < m; k++) {
            pts[(npts + k) * 2 + 0] = lx[k] + (double)c->org_x;
            pts[(npts + k) * 2 + 1] = lz[k] + (double)c->org_z;
        }
        rf[nr] = npts; rn[nr] = m; rk[nr] = (unsigned char)kind; nr++;
        npts += m;
    }
    /* de-interleave into the ctx's own pool, then the GREEN polygons */
    if (nr > 0) {
        double *pool = (double *)malloc((size_t)npts * 2u * sizeof(double));
        int cap_p = c->nlotp;
        if (!pool) goto out;
        for (k = 0; k < npts; k++) { pool[k] = pts[k * 2]; pool[npts + k] = pts[k * 2 + 1]; }
        free(c->ringpool);
        c->ringpool = pool;
        for (i = 0; i < nr; i++) {
            ScLotPoly q;
            int j;
            q.px = pool + rf[i]; q.pz = pool + npts + rf[i]; q.n = rn[i]; q.cls = SC_LC_GREEN; q.plaza = 1;
            q.x0 = q.x1 = q.px[0]; q.z0 = q.z1 = q.pz[0];
            for (j = 1; j < q.n; j++) {
                if (q.px[j] < q.x0) q.x0 = q.px[j];
                if (q.px[j] > q.x1) q.x1 = q.px[j];
                if (q.pz[j] < q.z0) q.z0 = q.pz[j];
                if (q.pz[j] > q.z1) q.z1 = q.pz[j];
            }
            if (!sc_grow((void **)&c->lotp, &cap_p, c->nlotp + 1, sizeof(ScLotPoly))) goto out;
            c->lotp[c->nlotp++] = q;
            if (rk[i] == 1) c->st.lot_rings++; else if (rk[i] == 2) c->st.lot_medians++; else c->st.lot_cover_green++;
        }
    }
    ok = 1;
out:
    free(seen); free(seq); free(pts); free(rf); free(rn); free(rk); free(lx); free(lz);
    return ok;
}

/* [IC] The pavement's ways and kerb pieces of the 3x3, gathered before the buildings: the ring plazas and avenue
 * medians (pv_open_loops) must be known when a footprint or an infill lot is judged. sc_emit_pavement consumes them. */
static int sc_pv_prepare(TD5_RoamScnSource *s, TG_TileCtx *c, ScCache **bc, int nbc)
{
    double corner[4][2], x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
    const double S = (double)s->grid.tile;
    PvWay **ways;
    PvB *B = NULL;
    int nways = 0, nb = 0, i;
    c->pv_ready = 0;
    if (!s->pv || !s->pv->on) return 1;
    roam_grid_corners(&s->grid, c->tx, c->tz, corner);
    for (i = 0; i < 4; i++) {
        if (corner[i][0] < x0) x0 = corner[i][0];
        if (corner[i][0] > x1) x1 = corner[i][0];
        if (corner[i][1] < z0) z0 = corner[i][1];
        if (corner[i][1] > z1) z1 = corner[i][1];
    }
    ways = pv_window(s, x0 - S, z0 - S, x1 + S, z1 + S, &nways);
    if (!pv_gather(s, c, ways, nways, &B, &nb)) { free(ways); return 0; }
    c->pv_ways = ways; c->pv_nways = nways; c->pv_B = B; c->pv_nb = nb; c->pv_ready = 1;
    (void)bc; (void)nbc;
    return pv_open_loops(s, c, B, nb);     /* ring plazas + medians + unmapped green: no infill (before any chain) */
}

static int sc_emit_pavement(TD5_RoamScnSource *s, TG_TileCtx *c, ScCache **bc, int nbc, const ScArea *ar, int nar)
{
    const uint32_t t0 = td5_plat_time_ms();
    PvWay **ways = (PvWay **)c->pv_ways;
    PvB *B = (PvB *)c->pv_B;
    PvBld bld;
    PvEmit e;
    const int nways = c->pv_nways, nb = c->pv_nb;
    int i, ok = 0, sb;
    if (!s->pv || !s->pv->on || !c->pv_ready) return 1;
    memset(&bld, 0, sizeof bld);
    if (!pv_bld_build(s, c, bc, nbc, ar, nar, &bld, 1)) goto out;
    memset(&e, 0, sizeof e);
    e.s = s; e.c = c; e.ways = ways; e.nways = nways; e.bld = &bld; e.ar = ar; e.nar = nar;
    e.bat = (ScFpBatch *)calloc((size_t)TD5_ROAM_SCN_NSB * PV_NB, sizeof(ScFpBatch));
    if (!e.bat) goto out;
    for (sb = 0; sb < TD5_ROAM_SCN_NSB; sb++) {
        e.bat[sb * PV_NB + PV_B_PAVE].page = TD5_TG_PAGE_SIDEWALK;
        e.bat[sb * PV_NB + PV_B_LAWN].page = TD5_TG_PAGE_R3_BLOCK + 0;   /* the F4a lawn page */
        e.bat[sb * PV_NB + PV_B_FENCE].page = TD5_TG_PAGE_FENCE;
    }
    for (i = 0; i < nb; i++)
        if (B[i].owned && !B[i].done && !pv_chain_emit(&e, B, nb, i)) { free(e.bat); goto out; }
    ok = 1;
    for (sb = 0; sb < TD5_ROAM_SCN_NSB && ok; sb++) {
        int q;
        for (q = 0; q < PV_NB && ok; q++)
            if (!sc_fp_flush(c, sb, &e.bat[sb * PV_NB + q])) ok = 0;
    }
    free(e.bat);
out:
    pv_bld_free(&bld);
    c->st.ms_pv += td5_plat_time_ms() - t0;
    return ok;
}

/* ============================================================================
 * SECTION: lots [HD] (the city's open ground paved)
 *
 * WHY. Mariano, 2026-10-19: "you need to fill much more the green spaces that are not parks in the city, it's
 * mostly concrete and buildings". The roam ground is one lawn page everywhere (td5_roam_render.c cmd 1), so every
 * backyard, every gap between two footprints and every vacant lot of the casco read as a park.
 *
 * LOTS. Each ground cell of the tile, on the RENDER's own lattice (td5_roam_render.c rr_ground_point: same
 * corners, TD5RE_ROAM_RENDER_CELL_M; same heights: the raster minus the G5 lowering, minus the ground drop), is
 * laid again SC_LOT_LIFT over the drawn ground on a concrete page: ONE quad in the drawn ground's own vertex
 * order (the pipeline splits every quad 0 1 2 / 0 2 3, so the two faces are parallel and never z-fight) or, where
 * the cell meets the drivable surface, its two fan triangles refined SC_LOT_DEPTH times, the pieces on the road
 * dropped, every new height interpolated in its parent's plane (still parallel). Under the road the drawn ground
 * is held at least the drop down, so a lot never shows through the asphalt; the guard keeps it off anyway.
 *
 * WHICH CELLS. CITY = the tile is one of the casco's (td5_roam_casco_tiles), or (TD5RE_ROAM_LOT_COVER, outside
 * it) at least SC_LOT_BUILT of the cell's 4 x 4 COVER samples are built-up. A city cell is paved unless most of
 * its 5 samples (centre + 4 quarter points) lie in an OSM GREEN (sc_lot_class: park, grass, garden, pitch, forest,
 * cemetery, ...) or its centre is water; the render's bore holes are left alone. A plaza lawn stands SC_GEOP_LIFT
 * over the raster, far above a lot, so a park keeps its lawn over a paved border cell.
 *
 * CENSUS (st.lot_*). The tile's cells are classified with the same refinement whether or not they are paved, so
 * lot_lawn_m2 (non-road ground, no OSM green, drawn as lawn) is comparable before / after (TD5RE_ROAM_LOTS=0).
 * ========================================================================== */

#define SC_LOT_LIFT    30.0          /* units over the DRAWN ground (7 cm)                       */
#define SC_LOT_DEPTH   3             /* road-cut refinement: a 16 m cell's pieces down to 2 m      */
#define SC_LOT_BUILT   10            /* of 16 COVER samples built-up: a city cell outside the casco */
#define SC_LOT_UV      (1.0 / 3440.0)  /* one repeat per 8 m                                       */
#define SC_LOT_CHUNK   64            /* primitives per mesh                                        */

/* What a lot under an AREAS polygon becomes, by its raw OSM kind: GREEN keeps the lawn (and takes no infill),
 * OPEN is paved but takes no infill (a car park, a school yard, a rail yard), BUILD is the city fabric. */
static int sc_lot_class(const char *k)
{
    static const char *const green[] = {
        "park", "grass", "grassland", "garden", "village_green", "common", "recreation_ground", "golf_course",
        "meadow", "greenfield", "forest", "wood", "orchard", "farmland", "plant_nursery", "wetland", "pitch",
        "playground", "cemetery", "grave_yard", "heath", "scrub", "allotments", "nature_reserve", "dog_park",
        "vineyard", "flowerbed", "zoo", "camp_site", "picnic_site", NULL };
    static const char *const open[] = {
        "parking", "school", "university", "college", "kindergarten", "hospital", "sports_centre", "stadium",
        "railway", "construction", "bleachers", "track", "prison", "religious", "education", "swimming_pool",
        "marketplace", "quarry", "brownfield", "greenhouse_horticulture", "farmyard", "sand", "landfill",
        "military", "depot", "garages", "fuel", "bus_station", NULL };
    int i;
    if (!k || !k[0]) return SC_LC_BUILD;
    for (i = 0; green[i]; i++) if (!strcmp(k, green[i])) return SC_LC_GREEN;
    for (i = 0; open[i]; i++) if (!strcmp(k, open[i])) return SC_LC_OPEN;
    return SC_LC_BUILD;
}


/* every AREAS polygon of class != BUILD whose box meets [x0,x1]x[z0,z1] (place frame), from every pinned cache
 * slot (a slot's extent covers its whole records, so this is every polygon that reaches the box) */
static int sc_lot_polys(ScCache **ac, int nac, double x0, double z0, double x1, double z1, ScLotPoly **out, int *nout,
                        int any)
{
    ScLotPoly *P = NULL;
    int n = 0, cap = 0, l, r, k;
    *out = NULL; *nout = 0;
    for (l = 0; l < nac; l++) {
        const ScCache *c = ac[l];
        if (!c->lot) continue;
        for (r = 0; r < c->n; r++) {
            const TD5_GeoArea *a = &c->a[r];
            ScLotPoly q;
            if ((!any && c->lot[r] == SC_LC_BUILD) || a->n < 3) continue;   /* [IC] any: every class (c->anyp) */
            if (a->cx + a->radius < x0 || a->cx - a->radius > x1 || a->cz + a->radius < z0 || a->cz - a->radius > z1) continue;
            q.px = &c->px[a->first]; q.pz = &c->pz[a->first]; q.n = a->n; q.cls = c->lot[r];
            q.plaza = td5_geob_area_is_plaza(a);
            q.x0 = q.x1 = q.px[0]; q.z0 = q.z1 = q.pz[0];
            for (k = 1; k < a->n; k++) {
                if (q.px[k] < q.x0) q.x0 = q.px[k];
                if (q.px[k] > q.x1) q.x1 = q.px[k];
                if (q.pz[k] < q.z0) q.z0 = q.pz[k];
                if (q.pz[k] > q.z1) q.z1 = q.pz[k];
            }
            if (!sc_grow((void **)&P, &cap, n + 1, sizeof *P)) { free(P); return 0; }
            P[n++] = q;
        }
    }
    *out = P; *nout = n;
    return 1;
}

/* the strongest class of the polygons over a place-frame point: GREEN over OPEN over BUILD */
static int sc_lot_class_at(const ScLotPoly *P, int n, double x, double z)
{
    int i, best = SC_LC_BUILD;
    for (i = 0; i < n; i++) {
        if (x < P[i].x0 || x > P[i].x1 || z < P[i].z0 || z > P[i].z1) continue;
        if (best == SC_LC_GREEN || (best == SC_LC_OPEN && P[i].cls == SC_LC_OPEN)) continue;
        if (td5_geob_point_in_ring(P[i].px, P[i].pz, P[i].n, x, z)) best = P[i].cls;
    }
    return best;
}

/* COVER class at a tile-local point (TD5_GEO_COVER_*), NONE without a window */
static int sc_cover_at(const TG_TileCtx *c, double x, double z)
{
    const GrRaster *r = &c->cover;
    double fx, fz;
    int i, j;
    if (!c->cover_ok || !r->data || r->w <= 0 || r->h <= 0) return TD5_GEO_COVER_NONE;
    fx = (x + (double)c->org_x - r->origin_x) / r->cell;
    fz = (z + (double)c->org_z - r->origin_z) / r->cell;
    i = (int)floor(fx); j = (int)floor(fz);
    if (i < 0 || j < 0 || i >= r->w || j >= r->h) return TD5_GEO_COVER_NONE;
    return ((const unsigned char *)r->data)[(size_t)j * (size_t)r->w + (size_t)i];
}

static int sc_casco_has(const TD5_RoamScnSource *s, int tx, int tz)
{
    const int64_t k = sc_tkey(tx, tz);
    int a = 0, b = s->ncasco;
    while (a < b) { const int m = (a + b) / 2; if (s->casco[m] < k) a = m + 1; else b = m; }
    return a < s->ncasco && s->casco[a] == k;
}

/* one sub-block's pending lot quads, x y z per corner */
typedef struct { double *q; int nq, cq; } ScLotSb;

static int sc_lot_put(double **a, int *n, int *cap, const double *p, int np)
{
    if (!sc_grow((void **)a, cap, (*n + 1) * np * 3, sizeof(double))) return 0;
    memcpy(*a + (size_t)*n * (size_t)np * 3u, p, (size_t)np * 3u * sizeof(double));
    (*n)++;
    return 1;
}

/* The non-road area (m2, added to *area_ok) of triangle p (x y z x3): whole off the road, else split `lvl` more
 * times and the pieces still on it left out (census only: a paved cell is laid whole, under the road too) */
static int sc_lot_split(TG_TileCtx *c, const double *p, int lvl, double *area_ok)
{
    const double x[3] = { p[0], p[3], p[6] }, z[3] = { p[2], p[5], p[8] };
    if (!sc_on_road(c, x, z, 3, NULL)) {
        *area_ok += fabs(sc_cross(x[0], z[0], x[1], z[1], x[2], z[2])) * 0.5 / (SC_UPM * SC_UPM);
        return 1;
    }
    if (lvl <= 0) return 1;
    {
        double m[3][3], q[4][9];
        int k, e;
        for (e = 0; e < 3; e++) for (k = 0; k < 3; k++) m[e][k] = 0.5 * (p[e * 3 + k] + p[((e + 1) % 3) * 3 + k]);
        memcpy(q[0] + 0, p + 0, sizeof m[0]); memcpy(q[0] + 3, m[0], sizeof m[0]); memcpy(q[0] + 6, m[2], sizeof m[0]);
        memcpy(q[1] + 0, m[0], sizeof m[0]); memcpy(q[1] + 3, p + 3, sizeof m[0]); memcpy(q[1] + 6, m[1], sizeof m[0]);
        memcpy(q[2] + 0, m[2], sizeof m[0]); memcpy(q[2] + 3, m[1], sizeof m[0]); memcpy(q[2] + 6, p + 6, sizeof m[0]);
        memcpy(q[3] + 0, m[0], sizeof m[0]); memcpy(q[3] + 3, m[1], sizeof m[0]); memcpy(q[3] + 6, m[2], sizeof m[0]);
        for (k = 0; k < 4; k++) if (!sc_lot_split(c, q[k], lvl - 1, area_ok)) return 0;
    }
    return 1;
}

static int sc_lot_flush(TG_TileCtx *c, int sbi, ScLotSb *L, int page)
{
    const double rep = 1.0 / SC_LOT_UV;
    const double uox = fmod((double)c->org_x, rep), uoz = fmod((double)c->org_z, rep);
    int qi = 0;
    while (qi < L->nq) {
        const int nq = (L->nq - qi < SC_LOT_CHUNK) ? L->nq - qi : SC_LOT_CHUNK;
        float v[SC_LOT_CHUNK * 4 * 5];
        unsigned int light[SC_LOT_CHUNK * 4];
        unsigned short cmd[3];
        ScSb *b = sc_sb(c, sbi, 1);
        int i;
        if (!b) return 0;
        for (i = 0; i < nq * 4; i++) {
            const double *P = &L->q[(size_t)(qi * 4 + i) * 3u];
            v[i * 5 + 0] = (float)P[0]; v[i * 5 + 1] = (float)P[1]; v[i * 5 + 2] = (float)P[2];
            v[i * 5 + 3] = (float)((P[0] + uox) * SC_LOT_UV); v[i * 5 + 4] = (float)((P[2] + uoz) * SC_LOT_UV);
            light[i] = 0xFFFFFFFFu;
        }
        cmd[0] = (unsigned short)page; cmd[1] = 0; cmd[2] = (unsigned short)nq;
        b->moff[b->nmesh++] = b->blk.len;
        if (!tg_write_prefab_mesh(&b->blk, v, light, nq * 4, cmd, 1, 0, 0.0, 0.0, 0.0, 1.0, 0.0) || b->blk.oom) return 0;
        qi += nq;
    }
    return 1;
}

static int sc_emit_lots(TD5_RoamScnSource *s, TG_TileCtx *c, const RoamTile *self, ScCache **ac, int nac)
{
    const int cell_m = td5_env_int("TD5RE_ROAM_RENDER_CELL_M", 16, 8, 64);
    const int G = 256 / ((64 % cell_m) ? 16 : cell_m), per = G / TD5_ROAM_SCN_SUB, W = G + 1;
    const double S = (double)c->grid->tile;
    const double drop = td5_env_int("TD5RE_ROAM_RENDER_GROUND_DROP_CM", 40, 0, 500) * (SC_UPM / 100.0);
    const int casco = sc_casco_has(s, c->tx, c->tz);
    ScLotSb L[TD5_ROAM_SCN_NSB];
    const ScLotPoly *P = NULL;
    double *px = NULL, *pz = NULL, *py = NULL, x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
    unsigned char *hole = NULL;
    int nP = 0, i, j, k, ok = 0;
    uint32_t e, q;
    if (!c->hc) return 1;
    memset(L, 0, sizeof L);
    px = (double *)malloc((size_t)W * W * sizeof(double));
    pz = (double *)malloc((size_t)W * W * sizeof(double));
    py = (double *)malloc((size_t)W * W * sizeof(double));
    hole = (unsigned char *)calloc((size_t)G * G, 1);
    if (!px || !pz || !py || !hole) goto out;
    /* the render's lattice points (rr_ground_point), tile-local, and their drawn heights */
    for (j = 0; j <= G; j++) for (i = 0; i <= G; i++) {
        const double u = ((double)c->tx + (double)i / (double)G) * S, v = ((double)c->tz + (double)j / (double)G) * S;
        const double X = c->grid->ox + c->grid->c * u - c->grid->s * v, Z = c->grid->oz + c->grid->s * u + c->grid->c * v;
        px[j * W + i] = X - (double)c->org_x; pz[j * W + i] = Z - (double)c->org_z;
        py[j * W + i] = sc_ground(c, px[j * W + i], pz[j * W + i]) - drop + SC_LOT_LIFT;
        if (X < x0) x0 = X;
        if (X > x1) x1 = X;
        if (Z < z0) z0 = Z;
        if (Z > z1) z1 = Z;
    }
    /* the render's bore holes (rr_ground_build): every cell round a tunnel span centre */
    for (e = 0; e < self->nedge; e++) {
        const RoamEdgeRec *er = &self->edge[e];
        if (!(er->flags & ROAM_EF_BORE)) continue;
        for (q = 0; q < er->span_count; q++) {
            const uint32_t si = (uint32_t)er->span_first + q;
            const TD5_StripSpan *sp;
            uint32_t Lv, Rv;
            int n, a, b, da, db;
            double u, v, X, Z;
            if (si >= self->nspan) continue;
            sp = &self->span[si];
            Lv = sp->left_vertex_index; Rv = sp->right_vertex_index; n = (int)(sp->pad_02[1] & 0x0Fu);
            if (n < 1 || n >= 16 || Lv + (uint32_t)n >= self->nvert || Rv + (uint32_t)n >= self->nvert) continue;
            X = (double)self->org_x + (double)sp->origin_x + 0.5 * ((double)self->vert[Lv].x + (double)self->vert[Rv + (uint32_t)n].x);
            Z = (double)self->org_z + (double)sp->origin_z + 0.5 * ((double)self->vert[Lv].z + (double)self->vert[Rv + (uint32_t)n].z);
            roam_grid_uv(c->grid, X, Z, &u, &v);
            a = (int)floor((u / S - (double)c->tx) * (double)G);
            b = (int)floor((v / S - (double)c->tz) * (double)G);
            for (db = -1; db <= 1; db++) for (da = -1; da <= 1; da++)
                if (a + da >= 0 && a + da < G && b + db >= 0 && b + db < G) hole[(b + db) * G + a + da] = 1;
        }
    }
    (void)ac; (void)nac; (void)x0; (void)z0; (void)x1; (void)z1;
    P = c->lotp; nP = c->nlotp;
    for (j = 0; j < G; j++) for (i = 0; i < G; i++) {
        static const int di[4] = { 0, 1, 1, 0 }, dj[4] = { 0, 0, 1, 1 };
        static const double fq[5][2] = { { 0.5, 0.5 }, { 0.25, 0.25 }, { 0.75, 0.25 }, { 0.75, 0.75 }, { 0.25, 0.75 } };
        double cx = 0.0, cz = 0.0, cell[12], area = 0.0;
        int city = casco, green, pave, ng = 0;
        const int sbi = (j / per) * TD5_ROAM_SCN_SUB + (i / per);
        for (k = 0; k < 4; k++) {
            const int pi = (j + dj[k]) * W + i + di[k];
            cell[k * 3 + 0] = px[pi]; cell[k * 3 + 1] = py[pi]; cell[k * 3 + 2] = pz[pi];
            cx += 0.25 * px[pi]; cz += 0.25 * pz[pi];
        }
        if (hole[j * G + i] || sc_cover_at(c, cx, cz) == TD5_GEO_COVER_WATER) { c->st.lot_skip++; continue; }
        if (!city && s->lot_cover) {          /* built-up cover, 4 x 4 samples over the cell */
            int a, b, built = 0;
            for (b = 0; b < 4; b++) for (a = 0; a < 4; a++) {
                const double fu = (a + 0.5) / 4.0, fv = (b + 0.5) / 4.0;
                const double sx = cell[0] + (cell[3] - cell[0]) * fu + (cell[9] - cell[0]) * fv;
                const double sz = cell[2] + (cell[5] - cell[2]) * fu + (cell[11] - cell[2]) * fv;
                built += sc_cover_at(c, sx, sz) == TD5_GEO_COVER_BUILT;
            }
            city = built >= SC_LOT_BUILT;
        }
        for (k = 0; k < 5; k++) {             /* OSM green: most of centre + 4 quarter points */
            const double sx = cell[0] + (cell[3] - cell[0]) * fq[k][0] + (cell[9] - cell[0]) * fq[k][1];
            const double sz = cell[2] + (cell[5] - cell[2]) * fq[k][0] + (cell[11] - cell[2]) * fq[k][1];
            ng += sc_lot_class_at(P, nP, sx + (double)c->org_x, sz + (double)c->org_z) == SC_LC_GREEN;
        }
        green = ng >= 3;
        pave = s->lots_on && city && !green;
        {
            const double t0[9] = { cell[0], cell[1], cell[2], cell[3], cell[4], cell[5], cell[6], cell[7], cell[8] };
            const double t1[9] = { cell[0], cell[1], cell[2], cell[6], cell[7], cell[8], cell[9], cell[10], cell[11] };
            const double qx[4] = { cell[0], cell[3], cell[6], cell[9] }, qz[4] = { cell[2], cell[5], cell[8], cell[11] };
            if (pave && !sc_lot_put(&L[sbi].q, &L[sbi].nq, &L[sbi].cq, cell, 4)) goto out;
            if (!sc_on_road(c, qx, qz, 4, NULL)) area = fabs(td5_geob_ring_area(qx, qz, 4)) / (SC_UPM * SC_UPM);
            else {
                if (pave) c->st.lot_under_road++;
                if (!sc_lot_split(c, t0, SC_LOT_DEPTH, &area) || !sc_lot_split(c, t1, SC_LOT_DEPTH, &area)) goto out;
            }
        }
        if (green) { c->st.lot_green++; c->st.lot_green_m2 += area; }
        else if (pave) { c->st.lot_cells++; c->st.lot_paved_m2 += area; }
        else { if (!city) c->st.lot_rural++; c->st.lot_lawn_m2 += area; }
    }
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) if (!sc_lot_flush(c, k, &L[k], s->lot_page)) goto out;
    ok = 1;
out:
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) free(L[k].q);
    free(px); free(pz); free(py); free(hole);
    return ok;
}

/* ---- [HD] INFILL: frontage lots on the building line ----
 *
 * WHY. Mariano, 2026-10-19: the casco is "mostly concrete and buildings" (buildings to the block edge, the La Plata
 * block fabric). The OSM / Overture footprints cover most of a block's frontage but leave gaps; the lots (SECTION
 * lots) pave the gaps, this fills the frontage gaps with a built mass.
 *
 * WHERE. A CLOSED pavement chain with the road on its left is a block's kerb loop; its outer line (pv_offset_chain)
 * is the block's building line. The line is cut into RUNS at its corners (a turn over INF_BREAK_DEG) and where it
 * stops being a street front (an island, a deck, a bore, no pavement), and each run into LOTS at the world lattice
 * lines (roam_grid_uv, INF_LOT_M apart along the run's dominant axis), so a lot is a pure function of the chain's
 * geometry and comes out the same from every tile that sees the block. A lot reaches INF_DEPTH_MIN..MAX into the
 * block (per-lot hash), never past INF_HALF of the block's own depth there; its back corners at a run end follow the
 * corner's mitre, so the two lots of a block corner meet on the bisector instead of overlapping.
 *
 * REFUSED: a real footprint on its front (the frontage is taken; a lot whose back meets one is cut to the front
 * half, else refused), the drivable surface, an OSM area that takes none (green or open, sc_lot_class), a landmark
 * within INF_LM_CLEAR_M, ground that is not CITY (sc_emit_lots' rule), a lot too shallow, bent or self-crossing.
 * OWNER: the tile holding the lot's front midpoint. The mass is the shared extrusion core with a synthetic flat-roof
 * footprint: INF_LEVELS[hash] storeys of 3 m, a facade page by its hash.
 *
 * [IC] Mariano, 2026-10-20: "these blocks are practically empty ... can we fall back to adding buildings if OSM does
 * not say explicitly that it is a park or that it is not green space?". The La Plata fabric: row houses on 8-10 m
 * fronts, 1-3 floors on the streets, taller on the avenues, deep lots. So the storeys come from the host street's
 * class (k_inf_levels_street / _avenue, an avenue = tertiary or above) and the lot reaches 12-24 m in. A footprint
 * the emitter drops no longer takes the frontage (sc_bld_drops), and a lot is refused when ANY of its sample points
 * (every quad's inset corners and centre) lies on a green / open polygon, not only its front and middle: a park
 * drawn inside its block's pavement no longer gets a ring of houses between the kerb and the lawn.
 */
#define INF_LOT_M       10.0          /* lot frontage (lattice lines this far apart)    */
#define INF_MIN_M       4.0           /* a piece shorter than this joins its neighbour  */
#define INF_DEPTH_MIN   12.0
#define INF_DEPTH_SPAN  13            /* depth = MIN + hash % SPAN metres               */
#define INF_SHALLOW_M   5.0
#define INF_HALF        0.48          /* of the block's depth in front of a lot          */
#define INF_BREAK_DEG   25.0
#define INF_MITRE_MAX   2.5
#define INF_BLOCK_M2    400.0         /* a smaller loop is an island, not a block       */
#define INF_LM_CLEAR_M  15.0
#define INF_PTS         24            /* front points per lot                            */
#define INF_SPLIT_MIN_M 3.0           /* a taken lot splits into halves no narrower      */
#define INF_STOREY_W    2.5           /* metres of frontage per storey at most (a street)   */
#define INF_STOREY_W_AV 1.25          /* ... on an avenue (a 10 m front takes 8 storeys)     */
#define INF_FRONT_IN    65.0          /* units (15 cm): the front stands off the slab's back face */
static const unsigned char k_inf_levels_street[16] = { 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3 };
static const unsigned char k_inf_levels_avenue[16] = { 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 8, 10 };

static uint32_t inf_hash(double X, double Z)
{
    const int32_t q[2] = { (int32_t)floor(X / 43.0), (int32_t)floor(Z / 43.0) };   /* 10 cm, place frame */
    uint32_t h = sc_fnv(2166136261u, q, sizeof q);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}

/* nearest crossing of the ray (px,pz)+t(dx,dz), t > 0, with the ring O (n points), skipping segment `skip`; 1e300 = none */
static double inf_ray(const double *ox, const double *oz, int n, int skip, double px, double pz, double dx, double dz)
{
    double best = 1e300;
    int i;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double ex = ox[j] - ox[i], ez = oz[j] - oz[i];
        const double den = dx * ez - dz * ex;
        double t, s;
        if (i == skip || fabs(den) < 1e-9) continue;
        t = ((ox[i] - px) * ez - (oz[i] - pz) * ex) / den;
        s = ((ox[i] - px) * dz - (oz[i] - pz) * dx) / den;
        if (t > 1.0 && s >= 0.0 && s <= 1.0 && t < best) best = t;
    }
    return best;
}

/* a real footprint overlaps the convex quad q (tile-local) */
static int inf_quad_in_building(const TG_TileCtx *c, const PvBld *pb, const double *qx, const double *qz)
{
    double X[4], Z[4], x0, x1, z0, z1;
    int k, a, b, a0, a1, b0, b1;
    for (k = 0; k < 4; k++) { X[k] = qx[k] + (double)c->org_x; Z[k] = qz[k] + (double)c->org_z; }
    x0 = x1 = X[0]; z0 = z1 = Z[0];
    for (k = 1; k < 4; k++) { if (X[k] < x0) x0 = X[k]; if (X[k] > x1) x1 = X[k]; if (Z[k] < z0) z0 = Z[k]; if (Z[k] > z1) z1 = Z[k]; }
    if (!pb->start) return 0;
    a0 = (int)floor((x0 - pb->gx0) / pb->cell); a1 = (int)floor((x1 - pb->gx0) / pb->cell);
    b0 = (int)floor((z0 - pb->gz0) / pb->cell); b1 = (int)floor((z1 - pb->gz0) / pb->cell);
    if (a0 < 0) a0 = 0;
    if (b0 < 0) b0 = 0;
    if (a1 >= pb->gnx) a1 = pb->gnx - 1;
    if (b1 >= pb->gnz) b1 = pb->gnz - 1;
    for (b = b0; b <= b1; b++) for (a = a0; a <= a1; a++) {
        const int cell = b * pb->gnx + a;
        for (k = pb->start[cell]; k < pb->start[cell + 1]; k++) {
            const PvRing *r = &pb->r[pb->item[k]];
            if (r->x1 < x0 || r->x0 > x1 || r->z1 < z0 || r->z0 > z1) continue;
            if (sc_poly_overlap(r->px, r->pz, r->n, X, Z, 4)) return 1;
        }
    }
    return 0;
}

/* a landmark footprint lies within d (units) of the place point */
static int inf_near_landmark(const PvBld *pb, double X, double Z, double d)
{
    int i, k;
    for (i = 0; i < pb->nr; i++) {
        const PvRing *r = &pb->r[i];
        if (!r->lm || r->x1 < X - d || r->x0 > X + d || r->z1 < Z - d || r->z0 > Z + d) continue;
        for (k = 0; k < r->n; k++) if (hypot(r->px[k] - X, r->pz[k] - Z) <= d) return 1;
        if (td5_geob_point_in_ring(r->px, r->pz, r->n, X, Z)) return 1;
    }
    return 0;
}

/* Extrude one accepted lot ring (tile-local, any winding, n points) as a flat-roof mass. */
static int inf_emit(PvEmit *e, const double *rx0, const double *rz0, int n, uint32_t h, double area_units, double width)
{
    TG_TileCtx *c = e->c;
    TD5_RoamScnSource *s = e->s;
    double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], cx = 0.0, cz = 0.0, r = 0.0, along = 0.0, lat = 0.0;
    TD5_GeoBuilding gb;
    ScBldU u;
    TG_GeoBldEnv env;
    ScSb *b;
    int k, edge = -1, side = 1;
    const int rev = td5_geob_ring_area(rx0, rz0, n) < 0.0;
    for (k = 0; k < n; k++) { rx[k] = rev ? rx0[n - 1 - k] : rx0[k]; rz[k] = rev ? rz0[n - 1 - k] : rz0[k]; cx += rx[k]; cz += rz[k]; }
    cx /= n; cz /= n;
    for (k = 0; k < n; k++) { const double d = hypot(rx[k] - cx, rz[k] - cz); if (d > r) r = d; }
    memset(&gb, 0, sizeof gb);
    gb.n = n;
    gb.cx = cx + (double)c->org_x; gb.cz = cz + (double)c->org_z;
    gb.radius = r;
    memset(&u, 0, sizeof u);
    u.c = c;
    u.host = roam_nearest_edge(c, cx, cz, SC_HOST_MAX_M * SC_UPM, &edge, &side, &along, &lat)
          && roam_edge_frame(c, edge, side, along, &u.f);
    {   /* [IC] storeys by the host street's class; a narrow front is a low house, not a pencil tower: at most one
         * storey per INF_STOREY_W (an avenue INF_STOREY_W_AV) of frontage */
        static const unsigned char k_inf_levels_m[16] = { 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 7 };   /* round M */
        const int avenue = s->ic_infill && u.host && edge >= 0 && edge < c->nedge && c->edge[edge].klass >= TD5_GEO_RC_TERTIARY;
        const int cap = (int)(width / ((avenue ? INF_STOREY_W_AV : INF_STOREY_W) * s->upm));
        gb.levels = (!s->ic_infill ? k_inf_levels_m : avenue ? k_inf_levels_avenue : k_inf_levels_street)[(h >> 8) & 15u];
        if (gb.levels > (cap > 1 ? cap : 1)) gb.levels = (unsigned short)(cap > 1 ? cap : 1);
    }
    gb.height = (double)gb.levels * 3.0 * s->upm;
    gb.area_m2 = area_units / (s->upm * s->upm);
    gb.id_hash = h;
    gb.host_span = -1;
    gb.hsrc = TD5_GEOB_HSRC_ESTIMATED;
    gb.roof = TD5_GEOB_ROOF_NONE;
    gb.fsrc = TD5_GEOB_FSRC_OVERTURE;
    u.side = side;
    u.minout = u.host ? u.f.half_carriage + s->gap : 0.0;
    u.gap = s->gap;
    b = sc_sb(c, sc_sb_of(c, cx, cz), 4);
    if (!b) return 0;
    memset(&env, 0, sizeof env);
    env.blk = &b->blk; env.moff = b->moff; env.nmesh = &b->nmesh; env.maxmesh = b->cap;
    env.b = &k_biomes[0];
    env.ground = sc_ground_u; env.u = &u;
    env.have_ceiling = u.host;
    env.ceiling_y = u.f.py + tg_city_kerb_h(env.b) + 400.0;
    env.have_pf_ceiling = u.host;
    env.pf_ceiling_y = env.ceiling_y;
    env.upm = s->upm;
    env.storey = 3.0 * s->upm;
    env.corner_ok = sc_bld_corner_ok;
    env.rect_on_road = sc_bld_rect_on_road;
    env.apron_ok = sc_bld_apron_ok;
    env.apron_quad_ok = sc_bld_apron_quad_ok;
    env.si = -1;
    env.st = &c->bst;
    {
        const long emitted0 = c->bst.emitted;
        if (!tg_geo_emit_building_core(&env, &gb, rx, rz, n, cx, cz, 0.0) || b->blk.oom) return 0;
        if (c->bst.emitted > emitted0) { c->st.inf_emitted++; c->st.inf_m2 += gb.area_m2; c->st.inf_m_built += width / s->upm; }
        if (s->bld_log && c->bst.emitted > emitted0)
            TD5_LOG_I(LOG_TAG, "ROAM_BLD tile %d,%d id %u src infill c (%.0f,%.0f) area %.0f h %.1f: emitted", c->tx, c->tz,
                      h, gb.cx, gb.cz, gb.area_m2, gb.height / s->upm);
    }
    return 1;
}

/* one block's building line, as the infill reads it (tile-local) */
typedef struct {
    int     n;
    double *ox, *oz;                 /* building-line vertices                          */
    double *nx, *nz;                 /* per segment i -> i+1: unit normal into the block */
    double *bx, *bz, *bm;            /* per vertex: unit bisector, mitre factor          */
    double *arc;                     /* the current run: arc length at its k-th vertex   */
    int     r0, cnt;                 /* the current run: first vertex, segment count     */
} InfLine;

/* the lot [a0, a1] (arc length along the current run) at depth D: front points, their back points, the front
 * midpoint and the segment holding it; 0 when degenerate */
static int inf_lot_geom(const InfLine *L, double a0, double a1, double D, double *fx, double *fz, double *gx,
                        double *gz, int *nf_out, double *mx, double *mz, int *seg_mid)
{
    int k, nf = 0;
    const double am = 0.5 * (a0 + a1);
    for (k = 0; k < L->cnt && nf < INF_PTS; k++) {
        const int a = (L->r0 + k) % L->n, b = (a + 1) % L->n;
        const double s0 = L->arc[k], s1 = L->arc[k + 1], sl = (s1 - s0 > 1e-9) ? s1 - s0 : 1.0;
        if (am >= s0 && am <= s1) {
            const double t = (am - s0) / sl;
            *mx = L->ox[a] + (L->ox[b] - L->ox[a]) * t; *mz = L->oz[a] + (L->oz[b] - L->oz[a]) * t; *seg_mid = a;
        }
        if (s1 <= a0 || s0 >= a1) continue;
        if (nf == 0) {
            const double t = (a0 - s0) / sl;
            fx[nf] = L->ox[a] + (L->ox[b] - L->ox[a]) * t; fz[nf] = L->oz[a] + (L->oz[b] - L->oz[a]) * t;
            if (t <= 1e-9) { gx[nf] = fx[nf] + L->bx[a] * D * L->bm[a]; gz[nf] = fz[nf] + L->bz[a] * D * L->bm[a]; }
            else           { gx[nf] = fx[nf] + L->nx[a] * D;            gz[nf] = fz[nf] + L->nz[a] * D; }
            nf++;
        }
        if (s1 < a1 - 1e-6) {          /* a vertex inside the lot: its mitre */
            fx[nf] = L->ox[b]; fz[nf] = L->oz[b];
            gx[nf] = fx[nf] + L->bx[b] * D * L->bm[b]; gz[nf] = fz[nf] + L->bz[b] * D * L->bm[b];
            nf++;
        } else {
            const double t = (a1 - s0) / sl;
            fx[nf] = L->ox[a] + (L->ox[b] - L->ox[a]) * t; fz[nf] = L->oz[a] + (L->oz[b] - L->oz[a]) * t;
            if (t >= 1.0 - 1e-9) { gx[nf] = fx[nf] + L->bx[b] * D * L->bm[b]; gz[nf] = fz[nf] + L->bz[b] * D * L->bm[b]; }
            else                 { gx[nf] = fx[nf] + L->nx[a] * D;            gz[nf] = fz[nf] + L->nz[a] * D; }
            nf++;
            break;
        }
    }
    *nf_out = nf;
    return nf >= 2;
}

/* 0 free, 1 bent, 2 on the road, 3 a real footprint (tested 0.5 m inside the lot, a facade on the line is not one),
 * 4 [IC] a green / open polygon under any inset corner or centre of the lot */
static int inf_lot_test(PvEmit *e, const double *fx, const double *fz, const double *gx, const double *gz, int nf)
{
    TG_TileCtx *c = e->c;
    const double in = 0.5 * e->s->upm;
    int q, k;
    for (q = 0; q + 1 < nf; q++) {
        const double qx[4] = { fx[q], fx[q + 1], gx[q + 1], gx[q] }, qz[4] = { fz[q], fz[q + 1], gz[q + 1], gz[q] };
        const double fdx = fx[q + 1] - fx[q], fdz = fz[q + 1] - fz[q], gdx = gx[q + 1] - gx[q], gdz = gz[q + 1] - gz[q];
        double sx[4], sz[4], cx = 0.0, cz = 0.0;
        if (fdx * gdx + fdz * gdz <= 0.0) return 1;
        if (sc_on_road(c, qx, qz, 4, NULL)) return 2;
        for (k = 0; k < 4; k++) { cx += 0.25 * qx[k]; cz += 0.25 * qz[k]; }
        for (k = 0; k < 4; k++) {
            const double dx = cx - qx[k], dz = cz - qz[k], l = hypot(dx, dz);
            const double t = l > 2.0 * in ? in / l : 0.5;
            sx[k] = qx[k] + dx * t; sz[k] = qz[k] + dz * t;
        }
        if (inf_quad_in_building(c, e->bld, sx, sz)) return 3;
        for (k = 0; k < 5 && e->s->ic_infill; k++) {
            const double X = (k < 4 ? sx[k] : cx) + (double)c->org_x, Z = (k < 4 ? sz[k] : cz) + (double)c->org_z;
            if (sc_lot_class_at(c->lotp, c->nlotp, X, Z) != SC_LC_BUILD) return 4;
        }
    }
    return 0;
}

/* One lot [a0, a1] of the current run: accept it, else split it along its front (twice at most), else halve its
 * depth, else count why it was refused. Counted once, by the tile owning its front midpoint. */
static int inf_record(TG_TileCtx *c, const double *rx, const double *rz, int n)
{
    InfRec *r;
    int k;
    if (!sc_grow((void **)&c->infr, &c->cap_infr, c->ninfr + 1, sizeof(InfRec))) return 0;
    r = &c->infr[c->ninfr++];
    r->n = n;
    r->x0 = r->x1 = rx[0]; r->z0 = r->z1 = rz[0];
    for (k = 0; k < n; k++) {
        r->x[k] = rx[k]; r->z[k] = rz[k];
        if (rx[k] < r->x0) r->x0 = rx[k];
        if (rx[k] > r->x1) r->x1 = rx[k];
        if (rz[k] < r->z0) r->z0 = rz[k];
        if (rz[k] > r->z1) r->z1 = rz[k];
    }
    return 1;
}

/* an accepted infill lot (this tile's or a neighbour's) holds the tile-local point: the pavement puts no fence there */
static int inf_in_lot(const TG_TileCtx *c, double x, double z)
{
    int i;
    for (i = 0; i < c->ninfr; i++) {
        const InfRec *r = &c->infr[i];
        if (x < r->x0 || x > r->x1 || z < r->z0 || z > r->z1) continue;
        if (td5_geob_point_in_ring(r->x, r->z, r->n, x, z)) return 1;
    }
    return 0;
}

/* the lot [a0, a1] with its front INF_FRONT_IN inside the building line and its back D behind that */
static int inf_lot_ring(const InfLine *L, double a0, double a1, double D, double *fx, double *fz, double *gx,
                        double *gz, int *nf, double *mx, double *mz, int *seg)
{
    double tx[INF_PTS + 2], tz[INF_PTS + 2];
    int n2 = 0;
    if (!inf_lot_geom(L, a0, a1, INF_FRONT_IN, tx, tz, fx, fz, nf, mx, mz, seg)) return 0;
    if (!inf_lot_geom(L, a0, a1, INF_FRONT_IN + D, tx, tz, gx, gz, &n2, mx, mz, seg) || n2 != *nf) return 0;
    return 1;
}

/* One lot [a0, a1] of the current run: accept it, else split it along its front (twice at most), else halve its
 * depth, else refuse it. Every tile that sees the block decides every lot the same way and records the accepted
 * ones (the fence test); only the tile owning the front midpoint counts and emits it. Returns 0 on out of memory. */
static int inf_lot(PvEmit *e, const InfLine *L, double a0, double a1, int split, int counted)
{
    TG_TileCtx *c = e->c;
    TD5_RoamScnSource *s = e->s;
    const double U = s->upm;
    double fx[INF_PTS + 2], fz[INF_PTS + 2], gx[INF_PTS + 2], gz[INF_PTS + 2];
    double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], mx = 0.0, mz = 0.0, D, Dcap;
    int nf = 0, seg = L->r0, q, res = 0, own, why = 0;
    uint32_t h;
    if (!inf_lot_geom(L, a0, a1, 0.0, fx, fz, gx, gz, &nf, &mx, &mz, &seg)) return 1;
    own = sc_owned_point(c, mx, mz) && !counted;
    if (own) c->st.inf_lots++;
    h = inf_hash(mx + (double)c->org_x, mz + (double)c->org_z);
    D = s->ic_infill ? (INF_DEPTH_MIN + (double)(h % (uint32_t)INF_DEPTH_SPAN)) * U
                     : (10.0 + (double)(h % 9u)) * U;                 /* round M: 10-18 m */
    Dcap = INF_HALF * inf_ray(L->ox, L->oz, L->n, seg, mx, mz, L->nx[seg], L->nz[seg]) - INF_FRONT_IN;
    if (D > Dcap) D = Dcap;
    if (D < INF_SHALLOW_M * U) { why = 1; goto refused; }
    {   /* CITY ground; no green / open area / ring plaza, no landmark */
        int city = sc_casco_has(s, c->tx, c->tz);
        const double X = mx + L->nx[seg] * 0.5 * D + (double)c->org_x, Z = mz + L->nz[seg] * 0.5 * D + (double)c->org_z;
        if (!city && s->lot_cover) {
            int k, bi = 0;
            for (k = 0; k < 4; k++) {
                const double t = (k + 0.5) / 4.0;
                bi += sc_cover_at(c, mx + L->nx[seg] * D * t, mz + L->nz[seg] * D * t) == TD5_GEO_COVER_BUILT;
            }
            city = bi >= 3;
        }
        if (!city || sc_lot_class_at(c->lotp, c->nlotp, X, Z) != SC_LC_BUILD ||
            sc_lot_class_at(c->lotp, c->nlotp, mx + (double)c->org_x, mz + (double)c->org_z) != SC_LC_BUILD ||
            inf_near_landmark(e->bld, X, Z, INF_LM_CLEAR_M * U)) { why = 4; goto refused; }
    }
    if (!inf_lot_ring(L, a0, a1, D, fx, fz, gx, gz, &nf, &mx, &mz, &seg)) { why = 1; goto refused; }
    res = inf_lot_test(e, fx, fz, gx, gz, nf);
    if (res == 3 && split < 2 && a1 - a0 >= 2.0 * INF_SPLIT_MIN_M * U) {
        /* a footprint on part of it: the two halves on their own (booked as this lot) */
        const int b0 = c->st.inf_emitted;
        const double m0 = c->st.inf_m_built;
        const double am = 0.5 * (a0 + a1);
        if (!inf_lot(e, L, a0, am, split + 1, 1) || !inf_lot(e, L, am, a1, split + 1, 1)) return 0;
        if (own && c->st.inf_emitted == b0) c->st.inf_drop_real++;
        if (own) c->st.inf_m_real += (a1 - a0) / U - (c->st.inf_m_built - m0);
        return 1;
    }
    if (!s->ic_infill) {
        if (res == 3 && 0.5 * D >= INF_SHALLOW_M * U) {          /* round M: its back meets one: the front half */
            D *= 0.5;
            if (!inf_lot_ring(L, a0, a1, D, fx, fz, gx, gz, &nf, &mx, &mz, &seg)) { why = 1; goto refused; }
            res = inf_lot_test(e, fx, fz, gx, gz, nf);
        }
    } else {
        /* [IC] its back meets a footprint or an area: shallower, step by step (the deeper 12-24 m lots must not
         * cost frontage the 10-18 m ones built) */
        static const double k_back[3] = { 0.7, 0.5, 0.35 };
        const double D0 = D;
        int q2;
        for (q2 = 0; q2 < 3 && (res == 3 || res == 4) && k_back[q2] * D0 >= INF_SHALLOW_M * U; q2++) {
            D = k_back[q2] * D0;
            if (!inf_lot_ring(L, a0, a1, D, fx, fz, gx, gz, &nf, &mx, &mz, &seg)) { why = 1; goto refused; }
            res = inf_lot_test(e, fx, fz, gx, gz, nf);
        }
    }
    if (res) { why = res; goto refused; }
    if (nf * 2 > TD5_GEOB_RING_MAX) { why = 1; goto refused; }
    for (q = 0; q < nf; q++) { rx[q] = fx[q]; rz[q] = fz[q]; }
    for (q = 0; q < nf; q++) { rx[nf + q] = gx[nf - 1 - q]; rz[nf + q] = gz[nf - 1 - q]; }
    if (!td5_geob_ring_simple(rx, rz, nf * 2)) { why = 1; goto refused; }
    if (!inf_record(c, rx, rz, nf * 2)) return 0;
    if (!sc_owned_point(c, mx, mz)) return 1;          /* a split half: its own midpoint's tile emits it */
    if (s->infill_log)                                  /* TD5RE_ROAM_INFILL_LOG=1: where each lot stands (place frame) */
        TD5_LOG_I(LOG_TAG, "ROAM_INFILL tile %d,%d front (%.0f,%.0f) normal (%.3f,%.3f) depth %.1f m width %.1f m",
                  c->tx, c->tz, mx + (double)c->org_x, mz + (double)c->org_z, L->nx[seg], L->nz[seg], D / U, (a1 - a0) / U);
    return inf_emit(e, rx, rz, nf * 2, h, fabs(td5_geob_ring_area(rx, rz, nf * 2)), a1 - a0);
refused:
    if (own) {
        if (why == 2) c->st.inf_drop_road++;
        else if (why == 3) c->st.inf_drop_real++;
        else if (why == 4) c->st.inf_drop_area++;
        else c->st.inf_drop_shape++;
        if (why == 3) c->st.inf_m_real += (a1 - a0) / U;
        else if (why == 4) c->st.inf_m_area += (a1 - a0) / U;
        else c->st.inf_m_other += (a1 - a0) / U;
    }
    return 1;
}

static int pv_infill(PvEmit *e, const PvChain *ch)
{
    TG_TileCtx *c = e->c;
    TD5_RoamScnSource *s = e->s;
    const double U = s->upm, LW = INF_LOT_M * U;
    InfLine L;
    double *blk = NULL, area;
    unsigned char *fr = NULL, *brk = NULL;
    int n = 0, v, i, ok = 0, first = -1;
    if (!s->infill_on || !ch->closed || ch->nv < 3) return 1;
    memset(&L, 0, sizeof L);
    blk = (double *)malloc((size_t)(ch->nv + 1) * 8u * sizeof(double));
    fr = (unsigned char *)calloc((size_t)ch->nv * 2u, 1);
    if (!blk || !fr) goto out;
    L.ox = blk; L.oz = L.ox + ch->nv; L.nx = L.oz + ch->nv; L.nz = L.nx + ch->nv;
    L.bx = L.nz + ch->nv; L.bz = L.bx + ch->nv; L.bm = L.bz + ch->nv; L.arc = L.bm + ch->nv;
    brk = fr + ch->nv;
    /* the building line, collapsed corner runs as one point (brk: not a street front there, for now) */
    for (v = 0; v < ch->nv; v++) {
        const int bad = (ch->vf[v] & (PV_VF_ISLAND | PV_VF_DECK | PV_VF_FLAT | PV_VF_MEDIAN)) || !(ch->w[v] > 0.0);
        if (n > 0 && hypot(ch->ox[v] - L.ox[n - 1], ch->oz[v] - L.oz[n - 1]) < 20.0) { if (bad) brk[n - 1] = 1; continue; }
        L.ox[n] = ch->ox[v]; L.oz[n] = ch->oz[v]; brk[n] = (unsigned char)bad; n++;
    }
    if (n > 1 && hypot(L.ox[n - 1] - L.ox[0], L.oz[n - 1] - L.oz[0]) < 20.0) { brk[0] |= brk[n - 1]; n--; }
    L.n = n;
    if (n < 3) { ok = 1; goto out; }
    area = td5_geob_ring_area(L.ox, L.oz, n);
    /* a block lies on the RIGHT of the chain (the road on its left): negative area, and big enough */
    if (!(area < -INF_BLOCK_M2 * U * U)) { ok = 1; goto out; }
    for (i = 0; i < n; i++) {          /* fr[i]: segment i -> i+1 is a street front (both ends) */
        const int j = (i + 1) % n;
        const double dx = L.ox[j] - L.ox[i], dz = L.oz[j] - L.oz[i], l = hypot(dx, dz);
        L.nx[i] = l > 1e-9 ? dz / l : 0.0; L.nz[i] = l > 1e-9 ? -dx / l : 0.0;      /* right of travel: into the block */
        fr[i] = (unsigned char)!(brk[i] || brk[j]);
    }
    for (i = 0; i < n; i++) {
        const int p = (i + n - 1) % n;
        double ux = L.nx[p] + L.nx[i], uz = L.nz[p] + L.nz[i], l = hypot(ux, uz), cs, turn;
        if (l > 1e-9) { ux /= l; uz /= l; } else { ux = L.nx[i]; uz = L.nz[i]; }
        cs = ux * L.nx[i] + uz * L.nz[i];
        L.bx[i] = ux; L.bz[i] = uz;
        L.bm[i] = 1.0 / (cs > 1.0 / INF_MITRE_MAX ? cs : 1.0 / INF_MITRE_MAX);
        turn = fabs(atan2(L.nx[p] * L.nz[i] - L.nz[p] * L.nx[i], L.nx[p] * L.nx[i] + L.nz[p] * L.nz[i])) * 57.29577951308232;
        brk[i] = (unsigned char)(turn > INF_BREAK_DEG || fr[p] != fr[i]);
        if (brk[i] && first < 0) first = i;
    }
    if (first < 0) {      /* a round loop: break at its least world point */
        double bk = 1e300;
        for (i = 0; i < n; i++) {
            const double k = (L.ox[i] + (double)c->org_x) * 1e-3 + (L.oz[i] + (double)c->org_z);
            if (k < bk) { bk = k; first = i; }
        }
        brk[first] = 1;
    }
    /* every run from a break to the next, cut at the world lattice lines */
    for (v = 0; v < n; v++) {
        const int r0 = (first + v) % n;
        int r1, cnt, k, ncut = 0, axis, ci;
        double Lr = 0.0, cut[256], u0, v0, u1, v1;
        if (!brk[r0] || !fr[r0]) continue;
        for (r1 = (r0 + 1) % n, cnt = 1; !brk[r1] && cnt < n; r1 = (r1 + 1) % n, cnt++) { }
        L.r0 = r0; L.cnt = cnt;
        L.arc[0] = 0.0;
        for (k = 0; k < cnt; k++) {
            const int a = (r0 + k) % n, b = (a + 1) % n;
            Lr += hypot(L.ox[b] - L.ox[a], L.oz[b] - L.oz[a]);
            L.arc[k + 1] = Lr;
        }
        if (Lr < INF_MIN_M * U) continue;
        roam_grid_uv(c->grid, L.ox[r0] + (double)c->org_x, L.oz[r0] + (double)c->org_z, &u0, &v0);
        roam_grid_uv(c->grid, L.ox[r1] + (double)c->org_x, L.oz[r1] + (double)c->org_z, &u1, &v1);
        axis = fabs(u1 - u0) >= fabs(v1 - v0) ? 0 : 1;
        cut[ncut++] = 0.0;
        for (k = 0; k < cnt && ncut < 250; k++) {
            const int a = (r0 + k) % n, b = (a + 1) % n;
            double ua, va, ub, vb, ca, cb, lo, hi, g;
            roam_grid_uv(c->grid, L.ox[a] + (double)c->org_x, L.oz[a] + (double)c->org_z, &ua, &va);
            roam_grid_uv(c->grid, L.ox[b] + (double)c->org_x, L.oz[b] + (double)c->org_z, &ub, &vb);
            ca = axis ? va : ua; cb = axis ? vb : ub;
            lo = ca < cb ? ca : cb; hi = ca < cb ? cb : ca;
            for (g = ceil(lo / LW) * LW; g < hi && ncut < 250; g += LW) {
                const double at = L.arc[k] + (g - ca) / (cb - ca) * (L.arc[k + 1] - L.arc[k]);
                if (at - cut[ncut - 1] >= INF_MIN_M * U && Lr - at >= INF_MIN_M * U) cut[ncut++] = at;
            }
        }
        cut[ncut++] = Lr;
        for (ci = 0; ci + 1 < ncut; ci++)
            if (!inf_lot(e, &L, cut[ci], cut[ci + 1], 0, 0)) goto out;
    }
    ok = 1;
out:
    free(blk); free(fr);
    return ok;
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
                if (m >= 2 && m <= 4) {
                    /* [F4b] a face meeting ONLY a dead-end quad a landmark was let stand on is booked apart */
                    int strict;
                    c->skip_lm = 1;
                    strict = sc_on_road(c, fx, fz, m, NULL);
                    c->skip_lm = 0;
                    if (!strict && sc_on_road(c, fx, fz, m, NULL)) c->st.guard_lm++;
                    if (!strict) m = 0;
                }
                if (m >= 2 && m <= 4) {
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
            if (m >= c->lot_first[k]) rec_furn[r] = 1;  /* [HD] a city lot is the ground itself (SECTION lots) */
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

/* [G4] a ROAD tile of the 3x3 for the render's ground clamp: the same LRU / file / builder as the
 * tile ctx, so a seam is judged from the same tiles on both sides. Valid until SC_ROAD_LRU more asks. */
const RoamTile *td5_roam_scn_road(TD5_RoamScnSource *s, int tx, int tz) { return s ? sc_road(s, tx, tz) : NULL; }


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
    td5_geo_source_path(s->cover_path, sizeof s->cover_path, place, "COVER.R8");
    /* [HD] city lots + infill: the casco's tiles, sorted for sc_casco_has */
    s->lots_on   = td5_env_int("TD5RE_ROAM_LOTS", 1, 0, 1);
    s->lot_cover = td5_env_int("TD5RE_ROAM_LOT_COVER", 1, 0, 1);
    s->infill_on = td5_env_int("TD5RE_ROAM_INFILL", 1, 0, 1);
    s->infill_log = td5_env_int("TD5RE_ROAM_INFILL_LOG", 0, 0, 1);
    s->bld_log = td5_env_int("TD5RE_ROAM_BLD_LOG", 0, 0, 1);
    s->cover_green = td5_env_int("TD5RE_ROAM_COVER_GREEN", 1, 0, 1);
    s->ic_despike = td5_env_int("TD5RE_ROAM_AREA_DESPIKE", 1, 0, 1);
    s->ic_drops = td5_env_int("TD5RE_ROAM_BLD_DROPS", 1, 0, 1);
    s->ic_infill = td5_env_int("TD5RE_ROAM_IC_INFILL", 1, 0, 1);
    s->lot_page  = td5_env_int("TD5RE_ROAM_LOT_PAGE", TD5_TG_PAGE_R4_PIER, 0, 1023);   /* light cast concrete */
    {
        int (*cl)[2] = (int (*)[2])malloc(2048 * sizeof(int[2]));
        const int n = cl ? td5_roam_casco_tiles(g, cl, 2048) : 0;
        int i, k;
        s->casco = n > 0 ? (int64_t *)malloc((size_t)n * sizeof(int64_t)) : NULL;
        for (i = 0; s->casco && i < n; i++) {
            const int64_t key = sc_tkey(cl[i][0], cl[i][1]);
            for (k = s->ncasco - 1; k >= 0 && s->casco[k] > key; k--) s->casco[k + 1] = s->casco[k];
            s->casco[k + 1] = key;
            s->ncasco++;
        }
        free(cl);
    }
    td5_geo_source_path(s->height_path, sizeof s->height_path, place, "HEIGHT.R16");
    td5_geo_source_path(s->water_path, sizeof s->water_path, place, "WATER.R8");
    s->exag = (double)td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400) / 100.0;
    s->lm_wins = td5_env_int("TD5RE_ROAM_LM_WINS", 1, 0, 1);
    s->pv = sc_pv_open(s);
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
    sc_pv_close(s->pv);
    free(s->casco);
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
    if (hc && td5_roam_gfix_enabled() && td5_env_flag_on("TD5RE_ROAM_SCN_LOWER")) td5_roam_gfix_build(&c->gf, &s->grid, (const RoamTile *const *)c->road, self->tx, self->tz, hc);
    c->ground_last = 0.0;
    if (hc) {
        double h;
        if (td5_roam_height_sample(hc, 0.5 * (x0 + x1), 0.5 * (z0 + z1), &h, NULL)) c->ground_last = h;
    }
    if (gr_raster_read_window_game(s->canopy_path, x0 - halo, z0 - halo, x1 + halo, z1 + halo, 2, &c->canopy))
        c->canopy_ok = c->canopy.kind == 2 && c->canopy.data != NULL;
    c->cover_ok = 0;                          /* [HD] the lots' built-up test: the tile box only */
    if (gr_raster_read_window_game(s->cover_path, x0, z0, x1, z1, 2, &c->cover))
        c->cover_ok = c->cover.kind == 2 && c->cover.data != NULL;
    c->cover3_ok = 0;                         /* [IC] the 3x3 window, a separate read: the lots keep theirs */
    if (s->cover_green && gr_raster_read_window_game(s->cover_path, x0 - (double)s->grid.tile, z0 - (double)s->grid.tile,
                                                     x1 + (double)s->grid.tile, z1 + (double)s->grid.tile, 2, &c->cover3))
        c->cover3_ok = c->cover3.kind == 2 && c->cover3.data != NULL;
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
    /* [HD] the AREAS polygons the lots and the infill ask (every one that reaches the 3x3, not only the owned) */
    if (!sc_lot_polys(ac, nac, x0 - (double)s->grid.tile, z0 - (double)s->grid.tile, x1 + (double)s->grid.tile,
                      z1 + (double)s->grid.tile, &c->lotp, &c->nlotp, 0)) goto out;
    if (s->cover_green && !sc_lot_polys(ac, nac, x0 - (double)s->grid.tile, z0 - (double)s->grid.tile,
                                        x1 + (double)s->grid.tile, z1 + (double)s->grid.tile, &c->anyp, &c->nanyp, 1)) goto out;
    {   /* [IC] the kerb loops first: ring plazas + avenue medians join c->lotp before any footprint is judged */
        const uint32_t tp = td5_plat_time_ms();
        if (!sc_pv_prepare(s, c, bc, nbc)) goto out;
        c->st.ms_pv = td5_plat_time_ms() - tp;
    }
    t1 = td5_plat_time_ms();
    c->st.ms_src = t1 - t0;
    if (td5_env_int("TD5RE_ROAM_SCN_PLAZAS", 1, 0, 1) && !sc_emit_plazas(c, ar, nar)) goto out;
    if (td5_env_int("TD5RE_ROAM_SCN_BUILDINGS", 1, 0, 1) && !sc_emit_buildings(s, c, bc, nbc, ar, nar)) goto out;
    if (!sc_emit_pavement(s, c, bc, nbc, ar, nar)) goto out;            /* [F4b] */
    /* [G5] after the pavement: a footpath crossing a slab or an island stands on it (sc_pv_y) */
    if (td5_env_int("TD5RE_ROAM_SCN_FOOTPATHS", 1, 0, 1) &&
        !sc_emit_footpaths(s, c, fc, nfc, x0 - (double)c->org_x, z0 - (double)c->org_z,
                           x1 - (double)c->org_x, z1 - (double)c->org_z)) goto out;
    /* [HD] the city's open ground paved (census runs with the knob off too); meshes from lot_first on are lots */
    {
        int k;
        for (k = 0; k < TD5_ROAM_SCN_NSB; k++) c->lot_first[k] = c->sb[k].nmesh;
    }
    if (!sc_emit_lots(s, c, self, ac, nac)) goto out;
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
    if (c->cover.data) { free(c->cover.data); c->cover.data = NULL; }
    c->cover_ok = 0;
    if (c->cover3.data) { free(c->cover3.data); c->cover3.data = NULL; }
    c->cover3_ok = 0;
    free(c->anyp); c->anyp = NULL; c->nanyp = 0;
    free(c->lotp); c->lotp = NULL; c->nlotp = 0;
    free(c->ringpool); c->ringpool = NULL;
    free(c->pv_B); c->pv_B = NULL; c->pv_nb = 0;
    free(c->pv_ways); c->pv_ways = NULL; c->pv_nways = 0; c->pv_ready = 0;
    c->ninfr = 0;                             /* infr itself is kept for the next tile (sc_ctx_free) */
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
    long tot[24], pv[16], pv_src[7], lm_stub = 0, guard_lm = 0;
    uint32_t pv_ms_max = 0;
    long ft[16], lot_t[12] = { 0 }, ic_t[4] = { 0, 0, 0, 0 };
    double shift_max = 0.0, lot_m2[4] = { 0.0, 0.0, 0.0, 0.0 }, ic_m[4] = { 0.0, 0.0, 0.0, 0.0 };
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
    memset(pv, 0, sizeof pv);
    memset(pv_src, 0, sizeof pv_src);
    memset(ft, 0, sizeof ft);
    if (!cens[0]) {
        const char *d = td5_plat_log_dir();
        snprintf(cens, sizeof cens, "%sroam_scenery_census.txt", d ? d : "");
    }
    f = fopen(cens, "w");
    if (f) fprintf(f, "roam scenery census (F4a), place %s, %d tile(s), order %s, thread %d, rules %u\n"
                      "tx tz ok hash meshes bytes ms ms_src ms_emit road_tiles bld_seen bld_emitted bld_prefab apron "
                      "drop_road drop_shift drop_ring veto_plaza veto_lm nudged shift_max_m fp_ways fp_pieces fp_cut "
                      "fp_merged areas lawn_tris lawn_cut trees trees_cut guard_overlaps "
                      "pv_bedges pv_interior pv_chains pv_pieces pv_corner pv_collapsed pv_island pv_median pv_free_clamp "
                      "pv_fence pv_rail pv_kerb pv_cut pv_drop pv_kerb_cut pv_noway ms_pv furn_meshes sig_clusters sig_arms "
                      "sig_inbound mast_near post_left mast_far far_shared plates plate_one_face plate_none sig_drop_frame "
                      "sig_drop_road sig_drop_dup lamps lamp_glow lamp_drop_road lamp_skip_class stops shelters benches bins "
                      "stop_drop_far stop_drop_road zebras limit_lines xing_unknown xing_drop_far furn_guard "
                      "lot_cells lot_green lot_rural lot_skip lot_under_road lot_rings lot_paved_m2 lot_lawn_m2 lot_green_m2 "
                      "inf_lots inf_emitted inf_drop_real inf_drop_road inf_drop_area inf_drop_shape inf_m2 "
                      "lot_medians bld_veto_green pv_bld_gone inf_m_built inf_m_real inf_m_area inf_m_other\n",
                   j.place, j.n, j.reverse ? "reverse" : "forward", threadmode, TD5_ROAM_SCENERY_RULES);
    for (i = 0; i < j.n; i++) {
        const TD5_RoamScnStats *r = &j.rows[i];
        if (f) fprintf(f, "%d %d %d %08x %d %u %u %u %u %d %d %d %d %d %d %d %d %d %d %d %.2f %d %d %d %d %d %d %d %d %d %d "
                          "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %u "
                          "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d "
                          "%d %d %d %d %d %d %.0f %.0f %.0f %d %d %d %d %d %d %.0f %d %d %d %.0f %.0f %.0f %.0f\n",
                       j.list[i][0], j.list[i][1], j.ok[i], r->hash, r->meshes, r->bytes, r->ms, r->ms_src, r->ms_emit,
                       r->road_tiles, r->bld_seen, r->bld_emitted, r->bld_prefab, r->bld_apron, r->bld_drop_road,
                       r->bld_drop_shift, r->bld_drop_ring, r->bld_veto_plaza, r->bld_veto_lm, r->bld_nudged,
                       r->bld_shift_max / SC_UPM, r->fp_ways, r->fp_pieces, r->fp_cut_road, r->fp_merged, r->areas,
                       r->lawn_tris, r->lawn_cut, r->trees, r->trees_cut, r->guard_overlaps,
                       r->pv_bedges, r->pv_interior, r->pv_chains, r->pv_pieces, r->pv_corner, r->pv_collapsed,
                       r->pv_island, r->pv_median, r->pv_free_clamp, r->pv_fence, r->pv_rail, r->pv_kerb, r->pv_cut,
                       r->pv_drop, r->pv_kerb_cut, r->pv_noway, (unsigned)r->ms_pv,
                       r->furn.meshes, r->furn.sig_clusters, r->furn.sig_arms, r->furn.sig_inbound, r->furn.mast_near,
                       r->furn.post_left, r->furn.mast_far, r->furn.far_shared, r->furn.plates, r->furn.plate_one_face,
                       r->furn.plate_none, r->furn.sig_drop_frame, r->furn.sig_drop_road, r->furn.sig_drop_dup, r->furn.lamps,
                       r->furn.lamp_glow, r->furn.lamp_drop_road, r->furn.lamp_skip_class, r->furn.stops, r->furn.shelters,
                       r->furn.benches, r->furn.bins, r->furn.stop_drop_far, r->furn.stop_drop_road, r->furn.zebras,
                       r->furn.limit_lines, r->furn.xing_unknown, r->furn.xing_drop_far, r->furn.guard_hits,
                       r->lot_cells, r->lot_green, r->lot_rural, r->lot_skip, r->lot_under_road, r->lot_rings,
                       r->lot_paved_m2, r->lot_lawn_m2, r->lot_green_m2, r->inf_lots, r->inf_emitted, r->inf_drop_real,
                       r->inf_drop_road, r->inf_drop_area, r->inf_drop_shape, r->inf_m2, r->lot_medians, r->bld_veto_green,
                       r->pv_bld_gone, r->inf_m_built, r->inf_m_real, r->inf_m_area, r->inf_m_other);
        ic_t[0] += r->lot_medians; ic_t[1] += r->bld_veto_green; ic_t[2] += r->pv_bld_gone; ic_t[3] += r->lot_cover_green;
        ic_m[0] += r->inf_m_built; ic_m[1] += r->inf_m_real; ic_m[2] += r->inf_m_area; ic_m[3] += r->inf_m_other;
        lot_t[0] += r->lot_cells; lot_t[1] += r->lot_green; lot_t[2] += r->lot_rural; lot_t[3] += r->lot_under_road; lot_t[4] += r->lot_rings;
        lot_t[6] += r->inf_lots; lot_t[7] += r->inf_emitted;
        lot_t[8] += r->inf_drop_real; lot_t[9] += r->inf_drop_road; lot_t[10] += r->inf_drop_area;
        lot_t[11] += r->inf_drop_shape;
        lot_m2[0] += r->lot_paved_m2; lot_m2[1] += r->lot_lawn_m2; lot_m2[2] += r->lot_green_m2; lot_m2[3] += r->inf_m2;
        {
            int q;
            for (q = 0; q < 7; q++) pv_src[q] += r->pv_src[q];
            pv[0] += r->pv_bedges; pv[1] += r->pv_interior; pv[2] += r->pv_pieces; pv[3] += r->pv_corner;
            pv[4] += r->pv_collapsed; pv[5] += r->pv_island; pv[6] += r->pv_median; pv[7] += r->pv_free_clamp;
            pv[8] += r->pv_fence; pv[9] += r->pv_rail; pv[10] += r->pv_kerb; pv[11] += r->pv_cut;
            pv[12] += r->pv_drop; pv[13] += r->pv_kerb_cut; pv[14] += r->pv_noway; pv[15] += (long)r->ms_pv;
            if (r->ms_pv > pv_ms_max) pv_ms_max = r->ms_pv;
            lm_stub += r->bld_lm_stub; guard_lm += r->guard_lm;
        }
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
                 "roam pavement: %ld kerb piece(s) owned (%ld inside another road, skipped), %ld piece(s) emitted "
                 "(%ld on junction outlines, %ld corner vertices merged), %ld island + %ld median piece(s), %ld vertices "
                 "clamped to half a gap, %ld kerb faces (%ld dropped), %ld fences + %ld deck railings, guard cut %ld "
                 "dropped %ld, %ld kerbs without a cache way | widths by source: tag %ld footway %ld facade %ld "
                 "frontage %ld class %ld kerb %ld none %ld | pavement ms mean %.1f max %u | landmarks over a dead-end stub "
                 "%ld quad(s), %ld face(s) booked on them (not guard overlaps)",
                 pv[0], pv[1], pv[2], pv[3], pv[4], pv[5], pv[6], pv[7], pv[10], pv[13], pv[8], pv[9], pv[11], pv[12],
                 pv[14], pv_src[TD5_GEO_SWSRC_TAG], pv_src[TD5_GEO_SWSRC_FOOTWAY], pv_src[TD5_GEO_SWSRC_FACADE],
                 pv_src[TD5_GEO_SWSRC_FRONTAGE], pv_src[TD5_GEO_SWSRC_CLASS], pv_src[TD5_GEO_SWSRC_KERB],
                 pv_src[TD5_GEO_SWSRC_NONE], j.n ? (double)pv[15] / j.n : 0.0, (unsigned)pv_ms_max, lm_stub, guard_lm);
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
        snprintf(line, sizeof line,
                 "roam lots: %ld cell(s) paved (one quad each, %ld of them reach under the road), "
                 "%ld kept green under an OSM green or a ring plaza (%ld rings), %ld left green outside the city | non-road ground: paved %.0f m2, "
                 "LAWN WITHOUT AN OSM GREEN %.0f m2, OSM green %.0f m2 | infill: %ld frontage lot(s), %ld built (%.0f m2), "
                 "refused: real building %ld, road / other lot %ld, area %ld, shape %ld",
                 lot_t[0], lot_t[3], lot_t[1], lot_t[4], lot_t[2], lot_m2[0], lot_m2[1], lot_m2[2],
                 lot_t[6], lot_t[7], lot_m2[3], lot_t[8], lot_t[9], lot_t[10], lot_t[11]);
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam IC: %ld avenue median(s) + %ld unmapped vegetated casco loop(s) kept green, %ld footprint(s) not built on a park / ring plaza / median "
                 "(green veto), %ld dropped footprint(s) no longer taking the frontage | infill frontage: built %.0f m, "
                 "taken by a real footprint %.0f m, refused by an area %.0f m, other (road / shape) %.0f m",
                 ic_t[0], ic_t[3], ic_t[1], ic_t[2], ic_m[0], ic_m[1], ic_m[2], ic_m[3]);
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        if (f) fclose(f);
    }
    free(j.list); free(j.rows); free(j.ok);
    return 1;
}

#endif /* !TD5RE_RELEASE */
