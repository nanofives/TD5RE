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
#include "td5_roam_muni.h"
#include "td5_roam_cut.h"
#include "td5_roam_greenhouse.h"   /* [roam PC] */

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
    unsigned char *pool;    /* [roam LE] AREAS: 1 = kind swimming_pool                      */
    unsigned char *kf;      /* [roam PC] SC_KF_*: BUILDINGS class greenhouse; AREAS greenhouse_horticulture / wetland */
    unsigned char *spt, *srf;   /* [roam PB] AREAS: TD5_RPS_* sport (TD5_RPS_STADIUM = leisure=stadium) + TD5_RPF_* surface */
    int64_t  *own;          /* owner tile key per record (b, a)          */
    int      *order;        /* record indices sorted by (own, id, cx, cz) */
    double   *px, *pz;      /* ring / way points                          */
    int       npt;
    double    storey_m;     /* BUILDINGS storey_height_m of this file     */
    size_t    bytes;
} ScCache;

/* [roam JC2] the courtyards of one S3D_RELATIONS.JSON outline: inner rings in the cache's point pool
 * (place frame, CW), and whether its height is an estimate the emitter may replace */
typedef struct ScRelX {
    int nh, hfirst[TG_S3D_HOLES_MAX], hn[TG_S3D_HOLES_MAX];
    int h_est;
    double hole_m2;
} ScRelX;

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
    int       pools_on;                       /* [roam LE] TD5RE_ROAM_POOLS: swimming pools (coping + water) */
    int       stadium_on, pitch_on;           /* [roam PB] TD5RE_ROAM_STADIUM / TD5RE_ROAM_PITCH_LINES (td5_roam_stadium.c) */
    int       infill_log;                     /* [HD] TD5RE_ROAM_INFILL_LOG: one log line per built lot */
    int       bld_log;                        /* [IC] TD5RE_ROAM_BLD_LOG: one log line per footprint (its fate) */
    int       cover_green;                    /* [IC] TD5RE_ROAM_COVER_GREEN: unmapped vegetated casco loops are green */
    int       inf_rail;                       /* [integ-roamI] TD5RE_ROAM_INF_RAIL_VETO: no infill on a railway */
    int       canopy_green;                      /* [roam LA] TD5RE_ROAM_CANOPY_GREEN (sc_canopy_green) */
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
    int       s3d;            /* [JC] TD5RE_ROAM_S3D: Simple 3D Buildings (parts, roofs, outlines hidden) */
    ScCache   rel;            /* [JC2] S3D_RELATIONS.JSON outlines (a pinned extra BUILDINGS cache)   */
    ScRelX   *relx;           /*   ... their courtyards, per record                                   */
    ScCache   prel;           /* [LD] PARK_RELATIONS.JSON greens / precincts (a pinned extra AREAS cache) */
    int       park_rel;       /* [LD] TD5RE_ROAM_PARK_REL                                             */
    struct TD5_RoamPlazaSource *plaza;   /* [LD] plaza furniture + monuments (td5_roam_plaza.c); NULL = none */
    struct ScPvSrc *pv;       /* [F4b] the pavement's ROADS facts (SECTION pavement) */
    /* reused per build */
    TG_TileCtx *ctx;
    TD5_RoamFurnSource *furn;   /* [F4c] street furniture (td5_roam_furniture.c), this thread's */
    TD5_RoamOutSource *outsk;   /* [roam LH] outskirts (td5_roam_outskirts.c), this thread's; NULL = none */
    TD5_RoamRailSource *rails;  /* [roam ID] railways (td5_roam_rails.c), this thread's; NULL = none */
    TD5_RoamShopSource *shops;  /* [roam LB] shopfronts (td5_roam_shops.c); NULL = knob off */
    /* [roam NA] the municipal line (SECTION municipal line): TD5RE_ROAM_MUNI_LINE / _MUNI_CENSUS / _LM_GROUNDS,
     * the snap band (TD5RE_ROAM_MUNI_MAX_CM, _MUNI_PROTRUDE_CM) and the ochava leg (TD5RE_ROAM_OCHAVA_CM), units */
    int       muni_on, muni_census, grounds_on, ring_log;
    int       gh_on, wet_on;  /* [roam PC] TD5RE_ROAM_GREENHOUSE; TD5RE_ROAM_OUT_COVER_WET (marsh paint + reeds) */
    int       shore_on;       /* [roam PC] TD5RE_ROAM_SHORE: no city lot on a shore cell (the render draws the bank) */
    int       muni_cut;           /* [roam PE] TD5RE_ROAM_MUNI_CUT: the line keeps off NC's cutting bands */
    double    muni_max, muni_protrude, ochava;
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
    free(c->b); free(c->a); free(c->f); free(c->id); free(c->own); free(c->order); free(c->lot); free(c->pool);
    free(c->kf);
    free(c->spt); free(c->srf);
    free(c->px); free(c->pz);
    memset(c, 0, sizeof *c);
}

/* [roam PC] per-record kind flags (ScCache.kf) */
#define SC_KF_GH    1u      /* BUILDINGS: class greenhouse; AREAS: landuse=greenhouse_horticulture */
#define SC_KF_WET   2u      /* AREAS: kind wetland (natural=wetland) */

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

/* [roam PB] the sport / surface codes of one AREAS (or PARK_RELATIONS) record: leisure=stadium -> TD5_RPS_STADIUM,
 * a leisure=pitch -> its sport= (TD5_RPS_NONE when untagged) and SC_SRF_PITCH | its surface= */
#define SC_SRF_PITCH 0x80u
static void sc_sport_codes(const cJSON *e, unsigned char *spt, unsigned char *srf)
{
    const char *le = cJSON_GetStringValue(cJSON_GetObjectItem(e, "leisure"));
    *spt = 0; *srf = 0;
    if (!le) return;
    if (!strcmp(le, "stadium")) { *spt = (unsigned char)TD5_RPS_STADIUM; return; }
    if (strcmp(le, "pitch")) return;
    *spt = (unsigned char)td5_roam_sport_of(cJSON_GetStringValue(cJSON_GetObjectItem(e, "sport")));
    *srf = (unsigned char)(SC_SRF_PITCH | td5_roam_surface_of(cJSON_GetStringValue(cJSON_GetObjectItem(e, "surface"))));
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
        if (layer == SC_L_AREA) c->pool = (unsigned char *)calloc((size_t)cap_rec, 1);   /* [roam LE] */
        if (layer != SC_L_FOOT) c->kf = (unsigned char *)calloc((size_t)cap_rec, 1);     /* [roam PC] */
        if (layer == SC_L_AREA) c->spt = (unsigned char *)calloc((size_t)cap_rec, 1);    /* [roam PB] */
        if (layer == SC_L_AREA) c->srf = (unsigned char *)calloc((size_t)cap_rec, 1);
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
            if (c->kf) {                                 /* [roam PC] */
                const char *cls = cJSON_GetStringValue(cJSON_GetObjectItem(e, "class"));
                c->kf[c->n] = (unsigned char)(cls && !strcmp(cls, "greenhouse") ? SC_KF_GH : 0u);
            }
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
                if (c->pool) c->pool[c->n] = (unsigned char)(kv && cJSON_IsString(kv) && !strcmp(kv->valuestring, "swimming_pool"));
                if (c->kf && kv && cJSON_IsString(kv))       /* [roam PC] */
                    c->kf[c->n] = (unsigned char)(!strcmp(kv->valuestring, "greenhouse_horticulture") ? SC_KF_GH
                                                  : !strcmp(kv->valuestring, "wetland") ? SC_KF_WET : 0u);
            }
            if (c->spt && c->srf) sc_sport_codes(e, &c->spt[c->n], &c->srf[c->n]);   /* [roam PB] */
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

/* [roam JC2] RELATION OUTLINES. geo_fetch never asked Overpass for relations, so a building mapped as a
 * multipolygon (an outline with courtyards) was not in the tiles: the Legislatura (relation 1646678, the
 * pick), the Casa de Gobierno (19835072), the Teatro Argentino, the Municipalidad and 46 more, 41 of them
 * with inner rings. re/tools/geo_s3d_relations.py writes them to S3D_RELATIONS.JSON (source frame,
 * BUILDINGS-style fields). Each outer ring becomes one TD5_GeoBuilding record (rel = 1) of a pinned extra
 * cache that every consumer of the BUILDINGS caches sees (emitter, pavement frontage, infill), its inner
 * rings kept beside it. id = 8e15 + relation id * 8 + outer index: clear of the OSM way and Overture ids.
 * Without a height or a levels tag the height is an estimate (one storey here) the emitter replaces. */
#define SC_REL_ID_BASE 8.0e15
static int sc_rel_ring(const cJSON *pts, double *x, double *z, int cap)
{
    const cJSON *q;
    int raw = 0, n = 0, i = 0, stride;
    for (q = pts ? pts->child : NULL; q; q = q->next) raw++;
    if (raw >= 2) {   /* the ring is closed in the file: the last point repeats the first */
        const cJSON *a = pts->child, *b = cJSON_GetArrayItem(pts, raw - 1);
        if (cJSON_GetNumberValue(cJSON_GetObjectItem(a, "x")) == cJSON_GetNumberValue(cJSON_GetObjectItem(b, "x"))
            && cJSON_GetNumberValue(cJSON_GetObjectItem(a, "z")) == cJSON_GetNumberValue(cJSON_GetObjectItem(b, "z")))
            raw--;
    }
    if (raw < 3) return 0;
    stride = (raw + cap - 1) / cap;
    for (q = pts->child; q && i < raw && n < cap; q = q->next, i++) {
        if (i % stride) continue;
        x[n] = cJSON_GetNumberValue(cJSON_GetObjectItem(q, "x"));
        z[n] = cJSON_GetNumberValue(cJSON_GetObjectItem(q, "z"));
        n++;
    }
    return n >= 3 ? n : 0;
}

static void sc_rel_copy(cJSON *dst, const cJSON *src, const char *key)
{
    const cJSON *v = src ? cJSON_GetObjectItem(src, key) : NULL;
    if (v && !cJSON_IsNull(v) && !cJSON_GetObjectItem(dst, key)) cJSON_AddItemToObject(dst, key, cJSON_Duplicate(v, 1));
}

static void sc_rel_load(TD5_RoamScnSource *s)
{
    static const char *const k_fields[] = { "class", "name", "roof_shape", "colour", "roof_colour", "material",
                                            "roof_material", "levels", "height_m", "min_height_m", "roof_height_m", NULL };
    static const char *const k_tags[] = { "government", "amenity", "tourism", "historic", "heritage", "denomination",
                                          "religion", "office", NULL };
    char path[512];
    char *json;
    cJSON *root, *arr, *r;
    ScCache *c = &s->rel;
    int cap = 0, cap_pt = 0, nrec = 0, k;
    double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX];
    memset(c, 0, sizeof *c);
    c->used = 1; c->layer = SC_L_BLD; c->pin = 1; c->storey_m = 3.0;
    td5_geo_source_path(path, sizeof path, s->place, "S3D_RELATIONS.JSON");
    json = sc_slurp(path, NULL);
    if (!json) return;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return;
    td5_geo_src_json_to_game(root);          /* [roam M] source frame -> game frame */
    arr = cJSON_GetObjectItem(root, "relations");
    for (r = arr ? arr->child : NULL; r; r = r->next) cap += cJSON_GetArraySize(cJSON_GetObjectItem(r, "outer"));
    if (cap > 0) {
        c->b = (TD5_GeoBuilding *)calloc((size_t)cap, sizeof(TD5_GeoBuilding));
        c->id = (double *)calloc((size_t)cap, sizeof(double));
        c->own = (int64_t *)calloc((size_t)cap, sizeof(int64_t));
        s->relx = (ScRelX *)calloc((size_t)cap, sizeof(ScRelX));
    }
    for (r = arr ? arr->child : NULL; r && c->b && c->id && c->own && s->relx; r = r->next) {
        const cJSON *outer = cJSON_GetObjectItem(r, "outer"), *inner = cJSON_GetObjectItem(r, "inner");
        const cJSON *tags = cJSON_GetObjectItem(r, "tags"), *o;
        const double rid = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "id"));
        int oi = 0;
        const char *cls = cJSON_GetStringValue(cJSON_GetObjectItem(r, "class"));
        if (!cls || cJSON_IsTrue(cJSON_GetObjectItem(r, "part"))) continue;        /* parts: none in La Plata */
        for (o = outer ? outer->child : NULL; o; o = o->next, oi++) {
            cJSON *rec = cJSON_CreateObject();
            const cJSON *h;
            TD5_GeoBuilding *b = &c->b[c->n];
            ScRelX *x = &s->relx[c->n];
            double id = SC_REL_ID_BASE + rid * 8.0 + oi, hm, lv;
            int n, first, tx, tz, f;
            if (!rec) break;
            cJSON_AddItemToObject(rec, "points", cJSON_Duplicate(o, 1));
            cJSON_AddNumberToObject(rec, "id", id);
            for (f = 0; k_fields[f]; f++) sc_rel_copy(rec, r, k_fields[f]);
            for (f = 0; k_tags[f]; f++) sc_rel_copy(rec, tags, k_tags[f]);
            hm = cJSON_GetNumberValue(cJSON_GetObjectItem(rec, "height_m"));
            lv = atof(cJSON_GetStringValue(cJSON_GetObjectItem(rec, "levels")) ? cJSON_GetStringValue(cJSON_GetObjectItem(rec, "levels")) : "0");
            x->h_est = 0;
            if (!(hm > 0.0) && lv > 0.0) {
                cJSON_DeleteItemFromObject(rec, "height_m");
                cJSON_AddNumberToObject(rec, "height_m", lv * 3.0);
                cJSON_AddStringToObject(rec, "height_src", "osm_levels");
            } else if (hm > 0.0) {
                cJSON_AddStringToObject(rec, "height_src", "osm_height");
            } else {
                cJSON_AddStringToObject(rec, "height_src", "estimated");
                x->h_est = 1;
            }
            cJSON_AddStringToObject(rec, "footprint_src", "osm");
            n = td5_geob_parse_building(rec, nrec, s->tag_lm, 3.0, s->upm, b, rx, rz, NULL);
            cJSON_Delete(rec);
            if (n <= 0) continue;
            first = sc_pool_push(c, &cap_pt, rx, rz, n);
            if (first < 0) break;
            b->first = first;
            b->rel = 1;
            b->area_m2 = fabs(td5_geob_ring_area(rx, rz, n)) / (s->upm * s->upm);
            c->id[c->n] = id;
            roam_grid_tile_of(&s->grid, b->cx, b->cz, &tx, &tz);
            c->own[c->n] = sc_tkey(tx, tz);
            /* the inner rings this outer holds (first point inside), CW */
            for (h = inner ? inner->child : NULL; h && x->nh < TG_S3D_HOLES_MAX; h = h->next) {
                double hx[TD5_GEOB_RING_MAX], hz[TD5_GEOB_RING_MAX];
                const int hn = sc_rel_ring(h, hx, hz, TD5_GEOB_RING_MAX);
                int hf;
                if (hn < 3 || !td5_geob_point_in_ring(rx, rz, n, hx[0], hz[0])) continue;
                if (td5_geob_ring_area(hx, hz, hn) > 0.0)
                    for (k = 0; k < hn / 2; k++) {
                        double t = hx[k]; hx[k] = hx[hn - 1 - k]; hx[hn - 1 - k] = t;
                        t = hz[k]; hz[k] = hz[hn - 1 - k]; hz[hn - 1 - k] = t;
                    }
                hf = sc_pool_push(c, &cap_pt, hx, hz, hn);
                if (hf < 0) break;
                x->hfirst[x->nh] = hf; x->hn[x->nh] = hn; x->nh++;
                x->hole_m2 += fabs(td5_geob_ring_area(hx, hz, hn)) / (s->upm * s->upm);
            }
            c->n++;
            nrec++;
        }
    }
    cJSON_Delete(root);
    sc_cache_sort(c);
    {
        int nh = 0, ne = 0;
        for (k = 0; k < c->n; k++) { nh += s->relx[k].nh; ne += s->relx[k].h_est; }
        TD5_LOG_I(LOG_TAG, "roam scenery: %d relation outline(s) from S3D_RELATIONS.JSON, %d courtyard ring(s), "
                  "%d with an estimated height", c->n, nh, ne);
    }
}

/* [roam JC2] the place point stands inside a relation outline (outer ring; courtyards included) */
static int sc_rel_covers(const TD5_RoamScnSource *s, double x, double z)
{
    int k;
    for (k = 0; k < s->rel.n; k++) {
        const TD5_GeoBuilding *b = &s->rel.b[k];
        const double dx = x - b->cx, dz = z - b->cz;
        if (dx * dx + dz * dz > b->radius * b->radius) continue;
        if (td5_geob_point_in_ring(&s->rel.px[b->first], &s->rel.pz[b->first], b->n, x, z)) return 1;
    }
    return 0;
}

/* [roam LD] PARK RELATIONS. geo_fetch never asked Overpass for relations, so a green mapped as a MULTIPOLYGON was
 * not in AREAS: in the casco Plaza Italia, Plaza Belgrano, Plaza Azcuenaga, Plaza Olazabal, Plaza Alsina and three
 * big grass polygons (12858959, 19828541, 19835071), the Hipodromo track, Gimnasia's ground, Estadio UNO, five
 * school / university / hospital precincts. re/tools/geo_plaza_parks.py writes them to PARK_RELATIONS.JSON (source
 * frame, AREAS field names). Each outer ring becomes one TD5_GeoArea record of a pinned extra AREAS cache that every
 * consumer of the AREAS caches sees (lawns, trees, plaza veto, green veto, lots, infill). Skipped: water (round L
 * LE draws water), a kind geo_fetch's whitelist would refuse (keep = false: the civic palaces, which
 * S3D_RELATIONS.JSON builds), a relation an AREAS way already maps (dup_of: a second lawn would z-fight).
 * PRECINCTS (sc_prel_precinct, and every class but green: a school, a hospital, the Hipodromo, a club ground, a
 * cemetery) keep their lot class only (no infill) and become TD5_GEOA_KIND_OTHER: no lawn, no plaza veto, no plaza
 * trees, because buildings stand in them (the Cementerio de La Plata alone would have lost 110). A real green keeps
 * its plaza kind, but never vetoes an OSM footprint (sc_in_plaza skip_rel): only Overture's ML blobs.
 * HOLES. The inner rings an outer holds (its first point inside) follow the outer's points, each joined to point
 * 0 by a bridge walked out and back (O0 H0 .. Hk-1 H0, then O0 again for the next): the bridge is crossed twice,
 * so point-in-ring parity is exact, and n_outer tells the lawn triangulation where the plain outline ends (a lawn
 * still covers a hole; what stands in one is drawn over it). A ring keeps at most TD5_GEOB_RING_MAX points: an
 * inner ring keeps SC_PREL_HOLE_PTS, the largest holes first, while the outer keeps >= SC_PREL_OUTER_MIN.
 * id = 7e15 + relation id * 8 + outer index: clear of the OSM way ids and JC2's 8e15 outlines. */
#define SC_PREL_ID_BASE    7.0e15
#define SC_PREL_HOLE_PTS   8
#define SC_PREL_OUTER_MIN  24
#define SC_PREL_HOLES_MAX  16

static int sc_prel_precinct(const char *kind)
{
    static const char *const k[] = { "cemetery", "grave_yard", "sports_centre", "stadium", "track", "golf_course",
                                     "pitch", NULL };
    int i;
    for (i = 0; kind && k[i]; i++) if (!strcmp(kind, k[i])) return 1;
    return 0;
}

static int sc_prel_orient(double *x, double *z, int n, int ccw)
{
    int k;
    if (n < 3) return 0;
    if ((td5_geob_ring_area(x, z, n) > 0.0) != (ccw != 0))
        for (k = 0; k < n / 2; k++) {
            double t = x[k]; x[k] = x[n - 1 - k]; x[n - 1 - k] = t;
            t = z[k]; z[k] = z[n - 1 - k]; z[n - 1 - k] = t;
        }
    return n;
}

static void sc_prel_load(TD5_RoamScnSource *s)
{
    static const char *const k_fields[] = { "name", "leisure", "landuse", "natural", "barrier", NULL };
    char path[512];
    char *json;
    cJSON *root, *arr, *r;
    ScCache *c = &s->prel;
    int cap = 0, cap_pt = 0, nrel = 0, nwater = 0, ndup = 0, nskip = 0, nh = 0, nh_drop = 0, nwith = 0, nprec = 0, k;
    memset(c, 0, sizeof *c);
    c->used = 1; c->layer = SC_L_AREA; c->pin = 1;
    td5_geo_source_path(path, sizeof path, s->place, "PARK_RELATIONS.JSON");
    json = sc_slurp(path, NULL);
    if (!json) { TD5_LOG_I(LOG_TAG, "roam scenery: no PARK_RELATIONS.JSON: no park relations"); return; }
    root = cJSON_Parse(json);
    free(json);
    if (!root) return;
    td5_geo_src_json_to_game(root);          /* [roam M] source frame -> game frame */
    arr = cJSON_GetObjectItem(root, "relations");
    for (r = arr ? arr->child : NULL; r; r = r->next) cap += cJSON_GetArraySize(cJSON_GetObjectItem(r, "outer"));
    if (cap > 0) {
        c->a = (TD5_GeoArea *)calloc((size_t)cap, sizeof(TD5_GeoArea));
        c->lot = (unsigned char *)calloc((size_t)cap, 1);
        c->spt = (unsigned char *)calloc((size_t)cap, 1);   /* [roam PB] */
        c->srf = (unsigned char *)calloc((size_t)cap, 1);
        c->id = (double *)calloc((size_t)cap, sizeof(double));
        c->own = (int64_t *)calloc((size_t)cap, sizeof(int64_t));
    }
    for (r = arr ? arr->child : NULL; r && c->a && c->lot && c->id && c->own; r = r->next) {
        const cJSON *outer = cJSON_GetObjectItem(r, "outer"), *inner = cJSON_GetObjectItem(r, "inner"), *o, *h;
        const char *cls = cJSON_GetStringValue(cJSON_GetObjectItem(r, "area_class"));
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(r, "kind"));
        const double rid = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "id"));
        int oi = 0;
        nrel++;
        if (cls && !strcmp(cls, "water")) { nwater++; continue; }
        if (!cJSON_IsTrue(cJSON_GetObjectItem(r, "keep"))) { nskip++; continue; }
        if (cJSON_IsNumber(cJSON_GetObjectItem(r, "dup_of"))) { ndup++; continue; }
        for (o = outer ? outer->child : NULL; o; o = o->next, oi++) {
            double ox[TD5_GEOB_RING_MAX], oz[TD5_GEOB_RING_MAX], X[TD5_GEOB_RING_MAX], Z[TD5_GEOB_RING_MAX];
            double hx[SC_PREL_HOLES_MAX][SC_PREL_HOLE_PTS], hz[SC_PREL_HOLES_MAX][SC_PREL_HOLE_PTS], ha[SC_PREL_HOLES_MAX];
            double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], cx = 0.0, cz = 0.0, rad = 0.0;
            int hn[SC_PREL_HOLES_MAX], ord[SC_PREL_HOLES_MAX], nho = 0, used = 0, take = 0, on, m, n, first, tx, tz, f;
            const double id = SC_PREL_ID_BASE + rid * 8.0 + oi;
            TD5_GeoArea *a = &c->a[c->n];
            cJSON *rec, *pa;
            /* the holes this outer holds (first point inside its 48-point outline) */
            on = sc_rel_ring(o, ox, oz, TD5_GEOB_RING_MAX);
            if (on < 3) continue;
            for (h = inner ? inner->child : NULL; h; h = h->next) {
                double tx2[SC_PREL_HOLE_PTS], tz2[SC_PREL_HOLE_PTS];
                const int q = sc_rel_ring(h, tx2, tz2, SC_PREL_HOLE_PTS);
                if (q < 3 || !td5_geob_point_in_ring(ox, oz, on, tx2[0], tz2[0])) continue;
                nh++;
                if (nho >= SC_PREL_HOLES_MAX) { nh_drop++; continue; }
                sc_prel_orient(tx2, tz2, q, 0);
                memcpy(hx[nho], tx2, sizeof tx2); memcpy(hz[nho], tz2, sizeof tz2);
                hn[nho] = q; ha[nho] = fabs(td5_geob_ring_area(tx2, tz2, q)); ord[nho] = nho;
                nho++;
            }
            for (k = 1; k < nho; k++) {            /* largest first (then file order) */
                const int t = ord[k];
                for (m = k - 1; m >= 0 && ha[ord[m]] < ha[t]; m--) ord[m + 1] = ord[m];
                ord[m + 1] = t;
            }
            while (take < nho && TD5_GEOB_RING_MAX - (used + hn[ord[take]] + 2) >= SC_PREL_OUTER_MIN) {
                used += hn[ord[take]] + 2;
                take++;
            }
            nh_drop += nho - take;
            on = sc_rel_ring(o, ox, oz, TD5_GEOB_RING_MAX - used);
            if (on < 3) continue;
            sc_prel_orient(ox, oz, on, 1);
            for (m = 0; m < on; m++) { X[m] = ox[m]; Z[m] = oz[m]; cx += ox[m]; cz += oz[m]; }
            cx /= (double)on; cz /= (double)on;
            for (m = 0; m < on; m++) { const double d = hypot(ox[m] - cx, oz[m] - cz); if (d > rad) rad = d; }
            for (k = 0; k < take; k++) {
                const int t = ord[k];
                int j;
                X[m] = ox[0]; Z[m] = oz[0]; m++;
                for (j = 0; j < hn[t]; j++) { X[m] = hx[t][j]; Z[m] = hz[t][j]; m++; }
                X[m] = hx[t][0]; Z[m] = hz[t][0]; m++;
            }
            rec = cJSON_CreateObject();
            pa = cJSON_CreateArray();
            if (!rec || !pa) { cJSON_Delete(rec); cJSON_Delete(pa); break; }
            for (k = 0; k < m; k++) {
                cJSON *p = cJSON_CreateObject();
                if (!p) break;
                cJSON_AddNumberToObject(p, "x", X[k]);
                cJSON_AddNumberToObject(p, "z", Z[k]);
                cJSON_AddItemToArray(pa, p);
            }
            cJSON_AddItemToObject(rec, "points", pa);
            cJSON_AddTrueToObject(rec, "closed");
            cJSON_AddNumberToObject(rec, "id", id);
            for (f = 0; k_fields[f]; f++) sc_rel_copy(rec, r, k_fields[f]);
            n = td5_geob_parse_area(rec, c->n, s->area_wide, a, rx, rz, NULL);
            cJSON_Delete(rec);
            if (n != m || rx[0] != X[0] || rz[0] != Z[0]) { memset(a, 0, sizeof *a); continue; }   /* reversed / decimated: no bridges */
            /* a PRECINCT keeps its lot class (no infill) but is no plaza: no lawn, no plaza veto, no plaza trees
             * (the census: the Cementerio de La Plata's 110 mausoleums and galleries, Gimnasia's club buildings) */
            if (!(cls && !strcmp(cls, "green")) || sc_prel_precinct(kind)) { a->kind = TD5_GEOA_KIND_OTHER; nprec++; }
            {   /* the plaza trees' species (sc_emit_trees: id_hash % TD5_TG_TREE_VARIANTS): from the RELATION id, past
                 * the two conifers (3, and 4 the snow conifer: pale spikes in a La Plata plaza) */
                unsigned int hh = (unsigned int)fmod(rid, 4294967296.0) * 2654435761u ^ 0x9E3779B9u;
                while ((hh % (unsigned)TD5_TG_TREE_VARIANTS) == 3u || (hh % (unsigned)TD5_TG_TREE_VARIANTS) == 4u)
                    hh = hh * 2654435761u + 0x7F4A7C15u;
                a->id_hash = hh;
            }
            first = sc_pool_push(c, &cap_pt, rx, rz, n);
            if (first < 0) break;
            a->first = first;
            a->n_outer = (unsigned short)(take > 0 ? on : 0);
            a->cx = cx; a->cz = cz; a->radius = rad;          /* the outline's, not the holes' */
            c->lot[c->n] = (unsigned char)sc_lot_class(kind);
            if (c->spt && c->srf) sc_sport_codes(r, &c->spt[c->n], &c->srf[c->n]);   /* [roam PB] */
            c->id[c->n] = id;
            roam_grid_tile_of(&s->grid, cx, cz, &tx, &tz);
            c->own[c->n] = sc_tkey(tx, tz);
            if (take > 0) nwith++;
            c->n++;
        }
    }
    cJSON_Delete(root);
    sc_cache_sort(c);
    c->bytes = (size_t)c->npt * 16u + (size_t)c->n * (sizeof(double) + sizeof(int64_t) + sizeof(int) + sizeof(TD5_GeoArea));
    TD5_LOG_I(LOG_TAG, "ROAM_PARK_REL %d relation(s) read: %d outline(s) loaded (%d precinct(s): lot class only, %d with "
              "holes, %d hole ring(s) kept, %d dropped by the point budget), skipped: %d water, %d not an area kind, %d already "
              "an AREAS way", nrel, c->n, nprec, nwith, nh - nh_drop, nh_drop, nwater, nskip, ndup);
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
typedef struct { double x[4], z[4]; int n; double x0, z0, x1, z1; int seg; int cap_row, lm_ok;
                 double y[4], y0, y1; } ScPrim;   /* [roam ID] the surface height per corner + its range */
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
/* [roam NA] one footprint's municipal-line answer (place-frame ring in c->mpool when snapped), and a landmark's
 * GROUNDS: the building-line polygon of the block a major landmark stands in (tile-local, c->gpool x then z) */
typedef struct ScMuniRing { double id, cx, cz; int first, n, fate; unsigned char fwd, back, corners, ochavas; } ScMuniRing;
typedef struct ScGround { int first, n; double x0, z0, x1, z1; double lm_id; } ScGround;

struct TG_TileCtx {
    const RoamGrid *grid;
    int      sport_on;        /* [roam PB] 1 = stadiums (TD5RE_ROAM_STADIUM), 2 = pitch paint (TD5RE_ROAM_PITCH_LINES) */
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
    ScLotPoly *ghp, *wetp;      /* [roam PC] greenhouse_horticulture / wetland AREAS polygons reaching the 3x3 (sc_kf_polys) */
    int      nghp, nwetp;
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
    /* [roam IA] the drawn ground's lattice for the lawns (sc_dg): cells per tile side, the render's ground drop,
     * and the knob (TD5RE_ROAM_LAWN_ON_GROUND) */
    int      dg_G, lawn_dg;
    int      lawn_clip, lawn_gap_cen;   /* [roam QB] the exact lawn cut, its census */
    struct ScLawnCover *lcov;            /* [roam QB] the plaza lawns of the 3x3 that draw (SECTION plazas)  */
    int      nlcov, lcov_self, lawn_dedup;
    double   dg_drop;
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
    /* [roam LA] SECTION trees: the knobs, the street / lot tree candidates (emitted after the furniture), every
     * planted crown (x, z, radius; plazas too) for the canopy census, and each sub-block's tree records */
    int      st_on, lt_on;
    int      shore_on;              /* [roam PC] TD5RE_ROAM_SHORE: lots / lawns / cover paint keep off shore cells */
    int      pt_lot_set;            /* [integ L] plaza trees from LA's lot species set (TD5RE_ROAM_PLAZA_TREE_PAGES) */
    int      xc_on;                 /* [integ L] tree cross-check census (TD5RE_ROAM_TREE_XCHECK, dev) */
    int      tv_on;                 /* [integ L] round L tree veto (TD5RE_ROAM_TREE_VETO_L): see sc_emit_street_trees */
    TD5_RoamRailSource *rails;      /* [integ L] this build's railways (roam_ctx_rails_bed_near), NULL = none */
    struct ScXcRange *xc_r;         /* [integ L] mesh ranges per emitter, (sub-block, first, end, category) */
    int      xc_n, xc_cap;
    int     *late_pz, nlate_pz, cap_late_pz;   /* [integ L] (sub-block, mesh) of each late plaza tree (cross-check) */
    void    *pv_bld;                /* [roam LA] the pavement's footprint set (PvBld), kept for the lots (sc_canopy_green) */
    struct ScTreeCand *tcand;
    int      ntcand, cap_tcand;
    double  *tpos;
    int      ntpos, cap_tpos;
    int      tree_first[TD5_ROAM_SCN_NSB], tree_end[TD5_ROAM_SCN_NSB];
    /* [roam PD] SECTION trees: rule street trees (TD5RE_ROAM_RULE_TREES, casco tiles only), the trunk-in-a-footprint
     * test at emission (TD5RE_ROAM_TREE_BLD) over every footprint ring this tile emitted (after its clearance shift),
     * the avenue / diagonal kerb samples of the coverage census (x, z, class), the dev tree log */
    int      rt_on, rt_casco, tb_on, tree_log, rt_face;
    struct StRing *ebr;
    int      nebr, cap_ebr;
    double  *ebp;
    int      nebp, cap_ebp;
    double  *rks;
    int      nrks, cap_rks;
    /* [roam NA] SECTION municipal line: the line pieces of the 3x3, every footprint's answer, the landmark grounds */
    TD5_Muni *muni;
    TD5_Muni *muni2;            /* the DRAWN line (second pass, with the snapped rings + the facade clip) */
    int      muni_ready;
    ScMuniRing *mr;  int nmr, cap_mr;
    double  *mpool;  int nmp, cap_mp;
    ScGround *grd;   int ngrd, cap_grd;
    double  *gpool;  int ngp, cap_gp;
    /* [roam PE] the NC cutting bands of the 3x3 (place frame): the band (kerb -> wall back) and the zone (+ the coping
     * apron). The line keeps off the zone (TD5RE_ROAM_MUNI_CUT); the census reads both whatever the knob. */
    TD5_RoamCutBands mcut_band, mcut_zone;
    int      mcut_skip;
    void    *shd;                    /* [roam PE] ScShopDefer: shopfronts waiting for the pavement + infill */
};

static int st_push_pos(TG_TileCtx *c, double x, double z, double r);   /* [roam LA] SECTION trees */
static int st_push_ring(TG_TileCtx *c, const double *rx, const double *rz, int n);   /* [roam PD] SECTION trees */
static int st_push_plaza_cand(TG_TileCtx *c, double x, double y, double z, double hw, double th, int var);   /* [integ L] */

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
    free(c->ghp); c->ghp = NULL; c->nghp = 0;            /* [roam PC] */
    free(c->wetp); c->wetp = NULL; c->nwetp = 0;
    td5_roam_gfix_free(&c->gf);
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) { c->sb[k].blk.len = 0; c->sb[k].blk.oom = 0; c->sb[k].nmesh = 0; c->furn_first[k] = 0x7FFFFFFF; c->lot_first[k] = 0x7FFFFFFF; }
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) c->tree_first[k] = c->tree_end[k] = 0x7FFFFFFF;   /* [roam LA] */
    c->ntcand = 0; c->ntpos = 0;
    c->nebr = 0; c->nebp = 0; c->nrks = 0;                                                  /* [roam PD] */
    c->xc_n = 0; c->nlate_pz = 0;                                                             /* [integ L] */
    memset(&c->st, 0, sizeof c->st);
    memset(&c->bst, 0, sizeof c->bst);
}

static void sc_shd_free(TG_TileCtx *c);                              /* [roam PE] */
static int inf_in_lot(const TG_TileCtx *c, double x, double z);   /* [HD] SECTION lots */

static void sc_ctx_free(TG_TileCtx *c)
{
    int k;
    if (!c) return;
    sc_ctx_reset(c);
    free(c->prim); free(c->seg); free(c->edge);
    free(c->lx); free(c->lz); free(c->ly);
    free(c->pvq);
    free(c->infr);
    free(c->tcand); free(c->tpos);   /* [roam LA] */
    free(c->ebr); free(c->ebp); free(c->rks);   /* [roam PD] */
    free(c->xc_r); free(c->late_pz); /* [integ L] */
    td5_muni_free(c->muni); td5_muni_free(c->muni2); free(c->mr); free(c->mpool); free(c->grd); free(c->gpool);   /* [roam NA] */
    td5_roam_cut_bands_free(&c->mcut_band); td5_roam_cut_bands_free(&c->mcut_zone);   /* [roam PE] */
    sc_shd_free(c);
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
    p->y0 = p->y1 = p->y[0];
    for (k = 1; k < p->n; k++) {
        if (p->y[k] < p->y0) p->y0 = p->y[k];
        if (p->y[k] > p->y1) p->y1 = p->y[k];
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
            pr->y[0] = ly0; pr->y[1] = lyn; pr->y[2] = ryn; pr->y[3] = ry0;
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
        for (k = 0; k < 3; k++) { sc_tvert(c, t, tr->ox, tr->oy, tr->oz, &t->vert[tr->v[k]], &pr->x[k], &y, &pr->z[k]); pr->y[k] = y; }
        pr->y[3] = pr->y[2];
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

/* [roam PA] */
int roam_ctx_nseg(const TG_TileCtx *c) { return c ? c->nseg : 0; }
int roam_ctx_seg(const TG_TileCtx *c, int i, RoamSegInfo *out)
{
    const ScSeg *s;
    const ScEdge *ed;
    if (!c || !out || i < 0 || i >= c->nseg) return 0;
    s = &c->seg[i];
    ed = &c->edge[s->edge];
    out->ax = s->ax; out->az = s->az; out->ay = s->ay; out->bx = s->bx; out->bz = s->bz; out->by = s->by;
    out->hw = s->hw; out->key = ed->key; out->klass = ed->klass;
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

/* [integ L] 1 when the drawn track bed / a platform comes within `clear` units of the tile-local polygon x/z */
int roam_ctx_rails_bed_near(const TG_TileCtx *c, const double *x, const double *z, int n, double clear)
{
    double X[8], Z[8];
    int k;
    if (!c->rails || n < 1 || n > 8) return 0;
    for (k = 0; k < n; k++) { X[k] = x[k] + (double)c->org_x; Z[k] = z[k] + (double)c->org_z; }
    return td5_roam_rails_bed_near(c->rails, X, Z, n, clear);
}
int roam_ctx_sb_of(const TG_TileCtx *c, double x, double z) { return sc_sb_of(c, x, z); }
double roam_ctx_ground(const TG_TileCtx *c, double x, double z) { return sc_ground((void *)c, x, z); }
static double sc_dg(TG_TileCtx *c, double x, double z);   /* [roam NC] SECTION plazas */
double roam_ctx_drawn_ground(const TG_TileCtx *c, double x, double z) { return sc_dg((TG_TileCtx *)c, x, z); }
TD5_RoamHeight *roam_ctx_hc(const TG_TileCtx *c) { return c->hc; }

/* [roam ID] the RAW raster at a tile-local point (no G4b road lowering): a pure function of the place
 * position, so two tiles meeting at a seam read the same value. 0 when no window covers it. */
int roam_ctx_ground_raw(const TG_TileCtx *c, double x, double z, double *h, int *wet)
{
    if (!c->hc) return 0;
    return td5_roam_height_sample(c->hc, x + (double)c->org_x, z + (double)c->org_z, h, wet);
}

/* [roam ID] the height of primitive p at (x, z) when the point lies on it (barycentric on its triangle;
 * a quad is the two triangles 0-1-2, 0-2-3) */
static int sc_prim_y_at(const ScPrim *p, double x, double z, double *y)
{
    int t;
    for (t = 0; t < (p->n == 4 ? 2 : 1); t++) {
        const int a = 0, b = t + 1, cc = t + 2;
        const double d = (p->z[b] - p->z[cc]) * (p->x[a] - p->x[cc]) + (p->x[cc] - p->x[b]) * (p->z[a] - p->z[cc]);
        double l0, l1, l2;
        if (fabs(d) < 1e-9) continue;
        l0 = ((p->z[b] - p->z[cc]) * (x - p->x[cc]) + (p->x[cc] - p->x[b]) * (z - p->z[cc])) / d;
        l1 = ((p->z[cc] - p->z[a]) * (x - p->x[cc]) + (p->x[a] - p->x[cc]) * (z - p->z[cc])) / d;
        l2 = 1.0 - l0 - l1;
        if (l0 < -1e-6 || l1 < -1e-6 || l2 < -1e-6) continue;
        *y = l0 * p->y[a] + l1 * p->y[b] + l2 * p->y[cc];
        return 1;
    }
    return 0;
}

/* [roam ID] diagnostics: the pavement slab / lawn plane height at a tile-local point (this tile's slabs; the
 * 3x3's lawns). 1 on a hit. */
static int sc_pv_y(const TG_TileCtx *c, double x, double z, double *y);
static int sc_lawn_y(const TG_TileCtx *c, double x, double z, double *y);
int roam_ctx_slab_y(const TG_TileCtx *c, double x, double z, double *y) { return sc_pv_y(c, x, z, y); }
int roam_ctx_lawn_y(const TG_TileCtx *c, double x, double z, double *y) { return sc_lawn_y(c, x, z, y); }

typedef struct { double x, z, ref, best, y; int hit; } ScRoadYQ;
static int sc_road_y_q(void *u, const ScPrim *p)
{
    ScRoadYQ *q = (ScRoadYQ *)u;
    double y;
    if (q->x < p->x0 || q->x > p->x1 || q->z < p->z0 || q->z > p->z1) return 0;
    if (!sc_prim_y_at(p, q->x, q->z, &y)) return 0;
    if (!q->hit || fabs(y - q->ref) < q->best) { q->best = fabs(y - q->ref); q->y = y; q->hit = 1; }
    return 0;                                     /* every prim: a bridge deck over a street is two answers */
}

int roam_ctx_road_y(const TG_TileCtx *c, double x, double z, double ref_y, double *y)
{
    ScRoadYQ q;
    q.x = x; q.z = z; q.ref = ref_y; q.best = 0.0; q.y = 0.0; q.hit = 0;
    sc_prims_in((TG_TileCtx *)c, x - 1.0, z - 1.0, x + 1.0, z + 1.0, sc_road_y_q, &q);
    if (q.hit) *y = q.y;
    return q.hit;
}

typedef struct { const double *x, *z; int n; double lo, hi; } ScBandQ;
static int sc_band_q(void *u, const ScPrim *p)
{
    ScBandQ *q = (ScBandQ *)u;
    if (p->y1 < q->lo || p->y0 > q->hi) return 0;
    return sc_poly_overlap(q->x, q->z, q->n, p->x, p->z, p->n) ? 1 : 0;
}

int roam_ctx_on_road_band(const TG_TileCtx *c, const double *x, const double *z, int n, double y_lo, double y_hi)
{
    ScBandQ q;
    double x0 = x[0], x1 = x[0], z0 = z[0], z1 = z[0];
    int k;
    for (k = 1; k < n; k++) {
        if (x[k] < x0) x0 = x[k];
        if (x[k] > x1) x1 = x[k];
        if (z[k] < z0) z0 = z[k];
        if (z[k] > z1) z1 = z[k];
    }
    q.x = x; q.z = z; q.n = n; q.lo = y_lo; q.hi = y_hi;
    return sc_prims_in((TG_TileCtx *)c, x0, z0, x1, z1, sc_band_q, &q);
}
void *roam_ctx_mesh_begin(TG_TileCtx *c, int sb)
{
    ScSb *b = sc_sb(c, sb, 1);
    if (!b) return NULL;
    b->moff[b->nmesh++] = b->blk.len;
    return &b->blk;
}

/* [integ L] TREE CROSS-CHECK ranges: which meshes each round L emitter wrote (a snapshot of every sub-block's mesh
 * count before the call, the new meshes noted after it). Read only by sc_tree_xcheck; writes no byte. */
/* [integ P] + SPORT (td5_roam_stadium.c stands, bowls, pitches), LANE (roam PA lane paint + track kerbs, ROAM_XC_LANE),
 * GH (roam PC greenhouse tunnels) */
enum { SC_XC_SHOP, SC_XC_FUEL, SC_XC_PLAZA, SC_XC_RAIL, SC_XC_OUT, SC_XC_POOL, SC_XC_SPORT, SC_XC_LANE, SC_XC_GH, SC_XC_NCAT,
       SC_XC_FURN = 10, SC_XC_TREE_PLAZA = 11 };
struct ScXcRange { int sb, cat, m0, m1; };

static void sc_xc_snap(const TG_TileCtx *c, int *snap)
{
    int k;
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) snap[k] = c->sb[k].nmesh;
}

static void sc_xc_note(TG_TileCtx *c, const int *snap, int cat)
{
    int k;
    if (!c->xc_on && !c->tv_on) return;          /* the tree veto reads the SHOP / POOL ranges too */
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) {
        if (c->sb[k].nmesh <= snap[k]) continue;
        if (!sc_grow((void **)&c->xc_r, &c->xc_cap, c->xc_n + 1, sizeof(struct ScXcRange))) return;
        c->xc_r[c->xc_n].sb = k; c->xc_r[c->xc_n].cat = cat;
        c->xc_r[c->xc_n].m0 = snap[k]; c->xc_r[c->xc_n].m1 = c->sb[k].nmesh;
        c->xc_n++;
    }
}

void roam_ctx_xc_snap(const TG_TileCtx *c, int *snap) { sc_xc_snap(c, snap); }                 /* [integ P] */
void roam_ctx_xc_note(TG_TileCtx *c, const int *snap, int cat) { sc_xc_note(c, snap, cat); }

/* ============================================================================
 * SECTION: buildings
 * ========================================================================== */

/* [roam NA] SECTION municipal line */
#define MU_SNAP_GAP 215.0      /* 0.5 m: a footprint on the line is still kept this far off a centreline's kerb */
static const ScMuniRing *sc_muni_find(const TG_TileCtx *c, double id, double cx, double cz);
static int sc_in_grounds(const TG_TileCtx *c, double x, double z);
static void sc_muni_census(TD5_RoamScnSource *s, TG_TileCtx *c, const double *rx, const double *rz, int n);
static int sc_ring_over_cut(const TD5_RoamCutBands *b, const TG_TileCtx *c, const double *rx, const double *rz, int n);   /* [roam PE] */
/* [roam NA] TD5RE_ROAM_RING_LOG=1 (dev): one line per emitted footprint / infill lot, its ring (place frame) */
static void sc_ring_dump(const TD5_RoamScnSource *s, const TG_TileCtx *c, double id, const double *rx, const double *rz, int n)
{
#ifndef TD5RE_RELEASE
    char pts[1400];
    int o = 0, k;
    if (!s->ring_log) return;
    pts[0] = '\0';
    for (k = 0; k < n && o < (int)sizeof pts - 40; k++)
        o += snprintf(pts + o, sizeof pts - (size_t)o, " %.0f,%.0f", rx[k] + (double)c->org_x, rz[k] + (double)c->org_z);
    TD5_LOG_I(LOG_TAG, "ROAM_RING tile %d,%d id %.0f n %d:%s", c->tx, c->tz, id, n, pts);
#else
    (void)s; (void)c; (void)id; (void)rx; (void)rz; (void)n;
#endif
}
static int sc_lot_class_at(const ScLotPoly *P, int n, double x, double z);   /* SECTION lots */
static int sc_kf_in(const ScLotPoly *P, int n, double x, double z);       /* [roam PC] SECTION lots */

typedef struct {
    const TD5_GeoBuilding *b;
    const double *px, *pz;     /* place-frame ring (cache pool) */
    double id;
    double storey_m;           /* its file's storey height      */
    const ScRelX *rx;          /* [JC2] courtyards (a relation outline), else NULL */
    const ScCache *rc;         /*   ... and the pool they live in */
} ScBld;

typedef struct {
    const TD5_GeoArea *a;
    const double *px, *pz;
    double id;
    unsigned char spt, srf;    /* [roam PB] the cache's sport / surface codes (sc_sport_codes) */
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

static int sc_in_plaza(const ScArea *ar, int na, double x, double z, int skip_rel)
{
    int k;
    for (k = 0; k < na; k++) {
        const TD5_GeoArea *a = ar[k].a;
        if (!td5_geob_area_is_plaza(a)) continue;
        if (skip_rel && ar[k].id >= SC_PREL_ID_BASE && ar[k].id < SC_PREL_ID_BASE + 1.0e15) continue;   /* [LD] */
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
        /* [JC2] lm_wins 2 (a building:part or an OSM relation outline): it also stands over SERVICE ways, the
         * courtyard driveways the Casa de Gobierno, the Municipalidad and the ministries are mapped with (the
         * walls open into a carriage arch there, td5_tg_s3d.c s3d_edge_portals) */
        ScPrim *marked[64];
        int nm = 0, g, clear;
        c->skip_lm = 1;
        for (g = 0; g < 64; g++) {
            if (!sc_on_road(c, rx, rz, n, &hit)) break;
            if (!sc_lm_stub_ok(c, hit, rx, rz, n)
                && !(lm_wins == 2 && hit->seg >= 0 && c->edge[c->seg[hit->seg].edge].klass == TD5_GEO_RC_SERVICE)) break;
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
        /* [LD] a park relation never takes an OSM footprint: the mapper drew that building in the park */
        in += sc_in_plaza(ar, nar, k < 0 ? gb->cx : px[k], k < 0 ? gb->cz : pz[k], gb->fsrc == TD5_GEOB_FSRC_OSM);
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
static int sc_bld_drops(TD5_RoamScnSource *s, TG_TileCtx *c, const TD5_GeoBuilding *gb, double id, const double *px,
                        const double *pz, const ScArea *ar, int nar)
{
    double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], shift = 0.0, gap = s->gap;
    ScBldU u;
    int k, n = gb->n;
    const ScMuniRing *mu;
    if (gb->landmark || gb->part || gb->rel) return 0;
    if (s->s3d && gb->fsrc == TD5_GEOB_FSRC_OVERTURE && sc_rel_covers(s, gb->cx, gb->cz)) return 1;   /* [JC2] */
    if (n < 3 || n > TD5_GEOB_RING_MAX) return 1;
    if (sc_bld_plaza_veto(ar, nar, gb, px, pz) || sc_bld_green_veto(c, gb, px, pz)) return 1;
    if (gb->fsrc != TD5_GEOB_FSRC_OSM && sc_in_grounds(c, gb->cx - (double)c->org_x, gb->cz - (double)c->org_z))
        return 1;                                                                  /* [roam NA] landmark grounds */
    for (k = 0; k < n; k++) { rx[k] = px[k] - (double)c->org_x; rz[k] = pz[k] - (double)c->org_z; }
    mu = sc_muni_find(c, id, gb->cx, gb->cz);                                     /* [roam NA] */
    if (mu && (mu->fate == TD5_MUNI_SNAPPED || mu->fate == TD5_MUNI_ON)) {
        if (mu->fate == TD5_MUNI_SNAPPED) {
            n = mu->n;
            for (k = 0; k < n; k++) { rx[k] = c->mpool[mu->first + k] - (double)c->org_x;
                                      rz[k] = c->mpool[mu->first + n + k] - (double)c->org_z; }
        }
        gap = MU_SNAP_GAP;
    }
    if (!td5_geob_ring_simple(rx, rz, n)) return 1;
    for (k = 0; k < n; k++) if (!sc_in_reach(c, rx[k], rz[k])) return 0;   /* beyond the 3x3: not ours to judge */
    memset(&u, 0, sizeof u);
    u.c = c;
    return sc_bld_clear(c, &u, gap, rx, rz, n, &shift, 0) != 0;
}

/* [IC] TD5RE_ROAM_BLD_LOG=1: what became of one footprint (place frame centroid), for the offline census */
static void sc_bld_note(const TD5_RoamScnSource *s, const TG_TileCtx *c, const ScBld *b, const char *fate)
{
    if (!s->bld_log) return;
    TD5_LOG_I(LOG_TAG, "ROAM_BLD tile %d,%d id %.0f src %d c (%.0f,%.0f) area %.0f h %.1f: %s", c->tx, c->tz, b->id,
              (int)b->b->fsrc, b->b->cx, b->b->cz, b->b->area_m2, b->b->height / SC_UPM, fate);
}

/* [JC] SIMPLE 3D BUILDINGS: an outline its building:parts replace. Mariano, 2026-10-21, on Plaza San Martin:
 * "isn't there the geometry available?". OSM's rule (wiki Simple_3D_Buildings) is that a building outline holding
 * building:part ways is NOT rendered: the parts are the building. Roam drew both, so the Catedral's 20 m outline box
 * swallowed its 25 parts below the spires. An outline (not itself a part) whose ring holds the centroids of parts
 * covering SC_S3D_COVER of its area is hidden; a partial set of parts (an outline with one tower mapped) keeps the
 * outline. Census over the partido (verify/roam/s3d_census.py): 9 outlines hold parts. */
#define SC_S3D_COVER 0.5
/* [JC2] parts covering this much of the outline need no BASE under them (the gaps they leave are slivers) */
#define SC_S3D_BASE_COVER 0.95
static int sc_s3d_hidden(const ScBld *set, int nset, int self, double *cover_out, double *low_top, int *civic,
                         unsigned int *roof_rgb)
{
    const TD5_GeoBuilding *o = set[self].b;
    double cover = 0.0, area;
    int k;
    *cover_out = 0.0; *low_top = 1e300; *civic = 0; *roof_rgb = 0;
    if (o->part || o->n < 3) return 0;
    area = fabs(td5_geob_ring_area(set[self].px, set[self].pz, o->n));
    if (set[self].rx) {                                /* [JC2] the solid area: courtyards out */
        int h;
        for (h = 0; h < set[self].rx->nh; h++)
            area -= fabs(td5_geob_ring_area(&set[self].rc->px[set[self].rx->hfirst[h]],
                                            &set[self].rc->pz[set[self].rx->hfirst[h]], set[self].rx->hn[h]));
    }
    if (!(area > 0.0)) return 0;
    for (k = 0; k < nset; k++) {
        const TD5_GeoBuilding *p = set[k].b;
        if (k == self || !p->part || p->n < 3) continue;
        if (hypot(p->cx - o->cx, p->cz - o->cz) > o->radius) continue;
        if (!td5_geob_point_in_ring(set[self].px, set[self].pz, o->n, p->cx, p->cz)) continue;
        cover += fabs(td5_geob_ring_area(set[k].px, set[k].pz, p->n));
        if (p->height < *low_top) *low_top = p->height;
        if (p->civic) *civic = 1;                      /* [JC2] what a BASE under them inherits */
        if (p->roof_rgb && !*roof_rgb) *roof_rgb = p->roof_rgb;
    }
    *cover_out = cover / area;
    return cover >= SC_S3D_COVER * area;
}

/* [JC2] the smallest non-part outline in the set whose ring holds this footprint's centroid: what a
 * building:part belongs to (its civic dressing is inherited) */
static int sc_s3d_host(const ScBld *set, int nset, int self)
{
    const TD5_GeoBuilding *c = set[self].b;
    double best = 1e300;
    int k, host = -1;
    for (k = 0; k < nset; k++) {
        const TD5_GeoBuilding *o = set[k].b;
        if (k == self || o->part || o->n < 3 || o->fsrc != TD5_GEOB_FSRC_OSM) continue;
        if ((c->cx - o->cx) * (c->cx - o->cx) + (c->cz - o->cz) * (c->cz - o->cz) > o->radius * o->radius) continue;
        if (!td5_geob_point_in_ring(set[k].px, set[k].pz, o->n, c->cx, c->cz)) continue;
        if (o->area_m2 < best) { best = o->area_m2; host = k; }
    }
    return host;
}

/* [roam LB] the shopfront's "is this tile-local point inside a footprint of the halo set".
 * [roam PE] With TD5RE_ROAM_SHOP_ABUT (pe = 1) the set is read as it will STAND: a footprint the municipal line moved is
 * tested on its moved ring (c->mpool, radius grown to it), the footprint being dressed (self) is not its own neighbour
 * (its OSM ring can stand in front of a wall the line cut back), and a fuel footprint drawn as the canopy is open
 * underneath (td5_roam_shops_canopy_fp). pe = 0 is LB's test, byte for byte. */
typedef struct {
    const ScBld *set; int nset; double ox, oz;
    int pe;                                   /* [roam PE] */
    const double *self;                       /* set[].px of the footprint being dressed (NULL = none) */
    const double **epx, **epz; int *en;       /* the standing ring of each set entry */
    double *er;                               /* ... its radius round the record's centroid */
    unsigned char *skip;                      /* a fuel canopy footprint: no mass */
    /* [roam PE] the DEFERRED dressing (TD5RE_ROAM_SHOP_ABUT): the shopfronts are laid after every footprint of the tile
     * stands (its clearance nudge, up to 4 m, moved neighbours onto 10 of the casco's buried shop walls) and after the
     * infill lots (sc_emit_pavement): gone[k] = an owned footprint the tile did not build, fin = the built rings */
    TG_TileCtx *c;
    unsigned char *gone;
    double *fin; int nfin, capfin; int *finoff;
    int infill;                               /* the infill lots are known (c->infr): a lot is a mass too */
} ScShopU;
static int sc_shop_in_bld(void *uv, double x, double z)
{
    const ScShopU *u = (const ScShopU *)uv;
    const double X = x + u->ox, Z = z + u->oz;
    int k;
    if (u->pe) {
        for (k = 0; k < u->nset; k++) {
            const TD5_GeoBuilding *b = u->set[k].b;
            const double dx = X - b->cx, dz = Z - b->cz;
            if (b->n < 3 || b->part || u->skip[k] || u->gone[k] || u->set[k].px == u->self || dx * dx + dz * dz > u->er[k] * u->er[k])
                continue;
            if (td5_geob_point_in_ring(u->epx[k], u->epz[k], u->en[k], X, Z)) return 1;
        }
        return u->infill && u->c && inf_in_lot(u->c, x, z);
    }
    for (k = 0; k < u->nset; k++) {
        const TD5_GeoBuilding *b = u->set[k].b;
        const double dx = X - b->cx, dz = Z - b->cz;
        if (b->n < 3 || b->part || dx * dx + dz * dz > b->radius * b->radius) continue;
        if (td5_geob_point_in_ring(u->set[k].px, u->set[k].pz, b->n, X, Z)) return 1;
    }
    return 0;
}

/* [roam PB] the leisure=stadium outline of the 3x3 holding a point (its ar index), -1 = none */
static int sc_stadium_at(const ScArea *ar, int nar, double x, double z)
{
    int k;
    for (k = 0; k < nar; k++) {
        const TD5_GeoArea *a = ar[k].a;
        const int no = a->n_outer ? a->n_outer : a->n;
        if (ar[k].spt != TD5_RPS_STADIUM || no < 3) continue;
        if ((x - a->cx) * (x - a->cx) + (z - a->cz) * (z - a->cz) > a->radius * a->radius) continue;
        if (td5_geob_point_in_ring(ar[k].px, ar[k].pz, no, x, z)) return k;
    }
    return -1;
}

/* [roam PB] WHAT td5_roam_stadium.c TAKES of the halo set: 1 = a stand (class stadium / grandstand / bleachers, its
 * centroid inside a leisure=stadium outline), 2 = an Overture / Open Buildings blob whose centroid falls inside such a
 * stand (the same structure traced twice: not drawn), 0 = neither. *stadium = the outline's ar index. A pure function
 * of the record and the 3x3, so the tile drawing the stadium and the tile owning a footprint agree. */
static int sc_sport_claim(const ScBld *set, int nset, int i, const ScArea *ar, int nar, int *stadium)
{
    const TD5_GeoBuilding *gb = set[i].b;
    const int st = sc_stadium_at(ar, nar, gb->cx, gb->cz);
    int k;
    if (stadium) *stadium = st;
    if (st < 0) return 0;
    if (gb->fsrc != TD5_GEOB_FSRC_OSM && !gb->rel) {
        for (k = 0; k < nset; k++) {
            const TD5_GeoBuilding *q = set[k].b;
            if (k == i || !q->stand || (q->fsrc != TD5_GEOB_FSRC_OSM && !q->rel)) continue;
            if ((gb->cx - q->cx) * (gb->cx - q->cx) + (gb->cz - q->cz) * (gb->cz - q->cz) > q->radius * q->radius) continue;
            if (sc_stadium_at(ar, nar, q->cx, q->cz) != st) continue;
            if (td5_geob_point_in_ring(set[k].px, set[k].pz, q->n, gb->cx, gb->cz)) return 2;
        }
        return 0;
    }
    return gb->stand ? 1 : 0;
}

/* [roam PE] one footprint waiting for its shopfront (deferred dressing) */
typedef struct {
    int    i, n, sb;
    double by, kerb_h;
    double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX];
    TD5_GeoBuilding g;
} ScShopItem;
typedef struct ScShopDefer {
    ScBld     *set;
    ScShopU    su;
    ScShopItem *it; int nit, cap;
} ScShopDefer;

static void sc_shopu_free(ScShopU *u)
{
    free((void *)u->epx); free(u->en); free(u->er); free(u->skip); free(u->gone); free(u->fin); free(u->finoff);
    u->epx = NULL; u->en = NULL; u->er = NULL; u->skip = NULL; u->gone = NULL; u->fin = NULL; u->finoff = NULL;
}

static void sc_shd_free(TG_TileCtx *c)
{
    ScShopDefer *d = (ScShopDefer *)c->shd;
    if (!d) return;
    sc_shopu_free(&d->su);
    free(d->set); free(d->it); free(d);
    c->shd = NULL;
}

/* [roam PE] the owned fuel site `site` has a footprint of the halo set that is drawn as its canopy */
static int sc_fuel_site_fp(void *uv, int site)
{
    const ScShopU *u = (const ScShopU *)uv;
    int k;
    if (!u->skip) return 0;
    for (k = 0; k < u->nset; k++) if (u->skip[k] == (unsigned char)(1 + (site & 0x7F)) && site < 127) return 1;
    return 0;
}

static void sc_xc_snap(const TG_TileCtx *c, int *xs);
static void sc_xc_note(TG_TileCtx *c, const int *xs, int cat);

/* [roam PE] the shopfronts of the tile, after every footprint and infill lot stands (TD5RE_ROAM_SHOP_ABUT) */
static int sc_shops_deferred(TD5_RoamScnSource *s, TG_TileCtx *c)
{
    ScShopDefer *d = (ScShopDefer *)c->shd;
    int k, ok = 1;
    if (!d) return 1;
    d->su.infill = 1;
    for (k = 0; k < d->nit && ok; k++) {
        const ScShopItem *it = &d->it[k];
        TD5_RoamShopBld sb_;
        int xs[TD5_ROAM_SCN_NSB];
        memset(&sb_, 0, sizeof sb_);
        sb_.id = d->set[it->i].id; sb_.gb = &it->g; sb_.rx = it->rx; sb_.rz = it->rz; sb_.n = it->n;
        sb_.by = it->by; sb_.kerb_h = it->kerb_h; sb_.sb = it->sb;
        sb_.in_bld = sc_shop_in_bld; sb_.u = &d->su;
        d->su.self = d->set[it->i].px;
        sc_xc_snap(c, xs);
        if (!td5_roam_shops_building(s->shops, c, &sb_, &c->st.shop)) ok = 0;
        sc_xc_note(c, xs, SC_XC_SHOP);
    }
    sc_shd_free(c);
    return ok;
}

static int sc_emit_buildings(TD5_RoamScnSource *s, TG_TileCtx *c, ScCache **bc, int nbc,
                             const ScArea *ar, int nar)
{
    ScBld *set = NULL;
    ScHull *fc = NULL;
    TD5_RoamGh *gh = NULL;
    ScShopU su;
    int nset = 0, cap = 0, i, k, l, dx, dz, nfc = 0, ghost_prev = 0;
    const int64_t self = sc_tkey(c->tx, c->tz);
    TD5_RoamScnStats *st_save = NULL;              /* [roam PD] the census, restored after a ghost footprint */
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
                    set[nset].rx = (bc[l] == &s->rel && s->relx) ? &s->relx[ri] : NULL;
                    set[nset].rc = bc[l];
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
    if (s->gh_on) {                                   /* [roam PC] greenhouses: batched here, flushed after the pass */
        gh = td5_roam_gh_begin();
        if (!gh) { free(set); free(fc); return 0; }
    }
    memset(&su, 0, sizeof su);
    sc_shd_free(c);                              /* [roam PE] nothing left from another tile */
    {
        su.set = set; su.nset = nset; su.ox = (double)c->org_x; su.oz = (double)c->org_z;
    }
    td5_roam_shops_tile_begin(s->shops);                               /* [roam PE] */
    if (s->shops && nset > 0 && (td5_roam_shops_abut(s->shops) || td5_roam_shops_canopy_fp_on(s->shops))) {
        /* [roam PE] the halo set as it will stand (read by the shopfront test when TD5RE_ROAM_SHOP_ABUT) and the fuel
         * footprints drawn as canopies (TD5RE_ROAM_SHOP_CANOPY_FP) */
        su.pe = td5_roam_shops_abut(s->shops);
        su.epx = (const double **)malloc((size_t)nset * 2u * sizeof(double *));
        su.en = (int *)malloc((size_t)nset * sizeof(int));
        su.er = (double *)malloc((size_t)nset * sizeof(double));
        su.skip = (unsigned char *)calloc((size_t)nset, 1);
        if (!su.epx || !su.en || !su.er || !su.skip) { sc_shopu_free(&su); free(set); free(fc); return 0; }
        su.epz = su.epx + nset;
        su.c = c;
        if (su.pe) {
            su.gone = (unsigned char *)calloc((size_t)nset, 1);
            su.finoff = (int *)malloc((size_t)nset * sizeof(int));
            if (!su.gone || !su.finoff) { sc_shopu_free(&su); free(set); free(fc); return 0; }
        }
        for (i = 0; i < nset; i++) {
            const TD5_GeoBuilding *gb = set[i].b;
            const ScMuniRing *mu = sc_muni_find(c, set[i].id, gb->cx, gb->cz);
            int site = -1;
            su.epx[i] = set[i].px; su.epz[i] = set[i].pz; su.en[i] = gb->n; su.er[i] = gb->radius;
            if (mu && mu->fate == TD5_MUNI_SNAPPED) {
                su.epx[i] = &c->mpool[mu->first]; su.epz[i] = &c->mpool[mu->first + mu->n]; su.en[i] = mu->n;
                for (k = 0; k < mu->n; k++) {
                    const double d = hypot(su.epx[i][k] - gb->cx, su.epz[i][k] - gb->cz);
                    if (d > su.er[i]) su.er[i] = d;
                }
            }
            if (td5_roam_shops_canopy_fp(s->shops, set[i].id, gb, &site) && site >= 0 && site < 127)
                su.skip[i] = (unsigned char)(1 + site);
            if (su.pe) {                        /* an owned footprint stands only once the pass builds it */
                int tx2, tz2;
                roam_grid_tile_of(c->grid, gb->cx, gb->cz, &tx2, &tz2);
                su.gone[i] = sc_tkey(tx2, tz2) == self;
                su.finoff[i] = -1;
            }
        }
    }
    for (i = 0; i < nset; i++) {
        const TD5_GeoBuilding *gb = set[i].b;
        double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], cx = 0.0, cz = 0.0, shift = 0.0;
        int tx, tz, n = gb->n, kind, anchor, veto, edge = -1, side = 1, r, sb;
        double along = 0.0, lat = 0.0;
        ScBldU u;
        TG_GeoBldEnv e;
        ScSb *b;
        int s3d_hide, s3d_base = 0, nh = 0, hn[TG_S3D_HOLES_MAX], np = 0, s3d_pciv = 0, mu_on_line = 0;
        const ScMuniRing *mu = NULL;
        unsigned int s3d_prgb = 0;
        double pseg[128 * 5];                          /* [integ J] 128 (was 16): a palace has dozens of drive segments */
        double s3d_cover = 0.0, s3d_low = 1e300, hx[TG_S3D_HOLES_MAX * TD5_GEOB_RING_MAX], hz[TG_S3D_HOLES_MAX * TD5_GEOB_RING_MAX];
        TD5_GeoBuilding gloc;
        int ghost = 0;
        if (ghost_prev) { c->st = *st_save; ghost_prev = 0; }
        roam_grid_tile_of(c->grid, gb->cx, gb->cz, &tx, &tz);
        if (sc_tkey(tx, tz) != self) {
            /* [roam PD] a GHOST: a neighbour's footprint that may stand over this tile. It runs the same vetoes and the
             * same clearance shift (this 3x3 holds every road that near the seam) but emits nothing: its shifted ring
             * joins the trunk test (st_in_bld), the neighbour builds it. The census is restored after it. */
            const double T = (double)c->grid->tile, R = gb->radius + SC_MAX_SHIFT;
            double uu, vv;
            if (!c->tb_on) continue;
            roam_grid_uv(c->grid, gb->cx, gb->cz, &uu, &vv);
            if (uu < (double)c->tx * T - R || uu > (double)(c->tx + 1) * T + R ||
                vv < (double)c->tz * T - R || vv > (double)(c->tz + 1) * T + R) continue;
            if (!st_save && !(st_save = (TD5_RoamScnStats *)malloc(sizeof *st_save))) { free(set); free(fc); return 0; }
            *st_save = c->st;
            ghost = ghost_prev = 1;
        }
        c->st.bld_seen++;
        if (n < 3 || n > TD5_GEOB_RING_MAX) { c->st.bld_drop_ring++; sc_bld_note(s, c, &set[i], "bad ring"); continue; }
        /* [JC2] an Overture / Open Buildings blob under an OSM relation outline: the outline is the building */
        if (s->s3d && gb->fsrc == TD5_GEOB_FSRC_OVERTURE && sc_rel_covers(s, gb->cx, gb->cz)) {
            c->st.bld_rel_ovt_drop++;
            sc_bld_note(s, c, &set[i], "under a relation outline");
            continue;
        }
        if (s->stadium_on) {   /* [roam PB] a stadium's stand is drawn by td5_roam_stadium.c, a blob traced over one not at all */
            const int cl = sc_sport_claim(set, nset, i, ar, nar, NULL);
            if (cl) {
                if (cl == 1) c->st.bld_stand++; else c->st.bld_stand_dup++;
                sc_bld_note(s, c, &set[i], cl == 1 ? "stadium stand (td5_roam_stadium.c)" : "traced over a stadium stand");
                continue;
            }
        }
        gloc = *gb;
        if (s->s3d && gb->rel) {
            /* [JC2] a relation with no height: the tallest blob under it (Open Buildings measured it), else two storeys */
            if (set[i].rx && set[i].rx->h_est) {
                double hmax = 0.0;
                for (k = 0; k < nset; k++) {
                    const TD5_GeoBuilding *q = set[k].b;
                    if (q->fsrc != TD5_GEOB_FSRC_OVERTURE
                        || (q->cx - gb->cx) * (q->cx - gb->cx) + (q->cz - gb->cz) * (q->cz - gb->cz) > gb->radius * gb->radius) continue;
                    if (!td5_geob_point_in_ring(set[i].px, set[i].pz, n, q->cx, q->cz)) continue;
                    if (q->height > hmax) hmax = q->height;
                }
                gloc.height = hmax > 0.0 ? hmax : 2.0 * set[i].storey_m * s->upm;
                if (!gloc.levels) gloc.levels = (unsigned short)(gloc.height / (3.0 * s->upm) + 0.5);
            }
            c->st.bld_rel++;
        }
        if (s->s3d && gb->part && !gb->civic) {                       /* [JC2] a part of a civic building is civic */
            const int h = sc_s3d_host(set, nset, i);
            if (h >= 0 && set[h].b->civic) gloc.civic = 1;
        }
        gb = &gloc;
        /* the plaza veto: most of the outline in a plaza, not a landmark, not a small OSM building. [JC] Nor a
         * building:part: a mapper modelled it in 3D, it stands. MEASURED: 6 of the 7 parts of the civic palace at Avenida 7 / 51 / 53 (Plaza San Martin) stand
         * on a mapped grass polygon (AREAS 176337780) and were vetoed, leaving its roof-only canopy alone as a box. */
        if (!(s->s3d && (gb->part || gb->rel)) && sc_bld_plaza_veto(ar, nar, gb, set[i].px, set[i].pz)) {
            c->st.bld_veto_plaza++;
            sc_bld_note(s, c, &set[i], "plaza veto");
            continue;
        }
        if (!gb->rel && sc_bld_green_veto(c, gb, set[i].px, set[i].pz)) {   /* [IC] park / ring plaza / avenue median */
            c->st.bld_veto_green++;
            sc_bld_note(s, c, &set[i], "green veto");
            continue;
        }
        if (c->ngrd > 0 && sc_in_grounds(c, gb->cx - (double)c->org_x, gb->cz - (double)c->org_z)) {   /* [roam NA] */
            if (gb->fsrc != TD5_GEOB_FSRC_OSM) {
                c->st.grounds_ovt++;
                sc_bld_note(s, c, &set[i], "landmark grounds veto");
                continue;
            }
            c->st.grounds_osm++;
            TD5_LOG_I(LOG_TAG, "ROAM_GROUNDS tile %d,%d OSM footprint id %.0f kept in landmark grounds (area %.0f m2, "
                      "landmark %d part %d)", c->tx, c->tz, set[i].id, gb->area_m2, (int)gb->landmark, (int)gb->part);
        }
        sc_lm_info(set, nset, i, &kind, &anchor, &veto);
        s3d_hide = s->s3d && sc_s3d_hidden(set, nset, i, &s3d_cover, &s3d_low, &s3d_pciv, &s3d_prgb);
        /* [JC2] parts that leave gaps: the outline stays as a flat BASE lower than every part (Mariano, 2026-10-21:
         * the Legislatura "stands on bare lawn" between its parts) */
        if (s3d_hide && s3d_cover < SC_S3D_BASE_COVER && s3d_low < 1e300) {
            const double top = (gloc.height < s3d_low ? gloc.height : s3d_low) - 0.3 * s->upm;
            if (top > 2.0 * s->upm) {
                s3d_base = 1;
                gloc.height = top;
                gloc.min_height = 0.0;
                gloc.roof3d = TD5_GEOB_R3_FLAT;
                gloc.roof_height = 0.0;
                gloc.roof_only = 0;
                if (s3d_pciv) gloc.civic = 1;              /* the base wears its parts' facade and roof */
                if (!gloc.roof_rgb) gloc.roof_rgb = s3d_prgb;
            }
        }
        if (s3d_hide && !s3d_base && !(anchor && kind == TD5_GEOB_LMK_WORSHIP)) {   /* a worship anchor still lays its apron */
            c->st.bld_s3d_hidden++;
            sc_bld_note(s, c, &set[i], "outline replaced by its building:parts");
            continue;
        }
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
        if (su.skip && su.skip[i]) {
            if (ghost) continue;          /* [integ P] a GHOST (roam PD) emits nothing: the neighbour draws its canopy */
            /* [roam PE] a fuel site's canopy traced as a building: LB's canopy on its rectangle, no mass. Before the
             * clearance: a forecourt reaches the kerb and its service drive, so 11 of the casco's 17 canopy footprints
             * were dropped as "on a road" / "past the shift cap" (nothing drawn); the canopy has its own road test (the
             * deck may span a service drive, the posts and islands stand clear of every drivable surface) */
            int xs_fp[TD5_ROAM_SCN_NSB], fp_ok;
            sc_xc_snap(c, xs_fp);
            fp_ok = td5_roam_shops_canopy_from_fp(s->shops, c, (int)su.skip[i] - 1, gb, rx, rz, n, &c->st.shop);
            sc_xc_note(c, xs_fp, SC_XC_FUEL);       /* [integ P] cross-check range */
            if (fp_ok) {
                sc_bld_note(s, c, &set[i], "fuel canopy");
                continue;
            }
            su.skip[i] = 0;               /* no rectangle fits: it stays a footprint (a mass for the tests after it) */
        }
        mu = sc_muni_find(c, set[i].id, set[i].b->cx, set[i].b->cz);       /* [roam NA] the municipal line */
        mu_on_line = mu && (mu->fate == TD5_MUNI_SNAPPED || mu->fate == TD5_MUNI_ON);
        if (mu && mu->fate == TD5_MUNI_SNAPPED && !(s3d_hide && !s3d_base)) {
            double r2 = 0.0, mx = 0.0, mz = 0.0;
            n = mu->n;
            for (k = 0; k < n; k++) {
                rx[k] = c->mpool[mu->first + k] - (double)c->org_x;
                rz[k] = c->mpool[mu->first + n + k] - (double)c->org_z;
                mx += rx[k]; mz += rz[k];
            }
            mx /= n; mz /= n;
            for (k = 0; k < n; k++) { const double d = hypot(rx[k] - mx, rz[k] - mz); if (d > r2) r2 = d; }
            if (r2 > gloc.radius) gloc.radius = r2;
        }
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
        u.gap = mu_on_line ? MU_SNAP_GAP : s->gap;
        r = sc_bld_clear(c, &u, mu_on_line ? MU_SNAP_GAP : s->gap, rx, rz, n, &shift,
                         !s->lm_wins ? 0 : (s->s3d && (gb->part || gb->rel)) ? 2
                                         : (gb->landmark || kind != TD5_GEOB_LMK_NONE) ? 1 : 0);
        if (gb->landmark && !gb->part && !ghost)   /* one line per landmark outline: what became of it */
            TD5_LOG_I(LOG_TAG, "[ROAM LM] tile %d,%d way_hash=%08x kind=%d anchor=%d host=%d half=%.0f minout=%.0f "
                      "area=%.0fm2 h=%.0f: %s (shift %.0f)", c->tx, c->tz, gb->id_hash, kind, anchor, u.host,
                      u.f.half_carriage, u.minout, gb->area_m2, gb->height,
                      r == 2 ? "DROPPED past the shift cap" : r == 1 ? "DROPPED on a road" : "kept", shift);
        if (r == 2) { c->st.bld_drop_shift++; sc_bld_note(s, c, &set[i], "past the shift cap"); continue; }
        if (r == 1) { c->st.bld_drop_road++; sc_bld_note(s, c, &set[i], "on a road"); continue; }
        if (shift > 0.0) { c->st.bld_nudged++; if (shift > c->st.bld_shift_max) c->st.bld_shift_max = shift; }
        if (ghost) {                                                          /* [roam PD] */
            if (!st_push_ring(c, rx, rz, n)) { free(set); free(fc); free(st_save); return 0; }
            continue;
        }
        cx = 0.0; cz = 0.0;
        for (k = 0; k < n; k++) { cx += rx[k]; cz += rz[k]; }
        cx /= n; cz /= n;
        if (gh && !gb->part && !gb->rel && !gb->landmark) {   /* [roam PC] a horticulture tunnel, not a building */
            const ScCache *rc = set[i].rc;
            const int ri = (int)(set[i].b - rc->b);
            const int osm_gh = rc->kf && (rc->kf[ri] & SC_KF_GH);
            const int in_area = c->nghp > 0 && sc_kf_in(c->ghp, c->nghp, set[i].b->cx, set[i].b->cz);
            const int r2 = (osm_gh || in_area)
                         ? td5_roam_gh_try(gh, c, rx, rz, n, gb->area_m2, gb->height / s->upm, osm_gh, in_area, &c->st.gh) : 0;
            if (r2 < 0) { td5_roam_gh_end(gh); free(set); free(fc); return 0; }
            if (r2 > 0) { c->st.bld_emitted++; sc_bld_note(s, c, &set[i], "greenhouse"); continue; }
        }
        sb = sc_sb_of(c, cx, cz);
        b = sc_sb(c, sb, 4);
        if (!b) { td5_roam_gh_end(gh); free(set); free(fc); return 0; }
        memset(&e, 0, sizeof e);
        e.blk = &b->blk; e.moff = b->moff; e.nmesh = &b->nmesh; e.maxmesh = b->cap;
        e.no_prefab = !td5_env_int("TD5RE_ROAM_LM_PREFAB", 0, 0, 1);   /* [IB] no shipped TD5 landmark in the real city: the OSM ring is extruded (=1 restores) */
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
        e.s3d = s->s3d;
        e.s3d_skip_mass = s3d_hide && !s3d_base;
        e.s3d_base = s3d_base;
        if (s3d_hide) c->st.bld_s3d_hidden++;
        if (set[i].rx && set[i].rx->nh > 0) {
            /* [JC2] the courtyards, in the tile frame, moved by the same clearance shift as the outline */
            const double sx = rx[0] - (set[i].px[0] - (double)c->org_x), sz = rz[0] - (set[i].pz[0] - (double)c->org_z);
            int h, off = 0;
            for (h = 0; h < set[i].rx->nh; h++) {
                const int hf = set[i].rx->hfirst[h];
                hn[nh] = set[i].rx->hn[h];
                for (k = 0; k < hn[nh]; k++) {
                    hx[off + k] = set[i].rc->px[hf + k] - (double)c->org_x + sx;
                    hz[off + k] = set[i].rc->pz[hf + k] - (double)c->org_z + sz;
                }
                off += hn[nh];
                nh++;
            }
        }
        e.s3d_nh = nh; e.s3d_hn = hn; e.s3d_hx = hx; e.s3d_hz = hz;
        np = 0;
        if (s->s3d && (gb->part || gb->rel)) {
            /* [JC2] the service ways near the outline, for the carriage arches */
            int q;
            for (q = 0; q < c->nseg && np < 128; q++) {
                const ScSeg *g = &c->seg[q];
                double d;
                if (c->edge[g->edge].klass != TD5_GEO_RC_SERVICE) continue;
                d = sc_seg_dist(cx, cz, g->ax, g->az, g->bx, g->bz, NULL);
                if (d > gb->radius + g->hw) continue;
                pseg[np * 5 + 0] = g->ax; pseg[np * 5 + 1] = g->az; pseg[np * 5 + 2] = g->bx; pseg[np * 5 + 3] = g->bz;
                pseg[np * 5 + 4] = g->hw;
                np++;
            }
        }
        e.s3d_np = np; e.s3d_pseg = pseg;
        {
            const long emitted0 = c->bst.emitted, prefab0 = c->bst.lm_prefab, apron0 = c->bst.lm_apron;
            long how0[6];
            const long s3d0 = c->bst.s3d_n, shaped0 = c->bst.roof_shaped;
            memcpy(how0, c->bst.s3d_how, sizeof how0);
            if (!tg_geo_emit_building_core(&e, gb, rx, rz, n, cx, cz, shift) || b->blk.oom) {
                td5_roam_gh_end(gh); free(set); free(fc); return 0;
            }
            if (gb->part || gb->roof3d != TD5_GEOB_R3_NONE || gb->roof_only) {
                /* [JC] every footprint OSM models in 3D, by either builder: what its roof became */
                int q, how = -1;
                for (q = 0; q < 6; q++) if (c->bst.s3d_how[q] > how0[q]) how = q;
                if (c->bst.emitted > emitted0) c->st.s3d_tagged++;
                if (c->bst.roof_shaped > shaped0) c->st.s3d_sloped++;
                if (s->bld_log)
                    TD5_LOG_I(LOG_TAG, "ROAM_S3D tile %d,%d id %.0f part %d roof3d %d roof_only %d n %d h %.1f rh %.1f: %s how %d",
                              c->tx, c->tz, set[i].id, (int)gb->part, (int)gb->roof3d, (int)gb->roof_only, n,
                              gb->height / SC_UPM, gb->roof_height / SC_UPM,
                              c->bst.s3d_n > s3d0 ? "s3d" : c->bst.emitted > emitted0 ? "core" : "not built", how);
            }
            if (c->bst.emitted > emitted0) c->st.bld_emitted++;
            if (c->bst.emitted > emitted0 && !st_push_ring(c, rx, rz, n)) { free(set); free(fc); return 0; }   /* [roam PD] */
            if (c->bst.emitted > emitted0 && !e.s3d_skip_mass && !s3d_base) {   /* [roam NA] */
                if (mu && mu->fate < TD5_MUNI__N) c->st.muni_fate[mu->fate]++;
                if (mu && mu->fate == TD5_MUNI_SNAPPED) {
                    c->st.muni_fwd += mu->fwd; c->st.muni_back += mu->back;
                    c->st.muni_corner += mu->corners; c->st.muni_ochava += mu->ochavas;
                }
                else if (mu && mu->fate > 16 && mu->fate < 21) c->st.muni_skip[mu->fate - 17]++;
                sc_muni_census(s, c, rx, rz, n);
                sc_ring_dump(s, c, set[i].id, rx, rz, n);
                if (c->mcut_band.n > 0) {                                  /* [roam PE] footprints over a cutting */
                    const int snapped = mu && mu->fate == TD5_MUNI_SNAPPED;
                    if (sc_ring_over_cut(&c->mcut_band, c, rx, rz, n)) { c->st.muni_cut_over[0]++; c->st.muni_cut_over[2] += snapped; }
                    if (sc_ring_over_cut(&c->mcut_zone, c, rx, rz, n)) { c->st.muni_cut_over[1]++; c->st.muni_cut_over[3] += snapped; }
                }
            }
            if (su.pe && c->bst.emitted > emitted0) {   /* [roam PE] the ring as built */
                if (!sc_grow((void **)&su.fin, &su.capfin, su.nfin + 2 * n, sizeof(double))) { sc_shopu_free(&su); free(set); free(fc); return 0; }
                su.finoff[i] = su.nfin;
                for (k = 0; k < n; k++) {
                    su.fin[su.nfin + k] = rx[k] + (double)c->org_x;
                    su.fin[su.nfin + n + k] = rz[k] + (double)c->org_z;
                }
                su.nfin += 2 * n;
                su.en[i] = n;
                su.gone[i] = 0;
            }
            if (s->shops && su.pe && c->bst.emitted > emitted0 && !e.s3d_skip_mass && !s3d_base) {
                /* [roam PE] deferred: dressed after the pavement + infill (sc_shops_deferred) */
                ScShopDefer *d = (ScShopDefer *)c->shd;
                ScShopItem *it;
                if (!d && !(d = (ScShopDefer *)calloc(1, sizeof *d))) { sc_shopu_free(&su); free(set); free(fc); return 0; }
                c->shd = d;
                if (!sc_grow((void **)&d->it, &d->cap, d->nit + 1, sizeof(ScShopItem))) { sc_shopu_free(&su); free(set); free(fc); return 0; }
                it = &d->it[d->nit++];
                it->i = i; it->n = n; it->sb = sb; it->g = *gb;
                it->by = sc_ground_u(&u, cx, cz) + tg_city_kerb_h(e.b);
                if (e.have_ceiling && it->by > e.ceiling_y) it->by = e.ceiling_y;
                it->kerb_h = tg_city_kerb_h(e.b);
                memcpy(it->rx, rx, (size_t)n * sizeof(double));
                memcpy(it->rz, rz, (size_t)n * sizeof(double));
            } else if (s->shops && c->bst.emitted > emitted0 && !e.s3d_skip_mass && !s3d_base) {
                /* [roam LB] the ground floor of a footprint with a business: the core's own base y */
                TD5_RoamShopBld sb_;
                double by = sc_ground_u(&u, cx, cz) + tg_city_kerb_h(e.b);
                if (e.have_ceiling && by > e.ceiling_y) by = e.ceiling_y;
                memset(&sb_, 0, sizeof sb_);
                sb_.id = set[i].id; sb_.gb = gb; sb_.rx = rx; sb_.rz = rz; sb_.n = n;
                sb_.by = by; sb_.kerb_h = tg_city_kerb_h(e.b); sb_.sb = sb;
                sb_.in_bld = sc_shop_in_bld; sb_.u = &su;
                su.self = set[i].px;                                             /* [roam PE] not its own neighbour */
                int xs[TD5_ROAM_SCN_NSB];
                sc_xc_snap(c, xs);
                if (!td5_roam_shops_building(s->shops, c, &sb_, &c->st.shop)) { td5_roam_gh_end(gh); free(set); free(fc); return 0; }
                sc_xc_note(c, xs, SC_XC_SHOP);                                   /* [integ L] */
                su.self = NULL;
            }
            sc_bld_note(s, c, &set[i], c->bst.emitted > emitted0 ? "emitted" : "core refused");
            if (c->bst.lm_prefab > prefab0) c->st.bld_prefab++;
            if (c->bst.lm_apron > apron0) c->st.bld_apron++;
            if (kind != TD5_GEOB_LMK_NONE || gb->landmark) c->st.bld_lm++;
        }
    }
    if (ghost_prev) c->st = *st_save;          /* [roam PD] restored before the passes after the loop count (integ P) */
    free(st_save);
    if (gh) {                                         /* [roam PC] */
        int xs[TD5_ROAM_SCN_NSB];
        int ok;
        sc_xc_snap(c, xs);
        ok = td5_roam_gh_flush(gh, c, &c->st.gh);
        sc_xc_note(c, xs, SC_XC_GH);                  /* [integ P] cross-check range */
        td5_roam_gh_end(gh);
        if (!ok) { free(set); free(fc); return 0; }
    }
    if (su.pe)                                   /* [roam PE] the built rings (the pool no longer moves) */
        for (i = 0; i < nset; i++)
            if (su.finoff[i] >= 0) {
                const TD5_GeoBuilding *gb = set[i].b;
                su.epx[i] = &su.fin[su.finoff[i]]; su.epz[i] = &su.fin[su.finoff[i] + su.en[i]];
                for (k = 0; k < su.en[i]; k++) {
                    const double d = hypot(su.epx[i][k] - gb->cx, su.epz[i][k] - gb->cz);
                    if (d > su.er[i]) su.er[i] = d;
                }
            }
    /* [roam LB] fuel forecourt canopies, kept off every footprint of the halo set */
    if (s->shops && !td5_roam_shops_fuel(s->shops, c, sc_shop_in_bld, &su, sc_fuel_site_fp, &c->st.shop)) {
        sc_shopu_free(&su); free(set); free(fc); return 0;
    }
    if (su.pe && c->shd) {                       /* [roam PE] kept for sc_shops_deferred */
        ScShopDefer *d = (ScShopDefer *)c->shd;
        d->set = set; d->su = su;
        d->su.set = set;
        set = NULL;
    } else sc_shopu_free(&su);
    free(set);
    free(fc);
    {   /* [JC] what the Simple 3D Buildings builder did on this tile (c->bst counts the whole tile) */
        int q;
        c->st.bld_s3d = (int)c->bst.s3d_n;
        for (q = 0; q < 6; q++) c->st.s3d_how[q] = (int)c->bst.s3d_how[q];
        c->st.s3d_roof_only = (int)c->bst.s3d_roof_only;
        c->st.s3d_tint = (int)(c->bst.s3d_tint_roof + c->bst.s3d_tint_wall);
        c->st.s3d_holes = (int)c->bst.s3d_holes;
        c->st.s3d_cap_holed = (int)c->bst.s3d_cap_holed;
        c->st.s3d_cap_lidded = (int)c->bst.s3d_cap_lidded;
        c->st.s3d_civic = (int)c->bst.s3d_civic;
        c->st.s3d_base = (int)c->bst.s3d_base;
        c->st.s3d_rel_built = (int)c->bst.s3d_rel;
        c->st.s3d_portals = (int)c->bst.s3d_portals;
    }
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


/* [roam QB] LAWN CLIP. Mariano, 2026-10-23: "grass texture leaves a dent around the road". A lawn piece touching the road
 * was refined twice and every sub-triangle still on the road dropped (sc_tri_split): the drawn ground showed in a saw-tooth
 * strip up to ~2 m wide between the lawn's edge and the kerb, drop + lift (54 cm) under the lawn, a trench along every
 * park road. Now the piece loses only what lies on the road: each drivable primitive over it is cut away along its own
 * edges (convex difference, SC_LCLIP_MARGIN outside them so the guard census still reads 0), the rest fanned into
 * triangles in the piece's own plane (one drawn-ground cell triangle: still parallel to the ground). A piece the cut
 * cannot hold (too many pieces) keeps the old refinement. TD5RE_ROAM_LAWN_CLIP=0 = the old lawn. */
#define SC_LCLIP_PTS     24
#define SC_LCLIP_PIECES  48
#define SC_LCLIP_PRIMS   64
#define SC_LCLIP_MARGIN  (0.03 * SC_UPM)

typedef struct { double x[SC_LCLIP_PTS], z[SC_LCLIP_PTS]; int n; } ScLcPoly;
typedef struct { const ScPrim *p[SC_LCLIP_PRIMS]; int n, skip_lm, over; } ScLcPrims;

static int sc_lc_collect(void *u, const ScPrim *p)
{
    ScLcPrims *q = (ScLcPrims *)u;
    if (q->skip_lm && p->lm_ok) return 0;
    if (q->n < SC_LCLIP_PRIMS) q->p[q->n++] = p; else q->over = 1;
    return 0;
}

/* a point this close to a cut line is ON it: the side test of a near-collinear vertex is the last bits of the
 * arithmetic, and those moved between the main thread and a worker (determinism arm T: an extra vertex) */
#define SC_LC_SNAP 1e-3

static double sc_lc_dist(double ax, double az, double bx, double bz, double l, double sgn, double k, double px, double pz)
{
    const double d = sgn * sc_cross(ax, az, bx, bz, px, pz) / l - k;
    return fabs(d) < SC_LC_SNAP ? 0.0 : d;
}

/* the part of `in` where sgn * (signed distance to the line a->b, + = left) >= k */
static int sc_lc_half(const ScLcPoly *in, double ax, double az, double bx, double bz, double sgn, double k, ScLcPoly *out)
{
    const double l = sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));   /* not hypot: a CRT call, its rounding is the thread's */
    int i, m = 0;
    if (l < 1e-9) { *out = *in; return out->n; }
    for (i = 0; i < in->n; i++) {
        const int j = (i + 1) % in->n;
        const double di = sc_lc_dist(ax, az, bx, bz, l, sgn, k, in->x[i], in->z[i]);
        const double dj = sc_lc_dist(ax, az, bx, bz, l, sgn, k, in->x[j], in->z[j]);
        if (di >= 0.0 && m < SC_LCLIP_PTS) { out->x[m] = in->x[i]; out->z[m] = in->z[i]; m++; }
        if (((di > 0.0 && dj < 0.0) || (di < 0.0 && dj > 0.0)) && m < SC_LCLIP_PTS) {   /* a point ON the line is no crossing */
            const double t = di / (di - dj);
            out->x[m] = in->x[i] + t * (in->x[j] - in->x[i]); out->z[m] = in->z[i] + t * (in->z[j] - in->z[i]); m++;
        }
    }
    out->n = m;
    return m;
}

static double sc_lc_area(const ScLcPoly *q)
{
    double a = 0.0;
    int i;
    for (i = 0; i < q->n; i++) { const int j = (i + 1) % q->n; a += q->x[i] * q->z[j] - q->x[j] * q->z[i]; }
    return 0.5 * fabs(a);
}

/* triangle p minus every drivable primitive over it, `margin` outside their edges, into piece list A (its count).
 * *border = the length of the cut edges (the lawn's edge along the road). -1 = too many pieces / primitives */
static int sc_lc_cut(TG_TileCtx *c, const double *p, double margin, ScLcPoly *A, double *border)
{
    static const int cap = SC_LCLIP_PIECES;
    ScLcPrims q;
    ScLcPoly B[SC_LCLIP_PIECES];
    double x0 = p[0], x1 = p[0], z0 = p[1], z1 = p[1];
    int na = 1, k, i, e;
    for (k = 1; k < 3; k++) {
        if (p[k * 2] < x0) x0 = p[k * 2];
        if (p[k * 2] > x1) x1 = p[k * 2];
        if (p[k * 2 + 1] < z0) z0 = p[k * 2 + 1];
        if (p[k * 2 + 1] > z1) z1 = p[k * 2 + 1];
    }
    memset(&q, 0, sizeof q);
    q.skip_lm = c->skip_lm;
    sc_prims_in(c, x0 - margin, z0 - margin, x1 + margin, z1 + margin, sc_lc_collect, &q);
    if (q.over) return -1;
    for (k = 0; k < 3; k++) { A[0].x[k] = p[k * 2]; A[0].z[k] = p[k * 2 + 1]; }
    A[0].n = 3;
    if (border) *border = 0.0;
    for (i = 0; i < q.n; i++) {
        const ScPrim *pr = q.p[i];
        double ar = 0.0, sgn;
        int nb = 0, a;
        if (pr->n < 3) continue;
        for (e = 0; e < pr->n; e++) { const int f = (e + 1) % pr->n; ar += pr->x[e] * pr->z[f] - pr->x[f] * pr->z[e]; }
        if (fabs(ar) < 1e-6) continue;
        sgn = ar > 0.0 ? 1.0 : -1.0;          /* inside = sgn * cross > 0 for every edge */
        for (a = 0; a < na; a++) {
            ScLcPoly rem = A[a], t;
            double bx0 = 1e300, bx1 = -1e300, bz0 = 1e300, bz1 = -1e300;
            for (k = 0; k < rem.n; k++) {
                if (rem.x[k] < bx0) bx0 = rem.x[k];
                if (rem.x[k] > bx1) bx1 = rem.x[k];
                if (rem.z[k] < bz0) bz0 = rem.z[k];
                if (rem.z[k] > bz1) bz1 = rem.z[k];
            }
            if (bx1 < pr->x0 - margin || bx0 > pr->x1 + margin || bz1 < pr->z0 - margin || bz0 > pr->z1 + margin) {
                if (nb >= cap) return -1;
                B[nb++] = rem;
                continue;
            }
            for (e = 0; e < pr->n && rem.n >= 3; e++) {
                const int f = (e + 1) % pr->n;
                /* outside this edge (beyond the margin): kept; the rest goes on to the next edge */
                if (sc_lc_half(&rem, pr->x[e], pr->z[e], pr->x[f], pr->z[f], -sgn, margin, &t) >= 3 && sc_lc_area(&t) > 1.0) {
                    if (nb >= cap) return -1;
                    B[nb++] = t;
                }
                sc_lc_half(&rem, pr->x[e], pr->z[e], pr->x[f], pr->z[f], sgn, -margin, &t);
                rem = t;
            }
            if (border && rem.n >= 3) {   /* what fell inside the primitive: its edges on the primitive's outline */
                for (k = 0; k < rem.n; k++) {
                    const int j = (k + 1) % rem.n;
                    const double mx = 0.5 * (rem.x[k] + rem.x[j]), mz = 0.5 * (rem.z[k] + rem.z[j]);
                    int on = -1;
                    for (e = 0; e < pr->n && on < 0; e++) {
                        const int f = (e + 1) % pr->n;
                        const double l = hypot(pr->x[f] - pr->x[e], pr->z[f] - pr->z[e]);
                        if (l > 1e-9 && fabs(sc_cross(pr->x[e], pr->z[e], pr->x[f], pr->z[f], mx, mz)) / l < margin + 1.0) on = e;
                    }
                    if (on >= 0) {   /* a road EDGE only when the ground beyond it is not road too (lane quads share edges) */
                        const int f = (on + 1) % pr->n;
                        const double l = hypot(pr->x[f] - pr->x[on], pr->z[f] - pr->z[on]);
                        const double ox = -sgn * -(pr->z[f] - pr->z[on]) / l, oz = -sgn * (pr->x[f] - pr->x[on]) / l;
                        const double tx = mx + ox * 0.2 * SC_UPM, tz = mz + oz * 0.2 * SC_UPM;
                        if (!sc_on_road(c, &tx, &tz, 1, NULL)) *border += hypot(rem.x[j] - rem.x[k], rem.z[j] - rem.z[k]);
                    }
                }
            }
        }
        memcpy(A, B, (size_t)nb * sizeof B[0]);
        na = nb;
        if (na == 0) break;
    }
    return na;
}

static int sc_tri_push(ScTris *t, const double *p);
/* 1 = done (pieces pushed), 0 = out of memory, -1 = not handled (the old refinement) */
static int sc_lawn_clip(TG_TileCtx *c, ScTris *out, const double *p)
{
    ScLcPoly A[SC_LCLIP_PIECES], F[SC_LCLIP_PIECES];
    double border = 0.0, free_m2 = 0.0, drawn_m2 = 0.0;
    const int na = sc_lc_cut(c, p, SC_LCLIP_MARGIN, A, NULL);
    const int nf = sc_lc_cut(c, p, 0.0, F, &border);         /* the census: the exact free part, the road edge in it */
    int a, k;
    if (nf > 0) for (a = 0; a < nf; a++) free_m2 += sc_lc_area(&F[a]);
    if (na < 0 || nf < 0) { c->st.lawn_clip_fallback++; return -1; }
    for (a = 0; a < na; a++)
        for (k = 1; k + 1 < A[a].n; k++) {
            const double t6[6] = { A[a].x[0], A[a].z[0], A[a].x[k], A[a].z[k], A[a].x[k + 1], A[a].z[k + 1] };
            const double xf[3] = { (double)(float)t6[0], (double)(float)t6[2], (double)(float)t6[4] };
            const double zf[3] = { (double)(float)t6[1], (double)(float)t6[3], (double)(float)t6[5] };
            const int dup = (fabs(xf[0] - xf[1]) < 1.0 && fabs(zf[0] - zf[1]) < 1.0) || (fabs(xf[1] - xf[2]) < 1.0 && fabs(zf[1] - zf[2]) < 1.0) ||
                            (fabs(xf[0] - xf[2]) < 1.0 && fabs(zf[0] - zf[2]) < 1.0);
            if (dup || fabs(td5_geob_ring_area(xf, zf, 3)) < 4.0) continue;
            if (sc_on_road(c, xf, zf, 3, NULL)) { c->st.lawn_clip_drop++; continue; }
            if (!sc_tri_push(out, t6)) return 0;
            drawn_m2 += fabs(td5_geob_ring_area(xf, zf, 3));
            c->st.lawn_clip++;
        }
    c->st.lawn_border_m += border / SC_UPM;
    if (free_m2 > drawn_m2) c->st.lawn_gap_m2 += (free_m2 - drawn_m2) / (SC_UPM * SC_UPM);
    return 1;
}

/* the old path's census twin: what the refinement keeps of a piece on the road, against its exact free part */
static void sc_lawn_gap_old(TG_TileCtx *c, const double *p, double kept_m2)
{
    ScLcPoly F[SC_LCLIP_PIECES];
    double border = 0.0, free_m2 = 0.0;
    const int nf = sc_lc_cut(c, p, 0.0, F, &border);
    int a;
    if (nf < 0) return;
    for (a = 0; a < nf; a++) free_m2 += sc_lc_area(&F[a]);
    c->st.lawn_border_m += border / SC_UPM;
    if (free_m2 > kept_m2) c->st.lawn_gap_m2 += (free_m2 - kept_m2) / (SC_UPM * SC_UPM);
}

/* [roam QB] DOUBLE GRASS. Mariano, 2026-10-23: "there's still double grass". Census (roam grass overlap, the Estadio
 * Unico block): 44,555 m2 of the 328,500 m2 of grass lay under two grass faces, 16,281 m2 of them two PLAZA LAWNS at the
 * same height (a grass or pitch area inside a park, two parks over each other: OSM maps them overlapping and each owner
 * laid its whole outline). The two lawns are coplanar but their UVs were tile-local (x * 1/3400 from each OWNER's
 * origin), so their textures were offset and z-fought, a second grass showing through the first in patches. Now:
 *   - a lawn leaves out the part of its outline another plaza lawn covers there (the larger one keeps it, by area, then
 *     id), cut exactly along that lawn's triangles: only where that lawn really draws (it is not a painted pitch or a
 *     stadium ground, and the spot is in its owner's 3x3, its reach);
 *   - lawn UVs are in the place frame, so whatever still overlaps, and two lawns meeting, show the same texels.
 * TD5RE_ROAM_LAWN_DEDUP=0 = the old lawns. */
#define SC_LCOV_TRI ((TD5_GEOB_RING_MAX - 2) * 3)
typedef struct ScLawnCover {
    double x[TD5_GEOB_RING_MAX], z[TD5_GEOB_RING_MAX];   /* tile-local outline */
    int    n, ntri, tri[SC_LCOV_TRI];
    double x0, z0, x1, z1, m2, id;
    int    otx, otz;                                     /* owner tile: the lawn draws in its 3x3 */
} ScLawnCover;

/* 1 when cover b keeps the ground over cover a (larger first, then the smaller id) */
static int sc_lcov_wins(const ScLawnCover *b, const ScLawnCover *a)
{
    if (b->m2 != a->m2) return b->m2 > a->m2;
    return b->id < a->id;
}

/* triangle p (x z x z x z) minus every winning cover over it, into A (count), -1 = too many pieces */
static int sc_lcov_cut(TG_TileCtx *c, const double *p, ScLcPoly *A)
{
    ScLcPoly B[SC_LCLIP_PIECES];
    const ScLawnCover *self = &c->lcov[c->lcov_self];
    double x0 = p[0], x1 = p[0], z0 = p[1], z1 = p[1], mx, mz;
    int na = 1, k, i, t, tx, tz;
    for (k = 1; k < 3; k++) {
        if (p[k * 2] < x0) x0 = p[k * 2];
        if (p[k * 2] > x1) x1 = p[k * 2];
        if (p[k * 2 + 1] < z0) z0 = p[k * 2 + 1];
        if (p[k * 2 + 1] > z1) z1 = p[k * 2 + 1];
    }
    for (k = 0; k < 3; k++) { A[0].x[k] = p[k * 2]; A[0].z[k] = p[k * 2 + 1]; }
    A[0].n = 3;
    mx = (p[0] + p[2] + p[4]) / 3.0; mz = (p[1] + p[3] + p[5]) / 3.0;
    roam_grid_tile_of(c->grid, mx + (double)c->org_x, mz + (double)c->org_z, &tx, &tz);   /* a cell piece: one tile */
    for (i = 0; i < c->nlcov && na > 0; i++) {
        const ScLawnCover *cv = &c->lcov[i];
        if (i == c->lcov_self || !sc_lcov_wins(cv, self)) continue;
        if (cv->x1 < x0 || cv->x0 > x1 || cv->z1 < z0 || cv->z0 > z1) continue;
        if (tx < cv->otx - 1 || tx > cv->otx + 1 || tz < cv->otz - 1 || tz > cv->otz + 1) continue;   /* it does not draw here */
        for (t = 0; t < cv->ntri && na > 0; t++) {
            const double tx3[3] = { cv->x[cv->tri[t * 3]], cv->x[cv->tri[t * 3 + 1]], cv->x[cv->tri[t * 3 + 2]] };
            const double tz3[3] = { cv->z[cv->tri[t * 3]], cv->z[cv->tri[t * 3 + 1]], cv->z[cv->tri[t * 3 + 2]] };
            double ar, sgn, bx0 = tx3[0], bx1 = tx3[0], bz0 = tz3[0], bz1 = tz3[0];
            int nb = 0, a, e;
            for (k = 1; k < 3; k++) {
                if (tx3[k] < bx0) bx0 = tx3[k];
                if (tx3[k] > bx1) bx1 = tx3[k];
                if (tz3[k] < bz0) bz0 = tz3[k];
                if (tz3[k] > bz1) bz1 = tz3[k];
            }
            if (bx1 < x0 || bx0 > x1 || bz1 < z0 || bz0 > z1) continue;
            ar = sc_cross(tx3[0], tz3[0], tx3[1], tz3[1], tx3[2], tz3[2]);
            if (fabs(ar) < 1e-6) continue;
            sgn = ar > 0.0 ? 1.0 : -1.0;
            for (a = 0; a < na; a++) {
                ScLcPoly rem = A[a], q;
                for (e = 0; e < 3 && rem.n >= 3; e++) {
                    const int f = (e + 1) % 3;
                    /* outside this edge: kept, overlapping the cover by (0.02 * SC_UPM) so no hairline opens between them */
                    if (sc_lc_half(&rem, tx3[e], tz3[e], tx3[f], tz3[f], -sgn, -(0.02 * SC_UPM), &q) >= 3 && sc_lc_area(&q) > 1.0) {
                        if (nb >= SC_LCLIP_PIECES) return -1;
                        B[nb++] = q;
                    }
                    sc_lc_half(&rem, tx3[e], tz3[e], tx3[f], tz3[f], sgn, (0.02 * SC_UPM), &q);
                    rem = q;
                }
            }
            memcpy(A, B, (size_t)nb * sizeof B[0]);
            na = nb;
        }
    }
    return na;
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
        if (c->lawn_dg) {
            /* [roam IA] a cell piece is judged as the guard census reads the bytes: float corners, a corner within
             * 1 unit of another merged, under 4 units2 a segment (sc_assemble) */
            const double xf[3] = { (double)(float)x[0], (double)(float)x[1], (double)(float)x[2] };
            const double zf[3] = { (double)(float)z[0], (double)(float)z[1], (double)(float)z[2] };
            const int dup = (fabs(xf[0] - xf[1]) < 1.0 && fabs(zf[0] - zf[1]) < 1.0) || (fabs(xf[1] - xf[2]) < 1.0 && fabs(zf[1] - zf[2]) < 1.0) ||
                            (fabs(xf[0] - xf[2]) < 1.0 && fabs(zf[0] - zf[2]) < 1.0);
            if (dup || fabs(td5_geob_ring_area(xf, zf, 3)) < 4.0) { c->st.lawn_cut++; return 1; }
            if (!sc_on_road(c, xf, zf, 3, NULL)) return sc_tri_push(out, p);
            if (c->lawn_clip && extra == 2) {                          /* [roam QB] the cell piece, cut exactly */
                const int r = sc_lawn_clip(c, out, p);
                if (r >= 0) return r;
            }
            if (c->lawn_gap_cen && extra == 2) {                       /* [roam QB] the old path, measured */
                const int n0 = out->n;
                int r, t;
                double kept = 0.0;
                c->lawn_gap_cen = 0;
                r = sc_tri_split(c, out, p, 1, 1);
                c->lawn_gap_cen = 1;
                for (t = n0; t < out->n; t++) {
                    const double qx[3] = { out->x[t * 3], out->x[t * 3 + 1], out->x[t * 3 + 2] };
                    const double qz[3] = { out->z[t * 3], out->z[t * 3 + 1], out->z[t * 3 + 2] };
                    kept += fabs(td5_geob_ring_area(qx, qz, 3));
                }
                sc_lawn_gap_old(c, p, kept);
                return r;
            }
        } else
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

/* [roam IA] LAWNS ON THE DRAWN GROUND. Mariano, 2026-10-20: "hay pasto debajo del pasto?" (is there grass under the
 * grass?). A plaza lawn stood SC_GEOP_LIFT over the RASTER at its own vertices (18 m triangles) while the drawn ground
 * under it is a 16 m lattice of planar cells: inside a lawn triangle the ground's kinks rose through it and the
 * ground's lighter grass showed in slivers (lawn census: 1.7 % of the casco's 2.7 km2 of plaza lawn, up to 2.6 m). Now
 * every lawn triangle is cut along the render's ground cells and their diagonal (rr_bake splits each cell 0 1 2 /
 * 0 2 3) and each piece takes the drawn ground's own plane + the ground drop + SC_GEOP_LIFT: parallel to the ground
 * everywhere, so the ground never shows through, and the same height as before at every lattice point. The lawn
 * planes the footpaths read are the same pieces. TD5RE_ROAM_LAWN_ON_GROUND=0 = the old lawn. */

/* the drawn ground at a tile-local point (render frame height, before the ground drop is subtracted: + c->dg_drop
 * gives the raster - lowering at the lattice points) */
static double sc_dg(TG_TileCtx *c, double x, double z)
{
    const double S = (double)c->grid->tile, G = (double)c->dg_G;
    static const int di[4] = { 0, 1, 1, 0 }, dj[4] = { 0, 0, 1, 1 };
    double u, v, fu, fv, a, b, h[4];
    int i, j, k;
    roam_grid_uv(c->grid, x + (double)c->org_x, z + (double)c->org_z, &u, &v);
    fu = u / S * G; fv = v / S * G;
    i = (int)floor(fu); j = (int)floor(fv);
    a = fu - i; b = fv - j;
    for (k = 0; k < 4; k++) {
        const double uu = (double)(i + di[k]) * S / G, vv = (double)(j + dj[k]) * S / G;
        const double X = c->grid->ox + c->grid->c * uu - c->grid->s * vv, Z = c->grid->oz + c->grid->s * uu + c->grid->c * vv;
        h[k] = sc_ground(c, X - (double)c->org_x, Z - (double)c->org_z) - c->dg_drop;
    }
    if (b <= a) return h[0] + a * (h[1] - h[0]) + b * (h[2] - h[1]);
    return h[0] + b * (h[3] - h[0]) + a * (h[2] - h[3]);
}

/* the lawn surface over the drawn ground at a tile-local point */
static double sc_lawn_h(TG_TileCtx *c, double x, double z)
{
    return c->lawn_dg ? sc_dg(c, x, z) + c->dg_drop + SC_GEOP_LIFT : sc_ground(c, x, z) + SC_GEOP_LIFT;
}

/* polygon (fu, fv) clipped to the half-plane sgn * (k - (fu, fv) . (ca, cb)) >= 0 */
static int sc_clip_hp(const double *iu, const double *iv, int n, double ca, double cb, double k, double sgn, double *ou, double *ov)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double di = sgn * (k - (iu[i] * ca + iv[i] * cb)), dj = sgn * (k - (iu[j] * ca + iv[j] * cb));
        if (di >= 0.0) { ou[m] = iu[i]; ov[m] = iv[i]; m++; }
        if ((di >= 0.0) != (dj >= 0.0)) {
            const double t = di / (di - dj);
            ou[m] = iu[i] + t * (iu[j] - iu[i]); ov[m] = iv[i] + t * (iv[j] - iv[i]); m++;
        }
    }
    return m;
}

/* triangle p (x z x z x z, tile-local) cut along the ground cells and their diagonal; emit() gets every piece */
static int sc_lawn_cells(TG_TileCtx *c, const double *p, int (*emit)(TG_TileCtx *, const double *, void *), void *ud)
{
    const double S = (double)c->grid->tile, G = (double)c->dg_G;
    double fu[3], fv[3], u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
    int k, i, j;
    for (k = 0; k < 3; k++) {
        double u, v;
        roam_grid_uv(c->grid, p[k * 2] + (double)c->org_x, p[k * 2 + 1] + (double)c->org_z, &u, &v);
        fu[k] = u / S * G; fv[k] = v / S * G;
        if (fu[k] < u0) u0 = fu[k];
        if (fu[k] > u1) u1 = fu[k];
        if (fv[k] < v0) v0 = fv[k];
        if (fv[k] > v1) v1 = fv[k];
    }
    for (j = (int)floor(v0); j <= (int)floor(v1); j++)
        for (i = (int)floor(u0); i <= (int)floor(u1); i++) {
            double au[16], av[16], bu[16], bv[16];
            int n = 3, half;
            memcpy(au, fu, sizeof fu); memcpy(av, fv, sizeof fv);
            n = sc_clip_hp(au, av, n, 1.0, 0.0, (double)i, -1.0, bu, bv);         /* fu >= i     */
            n = sc_clip_hp(bu, bv, n, 1.0, 0.0, (double)(i + 1), 1.0, au, av);    /* fu <= i + 1 */
            n = sc_clip_hp(au, av, n, 0.0, 1.0, (double)j, -1.0, bu, bv);         /* fv >= j     */
            n = sc_clip_hp(bu, bv, n, 0.0, 1.0, (double)(j + 1), 1.0, au, av);    /* fv <= j + 1 */
            if (n < 3) continue;
            for (half = 0; half < 2; half++) {
                /* (fv - j) - (fu - i) <= 0: the cell's (0 1 2) triangle, >= 0: its (0 2 3) */
                double cu[16], cv[16];
                const int m = sc_clip_hp(au, av, n, -1.0, 1.0, (double)(j - i), half ? -1.0 : 1.0, cu, cv);
                int q;
                for (q = 1; q + 1 < m; q++) {
                    double t6[6];
                    const int id[3] = { 0, q, q + 1 };
                    int r;
                    for (r = 0; r < 3; r++) {
                        const double uu = cu[id[r]] * S / G, vv = cv[id[r]] * S / G;
                        t6[r * 2] = c->grid->ox + c->grid->c * uu - c->grid->s * vv - (double)c->org_x;
                        t6[r * 2 + 1] = c->grid->oz + c->grid->s * uu + c->grid->c * vv - (double)c->org_z;
                    }
                    if (fabs(sc_cross(t6[0], t6[1], t6[2], t6[3], t6[4], t6[5])) < 1.0) continue;   /* a sliver */
                    if (!emit(c, t6, ud)) return 0;
                }
            }
        }
    return 1;
}

static int sc_lot_shore(TG_TileCtx *c, const double *cell);   /* [roam PC] SECTION lots */
static int sc_lawn_emit_drawn(TG_TileCtx *c, const double *t6, void *ud)
{
    if (c->shore_on) {   /* [roam PC] a piece in a shore cell is left to the render's bank (its ground is the lawn page) */
        const double S = (double)c->grid->tile, G = (double)c->dg_G;
        double u, v, cell[12];
        int i, j, k;
        roam_grid_uv(c->grid, (t6[0] + t6[2] + t6[4]) / 3.0 + (double)c->org_x, (t6[1] + t6[3] + t6[5]) / 3.0 + (double)c->org_z, &u, &v);
        i = (int)floor(u / S * G); j = (int)floor(v / S * G);
        for (k = 0; k < 4; k++) {
            const double uu = (i + ((k == 1 || k == 2) ? 1 : 0)) * S / G, vv = (j + (k >= 2 ? 1 : 0)) * S / G;
            cell[k * 3 + 0] = c->grid->ox + c->grid->c * uu - c->grid->s * vv - (double)c->org_x;
            cell[k * 3 + 1] = 0.0;
            cell[k * 3 + 2] = c->grid->oz + c->grid->s * uu + c->grid->c * vv - (double)c->org_z;
        }
        if (sc_lot_shore(c, cell)) { c->st.lawn_shore++; return 1; }
    }
    if (c->lawn_dedup && c->lcov && c->lcov_self >= 0) {   /* [roam QB] the part another plaza lawn draws is left to it */
        ScLcPoly A[SC_LCLIP_PIECES];
        const int na = sc_lcov_cut(c, t6, A);
        double in_m2 = fabs(sc_cross(t6[0], t6[1], t6[2], t6[3], t6[4], t6[5])) * 0.5, out_m2 = 0.0;
        int a, k;
        if (na < 0) { c->st.lawn_dedup_fallback++; return sc_tri_split(c, (ScTris *)ud, t6, 0, 2); }
        if (na == 1 && A[0].n == 3 && fabs(sc_lc_area(&A[0]) - in_m2) < 1.0) return sc_tri_split(c, (ScTris *)ud, t6, 0, 2);
        for (a = 0; a < na; a++) {
            out_m2 += sc_lc_area(&A[a]);
            for (k = 1; k + 1 < A[a].n; k++) {
                const double q6[6] = { A[a].x[0], A[a].z[0], A[a].x[k], A[a].z[k], A[a].x[k + 1], A[a].z[k + 1] };
                if (fabs(sc_cross(q6[0], q6[1], q6[2], q6[3], q6[4], q6[5])) < 1.0) continue;
                if (!sc_tri_split(c, (ScTris *)ud, q6, 0, 2)) return 0;
            }
        }
        if (in_m2 > out_m2) c->st.lawn_dedup_m2 += (in_m2 - out_m2) / (SC_UPM * SC_UPM);
        return 1;
    }
    return sc_tri_split(c, (ScTris *)ud, t6, 0, 2);
}

static int sc_lawn_emit_plane(TG_TileCtx *c, const double *t6, void *ud)
{
    int k;
    (void)ud;
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
        c->lx[c->nlt * 3 + k] = t6[k * 2];
        c->lz[c->nlt * 3 + k] = t6[k * 2 + 1];
        c->ly[c->nlt * 3 + k] = sc_lawn_h(c, t6[k * 2], t6[k * 2 + 1]);
    }
    c->nlt++;
    return 1;
}

/* lawn planes of the plazas of the 3x3 whose box meets the tile box (tile-local) */
static int sc_lawn_planes(TG_TileCtx *c, const ScArea *ar, int nar, double x0, double z0, double x1, double z1)
{
    int i, k;
    for (i = 0; i < nar; i++) {
        const TD5_GeoArea *a = ar[i].a;
        double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX];
        int tri[(TD5_GEOB_RING_MAX - 2) * 3], ntri, lvl;
        const int no = a->n_outer ? a->n_outer : a->n;   /* [LD] a multipolygon: its outline, not the hole bridges */
        if (!td5_geob_area_is_plaza(a) || a->n < 3 || a->n > TD5_GEOB_RING_MAX || a->radius < SC_GEOP_MIN_R) continue;
        if (a->cx - (double)c->org_x + a->radius < x0 || a->cx - (double)c->org_x - a->radius > x1 ||
            a->cz - (double)c->org_z + a->radius < z0 || a->cz - (double)c->org_z - a->radius > z1) continue;
        for (k = 0; k < no; k++) { rx[k] = ar[i].px[k] - (double)c->org_x; rz[k] = ar[i].pz[k] - (double)c->org_z; }
        ntri = td5_geob_triangulate(rx, rz, no, tri, TD5_GEOB_RING_MAX - 2);
        if (ntri <= 0) continue;
        lvl = sc_lawn_level(rx, rz, tri, ntri);
        for (k = 0; k < ntri; k++) {
            const int i0 = tri[k * 3], i1 = tri[k * 3 + 1], i2 = tri[k * 3 + 2];
            const double pp[6] = { rx[i0], rz[i0], rx[i1], rz[i1], rx[i2], rz[i2] };
            if (c->lawn_dg) { if (!sc_lawn_cells(c, pp, sc_lawn_emit_plane, NULL)) return 0; continue; }   /* [roam IA] */
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

static int sc_lawn_write(TG_TileCtx *c, ScTris *trp, int sb);

static int sc_emit_lawn(TG_TileCtx *c, const TD5_GeoArea *a, const double *rx, const double *rz, int n, int sb)
{
    int tri[(TD5_GEOB_RING_MAX - 2) * 3];
    int ntri, k, lvl = 0;
    double maxe = 0.0;
    ScTris tr;
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
        if (c->lawn_dg ? !sc_lawn_cells(c, p, sc_lawn_emit_drawn, &tr)      /* [roam IA] on the drawn ground */
                       : !sc_tri_split(c, &tr, p, lvl, 2)) { free(tr.x); free(tr.z); return 0; }
    }
    return sc_lawn_write(c, &tr, sb);
}

/* the lawn triangles tr (consumed) as meshes of sub-block sb */
static int sc_lawn_write(TG_TileCtx *c, ScTris *trp, int sb)
{
    const double inv_tile = td5_env_flag_on("TD5RE_GEO_PLAZA_TILE") ? (1.0 / (double)TD5_TG_SPAN_LENGTH) : (1.0 / 3400.0);
    ScTris tr = *trp;
    float v[SC_LAWN_CHUNK * 3 * 5];
    unsigned int light[SC_LAWN_CHUNK * 3];
    int t0 = 0;
    while (t0 < tr.n) {
        const int nt = (tr.n - t0 < SC_LAWN_CHUNK) ? tr.n - t0 : SC_LAWN_CHUNK;
        unsigned short cmd[3];
        ScSb *b = sc_sb(c, sb, 1);
        int i;
        if (!b) { free(tr.x); free(tr.z); return 0; }
        for (i = 0; i < nt * 3; i++) {
            const int q = t0 * 3 + i;
            v[i * 5 + 0] = (float)tr.x[q];
            v[i * 5 + 1] = (float)sc_lawn_h(c, tr.x[q], tr.z[q]);
            v[i * 5 + 2] = (float)tr.z[q];
            /* [roam QB] place-frame UVs: two lawns of two owners show the same texels where they meet or overlap */
            v[i * 5 + 3] = (float)((tr.x[q] + (c->lawn_dedup ? (double)c->org_x : 0.0)) * inv_tile);
            v[i * 5 + 4] = (float)((tr.z[q] + (c->lawn_dedup ? (double)c->org_z : 0.0)) * inv_tile);
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

/* round crowns on the street (La Plata's tilos, platanos, fresnos): deciduous 0, big deciduous 1, round topiary 7.
 * Not 2: a HALF page drawn mirrored (tg_emit_billboard_mesh) comes out as a pointed, cypress-like crown. Lots add a
 * conifer, a willow and a palm (one in eight each). */
static const int k_st_street_var[3] = { 0, 1, 7 };
static const int k_st_lot_var[8]    = { 0, 1, 7, 0, 1, 3, 9, 5 };

/* [integ L] a plaza's species: by default from LA's lot set (k_st_lot_var), so no snow conifer (4), no half page (2)
 * and none of the other off-climate pages stand in a La Plata park. TD5RE_ROAM_PLAZA_TREE_PAGES=0 = the old pick from
 * all TD5_TG_TREE_VARIANTS pages. */
static int sc_plaza_tree_var(const TG_TileCtx *c, const TD5_GeoArea *a)
{
    if (c->pt_lot_set) return k_st_lot_var[a->id_hash % 8u];
    return (int)(a->id_hash % (unsigned)TD5_TG_TREE_VARIANTS);
}

static int sc_emit_trees(TG_TileCtx *c, const TD5_GeoArea *a, const double *rx, const double *rz, int n)
{
    const int tv = sc_plaza_tree_var(c, a);
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
                    if (c->tv_on) {                                  /* [integ L] never on the water plane (LE) */
                        double wh;
                        int wet = 0;
                        if (roam_ctx_ground_raw(c, gx, gz, &wh, &wet) && wet) { if (pass == 1) c->st.pt_wet_drop++; continue; }
                    }
                    ok = !sc_square_on_road(c, gx, gz, SC_TREE_CLEAR);
                    if (!ok) { if (pass == 1) c->st.trees_cut++; continue; }
                    if (pass == 0) { cand++; continue; }
                    if (cand > cap) {
                        const int keep = (int)(((long)(seen + 1) * cap) / cand) > (int)(((long)seen * cap) / cand);
                        seen++;
                        if (!keep) continue;
                    }
                    if (c->tv_on) {
                        /* [integ L] planted with the street trees, after the rails, outskirts, plaza furniture and
                         * fuel canopies, so it keeps off their faces (sc_emit_street_trees) */
                        if (!st_push_plaza_cand(c, gx, sc_ground(c, gx, gz) + SC_GEOP_LIFT, gz, tw * 0.5, th, tv)) return 0;
                        planted++;
                        continue;
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
                    c->st.plaza_tree_var[tv]++;                           /* [integ L] */
                    if (!st_push_pos(c, gx, gz, tw * 0.5 * 0.8)) return 0;   /* [roam LA] the canopy census (ST_CROWN_COVER) */
                }
            }
        }
    }
    return 1;
}

static int sc_plaza_paints(TG_TileCtx *c, const ScArea *e, const double *rx, const double *rz)
{
    if ((c->sport_on & 1) && e->spt == TD5_RPS_STADIUM) return 1;
    if ((c->sport_on & 2) && (e->srf & SC_SRF_PITCH)) {
        TD5_RoamPitch pq;
        pq.id = e->id; pq.sport = e->spt; pq.surface = e->srf & 0x7F;
        pq.rx = rx; pq.rz = rz; pq.n = e->a->n_outer ? e->a->n_outer : e->a->n;
        return td5_roam_pitch_paints(c, &pq);
    }
    return 0;
}

/* [roam QB] the plaza lawns of the 3x3 that draw (the ones sc_emit_plazas lays, each by its owner): what a lawn leaves to
 * another. map[i] = the cover of ar[i], -1 = none */
static int sc_lcov_build(TG_TileCtx *c, const ScArea *ar, int nar, int *map)
{
    int i, k;
    c->nlcov = 0;
    c->lcov = (ScLawnCover *)calloc((size_t)(nar > 0 ? nar : 1), sizeof(ScLawnCover));
    if (!c->lcov) return 0;
    for (i = 0; i < nar; i++) {
        const TD5_GeoArea *a = ar[i].a;
        ScLawnCover *cv = &c->lcov[c->nlcov];
        const int no = a->n_outer ? a->n_outer : a->n;
        double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX];
        map[i] = -1;
        if (!td5_geob_area_is_plaza(a) || a->n < 3 || a->n > TD5_GEOB_RING_MAX || a->radius < SC_GEOP_MIN_R) continue;
        for (k = 0; k < a->n; k++) { rx[k] = ar[i].px[k] - (double)c->org_x; rz[k] = ar[i].pz[k] - (double)c->org_z; }
        if (sc_plaza_paints(c, &ar[i], rx, rz)) continue;
        cv->n = no;
        cv->x0 = cv->z0 = 1e300; cv->x1 = cv->z1 = -1e300;
        for (k = 0; k < no; k++) {
            cv->x[k] = rx[k]; cv->z[k] = rz[k];
            if (rx[k] < cv->x0) cv->x0 = rx[k];
            if (rx[k] > cv->x1) cv->x1 = rx[k];
            if (rz[k] < cv->z0) cv->z0 = rz[k];
            if (rz[k] > cv->z1) cv->z1 = rz[k];
        }
        cv->ntri = td5_geob_triangulate(cv->x, cv->z, no, cv->tri, TD5_GEOB_RING_MAX - 2);
        if (cv->ntri <= 0) continue;
        cv->m2 = fabs(td5_geob_ring_area(cv->x, cv->z, no));
        cv->id = ar[i].id;
        roam_grid_tile_of(c->grid, a->cx, a->cz, &cv->otx, &cv->otz);
        map[i] = c->nlcov++;
    }
    return 1;
}

static int sc_emit_plazas(TG_TileCtx *c, const ScArea *ar, int nar)
{
    int i, k, *lmap = NULL;
    if (!td5_env_flag_on("TD5RE_GEO_PLAZAS")) return 1;
    c->lcov_self = -1;
    if (c->lawn_dedup && c->lawn_dg) {   /* [roam QB] */
        lmap = (int *)malloc((size_t)(nar > 0 ? nar : 1) * sizeof(int));
        if (!lmap || !sc_lcov_build(c, ar, nar, lmap)) { free(lmap); return 0; }
    }
    for (i = 0; i < nar; i++) {
        c->lcov_self = lmap ? lmap[i] : -1;
        const TD5_GeoArea *a = ar[i].a;
        double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], cx = 0.0, cz = 0.0;
        const int n = a->n;
        if (!td5_geob_area_is_plaza(a)) continue;
        if (n < 3 || n > TD5_GEOB_RING_MAX || a->radius < SC_GEOP_MIN_R) continue;
        if (!sc_owned_point(c, a->cx - (double)c->org_x, a->cz - (double)c->org_z)) continue;
        for (k = 0; k < n; k++) { rx[k] = ar[i].px[k] - (double)c->org_x; rz[k] = ar[i].pz[k] - (double)c->org_z; cx += rx[k]; cz += rz[k]; }
        cx /= n; cz /= n;
        c->st.areas++;
        if (ar[i].id >= SC_PREL_ID_BASE && ar[i].id < SC_PREL_ID_BASE + 1.0e15) c->st.park_rel++;   /* [LD] */
        {   /* [roam PB] a painted pitch lays its own surface: no lawn under it (coplanar, it z-fought the paint) */
            int paint = 0;
            if ((c->sport_on & 1) && ar[i].spt == TD5_RPS_STADIUM) paint = 1;   /* the stadium grounds: the drawn ground (a lawn
                                                                                 * page) shows, the pitch paint has no lawn under it */
            else if ((c->sport_on & 2) && (ar[i].srf & SC_SRF_PITCH)) {
                TD5_RoamPitch pq;
                pq.id = ar[i].id; pq.sport = ar[i].spt; pq.surface = ar[i].srf & 0x7F;
                pq.rx = rx; pq.rz = rz; pq.n = a->n_outer ? a->n_outer : n;
                paint = td5_roam_pitch_paints(c, &pq);
            }
            if (paint) c->st.sp_lawn_skip++;
            else if (!sc_emit_lawn(c, a, rx, rz, a->n_outer ? a->n_outer : n, sc_sb_of(c, cx, cz))) return 0;
        }
        if ((c->sport_on & 1) && ar[i].spt == TD5_RPS_STADIUM) { c->st.sp_tree_skip++; continue; }   /* [roam PB] */
        if ((c->sport_on & 2) && (ar[i].srf & SC_SRF_PITCH)) { c->st.sp_tree_skip++; continue; }
        {
            int xs[TD5_ROAM_SCN_NSB];
            sc_xc_snap(c, xs);
            if (!sc_emit_trees(c, a, rx, rz, n)) { free(lmap); free(c->lcov); c->lcov = NULL; return 0; }
            sc_xc_note(c, xs, SC_XC_TREE_PLAZA);                                 /* [integ L] */
        }
    }
    c->lcov_self = -1;
    free(lmap);
    free(c->lcov); c->lcov = NULL; c->nlcov = 0;
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
 * [roam IA] a gap of at most PV_MEDIAN_M with no mapped building in this side's half is
 * an island too whichever streets bound it (it used to keep two sidewalks and a bare
 * strip of ground 40-70 cm below them between: Mariano's "holes in the medians"); an
 * island slab keeps its back face, so where the two halves do not meet (a wedge, two
 * kerb heights) the step between them is closed (TD5RE_ROAM_PV_ISLAND_FILL / _BACK), and
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
#define PV_LEVEL_TOL    43.0         /* [roam JA] ground this far (10 cm) over an island's level: ground follow */
#define PV_MED_SEAM     8.0          /* [roam JA] island halves overlap 2 cm (same plane, world-space uv): no hairline (pv_piece) */
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
#define PV_W_SKIP_PAIR  60.0         /* [roam JA] the same between the two one-way halves of ONE named street (14 cm):
                                      * a tight divided avenue keeps a narrow kerbed median, not a slot of lot */
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
#define PV_VF_ISLAND    0x04u        /* filled to the middle of a gap: no fence (no back face before roam IA) */
#define PV_VF_MEDIAN    0x08u        /* ... and it is a named avenue's median: lawn       */
#define PV_VF_FACADE    0x10u        /* clipped at a building front: no fence             */
#define PV_VF_COLLAPSE  0x20u        /* merged into a corner point                        */
#define PV_VF_PAIR      0x40u        /* [roam JA] its gap is a same-name one-way pair's median (PV_W_SKIP_PAIR) */
#define PV_VF_GAP       0x80u        /* [roam NA] frontage view only: another road within PV_MEDIAN_M behind the kerb, so
                                      * the pavement may fill the gap to an island (no municipal line there) */

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
    int       island_fill, island_back;   /* [roam IA] */
    int       join_guard;                 /* [roam JA] pv_join: no join on the road side of a kerb line */
    int       medial;                     /* [roam JA] pv_medial: fill to the medial line, not the half ray */
    int       level;                      /* [roam JA] pv_heights: an island's outer edge between the two kerb tops */
    int       pair_skip;                  /* [roam JA] PV_W_SKIP_PAIR in a same-name pair's median */
    int       side_fix;                   /* [roam PD] TD5RE_ROAM_PV_SIDE_FIX: sidewalk=left / right on the right kerb */
    double    dbg_x, dbg_z, dbg_r;        /* [roam IA] DEV TD5RE_ROAM_PV_DBG=x,z,r (place frame): width-rule trace */
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
    p->fence  = td5_env_int("TD5RE_ROAM_PV_FENCE", 0, 0, 1);   /* [IB] off: no fence along any kerb (deck railings stay) */
    p->facade = td5_env_int("TD5RE_ROAM_PV_FACADE", 1, 0, 1);
    p->merge  = td5_env_int("TD5RE_ROAM_PV_MERGE", 1, 0, 1);
    p->median = td5_env_int("TD5RE_ROAM_PV_MEDIAN", 1, 0, 1);
    p->island_fill = td5_env_int("TD5RE_ROAM_PV_ISLAND_FILL", 1, 0, 1);   /* [roam IA] */
    p->island_back = td5_env_int("TD5RE_ROAM_PV_ISLAND_BACK", 1, 0, 1);   /* [roam IA] */
    p->join_guard  = td5_env_int("TD5RE_ROAM_PV_JOIN_GUARD", 1, 0, 1);    /* [roam JA] */
    p->medial      = td5_env_int("TD5RE_ROAM_PV_MEDIAL", 1, 0, 1);        /* [roam JA] */
    p->level       = td5_env_int("TD5RE_ROAM_PV_ISLAND_LEVEL", 1, 0, 1);  /* [roam JA] */
    p->pair_skip   = td5_env_int("TD5RE_ROAM_PV_PAIR_SKIP", 1, 0, 1);     /* [roam JA] */
    p->side_fix    = td5_env_int("TD5RE_ROAM_PV_SIDE_FIX", 1, 0, 1);      /* [roam PD] */
#ifndef TD5RE_RELEASE
    {
        const char *q = getenv("TD5RE_ROAM_PV_DBG");
        if (q && sscanf(q, "%lf,%lf,%lf", &p->dbg_x, &p->dbg_z, &p->dbg_r) != 3) p->dbg_r = 0.0;
    }
#endif
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
    uint8_t *cutsp = NULL;                       /* [roam NC] */
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
        if (td5_roam_cut_on()) {                  /* [roam NC] the cutting spans: no sidewalk, the wall stands there */
            cutsp = (uint8_t *)realloc(cutsp, t->nspan ? t->nspan : 1u);
            if (!cutsp) { free(ser); free(B); return 0; }
            td5_roam_cut_spans(t, c->hc, cutsp);
        }
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
                /* [roam PD] (-dz, dx) is the LEFT of travel in a right-handed frame; the place frame is mirrored since
                 * roam M (x = -east, td5_geo_frame.h), so there it is the RIGHT: sidewalk=right / left landed on the
                 * other kerb (casco: every one of 789 bare one-sided kerbs stood on the tagged side). */
                if (s->pv->side_fix) left = !left;
                b->w = pv_width(s, w, er, left, &src);
                if (cutsp && be->prim < t->nspan && cutsp[be->prim]) { b->w = 0.0; if (ti == 4) c->st.cut.pv_skip++; }
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
    free(cutsp);
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
    double  *fy;                         /* [roam JA] per island vertex: the far kerb's road y (or -1e30) */
    unsigned char *vf;                   /* per vertex: PV_VF_*                          */
    int      join_guard;                 /* [roam JA] TD5RE_ROAM_PV_JOIN_GUARD (pv_join)  */
    int      level;                      /* [roam JA] TD5RE_ROAM_PV_ISLAND_LEVEL (pv_heights) */
} PvChain;

/* The junction of the outer lines of alive segments a and b (a before b), the
 * vertex between them being v. Clamped to PV_MITRE_MAX widths from it.
 * [roam JA] A segment whose two ends have different widths has a TILTED outer line, and two tilted lines
 * over short pieces (an acute block tip whose kerb vertices were filled to the middle of the gap, PV_VF_ISLAND,
 * beside a fillet that was not) can meet past the tip, on the ROAD side of a kerb line: the guard then halved
 * the piece six times and dropped it, and the block tip stayed a hole the city lot showed through (Parque
 * Vucetich, Diagonal 101 x Calle 27). A join on the road side of either kerb line is the vertex's own mitre
 * instead (TD5RE_ROAM_PV_JOIN_GUARD=0: the old join). */
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
    /* judged after the clamp: on a collapsed run v lies off segment b's line, so pulling a far join back toward
     * it can land on the road side of that line */
    if (ch->join_guard &&
        ((*x - ch->px[a]) * ch->nx[a] + (*z - ch->pz[a]) * ch->nz[a] < 0.0 ||
         (*x - ch->px[b]) * ch->nx[b] + (*z - ch->pz[b]) * ch->nz[b] < 0.0)) {
        *x = ch->px[v] + ch->ux[v] * ch->w[v] * ch->mf[v];   /* mf <= 2.5 = PV_MITRE_MAX: within lim */
        *z = ch->pz[v] + ch->uz[v] * ch->w[v] * ch->mf[v];
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

typedef struct { const double *px, *pz; int n; double x0, z0, x1, z1; int lm; int biz; } PvRing;   /* biz: [roam PE] */   /* place frame; lm: [HD] a landmark;
                                                                                  * [roam NA] px / pz: the snapped ring (c->mpool) when there is one */
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
                        sc_bld_drops(s, c, gb, bc[l]->id[bc[l]->order[j]], &bc[l]->px[gb->first], &bc[l]->pz[gb->first], ar, nar)) {
                        if (judge == 1) c->st.pv_bld_gone++;   /* [roam NA] 2: the municipal line's own walk, not counted */
                        continue;
                    }
                    if (!sc_grow((void **)&pb->r, &cap, pb->nr + 1, sizeof(PvRing))) return 0;
                    r = &pb->r[pb->nr++];
                    r->px = &bc[l]->px[gb->first]; r->pz = &bc[l]->pz[gb->first]; r->n = gb->n;
                    {   /* [roam NA] a footprint moved onto the municipal line stands there for the pavement too */
                        const ScMuniRing *mu = sc_muni_find(c, bc[l]->id[bc[l]->order[j]], gb->cx, gb->cz);
                        if (mu && mu->fate == TD5_MUNI_SNAPPED) {
                            r->px = &c->mpool[mu->first]; r->pz = &c->mpool[mu->first + mu->n]; r->n = mu->n;
                        }
                    }
                    r->lm = gb->landmark;
                    r->biz = 0;
                    if (s->shops && td5_roam_shops_infill_guard(s->shops)) {   /* [roam PE] a footprint with a shopfront */
                        int cat, from_poi;
                        r->biz = td5_roam_shops_category(s->shops, bc[l]->id[bc[l]->order[j]], gb, &cat, &from_poi);
                    }
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
    int          kerb_only;           /* [roam IA] the chain's top is its triangulated loop: pieces emit kerb faces only */
    int          trees;               /* [roam LA] street tree candidates along the chain (SECTION trees) */
    int          dry;                 /* [roam NA] a geometry-only walk (sc_muni_prepare): no stats */
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
        /* [roam JA] ISLAND LEVEL. An island vertex meets the far side's slab on the medial line (pv_medial), where
         * both kerbs are equally far: its outer edge stands at the mean of the two kerb tops, so the two halves meet
         * at one height (each used to follow the ground within its own kerb's cross-fall, and the higher half's back
         * face showed as a crease across the island). Not where the ground stands over that level (a mound, a
         * steep cross slope): the old rule there. */
        if (ch->level && (vf & PV_VF_ISLAND) && !(vf & (PV_VF_DECK | PV_VF_FLAT))) {
            double fs = 0.0;
            int fn = 0;
            for (j = 0; j < n; j++) { const int k = (v + j) % nv; if (ch->fy[k] > -1e29) { fs += ch->fy[k]; fn++; } }
            if (fn == n) {
                const double lvl = 0.5 * (yk + fs / (double)n + TD5_TG_KERB_H);
                if (g + TD5_TG_KERB_H <= lvl + PV_LEVEL_TOL) {
                    for (j = 0; j < n; j++) { const int k = (v + j) % nv; yo[k] = lvl; gr[k] = g - PV_BACK_SINK; }
                    done += n;
                    continue;
                }
            }
        }
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

/* [roam JA] the narrowest slab vertex v keeps (PV_W_SKIP; PV_W_SKIP_PAIR in a same-name pair's median) */
static double pv_skip(const PvChain *ch, int v) { return (ch->vf[v] & PV_VF_PAIR) ? PV_W_SKIP_PAIR : PV_W_SKIP; }

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
    if (ch->w[a] < pv_skip(ch, a) && ch->w[b] < pv_skip(ch, b)) return 1;
    if ((ox[1] - ox[0]) * ddx + (oz[1] - oz[0]) * ddz < 0.0) {
        /* an outer piece still running backwards (a reversal no corner rule may fold):
         * a triangle to its midpoint, never a bow-tie */
        ox[0] = ox[1] = 0.5 * (ox[0] + ox[1]);
        oz[0] = oz[1] = 0.5 * (oz[0] + oz[1]);
    }
    sb = sc_sb_of(c, 0.5 * (kx[a] + kx[b]), 0.5 * (kz[a] + kz[b]));
    /* the guard: the slab footprint off every drivable primitive, cut back, else dropped ([roam IA] no slab in
     * kerb-only mode: the loop's own top covers it) */
    for (; !e->kerb_only;) {
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
        /* [roam IA] an island piece (filled to the middle of a gap) is halved up to 6 times before it is dropped:
         * a dropped piece is the hole it was filling */
        if (++cut > (island ? 6 : 2)) {
#ifndef TD5RE_RELEASE
            if (e->s->pv->dbg_r > 0.0 && hypot(kx[a] + c->org_x - e->s->pv->dbg_x, kz[a] + c->org_z - e->s->pv->dbg_z) < e->s->pv->dbg_r)
                TD5_LOG_I(LOG_TAG, "PVDBG DROP piece %d..%d island %d k (%.0f,%.0f)-(%.0f,%.0f) o (%.0f,%.0f)-(%.0f,%.0f) w %.0f %.0f",
                          i0, i1, island, kx[a] + c->org_x, kz[a] + c->org_z, kx[b] + c->org_x, kz[b] + c->org_z,
                          ox[0] + c->org_x, oz[0] + c->org_z, ox[1] + c->org_x, oz[1] + c->org_z, ch->w[a], ch->w[b]);
#endif
            c->st.pv_drop++; return 1;
        }
        for (k = 0; k < 2; k++) {
            const double bx = k ? kx[b] : kx[a], bz = k ? kz[b] : kz[a];
            ox[k] = bx + (ox[k] - bx) * 0.5; oz[k] = bz + (oz[k] - bz) * 0.5;
        }
    }
    if (cut) c->st.pv_cut++;
    /* [roam JA] SEAM. Two island halves meet on the medial line from two independent chains: their outer edges
     * coincide only to the millimetre and the hairline between them showed dark down the middle of a median. Each
     * island end of an uncut piece reaches PV_MED_SEAM past it (same plane, world-space uv: the overlap does not
     * show), here and not in the chain's widths so the chain's collapse and mitre decisions stay as they are; kept
     * only while the footprint stays off the road. */
    if (!cut && island && e->s->pv->medial && !e->kerb_only) {
        double sx[2], sz[2], qx[4], qz[4];
        int q, moved = 0;
        for (q = 0; q < 2; q++) {
            const int vi = q ? b : a;
            const double kx0 = q ? kx[b] : kx[a], kz0 = q ? kz[b] : kz[a];
            const double dx = ox[q] - kx0, dz = oz[q] - kz0, l = hypot(dx, dz);
            sx[q] = ox[q]; sz[q] = oz[q];
            if ((ch->vf[vi] & PV_VF_ISLAND) && l > 4.0 * PV_MED_SEAM) { sx[q] += dx / l * PV_MED_SEAM; sz[q] += dz / l * PV_MED_SEAM; moved = 1; }
        }
        qx[0] = kx[a]; qx[1] = kx[b]; qx[2] = sx[1]; qx[3] = sx[0];
        qz[0] = kz[a]; qz[1] = kz[b]; qz[2] = sz[1]; qz[3] = sz[0];
        for (q = 0; q < 4; q++) { qx[q] = (double)(float)qx[q]; qz[q] = (double)(float)qz[q]; }
        if (moved && !sc_on_road(c, qx, qz, 4, NULL)) { ox[0] = sx[0]; oz[0] = sz[0]; ox[1] = sx[1]; oz[1] = sz[1]; }
    }
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
    if (!e->kerb_only && !pv_quad(e, sb, lawn ? PV_B_LAWN : PV_B_PAVE, x, y, z, u, v, 0xFFFFFFFFu)) return 0;
    if (!e->kerb_only && sc_grow((void **)&c->pvq, &c->cap_pvq, (c->npvq + 1) * 12, sizeof(double))) {   /* [G5] for the footpaths */
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
    if (cut || flat || e->kerb_only || (island && !e->s->pv->island_back)) return 1;   /* [roam IA] islands keep the back face */
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
        int fence = deck && !island;
        /* not on a junction outline: a block corner holds a building or a plaza, and the
         * outline has no street class of its own */
        if (!fence && !island && e->s->pv->fence && !corner && !((fa | fb) & PV_VF_FACADE) && klass >= TD5_GEO_RC_LIVING &&
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

/* [roam IA] ISLAND TOPS. A closed kerb loop whose every vertex is an island (filled to the middle of the gap
 * round it) used to be offset like a street sidewalk: on a narrow or bent island the outer lines reverse, the
 * loop collapses to its centroid (pv_offset_chain) and each piece became a triangle to that point. Across a
 * non-convex island (a wedge between a diagonal and a calle, a boomerang nose) those triangles cross the road,
 * the guard drops them and the island stays a hole. Here the top is the loop itself: the kerb points (already
 * PV_KERB_IN inside), ear-clipped, at kerb height (a flat, kerbed island); the pieces then emit their kerb faces
 * only. Used only on a loop where the guard would drop one of the pieces (pv_piece_dropped, a dry run over the whole
 * loop): elsewhere the pieces reach out to the road round the island, the top stops at its kerb line. Each tile
 * emits the triangles whose centroid it owns. Only loops no wider than PV_TOP_BOX_M: every tile
 * that owns a piece or a triangle of one then holds the whole loop in its 3x3, so all of them take the same
 * decision. Refused (the old pieces stay) when a triangle meets the drivable surface or the drawn ground rises
 * to within PV_TOP_CLEAR of the top. */
#define PV_TOP_BOX_M   200.0
#define PV_TOP_CLEAR   (5.0 * SC_UPM / 100.0)
#define PV_TOP_MAXV    2048

static double pv_cross2(double ax, double az, double bx, double bz, double cx, double cz)
{
    return (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
}

/* ear clipping of a simple polygon (any orientation); tri[] gets 3 indices per triangle, returns the count */
static int pv_earclip(const double *x, const double *z, int n, int *tri)
{
    int *idx = (int *)malloc((size_t)n * sizeof(int));
    int i, m = n, nt = 0, guard = 0;
    double area = 0.0;
    if (!idx) return 0;
    for (i = 0; i < n; i++) { idx[i] = i; area += x[i] * z[(i + 1) % n] - x[(i + 1) % n] * z[i]; }
    while (m > 3 && guard < 4 * n * n) {
        int found = 0;
        for (i = 0; i < m && !found; i++) {
            const int a = idx[(i + m - 1) % m], b = idx[i], cc = idx[(i + 1) % m];
            const double cr = pv_cross2(x[a], z[a], x[b], z[b], x[cc], z[cc]);
            int j, inside = 0;
            guard++;
            if ((area > 0.0 ? cr : -cr) <= 1e-6) continue;          /* reflex or flat */
            for (j = 0; j < m && !inside; j++) {
                const int q = idx[j];
                double c1, c2, c3;
                if (q == a || q == b || q == cc) continue;
                c1 = pv_cross2(x[a], z[a], x[b], z[b], x[q], z[q]);
                c2 = pv_cross2(x[b], z[b], x[cc], z[cc], x[q], z[q]);
                c3 = pv_cross2(x[cc], z[cc], x[a], z[a], x[q], z[q]);
                if (area > 0.0 ? (c1 >= 0.0 && c2 >= 0.0 && c3 >= 0.0) : (c1 <= 0.0 && c2 <= 0.0 && c3 <= 0.0)) inside = 1;
            }
            if (inside) continue;
            tri[nt * 3] = a; tri[nt * 3 + 1] = b; tri[nt * 3 + 2] = cc; nt++;
            for (j = i; j < m - 1; j++) idx[j] = idx[j + 1];
            m--;
            found = 1;
        }
        if (!found) { nt = -1; break; }                          /* not simple: refuse */
    }
    if (nt >= 0 && m == 3) { tri[nt * 3] = idx[0]; tri[nt * 3 + 1] = idx[1]; tri[nt * 3 + 2] = idx[2]; nt++; }
    free(idx);
    return nt;
}

/* would pv_piece's guard drop the slab of vertices a..b (island halving schedule)? Every piece of the loop, owned or
 * not, so every tile that holds the loop decides alike */
static int pv_piece_dropped(TG_TileCtx *c, const PvChain *ch, int a, int b, const double *kx, const double *kz)
{
    double ox[2] = { ch->ox[a], ch->ox[b] }, oz[2] = { ch->oz[a], ch->oz[b] };
    const double ddx = kx[b] - kx[a], ddz = kz[b] - kz[a];
    int cut = 0, k;
    if (ch->w[a] < pv_skip(ch, a) && ch->w[b] < pv_skip(ch, b)) return 0;
    if ((ox[1] - ox[0]) * ddx + (oz[1] - oz[0]) * ddz < 0.0) {
        ox[0] = ox[1] = 0.5 * (ox[0] + ox[1]);
        oz[0] = oz[1] = 0.5 * (oz[0] + oz[1]);
    }
    for (;;) {
        double qx[4] = { kx[a], kx[b], ox[1], ox[0] }, qz[4] = { kz[a], kz[b], oz[1], oz[0] }, fx[4], fz[4];
        int m = 0, j, d;
        for (j = 0; j < 4; j++) {
            for (d = 0; d < m; d++) if (fabs(fx[d] - qx[j]) < 1.0 && fabs(fz[d] - qz[j]) < 1.0) break;
            if (d == m) { fx[m] = (double)(float)qx[j]; fz[m] = (double)(float)qz[j]; m++; }
        }
        if (m >= 3 && fabs(td5_geob_ring_area(fx, fz, m)) < 4.0) m = 2;
        if (m < 2 || !sc_on_road(c, fx, fz, m, NULL)) return 0;
        if (++cut > 6) return 1;
        for (k = 0; k < 2; k++) {
            const double bx = k ? kx[b] : kx[a], bz = k ? kz[b] : kz[a];
            ox[k] = bx + (ox[k] - bx) * 0.5; oz[k] = bz + (oz[k] - bz) * 0.5;
        }
    }
}

/* 1 = the chain's top was handled here (emit the pieces kerb-only), 0 = use the pieces, -1 = out of memory */
static int pv_island_top(PvEmit *e, const PvChain *ch, const double *kx, const double *kz)
{
    TG_TileCtx *c = e->c;
    const int n = ch->nv;
    double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
    const double drop = td5_env_int("TD5RE_ROAM_RENDER_GROUND_DROP_CM", 40, 0, 500) * (SC_UPM / 100.0);   /* the drawn ground */
    double *ty;
    int *tri, nt, t, v, lawn = 1, ok = 1;
    if (!ch->closed || n < 3 || n > PV_TOP_MAXV || !e->s->pv->island_fill) return 0;
    for (v = 0; v < n; v++) {
        if (!(ch->vf[v] & PV_VF_ISLAND) || (ch->vf[v] & (PV_VF_DECK | PV_VF_FLAT))) return 0;
        if (!(ch->vf[v] & PV_VF_MEDIAN)) lawn = 0;
        if (kx[v] < x0) x0 = kx[v];
        if (kx[v] > x1) x1 = kx[v];
        if (kz[v] < z0) z0 = kz[v];
        if (kz[v] > z1) z1 = kz[v];
    }
    if (x1 - x0 > PV_TOP_BOX_M * SC_UPM || z1 - z0 > PV_TOP_BOX_M * SC_UPM) return 0;
    {   /* the pieces cover the island (and the gap round it, to the road) unless the guard would drop one */
        int any = 0;
        for (v = 0; v < n && !any; v++) any = pv_piece_dropped(e->c, ch, v, (v + 1) % n, kx, kz);
        if (!any) return 0;
    }
    tri = (int *)malloc((size_t)(n - 2) * 3u * sizeof(int));
    ty = (double *)malloc((size_t)n * sizeof(double));
    if (!tri || !ty) { free(tri); free(ty); return -1; }
    for (v = 0; v < n; v++) ty[v] = ch->py[v] + TD5_TG_KERB_H;
    nt = pv_earclip(kx, kz, n, tri);
    if (nt != n - 2) ok = 0;
    /* refuse when any triangle meets the road or the drawn ground comes up to the top (every tile tests the whole
     * loop, so all take the same decision) */
    for (t = 0; t < nt && ok; t++) {
        const int a = tri[t * 3], b = tri[t * 3 + 1], q = tri[t * 3 + 2];
        const double fx[3] = { (double)(float)kx[a], (double)(float)kx[b], (double)(float)kx[q] };
        const double fz[3] = { (double)(float)kz[a], (double)(float)kz[b], (double)(float)kz[q] };
        static const double wq[4][3] = { { 1.0 / 3, 1.0 / 3, 1.0 / 3 }, { 0.6, 0.2, 0.2 }, { 0.2, 0.6, 0.2 }, { 0.2, 0.2, 0.6 } };
        int k;
        if (fabs(td5_geob_ring_area(fx, fz, 3)) < 4.0) continue;     /* a sliver: drawn as nothing */
        if (sc_on_road(c, fx, fz, 3, NULL)) { ok = 0; break; }
        for (k = 0; k < 4 && ok; k++) {
            const double px = wq[k][0] * kx[a] + wq[k][1] * kx[b] + wq[k][2] * kx[q];
            const double pz = wq[k][0] * kz[a] + wq[k][1] * kz[b] + wq[k][2] * kz[q];
            const double top = wq[k][0] * ty[a] + wq[k][1] * ty[b] + wq[k][2] * ty[q];
            if (sc_ground(c, px, pz) - drop + 30.0 > top - PV_TOP_CLEAR) ok = 0;   /* a city lot: SC_LOT_LIFT over it */
        }
    }
#ifndef TD5RE_RELEASE
    {
        const struct ScPvSrc *P = e->s->pv;
        if (P->dbg_r > 0.0 && P->dbg_x >= x0 + c->org_x - P->dbg_r && P->dbg_x <= x1 + c->org_x + P->dbg_r &&
            P->dbg_z >= z0 + c->org_z - P->dbg_r && P->dbg_z <= z1 + c->org_z + P->dbg_r)
        {
            TD5_LOG_I(LOG_TAG, "PVDBG TOP tile %d,%d loop n %d box %.0fx%.0f m tris %d ok %d lawn %d", c->tx, c->tz, n,
                      (x1 - x0) / SC_UPM, (z1 - z0) / SC_UPM, nt, ok, lawn);
        }
    }
#endif
    if (ok) {
        for (t = 0; t < nt; t++) {
            const int a = tri[t * 3], b = tri[t * 3 + 1], q = tri[t * 3 + 2];
            const double mx = (kx[a] + kx[b] + kx[q]) / 3.0, mz = (kz[a] + kz[b] + kz[q]) / 3.0;
            double x[4], y[4], z[4], u[4], w[4];
            int k;
            const int id[4] = { a, b, q, q };
            if (!sc_owned_point(c, mx, mz)) continue;
            for (k = 0; k < 4; k++) {
                x[k] = kx[id[k]]; z[k] = kz[id[k]]; y[k] = ty[id[k]];
                u[k] = x[k] * (lawn ? PV_UV_LAWN : PV_UV); w[k] = z[k] * (lawn ? PV_UV_LAWN : PV_UV);
            }
            if (!pv_quad(e, sc_sb_of(c, mx, mz), lawn ? PV_B_LAWN : PV_B_PAVE, x, y, z, u, w, 0xFFFFFFFFu)) { ok = -1; break; }
            if (sc_grow((void **)&c->pvq, &c->cap_pvq, (c->npvq + 1) * 12, sizeof(double))) {   /* [G5] for the footpaths */
                double *P = &c->pvq[c->npvq++ * 12];
                for (k = 0; k < 4; k++) { P[k * 3] = (double)(float)x[k]; P[k * 3 + 1] = (double)(float)y[k]; P[k * 3 + 2] = (double)(float)z[k]; }
            }
            c->st.pv_island_tops++;
        }
    } else {
        c->st.pv_island_top_refused++;
    }
    free(tri); free(ty);
    return ok;
}

/* [roam JA] the free distance from kerb point (x, z) along unit (ux, uz) to the next drivable surface (the ray
 * of pv_chain_emit: PV_RAY_STEP march then 5 halvings), -1 when none within lim */
static double pv_ray_free(TG_TileCtx *c, double x, double z, double ux, double uz, double lim)
{
    double d;
    for (d = PV_RAY_STEP; d <= lim; d += PV_RAY_STEP) {
        const double qx = x + ux * (PV_KERB_IN + d), qz = z + uz * (PV_KERB_IN + d);
        if (sc_on_road(c, &qx, &qz, 1, NULL)) {
            double lo = d - PV_RAY_STEP, hi = d;
            int it;
            for (it = 0; it < 5; it++) {
                const double mid = 0.5 * (lo + hi);
                const double rx = x + ux * (PV_KERB_IN + mid), rz = z + uz * (PV_KERB_IN + mid);
                if (sc_on_road(c, &rx, &rz, 1, NULL)) hi = mid; else lo = mid;
            }
            return PV_KERB_IN + lo;
        }
    }
    return -1.0;
}

/* [roam JA] MEDIAL FILL. A vertex filled to the middle of a gap (an island, a median, a vertex clamped to half
 * the free distance) used to stop at HALF the ray, freed / 2. That is the medial line only when the far kerb is
 * square to the ray. Where two kerbs meet at an angle (a block tip between a diagonal and a calle: Parque Vucetich,
 * Diagonal 101 x Calle 27) each side's half-ray point lies past the true middle, so the two sides' slabs crossed
 * over each other a few cm apart (a stacked, ragged seam). The far kerb's direction comes from two more rays
 * PV_MED_PROBE_DEG either side; the slab stops at t = f cos(phi) / (1 + cos(phi)) along the ray (phi: the ray
 * against the far kerb's normal), the point as far from this kerb as from that one. Back to f / 2 when a side ray
 * misses or the two far points disagree (another road), and phi is capped at PV_MED_PHI_MAX.
 * TD5RE_ROAM_PV_MEDIAL=0: the old half ray. */
#define PV_MED_PROBE_DEG  12.0
#define PV_MED_PHI_MAX    70.0
#define PV_MED_STRAIGHT_COS 0.9659   /* cos 15 deg: a vertex turning more is a corner (half ray) */
static double pv_medial(PvEmit *e, const PvChain *ch, int v, double freed)
{
    TG_TileCtx *c = e->c;
    const double ux = ch->ux[v], uz = ch->uz[v], a = PV_MED_PROBE_DEG * 3.14159265358979323846 / 180.0;
    const double ca = cos(a), sa = sin(a);
    double fp, fm, bx[2], bz[2], dx, dz, dl, cphi;
    if (!e->s->pv->medial || !(freed > 0.0)) return 0.5 * freed;
    if (freed <= 2.0 * PV_MED_SEAM) return 0.5 * freed;
    {   /* a corner vertex (a fillet arc): its side rays fan round the bend and the far kerb read jumps from one
         * vertex to the next, so the old half ray (the arc's pieces must keep a smooth outer line) */
        const int kin = (v > 0) ? v - 1 : (ch->closed ? ch->ns - 1 : -1), kout = (v < ch->ns) ? v : -1;
        if (kin >= 0 && kout >= 0 && ch->nx[kin] * ch->nx[kout] + ch->nz[kin] * ch->nz[kout] < PV_MED_STRAIGHT_COS)
            return 0.5 * freed;
    }
    fp = pv_ray_free(c, ch->px[v], ch->pz[v], ux * ca - uz * sa, ux * sa + uz * ca, 2.0 * freed + PV_RAY_STEP);
    fm = pv_ray_free(c, ch->px[v], ch->pz[v], ux * ca + uz * sa, -ux * sa + uz * ca, 2.0 * freed + PV_RAY_STEP);
    if (fp < 0.0 || fm < 0.0) return 0.5 * freed;
    bx[0] = ch->px[v] + (ux * ca - uz * sa) * fp;  bz[0] = ch->pz[v] + (ux * sa + uz * ca) * fp;
    bx[1] = ch->px[v] + (ux * ca + uz * sa) * fm;  bz[1] = ch->pz[v] + (-ux * sa + uz * ca) * fm;
    dx = bx[0] - bx[1]; dz = bz[0] - bz[1]; dl = hypot(dx, dz);
    /* the two far points span about 2 f tan(a) on one straight kerb; much more or less is a corner or another road */
    if (!(dl > 0.5 * freed * tan(a)) || dl > 4.0 * freed * tan(a)) return 0.5 * freed;
    cphi = fabs(ux * dz - uz * dx) / dl;            /* |u x far kerb direction| = cos(ray, far normal) */
    if (cphi < cos(PV_MED_PHI_MAX * 3.14159265358979323846 / 180.0)) cphi = cos(PV_MED_PHI_MAX * 3.14159265358979323846 / 180.0);
    if (cphi > 1.0) cphi = 1.0;
    return freed * cphi / (1.0 + cphi);
}

static uint32_t sc_rd32(const uint8_t *p);   /* SECTION assemble */
static float    sc_rdf(const uint8_t *p);

/* ============================================================================
 * SECTION: trees [roam LA] (street trees + lot trees from CANOPY.R8)
 *
 * WHAT. CANOPY.R8 (Meta-WRI canopy height, ~3.5 m cells) reads >= 3 m over 12 % of the casco, and OSM maps 43 tree
 * nodes there: the raster is the only real source. Two emitters read it:
 *
 *   STREET BAND. Every kerb chain the pavement walks (pv_chain_emit) is walked again by arc length along its kerb
 *   line. A STATION every `spacing` (6.5 .. 12 m, 0.85 of the crown width the canopy height gives) on a ribbon kerb
 *   whose canopy (the trunk, 1.75 m each way along the kerb, 1.75 m out over the road: crowns overhang the road)
 *   reads >= 3 m; where it does not, the walk searches on in 2 m steps. No station within 5 m of a junction outline
 *   or a dead-end cap (corner sight lines, the crossings at the mouth), nor within 2 m of a deck, a bore or an
 *   island stretch. The trunk stands 40 % of the band width out from the kerb (0.55 .. 0.9 m), on a band at least
 *   1.4 m wide, at least 0.6 m short of a building front (mapped or infill; the crown is narrowed to the front and
 *   reaches at most 2.5 m over the carriageway, shorter when narrowed past 85 %),
 *   on the slab (its height), its 0.35 m trunk square off the road and 2 m each way along the kerb off the road too
 *   (a driveway or a side street mouth). Species: one round-crown page per named street (FNV of the name), one tree
 *   in five another; height from the canopy (5 .. 15 m, +-10 % jitter).
 *
 *   LOT. The plaza emitter's WORLD lattice (12 m) over the owned tile, jittered up to 2 m per point by its own hash:
 *   a point whose 3x3 canopy probe reads >= 3 m gets a tree (5 .. 18 m, deciduous / conifer / willow / palm pages) unless
 *   it stands on a road, a slab, a building (mapped or infill), a plaza / park / pitch / grass area (the plazas plant
 *   their own) or water, or is HIDDEN: a building stands between it and the nearest street centreline and the
 *   canopy is under 10 m (a tree that tall shows over two-three storey roofs). Its crown reaches at most 1.5 m over a
 *   road (narrowed, and shorter when narrowed past 85 %: a big camera-facing billboard beside a street reads as a
 *   green wall across it). At most 400 a tile.
 *
 * FURNITURE. Both emit AFTER the furniture and the railways, so they read the faces those wrote (this tile's tail of
 * every sub-block): a trunk keeps 1.5 m off any furniture face (2 m off a flat one: zebras, limit lines, rail beds),
 * else the station tries 1.5 m and 3 m each way along the kerb before it is dropped. A neighbour tile's furniture
 * is not in the tail: no tree stands within 2.5 m of the tile edge (each owner emits only what stands in it, so a
 * neighbour's pole is at least that far). The bytes census judges every tree record (sc_assemble): trees on the
 * drivable surface stay 0.
 *
 * Knobs: TD5RE_ROAM_STREET_TREES=0 / TD5RE_ROAM_LOT_TREES=0 (the old bytes, bar the rules number).
 * ========================================================================== */

#define ST_M(m)             ((m) * SC_UPM)
#define ST_ALTS             5
#define ST_W_MIN_M          1.4
#define ST_LAT_FRAC         0.40
#define ST_LAT_MIN_M        0.55
#define ST_LAT_MAX_M        0.9
#define ST_CORNER_CLEAR_M   5.0
#define ST_BLOCK_CLEAR_M    2.0
#define ST_SEARCH_M         2.0
#define ST_SPACE_K          0.85
#define ST_SPACE_MIN_M      6.5
#define ST_SPACE_MAX_M      12.0
#define ST_H_MIN_M          5.0
#define ST_H_MAX_M          15.0
#define ST_PROBE_M          1.75
#define ST_FACADE_MIN_M     0.6
#define ST_FACADE_OVER_M    0.4
#define ST_ROAD_OVER_M      2.5
#define ST_FACADE_PROBE_M   8.0
#define ST_MOUTH_M          2.0
#define ST_TRUNK_M          0.35
#define ST_DODGE_M          1.5
#define ST_KEEP_M           1.5
#define ST_KEEP_FLAT_M      2.0
#define ST_FLAT_M           0.3
#define ST_BORDER_M         2.5
#define ST_LOT_H_MAX_M      18.0
#define ST_LOT_SHOW_M       10
#define ST_LOT_JIT_M        2.0
#define ST_LOT_CLEAR_M      1.0
#define ST_LOT_OVER_M       1.5
#define ST_LOT_REACH_M      80.0
#define ST_LOT_CAP          400
#define ST_SQUEEZE          0.85     /* a crown narrowed past this share of its width gets shorter too (keeps the page's shape) */
#define ST_CROWN_COVER      0.8      /* the share of the billboard half width a crown covers on the canopy raster */

/* [roam LA] the street / lot species tables (k_st_street_var, k_st_lot_var) are with the plaza trees (sc_emit_trees) */

struct ScTreeCand {
    double x[ST_ALTS], z[ST_ALTS], y[ST_ALTS];
    int    nalt;
    double hw, th;                  /* billboard half width, height (units) */
    int    var, kind;               /* k_tree_pages variant; 0 = street band, 1 = lot, 2 = plaza [integ L],
                                     * 3 = rule sidewalk, 4 = rule median [roam PD] */
    int    cls;                     /* [roam PD] rule trees: 0 avenue, 1 diagonal */
};

static uint32_t st_hash_xz(double x, double z, uint32_t salt)
{
    const int32_t q[3] = { (int32_t)floor(x + 0.5), (int32_t)floor(z + 0.5), (int32_t)salt };
    return sc_fnv(2166136261u, q, sizeof q);
}

static int st_push_pos(TG_TileCtx *c, double x, double z, double r)
{
    if (!sc_grow((void **)&c->tpos, &c->cap_tpos, (c->ntpos + 1) * 3, sizeof(double))) return 0;
    c->tpos[c->ntpos * 3 + 0] = x; c->tpos[c->ntpos * 3 + 1] = z; c->tpos[c->ntpos * 3 + 2] = r;
    c->ntpos++;
    return 1;
}

/* [roam PD] every footprint ring this tile emitted (mapped, infill, 3D parts), tile-local, AFTER its clearance shift:
 * the pavement's own set (PvBld) holds the source rings, and a footprint sc_bld_clear moved off a road stands
 * where that set does not say (round N integ census: 72 casco trunks inside an emitted ring, 47 of them Overture /
 * Open Buildings blobs, every one also inside with the municipal line off) */
struct StRing { int first, n; double x0, z0, x1, z1; };

static int st_push_ring(TG_TileCtx *c, const double *rx, const double *rz, int n)
{
    struct StRing *r;
    int k;
    if (!c->tb_on || n < 3) return 1;
    if (!sc_grow((void **)&c->ebr, &c->cap_ebr, c->nebr + 1, sizeof(struct StRing)) ||
        !sc_grow((void **)&c->ebp, &c->cap_ebp, c->nebp + 2 * n, sizeof(double))) return 0;
    r = &c->ebr[c->nebr++];
    r->first = c->nebp; r->n = n;
    r->x0 = r->x1 = rx[0]; r->z0 = r->z1 = rz[0];
    for (k = 0; k < n; k++) {
        c->ebp[c->nebp + k] = rx[k]; c->ebp[c->nebp + n + k] = rz[k];
        if (rx[k] < r->x0) r->x0 = rx[k];
        if (rx[k] > r->x1) r->x1 = rx[k];
        if (rz[k] < r->z0) r->z0 = rz[k];
        if (rz[k] > r->z1) r->z1 = rz[k];
    }
    c->nebp += 2 * n;
    return 1;
}

/* [roam PD] a trunk (tile-local) inside a footprint: an emitted ring of this tile, the pavement's halo set (the 3x3's
 * rings: a neighbour's footprint reaching over the seam) or an infill lot */
static int st_in_bld(const TG_TileCtx *c, double x, double z)
{
    int i;
    for (i = 0; i < c->nebr; i++) {
        const struct StRing *r = &c->ebr[i];
        if (x < r->x0 || x > r->x1 || z < r->z0 || z > r->z1) continue;
        if (td5_geob_point_in_ring(&c->ebp[r->first], &c->ebp[r->first + r->n], r->n, x, z)) return 1;
    }
    if (c->pv_bld && pv_in_building(c, (const PvBld *)c->pv_bld, x, z)) return 1;
    return inf_in_lot(c, x, z);
}

static struct ScTreeCand *st_cand_new(TG_TileCtx *c)
{
    struct ScTreeCand *t;
    if (!sc_grow((void **)&c->tcand, &c->cap_tcand, c->ntcand + 1, sizeof(struct ScTreeCand))) return NULL;
    t = &c->tcand[c->ntcand];
    memset(t, 0, sizeof *t);
    return t;
}

/* [integ L] a plaza tree (sc_emit_trees) as a late candidate: one position, no tile-edge margin (a park's lattice
 * runs to the seam, as it always did) */
static int st_push_plaza_cand(TG_TileCtx *c, double x, double y, double z, double hw, double th, int var)
{
    struct ScTreeCand *t = st_cand_new(c);
    if (!t) return 0;
    t->x[0] = x; t->y[0] = y; t->z[0] = z; t->nalt = 1;
    t->hw = hw; t->th = th; t->var = var; t->kind = 2;
    c->ntcand++;
    return 1;
}

/* the highest canopy reading of a few points (-1 = none measured) */
static int st_canopy_max(const TG_TileCtx *c, const double *x, const double *z, int n)
{
    int k, best = -1;
    for (k = 0; k < n; k++) { const int v = sc_canopy_m(c, x[k], z[k]); if (v > best) best = v; }
    return best;
}

/* the billboard of variant v for a canopy height h_m (metres) and its jitter hash */
static void st_size(int v, double h_m, double h_max_m, uint32_t h, double *hw, double *th)
{
    const TG_TreePage *tp = &k_tree_pages[v];
    double t = h_m < ST_H_MIN_M ? ST_H_MIN_M : (h_m > h_max_m ? h_max_m : h_m);
    t *= 0.9 + (double)((h >> 11) % 21) * 0.01;                         /* 0.90 .. 1.10 */
    *th = ST_M(t);
    *hw = 0.5 * (double)tp->w * (*th / (double)tp->h);
}

/* the first blocked stretch whose clearance holds arc position s: the position just past it (s itself when free) */
static double st_blocked(const double *S, const double *clr, int m, double s)
{
    int k;
    for (k = 0; k < m; k++)
        if (clr[k] >= 0.0 && s >= S[k] - clr[k] && s <= S[k + 1] + clr[k]) return S[k + 1] + clr[k] + 1.0;
    return s;
}

/* ---- [roam PD] RULE STREET TREES ----
 * CANOPY.R8 reads almost nothing over La Plata's avenue sidewalks (Av 7 3.8 % of kerb samples >= 3 m, Diagonal 74
 * 8 %, Av 53 22 %: _archive/roam/LA street_canopy.py) though they are lined with tilos, platanos and jacarandas. On a
 * CASCO tile, every kerb of a way whose NAME makes it an avenue, a boulevard or a diagonal (TD5_GEO_NAMEK_AVENIDA /
 * _DIAGONAL) gets a regular row where the canopy gives none: a station every RT_SPACE_MIN_M .. +4 m (one spacing per
 * street, FNV of its name), the trunk RT_LAT_M out from the kerb on a sidewalk of at least RT_W_MIN_M, on the
 * medial line of a median lawn (PV_VF_MEDIAN, a same-name one-way pair or an OSM green) at least RT_MED_MIN_M wide.
 * Not within RT_CORNER_M of a junction outline (the ochava, the corner crossings), 2 m of a deck / bore / building
 * passage, RT_MOUTH_M of a road along the kerb (driveway, side street mouth, a forecourt drive), on a fuel forecourt
 * (POIS.JSON ring), nor where LA's walk owns the station (canopy >= 3 m there). Every other veto is LA's: the facade
 * probe and crown narrowing, the slab, the tile edge, the furniture tail (lamps, signals, bus stops, zebras, limit
 * lines, rail beds; a standing face RT_FURN_M off), the shopfront / awning crown rule. Emitted after every other tree
 * and refused within RT_DOUBLE_M of one (no doubles over LA's canopy trees, one row on a median both kerbs walk).
 * Species: LA's round-crown street pages (k_st_street_var: broadleaf only, no conifer), height RT_H_MIN_M .. MAX. */
#define RT_W_MIN_M      2.5          /* Mariano 2026-10-22: 2.5 (was 3), La Plata plants its 2.5 m veredas */
#define RT_FACE_LAT_M   1.5          /* a no-sidewalk kerb facing a median: the trunk this far onto the lawn */
#define RT_FACE_MAX_M   60.0         /* ... the partner carriageway searched this far out */
#define RT_MED_MIN_M    4.0
#define RT_SPACE_MIN_M  8.0
#define RT_LAT_M        0.9
#define RT_CORNER_M     8.0
#define RT_MOUTH_M      3.0
#define RT_DOUBLE_M     6.0
#define RT_FURN_M       2.5
#define RT_H_MIN_M      7.0
#define RT_H_MAX_M      11.0
#define RT_SAMPLE_M     2.0
#define RT_COVER_M      7.0          /* the coverage census: a kerb sample with a trunk this near has a tree */

static int rt_av_class(const PvB *p)
{
    if (!p || p->kind != PV_K_RIB || !p->way) return -1;
    if (p->way->namek == TD5_GEO_NAMEK_AVENIDA) return 0;
    if (p->way->namek == TD5_GEO_NAMEK_DIAGONAL) return 1;
    return -1;
}

/* [roam PD] Mariano 2026-10-22: the NO-SIDEWALK kerb of a one-way carriageway (sidewalk=left / right, the other side)
 * that faces its same-name partner across a median of RT_MED_MIN_M or more gets a row too, on the lawn. March out
 * from the kerb point (tile-local, outward normal nx nz): a footprint or an infill lot first = a block, not a
 * median; the first road hit must be a one-way way of the same name, at least RT_MED_MIN_M out. */
static int rt_faces_median(PvEmit *e, const PvB *p, double px, double pz, double nx, double nz)
{
    TG_TileCtx *c = e->c;
    const PvWay *me = p->way;
    double d;
    if (!me || !me->oneway || !me->name_h || (me->sidewalk != TD5_GEO_SW_LEFT && me->sidewalk != TD5_GEO_SW_RIGHT)) return 0;
    for (d = ST_M(0.5); d <= ST_M(RT_FACE_MAX_M); d += ST_M(0.5)) {
        const double qx = px + nx * d, qz = pz + nz * d;
        const ScPrim *hp = NULL;
        if (pv_in_building(c, e->bld, qx, qz) || inf_in_lot(c, qx, qz)) return 0;
        if (sc_on_road(c, &qx, &qz, 1, &hp)) {
            const PvWay *ot;
            if (d < ST_M(RT_MED_MIN_M) || !hp || hp->seg < 0) return 0;
            ot = pv_find(e->ways, e->nways, (int64_t)(c->edge[c->seg[hp->seg].edge].key >> 16));
            return ot && ot != me && ot->oneway && ot->name_h == me->name_h;
        }
    }
    return 0;
}

static int pv_rule_trees(PvEmit *e, const PvChain *ch, const PvB *B, const int *seq, int m, const double *kx,
                         const double *kz, const double *S)
{
    TG_TileCtx *c = e->c;
    double *clr, total = S[m], s;
    unsigned char *mode;
    int k, seg = 0, ok = 1;
    clr = (double *)malloc((size_t)m * sizeof(double));
    mode = (unsigned char *)calloc((size_t)m, 1);
    if (!clr || !mode) { free(clr); free(mode); return 0; }
    for (k = 0; k < m; k++) {
        const int a = k, b = (k + 1) % ch->nv;
        const PvB *p = &B[seq[k]];
        const unsigned f = ch->vf[a] | ch->vf[b];
        const int cls = rt_av_class(p);
        clr[k] = -1.0;
        if (p->kind != PV_K_RIB) clr[k] = ST_M(RT_CORNER_M);
        else if ((f & (PV_VF_DECK | PV_VF_FLAT)) || p->deck || p->flat || ch->w[a] <= 0.0 || ch->w[b] <= 0.0)
            clr[k] = ST_M(ST_BLOCK_CLEAR_M);
        else if (f & PV_VF_ISLAND) {
            if (cls >= 0 && (ch->vf[a] & ch->vf[b] & PV_VF_MEDIAN) &&
                2.0 * (ch->w[a] < ch->w[b] ? ch->w[a] : ch->w[b]) >= ST_M(RT_MED_MIN_M)) mode[k] = 2;
            else clr[k] = ST_M(ST_BLOCK_CLEAR_M);
        } else if (cls < 0) clr[k] = 0.0;
        else mode[k] = 1;
        if (mode[k] == 1 && p->owned) {        /* the coverage census: the owned sidewalk kerb of an avenue / diagonal */
            const double len = S[k + 1] - S[k];
            double d;
            for (d = 0.5 * ST_M(RT_SAMPLE_M); d < len; d += ST_M(RT_SAMPLE_M)) {
                const double t = d / len;
                if (!sc_grow((void **)&c->rks, &c->cap_rks, (c->nrks + 1) * 4, sizeof(double))) { ok = 0; break; }
                c->rks[c->nrks * 4 + 0] = kx[a] + (kx[b] - kx[a]) * t;
                c->rks[c->nrks * 4 + 1] = kz[a] + (kz[b] - kz[a]) * t;
                c->rks[c->nrks * 4 + 2] = (double)cls;
                c->rks[c->nrks * 4 + 3] = (double)p->way->name_h;
                c->nrks++;
                c->st.rt_kerb_m[cls] += RT_SAMPLE_M;
            }
        }
    }
    s = (double)(st_hash_xz(kx[0] + (double)c->org_x, kz[0] + (double)c->org_z, 0x2EE5u) % 800u) * (SC_UPM / 100.0);
    while (ok && c->rt_on && s < total) {
        const double past = st_blocked(S, clr, m, s);
        int a, b, var, alt;
        double t, len, dx, dz, nx, nz, px, pz, w, lat, hw, th, sp, hw0, dfac = ST_M(ST_FACADE_PROBE_M);
        double qx[4], qz[4];
        uint32_t h, name;
        const PvB *p;
        int m3 = 0;
        if (past > s) { s = past; continue; }
        while (seg < m - 1 && S[seg + 1] <= s) seg++;
        a = seg; b = (seg + 1) % ch->nv;
        p = &B[seq[seg]];
        len = S[seg + 1] - S[seg];
        if (!mode[seg] || !(len > 1.0)) { s = S[seg + 1] + 1.0; continue; }
        name = p->way->name_h ? p->way->name_h : (uint32_t)(p->ekey ^ (p->ekey >> 32));
        sp = ST_M(RT_SPACE_MIN_M + (double)(name % 5u));
        if (!p->owned) { s += sp; continue; }
        c->st.rt_stations++;
        t = (s - S[seg]) / len;
        dx = (kx[b] - kx[a]) / len; dz = (kz[b] - kz[a]) / len;
        nx = ch->nx[seg]; nz = ch->nz[seg];
        px = kx[a] + (kx[b] - kx[a]) * t; pz = kz[a] + (kz[b] - kz[a]) * t;
        w = ch->w[a] * (1.0 - t) + ch->w[b] * t;
        if (mode[seg] == 2) lat = w - PV_KERB_IN;                   /* the median's medial line */
        else {
            if (w < ST_M(RT_W_MIN_M - 0.05) && c->rt_face && rt_faces_median(e, p, px, pz, nx, nz)) {
                m3 = 1;                              /* the no-sidewalk kerb of a one-way pair, a median >= 4 m beyond */
                lat = ST_M(RT_FACE_LAT_M);
            } else if (w < ST_M(RT_W_MIN_M - 0.05)) {      /* 5 cm: a 3 m vereda interpolates to 2.9999 */
                c->st.rt_narrow++;
#ifndef TD5RE_RELEASE
                if (c->tree_log)
                    TD5_LOG_I(LOG_TAG, "ROAM_RTN tile %d,%d at %.0f,%.0f w %.2f m (%.2f / %.2f) vf %02x %02x way %lld name %08x "
                              "klass %d sw %d tag %.1f/%.1f", c->tx, c->tz, px + (double)c->org_x, pz + (double)c->org_z,
                              w / SC_UPM, ch->w[a] / SC_UPM, ch->w[b] / SC_UPM, ch->vf[a], ch->vf[b], (long long)p->way->id,
                              p->way->name_h, p->way->klass, p->way->sidewalk, p->way->tag_l, p->way->tag_r);
#endif
                s += sp; continue;
            } else lat = ST_M(RT_LAT_M);
        }
        qx[0] = px + nx * lat; qz[0] = pz + nz * lat;
        qx[1] = qx[0] + dx * ST_M(ST_PROBE_M); qz[1] = qz[0] + dz * ST_M(ST_PROBE_M);
        qx[2] = qx[0] - dx * ST_M(ST_PROBE_M); qz[2] = qz[0] - dz * ST_M(ST_PROBE_M);
        qx[3] = qx[0] - nx * ST_M(ST_PROBE_M); qz[3] = qz[0] - nz * ST_M(ST_PROBE_M);
        if (st_canopy_max(c, qx, qz, 4) >= SC_CANOPY_M) { c->st.rt_canopy++; s += sp; continue; }
        h = st_hash_xz(qx[0] + (double)c->org_x, qz[0] + (double)c->org_z, 0x2EE7u);
        {
            int vi = (int)(name % 3u);
            if (h % 5u == 0u) vi = (vi + 1 + (int)((h >> 8) & 1u)) % 3;
            var = k_st_street_var[vi];
        }
        st_size(var, RT_H_MIN_M + (double)((h >> 3) % 5u), RT_H_MAX_M, h, &hw, &th);
        if (e->s->shops && td5_roam_shops_fuel_at(e->s->shops, qx[0] + (double)c->org_x, qz[0] + (double)c->org_z)) {
            c->st.rt_fuel++; s += sp; continue;
        }
        if (mode[seg] == 1 && !m3) {   /* the building front behind the trunk: the crown is narrowed to it */
            double d;
            for (d = 0.0; d < ST_M(ST_FACADE_PROBE_M); d += ST_M(0.25)) {
                const double fx = qx[0] + nx * d, fz = qz[0] + nz * d;
                if (pv_in_building(c, e->bld, fx, fz) || inf_in_lot(c, fx, fz)) { dfac = d; break; }
            }
            if (dfac < ST_M(ST_FACADE_MIN_M)) { c->st.rt_facade++; s += sp; continue; }
        }
        hw0 = hw;
        if (hw > dfac + ST_M(ST_FACADE_OVER_M)) hw = dfac + ST_M(ST_FACADE_OVER_M);
        if (hw > lat + ST_M(ST_ROAD_OVER_M)) hw = lat + ST_M(ST_ROAD_OVER_M);
        if (hw < ST_SQUEEZE * hw0) th *= hw / (ST_SQUEEZE * hw0);
        if (th < ST_M(ST_H_MIN_M) * 0.8) { c->st.rt_facade++; s += sp; continue; }
        {
            struct ScTreeCand *tc = NULL;
            static const double k_off[ST_ALTS] = { 0.0, ST_DODGE_M, -ST_DODGE_M, 2.0 * ST_DODGE_M, -2.0 * ST_DODGE_M };
            for (alt = 0; alt < ST_ALTS; alt++) {
                const double so = s + ST_M(k_off[alt]);
                const double ax = qx[0] + dx * ST_M(k_off[alt]), az = qz[0] + dz * ST_M(k_off[alt]);
                double y, mx[2], mz[2];
                if (so < 0.0 || so > total || st_blocked(S, clr, m, so) != so) continue;
                if (m3) y = sc_ground(c, ax, az) + SC_GEOP_LIFT;   /* [roam PD] on the median lawn, no slab */
                else if (!sc_pv_y(c, ax, az, &y)) continue;
                if (sc_square_on_road(c, ax, az, ST_M(ST_TRUNK_M))) continue;
                mx[0] = ax + dx * ST_M(RT_MOUTH_M); mz[0] = az + dz * ST_M(RT_MOUTH_M);
                mx[1] = ax - dx * ST_M(RT_MOUTH_M); mz[1] = az - dz * ST_M(RT_MOUTH_M);
                if (sc_on_road(c, &mx[0], &mz[0], 1, NULL) || sc_on_road(c, &mx[1], &mz[1], 1, NULL)) continue;
                if (pv_in_building(c, e->bld, ax, az) || inf_in_lot(c, ax, az)) continue;
                if (!tc) {
                    tc = st_cand_new(c);
                    if (!tc) { ok = 0; break; }
                    tc->hw = hw; tc->th = th; tc->var = var; tc->kind = m3 ? 5 : mode[seg] == 2 ? 4 : 3;
                    tc->cls = rt_av_class(p);
                }
                tc->x[tc->nalt] = ax; tc->z[tc->nalt] = az; tc->y[tc->nalt] = y; tc->nalt++;
            }
            if (tc) c->ntcand++;
            else if (ok) c->st.rt_noslab++;
        }
        s += sp;
    }
    free(clr); free(mode);
    return ok;
}

/* Street trees along one kerb chain (SECTION trees). Candidates only: emitted after the furniture. */
static int pv_trees_chain(PvEmit *e, const PvChain *ch, const PvB *B, const int *seq, int m, const double *kx,
                          const double *kz)
{
    TG_TileCtx *c = e->c;
    double *S, *clr, total, s;
    int k, seg = 0, ok = 1;
    if (m <= 0) return 1;
    S = (double *)malloc((size_t)(m + 1) * 2u * sizeof(double));
    if (!S) return 0;
    clr = S + m + 1;
    S[0] = 0.0;
    for (k = 0; k < m; k++) {
        const int a = k, b = (k + 1) % ch->nv;
        const PvB *p = &B[seq[k]];
        const unsigned f = ch->vf[a] | ch->vf[b];
        S[k + 1] = S[k] + hypot(kx[b] - kx[a], kz[b] - kz[a]);
        clr[k] = -1.0;
        if (p->kind != PV_K_RIB) clr[k] = ST_M(ST_CORNER_CLEAR_M);
        else if ((f & (PV_VF_DECK | PV_VF_FLAT | PV_VF_ISLAND)) || p->deck || p->flat || ch->w[a] <= 0.0 || ch->w[b] <= 0.0)
            clr[k] = ST_M(ST_BLOCK_CLEAR_M);
    }
    total = S[m];
    /* the walk's phase: the chain's first kerb vertex in the WORLD frame */
    s = (double)(st_hash_xz(kx[0] + (double)c->org_x, kz[0] + (double)c->org_z, 0x5EEDu) % 400u) * (SC_UPM / 100.0);
    if (!e->trees) s = total;                                     /* [roam PD] the rule walk alone */
    while (s < total && ok) {
        const double past = st_blocked(S, clr, m, s);
        int a, b, var, alt;
        double t, len, dx, dz, nx, nz, px, pz, w, lat, hw, th, sp, dfac = ST_M(ST_FACADE_PROBE_M), hw0;
        double qx[4], qz[4];
        uint32_t h;
        const PvB *p;
        if (past > s) { s = past; continue; }
        while (seg < m - 1 && S[seg + 1] <= s) seg++;
        a = seg; b = (seg + 1) % ch->nv;
        p = &B[seq[seg]];
        len = S[seg + 1] - S[seg];
        if (!(len > 1.0)) { s = S[seg + 1] + 1.0; continue; }
        t = (s - S[seg]) / len;
        dx = (kx[b] - kx[a]) / len; dz = (kz[b] - kz[a]) / len;
        nx = ch->nx[seg]; nz = ch->nz[seg];
        px = kx[a] + (kx[b] - kx[a]) * t; pz = kz[a] + (kz[b] - kz[a]) * t;
        w = hypot(ch->ox[a] - kx[a], ch->oz[a] - kz[a]) * (1.0 - t) + hypot(ch->ox[b] - kx[b], ch->oz[b] - kz[b]) * t;
        lat = ST_LAT_FRAC * w;
        if (lat < ST_M(ST_LAT_MIN_M)) lat = ST_M(ST_LAT_MIN_M);
        if (lat > ST_M(ST_LAT_MAX_M)) lat = ST_M(ST_LAT_MAX_M);
        {   /* the canopy over the trunk and the crown's reach */
            const double tx0 = px + nx * lat, tz0 = pz + nz * lat;
            qx[0] = tx0; qz[0] = tz0;
            qx[1] = tx0 + dx * ST_M(ST_PROBE_M); qz[1] = tz0 + dz * ST_M(ST_PROBE_M);
            qx[2] = tx0 - dx * ST_M(ST_PROBE_M); qz[2] = tz0 - dz * ST_M(ST_PROBE_M);
            qx[3] = tx0 - nx * ST_M(ST_PROBE_M); qz[3] = tz0 - nz * ST_M(ST_PROBE_M);
        }
        {
            const int hm = st_canopy_max(c, qx, qz, 4);
            h = st_hash_xz(qx[0] + (double)c->org_x, qz[0] + (double)c->org_z, 0x7EE5u);
            {
                const uint32_t name = (p->way && p->way->name_h) ? p->way->name_h : (uint32_t)(p->ekey ^ (p->ekey >> 32));
                int vi = (int)(name % 3u);
                if (h % 5u == 0u) vi = (vi + 1 + (int)((h >> 8) & 1u)) % 3;
                var = k_st_street_var[vi];
            }
            if (hm < SC_CANOPY_M) {
                if (p->owned) c->st.st_nocanopy++;
                s += ST_M(ST_SEARCH_M);
                continue;
            }
            st_size(var, (double)hm, ST_H_MAX_M, h, &hw, &th);
        }
        sp = ST_SPACE_K * 2.0 * hw;
        if (sp < ST_M(ST_SPACE_MIN_M)) sp = ST_M(ST_SPACE_MIN_M);
        if (sp > ST_M(ST_SPACE_MAX_M)) sp = ST_M(ST_SPACE_MAX_M);
        if (!p->owned) { s += sp; continue; }
        c->st.st_stations++;
        if (w < ST_M(ST_W_MIN_M)) { c->st.st_narrow++; s += sp; continue; }
        {   /* the building front behind the trunk: the crown is narrowed to it */
            double d;
            for (d = 0.0; d < ST_M(ST_FACADE_PROBE_M); d += ST_M(0.25)) {
                const double fx = qx[0] + nx * d, fz = qz[0] + nz * d;
                if (pv_in_building(c, e->bld, fx, fz) || inf_in_lot(c, fx, fz)) { dfac = d; break; }
            }
        }
        if (dfac < ST_M(ST_FACADE_MIN_M)) { c->st.st_facade++; s += sp; continue; }
        hw0 = hw;
        if (hw > dfac + ST_M(ST_FACADE_OVER_M)) hw = dfac + ST_M(ST_FACADE_OVER_M);
        if (hw > lat + ST_M(ST_ROAD_OVER_M)) hw = lat + ST_M(ST_ROAD_OVER_M);   /* no green wall across a narrow street */
        if (hw < ST_SQUEEZE * hw0) th *= hw / (ST_SQUEEZE * hw0);
        if (th < ST_M(ST_H_MIN_M) * 0.8) { c->st.st_facade++; s += sp; continue; }
        {
            struct ScTreeCand *tc = NULL;
            static const double k_off[ST_ALTS] = { 0.0, ST_DODGE_M, -ST_DODGE_M, 2.0 * ST_DODGE_M, -2.0 * ST_DODGE_M };
            for (alt = 0; alt < ST_ALTS; alt++) {
                const double so = s + ST_M(k_off[alt]);
                const double ax = qx[0] + dx * ST_M(k_off[alt]), az = qz[0] + dz * ST_M(k_off[alt]);
                double y, mx[2], mz[2];
                if (so < 0.0 || so > total || st_blocked(S, clr, m, so) != so) continue;
                if (!sc_pv_y(c, ax, az, &y)) continue;
                if (sc_square_on_road(c, ax, az, ST_M(ST_TRUNK_M))) continue;
                mx[0] = ax + dx * ST_M(ST_MOUTH_M); mz[0] = az + dz * ST_M(ST_MOUTH_M);
                mx[1] = ax - dx * ST_M(ST_MOUTH_M); mz[1] = az - dz * ST_M(ST_MOUTH_M);
                if (sc_on_road(c, &mx[0], &mz[0], 1, NULL) || sc_on_road(c, &mx[1], &mz[1], 1, NULL)) continue;
                if (pv_in_building(c, e->bld, ax, az) || inf_in_lot(c, ax, az)) continue;
                if (!tc) {
                    tc = st_cand_new(c);
                    if (!tc) { ok = 0; break; }
                    tc->hw = hw; tc->th = th; tc->var = var; tc->kind = 0;
                }
                tc->x[tc->nalt] = ax; tc->z[tc->nalt] = az; tc->y[tc->nalt] = y; tc->nalt++;
            }
            if (tc) c->ntcand++;
            else if (ok) c->st.st_noslab++;
        }
        s += sp;
    }
    if (ok && c->rt_casco && !pv_rule_trees(e, ch, B, seq, m, kx, kz, S)) ok = 0;   /* [roam PD] */
    free(S);
    return ok;
}

/* Lot trees of the owned tile (SECTION trees). Candidates only: emitted after the furniture. */
static int sc_lot_trees(TD5_RoamScnSource *s, TG_TileCtx *c, const PvBld *bld, const ScArea *ar, int nar)
{
    double corner[4][2], x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300, gx, gz;
    const double ox = (double)c->org_x, oz = (double)c->org_z;
    int i, n = 0;
    roam_grid_corners(&s->grid, c->tx, c->tz, corner);
    for (i = 0; i < 4; i++) {
        if (corner[i][0] < x0) x0 = corner[i][0];
        if (corner[i][0] > x1) x1 = corner[i][0];
        if (corner[i][1] < z0) z0 = corner[i][1];
        if (corner[i][1] > z1) z1 = corner[i][1];
    }
    x0 -= ox; x1 -= ox; z0 -= oz; z1 -= oz;
    {
        const double lx0 = floor((x0 - ST_M(ST_LOT_JIT_M) + ox) / SC_TREE_STEP) * SC_TREE_STEP - ox;
        const double lz0 = floor((z0 - ST_M(ST_LOT_JIT_M) + oz) / SC_TREE_STEP) * SC_TREE_STEP - oz;
        for (gz = lz0; gz <= z1 + ST_M(ST_LOT_JIT_M) && n < ST_LOT_CAP; gz += SC_TREE_STEP)
            for (gx = lx0; gx <= x1 + ST_M(ST_LOT_JIT_M) && n < ST_LOT_CAP; gx += SC_TREE_STEP) {
                const uint32_t h = st_hash_xz(gx + ox, gz + oz, 0x10Fu);
                const double x = gx + ((double)(h % 401u) / 200.0 - 1.0) * ST_M(ST_LOT_JIT_M);
                const double z = gz + ((double)((h >> 9) % 401u) / 200.0 - 1.0) * ST_M(ST_LOT_JIT_M);
                double qx[9], qz[9], y, gh;
                int ddx, ddz, best, wet = 0, var;
                struct ScTreeCand *tc;
                if (!sc_owned_point(c, x, z)) continue;
                for (ddz = -1; ddz <= 1; ddz++)
                    for (ddx = -1; ddx <= 1; ddx++) {
                        qx[(ddz + 1) * 3 + ddx + 1] = x + ddx * SC_TREE_STEP * 0.33;
                        qz[(ddz + 1) * 3 + ddx + 1] = z + ddz * SC_TREE_STEP * 0.33;
                    }
                best = st_canopy_max(c, qx, qz, 9);
                if (best < SC_CANOPY_M) continue;
                if (sc_square_on_road(c, x, z, ST_M(ST_LOT_CLEAR_M)) || sc_pv_y(c, x, z, &y) ||
                    pv_in_building(c, bld, x, z) || inf_in_lot(c, x, z) || pv_in_green(c, ar, nar, x, z) ||
                    (roam_ctx_ground_raw(c, x, z, &gh, &wet) && wet)) { c->st.st_lot_refused++; continue; }
                if (best < ST_LOT_SHOW_M) {
                    /* hidden behind a facade from the nearest street? */
                    int edge, side, hid = 0;
                    double along, latv;
                    RoamFrame fr;
                    if (roam_nearest_edge(c, x, z, ST_M(ST_LOT_REACH_M), &edge, &side, &along, &latv) &&
                        roam_edge_frame(c, edge, side, along, &fr)) {
                        const double L = hypot(fr.px - x, fr.pz - z);
                        double d;
                        for (d = ST_M(1.5); d < L && !hid; d += ST_M(1.5)) {
                            const double fx = x + (fr.px - x) * d / L, fz = z + (fr.pz - z) * d / L;
                            if (pv_in_building(c, bld, fx, fz) || inf_in_lot(c, fx, fz)) hid = 1;
                        }
                    }
                    if (hid) { c->st.st_lot_hidden++; continue; }
                }
                tc = st_cand_new(c);            /* not counted until c->ntcand++ below: a refusal leaves it unused */
                if (!tc) return 0;
                var = k_st_lot_var[(h >> 17) % 8u];
                st_size(var, (double)best, ST_LOT_H_MAX_M, h, &tc->hw, &tc->th);
                {   /* a lot crown reaches at most ST_LOT_OVER_M over the road: a camera-facing billboard bigger than
                     * that stands across the street as a green wall */
                    const double hw0 = tc->hw;
                    double r = tc->hw - ST_M(ST_LOT_OVER_M);
                    while (r > ST_M(ST_LOT_CLEAR_M) && sc_square_on_road(c, x, z, r)) r -= ST_M(0.75);
                    if (r + ST_M(ST_LOT_OVER_M) < tc->hw) tc->hw = r + ST_M(ST_LOT_OVER_M);
                    if (tc->hw < ST_SQUEEZE * hw0) tc->th *= tc->hw / (ST_SQUEEZE * hw0);
                    if (tc->th < ST_M(ST_H_MIN_M) * 0.8) { c->st.st_lot_refused++; continue; }
                }
                tc->var = var; tc->kind = 1;
                tc->x[0] = x; tc->z[0] = z; tc->y[0] = sc_ground(c, x, z) + SC_GEOP_LIFT; tc->nalt = 1;
                c->ntcand++;
                n++;
            }
    }
    return 1;
}

/* one furniture / rail face as a keep-out: its XZ footprint (1-4 points) and the margin a trunk keeps off it.
 * [integ L] lit = 1: a shopfront / awning / pool face (the round L veto), judged by height: a face over the crown
 * floor (ylo) keeps the CROWN off it, a lower one keeps the trunk ST_LIT_TRUNK_M off it. lit = 2: an outskirts face
 * (pole, wire), its high faces only (the wires): the low ones are already in the furniture tail. */
/* [integ P] lit = 4: a rails face (catenary, boards, masts) judged like lit 2 (high faces keep the crown off) for RULE
 * trees only (TD5RE_ROAM_RULE_TREE_WIRE); lane = 1: roam PA lane paint / track kerb, a trunk keeps ST_LIT_TRUNK_M off it
 * instead of the flat-face ST_KEEP_FLAT_M (TD5RE_ROAM_TREE_LANE_KEEP): the lane is on the carriageway, the tree behind
 * the kerb */
typedef struct { double x[4], z[4], x0, z0, x1, z1, keep, ylo; int n, lit, lane; } StKeep;

#define ST_LIT_TRUNK_M   0.5          /* [integ L] trunk off a shopfront / pool face            */
#define ST_LIT_HIGH_M    2.0          /* [integ L] a face this far over the foot meets the crown */
#define ST_LIT_REACH_M   8.0          /* [integ L] bbox reach of a high face (the widest crown)  */

static int st_keep_mesh(TG_TileCtx *c, int k, int m, int lit, StKeep **kpp, int *np, int *capp);

static int st_keep_collect(TG_TileCtx *c, StKeep **out, int *nout)
{
    StKeep *kp = NULL;
    int n = 0, cap = 0, k, m, i;
    const int lane_keep = c->tv_on && td5_env_int("TD5RE_ROAM_TREE_LANE_KEEP", 1, 0, 1);      /* [integ P] */
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) {
        const ScSb *b = &c->sb[k];
        if (c->furn_first[k] >= b->nmesh) continue;
        for (m = c->furn_first[k]; m < b->nmesh; m++) {
            const int n0 = n;
            if (!st_keep_mesh(c, k, m, 0, &kp, &n, &cap)) { free(kp); return 0; }
            if (lane_keep && n > n0)
                for (i = 0; i < c->xc_n; i++) {
                    const struct ScXcRange *r = &c->xc_r[i];
                    if (r->cat == SC_XC_LANE && r->sb == k && m >= r->m0 && m < r->m1) {
                        int q;
                        for (q = n0; q < n; q++) {
                            const double d = kp[q].keep - ST_M(ST_LIT_TRUNK_M);
                            kp[q].lane = 1; kp[q].keep = ST_M(ST_LIT_TRUNK_M);
                            kp[q].x0 += d; kp[q].z0 += d; kp[q].x1 -= d; kp[q].z1 -= d;
                        }
                        break;
                    }
                }
        }
    }
    if (c->tv_on && td5_env_int("TD5RE_ROAM_RULE_TREE_WIRE", 1, 0, 1))           /* [integ P] rule crowns off catenary */
        for (i = 0; i < c->xc_n; i++) {
            const struct ScXcRange *r = &c->xc_r[i];
            if (r->cat != SC_XC_RAIL) continue;
            for (m = r->m0; m < r->m1; m++)
                if (!st_keep_mesh(c, r->sb, m, 4, &kp, &n, &cap)) { free(kp); return 0; }
        }
    if (c->tv_on) {                                                 /* [integ L] shopfronts, awnings, pools */
        const int gh_veto = td5_env_int("TD5RE_ROAM_TREE_VETO_GH", 1, 0, 1);   /* [integ P] + greenhouse tunnels (lit 1) */
        for (i = 0; i < c->xc_n; i++) {
            const struct ScXcRange *r = &c->xc_r[i];
            if (r->cat != SC_XC_SHOP && r->cat != SC_XC_POOL && r->cat != SC_XC_OUT && !(gh_veto && r->cat == SC_XC_GH)) continue;
            for (m = r->m0; m < r->m1; m++)   /* outskirts (lit 2): only its wires, the crown rule; the rest is tail */
                if (!st_keep_mesh(c, r->sb, m, r->cat == SC_XC_OUT ? 2 : 1, &kp, &n, &cap)) { free(kp); return 0; }
        }
    }
    *out = kp; *nout = n;
    return 1;
}

static int st_keep_mesh(TG_TileCtx *c, int k, int m, int lit, StKeep **kpp, int *np, int *capp)
{
    StKeep *kp = *kpp;
    int n = *np, cap = *capp;
    {
        const ScSb *b = &c->sb[k];
        {
            const uint8_t *rec = b->blk.b + b->moff[m];
            const size_t avail = ((m + 1 < b->nmesh) ? b->moff[m + 1] : b->blk.len) - b->moff[m];
            const int tag = (int)(int16_t)(sc_rd32(rec) >> 16);
            const uint32_t ncmd = sc_rd32(rec + 4), nv = sc_rd32(rec + 8);
            const uint32_t cmd_off = sc_rd32(rec + TD5_MESH_DISK_OFF_COMMANDS), vtx_off = sc_rd32(rec + TD5_MESH_DISK_OFF_VERTICES);
            const float ox = sc_rdf(rec + 28) / 256.0f, oz = sc_rdf(rec + 36) / 256.0f;
            uint32_t ci, cursor = 0;
            if (cmd_off + ncmd * 16u > avail || vtx_off + nv * 44u > avail) return 1;
            for (ci = 0; ci < ncmd; ci++) {
                const uint8_t *cm = rec + cmd_off + ci * 16u;
                const uint32_t tc = (uint32_t)(sc_rd32(cm + 8) & 0xFFFFu), qc = sc_rd32(cm + 8) >> 16;
                uint32_t p;
                for (p = 0; p < tc + qc; p++) {
                    const int nvp = p < tc ? 3 : 4;
                    StKeep q;
                    double ylo = 1e30, yhi = -1e30;
                    int j;
                    if (cursor + (uint32_t)nvp > nv) break;
                    memset(&q, 0, sizeof q);
                    for (j = 0; j < nvp; j++) {
                        const uint8_t *v = rec + vtx_off + (cursor + (uint32_t)j) * 44u;
                        const double vy = (double)sc_rdf(v + 4);
                        q.x[j] = (double)sc_rdf(v) + (tag ? ox : 0.0f);
                        q.z[j] = (double)sc_rdf(v + 8) + (tag ? oz : 0.0f);
                        if (vy < ylo) ylo = vy;
                        if (vy > yhi) yhi = vy;
                    }
                    cursor += (uint32_t)nvp;
                    q.n = nvp;
                    if (tag) { q.x[0] = (double)ox; q.z[0] = (double)oz; q.n = 1; }
                    q.keep = (yhi - ylo < ST_M(ST_FLAT_M)) ? ST_M(ST_KEEP_FLAT_M) : ST_M(ST_KEEP_M);
                    q.ylo = ylo; q.lit = lit;
                    if (lit) q.keep = ST_M(ST_LIT_REACH_M);                 /* bbox only: st_keep_hit judges it */
                    q.x0 = q.x1 = q.x[0]; q.z0 = q.z1 = q.z[0];
                    for (j = 1; j < q.n; j++) {
                        if (q.x[j] < q.x0) q.x0 = q.x[j];
                        if (q.x[j] > q.x1) q.x1 = q.x[j];
                        if (q.z[j] < q.z0) q.z0 = q.z[j];
                        if (q.z[j] > q.z1) q.z1 = q.z[j];
                    }
                    q.x0 -= q.keep; q.z0 -= q.keep; q.x1 += q.keep; q.z1 += q.keep;
                    if (!sc_grow((void **)&kp, &cap, n + 1, sizeof(StKeep))) { *kpp = kp; *capp = cap; return 0; }
                    kp[n++] = q;
                }
            }
        }
    }
    *kpp = kp; *np = n; *capp = cap;
    return 1;
}

/* [integ L] y / crown: the candidate's foot height and crown radius, used by the lit (shop / pool) faces only.
 * [roam PD] stand: a standing (not flat) furniture face keeps at least this far (0 = its own keep) */
static int st_keep_hit(const StKeep *kp, int n, double x, double z, double y, double crown, double stand)
{
    int i, j;
    for (i = 0; i < n; i++) {
        const StKeep *q = &kp[i];
        double d = 1e30, t, keep = q->keep;
        const double more = (!q->lit && !q->lane && stand > keep && keep < ST_M(ST_KEEP_FLAT_M)) ? stand - keep : 0.0;
        if (q->lit == 4 && stand <= 0.0) continue;                 /* [integ P] rails crown rule: rule trees only */
        if (x < q->x0 - more || x > q->x1 + more || z < q->z0 - more || z > q->z1 + more) continue;
        keep += more;
        if (q->lit) keep = (q->ylo >= y + ST_M(ST_LIT_HIGH_M)) ? crown : (q->lit >= 2 ? 0.0 : ST_M(ST_LIT_TRUNK_M));
        if (q->n == 1) d = hypot(x - q->x[0], z - q->z[0]);
        else {
            if (q->n >= 3 && td5_geob_point_in_ring(q->x, q->z, q->n, x, z)) return 1;
            for (j = 0; j < q->n; j++) {
                const int j2 = (j + 1) % q->n;
                const double dd = sc_seg_dist(x, z, q->x[j], q->z[j], q->x[j2], q->z[j2], &t);
                if (dd < d) d = dd;
                if (q->n == 2) break;
            }
        }
        if (d < keep) return 1;
    }
    return 0;
}

/* the trunk is far enough inside the owned tile that no neighbour's furniture can stand within its keep-out */
static int st_inside(const TG_TileCtx *c, double x, double z)
{
    static const double k_d[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
                                      { 0.7071, 0.7071 }, { -0.7071, 0.7071 }, { 0.7071, -0.7071 }, { -0.7071, -0.7071 } };
    int k;
    for (k = 0; k < 8; k++)
        if (!sc_owned_point(c, x + k_d[k][0] * ST_M(ST_BORDER_M), z + k_d[k][1] * ST_M(ST_BORDER_M))) return 0;
    return 1;
}

/* the canopy coverage census: canopy >= 3 m cells of the owned tile, those under a crown of any tree */
static void st_canopy_census(TG_TileCtx *c)
{
    const GrRaster *r = &c->canopy;
    unsigned char *mask;
    int i, a, b;
    if (!c->canopy_ok || !r->data || r->w <= 0 || r->h <= 0 || !(r->cell > 0.0)) return;
    mask = (unsigned char *)calloc((size_t)r->w * (size_t)r->h, 1);
    if (!mask) return;
    for (i = 0; i < c->ntpos; i++) {
        const double X = c->tpos[i * 3] + (double)c->org_x, Z = c->tpos[i * 3 + 1] + (double)c->org_z, R = c->tpos[i * 3 + 2];
        const int a0 = (int)floor((X - R - r->origin_x) / r->cell + 0.5), a1 = (int)floor((X + R - r->origin_x) / r->cell + 0.5);
        const int b0 = (int)floor((Z - R - r->origin_z) / r->cell + 0.5), b1 = (int)floor((Z + R - r->origin_z) / r->cell + 0.5);
        for (b = b0 < 0 ? 0 : b0; b <= b1 && b < r->h; b++)
            for (a = a0 < 0 ? 0 : a0; a <= a1 && a < r->w; a++) {
                const double cx = r->origin_x + (double)a * r->cell, cz = r->origin_z + (double)b * r->cell;
                if (hypot(cx - X, cz - Z) <= R) mask[(size_t)b * (size_t)r->w + (size_t)a] = 1;
            }
    }
    for (b = 0; b < r->h; b++)
        for (a = 0; a < r->w; a++) {
            const unsigned v = ((const unsigned char *)r->data)[(size_t)b * (size_t)r->w + (size_t)a];
            const double x = r->origin_x + (double)a * r->cell - (double)c->org_x, z = r->origin_z + (double)b * r->cell - (double)c->org_z;
            if (r->nodata_raw >= 0 && r->nodata_raw <= 255 && (int)v == r->nodata_raw) continue;
            if ((int)v < SC_CANOPY_M) continue;
            if (!sc_owned_point(c, x, z)) continue;
            c->st.canopy_cells++;
            if (mask[(size_t)b * (size_t)r->w + (size_t)a]) c->st.canopy_cov++;
        }
    free(mask);
}

/* [roam PD] a planted trunk within r of (x, z), tile-local */
static int st_tree_near(const TG_TileCtx *c, double x, double z, double r)
{
    int i;
    for (i = 0; i < c->ntpos; i++)
        if (fabs(c->tpos[i * 3] - x) < r && fabs(c->tpos[i * 3 + 1] - z) < r &&
            hypot(c->tpos[i * 3] - x, c->tpos[i * 3 + 1] - z) < r) return 1;
    return 0;
}

/* [roam PD] the coverage census: the avenue / diagonal kerb samples with a trunk within RT_COVER_M among the first n
 * planted trees, into cov[2] (m). Dev: one ROAM_AVT line per (tile, street name) with TD5RE_ROAM_TREE_LOG=1 */
static void st_kerb_cover(TG_TileCtx *c, int n, double *cov, int which)
{
    const double R = ST_M(RT_COVER_M);
    int k, i;
    for (k = 0; k < c->nrks; k++) {
        const double x = c->rks[k * 4], z = c->rks[k * 4 + 1];
        const int cls = (int)c->rks[k * 4 + 2] & 3;
        for (i = 0; i < n; i++)
            if (fabs(c->tpos[i * 3] - x) < R && fabs(c->tpos[i * 3 + 1] - z) < R &&
                hypot(c->tpos[i * 3] - x, c->tpos[i * 3 + 1] - z) < R) break;
        if (i < n) { cov[cls] += RT_SAMPLE_M; c->rks[k * 4 + 2] = (double)((int)c->rks[k * 4 + 2] | (4 << which)); }
    }
}

/* the candidates, after the furniture: the first along-kerb position clear of every furniture face wins.
 * [roam PD] rule trees (kind 3 / 4) in a second pass, after every other tree: they keep RT_DOUBLE_M off any of them */
static int sc_emit_street_trees(TG_TileCtx *c)
{
    StKeep *kp = NULL;
    int nk = 0, i, k, ok = 1, pass, n_la = 0;
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) c->tree_first[k] = c->sb[k].nmesh;
    if (c->ntcand > 0 && !st_keep_collect(c, &kp, &nk)) return 0;
    for (pass = 0; pass < 2 && ok; pass++) {
        if (pass == 1) n_la = c->ntpos;
        for (i = 0; i < c->ntcand && ok; i++) {
            const struct ScTreeCand *t = &c->tcand[i];
            const int rule = t->kind >= 3, tk = rule ? 3 : t->kind;
            int a, hit = 0, inb = 0, dbl = 0;
            if (rule != pass) continue;
            for (a = 0; a < t->nalt; a++) {
                if (t->kind != 2 && !st_inside(c, t->x[a], t->z[a])) continue;
                if (c->tb_on && st_in_bld(c, t->x[a], t->z[a])) { inb++; continue; }      /* [roam PD] */
                if (rule && st_tree_near(c, t->x[a], t->z[a], ST_M(RT_DOUBLE_M))) { dbl++; continue; }
                if (st_keep_hit(kp, nk, t->x[a], t->z[a], t->y[a], t->hw * ST_CROWN_COVER, rule ? ST_M(RT_FURN_M) : 0.0)) {
                    hit++; continue;
                }
                break;
            }
            if (a >= t->nalt) {
                if (rule) {
                    if (dbl) c->st.rt_double++; else if (hit) c->st.rt_furn++; else if (inb) c->st.tb_drop[3]++;
                    else c->st.rt_border++;
                } else if (inb && !hit) c->st.tb_drop[tk]++;
                else if (t->kind == 2) c->st.pt_furn_drop++;                 /* [integ L] */
                else if (hit) c->st.st_furn_drop++; else c->st.st_border++;
                continue;
            }
            if (inb) c->st.tb_moved[tk]++;
            if (st_in_bld(c, t->x[a], t->z[a])) c->st.tb_left[tk]++;          /* [roam PD] the census (fix off) */
            if (a > 0 && !rule) c->st.st_furn_moved++;
            {
                ScSb *b = sc_sb(c, sc_sb_of(c, t->x[a], t->z[a]), 1);
                if (!b) { ok = 0; break; }
                b->moff[b->nmesh++] = b->blk.len;
                if (!tg_emit_billboard_mesh(&b->blk, t->x[a], t->y[a], t->z[a], t->hw, t->th, tg_tree_slot(t->var), 1)) { ok = 0; break; }
                if (t->kind == 2 && c->xc_on) {                                 /* [integ L] for the cross-check census */
                    if (sc_grow((void **)&c->late_pz, &c->cap_late_pz, (c->nlate_pz + 1) * 2, sizeof(int))) {
                        c->late_pz[c->nlate_pz * 2] = sc_sb_of(c, t->x[a], t->z[a]);
                        c->late_pz[c->nlate_pz * 2 + 1] = b->nmesh - 1;
                        c->nlate_pz++;
                    }
                }
            }
            if (t->kind == 0) c->st.st_band++;
            else if (t->kind == 1) c->st.st_lot++;
            else if (t->kind == 2) { c->st.trees++; c->st.plaza_tree_var[t->var]++; }    /* [integ L] a plaza tree planted late */
            else if (t->kind == 4) c->st.rt_med++;
            else if (t->kind == 5) c->st.rt_face++;
            else if (t->cls == 1) c->st.rt_diag++;
            else c->st.rt_av++;
#ifndef TD5RE_RELEASE
            if (c->tree_log)
                TD5_LOG_I(LOG_TAG, "ROAM_TREE tile %d,%d kind %d at %.0f,%.0f y %.0f hw %.0f th %.0f var %d", c->tx, c->tz,
                          t->kind, t->x[a] + (double)c->org_x, t->z[a] + (double)c->org_z, t->y[a], t->hw, t->th, t->var);
#endif
            if (!st_push_pos(c, t->x[a], t->z[a], t->hw * ST_CROWN_COVER)) { ok = 0; break; }
        }
    }
    free(kp);
    if (c->nrks > 0) {                                                       /* [roam PD] the coverage census */
        st_kerb_cover(c, n_la, c->st.rt_cov0_m, 0);
        st_kerb_cover(c, c->ntpos, c->st.rt_cov1_m, 1);
#ifndef TD5RE_RELEASE
        if (c->tree_log) {   /* per street name: kerb m, with LA's trees, with the rule trees */
            int j;
            for (k = 0; k < c->nrks; k++) {
                const uint32_t nm = (uint32_t)c->rks[k * 4 + 3];
                double L = 0.0, c0 = 0.0, c1 = 0.0;
                int seen = 0;
                for (j = 0; j < k && !seen; j++) if ((uint32_t)c->rks[j * 4 + 3] == nm) seen = 1;
                if (seen) continue;
                for (j = k; j < c->nrks; j++) {
                    const int f = (int)c->rks[j * 4 + 2];
                    if ((uint32_t)c->rks[j * 4 + 3] != nm) continue;
                    L += RT_SAMPLE_M;
                    if (f & 4) c0 += RT_SAMPLE_M;
                    if (f & 8) c1 += RT_SAMPLE_M;
                }
                TD5_LOG_I(LOG_TAG, "ROAM_AVT tile %d,%d name %08x class %d kerb %.0f la %.0f rule %.0f", c->tx, c->tz, nm,
                          (int)c->rks[k * 4 + 2] & 3, L, c0, c1);
            }
        }
#endif
    }
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) c->tree_end[k] = c->sb[k].nmesh;
    st_canopy_census(c);
    return ok;
}

#ifndef TD5RE_RELEASE
/* [integ L] TREE CROSS-CHECK (TD5RE_ROAM_TREE_XCHECK=1, dev census, reads the tile's records, writes no byte).
 * Every tree billboard of the tile (plaza trees from sc_emit_trees, street + lot trees from tree_first..tree_end)
 * against the faces of the round L emitters (shopfronts + awnings, fuel canopies, plaza furniture, rails + level-
 * crossing furniture + catenary, outskirts poles / wires / guard rails, pools):
 *   TRUNK hit: a face that starts lower than SC_XC_LOW_M over the tree's foot comes within SC_XC_TRUNK_M of the trunk;
 *   CROWN hit: a face wholly over SC_XC_HIGH_M (an awning, a canopy deck, a wire) inside the crown's radius (0.8 of
 *              the billboard half width) and inside its height band (the top 65 % of the billboard).
 * Plus WATER: a trunk on a wet WATER.R8 sample (the render's water plane). One hit per tree and category. */
#define SC_XC_TRUNK_M  0.5
#define SC_XC_LOW_M    2.5
#define SC_XC_HIGH_M   2.0

typedef struct { double x[4], z[4], ylo, yhi, x0, z0, x1, z1; int n, cat; } ScXcFace;
typedef struct { double x, z, y0, y1, r; int kind; } ScXcTree;

/* the faces of mesh m of sub-block k (a billboard: its origin) appended to *F, or with tree_kind >= 0 the tree it is */
static int sc_xc_faces(const TG_TileCtx *c, int k, int m, int cat, ScXcFace **F, int *nf, int *cap, ScXcTree **T,
                       int *nt, int *tcap, int tree_kind)
{
    const ScSb *b = &c->sb[k];
    const uint8_t *rec = b->blk.b + b->moff[m];
    const size_t avail = ((m + 1 < b->nmesh) ? b->moff[m + 1] : b->blk.len) - b->moff[m];
    const int tag = (int)(int16_t)(sc_rd32(rec) >> 16);
    const uint32_t ncmd = sc_rd32(rec + 4), nv = sc_rd32(rec + 8);
    const uint32_t cmd_off = sc_rd32(rec + TD5_MESH_DISK_OFF_COMMANDS), vtx_off = sc_rd32(rec + TD5_MESH_DISK_OFF_VERTICES);
    const double ox = sc_rdf(rec + 28) / 256.0, oy = sc_rdf(rec + 32) / 256.0, oz = sc_rdf(rec + 36) / 256.0;
    uint32_t ci, cursor = 0, e;
    if (cmd_off + ncmd * 16u > avail || vtx_off + nv * 44u > avail) return 1;
    if (tree_kind >= 0) {                                          /* a tree billboard: foot, top, half width */
        ScXcTree t;
        double hw = 0.0, top = 0.0;
        if (!tag) return 1;
        for (e = 0; e < nv; e++) {
            const uint8_t *v = rec + vtx_off + e * 44u;
            if (fabs((double)sc_rdf(v)) > hw) hw = fabs((double)sc_rdf(v));
            if ((double)sc_rdf(v + 4) > top) top = (double)sc_rdf(v + 4);
        }
        t.x = ox; t.z = oz; t.y0 = oy; t.y1 = oy + top; t.r = hw * ST_CROWN_COVER; t.kind = tree_kind;
        if (!sc_grow((void **)T, tcap, *nt + 1, sizeof(ScXcTree))) return 0;
        (*T)[(*nt)++] = t;
        return 1;
    }
    for (ci = 0; ci < ncmd; ci++) {
        const uint8_t *cm = rec + cmd_off + ci * 16u;
        const uint32_t tc = (uint32_t)(sc_rd32(cm + 8) & 0xFFFFu), qc = sc_rd32(cm + 8) >> 16;
        uint32_t p;
        for (p = 0; p < tc + qc; p++) {
            const int nvp = p < tc ? 3 : 4;
            ScXcFace q;
            int j;
            if (cursor + (uint32_t)nvp > nv) return 1;
            memset(&q, 0, sizeof q);
            q.ylo = 1e30; q.yhi = -1e30; q.cat = cat; q.n = tag ? 1 : nvp;
            for (j = 0; j < nvp; j++) {
                const uint8_t *v = rec + vtx_off + (cursor + (uint32_t)j) * 44u;
                const double vy = (double)sc_rdf(v + 4) + (tag ? oy : 0.0);
                if (!tag) { q.x[j] = (double)sc_rdf(v); q.z[j] = (double)sc_rdf(v + 8); }
                if (vy < q.ylo) q.ylo = vy;
                if (vy > q.yhi) q.yhi = vy;
            }
            if (tag) { q.x[0] = ox; q.z[0] = oz; }
            cursor += (uint32_t)nvp;
            q.x0 = q.x1 = q.x[0]; q.z0 = q.z1 = q.z[0];
            for (j = 1; j < q.n; j++) {
                if (q.x[j] < q.x0) q.x0 = q.x[j];
                if (q.x[j] > q.x1) q.x1 = q.x[j];
                if (q.z[j] < q.z0) q.z0 = q.z[j];
                if (q.z[j] > q.z1) q.z1 = q.z[j];
            }
            if (!sc_grow((void **)F, cap, *nf + 1, sizeof(ScXcFace))) return 0;
            (*F)[(*nf)++] = q;
        }
    }
    return 1;
}

/* a late-planted record is a plaza tree when its slot is in the plaza-tree mesh list (st_late_plaza) */
static int sc_xc_is_plaza_page(const TG_TileCtx *c, int k, int m)
{
    int i;
    for (i = 0; i < c->nlate_pz; i++) if (c->late_pz[i * 2] == k && c->late_pz[i * 2 + 1] == m) return 1;
    return 0;
}

static double sc_xc_dist(const ScXcFace *q, double x, double z)
{
    double d = 1e30, t;
    int j;
    if (q->n == 1) return hypot(x - q->x[0], z - q->z[0]);
    if (q->n >= 3 && td5_geob_point_in_ring(q->x, q->z, q->n, x, z)) return 0.0;
    for (j = 0; j < q->n; j++) {
        const int j2 = (j + 1) % q->n;
        const double dd = sc_seg_dist(x, z, q->x[j], q->z[j], q->x[j2], q->z[j2], &t);
        if (dd < d) d = dd;
        if (q->n == 2) break;
    }
    return d;
}

/* [integ L] XZ overlap of two faces (1-4 points; a 2-point face is a wall segment, 1 point a billboard foot) */
static int sc_xc_seg_x(double ax, double az, double bx, double bz, double cx, double cz, double dx, double dz)
{
    const double d1 = (bx - ax) * (cz - az) - (bz - az) * (cx - ax), d2 = (bx - ax) * (dz - az) - (bz - az) * (dx - ax);
    const double d3 = (dx - cx) * (az - cz) - (dz - cz) * (ax - cx), d4 = (dx - cx) * (bz - cz) - (dz - cz) * (bx - cx);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

static int sc_xc_overlap(const ScXcFace *a, const ScXcFace *b)
{
    int i, j;
    if (a->x1 < b->x0 || b->x1 < a->x0 || a->z1 < b->z0 || b->z1 < a->z0) return 0;
    if (a->n >= 3) for (i = 0; i < b->n; i++) if (td5_geob_point_in_ring(a->x, a->z, a->n, b->x[i], b->z[i])) return 1;
    if (b->n >= 3) for (i = 0; i < a->n; i++) if (td5_geob_point_in_ring(b->x, b->z, b->n, a->x[i], a->z[i])) return 1;
    if (a->n < 2 || b->n < 2) return 0;
    for (i = 0; i < (a->n == 2 ? 1 : a->n); i++)
        for (j = 0; j < (b->n == 2 ? 1 : b->n); j++)
            if (sc_xc_seg_x(a->x[i], a->z[i], a->x[(i + 1) % a->n], a->z[(i + 1) % a->n],
                            b->x[j], b->z[j], b->x[(j + 1) % b->n], b->z[(j + 1) % b->n])) return 1;
    return 0;
}

static void sc_tree_xcheck(TG_TileCtx *c)
{
    ScXcFace *F = NULL;
    ScXcTree *T = NULL;
    int nf = 0, cap = 0, nt = 0, tcap = 0, i, k, m, f;
    for (i = 0; i < c->xc_n; i++) {
        const struct ScXcRange *r = &c->xc_r[i];
        for (m = r->m0; m < r->m1; m++)
            if (!sc_xc_faces(c, r->sb, m, r->cat, &F, &nf, &cap, &T, &nt, &tcap, r->cat == SC_XC_TREE_PLAZA ? 0 : -1)) goto done;
    }
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++)
        for (m = c->tree_first[k]; m < c->tree_end[k] && m < c->sb[k].nmesh; m++)
            if (!sc_xc_faces(c, k, m, -1, &F, &nf, &cap, &T, &nt, &tcap,
                             sc_xc_is_plaza_page(c, k, m) ? 0 : 1)) goto done;
    for (i = 0; i < nt; i++) {
        const ScXcTree *t = &T[i];
        const double reach = (t->r > SC_XC_TRUNK_M * SC_UPM) ? t->r : SC_XC_TRUNK_M * SC_UPM;
        const double cy0 = t->y0 + 0.35 * (t->y1 - t->y0);
        int hit_t[SC_XC_NCAT], hit_c[SC_XC_NCAT], wet = 0;
        double h;
        memset(hit_t, 0, sizeof hit_t); memset(hit_c, 0, sizeof hit_c);
        c->st.xc_trees[t->kind]++;
        for (f = 0; f < nf; f++) {
            const ScXcFace *q = &F[f];
            double d;
            if (q->cat < 0 || q->cat >= SC_XC_NCAT) continue;
            if (t->x < q->x0 - reach || t->x > q->x1 + reach || t->z < q->z0 - reach || t->z > q->z1 + reach) continue;
            d = sc_xc_dist(q, t->x, t->z);
            if (q->ylo < t->y0 + SC_XC_LOW_M * SC_UPM) {
                if (d < SC_XC_TRUNK_M * SC_UPM) hit_t[q->cat] = 1;
            } else if (q->ylo >= t->y0 + SC_XC_HIGH_M * SC_UPM && q->ylo < t->y1 && q->yhi > cy0 && d < t->r) {
                hit_c[q->cat] = 1;
            }
        }
        for (k = 0; k < SC_XC_NCAT; k++) { c->st.xc_trunk[t->kind][k] += hit_t[k]; c->st.xc_crown[t->kind][k] += hit_c[k]; }
        if (roam_ctx_ground_raw(c, t->x, t->z, &h, &wet) && wet) c->st.xc_wet[t->kind]++;
    }
    /* [integ L] street furniture (LC crossing paint, limit lines, posts) against the rails layer (LF level-crossing posts,
     * arms, boards, masts, the track itself): faces of one overlapping faces of the other in XZ, split by the furniture
     * face being flat paint (zebra / line, under 0.3 m tall) or standing */
    {
        int g;
        for (f = 0; f < nf; f++) {
            const ScXcFace *q = &F[f];
            int flat, any = 0;
            if (q->cat != SC_XC_FURN) continue;
            flat = (q->yhi - q->ylo) < 0.3 * SC_UPM;
            for (g = 0; g < nf && !any; g++)
                if (F[g].cat == SC_XC_RAIL && sc_xc_overlap(q, &F[g])) any = 1 + (F[g].yhi - F[g].ylo >= 0.3 * SC_UPM);
            if (any) { if (flat) c->st.xc_xing_rail++; else c->st.xc_furn_rail++; }
            if (any && flat)
                fprintf(stderr, "ROAM_XC_XING tile %d,%d paint face at place (%.0f,%.0f) over a %s rail face\n", c->tx, c->tz,
                          q->x[0] + (double)c->org_x, q->z[0] + (double)c->org_z, any == 2 ? "standing" : "flat");
        }
    }
done:
    free(F); free(T);
}
#endif

/* The chain through piece `start` (walked, every piece marked done): its vertices, widths and outer points
 * (pv_offset_chain), in *ch, with the piece sequence in *seq_out (m pieces) and the block the arrays live in
 * (*blk_out, kx kz yo gr after ch->mf: ch->nv each). 0 = out of memory (nothing allocated), -1 = the offset failed
 * (caller frees). [roam NA] e->bld NULL is the FRONTAGE view (td5_roam_muni.h): no island by the no-building rule
 * and no clip at a mapped facade, so the outer line is the municipal line itself, kerb + the street's pavement. */
static int pv_chain_geom(PvEmit *e, PvB *B, int start, PvChain *out, int **seq_out, int *m_out, double **blk_out)
{
    TG_TileCtx *c = e->c;
    int *seq = NULL, m = 0, cap = 0, s, closed = 0, k, v;
    PvChain ch;
    double *gr, *blk;
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
    memset(&ch, 0, sizeof ch);
    ch.ns = m;
    ch.nv = closed ? m : m + 1;
    ch.closed = closed;
    ch.join_guard = e->s->pv->join_guard;
    ch.level = e->s->pv->level;
    blk = (double *)calloc((size_t)ch.nv * 15u + (size_t)m * 2u, sizeof(double));
    ch.vf = (unsigned char *)calloc((size_t)ch.nv, 1);
    if (!blk || !ch.vf) { free(blk); free(ch.vf); free(seq); return 0; }
    ch.px = blk; ch.pz = ch.px + ch.nv; ch.py = ch.pz + ch.nv; ch.w = ch.py + ch.nv;
    ch.ox = ch.w + ch.nv; ch.oz = ch.ox + ch.nv; ch.ux = ch.oz + ch.nv; ch.uz = ch.ux + ch.nv; ch.mf = ch.uz + ch.nv;
    gr = ch.mf + 4 * ch.nv;                      /* kx kz yo gr (pv_chain_emit) sit between mf and nx */
    ch.nx = gr + ch.nv; ch.nz = ch.nx + m;
    ch.fy = ch.nz + m;
    for (v = 0; v < ch.nv; v++) ch.fy[v] = -1e30;
    if (e->bld && !e->dry) c->st.pv_chains++;
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
        if (!wk) { free(blk); free(ch.vf); free(seq); return 0; }
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
#ifndef TD5RE_RELEASE
        if (e->s->pv->dbg_r > 0.0 && hypot(ch.px[v] + c->org_x - e->s->pv->dbg_x, ch.pz[v] + c->org_z - e->s->pv->dbg_z) < e->s->pv->dbg_r)
            TD5_LOG_I(LOG_TAG, "PVDBG tile %d,%d v %d p (%.0f,%.0f) u (%.3f,%.3f) w0 %.0f freed %.0f (ray limit %.0f)", c->tx, c->tz,
                      v, ch.px[v] + c->org_x, ch.pz[v] + c->org_z, ch.ux[v], ch.uz[v], w0, freed, lim);
#endif
        if (freed >= 0.0 && !e->bld && freed <= PV_MEDIAN_M * SC_UPM) ch.vf[v] |= PV_VF_GAP;   /* [roam NA] */
        if (freed >= 0.0) {
            int median = 0;
            /* [roam JA] where the slab stops: the MEDIAL line between this kerb and the surface the ray hit */
            const double half = pv_medial(e, &ch, v, freed);
            if (hp && hp->seg >= 0 && freed <= PV_MEDIAN_M * SC_UPM && e->s->pv->median) {
                const int kin = (v > 0) ? v - 1 : (closed ? m - 1 : -1), kout = (v < m) ? v : -1;
                const PvWay *me = (kout >= 0 && B[seq[kout]].way) ? B[seq[kout]].way
                                : (kin >= 0 ? B[seq[kin]].way : NULL);
                const PvWay *ot = pv_find(e->ways, e->nways, (int64_t)(c->edge[c->seg[hp->seg].edge].key >> 16));
                median = me && ot && me != ot && me->name_h && me->name_h == ot->name_h && me->oneway && ot->oneway;
            }
            /* [roam IA] a gap no building stands in (this side's half, probed every 0.5 m) is an island
             * whichever streets bound it; a lawn when the middle of the gap is an OSM green */
            int fill = 0, green = 0;
            if (!median && e->bld && e->s->pv->island_fill && ch.w[v] <= half && freed <= PV_MEDIAN_M * SC_UPM) {
                fill = 1;
                for (d = ch.w[v]; d <= half && fill; d += PV_FACE_STEP)
                    if (pv_in_building(c, e->bld, ch.px[v] + ch.ux[v] * d, ch.pz[v] + ch.uz[v] * d)) fill = 0;
#ifndef TD5RE_RELEASE
                if (e->s->pv->dbg_r > 0.0 && hypot(ch.px[v] + c->org_x - e->s->pv->dbg_x, ch.pz[v] + c->org_z - e->s->pv->dbg_z) < e->s->pv->dbg_r)
                    TD5_LOG_I(LOG_TAG, "PVDBG   v %d fill %d (a building %.0f along the ray)", v, fill, fill ? -1.0 : d);
#endif
                if (fill)
                    green = pv_in_green(c, e->ar, e->nar, ch.px[v] + ch.ux[v] * half, ch.pz[v] + ch.uz[v] * half);
                if (fill && !e->dry && B[seq[v < m ? v : m - 1]].owned) c->st.pv_island_fill++;
            }
            if (median || fill || ch.w[v] > half) {
                ch.w[v] = half;
                ch.vf[v] |= PV_VF_ISLAND;
                if (median && e->s->pv->pair_skip) ch.vf[v] |= PV_VF_PAIR;
                if (ch.level) {   /* [roam JA] the far kerb's road height, just past the ray's hit */
                    const double qx = ch.px[v] + ch.ux[v] * (freed + 2.0 * PV_KERB_IN), qz = ch.pz[v] + ch.uz[v] * (freed + 2.0 * PV_KERB_IN);
                    double fyv;
                    if (roam_ctx_road_y(c, qx, qz, ch.py[v], &fyv)) ch.fy[v] = fyv;
                }
                if ((median || green) && freed > PV_LAWN_MIN_M * SC_UPM) ch.vf[v] |= PV_VF_MEDIAN;
                if (e->bld && !e->dry && B[seq[v < m ? v : m - 1]].owned) c->st.pv_free_clamp++;
            }
        }
        if (ch.w[v] < PV_KERB_IN + pv_skip(&ch, v)) { ch.w[v] = 0.0; continue; }
        /* a mapped building front behind the kerb: the slab stops there (not inside the clearance gap) */
        if (e->bld && e->s->pv->facade && !(ch.vf[v] & (PV_VF_ISLAND | PV_VF_DECK | PV_VF_FLAT)) && ch.w[v] > e->s->gap) {
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
    if (pv_offset_chain(&ch) < 0) { free(blk); free(ch.vf); free(seq); return 0; }
    *out = ch; *seq_out = seq; *m_out = m; *blk_out = blk;
    return 1;
}

static int pv_chain_emit(PvEmit *e, PvB *B, int nb, int start)
{
    TG_TileCtx *c = e->c;
    int *seq = NULL, m = 0, ok = 1, k, v;
    PvChain ch;
    double *kx, *kz, *yo, *gr, *blk = NULL;
    (void)nb;
    if (!pv_chain_geom(e, B, start, &ch, &seq, &m, &blk)) return 0;
    kx = ch.mf + ch.nv; kz = kx + ch.nv; yo = kz + ch.nv; gr = yo + ch.nv;
#ifndef TD5RE_RELEASE
    if (e->s->pv->dbg_r > 0.0)
        for (v = 0; v < ch.nv; v++)
            if (hypot(ch.ox[v] + c->org_x - e->s->pv->dbg_x, ch.oz[v] + c->org_z - e->s->pv->dbg_z) < e->s->pv->dbg_r)
                TD5_LOG_I(LOG_TAG, "PVOUT tile %d,%d v %d kerb (%.0f,%.0f) out (%.0f,%.0f) w %.0f vf %02x", c->tx, c->tz, v,
                          ch.px[v] + c->org_x, ch.pz[v] + c->org_z, ch.ox[v] + c->org_x, ch.oz[v] + c->org_z, ch.w[v], ch.vf[v]);
#endif
    for (v = 0; v < ch.nv; v++) {
        kx[v] = ch.px[v] + ch.ux[v] * PV_KERB_IN * ch.mf[v];
        kz[v] = ch.pz[v] + ch.uz[v] * PV_KERB_IN * ch.mf[v];
    }
    pv_heights(e, &ch, yo, gr);
    {   /* [roam IA] an island loop: the loop itself is the top */
        const int top = pv_island_top(e, &ch, kx, kz);
        if (top < 0) { ok = 0; goto done; }
        e->kerb_only = top;
    }
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
    /* [roam LA] the street trees of this chain (an island loop's top takes none) */
    if ((e->trees || c->rt_casco) && !e->kerb_only && !pv_trees_chain(e, &ch, B, seq, m, kx, kz)) { ok = 0; goto done; }
done:
    e->kerb_only = 0;
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

/* ============================================================================
 * SECTION: municipal line [roam NA] (td5_roam_muni.h)
 *
 * WHY. Mariano, 2026-10-22: "La Plata's building code says every building must stand on the municipal line (linea
 * municipal); in the game buildings can be set back from the sidewalk; normalize this." And on the Plaza Moreno side
 * of the Catedral: "there are buildings on the Cathedral grounds, they should not be there".
 *
 * THE LINE. Before the buildings, every kerb chain of the 3x3 is offset once more in the FRONTAGE view
 * (pv_chain_geom with no footprint set: no island by the no-building rule, no clip at a mapped facade), so its outer
 * line is the kerb moved into the block by the street's own pavement width. Its pieces that are a street front (not
 * an island, a deck, a bore, a median) are the municipal line (c->muni).
 *
 * THE SNAP. Every footprint of the 3x3 near this tile gets one answer (c->mr, by centroid), read by the emitter, the
 * pavement's footprint set (pv_bld_build) and the drop judge (sc_bld_drops) alike: its street walls moved onto the
 * line (td5_muni_snap), or left with the reason. A footprint on the line (moved there or already there) is then
 * cleared only MU_SNAP_GAP off the kerbs instead of the strip's kerb + pavement gap (s->gap, 2.8 m), which used to
 * push a building standing on a 2 m pavement's back edge 0.8 m off it. Left as OSM has them: landmarks, civic and
 * relation buildings, building:parts (round JC's 3D models), fuel sites (LB forecourts), footprints on a park /
 * plaza / school / hospital / sports / rail area (sc_lot_class not BUILD) or in landmark grounds, and the outskirts.
 *
 * THE GROUNDS. A closed block loop of the frontage view holding a MAJOR landmark (a place of worship with its
 * building:parts, the forecourt rule of td5_geo_landmarks.c; or an OSM landmark / civic / relation outline covering
 * TD5RE_ROAM_LM_GROUNDS_PCT (50) % of the block: a palace block) is that landmark's grounds: no Overture / Open Buildings blob and no infill
 * lot stands in it (OSM's own footprints do, the census lists them). The Catedral block has no OSM area at all (no
 * amenity / landuse polygon), so the block is the only outline of its grounds there is.
 * ========================================================================== */
#define MU_BLOCK_M2       400.0   /* a smaller loop is an island (INF_BLOCK_M2)                      */
#define MU_JUDGE_M        80.0    /* footprints this far past the tile get an answer (PV_BLD_JUDGE_M) */
#define MU_REACH_M        20.0    /* walls are looked for this far either side of the line           */
#define MU_TOL_M          0.05
#define MU_INSET          9.0     /* units (2 cm): a moved wall stands this far behind the line       */
#define MU_MIN_WALL_M     1.0

static int sc_mr_cmp(const void *pa, const void *pb)
{
    const ScMuniRing *a = (const ScMuniRing *)pa, *b = (const ScMuniRing *)pb;
    if (a->cx != b->cx) return a->cx < b->cx ? -1 : 1;
    if (a->cz != b->cz) return a->cz < b->cz ? -1 : 1;
    /* MEASURED (tile 6,44): a relation outline and the OSM way drawn on the same ring share the centroid exactly */
    return a->id < b->id ? -1 : (a->id > b->id);
}

static const ScMuniRing *sc_muni_find(const TG_TileCtx *c, double id, double cx, double cz)
{
    ScMuniRing key;
    if (!c->muni_ready || c->nmr <= 0) return NULL;
    key.cx = cx; key.cz = cz; key.id = id;
    return (const ScMuniRing *)bsearch(&key, c->mr, (size_t)c->nmr, sizeof(ScMuniRing), sc_mr_cmp);
}

/* a landmark's grounds hold the tile-local point */
static int sc_in_grounds(const TG_TileCtx *c, double x, double z)
{
    int i;
    for (i = 0; i < c->ngrd; i++) {
        const ScGround *g = &c->grd[i];
        if (x < g->x0 || x > g->x1 || z < g->z0 || z > g->z1) continue;
        if (td5_geob_point_in_ring(&c->gpool[g->first], &c->gpool[g->first + g->n], g->n, x, z)) return 1;
    }
    return 0;
}

/* why a footprint keeps its real position (0 = it follows the line) */
enum { MU_SKIP_NONE = 0, MU_SKIP_LM, MU_SKIP_FUEL, MU_SKIP_AREA, MU_SKIP_OUT };
static int sc_muni_skip(TD5_RoamScnSource *s, TG_TileCtx *c, const TD5_GeoBuilding *gb, double id)
{
    int tx, tz, cat = TD5_GEOB_POI_NONE, from_poi = 0;
    if (gb->landmark || gb->part || gb->rel || gb->civic || gb->lmkind != TD5_GEOB_LMK_NONE) return MU_SKIP_LM;
    roam_grid_tile_of(c->grid, gb->cx, gb->cz, &tx, &tz);
    if (!sc_casco_has(s, tx, tz)) return MU_SKIP_OUT;
    if (gb->poi_cat == TD5_GEOB_POI_FUEL) return MU_SKIP_FUEL;
    if (s->shops && td5_roam_shops_category(s->shops, id, gb, &cat, &from_poi) && cat == TD5_GEOB_POI_FUEL) return MU_SKIP_FUEL;
    if (sc_lot_class_at(c->lotp, c->nlotp, gb->cx, gb->cz) != SC_LC_BUILD) return MU_SKIP_AREA;
    if (sc_in_grounds(c, gb->cx - (double)c->org_x, gb->cz - (double)c->org_z)) return MU_SKIP_AREA;
    return MU_SKIP_NONE;
}

/* the area a wall sweeps toward the line meets another footprint, or a mapped footway crossing it (a passage
 * between two buildings: MEASURED, a snapped wall closed a 1.5 m footpath slab at tile 18,55) (tile-local quad) */
typedef struct { TG_TileCtx *c; const PvBld *pb; const double *self; ScCache **fc; int nfc; } ScMuniU;

static int sc_muni_footway(const ScMuniU *u, const double *X, const double *Z)   /* place-frame quad, q0 -> q1 = the wall */
{
    const double wx = X[1] - X[0], wz = Z[1] - Z[0], wl = hypot(wx, wz);
    double x0 = X[0], x1 = X[0], z0 = Z[0], z1 = Z[0];
    int l, i, k, j;
    if (!(wl > 1.0)) return 0;
    for (k = 1; k < 4; k++) { if (X[k] < x0) x0 = X[k]; if (X[k] > x1) x1 = X[k]; if (Z[k] < z0) z0 = Z[k]; if (Z[k] > z1) z1 = Z[k]; }
    for (l = 0; l < u->nfc; l++)
        for (i = 0; i < u->fc[l]->n; i++) {
            const ScFoot *f = &u->fc[l]->f[i];
            const double *px = &u->fc[l]->px[f->first], *pz = &u->fc[l]->pz[f->first];
            const double hw = 0.5 * (f->width_m > 0.0 ? f->width_m * TD5_TG_INFRA_M : SC_FP_DEF_W);
            if (!((1u << f->kind) & SC_FP_KIND_MASK) || f->structure) continue;
            if (f->maxx + hw < x0 || f->minx - hw > x1 || f->maxz + hw < z0 || f->minz - hw > z1) continue;
            for (j = 0; j + 1 < f->count; j++) {
                const double dx = px[j + 1] - px[j], dz = pz[j + 1] - pz[j], dl = hypot(dx, dz);
                double sx[4], sz[4];
                if (!(dl > 1.0) || fabs((dx * wx + dz * wz) / (dl * wl)) > 0.7071) continue;   /* along the wall: a sidewalk */
                /* the footway's band as a quad, against the swept quad */
                sx[0] = px[j] - dz / dl * hw; sz[0] = pz[j] + dx / dl * hw;
                sx[1] = px[j + 1] - dz / dl * hw; sz[1] = pz[j + 1] + dx / dl * hw;
                sx[2] = px[j + 1] + dz / dl * hw; sz[2] = pz[j + 1] - dx / dl * hw;
                sx[3] = px[j] + dz / dl * hw; sz[3] = pz[j] - dx / dl * hw;
                if (sc_poly_overlap(sx, sz, 4, X, Z, 4)) return 1;
            }
        }
    return 0;
}
/* [roam PE] Mariano's open item from round N: NA put the line at kerb + pavement width on NC's cutting spans too, where
 * the cut has no sidewalk (pv_gather w = 0) and the wall + coping apron stand, so 3 footprints by Avenida 32 were
 * moved onto NC's at-grade band. A polygon (tile-local, n points: its corners, edge midpoints and centre sampled)
 * reaching a cutting ZONE (the band widened by the coping apron, TD5_ROAM_CUT_APRON) is off the line. */
static int sc_muni_in_cut_z(const TD5_RoamCutBands *z, const TG_TileCtx *c, const double *x, const double *y, int n)
{
    double cx = 0.0, cz = 0.0;
    int k;
    if (z->n <= 0 || n <= 0) return 0;
    for (k = 0; k < n; k++) {
        const int j = (k + 1) % n;
        if (td5_roam_cut_inside(z, x[k] + (double)c->org_x, y[k] + (double)c->org_z)) return 1;
        if (td5_roam_cut_inside(z, 0.5 * (x[k] + x[j]) + (double)c->org_x, 0.5 * (y[k] + y[j]) + (double)c->org_z)) return 1;
        cx += x[k]; cz += y[k];
    }
    return td5_roam_cut_inside(z, cx / n + (double)c->org_x, cz / n + (double)c->org_z);
}
static int sc_muni_in_cut(const TG_TileCtx *c, const double *x, const double *z, int n)
{
    return sc_muni_in_cut_z(&c->mcut_zone, c, x, z, n);
}

/* [roam PE] census: does an emitted ring (tile-local) overlap a band / zone quad (exact polygon test) */
static int sc_ring_over_cut(const TD5_RoamCutBands *b, const TG_TileCtx *c, const double *rx, const double *rz, int n)
{
    double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
    int i, k;
    for (k = 0; k < n; k++) {
        const double X = rx[k] + (double)c->org_x, Z = rz[k] + (double)c->org_z;
        if (X < x0) x0 = X;
        if (X > x1) x1 = X;
        if (Z < z0) z0 = Z;
        if (Z > z1) z1 = Z;
    }
    for (i = 0; i < b->n; i++) {
        const double *q = &b->q[i * 12];
        double qx[4], qz[4];
        if (q[10] < x0 || q[8] > x1 || q[11] < z0 || q[9] > z1) continue;
        for (k = 0; k < 4; k++) { qx[k] = q[k * 2] - (double)c->org_x; qz[k] = q[k * 2 + 1] - (double)c->org_z; }
        if (sc_poly_overlap(rx, rz, n, qx, qz, 4)) return 1;
    }
    return 0;
}

static int sc_muni_blocked(void *uv, const double *qx, const double *qz)
{
    const ScMuniU *u = (const ScMuniU *)uv;
    const PvBld *pb = u->pb;
    double X[4], Z[4], x0, x1, z0, z1, in[4], iz[4], cx = 0.0, cz = 0.0;
    int k, a, b, a0, a1, b0, b1;
    if (u->c->mcut_skip && sc_muni_in_cut(u->c, qx, qz, 4)) return 1;   /* [roam PE] */
    if (!pb->start) return 0;
    for (k = 0; k < 4; k++) { cx += 0.25 * qx[k]; cz += 0.25 * qz[k]; }
    for (k = 0; k < 4; k++) {   /* 10 cm in: a neighbour wall flush with this one's side is no block */
        const double dx = cx - qx[k], dz = cz - qz[k], l = hypot(dx, dz);
        const double t = l > 86.0 ? 43.0 / l : 0.5;
        in[k] = qx[k] + dx * t; iz[k] = qz[k] + dz * t;
        X[k] = in[k] + (double)u->c->org_x; Z[k] = iz[k] + (double)u->c->org_z;
    }
    x0 = x1 = X[0]; z0 = z1 = Z[0];
    for (k = 1; k < 4; k++) { if (X[k] < x0) x0 = X[k]; if (X[k] > x1) x1 = X[k]; if (Z[k] < z0) z0 = Z[k]; if (Z[k] > z1) z1 = Z[k]; }
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
            if (r->px == u->self || r->x1 < x0 || r->x0 > x1 || r->z1 < z0 || r->z0 > z1) continue;
            if (sc_poly_overlap(r->px, r->pz, r->n, X, Z, 4)) return 1;
        }
    }
    return u->nfc > 0 && sc_muni_footway(u, X, Z);
}

static int sc_muni_opt(const TD5_RoamScnSource *s, TD5_MuniOpt *o)
{
    memset(o, 0, sizeof *o);
    o->snap_max = s->muni_max;
    o->protrude_max = s->muni_protrude;
    o->tol = MU_TOL_M * s->upm;
    o->inset = MU_INSET;
    o->ochava_leg = s->ochava;
    o->min_wall = MU_MIN_WALL_M * s->upm;
    o->reach = MU_REACH_M * s->upm;
    return 1;
}

/* a closed loop of the frontage view: the grounds of the major landmark it holds, if any */
static int sc_muni_block(TD5_RoamScnSource *s, TG_TileCtx *c, ScCache **bc, int nbc, const PvChain *ch)
{
    const double U = s->upm;
    const int pct = td5_env_int("TD5RE_ROAM_LM_GROUNDS_PCT", 50, 1, 100);
    const int min_parts = td5_env_int("TD5RE_GEO_LM_FORECOURT_PARTS", 3, 1, 64);
    double *px, *pz, area, x0, z0, x1, z1;
    int n = 0, v, dx, dz, l, j, ok = 1, hit = 0;
    double lm_id = 0.0;
    px = (double *)malloc((size_t)ch->nv * 2u * sizeof(double));
    if (!px) return 0;
    pz = px + ch->nv;
    for (v = 0; v < ch->nv; v++) {
        if (n > 0 && hypot(ch->ox[v] - px[n - 1], ch->oz[v] - pz[n - 1]) < 20.0) continue;
        px[n] = ch->ox[v]; pz[n] = ch->oz[v]; n++;
    }
    if (n > 1 && hypot(px[n - 1] - px[0], pz[n - 1] - pz[0]) < 20.0) n--;
    area = n >= 3 ? td5_geob_ring_area(px, pz, n) : 0.0;
    if (!(area < -MU_BLOCK_M2 * U * U)) { free(px); return 1; }    /* the road on the left: a block is clockwise */
    x0 = x1 = px[0]; z0 = z1 = pz[0];
    for (v = 1; v < n; v++) {
        if (px[v] < x0) x0 = px[v];
        if (px[v] > x1) x1 = px[v];
        if (pz[v] < z0) z0 = pz[v];
        if (pz[v] > z1) z1 = pz[v];
    }
    for (dz = -1; dz <= 1 && !hit; dz++)
        for (dx = -1; dx <= 1 && !hit; dx++) {
            const int64_t key = sc_tkey(c->tx + dx, c->tz + dz);
            for (l = 0; l < nbc && !hit; l++) {
                int lo, hi;
                sc_owned_range(bc[l], key, &lo, &hi);
                for (j = lo; j < hi && !hit; j++) {
                    const int ri = bc[l]->order[j];
                    const TD5_GeoBuilding *gb = &bc[l]->b[ri];
                    const double *gx = &bc[l]->px[gb->first], *gz = &bc[l]->pz[gb->first];
                    const double lx = gb->cx - (double)c->org_x, lz = gb->cz - (double)c->org_z;
                    double share;
                    int parts = 0, inside = 0, k, l2, j2;
                    if (gb->n < 3 || gb->part || !(gb->landmark || gb->civic || gb->rel)) continue;
                    if (gb->fsrc != TD5_GEOB_FSRC_OSM && !gb->rel) continue;   /* a blob Overture calls civic is no landmark */
                    if (lx < x0 || lx > x1 || lz < z0 || lz > z1) continue;
                    if (td5_geob_point_in_ring(px, pz, n, lx, lz)) inside = 1;
                    else {
                        int vin = 0;
                        for (k = 0; k < gb->n; k++)
                            vin += td5_geob_point_in_ring(px, pz, n, gx[k] - (double)c->org_x, gz[k] - (double)c->org_z);
                        inside = vin * 4 >= gb->n;
                    }
                    if (!inside) continue;
                    if (gb->lmkind == TD5_GEOB_LMK_WORSHIP)       /* its building:parts (the forecourt rule) */
                        for (l2 = 0; l2 < nbc; l2++)
                            for (j2 = 0; j2 < bc[l2]->n; j2++) {
                                const TD5_GeoBuilding *p = &bc[l2]->b[j2];
                                if (!p->part || hypot(p->cx - gb->cx, p->cz - gb->cz) > gb->radius) continue;
                                if (td5_geob_point_in_ring(gx, gz, gb->n, p->cx, p->cz)) parts++;
                            }
                    share = 100.0 * gb->area_m2 / (-area / (U * U));
                    TD5_LOG_I(LOG_TAG, "ROAM_GROUNDS tile %d,%d block %.0f m2 holds id %.0f kind %d civic %d rel %d area %.0f m2 "
                              "(%.0f %% of it) parts %d: %s", c->tx, c->tz, -area / (U * U), bc[l]->id[ri], (int)gb->lmkind,
                              (int)gb->civic, (int)gb->rel, gb->area_m2, share, parts,
                              (gb->lmkind == TD5_GEOB_LMK_WORSHIP && parts >= min_parts) || share >= pct ? "GROUNDS" : "no");
                    if ((gb->lmkind == TD5_GEOB_LMK_WORSHIP && parts >= min_parts) || share >= pct) { hit = 1; lm_id = bc[l]->id[ri]; }
                }
            }
        }
    if (hit) {
        ScGround *g;
        if (!sc_grow((void **)&c->grd, &c->cap_grd, c->ngrd + 1, sizeof(ScGround)) ||
            !sc_grow((void **)&c->gpool, &c->cap_gp, c->ngp + 2 * n, sizeof(double))) ok = 0;
        else {
            g = &c->grd[c->ngrd++];
            g->first = c->ngp; g->n = n; g->x0 = x0; g->z0 = z0; g->x1 = x1; g->z1 = z1; g->lm_id = lm_id;
            memcpy(&c->gpool[c->ngp], px, (size_t)n * sizeof(double));
            memcpy(&c->gpool[c->ngp + n], pz, (size_t)n * sizeof(double));
            c->ngp += 2 * n;
        }
    }
    free(px);
    return ok;
}

/* After sc_pv_prepare (the kerb pieces) and before the buildings: the line, the grounds, every footprint's answer */
static int sc_muni_prepare(TD5_RoamScnSource *s, TG_TileCtx *c, ScCache **bc, int nbc, const ScArea *ar, int nar,
                           ScCache **fc, int nfc)
{
    PvB *B = (PvB *)c->pv_B;
    const int nb = c->pv_nb;
    PvEmit e;
    int i, chain = 0, ok = 0;
    c->muni_ready = 0; c->nmr = 0; c->nmp = 0; c->ngrd = 0; c->ngp = 0;
    td5_roam_cut_bands_free(&c->mcut_band); td5_roam_cut_bands_free(&c->mcut_zone);
    if (!(s->muni_on || s->muni_census || s->grounds_on) || !c->pv_ready) return 1;
    if (td5_roam_cut_on() && c->hc) {           /* [roam PE] the cutting bands over the ctx window */
        const double X0 = c->gx0 + (double)c->org_x, Z0 = c->gz0 + (double)c->org_z;
        const double X1 = X0 + (double)c->gnx * SC_CELL, Z1 = Z0 + (double)c->gnz * SC_CELL, m = TD5_ROAM_CUT_APRON + 1.0;
        if (!td5_roam_cut_bands(&c->mcut_band, c->road, c->hc, X0 - m, Z0 - m, X1 + m, Z1 + m) ||
            !td5_roam_cut_bands(&c->mcut_zone, c->road, c->hc, X0 - m, Z0 - m, X1 + m, Z1 + m)) return 0;
        td5_roam_cut_bands_widen(&c->mcut_zone, TD5_ROAM_CUT_APRON);
    }
    c->mcut_skip = s->muni_cut && c->mcut_zone.n > 0;   /* the knob off: the line ignores them (the census still reads them) */
    if (!c->muni && !(c->muni = td5_muni_new())) return 0;
    td5_muni_reset(c->muni);
    memset(&e, 0, sizeof e);
    e.s = s; e.c = c; e.ways = (PvWay *const *)c->pv_ways; e.nways = c->pv_nways;   /* e.bld NULL: the frontage view */
    for (i = 0; i < nb; i++) {
        PvChain ch;
        int *seq = NULL, m = 0, k;
        double *blk = NULL;
        if (B[i].done) continue;
        if (!pv_chain_geom(&e, B, i, &ch, &seq, &m, &blk)) goto out;
        for (k = 0; k < ch.ns; k++) {
            const int a = k, b = (k + 1) % ch.nv;
            TD5_MuniSeg g;
            if ((ch.vf[a] | ch.vf[b]) & (PV_VF_ISLAND | PV_VF_DECK | PV_VF_FLAT | PV_VF_MEDIAN | PV_VF_GAP)) continue;
            if (!(ch.w[a] > 0.0) || !(ch.w[b] > 0.0)) continue;
            if (hypot(ch.ox[b] - ch.ox[a], ch.oz[b] - ch.oz[a]) < 4.0) continue;
            g.ax = ch.ox[a]; g.az = ch.oz[a]; g.bx = ch.ox[b]; g.bz = ch.oz[b];
            if (c->mcut_skip) {                      /* [roam PE] no line over a cutting (its kerb or its line point there) */
                const double lx[4] = { ch.px[a], ch.px[b], ch.ox[b], ch.ox[a] }, lz[4] = { ch.pz[a], ch.pz[b], ch.oz[b], ch.oz[a] };
                if (sc_muni_in_cut(c, lx, lz, 4)) {
                    c->st.muni_cut_skip++;
                    if (s->bld_log)                  /* TD5RE_ROAM_BLD_LOG=1: which pieces the cutting took */
                        TD5_LOG_I(LOG_TAG, "ROAM_MUNI_CUT tile %d,%d piece (%.0f,%.0f)-(%.0f,%.0f) w %.0f %.0f vf %02x %02x", c->tx,
                                  c->tz, g.ax + c->org_x, g.az + c->org_z, g.bx + c->org_x, g.bz + c->org_z, ch.w[a], ch.w[b],
                                  ch.vf[a], ch.vf[b]);
                    continue;
                }
            }
            {   /* the OUTER line's own normal: a piece whose two ends have different widths is tilted against its kerb */
                const double dx = g.bx - g.ax, dz = g.bz - g.az, l = hypot(dx, dz);
                g.nx = dz / l; g.nz = -dx / l;
                if (g.nx * ch.nx[k] + g.nz * ch.nz[k] < 0.0) { g.nx = -g.nx; g.nz = -g.nz; }
            }
            g.chain = chain;
            if (!td5_muni_add(c->muni, &g)) { free(blk); free(ch.vf); free(seq); goto out; }
#ifndef TD5RE_RELEASE
            if (s->pv->dbg_r > 0.0 && hypot(g.ax + c->org_x - s->pv->dbg_x, g.az + c->org_z - s->pv->dbg_z) < s->pv->dbg_r)
                TD5_LOG_I(LOG_TAG, "MUNIDBG tile %d,%d chain %d piece %d (%.0f,%.0f)-(%.0f,%.0f) n (%.3f,%.3f) w %.0f %.0f kerb (%.0f,%.0f) "
                          "vf %02x %02x", c->tx, c->tz, chain, k, g.ax + c->org_x, g.az + c->org_z, g.bx + c->org_x, g.bz + c->org_z,
                          g.nx, g.nz, ch.w[a], ch.w[b], ch.px[a] + c->org_x, ch.pz[a] + c->org_z, ch.vf[a], ch.vf[b]);
#endif
        }
        if (s->grounds_on && ch.closed && !sc_muni_block(s, c, bc, nbc, &ch)) { free(blk); free(ch.vf); free(seq); goto out; }
        chain++;
        free(blk); free(ch.vf); free(seq);
    }
    for (i = 0; i < nb; i++) B[i].done = 0;   /* the pavement walks the chains again */
    if (!td5_muni_index(c->muni, c->gx0, c->gz0, c->gx0 + (double)c->gnx * SC_CELL, c->gz0 + (double)c->gnz * SC_CELL,
                        SC_CELL)) goto out;
    c->st.muni_segs = td5_muni_count(c->muni);
    c->st.grounds = c->ngrd;
    if (s->muni_on) {
        /* every footprint of the 3x3 within MU_JUDGE_M of the tile: its answer, against the others' real rings */
        PvBld pb;
        ScMuniU u;
        TD5_MuniOpt o;
        const double T = (double)c->grid->tile, J = MU_JUDGE_M * SC_UPM;
        int dx, dz, l, j;
        if (!pv_bld_build(s, c, bc, nbc, ar, nar, &pb, 2)) goto out;
        sc_muni_opt(s, &o);
        u.c = c; u.pb = &pb; u.self = NULL; u.fc = fc; u.nfc = nfc;
        o.blocked = sc_muni_blocked; o.u = &u;
        for (dz = -1; dz <= 1; dz++)
            for (dx = -1; dx <= 1; dx++) {
                const int64_t key = sc_tkey(c->tx + dx, c->tz + dz);
                for (l = 0; l < nbc; l++) {
                    int lo, hi;
                    sc_owned_range(bc[l], key, &lo, &hi);
                    for (j = lo; j < hi; j++) {
                        const int ri = bc[l]->order[j];
                        const TD5_GeoBuilding *gb = &bc[l]->b[ri];
                        const double *gx = &bc[l]->px[gb->first], *gz = &bc[l]->pz[gb->first];
                        double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], ox[TD5_GEOB_RING_MAX], oz[TD5_GEOB_RING_MAX], uu, vv;
                        int k, on = 0, skip, fate;
                        TD5_MuniRes res;
                        ScMuniRing *mr;
                        if (gb->n < 3 || gb->n > TD5_GEOB_RING_MAX || gb->part) continue;
                        roam_grid_uv(c->grid, gb->cx, gb->cz, &uu, &vv);
                        if (!(uu > (double)c->tx * T - J - gb->radius && uu < (double)(c->tx + 1) * T + J + gb->radius &&
                              vv > (double)c->tz * T - J - gb->radius && vv < (double)(c->tz + 1) * T + J + gb->radius)) continue;
                        skip = sc_muni_skip(s, c, gb, bc[l]->id[ri]);
                        if (skip) fate = 16 + skip;
                        else {
                            for (k = 0; k < gb->n; k++) { rx[k] = gx[k] - (double)c->org_x; rz[k] = gz[k] - (double)c->org_z; }
                            u.self = gx;
                            fate = td5_muni_snap(c->muni, &o, rx, rz, gb->n, ox, oz, &on, TD5_GEOB_RING_MAX, &res);
                            if (fate == TD5_MUNI_SNAPPED) {       /* the emitter's test, on the values it will read */
                                double ex[TD5_GEOB_RING_MAX], ez[TD5_GEOB_RING_MAX];
                                for (k = 0; k < on; k++) {
                                    ex[k] = (ox[k] + (double)c->org_x) - (double)c->org_x;
                                    ez[k] = (oz[k] + (double)c->org_z) - (double)c->org_z;
                                }
                                if (!td5_geob_ring_simple(ex, ez, on)) fate = TD5_MUNI_RING;
                            }
                            if (fate == TD5_MUNI_SNAPPED || fate == TD5_MUNI_ON) {
                                /* on the line it is kept only MU_SNAP_GAP off the kerbs: a ring that needs any shift off
                                 * the drivable surface there (a junction patch, a drive the line does not front) keeps its
                                 * OSM position and the old clearance instead */
                                double cxr[TD5_GEOB_RING_MAX], czr[TD5_GEOB_RING_MAX], sh = 0.0;
                                const int nn = fate == TD5_MUNI_SNAPPED ? on : gb->n;
                                int inr = 1;
                                ScBldU bu;
                                for (k = 0; k < nn; k++) {
                                    cxr[k] = fate == TD5_MUNI_SNAPPED ? ox[k] : rx[k];
                                    czr[k] = fate == TD5_MUNI_SNAPPED ? oz[k] : rz[k];
                                    if (!sc_in_reach(c, cxr[k], czr[k])) inr = 0;
                                }
                                memset(&bu, 0, sizeof bu);
                                bu.c = c;
                                if (inr && (sc_bld_clear(c, &bu, MU_SNAP_GAP, cxr, czr, nn, &sh, 0) != 0 || sh > 0.0))
                                    fate = TD5_MUNI_ROAD;
                            }
                        }
                        if (!sc_grow((void **)&c->mr, &c->cap_mr, c->nmr + 1, sizeof(ScMuniRing))) { pv_bld_free(&pb); goto out; }
                        mr = &c->mr[c->nmr++];
                        mr->id = bc[l]->id[ri]; mr->cx = gb->cx; mr->cz = gb->cz; mr->fate = fate; mr->first = 0; mr->n = 0;
                        mr->fwd = mr->back = mr->corners = mr->ochavas = 0;
                        if (fate == TD5_MUNI_SNAPPED) {
                            mr->fwd = (unsigned char)(res.fwd < 255 ? res.fwd : 255);
                            mr->back = (unsigned char)(res.back < 255 ? res.back : 255);
                            mr->corners = (unsigned char)(res.corners < 255 ? res.corners : 255);
                            mr->ochavas = (unsigned char)(res.ochavas < 255 ? res.ochavas : 255);
                        }
                        if (fate == TD5_MUNI_SNAPPED) {
                            if (!sc_grow((void **)&c->mpool, &c->cap_mp, c->nmp + 2 * on, sizeof(double))) { pv_bld_free(&pb); goto out; }
                            mr->first = c->nmp; mr->n = on;                 /* x then z */
                            for (k = 0; k < on; k++) c->mpool[c->nmp + k] = ox[k] + (double)c->org_x;
                            for (k = 0; k < on; k++) c->mpool[c->nmp + on + k] = oz[k] + (double)c->org_z;
                            c->nmp += 2 * on;
                        }
#ifndef TD5RE_RELEASE
                        if (s->bld_log && sc_tkey(c->tx, c->tz) == key) {   /* TD5RE_ROAM_BLD_LOG=1: the answer, the moved ring */
                            char pts[1400];
                            int o2 = 0;
                            pts[0] = '\0';
                            for (k = 0; fate == TD5_MUNI_SNAPPED && k < on && o2 < (int)sizeof pts - 40; k++)
                                o2 += snprintf(pts + o2, sizeof pts - (size_t)o2, " %.0f,%.0f", ox[k] + (double)c->org_x,
                                               oz[k] + (double)c->org_z);
                            TD5_LOG_I(LOG_TAG, "ROAM_MUNI tile %d,%d id %.0f c (%.0f,%.0f) fate %d walls %d fwd %d back %d corners %d "
                                      "ochavas %d:%s", c->tx, c->tz, bc[l]->id[ri], gb->cx, gb->cz, fate, skip ? 0 : res.walls,
                                      skip ? 0 : res.fwd, skip ? 0 : res.back, skip ? 0 : res.corners, skip ? 0 : res.ochavas, pts);
                        }
#endif
                    }
                }
            }
        pv_bld_free(&pb);
        qsort(c->mr, (size_t)c->nmr, sizeof(ScMuniRing), sc_mr_cmp);
        /* SECOND PASS. The pavement the tile will draw clips at every footprint that stands in front of the line, and
         * a clipped vertex can fold a corner otherwise than the frontage view folded it, so a footprint moved onto
         * the frontage line can still meet the drawn slab there (MEASURED: 598 of 25,400 casco snaps, 155 m of wall
         * 0.6 m deep). The chains are walked once more exactly as sc_emit_pavement will (the snapped rings, the clip,
         * the island fill), and every answer on the line not wholly behind that DRAWN line keeps its OSM position and
         * the old clearance (TD5_MUNI_FRONT). Reverting one can move the drawn line again near it: not iterated. */
        c->muni_ready = 1;
        if (td5_env_int("TD5RE_ROAM_MUNI_PASS2", 0, 0, 1)) {   /* default off: +30 ms a tile for 8 casco footprints */
        if (!pv_bld_build(s, c, bc, nbc, ar, nar, &pb, 2)) goto out;
        if (!c->muni2 && !(c->muni2 = td5_muni_new())) { pv_bld_free(&pb); goto out; }
        td5_muni_reset(c->muni2);
        {
            PvEmit e2;
            memset(&e2, 0, sizeof e2);
            e2.s = s; e2.c = c; e2.ways = (PvWay *const *)c->pv_ways; e2.nways = c->pv_nways; e2.bld = &pb;
            e2.ar = ar; e2.nar = nar; e2.dry = 1;
            for (i = 0; i < nb; i++) {
                PvChain ch;
                int *seq = NULL, m = 0, k;
                double *blk = NULL;
                if (B[i].done) continue;
                if (!pv_chain_geom(&e2, B, i, &ch, &seq, &m, &blk)) { pv_bld_free(&pb); goto out; }
                for (k = 0; k < ch.ns; k++) {
                    const int a = k, b = (k + 1) % ch.nv;
                    TD5_MuniSeg g;
                    double dx, dz, l;
                    if ((ch.vf[a] | ch.vf[b]) & (PV_VF_ISLAND | PV_VF_DECK | PV_VF_FLAT | PV_VF_MEDIAN)) continue;
                    if (!(ch.w[a] > 0.0) || !(ch.w[b] > 0.0)) continue;
                    dx = ch.ox[b] - ch.ox[a]; dz = ch.oz[b] - ch.oz[a]; l = hypot(dx, dz);
                    if (l < 4.0) continue;
                    g.ax = ch.ox[a]; g.az = ch.oz[a]; g.bx = ch.ox[b]; g.bz = ch.oz[b];
                    g.nx = dz / l; g.nz = -dx / l;
                    if (g.nx * ch.nx[k] + g.nz * ch.nz[k] < 0.0) { g.nx = -g.nx; g.nz = -g.nz; }
                    g.chain = i;
                    if (!td5_muni_add(c->muni2, &g)) { free(blk); free(ch.vf); free(seq); pv_bld_free(&pb); goto out; }
                }
                free(blk); free(ch.vf); free(seq);
            }
            for (i = 0; i < nb; i++) B[i].done = 0;
        }
        pv_bld_free(&pb);
        if (!td5_muni_index(c->muni2, c->gx0, c->gz0, c->gx0 + (double)c->gnx * SC_CELL, c->gz0 + (double)c->gnz * SC_CELL,
                            SC_CELL)) goto out;
        for (i = 0; i < c->nmr; i++) {
            ScMuniRing *mr = &c->mr[i];
            const TD5_GeoBuilding *gb = NULL;
            double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX];
            int k, n = 0;
            if (mr->fate != TD5_MUNI_SNAPPED && mr->fate != TD5_MUNI_ON) continue;
            if (mr->fate == TD5_MUNI_SNAPPED) {
                n = mr->n;
                for (k = 0; k < n; k++) { rx[k] = c->mpool[mr->first + k] - (double)c->org_x; rz[k] = c->mpool[mr->first + n + k] - (double)c->org_z; }
            } else {
                int l;
                for (l = 0; l < nbc && !gb; l++) {     /* the record (ON keeps its own ring): by centroid */
                    int j;
                    for (j = 0; j < bc[l]->n; j++)
                        if (bc[l]->b[j].cx == mr->cx && bc[l]->b[j].cz == mr->cz && bc[l]->id[j] == mr->id) {
                            gb = &bc[l]->b[j];
                            n = gb->n;
                            for (k = 0; k < n; k++) { rx[k] = bc[l]->px[gb->first + k] - (double)c->org_x; rz[k] = bc[l]->pz[gb->first + k] - (double)c->org_z; }
                            break;
                        }
                }
                if (!gb) continue;
            }
            {
                TD5_MuniOpt o;
                sc_muni_opt(s, &o);
                if (!td5_muni_ring_behind(c->muni2, &o, rx, rz, n)) { mr->fate = TD5_MUNI_FRONT; mr->n = 0; }
            }
        }
        }   /* TD5RE_ROAM_MUNI_PASS2 */
    }
    c->muni_ready = 1;
    ok = 1;
out:
    if (!ok) for (i = 0; i < nb; i++) B[i].done = 0;
    return ok;
}

/* [roam NA] census: the emitted ring's street walls by their gap to the line (st.muni_gap) */
static void sc_muni_census(TD5_RoamScnSource *s, TG_TileCtx *c, const double *rx, const double *rz, int n)
{
    TD5_MuniOpt o;
    double g[TD5_GEOB_RING_MAX];
    int nw, k;
    if (!c->muni_ready || !c->muni || !(s->muni_on || s->muni_census)) return;
    sc_muni_opt(s, &o);
    nw = td5_muni_walls(c->muni, &o, rx, rz, n, g, TD5_GEOB_RING_MAX);
    for (k = 0; k < nw && k < TD5_GEOB_RING_MAX; k++) {
        const double m = (g[k] - MU_INSET) / s->upm;
        const int bin = m < -MU_TOL_M ? 0 : m <= MU_TOL_M ? 1 : m < 1.0 ? 2 : m < 3.0 ? 3 : m < 6.0 ? 4 : m < 12.0 ? 5 : 6;
        c->st.muni_gap[bin]++;
    }
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
    e.trees = c->st_on;
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
    if (ok && c->lt_on && !sc_lot_trees(s, c, &bld, ar, nar)) ok = 0;   /* [roam LA] while the footprints are at hand */
out:
    if (ok && (s->canopy_green || c->tb_on) && !c->pv_bld) {   /* [roam LA] the lots ask it (sc_canopy_green), [roam PD] the trees; freed by the build */
        PvBld *keep = (PvBld *)malloc(sizeof *keep);
        if (keep) { *keep = bld; c->pv_bld = keep; memset(&bld, 0, sizeof bld); }
    }
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

/* [roam PC] every AREAS polygon carrying kind flag `kf` (SC_KF_*) whose box meets [x0,x1]x[z0,z1] (place frame) */
static int sc_kf_polys(ScCache **ac, int nac, double x0, double z0, double x1, double z1, unsigned kf, ScLotPoly **out, int *nout)
{
    ScLotPoly *P = NULL;
    int n = 0, cap = 0, l, r, k;
    *out = NULL; *nout = 0;
    for (l = 0; l < nac; l++) {
        const ScCache *c = ac[l];
        if (!c->kf) continue;
        for (r = 0; r < c->n; r++) {
            const TD5_GeoArea *a = &c->a[r];
            ScLotPoly q;
            if (!(c->kf[r] & kf) || a->n < 3) continue;
            if (a->cx + a->radius < x0 || a->cx - a->radius > x1 || a->cz + a->radius < z0 || a->cz - a->radius > z1) continue;
            memset(&q, 0, sizeof q);
            q.px = &c->px[a->first]; q.pz = &c->pz[a->first]; q.n = a->n; q.cls = (int)kf;
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

/* [roam PC] 1 when a place-frame point lies in one of the polygons */
static int sc_kf_in(const ScLotPoly *P, int n, double x, double z)
{
    int i;
    for (i = 0; i < n; i++) {
        if (x < P[i].x0 || x > P[i].x1 || z < P[i].z0 || z > P[i].z1) continue;
        if (td5_geob_point_in_ring(P[i].px, P[i].pz, P[i].n, x, z)) return 1;
    }
    return 0;
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

static int sc_lot_flush_col(TG_TileCtx *c, int sbi, ScLotSb *L, int page, unsigned int col)
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
            light[i] = col;
        }
        cmd[0] = (unsigned short)page; cmd[1] = 0; cmd[2] = (unsigned short)nq;
        b->moff[b->nmesh++] = b->blk.len;
        if (!tg_write_prefab_mesh(&b->blk, v, light, nq * 4, cmd, 1, 0, 0.0, 0.0, 0.0, 1.0, 0.0) || b->blk.oom) return 0;
        qi += nq;
    }
    return 1;
}

/* [roam LA] CANOPY GREEN. Paseo del Bosque by the lake (-34.9090, -57.9386): a real wooded park no OSM area covers, so its
 * ground was a paved city lot and its frontage could take infill. A quad (tile-local, 4 corners) is canopy green when
 * CANOPY.R8 reads >= 3 m on at least SC_CG_CANOPY of its 4 x 4 sample points and no AREAS polygon of any kind (anyp, else
 * the lots' own set) holds more than SC_CG_MAPPED of them. TD5RE_ROAM_CANOPY_GREEN=0 turns it off. */
#define SC_CG_CANOPY    12
#define SC_CG_MAPPED    2
static int sc_canopy_green(const TD5_RoamScnSource *s, const TG_TileCtx *c, const double *qx, const double *qz)
{
    int a, b, can = 0, mapped = 0;
    if (!s->canopy_green || !c->canopy_ok) return 0;
    for (b = 0; b < 4; b++)
        for (a = 0; a < 4; a++) {
            const double fu = (a + 0.5) / 4.0, fv = (b + 0.5) / 4.0;
            const double x = qx[0] + (qx[1] - qx[0]) * fu + (qx[3] - qx[0]) * fv;
            const double z = qz[0] + (qz[1] - qz[0]) * fu + (qz[3] - qz[0]) * fv;
            const double X = x + (double)c->org_x, Z = z + (double)c->org_z;
            can += sc_canopy_m(c, x, z) >= SC_CANOPY_M;
            mapped += c->anyp ? pv_poly_has(c->anyp, c->nanyp, X, Z) : (sc_lot_class_at(c->lotp, c->nlotp, X, Z) != SC_LC_BUILD);
        }
    return can >= SC_CG_CANOPY && mapped <= SC_CG_MAPPED;
}
static int sc_lot_flush(TG_TileCtx *c, int sbi, ScLotSb *L, int page) { return sc_lot_flush_col(c, sbi, L, page, 0xFFFFFFFFu); }

/* [roam PC] SHORE CELLS. The render draws a ground cell with wet WATER-raster samples as a shore sub-mesh (td5_roam_render.c
 * SHORES): the pit under the water and a bank at the real edge. A lot laid over it on the 16 m lattice would pave across the
 * real shore (round L's 16 m steps), so a cell with a wet lattice corner (the nearest-cell rule) or a wet 2 m sub-sample
 * (bilinear >= 0.5, the render's same 9 x 9 points) is left to the render. `cell` = x y z per corner, tile-local. */
static int sc_lot_shore(TG_TileCtx *c, const double *cell)
{
    int a, b;
    for (b = 0; b <= 8; b++) for (a = 0; a <= 8; a++) {
        const double fu = a / 8.0, fv = b / 8.0;
        const double x = cell[0] + (cell[3] - cell[0]) * fu + (cell[9] - cell[0]) * fv + (double)c->org_x;
        const double z = cell[2] + (cell[5] - cell[2]) * fu + (cell[11] - cell[2]) * fv + (double)c->org_z;
        double f = 0.0, h;
        int wt = 0;
        if ((a == 0 || a == 8) && (b == 0 || b == 8)) {
            if (td5_roam_height_sample(c->hc, x, z, &h, &wt) && wt) return 1;
        }
        if (td5_roam_height_wetfrac(c->hc, x, z, &f) && f >= 0.5) return 1;
    }
    return 0;
}


#ifndef TD5RE_RELEASE
/* [roam QB] DEV probe: TD5RE_QB_PROBE=x,z (place frame) prints a cross-section of the surfaces outward from the nearest
 * street centreline through that point, on the tile that owns it: road, lawn plane, slab, drawn ground, raster */
static void sc_qb_probe(TG_TileCtx *c)
{
    const char *e = getenv("TD5RE_QB_PROBE");
    double px, pz, along, lat, x, z;
    int edge, side, k;
    if (!e || sscanf(e, "%lf,%lf", &px, &pz) != 2) return;
    x = px - (double)c->org_x; z = pz - (double)c->org_z;
    if (!sc_owned_point(c, x, z)) return;
    if (!roam_nearest_edge(c, x, z, 60.0 * SC_UPM, &edge, &side, &along, &lat)) { fprintf(stderr, "QBPROBE no street\n"); return; }
    {
        const ScSeg *sg = &c->seg[edge];
        const double dx = sg->bx - sg->ax, dz = sg->bz - sg->az, l = hypot(dx, dz);
        const double tx = dx / l, tz = dz / l, nx = tz * (lat >= 0 ? 1.0 : -1.0), nz = -tx * (lat >= 0 ? 1.0 : -1.0);
        const double t = ((x - sg->ax) * tx + (z - sg->az) * tz);
        const double cx = sg->ax + tx * t, cz = sg->az + tz * t;
        fprintf(stderr, "QBPROBE tile %d,%d seg %d hw %.2f m lat %.2f m (point) centre y %.3f m\n", c->tx, c->tz, edge, sg->hw / SC_UPM,
                fabs(lat) / SC_UPM, (sg->ay + (sg->by - sg->ay) * (t / l)) / SC_UPM);
        for (k = 0; k <= 60; k++) {
            const double d = (double)k * 0.5 * SC_UPM;
            const double qx = cx + nx * d, qz = cz + nz * d;
            double ry = 0.0, ly = 0.0, sy = 0.0, raw = 0.0;
            const int hr = roam_ctx_road_y(c, qx, qz, sg->ay, &ry), hl = sc_lawn_y(c, qx, qz, &ly), hs = sc_pv_y(c, qx, qz, &sy);
            roam_ctx_ground_raw(c, qx, qz, &raw, NULL);
            fprintf(stderr, "QBPROBE d %5.1f road %s lawn %s slab %s dg %8.3f ground %8.3f raw %8.3f low %6.3f\n", d / SC_UPM,
                    hr ? "   y    " : "    -   ", hl ? "y" : "-", hs ? "y" : "-", sc_dg(c, qx, qz) / SC_UPM,
                    sc_ground(c, qx, qz) / SC_UPM, raw / SC_UPM, sc_lowering(c, qx, qz) / SC_UPM);
            if (hr || hl || hs) fprintf(stderr, "QBPROBE   d %5.1f road_y %8.3f lawn_y %8.3f slab_y %8.3f\n", d / SC_UPM,
                                        hr ? ry / SC_UPM : -1.0, hl ? ly / SC_UPM : -1.0, hs ? sy / SC_UPM : -1.0);
        }
    }
}
#endif

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
    /* the render's ground holes (rr_ground_build): round a bore / [roam NC] cutting span centre */
    {
        const RoamTile *nb9[9];
        for (k = 0; k < 9; k++) nb9[k] = c->road[k];
        if (!nb9[4]) nb9[4] = self;
        td5_roam_cut_holes(c->grid, nb9, c->hc, c->tx, c->tz, G, hole);
        (void)e; (void)q;
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
        if (s->shore_on && sc_lot_shore(c, cell)) { c->st.lot_shore++; continue; }   /* [roam PC] the render's bank */
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
        if (pave && c->pv_bld) {              /* [roam LA] canopy green: wooded ground no OSM area maps, no footprint on it */
            const double gqx[4] = { cell[0], cell[3], cell[6], cell[9] }, gqz[4] = { cell[2], cell[5], cell[8], cell[11] };
            if (sc_canopy_green(s, c, gqx, gqz)) {
                int bl = 0;
                for (k = 0; k < 5 && !bl; k++) {
                    const double sx = cell[0] + (cell[3] - cell[0]) * fq[k][0] + (cell[9] - cell[0]) * fq[k][1];
                    const double sz = cell[2] + (cell[5] - cell[2]) * fq[k][0] + (cell[11] - cell[2]) * fq[k][1];
                    bl = pv_in_building(c, (const PvBld *)c->pv_bld, sx, sz) || inf_in_lot(c, sx, sz);
                }
                if (!bl) { pave = 0; c->st.lot_canopy_green++; }
            }
        }
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

/* ---- [roam LE] SWIMMING POOLS ----
 *
 * AREAS kind swimming_pool (82 in the partido, 2 in the casco: club and backyard pools in City Bell / Gonnet) were
 * paved over as OPEN lots. Each one OWNED by the tile (sc_owned_range) is now a flat box top at its highest base
 * point (the drawn ground + SC_LOT_LIFT, or a lawn standing over it): a concrete coping ring on the sidewalk page
 * (the outline to the outline pulled SC_POOL_INSET towards its centroid) and the inset polygon on the water page
 * SC_POOL_WATER_Y over the base, a little under the coping (SC_POOL_COPING_Y). An outline touching the drivable
 * surface is left out (st.pool_drop_road). TD5RE_ROAM_POOLS=0 = the old paved lot. */
#define SC_POOL_INSET     (0.50 * SC_UPM)   /* coping width (at most 30 % of the mean radius)   */
#define SC_POOL_COPING_Y  40.0              /* units over the base (9 cm)                       */
#define SC_POOL_WATER_Y   30.0              /* units over the base (7 cm)                       */
#define SC_POOL_WATER_UV  (1.0 / 6000.0)    /* TD5_TG_WATER_TILE                                */

static int sc_emit_pools(TG_TileCtx *c, const RoamTile *self, ScCache **ac, int nac)
{
    const int64_t key = sc_tkey(self->tx, self->tz);
    const double wrep = 1.0 / SC_POOL_WATER_UV, crep = 1.0 / SC_LOT_UV;
    const double wox = fmod((double)c->org_x, wrep), woz = fmod((double)c->org_z, wrep);
    const double cox = fmod((double)c->org_x, crep), coz = fmod((double)c->org_z, crep);
    int l;
    for (l = 0; l < nac; l++) {
        const ScCache *ca = ac[l];
        int lo, hi, j;
        if (!ca->pool) continue;
        sc_owned_range(ca, key, &lo, &hi);
        for (j = lo; j < hi; j++) {
            const int ri = ca->order[j];
            const TD5_GeoArea *a = &ca->a[ri];
            double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX], ix[TD5_GEOB_RING_MAX], iz[TD5_GEOB_RING_MAX];
            int tri[(TD5_GEOB_RING_MAX - 2) * 3];
            double cx = 0.0, cz = 0.0, rm = 0.0, f, base = -1e300, y;
            float *v;
            unsigned int *light;
            unsigned short cmd[6];
            int n = a->n, k, ntri, nv;
            ScSb *b;
            if (!ca->pool[ri]) continue;
            if (n < 3 || n > TD5_GEOB_RING_MAX) { c->st.pool_bad++; continue; }
            for (k = 0; k < n; k++) {
                rx[k] = ca->px[a->first + k] - (double)c->org_x;
                rz[k] = ca->pz[a->first + k] - (double)c->org_z;
            }
            if (n > 3 && fabs(rx[0] - rx[n - 1]) < 1.0 && fabs(rz[0] - rz[n - 1]) < 1.0) n--;   /* closed ring */
            for (k = 0; k < n; k++) { cx += rx[k]; cz += rz[k]; }
            cx /= n; cz /= n;
            if (sc_on_road(c, rx, rz, n, NULL)) { c->st.pool_drop_road++; continue; }
            for (k = 0; k < n; k++) rm += hypot(rx[k] - cx, rz[k] - cz);
            rm /= n;
            if (rm < 1.0) { c->st.pool_bad++; continue; }
            f = 1.0 - ((SC_POOL_INSET < 0.3 * rm) ? SC_POOL_INSET : 0.3 * rm) / rm;
            for (k = 0; k <= n; k++) {                       /* the base: the highest surface under the outline */
                const double px = (k < n) ? rx[k] : cx, pz = (k < n) ? rz[k] : cz;
                double h = sc_dg(c, px, pz) + SC_LOT_LIFT, lw;
                if (sc_lawn_y(c, px, pz, &lw) && lw > h) h = lw;
                if (h > base) base = h;
            }
            for (k = 0; k < n; k++) { ix[k] = cx + f * (rx[k] - cx); iz[k] = cz + f * (rz[k] - cz); }
            ntri = td5_geob_triangulate(ix, iz, n, tri, TD5_GEOB_RING_MAX - 2);
            if (ntri <= 0) { c->st.pool_bad++; continue; }
            nv = n * 4 + ntri * 3;
            v = (float *)malloc((size_t)nv * 5u * sizeof(float));
            light = (unsigned int *)malloc((size_t)nv * sizeof(unsigned int));
            if (!v || !light) { free(v); free(light); return 0; }
            y = base + SC_POOL_COPING_Y;
            for (k = 0; k < n; k++) {                        /* coping: outer k, outer k+1, inner k+1, inner k */
                const int k1 = (k + 1) % n;
                const double qx[4] = { rx[k], rx[k1], ix[k1], ix[k] }, qz[4] = { rz[k], rz[k1], iz[k1], iz[k] };
                int e;
                for (e = 0; e < 4; e++) {
                    float *o = &v[(k * 4 + e) * 5];
                    o[0] = (float)qx[e]; o[1] = (float)y; o[2] = (float)qz[e];
                    o[3] = (float)((qx[e] + cox) * SC_LOT_UV); o[4] = (float)((qz[e] + coz) * SC_LOT_UV);
                }
            }
            y = base + SC_POOL_WATER_Y;
            for (k = 0; k < ntri * 3; k++) {
                float *o = &v[(n * 4 + k) * 5];
                const int p = tri[k];
                o[0] = (float)ix[p]; o[1] = (float)y; o[2] = (float)iz[p];
                o[3] = (float)((ix[p] + wox) * SC_POOL_WATER_UV); o[4] = (float)((iz[p] + woz) * SC_POOL_WATER_UV);
            }
            for (k = 0; k < nv; k++) light[k] = 0xFFFFFFFFu;
            cmd[0] = (unsigned short)TD5_TG_PAGE_SIDEWALK; cmd[1] = 0; cmd[2] = (unsigned short)n;
            cmd[3] = (unsigned short)TD5_TG_PAGE_WATER;    cmd[4] = (unsigned short)ntri; cmd[5] = 0;
            b = sc_sb(c, sc_sb_of(c, cx, cz), 1);
            if (!b) { free(v); free(light); return 0; }
            b->moff[b->nmesh++] = b->blk.len;
            if (!tg_write_prefab_mesh(&b->blk, v, light, nv, cmd, 2, 0, 0.0, 0.0, 0.0, 1.0, 0.0) || b->blk.oom) {
                free(v); free(light); return 0;
            }
            free(v); free(light);
            c->st.pools++;
            c->st.pool_m2 += sc_ring_area_m2(ix, iz, n);
        }
    }
    return 1;
}

/* ---- [roam PB] STADIUMS + SPORTS PITCHES (td5_roam_stadium.c) ----
 *
 * The leisure=stadium outlines this tile owns, each with its main pitch (the largest leisure=pitch inside it) and its
 * stands (sc_sport_claim == 1 over the 3x3's footprints, a relation outline's courtyards included), and the
 * leisure=pitch areas it owns, handed to td5_roam_sport_emit in (id, cx, cz) order. Coordinates tile-local. */
#define SC_SP_STAND_MAX 32
#define SC_SP_PITCH_MAX 256
#define SC_SP_STADIA    8

typedef struct {
    double px[TD5_GEOB_RING_MAX], pz[TD5_GEOB_RING_MAX];
    double hx[TG_S3D_HOLES_MAX * TD5_GEOB_RING_MAX], hz[TG_S3D_HOLES_MAX * TD5_GEOB_RING_MAX];
    int hn[TG_S3D_HOLES_MAX];
} ScSpRing;

typedef struct { double x[TD5_GEOB_RING_MAX], z[TD5_GEOB_RING_MAX]; } ScSpPoly;

static int sc_emit_sport(TD5_RoamScnSource *s, TG_TileCtx *c, ScCache **bc, int nbc, const ScArea *ar, int nar)
{
    const int64_t self = sc_tkey(c->tx, c->tz);
    ScBld *set = NULL;
    ScSpRing *rings = NULL;
    TD5_RoamStand *st = NULL;
    TD5_RoamStadium sd[SC_SP_STADIA];
    TD5_RoamPitch *pt = NULL;
    ScSpPoly *sdr = NULL, *sdq = NULL, *ptr = NULL;
    int nset = 0, cap = 0, nsd = 0, nst = 0, npt = 0, i, k, l, dx, dz, ok = 0;
    if (!c->sport_on) return 1;
    rings = (ScSpRing *)calloc(SC_SP_STAND_MAX, sizeof(ScSpRing));
    st = (TD5_RoamStand *)calloc(SC_SP_STAND_MAX, sizeof(TD5_RoamStand));
    pt = (TD5_RoamPitch *)calloc(SC_SP_PITCH_MAX, sizeof(TD5_RoamPitch));
    sdr = (ScSpPoly *)calloc(SC_SP_STADIA, sizeof(ScSpPoly));
    sdq = (ScSpPoly *)calloc(SC_SP_STADIA, sizeof(ScSpPoly));
    ptr = (ScSpPoly *)calloc(SC_SP_PITCH_MAX, sizeof(ScSpPoly));
    if (!rings || !st || !pt || !sdr || !sdq || !ptr) goto out;
    if (c->sport_on & 1) {   /* the halo set, as sc_emit_buildings gathers and sorts it */
        for (dz = -1; dz <= 1; dz++)
            for (dx = -1; dx <= 1; dx++) {
                const int64_t key = sc_tkey(c->tx + dx, c->tz + dz);
                for (l = 0; l < nbc; l++) {
                    int lo, hi, j;
                    sc_owned_range(bc[l], key, &lo, &hi);
                    for (j = lo; j < hi; j++) {
                        const int ri = bc[l]->order[j];
                        if (!sc_grow((void **)&set, &cap, nset + 1, sizeof(ScBld))) goto out;
                        memset(&set[nset], 0, sizeof set[nset]);
                        set[nset].b = &bc[l]->b[ri];
                        set[nset].px = &bc[l]->px[bc[l]->b[ri].first];
                        set[nset].pz = &bc[l]->pz[bc[l]->b[ri].first];
                        set[nset].id = bc[l]->id[ri];
                        set[nset].storey_m = bc[l]->storey_m;
                        set[nset].rx = (bc[l] == &s->rel && s->relx) ? &s->relx[ri] : NULL;
                        set[nset].rc = bc[l];
                        nset++;
                    }
                }
            }
        for (i = 1; i < nset; i++) {
            ScBld t = set[i];
            for (k = i - 1; k >= 0; k--) {
                const ScBld *q = &set[k];
                if (!(q->id > t.id || (q->id == t.id && (q->b->cx > t.b->cx || (q->b->cx == t.b->cx && q->b->cz > t.b->cz))))) break;
                set[k + 1] = set[k];
            }
            set[k + 1] = t;
        }
    }
    for (i = 0; i < nar; i++) {
        const TD5_GeoArea *a = ar[i].a;
        const int no = a->n_outer ? a->n_outer : a->n;
        int tx, tz;
        roam_grid_tile_of(c->grid, a->cx, a->cz, &tx, &tz);
        if (sc_tkey(tx, tz) != self || no < 3 || no > TD5_GEOB_RING_MAX) continue;
        if (ar[i].spt == TD5_RPS_STADIUM && (c->sport_on & 1) && nsd < SC_SP_STADIA) {
            TD5_RoamStadium *d = &sd[nsd];
            double best = 0.0;
            int q, j;
            memset(d, 0, sizeof *d);
            d->osm_id = ar[i].id >= SC_PREL_ID_BASE ? floor((ar[i].id - SC_PREL_ID_BASE) / 8.0) : ar[i].id;
            for (k = 0; k < no; k++) { sdr[nsd].x[k] = ar[i].px[k] - (double)c->org_x; sdr[nsd].z[k] = ar[i].pz[k] - (double)c->org_z; }
            d->rx = sdr[nsd].x; d->rz = sdr[nsd].z; d->n = no;
            for (q = 0; q < nar; q++) {   /* the main pitch: the largest leisure=pitch inside */
                const TD5_GeoArea *b = ar[q].a;
                const int bn = b->n_outer ? b->n_outer : b->n;
                double m2;
                if (!(ar[q].srf & SC_SRF_PITCH) || bn < 3 || bn > TD5_GEOB_RING_MAX) continue;
                if (sc_stadium_at(ar, nar, b->cx, b->cz) != i) continue;
                m2 = fabs(td5_geob_ring_area(ar[q].px, ar[q].pz, bn));
                if (m2 <= best) continue;
                best = m2;
                for (k = 0; k < bn; k++) { sdq[nsd].x[k] = ar[q].px[k] - (double)c->org_x; sdq[nsd].z[k] = ar[q].pz[k] - (double)c->org_z; }
                d->qx = sdq[nsd].x; d->qz = sdq[nsd].z; d->qn = bn;
            }
            d->st = &st[nst];
            for (j = 0; j < nset && nst < SC_SP_STAND_MAX; j++) {
                int which = -1, h = 0, off = 0;
                const TD5_GeoBuilding *gb = set[j].b;
                ScSpRing *r = &rings[nst];
                TD5_RoamStand *u = &st[nst];
                if (sc_sport_claim(set, nset, j, ar, nar, &which) != 1 || which != i) continue;
                if (gb->n < 3 || gb->n > TD5_GEOB_RING_MAX) continue;
                for (k = 0; k < gb->n; k++) { r->px[k] = set[j].px[k] - (double)c->org_x; r->pz[k] = set[j].pz[k] - (double)c->org_z; }
                u->px = r->px; u->pz = r->pz; u->n = gb->n;
                u->height = gb->height > 0.0 ? gb->height : 9.0 * s->upm;
                u->id = set[j].id;
                if (set[j].rx) {
                    for (h = 0; h < set[j].rx->nh && h < TG_S3D_HOLES_MAX; h++) {
                        const int hf = set[j].rx->hfirst[h];
                        r->hn[h] = set[j].rx->hn[h];
                        for (k = 0; k < r->hn[h]; k++) {
                            r->hx[off + k] = set[j].rc->px[hf + k] - (double)c->org_x;
                            r->hz[off + k] = set[j].rc->pz[hf + k] - (double)c->org_z;
                        }
                        off += r->hn[h];
                    }
                    u->nh = h; u->hn = r->hn; u->hx = r->hx; u->hz = r->hz;
                }
                nst++;
                d->nst++;
            }
            nsd++;
        } else if ((ar[i].srf & SC_SRF_PITCH) && (c->sport_on & 2) && npt < SC_SP_PITCH_MAX) {
            TD5_RoamPitch *p = &pt[npt];
            for (k = 0; k < no; k++) { ptr[npt].x[k] = ar[i].px[k] - (double)c->org_x; ptr[npt].z[k] = ar[i].pz[k] - (double)c->org_z; }
            p->id = ar[i].id; p->sport = ar[i].spt; p->surface = ar[i].srf & 0x7F;
            p->rx = ptr[npt].x; p->rz = ptr[npt].z; p->n = no;
            npt++;
        }
    }
    ok = td5_roam_sport_emit(c, sd, nsd, pt, npt, &c->st.sport);
out:
    free(set); free(rings); free(st); free(pt); free(sdr); free(sdq); free(ptr);
    return ok;
}

/* ---- [roam LH] GROUND COVER outside the casco ----
 *
 * The roam ground is one lawn page everywhere, so a horticulture belt or a marsh reads as the same meadow as a park.
 * Outside the casco every drawn-ground cell whose COVER samples are mostly CROP (4 x 4, >= 12 of 16) is laid again
 * SC_LOT_LIFT over the drawn ground on the plank-wood page (brown, furrowed: tilled soil), mostly WETLAND on the grass
 * page, the lots' own way (one quad in the drawn ground's own vertex heights, the lots' UV). A cell with a road on it,
 * a bore hole under it or water is left to the lawn. TD5RE_ROAM_OUT_COVER=0 = off.
 *
 * [roam PC] MARSH. The wetland paint is ON (TD5RE_ROAM_OUT_COVER_WET, default 1; 0 = round L's bytes) and reads as a
 * marsh instead of LH's plain grass (TD5RE_ROAM_MARSH=0 = LH's paint as written, >= 12 of 16 samples):
 *   - WHICH CELLS. ESA's wetland class speckles, so a cell is marsh when >= SC_MARSH_MIN of its 16 COVER samples are
 *     wetland AND >= SC_MARSH_NB of its 8 neighbour cells pass the same count (a lone speckle is dropped, a ragged
 *     edge is filled), or when its centre lies in an OSM natural=wetland area (c->wetp, 29 in the partido, 24 km2).
 *     The counts read the 3x3 COVER window, so a cell on a tile seam is judged the same from both tiles.
 *   - LOOK. The grass page shifted to straw (SC_MARSH_COL: the light array multiplies the page, so a dark tint went
 *     black) and SC_REED_PER reed clumps per cell: two crossed upright quads, 1.2-1.9 m tall, on
 *     the willow tree page (SC_REED_PAGE, alpha cut, a half page: a ragged drooping tuft), straw or green. Open-water patches were tried and
 *     dropped (square blue diamonds read as noise). Positions and sizes hash the GLOBAL lattice cell, so a tile's bytes
 *     do not depend on its neighbours or the build order. A clump touching a road is left out.
 * Greenhouses: td5_roam_greenhouse.c (the building pass). */
#define SC_MARSH_MIN    8
#define SC_MARSH_NB     3
#define SC_MARSH_COL    0xFFF0E6B4u     /* straw over the grass page                         */
#define SC_REED_PER     2
#define SC_REED_PAGE    (TD5_TG_PAGE_TREE_EXTRA + 8)   /* tree variant 9, the willow (td5_tg_terrain.c) */
#define SC_REED_U0      0.0             /* the whole half page: its alpha cut gives the clump a ragged outline */
#define SC_REED_V0      0.0
#define SC_REED_COL0    0xFFFFF2C0u     /* straw */
#define SC_REED_COL1    0xFFE0FFC8u     /* green */

/* [roam PC] upright quads with their own UVs (the reeds), one sub-block's pending batch */
typedef struct { float *v; unsigned int *l; int nq, cq; } ScUvSb;

static int sc_uv_put(ScUvSb *b, const double *q, const double *uv, unsigned int col)
{
    int k;
    if (b->nq + 1 > b->cq) {
        const int nc = b->cq ? b->cq * 2 : 64;
        float *nv = (float *)realloc(b->v, (size_t)nc * 4 * 5 * sizeof(float));
        unsigned int *nl;
        if (!nv) return 0;
        b->v = nv;
        nl = (unsigned int *)realloc(b->l, (size_t)nc * 4 * sizeof(unsigned int));
        if (!nl) return 0;
        b->l = nl;
        b->cq = nc;
    }
    for (k = 0; k < 4; k++) {
        float *o = &b->v[(size_t)(b->nq * 4 + k) * 5];
        o[0] = (float)q[k * 3]; o[1] = (float)q[k * 3 + 1]; o[2] = (float)q[k * 3 + 2];
        o[3] = (float)uv[k * 2]; o[4] = (float)uv[k * 2 + 1];
        b->l[b->nq * 4 + k] = col;
    }
    b->nq++;
    return 1;
}

static int sc_uv_flush(TG_TileCtx *c, int sbi, ScUvSb *L, int page)
{
    int qi = 0;
    while (qi < L->nq) {
        const int nq = (L->nq - qi < SC_LOT_CHUNK) ? L->nq - qi : SC_LOT_CHUNK;
        unsigned short cmd[3];
        ScSb *b = sc_sb(c, sbi, 1);
        if (!b) return 0;
        cmd[0] = (unsigned short)page; cmd[1] = 0; cmd[2] = (unsigned short)nq;
        b->moff[b->nmesh++] = b->blk.len;
        if (!tg_write_prefab_mesh(&b->blk, &L->v[(size_t)qi * 4 * 5], &L->l[(size_t)qi * 4], nq * 4, cmd, 1, 0, 0.0, 0.0, 0.0, 1.0, 0.0)
            || b->blk.oom) return 0;
        qi += nq;
    }
    return 1;
}

/* COVER class at a tile-local point from the 3x3 window (falls back to the tile window) */
static int sc_cover3_at(const TG_TileCtx *c, double x, double z)
{
    const GrRaster *r = &c->cover3;
    double fx, fz;
    int i, j;
    if (!c->cover3_ok || !r->data || r->w <= 0 || r->h <= 0) return sc_cover_at(c, x, z);
    fx = (x + (double)c->org_x - r->origin_x) / r->cell;
    fz = (z + (double)c->org_z - r->origin_z) / r->cell;
    i = (int)floor(fx); j = (int)floor(fz);
    if (i < 0 || j < 0 || i >= r->w || j >= r->h) return TD5_GEO_COVER_NONE;
    return ((const unsigned char *)r->data)[(size_t)j * (size_t)r->w + (size_t)i];
}

static uint32_t sc_cell_hash(int gi, int gj, uint32_t salt)
{
    uint32_t q[3];
    q[0] = (uint32_t)gi; q[1] = (uint32_t)gj; q[2] = salt;
    return sc_fnv(2166136261u, q, sizeof q);
}

static double sc_h01(uint32_t *h)      /* the next [0,1) from a running hash */
{
    *h = *h * 1664525u + 1013904223u;
    return (double)(*h >> 8) / 16777216.0;
}

static int sc_emit_cover(TD5_RoamScnSource *s, TG_TileCtx *c)
{
    const int cell_m = td5_env_int("TD5RE_ROAM_RENDER_CELL_M", 16, 8, 64);
    const int G = 256 / ((64 % cell_m) ? 16 : cell_m), per = G / TD5_ROAM_SCN_SUB, W = G + 1;
    const double S = (double)c->grid->tile;
    const double drop = td5_env_int("TD5RE_ROAM_RENDER_GROUND_DROP_CM", 40, 0, 500) * (SC_UPM / 100.0);
    const int wet_on = s->wet_on;                                    /* [roam PC] on by default */
    const int marsh = wet_on && td5_env_int("TD5RE_ROAM_MARSH", 1, 0, 1);
    const int R = G + 2;                                             /* cells -1..G, for the neighbour count */
    ScLotSb L[2][TD5_ROAM_SCN_NSB];
    ScUvSb RD[TD5_ROAM_SCN_NSB];
    double *px = NULL, *pz = NULL, *py = NULL;
    unsigned char *wc = NULL;
    int i, j, k, cl, ok = 0;
    if (!c->hc) return 1;
    memset(L, 0, sizeof L);
    memset(RD, 0, sizeof RD);
    px = (double *)malloc((size_t)W * W * sizeof(double));
    pz = (double *)malloc((size_t)W * W * sizeof(double));
    py = (double *)malloc((size_t)W * W * sizeof(double));
    if (!px || !pz || !py) goto out;
    for (j = 0; j <= G; j++) for (i = 0; i <= G; i++) {
        const double u = ((double)c->tx + (double)i / (double)G) * S, v = ((double)c->tz + (double)j / (double)G) * S;
        const double X = c->grid->ox + c->grid->c * u - c->grid->s * v, Z = c->grid->oz + c->grid->s * u + c->grid->c * v;
        px[j * W + i] = X - (double)c->org_x; pz[j * W + i] = Z - (double)c->org_z;
        py[j * W + i] = sc_ground(c, px[j * W + i], pz[j * W + i]) - drop + 3.0 * SC_LOT_LIFT;   /* a 21 cm lift: coplanar quads z-fight with the ground at distance */
    }
    if (marsh) {   /* [roam PC] wetland samples per cell over the ring -1..G (global lattice positions) */
        wc = (unsigned char *)calloc((size_t)R * R, 1);
        if (!wc) goto out;
        for (j = -1; j <= G; j++) for (i = -1; i <= G; i++) {
            int a, b, n = 0;
            for (b = 0; b < 4; b++) for (a = 0; a < 4; a++) {
                const double u = ((double)c->tx + (i + (a + 0.5) / 4.0) / (double)G) * S;
                const double v = ((double)c->tz + (j + (b + 0.5) / 4.0) / (double)G) * S;
                const double X = c->grid->ox + c->grid->c * u - c->grid->s * v, Z = c->grid->oz + c->grid->s * u + c->grid->c * v;
                n += sc_cover3_at(c, X - (double)c->org_x, Z - (double)c->org_z) == TD5_GEO_COVER_WETLAND;
            }
            wc[(j + 1) * R + i + 1] = (unsigned char)n;
        }
    }
    for (j = 0; j < G; j++) for (i = 0; i < G; i++) {
        static const int di[4] = { 0, 1, 1, 0 }, dj[4] = { 0, 0, 1, 1 };
        double cell[12];
        int crop = 0, wet = 0, water = 0, a, b;
        const int sbi = (j / per) * TD5_ROAM_SCN_SUB + (i / per);
        for (k = 0; k < 4; k++) {
            const int pi = (j + dj[k]) * W + i + di[k];
            cell[k * 3 + 0] = px[pi]; cell[k * 3 + 1] = py[pi]; cell[k * 3 + 2] = pz[pi];
        }
        for (b = 0; b < 4; b++) for (a = 0; a < 4; a++) {
            const double fu = (a + 0.5) / 4.0, fv = (b + 0.5) / 4.0;
            const double sx = cell[0] + (cell[3] - cell[0]) * fu + (cell[9] - cell[0]) * fv;
            const double sz = cell[2] + (cell[5] - cell[2]) * fu + (cell[11] - cell[2]) * fv;
            const int cv = sc_cover_at(c, sx, sz);
            crop += cv == TD5_GEO_COVER_CROP;
            wet += cv == TD5_GEO_COVER_WETLAND;
            water += cv == TD5_GEO_COVER_WATER;
        }
        if (!marsh) cl = crop >= 12 ? 0 : (wet >= 12 && wet_on ? 1 : -1);
        else {
            int nb = 0, area = 0, mw;
            for (b = -1; b <= 1; b++) for (a = -1; a <= 1; a++)
                if ((a || b) && wc[(j + 1 + b) * R + i + 1 + a] >= SC_MARSH_MIN) nb++;
            mw = wc[(j + 1) * R + i + 1] >= SC_MARSH_MIN && nb >= SC_MARSH_NB;
            if (!mw && c->nwetp > 0) {
                const double mx = 0.25 * (cell[0] + cell[3] + cell[6] + cell[9]), mz = 0.25 * (cell[2] + cell[5] + cell[8] + cell[11]);
                area = mw = sc_kf_in(c->wetp, c->nwetp, mx + (double)c->org_x, mz + (double)c->org_z);
            }
            cl = (crop >= 12 && !area) ? 0 : (mw ? 1 : -1);
            if (cl == 1 && area) c->st.out.cover_marsh_area++;
        }
        if (cl < 0 || water) continue;
        {
            const double qx[4] = { cell[0], cell[3], cell[6], cell[9] }, qz[4] = { cell[2], cell[5], cell[8], cell[11] };
            if (sc_on_road(c, qx, qz, 4, NULL)) { c->st.out.cover_road_skip++; continue; }
        }
        if (c->shore_on && sc_lot_shore(c, cell)) { c->st.out.cover_shore_skip++; continue; }   /* [roam PC] the render's bank */
        if (!sc_lot_put(&L[cl][sbi].q, &L[cl][sbi].nq, &L[cl][sbi].cq, cell, 4)) goto out;
        if (cl == 0) { c->st.out.cover_crop++; continue; }
        c->st.out.cover_wet++;
        if (!marsh) continue;
        {   /* [roam PC] reed clumps, by the global cell's hash */
            const int gi = c->tx * G + i, gj = c->tz * G + j;
            uint32_t h = sc_cell_hash(gi, gj, 0x5EEDu);
            int r, f;
            for (r = 0; r < SC_REED_PER; r++) {
                const double fu = 0.15 + 0.7 * sc_h01(&h), fv = 0.15 + 0.7 * sc_h01(&h);
                const double rx0 = cell[0] + (cell[3] - cell[0]) * fu + (cell[9] - cell[0]) * fv;
                const double rz0 = cell[2] + (cell[5] - cell[2]) * fu + (cell[11] - cell[2]) * fv;
                const double ry0 = cell[1] + (cell[4] - cell[1]) * fu + (cell[10] - cell[1]) * fv - 0.5 * SC_LOT_LIFT;
                const double hw = (0.7 + 0.3 * sc_h01(&h)) * SC_UPM, ht = (1.2 + 0.7 * sc_h01(&h)) * SC_UPM;
                const double ang = 3.14159265 * sc_h01(&h);
                const unsigned int col = sc_h01(&h) < 0.5 ? SC_REED_COL0 : SC_REED_COL1;
                const double bx[4] = { rx0 - hw, rx0 + hw, rx0 + hw, rx0 - hw }, bz[4] = { rz0 - hw, rz0 - hw, rz0 + hw, rz0 + hw };
                if (sc_on_road(c, bx, bz, 4, NULL)) continue;
                for (f = 0; f < 2; f++) {     /* two crossed uprights */
                    const double ca = cos(ang + f * 1.5707963), sa = sin(ang + f * 1.5707963);
                    const double q[12] = { rx0 - ca * hw, ry0, rz0 - sa * hw,   rx0 + ca * hw, ry0, rz0 + sa * hw,
                                           rx0 + ca * hw, ry0 + ht, rz0 + sa * hw,   rx0 - ca * hw, ry0 + ht, rz0 - sa * hw };
                    const double uv[8] = { SC_REED_U0, 1.0, 1.0, 1.0, 1.0, SC_REED_V0, SC_REED_U0, SC_REED_V0 };
                    if (!sc_uv_put(&RD[sbi], q, uv, col)) goto out;
                }
                c->st.out.cover_reeds++;
            }
        }
    }
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) {
        if (!sc_lot_flush_col(c, k, &L[0][k], TD5_TG_PAGE_ROAD_EXTRA + 1, 0xFFFFFFFFu)) goto out;
        if (!sc_lot_flush_col(c, k, &L[1][k], TD5_TG_PAGE_GREEN, marsh ? SC_MARSH_COL : 0xFFFFFFFFu)) goto out;
        if (marsh && !sc_uv_flush(c, k, &RD[k], SC_REED_PAGE)) goto out;
    }
    ok = 1;
out:
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) { free(L[0][k].q); free(L[1][k].q); free(RD[k].v); free(RD[k].l); }
    free(px); free(pz); free(py); free(wc);
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
#define INF_RAIL_STEP_M 1.0           /* [integ-roamI] a lot a rail bed reaches: its front is searched back in these steps */
#define INF_RAIL_GAP_M  3.0           /* ... and stands this far behind the first offset clear of the bed            */
#define INF_RAIL_MIN_M  8.0           /* ... refused when less than this depth is left                               */
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
static int inf_quad_in_building_k(const TG_TileCtx *c, const PvBld *pb, const double *qx, const double *qz, int biz_only);
static int inf_quad_in_building(const TG_TileCtx *c, const PvBld *pb, const double *qx, const double *qz)
{
    return inf_quad_in_building_k(c, pb, qx, qz, 0);
}
/* biz_only = 1: [roam PE] only the footprints with a business (PvRing.biz) */
static int inf_quad_in_building_k(const TG_TileCtx *c, const PvBld *pb, const double *qx, const double *qz, int biz_only)
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
            if (biz_only && !r->biz) continue;
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
        if (c->bst.emitted > emitted0) sc_ring_dump(s, c, -1.0, rx, rz, n);   /* [roam NA] census */
        if (c->bst.emitted > emitted0 && !st_push_ring(c, rx, rz, n)) return 0;   /* [roam PD] */
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
    unsigned char *och;              /* [roam NA] per vertex: a block corner of two fronts the ochava cuts */
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
 * 4 [IC] a green / open polygon under any inset corner or centre of the lot, 5 [integ-roamI] a drawn railway bed or
 * platform reaches the lot (open land like a rail yard: IC's veto, refused as an area) */
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
        /* [roam PE] Round N integ census: 34 of the 43 shopfronts buried in a mass stood inside an infill lot. The lot
         * quad is tested 0.5 m in from its edges, so it may lap 0.5 m over a footprint: over a shop's wall that hides
         * the shopfront. A footprint with a business is tested on the full quad. */
        if (e->s->shops && td5_roam_shops_infill_guard(e->s->shops) && inf_quad_in_building_k(c, e->bld, qx, qz, 1)) {
            if (!e->dry) c->st.shop_inf_guard++;
            return 3;
        }
        for (k = 0; k < 5 && e->s->ic_infill; k++) {
            const double X = (k < 4 ? sx[k] : cx) + (double)c->org_x, Z = (k < 4 ? sz[k] : cz) + (double)c->org_z;
            if (sc_lot_class_at(c->lotp, c->nlotp, X, Z) != SC_LC_BUILD) return 4;
        }
        if (e->s->inf_rail && e->s->rails) {   /* [integ-roamI] round I combined: ID's track vs IC's infill */
            double X[4], Z[4];
            for (k = 0; k < 4; k++) { X[k] = qx[k] + (double)c->org_x; Z[k] = qz[k] + (double)c->org_z; }
            if (td5_roam_rails_bed_near(e->s->rails, X, Z, 4, 0.5 * e->s->upm)) return 5;
        }
        if (sc_canopy_green(e->s, c, qx, qz)) return 6;   /* [roam LA] wooded ground no OSM area maps: no row house */
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

/* the lot [a0, a1] with its front o0 and its back o1 behind the building line */
static int inf_lot_ring_at(const InfLine *L, double a0, double a1, double o0, double o1, double *fx, double *fz,
                           double *gx, double *gz, int *nf, double *mx, double *mz, int *seg)
{
    double tx[INF_PTS + 2], tz[INF_PTS + 2];
    int n2 = 0;
    if (!inf_lot_geom(L, a0, a1, o0, tx, tz, fx, fz, nf, mx, mz, seg)) return 0;
    if (!inf_lot_geom(L, a0, a1, o1, tx, tz, gx, gz, &n2, mx, mz, seg) || n2 != *nf) return 0;
    return 1;
}

/* the lot [a0, a1] with its front INF_FRONT_IN inside the building line and its back D behind that */
static int inf_lot_ring(const InfLine *L, double a0, double a1, double D, double *fx, double *fz, double *gx,
                        double *gz, int *nf, double *mx, double *mz, int *seg)
{
    return inf_lot_ring_at(L, a0, a1, INF_FRONT_IN, INF_FRONT_IN + D, fx, fz, gx, gz, nf, mx, mz, seg);
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
    int nf = 0, seg = L->r0, q, res = 0, own, why = 0, rail_hit = 0, rail_moved = 0, nr_lot = 0;
    double front = 0.0;                  /* [integ-roamI] the lot's front behind the building line when moved back */
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
        if (c->ngrd > 0 && sc_in_grounds(c, X - (double)c->org_x, Z - (double)c->org_z)) { why = 7; goto refused; }   /* [roam NA] */
        if (!city || sc_lot_class_at(c->lotp, c->nlotp, X, Z) != SC_LC_BUILD ||
            sc_lot_class_at(c->lotp, c->nlotp, mx + (double)c->org_x, mz + (double)c->org_z) != SC_LC_BUILD ||
            inf_near_landmark(e->bld, X, Z, INF_LM_CLEAR_M * U)) { why = 4; goto refused; }
    }
    if (!inf_lot_ring(L, a0, a1, D, fx, fz, gx, gz, &nf, &mx, &mz, &seg)) { why = 1; goto refused; }
    res = inf_lot_test(e, fx, fz, gx, gz, nf);
    if (res == 5 && s->ic_infill) {
        /* [integ-roamI] a railway runs along the block edge (FC Roca beside Avenida 72): the lot keeps its frontage
         * and steps back behind the bed + INF_RAIL_GAP_M, as deep as before but never past the half-block cap
         * (Dcap); refused only when less than INF_RAIL_MIN_M is left. No offset clear of the bed (the track runs
         * behind the lot): the shallowing below keeps the front instead. */
        double sb;
        rail_hit = 1;
        for (sb = INF_RAIL_STEP_M * U; sb + INF_RAIL_MIN_M * U <= Dcap + 1e-6; sb += INF_RAIL_STEP_M * U) {
            const double back = (sb + D < Dcap ? sb + D : Dcap);
            int r2;
            if (!inf_lot_ring_at(L, a0, a1, INF_FRONT_IN + sb, INF_FRONT_IN + back, fx, fz, gx, gz, &nf, &mx, &mz, &seg))
                break;
            r2 = inf_lot_test(e, fx, fz, gx, gz, nf);
            if (r2 == 5) continue;
            sb += INF_RAIL_GAP_M * U;
            {
                const double b2 = (sb + D < Dcap ? sb + D : Dcap);
                if (b2 - sb < INF_RAIL_MIN_M * U) { why = 5; goto refused; }
                if (!inf_lot_ring_at(L, a0, a1, INF_FRONT_IN + sb, INF_FRONT_IN + b2, fx, fz, gx, gz, &nf, &mx, &mz, &seg))
                    { why = 1; goto refused; }
                res = inf_lot_test(e, fx, fz, gx, gz, nf);
                if (res) { why = res; goto refused; }
                front = sb; D = b2 - sb; rail_moved = 1;
            }
            break;
        }
        if (!rail_moved) {   /* back to the lot at the building line for the shallowing */
            if (!inf_lot_ring(L, a0, a1, D, fx, fz, gx, gz, &nf, &mx, &mz, &seg)) { why = 1; goto refused; }
            res = 5;
        }
    }
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
        for (q2 = 0; q2 < 3 && (res == 3 || res == 4 || res == 5) && k_back[q2] * D0 >= INF_SHALLOW_M * U; q2++) {
            D = k_back[q2] * D0;
            if (!inf_lot_ring(L, a0, a1, D, fx, fz, gx, gz, &nf, &mx, &mz, &seg)) { why = 1; goto refused; }
            res = inf_lot_test(e, fx, fz, gx, gz, nf);
        }
    }
    if (res) { why = res; goto refused; }
    if (nf * 2 + (s->muni_on && s->ochava > 0.0 ? 2 : 0) > TD5_GEOB_RING_MAX) { why = 1; goto refused; }
    {   /* [roam NA] THE OCHAVA (La Plata's chamfered block corner): a lot at a corner of two fronts stops LG short of
         * the corner on its street and closes along the chamfer to the corner's bisector, where the other street's
         * lot takes the other half (both from this InfLine, so the two halves meet on one point M) */
        const double LG = s->muni_on ? s->ochava : 0.0;
        const int v0 = L->r0, v1 = (L->r0 + L->cnt) % L->n;
        int nfr = 0, cut0 = 0, cut1 = 0;
        double frx[INF_PTS + 4], frz[INF_PTS + 4];
        if (LG > 0.0 && L->och && !rail_moved && nf >= 2 && a0 <= 1e-6 && L->och[v0]) {
            const double ex = fx[1] - fx[0], ez = fz[1] - fz[0], el = hypot(ex, ez);
            const double c2 = el > 1e-6 ? (L->bx[v0] * ex + L->bz[v0] * ez) / el : 0.0;
            if (el > LG + 0.5 * U && c2 > 0.2) {
                frx[nfr] = fx[0] + L->bx[v0] * LG * c2; frz[nfr] = fz[0] + L->bz[v0] * LG * c2; nfr++;
                frx[nfr] = fx[0] + ex / el * LG; frz[nfr] = fz[0] + ez / el * LG; nfr++;
                cut0 = 1;
            }
        }
        for (q = cut0; q < nf; q++) { frx[nfr] = fx[q]; frz[nfr] = fz[q]; nfr++; }
        if (LG > 0.0 && L->och && !rail_moved && nf >= 2 && a1 >= L->arc[L->cnt] - 1e-6 && L->och[v1] &&
            !(nf == 2 && cut0)) {
            const double ex = fx[nf - 1] - fx[nf - 2], ez = fz[nf - 1] - fz[nf - 2], el = hypot(ex, ez);
            const double c2 = el > 1e-6 ? -(L->bx[v1] * ex + L->bz[v1] * ez) / el : 0.0;
            if (el > LG + 0.5 * U && c2 > 0.2) {
                const double X = fx[nf - 1], Z = fz[nf - 1];
                nfr--;                                       /* the corner point itself */
                frx[nfr] = X - ex / el * LG; frz[nfr] = Z - ez / el * LG; nfr++;
                frx[nfr] = X + L->bx[v1] * LG * c2; frz[nfr] = Z + L->bz[v1] * LG * c2; nfr++;
                cut1 = 1;
            }
        }
        for (q = 0; q < nfr; q++) { rx[q] = frx[q]; rz[q] = frz[q]; }
        for (q = 0; q < nf; q++) { rx[nfr + q] = gx[nf - 1 - q]; rz[nfr + q] = gz[nf - 1 - q]; }
        nr_lot = nfr + nf;
        if (!td5_geob_ring_simple(rx, rz, nr_lot)) { why = 1; goto refused; }
        if ((cut0 || cut1) && sc_owned_point(c, mx, mz)) c->st.inf_ochava++;
    }
    if (!inf_record(c, rx, rz, nr_lot)) return 0;
    if (!sc_owned_point(c, mx, mz)) return 1;          /* a split half: its own midpoint's tile emits it */
    if (rail_moved) c->st.inf_rail_moved++;
    else if (rail_hit) c->st.inf_rail_shallow++;
    if (s->infill_log)                                  /* TD5RE_ROAM_INFILL_LOG=1: where each lot stands (place frame) */
        TD5_LOG_I(LOG_TAG, "ROAM_INFILL tile %d,%d front (%.0f,%.0f) normal (%.3f,%.3f) depth %.1f m width %.1f m",
                  c->tx, c->tz, mx + L->nx[seg] * front + (double)c->org_x, mz + L->nz[seg] * front + (double)c->org_z,
                  L->nx[seg], L->nz[seg], D / U, (a1 - a0) / U);
    return inf_emit(e, rx, rz, nr_lot, h, fabs(td5_geob_ring_area(rx, rz, nr_lot)), a1 - a0);
refused:
    if (own) {
        if (why == 2) c->st.inf_drop_road++;
        else if (why == 3) c->st.inf_drop_real++;
        else if (why == 4 || why == 5 || why == 7) c->st.inf_drop_area++;
        else if (why == 6) c->st.inf_drop_canopy++;   /* [roam LA] */
        if (why == 7) c->st.grounds_inf++;            /* [roam NA] landmark grounds */
        else c->st.inf_drop_shape++;
        if (why == 5 || rail_hit) { c->st.inf_drop_rail++; c->st.inf_m_rail += (a1 - a0) / U; }
        if ((why == 5 || rail_hit) && s->infill_log)
            TD5_LOG_I(LOG_TAG, "ROAM_INFILL_RAIL tile %d,%d front (%.0f,%.0f) refused why %d (5 bed / too shallow, 4 area, 3 real, "
                      "2 road, 1 shape) block half %.1f m width %.1f m", c->tx, c->tz, mx + (double)c->org_x, mz + (double)c->org_z,
                      why, Dcap / U, (a1 - a0) / U);
        if (why == 3) c->st.inf_m_real += (a1 - a0) / U;
        else if (why == 4 || why == 5 || why == 7) c->st.inf_m_area += (a1 - a0) / U;
        else if (why == 6) c->st.inf_m_canopy += (a1 - a0) / U;   /* [roam LA] */
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
    fr = (unsigned char *)calloc((size_t)ch->nv * 3u, 1);
    if (!blk || !fr) goto out;
    L.ox = blk; L.oz = L.ox + ch->nv; L.nx = L.oz + ch->nv; L.nz = L.nx + ch->nv;
    L.bx = L.nz + ch->nv; L.bz = L.bx + ch->nv; L.bm = L.bz + ch->nv; L.arc = L.bm + ch->nv;
    brk = fr + ch->nv;
    L.och = fr + 2 * ch->nv;                 /* [roam NA] */
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
        L.och[i] = (unsigned char)(fr[p] && fr[i] && turn >= 30.0 && turn <= 150.0);   /* [roam NA] an ochava corner */
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

#ifndef TD5RE_RELEASE
/* [roam QB] GRASS OVERLAP census (DEV, --RoamBuildScenery with TD5RE_ROAM_GRASS_CENSUS=1, single thread only): every
 * up-facing face on a grass page (the lawn page R3_BLOCK, the GREEN page) of every tile built, rasterised on 1 m cells
 * of the place frame with the drawn ground under it; after the run, cells holding two or more grass faces are the
 * double grass (classed by the height gap between the top two and by where they come from). */
typedef struct { int32_t ix, iz; float y, g; int32_t tile, mesh; int16_t page; int16_t pad; } ScGrassCell;
static ScGrassCell *s_gc;
static size_t s_gc_n, s_gc_cap;
static int s_gc_on = -1;

static int sc_gc_page(int pg) { return pg == TD5_TG_PAGE_R3_BLOCK + 0 || pg == TD5_TG_PAGE_GREEN; }

static void sc_gc_tri(TG_TileCtx *c, const double *x, const double *y, const double *z, int mesh, int page)
{
    const double ux = x[1] - x[0], uy = y[1] - y[0], uz = z[1] - z[0], vx = x[2] - x[0], vy = y[2] - y[0], vz = z[2] - z[0];
    const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, L = sqrt(nx * nx + ny * ny + nz * nz);
    const double C = SC_UPM;
    double x0 = x[0], x1 = x[0], z0 = z[0], z1 = z[0], d;
    int k, i, j;
    if (L < 1e-9 || fabs(ny) / L < 0.7) return;                 /* up-facing (either winding) */
    for (k = 1; k < 3; k++) {
        if (x[k] < x0) x0 = x[k];
        if (x[k] > x1) x1 = x[k];
        if (z[k] < z0) z0 = z[k];
        if (z[k] > z1) z1 = z[k];
    }
    d = (z[1] - z[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (z[0] - z[2]);
    if (fabs(d) < 1e-9) return;
    for (j = (int)floor((z0 + c->org_z) / C); j <= (int)floor((z1 + c->org_z) / C); j++)
        for (i = (int)floor((x0 + c->org_x) / C); i <= (int)floor((x1 + c->org_x) / C); i++) {
            const double px = (i + 0.5) * C - c->org_x, pz = (j + 0.5) * C - c->org_z;
            const double w0 = ((z[1] - z[2]) * (px - x[2]) + (x[2] - x[1]) * (pz - z[2])) / d;
            const double w1 = ((z[2] - z[0]) * (px - x[2]) + (x[0] - x[2]) * (pz - z[2])) / d;
            const double w2 = 1.0 - w0 - w1;
            ScGrassCell *g;
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
            if (s_gc_n >= s_gc_cap) {
                const size_t nc = s_gc_cap ? s_gc_cap * 2 : (size_t)1 << 20;
                ScGrassCell *a = (ScGrassCell *)realloc(s_gc, nc * sizeof *a);
                if (!a) return;
                s_gc = a; s_gc_cap = nc;
            }
            g = &s_gc[s_gc_n++];
            g->ix = i; g->iz = j;
            g->y = (float)((w0 * y[0] + w1 * y[1] + w2 * y[2]) / C);
            g->g = (float)(sc_dg(c, px, pz) / C);
            g->tile = c->tx * 1000 + c->tz; g->mesh = mesh; g->page = (int16_t)page; g->pad = 0;
        }
}

static void sc_gc_record(TG_TileCtx *c, const uint8_t *rec, size_t avail, int mesh)
{
    const int tag = (int)(int16_t)(sc_rd32(rec) >> 16);
    const uint32_t ncmd = sc_rd32(rec + 4), nv = sc_rd32(rec + 8);
    const uint32_t cmd_off = sc_rd32(rec + TD5_MESH_DISK_OFF_COMMANDS), vtx_off = sc_rd32(rec + TD5_MESH_DISK_OFF_VERTICES);
    uint32_t ci, cursor = 0;
    if (tag || cmd_off + ncmd * 16u > avail || vtx_off + nv * 44u > avail) return;
    for (ci = 0; ci < ncmd; ci++) {
        const uint8_t *cm = rec + cmd_off + ci * 16u;
        const uint32_t tc = (uint32_t)(sc_rd32(cm + 8) & 0xFFFFu), qc = sc_rd32(cm + 8) >> 16;
        const int page = (int)(int16_t)(sc_rd32(cm) >> 16);
        uint32_t p;
        for (p = 0; p < tc + qc; p++) {
            const int nvp = p < tc ? 3 : 4;
            double x[4], y[4], z[4];
            int k;
            if (cursor + (uint32_t)nvp > nv) return;
            for (k = 0; k < nvp; k++) {
                const uint8_t *v = rec + vtx_off + (cursor + (uint32_t)k) * 44u;
                x[k] = (double)sc_rdf(v); y[k] = (double)sc_rdf(v + 4); z[k] = (double)sc_rdf(v + 8);
            }
            cursor += (uint32_t)nvp;
            if (!sc_gc_page(page)) continue;
            sc_gc_tri(c, x, y, z, mesh, page);
            if (nvp == 4) {
                const double x2[3] = { x[0], x[2], x[3] }, y2[3] = { y[0], y[2], y[3] }, z2[3] = { z[0], z[2], z[3] };
                sc_gc_tri(c, x2, y2, z2, mesh, page);
            }
        }
    }
}

static int sc_gc_cmp(const void *a, const void *b)
{
    const ScGrassCell *p = (const ScGrassCell *)a, *q = (const ScGrassCell *)b;
    if (p->iz != q->iz) return p->iz < q->iz ? -1 : 1;
    if (p->ix != q->ix) return p->ix < q->ix ? -1 : 1;
    return p->y > q->y ? -1 : (p->y < q->y ? 1 : 0);   /* top first */
}

/* the report: m2 of cells with >= 2 grass faces, by the gap between the top two (coplanar < 5 cm, 5-50 cm, > 50 cm),
 * by origin (same mesh, same tile other mesh, another tile), and the top grass face's height over the drawn ground */
static void sc_gc_report(FILE *f)
{
    size_t i = 0;
    long cells = 0, over = 0, gap[3] = { 0 }, org[3] = { 0 }, low[3] = { 0 }, lpage[2][2] = { { 0 } };
    long gdis = 0, gl_dis = 0;
    struct { int la, lb; long n; } pr[64];
    int npr = 0;
    char ln[900];
    if (!s_gc_n) return;
    qsort(s_gc, s_gc_n, sizeof *s_gc, sc_gc_cmp);
    while (i < s_gc_n) {
        size_t j = i + 1;
        while (j < s_gc_n && s_gc[j].ix == s_gc[i].ix && s_gc[j].iz == s_gc[i].iz) j++;
        cells++;
        {   /* the top face over the drawn ground: < 5 cm z-fights with the ground's own grass */
            const double h = s_gc[i].y - s_gc[i].g;
            low[h < 0.05 ? 0 : (h < 0.25 ? 1 : 2)]++;
        }
        if (j - i >= 2) {
            const ScGrassCell *a = &s_gc[i], *b = &s_gc[i + 1];
            const double dy = a->y - b->y;
            over++;
            gap[dy < 0.05 ? 0 : (dy < 0.5 ? 1 : 2)]++;
            if (fabs(a->g - b->g) >= 0.02) gdis++;                     /* the two tiles see different ground there */
            {   /* the (top, second) lift over the drawn ground, in cm, 2 cm bins */
                const int la = (int)floor((a->y - a->g) * 50.0 + 0.5) * 2, lb = (int)floor((b->y - b->g) * 50.0 + 0.5) * 2;
                int q;
                for (q = 0; q < npr; q++) if (pr[q].la == la && pr[q].lb == lb) break;
                if (q == npr && npr < 64) { pr[npr].la = la; pr[npr].lb = lb; pr[npr].n = 0; npr++; }
                if (q < npr) pr[q].n++;
            }
            if (dy >= 0.05 && fabs((a->y - a->g) - (b->y - b->g)) >= 0.02) gl_dis++;   /* different lift over their ground */
            org[a->tile != b->tile ? 2 : (a->mesh == b->mesh ? 0 : 1)]++;
            lpage[a->page == TD5_TG_PAGE_GREEN][b->page == TD5_TG_PAGE_GREEN]++;

        }
        i = j;
    }
    snprintf(ln, sizeof ln, "roam grass overlap [roam QB]: grass-covered %ld m2, under 2+ grass faces %ld m2 (top two coplanar < 5 cm %ld, "
             "5-50 cm %ld, > 50 cm %ld; same mesh %ld, same tile %ld, two tiles %ld; lawn/lawn %ld lawn/green %ld green/lawn %ld "
             "green/green %ld; ground differs between the two >= 2 cm %ld, lift over own ground differs %ld) | top grass face over the drawn ground < 5 cm %ld m2, 5-25 cm %ld, > 25 cm %ld",
             cells, over, gap[0], gap[1], gap[2], org[0], org[1], org[2], lpage[0][0], lpage[0][1], lpage[1][0], lpage[1][1],
             gdis, gl_dis, low[0], low[1], low[2]);
    fprintf(stderr, "%s\n", ln);
    TD5_LOG_I(LOG_TAG, "%s", ln);
    if (f) fprintf(f, "# %s\n", ln);
    {
        int q, w;
        for (w = 0; w < 10 && w < npr; w++) {       /* the 10 most common lift pairs */
            int b2 = w;
            for (q = w + 1; q < npr; q++) if (pr[q].n > pr[b2].n) b2 = q;
            if (b2 != w) { const int ta = pr[w].la, tb = pr[w].lb; const long tn = pr[w].n;
                           pr[w] = pr[b2]; pr[b2].la = ta; pr[b2].lb = tb; pr[b2].n = tn; }
            snprintf(ln, sizeof ln, "roam grass overlap pair: top %d cm / second %d cm over the drawn ground: %ld m2", pr[w].la, pr[w].lb, pr[w].n);
            fprintf(stderr, "%s\n", ln);
            if (f) fprintf(f, "# %s\n", ln);
        }
    }
    free(s_gc); s_gc = NULL; s_gc_n = s_gc_cap = 0;
}
#endif

static TD5_RoamScenery *sc_assemble(TG_TileCtx *c)
{
    TD5_RoamScenery *sc = (TD5_RoamScenery *)calloc(1, sizeof *sc);
    size_t total = 0, off = 0;
    int k, m;
    uint32_t nrec = 0, r = 0, h = 2166136261u;
    int *rec_furn = NULL;
    unsigned char *rec_tree = NULL;
    if (!sc) return NULL;
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) { total += c->sb[k].blk.len; nrec += (uint32_t)c->sb[k].nmesh; }
    sc->blob = (uint8_t *)malloc(total ? total : 1u);
    sc->rec_off = (uint32_t *)malloc((nrec ? nrec : 1u) * sizeof(uint32_t));
    rec_furn = (int *)calloc(nrec ? nrec : 1u, sizeof(int));
    rec_tree = (unsigned char *)calloc(nrec ? nrec : 1u, 1);
    if (!sc->blob || !sc->rec_off || !rec_furn || !rec_tree) { free(rec_furn); free(rec_tree); td5_roam_scn_free(sc); return NULL; }
    for (k = 0; k < TD5_ROAM_SCN_NSB; k++) {
        const ScSb *b = &c->sb[k];
        if (b->blk.len) memcpy(sc->blob + off, b->blk.b, b->blk.len);
        sc->sb_first[k] = r;
        sc->sb_count[k] = (uint32_t)b->nmesh;
        if (b->nmesh > 256) c->st.sb_over256++;                                      /* [roam QB] */
        if (b->nmesh > c->st.sb_max_meshes) c->st.sb_max_meshes = b->nmesh;
        for (m = 0; m < b->nmesh; m++) {
            rec_furn[r] = (m >= c->furn_first[k]);      /* [F4c] furniture: judged by its own census */
            if (m >= c->lot_first[k]) rec_furn[r] = 1;  /* [HD] a city lot is the ground itself (SECTION lots) */
            if (m >= c->tree_first[k] && m < c->tree_end[k]) { rec_furn[r] = 0; rec_tree[r] = 1; }   /* [roam LA] judged */
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
        int bad;
#ifndef TD5RE_RELEASE
        if (s_gc_on < 0) s_gc_on = td5_env_int("TD5RE_ROAM_GRASS_CENSUS", 0, 0, 1);
        if (s_gc_on) sc_gc_record(c, sc->blob + ro, (end > ro && end <= total) ? end - ro : total - ro, (int)r);
#endif
        if (rec_furn[r]) continue;
        bad = sc_census_record(c, sc->blob + ro, (end > ro && end <= total) ? end - ro : total - ro);
        c->st.guard_overlaps += bad;
        if (rec_tree[r]) c->st.st_on_road += bad;   /* [roam LA] */
    }
    free(rec_furn);
    free(rec_tree);
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
    s->pools_on  = td5_env_int("TD5RE_ROAM_POOLS", 1, 0, 1);
    s->stadium_on = td5_roam_stadium_enabled();   /* [roam PB] */
    s->pitch_on   = td5_roam_pitch_enabled();
    s->lot_cover = td5_env_int("TD5RE_ROAM_LOT_COVER", 1, 0, 1);
    s->infill_on = td5_env_int("TD5RE_ROAM_INFILL", 1, 0, 1);
    s->infill_log = td5_env_int("TD5RE_ROAM_INFILL_LOG", 0, 0, 1);
    s->bld_log = td5_env_int("TD5RE_ROAM_BLD_LOG", 0, 0, 1);
    s->cover_green = td5_env_int("TD5RE_ROAM_COVER_GREEN", 1, 0, 1);
    s->ic_despike = td5_env_int("TD5RE_ROAM_AREA_DESPIKE", 1, 0, 1);
    s->ic_drops = td5_env_int("TD5RE_ROAM_BLD_DROPS", 1, 0, 1);
    /* [roam NA] the municipal line + landmark grounds (SECTION municipal line); every knob 0 = round L's bytes */
    s->muni_on = td5_env_int("TD5RE_ROAM_MUNI_LINE", 1, 0, 1);
    s->muni_cut = td5_env_int("TD5RE_ROAM_MUNI_CUT", 1, 0, 1);
    s->muni_census = td5_env_int("TD5RE_ROAM_MUNI_CENSUS", 0, 0, 1);
    s->ring_log = td5_env_int("TD5RE_ROAM_RING_LOG", 0, 0, 1);
    s->grounds_on = td5_env_int("TD5RE_ROAM_LM_GROUNDS", 1, 0, 1);
    s->muni_max = (double)td5_env_int("TD5RE_ROAM_MUNI_MAX_CM", 1200, 0, 5000) * 0.01 * s->upm;
    s->muni_protrude = (double)td5_env_int("TD5RE_ROAM_MUNI_PROTRUDE_CM", 300, 0, 2000) * 0.01 * s->upm;
    s->ochava = (double)td5_env_int("TD5RE_ROAM_OCHAVA_CM", 300, 0, 1000) * 0.01 * s->upm;
    s->ic_infill = td5_env_int("TD5RE_ROAM_IC_INFILL", 1, 0, 1);
    s->canopy_green = td5_env_int("TD5RE_ROAM_CANOPY_GREEN", 1, 0, 1);   /* [roam LA] */
    s->inf_rail = td5_env_int("TD5RE_ROAM_INF_RAIL_VETO", 1, 0, 1);
    s->lot_page  = td5_env_int("TD5RE_ROAM_LOT_PAGE", TD5_TG_PAGE_R4_PIER, 0, 1023);   /* light cast concrete */
    s->gh_on = td5_roam_gh_enabled();                                                 /* [roam PC] */
    s->wet_on = td5_env_int("TD5RE_ROAM_OUT_COVER_WET", 1, 0, 1);
    s->shore_on = td5_env_int("TD5RE_ROAM_SHORE", 1, 0, 1);
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
    s->s3d = td5_env_int("TD5RE_ROAM_S3D", 1, 0, 1);
    s->pv = sc_pv_open(s);
    s->hc = (TD5_RoamHeight *)calloc(1, sizeof(TD5_RoamHeight));
    if (!sc_index_load(s)) TD5_LOG_W(LOG_TAG, "roam scenery: %s has no tiles/INDEX.JSON: no buildings", place);
    if (s->s3d) sc_rel_load(s);               /* [JC2] relation outlines + courtyards */
    s->park_rel = td5_env_int("TD5RE_ROAM_PARK_REL", 1, 0, 1);
    if (s->park_rel) sc_prel_load(s);         /* [LD] park / precinct multipolygons */
    s->ctx = (TG_TileCtx *)calloc(1, sizeof(TG_TileCtx));
    if (!s->ctx) { td5_roam_scn_close(s); return NULL; }
    s->furn = td5_roam_furn_open(place);      /* [F4c] NULL = no furniture (knob off / no data) */
    s->rails = td5_roam_rails_open(place);    /* [roam ID] NULL = no railways (knob off / no RAILS layer) */
    s->plaza = td5_roam_plaza_open(place);    /* [LD] NULL = no plaza furniture (knob off / no PLAZA_FURNITURE.JSON) */
    s->shops = td5_roam_shops_open(place);    /* [roam LB] NULL = TD5RE_ROAM_SHOPS=0 (POIS.JSON read on the first build) */
    if (s->rails) {
        int nt;
        td5_roam_rails_summary(s->rails, &nt, NULL, NULL, NULL, NULL);
        TD5_LOG_I(LOG_TAG, "roam rails: source open, %d RAILS tile(s) listed (parsed on the first tile build)", nt);
    }
    s->outsk = td5_roam_out_open(place);      /* [roam LH] NULL = no outskirts (knob off / no OUTSKIRTS layer) */
    if (s->outsk) {
        int nt;
        td5_roam_out_summary(s->outsk, &nt, NULL, NULL, NULL, NULL);
        TD5_LOG_I(LOG_TAG, "roam outskirts: source open, %d OUTSKIRTS tile(s) listed (parsed on the first tile build)", nt);
    }
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
    sc_cache_free(&s->rel);                   /* [JC2] */
    sc_cache_free(&s->prel);                  /* [LD] */
    free(s->relx);
    free(s->casco);
    td5_roam_furn_close(s->furn);
    td5_roam_rails_close(s->rails);
    td5_roam_plaza_close(s->plaza);
    td5_roam_shops_close(s->shops);
    td5_roam_out_close(s->outsk);
    if (s->hc) { if (s->hc_open) td5_roam_height_close(s->hc); free(s->hc); }
    free(s);
}

TD5_RoamScenery *td5_roam_scn_build(TD5_RoamScnSource *s, const RoamTile *self, TD5_RoamHeight *hc)
{
    TG_TileCtx *c;
    ScCache *bc[SC_CACHE_LRU + 1], *ac[SC_CACHE_LRU + 1], *fc[SC_CACHE_LRU];
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
    c->st_on = td5_env_int("TD5RE_ROAM_STREET_TREES", 1, 0, 1);   /* [roam LA] */
    c->shore_on = s->shore_on;                                     /* [roam PC] */
    c->lt_on = td5_env_int("TD5RE_ROAM_LOT_TREES", 1, 0, 1);
    c->pt_lot_set = td5_env_int("TD5RE_ROAM_PLAZA_TREE_PAGES", 1, 0, 1);   /* [integ L] */
#ifndef TD5RE_RELEASE
    c->xc_on = td5_env_int("TD5RE_ROAM_TREE_XCHECK", 0, 0, 1);              /* [integ L] dev census */
#endif
    c->tv_on = td5_env_int("TD5RE_ROAM_TREE_VETO_L", 1, 0, 1);              /* [integ L] */
    c->rt_casco = sc_casco_has(s, self->tx, self->tz);                       /* [roam PD] */
    c->rt_on = c->rt_casco && td5_env_int("TD5RE_ROAM_RULE_TREES", 1, 0, 1);
    c->tb_on = td5_env_int("TD5RE_ROAM_TREE_BLD", 1, 0, 1);
    c->rt_face = td5_env_int("TD5RE_ROAM_RULE_TREE_FACE", 1, 0, 1);
#ifndef TD5RE_RELEASE
    c->tree_log = td5_env_int("TD5RE_ROAM_TREE_LOG", 0, 0, 1);
#endif
    c->rails = s->rails;                                                     /* [integ L] */
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
    {   /* [roam IA] the render's ground lattice, for the lawns laid on it (sc_dg) */
        const int cm = td5_env_int("TD5RE_ROAM_RENDER_CELL_M", 16, 8, 64);
        c->dg_G = 256 / ((64 % cm) ? 16 : cm);
        c->dg_drop = td5_env_int("TD5RE_ROAM_RENDER_GROUND_DROP_CM", 40, 0, 500) * (SC_UPM / 100.0);
        c->lawn_dg = td5_env_int("TD5RE_ROAM_LAWN_ON_GROUND", 1, 0, 1);
        c->lawn_clip = c->lawn_dg && td5_env_int("TD5RE_ROAM_LAWN_CLIP", 1, 0, 1);   /* [roam QB] */
        c->lawn_gap_cen = 1;
        c->lawn_dedup = c->lawn_dg && td5_env_int("TD5RE_ROAM_LAWN_DEDUP", 1, 0, 1);   /* [roam QB] */
        c->sport_on = (s->stadium_on ? 1 : 0) | (s->pitch_on ? 2 : 0);   /* [roam PB] */
    }
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
        if (s->s3d && s->rel.n > 0) bc[nbc++] = &s->rel;   /* [JC2] the relation outlines, after the tiles */
        nac = sc_cache_window(s, SC_L_AREA, x0 - S, z0 - S, x1 + S, z1 + S, ac, k_sc_lru[SC_L_AREA]);
        if (s->park_rel && s->prel.n > 0) ac[nac++] = &s->prel;   /* [LD] the park relations, after the tiles */
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
                    ar[nar].spt = ac[l]->spt ? ac[l]->spt[ri] : 0;   /* [roam PB] */
                    ar[nar].srf = ac[l]->srf ? ac[l]->srf[ri] : 0;
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
    if (s->gh_on && !sc_kf_polys(ac, nac, x0 - (double)s->grid.tile, z0 - (double)s->grid.tile, x1 + (double)s->grid.tile,
                                 z1 + (double)s->grid.tile, SC_KF_GH, &c->ghp, &c->nghp)) goto out;      /* [roam PC] */
    if (s->wet_on && !sc_kf_polys(ac, nac, x0, z0, x1, z1, SC_KF_WET, &c->wetp, &c->nwetp)) goto out;
    if (s->cover_green && !sc_lot_polys(ac, nac, x0 - (double)s->grid.tile, z0 - (double)s->grid.tile,
                                        x1 + (double)s->grid.tile, z1 + (double)s->grid.tile, &c->anyp, &c->nanyp, 1)) goto out;
    {   /* [IC] the kerb loops first: ring plazas + avenue medians join c->lotp before any footprint is judged */
        const uint32_t tp = td5_plat_time_ms();
        if (!sc_pv_prepare(s, c, bc, nbc)) goto out;
        {   /* [roam NA] the municipal line; its footways reach as far as the footprints it judges (MU_JUDGE_M) */
            ScCache *mfc[SC_CACHE_LRU];
            const double S = (double)s->grid.tile;
            const int nmfc = s->muni_on ? sc_cache_window(s, SC_L_FOOT, x0 - S, z0 - S, x1 + S, z1 + S, mfc, k_sc_lru[SC_L_FOOT]) : 0;
            if (!sc_muni_prepare(s, c, bc, nbc, ar, nar, mfc, nmfc)) goto out;
        }
        c->st.ms_pv = td5_plat_time_ms() - tp;
    }
    t1 = td5_plat_time_ms();
    c->st.ms_src = t1 - t0;
    if (td5_env_int("TD5RE_ROAM_SCN_PLAZAS", 1, 0, 1) && !sc_emit_plazas(c, ar, nar)) goto out;
    if (td5_env_int("TD5RE_ROAM_SCN_BUILDINGS", 1, 0, 1) && !sc_emit_buildings(s, c, bc, nbc, ar, nar)) goto out;
    if (!sc_emit_pavement(s, c, bc, nbc, ar, nar)) goto out;            /* [F4b] */
    if (!sc_shops_deferred(s, c)) goto out;                             /* [roam PE] after the infill */
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
    {
        int xs[TD5_ROAM_SCN_NSB];
        sc_xc_snap(c, xs);
        if (s->pools_on && !sc_emit_pools(c, self, ac, nac)) goto out;   /* [roam LE] over the lots */
        sc_xc_note(c, xs, SC_XC_POOL);                                   /* [integ L] */
    }
    if (s->outsk && !sc_casco_has(s, c->tx, c->tz) && td5_env_int("TD5RE_ROAM_OUT_COVER", 1, 0, 1)) {   /* [roam LH] */
        if (!sc_emit_cover(s, c)) goto out;
        if (td5_env_int("TD5RE_ROAM_OUT_LOG", 0, 0, 1))
            TD5_LOG_I(LOG_TAG, "roam cover tile %d,%d: cover_ok %d, cropland cells %d, wetland cells %d, left to the lawn (road) %d",
                      c->tx, c->tz, c->cover_ok, c->st.out.cover_crop, c->st.out.cover_wet, c->st.out.cover_road_skip);
    }
    /* [F4c] street furniture: appended LAST in every sub-block (meshes from furn_first on are its own).
     * [roam ID] the railways go first in that tail: a rail laid flush on a level crossing is ON the road by
     * design, so the F4a footprint census does not judge them either; td5_roam_rails.c runs its own,
     * height-aware one (st.rail.guard_hits). */
    if (s->furn || s->rails || s->plaza || s->shops || s->outsk || td5_roam_cut_on() || s->stadium_on || s->pitch_on) {
        int k;
        for (k = 0; k < TD5_ROAM_SCN_NSB; k++) c->furn_first[k] = c->sb[k].nmesh;
    }
    {
        int xs[TD5_ROAM_SCN_NSB];                                                /* [integ L] cross-check ranges */
        sc_xc_snap(c, xs);
        if (!td5_roam_cut_emit(c, &c->st.cut, sc_casco_has(s, c->tx, c->tz))) goto out;   /* [roam NC] cutting walls */
        sc_xc_snap(c, xs);
        if (!sc_emit_sport(s, c, bc, nbc, ar, nar)) goto out;                              /* [roam PB] stadiums + pitches */
        sc_xc_note(c, xs, SC_XC_SPORT); sc_xc_snap(c, xs);                               /* [integ P] */
        if (s->rails && !td5_roam_rails_emit(s->rails, c, &c->st.rail)) goto out;
        sc_xc_note(c, xs, SC_XC_RAIL); sc_xc_snap(c, xs);
        if (s->outsk && !td5_roam_out_emit(s->outsk, c, &c->st.out, sc_casco_has(s, c->tx, c->tz))) goto out;   /* [roam LH] */
        sc_xc_note(c, xs, SC_XC_OUT); sc_xc_snap(c, xs);
        /* [LD] plaza furniture + monuments: after the railways, before the street furniture */
        if (s->plaza && !td5_roam_plaza_emit(s->plaza, c, &c->st.plaza)) goto out;
        sc_xc_note(c, xs, SC_XC_PLAZA); sc_xc_snap(c, xs);
        /* [roam LB] the fuel canopies planned in the building pass (a deck may span a forecourt drive) */
        if (s->shops && !td5_roam_shops_fuel_flush(s->shops, c, &c->st.shop)) goto out;
        sc_xc_note(c, xs, SC_XC_FUEL);
    }
    if (s->furn) {
        int xs[TD5_ROAM_SCN_NSB];
        sc_xc_snap(c, xs);
        if (!td5_roam_furn_emit(s->furn, c, &c->st.furn, &c->lamp, &c->nlamp)) goto out;
        if (c->xc_on) sc_xc_note(c, xs, SC_XC_FURN);                     /* [integ L] cross-check only */
    }
    /* [roam LA] street + lot trees last: they keep off the furniture and rail faces just written */
    if (!sc_emit_street_trees(c)) goto out;
#ifndef TD5RE_RELEASE
    if (c->xc_on) sc_tree_xcheck(c);                                             /* [integ L] dev census */
#endif
#ifndef TD5RE_RELEASE
    sc_qb_probe(c);                                                               /* [roam QB] dev */
#endif
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
    if (c->pv_bld) { pv_bld_free((PvBld *)c->pv_bld); free(c->pv_bld); c->pv_bld = NULL; }   /* [roam LA] */
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
    long tot[24], pv[16], pv_src[7], lm_stub = 0, guard_lm = 0, pv_fill = 0, pv_tops = 0, pv_topref = 0;
    uint32_t pv_ms_max = 0;
    long ct_i[8] = { 0 };                       /* [roam NC] */
    double ct_d[5] = { 0 }, ct_cen[2][6] = { { 0 } };
    long pa_t[8] = { 0 }; double pa_m[3] = { 0.0, 0.0, 0.0 };   /* [roam PA] lane paint census ([integ P] pa_t[7] off a rail bed) */
    long ft[16], lc_t[6] = { 0 }, ft_unknown_skipped = 0, lot_t[12] = { 0 }, ic_t[4] = { 0, 0, 0, 0 }, rl_t[24] = { 0 }, lf_t[24] = { 0 }, lf_wire_m = 0.0, ou_t[16] = { 0 }, cov_t[3] = { 0 }, inf_rail_n = 0, inf_rail_mv = 0, inf_rail_sh = 0;
    double ie_m[4] = { 0.0, 0.0, 0.0, 0.0 };   /* [roam IE] */
    long s3d_t[10] = { 0 }, s3d_tag = 0, s3d_slope = 0;   /* [roam JC] */
    long s3d_r[9] = { 0 };                                 /* [roam JC2] */
    long lt[16] = { 0 };                                   /* [roam LA] */
    long rt_t[12] = { 0 }, tb_t[12] = { 0 }, rt_face_n = 0;   /* [roam PD] */
    double rt_m[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    long ptv[TD5_TG_TREE_VARIANTS] = { 0 };                /* [integ L] plaza trees per tree page variant */
    long pt_drop[2] = { 0, 0 };                            /* [integ L] plaza trees vetoed: faces, water */
    long xct[2] = { 0, 0 }, xtr[2][9] = { { 0 } }, xcr[2][9] = { { 0 } }, xwt[2] = { 0, 0 };   /* [integ L] cross-check */
    long xfr[2] = { 0, 0 }, xr_drop = 0;
    long cg_n[2] = { 0, 0 };
    double cg_m = 0.0;
    long ie_n[6] = { 0, 0, 0, 0, 0, 0 };
    long pool_n[3] = { 0, 0, 0 };              /* [roam LE] drawn, on a road, bad ring */
    long pc_t[18] = { 0 };                     /* [roam PC] greenhouses, marsh, lattice towers, shore lots */
    double pc_m2 = 0.0;
    double pool_m2 = 0.0;
    long pz_t[13] = { 0 };                     /* [roam LD] plaza furniture + park relations */
    TD5_RoamSportStats sp_t = { 0 };           /* [roam PB] stadiums + pitches */
    long qb_lc[4] = { 0 };                     /* [roam QB] lawn clip: pieces, dropped, fallback, lawn tris */
    double qb_lm[2] = { 0.0, 0.0 };            /* [roam QB] lawn border along the road (m), gap (m2) */
    double qb_dd[2] = { 0.0, 0.0 };            /* [roam QB] lawn left to another plaza lawn (m2), dedupe fallbacks */
    long qb_sb[2] = { 0, 0 };                  /* [roam QB] sub-blocks over 256 meshes, the most meshes in one */
    long sp_b[4] = { 0 };                      /* [roam PB] stands left to the module, blobs traced over one, plaza trees skipped */
    long sh_t[16] = { 0 }, sh_cat[TD5_GEOB_POI__N] = { 0 };   /* [roam LB] */
    long mu_t[32] = { 0 };                                     /* [roam NA] */
    long pe_t[16] = { 0 };                                     /* [roam PE] */
    double sh_m = 0.0;
    double shift_max = 0.0, lot_m2[4] = { 0.0, 0.0, 0.0, 0.0 }, ic_m[4] = { 0.0, 0.0, 0.0, 0.0 }, rl_lift_max = 0.0, inf_rail_m = 0.0;
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
    } else if (_strnicmp(val, "list:", 5) == 0) {       /* [roam IE] */
        int (*l)[2] = NULL, k;
        const int nl = td5_roam_read_tile_list(val + 5, &l);
        for (k = 0; k < nl && j.n < cap; k++) { j.list[j.n][0] = l[k][0]; j.list[j.n][1] = l[k][1]; j.n++; }
        free(l);
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
    td5_roam_shops_atlas_prepare(j.place);                /* [roam PE] the shop-name bank, same rule */
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
        inf_rail_n += r->inf_drop_rail; inf_rail_m += r->inf_m_rail;   /* [integ-roamI] */
        {   /* [roam JC] */
            int q;
            s3d_t[0] += r->bld_s3d; s3d_t[1] += r->bld_s3d_hidden; s3d_t[2] += r->s3d_roof_only; s3d_t[3] += r->s3d_tint;
            s3d_tag += r->s3d_tagged; s3d_slope += r->s3d_sloped;
            s3d_r[0] += r->bld_rel; s3d_r[1] += r->s3d_rel_built; s3d_r[2] += r->s3d_holes; s3d_r[3] += r->s3d_cap_holed;
            s3d_r[4] += r->s3d_cap_lidded; s3d_r[5] += r->s3d_civic; s3d_r[6] += r->s3d_base; s3d_r[7] += r->bld_rel_ovt_drop;
            s3d_r[8] += r->s3d_portals;
            for (q = 0; q < 6; q++) s3d_t[4 + q] += r->s3d_how[q];
        }
        inf_rail_mv += r->inf_rail_moved; inf_rail_sh += r->inf_rail_shallow;
        lt[0] += r->st_band; lt[1] += r->st_lot; lt[2] += r->trees; lt[3] += r->st_stations; lt[4] += r->st_nocanopy;
        lt[5] += r->st_narrow; lt[6] += r->st_facade; lt[7] += r->st_noslab; lt[8] += r->st_furn_drop;
        lt[9] += r->st_furn_moved; lt[10] += r->st_border; lt[11] += r->st_lot_hidden; lt[12] += r->st_lot_refused;
        lt[13] += r->st_on_road; lt[14] += r->canopy_cells; lt[15] += r->canopy_cov;
        {   /* [roam PD] */
            int q;
            rt_face_n += r->rt_face;
            rt_t[0] += r->rt_av; rt_t[1] += r->rt_diag; rt_t[2] += r->rt_med; rt_t[3] += r->rt_stations;
            rt_t[4] += r->rt_canopy; rt_t[5] += r->rt_narrow; rt_t[6] += r->rt_facade; rt_t[7] += r->rt_noslab;
            rt_t[8] += r->rt_fuel; rt_t[9] += r->rt_furn; rt_t[10] += r->rt_border; rt_t[11] += r->rt_double;
            for (q = 0; q < 2; q++) { rt_m[q] += r->rt_kerb_m[q]; rt_m[2 + q] += r->rt_cov0_m[q]; rt_m[4 + q] += r->rt_cov1_m[q]; }
            for (q = 0; q < 4; q++) { tb_t[q] += r->tb_left[q]; tb_t[4 + q] += r->tb_moved[q]; tb_t[8 + q] += r->tb_drop[q]; }
        }
        { int q; for (q = 0; q < TD5_TG_TREE_VARIANTS; q++) ptv[q] += r->plaza_tree_var[q]; }   /* [integ L] */
        pt_drop[0] += r->pt_furn_drop; pt_drop[1] += r->pt_wet_drop;
        {   /* [integ L] tree cross-check */
            int q, w;
            for (w = 0; w < 2; w++) {
                xct[w] += r->xc_trees[w]; xwt[w] += r->xc_wet[w];
                if (!w) { xfr[0] += r->xc_xing_rail; xfr[1] += r->xc_furn_rail; }
                for (q = 0; q < 9; q++) { xtr[w][q] += r->xc_trunk[w][q]; xcr[w][q] += r->xc_crown[w][q]; }
            }
        }
        cg_n[0] += r->lot_canopy_green; cg_n[1] += r->inf_drop_canopy; cg_m += r->inf_m_canopy;
        lot_t[0] += r->lot_cells; lot_t[1] += r->lot_green; lot_t[2] += r->lot_rural; lot_t[3] += r->lot_under_road; lot_t[4] += r->lot_rings;
        lot_t[6] += r->inf_lots; lot_t[7] += r->inf_emitted;
        lot_t[8] += r->inf_drop_real; lot_t[9] += r->inf_drop_road; lot_t[10] += r->inf_drop_area;
        lot_t[11] += r->inf_drop_shape;
        lot_m2[0] += r->lot_paved_m2; lot_m2[1] += r->lot_lawn_m2; lot_m2[2] += r->lot_green_m2; lot_m2[3] += r->inf_m2;
        {
            int q;
            for (q = 0; q < 7; q++) pv_src[q] += r->pv_src[q];
            pv[0] += r->pv_bedges; pv[1] += r->pv_interior; pv[2] += r->pv_pieces; pv[3] += r->pv_corner;
            pv_fill += r->pv_island_fill;                 /* [roam IA] */
            pv_tops += r->pv_island_tops; pv_topref += r->pv_island_top_refused;
            pv[4] += r->pv_collapsed; pv[5] += r->pv_island; pv[6] += r->pv_median; pv[7] += r->pv_free_clamp;
            pv[8] += r->pv_fence; pv[9] += r->pv_rail; pv[10] += r->pv_kerb; pv[11] += r->pv_cut;
            pv[12] += r->pv_drop; pv[13] += r->pv_kerb_cut; pv[14] += r->pv_noway; pv[15] += (long)r->ms_pv;
            if (r->ms_pv > pv_ms_max) pv_ms_max = r->ms_pv;
            lm_stub += r->bld_lm_stub; guard_lm += r->guard_lm;
        }
        ft[0] += r->furn.meshes; ft[1] += r->furn.sig_clusters; ft[2] += r->furn.sig_inbound; ft[3] += r->furn.mast_near;
        ft[4] += r->furn.post_left; ft[5] += r->furn.mast_far; ft[6] += r->furn.plates; ft[7] += r->furn.lamps;
        ft[8] += r->furn.stops; ft[9] += r->furn.zebras; ft[10] += r->furn.limit_lines; ft[11] += r->furn.guard_hits;
        ft_unknown_skipped += r->furn.xing_unknown; lc_t[0] += r->furn.xing_unmarked; lc_t[1] += r->furn.xing_unk_zebra;
        lc_t[2] += r->furn.xing_unk_lines; lc_t[3] += r->furn.xing_ways; lc_t[4] += r->furn.xing_way_dup; lc_t[5] += r->furn.xing_way_drawn;
        xr_drop += r->furn.xing_rail_drop;                                  /* [integ L] */
        ft[12] += r->furn.sig_drop_road + r->furn.sig_drop_frame + r->furn.sig_drop_dup;
        ft[13] += r->furn.lamp_drop_road; ft[14] += r->furn.far_shared; ft[15] += r->furn.plate_none;
        rl_t[0] += r->rail.ways; rl_t[1] += r->rail.pieces; rl_t[2] += r->rail.full; rl_t[3] += r->rail.narrow;
        rl_t[4] += r->rail.flush; rl_t[5] += r->rail.flush_nodata; rl_t[6] += r->rail.blend; rl_t[7] += r->rail.bridge;
        rl_t[8] += r->rail.piers; rl_t[9] += r->rail.lifted; rl_t[10] += r->rail.under_deck; rl_t[11] += r->rail.tunnel_skip;
        rl_t[12] += r->rail.sleepers; rl_t[13] += r->rail.platforms; rl_t[14] += r->rail.plat_pieces;
        rl_t[15] += r->rail.plat_drop; rl_t[16] += r->rail.meshes; rl_t[17] += r->rail.guard_hits;
        rl_t[19] += r->rail.sleeper_cut; rl_t[20] += r->rail.wall_cut; rl_t[21] += r->rail.squeezed; rl_t[22] += r->rail.nudged; rl_t[23] += r->rail.floored;
        {   /* [roam NC] cuttings */
            int q, w;
            ct_i[0] += r->cut.spans; ct_i[1] += r->cut.pieces; ct_i[2] += r->cut.deck_pieces; ct_i[3] += r->cut.apron_cut;
            ct_i[4] += r->cut.on_road; ct_i[5] += r->cut.low_skip; ct_i[6] += r->cut.pv_skip; if (r->cut.spans) ct_i[7]++;
            ct_d[0] += r->cut.walls_m; ct_d[1] += r->cut.parapet_m; ct_d[2] += r->cut.apron_m;
            if (r->cut.max_h_m > ct_d[3]) ct_d[3] = r->cut.max_h_m;
            for (w = 0; w < 2; w++) for (q = 0; q < 6; q++) ct_cen[w][q] += r->cut.cen_m[w][q];
            ct_d[4] += r->cut.verge_m;
        }
        if (r->rail.pieces) rl_t[18]++;
        {   /* [roam LB] */
            int q;
            sh_t[0] += r->shop.cand; sh_t[1] += r->shop.from_tag; sh_t[2] += r->shop.from_poi;
            sh_t[3] += r->shop.dressed; sh_t[4] += r->shop.fronts; sh_t[5] += r->shop.no_front;
            sh_t[6] += r->shop.skip_kind; sh_t[7] += r->shop.skip_low; sh_t[8] += r->shop.awnings;
            sh_t[9] += r->shop.awning_tight; sh_t[10] += r->shop.crosses; sh_t[11] += r->shop.canopies;
            sh_t[12] += r->shop.canopy_refused; sh_t[13] += r->shop.meshes;
            sh_t[14] += r->shop.canopy_islands; sh_t[15] += r->shop.canopy_service;
            for (q = 0; q < TD5_GEOB_POI__N; q++) sh_cat[q] += r->shop.by_cat[q];
            sh_m += r->shop.front_m;
        }
        pa_t[0] += r->furn.lane_bus_segs; pa_t[1] += r->furn.lane_bike_segs; pa_t[2] += r->furn.lane_track_segs; pa_t[3] += r->furn.lane_kerbs;
        pa_t[4] += r->furn.lane_ways; pa_t[5] += r->furn.lane_no_dir; pa_t[6] += r->furn.lane_narrow; pa_t[7] += r->furn.lane_rail;
        pa_m[0] += r->furn.lane_bus_m; pa_m[1] += r->furn.lane_bike_m; pa_m[2] += r->furn.lane_track_m;
        lf_t[0] += r->rail.lx_nodes; lf_t[1] += r->rail.lx_equipped; lf_t[2] += r->rail.lx_untagged;
        lf_t[3] += r->rail.lx_barrier_half; lf_t[4] += r->rail.lx_barrier_full; lf_t[5] += r->rail.lx_lights; lf_t[6] += r->rail.lx_saltires; lf_t[21] += r->rail.lx_untagged_x;
        lf_t[7] += r->rail.lx_no_road; lf_t[8] += r->rail.lx_no_rail; lf_t[9] += r->rail.lx_post_drop;
        lf_t[10] += r->rail.stn_boards; lf_t[11] += r->rail.stn_named; lf_t[12] += r->rail.stn_plain; lf_t[13] += r->rail.stn_on_platform;
        lf_t[14] += r->rail.stn_by_track; lf_t[15] += r->rail.stn_drop;
        lf_t[16] += r->rail.cat_ways; lf_t[17] += r->rail.cat_masts; lf_t[18] += r->rail.cat_mast_drop; lf_t[19] += r->rail.cat_wire_pieces;
        lf_t[20] += r->rail.cat_skip_deck; lf_wire_m += r->rail.cat_wire_m;
        ou_t[0] += r->out.poles; ou_t[1] += r->out.wood; ou_t[2] += r->out.concrete; ou_t[3] += r->out.towers;
        ou_t[4] += r->out.wires; ou_t[5] += r->out.wire_quads; ou_t[6] += (long)(r->out.wire_m + 0.5);
        ou_t[7] += r->out.guards; ou_t[8] += r->out.guard_posts; ou_t[9] += (long)(r->out.guard_m + 0.5);
        ou_t[10] += r->out.gates; cov_t[0] += r->out.cover_crop; cov_t[1] += r->out.cover_wet; cov_t[2] += r->out.cover_road_skip; ou_t[11] += r->out.gates_lift; ou_t[12] += r->out.on_road_skipped;
        ou_t[13] += r->out.portal_skipped; ou_t[14] += r->out.guard_hits; ou_t[15] += r->out.meshes;
        if (r->rail.lift_max_m > rl_lift_max) rl_lift_max = r->rail.lift_max_m;
        tot[0] += j.ok[i]; tot[1] += r->meshes; tot[2] += r->bytes; tot[3] += r->bld_seen; tot[4] += r->bld_emitted;
        tot[5] += r->bld_prefab; tot[6] += r->bld_drop_road; tot[7] += r->bld_drop_shift; tot[8] += r->bld_veto_plaza;
        tot[9] += r->bld_veto_lm; tot[10] += r->fp_pieces; tot[11] += r->fp_cut_road; tot[12] += r->fp_merged;
        tot[13] += r->areas; tot[14] += r->lawn_tris; tot[15] += r->trees; tot[16] += r->guard_overlaps;
        tot[17] += r->bld_nudged; tot[18] += r->bld_apron; tot[19] += r->lawn_cut; tot[20] += r->trees_cut;
        tot[21] += r->bld_drop_ring;
        ie_m[0] += r->furn.bar_guard_m; ie_m[1] += r->furn.bar_nj_m; ie_m[2] += r->furn.bar_parapet_m; ie_m[3] += r->furn.emb_m;
        ie_n[0] += r->furn.bar_drop_road; ie_n[1] += r->furn.bar_patch_edges; ie_n[2] += r->furn.piers;
        ie_n[3] += r->furn.pier_cols; ie_n[4] += r->furn.pier_drop_road; ie_n[5] += r->furn.abutments;
        pool_n[0] += r->pools; pool_n[1] += r->pool_drop_road; pool_n[2] += r->pool_bad; pool_m2 += r->pool_m2;
        pc_t[0] += r->gh.cand_osm; pc_t[1] += r->gh.cand_area; pc_t[2] += r->gh.drawn; pc_t[3] += r->gh.spans;   /* [roam PC] */
        pc_t[4] += r->gh.small; pc_t[5] += r->gh.tall; pc_t[6] += r->gh.irregular; pc_t[7] += r->gh.road; pc_t[8] += r->gh.quads;
        pc_m2 += r->gh.m2;
        pc_t[9] += r->out.cover_wet; pc_t[10] += r->out.cover_marsh_area; pc_t[11] += r->out.cover_reeds;
        pc_t[12] += r->out.lattice_towers; pc_t[13] += r->out.lattice_members; pc_t[14] += r->lot_shore; pc_t[15] += r->out.towers;
        pc_t[16] += r->lawn_shore; pc_t[17] += r->out.cover_shore_skip;
        qb_lc[0] += r->lawn_clip; qb_lc[1] += r->lawn_clip_drop; qb_lc[2] += r->lawn_clip_fallback; qb_lc[3] += r->lawn_tris;
        qb_lm[0] += r->lawn_border_m; qb_lm[1] += r->lawn_gap_m2;
        qb_dd[0] += r->lawn_dedup_m2; qb_dd[1] += r->lawn_dedup_fallback;
        qb_sb[0] += r->sb_over256; if (r->sb_max_meshes > qb_sb[1]) qb_sb[1] = r->sb_max_meshes;
        pz_t[0] += r->plaza.items; pz_t[1] += r->plaza.statues; pz_t[2] += r->plaza.monuments; pz_t[3] += r->plaza.memorials;
        pz_t[4] += r->plaza.fountains; pz_t[5] += r->plaza.benches; pz_t[6] += r->plaza.playgrounds; pz_t[7] += r->plaza.flagpoles;
        pz_t[8] += r->plaza.named; pz_t[9] += r->plaza.skip_kind; pz_t[10] += r->plaza.drop_road; pz_t[11] += r->plaza.guard_hits;
        pz_t[12] += r->park_rel;
        {   /* [roam PB] */
            int q;
            sp_t.stadiums += r->sport.stadiums; sp_t.bowls += r->sport.bowls; sp_t.stands += r->sport.stands;
            sp_t.stand_roofs += r->sport.stand_roofs; sp_t.roofs += r->sport.roofs; sp_t.portals += r->sport.portals;
            sp_t.tier_quads += r->sport.tier_quads; sp_t.openings += r->sport.openings; sp_t.seats += r->sport.seats; sp_t.pitches += r->sport.pitches;
            for (q = 0; q < TD5_RPS__N; q++) sp_t.by_sport[q] += r->sport.by_sport[q];
            for (q = 0; q < TD5_RPF__N; q++) sp_t.by_surface[q] += r->sport.by_surface[q];
            sp_t.surface_only += r->sport.surface_only; sp_t.skip_small += r->sport.skip_small; sp_t.skip_sport += r->sport.skip_sport;
            sp_t.skip_none += r->sport.skip_none; sp_t.drop_road += r->sport.drop_road; sp_t.courts += r->sport.courts;
            sp_t.goals += r->sport.goals; sp_t.nets += r->sport.nets; sp_t.hoops += r->sport.hoops;
            sp_t.line_quads += r->sport.line_quads; sp_t.meshes += r->sport.meshes; sp_t.guard_hits += r->sport.guard_hits;
            sp_b[0] += r->bld_stand; sp_b[1] += r->bld_stand_dup; sp_b[2] += r->sp_tree_skip; sp_b[3] += r->sp_lawn_skip;
            for (q = 0; q < TD5_RSB__N; q++) {   /* [roam QB] */
                sp_t.foot_n[q] += r->sport.foot_n[q]; sp_t.foot_over[q] += r->sport.foot_over[q];
                sp_t.foot_sum[q] += r->sport.foot_sum[q];
                if (r->sport.foot_max[q] > sp_t.foot_max[q]) sp_t.foot_max[q] = r->sport.foot_max[q];
            }
            sp_t.open_n += r->sport.open_n; sp_t.open_m += r->sport.open_m; sp_t.pulls += r->sport.pulls; sp_t.pull_m += r->sport.pull_m;
        }
        {   /* [roam NA] */
            int q;
            for (q = 0; q < 10; q++) mu_t[q] += r->muni_fate[q];
            for (q = 0; q < 4; q++) mu_t[10 + q] += r->muni_skip[q];
            for (q = 0; q < 7; q++) mu_t[14 + q] += r->muni_gap[q];
            mu_t[21] += r->muni_segs; mu_t[22] += r->grounds; mu_t[23] += r->grounds_ovt; mu_t[24] += r->grounds_inf;
            mu_t[25] += r->grounds_osm;
            mu_t[26] += r->muni_fwd; mu_t[27] += r->muni_back; mu_t[28] += r->muni_corner; mu_t[29] += r->muni_ochava;
            mu_t[30] += r->inf_ochava;
        }
        {   /* [roam PE] */
            int q;
            for (q = 0; q < 4; q++) pe_t[q] += r->muni_cut_over[q];
            pe_t[4] += r->muni_cut_skip; pe_t[5] += r->shop.canopy_fp; pe_t[6] += r->shop.canopy_fp_kept;
            pe_t[7] += r->shop.canopy_fp_site; pe_t[8] += r->shop.abut_drop; pe_t[9] += r->shop.abut_trim;
            pe_t[10] += r->shop_inf_guard; pe_t[11] += r->shop.name_signs; pe_t[12] += r->shop.name_nocell;
            pe_t[13] += r->shop.name_none;
        }
        if (r->bld_shift_max > shift_max) shift_max = r->bld_shift_max;
        if (r->ms > ms_max) ms_max = r->ms;
        if (r->bytes > bytes_max) bytes_max = r->bytes;
        ms_sum += r->ms;
    }
    {   /* [roam NA] */
        char ln[1400];
        snprintf(ln, sizeof ln, "roam muni [roam NA]: %ld line piece(s) | footprints emitted: none facing a line %ld, on the line "
                 "%ld, SNAPPED %ld (walls brought forward %ld, cut back %ld; block corners filled %ld, new ochavas %ld), left: inside the block (> max) %ld, blocked by a neighbour %ld, corner / line end %ld, ring %ld, meets the road on "
                 "the line %ld, a part still in front of the line %ld | kept by "
                 "rule: landmark / civic / part / relation %ld, fuel %ld, park / plaza / open area / grounds %ld, outside the "
                 "casco %ld | street walls by gap to the line: ON THE PAVEMENT %ld, 0 %ld, < 1 m %ld, 1-3 m %ld, 3-6 m %ld, "
                 "6-12 m %ld, 12-20 m %ld | landmark grounds: %ld block(s), Overture blobs not built %ld, infill lots refused "
                 "%ld, OSM footprints kept %ld | infill corner lots cut by an ochava %ld (TD5RE_ROAM_MUNI_LINE=%d TD5RE_ROAM_LM_GROUNDS=%d TD5RE_ROAM_OCHAVA_CM=%d)",
                 mu_t[21], mu_t[TD5_MUNI_NONE], mu_t[TD5_MUNI_ON], mu_t[TD5_MUNI_SNAPPED], mu_t[26], mu_t[27], mu_t[28], mu_t[29],
                 mu_t[TD5_MUNI_FAR],
                 mu_t[TD5_MUNI_BLOCKED], mu_t[TD5_MUNI_CORNER], mu_t[TD5_MUNI_RING], mu_t[TD5_MUNI_ROAD], mu_t[TD5_MUNI_FRONT],
                 mu_t[10], mu_t[11], mu_t[12], mu_t[13],
                 mu_t[14], mu_t[15], mu_t[16], mu_t[17], mu_t[18], mu_t[19], mu_t[20], mu_t[22], mu_t[23], mu_t[24], mu_t[25], mu_t[30],
                 td5_env_int("TD5RE_ROAM_MUNI_LINE", 1, 0, 1), td5_env_int("TD5RE_ROAM_LM_GROUNDS", 1, 0, 1),
                 td5_env_int("TD5RE_ROAM_OCHAVA_CM", 300, 0, 1000));
        TD5_LOG_I(LOG_TAG, "%s", ln);
        if (f) fprintf(f, "# %s\n", ln);
        fprintf(stderr, "%s\n", ln);
    }
    {   /* [roam LE] */
        char ln[200];
        snprintf(ln, sizeof ln, "roam pools [roam LE]: %ld drawn (%.0f m2 of water), %ld left out on the drivable surface, "
                 "%ld bad ring(s)", pool_n[0], pool_m2, pool_n[1], pool_n[2]);
        TD5_LOG_I(LOG_TAG, "%s", ln);
        if (f) fprintf(f, "# %s\n", ln);
        fprintf(stderr, "%s\n", ln);
    }
    {   /* [roam PC] */
        char ln[900];
        snprintf(ln, sizeof ln, "roam PC: greenhouses %ld drawn (%ld spans, %.0f m2, %ld quads) of %ld OSM building=greenhouse + %ld "
                 "footprints in a horticulture area | left to the building core: small %ld, tall %ld, not a rectangle %ld, on a road %ld | "
                 "marsh cells %ld (%ld from an OSM wetland area), reed clumps %ld | towers %ld, lattice %ld (%ld members) | city lot "
                 "cells left to the shore %ld, lawn pieces %ld, cover cells %ld (TD5RE_ROAM_GREENHOUSE=%d TD5RE_ROAM_OUT_COVER_WET=%d TD5RE_ROAM_MARSH=%d "
                 "TD5RE_ROAM_OUT_LATTICE=%d TD5RE_ROAM_SHORE=%d)",
                 pc_t[2], pc_t[3], pc_m2, pc_t[8], pc_t[0], pc_t[1], pc_t[4], pc_t[5], pc_t[6], pc_t[7], pc_t[9], pc_t[10], pc_t[11],
                 pc_t[15], pc_t[12], pc_t[13], pc_t[14], pc_t[16], pc_t[17], td5_roam_gh_enabled(), td5_env_int("TD5RE_ROAM_OUT_COVER_WET", 1, 0, 1),
                 td5_env_int("TD5RE_ROAM_MARSH", 1, 0, 1), td5_env_int("TD5RE_ROAM_OUT_LATTICE", 1, 0, 1),
                 td5_env_int("TD5RE_ROAM_SHORE", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", ln);
        if (f) fprintf(f, "# %s\n", ln);
        fprintf(stderr, "%s\n", ln);
    }
    {
        /* [roam LB] */
        char ln[700];
        snprintf(ln, sizeof ln, "roam shops [roam LB]: %ld footprint(s) with a business (tags %ld, POIS.JSON %ld), %ld dressed "
                 "(fuel %ld pharmacy %ld bank %ld food %ld kiosk %ld health %ld shop %ld office %ld), %ld street wall(s), %.0f m of "
                 "shopfront | no street wall %ld, skipped landmark/civic/part %ld, too low %ld | awnings %ld (%ld refused: pavement "
                 "narrow / low / road), pharmacy crosses %ld | fuel canopies %ld (%ld with pump islands, %ld over a forecourt service drive; %ld site(s) with no clear spot) | %ld meshes "
                 "(TD5RE_ROAM_SHOPS=%d)", sh_t[0], sh_t[1], sh_t[2], sh_t[3], sh_cat[TD5_GEOB_POI_FUEL],
                 sh_cat[TD5_GEOB_POI_PHARMACY], sh_cat[TD5_GEOB_POI_BANK], sh_cat[TD5_GEOB_POI_FOOD], sh_cat[TD5_GEOB_POI_KIOSK],
                 sh_cat[TD5_GEOB_POI_HEALTH], sh_cat[TD5_GEOB_POI_SHOP], sh_cat[TD5_GEOB_POI_OFFICE], sh_t[4], sh_m, sh_t[5],
                 sh_t[6], sh_t[7], sh_t[8], sh_t[9], sh_t[10], sh_t[11], sh_t[14], sh_t[15], sh_t[12], sh_t[13], td5_roam_shops_enabled());
        TD5_LOG_I(LOG_TAG, "%s", ln);
        if (f) fprintf(f, "# %s\n", ln);
        fprintf(stderr, "%s\n", ln);
    }
    {
        /* [roam PE] */
        char ln[900];
        int aw = 0, ap = 0, acap = 0, along = 0, aab = 0;
        td5_roam_shops_atlas_census(&aw, &ap, &acap, &along, &aab);
        snprintf(ln, sizeof ln, "roam PE: fuel footprints drawn as the canopy %ld (kept as a building %ld), owned sites served by "
                 "their footprint %ld | shopfront walls dropped as covered %ld, trimmed %ld, infill lot tests refused by a shop "
                 "%ld | shop names: %d distinct, %d cells (%d over the cap, %d unreadable, %d abbreviated), signs drawn %ld, "
                 "named fronts with no cell %ld, unnamed fronts %ld | footprints over a cutting band %ld (moved by the line %ld), "
                 "over a cutting zone (+ apron) %ld (moved %ld), line pieces left off a cutting %ld (TD5RE_ROAM_SHOP_CANOPY_FP=%d "
                 "ABUT=%d INFILL_GUARD=%d NAMES=%d MUNI_CUT=%d)",
                 pe_t[5], pe_t[6], pe_t[7], pe_t[8], pe_t[9], pe_t[10], aw, ap, acap, along, aab, pe_t[11], pe_t[12], pe_t[13],
                 pe_t[0], pe_t[2], pe_t[1], pe_t[3], pe_t[4],
                 td5_env_int("TD5RE_ROAM_SHOP_CANOPY_FP", 1, 0, 1), td5_env_int("TD5RE_ROAM_SHOP_ABUT", 1, 0, 1),
                 td5_env_int("TD5RE_ROAM_SHOP_INFILL_GUARD", 1, 0, 1), td5_env_int("TD5RE_ROAM_SHOP_NAMES", 1, 0, 1),
                 td5_env_int("TD5RE_ROAM_MUNI_CUT", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", ln);
        if (f) fprintf(f, "# %s\n", ln);
        fprintf(stderr, "%s\n", ln);
    }
    {
        /* [roam IE] */
        char ln[400];
        snprintf(ln, sizeof ln, "roam structures [roam IE]: guardrail %.0f m, New Jersey %.0f m, deck parapet %.0f m, embankment %.0f m, "
                 "barrier segments dropped on a carriageway %ld, junction kerb edges railed %ld | piers %ld (%ld columns, %ld moved off "
                 "a road below failed), abutments %ld", ie_m[0], ie_m[1], ie_m[2], ie_m[3], ie_n[0], ie_n[1], ie_n[2], ie_n[3], ie_n[4], ie_n[5]);
        TD5_LOG_I(LOG_TAG, "%s", ln);
        if (f) fprintf(f, "# %s\n", ln);
        fprintf(stderr, "%s\n", ln);
        /* [roam LD] */
        snprintf(ln, sizeof ln, "roam plaza [roam LD]: %ld item(s) owned: statues %ld, monuments %ld, memorials %ld, fountains %ld, "
                 "benches %ld, playgrounds %ld, flagpoles %ld (%ld named) | left out by kind %ld, dropped on a road %ld | "
                 "GUARD %ld face(s) on the drivable surface | park-relation plazas drawn %ld",
                 pz_t[0], pz_t[1], pz_t[2], pz_t[3], pz_t[4], pz_t[5], pz_t[6], pz_t[7], pz_t[8], pz_t[9], pz_t[10], pz_t[11], pz_t[12]);
        TD5_LOG_I(LOG_TAG, "%s", ln);
        if (f) fprintf(f, "# %s\n", ln);
        fprintf(stderr, "%s\n", ln);
    }
    {   /* [roam PB] */
        char ln[1600];
        snprintf(ln, sizeof ln, "roam sport [roam PB]: stadiums %d, stands %d (bowls %d, open-stand roofs %d), membrane roofs %d, "
                 "portals %d, wall openings over a drive %d, tier quads %d, seats %d | footprints left to the module %ld, blobs traced over a "
                 "stand %ld, plaza trees not planted on a stadium / pitch %ld, lawns left to a painted pitch %ld | pitches %d: soccer %d tennis %d padel %d basketball %d "
                 "volleyball %d hockey %d rugby %d beach %d surface-only %d | surfaces grass %d clay %d hard %d acrylic %d turf %d "
                 "sand %d | courts %d goals %d nets %d hoops %d line quads %d | left out: small %d, sport %d, untagged %d, on a road "
                 "%d | meshes %d | GUARD %d face(s) on the drivable surface (TD5RE_ROAM_STADIUM=%d TD5RE_ROAM_PITCH_LINES=%d)",
                 sp_t.stadiums, sp_t.stands, sp_t.bowls, sp_t.stand_roofs, sp_t.roofs, sp_t.portals, sp_t.openings, sp_t.tier_quads, sp_t.seats,
                 sp_b[0], sp_b[1], sp_b[2], sp_b[3], sp_t.pitches, sp_t.by_sport[TD5_RPS_SOCCER], sp_t.by_sport[TD5_RPS_TENNIS],
                 sp_t.by_sport[TD5_RPS_PADEL], sp_t.by_sport[TD5_RPS_BASKET], sp_t.by_sport[TD5_RPS_VOLLEY],
                 sp_t.by_sport[TD5_RPS_HOCKEY], sp_t.by_sport[TD5_RPS_RUGBY], sp_t.by_sport[TD5_RPS_BEACH], sp_t.surface_only,
                 sp_t.by_surface[TD5_RPF_GRASS], sp_t.by_surface[TD5_RPF_CLAY], sp_t.by_surface[TD5_RPF_HARD],
                 sp_t.by_surface[TD5_RPF_ACRYLIC], sp_t.by_surface[TD5_RPF_TURF], sp_t.by_surface[TD5_RPF_SAND], sp_t.courts,
                 sp_t.goals, sp_t.nets, sp_t.hoops, sp_t.line_quads, sp_t.skip_small, sp_t.skip_sport, sp_t.skip_none,
                 sp_t.drop_road, sp_t.meshes, sp_t.guard_hits, td5_roam_stadium_enabled(), td5_roam_pitch_enabled());
        TD5_LOG_I(LOG_TAG, "%s", ln);
        if (f) fprintf(f, "# %s\n", ln);
        fprintf(stderr, "%s\n", ln);
    }
#ifndef TD5RE_RELEASE
    if (s_gc_on > 0 && threadmode == 0) sc_gc_report(f);       /* [roam QB] */
#endif
    {   /* [roam QB] the lawn's edge along the road */
        char ln[600];
        snprintf(ln, sizeof ln, "roam lawn edge [roam QB]: lawn along the road %.0f m, gap between the drawn lawn and the road %.1f m2 "
                 "(mean width %.3f m, the drawn ground %.0f cm under the lawn there) | lawn tris %ld, clipped pieces %ld, dropped on the road "
                 "%ld, fallback %ld (TD5RE_ROAM_LAWN_CLIP=%d) | sub-blocks over 256 meshes %ld (most %ld) | lawn left to another plaza lawn %.0f m2, %.0f fallback(s) (TD5RE_ROAM_LAWN_DEDUP=%d)", qb_lm[0], qb_lm[1], qb_lm[0] > 0.0 ? qb_lm[1] / qb_lm[0] : 0.0,
                 (td5_env_int("TD5RE_ROAM_RENDER_GROUND_DROP_CM", 40, 0, 500) * (SC_UPM / 100.0) + SC_GEOP_LIFT) / SC_UPM * 100.0,
                 qb_lc[3], qb_lc[0], qb_lc[1], qb_lc[2], td5_env_int("TD5RE_ROAM_LAWN_CLIP", 1, 0, 1), qb_sb[0], qb_sb[1], qb_dd[0], qb_dd[1],
                 td5_env_int("TD5RE_ROAM_LAWN_DEDUP", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", ln);
        if (f) fprintf(f, "# %s\n", ln);
        fprintf(stderr, "%s\n", ln);
    }
    {   /* [roam QB] stadium base census: wall bottoms against the drawn ground */
        static const char *const nm[TD5_RSB__N] = { "bowl front", "bowl outer", "stand front", "stand back", "stand ends", "portals" };
        char ln[1600];
        int q, o = 0, tn = 0, tov = 0;
        double ts = 0.0, tm = 0.0;
        o += snprintf(ln + o, sizeof ln - (size_t)o, "roam sport base [roam QB]:");
        for (q = 0; q < TD5_RSB__N; q++) {
            o += snprintf(ln + o, sizeof ln - (size_t)o, " %s n %d mean %.2f max %.2f over25 %d |", nm[q], sp_t.foot_n[q],
                          sp_t.foot_n[q] ? sp_t.foot_sum[q] / sp_t.foot_n[q] : 0.0, sp_t.foot_max[q], sp_t.foot_over[q]);
            tn += sp_t.foot_n[q]; tov += sp_t.foot_over[q]; ts += sp_t.foot_sum[q];
            if (sp_t.foot_max[q] > tm) tm = sp_t.foot_max[q];
        }
        snprintf(ln + o, sizeof ln - (size_t)o, " ALL n %d mean %.3f max %.2f over25 %d (m over the drawn ground) | openings %d, %.0f m | facade sectors pulled off a drive %d (mean %.1f m) (TD5RE_ROAM_STADIUM_BASE=%d)",
                 tn, tn ? ts / tn : 0.0, tm, tov, sp_t.open_n, sp_t.open_m, sp_t.pulls, sp_t.pulls ? sp_t.pull_m / sp_t.pulls : 0.0,
                 td5_env_int("TD5RE_ROAM_STADIUM_BASE", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", ln);
        if (f) fprintf(f, "# %s\n", ln);
        fprintf(stderr, "%s\n", ln);
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
        TD5_LOG_I(LOG_TAG, "roam pavement [IA]: %ld kerb vertices made an island by the no-building gap rule "
                  "(TD5RE_ROAM_PV_ISLAND_FILL); island loop tops: %ld triangle(s), %ld loop view(s) refused", pv_fill, pv_tops, pv_topref);
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
                 "roam crossings [LC]: unmarked skipped %ld | unknown painted by the rule: zebra %ld, lines %ld, skipped %ld | "
                 "footway=crossing ways %ld: already a node %ld, drawn %ld | zebras %ld, limit lines %ld in all | "
                 "rule / way paint left off a rail bed %ld (TD5RE_ROAM_XING_RAIL_VETO=%d)",
                 lc_t[0], lc_t[1], lc_t[2], ft_unknown_skipped, lc_t[3], lc_t[4], lc_t[5], ft[9], ft[10], xr_drop,
                 td5_env_int("TD5RE_ROAM_XING_RAIL_VETO", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam rails: %ld tile(s) with track, %ld way piece(s) in %ld meshes (%.1f km): full bed %ld, narrow (road beside) "
                 "%ld, squeezed (tight median) %ld, nudged off a parallel street %ld, raised over a street slab %ld, flush on a road %ld (no data crossing near %ld), blended to a level crossing %ld | bridges %ld piece(s), "
                 "%ld piers, lifted over a road under %ld (max %.1f m), under a road deck %ld, tunnel pieces skipped %ld | "
                 "%ld sleepers (%ld sleeper ends + %ld walls left off a road) | platforms %ld + %ld line piece(s), %ld face(s) kept off the road | RAIL GUARD: %ld face(s) on "
                 "the drivable surface",
                 rl_t[18], rl_t[1], rl_t[16], (double)rl_t[1] * 4.0 / 1000.0, rl_t[2], rl_t[3], rl_t[21], rl_t[22], rl_t[23], rl_t[4], rl_t[5], rl_t[6],
                 rl_t[7], rl_t[8], rl_t[9], rl_lift_max, rl_t[10], rl_t[11], rl_t[12], rl_t[19], rl_t[20], rl_t[13], rl_t[14], rl_t[15], rl_t[17]);
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam lane paint [roam PA]: bus lane %.2f km (%ld spans), bike lane %.2f km (%ld spans), separated track %.2f km (%ld spans, %ld kerb boxes) | "
                 "ways painted %ld (ribbon pieces counted per tile) | spans with no way direction %ld, too narrow %ld, "
                 "at a rail bed %ld [integ P, TD5RE_ROAM_LANE_RAIL_VETO=%d]",
                 pa_m[0] / 1000.0, pa_t[0], pa_m[1] / 1000.0, pa_t[1], pa_m[2] / 1000.0, pa_t[2], pa_t[3], pa_t[4], pa_t[5], pa_t[6],
                 pa_t[7], td5_env_int("TD5RE_ROAM_LANE_RAIL_VETO", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam rail furniture [roam LF]: level-crossing nodes owned %ld (%ld equipped, %ld untagged [roam PA: %ld plain saltire approaches drawn]; no street within 12 m %ld, "
                 "no track %ld, post on a road %ld) | drawn per approach: half barriers %ld, full barriers %ld, light heads %ld, saltires %ld | "
                 "station boards %ld (%ld with the name, %ld blank; %ld on a platform, %ld beside the track, %ld refused) | catenary: %ld way(s), %ld mast(s) "
                 "(%ld dropped on a road), %ld wire piece(s) = %.2f km of contact wire (%ld left off under a road deck)",
                 lf_t[0], lf_t[1], lf_t[2], lf_t[21], lf_t[7], lf_t[8], lf_t[9], lf_t[3], lf_t[4], lf_t[5], lf_t[6], lf_t[10], lf_t[11], lf_t[12],
                 lf_t[13], lf_t[14], lf_t[15], lf_t[16], lf_t[17], lf_t[18], lf_t[19], lf_wire_m / 1000.0, lf_t[20]);
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam cuttings [roam NC] (TD5RE_ROAM_CUT=%d): %ld cutting span(s) in %ld tile(s), %ld wall piece(s) = %.0f m "
                 "(parapet %.0f m, coping apron %.0f m, %ld under a deck, %ld apron(s) cut short by another cut), tallest face %.1f m | "
                 "no wall: %ld piece(s) the ground meets the road (verge strip %.0f m), %ld on a road | %ld kerb piece(s) without a sidewalk | "
                 "CENSUS kerb m casco WALL %.0f VERGE %.0f WALL_VOID %.0f VOID %.0f BANK %.0f SHALLOW %.0f | partido WALL %.0f VERGE %.0f "
                 "WALL_VOID %.0f VOID %.0f BANK %.0f SHALLOW %.0f",
                 td5_roam_cut_on(), ct_i[0], ct_i[7], ct_i[1], ct_d[0], ct_d[1], ct_d[2], ct_i[2], ct_i[3], ct_d[3], ct_i[5], ct_d[4], ct_i[4], ct_i[6],
                 ct_cen[1][0], ct_cen[1][5], ct_cen[1][1], ct_cen[1][2], ct_cen[1][3], ct_cen[1][4],
                 ct_cen[0][0], ct_cen[0][5], ct_cen[0][1], ct_cen[0][2], ct_cen[0][3], ct_cen[0][4]);
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam outskirts [roam LH]: %ld pole(s) (%ld wood, %ld concrete, %ld tower), %ld wire span(s) = %ld m in %ld quads, "
                 "%ld guard-rail segment(s) = %ld m on %ld posts, %ld open gate(s) (%ld lift), %ld footprint(s) skipped on the road, "
                 "%ld portal(s) not drawn, %ld meshes | ground cover: %ld cropland cell(s), %ld wetland cell(s), %ld left to the lawn (road on them) | OUTSKIRTS GUARD: %ld face(s) on the drivable surface",
                 ou_t[0], ou_t[1], ou_t[2], ou_t[3], ou_t[4], ou_t[6], ou_t[5], ou_t[7], ou_t[9], ou_t[8], ou_t[10], ou_t[11],
                 ou_t[12], ou_t[13], ou_t[15], cov_t[0], cov_t[1], cov_t[2], ou_t[14]);
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
                 "taken by a real footprint %.0f m, refused by an area %.0f m, other (road / shape) %.0f m | lots a railway bed reached: %ld moved back behind it, "
                 "%ld shallower, %ld refused (%.0f m)",
                 ic_t[0], ic_t[3], ic_t[1], ic_t[2], ic_m[0], ic_m[1], ic_m[2], ic_m[3], inf_rail_mv, inf_rail_sh,
                 inf_rail_n, inf_rail_m);
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam JC: %ld footprint(s) built as Simple 3D Buildings (building:part / roof:shape), roofs: flat %ld, "
                 "ridge %ld, barrel %ld, centroid (pyramid / cone / dome / onion) %ld, inset deck (mansard / concave) %ld, "
                 "sloped tag built flat %ld | %ld roof-only part(s), %ld tinted surface(s), %ld outline(s) replaced by "
                 "their parts | 3D-tagged footprints drawn %ld, with a sloped roof %ld (TD5RE_ROAM_S3D=%d)",
                 s3d_t[0], s3d_t[4], s3d_t[5], s3d_t[6], s3d_t[7], s3d_t[8], s3d_t[9], s3d_t[2], s3d_t[3], s3d_t[1],
                 s3d_tag, s3d_slope, td5_env_int("TD5RE_ROAM_S3D", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam JC2: relation outlines owned %ld, built %ld | courtyard rings walled %ld, holed caps %ld, lidded "
                 "(clipper gave up) %ld | civic facades %ld | bases under parts %ld | Overture blobs under a relation "
                 "not built %ld | carriage arches %ld", s3d_r[0], s3d_r[1], s3d_r[2], s3d_r[3], s3d_r[4], s3d_r[5], s3d_r[6],
                 s3d_r[7], s3d_r[8]);
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam trees [LA]: street band %ld, lot %ld, plaza %ld (total %ld) | band stations %ld: no canopy >= 3 m "
                 "(2 m search steps) %ld, band too narrow %ld, facade too close %ld, no slab / road / mouth %ld, refused by "
                 "furniture or a rail bed %ld, moved along the kerb %ld, near the tile edge %ld | lot points hidden behind a "
                 "facade %ld, refused (road / slab / building / green / water) %ld | canopy >= 3 m cells %ld, under a crown "
                 "%ld (%.1f %%) | TREES ON ROAD: %ld (TD5RE_ROAM_STREET_TREES=%d TD5RE_ROAM_LOT_TREES=%d)",
                 lt[0], lt[1], lt[2], lt[0] + lt[1] + lt[2], lt[3], lt[4], lt[5], lt[6], lt[7], lt[8], lt[9], lt[10],
                 lt[11], lt[12], lt[14], lt[15], lt[14] ? 100.0 * (double)lt[15] / (double)lt[14] : 0.0, lt[13],
                 td5_env_int("TD5RE_ROAM_STREET_TREES", 1, 0, 1), td5_env_int("TD5RE_ROAM_LOT_TREES", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam rule trees [PD]: avenue %ld, diagonal %ld, median %ld, no-sidewalk kerb facing a median %ld (total %ld) | rule stations %ld: canopy "
                 ">= 3 m (LA's) %ld, sidewalk < 3 m %ld, facade %ld, no slab / road / mouth %ld, fuel forecourt %ld, "
                 "furniture / shop face %ld, tile edge %ld, a tree within %.0f m %ld | kerb km with a trunk within %.0f m: "
                 "avenue %.2f -> %.2f of %.2f, diagonal %.2f -> %.2f of %.2f (LA -> with rule) | TRUNK IN A FOOTPRINT "
                 "planted: street %ld, lot %ld, plaza %ld, rule %ld; moved off one %ld/%ld/%ld/%ld; dropped %ld/%ld/%ld/%ld "
                 "(TD5RE_ROAM_RULE_TREES=%d TD5RE_ROAM_TREE_BLD=%d)",
                 rt_t[0], rt_t[1], rt_t[2], rt_face_n, rt_t[0] + rt_t[1] + rt_t[2] + rt_face_n, rt_t[3], rt_t[4], rt_t[5], rt_t[6], rt_t[7],
                 rt_t[8], rt_t[9], rt_t[10], RT_DOUBLE_M, rt_t[11], RT_COVER_M, rt_m[2] / 1000.0, rt_m[4] / 1000.0,
                 rt_m[0] / 1000.0, rt_m[3] / 1000.0, rt_m[5] / 1000.0, rt_m[1] / 1000.0, tb_t[0], tb_t[1], tb_t[2],
                 tb_t[3], tb_t[4], tb_t[5], tb_t[6], tb_t[7], tb_t[8], tb_t[9], tb_t[10], tb_t[11],
                 td5_env_int("TD5RE_ROAM_RULE_TREES", 1, 0, 1), td5_env_int("TD5RE_ROAM_TREE_BLD", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        if (td5_env_int("TD5RE_ROAM_TREE_XCHECK", 0, 0, 1)) {
            int w;
            for (w = 0; w < 2; w++) {
                snprintf(line, sizeof line,
                         "roam tree xcheck [integ L] %s trees %ld | TRUNK within 0.5 m of: shopfront %ld, fuel canopy %ld, "
                         "plaza furniture %ld, rails %ld, outskirts %ld, pool %ld | CROWN through: awning / shopfront %ld, "
                         "fuel canopy %ld, plaza furniture %ld, rails / catenary %ld, outskirts wire %ld, pool %ld | "
                         "trunk on water %ld", w ? "street+lot" : "plaza", xct[w], xtr[w][0], xtr[w][1], xtr[w][2],
                         xtr[w][3], xtr[w][4], xtr[w][5], xcr[w][0], xcr[w][1], xcr[w][2], xcr[w][3], xcr[w][4], xcr[w][5],
                         xwt[w]);
                TD5_LOG_I(LOG_TAG, "%s", line);
                if (f) fprintf(f, "# %s\n", line);
                fprintf(stderr, "%s\n", line);
                snprintf(line, sizeof line,
                         "roam tree xcheck [integ P] %s trees %ld | TRUNK within 0.5 m of: stadium / pitch %ld, lane paint / "
                         "track kerb %ld, greenhouse %ld | CROWN through: stadium / pitch %ld, lane %ld, greenhouse %ld",
                         w ? "street+lot+rule" : "plaza", xct[w], xtr[w][6], xtr[w][7], xtr[w][8], xcr[w][6], xcr[w][7],
                         xcr[w][8]);
                TD5_LOG_I(LOG_TAG, "%s", line);
                if (f) fprintf(f, "# %s\n", line);
                fprintf(stderr, "%s\n", line);
            }
            snprintf(line, sizeof line, "roam xcheck [integ L] street furniture over the rails layer (track, level-crossing "
                     "furniture, boards, masts): flat paint faces (zebras / limit lines) %ld, standing faces %ld", xfr[0], xfr[1]);
            TD5_LOG_I(LOG_TAG, "%s", line);
            if (f) fprintf(f, "# %s\n", line);
            fprintf(stderr, "%s\n", line);
        }
        snprintf(line, sizeof line,
                 "roam plaza tree pages [integ L]: v0 %ld, v1 %ld, v2 %ld, v3 %ld, v4 (snow conifer) %ld, v5 %ld, v6 %ld, v7 %ld, "
                 "v8 %ld, v9 %ld (TD5RE_ROAM_PLAZA_TREE_PAGES=%d) | round L tree veto: plaza trees dropped on a rail / "
                 "outskirts / plaza-furniture / fuel / street-furniture face %ld, on water %ld (TD5RE_ROAM_TREE_VETO_L=%d)",
                 ptv[0], ptv[1], ptv[2], ptv[3], ptv[4], ptv[5], ptv[6], ptv[7], ptv[8], ptv[9],
                 td5_env_int("TD5RE_ROAM_PLAZA_TREE_PAGES", 1, 0, 1), pt_drop[0], pt_drop[1],
                 td5_env_int("TD5RE_ROAM_TREE_VETO_L", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        snprintf(line, sizeof line,
                 "roam canopy green [LA]: %ld city lot cell(s) left lawn (CANOPY >= 3 m on 12 of 16 points, no OSM area, no "
                 "footprint), %ld infill lot(s) refused on such ground (%.0f m of frontage) (TD5RE_ROAM_CANOPY_GREEN=%d)",
                 cg_n[0], cg_n[1], cg_m, td5_env_int("TD5RE_ROAM_CANOPY_GREEN", 1, 0, 1));
        TD5_LOG_I(LOG_TAG, "%s", line);
        if (f) fprintf(f, "# %s\n", line);
        fprintf(stderr, "%s\n", line);
        if (f) fclose(f);
    }
    free(j.list); free(j.rows); free(j.ok);
    return 1;
}

#endif /* !TD5RE_RELEASE */
