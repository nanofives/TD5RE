/**
 * td5_roam_outskirts.c -- GEO FREE ROAM: poles, wires, guard rails, gates outside the casco
 * (PORT-ONLY, roam LH). Contract and rules: td5_roam_outskirts.h.
 *
 * SECTIONS
 *   source    INDEX.JSON layers.OUTSKIRTS, every tile parsed once (game frame): poles sorted by id,
 *             wire spans (pole index pairs), guard-rail segments, gates
 *   writers   per (sub-block, page) quad batches -> one MODELS.DAT prefab record each
 *   shapes    prism / frustum, wire ribbon
 *   emit      the tile: owned features only, each footprint tested against the drivable surface
 *
 * Every decision keys off the OSM id and the place-frame position, never off the order tiles are
 * built in: same bytes on any thread, in any order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_trackgen_internal.h"   /* TG_Buf, tg_write_prefab_mesh, TD5_TG_PAGE_* */
#include "td5_geo.h"                 /* td5_geo_source_path */
#include "td5_geo_roads.h"           /* td5_geo_roads_slurp */
#include "td5_geo_frame.h"
#include "td5_config.h"
#include "td5_platform.h"
#include "deps/cjson/cJSON.h"
#include "td5_roam_scenery.h"
#include "td5_roam_outskirts.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "roam"

#define OU_UPM        430.0
#define OU_M(m)       ((m) * OU_UPM)
#define OU_CHUNK      128
#define OU_SAG        0.012           /* wire sag as a fraction of the span */
#define OU_WIRE_STEP  OU_M(25.0)
#define OU_WIRE_W     OU_M(0.05)      /* half ribbon width */
#define OU_GUARD_POST OU_M(4.0)
#define OU_GATE_HALF  OU_M(4.0)       /* gate posts this far either side of the road axis */

enum { OK_POLE = 0, OK_TOWER, OK_PORTAL };
enum { OM_WOOD = 0, OM_CONC, OM_METAL };
enum { OP_WOOD = 0, OP_CONC, OP_METAL, OP_GUARD, OP_N };

#define OC_WOOD   0xFF9A8268u
#define OC_CONC   0xFFC4C4BCu
#define OC_METAL  0xFFB4BCC4u
#define OC_WIRE   0xFF2A2A2Au
#define OC_GUARD  0xFFDCE0E4u
#define OC_RED    0xFFD04030u
#define OC_WHITE  0xFFF0F0F0u

typedef struct { int64_t id; double x, z, dx, dz, h; int kind, mat, hasdir; } OuPole;
typedef struct { int64_t a, b; int ia, ib, cls; } OuSpan;
typedef struct { double ax, az, bx, bz; } OuGuard;
typedef struct { double x, z, dx, dz; int lift; } OuGate;

struct TD5_RoamOutSource {
    char place[64];
    OuPole *p;   int np, capp;
    OuSpan *s;   int ns, caps;
    OuGuard *g;  int ng, capg;
    OuGate *t;   int nt, capt;
    int loaded, load_ok;
    int (*tile)[2];  int ntile;
};

int td5_roam_out_enabled(void) { return td5_env_int("TD5RE_ROAM_OUTSKIRTS", 1, 0, 1); }

/* ============================================================================
 * SECTION: source
 * ========================================================================== */

static int ou_grow(void **p, int *cap, int need, size_t elem)
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

static double ou_num(const cJSON *e, const char *key, double dflt)
{
    const cJSON *v = cJSON_GetObjectItem(e, key);
    if (!v) return dflt;
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) return atof(v->valuestring);
    return dflt;
}

static const char *ou_str(const cJSON *e, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(e, key);
    return v && cJSON_IsString(v) ? v->valuestring : NULL;
}

static int ou_load_tile(TD5_RoamOutSource *o, int ix, int iz)
{
    char rel[96], path[512];
    char *json;
    cJSON *root;
    const cJSON *arr, *e, *q;
    snprintf(rel, sizeof rel, "tiles/OUTSKIRTS_%d_%d.JSON", ix, iz);
    td5_geo_source_path(path, sizeof path, o->place, rel);
    json = td5_geo_roads_slurp(path);
    if (!json) return 1;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 1;
    td5_geo_src_json_to_game(root);                 /* every "x","z" into the game frame */
    arr = cJSON_GetObjectItem(root, "poles");
    for (e = arr ? arr->child : NULL; e; e = e->next) {
        const char *k = ou_str(e, "kind"), *m = ou_str(e, "material");
        OuPole *p;
        if (!k || !cJSON_GetObjectItem(e, "id")) continue;
        if (!ou_grow((void **)&o->p, &o->capp, o->np + 1, sizeof(OuPole))) { cJSON_Delete(root); return 0; }
        p = &o->p[o->np++];
        memset(p, 0, sizeof *p);
        p->id = (int64_t)ou_num(e, "id", 0.0);
        p->x = ou_num(e, "x", 0.0); p->z = ou_num(e, "z", 0.0);
        p->kind = !strcmp(k, "tower") ? OK_TOWER : (!strcmp(k, "portal") ? OK_PORTAL : OK_POLE);
        if (m && (!strcmp(m, "wood"))) p->mat = OM_WOOD;
        else if (m && (!strcmp(m, "concrete") || !strcmp(m, "reinforced_concrete"))) p->mat = OM_CONC;
        else if (m && (!strcmp(m, "metal") || !strcmp(m, "iron") || !strcmp(m, "steel"))) p->mat = OM_METAL;
        else p->mat = (int)(((p->id >> 3) ^ p->id) & 1) ? OM_CONC : OM_WOOD;   /* untagged: by id */
        if (p->kind == OK_TOWER && !m) p->mat = OM_METAL;
        p->h = ou_num(e, "height", 0.0);
    }
    arr = cJSON_GetObjectItem(root, "lines");
    for (e = arr ? arr->child : NULL; e; e = e->next) {
        const cJSON *ids = cJSON_GetObjectItem(e, "pole_ids");
        const char *k = ou_str(e, "kind");
        const int cls = (k && !strcmp(k, "line")) ? 1 : 0;
        int prev = 0;
        int64_t last = 0;
        if (!ids || !cJSON_IsArray(ids)) continue;
        for (q = ids->child; q; q = q->next) {
            const int64_t id = cJSON_IsNumber(q) ? (int64_t)q->valuedouble : 0;
            if (prev && last && id) {
                OuSpan *s;
                if (!ou_grow((void **)&o->s, &o->caps, o->ns + 1, sizeof(OuSpan))) { cJSON_Delete(root); return 0; }
                s = &o->s[o->ns++];
                s->a = last; s->b = id; s->ia = s->ib = -1; s->cls = cls;
            }
            last = id; prev = 1;
        }
    }
    arr = cJSON_GetObjectItem(root, "guards");
    for (e = arr ? arr->child : NULL; e; e = e->next) {
        const cJSON *pts = cJSON_GetObjectItem(e, "points");
        double px = 0.0, pz = 0.0;
        int n = 0;
        if (!pts || !cJSON_IsArray(pts)) continue;
        for (q = pts->child; q; q = q->next) {
            const double x = ou_num(q, "x", 0.0), z = ou_num(q, "z", 0.0);
            if (n > 0 && hypot(x - px, z - pz) > 1.0) {
                OuGuard *g;
                if (!ou_grow((void **)&o->g, &o->capg, o->ng + 1, sizeof(OuGuard))) { cJSON_Delete(root); return 0; }
                g = &o->g[o->ng++];
                g->ax = px; g->az = pz; g->bx = x; g->bz = z;
            }
            px = x; pz = z; n++;
        }
    }
    arr = cJSON_GetObjectItem(root, "gates");
    for (e = arr ? arr->child : NULL; e; e = e->next) {
        const char *k = ou_str(e, "kind");
        OuGate *t;
        if (!ou_grow((void **)&o->t, &o->capt, o->nt + 1, sizeof(OuGate))) { cJSON_Delete(root); return 0; }
        t = &o->t[o->nt++];
        t->x = ou_num(e, "x", 0.0); t->z = ou_num(e, "z", 0.0);
        t->dx = -ou_num(e, "dx", 1.0);          /* direction is not an "x"/"z" pair: reflect it by hand */
        t->dz = ou_num(e, "dz", 0.0);
        t->lift = (k && !strcmp(k, "lift_gate")) ? 1 : 0;
    }
    cJSON_Delete(root);
    return 1;
}

static int ou_pole_cmp(const void *pa, const void *pb)
{
    const OuPole *a = (const OuPole *)pa, *b = (const OuPole *)pb;
    return a->id < b->id ? -1 : (a->id > b->id);
}

static int ou_find(const OuPole *p, int n, int64_t id)
{
    int a = 0, b = n;
    while (a < b) { const int m = (a + b) / 2; if (p[m].id < id) a = m + 1; else b = m; }
    return (a < n && p[a].id == id) ? a : -1;
}

static void ou_set_dir(OuPole *p, double dx, double dz)
{
    const double l = hypot(dx, dz);
    if (p->hasdir || l < 1e-6) return;
    p->dx = dx / l; p->dz = dz / l; p->hasdir = 1;
}

static int ou_load_all(TD5_RoamOutSource *o)
{
    int i, w = 0;
    o->loaded = 1;
    for (i = 0; i < o->ntile; i++)
        if (!ou_load_tile(o, o->tile[i][0], o->tile[i][1])) return 0;
    qsort(o->p, (size_t)o->np, sizeof(OuPole), ou_pole_cmp);
    for (i = 0; i < o->ns; i++) {
        OuSpan *s = &o->s[i];
        s->ia = ou_find(o->p, o->np, s->a);
        s->ib = ou_find(o->p, o->np, s->b);
        if (s->ia < 0 || s->ib < 0 || s->ia == s->ib) continue;
        ou_set_dir(&o->p[s->ia], o->p[s->ib].x - o->p[s->ia].x, o->p[s->ib].z - o->p[s->ia].z);
        ou_set_dir(&o->p[s->ib], o->p[s->ib].x - o->p[s->ia].x, o->p[s->ib].z - o->p[s->ia].z);
        o->s[w++] = *s;
    }
    o->ns = w;
    o->load_ok = 1;
    TD5_LOG_I(LOG_TAG, "roam outskirts: layer loaded, %d pole(s), %d wire span(s), %d guard-rail segment(s), %d gate(s)",
              o->np, o->ns, o->ng, o->nt);
    return 1;
}

TD5_RoamOutSource *td5_roam_out_open(const char *place)
{
    TD5_RoamOutSource *o;
    char path[512];
    char *json;
    cJSON *root;
    const cJSON *tl, *t;
    if (!place || !td5_roam_out_enabled()) return NULL;
    td5_geo_source_path(path, sizeof path, place, "tiles/INDEX.JSON");
    json = td5_geo_roads_slurp(path);
    if (!json) return NULL;
    root = cJSON_Parse(json);
    free(json);
    tl = root ? cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "layers"), "OUTSKIRTS"), "tiles") : NULL;
    if (!tl || !cJSON_IsArray(tl)) {
        if (root) cJSON_Delete(root);
        TD5_LOG_I(LOG_TAG, "roam outskirts: %s has no OUTSKIRTS layer (re/tools/geo_outskirts.py): nothing outside the casco", place);
        return NULL;
    }
    o = (TD5_RoamOutSource *)calloc(1, sizeof *o);
    if (!o) { cJSON_Delete(root); return NULL; }
    snprintf(o->place, sizeof o->place, "%s", place);
    o->tile = (int (*)[2])malloc((size_t)(cJSON_GetArraySize(tl) + 1) * sizeof(int[2]));
    if (!o->tile) { cJSON_Delete(root); td5_roam_out_close(o); return NULL; }
    for (t = tl->child; t; t = t->next) {
        const cJSON *a = t->child, *b = a ? a->next : NULL;
        if (!a || !b || !cJSON_IsNumber(a) || !cJSON_IsNumber(b)) continue;
        o->tile[o->ntile][0] = (int)a->valuedouble; o->tile[o->ntile][1] = (int)b->valuedouble;
        o->ntile++;
    }
    cJSON_Delete(root);
    return o;
}

void td5_roam_out_close(TD5_RoamOutSource *o)
{
    if (!o) return;
    free(o->p); free(o->s); free(o->g); free(o->t); free(o->tile);
    free(o);
}

void td5_roam_out_summary(const TD5_RoamOutSource *o, int *ntiles, int *poles, int *spans, int *guards, int *gates)
{
    if (ntiles) *ntiles = o ? o->ntile : 0;
    if (poles) *poles = o ? o->np : 0;
    if (spans) *spans = o ? o->ns : 0;
    if (guards) *guards = o ? o->ng : 0;
    if (gates) *gates = o ? o->nt : 0;
}

/* ============================================================================
 * SECTION: writers
 * ========================================================================== */

typedef struct OuBatch { int nq; float v[OU_CHUNK * 4 * 5]; unsigned int light[OU_CHUNK * 4]; } OuBatch;

typedef struct {
    TG_TileCtx *c;
    TD5_RoamOutStats *st;
    int32_t ox, oz;                /* tile origin: tile-local = place - origin */
    OuBatch *bat;
    int oom;
} OuEm;

static int ou_page(int p)
{
    switch (p) {
    case OP_WOOD:  return TD5_TG_PAGE_ROAD_EXTRA + 1;     /* plank wood */
    case OP_CONC:  return TD5_TG_PAGE_R4_PIER;            /* cast concrete */
    case OP_METAL: return TD5_TG_PAGE_R11_SIGN_POST;      /* brushed metal */
    default:       return TD5_TG_PAGE_R4_GUARDRAIL;       /* W-beam steel */
    }
}

static int ou_flush(OuEm *e, int sb, int pg)
{
    OuBatch *q = &e->bat[sb * OP_N + pg];
    unsigned short cmd[3];
    TG_Buf *b;
    if (q->nq <= 0) return 1;
    b = (TG_Buf *)roam_ctx_mesh_begin(e->c, sb);
    if (!b) { e->oom = 1; return 0; }
    cmd[0] = (unsigned short)ou_page(pg); cmd[1] = 0; cmd[2] = (unsigned short)q->nq;
    if (!tg_write_prefab_mesh(b, q->v, q->light, q->nq * 4, cmd, 1, 0, 0.0, 0.0, 0.0, 1.0, 0.0) || b->oom) { e->oom = 1; return 0; }
    e->st->meshes++;
    q->nq = 0;
    return 1;
}

static int ou_put(OuEm *e, int sb, int pg, const double *x, const double *y, const double *z,
                  const double *u, const double *v, unsigned int col, const int *ord)
{
    OuBatch *q = &e->bat[sb * OP_N + pg];
    int k;
    if (q->nq >= OU_CHUNK && !ou_flush(e, sb, pg)) return 0;
    for (k = 0; k < 4; k++) {
        const int i = ord[k], o = (q->nq * 4 + k) * 5;
        q->v[o + 0] = (float)x[i]; q->v[o + 1] = (float)y[i]; q->v[o + 2] = (float)z[i];
        q->v[o + 3] = (float)u[i]; q->v[o + 4] = (float)v[i];
        q->light[q->nq * 4 + k] = col;
    }
    q->nq++;
    e->st->quads++;
    return 1;
}

/* one quad, written both ways round (a thin or unlit-side face must read from either side) */
static int ou_quad2(OuEm *e, int pg, const double *x, const double *y, const double *z,
                    const double *u, const double *v, unsigned int col)
{
    static const int fwd[4] = { 0, 1, 2, 3 }, rev[4] = { 3, 2, 1, 0 };
    const int sb = roam_ctx_sb_of(e->c, 0.25 * (x[0] + x[1] + x[2] + x[3]), 0.25 * (z[0] + z[1] + z[2] + z[3]));
    if (!ou_put(e, sb, pg, x, y, z, u, v, col, fwd)) { e->oom = 1; return 0; }
    if (!ou_put(e, sb, pg, x, y, z, u, v, col, rev)) { e->oom = 1; return 0; }
    return 1;
}

/* ============================================================================
 * SECTION: shapes
 * ========================================================================== */

/* a square-section frustum / box standing from y0 to y1 at (cx, cz): axes u = (ux, uz) and its
 * normal w; half sizes along u / w at the bottom (hl0 / hw0) and top (hl1 / hw1); 4 sides + top */
static int ou_prism(OuEm *e, int pg, double cx, double cz, double ux, double uz, double y0, double y1,
                    double hl0, double hw0, double hl1, double hw1, double vrep, unsigned int col)
{
    const double wx = -uz, wz = ux;
    const int sa[4] = { -1, 1, 1, -1 }, sw[4] = { -1, -1, 1, 1 };
    double bx[4], bz[4], tx[4], tz[4];
    int k;
    for (k = 0; k < 4; k++) {
        bx[k] = cx + ux * sa[k] * hl0 + wx * sw[k] * hw0; bz[k] = cz + uz * sa[k] * hl0 + wz * sw[k] * hw0;
        tx[k] = cx + ux * sa[k] * hl1 + wx * sw[k] * hw1; tz[k] = cz + uz * sa[k] * hl1 + wz * sw[k] * hw1;
    }
    for (k = 0; k < 4; k++) {
        const int j = (k + 1) & 3;
        const double x[4] = { bx[k], bx[j], tx[j], tx[k] }, z[4] = { bz[k], bz[j], tz[j], tz[k] };
        const double y[4] = { y0, y0, y1, y1 }, u[4] = { 0.0, 1.0, 1.0, 0.0 }, v[4] = { 0.0, 0.0, vrep, vrep };
        if (!ou_quad2(e, pg, x, y, z, u, v, col)) return 0;
    }
    {
        const double y[4] = { y1, y1, y1, y1 }, u[4] = { 0.0, 1.0, 1.0, 0.0 }, v[4] = { 0.0, 0.0, 1.0, 1.0 };
        if (!ou_quad2(e, pg, tx, y, tz, u, v, col)) return 0;
    }
    return 1;
}

/* a wire ribbon A -> B (tile-local, absolute y), sagging by OU_SAG * span at the middle */
static int ou_wire(OuEm *e, const double *A, const double *B, int *quads)
{
    const double span = hypot(B[0] - A[0], B[2] - A[2]);
    const double sag = OU_SAG * span;
    int n = (int)ceil(span / OU_WIRE_STEP), i;
    double px = A[0], py = A[1], pz = A[2], lx = 0.0, lz = 1.0;
    if (n < 2) n = 2;
    if (n > 10) n = 10;
    if (span > 1.0) { lx = -(B[2] - A[2]) / span; lz = (B[0] - A[0]) / span; }
    for (i = 1; i <= n; i++) {
        const double t = (double)i / n;
        const double qx = A[0] + (B[0] - A[0]) * t, qz = A[2] + (B[2] - A[2]) * t;
        const double qy = A[1] + (B[1] - A[1]) * t - 4.0 * sag * t * (1.0 - t);
        const double u[4] = { 0.0, 0.0, 1.0, 1.0 }, v[4] = { 0.0, 1.0, 1.0, 0.0 };
        {   /* flat ribbon (seen from above / below) */
            const double x[4] = { px + lx * OU_WIRE_W, px - lx * OU_WIRE_W, qx - lx * OU_WIRE_W, qx + lx * OU_WIRE_W };
            const double z[4] = { pz + lz * OU_WIRE_W, pz - lz * OU_WIRE_W, qz - lz * OU_WIRE_W, qz + lz * OU_WIRE_W };
            const double y[4] = { py, py, qy, qy };
            if (!ou_quad2(e, OP_METAL, x, y, z, u, v, OC_WIRE)) return 0;
        }
        {   /* upright ribbon (seen from the side) */
            const double x[4] = { px, px, qx, qx }, z[4] = { pz, pz, qz, qz };
            const double y[4] = { py + OU_WIRE_W, py - OU_WIRE_W, qy - OU_WIRE_W, qy + OU_WIRE_W };
            if (!ou_quad2(e, OP_METAL, x, y, z, u, v, OC_WIRE)) return 0;
        }
        *quads += 2;
        px = qx; py = qy; pz = qz;
    }
    return 1;
}

/* ============================================================================
 * SECTION: emit
 * ========================================================================== */

static double ou_pole_h(const OuPole *p, int cls_hint)
{
    if (p->h >= 3.0 && p->h <= 80.0) return p->h;
    if (p->kind == OK_TOWER) return 32.0;
    return cls_hint ? 14.0 : (p->mat == OM_CONC ? 11.0 : 9.0);
}

/* the attach points of pole p (tile-local, three conductors): out[3][3] = x, y, z */
static void ou_attach(const OuPole *p, double lx, double lz, double g, double h, double out[3][3])
{
    const double ux = p->hasdir ? p->dx : 1.0, uz = p->hasdir ? p->dz : 0.0;
    const double wx = -uz, wz = ux;
    const double off = p->kind == OK_TOWER ? OU_M(4.0) : OU_M(0.8);
    const double y = g + OU_M(p->kind == OK_TOWER ? h - 3.0 : h - 0.2);
    int k;
    for (k = 0; k < 3; k++) {
        const double o = (k - 1) * off;
        out[k][0] = lx + wx * o; out[k][1] = y; out[k][2] = lz + wz * o;
    }
}

static int ou_footprint_on_road(OuEm *e, double cx, double cz, double hw, double y0, double y1)
{
    const double x[4] = { cx - hw, cx + hw, cx + hw, cx - hw }, z[4] = { cz - hw, cz - hw, cz + hw, cz + hw };
    return roam_ctx_on_road_band(e->c, x, z, 4, y0, y1);
}

static int ou_emit_pole(OuEm *e, const OuPole *p, double lx, double lz)
{
    TD5_RoamOutStats *st = e->st;
    const int tower = p->kind == OK_TOWER;
    const double g = roam_ctx_ground(e->c, lx, lz);
    const double h = ou_pole_h(p, 0);
    const double hw0 = OU_M(tower ? 2.4 : (p->mat == OM_CONC ? 0.20 : 0.16)), hw1 = OU_M(tower ? 0.45 : (p->mat == OM_CONC ? 0.13 : 0.11));
    const double ux = p->hasdir ? p->dx : 1.0, uz = p->hasdir ? p->dz : 0.0;
    const int pg = tower || p->mat == OM_METAL ? OP_METAL : (p->mat == OM_CONC ? OP_CONC : OP_WOOD);
    const unsigned int col = pg == OP_METAL ? OC_METAL : (pg == OP_CONC ? OC_CONC : OC_WOOD);
    if (ou_footprint_on_road(e, lx, lz, hw0, g - OU_M(0.5), g + OU_M(2.0))) { st->on_road_skipped++; return 1; }
    if (!ou_prism(e, pg, lx, lz, ux, uz, g, g + OU_M(h), hw0, hw0, hw1, hw1, h / 2.0, col)) return 0;
    /* cross arm(s): a thin bar across the line */
    if (tower) {
        if (!ou_prism(e, OP_METAL, lx, lz, -uz, ux, g + OU_M(h - 3.2), g + OU_M(h - 2.9), OU_M(4.6), OU_M(0.15), OU_M(4.6), OU_M(0.15), 1.0, OC_METAL)) return 0;
        if (!ou_prism(e, OP_METAL, lx, lz, -uz, ux, g + OU_M(h - 9.2), g + OU_M(h - 8.9), OU_M(3.4), OU_M(0.15), OU_M(3.4), OU_M(0.15), 1.0, OC_METAL)) return 0;
    } else {
        if (!ou_prism(e, OP_WOOD, lx, lz, -uz, ux, g + OU_M(h - 0.45), g + OU_M(h - 0.30), OU_M(0.95), OU_M(0.06), OU_M(0.95), OU_M(0.06), 1.0, OC_WOOD)) return 0;
    }
    st->poles++;
    if (tower) st->towers++;
    else if (p->mat == OM_CONC) st->concrete++;
    else st->wood++;
    return 1;
}

static int ou_emit_guard(OuEm *e, const OuGuard *s, double ax, double az, double bx, double bz)
{
    TD5_RoamOutStats *st = e->st;
    const double l = hypot(bx - ax, bz - az);
    const double ga = roam_ctx_ground(e->c, ax, az), gb = roam_ctx_ground(e->c, bx, bz);
    const double px[2] = { ax, bx }, pz[2] = { az, bz };
    double d;
    (void)s;
    if (l < 1.0) return 1;
    if (roam_ctx_on_road_band(e->c, px, pz, 2, (ga < gb ? ga : gb) - OU_M(0.5), (ga > gb ? ga : gb) + OU_M(1.5))) { st->on_road_skipped++; return 1; }
    {   /* the W-beam band */
        const double x[4] = { ax, bx, bx, ax }, z[4] = { az, bz, bz, az };
        const double y[4] = { ga + OU_M(0.50), gb + OU_M(0.50), gb + OU_M(0.82), ga + OU_M(0.82) };
        const double u[4] = { 0.0, l / OU_M(4.0), l / OU_M(4.0), 0.0 }, v[4] = { 0.0, 0.0, 1.0, 1.0 };
        if (!ou_quad2(e, OP_GUARD, x, y, z, u, v, OC_GUARD)) return 0;
    }
    {   /* posts every 4 m (the first half a step in) */
        const double ux = (bx - ax) / l, uz = (bz - az) / l;
        for (d = 0.5 * OU_GUARD_POST; d < l; d += OU_GUARD_POST) {
            const double cx = ax + ux * d, cz = az + uz * d, g = ga + (gb - ga) * (d / l);
            if (!ou_prism(e, OP_METAL, cx, cz, ux, uz, g, g + OU_M(0.90), OU_M(0.05), OU_M(0.05), OU_M(0.05), OU_M(0.05), 1.0, OC_METAL)) return 0;
            st->guard_posts++;
        }
    }
    st->guards++;
    st->guard_m += l / OU_UPM;
    return 1;
}

static int ou_emit_gate(OuEm *e, const OuGate *t, double lx, double lz)
{
    TD5_RoamOutStats *st = e->st;
    const double l = hypot(t->dx, t->dz);
    const double ux = l > 1e-6 ? t->dx / l : 1.0, uz = l > 1e-6 ? t->dz / l : 0.0;
    const double wx = -uz, wz = ux;
    int side;
    double c[2][2], g[2];
    for (side = 0; side < 2; side++) {
        const double s = side ? 1.0 : -1.0;
        c[side][0] = lx + wx * s * OU_GATE_HALF; c[side][1] = lz + wz * s * OU_GATE_HALF;
        g[side] = roam_ctx_ground(e->c, c[side][0], c[side][1]);
        if (ou_footprint_on_road(e, c[side][0], c[side][1], OU_M(0.15), g[side] - OU_M(0.5), g[side] + OU_M(2.0))) { st->on_road_skipped++; return 1; }
    }
    for (side = 0; side < 2; side++) {
        if (!ou_prism(e, OP_METAL, c[side][0], c[side][1], ux, uz, g[side], g[side] + OU_M(1.5), OU_M(0.12), OU_M(0.12), OU_M(0.12), OU_M(0.12), 1.0, OC_WHITE)) return 0;
        if (t->lift) {   /* the boom stands up beside its post */
            if (side == 0 && !ou_prism(e, OP_METAL, c[0][0] + wx * OU_M(0.3), c[0][1] + wz * OU_M(0.3), ux, uz, g[0], g[0] + OU_M(3.6),
                                       OU_M(0.06), OU_M(0.06), OU_M(0.06), OU_M(0.06), 3.0, OC_RED)) return 0;
        } else if (!ou_prism(e, OP_METAL, c[side][0] - wx * (side ? 1.0 : -1.0) * OU_M(0.6), c[side][1] - wz * (side ? 1.0 : -1.0) * OU_M(0.6),
                             ux, uz, g[side] + OU_M(0.4), g[side] + OU_M(1.3), OU_M(0.04), OU_M(0.6), OU_M(0.04), OU_M(0.6), 1.0, OC_RED)) return 0;
    }
    st->gates++;
    if (t->lift) st->gates_lift++;
    return 1;
}

int td5_roam_out_emit(TD5_RoamOutSource *o, TG_TileCtx *c, TD5_RoamOutStats *st, int casco)
{
    OuEm e;
    double corner[4][2], x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
    const RoamGrid *g = roam_ctx_grid(c);
    int tx, tz, i, k, sb, pg;
    const double margin = OU_M(40.0);
    if (!o || !c || !st || casco) return 1;
    if (!o->loaded && !ou_load_all(o)) TD5_LOG_W(LOG_TAG, "roam outskirts: the OUTSKIRTS layer of %s did not load", o->place);
    if (!o->load_ok) return 1;
    memset(&e, 0, sizeof e);
    e.c = c; e.st = st;
    roam_ctx_origin(c, &tx, &tz, &e.ox, &e.oz);
    roam_grid_corners(g, tx, tz, corner);
    for (k = 0; k < 4; k++) {
        if (corner[k][0] < x0) x0 = corner[k][0];
        if (corner[k][0] > x1) x1 = corner[k][0];
        if (corner[k][1] < z0) z0 = corner[k][1];
        if (corner[k][1] > z1) z1 = corner[k][1];
    }
    e.bat = (OuBatch *)calloc((size_t)TD5_ROAM_SCN_NSB * OP_N, sizeof(OuBatch));
    if (!e.bat) return 0;
    for (i = 0; i < o->np && !e.oom; i++) {          /* poles: id order */
        const OuPole *p = &o->p[i];
        const double lx = p->x - (double)e.ox, lz = p->z - (double)e.oz;
        if (p->x < x0 - margin || p->x > x1 + margin || p->z < z0 - margin || p->z > z1 + margin) continue;
        if (!roam_ctx_owned(c, lx, lz)) continue;
        if (p->kind == OK_PORTAL) { st->portal_skipped++; continue; }
        if (!ou_emit_pole(&e, p, lx, lz)) e.oom = 1;
    }
    for (i = 0; i < o->ns && !e.oom; i++) {          /* wires: one span, three conductors */
        const OuSpan *s = &o->s[i];
        const OuPole *a = &o->p[s->ia], *b = &o->p[s->ib];
        double A[3][3], B[3][3], mx, mz;
        int j, q = 0;
        if (a->kind == OK_PORTAL || b->kind == OK_PORTAL) continue;
        mx = 0.5 * (a->x + b->x) - (double)e.ox; mz = 0.5 * (a->z + b->z) - (double)e.oz;
        if (0.5 * (a->x + b->x) < x0 - margin || 0.5 * (a->x + b->x) > x1 + margin ||
            0.5 * (a->z + b->z) < z0 - margin || 0.5 * (a->z + b->z) > z1 + margin) continue;
        if (!roam_ctx_owned(c, mx, mz)) continue;
        ou_attach(a, a->x - (double)e.ox, a->z - (double)e.oz, roam_ctx_ground(c, a->x - (double)e.ox, a->z - (double)e.oz), ou_pole_h(a, s->cls), A);
        ou_attach(b, b->x - (double)e.ox, b->z - (double)e.oz, roam_ctx_ground(c, b->x - (double)e.ox, b->z - (double)e.oz), ou_pole_h(b, s->cls), B);
        for (j = 0; j < 3 && !e.oom; j++)
            if (!ou_wire(&e, A[j], B[j], &q)) e.oom = 1;
        st->wires++;
        st->wire_quads += q;
        st->wire_m += hypot(b->x - a->x, b->z - a->z) / OU_UPM;
    }
    for (i = 0; i < o->ng && !e.oom; i++) {          /* guard rails */
        const OuGuard *s = &o->g[i];
        const double ax = s->ax - (double)e.ox, az = s->az - (double)e.oz, bx = s->bx - (double)e.ox, bz = s->bz - (double)e.oz;
        if ((s->ax < x0 - margin && s->bx < x0 - margin) || (s->ax > x1 + margin && s->bx > x1 + margin) ||
            (s->az < z0 - margin && s->bz < z0 - margin) || (s->az > z1 + margin && s->bz > z1 + margin)) continue;
        if (!roam_ctx_owned(c, 0.5 * (ax + bx), 0.5 * (az + bz))) continue;
        if (!ou_emit_guard(&e, s, ax, az, bx, bz)) e.oom = 1;
    }
    for (i = 0; i < o->nt && !e.oom; i++) {          /* gates */
        const OuGate *t = &o->t[i];
        const double lx = t->x - (double)e.ox, lz = t->z - (double)e.oz;
        if (t->x < x0 - margin || t->x > x1 + margin || t->z < z0 - margin || t->z > z1 + margin) continue;
        if (!roam_ctx_owned(c, lx, lz)) continue;
        if (!ou_emit_gate(&e, t, lx, lz)) e.oom = 1;
    }
    for (sb = 0; sb < TD5_ROAM_SCN_NSB && !e.oom; sb++)
        for (pg = 0; pg < OP_N && !e.oom; pg++)
            if (!ou_flush(&e, sb, pg)) e.oom = 1;
    free(e.bat);
    if (td5_env_int("TD5RE_ROAM_OUT_LOG", 0, 0, 1) && (st->poles || st->wires || st->guards || st->gates || st->on_road_skipped))
        TD5_LOG_I(LOG_TAG, "roam outskirts tile %d,%d: poles %d (tower %d) wires %d guards %d (%.0f m) gates %d, skipped on road %d, quads %d, meshes %d",
                  tx, tz, st->poles, st->towers, st->wires, st->guards, st->guard_m, st->gates, st->on_road_skipped, st->quads, st->meshes);
    return e.oom ? 0 : 1;
}
