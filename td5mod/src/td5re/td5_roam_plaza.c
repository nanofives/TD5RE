/**
 * td5_roam_plaza.c -- GEO FREE ROAM: PLAZA FURNITURE AND MONUMENTS of one tile (PORT-ONLY, roam LD).
 * Contract, kinds and what is left out: td5_roam_plaza.h.
 *
 * SECTIONS
 *   source    PLAZA_FURNITURE.JSON, parsed once on the first emit (game frame), file order (osm type, id)
 *   writers   per (sub-block, page) quad batches -> one MODELS.DAT prefab record each, + the guard audit
 *   pieces    box, n-gon prism / frustum, flat n-gon, billboard figure
 *   kinds     statue, stele, stone, fountain, bench, playground, flagpole
 *   emit      the items the tile owns
 *
 * Every decision keys off the item (its id, kind, tags and place-frame point), never off the order
 * tiles are built in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_trackgen_internal.h"   /* TG_Buf, tg_write_prefab_mesh, tg_emit_billboard_mesh, TD5_TG_PAGE_* */
#include "td5_geo.h"                 /* td5_geo_source_path                                               */
#include "td5_geo_roads.h"           /* td5_geo_roads_slurp                                               */
#include "td5_geo_frame.h"           /* [roam M] the source -> game reflection                            */
#include "td5_config.h"
#include "td5_platform.h"
#include "deps/cjson/cJSON.h"
#include "td5_roam_scenery.h"
#include "td5_roam_plaza.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "roam"

#define PZ_UPM        430.0
#define PZ_M(m)       ((m) * PZ_UPM)
#define PZ_CHUNK      128            /* quads per record                                  */
#define PZ_SINK       PZ_M(0.25)     /* plinths / basins start this far under the base    */
#define PZ_GUARD_LO   PZ_M(0.5)
#define PZ_GUARD_HI   PZ_M(2.5)

enum { PK_STATUE = 0, PK_MONUMENT, PK_MEMORIAL, PK_FOUNTAIN, PK_BENCH, PK_PLAYGROUND, PK_FLAGPOLE, PK_SKIP };
enum { PP_STONE = 0, PP_METAL, PP_WATER, PP_WOOD, PP_N };

/* ARGB, baked per vertex */
#define PC_STONE    0xFFE4DCCAu      /* light limestone                    */
#define PC_STONE_D  0xFFB8B0A0u      /* cornice / base course              */
#define PC_GRANITE  0xFF9A948Cu      /* the foundation stone               */
#define PC_WATER    0xFFB8D8F0u
#define PC_WOOD     0xFF8A6440u
#define PC_IRON     0xFF3C3E42u
#define PC_WHITE    0xFFF4F4F4u
#define PC_RED      0xFFC8382Cu
#define PC_YELLOW   0xFFE8C030u
#define PC_BLUE     0xFF3C70C0u
#define PC_CELESTE  0xFFA8D8FFu      /* the Argentine flag's light blue (over the darker metal page) */

/* ============================================================================
 * SECTION: source
 * ========================================================================== */

typedef struct {
    double x, z;        /* game frame (place)                                   */
    double h_m;         /* height= when tagged, else 0                          */
    double r_m;         /* fountain basin radius from the mapped polygon, else 0 */
    double id;
    int    kind, small, tall, named;
    int    in_fnt;      /* statue: stands inside a fountain's basin (on a pedestal out of the water) */
    int    has_fig;     /* fountain: a statue stands in it (no centre column / bowl of its own)      */
    unsigned int flag;  /* flagpole: the flag colour                            */
} PzItem;

struct TD5_RoamPlazaSource {
    char    place[64];
    PzItem *it;  int n;
    int     loaded, load_ok;
    int     nskip_file;   /* items of a left-out kind in the whole file (kept, kind PK_SKIP) */
};

int td5_roam_plaza_enabled(void) { return td5_env_int("TD5RE_ROAM_PLAZA_FURN", 1, 0, 1); }

static const char *pz_str(const cJSON *o, const char *k)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return (s && s[0]) ? s : NULL;
}

static int pz_is(const char *s, const char *v) { return s && !strcmp(s, v); }

/* the drawn kind of one record (td5_roam_plaza.h WHAT IS DRAWN / LEFT OUT) */
static int pz_kind_of(const char *kind, const char *sub, const char *name, int *small)
{
    *small = 0;
    if (!kind) return PK_SKIP;
    if (!strcmp(kind, "statue")) { *small = pz_is(sub, "bust"); return PK_STATUE; }
    if (!strcmp(kind, "monument")) return PK_MONUMENT;
    if (!strcmp(kind, "memorial")) {
        if (pz_is(sub, "plaque") || pz_is(sub, "tree") || pz_is(sub, "building") || pz_is(sub, "blue_plaque")
            || pz_is(sub, "stolperstein")) return PK_SKIP;
        if (pz_is(sub, "statue") || pz_is(sub, "sculpture")) return PK_STATUE;
        if (pz_is(sub, "bust")) { *small = 1; return PK_STATUE; }
        if (pz_is(sub, "stone")) { *small = 1; return PK_MEMORIAL; }   /* small = the low rough block */
        return PK_MEMORIAL;                                             /* a stele                     */
    }
    if (!strcmp(kind, "artwork")) {
        if (!sub || pz_is(sub, "sculpture") || pz_is(sub, "statue")) return PK_STATUE;
        if (pz_is(sub, "bust")) { *small = 1; return PK_STATUE; }
        return PK_SKIP;                                                 /* mural, architecture, entrance arch */
    }
    if (!strcmp(kind, "fountain")) return PK_FOUNTAIN;
    if (!strcmp(kind, "bench")) return PK_BENCH;
    if (!strcmp(kind, "playground")) return PK_PLAYGROUND;
    if (!strcmp(kind, "flagpole")) return PK_FLAGPOLE;
    (void)name;
    return PK_SKIP;                                                     /* fitness_station, picnic_table, bandstand, obelisk */
}

static int pz_load(TD5_RoamPlazaSource *p)
{
    char path[512];
    char *json;
    cJSON *root, *arr, *e;
    int cap;
    p->loaded = 1;
    td5_geo_source_path(path, sizeof path, p->place, "PLAZA_FURNITURE.JSON");
    json = td5_geo_roads_slurp(path);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    td5_geo_src_json_to_game(root);          /* [roam M] source frame -> game frame */
    arr = cJSON_GetObjectItem(root, "items");
    cap = arr ? cJSON_GetArraySize(arr) : 0;
    p->it = (PzItem *)calloc((size_t)(cap > 0 ? cap : 1), sizeof(PzItem));
    if (!p->it) { cJSON_Delete(root); return 0; }
    for (e = arr ? arr->child : NULL; e; e = e->next) {
        const cJSON *tags = cJSON_GetObjectItem(e, "tags"), *pts = cJSON_GetObjectItem(e, "points");
        const char *name = pz_str(e, "name");
        PzItem *q = &p->it[p->n];
        double h;
        memset(q, 0, sizeof *q);
        q->kind = pz_kind_of(pz_str(e, "kind"), pz_str(e, "sub"), name, &q->small);
        if (q->kind == PK_SKIP) p->nskip_file++;   /* kept: the owning tile counts it (skip_kind) */
        if (!cJSON_IsNumber(cJSON_GetObjectItem(e, "x")) || !cJSON_IsNumber(cJSON_GetObjectItem(e, "z"))) continue;
        q->x = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "x"));
        q->z = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "z"));
        q->id = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "id"));
        h = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "height_m"));
        q->h_m = (h > 0.0 && h < 60.0) ? h : 0.0;
        q->named = name != NULL;
        q->tall = q->kind == PK_MONUMENT || (name && !strncmp(name, "Monumento", 9));
        if (q->kind == PK_FOUNTAIN && pts && cJSON_GetArraySize(pts) >= 4) {   /* the basin from the mapped ring */
            const cJSON *a, *b;
            double ar = 0.0;
            for (a = pts->child; a && a->next; a = a->next) {
                b = a->next;
                ar += cJSON_GetNumberValue(cJSON_GetObjectItem(a, "x")) * cJSON_GetNumberValue(cJSON_GetObjectItem(b, "z"))
                    - cJSON_GetNumberValue(cJSON_GetObjectItem(b, "x")) * cJSON_GetNumberValue(cJSON_GetObjectItem(a, "z"));
            }
            q->r_m = sqrt(fabs(0.5 * ar) / 3.14159265358979) / PZ_UPM;
            if (q->r_m < 1.5) q->r_m = 1.5;
            if (q->r_m > 12.0) q->r_m = 12.0;
        }
        q->flag = PC_CELESTE;
        if (pz_is(pz_str(tags, "subject"), "Provincia de Buenos Aires")) q->flag = PC_WHITE;
        p->n++;
    }
    cJSON_Delete(root);
    {   /* a statue mapped at a fountain's centre (Plaza Moreno's four seasons: 0.2 - 0.6 m) stands IN it */
        int a, b;
        for (a = 0; a < p->n; a++) {
            PzItem *q = &p->it[a];
            if (q->kind != PK_STATUE && q->kind != PK_MONUMENT) continue;
            for (b = 0; b < p->n; b++) {
                PzItem *f = &p->it[b];
                const double r = PZ_M((f->r_m > 0.0 ? f->r_m : 2.5) - 0.6);
                if (f->kind != PK_FOUNTAIN || f->has_fig) continue;
                if (hypot(f->x - q->x, f->z - q->z) < r) { q->in_fnt = 1; f->has_fig = 1; break; }
            }
        }
    }
    p->load_ok = 1;
    TD5_LOG_I(LOG_TAG, "ROAM_PLAZA layer loaded: %d item(s) of a drawn kind, %d of a kind left out", p->n - p->nskip_file,
              p->nskip_file);
    return 1;
}

TD5_RoamPlazaSource *td5_roam_plaza_open(const char *place)
{
    TD5_RoamPlazaSource *p;
    char path[512];
    FILE *f;
    if (!place || !td5_roam_plaza_enabled()) return NULL;
    td5_geo_source_path(path, sizeof path, place, "PLAZA_FURNITURE.JSON");
    f = fopen(path, "rb");
    if (!f) return NULL;
    fclose(f);
    p = (TD5_RoamPlazaSource *)calloc(1, sizeof *p);
    if (!p) return NULL;
    snprintf(p->place, sizeof p->place, "%s", place);
    return p;
}

void td5_roam_plaza_close(TD5_RoamPlazaSource *p)
{
    if (!p) return;
    free(p->it);
    free(p);
}

/* ============================================================================
 * SECTION: writers
 * ========================================================================== */

typedef struct PzBatch { int nq; float v[PZ_CHUNK * 4 * 5]; unsigned int light[PZ_CHUNK * 4]; } PzBatch;

typedef struct {
    TG_TileCtx *c;
    TD5_RoamPlazaStats *st;
    int32_t ox, oz;
    PzBatch *bat;
    int sb;
    int oom;
} PzEm;

static int pz_page(int pg)
{
    switch (pg) {
    case PP_STONE: return TD5_TG_PAGE_R4_PIER;          /* smooth cast stone          */
    case PP_METAL: return TD5_TG_PAGE_R11_SIGN_POST;    /* brushed metal, tinted      */
    case PP_WATER: return TD5_TG_PAGE_WATER;
    default:       return TD5_TG_PAGE_ROAD_EXTRA + 1;   /* plank road: bench slats   */
    }
}

static int pz_flush(PzEm *e, int sb, int pg)
{
    PzBatch *q = &e->bat[sb * PP_N + pg];
    unsigned short cmd[3];
    TG_Buf *b;
    if (q->nq <= 0) return 1;
    b = (TG_Buf *)roam_ctx_mesh_begin(e->c, sb);
    if (!b) { e->oom = 1; return 0; }
    cmd[0] = (unsigned short)pz_page(pg); cmd[1] = 0; cmd[2] = (unsigned short)q->nq;
    if (!tg_write_prefab_mesh(b, q->v, q->light, q->nq * 4, cmd, 1, 0, 0.0, 0.0, 0.0, 1.0, 0.0) || b->oom) { e->oom = 1; return 0; }
    e->st->meshes++;
    q->nq = 0;
    return 1;
}

/* the guard audit of one face (td5_roam_rails.c rl_face_on_road): its XZ footprint, a wall projecting
 * onto a segment, against the drivable primitives within [ymin - LO, ymax + HI] */
static int pz_face_on_road(PzEm *e, const double *x, const double *y, const double *z)
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
        if (fabs(0.5 * ar) < 4.0) {
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
    return roam_ctx_on_road_band(e->c, fx, fz, m, ymin - PZ_GUARD_LO, ymax + PZ_GUARD_HI);
}

/* one quad (tile-local x/z, absolute y), UVs in metres / tile_m */
static int pz_quad(PzEm *e, int pg, const double *x, const double *y, const double *z, const double *u, const double *v,
                   unsigned int col)
{
    PzBatch *q = &e->bat[e->sb * PP_N + pg];
    int k;
    if (pz_face_on_road(e, x, y, z)) {
        e->st->guard_hits++;
        if (e->st->guard_hits <= 3)
            TD5_LOG_W(LOG_TAG, "ROAM_PLAZA_GUARD page %d face at local (%.0f,%.0f) y %.0f on the drivable surface",
                      pz_page(pg), x[0], z[0], y[0]);
    }
    if (q->nq >= PZ_CHUNK && !pz_flush(e, e->sb, pg)) return 0;
    for (k = 0; k < 4; k++) {
        const int o = (q->nq * 4 + k) * 5;
        q->v[o + 0] = (float)x[k]; q->v[o + 1] = (float)y[k]; q->v[o + 2] = (float)z[k];
        q->v[o + 3] = (float)u[k]; q->v[o + 4] = (float)v[k];
        q->light[q->nq * 4 + k] = col;
    }
    q->nq++;
    return 1;
}

/* ============================================================================
 * SECTION: pieces
 * ========================================================================== */

/* A box: centre (cx, cz), from y0 to y1, half sizes hx along (ux, uz) and hz across; four sides + top.
 * UV: one repeat per `tm` metres. */
static int pz_box(PzEm *e, int pg, double cx, double cz, double y0, double y1, double hx, double hz,
                  double ux, double uz, unsigned int col, double tm)
{
    const double wx = -uz, wz = ux;                 /* across */
    double X[4], Z[4];
    int k;
    const double t = PZ_M(tm);
    X[0] = cx - ux * hx - wx * hz; Z[0] = cz - uz * hx - wz * hz;
    X[1] = cx + ux * hx - wx * hz; Z[1] = cz + uz * hx - wz * hz;
    X[2] = cx + ux * hx + wx * hz; Z[2] = cz + uz * hx + wz * hz;
    X[3] = cx - ux * hx + wx * hz; Z[3] = cz - uz * hx + wz * hz;
    for (k = 0; k < 4; k++) {
        const int j = (k + 1) & 3;
        const double L = hypot(X[j] - X[k], Z[j] - Z[k]) / t, H = (y1 - y0) / t;
        const double x[4] = { X[k], X[j], X[j], X[k] }, y[4] = { y0, y0, y1, y1 }, z[4] = { Z[k], Z[j], Z[j], Z[k] };
        const double u[4] = { 0.0, L, L, 0.0 }, v[4] = { H, H, 0.0, 0.0 };
        if (!pz_quad(e, pg, x, y, z, u, v, col)) return 0;
    }
    {
        const double y[4] = { y1, y1, y1, y1 };
        const double u[4] = { 0.0, 2.0 * hx / t, 2.0 * hx / t, 0.0 }, v[4] = { 0.0, 0.0, 2.0 * hz / t, 2.0 * hz / t };
        return pz_quad(e, pg, X, y, Z, u, v, col);
    }
}

/* An n-gon frustum (n even, 4..16): radius r0 at y0, r1 at y1, rotated `rot` rad; sides, + the top cap
 * (cap = 1) as n/2 quads (centre, v, v+1, v+2) */
static int pz_prism(PzEm *e, int pg, double cx, double cz, double y0, double y1, double r0, double r1, int n,
                    double rot, unsigned int col, int cap, double tm)
{
    double ax[16], az[16], bx[16], bz[16];
    const double t = PZ_M(tm);
    int k;
    if (n > 16) n = 16;
    for (k = 0; k < n; k++) {
        const double a = rot + 6.283185307179586 * k / n;
        ax[k] = cx + cos(a) * r0; az[k] = cz + sin(a) * r0;
        bx[k] = cx + cos(a) * r1; bz[k] = cz + sin(a) * r1;
    }
    for (k = 0; k < n; k++) {
        const int j = (k + 1) % n;
        const double L = hypot(ax[j] - ax[k], az[j] - az[k]) / t, H = (y1 - y0) / t;
        const double x[4] = { ax[k], ax[j], bx[j], bx[k] }, y[4] = { y0, y0, y1, y1 }, z[4] = { az[k], az[j], bz[j], bz[k] };
        const double u[4] = { (double)k * L, (double)(k + 1) * L, (double)(k + 1) * L, (double)k * L };
        const double v[4] = { H, H, 0.0, 0.0 };
        if (!pz_quad(e, pg, x, y, z, u, v, col)) return 0;
    }
    if (cap && r1 > 0.0)
        for (k = 0; k < n; k += 2) {
            const int j = (k + 1) % n, l = (k + 2) % n;
            const double x[4] = { cx, bx[k], bx[j], bx[l] }, y[4] = { y1, y1, y1, y1 }, z[4] = { cz, bz[k], bz[j], bz[l] };
            double u[4], v[4];
            int q;
            for (q = 0; q < 4; q++) { u[q] = (x[q] - cx) / t; v[q] = (z[q] - cz) / t; }
            if (!pz_quad(e, pg, x, y, z, u, v, col)) return 0;
        }
    return 1;
}

/* a flat n-gon at height y (the fountain's water) */
static int pz_disc(PzEm *e, int pg, double cx, double cz, double y, double r, int n, double rot, unsigned int col, double tm)
{
    return pz_prism(e, pg, cx, cz, y, y, r, r, n, rot, col, 1, tm) ;
}

/* the shipped bronze figure (k_prop_pages[PP_MONUMENT], alpha-keyed, camera-facing), standing at (x, y, z),
 * `h` tall at the page's aspect */
static int pz_figure(PzEm *e, double x, double y, double z, double h)
{
    const TG_PropPage *pp = &k_prop_pages[PP_MONUMENT];
    const double hw = 0.5 * h * (double)pp->w / (double)pp->h;
    TG_Buf *b = (TG_Buf *)roam_ctx_mesh_begin(e->c, e->sb);
    if (!b) { e->oom = 1; return 0; }
    if (!tg_emit_billboard_mesh(b, x, y, z, hw, h, tg_prop_slot(PP_MONUMENT), pp->tag) || b->oom) { e->oom = 1; return 0; }
    e->st->meshes++;
    return 1;
}

/* ============================================================================
 * SECTION: kinds
 * ========================================================================== */

/* the base height under (x, z): the plaza lawn, else the pavement slab, else the drawn ground */
static double pz_base(TG_TileCtx *c, double x, double z)
{
    double y;
    if (roam_ctx_lawn_y(c, x, z, &y)) return y;
    if (roam_ctx_slab_y(c, x, z, &y)) return y;
    return roam_ctx_ground(c, x, z);
}

/* the footprint half size (m) an item needs clear of the road */
static double pz_reach_m(const PzItem *q)
{
    switch (q->kind) {
    case PK_STATUE:     return q->small ? 0.8 : (q->tall ? 1.8 : 1.3);
    case PK_MONUMENT:   return 1.8;
    case PK_MEMORIAL:   return q->small ? 0.7 : 0.8;
    case PK_FOUNTAIN:   return (q->r_m > 0.0 ? q->r_m : 2.5) + 0.2;
    case PK_BENCH:      return 1.0;
    case PK_PLAYGROUND: return 3.0;
    default:            return 0.6;
    }
}

/* statue / monument: plinth + base course + cornice + the figure; a height= tag sets the total */
static int pz_statue(PzEm *e, const PzItem *q, double x, double z, double by, double ux, double uz)
{
    double side = q->small ? 0.8 : (q->tall ? 3.0 : 2.0), ph = q->small ? 1.3 : (q->tall ? 4.2 : 2.2);
    double fh = q->small ? 1.1 : (q->tall ? 4.0 : 2.8);
    if (q->h_m > 0.0) { const double s = q->h_m / (ph + fh); ph *= s; fh *= s; }
    if (q->in_fnt) {   /* a pedestal rising out of the basin's water (pz_fountain: water at +0.40 m) */
        if (!pz_box(e, PP_STONE, x, z, by + PZ_M(0.30), by + PZ_M(1.7), PZ_M(0.55), PZ_M(0.55), ux, uz, PC_STONE, 1.0)) return 0;
        if (!pz_box(e, PP_STONE, x, z, by + PZ_M(1.7), by + PZ_M(1.85), PZ_M(0.68), PZ_M(0.68), ux, uz, PC_STONE_D, 1.0)) return 0;
        return pz_figure(e, x, by + PZ_M(1.85), z, PZ_M(fh * 0.85));
    }
    if (!pz_box(e, PP_STONE, x, z, by - PZ_SINK, by + PZ_M(0.35), PZ_M(side * 0.5 + 0.25), PZ_M(side * 0.5 + 0.25), ux, uz,
                PC_STONE_D, 1.0)) return 0;
    if (!pz_box(e, PP_STONE, x, z, by + PZ_M(0.35), by + PZ_M(ph - 0.25), PZ_M(side * 0.5), PZ_M(side * 0.5), ux, uz,
                PC_STONE, 1.0)) return 0;
    if (!pz_box(e, PP_STONE, x, z, by + PZ_M(ph - 0.25), by + PZ_M(ph), PZ_M(side * 0.5 + 0.12), PZ_M(side * 0.5 + 0.12), ux, uz,
                PC_STONE_D, 1.0)) return 0;
    return pz_figure(e, x, by + PZ_M(ph), z, PZ_M(fh));
}

/* memorial: a stele (1.4 x 0.5 x 1.8 m on a step) or, small, the low rough foundation stone */
static int pz_memorial(PzEm *e, const PzItem *q, double x, double z, double by, double ux, double uz)
{
    if (q->small)
        return pz_box(e, PP_STONE, x, z, by - PZ_SINK, by + PZ_M(q->h_m > 0.0 ? q->h_m : 0.9), PZ_M(0.65), PZ_M(0.5), ux, uz,
                      PC_GRANITE, 1.0);
    if (!pz_box(e, PP_STONE, x, z, by - PZ_SINK, by + PZ_M(0.2), PZ_M(0.8), PZ_M(0.4), ux, uz, PC_STONE_D, 1.0)) return 0;
    return pz_box(e, PP_STONE, x, z, by + PZ_M(0.2), by + PZ_M(q->h_m > 0.0 ? q->h_m : 1.8), PZ_M(0.7), PZ_M(0.25), ux, uz,
                  PC_STONE, 1.0);
}

/* fountain: octagonal basin wall (outer face, rim, inner face), water, centre column and bowl */
static int pz_fountain(PzEm *e, const PzItem *q, double x, double z, double by)
{
    const double R = PZ_M(q->r_m > 0.0 ? q->r_m : 2.5), T = PZ_M(0.3), wall = by + PZ_M(0.55), rot = 0.3926990817;
    const double cr = R * 0.12 < PZ_M(0.3) ? PZ_M(0.3) : (R * 0.12 > PZ_M(0.8) ? PZ_M(0.8) : R * 0.12);
    if (!pz_prism(e, PP_STONE, x, z, by - PZ_SINK, wall, R, R, 8, rot, PC_STONE, 0, 1.0)) return 0;
    if (!pz_prism(e, PP_STONE, x, z, wall, wall, R, R - T, 8, rot, PC_STONE_D, 0, 1.0)) return 0;        /* the rim */
    if (!pz_prism(e, PP_STONE, x, z, by + PZ_M(0.40), wall, R - T, R - T, 8, rot, PC_STONE, 0, 1.0)) return 0;
    if (!pz_disc(e, PP_WATER, x, z, by + PZ_M(0.40), R - T, 8, rot, PC_WATER, 2.0)) return 0;
    if (q->has_fig) return 1;                    /* the statue in it is its centrepiece (pz_statue in_fnt) */
    if (!pz_prism(e, PP_STONE, x, z, by + PZ_M(0.40), by + PZ_M(1.6), cr, cr * 0.8, 8, rot, PC_STONE, 0, 1.0)) return 0;
    if (!pz_prism(e, PP_STONE, x, z, by + PZ_M(1.6), by + PZ_M(1.9), cr * 0.8, cr * 2.6, 8, rot, PC_STONE_D, 0, 1.0)) return 0;
    return pz_disc(e, PP_WATER, x, z, by + PZ_M(1.88), cr * 2.4, 8, rot, PC_WATER, 2.0);
}

/* bench: seat, backrest on the (bx, bz) side, two cast legs; long axis (ux, uz) */
static int pz_bench(PzEm *e, double x, double z, double by, double ux, double uz, double bx, double bz)
{
    const double lx = -uz, lz = ux;
    int k;
    for (k = -1; k <= 1; k += 2)
        if (!pz_box(e, PP_METAL, x + ux * PZ_M(0.75) * k, z + uz * PZ_M(0.75) * k, by - PZ_M(0.05), by + PZ_M(0.42),
                    PZ_M(0.04), PZ_M(0.24), ux, uz, PC_IRON, 1.0)) return 0;
    if (!pz_box(e, PP_WOOD, x, z, by + PZ_M(0.40), by + PZ_M(0.46), PZ_M(0.9), PZ_M(0.22), ux, uz, PC_WOOD, 1.0)) return 0;
    {
        const double s = (bx * lx + bz * lz) >= 0.0 ? 1.0 : -1.0;
        return pz_box(e, PP_WOOD, x + lx * PZ_M(0.21) * s, z + lz * PZ_M(0.21) * s, by + PZ_M(0.55), by + PZ_M(0.85),
                      PZ_M(0.9), PZ_M(0.03), ux, uz, PC_WOOD, 1.0);
    }
}

/* playground: a swing frame (two posts each end, top bar) and a slide tower with its chute */
static int pz_playground(PzEm *e, double x, double z, double by, double ux, double uz)
{
    const double lx = -uz, lz = ux;
    const double sx = x - lx * PZ_M(1.2), sz = z - lz * PZ_M(1.2);     /* swings */
    const double tx = x + lx * PZ_M(1.4), tz = z + lz * PZ_M(1.4);     /* tower  */
    int i, j;
    for (i = -1; i <= 1; i += 2)
        for (j = -1; j <= 1; j += 2)
            if (!pz_box(e, PP_METAL, sx + ux * PZ_M(1.5) * i + lx * PZ_M(0.5) * j, sz + uz * PZ_M(1.5) * i + lz * PZ_M(0.5) * j,
                        by - PZ_M(0.1), by + PZ_M(2.2), PZ_M(0.05), PZ_M(0.05), ux, uz, PC_RED, 1.0)) return 0;
    if (!pz_box(e, PP_METAL, sx, sz, by + PZ_M(2.2), by + PZ_M(2.3), PZ_M(1.55), PZ_M(0.55), ux, uz, PC_YELLOW, 1.0)) return 0;
    for (i = -1; i <= 1; i += 2) {                                       /* two seats */
        const double qx = sx + ux * PZ_M(0.6) * i, qz = sz + uz * PZ_M(0.6) * i;
        if (!pz_box(e, PP_METAL, qx, qz, by + PZ_M(0.45), by + PZ_M(0.5), PZ_M(0.22), PZ_M(0.12), ux, uz, PC_IRON, 1.0)) return 0;
    }
    for (i = -1; i <= 1; i += 2)
        for (j = -1; j <= 1; j += 2)
            if (!pz_box(e, PP_METAL, tx + ux * PZ_M(0.55) * i + lx * PZ_M(0.55) * j, tz + uz * PZ_M(0.55) * i + lz * PZ_M(0.55) * j,
                        by - PZ_M(0.1), by + PZ_M(1.9), PZ_M(0.05), PZ_M(0.05), ux, uz, PC_BLUE, 1.0)) return 0;
    if (!pz_box(e, PP_WOOD, tx, tz, by + PZ_M(1.2), by + PZ_M(1.28), PZ_M(0.6), PZ_M(0.6), ux, uz, PC_WOOD, 1.0)) return 0;
    {   /* the chute: from the platform edge down 2.4 m along +u */
        const double ax = tx + ux * PZ_M(0.6), az = tz + uz * PZ_M(0.6), cx = tx + ux * PZ_M(3.0), cz = tz + uz * PZ_M(3.0);
        const double x[4] = { ax - lx * PZ_M(0.3), ax + lx * PZ_M(0.3), cx + lx * PZ_M(0.3), cx - lx * PZ_M(0.3) };
        const double z[4] = { az - lz * PZ_M(0.3), az + lz * PZ_M(0.3), cz + lz * PZ_M(0.3), cz - lz * PZ_M(0.3) };
        const double y[4] = { by + PZ_M(1.25), by + PZ_M(1.25), by + PZ_M(0.15), by + PZ_M(0.15) };
        const double u[4] = { 0, 1, 1, 0 }, v[4] = { 0, 0, 3, 3 };
        return pz_quad(e, PP_METAL, x, y, z, u, v, PC_YELLOW);
    }
}

/* flagpole: an octagonal white pole on a stone step, the flag at the top along (ux, uz) */
static int pz_flagpole(PzEm *e, const PzItem *q, double x, double z, double by, double ux, double uz)
{
    const double H = PZ_M(q->h_m > 0.0 ? q->h_m : 12.0);
    if (!pz_box(e, PP_STONE, x, z, by - PZ_SINK, by + PZ_M(0.4), PZ_M(0.7), PZ_M(0.7), ux, uz, PC_STONE, 1.0)) return 0;
    if (!pz_prism(e, PP_METAL, x, z, by + PZ_M(0.4), by + H, PZ_M(0.09), PZ_M(0.05), 8, 0.0, PC_WHITE, 1, 1.0)) return 0;
    {
        const double fy1 = by + H - PZ_M(0.3), fy0 = fy1 - PZ_M(1.2);
        const double fx[4] = { x, x + ux * PZ_M(1.8), x + ux * PZ_M(1.8), x }, fz[4] = { z, z + uz * PZ_M(1.8), z + uz * PZ_M(1.8), z };
        const double fy[4] = { fy0, fy0, fy1, fy1 }, u[4] = { 0, 1, 1, 0 }, v[4] = { 1, 1, 0, 0 };
        return pz_quad(e, PP_METAL, fx, fy, fz, u, v, q->flag);
    }
}

/* ============================================================================
 * SECTION: emit
 * ========================================================================== */

int td5_roam_plaza_emit(TD5_RoamPlazaSource *p, TG_TileCtx *c, TD5_RoamPlazaStats *st)
{
    PzEm e;
    int i, sb, pg, tx, tz;
    if (!p || !c || !st) return 1;
    if (!p->loaded && !pz_load(p)) TD5_LOG_W(LOG_TAG, "roam plaza: PLAZA_FURNITURE.JSON of %s did not load: no plaza furniture", p->place);
    if (!p->load_ok) return 1;
    memset(&e, 0, sizeof e);
    e.c = c; e.st = st;
    roam_ctx_origin(c, &tx, &tz, &e.ox, &e.oz);
    e.bat = (PzBatch *)calloc((size_t)TD5_ROAM_SCN_NSB * PP_N, sizeof(PzBatch));
    if (!e.bat) return 0;
    for (i = 0; i < p->n && !e.oom; i++) {
        const PzItem *q = &p->it[i];
        const double x = q->x - (double)e.ox, z = q->z - (double)e.oz;
        const double r = PZ_M(pz_reach_m(q));
        double ux, uz, by, bx = 0.0, bz = 0.0;
        int ok = 1;
        if (!roam_ctx_owned(c, x, z)) continue;
        if (q->kind == PK_SKIP) { st->skip_kind++; continue; }
        st->items++;
        {
            const double qx[4] = { x - r, x + r, x + r, x - r }, qz[4] = { z - r, z - r, z + r, z + r };
            if (roam_ctx_on_road(c, qx, qz, 4)) { st->drop_road++; continue; }
        }
        {   /* the long axis: parallel to the nearest street (benches face away from it), else by the id */
            int ed = -1, sd = 0;
            double al = 0.0, lat = 0.0;
            RoamFrame fr;
            if (roam_nearest_edge(c, x, z, PZ_M(80.0), &ed, &sd, &al, &lat) && roam_edge_frame(c, ed, 1, al, &fr)) {
                ux = fr.tx; uz = fr.tz;
                bx = fr.px - x; bz = fr.pz - z;
            } else {
                const double a = fmod(q->id, 360.0) * 0.017453292519943295;
                ux = cos(a); uz = sin(a);
            }
        }
        by = pz_base(c, x, z);
        e.sb = roam_ctx_sb_of(c, x, z);
        switch (q->kind) {
        case PK_STATUE:     ok = pz_statue(&e, q, x, z, by, ux, uz); st->statues++; break;
        case PK_MONUMENT:   ok = pz_statue(&e, q, x, z, by, ux, uz); st->monuments++; break;
        case PK_MEMORIAL:   ok = pz_memorial(&e, q, x, z, by, ux, uz); st->memorials++; break;
        case PK_FOUNTAIN:   ok = pz_fountain(&e, q, x, z, by); st->fountains++; break;
        case PK_BENCH:      ok = pz_bench(&e, x, z, by, ux, uz, bx, bz); st->benches++; break;
        case PK_PLAYGROUND: ok = pz_playground(&e, x, z, by, ux, uz); st->playgrounds++; break;
        case PK_FLAGPOLE:   ok = pz_flagpole(&e, q, x, z, by, ux, uz); st->flagpoles++; break;
        default:            break;
        }
        if (!ok) e.oom = 1;
        if (q->named) st->named++;
    }
    for (sb = 0; sb < TD5_ROAM_SCN_NSB && !e.oom; sb++)
        for (pg = 0; pg < PP_N && !e.oom; pg++)
            if (!pz_flush(&e, sb, pg)) e.oom = 1;
    free(e.bat);
    return e.oom ? 0 : 1;
}
