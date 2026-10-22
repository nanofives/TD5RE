/**
 * td5_roam_rails.c -- GEO FREE ROAM: RAILWAYS of one tile (PORT-ONLY, roam ID).
 * Contract, heights and the guard rule: td5_roam_rails.h.
 *
 * SECTIONS
 *   source        INDEX.JSON layers.RAILS, every RAILS tile parsed once (game frame), ways sorted by id
 *   lift          the rail NETWORK (ways joined at shared vertices) and the max-plus Dijkstra that
 *                 carries every rail_over clearance requirement along it at RL_GRADE
 *   heights       base (raw raster lattice mean), the bridge line, the level-crossing blend
 *   writers       per (sub-block, page) quad batches -> one MODELS.DAT prefab record each, + the guard audit
 *   track         ballast bed / embankment, sleepers, rails, flush rails on a road, bridge deck + piers
 *   platforms     polygon slabs (ear clipping) and line platforms
 *   emit          the tile: owned pieces of every way that reaches it
 *
 * Every decision keys off the way (id order), its cumulative along-distance and the place-frame
 * position, never off the order tiles are built in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_trackgen_internal.h"   /* TG_Buf, tg_write_prefab_mesh, TD5_TG_PAGE_*, k_biomes */
#include "td5_geo.h"                 /* td5_geo_source_path, TD5_GEO_EXAGGERATION_X100_DEFAULT  */
#include "td5_geo_roads.h"           /* td5_geo_roads_slurp                                    */
#include "td5_geo_frame.h"           /* [roam M] the source -> game reflection                 */
#include "td5_config.h"
#include "td5_platform.h"
#include "deps/cjson/cJSON.h"
#include "td5_roam_height.h"
#include "td5_roam_scenery.h"
#include "td5_roam_rails.h"
#include "td5_roam_furniture.h"   /* [roam LF] td5_roam_furn_plate_cell: station board text */

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "roam"

#define RL_UPM          430.0
#define RL_M(m)         ((m) * RL_UPM)
/* the vertical stack, from the rail head down */
#define RL_HEAD         RL_M(0.45)     /* rail head over the ground on plain track          */
#define RL_RAIL_H       RL_M(0.15)     /* rail height (head to foot = sleeper top)          */
#define RL_RAIL_HW      RL_M(0.036)    /* half the rail head width                          */
#define RL_CREST_DY     RL_M(0.22)     /* ballast crest under the head                      */
#define RL_FLUSH        8.0            /* a rail laid on a road: this far over the surface  */
#define RL_SLEEPER_STEP RL_M(0.65)
#define RL_SLEEPER_HW   RL_M(0.125)
#define RL_SLEEPER_OUT  RL_M(0.55)     /* sleeper end past the rail                         */
#define RL_CREST_OUT    RL_M(0.30)     /* ballast crest past the sleeper end                */
#define RL_SLOPE        1.5            /* shoulder run per unit drop                         */
#define RL_TOE_MIN      RL_M(0.60)
#define RL_TOE_MAX      RL_M(12.0)
#define RL_EMBANK_M     1.0            /* crest this far over the ground: earth embankment   */
/* bridges */
#define RL_DECK_TOP     RL_M(0.40)     /* deck top under the head                            */
#define RL_DECK_BOT     RL_M(1.20)     /* deck soffit under the head                         */
#define RL_DECK_HW      RL_M(2.40)
#define RL_PARAPET      RL_M(0.90)
#define RL_PIER_STEP    RL_M(15.0)
#define RL_PIER_END     RL_M(3.0)      /* no pier this near a bridge end (the abutment)      */
#define RL_PIER_HA      RL_M(0.50)     /* half size along the track                          */
#define RL_PIER_HC      RL_M(1.60)     /* half size across                                   */
#define RL_CLEAR        RL_M(6.20)     /* rail head over a road that passes under: 5 m + deck */
#define RL_GRADE        0.03
/* crossings */
#define RL_BLEND        RL_M(12.0)     /* the bed blends to a level crossing's road over this */
#define RL_FLOOR        245.0          /* [Avenida 1] head over a street's kerb: the F4b median / sidewalk slab
                                        * rises up to ~126 units over the kerb line (measured at the pick), the
                                        * crest sits RL_CREST_DY under the head, + 1 cm                      */
#define RL_FLOOR_REACH  RL_M(12.0)     /* full floor this far past the carriageway edge                      */
#define RL_FLOOR_FALL   0.15           /* then it falls off at this lateral grade                            */
#define RL_NUDGE        RL_M(1.5)      /* the most a section is moved off a parallel street   */
/* platforms */
#define RL_PLAT_TOP     RL_M(0.95)     /* platform top over the rail head (base)             */
#define RL_PLAT_HW      RL_M(2.00)     /* half width of a platform mapped as a line          */
#define RL_PLAT_RING    512
/* pieces / batches */
#define RL_PIECE        RL_M(4.0)
#define RL_CHUNK        128            /* quads per record                                   */
#define RL_BASE_STEP    RL_M(6.0)      /* the base lattice: 5 x 5 at this step               */
#define RL_GUARD_LO     RL_M(0.5)
#define RL_GUARD_HI     RL_M(2.5)

enum { RK_RAIL = 0, RK_LIGHT, RK_DISUSED, RK_PLATFORM };
enum { RX_GRADE = 0, RX_RAIL_OVER, RX_ROAD_OVER, RX_TUNNEL };
enum { RP_BALLAST = 0, RP_SLEEPER, RP_RAIL, RP_CONC, RP_PLAT, RP_EARTH, RP_N };

/* ARGB, baked per vertex */
#define RC_WHITE     0xFFFFFFFFu
#define RC_BALLAST   0xFFD4D4D4u
#define RC_BALLAST_D 0xFFB4BA9Cu      /* disused: greener, darker */
#define RC_SLEEPER   0xFF8C7C6Cu
#define RC_RAIL_TOP  0xFFF0F0F4u
#define RC_RAIL_SIDE 0xFF8A8A90u
#define RC_RUST_TOP  0xFFB08060u
#define RC_RUST_SIDE 0xFF805840u

/* ============================================================================
 * SECTION: source
 * ========================================================================== */

typedef struct { double x, z, s, yreq; int seg, kind; } RlX;     /* place frame (game) */

/* [roam LF] the RAILS nodes[] this module reads: level crossings (with their equipment tags), stations / halts / stops */
enum { RN_LX = 0, RN_STATION, RN_HALT, RN_STOP };
typedef struct { double x, z; int kind, barrier, light, saltire; char name[96]; } RlNode;   /* barrier 0 none/no, 1 half, 2 full */
typedef struct { double x, z; int kind; char name[96]; } RlStation;                          /* one per name + 400 m cluster */

typedef struct {
    int64_t id;
    int     kind, bridge, tunnel, closed;
    int     elec;                  /* [roam LF] electrified=contact_line: catenary */
    double  gauge;                 /* units, between the rails' inner faces */
    int     p0, n;                 /* into px / pz / ps / node              */
    int     x0, nx;                /* into x[]                              */
    double  bx0, bz0, bx1, bz1;    /* bbox, place frame                     */
    double  len;
} RlWay;

struct TD5_RoamRailSource {
    char     place[64];
    RlWay   *w;  int nw, capw;
    double  *px, *pz, *ps;  int *node;  int np, capp;
    RlX     *x;  int nx, capx;
    RlNode  *nd; int nnd, capnd;       /* [roam LF] */
    RlStation *stn; int nstn;
    double  *L;  int nnode;        /* lift head per network node (-1e30 = none)          */
    int      nlift;
    double   exag;
    /* LAZY: open() only reads INDEX.JSON; the RAILS tiles are parsed and the lift solved on the first
     * emit, i.e. on the building thread AFTER it took the main thread's FP environment (strtod and the
     * height sampling are FP-env sensitive: parsed at open() on a fresh worker, a coordinate's last bits
     * moved and the determinism T arm differed on the light-rail tiles 5,49 / 5,51) */
    int      loaded, load_ok;
    int    (*tile)[2];  int ntile;
};

int td5_roam_rails_enabled(void) { return td5_env_int("TD5RE_ROAM_RAILS", 1, 0, 1); }

static int rl_grow(void **p, int *cap, int need, size_t elem)
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

/* the four per-point arrays grow together */
static int rl_grow_pts(TD5_RoamRailSource *r, int need)
{
    int nc;
    void *a, *b, *c, *d;
    if (need <= r->capp) return 1;
    nc = r->capp ? r->capp * 2 : 1024;
    while (nc < need) nc *= 2;
    a = realloc(r->px, (size_t)nc * sizeof(double));
    if (!a) return 0;
    r->px = (double *)a;
    b = realloc(r->pz, (size_t)nc * sizeof(double));
    if (!b) return 0;
    r->pz = (double *)b;
    c = realloc(r->ps, (size_t)nc * sizeof(double));
    if (!c) return 0;
    r->ps = (double *)c;
    d = realloc(r->node, (size_t)nc * sizeof(int));
    if (!d) return 0;
    r->node = (int *)d;
    r->capp = nc;
    return 1;
}

static int rl_kind(const char *k)
{
    if (!k) return -1;
    if (!strcmp(k, "rail")) return RK_RAIL;
    if (!strcmp(k, "light_rail")) return RK_LIGHT;
    if (!strcmp(k, "disused")) return RK_DISUSED;
    if (!strcmp(k, "platform")) return RK_PLATFORM;
    return -1;                     /* abandoned / dismantled / razed / proposed / miniature / yards: not drawn */
}

static int rl_xkind(const char *k)
{
    if (!k) return -1;
    if (!strcmp(k, "grade")) return RX_GRADE;
    if (!strcmp(k, "rail_over")) return RX_RAIL_OVER;
    if (!strcmp(k, "road_over")) return RX_ROAD_OVER;
    if (!strcmp(k, "tunnel")) return RX_TUNNEL;
    return -1;
}

static int rl_true(const cJSON *e, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(e, key);
    return v && (cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0.0));
}

/* gauge tag "1676" / "1000" / "1000;1676" (the widest wins: the outer rails are drawn), default 1676 */
static double rl_gauge(const cJSON *e)
{
    const cJSON *g = cJSON_GetObjectItem(e, "gauge");
    double best = 0.0;
    if (g && cJSON_IsString(g) && g->valuestring) {
        const char *p = g->valuestring;
        while (*p) {
            char *end;
            const double v = strtod(p, &end);
            if (end == p) { p++; continue; }
            if (v > best && v >= 500.0 && v <= 2000.0) best = v;
            p = end;
        }
    }
    if (best <= 0.0) best = 1676.0;
    return best / 1000.0 * RL_UPM;
}

static int rl_load_tile(TD5_RoamRailSource *r, int ix, int iz)
{
    char rel[96], path[512];
    char *json;
    cJSON *root;
    const cJSON *arr, *e;
    snprintf(rel, sizeof rel, "tiles/RAILS_%d_%d.JSON", ix, iz);
    td5_geo_source_path(path, sizeof path, r->place, rel);
    json = td5_geo_roads_slurp(path);
    if (!json) return 1;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 1;
    td5_geo_src_json_to_game(root);                 /* [roam M] every "x","z" into the game frame */
    arr = cJSON_GetObjectItem(root, "rails");
    for (e = arr ? arr->child : NULL; e; e = e->next) {
        const cJSON *pk = cJSON_GetObjectItem(e, "kind"), *id = cJSON_GetObjectItem(e, "id");
        const cJSON *pts = cJSON_GetObjectItem(e, "points"), *xs = cJSON_GetObjectItem(e, "road_x"), *p;
        const int kind = rl_kind(pk && cJSON_IsString(pk) ? pk->valuestring : NULL);
        RlWay *w;
        int n = 0;
        if (kind < 0 || !id || !cJSON_IsNumber(id) || !pts || !cJSON_IsArray(pts)) continue;
        if (cJSON_GetArraySize(pts) < 2) continue;
        if (!rl_grow((void **)&r->w, &r->capw, r->nw + 1, sizeof(RlWay))) { cJSON_Delete(root); return 0; }
        w = &r->w[r->nw];
        memset(w, 0, sizeof *w);
        w->id = (int64_t)id->valuedouble;
        w->kind = kind;
        w->bridge = rl_true(e, "bridge");
        w->tunnel = rl_true(e, "tunnel");
        w->closed = rl_true(e, "closed");
        {   const cJSON *el = cJSON_GetObjectItem(e, "electrified");
            w->elec = el && cJSON_IsString(el) && el->valuestring && !strcmp(el->valuestring, "contact_line");
        }
        w->gauge = rl_gauge(e);
        w->p0 = r->np;
        w->bx0 = w->bz0 = 1e300; w->bx1 = w->bz1 = -1e300;
        for (p = pts->child; p; p = p->next) {
            const cJSON *px = cJSON_GetObjectItem(p, "x"), *pz = cJSON_GetObjectItem(p, "z");
            if (!px || !pz || !cJSON_IsNumber(px) || !cJSON_IsNumber(pz)) continue;
            if (!rl_grow_pts(r, r->np + 1)) { cJSON_Delete(root); return 0; }
            r->px[r->np] = px->valuedouble; r->pz[r->np] = pz->valuedouble;
            r->ps[r->np] = (n == 0) ? 0.0 : r->ps[r->np - 1] + hypot(r->px[r->np] - r->px[r->np - 1], r->pz[r->np] - r->pz[r->np - 1]);
            r->node[r->np] = -1;
            if (r->px[r->np] < w->bx0) w->bx0 = r->px[r->np];
            if (r->px[r->np] > w->bx1) w->bx1 = r->px[r->np];
            if (r->pz[r->np] < w->bz0) w->bz0 = r->pz[r->np];
            if (r->pz[r->np] > w->bz1) w->bz1 = r->pz[r->np];
            r->np++; n++;
        }
        w->n = n;
        if (n < 2) { r->np -= n; continue; }
        w->len = r->ps[r->np - 1];
        w->x0 = r->nx;
        for (p = xs ? xs->child : NULL; p; p = p->next) {
            const cJSON *qx = cJSON_GetObjectItem(p, "x"), *qz = cJSON_GetObjectItem(p, "z");
            const cJSON *qs = cJSON_GetObjectItem(p, "seg"), *qt = cJSON_GetObjectItem(p, "t"), *qk = cJSON_GetObjectItem(p, "kind");
            const int xk = rl_xkind(qk && cJSON_IsString(qk) ? qk->valuestring : NULL);
            int seg;
            RlX *x;
            if (xk < 0 || !qx || !qz || !qs || !qt || !cJSON_IsNumber(qs) || !cJSON_IsNumber(qt)) continue;
            seg = (int)qs->valuedouble;
            if (seg < 0 || seg + 1 >= n) continue;
            if (!rl_grow((void **)&r->x, &r->capx, r->nx + 1, sizeof(RlX))) { cJSON_Delete(root); return 0; }
            x = &r->x[r->nx++];
            x->x = qx->valuedouble; x->z = qz->valuedouble;
            x->seg = seg; x->kind = xk; x->yreq = -1e30;
            x->s = r->ps[w->p0 + seg] + qt->valuedouble * (r->ps[w->p0 + seg + 1] - r->ps[w->p0 + seg]);
        }
        w->nx = r->nx - w->x0;
        r->nw++;
    }
    {   /* [roam LF] nodes[]: level crossings with equipment, stations / halts / stops (x already in the game frame) */
        const cJSON *na = cJSON_GetObjectItem(root, "nodes"), *nn;
        for (nn = na ? na->child : NULL; nn; nn = nn->next) {
            const cJSON *pk = cJSON_GetObjectItem(nn, "kind"), *qx = cJSON_GetObjectItem(nn, "x"), *qz = cJSON_GetObjectItem(nn, "z");
            const cJSON *tg = cJSON_GetObjectItem(nn, "tags"), *nm = cJSON_GetObjectItem(nn, "name"), *t;
            const char *k = pk && cJSON_IsString(pk) ? pk->valuestring : NULL;
            RlNode *o;
            int kind = -1;
            if (!k || !qx || !qz || !cJSON_IsNumber(qx) || !cJSON_IsNumber(qz)) continue;
            if (!strcmp(k, "level_crossing")) kind = RN_LX;
            else if (!strcmp(k, "station")) kind = RN_STATION;
            else if (!strcmp(k, "halt")) kind = RN_HALT;
            else if (!strcmp(k, "stop")) kind = RN_STOP;
            if (kind < 0) continue;
            if (kind != RN_LX && !(nm && cJSON_IsString(nm) && nm->valuestring && nm->valuestring[0])) continue;
            if (!rl_grow((void **)&r->nd, &r->capnd, r->nnd + 1, sizeof(RlNode))) { cJSON_Delete(root); return 0; }
            o = &r->nd[r->nnd++];
            memset(o, 0, sizeof *o);
            o->x = qx->valuedouble; o->z = qz->valuedouble; o->kind = kind;
            if (nm && cJSON_IsString(nm) && nm->valuestring) snprintf(o->name, sizeof o->name, "%s", nm->valuestring);
            if (kind == RN_LX && tg) {
                t = cJSON_GetObjectItem(tg, "crossing:barrier");
                if (t && cJSON_IsString(t) && t->valuestring) {
                    if (!strcmp(t->valuestring, "half")) o->barrier = 1;
                    else if (!strcmp(t->valuestring, "full") || !strcmp(t->valuestring, "yes")) o->barrier = 2;
                }
                t = cJSON_GetObjectItem(tg, "crossing:light");
                o->light = t && cJSON_IsString(t) && t->valuestring && !strcmp(t->valuestring, "yes");
                t = cJSON_GetObjectItem(tg, "crossing:saltire");
                o->saltire = t && cJSON_IsString(t) && t->valuestring && !strcmp(t->valuestring, "yes");
            }
        }
    }
    cJSON_Delete(root);
    return 1;
}

static int rl_node_cmp(const void *pa, const void *pb)
{
    const RlNode *a = (const RlNode *)pa, *b = (const RlNode *)pb;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    if (a->z != b->z) return a->z < b->z ? -1 : 1;
    return a->kind - b->kind;
}

/* one board site per (name, 400 m cluster): a station, halt or stop of the same name within 400 m of a
 * cluster's running centroid joins it; the first kind seen (station < halt < stop by the node sort below) names it */
static void rl_build_stations(TD5_RoamRailSource *r)
{
    int i, j;
    int *cnt;
    r->nstn = 0;
    r->stn = (RlStation *)calloc((size_t)(r->nnd ? r->nnd : 1), sizeof(RlStation));
    cnt = (int *)calloc((size_t)(r->nnd ? r->nnd : 1), sizeof(int));
    if (!r->stn || !cnt) { free(r->stn); r->stn = NULL; free(cnt); return; }
    for (i = 0; i < r->nnd; i++) {
        const RlNode *n = &r->nd[i];
        if (n->kind == RN_LX) continue;
        if (n->kind == RN_STOP) {   /* a stop within 150 m of a station / halt is that station's platform stop, whatever its spelling */
            int k, shadow = 0;
            for (k = 0; k < r->nnd && !shadow; k++)
                if ((r->nd[k].kind == RN_STATION || r->nd[k].kind == RN_HALT) && hypot(r->nd[k].x - n->x, r->nd[k].z - n->z) < RL_M(150.0)) shadow = 1;
            if (shadow) continue;
        }
        for (j = 0; j < r->nstn; j++)
            if (!strcmp(r->stn[j].name, n->name) && hypot(r->stn[j].x - n->x, r->stn[j].z - n->z) < RL_M(400.0)) break;
        if (j == r->nstn) {
            r->stn[j].x = n->x; r->stn[j].z = n->z; r->stn[j].kind = n->kind;
            snprintf(r->stn[j].name, sizeof r->stn[j].name, "%s", n->name);
            cnt[j] = 1; r->nstn++;
        } else {
            r->stn[j].x = (r->stn[j].x * cnt[j] + n->x) / (cnt[j] + 1);
            r->stn[j].z = (r->stn[j].z * cnt[j] + n->z) / (cnt[j] + 1);
            cnt[j]++;
            if (n->kind < r->stn[j].kind) r->stn[j].kind = n->kind;
        }
    }
    free(cnt);
}

static int rl_way_cmp(const void *pa, const void *pb)
{
    const RlWay *a = (const RlWay *)pa, *b = (const RlWay *)pb;
    return a->id < b->id ? -1 : (a->id > b->id ? 1 : 0);
}

/* ============================================================================
 * SECTION: lift (the rail network + the max-plus Dijkstra)
 * ========================================================================== */

typedef struct { double v; int n; } RlHeap;

static void rl_heap_push(RlHeap *h, int *nh, double v, int n)
{
    int i = (*nh)++;
    while (i > 0) {
        const int p = (i - 1) / 2;
        if (h[p].v > v || (h[p].v == v && h[p].n <= n)) break;
        h[i] = h[p]; i = p;
    }
    h[i].v = v; h[i].n = n;
}

static RlHeap rl_heap_pop(RlHeap *h, int *nh)
{
    RlHeap top = h[0], last = h[--(*nh)];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= *nh) break;
        if (c + 1 < *nh && (h[c + 1].v > h[c].v || (h[c + 1].v == h[c].v && h[c + 1].n < h[c].n))) c++;
        if (last.v > h[c].v || (last.v == h[c].v && last.n <= h[c].n)) break;
        h[i] = h[c]; i = c;
    }
    if (*nh > 0) h[i] = last;
    return top;
}

static const TD5_RoamRailSource *s_sort_src;
static int rl_pt_cmp(const void *pa, const void *pb)
{
    const int a = *(const int *)pa, b = *(const int *)pb;
    const TD5_RoamRailSource *r = s_sort_src;
    if (r->px[a] != r->px[b]) return r->px[a] < r->px[b] ? -1 : 1;
    if (r->pz[a] != r->pz[b]) return r->pz[a] < r->pz[b] ? -1 : 1;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* mean of the raw raster over the 5 x 5 base lattice about a place-frame point, wet samples left out */
static int rl_base_ground_hc(TD5_RoamHeight *hc, double x, double z, double *out)
{
    double sum = 0.0, sumw = 0.0;
    int i, j, n = 0, nw = 0;
    for (j = -2; j <= 2; j++)
        for (i = -2; i <= 2; i++) {
            double h;
            int wet = 0;
            if (!td5_roam_height_sample(hc, x + i * RL_BASE_STEP, z + j * RL_BASE_STEP, &h, &wet)) continue;
            if (wet) { sumw += h; nw++; } else { sum += h; n++; }
        }
    if (n > 0) { *out = sum / n; return 1; }
    if (nw > 0) { *out = sumw / nw; return 1; }
    return 0;
}

static int rl_solve_lift(TD5_RoamRailSource *r, const char *height_path, const char *water_path)
{
    int *ord = NULL, *estart = NULL, *eitem = NULL;
    double *ew = NULL;
    RlHeap *heap = NULL;
    int i, k, nh = 0, nn = 0, ne = 0, ok = 0;
    TD5_RoamHeight hc;
    int hc_ok;
    memset(&hc, 0, sizeof hc);
    hc_ok = td5_roam_height_open(&hc, height_path, water_path, r->exag);
    /* 1. network nodes: track vertices that share a position are one node (OSM shares the node) */
    ord = (int *)malloc((size_t)(r->np > 0 ? r->np : 1) * sizeof(int));
    if (!ord) goto out;
    for (i = 0, k = 0; i < r->nw; i++) {
        const RlWay *w = &r->w[i];
        int p;
        if (w->kind == RK_PLATFORM) continue;
        for (p = 0; p < w->n; p++) ord[k++] = w->p0 + p;
    }
    s_sort_src = r;
    qsort(ord, (size_t)k, sizeof(int), rl_pt_cmp);
    s_sort_src = NULL;
    for (i = 0; i < k; i++) {
        if (i == 0 || r->px[ord[i]] != r->px[ord[i - 1]] || r->pz[ord[i]] != r->pz[ord[i - 1]]) nn++;
        r->node[ord[i]] = nn - 1;
    }
    r->nnode = nn;
    r->L = (double *)malloc((size_t)(nn > 0 ? nn : 1) * sizeof(double));
    if (!r->L) goto out;
    for (i = 0; i < nn; i++) r->L[i] = -1e30;
    /* 2. edges (CSR, both directions) */
    estart = (int *)calloc((size_t)nn + 2u, sizeof(int));
    if (!estart) goto out;
    for (i = 0; i < r->nw; i++) {
        const RlWay *w = &r->w[i];
        int p;
        if (w->kind == RK_PLATFORM) continue;
        for (p = 0; p + 1 < w->n; p++) { estart[r->node[w->p0 + p] + 1]++; estart[r->node[w->p0 + p + 1] + 1]++; ne += 2; }
    }
    for (i = 0; i < nn; i++) estart[i + 1] += estart[i];
    eitem = (int *)malloc((size_t)(ne > 0 ? ne : 1) * sizeof(int));
    ew = (double *)malloc((size_t)(ne > 0 ? ne : 1) * sizeof(double));
    if (!eitem || !ew) goto out;
    {
        int *fill = (int *)calloc((size_t)nn + 1u, sizeof(int));
        if (!fill) goto out;
        for (i = 0; i < r->nw; i++) {
            const RlWay *w = &r->w[i];
            int p;
            if (w->kind == RK_PLATFORM) continue;
            for (p = 0; p + 1 < w->n; p++) {
                const int a = r->node[w->p0 + p], b = r->node[w->p0 + p + 1];
                const double l = r->ps[w->p0 + p + 1] - r->ps[w->p0 + p];
                eitem[estart[a] + fill[a]] = b; ew[estart[a] + fill[a]++] = l;
                eitem[estart[b] + fill[b]] = a; ew[estart[b] + fill[b]++] = l;
            }
        }
        free(fill);
    }
    /* 3. sources: every rail_over crossing's clearance, onto its segment's two nodes */
    heap = (RlHeap *)malloc((size_t)(ne + 2 * r->nx + 4) * sizeof(RlHeap));
    if (!heap) goto out;
    for (i = 0; i < r->nw; i++) {
        const RlWay *w = &r->w[i];
        if (w->kind == RK_PLATFORM) continue;
        for (k = 0; k < w->nx; k++) {
            RlX *x = &r->x[w->x0 + k];
            double g;
            int a, b;
            if (x->kind != RX_RAIL_OVER) continue;
            if (!hc_ok || !td5_roam_height_prefetch(&hc, x->x - RL_M(20.0), x->z - RL_M(20.0), x->x + RL_M(20.0), x->z + RL_M(20.0)) ||
                !rl_base_ground_hc(&hc, x->x, x->z, &g)) continue;
            x->yreq = g + RL_CLEAR;
            r->nlift++;
            a = r->node[w->p0 + x->seg]; b = r->node[w->p0 + x->seg + 1];
            {
                const double va = x->yreq - RL_GRADE * (x->s - r->ps[w->p0 + x->seg]);
                const double vb = x->yreq - RL_GRADE * (r->ps[w->p0 + x->seg + 1] - x->s);
                if (va > r->L[a]) { r->L[a] = va; rl_heap_push(heap, &nh, va, a); }
                if (vb > r->L[b]) { r->L[b] = vb; rl_heap_push(heap, &nh, vb, b); }
            }
        }
    }
    /* 4. max-plus Dijkstra: the highest requirement first, falling RL_GRADE per unit of track */
    while (nh > 0) {
        const RlHeap t = rl_heap_pop(heap, &nh);
        int e;
        if (t.v < r->L[t.n]) continue;
        for (e = estart[t.n]; e < estart[t.n + 1]; e++) {
            const int v = eitem[e];
            const double cand = t.v - RL_GRADE * ew[e];
            if (cand > r->L[v] && cand > -1e29) {
                r->L[v] = cand;
                if (nh < ne + 2 * r->nx + 4) rl_heap_push(heap, &nh, cand, v);
            }
        }
    }
    ok = 1;
out:
    if (hc_ok) td5_roam_height_close(&hc);
    free(ord); free(estart); free(eitem); free(ew); free(heap);
    return ok;
}

static int rl_load_all(TD5_RoamRailSource *r)
{
    char hp[512], wp[512];
    int i;
    r->loaded = 1;
    for (i = 0; i < r->ntile; i++)
        if (!rl_load_tile(r, r->tile[i][0], r->tile[i][1])) return 0;
    qsort(r->w, (size_t)r->nw, sizeof(RlWay), rl_way_cmp);   /* id order: the emit order */
    qsort(r->nd, (size_t)r->nnd, sizeof(RlNode), rl_node_cmp);   /* [roam LF] position order: tiles load in any order */
    rl_build_stations(r);
    td5_geo_source_path(hp, sizeof hp, r->place, "HEIGHT.R16");
    td5_geo_source_path(wp, sizeof wp, r->place, "WATER.R8");
    if (!rl_solve_lift(r, hp, wp)) return 0;
    r->load_ok = 1;
    TD5_LOG_I(LOG_TAG, "roam rails: layer loaded, %d way(s) kept, %d vertices, %d road crossing(s), %d rail-over-road "
              "clearance source(s)", r->nw, r->np, r->nx, r->nlift);
    return 1;
}

TD5_RoamRailSource *td5_roam_rails_open(const char *place)
{
    TD5_RoamRailSource *r;
    char path[512];
    char *json;
    cJSON *root;
    const cJSON *tl, *t;
    if (!place || !td5_roam_rails_enabled()) return NULL;
    td5_geo_source_path(path, sizeof path, place, "tiles/INDEX.JSON");
    json = td5_geo_roads_slurp(path);
    if (!json) return NULL;
    root = cJSON_Parse(json);
    free(json);
    tl = root ? cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "layers"), "RAILS"), "tiles") : NULL;
    if (!tl || !cJSON_IsArray(tl)) {
        if (root) cJSON_Delete(root);
        TD5_LOG_I(LOG_TAG, "roam rails: %s has no RAILS layer (re/tools/geo_rails.py): no railways", place);
        return NULL;
    }
    r = (TD5_RoamRailSource *)calloc(1, sizeof *r);
    if (!r) { cJSON_Delete(root); return NULL; }
    snprintf(r->place, sizeof r->place, "%s", place);
    r->exag = (double)td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400) / 100.0;
    r->tile = (int (*)[2])malloc((size_t)(cJSON_GetArraySize(tl) + 1) * sizeof(int[2]));
    if (!r->tile) { cJSON_Delete(root); td5_roam_rails_close(r); return NULL; }
    for (t = tl->child; t; t = t->next) {
        const cJSON *a = t->child, *b = a ? a->next : NULL;
        if (!a || !b || !cJSON_IsNumber(a) || !cJSON_IsNumber(b)) continue;
        r->tile[r->ntile][0] = (int)a->valuedouble; r->tile[r->ntile][1] = (int)b->valuedouble;
        r->ntile++;
    }
    cJSON_Delete(root);
    return r;
}

void td5_roam_rails_close(TD5_RoamRailSource *r)
{
    if (!r) return;
    free(r->nd); free(r->stn); free(r->w); free(r->px); free(r->pz); free(r->ps); free(r->node); free(r->x); free(r->L); free(r->tile);
    free(r);
}

void td5_roam_rails_summary(const TD5_RoamRailSource *r, int *ntiles, int *ways, int *verts, int *lifts, int *xings)
{
    if (ntiles) *ntiles = r ? r->ntile : 0;
    if (ways) *ways = r ? r->nw : 0;
    if (verts) *verts = r ? r->np : 0;
    if (lifts) *lifts = r ? r->nlift : 0;
    if (xings) *xings = r ? r->nx : 0;
}

/* [integ-roamI] the distance from segment p0-p1 to segment q0-q1 (0 when they cross) */
static double rl_seg_seg(double p0x, double p0z, double p1x, double p1z, double q0x, double q0z, double q1x, double q1z)
{
    const double rx = p1x - p0x, rz = p1z - p0z, sx = q1x - q0x, sz = q1z - q0z;
    const double den = rx * sz - rz * sx;
    double best = 1e300;
    int k;
    if (fabs(den) > 1e-9) {
        const double t = ((q0x - p0x) * sz - (q0z - p0z) * sx) / den, u = ((q0x - p0x) * rz - (q0z - p0z) * rx) / den;
        if (t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0) return 0.0;
    }
    for (k = 0; k < 4; k++) {   /* each end against the other segment */
        const double ax = k < 2 ? p0x : q0x, az = k < 2 ? p0z : q0z, dx = k < 2 ? rx : sx, dz = k < 2 ? rz : sz;
        const double px = k == 0 ? q0x : k == 1 ? q1x : k == 2 ? p0x : p1x, pz = k == 0 ? q0z : k == 1 ? q1z : k == 2 ? p0z : p1z;
        const double l2 = dx * dx + dz * dz;
        double t = l2 > 0.0 ? ((px - ax) * dx + (pz - az) * dz) / l2 : 0.0, d;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        d = hypot(px - (ax + t * dx), pz - (az + t * dz));
        if (d < best) best = d;
    }
    return best;
}

int td5_roam_rails_bed_near(TD5_RoamRailSource *r, const double *X, const double *Z, int n, double clear)
{
    double x0, x1, z0, z1;
    int i, k, seg;
    if (!r || !X || !Z || n < 3) return 0;
    if (!r->loaded && !rl_load_all(r)) TD5_LOG_W(LOG_TAG, "roam rails: the RAILS layer of %s did not load: no railways", r->place);
    if (!r->load_ok) return 0;
    x0 = x1 = X[0]; z0 = z1 = Z[0];
    for (k = 1; k < n; k++) {
        if (X[k] < x0) x0 = X[k];
        if (X[k] > x1) x1 = X[k];
        if (Z[k] < z0) z0 = Z[k];
        if (Z[k] > z1) z1 = Z[k];
    }
    for (i = 0; i < r->nw; i++) {
        const RlWay *w = &r->w[i];
        /* what rl_emit draws: track (not a tunnel) and platforms; the bed reaches its crest + the shortest toe */
        const double rad = clear + (w->kind == RK_PLATFORM ? RL_PLAT_HW
                                    : 0.5 * w->gauge + RL_SLEEPER_OUT + RL_CREST_OUT + RL_TOE_MIN);
        if (w->tunnel) continue;
        if (w->bx1 < x0 - rad || w->bx0 > x1 + rad || w->bz1 < z0 - rad || w->bz0 > z1 + rad) continue;
        for (seg = 0; seg + 1 < w->n; seg++) {
            const int a = w->p0 + seg, b = a + 1;
            const double ax = r->px[a], az = r->pz[a], bx = r->px[b], bz = r->pz[b];
            if ((ax < x0 - rad && bx < x0 - rad) || (ax > x1 + rad && bx > x1 + rad) ||
                (az < z0 - rad && bz < z0 - rad) || (az > z1 + rad && bz > z1 + rad)) continue;
            {   /* a track vertex inside the polygon (crossing parity), else the nearest polygon edge */
                int in = 0, j;
                for (k = 0, j = n - 1; k < n; j = k++)
                    if (((Z[k] > az) != (Z[j] > az)) && ax < (X[j] - X[k]) * (az - Z[k]) / (Z[j] - Z[k]) + X[k]) in = !in;
                if (in) return 1;
                for (k = 0, j = n - 1; k < n; j = k++)
                    if (rl_seg_seg(ax, az, bx, bz, X[j], Z[j], X[k], Z[k]) <= rad) return 1;
            }
        }
    }
    return 0;
}

/* ============================================================================
 * SECTION: heights
 * ========================================================================== */

typedef struct {
    TG_TileCtx *c;
    TD5_RoamRailStats *st;
    const TD5_RoamRailSource *r;
    int32_t ox, oz;                /* the tile origin: tile-local = place - origin       */
    struct RlBatch *bat;
    int oom;
    int log;                       /* TD5RE_ROAM_RAILS_LOG: one line per flush piece with no data crossing */
    double gx_end[2];              /* the current way's end heads (a bridge's line)      */
} RlEm;

/* base ground (no head) at a TILE-LOCAL point: the lattice is laid in place coordinates */
static double rl_base_ground(RlEm *e, double lx, double lz)
{
    double sum = 0.0, sumw = 0.0;
    int i, j, n = 0, nw = 0;
    for (j = -2; j <= 2; j++)
        for (i = -2; i <= 2; i++) {
            double h;
            int wet = 0;
            if (!roam_ctx_ground_raw(e->c, lx + i * RL_BASE_STEP, lz + j * RL_BASE_STEP, &h, &wet)) continue;
            if (wet) { sumw += h; nw++; } else { sum += h; n++; }
        }
    if (n > 0) return sum / n;
    if (nw > 0) return sumw / nw;
    return roam_ctx_ground(e->c, lx, lz);
}

/* the lift requirement at along-distance s of way w, on its segment seg */
static double rl_lift(const RlEm *e, const RlWay *w, int seg, double s)
{
    const TD5_RoamRailSource *r = e->r;
    const int a = r->node[w->p0 + seg], b = r->node[w->p0 + seg + 1];
    double v = -1e30;
    int k;
    if (a >= 0 && r->L[a] > -1e29) { const double t = r->L[a] - RL_GRADE * (s - r->ps[w->p0 + seg]); if (t > v) v = t; }
    if (b >= 0 && r->L[b] > -1e29) { const double t = r->L[b] - RL_GRADE * (r->ps[w->p0 + seg + 1] - s); if (t > v) v = t; }
    for (k = 0; k < w->nx; k++) {
        const RlX *x = &r->x[w->x0 + k];
        if (x->kind != RX_RAIL_OVER || x->yreq < -1e29 || x->seg != seg) continue;
        { const double t = x->yreq - RL_GRADE * fabs(s - x->s); if (t > v) v = t; }
    }
    return v;
}

static double rl_smooth(double u) { if (u <= 0.0) return 0.0; if (u >= 1.0) return 1.0; return u * u * (3.0 - 2.0 * u); }

/* the rail head at along-distance s (tile-local point lx, lz) of way w; *lifted = head over base by lift;
 * *blend = 1 when a level crossing pulled it toward a road */
static double rl_head(RlEm *e, const RlWay *w, int seg, double s, double lx, double lz, double *lift_over, int *blend,
                      int *floored)
{
    const TD5_RoamRailSource *r = e->r;
    double base, head, lf;
    int k, best = -1;
    double bd = RL_BLEND;
    if (w->bridge && w->len > 0.0) base = e->gx_end[0] + (e->gx_end[1] - e->gx_end[0]) * (s / w->len);
    else base = rl_base_ground(e, lx, lz) + RL_HEAD;
    lf = rl_lift(e, w, seg, s);
    head = lf > base ? lf : base;
    if (lift_over) *lift_over = head - base;
    if (blend) *blend = 0;
    if (floored) *floored = 0;
    {   /* [Avenida 1] beside a street at track level: over its median / sidewalk slab, never under it (the
         * nearest street only, from the 3x3's ribbons: both tiles at a seam read the same one) */
        int ed = -1, sd = 0;
        double al = 0.0, lat = 0.0;
        RoamFrame fr;
        if (roam_nearest_edge(e->c, lx, lz, RL_M(30.0), &ed, &sd, &al, &lat) && roam_edge_frame(e->c, ed, sd, al, &fr) &&
            fr.kerb_y < head + RL_M(2.0)) {
            const double over = fabs(lat) - fr.half_carriage - RL_FLOOR_REACH;
            double fl = fr.kerb_y + RL_FLOOR;
            if (over > 0.0) fl -= over * RL_FLOOR_FALL;
            if (fl > head) { head = fl; if (floored) *floored = 1; }
        }
    }
    for (k = 0; k < w->nx; k++) {
        const RlX *x = &r->x[w->x0 + k];
        const double d = fabs(s - x->s);
        if (x->kind != RX_GRADE || d >= bd) continue;
        bd = d; best = k;
    }
    if (best >= 0) {
        const RlX *x = &r->x[w->x0 + best];
        double ry;
        if (roam_ctx_road_y(e->c, x->x - (double)e->ox, x->z - (double)e->oz, head, &ry)) {
            const double hx = ry + RL_FLUSH;
            head = hx + (head - hx) * rl_smooth(bd / RL_BLEND);
            if (blend) *blend = 1;
            if (e->log && bd < RL_M(1.0))
                TD5_LOG_I(LOG_TAG, "ROAM_RAIL_XING way %lld at place (%.0f,%.0f) road_y %.0f", (long long)w->id, x->x, x->z, ry);
        }
    }
    return head;
}

/* ============================================================================
 * SECTION: writers
 * ========================================================================== */

typedef struct RlBatch { int nq; float v[RL_CHUNK * 4 * 5]; unsigned int light[RL_CHUNK * 4]; } RlBatch;

static int rl_page(int p)
{
    switch (p) {
    case RP_BALLAST: return TD5_TG_PAGE_RS_DIRT + 5;          /* the gravel variant of the dirt class */
    case RP_SLEEPER: return TD5_TG_PAGE_ROAD_EXTRA + 1;       /* plank road: sleeper wood             */
    case RP_RAIL:    return TD5_TG_PAGE_R11_SIGN_POST;        /* brushed metal                        */
    case RP_CONC:    return TD5_TG_PAGE_R4_PIER;              /* smooth cast concrete                 */
    case RP_PLAT:    return TD5_TG_PAGE_SIDEWALK;
    default:         return k_biomes[0].ground_page;           /* embankment earth / grass             */
    }
}

static int rl_flush(RlEm *e, int sb, int pg)
{
    RlBatch *q = &e->bat[sb * RP_N + pg];
    unsigned short cmd[3];
    TG_Buf *b;
    if (q->nq <= 0) return 1;
    b = (TG_Buf *)roam_ctx_mesh_begin(e->c, sb);
    if (!b) { e->oom = 1; return 0; }
    cmd[0] = (unsigned short)rl_page(pg); cmd[1] = 0; cmd[2] = (unsigned short)q->nq;
    if (!tg_write_prefab_mesh(b, q->v, q->light, q->nq * 4, cmd, 1, 0, 0.0, 0.0, 0.0, 1.0, 0.0) || b->oom) { e->oom = 1; return 0; }
    e->st->meshes++;
    q->nq = 0;
    return 1;
}

/* the guard audit of one face about to be written: its XZ footprint (a wall projects onto a segment)
 * against the drivable primitives within [ymin - LO, ymax + HI] */
static int rl_face_on_road(RlEm *e, const double *x, const double *y, const double *z)
{
    double fx[4], fz[4], ymin = y[0], ymax = y[0];
    int m = 0, k, a;
    for (k = 0; k < 4; k++) {
        if (y[k] < ymin) ymin = y[k];
        if (y[k] > ymax) ymax = y[k];
        for (a = 0; a < m; a++) if (fabs(fx[a] - x[k]) < 1.0 && fabs(fz[a] - z[k]) < 1.0) break;
        if (a == m) { fx[m] = x[k]; fz[m] = z[k]; m++; }
    }
    if (m >= 3) {
        double ar = 0.0;
        int q;
        for (q = 0; q < m; q++) { const int q2 = (q + 1) % m; ar += fx[q] * fz[q2] - fx[q2] * fz[q]; }
        if (fabs(0.5 * ar) < 4.0) {          /* collinear: the two farthest points */
            int i0 = 0, i1 = 1, i, j;
            double bd = -1.0;
            for (i = 0; i < m; i++) for (j = i + 1; j < m; j++) {
                const double d = hypot(fx[i] - fx[j], fz[i] - fz[j]);
                if (d > bd) { bd = d; i0 = i; i1 = j; }
            }
            fx[0] = fx[i0]; fz[0] = fz[i0]; fx[1] = fx[i1]; fz[1] = fz[i1];
            m = 2;
        }
    }
    if (m < 2) return 0;
    return roam_ctx_on_road_band(e->c, fx, fz, m, ymin - RL_GUARD_LO, ymax + RL_GUARD_HI);
}

/* one quad (tile-local x/z, absolute y); flush = a rail laid on a road (not audited) */
static int rl_quad(RlEm *e, int sb, int pg, const double *x, const double *y, const double *z,
                   const double *u, const double *v, unsigned int col, int flush)
{
    RlBatch *q = &e->bat[sb * RP_N + pg];
    int k;
    if (!flush && rl_face_on_road(e, x, y, z)) {
        e->st->guard_hits++;
        if (e->st->guard_hits <= 3)
            TD5_LOG_W(LOG_TAG, "ROAM_RAIL_GUARD page %d face at local (%.0f,%.0f) y %.0f on the drivable surface",
                      rl_page(pg), x[0], z[0], y[0]);
    }
    if (q->nq >= RL_CHUNK && !rl_flush(e, sb, pg)) return 0;
    for (k = 0; k < 4; k++) {
        const int o = (q->nq * 4 + k) * 5;
        q->v[o + 0] = (float)x[k]; q->v[o + 1] = (float)y[k]; q->v[o + 2] = (float)z[k];
        q->v[o + 3] = (float)u[k]; q->v[o + 4] = (float)v[k];
        q->light[q->nq * 4 + k] = col;
    }
    q->nq++;
    return 1;
}

/* a face that may be left out: a sleeper end or a retaining wall that would touch the road is dropped
 * (counted in *cut), the piece around it stays */
static int rl_quad_cut(RlEm *e, int sb, int pg, const double *x, const double *y, const double *z,
                       const double *u, const double *v, unsigned int col, int *cut)
{
    if (rl_face_on_road(e, x, y, z)) { (*cut)++; return 1; }
    return rl_quad(e, sb, pg, x, y, z, u, v, col, 0);
}

/* a ribbon quad between two cross sections A, B: lateral offsets oa0..oa1 at A, ob0..ob1 at B (along the
 * section normals na / nb), heights ya0 ya1 yb0 yb1 */
typedef struct { double x, z, nx, nz, tx, tz, s, head, ground; } RlSec;

static int rl_band(RlEm *e, int sb, int pg, const RlSec *A, const RlSec *B, double o0, double o1,
                   double ya0, double ya1, double yb0, double yb1, double ustep, unsigned int col, int flush)
{
    double x[4], y[4], z[4], u[4], v[4];
    x[0] = A->x + A->nx * o0; z[0] = A->z + A->nz * o0; y[0] = ya0;
    x[1] = A->x + A->nx * o1; z[1] = A->z + A->nz * o1; y[1] = ya1;
    x[2] = B->x + B->nx * o1; z[2] = B->z + B->nz * o1; y[2] = yb1;
    x[3] = B->x + B->nx * o0; z[3] = B->z + B->nz * o0; y[3] = yb0;
    u[0] = u[3] = o0 / ustep; u[1] = u[2] = o1 / ustep;
    v[0] = v[1] = A->s / RL_M(2.0); v[2] = v[3] = B->s / RL_M(2.0);
    return rl_quad(e, sb, pg, x, y, z, u, v, col, flush);
}

/* ============================================================================
 * SECTION: track
 * ========================================================================== */

/* the cross section at along-distance s on segment seg (t in [0,1]); at a vertex the normal is the miter */
static void rl_section(RlEm *e, const RlWay *w, int seg, double t, RlSec *o)
{
    const TD5_RoamRailSource *r = e->r;
    const int i = w->p0 + seg;
    const double ax = r->px[i], az = r->pz[i], bx = r->px[i + 1], bz = r->pz[i + 1];
    const double dx = bx - ax, dz = bz - az, l = hypot(dx, dz);
    double tx = l > 1e-9 ? dx / l : 1.0, tz = l > 1e-9 ? dz / l : 0.0;
    double nx = -tz, nz = tx;                 /* right of travel in the engine frame (td5_geo_frame.h) */
    int j = -1;
    if (t <= 0.0 && seg > 0) j = seg - 1;
    else if (t >= 1.0 && seg + 2 < w->n) j = seg + 1;
    if (j >= 0) {
        /* the miter at the shared vertex: the bisector of both segments' normals, scaled by
         * 1 / cos(half the bend) so the offset edges meet (capped at 2x for a hairpin) */
        const int q = w->p0 + j;
        const double ex = r->px[q + 1] - r->px[q], ez = r->pz[q + 1] - r->pz[q], el = hypot(ex, ez);
        if (el > 1e-9) {
            const double mx = nx + (-ez / el), mz = nz + (ex / el), ml = hypot(mx, mz);
            if (ml > 1e-6) {
                const double ux = mx / ml, uz = mz / ml, ch = ux * nx + uz * nz;
                const double sc = ch > 0.5 ? 1.0 / ch : 2.0;
                nx = ux * sc; nz = uz * sc;
            }
        }
    }
    o->x = ax + dx * t - (double)e->ox;
    o->z = az + dz * t - (double)e->oz;
    o->nx = nx; o->nz = nz;
    o->tx = tx; o->tz = tz;
    o->s = r->ps[i] + t * l;
}

/* footprint quad of the section pair at half width hw */
static void rl_foot(const RlSec *A, const RlSec *B, double hw, double *x, double *z)
{
    x[0] = A->x - A->nx * hw; z[0] = A->z - A->nz * hw;
    x[1] = A->x + A->nx * hw; z[1] = A->z + A->nz * hw;
    x[2] = B->x + B->nx * hw; z[2] = B->z + B->nz * hw;
    x[3] = B->x - B->nx * hw; z[3] = B->z - B->nz * hw;
}

/* road surface height at a tile-local point near ref, else fall back */
static double rl_road_or(RlEm *e, double x, double z, double ref, double fallback)
{
    double y;
    return roam_ctx_road_y(e->c, x, z, ref, &y) ? y + RL_FLUSH : fallback;
}

static int rl_rails(RlEm *e, int sb, const RlWay *w, const RlSec *A, const RlSec *B, int flush)
{
    const double g = 0.5 * w->gauge + RL_RAIL_HW;
    const unsigned int ct = w->kind == RK_DISUSED ? RC_RUST_TOP : RC_RAIL_TOP;
    const unsigned int cs = w->kind == RK_DISUSED ? RC_RUST_SIDE : RC_RAIL_SIDE;
    int sd;
    for (sd = -1; sd <= 1; sd += 2) {
        const double o0 = sd * g - RL_RAIL_HW, o1 = sd * g + RL_RAIL_HW;
        if (flush) {
            double x[4], y[4], z[4], u[4], v[4];
            int k;
            x[0] = A->x + A->nx * o0; z[0] = A->z + A->nz * o0;
            x[1] = A->x + A->nx * o1; z[1] = A->z + A->nz * o1;
            x[2] = B->x + B->nx * o1; z[2] = B->z + B->nz * o1;
            x[3] = B->x + B->nx * o0; z[3] = B->z + B->nz * o0;
            for (k = 0; k < 4; k++) y[k] = rl_road_or(e, x[k], z[k], k < 2 ? A->head : B->head, k < 2 ? A->head : B->head);
            u[0] = u[3] = 0.0; u[1] = u[2] = 1.0;
            v[0] = v[1] = A->s / RL_M(2.0); v[2] = v[3] = B->s / RL_M(2.0);
            if (!rl_quad(e, sb, RP_RAIL, x, y, z, u, v, ct, 1)) return 0;
            continue;
        }
        if (!rl_band(e, sb, RP_RAIL, A, B, o0, o1, A->head, A->head, B->head, B->head, RL_M(0.10), ct, 0)) return 0;
        if (!rl_band(e, sb, RP_RAIL, A, B, o0, o0, A->head - RL_RAIL_H, A->head, B->head - RL_RAIL_H, B->head, RL_M(0.10), cs, 0)) return 0;
        if (!rl_band(e, sb, RP_RAIL, A, B, o1, o1, A->head - RL_RAIL_H, A->head, B->head - RL_RAIL_H, B->head, RL_M(0.10), cs, 0)) return 0;
    }
    return 1;
}

static int rl_sleepers(RlEm *e, int sb, const RlSec *A, const RlSec *B, double hl)
{
    const double ds = B->s - A->s;
    double s = ceil(A->s / RL_SLEEPER_STEP) * RL_SLEEPER_STEP;
    double tx = B->x - A->x, tz = B->z - A->z;
    const double tl = hypot(tx, tz);
    if (!(ds > 1.0) || !(tl > 1.0)) return 1;
    tx /= tl; tz /= tl;
    for (; s < B->s; s += RL_SLEEPER_STEP) {
        const double f = (s - A->s) / ds;
        const double cx = A->x + (B->x - A->x) * f, cz = A->z + (B->z - A->z) * f;
        const double y = A->head + (B->head - A->head) * f - RL_RAIL_H;
        const double nx = -tz, nz = tx;
        double x[4], yy[4], z[4], u[4], v[4];
        x[0] = cx - nx * hl - tx * RL_SLEEPER_HW; z[0] = cz - nz * hl - tz * RL_SLEEPER_HW;
        x[1] = cx + nx * hl - tx * RL_SLEEPER_HW; z[1] = cz + nz * hl - tz * RL_SLEEPER_HW;
        x[2] = cx + nx * hl + tx * RL_SLEEPER_HW; z[2] = cz + nz * hl + tz * RL_SLEEPER_HW;
        x[3] = cx - nx * hl + tx * RL_SLEEPER_HW; z[3] = cz - nz * hl + tz * RL_SLEEPER_HW;
        yy[0] = yy[1] = yy[2] = yy[3] = y;
        u[0] = u[3] = 0.0; u[1] = u[2] = 1.0;
        v[0] = v[1] = 0.0; v[2] = v[3] = 0.25;
        {
            const int before = e->st->sleeper_cut;
            if (!rl_quad_cut(e, sb, RP_SLEEPER, x, yy, z, u, v, RC_SLEEPER, &e->st->sleeper_cut)) return 0;
            if (e->st->sleeper_cut == before) e->st->sleepers++;
        }
    }
    return 1;
}

/* the bridge deck, parapets and piers of one piece */
static int rl_deck(RlEm *e, int sb, const RlWay *w, const RlSec *A, const RlSec *B)
{
    const double at = A->head - RL_DECK_TOP, ab = A->head - RL_DECK_BOT;
    const double bt = B->head - RL_DECK_TOP, bb = B->head - RL_DECK_BOT;
    double s;
    if (!rl_band(e, sb, RP_CONC, A, B, -RL_DECK_HW, RL_DECK_HW, at, at, bt, bt, RL_M(2.0), RC_WHITE, 0)) return 0;
    if (!rl_band(e, sb, RP_CONC, A, B, -RL_DECK_HW, RL_DECK_HW, ab, ab, bb, bb, RL_M(2.0), RC_WHITE, 0)) return 0;
    if (!rl_band(e, sb, RP_CONC, A, B, -RL_DECK_HW, -RL_DECK_HW, ab, at + RL_PARAPET, bb, bt + RL_PARAPET, RL_M(2.0), RC_WHITE, 0)) return 0;
    if (!rl_band(e, sb, RP_CONC, A, B, RL_DECK_HW, RL_DECK_HW, ab, at + RL_PARAPET, bb, bt + RL_PARAPET, RL_M(2.0), RC_WHITE, 0)) return 0;
    /* piers on the way's own 15 m lattice, off the ends (abutments) and off the road */
    for (s = ceil(A->s / RL_PIER_STEP) * RL_PIER_STEP; s < B->s; s += RL_PIER_STEP) {
        const double f = (B->s - A->s) > 1.0 ? (s - A->s) / (B->s - A->s) : 0.0;
        double cx, cz, tx, tz, tl, nx, nz, g, top, fx[4], fz[4];
        int k;
        if (s < RL_PIER_END || s > w->len - RL_PIER_END) continue;
        cx = A->x + (B->x - A->x) * f; cz = A->z + (B->z - A->z) * f;
        tx = B->x - A->x; tz = B->z - A->z; tl = hypot(tx, tz);
        if (!(tl > 1.0)) continue;
        tx /= tl; tz /= tl; nx = -tz; nz = tx;
        g = roam_ctx_ground(e->c, cx, cz);
        top = ab + (bb - ab) * f;
        if (top - g < RL_M(0.30)) continue;
        fx[0] = cx - tx * RL_PIER_HA - nx * RL_PIER_HC; fz[0] = cz - tz * RL_PIER_HA - nz * RL_PIER_HC;
        fx[1] = cx + tx * RL_PIER_HA - nx * RL_PIER_HC; fz[1] = cz + tz * RL_PIER_HA - nz * RL_PIER_HC;
        fx[2] = cx + tx * RL_PIER_HA + nx * RL_PIER_HC; fz[2] = cz + tz * RL_PIER_HA + nz * RL_PIER_HC;
        fx[3] = cx - tx * RL_PIER_HA + nx * RL_PIER_HC; fz[3] = cz - tz * RL_PIER_HA + nz * RL_PIER_HC;
        if (roam_ctx_on_road_band(e->c, fx, fz, 4, g - RL_M(2.0), top)) continue;   /* a road passes here */
        for (k = 0; k < 4; k++) {
            const int k2 = (k + 1) & 3;
            double x[4], y[4], z[4], u[4], v[4];
            x[0] = fx[k]; z[0] = fz[k]; y[0] = g - RL_M(0.3);
            x[1] = fx[k2]; z[1] = fz[k2]; y[1] = g - RL_M(0.3);
            x[2] = fx[k2]; z[2] = fz[k2]; y[2] = top;
            x[3] = fx[k]; z[3] = fz[k]; y[3] = top;
            u[0] = u[3] = 0.0; u[1] = u[2] = 1.0;
            v[0] = v[1] = 0.0; v[2] = v[3] = (top - g) / RL_M(2.0);
            if (!rl_quad(e, sb, RP_CONC, x, y, z, u, v, RC_WHITE, 0)) return 0;
        }
        e->st->piers++;
    }
    return 1;
}

/* [Avenida 1] a section a little inside a PARALLEL street (roam draws carriageways wider than the
 * real ones) is moved off it, at most RL_NUDGE (1.5 m: a 4 m track pair keeps 2.5 m between centres,
 * room for two squeezed beds): the street's own outward normal, so both ends of a
 * piece and both tiles at a seam move the same way. Never near a level crossing (a crossing street's
 * "lateral" runs along the rail). 1 when moved. */
static int rl_nudge(RlEm *e, RlSec *S, double strip_hw)
{
    int ed = -1, sd = 0;
    double al = 0.0, lat = 0.0, need;
    RoamFrame fr;
    if (!roam_nearest_edge(e->c, S->x, S->z, RL_M(20.0), &ed, &sd, &al, &lat)) return 0;
    if (!roam_edge_frame(e->c, ed, sd, al, &fr)) return 0;
    if (fabs(fr.tx * S->tx + fr.tz * S->tz) < 0.9) return 0;         /* not parallel (> ~25 deg) */
    need = fr.half_carriage + strip_hw + RL_M(0.05) - fabs(lat);
    if (!(need > 0.0) || need > RL_NUDGE) return 0;
    S->x += fr.nx * need; S->z += fr.nz * need;
    return 1;
}

/* a level crossing of the data within d of along-distance s */
static int rl_near_grade(const TD5_RoamRailSource *r, const RlWay *w, double s, double d)
{
    int k;
    for (k = 0; k < w->nx; k++) if (r->x[w->x0 + k].kind == RX_GRADE && fabs(r->x[w->x0 + k].s - s) < d) return 1;
    return 0;
}

/* one owned piece [A, B] of way w */
static int rl_piece(RlEm *e, const RlWay *w, RlSec *A, RlSec *B, double lift_over, int blended)
{
    TG_TileCtx *c = e->c;
    const double sleeper_hw = 0.5 * w->gauge + RL_SLEEPER_OUT;
    const double crest_hw = sleeper_hw + RL_CREST_OUT;
    const double strip_hw = 0.5 * w->gauge + 2.0 * RL_RAIL_HW + RL_M(0.03);   /* the rails' outer faces */
    const double ylo = (A->head < B->head ? A->head : B->head) - RL_HEAD - RL_M(0.5);
    const double yhi = (A->head > B->head ? A->head : B->head) + RL_GUARD_HI;
    const double mx = 0.5 * (A->x + B->x), mz = 0.5 * (A->z + B->z);
    const int sb = roam_ctx_sb_of(c, mx, mz);
    const unsigned int cb = w->kind == RK_DISUSED ? RC_BALLAST_D : RC_BALLAST;
    double fx[4], fz[4];
    double acr = A->head - RL_CREST_DY, bcr = B->head - RL_CREST_DY;
    double ag, bg, adrop, bdrop, atoe, btoe;
    int mode, is_deck = 0;          /* mode 0 full, 1 narrow, 2 flush, 3 squeezed (rails + short sleepers) */
    A->ground = ag = roam_ctx_ground(c, A->x, A->z);
    B->ground = bg = roam_ctx_ground(c, B->x, B->z);
    adrop = acr - ag; bdrop = bcr - bg;
    atoe = crest_hw + (adrop > 0.0 ? adrop * RL_SLOPE : 0.0);
    btoe = crest_hw + (bdrop > 0.0 ? bdrop * RL_SLOPE : 0.0);
    if (atoe < crest_hw + RL_TOE_MIN) atoe = crest_hw + RL_TOE_MIN;
    if (btoe < crest_hw + RL_TOE_MIN) btoe = crest_hw + RL_TOE_MIN;
    if (atoe > crest_hw + RL_TOE_MAX) atoe = crest_hw + RL_TOE_MAX;
    if (btoe > crest_hw + RL_TOE_MAX) btoe = crest_hw + RL_TOE_MAX;
    /* the mode: a deck needs its own footprint clear in the band; plain track the strip / crest / toe */
    if (w->bridge) {
        rl_foot(A, B, RL_DECK_HW, fx, fz);
        if (!roam_ctx_on_road_band(c, fx, fz, 4, ylo - RL_DECK_BOT, yhi)) is_deck = 1;
    }
    rl_foot(A, B, strip_hw, fx, fz);
    if (!is_deck && roam_ctx_on_road_band(c, fx, fz, 4, ylo, yhi)) mode = 2;
    else {
        rl_foot(A, B, crest_hw, fx, fz);
        if (!is_deck && roam_ctx_on_road_band(c, fx, fz, 4, ylo, yhi)) mode = 3;   /* a tight median: squeezed */
        else {
            double tx[4], tz[4];
            tx[0] = A->x - A->nx * atoe; tz[0] = A->z - A->nz * atoe;
            tx[1] = A->x + A->nx * atoe; tz[1] = A->z + A->nz * atoe;
            tx[2] = B->x + B->nx * btoe; tz[2] = B->z + B->nz * btoe;
            tx[3] = B->x - B->nx * btoe; tz[3] = B->z - B->nz * btoe;
            mode = (!is_deck && roam_ctx_on_road_band(c, tx, tz, 4, ylo - RL_TOE_MAX, yhi)) ? 1 : 0;
        }
    }
    {   /* under a road deck: the piece overlaps a drivable primitive above the band (not a conflict) */
        rl_foot(A, B, crest_hw, fx, fz);
        if (mode != 2 && roam_ctx_on_road_band(c, fx, fz, 4, yhi, 1e12)) e->st->under_deck++;
    }
    if (e->log && mode != 2) {   /* any drivable primitive under the crest at ANY height: band or a hole? */
        double ry = 0.0;
        rl_foot(A, B, crest_hw, fx, fz);
        if (roam_ctx_on_road_band(c, fx, fz, 4, -1e12, 1e12))
            TD5_LOG_I(LOG_TAG, "ROAM_RAIL_ANYY way %lld mode %d deck %d at place (%.0f,%.0f): head %.0f band %.0f..%.0f road_y %s%.0f",
                      (long long)w->id, mode, is_deck, mx + (double)e->ox, mz + (double)e->oz, 0.5 * (A->head + B->head), ylo, yhi,
                      roam_ctx_road_y(c, mx, mz, A->head, &ry) ? "" : "(none at the centre) ", ry);
    }
    if (e->log && mode != 2) {
        double sy = 0.0, ly = 0.0, kerb = -1e30;
        int ed = -1, sd = 0;
        double al = 0.0, lat = 0.0;
        RoamFrame fr;
        const int hs = roam_ctx_slab_y(c, mx, mz, &sy), hl = roam_ctx_lawn_y(c, mx, mz, &ly);
        if (roam_nearest_edge(c, mx, mz, RL_M(20.0), &ed, &sd, &al, &lat) && roam_edge_frame(c, ed, sd, al, &fr)) kerb = fr.kerb_y;
        if (hs || hl)
            TD5_LOG_I(LOG_TAG, "ROAM_RAIL_FLOOR way %lld mode %d at place (%.0f,%.0f): head %.0f crest %.0f ground %.0f | slab %s%.0f "
                      "lawn %s%.0f | street lat %.1f m kerb_y %.0f", (long long)w->id, mode, mx + (double)e->ox, mz + (double)e->oz,
                      0.5 * (A->head + B->head), 0.5 * (acr + bcr), 0.5 * (ag + bg), hs ? "" : "-", hs ? sy : 0.0, hl ? "" : "-",
                      hl ? ly : 0.0, lat / RL_UPM, kerb);
    }
    e->st->pieces++;
    if (blended) e->st->blend++;
    if (lift_over > RL_M(0.05)) {
        e->st->lifted++;
        if (lift_over / RL_UPM > e->st->lift_max_m) e->st->lift_max_m = lift_over / RL_UPM;
    }
    if (mode == 2) {
        e->st->flush++;
        if (!blended) {
            e->st->flush_nodata++;
            if (e->log) {
                int ed = -1, sd = 0;
                double al = 0.0, lat = 0.0;
                const int ne = roam_nearest_edge(c, mx, mz, RL_M(30.0), &ed, &sd, &al, &lat);
                RoamFrame fr;
                const int okf = ne && roam_edge_frame(c, ed, sd, al, &fr);
                TD5_LOG_I(LOG_TAG, "ROAM_RAIL_FLUSH_NODATA way %lld kind %d at place (%.0f,%.0f) east %.0f: nearest street lateral %.1f m "
                          "half carriage %.1f m class %d", (long long)w->id, w->kind, mx + (double)e->ox, mz + (double)e->oz,
                          -(mx + (double)e->ox), ne ? lat / RL_UPM : -1.0, okf ? fr.half_carriage / RL_UPM : -1.0, okf ? fr.klass : -1);
            }
        }
        return rl_rails(e, sb, w, A, B, 1);
    }
    if (mode == 3) {
        /* the rails clear the road but the bed does not (Avenida 1: the Roca in a median that roam's
         * carriageways leave ~6 m of): ballast and sleepers cut back to just past the rails */
        const double hw = strip_hw - RL_M(0.01);
        double x[4], y[4], z[4], u[4], v[4];
        e->st->squeezed++;
        rl_foot(A, B, hw, x, z);
        y[0] = y[1] = acr; y[2] = y[3] = bcr;
        u[0] = u[3] = 0.0; u[1] = u[2] = 2.0 * hw / RL_M(2.0);
        v[0] = v[1] = A->s / RL_M(2.0); v[2] = v[3] = B->s / RL_M(2.0);
        if (!rl_quad_cut(e, sb, RP_BALLAST, x, y, z, u, v, cb, &e->st->sleeper_cut)) return 0;
        if (!rl_sleepers(e, sb, A, B, hw)) return 0;
        return rl_rails(e, sb, w, A, B, 0);
    }
    if (is_deck) {
        e->st->bridge++;
        if (!rl_deck(e, sb, w, A, B)) return 0;
        if (!rl_band(e, sb, RP_BALLAST, A, B, -crest_hw, crest_hw, acr, acr, bcr, bcr, RL_M(2.0), cb, 0)) return 0;
    } else {
        if (mode == 0) e->st->full++; else e->st->narrow++;
        if (!rl_band(e, sb, RP_BALLAST, A, B, -crest_hw, crest_hw, acr, acr, bcr, bcr, RL_M(2.0), cb, 0)) return 0;
        if (mode == 0) {
            /* shoulders: ballast down to the drawn ground; an embankment (crest > 1 m up) in earth */
            const int pg = (adrop > RL_M(RL_EMBANK_M) || bdrop > RL_M(RL_EMBANK_M)) ? RP_EARTH : RP_BALLAST;
            const unsigned int col = pg == RP_EARTH ? RC_WHITE : cb;
            double gl0 = roam_ctx_ground(c, A->x - A->nx * atoe, A->z - A->nz * atoe);
            double gl1 = roam_ctx_ground(c, B->x - B->nx * btoe, B->z - B->nz * btoe);
            double gr0 = roam_ctx_ground(c, A->x + A->nx * atoe, A->z + A->nz * atoe);
            double gr1 = roam_ctx_ground(c, B->x + B->nx * btoe, B->z + B->nz * btoe);
            /* the toe sections at their own offsets: a band with per-end widths */
            {
                double x[4], y[4], z[4], u[4], v[4];
                x[0] = A->x - A->nx * atoe; z[0] = A->z - A->nz * atoe; y[0] = gl0 - RL_M(0.05);
                x[1] = A->x - A->nx * crest_hw; z[1] = A->z - A->nz * crest_hw; y[1] = acr;
                x[2] = B->x - B->nx * crest_hw; z[2] = B->z - B->nz * crest_hw; y[2] = bcr;
                x[3] = B->x - B->nx * btoe; z[3] = B->z - B->nz * btoe; y[3] = gl1 - RL_M(0.05);
                u[0] = u[3] = 0.0; u[1] = u[2] = (atoe - crest_hw) / RL_M(2.0);
                v[0] = v[1] = A->s / RL_M(2.0); v[2] = v[3] = B->s / RL_M(2.0);
                if (!rl_quad(e, sb, pg, x, y, z, u, v, col, 0)) return 0;
                x[0] = A->x + A->nx * crest_hw; z[0] = A->z + A->nz * crest_hw; y[0] = acr;
                x[1] = A->x + A->nx * atoe; z[1] = A->z + A->nz * atoe; y[1] = gr0 - RL_M(0.05);
                x[2] = B->x + B->nx * btoe; z[2] = B->z + B->nz * btoe; y[2] = gr1 - RL_M(0.05);
                x[3] = B->x + B->nx * crest_hw; z[3] = B->z + B->nz * crest_hw; y[3] = bcr;
                if (!rl_quad(e, sb, pg, x, y, z, u, v, col, 0)) return 0;
            }
        } else if (adrop > RL_M(0.30) || bdrop > RL_M(0.30)) {
            /* narrow and raised (a road beside the ramp): retaining walls down to the ground, each one
             * left out where it would stand on the road */
            int sd;
            for (sd = -1; sd <= 1; sd += 2) {
                double x[4], y[4], z[4], u[4], v[4];
                const double o = sd * crest_hw;
                x[0] = A->x + A->nx * o; z[0] = A->z + A->nz * o; y[0] = ag - RL_M(0.1);
                x[1] = A->x + A->nx * o; z[1] = A->z + A->nz * o; y[1] = acr;
                x[2] = B->x + B->nx * o; z[2] = B->z + B->nz * o; y[2] = bcr;
                x[3] = B->x + B->nx * o; z[3] = B->z + B->nz * o; y[3] = bg - RL_M(0.1);
                u[0] = u[3] = A->s / RL_M(2.0); u[1] = u[2] = B->s / RL_M(2.0);
                v[0] = v[3] = 0.0; v[1] = v[2] = 0.5;
                if (!rl_quad_cut(e, sb, RP_CONC, x, y, z, u, v, RC_WHITE, &e->st->wall_cut)) return 0;
            }
        }
    }
    if (!rl_sleepers(e, sb, A, B, sleeper_hw)) return 0;
    return rl_rails(e, sb, w, A, B, 0);
}

/* ============================================================================
 * SECTION: platforms
 * ========================================================================== */

static int rl_plat_tri(RlEm *e, int sb, const double *x, const double *z, double top)
{
    double qx[4], qy[4], qz[4], u[4], v[4];
    int k;
    if (roam_ctx_on_road_band(e->c, x, z, 3, top - RL_M(3.0), top + RL_GUARD_HI)) { e->st->plat_drop++; return 1; }
    for (k = 0; k < 4; k++) {
        const int s = k < 3 ? k : 2;          /* a degenerate quad: the triangle */
        qx[k] = x[s]; qz[k] = z[s]; qy[k] = top;
        u[k] = (x[s] + (double)e->ox) / RL_M(2.0);
        v[k] = (z[s] + (double)e->oz) / RL_M(2.0);
    }
    return rl_quad(e, sb, RP_PLAT, qx, qy, qz, u, v, RC_WHITE, 0);
}

static int rl_plat_wall(RlEm *e, int sb, double x0, double z0, double x1, double z1, double top)
{
    double x[4], y[4], z[4], u[4], v[4], sx[2], sz[2];
    const double g0 = roam_ctx_ground(e->c, x0, z0), g1 = roam_ctx_ground(e->c, x1, z1);
    sx[0] = x0; sz[0] = z0; sx[1] = x1; sz[1] = z1;
    if (roam_ctx_on_road_band(e->c, sx, sz, 2, (g0 < g1 ? g0 : g1) - RL_GUARD_LO, top + RL_GUARD_HI)) { e->st->plat_drop++; return 1; }
    x[0] = x0; z[0] = z0; y[0] = g0 - RL_M(0.1);
    x[1] = x1; z[1] = z1; y[1] = g1 - RL_M(0.1);
    x[2] = x1; z[2] = z1; y[2] = top;
    x[3] = x0; z[3] = z0; y[3] = top;
    u[0] = u[3] = 0.0; u[1] = u[2] = hypot(x1 - x0, z1 - z0) / RL_M(2.0);
    v[0] = v[1] = 0.0; v[2] = v[3] = 0.5;
    return rl_quad(e, sb, RP_CONC, x, y, z, u, v, RC_WHITE, 0);
}

static double rl_cross3(double ax, double az, double bx, double bz, double cx, double cz)
{
    return (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
}

/* a closed platform polygon: owned by its vertex mean, ear-clipped top + side walls */
static int rl_plat_poly(RlEm *e, const RlWay *w)
{
    const TD5_RoamRailSource *r = e->r;
    double x[RL_PLAT_RING], z[RL_PLAT_RING], cx = 0.0, cz = 0.0, area = 0.0, top;
    int idx[RL_PLAT_RING];
    int n = w->n, i, m, guard, sb;
    if (n > 1 && r->px[w->p0] == r->px[w->p0 + n - 1] && r->pz[w->p0] == r->pz[w->p0 + n - 1]) n--;
    if (n < 3 || n > RL_PLAT_RING) return 1;
    for (i = 0; i < n; i++) {
        x[i] = r->px[w->p0 + i] - (double)e->ox; z[i] = r->pz[w->p0 + i] - (double)e->oz;
        cx += x[i]; cz += z[i];
    }
    cx /= n; cz /= n;
    if (!roam_ctx_owned(e->c, cx, cz)) return 1;
    sb = roam_ctx_sb_of(e->c, cx, cz);
    top = rl_base_ground(e, cx, cz) + RL_HEAD + RL_PLAT_TOP;
    for (i = 0; i < n; i++) { const int j = (i + 1) % n; area += x[i] * z[j] - x[j] * z[i]; }
    for (i = 0; i < n; i++) idx[i] = area > 0.0 ? i : n - 1 - i;     /* counter-clockwise in (x, z) */
    e->st->platforms++;
    m = n;
    guard = 4 * n * n;
    while (m > 3 && guard-- > 0) {
        int found = 0;
        for (i = 0; i < m && !found; i++) {
            const int a = idx[(i + m - 1) % m], b = idx[i], cc = idx[(i + 1) % m];
            int k, inside = 0;
            if (rl_cross3(x[a], z[a], x[b], z[b], x[cc], z[cc]) <= 0.0) continue;   /* reflex */
            for (k = 0; k < m && !inside; k++) {
                const int p = idx[k];
                if (p == a || p == b || p == cc) continue;
                if (rl_cross3(x[a], z[a], x[b], z[b], x[p], z[p]) >= 0.0 &&
                    rl_cross3(x[b], z[b], x[cc], z[cc], x[p], z[p]) >= 0.0 &&
                    rl_cross3(x[cc], z[cc], x[a], z[a], x[p], z[p]) >= 0.0) inside = 1;
            }
            if (inside) continue;
            {
                double tx[3], tz[3];
                tx[0] = x[a]; tz[0] = z[a]; tx[1] = x[b]; tz[1] = z[b]; tx[2] = x[cc]; tz[2] = z[cc];
                if (!rl_plat_tri(e, sb, tx, tz, top)) return 0;
            }
            for (k = i; k + 1 < m; k++) idx[k] = idx[k + 1];
            m--;
            found = 1;
        }
        if (!found) break;                       /* self-intersecting ring: stop, the rest is left open */
    }
    if (m == 3) {
        double tx[3], tz[3];
        for (i = 0; i < 3; i++) { tx[i] = x[idx[i]]; tz[i] = z[idx[i]]; }
        if (!rl_plat_tri(e, sb, tx, tz, top)) return 0;
    }
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        if (!rl_plat_wall(e, sb, x[i], z[i], x[j], z[j], top)) return 0;
    }
    return 1;
}

/* a platform mapped as a LINE: a 4 m wide slab along it, in owned pieces */
static int rl_plat_line(RlEm *e, const RlWay *w)
{
    const TD5_RoamRailSource *r = e->r;
    int seg;
    for (seg = 0; seg + 1 < w->n; seg++) {
        const double l = r->ps[w->p0 + seg + 1] - r->ps[w->p0 + seg];
        const int np = (int)ceil(l / RL_PIECE);
        int j;
        if (!(l > 1.0)) continue;
        for (j = 0; j < np; j++) {
            RlSec A, B;
            double fx[4], fz[4], ta, tb;
            int sb, k;
            rl_section(e, w, seg, (double)j / np, &A);
            rl_section(e, w, seg, (double)(j + 1) / np, &B);
            if (!roam_ctx_owned(e->c, 0.5 * (A.x + B.x), 0.5 * (A.z + B.z))) continue;
            sb = roam_ctx_sb_of(e->c, 0.5 * (A.x + B.x), 0.5 * (A.z + B.z));
            ta = rl_base_ground(e, A.x, A.z) + RL_HEAD + RL_PLAT_TOP;
            tb = rl_base_ground(e, B.x, B.z) + RL_HEAD + RL_PLAT_TOP;
            rl_foot(&A, &B, RL_PLAT_HW, fx, fz);
            if (roam_ctx_on_road_band(e->c, fx, fz, 4, (ta < tb ? ta : tb) - RL_M(3.0), (ta > tb ? ta : tb) + RL_GUARD_HI)) {
                e->st->plat_drop++;
                continue;
            }
            {
                double y[4], u[4], v[4];
                y[0] = y[1] = ta; y[2] = y[3] = tb;
                for (k = 0; k < 4; k++) { u[k] = (fx[k] + (double)e->ox) / RL_M(2.0); v[k] = (fz[k] + (double)e->oz) / RL_M(2.0); }
                if (!rl_quad(e, sb, RP_PLAT, fx, y, fz, u, v, RC_WHITE, 0)) return 0;
            }
            if (!rl_plat_wall(e, sb, fx[0], fz[0], fx[3], fz[3], 0.5 * (ta + tb))) return 0;
            if (!rl_plat_wall(e, sb, fx[1], fz[1], fx[2], fz[2], 0.5 * (ta + tb))) return 0;
            e->st->plat_pieces++;
        }
    }
    return 1;
}

/* ============================================================================
 * SECTION: furniture  [roam LF]
 * Level-crossing equipment, station name boards and catenary. Everything is VISUAL (no collision) and is
 * written as double-sided quads (a tinted concrete page; the station board's own atlas cell). Every position
 * is a pure function of the data and the place frame, so two tiles at a seam draw the same piece.
 * ========================================================================== */

#define RL_LX_STOP      RL_M(4.2)      /* the barrier line: this far from the track axis, square to it           */
#define RL_LX_POST_OFF  RL_M(1.3)      /* post centre beyond the carriageway edge                                */
#define RL_LX_HOUSE_H   RL_M(1.05)     /* barrier housing height = arm pivot                                     */
#define RL_LX_ARM_ANG   80.0           /* raised arm angle over the horizontal (open barrier)                    */
#define RL_LX_ARM_HALF  RL_M(5.5)      /* arm length cap, half barrier                                           */
#define RL_LX_ARM_FULL  RL_M(7.5)      /* arm length cap, full barrier                                           */
#define RL_LX_POLE_H    RL_M(3.4)
#define RL_BOARD_W      RL_M(2.4)
#define RL_BOARD_ROW    RL_M(0.6)
#define RL_BOARD_Y      RL_M(1.9)      /* board bottom over the platform                                         */
#define RL_CAT_STEP     RL_M(50.0)     /* mast spacing along the way                                             */
#define RL_CAT_PIECE    RL_M(12.5)     /* wire piece (owned by its midpoint)                                     */
#define RL_CAT_OFF      RL_M(3.3)      /* mast axis from the track axis                                          */
#define RL_CAT_WIRE     RL_M(5.6)      /* contact wire over the rail head                                        */
#define RL_CAT_MESS     RL_M(6.6)      /* messenger wire                                                         */
#define RL_CAT_ARM      RL_M(6.9)      /* cantilever                                                             */
#define RL_CAT_TOP      RL_M(7.4)      /* mast top                                                               */
#define RL_CAT_DECK     RL_M(10.0)     /* no catenary this near a road that passes over the track                */

int td5_roam_rails_furn_enabled(void) { return td5_env_int("TD5RE_ROAM_RAILS_FURN", 1, 0, 1); }

#define RC_STEEL   0xFF8E9096u
#define RC_HOUSE   0xFFD0D0D4u
#define RC_RED     0xFFD02020u
#define RC_PAINT_W 0xFFF4F4F4u
#define RC_LAMP    0xFFFF2828u
#define RC_BLACK   0xFF202024u
#define RC_WIRE    0xFF303034u
#define RC_BOARD   0xFF1650A0u

static const double k_rl_u4[4] = { 0.0, 1.0, 1.0, 0.0 }, k_rl_v4[4] = { 0.0, 0.0, 1.0, 1.0 };
static const double k_rl_up[3] = { 0.0, 1.0, 0.0 };

/* a quad with both windings (thin furniture is seen from either side) */
static int rl_quad2(RlEm *e, int sb, int pg, const double *x, const double *y, const double *z,
                    const double *u, const double *v, unsigned int col)
{
    double rx[4], ry[4], rz[4], ru[4], rv[4];
    int k;
    if (!rl_quad(e, sb, pg, x, y, z, u, v, col, 0)) return 0;
    for (k = 0; k < 4; k++) { rx[k] = x[3 - k]; ry[k] = y[3 - k]; rz[k] = z[3 - k]; ru[k] = u[3 - k]; rv[k] = v[3 - k]; }
    return rl_quad(e, sb, pg, rx, ry, rz, ru, rv, col, 1);
}

/* an oriented box: the axis from p0 to p1, cross section half sizes ha along unit A and hb along unit B */
static int rl_obox(RlEm *e, int sb, const double *p0, const double *p1, const double *A, const double *B,
                   double ha, double hb, unsigned int col)
{
    static const int F[6][4] = { { 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 1, 3, 7, 5 }, { 3, 2, 6, 7 }, { 2, 0, 4, 6 } };
    double c[8][3];
    int i, f, k;
    for (i = 0; i < 8; i++) {
        const double *p = (i & 4) ? p1 : p0;
        const double sa = (i & 1) ? ha : -ha, sbb = (i & 2) ? hb : -hb;
        for (k = 0; k < 3; k++) c[i][k] = p[k] + sa * A[k] + sbb * B[k];
    }
    for (f = 0; f < 6; f++) {
        double x[4], y[4], z[4];
        for (k = 0; k < 4; k++) { x[k] = c[F[f][k]][0]; y[k] = c[F[f][k]][1]; z[k] = c[F[f][k]][2]; }
        if (!rl_quad2(e, sb, RP_CONC, x, y, z, k_rl_u4, k_rl_v4, col)) return 0;
    }
    return 1;
}

/* a flat ribbon p0 -> p1, half width w along unit axis a */
static int rl_ribbon(RlEm *e, int sb, const double *p0, const double *p1, const double *a, double w, unsigned int col)
{
    double x[4], y[4], z[4];
    x[0] = p0[0] - a[0] * w; y[0] = p0[1] - a[1] * w; z[0] = p0[2] - a[2] * w;
    x[1] = p0[0] + a[0] * w; y[1] = p0[1] + a[1] * w; z[1] = p0[2] + a[2] * w;
    x[2] = p1[0] + a[0] * w; y[2] = p1[1] + a[1] * w; z[2] = p1[2] + a[2] * w;
    x[3] = p1[0] - a[0] * w; y[3] = p1[1] - a[1] * w; z[3] = p1[2] - a[2] * w;
    return rl_quad2(e, sb, RP_CONC, x, y, z, k_rl_u4, k_rl_v4, col);
}

/* ---- nearest track of the layer ---- */
typedef struct { const RlWay *w; int seg; double t, d, tx, tz, px, pz; } RlNear;

static int rl_nearest_track(const TD5_RoamRailSource *r, double x, double z, double maxd, RlNear *o)
{
    double best = maxd;
    int i, hit = 0;
    for (i = 0; i < r->nw; i++) {
        const RlWay *w = &r->w[i];
        int seg;
        if (w->kind == RK_PLATFORM || w->tunnel) continue;
        if (x < w->bx0 - maxd || x > w->bx1 + maxd || z < w->bz0 - maxd || z > w->bz1 + maxd) continue;
        for (seg = 0; seg + 1 < w->n; seg++) {
            const double ax = r->px[w->p0 + seg], az = r->pz[w->p0 + seg];
            const double dx = r->px[w->p0 + seg + 1] - ax, dz = r->pz[w->p0 + seg + 1] - az, l2 = dx * dx + dz * dz;
            double t = l2 > 1e-9 ? ((x - ax) * dx + (z - az) * dz) / l2 : 0.0, d;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            d = hypot(x - (ax + dx * t), z - (az + dz * t));
            if (d < best) {
                const double l = sqrt(l2);
                best = d; hit = 1;
                o->w = w; o->seg = seg; o->t = t; o->d = d;
                o->tx = l > 1e-9 ? dx / l : 1.0; o->tz = l > 1e-9 ? dz / l : 0.0;
                o->px = ax + dx * t; o->pz = az + dz * t;
            }
        }
    }
    return hit;
}

static int rl_on_road(RlEm *e, double x, double z, double hs, double ylo, double yhi)
{
    double fx[4], fz[4];
    fx[0] = x - hs; fz[0] = z - hs; fx[1] = x + hs; fz[1] = z - hs; fx[2] = x + hs; fz[2] = z + hs; fx[3] = x - hs; fz[3] = z + hs;
    return roam_ctx_on_road_band(e->c, fx, fz, 4, ylo, yhi);
}

/* ---- level crossings ---- */

static int rl_lx_post(RlEm *e, int sb, double px, double pz, double ky, const double *T, const double *R,
                      const RlNode *n, double arm_len)
{
    const double A[3] = { T[0], 0.0, T[1] }, B[3] = { R[0], 0.0, R[1] };
    double p0[3], p1[3];
    p0[0] = p1[0] = px; p0[2] = p1[2] = pz;
    if (n->barrier) {
        const double ang = RL_LX_ARM_ANG * 3.14159265358979323846 / 180.0, ca = cos(ang), sa = sin(ang);
        double dir[3], bn[3], bl;
        int i;
        p0[1] = ky; p1[1] = ky + RL_LX_HOUSE_H;
        if (!rl_obox(e, sb, p0, p1, A, B, RL_M(0.17), RL_M(0.17), RC_HOUSE)) return 0;
        dir[0] = -R[0] * ca; dir[1] = sa; dir[2] = -R[1] * ca;
        bn[0] = A[1] * dir[2] - A[2] * dir[1]; bn[1] = A[2] * dir[0] - A[0] * dir[2]; bn[2] = A[0] * dir[1] - A[1] * dir[0];
        bl = sqrt(bn[0] * bn[0] + bn[1] * bn[1] + bn[2] * bn[2]);
        if (bl > 1e-9) { bn[0] /= bl; bn[1] /= bl; bn[2] /= bl; }
        for (i = 0; i < 6; i++) {
            double q0[3], q1[3];
            const double f0 = arm_len * i / 6.0, f1 = arm_len * (i + 1) / 6.0;
            q0[0] = px + dir[0] * f0; q0[1] = ky + RL_LX_HOUSE_H + dir[1] * f0; q0[2] = pz + dir[2] * f0;
            q1[0] = px + dir[0] * f1; q1[1] = ky + RL_LX_HOUSE_H + dir[1] * f1; q1[2] = pz + dir[2] * f1;
            if (!rl_obox(e, sb, q0, q1, A, bn, RL_M(0.06), RL_M(0.08), (i & 1) ? RC_PAINT_W : RC_RED)) return 0;
        }
        if (n->barrier == 1) e->st->lx_barrier_half++; else e->st->lx_barrier_full++;
    }
    if (n->light || n->saltire) {
        p0[1] = ky; p1[1] = ky + RL_LX_POLE_H;
        if (!rl_obox(e, sb, p0, p1, A, B, RL_M(0.06), RL_M(0.06), RC_STEEL)) return 0;
    }
    if (n->light) {
        int h;
        for (h = -1; h <= 1; h += 2) {
            const double cx = px + R[0] * RL_M(0.28) * h, cz = pz + R[1] * RL_M(0.28) * h, cy = ky + RL_M(2.3);
            double q0[3], q1[3];
            q0[0] = q1[0] = cx + T[0] * RL_M(0.04); q0[2] = q1[2] = cz + T[1] * RL_M(0.04);
            q0[1] = cy - RL_M(0.22); q1[1] = cy + RL_M(0.22);
            if (!rl_obox(e, sb, q0, q1, A, B, RL_M(0.05), RL_M(0.20), RC_BLACK)) return 0;
            q0[0] = q1[0] = cx + T[0] * RL_M(0.11); q0[2] = q1[2] = cz + T[1] * RL_M(0.11);
            q0[1] = cy - RL_M(0.12); q1[1] = cy + RL_M(0.12);
            if (!rl_obox(e, sb, q0, q1, A, B, RL_M(0.02), RL_M(0.12), RC_LAMP)) return 0;
        }
        e->st->lx_lights++;
    }
    if (n->saltire) {
        const double c45 = 0.70710678118654752, cy = ky + RL_M(3.0);
        int d;
        for (d = -1; d <= 1; d += 2) {
            const double dir[3] = { R[0] * c45 * d, c45, R[1] * c45 * d };
            const double pp[3] = { R[0] * c45, -c45 * d, R[1] * c45 };     /* perpendicular in the plane */
            double q0[3], q1[3];
            int pass;
            for (pass = 0; pass < 2; pass++) {
                const double off = RL_M(0.08) + pass * RL_M(0.035), hl = RL_M(0.62) - pass * RL_M(0.03);
                q0[0] = px + T[0] * off - dir[0] * hl; q0[1] = cy - dir[1] * hl; q0[2] = pz + T[1] * off - dir[2] * hl;
                q1[0] = px + T[0] * off + dir[0] * hl; q1[1] = cy + dir[1] * hl; q1[2] = pz + T[1] * off + dir[2] * hl;
                if (!rl_obox(e, sb, q0, q1, A, pp, RL_M(0.02), pass ? RL_M(0.045) : RL_M(0.085), pass ? RC_RED : RC_PAINT_W)) return 0;
            }
        }
        e->st->lx_saltires++;
    }
    return 1;
}

int td5_roam_rails_untagged_x_enabled(void) { return td5_env_int("TD5RE_ROAM_RAILS_UNTAGGED_X", 1, 0, 1); }

static int rl_lx_node(RlEm *e, const RlNode *n0)
{
    TG_TileCtx *c = e->c;
    RlNode nx;
    const RlNode *n = n0;
    const int untagged = !n0->barrier && !n0->light && !n0->saltire;
    if (untagged && td5_roam_rails_untagged_x_enabled()) {   /* [roam PA] untagged: a plain St Andrew's cross, no arms, no lamps */
        nx = *n0; nx.saltire = 1; n = &nx;
    }
    const double lx = n->x - (double)e->ox, lz = n->z - (double)e->oz;
    const int own = roam_ctx_owned(c, lx, lz);
    int ed = -1, sd = 0, ap;
    double al = 0.0, lat = 0.0, sinang, dalong, hw;
    RoamFrame fr;
    RlNear tr;
    if (own) {
        e->st->lx_nodes++;
        if (untagged) e->st->lx_untagged++;
        if (!n->barrier && !n->light && !n->saltire) return 1;
        if (!untagged) e->st->lx_equipped++;
    } else if (!n->barrier && !n->light && !n->saltire) return 1;
    if (!rl_nearest_track(e->r, n->x, n->z, RL_M(12.0), &tr)) { if (own) e->st->lx_no_rail++; return 1; }
    if (!roam_nearest_edge(c, lx, lz, RL_M(12.0), &ed, &sd, &al, &lat) || !roam_edge_frame(c, ed, sd, al, &fr)) {
        if (own) e->st->lx_no_road++;
        return 1;
    }
    sinang = fabs(tr.tx * fr.tz - tr.tz * fr.tx);
    if (sinang < 0.5) sinang = 0.5;
    dalong = (0.5 * tr.w->gauge + RL_LX_STOP) / sinang;
    hw = fr.half_carriage;
    for (ap = -1; ap <= 1; ap += 2) {
        const double T[2] = { fr.tx * ap, fr.tz * ap };
        const double R[2] = { T[1], -T[0] };               /* right of an approaching car (it travels along -T) */
        const double ex = fr.px + T[0] * dalong + R[0] * hw, ez = fr.pz + T[1] * dalong + R[1] * hw;   /* the kerb */
        double ky = fr.kerb_y, px, pz, arm, cap;
        int sb, tries;
        (void)roam_ctx_road_y(c, ex, ez, fr.kerb_y, &ky);
        px = ex + R[0] * RL_LX_POST_OFF; pz = ez + R[1] * RL_LX_POST_OFF;
        for (tries = 0; tries < 2; tries++) {
            if (!rl_on_road(e, px, pz, RL_M(0.25), ky - RL_M(0.5), ky + RL_M(1.0))) break;
            px += R[0] * RL_M(1.0); pz += R[1] * RL_M(1.0);
        }
        if (!roam_ctx_owned(c, px, pz)) continue;
        if (tries == 2) { e->st->lx_post_drop++; continue; }
        arm = n->barrier == 2 ? 2.0 * hw + RL_LX_POST_OFF : hw + RL_LX_POST_OFF;
        cap = n->barrier == 2 ? RL_LX_ARM_FULL : RL_LX_ARM_HALF;
        if (arm > cap) arm = cap;
        sb = roam_ctx_sb_of(c, px, pz);
        if (!rl_lx_post(e, sb, px, pz, ky, T, R, n, arm)) return 0;
        if (untagged) e->st->lx_untagged_x++;
        if (untagged && e->log) TD5_LOG_I(LOG_TAG, "ROAM_RAIL_UNTAGGED_X at place (%.0f,%.0f)", px + (double)e->ox, pz + (double)e->oz);
    }
    return 1;
}

/* ---- station name boards ---- */

/* a double-faced board in the plate atlas page (2 quads, one record) */
static int rl_board_mesh(RlEm *e, int sb, int page, const double *x, const double *y, const double *z,
                         const double *u, const double *v)
{
    /* the same writer the street plates use (tg_write_quad_mesh): its UV encoding, not the prefab writer's */
    int seg_page[1], seg_nq[1];
    TG_Buf *b = (TG_Buf *)roam_ctx_mesh_begin(e->c, sb);
    if (!b) { e->oom = 1; return 0; }
    seg_page[0] = page; seg_nq[0] = 2;
    if (!tg_write_quad_mesh(b, x, y, z, u, v, 8, seg_page, seg_nq, 1) || b->oom) { e->oom = 1; return 0; }
    e->st->meshes++;
    return 1;
}

/* the board at tile-local (cx, cz), top of the platform / ground `top`, along unit tangent T */
static int rl_board(RlEm *e, const char *name, double cx, double cz, const double *T, double top)
{
    const double N[2] = { -T[1], T[0] };
    const double hw = 0.5 * RL_BOARD_W;
    int page = 0, rows = 1, sb, named, face, k, side;
    double vt = 0.0, vb = 1.0, bot = top + RL_BOARD_Y, h;
    double x[8], y[8], z[8], u[8], v[8];
    named = td5_roam_furn_plate_cell(name, &page, &vt, &vb, &rows);
    h = (named ? rows : 1) * RL_BOARD_ROW;
    sb = roam_ctx_sb_of(e->c, cx, cz);
    for (face = 0; face < 2; face++) {
        const double sgn = face == 0 ? 1.0 : -1.0;           /* the viewer's side */
        const double off = sgn * RL_M(0.03);
        const double xl = cx - T[0] * hw * sgn + N[0] * off, zl = cz - T[1] * hw * sgn + N[1] * off;   /* left edge */
        const double xr = cx + T[0] * hw * sgn + N[0] * off, zr = cz + T[1] * hw * sgn + N[1] * off;
        const double U0 = 1.5 / 64.0, U1 = 62.5 / 64.0;
        const double cxs[4] = { xl, xr, xr, xl }, czs[4] = { zl, zr, zr, zl };
        const double cys[4] = { bot, bot, bot + h, bot + h };
        for (k = 0; k < 4; k++) {
            const int o = face * 4 + k;
            x[o] = cxs[k]; z[o] = czs[k]; y[o] = cys[k];
            u[o] = (k == 1 || k == 2) ? U1 : U0;
            v[o] = (k < 2) ? vb : vt;
        }
    }
    if (named) {
        if (!rl_board_mesh(e, sb, page, x, y, z, u, v)) return 0;
        e->st->stn_named++;
    } else {
        for (face = 0; face < 2; face++) {
            double qx[4], qy[4], qz[4];
            for (k = 0; k < 4; k++) { qx[k] = x[face * 4 + k]; qy[k] = y[face * 4 + k]; qz[k] = z[face * 4 + k]; }
            if (!rl_quad(e, sb, RP_CONC, qx, qy, qz, k_rl_u4, k_rl_v4, RC_BOARD, 0)) return 0;
        }
        e->st->stn_plain++;
    }
    {   /* two posts */
        const double A[3] = { T[0], 0.0, T[1] }, B[3] = { N[0], 0.0, N[1] };
        for (side = -1; side <= 1; side += 2) {
            double p0[3], p1[3];
            p0[0] = p1[0] = cx + T[0] * (hw + RL_M(0.07)) * side; p0[2] = p1[2] = cz + T[1] * (hw + RL_M(0.07)) * side;   /* beyond the board's ends: a post in front of the text hid the first and last letters */
            p0[1] = top - RL_M(0.1); p1[1] = bot + h;
            if (!rl_obox(e, sb, p0, p1, A, B, RL_M(0.06), RL_M(0.06), RC_STEEL)) return 0;
        }
    }
    if (e->log)
        TD5_LOG_I(LOG_TAG, "ROAM_RAIL_BOARD '%s' at place (%.0f,%.0f) %s", name, cx + (double)e->ox, cz + (double)e->oz, named ? "named" : "blank");
    e->st->stn_boards++;
    return 1;
}

static int rl_station(RlEm *e, const RlStation *s)
{
    const TD5_RoamRailSource *r = e->r;
    int cand[3] = { 0, 0, 0 }, nc = 0, i, j;
    double cd[3] = { 0.0, 0.0, 0.0 };
    for (i = 0; i < r->nw; i++) {
        const RlWay *w = &r->w[i];
        double mx, mz, d;
        if (w->kind != RK_PLATFORM) continue;
        mx = 0.5 * (w->bx0 + w->bx1); mz = 0.5 * (w->bz0 + w->bz1);
        d = hypot(mx - s->x, mz - s->z);
        if (d > RL_M(160.0)) continue;
        for (j = nc < 3 ? nc : 2; j > 0 && cd[j - 1] > d; j--) { cand[j] = cand[j - 1]; cd[j] = cd[j - 1]; }
        if (j < 3) { cand[j] = i; cd[j] = d; if (nc < 3) nc++; }
    }
    if (nc > 0) {
        for (j = 0; j < nc; j++) {
            const RlWay *w = &r->w[cand[j]];
            double ax = 0.0, az = 0.0;
            RlNear tr;
            int n = w->n;
            if (w->closed) {
                if (n > 1 && r->px[w->p0] == r->px[w->p0 + n - 1] && r->pz[w->p0] == r->pz[w->p0 + n - 1]) n--;
                for (i = 0; i < n; i++) { ax += r->px[w->p0 + i]; az += r->pz[w->p0 + i]; }
                ax /= n; az /= n;
            } else {
                int seg, k = 0;
                for (seg = 0; seg + 1 < w->n; seg++) if (r->ps[w->p0 + seg + 1] >= 0.5 * w->len) { k = seg; break; }
                {
                    const double s0 = r->ps[w->p0 + k], s1 = r->ps[w->p0 + k + 1], t = s1 > s0 ? (0.5 * w->len - s0) / (s1 - s0) : 0.0;
                    ax = r->px[w->p0 + k] + (r->px[w->p0 + k + 1] - r->px[w->p0 + k]) * t;
                    az = r->pz[w->p0 + k] + (r->pz[w->p0 + k + 1] - r->pz[w->p0 + k]) * t;
                }
            }
            {
                const double lx = ax - (double)e->ox, lz = az - (double)e->oz;
                double T[2], top;
                if (!roam_ctx_owned(e->c, lx, lz)) continue;
                if (!rl_nearest_track(r, ax, az, RL_M(25.0), &tr)) { e->st->stn_drop++; continue; }
                T[0] = tr.tx; T[1] = tr.tz;
                top = rl_base_ground(e, lx, lz) + RL_HEAD + RL_PLAT_TOP;
                if (rl_on_road(e, lx, lz, RL_M(0.3), top - RL_M(3.0), top + RL_GUARD_HI)) { e->st->stn_drop++; continue; }
                if (!rl_board(e, s->name, lx, lz, T, top)) return 0;
                e->st->stn_on_platform++;
            }
        }
        return 1;
    } else {
        RlNear tr;
        int side;
        if (!rl_nearest_track(r, s->x, s->z, RL_M(80.0), &tr)) return 1;
        for (side = -1; side <= 1; side += 2) {
            const double ax = tr.px + (-tr.tz) * side * (0.5 * tr.w->gauge + RL_M(3.4));
            const double az = tr.pz + tr.tx * side * (0.5 * tr.w->gauge + RL_M(3.4));
            const double lx = ax - (double)e->ox, lz = az - (double)e->oz;
            const double g = roam_ctx_ground(e->c, lx, lz);
            double T[2];
            T[0] = tr.tx; T[1] = tr.tz;
            if (rl_on_road(e, lx, lz, RL_M(1.4), g - RL_M(1.0), g + RL_GUARD_HI)) {
                if (side == 1 && roam_ctx_owned(e->c, lx, lz)) e->st->stn_drop++;
                continue;
            }
            if (!roam_ctx_owned(e->c, lx, lz)) return 1;
            if (!rl_board(e, s->name, lx, lz, T, rl_base_ground(e, lx, lz) + RL_HEAD)) return 0;
            e->st->stn_by_track++;
            return 1;
        }
    }
    return 1;
}

/* ---- catenary ---- */

static int rl_near_kind(const TD5_RoamRailSource *r, const RlWay *w, double s, double d, int kind)
{
    int k;
    for (k = 0; k < w->nx; k++) if (r->x[w->x0 + k].kind == kind && fabs(r->x[w->x0 + k].s - s) < d) return 1;
    return 0;
}

/* segment + parameter of along-distance s */
static void rl_locate(const TD5_RoamRailSource *r, const RlWay *w, double s, int *seg_o, double *t_o)
{
    int seg;
    double t = 0.0;
    for (seg = 0; seg + 1 < w->n; seg++) {
        const double s0 = r->ps[w->p0 + seg], s1 = r->ps[w->p0 + seg + 1];
        if (s <= s1 || seg + 2 >= w->n) { t = s1 > s0 ? (s - s0) / (s1 - s0) : 0.0; break; }
    }
    if (seg + 1 >= w->n) seg = w->n - 2;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    *seg_o = seg; *t_o = t;
}

/* the section at along-distance s of way w, nudged like the track's own, with its head */
static void rl_cat_section(RlEm *e, const RlWay *w, double s, RlSec *S)
{
    int seg, bl, fl;
    double t, lo;
    const double shw = 0.5 * w->gauge + 2.0 * RL_RAIL_HW + RL_M(0.03);
    rl_locate(e->r, w, s, &seg, &t);
    rl_section(e, w, seg, t, S);
    if (!rl_near_grade(e->r, w, S->s, RL_BLEND)) (void)rl_nudge(e, S, shw);
    S->head = rl_head(e, w, seg, S->s, S->x, S->z, &lo, &bl, &fl);
}

static int rl_catenary(RlEm *e, const RlWay *w, double x0, double z0, double x1, double z1)
{
    const TD5_RoamRailSource *r = e->r;
    const double margin = RL_M(40.0) + RL_CAT_STEP;
    int nspan = (int)ceil(w->len / RL_CAT_STEP), k, used = 0, have_prev = 0;
    RlSec prev;
    if (nspan < 1) nspan = 1;
    memset(&prev, 0, sizeof prev);
    for (k = 0; k <= nspan && !e->oom; k++) {
        double s = k * RL_CAT_STEP, px, pz, t;
        int seg;
        RlSec S;
        if (s > w->len) s = w->len;
        rl_locate(r, w, s, &seg, &t);
        px = r->px[w->p0 + seg] + (r->px[w->p0 + seg + 1] - r->px[w->p0 + seg]) * t;
        pz = r->pz[w->p0 + seg] + (r->pz[w->p0 + seg + 1] - r->pz[w->p0 + seg]) * t;
        if (px < x0 - margin || px > x1 + margin || pz < z0 - margin || pz > z1 + margin) { have_prev = 0; continue; }
        rl_cat_section(e, w, s, &S);
        if (roam_ctx_owned(e->c, S.x, S.z) && !rl_near_kind(r, w, s, RL_CAT_DECK, RX_ROAD_OVER)) {   /* the mast at S */
            int side = (k & 1) ? 1 : -1, tries;
            double fxm = 0.0, fzm = 0.0, g = 0.0;
            for (tries = 0; tries < 2; tries++) {
                fxm = S.x + S.nx * side * RL_CAT_OFF; fzm = S.z + S.nz * side * RL_CAT_OFF;
                g = roam_ctx_ground(e->c, fxm, fzm);
                if (!rl_on_road(e, fxm, fzm, RL_M(0.2), g - RL_M(0.5), S.head + RL_CAT_TOP)) break;
                side = -side;
            }
            if (tries == 2) e->st->cat_mast_drop++;
            else {
                const double A[3] = { S.tx, 0.0, S.tz }, B[3] = { S.nx, 0.0, S.nz };
                const int sb = roam_ctx_sb_of(e->c, S.x, S.z);
                double p0[3], p1[3];
                p0[0] = p1[0] = fxm; p0[2] = p1[2] = fzm; p0[1] = g - RL_M(0.2); p1[1] = S.head + RL_CAT_TOP;
                if (!rl_obox(e, sb, p0, p1, A, B, RL_M(0.11), RL_M(0.11), RC_STEEL)) return 0;
                p0[0] = fxm; p0[2] = fzm; p0[1] = S.head + RL_CAT_ARM;          /* the cantilever over the track */
                p1[0] = S.x; p1[2] = S.z; p1[1] = S.head + RL_CAT_ARM;
                if (!rl_obox(e, sb, p0, p1, A, k_rl_up, RL_M(0.04), RL_M(0.05), RC_STEEL)) return 0;
                p0[0] = p1[0] = S.x; p0[2] = p1[2] = S.z; p0[1] = S.head + RL_CAT_WIRE; p1[1] = S.head + RL_CAT_ARM;   /* hanger */
                if (!rl_obox(e, sb, p0, p1, A, B, RL_M(0.02), RL_M(0.02), RC_WIRE)) return 0;
                e->st->cat_masts++;
                used = 1;
            }
        }
        if (have_prev) {   /* the wire pieces of the span that ends here */
            const double sa = prev.s, sbm = S.s;
            const int np = (int)ceil((sbm - sa) / RL_CAT_PIECE);
            RlSec P0 = prev;
            int j;
            for (j = 1; j <= np && sbm > sa + 1.0; j++) {
                RlSec P1;
                double mx, mz;
                if (j == np) P1 = S; else rl_cat_section(e, w, sa + (sbm - sa) * j / np, &P1);
                mx = 0.5 * (P0.x + P1.x); mz = 0.5 * (P0.z + P1.z);
                if (roam_ctx_owned(e->c, mx, mz)) {
                    if (rl_near_kind(r, w, 0.5 * (P0.s + P1.s), RL_CAT_DECK, RX_ROAD_OVER)) e->st->cat_skip_deck++;
                    else {
                        const int sb2 = roam_ctx_sb_of(e->c, mx, mz);
                        int wi;
                        for (wi = 0; wi < 2; wi++) {
                            const double dy = wi == 0 ? RL_CAT_WIRE : RL_CAT_MESS;
                            double a0[3], a1[3], al[3], hz[3], len, hl;
                            a0[0] = P0.x; a0[1] = P0.head + dy; a0[2] = P0.z;
                            a1[0] = P1.x; a1[1] = P1.head + dy; a1[2] = P1.z;
                            al[0] = a1[0] - a0[0]; al[1] = a1[1] - a0[1]; al[2] = a1[2] - a0[2];
                            len = sqrt(al[0] * al[0] + al[1] * al[1] + al[2] * al[2]);
                            if (!(len > 1.0)) continue;
                            hz[0] = -al[2]; hz[1] = 0.0; hz[2] = al[0];          /* horizontal, square to the wire */
                            hl = sqrt(hz[0] * hz[0] + hz[2] * hz[2]);
                            if (hl > 1e-9) { hz[0] /= hl; hz[2] /= hl; } else { hz[0] = 1.0; hz[2] = 0.0; }
                            if (!rl_ribbon(e, sb2, a0, a1, hz, RL_M(0.035), RC_WIRE)) return 0;
                            if (!rl_ribbon(e, sb2, a0, a1, k_rl_up, RL_M(0.035), RC_WIRE)) return 0;
                        }
                        e->st->cat_wire_pieces++;
                        e->st->cat_wire_m += hypot(P1.x - P0.x, P1.z - P0.z) / RL_UPM;
                        used = 1;
                    }
                }
                P0 = P1;
            }
        }
        prev = S; have_prev = 1;
    }
    if (used) e->st->cat_ways++;
    return 1;
}

/* ============================================================================
 * SECTION: names (for the plate atlas)
 * ========================================================================== */

static int rl_name_cmp(const void *pa, const void *pb) { return strcmp((const char *)pa, (const char *)pb); }

int td5_roam_rails_station_names(const char *place, char (*out)[96], double (*xz)[2], int max)
{
    char path[512];
    char *json;
    cJSON *root;
    const cJSON *tl, *t;
    int n = 0, nhs = 0;
    int kind_of[256];
    static double hs[1024][2];
    if (!place || !out || !xz || max <= 0 || !td5_roam_rails_enabled()) return 0;
    td5_geo_source_path(path, sizeof path, place, "tiles/INDEX.JSON");
    json = td5_geo_roads_slurp(path);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    tl = root ? cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "layers"), "RAILS"), "tiles") : NULL;
    for (t = (tl && cJSON_IsArray(tl)) ? tl->child : NULL; t; t = t->next) {
        const cJSON *a = t->child, *b = a ? a->next : NULL, *nodes, *nn;
        char rel[96];
        cJSON *tile;
        if (!a || !b || !cJSON_IsNumber(a) || !cJSON_IsNumber(b)) continue;
        snprintf(rel, sizeof rel, "tiles/RAILS_%d_%d.JSON", (int)a->valuedouble, (int)b->valuedouble);
        td5_geo_source_path(path, sizeof path, place, rel);
        json = td5_geo_roads_slurp(path);
        if (!json) continue;
        tile = cJSON_Parse(json);
        free(json);
        nodes = tile ? cJSON_GetObjectItem(tile, "nodes") : NULL;
        for (nn = nodes ? nodes->child : NULL; nn; nn = nn->next) {
            const cJSON *pk = cJSON_GetObjectItem(nn, "kind"), *nm = cJSON_GetObjectItem(nn, "name");
            int i;
            const int nk = (pk && cJSON_IsString(pk) && pk->valuestring) ? (!strcmp(pk->valuestring, "station") ? 1 : !strcmp(pk->valuestring, "halt") ? 2 : !strcmp(pk->valuestring, "stop") ? 3 : 0) : 0;
            if (!nk) continue;
            if (!nm || !cJSON_IsString(nm) || !nm->valuestring || !nm->valuestring[0]) continue;
            if (nk != 3 && nhs < 1024) {
                const cJSON *hx = cJSON_GetObjectItem(nn, "x"), *hz = cJSON_GetObjectItem(nn, "z");
                if (hx && hz && cJSON_IsNumber(hx) && cJSON_IsNumber(hz)) { hs[nhs][0] = td5_geo_game_x(hx->valuedouble); hs[nhs][1] = hz->valuedouble; nhs++; }
            }
            for (i = 0; i < n; i++) if (!strncmp(out[i], nm->valuestring, 95)) break;
            if (i < n && nk < kind_of[i]) {   /* a better kind of the same name: its position */
                const cJSON *qx = cJSON_GetObjectItem(nn, "x"), *qz = cJSON_GetObjectItem(nn, "z");
                kind_of[i] = nk;
                xz[i][0] = qx && cJSON_IsNumber(qx) ? td5_geo_game_x(qx->valuedouble) : xz[i][0];
                xz[i][1] = qz && cJSON_IsNumber(qz) ? qz->valuedouble : xz[i][1];
            }
            if (i < n || n >= max || n >= 256) continue;
            kind_of[n] = nk;
            snprintf(out[n], 96, "%s", nm->valuestring);
            {   const cJSON *qx = cJSON_GetObjectItem(nn, "x"), *qz = cJSON_GetObjectItem(nn, "z");
                xz[n][0] = qx && cJSON_IsNumber(qx) ? td5_geo_game_x(qx->valuedouble) : 0.0;
                xz[n][1] = qz && cJSON_IsNumber(qz) ? qz->valuedouble : 0.0;
            }
            n++;
        }
        if (tile) cJSON_Delete(tile);
    }
    if (root) cJSON_Delete(root);
    {   /* a stop-only name within 150 m of a station / halt is that station's platform stop: no board of its own */
        int i, w = 0;
        for (i = 0; i < n; i++) {
            int k, shadow = 0;
            if (kind_of[i] == 3)
                for (k = 0; k < nhs && !shadow; k++) if (hypot(hs[k][0] - xz[i][0], hs[k][1] - xz[i][1]) < RL_M(150.0)) shadow = 1;
            if (shadow) continue;
            if (w != i) { memcpy(out[w], out[i], 96); xz[w][0] = xz[i][0]; xz[w][1] = xz[i][1]; }
            w++;
        }
        n = w;
    }
    {   /* by name, the position travelling with it (insertion sort: n is tiny) */
        int i, j;
        for (i = 1; i < n; i++) {
            char nm[96];
            double q[2];
            memcpy(nm, out[i], 96); q[0] = xz[i][0]; q[1] = xz[i][1];
            for (j = i; j > 0 && rl_name_cmp(out[j - 1], nm) > 0; j--) { memcpy(out[j], out[j - 1], 96); xz[j][0] = xz[j - 1][0]; xz[j][1] = xz[j - 1][1]; }
            memcpy(out[j], nm, 96); xz[j][0] = q[0]; xz[j][1] = q[1];
        }
    }
    return n;
}

/* ============================================================================
 * SECTION: emit
 * ========================================================================== */

int td5_roam_rails_emit(TD5_RoamRailSource *r, TG_TileCtx *c, TD5_RoamRailStats *st)
{
    RlEm e;
    double corner[4][2], x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
    const RoamGrid *g = roam_ctx_grid(c);
    int tx, tz, i, k, sb, pg;
    const double margin = RL_M(30.0);
    if (!r || !c || !st) return 1;
    if (!r->loaded && !rl_load_all(r)) TD5_LOG_W(LOG_TAG, "roam rails: the RAILS layer of %s did not load: no railways", r->place);
    if (!r->load_ok) return 1;
    memset(&e, 0, sizeof e);
    e.c = c; e.st = st; e.r = r;
    e.log = td5_env_int("TD5RE_ROAM_RAILS_LOG", 0, 0, 1);
    roam_ctx_origin(c, &tx, &tz, &e.ox, &e.oz);
    roam_grid_corners(g, tx, tz, corner);
    for (k = 0; k < 4; k++) {
        if (corner[k][0] < x0) x0 = corner[k][0];
        if (corner[k][0] > x1) x1 = corner[k][0];
        if (corner[k][1] < z0) z0 = corner[k][1];
        if (corner[k][1] > z1) z1 = corner[k][1];
    }
    e.bat = (RlBatch *)calloc((size_t)TD5_ROAM_SCN_NSB * RP_N, sizeof(RlBatch));
    if (!e.bat) return 0;
    for (i = 0; i < r->nw && !e.oom; i++) {
        const RlWay *w = &r->w[i];
        int seg, used = 0;
        if (w->bx1 < x0 - margin || w->bx0 > x1 + margin || w->bz1 < z0 - margin || w->bz0 > z1 + margin) continue;
        if (w->kind == RK_PLATFORM) {
            if (w->closed) { if (!rl_plat_poly(&e, w)) e.oom = 1; }
            else if (!rl_plat_line(&e, w)) e.oom = 1;
            continue;
        }
        if (w->bridge) {   /* the bridge's straight line between its two ends' heads */
            const int a = w->p0, b = w->p0 + w->n - 1;
            e.gx_end[0] = rl_base_ground(&e, r->px[a] - (double)e.ox, r->pz[a] - (double)e.oz) + RL_HEAD;
            e.gx_end[1] = rl_base_ground(&e, r->px[b] - (double)e.ox, r->pz[b] - (double)e.oz) + RL_HEAD;
        }
        for (seg = 0; seg + 1 < w->n && !e.oom; seg++) {
            const double l = r->ps[w->p0 + seg + 1] - r->ps[w->p0 + seg];
            const int np = (int)ceil(l / RL_PIECE);
            int j;
            if (!(l > 1.0)) continue;
            for (j = 0; j < np && !e.oom; j++) {
                RlSec A, B;
                double la, lb;
                int ba, bb, fa, fb;
                rl_section(&e, w, seg, (double)j / np, &A);
                rl_section(&e, w, seg, (double)(j + 1) / np, &B);
                if (!roam_ctx_owned(c, 0.5 * (A.x + B.x), 0.5 * (A.z + B.z))) continue;
                if (w->tunnel) { st->tunnel_skip++; continue; }
                {   /* each section by its own along-distance: a shared end moves the same in both pieces */
                    const double shw = 0.5 * w->gauge + 2.0 * RL_RAIL_HW + RL_M(0.03);
                    const int na = !rl_near_grade(r, w, A.s, RL_BLEND) && rl_nudge(&e, &A, shw);
                    const int nb = !rl_near_grade(r, w, B.s, RL_BLEND) && rl_nudge(&e, &B, shw);
                    if (na || nb) st->nudged++;
                }
                A.head = rl_head(&e, w, seg, A.s, A.x, A.z, &la, &ba, &fa);
                B.head = rl_head(&e, w, seg, B.s, B.x, B.z, &lb, &bb, &fb);
                if (fa || fb) st->floored++;
                if (!rl_piece(&e, w, &A, &B, la > lb ? la : lb, ba || bb)) e.oom = 1;
                used = 1;
            }
        }
        if (used) st->ways++;
    }
    if (td5_roam_rails_furn_enabled() && !e.oom) {   /* [roam LF] */
        const double nm = RL_M(30.0), sm = RL_M(200.0);
        for (i = 0; i < r->nnd && !e.oom; i++) {
            const RlNode *n = &r->nd[i];
            if (n->kind != RN_LX) continue;
            if (n->x < x0 - nm || n->x > x1 + nm || n->z < z0 - nm || n->z > z1 + nm) continue;
            if (!rl_lx_node(&e, n)) e.oom = 1;
        }
        for (i = 0; i < r->nstn && !e.oom; i++) {
            const RlStation *s = &r->stn[i];
            if (s->x < x0 - sm || s->x > x1 + sm || s->z < z0 - sm || s->z > z1 + sm) continue;
            if (!rl_station(&e, s)) e.oom = 1;
        }
        for (i = 0; i < r->nw && !e.oom; i++) {
            const RlWay *w = &r->w[i];
            if (!w->elec || w->bridge || w->tunnel || (w->kind != RK_RAIL && w->kind != RK_LIGHT)) continue;
            if (w->bx1 < x0 - margin || w->bx0 > x1 + margin || w->bz1 < z0 - margin || w->bz0 > z1 + margin) continue;
            if (!rl_catenary(&e, w, x0, z0, x1, z1)) e.oom = 1;
        }
    }
    for (sb = 0; sb < TD5_ROAM_SCN_NSB && !e.oom; sb++)
        for (pg = 0; pg < RP_N && !e.oom; pg++)
            if (!rl_flush(&e, sb, pg)) e.oom = 1;
    free(e.bat);
    return e.oom ? 0 : 1;
}
