/**
 * td5_roam_shops.c -- GEO FREE ROAM: shopfronts and the ground-floor identity of a footprint
 * (PORT-ONLY, roam LB). See td5_roam_shops.h.
 *
 * Geometry is the tile frame (tile-local x/z, place y), the frame td5_roam_scenery.c hands the
 * building core. One mesh per dressed footprint (glass page + white page commands) and one per fuel
 * canopy, written with tg_write_quad_mesh_col into the footprint's sub-block, BEFORE the furniture
 * tail, so the F4a footprint census judges them like the building they stand on (GUARD must stay 0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "td5_trackgen_internal.h"   /* TG_Buf, tg_write_quad_mesh_col, TD5_TG_PAGE_* */
#include "td5_geo.h"                 /* td5_geo_source_path                          */
#include "td5_geo_roads.h"           /* td5_geo_roads_slurp                          */
#include "td5_geo_frame.h"           /* [roam M] the source -> game reflection       */
#include "td5_config.h"
#include "td5_platform.h"
#include "deps/cjson/cJSON.h"
#include "td5_roam_scenery.h"
#include "td5_roam_shops.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "roam"

#define SH_UPM          430.0
#define SH_M(m)         ((m) * SH_UPM)
#define SH_SINK         300.0              /* TD5_TG_GEO_BASE_SINK (td5_tg_city.c)    */
#define SH_MAX_FOUND    4300.0             /* TD5_TG_GEO_MAX_FOUND                    */
#define SH_MIN_H        SH_M(2.6)          /* lower than this is a shed, not a shop   */
#define SH_GLASS_H      SH_M(3.0)          /* shop head height over the base          */
#define SH_FASCIA_H     SH_M(0.55)
#define SH_OFF_GLASS    SH_M(0.06)         /* proud of the facade                     */
#define SH_OFF_FASCIA   SH_M(0.12)
#define SH_EDGE_MIN     SH_M(2.5)          /* shorter walls are corners / jogs        */
#define SH_STREET_M     SH_M(14.0)         /* wall to carriageway edge, at most       */
#define SH_FACING       0.6                /* cos of the wall normal to the street    */
#define SH_BIG_M2       600.0              /* bigger footprints: one front only       */
#define SH_MAX_FRONTS   3
#define SH_BAY          SH_M(4.5)          /* one store page cell                     */
#define SH_AWN_D        SH_M(1.3)
#define SH_AWN_MIN      SH_M(0.7)
#define SH_AWN_DROP     SH_M(0.35)
#define SH_AWN_CLEAR    SH_M(2.2)          /* lowest awning edge over the pavement    */
#define SH_AWN_VAL      SH_M(0.20)         /* valance                                 */
#define SH_PAGE_WHITE   TD5_TG_PAGE_SNOW   /* near-white page every colour tints      */
#define SH_QMAX         64

#define SH_CAN_L     SH_M(16.0)
#define SH_CAN_W     SH_M(9.0)
#define SH_CAN_CLEAR SH_M(4.8)
#define SH_CAN_T     SH_M(0.8)
#define SH_CAN_MAX   32                    /* planned canopies per tile */

typedef struct {
    double cx, cz, tx, tz, hl, hw, gy;
    unsigned int col;
    int islands;                           /* the pump islands are clear too */
    int posts;                             /* bit k: post k stands on clear ground (drawn) */
    int sb;
} ShCanopy;

struct TD5_RoamShopSource {
    char   place[64];
    int    loaded, use_pois, awnings, canopy, log;
    /* POIS.JSON buildings[], sorted by id */
    int    nb;
    double *bid;
    unsigned char *bcat, *bpt;
    double *bx, *bz;
    /* fuel sites (game frame, place coords) */
    int    nf;
    double *fx, *fz;
    unsigned int *fcol;
    int    *fring0, *fringn;               /* into rx / rz */
    double *rx, *rz;
    int    nr;
    /* the tile being built: canopies planned in the building pass, written in the furniture tail */
    ShCanopy pend[SH_CAN_MAX];
    int    npend;
};

int td5_roam_shops_enabled(void) { return td5_env_int("TD5RE_ROAM_SHOPS", 1, 0, 1); }

TD5_RoamShopSource *td5_roam_shops_open(const char *place)
{
    TD5_RoamShopSource *s;
    if (!place || !td5_roam_shops_enabled()) return NULL;
    s = (TD5_RoamShopSource *)calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->place, sizeof s->place, "%s", place);
    s->use_pois = td5_env_int("TD5RE_ROAM_SHOP_POIS", 1, 0, 1);
    s->awnings  = td5_env_int("TD5RE_ROAM_SHOP_AWNINGS", 1, 0, 1);
    s->canopy   = td5_env_int("TD5RE_ROAM_SHOP_CANOPY", 1, 0, 1);
    s->log      = td5_env_int("TD5RE_ROAM_SHOP_LOG", 0, 0, 1);
    return s;
}

void td5_roam_shops_close(TD5_RoamShopSource *s)
{
    if (!s) return;
    free(s->bid); free(s->bcat); free(s->bpt); free(s->bx); free(s->bz);
    free(s->fx); free(s->fz); free(s->fcol); free(s->fring0); free(s->fringn); free(s->rx); free(s->rz);
    free(s);
}

void td5_roam_shops_summary(const TD5_RoamShopSource *s, int *nbld, int *nfuel, int *loaded)
{
    if (nbld) *nbld = s ? s->nb : 0;
    if (nfuel) *nfuel = s ? s->nf : 0;
    if (loaded) *loaded = s ? s->loaded : 0;
}

/* ---------------------------------------------------------------- POIS.JSON */

static int sh_ci_has(const char *hay, const char *needle)
{
    size_t n = strlen(needle), i, k;
    if (!hay) return 0;
    for (i = 0; hay[i]; i++) {
        for (k = 0; k < n && hay[i + k] && tolower((unsigned char)hay[i + k]) == needle[k]; k++) { }
        if (k == n) return 1;
    }
    return 0;
}

/* the brand's canopy fascia colour (0xFFRRGGBB); unbranded = a neutral blue */
static unsigned int sh_brand_col(const char *brand, const char *name)
{
    static const struct { const char *k; unsigned int col; } t[] = {
        { "ypf", 0xFF0A4DA2u }, { "shell", 0xFFF2C200u }, { "axion", 0xFF7A2A8Au }, { "puma", 0xFFC8102Eu },
        { "gulf", 0xFFF07800u }, { "petrobras", 0xFF00873Eu }, { "esso", 0xFFC8102Eu } };
    size_t i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (sh_ci_has(brand, t[i].k) || sh_ci_has(name, t[i].k)) return t[i].col;
    return 0xFF1A5FB4u;
}

static double sh_num(const cJSON *o, const char *k, double d)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : d;
}

static const char *sh_str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void sh_sort_b(TD5_RoamShopSource *s)
{
    int i, k;
    for (i = 1; i < s->nb; i++) {          /* insertion sort: the file is already sorted by id */
        const double id = s->bid[i], x = s->bx[i], z = s->bz[i];
        const unsigned char c = s->bcat[i], p = s->bpt[i];
        for (k = i - 1; k >= 0 && s->bid[k] > id; k--) {
            s->bid[k + 1] = s->bid[k]; s->bcat[k + 1] = s->bcat[k]; s->bpt[k + 1] = s->bpt[k];
            s->bx[k + 1] = s->bx[k]; s->bz[k + 1] = s->bz[k];
        }
        s->bid[k + 1] = id; s->bcat[k + 1] = c; s->bpt[k + 1] = p; s->bx[k + 1] = x; s->bz[k + 1] = z;
    }
}

/* Lazy: on the building thread, under its FP env (strtod of the coordinates). */
static void sh_load(TD5_RoamShopSource *s)
{
    char path[512];
    char *json;
    cJSON *root;
    const cJSON *arr, *e;
    int n, i;
    if (s->loaded) return;
    s->loaded = 1;
    if (!s->use_pois) return;
    td5_geo_source_path(path, sizeof path, s->place, "POIS.JSON");
    json = td5_geo_roads_slurp(path);
    if (!json) {
        TD5_LOG_I(LOG_TAG, "roam shops: %s has no POIS.JSON (re/tools/geo_pois.py): footprint tags only", s->place);
        return;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) { TD5_LOG_W(LOG_TAG, "roam shops: POIS.JSON is not valid JSON"); return; }
    td5_geo_src_json_to_game(root);           /* every {x, z} -> game frame (x = -east) */
    arr = cJSON_GetObjectItem(root, "buildings");
    n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    if (n > 0) {
        s->bid = (double *)malloc((size_t)n * sizeof(double));
        s->bx = (double *)malloc((size_t)n * sizeof(double));
        s->bz = (double *)malloc((size_t)n * sizeof(double));
        s->bcat = (unsigned char *)malloc((size_t)n);
        s->bpt = (unsigned char *)malloc((size_t)n);
        if (!s->bid || !s->bx || !s->bz || !s->bcat || !s->bpt) n = 0;
    }
    i = 0;
    for (e = n > 0 ? arr->child : NULL; e; e = e->next) {   /* linked walk: GetArrayItem(i) is O(n^2) */
        const int cat = td5_geob_poi_cat_of_name(sh_str(e, "cat"));
        if (cat == TD5_GEOB_POI_NONE || i >= n) continue;
        s->bid[i] = sh_num(e, "id", 0.0);
        s->bcat[i] = (unsigned char)cat;
        s->bpt[i] = (unsigned char)(cJSON_IsNumber(cJSON_GetObjectItem(e, "x")) ? 1 : 0);
        s->bx[i] = sh_num(e, "x", 0.0);
        s->bz[i] = sh_num(e, "z", 0.0);
        i++;
    }
    s->nb = i;
    sh_sort_b(s);
    arr = cJSON_GetObjectItem(root, "fuel");
    n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    if (n > 0) {
        int nrp = 0;
        for (e = arr->child; e; e = e->next) {
            const cJSON *r = cJSON_GetObjectItem(e, "ring");
            if (cJSON_IsArray(r)) nrp += cJSON_GetArraySize(r);
        }
        s->fx = (double *)malloc((size_t)n * sizeof(double));
        s->fz = (double *)malloc((size_t)n * sizeof(double));
        s->fcol = (unsigned int *)malloc((size_t)n * sizeof(unsigned int));
        s->fring0 = (int *)malloc((size_t)n * sizeof(int));
        s->fringn = (int *)malloc((size_t)n * sizeof(int));
        s->rx = (double *)malloc((size_t)(nrp + 1) * sizeof(double));
        s->rz = (double *)malloc((size_t)(nrp + 1) * sizeof(double));
        if (s->fx && s->fz && s->fcol && s->fring0 && s->fringn && s->rx && s->rz) {
            i = 0;
            for (e = arr->child; e; e = e->next, i++) {
                const cJSON *r = cJSON_GetObjectItem(e, "ring"), *p;
                s->fx[i] = sh_num(e, "x", 0.0);
                s->fz[i] = sh_num(e, "z", 0.0);
                s->fcol[i] = sh_brand_col(sh_str(e, "brand"), sh_str(e, "name"));
                s->fring0[i] = s->nr;
                s->fringn[i] = 0;
                if (cJSON_IsArray(r))
                    for (p = r->child; p; p = p->next) {
                        s->rx[s->nr] = sh_num(p, "x", 0.0);
                        s->rz[s->nr] = sh_num(p, "z", 0.0);
                        s->nr++;
                        s->fringn[i]++;
                    }
            }
            s->nf = i;
        }
    }
    cJSON_Delete(root);
    TD5_LOG_I(LOG_TAG, "roam shops: POIS.JSON loaded, %d footprint(s) with a business, %d fuel site(s)", s->nb, s->nf);
}

static int sh_find(const TD5_RoamShopSource *s, double id)
{
    int lo = 0, hi = s->nb - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (s->bid[m] < id) lo = m + 1;
        else if (s->bid[m] > id) hi = m - 1;
        else return m;
    }
    return -1;
}

int td5_roam_shops_category(TD5_RoamShopSource *s, double id, const TD5_GeoBuilding *gb, int *cat, int *from_poi)
{
    int c = gb ? gb->poi_cat : TD5_GEOB_POI_NONE, k;
    *from_poi = 0;
    if (!s) { *cat = TD5_GEOB_POI_NONE; return 0; }
    sh_load(s);
    k = sh_find(s, id);
    /* the more specific of the two (the TD5_GEOB_POI_* order is the priority) */
    if (k >= 0 && (c == TD5_GEOB_POI_NONE || s->bcat[k] < c)) { c = s->bcat[k]; *from_poi = 1; }
    *cat = c;
    return c != TD5_GEOB_POI_NONE;
}

/* ---------------------------------------------------------------- quads */

typedef struct {
    double x[SH_QMAX * 4], y[SH_QMAX * 4], z[SH_QMAX * 4], u[SH_QMAX * 4], v[SH_QMAX * 4];
    unsigned int col[SH_QMAX * 4];
    int page[SH_QMAX];
    int nq;
} ShQB;

/* corners p0..p3 (x, y, z each), uv per corner, one colour */
static void sh_quad(ShQB *q, int page, const double p[4][3], const double uv[4][2], unsigned int col)
{
    int k;
    if (q->nq >= SH_QMAX) return;
    for (k = 0; k < 4; k++) {
        const int o = q->nq * 4 + k;
        q->x[o] = p[k][0]; q->y[o] = p[k][1]; q->z[o] = p[k][2];
        q->u[o] = uv[k][0]; q->v[o] = uv[k][1];
        q->col[o] = col;
    }
    q->page[q->nq++] = page;
}

/* a plain coloured quad on the white page */
static void sh_flat(ShQB *q, const double p[4][3], unsigned int col)
{
    static const double uv[4][2] = { { 0.1, 0.9 }, { 0.9, 0.9 }, { 0.9, 0.1 }, { 0.1, 0.1 } };
    sh_quad(q, SH_PAGE_WHITE, p, uv, col);
}

/* an axis box (centre, half extents along the unit tangent t and its normal) on the white page:
 * top + 4 sides (+ bottom when `under`) */
static void sh_box(ShQB *q, double cx, double cz, double y0, double y1, double tx, double tz, double hl, double hw,
                   unsigned int side_col, unsigned int top_col, int under)
{
    const double nx = -tz, nz = tx;
    double c[4][2], p[4][3];
    int k;
    c[0][0] = cx - tx * hl - nx * hw; c[0][1] = cz - tz * hl - nz * hw;
    c[1][0] = cx + tx * hl - nx * hw; c[1][1] = cz + tz * hl - nz * hw;
    c[2][0] = cx + tx * hl + nx * hw; c[2][1] = cz + tz * hl + nz * hw;
    c[3][0] = cx - tx * hl + nx * hw; c[3][1] = cz - tz * hl + nz * hw;
    for (k = 0; k < 4; k++) { p[k][0] = c[k][0]; p[k][1] = y1; p[k][2] = c[k][1]; }
    sh_flat(q, p, top_col);
    if (under) {
        for (k = 0; k < 4; k++) { p[k][0] = c[3 - k][0]; p[k][1] = y0; p[k][2] = c[3 - k][1]; }
        sh_flat(q, p, top_col);
    }
    for (k = 0; k < 4; k++) {
        const int j = (k + 1) % 4;
        p[0][0] = c[k][0]; p[0][1] = y0; p[0][2] = c[k][1];
        p[1][0] = c[j][0]; p[1][1] = y0; p[1][2] = c[j][1];
        p[2][0] = c[j][0]; p[2][1] = y1; p[2][2] = c[j][1];
        p[3][0] = c[k][0]; p[3][1] = y1; p[3][2] = c[k][1];
        sh_flat(q, p, side_col);
    }
}

/* write the batch as ONE mesh in sub-block sb, quads grouped by page */
static int sh_flush(TG_TileCtx *c, int sb, ShQB *q, TD5_RoamShopStats *st)
{
    double *px;
    unsigned int *pc;
    int pages[4], np = 0, seg_page[4], seg_nq[4], nseg = 0, i, k, n = 0, ok;
    TG_Buf *b;
    if (q->nq <= 0) return 1;
    px = (double *)malloc((size_t)q->nq * 4u * 5u * sizeof(double));
    pc = (unsigned int *)malloc((size_t)q->nq * 4u * sizeof(unsigned int));
    if (!px || !pc) { free(px); free(pc); return 0; }
    for (i = 0; i < q->nq; i++) {
        for (k = 0; k < np; k++) if (pages[k] == q->page[i]) break;
        if (k == np && np < 4) pages[np++] = q->page[i];
    }
    for (k = 0; k < np; k++) {
        int cnt = 0;
        for (i = 0; i < q->nq; i++) {
            int v;
            if (q->page[i] != pages[k]) continue;
            for (v = 0; v < 4; v++) {
                const int o = i * 4 + v;
                px[n] = q->x[o];
                px[q->nq * 4 + n] = q->y[o];
                px[q->nq * 8 + n] = q->z[o];
                px[q->nq * 12 + n] = q->u[o];
                px[q->nq * 16 + n] = q->v[o];
                pc[n] = q->col[o];
                n++;
            }
            cnt++;
        }
        seg_page[nseg] = pages[k]; seg_nq[nseg] = cnt; nseg++;
    }
    b = (TG_Buf *)roam_ctx_mesh_begin(c, sb);
    ok = b && tg_write_quad_mesh_col(b, px, px + q->nq * 4, px + q->nq * 8, px + q->nq * 12, px + q->nq * 16, pc, n,
                                     seg_page, seg_nq, nseg) && !b->oom;
    free(px); free(pc);
    q->nq = 0;
    if (ok) st->meshes++;
    return ok;
}

/* ---------------------------------------------------------------- shopfronts */

static unsigned int sh_hash(double id)
{
    long long v = (long long)id;
    unsigned int h = (unsigned int)(v ^ (v >> 32)) * 2654435761u;
    return h ^ (h >> 15);
}

static unsigned int sh_fascia_col(int cat, unsigned int h)
{
    static const unsigned int shop[6] = { 0xFF7A2E2Eu, 0xFF3E5A2Au, 0xFF34495Eu, 0xFFB07A2Au, 0xFF6A3F8Au, 0xFF8A5A3Au };
    switch (cat) {
    case TD5_GEOB_POI_FUEL:     return 0xFFE8E8E8u;
    case TD5_GEOB_POI_PHARMACY: return 0xFF1E8C3Cu;
    case TD5_GEOB_POI_BANK:     return 0xFF1F3F8Fu;
    case TD5_GEOB_POI_FOOD:     return 0xFF8E1F1Fu;
    case TD5_GEOB_POI_KIOSK:    return 0xFFD8A020u;
    case TD5_GEOB_POI_HEALTH:   return 0xFF2A8C8Cu;
    case TD5_GEOB_POI_OFFICE:   return 0xFF505860u;
    default:                    return shop[(h >> 7) % 6u];
    }
}

static unsigned int sh_awning_col(int cat, unsigned int h, unsigned int fascia)
{
    static const unsigned int food[3] = { 0xFFB02A2Au, 0xFF2A6A3Au, 0xFF8A2A5Au };
    if (cat == TD5_GEOB_POI_FOOD) return food[(h >> 11) % 3u];
    {   /* the fascia colour, a third of the way to white */
        unsigned int r = (fascia >> 16) & 255u, g = (fascia >> 8) & 255u, b = fascia & 255u;
        r += (255u - r) / 3u; g += (255u - g) / 3u; b += (255u - b) / 3u;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

/* the glazed page: the shipped Sydney storefronts for retail, the San Francisco ones for the formal
 * fronts (bank, office, clinic). Never the Tokyo page (TD5_TG_PAGE_STORE + 5: Japanese signage). */
static int sh_store_page(int cat, unsigned int h)
{
    if (cat == TD5_GEOB_POI_BANK || cat == TD5_GEOB_POI_OFFICE || cat == TD5_GEOB_POI_HEALTH)
        return TD5_TG_PAGE_STORE + 3 + (int)((h >> 3) % 2u);
    return TD5_TG_PAGE_STORE + (int)((h >> 3) % 3u);
}

static int sh_wants_awning(int cat, unsigned int h)
{
    if (cat == TD5_GEOB_POI_FOOD) return 1;
    if (cat == TD5_GEOB_POI_KIOSK || cat == TD5_GEOB_POI_SHOP) return ((h >> 19) & 1u) != 0;
    return 0;
}

typedef struct {
    int    k;                   /* ring edge k -> k+1 */
    double len, kerb_d;         /* units */
    double nx, nz;              /* outward unit normal */
} ShFront;

/* Is ring edge k a street wall? Probe just outside its midpoint: the nearest street within reach,
 * the wall facing it, nothing built between them. */
static int sh_street_wall(TG_TileCtx *c, const TD5_RoamShopBld *b, int k, double sgn, ShFront *out)
{
    const int j = (k + 1) % b->n;
    const double ax = b->rx[k], az = b->rz[k], bx = b->rx[j], bz = b->rz[j];
    const double dx = bx - ax, dz = bz - az, len = hypot(dx, dz);
    double nx, nz, mx, mz, qx, qz, along, lat, wx, wz, wd, kd;
    int edge, side, t;
    RoamFrame f;
    if (!(len >= SH_EDGE_MIN)) return 0;
    nx = sgn * dz / len; nz = -sgn * dx / len;         /* outward for the ring's winding */
    mx = 0.5 * (ax + bx); mz = 0.5 * (az + bz);
    qx = mx + nx * SH_M(0.3); qz = mz + nz * SH_M(0.3);
    if (!roam_nearest_edge(c, qx, qz, SH_STREET_M + SH_M(12.0), &edge, &side, &along, &lat)) return 0;
    if (!roam_edge_frame(c, edge, side, along, &f)) return 0;
    kd = fabs(lat) - f.half_carriage;
    if (kd > SH_STREET_M) return 0;
    wx = f.px - mx; wz = f.pz - mz; wd = hypot(wx, wz);
    if (!(wd > 1.0) || (nx * wx + nz * wz) / wd < SH_FACING) return 0;
    if (b->in_bld)                                      /* another building between the wall and the street */
        for (t = 1; (double)t * SH_M(2.0) < kd; t++)
            if (b->in_bld(b->u, qx + nx * (double)t * SH_M(2.0), qz + nz * (double)t * SH_M(2.0))) return 0;
    out->k = k; out->len = len; out->kerb_d = kd < 0.0 ? 0.0 : kd; out->nx = nx; out->nz = nz;
    return 1;
}

/* the foot of the wall at a ring vertex, as the core lays its foundation */
static double sh_foot(TG_TileCtx *c, const TD5_RoamShopBld *b, double x, double z)
{
    const double g = roam_ctx_ground(c, x, z) + b->kerb_h - SH_SINK;
    double y = b->by - SH_SINK;
    if (g < y) y = (g < b->by - SH_MAX_FOUND) ? b->by - SH_MAX_FOUND : g;
    return y;
}

static void sh_dress_front(TD5_RoamShopSource *s, TG_TileCtx *c, const TD5_RoamShopBld *b, const ShFront *fr, int cat,
                           unsigned int h, double glass_top, double fascia_top, int cross, ShQB *q, TD5_RoamShopStats *st)
{
    const int k = fr->k, j = (k + 1) % b->n;
    const double ax = b->rx[k], az = b->rz[k], bx = b->rx[j], bz = b->rz[j];
    const double tx = (bx - ax) / fr->len, tz = (bz - az) / fr->len;
    const unsigned int fcol = sh_fascia_col(cat, h);
    const double bays = floor(fr->len / SH_BAY + 0.5) < 1.0 ? 1.0 : floor(fr->len / SH_BAY + 0.5);
    double p[4][3], uv[4][2];
    double o;

    /* glass, from the foot of the wall to the shop's head */
    o = SH_OFF_GLASS;
    p[0][0] = ax + fr->nx * o; p[0][1] = sh_foot(c, b, ax, az); p[0][2] = az + fr->nz * o;
    p[1][0] = bx + fr->nx * o; p[1][1] = sh_foot(c, b, bx, bz); p[1][2] = bz + fr->nz * o;
    p[2][0] = p[1][0];          p[2][1] = glass_top;             p[2][2] = p[1][2];
    p[3][0] = p[0][0];          p[3][1] = glass_top;             p[3][2] = p[0][2];
    uv[0][0] = 0.0;  uv[0][1] = 1.0; uv[1][0] = bays; uv[1][1] = 1.0;
    uv[2][0] = bays; uv[2][1] = 0.0; uv[3][0] = 0.0;  uv[3][1] = 0.0;
    sh_quad(q, sh_store_page(cat, h), p, uv, 0xFFFFFFFFu);
    /* fascia band */
    o = SH_OFF_FASCIA;
    p[0][0] = ax + fr->nx * o; p[0][1] = glass_top;  p[0][2] = az + fr->nz * o;
    p[1][0] = bx + fr->nx * o; p[1][1] = glass_top;  p[1][2] = bz + fr->nz * o;
    p[2][0] = p[1][0];          p[2][1] = fascia_top; p[2][2] = p[1][2];
    p[3][0] = p[0][0];          p[3][1] = fascia_top; p[3][2] = p[0][2];
    sh_flat(q, p, fcol);
    st->fronts++;
    st->front_m += fr->len / SH_UPM;

    /* awning: over the pavement, never out to the carriageway, never low over a head */
    if (s->awnings && sh_wants_awning(cat, h)) {
        double d = SH_AWN_D;
        const double inset = fr->len > SH_M(1.5) ? SH_M(0.3) : 0.0;
        const double a0x = ax + tx * inset, a0z = az + tz * inset, b0x = bx - tx * inset, b0z = bz - tz * inset;
        const double y_in = glass_top + SH_M(0.05), y_out = glass_top - SH_AWN_DROP;
        if (fr->kerb_d - SH_M(0.5) < d) d = fr->kerb_d - SH_M(0.5);
        if (d < SH_AWN_MIN || y_out - SH_AWN_VAL < b->by + SH_AWN_CLEAR) {
            st->awning_tight++;
        } else {
            double ox[4], oz[4];
            const unsigned int acol = sh_awning_col(cat, h, fcol);
            ox[0] = a0x + fr->nx * SH_OFF_FASCIA; oz[0] = a0z + fr->nz * SH_OFF_FASCIA;
            ox[1] = b0x + fr->nx * SH_OFF_FASCIA; oz[1] = b0z + fr->nz * SH_OFF_FASCIA;
            ox[2] = b0x + fr->nx * d; oz[2] = b0z + fr->nz * d;
            ox[3] = a0x + fr->nx * d; oz[3] = a0z + fr->nz * d;
            if (roam_ctx_on_road(c, ox, oz, 4)) {
                st->awning_tight++;
            } else {
                p[0][0] = ox[0]; p[0][1] = y_in;  p[0][2] = oz[0];
                p[1][0] = ox[1]; p[1][1] = y_in;  p[1][2] = oz[1];
                p[2][0] = ox[2]; p[2][1] = y_out; p[2][2] = oz[2];
                p[3][0] = ox[3]; p[3][1] = y_out; p[3][2] = oz[3];
                sh_flat(q, p, acol);
                p[0][0] = ox[3]; p[0][1] = y_out - SH_AWN_VAL; p[0][2] = oz[3];
                p[1][0] = ox[2]; p[1][1] = y_out - SH_AWN_VAL; p[1][2] = oz[2];
                p[2][0] = ox[2]; p[2][1] = y_out;              p[2][2] = oz[2];
                p[3][0] = ox[3]; p[3][1] = y_out;              p[3][2] = oz[3];
                sh_flat(q, p, acol);
                st->awnings++;
            }
        }
    }
    /* the pharmacy's green cross: a blade sign out of the wall near the front's first end */
    if (cross) {
        const double at = fr->len * 0.2 < SH_M(1.0) ? fr->len * 0.2 : SH_M(1.0);
        const double cx = ax + tx * at, cz = az + tz * at;
        const double yc = 0.5 * (glass_top + fascia_top) + SH_M(0.15);
        const double o0 = SH_M(0.25), o1 = SH_M(0.95), om = 0.5 * (o0 + o1), arm = 0.5 * (o1 - o0), bar = SH_M(0.12);
        double ox[2], oz[2];
        ox[0] = cx + fr->nx * o0; oz[0] = cz + fr->nz * o0;
        ox[1] = cx + fr->nx * o1; oz[1] = cz + fr->nz * o1;
        if (!roam_ctx_on_road(c, ox, oz, 2)) {
            const unsigned int g = 0xFF19B24Au;
            /* vertical bar */
            p[0][0] = cx + fr->nx * (om - bar); p[0][1] = yc - arm; p[0][2] = cz + fr->nz * (om - bar);
            p[1][0] = cx + fr->nx * (om + bar); p[1][1] = yc - arm; p[1][2] = cz + fr->nz * (om + bar);
            p[2][0] = p[1][0];                  p[2][1] = yc + arm; p[2][2] = p[1][2];
            p[3][0] = p[0][0];                  p[3][1] = yc + arm; p[3][2] = p[0][2];
            sh_flat(q, p, g);
            /* horizontal bar */
            p[0][0] = ox[0]; p[0][1] = yc - bar; p[0][2] = oz[0];
            p[1][0] = ox[1]; p[1][1] = yc - bar; p[1][2] = oz[1];
            p[2][0] = ox[1]; p[2][1] = yc + bar; p[2][2] = oz[1];
            p[3][0] = ox[0]; p[3][1] = yc + bar; p[3][2] = oz[0];
            sh_flat(q, p, g);
            /* bracket back to the wall */
            p[0][0] = cx + fr->nx * SH_OFF_FASCIA; p[0][1] = yc + arm - SH_M(0.05); p[0][2] = cz + fr->nz * SH_OFF_FASCIA;
            p[1][0] = cx + fr->nx * o0;            p[1][1] = yc + arm - SH_M(0.05); p[1][2] = cz + fr->nz * o0;
            p[2][0] = p[1][0];                     p[2][1] = yc + arm + SH_M(0.03); p[2][2] = p[1][2];
            p[3][0] = p[0][0];                     p[3][1] = yc + arm + SH_M(0.03); p[3][2] = p[0][2];
            sh_flat(q, p, 0xFF606060u);
            st->crosses++;
        }
    }
}

int td5_roam_shops_building(TD5_RoamShopSource *s, TG_TileCtx *c, const TD5_RoamShopBld *b, TD5_RoamShopStats *st)
{
    const TD5_GeoBuilding *gb = b->gb;
    ShFront fr[TD5_GEOB_RING_MAX], pick[SH_MAX_FRONTS];
    int cat, from_poi, k, nfr = 0, npick = 0, bk;
    double area2 = 0.0, H, glass_top, fascia_top, px = 0.0, pz = 0.0;
    int has_pt = 0;
    unsigned int h;
    ShQB *q;
    if (!s || !td5_roam_shops_category(s, b->id, gb, &cat, &from_poi)) return 1;
    st->cand++;
    if (from_poi) st->from_poi++; else st->from_tag++;
    if (gb->landmark || gb->civic || gb->part || gb->rel || gb->roof_only || gb->min_height > 0.0
        || gb->lmkind != TD5_GEOB_LMK_NONE) { st->skip_kind++; return 1; }
    H = gb->height - gb->min_height;
    if (H < SH_MIN_H) { st->skip_low++; return 1; }
    glass_top = b->by + (SH_GLASS_H < H - SH_M(0.75) ? SH_GLASS_H : H - SH_M(0.75));
    fascia_top = glass_top + SH_FASCIA_H;
    if (fascia_top > b->by + H - SH_M(0.1)) fascia_top = b->by + H - SH_M(0.1);
    if (fascia_top - glass_top < SH_M(0.25)) { st->skip_low++; return 1; }
    for (k = 0; k < b->n; k++) {
        const int j = (k + 1) % b->n;
        area2 += b->rx[k] * b->rz[j] - b->rx[j] * b->rz[k];
    }
    for (k = 0; k < b->n; k++)
        if (sh_street_wall(c, b, k, area2 > 0.0 ? 1.0 : -1.0, &fr[nfr])) nfr++;
    if (nfr == 0) { st->no_front++; return 1; }
    if (from_poi) {
        const int i = sh_find(s, b->id);
        if (i >= 0 && s->bpt[i]) {
            int32_t ox, oz;
            int tx, tz;
            roam_ctx_origin(c, &tx, &tz, &ox, &oz);
            px = s->bx[i] - (double)ox; pz = s->bz[i] - (double)oz;
            has_pt = 1;
        }
    }
    /* which street walls: a big footprint gets ONE front (the one nearest the POI, else the longest),
     * a lot-sized one its street walls, longest first, at most SH_MAX_FRONTS */
    if (has_pt) {
        double best = 1e300;
        bk = 0;
        for (k = 0; k < nfr; k++) {
            const int a = fr[k].k, j = (a + 1) % b->n;
            const double dx = b->rx[j] - b->rx[a], dz = b->rz[j] - b->rz[a];
            double t = ((px - b->rx[a]) * dx + (pz - b->rz[a]) * dz) / (fr[k].len * fr[k].len), d;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            d = hypot(b->rx[a] + t * dx - px, b->rz[a] + t * dz - pz);
            if (d < best) { best = d; bk = k; }
        }
        pick[npick++] = fr[bk];
        fr[bk].len = -1.0;
    }
    if (!(npick > 0 && gb->area_m2 > SH_BIG_M2)) {
        while (npick < SH_MAX_FRONTS) {
            double best = 0.0;
            bk = -1;
            for (k = 0; k < nfr; k++) if (fr[k].len > best) { best = fr[k].len; bk = k; }
            if (bk < 0) break;
            pick[npick++] = fr[bk];
            fr[bk].len = -1.0;
            if (gb->area_m2 > SH_BIG_M2) break;
        }
    }
    h = sh_hash(b->id);
    q = (ShQB *)malloc(sizeof *q);
    if (!q) return 0;
    q->nq = 0;
    for (k = 0; k < npick; k++)
        sh_dress_front(s, c, b, &pick[k], cat, h, glass_top, fascia_top,
                       cat == TD5_GEOB_POI_PHARMACY && k == 0, q, st);
    st->dressed++;
    st->by_cat[cat]++;
    k = sh_flush(c, b->sb, q, st);
    free(q);
    return k;
}

/* ---------------------------------------------------------------- fuel canopies
 *
 * La Plata's stations are corner lots whose forecourt OSM maps with a SERVICE drive through it (MEASURED on the casco
 * census, TD5RE_ROAM_SHOP_LOG=1: the nearest street of 30 of the 41 owned sites is class SERVICE, the site 0-9 m off its
 * kerb). So the deck may span a service drive (it stands 4.8 m over it), while the posts and the pump islands stand on
 * no drivable surface at all and the deck covers no real street. The placement is planned in the building pass (it
 * needs the halo footprints) and written in the furniture tail (td5_roam_shops_fuel_flush): the F4a census is 2D and
 * would read a deck over a forecourt drive as "on the road"; this module's own test above is the canopy's guard. */

static int sh_pip(const double *x, const double *z, int n, double px, double pz)
{
    int i, j, in = 0;
    for (i = 0, j = n - 1; i < n; j = i++)
        if ((z[i] > pz) != (z[j] > pz) && px < (x[j] - x[i]) * (pz - z[i]) / (z[j] - z[i]) + x[i]) in = !in;
    return in;
}

/* a deck point is acceptable: off the drivable surface, or on it only inside a SERVICE drive's carriageway */
static int sh_deck_pt_ok(TG_TileCtx *c, double x, double z)
{
    int edge, side;
    double along, lat;
    RoamFrame f;
    if (!roam_ctx_on_road(c, &x, &z, 1)) return 1;
    if (!roam_nearest_edge(c, x, z, SH_M(30.0), &edge, &side, &along, &lat) || !roam_edge_frame(c, edge, side, along, &f))
        return 0;                          /* on a junction patch: a street */
    return f.klass == TD5_GEO_RC_SERVICE && fabs(lat) <= f.half_carriage + SH_M(0.5);
}

/* a small rectangle (posts, islands) on no drivable surface */
static int sh_rect_clear(TG_TileCtx *c, double cx, double cz, double tx, double tz, double hl, double hw)
{
    const double nx = -tz, nz = tx;
    double qx[4], qz[4];
    qx[0] = cx - tx * hl - nx * hw; qz[0] = cz - tz * hl - nz * hw;
    qx[1] = cx + tx * hl - nx * hw; qz[1] = cz + tz * hl - nz * hw;
    qx[2] = cx + tx * hl + nx * hw; qz[2] = cz + tz * hl + nz * hw;
    qx[3] = cx - tx * hl + nx * hw; qz[3] = cz - tz * hl + nz * hw;
    return !roam_ctx_on_road(c, qx, qz, 4);
}

#define SH_POST_L(hl) ((hl) - SH_M(1.5))
#define SH_POST_W(hw) ((hw) * 0.85)
#define SH_ISL_W(hw)  ((hw) * 0.45)

/* why[]: 0 deck over a street, 1 a post on a road, 2 outside the forecourt, 3 on a footprint */
static int sh_can_ok(TG_TileCtx *c, double cx, double cz, double tx, double tz, double hl, double hw,
                     int (*in_bld)(void *u, double x, double z), void *u, const double *rx, const double *rz, int rn,
                     int *why, int *islands, int *posts)
{
    const double nx = -tz, nz = tx;
    int a, bb, k;
    if (rn >= 3 && !sh_pip(rx, rz, rn, cx, cz)) { why[2]++; return 0; }
    for (a = -2; a <= 2; a++)
        for (bb = -2; bb <= 2; bb++) {
            const double x = cx + tx * hl * a * 0.5 + nx * hw * bb * 0.5, z = cz + tz * hl * a * 0.5 + nz * hw * bb * 0.5;
            if (!sh_deck_pt_ok(c, x, z)) { why[0]++; return 0; }
        }
    {   /* at least two posts on clear ground (a cantilevered deck); only the clear ones are drawn */
        int nposts = 0;
        *posts = 0;
        for (k = 0; k < 4; k++) {
            const double sl = (k & 1) ? 1.0 : -1.0, sw = (k & 2) ? 1.0 : -1.0;
            if (sh_rect_clear(c, cx + tx * sl * SH_POST_L(hl) + nx * sw * SH_POST_W(hw),
                              cz + tz * sl * SH_POST_L(hl) + nz * sw * SH_POST_W(hw), tx, tz, SH_M(0.4), SH_M(0.4)))
                { *posts |= 1 << k; nposts++; }
        }
        if (nposts < 2) { why[1]++; return 0; }
    }
    if (in_bld)
        for (a = -2; a <= 2; a++)
            for (bb = -2; bb <= 2; bb++)
                if (in_bld(u, cx + tx * hl * a * 0.5 + nx * hw * bb * 0.5, cz + tz * hl * a * 0.5 + nz * hw * bb * 0.5))
                    { why[3]++; return 0; }
    *islands = 1;
    for (k = 0; k < 2; k++) {
        const double sw = k ? 1.0 : -1.0;
        if (!sh_rect_clear(c, cx + nx * sw * SH_ISL_W(hw), cz + nz * sw * SH_ISL_W(hw), tx, tz, hl - SH_M(3.0), SH_M(0.8)))
            *islands = 0;
    }
    return 1;
}

int td5_roam_shops_fuel(TD5_RoamShopSource *s, TG_TileCtx *c, int (*in_bld)(void *u, double x, double z), void *u,
                        TD5_RoamShopStats *st)
{
    int i, tx, tz;
    int32_t ox, oz;
    if (!s) return 1;
    s->npend = 0;
    if (!s->canopy) return 1;
    sh_load(s);
    if (s->nf <= 0) return 1;
    roam_ctx_origin(c, &tx, &tz, &ox, &oz);
    for (i = 0; i < s->nf; i++) {
        const double px = s->fx[i] - (double)ox, pz = s->fz[i] - (double)oz;
        double rx[64], rz[64], along, lat, rdx, rdz, rd;
        int rn = s->fringn[i] > 64 ? 0 : s->fringn[i], k, edge, side, placed = 0, why[4] = { 0, 0, 0, 0 };
        RoamFrame f;
        if (!roam_ctx_owned(c, px, pz)) continue;
        for (k = 0; k < rn; k++) { rx[k] = s->rx[s->fring0[i] + k] - (double)ox; rz[k] = s->rz[s->fring0[i] + k] - (double)oz; }
        if (!roam_nearest_edge(c, px, pz, SH_M(45.0), &edge, &side, &along, &lat) ||
            !roam_edge_frame(c, edge, side, along, &f)) {
            st->canopy_refused++;
            if (s->log) TD5_LOG_I(LOG_TAG, "ROAM_SHOP_FUEL tile %d,%d site %d: no street within 45 m", tx, tz, i);
            continue;
        }
        rdx = f.px - px; rdz = f.pz - pz; rd = hypot(rdx, rdz);
        if (rd > 1.0) { rdx /= rd; rdz /= rd; } else { rdx = 0.0; rdz = 0.0; }
        {
            /* the site point is often on the pavement or on the forecourt's own SERVICE drive (the nearest edge,
             * so its direction says nothing about where the street is): rings of candidate centres round the
             * point, 2.5 m apart out to 12.5 m in eight directions, the deck along the nearest edge or across it,
             * full size first */
            static const double ring_m[6] = { 0.0, 2.5, 5.0, 7.5, 10.0, 12.5 };
            static const double scale[3] = { 1.0, 0.8, 0.6 };
            int a, sc, sd, ori, isl = 0, posts = 0;
            (void)rdx; (void)rdz;
            for (sc = 0; sc < 3 && !placed; sc++)
                for (a = 0; a < 6 && !placed; a++)
                    for (sd = 0; sd < (a ? 8 : 1) && !placed; sd++)
                        for (ori = 0; ori < 2 && !placed; ori++) {
                            const double ang = (double)sd * 0.78539816339744831;
                            const double dx = f.tx * cos(ang) - f.tz * sin(ang), dz = f.tz * cos(ang) + f.tx * sin(ang);
                            const double ttx = ori ? -f.tz : f.tx, ttz = ori ? f.tx : f.tz;
                            const double cx = px + dx * SH_M(ring_m[a]);
                            const double cz = pz + dz * SH_M(ring_m[a]);
                            const double hl = 0.5 * SH_CAN_L * scale[sc], hw = 0.5 * SH_CAN_W * scale[sc];
                            ShCanopy *cp;
                            if (!sh_can_ok(c, cx, cz, ttx, ttz, hl, hw, in_bld, u, rx, rz, rn, why, &isl, &posts)) continue;
                            if (s->npend >= SH_CAN_MAX) break;
                            cp = &s->pend[s->npend++];
                            cp->cx = cx; cp->cz = cz; cp->tx = ttx; cp->tz = ttz; cp->hl = hl; cp->hw = hw;
                            cp->gy = roam_ctx_ground(c, cx, cz) + SH_M(0.15);
                            cp->col = s->fcol[i];
                            cp->islands = isl;
                            cp->posts = posts;
                            cp->sb = roam_ctx_sb_of(c, cx, cz);
                            placed = 1;
                        }
        }
        if (!placed) st->canopy_refused++;
        if (s->log)
            TD5_LOG_I(LOG_TAG, "ROAM_SHOP_FUEL tile %d,%d site %d (%.0f,%.0f) ring %d: %s (tries refused: deck over a "
                      "street %d, post on a road %d, outside the forecourt %d, on a footprint %d) nearest street class %d, "
                      "site %.1f m off its kerb", tx, tz, i, s->fx[i], s->fz[i], rn, placed ? "canopy" : "no clear spot",
                      why[0], why[1], why[2], why[3], f.klass, (fabs(lat) - f.half_carriage) / SH_UPM);
    }
    return 1;
}

int td5_roam_shops_fuel_flush(TD5_RoamShopSource *s, TG_TileCtx *c, TD5_RoamShopStats *st)
{
    int i, pk;
    if (!s) return 1;
    for (i = 0; i < s->npend; i++) {
        const ShCanopy *cp = &s->pend[i];
        const double nnx = -cp->tz, nnz = cp->tx, gy = cp->gy;
        ShQB *q = (ShQB *)malloc(sizeof *q);
        int ok;
        if (!q) return 0;
        q->nq = 0;
        /* deck: light grey soffit and top, the brand fascia round it */
        sh_box(q, cp->cx, cp->cz, gy + SH_CAN_CLEAR, gy + SH_CAN_CLEAR + SH_CAN_T, cp->tx, cp->tz, cp->hl, cp->hw,
               cp->col, 0xFFD8D8D8u, 1);
        for (pk = 0; pk < 4; pk++) {
            const double sl = (pk & 1) ? 1.0 : -1.0, sw = (pk & 2) ? 1.0 : -1.0;
            if (!(cp->posts & (1 << pk))) continue;
            const double qx = cp->cx + cp->tx * sl * SH_POST_L(cp->hl) + nnx * sw * SH_POST_W(cp->hw);
            const double qz = cp->cz + cp->tz * sl * SH_POST_L(cp->hl) + nnz * sw * SH_POST_W(cp->hw);
            sh_box(q, qx, qz, gy - SH_M(0.3), gy + SH_CAN_CLEAR, cp->tx, cp->tz, SH_M(0.2), SH_M(0.2),
                   0xFFB8B8B8u, 0xFFB8B8B8u, 0);
        }
        if (cp->islands)
            for (pk = 0; pk < 2; pk++) {
                const double sw = pk ? 1.0 : -1.0;
                const double qx = cp->cx + nnx * sw * SH_ISL_W(cp->hw), qz = cp->cz + nnz * sw * SH_ISL_W(cp->hw);
                sh_box(q, qx, qz, gy - SH_M(0.1), gy + SH_M(0.25), cp->tx, cp->tz, cp->hl - SH_M(3.0), SH_M(0.6),
                       0xFFA0A0A0u, 0xFFA0A0A0u, 0);
                sh_box(q, qx + cp->tx * SH_M(1.6), qz + cp->tz * SH_M(1.6), gy + SH_M(0.25), gy + SH_M(1.9), cp->tx, cp->tz,
                       SH_M(0.45), SH_M(0.3), cp->col, 0xFFE0E0E0u, 0);
                sh_box(q, qx - cp->tx * SH_M(1.6), qz - cp->tz * SH_M(1.6), gy + SH_M(0.25), gy + SH_M(1.9), cp->tx, cp->tz,
                       SH_M(0.45), SH_M(0.3), cp->col, 0xFFE0E0E0u, 0);
            }
        ok = sh_flush(c, cp->sb, q, st);
        free(q);
        if (!ok) return 0;
        st->canopies++;
        if (cp->islands) st->canopy_islands++;
        if (roam_ctx_on_road(c, &cp->cx, &cp->cz, 1)) st->canopy_service++;   /* the deck centre over a service drive */
    }
    s->npend = 0;
    return 1;
}
