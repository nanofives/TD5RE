/**
 * td5_roam_tile.c -- GEO FREE ROAM: tile assembly + the ROAMTILE file
 * (PORT-ONLY, F1 task T5). Contract, file layout and deviations: td5_roam_tile.h.
 *
 * SECTIONS: lattice, file image (writer / reader), builder context, working set
 * (graph + heights + ribbons for one window), slice (ownership + relocation into
 * the tile table), serialisation, casco tile list, dev CLI.
 *
 * DETERMINISM (design 1.10). Every loop is over a sorted array (edges and
 * clusters are key-sorted by T3), every stored integer is formed with the explicit
 * td5_roam_quant / roam_round, no clock, pointer or load order is read, the height
 * context is opened fresh per build (so which raster window answers a sample never
 * depends on the LRU history), and a worker thread applies the main thread's FP
 * environment first (td5_plat_fpenv_apply). The elapsed-ms numbers are logged and
 * never stored in the file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <direct.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_geo_sidewalk.h"    /* td5_geo_sw_place */
#include "td5_roam_cache.h"
#include "td5_roam_height.h"
#include "td5_roam_world.h"
#include "td5_roam_junction.h"
#include "td5_roam_ribbon.h"
#include "td5_roam_tile.h"
#include "td5_geo_frame.h"       /* [roam M] the geographic -> game reflection */
#include "deps/cjson/cJSON.h"

#define LOG_TAG "roam"

#define FNV_INIT 2166136261u
#define ROAM_PAD_TOUCH_M   100.0     /* an edge "touches" the tile when its box meets the
                                      * tile box grown by this (an arm leaves a cluster
                                      * whose centroid is inside the tile, <= 60 m)     */
#define ROAM_GROW_PASSES   4
#define ROAM_HC_PLANE      1         /* [F1-T10] sloped clusters take a plane (work_hc); in the stamp */

static uint32_t fnv_add(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}
static uint32_t fnv_u32(uint32_t h, uint32_t v) { return fnv_add(h, &v, sizeof v); }
static uint32_t fnv_i64(uint32_t h, int64_t v)  { return fnv_add(h, &v, sizeof v); }
static uint32_t fnv_dbl(uint32_t h, double v)   { return fnv_add(h, &v, sizeof v); }

/* ----------------------------------------------------------- growable vec ---- */

typedef struct { uint8_t *p; size_t n, cap, esz; int oom; } Vec;

static void vec_init(Vec *v, size_t esz) { memset(v, 0, sizeof *v); v->esz = esz; }
static void vec_free(Vec *v) { free(v->p); memset(v, 0, sizeof *v); }
/* Append one zeroed element; NULL (and v->oom) on out of memory. */
static void *vec_add(Vec *v)
{
    if (v->n == v->cap) {
        const size_t nc = v->cap ? v->cap * 2 : 64;
        uint8_t *np = (uint8_t *)realloc(v->p, nc * v->esz);
        if (!np) { v->oom = 1; return NULL; }
        v->p = np;
        v->cap = nc;
    }
    memset(v->p + v->n * v->esz, 0, v->esz);
    return v->p + (v->n++) * v->esz;
}
#define VEC_AT(v, T, i) (((T *)(v).p)[i])

/* ============================================================================
 * SECTION: lattice
 * ========================================================================== */

void roam_grid_uv(const RoamGrid *g, double x, double z, double *u, double *v)
{
    const double dx = x - g->ox, dz = z - g->oz;
    *u = g->c * dx + g->s * dz;
    *v = g->c * dz - g->s * dx;
}

void roam_grid_tile_of(const RoamGrid *g, double x, double z, int *tx, int *tz)
{
    double u, v;
    roam_grid_uv(g, x, z, &u, &v);
    *tx = (int)floor(u / (double)g->tile);
    *tz = (int)floor(v / (double)g->tile);
}

void roam_grid_corners(const RoamGrid *g, int tx, int tz, double out[4][2])
{
    const double S = (double)g->tile;
    static const int cu[4] = { 0, 1, 1, 0 }, cv[4] = { 0, 0, 1, 1 };
    int i;
    for (i = 0; i < 4; i++) {
        const double u = ((double)tx + (double)cu[i]) * S;
        const double v = ((double)tz + (double)cv[i]) * S;
        out[i][0] = g->ox + g->c * u - g->s * v;
        out[i][1] = g->oz + g->s * u + g->c * v;
    }
}

void roam_grid_origin(const RoamGrid *g, int tx, int tz, int32_t *ox, int32_t *oz)
{
    double c[4][2];
    roam_grid_corners(g, tx, tz, c);
    *ox = td5_roam_quant(c[0][0]);
    *oz = td5_roam_quant(c[0][1]);
}

/* [roam M] The casco axes run at -42.13 deg in the SOURCE frame (x = east); the game frame reflects
 * x (td5_geo_frame.h), which turns them to +42.13 deg. The reflected lattice covers exactly the same
 * squares on the ground: u' = -u, v' = v, so tile (tx, tz) is now (-tx - 1, tz). */
#define ROAM_GRID_ANGLE_DEG 42.13

void roam_grid_default(const char *place, RoamGrid *g)
{
    const double pi = 3.14159265358979323846;
    const double rad = ROAM_GRID_ANGLE_DEG * pi / 180.0;
    memset(g, 0, sizeof *g);
    snprintf(g->place, sizeof g->place, "%s", place ? place : "");
    g->ox = 0.0;
    g->oz = 0.0;
    g->angle_deg = ROAM_GRID_ANGLE_DEG;
    g->c = cos(rad);
    g->s = sin(rad);
    g->tile = ROAM_TILE_UNITS;
}

int roam_grid_format(const RoamGrid *g, char *out, size_t cap)
{
    return snprintf(out, cap,
        "{\n"
        " \"format\": \"" ROAM_GRID_FORMAT "\",\n"
        " \"place\": \"%s\",\n"
        " \"tile_units\": %d,\n"
        " \"origin\": [%.17g, %.17g],\n"
        " \"angle_deg\": %.17g,\n"
        " \"c\": %.17g,\n"
        " \"s\": %.17g,\n"
        " \"note\": \"u = c*(x-ox) + s*(z-oz), v = c*(z-oz) - s*(x-ox); tx = floor(u/tile_units), "
        "tz = floor(v/tile_units); game frame x = -east (td5_geo_frame.h), casco axes +42.13 deg, design 1.8\"\n"
        "}\n",
        g->place, (int)g->tile, g->ox, g->oz, g->angle_deg, g->c, g->s);
}

int roam_grid_parse(const char *json, RoamGrid *g)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *o, *c, *s, *t, *a, *p;
    int ok = 0;
    if (!root) return 0;
    memset(g, 0, sizeof *g);
    o = cJSON_GetObjectItem(root, "origin");
    c = cJSON_GetObjectItem(root, "c");
    s = cJSON_GetObjectItem(root, "s");
    t = cJSON_GetObjectItem(root, "tile_units");
    a = cJSON_GetObjectItem(root, "angle_deg");
    p = cJSON_GetObjectItem(root, "place");
    if (o && cJSON_IsArray(o) && cJSON_GetArraySize(o) == 2 && cJSON_IsNumber(c) &&
        cJSON_IsNumber(s) && cJSON_IsNumber(t) && t->valuedouble >= 1000.0) {
        g->ox = cJSON_GetArrayItem(o, 0)->valuedouble;
        g->oz = cJSON_GetArrayItem(o, 1)->valuedouble;
        g->c = c->valuedouble;
        g->s = s->valuedouble;
        g->tile = (int32_t)t->valuedouble;
        g->angle_deg = cJSON_IsNumber(a) ? a->valuedouble : 0.0;
        if (cJSON_IsString(p) && p->valuestring) snprintf(g->place, sizeof g->place, "%s", p->valuestring);
        {   /* [roam M] a TD5ROAMGRID1 lattice was written in the SOURCE frame (x = east): reflected
             * into the game frame it covers the same squares on the ground (td5_roam_grid_ensure
             * rewrites the file once) */
            const cJSON *fm = cJSON_GetObjectItem(root, "format");
            if (!(cJSON_IsString(fm) && fm->valuestring && strcmp(fm->valuestring, ROAM_GRID_FORMAT) == 0)) {
                g->ox = td5_geo_game_x(g->ox);
                g->s = -g->s;
                g->angle_deg = -g->angle_deg;
                g->src_frame = 1;
            }
        }
        ok = 1;
    }
    cJSON_Delete(root);
    return ok;
}

/* ============================================================================
 * SECTION: casco polygon -> tile list
 * ========================================================================== */

/* F0a's casco polygon (_archive/roam/F0a/casco_poly.json, place frame): the four
 * ring-avenue corners, the same quad td5_roam_junction.c and td5_roam_cache.c use. */
static const double k_casco[4][2] = {
    { -1078705.99,        5374175.73 },     /* [roam M] game frame: x = -east */
    { -2582491.695195345, 7019045.535332896 },
    { -4255470.01,        5615132.29 },
    { -2731713.44,        3880755.48 },
};

int td5_roam_casco_tiles(const RoamGrid *g, int (*out)[2], int cap)
{
    double pu[4], pv[4], umin, umax, vmin, vmax;
    const double S = (double)g->tile;
    int i, tx, tz, tx0, tx1, tz0, tz1, n = 0;
    for (i = 0; i < 4; i++) roam_grid_uv(g, k_casco[i][0], k_casco[i][1], &pu[i], &pv[i]);
    umin = umax = pu[0];
    vmin = vmax = pv[0];
    for (i = 1; i < 4; i++) {
        if (pu[i] < umin) umin = pu[i];
        if (pu[i] > umax) umax = pu[i];
        if (pv[i] < vmin) vmin = pv[i];
        if (pv[i] > vmax) vmax = pv[i];
    }
    tx0 = (int)floor(umin / S); tx1 = (int)floor(umax / S);
    tz0 = (int)floor(vmin / S); tz1 = (int)floor(vmax / S);
    for (tx = tx0; tx <= tx1; tx++) {
        for (tz = tz0; tz <= tz1; tz++) {
            const double ru0 = (double)tx * S, ru1 = ru0 + S, rv0 = (double)tz * S, rv1 = rv0 + S;
            int sep = 0;
            if (ru1 < umin || ru0 > umax || rv1 < vmin || rv0 > vmax) sep = 1;
            /* the polygon's own edge normals (SAT; the polygon is convex) */
            for (i = 0; i < 4 && !sep; i++) {
                const int j = (i + 1) & 3;
                const double ex = pu[j] - pu[i], ez = pv[j] - pv[i];
                const double nx = -ez, nz = ex;
                double rmin, rmax, qmin, qmax;
                int k;
                const double rc[4][2] = { { ru0, rv0 }, { ru1, rv0 }, { ru1, rv1 }, { ru0, rv1 } };
                rmin = rmax = rc[0][0] * nx + rc[0][1] * nz;
                for (k = 1; k < 4; k++) {
                    const double d = rc[k][0] * nx + rc[k][1] * nz;
                    if (d < rmin) rmin = d;
                    if (d > rmax) rmax = d;
                }
                qmin = qmax = pu[0] * nx + pv[0] * nz;
                for (k = 1; k < 4; k++) {
                    const double d = pu[k] * nx + pv[k] * nz;
                    if (d < qmin) qmin = d;
                    if (d > qmax) qmax = d;
                }
                if (rmax < qmin || qmax < rmin) sep = 1;
            }
            if (sep) continue;
            if (n < cap) { out[n][0] = tx; out[n][1] = tz; }
            n++;
        }
    }
    return n < cap ? n : cap;
}

/* ============================================================================
 * SECTION: the file image (reader / writer)
 * ========================================================================== */

#pragma pack(push, 1)
typedef struct {
    char     magic[8];
    uint32_t version, rules, input_hash;
    int32_t  tx, tz, org_x, org_z;
    uint32_t nsect;
    uint32_t reserved[6];
} RoamFileHeader;                                    /* 64 B */
typedef struct { uint32_t tag, offset, bytes, count; } RoamSectEnt;
#pragma pack(pop)
_Static_assert(sizeof(RoamFileHeader) == 64, "ROAMTILE header is 64 bytes");

static const uint32_t k_sect_tags[ROAM_NSECT] = {
    ROAM_SECT_SPAN, ROAM_SECT_VERT, ROAM_SECT_TRI, ROAM_SECT_BEDG, ROAM_SECT_EDGE,
    ROAM_SECT_NODE, ROAM_SECT_ARM, ROAM_SECT_GRID, ROAM_SECT_SIGN, ROAM_SECT_NAME,
    ROAM_SECT_STMP
};

static size_t round16(size_t v) { return (v + 15u) & ~(size_t)15u; }

void td5_roam_tile_free(RoamTile *t)
{
    if (!t) return;
    free(t->arena);
    memset(t, 0, sizeof *t);
}

int td5_roam_tile_from_image(RoamTile *t, uint8_t *image, size_t bytes,
                             uint32_t expect_hash, char *err, size_t errn)
{
    const RoamFileHeader *h = (const RoamFileHeader *)image;
    const RoamSectEnt *se;
    uint32_t i, trailer, want;
#define FAIL(...) do { if (err && errn) snprintf(err, errn, __VA_ARGS__); return 0; } while (0)
    if (!image || bytes < sizeof(RoamFileHeader) + 4u) FAIL("file too short (%u bytes)", (unsigned)bytes);
    if (memcmp(h->magic, "TD5ROAM1", 8) != 0) FAIL("bad magic");
    if (h->version != ROAM_FORMAT_VERSION) FAIL("format version %u, want %u", h->version, ROAM_FORMAT_VERSION);
    if (h->rules != ROAM_RULES_VERSION) FAIL("rules version %u, want %u", h->rules, ROAM_RULES_VERSION);
    if (expect_hash && h->input_hash != expect_hash)
        FAIL("input hash %08x, want %08x", h->input_hash, expect_hash);
    if (h->nsect != ROAM_NSECT) FAIL("%u sections, want %d", h->nsect, ROAM_NSECT);
    if (bytes < sizeof(RoamFileHeader) + (size_t)h->nsect * sizeof(RoamSectEnt) + 4u)
        FAIL("section table does not fit");
    memcpy(&trailer, image + bytes - 4u, 4u);
    want = fnv_add(FNV_INIT, image, bytes - 4u);
    if (trailer != want) FAIL("trailer fnv %08x, computed %08x", trailer, want);
    se = (const RoamSectEnt *)(image + sizeof(RoamFileHeader));
    memset(t, 0, sizeof *t);
    for (i = 0; i < ROAM_NSECT; i++) {
        if (se[i].tag != k_sect_tags[i]) FAIL("section %u has tag %08x", i, se[i].tag);
        if ((size_t)se[i].offset + se[i].bytes > bytes - 4u) FAIL("section %u runs past the file", i);
        if ((se[i].offset & 15u) != 0) FAIL("section %u is not 16-byte aligned", i);
    }
#define SECT(i, T, ptr, n, esz) do { \
        if ((size_t)se[i].count * (esz) != se[i].bytes) FAIL("section %u: %u x %u != %u bytes", \
            (unsigned)(i), (unsigned)se[i].count, (unsigned)(esz), (unsigned)se[i].bytes); \
        (ptr) = (T *)(image + se[i].offset); (n) = se[i].count; } while (0)
    SECT(0, TD5_StripSpan, t->span, t->nspan, sizeof(TD5_StripSpan));
    SECT(1, TD5_StripVertex, t->vert, t->nvert, sizeof(TD5_StripVertex));
    SECT(2, RoamTri, t->tri, t->ntri, sizeof(RoamTri));
    SECT(3, RoamBEdge, t->bedge, t->nbedge, sizeof(RoamBEdge));
    SECT(4, RoamEdgeRec, t->edge, t->nedge, sizeof(RoamEdgeRec));
    SECT(5, RoamNodeRec, t->node, t->nnode, sizeof(RoamNodeRec));
    SECT(6, RoamArmRec, t->arm, t->narm, sizeof(RoamArmRec));
    SECT(8, RoamSignRec, t->sign, t->nsign, sizeof(RoamSignRec));
#undef SECT
    /* GRID: 5 words, start[nx*nz+1], items */
    if (se[7].bytes < 20u + 4u) FAIL("GRID section too short");
    t->grid = (uint32_t *)(image + se[7].offset);
    t->ngrid_words = se[7].bytes / 4u;
    t->grid_nx = t->grid[0];
    t->grid_nz = t->grid[1];
    t->grid_cell = t->grid[2];
    t->grid_margin = t->grid[3];
    if ((uint64_t)t->grid_nx * t->grid_nz + 1u + 5u > t->ngrid_words) FAIL("GRID start[] does not fit");
    t->grid_start = t->grid + 5;
    t->grid_item = t->grid + 5 + (size_t)t->grid_nx * t->grid_nz + 1u;
    t->nitem = t->ngrid_words - 5u - t->grid_nx * t->grid_nz - 1u;
    t->names = (const char *)(image + se[9].offset);
    t->names_bytes = se[9].bytes;
    t->nnames = se[9].count;
    t->stamp_text = (const char *)(image + se[10].offset);
    t->tx = h->tx; t->tz = h->tz;
    t->org_x = h->org_x; t->org_z = h->org_z;
    t->rules = h->rules; t->stamp = h->input_hash;
    t->arena = image;
    t->arena_bytes = bytes;
#undef FAIL
    return 1;
}

int td5_roam_mkdirs(const char *dir)
{
    char buf[512];
    size_t i, n;
    if (!dir || !dir[0]) return 0;
    n = strlen(dir);
    if (n >= sizeof buf) return 0;
    memcpy(buf, dir, n + 1u);
    for (i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\\' || buf[i] == '\0') {
            const char sv = buf[i];
            buf[i] = '\0';
            if (!(i == 2 && buf[1] == ':')) _mkdir(buf);
            buf[i] = sv;
        }
    }
    return 1;
}

void td5_roam_tile_path(char *out, size_t cap, const char *dir, const char *place,
                        int tx, int tz)
{
    if (dir && dir[0]) {
        const size_t n = strlen(dir);
        const int has_sep = (dir[n - 1] == '/' || dir[n - 1] == '\\');
        snprintf(out, cap, "%s%sROAMTILE_%d_%d.BIN", dir, has_sep ? "" : "/", tx, tz);
    } else {
        char rel[64];
        snprintf(rel, sizeof rel, "roam/ROAMTILE_%d_%d.BIN", tx, tz);
        td5_geo_source_path(out, cap, place, rel);
    }
}

int td5_roam_tile_write(const char *path, const RoamTile *t)
{
    char dir[512];
    FILE *f;
    size_t n, k;
    if (!t || !t->arena) return 0;
    n = strlen(path);
    if (n >= sizeof dir) return 0;
    memcpy(dir, path, n + 1u);
    for (k = n; k > 0; k--)
        if (dir[k - 1] == '/' || dir[k - 1] == '\\') { dir[k - 1] = '\0'; break; }
    if (k > 0) td5_roam_mkdirs(dir);
    f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(t->arena, 1, t->arena_bytes, f) != t->arena_bytes) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

int td5_roam_tile_read(const char *path, RoamTile *t, uint32_t expect_hash,
                       char *err, size_t errn)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long sz;
    if (!f) { if (err && errn) snprintf(err, errn, "cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); if (err && errn) snprintf(err, errn, "empty file"); return 0; }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f);
        if (err && errn) snprintf(err, errn, "read failed");
        return 0;
    }
    fclose(f);
    if (!td5_roam_tile_from_image(t, buf, (size_t)sz, expect_hash, err, errn)) { free(buf); return 0; }
    return 1;
}

/* ============================================================================
 * SECTION: the builder context
 * ========================================================================== */

typedef struct {
    int      layer;                 /* 0 ROADS, 1 SIGNALS                         */
    int      ix, iz;
    double   v[7];                  /* n, minx, minz, maxx, maxz as in INDEX.JSON */
    int      hashed;
    uint32_t fnv;
    int64_t  size;
    int      sig_loaded;
    double  *sig;                   /* x,z pairs                                  */
    int      nsig;
} IdxRow;

typedef struct Work Work;

struct TD5_RoamTileCtx {
    char     place[64];
    RoamGrid grid;
    char     grid_json[2048];
    int      grid_json_n;
    uint32_t place_fnv, hdr_fnv;
    char     hpath[400], wpath[400];
    int      ex100;
    TD5_RoamGraphParams prm;
    double   halo;                  /* units                                      */
    IdxRow  *idx;  int nidx;
    int      have_global;
    double   gwin[4];
    Work    *gwork;                 /* GLOBAL mode working set, built lazily      */
    int      session;
};

static char *slurp_len(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 64L * 1024 * 1024) { fclose(f); return NULL; }
    b = (char *)malloc((size_t)n + 1u);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = '\0';
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

/* FNV of the 72-byte header and the size of one raster file. */
static uint32_t raster_header_hash(uint32_t h, const char *path)
{
    unsigned char hd[72];
    FILE *f = fopen(path, "rb");
    long sz = 0;
    memset(hd, 0, sizeof hd);
    if (f) {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fread(hd, 1, sizeof hd, f) != sizeof hd) memset(hd, 0, sizeof hd);
        fclose(f);
    }
    h = fnv_add(h, hd, sizeof hd);
    return fnv_i64(h, (int64_t)sz);
}

static int idx_load(TD5_RoamTileCtx *c)
{
    char path[400];
    size_t n = 0;
    char *json;
    cJSON *root, *layers;
    static const char *const layer_name[2] = { "ROADS", "SIGNALS" };
    int L, cap = 0;
    td5_geo_source_path(path, sizeof path, c->place, "tiles/INDEX.JSON");
    json = slurp_len(path, &n);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    layers = cJSON_GetObjectItem(root, "layers");
    for (L = 0; L < 2; L++) {
        const cJSON *tl = layers ? cJSON_GetObjectItem(cJSON_GetObjectItem(layers, layer_name[L]), "tiles") : NULL;
        const cJSON *t;
        for (t = tl ? tl->child : NULL; t; t = t->next) {
            const cJSON *a = t->child;
            double v[7];
            int k;
            IdxRow *r;
            for (k = 0; k < 7 && a; k++, a = a->next) v[k] = a->valuedouble;
            if (k < 7) continue;
            td5_geo_src_bbox_to_game(&v[3], &v[5]);   /* [roam M] the extent in the game frame */
            if (c->nidx >= cap) {
                IdxRow *nr;
                cap = cap ? cap * 2 : 256;
                nr = (IdxRow *)realloc(c->idx, (size_t)cap * sizeof(IdxRow));
                if (!nr) { cJSON_Delete(root); return 0; }
                c->idx = nr;
            }
            r = &c->idx[c->nidx++];
            memset(r, 0, sizeof *r);
            r->layer = L;
            r->ix = (int)v[0];
            r->iz = (int)v[1];
            memcpy(r->v, v, sizeof v);
        }
    }
    cJSON_Delete(root);
    return 1;
}

static void idx_hash_file(TD5_RoamTileCtx *c, IdxRow *r)
{
    char rel[64], path[400];
    size_t len = 0;
    char *b;
    if (r->hashed) return;
    snprintf(rel, sizeof rel, "tiles/%s_%d_%d.JSON", r->layer ? "SIGNALS" : "ROADS", r->ix, r->iz);
    td5_geo_source_path(path, sizeof path, c->place, rel);
    b = slurp_len(path, &len);
    r->fnv = b ? fnv_add(FNV_INIT, b, len) : 0u;
    r->size = b ? (int64_t)len : -1;
    r->hashed = 1;
    free(b);
}

static void idx_load_signals(TD5_RoamTileCtx *c, IdxRow *r)
{
    char rel[64], path[400];
    char *b;
    cJSON *doc, *arr, *s;
    int cap = 0;
    if (r->sig_loaded) return;
    r->sig_loaded = 1;
    snprintf(rel, sizeof rel, "tiles/SIGNALS_%d_%d.JSON", r->ix, r->iz);
    td5_geo_source_path(path, sizeof path, c->place, rel);
    b = slurp_len(path, NULL);
    if (!b) return;
    doc = cJSON_Parse(b);
    free(b);
    if (!doc) return;
    td5_geo_src_json_to_game(doc);       /* [roam M] source frame -> game frame */
    arr = cJSON_GetObjectItem(doc, "signals");
    for (s = arr ? arr->child : NULL; s; s = s->next) {
        const cJSON *x = cJSON_GetObjectItem(s, "x"), *z = cJSON_GetObjectItem(s, "z");
        if (!x || !z) continue;
        if (2 * r->nsig + 2 > cap) {
            double *nb;
            cap = cap ? cap * 2 : 128;
            nb = (double *)realloc(r->sig, (size_t)cap * sizeof(double));
            if (!nb) break;
            r->sig = nb;
        }
        r->sig[2 * r->nsig] = x->valuedouble;
        r->sig[2 * r->nsig + 1] = z->valuedouble;
        r->nsig++;
    }
    cJSON_Delete(doc);
}

static int row_meets(const IdxRow *r, const double win[4], double halo)
{
    return !(r->v[5] < win[0] - halo || r->v[3] > win[2] + halo ||
             r->v[6] < win[1] - halo || r->v[4] > win[3] + halo);
}

/* [roam G6] The lattice file, written when missing. It is a constant (roam_grid_default), but until
 * now only --RoamBuildTiles wrote it, and the FREE ROAM / RACE ROUTE gate (td5_roam_place_for_latlon)
 * refuses every point of a place without it -- so a place whose roam/ folder was never built (or was
 * cleared) read "START IS OUTSIDE THE CITY MAP" everywhere, although the stream builds the tiles on
 * demand. Only for a place with the tiled OSM source (tiles/INDEX.JSON), the only kind roam can build. */
int td5_roam_grid_ensure(const char *place)
{
    char path[400], dir[400], buf[2048];
    RoamGrid g;
    FILE *f;
    int n;
    if (!place || !place[0]) return 0;
    td5_geo_source_path(path, sizeof path, place, "roam/ROAMGRID.JSON");
    if ((f = fopen(path, "rb")) != NULL) {
        /* [roam M] a source-frame (TD5ROAMGRID1) lattice is rewritten once, reflected */
        n = (int)fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n > 0 ? n : 0] = '\0';
        if (!roam_grid_parse(buf, &g) || !g.src_frame) return 1;
        g.src_frame = 0;
        n = roam_grid_format(&g, buf, sizeof buf);
        if (n <= 0 || n >= (int)sizeof buf || (f = fopen(path, "wb")) == NULL) return 1;
        fwrite(buf, 1, (size_t)n, f);
        fclose(f);
        TD5_LOG_I(LOG_TAG, "roam tile: %s was a source-frame lattice, rewritten in the game frame", path);
        return 1;
    }
    td5_geo_source_path(dir, sizeof dir, place, "tiles/INDEX.JSON");
    if ((f = fopen(dir, "rb")) == NULL) return 0;
    fclose(f);
    roam_grid_default(place, &g);
    n = roam_grid_format(&g, buf, sizeof buf);
    if (n <= 0 || n >= (int)sizeof buf) return 0;
    td5_geo_source_path(dir, sizeof dir, place, "roam");
    td5_roam_mkdirs(dir);
    if ((f = fopen(path, "wb")) == NULL) {
        TD5_LOG_W(LOG_TAG, "roam tile: cannot write the default lattice %s", path);
        return 0;
    }
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    TD5_LOG_I(LOG_TAG, "roam tile: wrote default %s", path);
    return 1;
}

TD5_RoamTileCtx *td5_roam_tile_ctx_open(const char *place, int create_grid,
                                        const double *global_win)
{
    TD5_RoamTileCtx *c = (TD5_RoamTileCtx *)calloc(1, sizeof *c);
    char path[400], *txt;
    size_t len = 0;
    if (!c) return NULL;
    snprintf(c->place, sizeof c->place, "%s", place);
    c->halo = ROAM_HALO_M * TD5_ROAM_UPM;
    td5_roam_graph_params_default(&c->prm);
    c->ex100 = td5_env_int("TD5RE_GEO_EXAGGERATION", TD5_GEO_EXAGGERATION_X100_DEFAULT, 100, 400);

    /* lattice */
    td5_geo_source_path(path, sizeof path, place, "roam/ROAMGRID.JSON");
    txt = slurp_len(path, &len);
    if (!txt && create_grid && td5_roam_grid_ensure(place))
        txt = slurp_len(path, &len);
    if (!txt || len >= sizeof c->grid_json || !roam_grid_parse(txt, &c->grid)) {
        TD5_LOG_E(LOG_TAG, "roam tile: no usable lattice file %s", path);
        free(txt);
        free(c);
        return NULL;
    }
    memcpy(c->grid_json, txt, len + 1u);
    c->grid_json_n = (int)len;
    free(txt);
    if (c->grid.tile != ROAM_TILE_UNITS)
        TD5_LOG_W(LOG_TAG, "roam tile: lattice tile %d differs from the %d the locator grid assumes",
                  (int)c->grid.tile, ROAM_TILE_UNITS);

    /* identity inputs that do not depend on the tile */
    td5_geo_source_path(path, sizeof path, place, "PLACE.JSON");
    txt = slurp_len(path, &len);
    c->place_fnv = txt ? fnv_add(FNV_INIT, txt, len) : 0u;
    free(txt);
    td5_geo_source_path(c->hpath, sizeof c->hpath, place, "HEIGHT.R16");
    td5_geo_source_path(c->wpath, sizeof c->wpath, place, "WATER.R8");
    c->hdr_fnv = raster_header_hash(FNV_INIT, c->hpath);
    c->hdr_fnv = raster_header_hash(c->hdr_fnv, c->wpath);

    if (!idx_load(c)) {
        TD5_LOG_E(LOG_TAG, "roam tile: no readable tiles/INDEX.JSON for %s", place);
        free(c);
        return NULL;
    }
    /* The pavement table the lane floor reads: select it here, on the opening
     * thread, so a worker never makes the first (lazy) write (design 1.1). */
    td5_geo_sw_place(place);
    if (td5_roam_cache_session_begin(place) <= 0) {
        TD5_LOG_E(LOG_TAG, "roam tile: cache session for %s did not start", place);
        free(c->idx);
        free(c);
        return NULL;
    }
    c->session = 1;
    if (global_win) {
        c->have_global = 1;
        memcpy(c->gwin, global_win, sizeof c->gwin);
    }
    return c;
}

const RoamGrid *td5_roam_tile_ctx_grid(const TD5_RoamTileCtx *c) { return &c->grid; }

static void work_free(Work *w);

void td5_roam_tile_ctx_close(TD5_RoamTileCtx *c)
{
    int i;
    if (!c) return;
    if (c->gwork) work_free(c->gwork);
    for (i = 0; i < c->nidx; i++) free(c->idx[i].sig);
    free(c->idx);
    if (c->session) td5_roam_cache_session_end();
    free(c);
}

/* The input hash of one tile (design 1.7): place, lattice bytes, every rule knob,
 * the raster headers, and the INDEX row + size + FNV of every cache tile read. */
static uint32_t ctx_input_hash(TD5_RoamTileCtx *c, const double win[4], int *ntiles)
{
    uint32_t h = FNV_INIT;
    const TD5_RoamGraphParams *p = &c->prm;
    int i, nt = 0;
    h = fnv_add(h, c->place, strlen(c->place) + 1u);
    h = fnv_u32(h, c->place_fnv);
    h = fnv_u32(h, (uint32_t)c->grid.tile);
    h = fnv_add(h, c->grid_json, (size_t)c->grid_json_n);
    h = fnv_u32(h, ROAM_FORMAT_VERSION);
    h = fnv_u32(h, ROAM_RULES_VERSION);
    h = fnv_dbl(h, c->halo);
    h = fnv_u32(h, (uint32_t)c->ex100);
    h = fnv_dbl(h, p->join_max_defl_deg); h = fnv_dbl(h, p->merge_len);
    h = fnv_dbl(h, p->cap_diameter);      h = fnv_dbl(h, p->disc_pad);
    h = fnv_dbl(h, p->arm_dir_len);       h = fnv_dbl(h, p->parallel_deg);
    h = fnv_dbl(h, p->corner_max);        h = fnv_dbl(h, p->kerb_r_minor);
    h = fnv_dbl(h, p->kerb_r_major);      h = fnv_u32(h, (uint32_t)p->fillet_segs);
    h = fnv_dbl(h, p->tri_edge_max);      h = fnv_dbl(h, p->signal_r);
    h = fnv_dbl(h, p->body_half_w);       h = fnv_dbl(h, p->body_lf);
    h = fnv_dbl(h, ROAM_RIB_STEP);   h = fnv_dbl(h, ROAM_RIB_LANE_W);
    h = fnv_u32(h, ROAM_RIB_BLOCK);  h = fnv_u32(h, ROAM_RIB_CHUNK_ROWS);
    h = fnv_u32(h, ROAM_RIB_CHUNK_TAIL); h = fnv_u32(h, ROAM_RIB_SMOOTH);
    h = fnv_u32(h, ROAM_RIB_BLEND_ROWS);
    h = fnv_dbl(h, ROAM_GRADE_CAP);  h = fnv_dbl(h, ROAM_GRADE_ABSMAX);
    h = fnv_dbl(h, ROAM_CURV_CAP);   h = fnv_dbl(h, ROAM_XSEP_LIFT);
    h = fnv_dbl(h, ROAM_XSEP_RAMP_FRAC); h = fnv_dbl(h, ROAM_RIB_DECK_FLAT);
    h = fnv_dbl(h, ROAM_PAD_TOUCH_M);    h = fnv_dbl(h, ROAM_SPREAD_CLAMP_M);
    h = fnv_u32(h, ROAM_HC_PLANE);
    h = fnv_u32(h, c->hdr_fnv);
    for (i = 0; i < c->nidx; i++) {
        IdxRow *r = &c->idx[i];
        int k;
        if (!row_meets(r, win, c->halo)) continue;
        idx_hash_file(c, r);
        h = fnv_u32(h, (uint32_t)r->layer);
        h = fnv_u32(h, (uint32_t)r->ix);
        h = fnv_u32(h, (uint32_t)r->iz);
        for (k = 0; k < 7; k++) h = fnv_dbl(h, r->v[k]);
        h = fnv_i64(h, r->size);
        h = fnv_u32(h, r->fnv);
        nt++;
    }
    if (ntiles) *ntiles = nt;
    return h;
}

/* ============================================================================
 * SECTION: the working set (graph + heights + ribbons over one window)
 * ========================================================================== */

struct Work {
    TD5_RoamGraph g;       int g_live;
    TD5_RoamHeight h;      int h_live;
    uint8_t   *sel;                /* per edge: 0 no, 1 touching, 2 bridge partner     */
    double    *ebox;               /* per edge: minx, minz, maxx, maxz                 */
    RoamRibbon *rib;               /* per edge                                         */
    uint8_t   *built;              /* per edge: ribbon build was attempted             */
    uint8_t   *rev;                /* per edge: T4 built it reversed (a loop)          */
    double    *hc;                 /* per cluster: ground mean                         */
    double    *cspread;            /* per cluster: ground max - min                    */
    double    *cgr;                /* [F1-T10] per cluster: ground plane gx, gz         */
    uint8_t   *cpl;                /* [F1-T10] per cluster: 1 = heights from the plane  */
    uint8_t   *hcs;                /* per cluster: 0 unknown, 1 ok, 2 no sample        */
    double    *apin;               /* per global arm: pin height                       */
    uint8_t   *apins;              /* per global arm: 0 unknown, 1 ok, 2 failed        */
    double    *px, *pz;            /* scratch polyline for the adapter                 */
    int        pcap;
    /* [F1-T10] build dependencies (per edge, CSR into dep[] / depk[]):
     *   ROAM_DEP_LEAD   a higher-priority edge whose ribbon this one overlaps
     *                   (height coupling, roam_ribbon_build step 5b);
     *   ROAM_DEP_OVER   for a tunnel=1 edge: a higher-layer edge above it;
     *   ROAM_DEP_BORE   for an approach within two JOIN hops of a tunnel: that
     *                   tunnel (its node lowering must exist first). */
    int       *dep_first, *dep_n;
    int       *dep;  uint8_t *depk;
    int        ndep, capdep;
    uint8_t   *bstate;             /* per edge: 0 todo, 1 building, 2 done            */
    double    *nlow;               /* per node: lowered JOIN pin (bore chain), NAN none */
    double    *cbox;               /* per cluster: patch vertex bbox minx,minz,maxx,maxz */
    double     win[4];             /* the final graph window                           */
    uint32_t   hash;               /* input hash for the final window                  */
    int        cache_tiles;
    int        passes;
    TD5_RoamTileStats st;          /* counters of the build phase                      */
};

static void work_free(Work *w)
{
    int i;
    if (!w) return;
    if (w->rib) for (i = 0; i < w->g.nedge; i++) roam_ribbon_free(&w->rib[i]);
    free(w->rib); free(w->sel); free(w->ebox); free(w->built); free(w->rev);
    free(w->hc); free(w->cspread); free(w->cgr); free(w->cpl); free(w->hcs); free(w->apin); free(w->apins);
    free(w->px); free(w->pz);
    free(w->dep_first); free(w->dep_n); free(w->dep); free(w->depk); free(w->bstate); free(w->nlow); free(w->cbox);
    if (w->h_live) td5_roam_height_close(&w->h);
    if (w->g_live) td5_roam_graph_free(&w->g);
    free(w);
}

/* ground height at a place-frame point (the ribbon's own rule: a wet cell reads
 * as the water surface, not the bed). */
static int work_ground(Work *w, double x, double z, double *out)
{
    const double pt[1][2] = { { x, z } };
    return roam_cluster_height(&w->h, pt, 1, out);
}

/* Cluster ground mean + spread; memoised.
 *
 * [F1-T10] A SLOPED CLUSTER TAKES A PLANE, NOT ONE HEIGHT. With one H_c a
 * cluster on a slope (933 of 2127 in the casco spread > 0.5 m) pinned every arm
 * 0.5 m off its own ground towards the mean, uphill arms down and downhill arms
 * up. Two such clusters a block apart then pinned the short street between them
 * in opposite directions: a Calle 14 piece next to the unnamed primary 23406181,
 * on a 12.5 % real slope, needed 28 % between its pins (a PIN_MISS piece) and the
 * primary beside it rode 0.6 m over the ground, a 1.03 m step where they overlap.
 * Now a cluster whose ground spread exceeds ROAM_SPREAD_CLAMP_M gets the
 * least-squares plane through the same samples, its gradient capped at the grade
 * cap. H_c is that plane at the centroid, the CENTRE term of every patch vertex
 * recipe reads the plane at the vertex (work_vert_y) and an arm pins to the plane
 * at its cut row (work_arm_pin). Not for a ROW_COVERED union: its covered cut row
 * lies flat under the patch, so that patch stays flat at H_c as before. A cluster
 * within the spread keeps its mean exactly. */
static int work_hc(Work *w, int ci, double *out)
{
    if (w->hcs[ci] == 0) {
        const TD5_RoamGraph *g = &w->g;
        const TD5_RoamCluster *cl = &g->cluster[ci];
        double sum = 0.0, gmin = 1e300, gmax = -1e300, gs;
        double sx = 0.0, sz = 0.0;
        int k, n = 0, ok = 1;
        for (k = 0; k < cl->nnode && ok; k++) {
            const TD5_RoamNode *nd = &g->node[g->cluster_node[cl->node_first + k]];
            if (!work_ground(w, nd->x, nd->z, &gs)) { ok = 0; break; }
            sum += gs; n++; sx += nd->x; sz += nd->z;
            if (gs < gmin) gmin = gs;
            if (gs > gmax) gmax = gs;
        }
        for (k = 0; k < cl->nout && ok; k++) {
            const TD5_RoamPatchVert *pv = &g->pvert[cl->vert_first + k];
            if (!work_ground(w, pv->x, pv->z, &gs)) { ok = 0; break; }
            sum += gs; n++; sx += pv->x; sz += pv->z;
            if (gs < gmin) gmin = gs;
            if (gs > gmax) gmax = gs;
        }
        if (!ok || n < 1) { w->hcs[ci] = 2; }
        else {
            w->hc[ci] = sum / (double)n;
            w->cspread[ci] = gmax - gmin;
            w->hcs[ci] = 1;
            if (w->cspread[ci] > ROAM_SPREAD_CLAMP_M * TD5_ROAM_UPM && n >= 3 &&
                !(cl->flags & (TD5_ROAM_CL_ROW_COVERED | TD5_ROAM_CL_HULL))) {
                /* second pass over the same samples, centred on their mean */
                const double mx = sx / (double)n, mz = sz / (double)n, my = sum / (double)n;
                double sxx = 0.0, szz = 0.0, sxz = 0.0, sxy = 0.0, szy = 0.0, det;
                for (k = 0; k < cl->nnode + cl->nout; k++) {
                    double x, z;
                    if (k < cl->nnode) {
                        const TD5_RoamNode *nd = &g->node[g->cluster_node[cl->node_first + k]];
                        x = nd->x; z = nd->z;
                    } else {
                        const TD5_RoamPatchVert *pv = &g->pvert[cl->vert_first + (k - cl->nnode)];
                        x = pv->x; z = pv->z;
                    }
                    if (!work_ground(w, x, z, &gs)) break;
                    x -= mx; z -= mz; gs -= my;
                    sxx += x * x; szz += z * z; sxz += x * z; sxy += x * gs; szy += z * gs;
                }
                det = sxx * szz - sxz * sxz;
                if (k == cl->nnode + cl->nout && det > 1e-6 * (sxx + szz) * (sxx + szz)) {
                    double gx = (sxy * szz - szy * sxz) / det;
                    double gz = (szy * sxx - sxy * sxz) / det;
                    const double gm = hypot(gx, gz), capg = ROAM_GRADE_CAP * 0.97;
                    if (gm > capg) { gx *= capg / gm; gz *= capg / gm; }
                    w->cgr[2 * ci] = gx;
                    w->cgr[2 * ci + 1] = gz;
                    w->hc[ci] = my + gx * (cl->cx - mx) + gz * (cl->cz - mz);
                    w->cpl[ci] = 1;
                }
            }
        }
    }
    if (w->hcs[ci] != 1) return 0;
    *out = w->hc[ci];
    return 1;
}

/* [F1-T10] The cluster's height at (x, z): its plane on a sloped cluster, H_c
 * elsewhere. Valid after work_hc succeeded. */
static double work_plane_y(const Work *w, int ci, double x, double z)
{
    const TD5_RoamCluster *cl = &w->g.cluster[ci];
    if (!w->cpl[ci]) return w->hc[ci];
    return w->hc[ci] + w->cgr[2 * ci] * (x - cl->cx) + w->cgr[2 * ci + 1] * (z - cl->cz);
}

/* [F1-T10] td5_roam_patch_vert_y with the CENTRE term read from the plane. */
static double work_vert_y(const Work *w, int ci, const TD5_RoamPatchVert *v, double hc,
                          const double *arm_y)
{
    double y = 0.0;
    int k;
    if (!w->cpl[ci]) return td5_roam_patch_vert_y(v, hc, arm_y);
    for (k = 0; k < TD5_ROAM_HREF_TERMS; k++) {
        if (v->ref[k] == TD5_ROAM_HREF_NONE) continue;
        y += v->w[k] * (v->ref[k] == TD5_ROAM_HREF_CENTRE ? work_plane_y(w, ci, v->x, v->z)
                                                          : arm_y[v->ref[k]]);
    }
    return y;
}

/* The pin height of one cluster arm (global arm index). When the cluster's ground
 * samples spread more than ROAM_SPREAD_CLAMP_M, the arm's own ground at its cut row
 * limits how far the pin may sit from it (design risk 3, "clamp the arm blend"). */
static int work_arm_pin(Work *w, int gi, double *out)
{
    if (w->apins[gi] == 0) {
        const TD5_RoamGraph *g = &w->g;
        const TD5_RoamArm *a = &g->arm[gi];
        const TD5_RoamEdge *e = &g->edge[a->edge];
        const TD5_RoamEdgeEnd *en = &e->end[a->end];
        const double clamp_u = ROAM_SPREAD_CLAMP_M * TD5_ROAM_UPM;
        double hc, gr, dev, rmx, rmz;
        const int ci = en->cluster;
        if (ci < 0 || !work_hc(w, ci, &hc) || en->nrow < 1) { w->apins[gi] = 2; return 0; }
        rmx = 0.5 * (en->row[0].x + en->row[en->nrow - 1].x);
        rmz = 0.5 * (en->row[0].z + en->row[en->nrow - 1].z);
        if (!work_ground(w, rmx, rmz, &gr)) {
            w->apins[gi] = 2;
            return 0;
        }
        hc = work_plane_y(w, ci, rmx, rmz);        /* [F1-T10] the plane at the cut row */
        dev = fabs(hc - gr);
        if (dev > w->st.max_blend_m * TD5_ROAM_UPM) w->st.max_blend_m = dev / TD5_ROAM_UPM;
        /* [F1-T9] Not on a ROW_COVERED patch: there an arm's cut row lies UNDER
         * the patch rather than on its outline, and the patch surface there is
         * made from the other arms' pins, so a per-arm pin left the covered row
         * standing off the patch (casco: up to 6 units; 0 with every arm at H_c). */
        if (w->cspread[ci] > clamp_u && dev > clamp_u &&
            !(g->cluster[ci].flags & TD5_ROAM_CL_ROW_COVERED)) {
            w->apin[gi] = gr + (hc > gr ? clamp_u : -clamp_u);
            w->st.arm_clamped++;
        } else {
            w->apin[gi] = hc;
        }
        w->apins[gi] = 1;
    }
    if (w->apins[gi] != 1) return 0;
    *out = w->apin[gi];
    return 1;
}

/* The pin of one end of an edge: cluster arm pin, ground at a JOIN node, NaN at a
 * dead end. Returns 0 when the end needs a ribbon-less treatment (INTERNAL). */
static int work_end_pin(Work *w, int ei, int end, double *pin)
{
    const TD5_RoamGraph *g = &w->g;
    const TD5_RoamEdge *e = &g->edge[ei];
    const TD5_RoamEdgeEnd *en = &e->end[end];
    *pin = NAN;
    if (en->kind == TD5_ROAM_END_CLUSTER) {
        if (!work_arm_pin(w, en->arm, pin)) { *pin = NAN; return -1; }
        return 1;
    }
    if (en->kind == TD5_ROAM_END_JOIN) {
        const TD5_RoamNode *nd = &g->node[en->node];
        double gr;
        if (!work_ground(w, nd->x, nd->z, &gr)) return -1;
        /* [F1-T10] a JOIN on a bore chain sits lower (td5_roam_tile.c work_bore) */
        if (w->nlow && !isnan(w->nlow[en->node]) && w->nlow[en->node] < gr) gr = w->nlow[en->node];
        *pin = gr;
        return 1;
    }
    if (en->kind == TD5_ROAM_END_DEAD) return 1;
    return 0;
}

/* One graph edge -> one RoamEdgeIn (the T3 -> T4 adapter of _archive/roam/F1/T4).
 * Returns 1 when a ribbon is to be built, 0 for an INTERNAL / unset end, -1 when a
 * pin could not be sampled. */
static int work_edge_in(Work *w, int ei, const RoamCross *cross, int ncross, RoamEdgeIn *in)
{
    const TD5_RoamGraph *g = &w->g;
    const TD5_RoamEdge *e = &g->edge[ei];
    double pa, pb;
    int i, ra, rb;
    if (e->end[0].kind == TD5_ROAM_END_INTERNAL || e->end[1].kind == TD5_ROAM_END_INTERNAL) return 0;
    ra = work_end_pin(w, ei, 0, &pa);
    rb = work_end_pin(w, ei, 1, &pb);
    if (ra == 0 || rb == 0) return 0;
    if (ra < 0 || rb < 0) return -1;
    if (e->npt > w->pcap) {
        double *nx = (double *)realloc(w->px, (size_t)e->npt * sizeof(double));
        double *nz;
        if (!nx) return -1;
        w->px = nx;
        nz = (double *)realloc(w->pz, (size_t)e->npt * sizeof(double));
        if (!nz) return -1;
        w->pz = nz;
        w->pcap = e->npt;
    }
    for (i = 0; i < e->npt; i++) {
        w->px[i] = g->pt[e->pt_first + i].x;
        w->pz[i] = g->pt[e->pt_first + i].z;
    }
    memset(in, 0, sizeof *in);
    in->key     = e->key;
    in->px      = w->px;
    in->pz      = w->pz;
    in->n       = e->npt;
    in->node_a  = g->node[e->node_a].key;
    in->node_b  = g->node[e->node_b].key;
    in->lanes   = e->lanes;
    in->lane_w  = e->lane_w;
    in->surface = e->surface;
    in->layer   = e->layer;
    in->bridge  = (e->bridge && e->layer >= 1) ? 1 : 0;
    in->tunnel  = e->tunnel;
    in->cut_a   = e->end[0].s;
    in->cut_b   = e->length - e->end[1].s;
    in->pin_a   = pa;
    in->pin_b   = pb;
    in->cross   = cross;
    in->ncross  = ncross;
    in->rowq_a  = e->end[0].rowq;
    in->rowq_b  = e->end[1].rowq;
    return 1;
}

/* T4's own orientation rule (td5_roam_ribbon.c rib_is_reversed): only a loop can be
 * built against T3's a -> b direction. */
static int edge_in_reversed(const RoamEdgeIn *in)
{
    if (in->node_a == in->node_b && in->n >= 3) {
        const double ax = in->px[1], az = in->pz[1];
        const double bx = in->px[in->n - 2], bz = in->pz[in->n - 2];
        return (ax > bx) || (ax == bx && az > bz);
    }
    return in->node_a > in->node_b;
}

static int seg_cross(double ax, double az, double bx, double bz,
                     double cx, double cz, double dx, double dz, double *ox, double *oz)
{
    const double r_x = bx - ax, r_z = bz - az, s_x = dx - cx, s_z = dz - cz;
    const double den = r_x * s_z - r_z * s_x;
    double t, u;
    if (fabs(den) < 1e-9) return 0;
    t = ((cx - ax) * s_z - (cz - az) * s_x) / den;
    u = ((cx - ax) * r_z - (cz - az) * r_x) / den;
    if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) return 0;
    *ox = ax + t * r_x;
    *oz = az + t * r_z;
    return 1;
}

static int box_meets(const double *a, const double *b)
{
    return !(a[2] < b[0] || a[0] > b[2] || a[3] < b[1] || a[1] > b[3]);
}

/* ---------------------------------------------------------------------------
 * [F1-T10] Build dependencies: height coupling, bore chains.
 *
 * PRIORITY. A strict total order over edges: road class, then lanes, then the
 * smaller edge key. Where two ribbons of the same layer overlap, the LOWER one
 * follows the higher one's surface (roam_ribbon_build 5b), so a leader never
 * depends on its follower and the coupling graph has no cycle.
 *
 * WHO OVERLAPS WHOM is decided from the graph alone (polylines, cut rows, lane
 * widths), so every tile build that holds both edges agrees: P's centreline
 * between its cut rows is sampled every half row, and a sample nearer to Q's
 * centreline (between Q's cut rows) than hw_P + hw_Q - 100 units is an overlap.
 * Not coupled: two pieces sharing a JOIN row (they meet on one row by
 * construction), bridges (layer >= 1 decks), bores, and an INTERNAL edge.
 *
 * BORE CHAIN. A tunnel=1 edge is lowered under every higher-layer edge it
 * crosses by ROAM_XSEP_LIFT (design 1.5, the negative of the bridge rule). Its
 * JOIN ends and, one hop further, the far JOIN of each approach are lowered so
 * the approaches ramp down at the ramp grade; an at-grade cluster end is never
 * moved (ROAM_RAMP_SHORT territory: the ribbon's chunk cap is raised instead).
 * ------------------------------------------------------------------------- */

#define TD5_ROAM_ARM_MAX_LOCAL 64   /* arms per cluster the slice keeps on the stack */
#define ROAM_DEP_LEAD 1
#define ROAM_DEP_OVER 2
#define ROAM_DEP_BORE 3
#define ROAM_COUPLE_MARGIN 100.0   /* units: an overlap must be deeper than this */

/* >0 when a outranks b. */
static int edge_prio_cmp(const TD5_RoamEdge *a, const TD5_RoamEdge *b)
{
    if (a->klass != b->klass) return a->klass > b->klass ? 1 : -1;
    if (a->lanes != b->lanes) return a->lanes > b->lanes ? 1 : -1;
    if (a->key != b->key) return a->key < b->key ? 1 : -1;
    return 0;
}

static int edge_has_ribbon(const TD5_RoamEdge *e)
{
    return e->end[0].kind != TD5_ROAM_END_INTERNAL && e->end[1].kind != TD5_ROAM_END_INTERNAL &&
           e->end[0].kind != 0 && e->end[1].kind != 0;
}

/* A deck or a bore: its profile is the structure's, never coupled. */
static int edge_is_structure(const TD5_RoamEdge *e)
{
    return (e->bridge && e->layer >= 1) || e->tunnel == 1;
}

/* Nearest point of e's polyline to (x, z): arc length from node_a and distance. */
static void poly_nearest(const TD5_RoamGraph *g, const TD5_RoamEdge *e, double x, double z,
                         double *s_out, double *d_out)
{
    const TD5_RoamPt *p = &g->pt[e->pt_first];
    double acc = 0.0, best = 1e300, bs = 0.0;
    int i;
    for (i = 0; i + 1 < e->npt; i++) {
        const double dx = p[i + 1].x - p[i].x, dz = p[i + 1].z - p[i].z;
        const double l2 = dx * dx + dz * dz, len = sqrt(l2);
        double t = l2 > 1e-12 ? ((x - p[i].x) * dx + (z - p[i].z) * dz) / l2 : 0.0, qx, qz, d;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        qx = p[i].x + dx * t - x; qz = p[i].z + dz * t - z;
        d = qx * qx + qz * qz;
        if (d < best) { best = d; bs = acc + t * len; }
        acc += len;
    }
    *s_out = bs;
    *d_out = sqrt(best);
}

static int share_join(const TD5_RoamGraph *g, const TD5_RoamEdge *p, const TD5_RoamEdge *q)
{
    const int pn[2] = { p->node_a, p->node_b }, qn[2] = { q->node_a, q->node_b };
    int a, b;
    for (a = 0; a < 2; a++)
        for (b = 0; b < 2; b++)
            if (pn[a] == qn[b] && g->node[pn[a]].kind == TD5_ROAM_NODE_JOIN) return 1;
    return 0;
}

/* Does P's ribbon overlap Q's (both between their cut rows)? */
static int edges_overlap(const TD5_RoamGraph *g, const TD5_RoamEdge *P, const TD5_RoamEdge *Q)
{
    const double s0 = P->end[0].s, s1 = P->end[1].s;
    const double q0 = Q->end[0].s - 1.0, q1 = Q->end[1].s + 1.0;
    const double lim = P->half_w + Q->half_w - ROAM_COUPLE_MARGIN;
    const double step = 0.5 * ROAM_RIB_STEP;
    double s;
    int last = 0;
    if (s1 - s0 < 1.0 || lim <= 0.0) return 0;
    for (s = s0; !last; s += step) {
        double x, z, sq, d;
        if (s >= s1) { s = s1; last = 1; }
        td5_roam_edge_point(g, P, s, &x, &z, NULL, NULL);
        poly_nearest(g, Q, x, z, &sq, &d);
        if (sq >= q0 && sq <= q1 && d < lim) return 1;
    }
    return 0;
}

/* The tunnel=1 edge within two JOIN hops of e (not e itself), or -1. */
static int bore_of_chain(const TD5_RoamGraph *g, int ei)
{
    int end;
    for (end = 0; end < 2; end++) {
        int cur = ei, node = end == 0 ? g->edge[ei].node_a : g->edge[ei].node_b, hop;
        for (hop = 0; hop < 2; hop++) {
            const TD5_RoamNode *nd = &g->node[node];
            const TD5_RoamEndRef *r;
            int nxt;
            if (nd->kind != TD5_ROAM_NODE_JOIN) break;
            r = &g->node_end[nd->end_first];
            nxt = r[0].edge == cur ? r[1].edge : r[0].edge;
            if (nxt == ei || nxt == cur) break;
            if (g->edge[nxt].tunnel == 1) return nxt;
            node = g->edge[nxt].node_a == node ? g->edge[nxt].node_b : g->edge[nxt].node_a;
            cur = nxt;
        }
    }
    return -1;
}

static int dep_push(Work *w, int d, int kind)
{
    if (w->ndep >= w->capdep) {
        const int c = w->capdep ? w->capdep * 2 : 1024;
        int *nd = (int *)realloc(w->dep, (size_t)c * sizeof(int));
        uint8_t *nk;
        if (!nd) return 0;
        w->dep = nd;
        nk = (uint8_t *)realloc(w->depk, (size_t)c);
        if (!nk) return 0;
        w->depk = nk;
        w->capdep = c;
    }
    w->dep[w->ndep] = d;
    w->depk[w->ndep] = (uint8_t)kind;
    w->ndep++;
    return 1;
}

/* Fill edge ei's dependency slice (sorted by edge index: leaders in priority
 * order are re-sorted by the caller of the coupling). Returns 0 on OOM. */
static int work_deps(Work *w, int ei)
{
    const TD5_RoamGraph *g = &w->g;
    const TD5_RoamEdge *e = &g->edge[ei];
    const double *be = &w->ebox[4 * ei];
    int j, t;
    w->dep_first[ei] = w->ndep;
    if (e->tunnel == 1) {
        for (j = 0; j < g->nedge; j++) {
            const TD5_RoamEdge *q = &g->edge[j];
            if (j == ei || q->layer <= e->layer || q->way == e->way) continue;
            if (!box_meets(be, &w->ebox[4 * j])) continue;
            /* its own approaches are higher layer too, but they follow the bore */
            if (q->tunnel == 1 || bore_of_chain(g, j) >= 0) continue;
            if (!dep_push(w, j, ROAM_DEP_OVER)) return 0;
        }
    } else {
        t = bore_of_chain(g, ei);
        if (t >= 0 && !dep_push(w, t, ROAM_DEP_BORE)) return 0;
        /* a bore approach is shaped by the bore: neither follower nor leader */
        if (t < 0 && !edge_is_structure(e) && edge_has_ribbon(e)) {
            const double pad = e->half_w + TD5_ROAM_LANE_W * 0.5 * TD5_GEO_ROADS_LANES_MAX;
            double bp[4];
            bp[0] = be[0] - pad; bp[1] = be[1] - pad; bp[2] = be[2] + pad; bp[3] = be[3] + pad;
            for (j = 0; j < g->nedge; j++) {
                const TD5_RoamEdge *q = &g->edge[j];
                if (j == ei || q->layer != e->layer || edge_is_structure(q) || !edge_has_ribbon(q)) continue;
                if (edge_prio_cmp(q, e) <= 0) continue;
                {
                    /* a bore approach may lead, but never a road above its own bore
                     * (that road is one of the bore's dependencies: a cycle) */
                    const int tq = bore_of_chain(g, j);
                    if (tq >= 0 && e->layer > g->edge[tq].layer && box_meets(be, &w->ebox[4 * tq])) continue;
                }
                if (!box_meets(bp, &w->ebox[4 * j])) continue;
                if (share_join(g, e, q)) continue;
                if (!edges_overlap(g, e, q) && !edges_overlap(g, q, e)) continue;
                if (!dep_push(w, j, ROAM_DEP_LEAD)) return 0;
            }
        }
    }
    w->dep_n[ei] = w->ndep - w->dep_first[ei];
    return 1;
}

/* Edge selection: 1 = the edge's box meets `rect` (or all, in global mode);
 * 2 = a lower-layer edge of ANOTHER way under a selected bridge deck;
 * 3 = [F1-T10] a build dependency of a selected edge (closure). */
static void work_select(Work *w, const double rect[4], int all)
{
    const TD5_RoamGraph *g = &w->g;
    int i, j;
    for (i = 0; i < g->nedge; i++) {
        const TD5_RoamEdge *e = &g->edge[i];
        double *b = &w->ebox[4 * i];
        int k;
        b[0] = b[2] = g->pt[e->pt_first].x;
        b[1] = b[3] = g->pt[e->pt_first].z;
        for (k = 1; k < e->npt; k++) {
            const TD5_RoamPt *p = &g->pt[e->pt_first + k];
            if (p->x < b[0]) b[0] = p->x;
            if (p->x > b[2]) b[2] = p->x;
            if (p->z < b[1]) b[1] = p->z;
            if (p->z > b[3]) b[3] = p->z;
        }
        w->sel[i] = (all || box_meets(b, rect)) ? 1 : 0;
    }
    for (i = 0; i < g->nedge; i++) {
        const TD5_RoamEdge *p = &g->edge[i];
        if (w->sel[i] != 1 || !p->bridge || p->layer < 1) continue;
        for (j = 0; j < g->nedge; j++) {
            const TD5_RoamEdge *q = &g->edge[j];
            if (j == i || w->sel[j] != 0 || q->layer >= p->layer || q->way == p->way) continue;
            if (box_meets(&w->ebox[4 * i], &w->ebox[4 * j])) w->sel[j] = 2;
        }
    }
}

/* [F1-T10] Dependencies of every selected edge, and their closure (sel = 3).
 * Returns 0 on out of memory. */
static int work_close_deps(Work *w)
{
    const TD5_RoamGraph *g = &w->g;
    int *queue, nq = 0, qh = 0, i, k;
    free(w->dep_first); free(w->dep_n);
    w->ndep = 0;
    w->dep_first = (int *)calloc((size_t)g->nedge + 1u, sizeof(int));
    w->dep_n = (int *)calloc((size_t)g->nedge + 1u, sizeof(int));
    queue = (int *)malloc(((size_t)g->nedge + 1u) * sizeof(int));
    if (!w->dep_first || !w->dep_n || !queue) { free(queue); return 0; }
    for (i = 0; i < g->nedge; i++) if (w->sel[i]) queue[nq++] = i;
    while (qh < nq) {
        const int ei = queue[qh++];
        if (!work_deps(w, ei)) { free(queue); return 0; }
        for (k = 0; k < w->dep_n[ei]; k++) {
            const int d = w->dep[w->dep_first[ei] + k];
            if (!w->sel[d]) { w->sel[d] = 3; queue[nq++] = d; }
        }
    }
    free(queue);
    return 1;
}

/* [F1-T10] A bore: crossings with the roads above (their FINISHED ribbons), then
 * the JOIN pins of its chain lowered so the approaches can ramp down. Returns the
 * crossing count; *list is malloc'd (caller frees). */
static int work_bore(Work *w, int ei, RoamCross **list_out)
{
    const TD5_RoamGraph *g = &w->g;
    const TD5_RoamEdge *p = &g->edge[ei];
    const double rg = ROAM_XSEP_RAMP_FRAC * ROAM_GRADE_CAP;
    const double flat = ROAM_RIB_DECK_FLAT * ROAM_RIB_STEP;
    RoamCross *list = NULL;
    double *cs = NULL;
    int nl = 0, cap = 0, k, a, b, end;
    *list_out = NULL;
    for (k = 0; k < w->dep_n[ei]; k++) {
        const int j = w->dep[w->dep_first[ei] + k];
        const TD5_RoamEdge *q = &g->edge[j];
        if (w->depk[w->dep_first[ei] + k] != ROAM_DEP_OVER) continue;
        if (!w->built[j] || w->rib[j].status != ROAM_RIB_OK) continue;
        for (a = 0; a + 1 < p->npt; a++) {
            for (b = 0; b + 1 < q->npt; b++) {
                const TD5_RoamPt *pa0 = &g->pt[p->pt_first + a], *pa1 = &g->pt[p->pt_first + a + 1];
                const TD5_RoamPt *qb0 = &g->pt[q->pt_first + b], *qb1 = &g->pt[q->pt_first + b + 1];
                const TD5_RoamPt *qe0 = &g->pt[q->pt_first], *qe1 = &g->pt[q->pt_first + q->npt - 1];
                double xx, zz, yo, dist, sp, dp;
                if (!seg_cross(pa0->x, pa0->z, pa1->x, pa1->z, qb0->x, qb0->z, qb1->x, qb1->z, &xx, &zz))
                    continue;
                if (hypot(xx - qe0->x, zz - qe0->z) < 60.0 || hypot(xx - qe1->x, zz - qe1->z) < 60.0) continue;
                if (!roam_ribbon_y_at(&w->rib[j], xx, zz, &yo, &dist)) continue;
                if (nl >= cap) {
                    RoamCross *nlst;
                    double *ncs;
                    cap = cap ? cap * 2 : 4;
                    nlst = (RoamCross *)realloc(list, (size_t)cap * sizeof(RoamCross));
                    if (!nlst) { cap = nl; continue; }
                    list = nlst;
                    ncs = (double *)realloc(cs, (size_t)cap * sizeof(double));
                    if (!ncs) { cap = nl; continue; }
                    cs = ncs;
                }
                poly_nearest(g, p, xx, zz, &sp, &dp);
                list[nl].x = xx; list[nl].z = zz; list[nl].y_under = yo;
                cs[nl] = sp;
                nl++;
            }
        }
    }
    /* lower the chain: the bore's JOIN ends, then the far JOIN of each approach */
    for (end = 0; end < 2 && nl > 0; end++) {
        const int node = end == 0 ? p->node_a : p->node_b;
        const double s_end = end == 0 ? 0.0 : p->length;
        double low = 1e300;
        const TD5_RoamNode *nd = &g->node[node];
        if (nd->kind != TD5_ROAM_NODE_JOIN) continue;          /* at grade: never moved */
        for (k = 0; k < nl; k++) {
            double d = fabs(cs[k] - s_end) - flat;
            if (d < 0.0) d = 0.0;
            if (list[k].y_under - ROAM_XSEP_LIFT + rg * d < low) low = list[k].y_under - ROAM_XSEP_LIFT + rg * d;
        }
        if (isnan(w->nlow[node]) || low < w->nlow[node]) w->nlow[node] = low;
        {
            const TD5_RoamEndRef *r = &g->node_end[nd->end_first];
            const int ap = r[0].edge == ei ? r[1].edge : r[0].edge;
            const TD5_RoamEdge *ae = &g->edge[ap];
            const int fnode = ae->node_a == node ? ae->node_b : ae->node_a;
            if (ap != ei && fnode != node && g->node[fnode].kind == TD5_ROAM_NODE_JOIN) {
                const double lf = low + rg * ae->length;
                if (isnan(w->nlow[fnode]) || lf < w->nlow[fnode]) w->nlow[fnode] = lf;
            }
        }
    }
    free(cs);
    *list_out = list;
    return nl;
}

/* [F1-T10] The arm heights of cluster ci WITHOUT its ribbons: a ribbon's cut row
 * is its pin (the solver never moves a pinned end row, nor does the coupling),
 * an absorbed edge's shared row comes from its two pins (work_absorbed_y), and an
 * arm with no ribbon falls back to its pin or H_c, as the slice does. So a patch
 * is a leader whose surface is known before any ribbon is built. */
static int work_absorbed_y(Work *w, int ei, double *y);

static int work_patch_heights(Work *w, int ci, double *hc, double *arm_y)
{
    const TD5_RoamGraph *g = &w->g;
    const TD5_RoamCluster *cl = &g->cluster[ci];
    int i;
    if (cl->narm > TD5_ROAM_ARM_MAX_LOCAL || !work_hc(w, ci, hc)) return 0;
    for (i = 0; i < cl->narm; i++) {
        const TD5_RoamArm *a = &g->arm[cl->arm_first + i];
        const TD5_RoamEdge *e = &g->edge[a->edge];
        double pin;
        if (e->end[0].kind != TD5_ROAM_END_INTERNAL && e->end[1].kind != TD5_ROAM_END_INTERNAL &&
            e->end[1].s - e->end[0].s < 1.0) {
            if (work_absorbed_y(w, a->edge, &arm_y[i])) continue;
        } else if (work_end_pin(w, a->edge, a->end, &pin) > 0 && !isnan(pin)) {
            arm_y[i] = pin;
            continue;
        }
        arm_y[i] = work_arm_pin(w, cl->arm_first + i, &pin) ? pin : *hc;
    }
    return 1;
}

/* [F1-T10] Triangles (9 doubles each, place frame) of every junction patch that
 * edge ei's ribbon runs over (its own end clusters included: a UNION patch can
 * reach over its arm past the cut row, the Diagonal 74 gore; the cut row itself
 * is the patch boundary and pinned, so it never counts). */
static int work_patch_leads(Work *w, int ei, double **tri_out, int *ntri_out)
{
    const TD5_RoamGraph *g = &w->g;
    const TD5_RoamEdge *e = &g->edge[ei];
    const double *be = &w->ebox[4 * ei];
    double *tri = NULL, arm_y[TD5_ROAM_ARM_MAX_LOCAL], hc;
    int ntri = 0, cap = 0, ci;
    *tri_out = NULL;
    *ntri_out = 0;
    if (!edge_has_ribbon(e) || edge_is_structure(e) || e->end[1].s - e->end[0].s < 1.0) return 1;
    for (ci = 0; ci < g->ncluster; ci++) {
        const TD5_RoamCluster *cl = &g->cluster[ci];
        const double *cb = &w->cbox[4 * ci];
        int hit = 0, last = 0, k;
        double s;
        if (cl->ntri <= 0) continue;
        if (cl->layer != e->layer) continue;
        if (cb[2] < be[0] - e->half_w || cb[0] > be[2] + e->half_w ||
            cb[3] < be[1] - e->half_w || cb[1] > be[3] + e->half_w) continue;
        for (s = e->end[0].s; !last && !hit; s += 0.5 * ROAM_RIB_STEP) {
            double x, z, tx, tz;
            if (s >= e->end[1].s) { s = e->end[1].s; last = 1; }
            td5_roam_edge_point(g, e, s, &x, &z, &tx, &tz);
            for (k = 0; k <= 2 * e->lanes && !hit; k++) {
                const double off = -e->half_w + (double)k * (e->half_w / (double)e->lanes);
                const double px = x - tz * off, pz = z + tx * off;
                int t;
                if (px < cb[0] || px > cb[2] || pz < cb[1] || pz > cb[3]) continue;
                for (t = 0; t < cl->ntri && !hit; t++) {
                    const TD5_RoamPatchTri *pt = &g->ptri[cl->tri_first + t];
                    const TD5_RoamPatchVert *v0 = &g->pvert[pt->v[0]], *v1 = &g->pvert[pt->v[1]], *v2 = &g->pvert[pt->v[2]];
                    const double den = (v1->z - v2->z) * (v0->x - v2->x) + (v2->x - v1->x) * (v0->z - v2->z);
                    double l1, l2;
                    if (fabs(den) < 1e-9) continue;
                    l1 = ((v1->z - v2->z) * (px - v2->x) + (v2->x - v1->x) * (pz - v2->z)) / den;
                    l2 = ((v2->z - v0->z) * (px - v2->x) + (v0->x - v2->x) * (pz - v2->z)) / den;
                    if (l1 > 1e-6 && l2 > 1e-6 && l1 + l2 < 1.0 - 1e-6) hit = 1;
                }
            }
        }
        if (!hit || !work_patch_heights(w, ci, &hc, arm_y)) continue;
        for (k = 0; k < cl->ntri; k++) {
            const TD5_RoamPatchTri *pt = &g->ptri[cl->tri_first + k];
            int q;
            if (ntri >= cap) {
                double *nt;
                cap = cap ? cap * 2 : 64;
                nt = (double *)realloc(tri, (size_t)cap * 9u * sizeof(double));
                if (!nt) { free(tri); return 0; }
                tri = nt;
            }
            for (q = 0; q < 3; q++) {
                const TD5_RoamPatchVert *v = &g->pvert[pt->v[q]];
                tri[9 * ntri + 3 * q + 0] = (double)v->xq;
                tri[9 * ntri + 3 * q + 1] = (double)td5_roam_quant(work_vert_y(w, ci, v, hc, arm_y));
                tri[9 * ntri + 3 * q + 2] = (double)v->zq;
            }
            ntri++;
        }
    }
    *tri_out = tri;
    *ntri_out = ntri;
    return 1;
}

/* Build edge ei after everything it depends on. Depth-first, dependencies in
 * edge-index order; the coupling and bore relations go strictly up the priority
 * order / layers, so a cycle means bad data: it is broken and counted. */
static int work_build_rec(Work *w, int ei, int depth)
{
    const TD5_RoamGraph *g = &w->g;
    const struct RoamRibbon *lead[32];
    RoamCross *cross = NULL;
    double *ltri = NULL;
    RoamEdgeIn in;
    int k, r, nlead = 0, ncross = 0, nltri = 0;
    if (w->bstate[ei] == 2) return 1;
    if (w->bstate[ei] == 1 || depth > 64) {
        if (w->st.dep_cycles++ < 4)
            TD5_LOG_W(LOG_TAG, "ROAM_DEP_CYCLE edge %llu (depth %d)", (unsigned long long)g->edge[ei].key, depth);
        return 1;
    }
    w->bstate[ei] = 1;
    for (k = 0; k < w->dep_n[ei]; k++) {
        const int d = w->dep[w->dep_first[ei] + k];
        if (w->sel[d] && !work_build_rec(w, d, depth + 1)) return 0;
    }
    w->bstate[ei] = 2;
    if (g->edge[ei].tunnel == 1) ncross = work_bore(w, ei, &cross);
    /* leaders in priority order (insertion sort, at most 32) */
    for (k = 0; k < w->dep_n[ei] && nlead < 32; k++) {
        const int d = w->dep[w->dep_first[ei] + k];
        int m;
        if (w->depk[w->dep_first[ei] + k] != ROAM_DEP_LEAD) continue;
        if (!w->built[d] || w->rib[d].status != ROAM_RIB_OK) continue;
        for (m = nlead; m > 0; m--) {
            const int pidx = (int)(lead[m - 1] - w->rib);
            if (edge_prio_cmp(&g->edge[d], &g->edge[pidx]) <= 0) break;
            lead[m] = lead[m - 1];
        }
        lead[m] = &w->rib[d];
        nlead++;
    }
    if (bore_of_chain(g, ei) < 0 && !work_patch_leads(w, ei, &ltri, &nltri)) { free(cross); return 0; }
    r = work_edge_in(w, ei, cross, ncross, &in);
    if (r <= 0) { if (r < 0) w->st.ribbon_height_fail++; free(cross); free(ltri); return 1; }
    in.lead = lead;
    in.nlead = nlead;
    in.ltri = ltri;
    in.nltri = nltri;
    w->rev[ei] = (uint8_t)edge_in_reversed(&in);
    w->built[ei] = 1;
    roam_ribbon_build(&in, &w->h, &w->rib[ei]);
    free(cross);
    free(ltri);
    return 1;
}

/* Build the working set over `tile_box` (place frame): graph window grown until
 * every edge the tile touches is inside it, ribbons, crossings. `all` = global mode
 * (select every edge of `tile_box`, one pass). */
/* The graph window of one build: the tile box grown until it holds every edge the
 * tile selects (the passes loop of work_build). Leaves the graph built over the final
 * window in w (w->win, w->passes). Split out of work_build unchanged so the [F3]
 * stale test (td5_roam_tile_input_hash) runs exactly the same window rule. */
static int work_window(TD5_RoamTileCtx *c, Work *w, const double tile_box[4], int all)
{
    TD5_RoamCacheCensus cc;
    double rect[4], win[4];
    int pass, i;

    memcpy(win, tile_box, sizeof win);
    rect[0] = tile_box[0] - ROAM_PAD_TOUCH_M * TD5_ROAM_UPM;
    rect[1] = tile_box[1] - ROAM_PAD_TOUCH_M * TD5_ROAM_UPM;
    rect[2] = tile_box[2] + ROAM_PAD_TOUCH_M * TD5_ROAM_UPM;
    rect[3] = tile_box[3] + ROAM_PAD_TOUCH_M * TD5_ROAM_UPM;

    for (pass = 0; pass < ROAM_GROW_PASSES; pass++) {
        double un[4];
        td5_roam_cache_load_window(win, c->halo, &cc);
        if (w->g_live) { td5_roam_graph_free(&w->g); w->g_live = 0; }
        if (!td5_roam_graph_build(&w->g, win, c->halo, &c->prm)) return 0;
        w->g_live = 1;
        w->passes = pass + 1;
        free(w->sel); free(w->ebox);
        w->sel  = (uint8_t *)calloc((size_t)w->g.nedge + 1u, 1);
        w->ebox = (double *)calloc((size_t)w->g.nedge * 4u + 4u, sizeof(double));
        if (!w->sel || !w->ebox) return 0;
        work_select(w, rect, all);
        if (!work_close_deps(w)) return 0;
        if (all) break;
        memcpy(un, win, sizeof un);
        for (i = 0; i < w->g.nedge; i++) {
            const double *b = &w->ebox[4 * i];
            if (!w->sel[i]) continue;
            if (b[0] < un[0]) un[0] = b[0];
            if (b[1] < un[1]) un[1] = b[1];
            if (b[2] > un[2]) un[2] = b[2];
            if (b[3] > un[3]) un[3] = b[3];
        }
        if (un[0] >= win[0] && un[1] >= win[1] && un[2] <= win[2] && un[3] <= win[3]) break;
        memcpy(win, un, sizeof win);
        if (pass == ROAM_GROW_PASSES - 1)
            TD5_LOG_W(LOG_TAG, "roam tile: window still growing after %d passes", ROAM_GROW_PASSES);
    }
    memcpy(w->win, win, sizeof w->win);
    return 1;
}

static int work_build(TD5_RoamTileCtx *c, Work *w, const double tile_box[4], int all)
{
    double win[4], hwin[4];
    int i, j;
    const uint32_t t0 = td5_plat_time_ms();

    if (!work_window(c, w, tile_box, all)) return 0;
    memcpy(win, w->win, sizeof win);
    {
        /* signals inside the final window */
        double *sig = NULL;
        int nsig = 0, cap = 0, k;
        for (i = 0; i < c->nidx; i++) {
            IdxRow *r = &c->idx[i];
            if (r->layer != 1 || !row_meets(r, win, c->halo)) continue;
            idx_load_signals(c, r);
            for (k = 0; k < r->nsig; k++) {
                if (2 * nsig + 2 > cap) {
                    double *nb;
                    cap = cap ? cap * 2 : 256;
                    nb = (double *)realloc(sig, (size_t)cap * sizeof(double));
                    if (!nb) break;
                    sig = nb;
                }
                sig[2 * nsig] = r->sig[2 * k];
                sig[2 * nsig + 1] = r->sig[2 * k + 1];
                nsig++;
            }
        }
        td5_roam_graph_attach_signals(&w->g, sig, nsig);
        free(sig);
    }
    w->hash = ctx_input_hash(c, win, &w->cache_tiles);
    w->st.ms_graph = td5_plat_time_ms() - t0;

    /* ---- heights ---- */
    {
        const uint32_t t1 = td5_plat_time_ms();
        const TD5_RoamGraph *g = &w->g;
        hwin[0] = win[0] - c->halo - 2000.0; hwin[1] = win[1] - c->halo - 2000.0;
        hwin[2] = win[2] + c->halo + 2000.0; hwin[3] = win[3] + c->halo + 2000.0;
        if (!td5_roam_height_open(&w->h, c->hpath, c->wpath, (double)c->ex100 / 100.0)) return 0;
        w->h_live = 1;
        td5_roam_height_prefetch(&w->h, hwin[0], hwin[1], hwin[2], hwin[3]);

        w->rib    = (RoamRibbon *)calloc((size_t)g->nedge + 1u, sizeof(RoamRibbon));
        w->built  = (uint8_t *)calloc((size_t)g->nedge + 1u, 1);
        w->rev    = (uint8_t *)calloc((size_t)g->nedge + 1u, 1);
        w->hc     = (double *)calloc((size_t)g->ncluster + 1u, sizeof(double));
        w->cspread = (double *)calloc((size_t)g->ncluster + 1u, sizeof(double));
        w->cgr    = (double *)calloc(2u * ((size_t)g->ncluster + 1u), sizeof(double));
        w->cpl    = (uint8_t *)calloc((size_t)g->ncluster + 1u, 1);
        w->hcs    = (uint8_t *)calloc((size_t)g->ncluster + 1u, 1);
        w->apin   = (double *)calloc((size_t)g->narm + 1u, sizeof(double));
        w->apins  = (uint8_t *)calloc((size_t)g->narm + 1u, 1);
        if (!w->rib || !w->built || !w->rev || !w->hc || !w->cspread || !w->cgr || !w->cpl || !w->hcs || !w->apin ||
            !w->apins) return 0;

        /* pass 1: every selected ribbon, each after its dependencies (leaders,
         * the roads above a bore, the bore of an approach) [F1-T10] */
        w->bstate = (uint8_t *)calloc((size_t)g->nedge + 1u, 1);
        w->nlow = (double *)malloc(((size_t)g->nnode + 1u) * sizeof(double));
        if (!w->bstate || !w->nlow) return 0;
        for (i = 0; i <= g->nnode; i++) w->nlow[i] = NAN;
        w->cbox = (double *)malloc(((size_t)g->ncluster + 1u) * 4u * sizeof(double));
        if (!w->cbox) return 0;
        for (i = 0; i < g->ncluster; i++) {
            const TD5_RoamCluster *cl = &g->cluster[i];
            double *cb = &w->cbox[4 * i];
            int v;
            cb[0] = cb[1] = 1e300; cb[2] = cb[3] = -1e300;
            for (v = 0; v < cl->nvert; v++) {
                const TD5_RoamPatchVert *pv = &g->pvert[cl->vert_first + v];
                if (pv->x < cb[0]) cb[0] = pv->x;
                if (pv->z < cb[1]) cb[1] = pv->z;
                if (pv->x > cb[2]) cb[2] = pv->x;
                if (pv->z > cb[3]) cb[3] = pv->z;
            }
        }
        for (i = 0; i < g->nedge; i++)
            if (w->sel[i] && !work_build_rec(w, i, 0)) return 0;
        /* crossings under a bridge deck: the lower edge's pass-1 height */
        for (i = 0; i < g->nedge; i++) {
            const TD5_RoamEdge *p = &g->edge[i];
            RoamCross *list = NULL;
            int nl = 0, cap = 0;
            RoamEdgeIn in;
            if (w->sel[i] != 1 || !p->bridge || p->layer < 1 || !w->built[i]) continue;
            for (j = 0; j < g->nedge; j++) {
                const TD5_RoamEdge *q = &g->edge[j];
                int a, b;
                if (j == i || !w->sel[j] || !w->built[j] || q->layer >= p->layer || q->way == p->way) continue;
                if (w->rib[j].status != ROAM_RIB_OK) continue;
                if (!box_meets(&w->ebox[4 * i], &w->ebox[4 * j])) continue;
                for (a = 0; a + 1 < p->npt; a++) {
                    for (b = 0; b + 1 < q->npt; b++) {
                        const TD5_RoamPt *pa0 = &g->pt[p->pt_first + a], *pa1 = &g->pt[p->pt_first + a + 1];
                        const TD5_RoamPt *qb0 = &g->pt[q->pt_first + b], *qb1 = &g->pt[q->pt_first + b + 1];
                        const TD5_RoamPt *pe0 = &g->pt[p->pt_first], *pe1 = &g->pt[p->pt_first + p->npt - 1];
                        const TD5_RoamPt *qe0 = &g->pt[q->pt_first], *qe1 = &g->pt[q->pt_first + q->npt - 1];
                        double xx, zz, yu, dist;
                        if (!seg_cross(pa0->x, pa0->z, pa1->x, pa1->z, qb0->x, qb0->z, qb1->x, qb1->z, &xx, &zz))
                            continue;
                        /* a node-shared connection sits at an end: not a crossing */
                        if (hypot(xx - pe0->x, zz - pe0->z) < 60.0 || hypot(xx - pe1->x, zz - pe1->z) < 60.0 ||
                            hypot(xx - qe0->x, zz - qe0->z) < 60.0 || hypot(xx - qe1->x, zz - qe1->z) < 60.0)
                            continue;
                        if (!roam_ribbon_y_at(&w->rib[j], xx, zz, &yu, &dist)) continue;
                        if (nl >= cap) {
                            RoamCross *nlst;
                            cap = cap ? cap * 2 : 4;
                            nlst = (RoamCross *)realloc(list, (size_t)cap * sizeof(RoamCross));
                            if (!nlst) { cap = nl; continue; }
                            list = nlst;
                        }
                        list[nl].x = xx; list[nl].z = zz; list[nl].y_under = yu;
                        nl++;
                    }
                }
            }
            if (nl > 0) {
                int r = work_edge_in(w, i, list, nl, &in);
                if (r > 0) {
                    roam_ribbon_free(&w->rib[i]);
                    roam_ribbon_build(&in, &w->h, &w->rib[i]);
                }
            }
            free(list);
        }
        w->st.ms_ribbon = td5_plat_time_ms() - t1;
    }
    w->st.ms = td5_plat_time_ms() - t0;
    return 1;
}

/* ============================================================================
 * SECTION: slice (ownership, relocation into the tile table)
 * ========================================================================== */


typedef struct {
    Vec span, vert, tri, bedge, edgec, nodec, armc, signc;
    Vec names;                     /* char[] blob of every distinct name, sorted */
    Vec nameref;                   /* per EDGE record: the street name (char *)
                                    * in the T1 session pool, NULL = unnamed      */
    int ovf;
} TileBuf;

static void tb_init(TileBuf *b)
{
    vec_init(&b->span, sizeof(TD5_StripSpan));
    vec_init(&b->vert, sizeof(TD5_StripVertex));
    vec_init(&b->tri, sizeof(RoamTri));
    vec_init(&b->bedge, sizeof(RoamBEdge));
    vec_init(&b->edgec, sizeof(RoamEdgeRec));
    vec_init(&b->nodec, sizeof(RoamNodeRec));
    vec_init(&b->armc, sizeof(RoamArmRec));
    vec_init(&b->signc, sizeof(RoamSignRec));
    vec_init(&b->names, 1);
    vec_init(&b->nameref, sizeof(const char *));
    b->ovf = 0;
}
static void tb_free(TileBuf *b)
{
    vec_free(&b->span); vec_free(&b->vert); vec_free(&b->tri); vec_free(&b->bedge);
    vec_free(&b->edgec); vec_free(&b->nodec); vec_free(&b->armc); vec_free(&b->signc);
    vec_free(&b->names); vec_free(&b->nameref);
}
static int tb_oom(const TileBuf *b)
{
    return b->span.oom || b->vert.oom || b->tri.oom || b->bedge.oom || b->edgec.oom ||
           b->nodec.oom || b->armc.oom || b->signc.oom || b->names.oom || b->nameref.oom;
}

static int rib_grip(int surface)
{
    switch (surface) {
    case TD5_GEO_SURF_COBBLE: return 4;
    case TD5_GEO_SURF_LOOSE:  return 2;
    default:                  return 0;
    }
}

/* y of end `end` (T3's end index) of edge ei: the ribbon's solved row height, the
 * shared row height of an absorbed edge, or the arm pin as a last resort. */
/* [F1-T10] The one row an absorbed edge's two patches share: the mean of its two
 * end pins, unless exactly one end is a cluster whose patch must stay flat (a
 * ROW_COVERED or HULL cluster: a cut row of it lies UNDER its patch, flat at H_c).
 * That side's pin then wins, and the slope goes into the other patch. The
 * Diagonal 74 gore: cluster 1130465457611351 (ROW_COVERED, 17156) shares an
 * absorbed row with 1128021621221683 1.35 m higher; the mean tilted the flat
 * patch and the Diagonal 74 cut row under it stood 51-65 units off it.
 * Returns 0 when neither end has a pin. Symmetric: both patches read the same. */
static int work_absorbed_y(Work *w, int ei, double *y)
{
    const TD5_RoamGraph *g = &w->g;
    const TD5_RoamEdge *e = &g->edge[ei];
    double pa, pb;
    const int ra = work_end_pin(w, ei, 0, &pa);
    const int rb = work_end_pin(w, ei, 1, &pb);
    int fa = 0, fb = 0;
    if (ra <= 0 || rb <= 0) return 0;
    if (isnan(pa) && isnan(pb)) return 0;
    if (isnan(pa)) { *y = pb; return 1; }
    if (isnan(pb)) { *y = pa; return 1; }
    if (e->end[0].kind == TD5_ROAM_END_CLUSTER && e->end[0].cluster >= 0)
        fa = (g->cluster[e->end[0].cluster].flags & (TD5_ROAM_CL_ROW_COVERED | TD5_ROAM_CL_HULL)) != 0;
    if (e->end[1].kind == TD5_ROAM_END_CLUSTER && e->end[1].cluster >= 0)
        fb = (g->cluster[e->end[1].cluster].flags & (TD5_ROAM_CL_ROW_COVERED | TD5_ROAM_CL_HULL)) != 0;
    if (fa && !fb) *y = pa;
    else if (fb && !fa) *y = pb;
    else *y = 0.5 * (pa + pb);
    return 1;
}

static int work_end_y(Work *w, int ei, int end, double *y)
{
    const TD5_RoamGraph *g = &w->g;
    const TD5_RoamEdge *e = &g->edge[ei];
    if (w->built[ei] && w->rib[ei].status == ROAM_RIB_OK) {
        const int first = ((end == 0) != (w->rev[ei] != 0));
        *y = first ? w->rib[ei].y_first : w->rib[ei].y_last;
        return 1;
    }
    if (w->built[ei] && w->rib[ei].status == ROAM_RIB_ABSORBED) {
        /* one row shared by the two patches (same x/z by construction), so both
         * sides compute the same y (work_absorbed_y) */
        return work_absorbed_y(w, ei, y);
    }
    (void)e;
    return 0;
}

/* <=0.5 m, <=1, <=2, <=4, >4 */
static int hist_bucket(double m)
{
    return m <= 0.5 ? 0 : (m <= 1.0 ? 1 : (m <= 2.0 ? 2 : (m <= 4.0 ? 3 : 4)));
}

static int tile_is(const RoamGrid *gr, double x, double z, int tx, int tz)
{
    int a, b;
    roam_grid_tile_of(gr, x, z, &a, &b);
    return a == tx && b == tz;
}

static int cmp_str_ptr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Add a vertex of the tile table; returns its index or -1 (overflow). */
static int tb_vert(TileBuf *b, int16_t x, int16_t y, int16_t z)
{
    TD5_StripVertex *v;
    if (b->vert.n >= 65535u) { b->ovf = 1; return -1; }
    v = (TD5_StripVertex *)vec_add(&b->vert);
    if (!v) return -1;
    v->x = x; v->y = y; v->z = z;
    return (int)(b->vert.n - 1);
}

static int i16_ok(int v) { return v >= -32768 && v <= 32767; }

/* Per-primitive lattice-frame extent for the GRID section and the overhang stat. */
typedef struct { double umin, umax, vmin, vmax; } PBox;

static void pbox_init(PBox *b) { b->umin = b->vmin = 1e300; b->umax = b->vmax = -1e300; }
static void pbox_add(PBox *b, const RoamGrid *gr, double x, double z, int tx, int tz)
{
    double u, v;
    roam_grid_uv(gr, x, z, &u, &v);
    u -= (double)tx * (double)gr->tile;
    v -= (double)tz * (double)gr->tile;
    if (u < b->umin) b->umin = u;
    if (u > b->umax) b->umax = u;
    if (v < b->vmin) b->vmin = v;
    if (v > b->vmax) b->vmax = v;
}

static int slice_tile(TD5_RoamTileCtx *c, Work *w, int tx, int tz, RoamTile *out,
                      TD5_RoamTileStats *st);

int td5_roam_tile_build(TD5_RoamTileCtx *c, int tx, int tz, RoamTile *out,
                        TD5_RoamTileStats *st)
{
    TD5_RoamTileStats local;
    Work *w;
    double corners[4][2], box[4];
    int i, ok;
    const uint32_t t0 = td5_plat_time_ms();
    if (!st) st = &local;
    memset(st, 0, sizeof *st);
    memset(out, 0, sizeof *out);
    if (c->have_global) {
        if (!c->gwork) {
            c->gwork = (Work *)calloc(1, sizeof(Work));
            if (!c->gwork || !work_build(c, c->gwork, c->gwin, 1)) {
                if (c->gwork) { work_free(c->gwork); c->gwork = NULL; }
                return 0;
            }
        }
        w = c->gwork;
    } else {
        w = (Work *)calloc(1, sizeof(Work));
        if (!w) return 0;
        roam_grid_corners(&c->grid, tx, tz, corners);
        box[0] = box[2] = corners[0][0];
        box[1] = box[3] = corners[0][1];
        for (i = 1; i < 4; i++) {
            if (corners[i][0] < box[0]) box[0] = corners[i][0];
            if (corners[i][1] < box[1]) box[1] = corners[i][1];
            if (corners[i][0] > box[2]) box[2] = corners[i][0];
            if (corners[i][1] > box[3]) box[3] = corners[i][1];
        }
        if (!work_build(c, w, box, 0)) { work_free(w); return 0; }
    }
    ok = slice_tile(c, w, tx, tz, out, st);
    st->passes = w->passes;
    st->cache_tiles = w->cache_tiles;
    st->ms_graph = w->st.ms_graph;
    st->ms_ribbon = w->st.ms_ribbon;
    st->dep_cycles = w->st.dep_cycles;
    if (!c->have_global) work_free(w);
    st->ms = td5_plat_time_ms() - t0;
    return ok;
}

/* [F3] The input hash a fresh build of (tx,tz) would stamp, without building it:
 * the window rule of the build (work_window) and the same ctx_input_hash. Costs
 * the graph phase only. 0 on failure (GLOBAL mode included: no per-tile window). */
int td5_roam_tile_input_hash(TD5_RoamTileCtx *c, int tx, int tz, uint32_t *out)
{
    Work *w;
    double corners[4][2], box[4];
    int i, ok;
    if (!c || !out || c->have_global) return 0;
    w = (Work *)calloc(1, sizeof(Work));
    if (!w) return 0;
    roam_grid_corners(&c->grid, tx, tz, corners);
    box[0] = box[2] = corners[0][0];
    box[1] = box[3] = corners[0][1];
    for (i = 1; i < 4; i++) {
        if (corners[i][0] < box[0]) box[0] = corners[i][0];
        if (corners[i][1] < box[1]) box[1] = corners[i][1];
        if (corners[i][0] > box[2]) box[2] = corners[i][0];
        if (corners[i][1] > box[3]) box[3] = corners[i][1];
    }
    ok = work_window(c, w, box, 0);
    if (ok) *out = ctx_input_hash(c, w->win, NULL);
    work_free(w);
    return ok;
}

/* ---- the slice ---- */

static int slice_tile(TD5_RoamTileCtx *c, Work *w, int tx, int tz, RoamTile *out,
                      TD5_RoamTileStats *st)
{
    const RoamGrid *gr = &c->grid;
    const TD5_RoamGraph *g = &w->g;
    TileBuf b;
    int32_t org_x, org_z;
    int ei, ci, k;
    int ok = 0;
    const uint32_t t0 = td5_plat_time_ms();
    int *tspan = NULL;                      /* per quad of the current ribbon          */
    int tspan_cap = 0;
    int *cut_first = NULL, *cut_last = NULL;/* per edge: tile span of ribbon quad 0 / the last quad, -1 */
    uint8_t *owned = NULL;
    PBox *pspan = NULL, *ptri = NULL, *pbed = NULL;   /* lattice extents of the primitives */

    tb_init(&b);
    roam_grid_origin(gr, tx, tz, &org_x, &org_z);
    cut_first = (int *)malloc(((size_t)g->nedge + 1u) * sizeof(int));
    cut_last  = (int *)malloc(((size_t)g->nedge + 1u) * sizeof(int));
    if (!cut_first || !cut_last) goto done;
    for (ei = 0; ei < g->nedge; ei++) cut_first[ei] = cut_last[ei] = -1;

    /* carry the build-phase counters */
    st->ribbon_height_fail += w->st.ribbon_height_fail;
    st->arm_clamped = w->st.arm_clamped;
    st->max_blend_m = w->st.max_blend_m;

    /* ---------------- ribbons: owned quads ---------------- */
    for (ei = 0; ei < g->nedge; ei++) {
        const RoamRibbon *r;
        const TD5_RoamEdge *e;
        int nq, per_row, L, q, blk, nown = 0;
        int first_end;
        if (w->sel[ei] != 1 || !w->built[ei]) continue;
        r = &w->rib[ei];
        e = &g->edge[ei];
        if (r->status == ROAM_RIB_ABSORBED) { st->ribbon_absorbed++; continue; }
        if (r->status == ROAM_RIB_BAD_INPUT) { st->ribbon_bad++; continue; }
        if (r->status == ROAM_RIB_HEIGHT) { st->ribbon_height_fail++; continue; }
        if (r->status != ROAM_RIB_OK) continue;
        nq = r->nquad;
        L = r->lanes;
        per_row = L + 1;
        if (nq > tspan_cap) {
            int *nt = (int *)realloc(tspan, (size_t)nq * sizeof(int));
            uint8_t *no = (uint8_t *)realloc(owned, (size_t)nq);
            if (!nt || !no) { free(nt ? nt : tspan); free(no ? no : owned); tspan = NULL; owned = NULL; goto done; }
            tspan = nt; owned = no;
            tspan_cap = nq;
        }
        for (q = 0; q < nq; q++) {
            owned[q] = (uint8_t)tile_is(gr, (double)r->meta[q].mx, (double)r->meta[q].mz, tx, tz);
            nown += owned[q];
        }
        if (nown == 0) continue;
        {
            /* span indices in this tile, ribbon order */
            int next = (int)b.span.n;
            for (q = 0; q < nq; q++) tspan[q] = owned[q] ? next++ : -1;
            if (next > 32767) { st->overflow++; goto done; }
            cut_first[ei] = tspan[0];
            cut_last[ei]  = tspan[nq - 1];
        }
        first_end = (w->rev[ei] != 0) ? 1 : 0;      /* the T3 end at ribbon row 0 */
        for (blk = 0; blk * ROAM_RIB_BLOCK < nq; blk++) {
            const int r0 = blk * ROAM_RIB_BLOCK;
            const int ns = (r0 + ROAM_RIB_BLOCK <= nq) ? ROAM_RIB_BLOCK : nq - r0;
            int kmin = -1, kmax = -1, newbase, rr, j;
            for (q = 0; q < ns; q++) {
                if (!owned[r0 + q]) continue;
                if (kmin < 0) kmin = q;
                kmax = q;
            }
            if (kmin < 0) continue;
            newbase = (int)b.vert.n;
            for (rr = kmin; rr <= kmax + 1; rr++) {
                for (j = 0; j < per_row; j++) {
                    const TD5_StripVertex *sv = &r->vert[(size_t)blk * (ROAM_RIB_BLOCK + 1) * per_row +
                                                         (size_t)rr * per_row + j];
                    if (tb_vert(&b, sv->x, sv->y, sv->z) < 0) goto done;
                }
            }
            for (q = kmin; q <= kmax; q++) {
                TD5_StripSpan *sp;
                const int gq = r0 + q;
                if (!owned[gq]) continue;
                sp = (TD5_StripSpan *)vec_add(&b.span);
                if (!sp) goto done;
                *sp = r->span[gq];
                sp->left_vertex_index  = (uint16_t)(newbase + (q - kmin) * per_row);
                sp->right_vertex_index = (uint16_t)(newbase + (q - kmin + 1) * per_row);
                sp->link_next = (int16_t)((gq + 1 < nq && owned[gq + 1]) ? tspan[gq + 1] : -1);
                sp->link_prev = (int16_t)((gq > 0 && owned[gq - 1]) ? tspan[gq - 1] : -1);
                sp->origin_x -= org_x;
                sp->origin_z -= org_z;
                {
                    /* wall edges: both kerbs, plus a dead-end cap on the end rows */
                    RoamBEdge *be;
                    const uint8_t kind = (e->bridge && e->layer >= 1) ? ROAM_BEDGE_DECK : ROAM_BEDGE_KERB;
                    be = (RoamBEdge *)vec_add(&b.bedge);
                    if (!be) goto done;
                    be->v[0] = sp->left_vertex_index; be->v[1] = sp->right_vertex_index;
                    be->prim = (uint16_t)tspan[gq]; be->kind = kind; be->layer = (uint8_t)(int8_t)e->layer;
                    be = (RoamBEdge *)vec_add(&b.bedge);
                    if (!be) goto done;
                    be->v[0] = (uint16_t)(sp->left_vertex_index + L);
                    be->v[1] = (uint16_t)(sp->right_vertex_index + L);
                    be->prim = (uint16_t)tspan[gq]; be->kind = kind; be->layer = (uint8_t)(int8_t)e->layer;
                    if (gq == 0 && e->end[first_end].kind == TD5_ROAM_END_DEAD) {
                        be = (RoamBEdge *)vec_add(&b.bedge);
                        if (!be) goto done;
                        be->v[0] = sp->left_vertex_index; be->v[1] = (uint16_t)(sp->left_vertex_index + L);
                        be->prim = (uint16_t)tspan[gq]; be->kind = ROAM_BEDGE_KERB; be->layer = (uint8_t)(int8_t)e->layer;
                    }
                    if (gq == nq - 1 && e->end[1 - first_end].kind == TD5_ROAM_END_DEAD) {
                        be = (RoamBEdge *)vec_add(&b.bedge);
                        if (!be) goto done;
                        be->v[0] = sp->right_vertex_index; be->v[1] = (uint16_t)(sp->right_vertex_index + L);
                        be->prim = (uint16_t)tspan[gq]; be->kind = ROAM_BEDGE_KERB; be->layer = (uint8_t)(int8_t)e->layer;
                    }
                }
            }
        }
        /* one EDGE record per contiguous run of owned quads */
        for (q = 0; q < nq;) {
            RoamEdgeRec *er;
            int q2;
            const char *nm;
            if (!owned[q]) { q++; continue; }
            for (q2 = q; q2 < nq && owned[q2]; q2++) {}
            er = (RoamEdgeRec *)vec_add(&b.edgec);
            if (!er) goto done;
            er->key = e->key;
            for (k = 0; k < 2; k++) {
                const TD5_RoamEdgeEnd *en = &e->end[k];
                er->end_key[k] = (en->cluster >= 0 && (en->kind == TD5_ROAM_END_CLUSTER ||
                                  en->kind == TD5_ROAM_END_INTERNAL)) ? g->cluster[en->cluster].key
                                                                       : g->node[en->node].key;
                er->end_kind[k] = (uint8_t)en->kind;
            }
            er->klass = (uint8_t)e->klass; er->lanes = (uint8_t)e->lanes;
            er->oneway = (uint8_t)(int8_t)e->oneway; er->surface = (uint8_t)e->surface;
            er->layer = (int8_t)e->layer;
            er->flags = (uint8_t)(((e->bridge && e->layer >= 1) ? ROAM_EF_DECK : 0) |
                                  (e->tunnel == 1 ? ROAM_EF_BORE : 0) | (e->tunnel == 2 ? ROAM_EF_PASSAGE : 0) |
                                  (e->roundabout ? ROAM_EF_ROUNDABOUT : 0) |
                                  ((r->flags & ROAM_RIBF_RAMP_SHORT) ? ROAM_EF_RAMP_SHORT : 0) |
                                  ((r->flags & ROAM_RIBF_PIN_MISS) ? ROAM_EF_PIN_MISS : 0));
            nm = e->name_id >= 0 ? td5_roam_cache_name(e->name_id) : NULL;
            er->name_id = 0xFFFFu;              /* remapped to the sorted index below */
            {
                const char **ref = (const char **)vec_add(&b.nameref);
                if (!ref) goto done;
                *ref = (nm && nm[0]) ? nm : NULL;
            }
            er->length = td5_roam_quant(e->length);
            er->span_first = (uint16_t)tspan[q];
            er->span_count = (uint16_t)(q2 - q);
            er->row0 = (uint16_t)q;
            er->maxspeed_kph = (uint8_t)(e->maxspeed_kph > 255 ? 255 : (e->maxspeed_kph < 0 ? 0 : e->maxspeed_kph));
            q = q2;
        }
        /* census: ownership on the lattice */
        {
            double u, v;
            for (q = 0; q < nq; q++) {
                int ph;
                if (!owned[q]) continue;
                st->quads++;
                roam_grid_uv(gr, (double)r->meta[q].mx, (double)r->meta[q].mz, &u, &v);
                for (ph = 0; ph < 16; ph++) {
                    const double S = (double)gr->tile;
                    double uu = u + (double)(ph >> 2) * (S * 0.25), vv = v + (double)(ph & 3) * (S * 0.25);
                    double fu = uu - floor(uu / S) * S, fv = vv - floor(vv / S) * S;
                    if (fu > S - fu) fu = S - fu;
                    if (fv > S - fv) fv = S - fv;
                    if (fu < 2.0 * TD5_ROAM_UPM || fv < 2.0 * TD5_ROAM_UPM) st->hug_phase[ph]++;
                }
            }
        }
        if (r->flags & ROAM_RIBF_RAMP_SHORT) st->ramp_short++;
        if (r->flags & ROAM_RIBF_RAMP_FAIL) st->ramp_fail++;
        if (r->flags & ROAM_RIBF_PIN_MISS) st->pin_miss++;
        if (r->flags & ROAM_RIBF_I16) st->i16_overflow++;
        st->edges_rec++;
        /* [F1-T10] coupling and grade over the owned quads */
        if (r->couple_rows > 0) {
            st->couple_ribbons++;
            st->couple_rows += r->couple_rows;
            if (r->couple_max_dy / TD5_ROAM_UPM > st->couple_max_dy_m) st->couple_max_dy_m = r->couple_max_dy / TD5_ROAM_UPM;
        }
        for (q = 0; q < nq; q++) {
            double run, gq;
            if (!owned[q]) continue;
            run = hypot(r->cx[q + 1] - r->cx[q], r->cz[q + 1] - r->cz[q]);
            if (run < 1.0) continue;
            gq = fabs(r->y[q + 1] - r->y[q]) / run;
            if (gq > st->max_grade) st->max_grade = gq;
            if (gq > ROAM_GRADE_CAP * 1.0005 + 1e-6) st->grade_over_cap++;
        }
        if (e->tunnel == 1 && r->clear_n > 0) {
            st->bore_cross += r->clear_n;
            if (st->bore_min_clear_m == 0.0 || r->clear_min / TD5_ROAM_UPM < st->bore_min_clear_m)
                st->bore_min_clear_m = r->clear_min / TD5_ROAM_UPM;
        }
    }
    st->hug_quads = st->hug_phase[0];
    st->edges_built = 0;
    for (ei = 0; ei < g->nedge; ei++) if (w->built[ei]) st->edges_built++;

    /* ---------------- junction clusters: owned patches ---------------- */
    for (ci = 0; ci < g->ncluster; ci++) {
        const TD5_RoamCluster *cl = &g->cluster[ci];
        double hc, arm_y[TD5_ROAM_ARM_MAX_LOCAL];
        int i, base, ntri0;
        int32_t cox, coz, coy;   /* node position (cluster centroid, patch centre height) */
        int32_t vox, voz, voy;   /* origin of the patch's int16 vertex offsets            */
        int surf = 0, bestarm = -1;
        double bw = -1.0;
        RoamNodeRec *nr;
        if (!tile_is(gr, cl->cx, cl->cz, tx, tz)) continue;
        if (cl->narm > TD5_ROAM_ARM_MAX_LOCAL) { st->overflow++; continue; }
        if (cl->flags & TD5_ROAM_CL_FAIL) st->cluster_fail++;
        if (!work_hc(w, ci, &hc)) { st->clusters_no_height++; continue; }
        if (w->cspread[ci] > ROAM_SPREAD_CLAMP_M * TD5_ROAM_UPM) st->spread_clusters++;
        if (w->cspread[ci] / TD5_ROAM_UPM > st->max_spread_m) st->max_spread_m = w->cspread[ci] / TD5_ROAM_UPM;
        st->spread_hist[hist_bucket(w->cspread[ci] / TD5_ROAM_UPM)]++;
        {
            double crest = 0.0;
            for (i = 0; i < cl->narm; i++) {
                const TD5_RoamArm *a = &g->arm[cl->arm_first + i];
                const TD5_RoamEdge *ae = &g->edge[a->edge];
                double dy;
                if (!work_end_y(w, a->edge, a->end, &arm_y[i])) {
                    double pin;
                    arm_y[i] = work_arm_pin(w, cl->arm_first + i, &pin) ? pin : hc;
                    st->ribbon_bad++;
                }
                {
                    /* [F1-T10] against the cluster height AT the arm (its plane
                     * on a sloped cluster): a plane is not a crest */
                    const TD5_RoamEdgeEnd *aen = &ae->end[a->end];
                    double ref = hc;
                    if (aen->nrow >= 1)
                        ref = work_plane_y(w, ci, 0.5 * (aen->row[0].x + aen->row[aen->nrow - 1].x),
                                           0.5 * (aen->row[0].z + aen->row[aen->nrow - 1].z));
                    dy = fabs(arm_y[i] - ref) / TD5_ROAM_UPM;
                }
                if (dy > crest) crest = dy;
                if (dy > st->max_patch_dy_m) st->max_patch_dy_m = dy;
                if (ae->half_w > bw) { bw = ae->half_w; bestarm = i; }
            }
            st->pdy_hist[hist_bucket(crest)]++;
            if (crest > 2.0)
                TD5_LOG_W(LOG_TAG, "ROAM_CREST cluster %lld at (%.0f, %.0f): arm cut rows differ %.2f m from the "
                          "patch centre (ground spread %.2f m, %d arm(s), layer %d, flags 0x%x)",
                          (long long)cl->key, cl->cx, cl->cz, crest, w->cspread[ci] / TD5_ROAM_UPM, cl->narm,
                          cl->layer, (unsigned)cl->flags);
        }
        if (bestarm >= 0) surf = 0x10 | (rib_grip(g->edge[g->arm[cl->arm_first + bestarm].edge].surface) & 0x0F);
        cox = td5_roam_quant(cl->cx);
        coz = td5_roam_quant(cl->cz);
        coy = td5_roam_quant(hc);
        /* Vertex-offset origin. The centroid, unless some vertex then leaves int16 (a UNION
         * patch of a wide multi-arm cluster reaches > 76 m from its centroid, F1-T5b): the
         * midpoint of the patch's own bounding box is then used, which halves the reach.
         * The triangles carry this origin (RoamTri.ox/oy/oz), the NODE keeps the centroid. */
        vox = cox; voz = coz; voy = coy;
        {
            int bad = 0;
            int32_t mnx = INT32_MAX, mxx = INT32_MIN, mnz = INT32_MAX, mxz = INT32_MIN;
            int32_t mny = INT32_MAX, mxy = INT32_MIN;
            for (i = 0; i < cl->nvert; i++) {
                const TD5_RoamPatchVert *pv = &g->pvert[cl->vert_first + i];
                const int32_t yq = td5_roam_quant(work_vert_y(w, ci, pv, hc, arm_y));
                if (!i16_ok(pv->xq - cox) || !i16_ok(pv->zq - coz) || !i16_ok(yq - coy)) bad = 1;
                if (pv->xq < mnx) mnx = pv->xq;
                if (pv->xq > mxx) mxx = pv->xq;
                if (pv->zq < mnz) mnz = pv->zq;
                if (pv->zq > mxz) mxz = pv->zq;
                if (yq < mny) mny = yq;
                if (yq > mxy) mxy = yq;
            }
            if (bad && cl->nvert > 0) {
                vox = (int32_t)(((int64_t)mnx + mxx) >> 1);
                voz = (int32_t)(((int64_t)mnz + mxz) >> 1);
                voy = (int32_t)(((int64_t)mny + mxy) >> 1);
                TD5_LOG_W(LOG_TAG, "ROAM_I16_REORIGIN cluster %lld at (%.0f, %.0f): vertex reach from the centroid exceeds int16, "
                          "origin moved (%.1f, %.1f, %.1f) m to the patch bbox centre (flags 0x%x, %d arm(s), %d vert(s))",
                          (long long)cl->key, cl->cx, cl->cz, (vox - cox) / TD5_ROAM_UPM, (voy - coy) / TD5_ROAM_UPM,
                          (voz - coz) / TD5_ROAM_UPM, (unsigned)cl->flags, cl->narm, cl->nvert);
                st->i16_reorigin++;
            }
        }
        base = (int)b.vert.n;
        for (i = 0; i < cl->nvert; i++) {
            const TD5_RoamPatchVert *pv = &g->pvert[cl->vert_first + i];
            const double yv = work_vert_y(w, ci, pv, hc, arm_y);
            const int yq = td5_roam_quant(yv);
            const int dx = pv->xq - vox, dz = pv->zq - voz, dyv = yq - voy;
            if (!i16_ok(dx) || !i16_ok(dyv) || !i16_ok(dz)) {
                st->i16_overflow++;
                TD5_LOG_W(LOG_TAG, "ROAM_I16 cluster %lld vert %d/%d kind %d: dx %d dy %d dz %d (%.1f, %.1f, %.1f m) flags 0x%x narm %d",
                          (long long)cl->key, i, cl->nvert, (int)pv->kind, dx, dyv, dz,
                          dx / TD5_ROAM_UPM, dyv / TD5_ROAM_UPM, dz / TD5_ROAM_UPM, (unsigned)cl->flags, cl->narm);
            }
            if (tb_vert(&b, (int16_t)dx, (int16_t)dyv, (int16_t)dz) < 0) goto done;
            /* every cut vertex must be a vertex of its arm's ribbon row, same y */
            if (pv->kind == TD5_ROAM_PV_CUT && pv->ref[0] >= 0) {
                const int gi = cl->arm_first + pv->ref[0];
                const TD5_RoamArm *a = &g->arm[gi];
                const int aei = a->edge;
                if (w->built[aei] && w->rib[aei].status == ROAM_RIB_OK) {
                    int32_t rowv[ROAM_RIB_MAX_LANES + 1][3];
                    const int first = ((a->end == 0) != (w->rev[aei] != 0));
                    const int n = roam_ribbon_row_world(&w->rib[aei], first ? 0 : w->rib[aei].nrows - 1, rowv);
                    int j2, hit = 0;
                    st->cut_checked++;
                    for (j2 = 0; j2 < n; j2++)
                        if (rowv[j2][0] == pv->xq && rowv[j2][2] == pv->zq) { hit = 1; if (rowv[j2][1] != yq) hit = 2; break; }
                    if (hit != 1) st->cut_mismatch++;
                }
            }
        }
        ntri0 = (int)b.tri.n;
        for (i = 0; i < cl->ntri; i++) {
            const TD5_RoamPatchTri *pt = &g->ptri[cl->tri_first + i];
            RoamTri *tr;
            if (b.tri.n >= 32767u) { st->overflow++; goto done; }
            tr = (RoamTri *)vec_add(&b.tri);
            if (!tr) goto done;
            for (k = 0; k < 3; k++) tr->v[k] = (uint16_t)(base + (pt->v[k] - cl->vert_first));
            tr->patch = (uint16_t)b.nodec.n;
            tr->surface = (uint8_t)surf;
            tr->flags = (uint8_t)(cl->flags & 0xFF);
            tr->ox = vox - org_x; tr->oy = voy; tr->oz = voz - org_z;
        }
        /* wall edges: the patch kerb segments, each on a triangle that holds them */
        for (i = 0; i < cl->nol; i++) {
            const TD5_RoamOutline *ol = &g->outline[cl->ol_first + i];
            RoamBEdge *be;
            int t, found = -1, half = -1;
            if (ol->kind != TD5_ROAM_OL_KERB) continue;
            for (t = 0; t < cl->ntri && found < 0; t++) {
                const TD5_RoamPatchTri *pt = &g->ptri[cl->tri_first + t];
                int h0 = 0, h1 = 0;
                for (k = 0; k < 3; k++) { if (pt->v[k] == ol->v[0]) h0 = 1; if (pt->v[k] == ol->v[1]) h1 = 1; }
                if (h0 && h1) found = t;
                else if ((h0 || h1) && half < 0) half = t;
            }
            if (found < 0) { found = half; st->bedge_no_tri++; }
            if (found < 0) continue;
            be = (RoamBEdge *)vec_add(&b.bedge);
            if (!be) goto done;
            be->v[0] = (uint16_t)(base + (ol->v[0] - cl->vert_first));
            be->v[1] = (uint16_t)(base + (ol->v[1] - cl->vert_first));
            be->prim = (uint16_t)(ROAM_PRIM_TRI | (uint16_t)(ntri0 + found));
            be->kind = ROAM_BEDGE_KERB;
            be->layer = (uint8_t)(int8_t)cl->layer;
        }
        /* node, arms, signals */
        nr = (RoamNodeRec *)vec_add(&b.nodec);
        if (!nr) goto done;
        nr->cluster_key = cl->key;
        nr->x = cox - org_x; nr->y = coy; nr->z = coz - org_z;
        nr->narm = (uint8_t)cl->narm;
        nr->flags = (uint16_t)cl->flags;
        nr->tri_first = (uint16_t)ntri0;
        nr->tri_count = (uint16_t)cl->ntri;
        nr->arm_first = (uint16_t)b.armc.n;
        nr->vert_first = (uint16_t)base;
        for (i = 0; i < cl->narm; i++) {
            const TD5_RoamArm *a = &g->arm[cl->arm_first + i];
            const TD5_RoamEdge *ae = &g->edge[a->edge];
            RoamArmRec *ar = (RoamArmRec *)vec_add(&b.armc);
            double ang;
            int q4;
            if (!ar) goto done;
            ar->edge_key = ae->key;
            ang = atan2(a->dir_z, a->dir_x);
            q4 = td5_roam_quant(ang * 4096.0 / 6.283185307179586);
            ar->bearing12 = (int16_t)(((q4 % 4096) + 4096) % 4096);
            {
                /* the owned quad of the arm's edge that touches the cut row, if any */
                const int first_end = ((a->end == 0) != (w->rev[a->edge] != 0));
                const int cs = first_end ? cut_first[a->edge] : cut_last[a->edge];
                ar->cut_span = cs >= 0 ? (uint16_t)cs : 0xFFFFu;
            }
            ar->end = (uint8_t)a->end;
            ar->lanes = (uint8_t)ae->lanes;
            ar->flags = (uint8_t)(a->corner ? 1 : 0);
            if (ae->end[a->end].signal) {
                RoamSignRec *sg = (RoamSignRec *)vec_add(&b.signc);
                if (!sg) goto done;
                sg->cluster_key = cl->key;
                sg->arm = (uint8_t)i;
                sg->kind = ROAM_SIGN_SIGNAL;
                st->signals++;
            }
        }
        st->clusters++;
        st->arms += cl->narm;
    }
    st->tris = (int)b.tri.n;
    st->verts = (int)b.vert.n;
    st->bedges = (int)b.bedge.n;
    if (tb_oom(&b)) goto done;
    if (b.ovf) { st->overflow++; goto done; }

    /* ---------------- names ---------------- */
    {
        /* distinct names sorted by string; EDGE.name_id = index. The session's own
         * name ids depend on load order, so the strings are re-interned here. */
        size_t i, nn = 0;
        const char **list = (const char **)calloc(b.edgec.n + 1u, sizeof(char *));
        if (!list) goto done;
        for (i = 0; i < b.nameref.n; i++) {
            const char *s = VEC_AT(b.nameref, const char *, i);
            size_t j;
            if (!s) continue;
            for (j = 0; j < nn; j++) if (strcmp(list[j], s) == 0) break;
            if (j == nn) list[nn++] = s;
        }
        qsort(list, nn, sizeof(char *), cmp_str_ptr);
        for (i = 0; i < nn; i++) {
            const size_t ln = strlen(list[i]) + 1u;
            size_t z;
            for (z = 0; z < ln; z++) { char *ch = (char *)vec_add(&b.names); if (!ch) { free(list); goto done; } *ch = list[i][z]; }
        }
        for (i = 0; i < b.edgec.n; i++) {
            const char *s = VEC_AT(b.nameref, const char *, i);
            size_t j;
            if (!s) continue;
            for (j = 0; j < nn; j++) if (strcmp(list[j], s) == 0) break;
            VEC_AT(b.edgec, RoamEdgeRec, i).name_id = (uint16_t)j;
        }
        free(list);
    }

    /* ---------------- GRID + overhang census ---------------- */
    {
        const double S = (double)gr->tile;
        const double cell = (double)ROAM_GRID_CELL;
        const uint32_t nspan = (uint32_t)b.span.n, ntri = (uint32_t)b.tri.n, nbed = (uint32_t)b.bedge.n;
        uint32_t nprim = nspan + ntri + nbed, p;
        int margin, nx, nz, ncell;
        double overhang = 0.0;
        uint32_t *start = NULL, *item = NULL, *fill = NULL;
        size_t nitem = 0;
        pspan = (PBox *)malloc((size_t)(nspan + 1u) * sizeof(PBox));
        ptri  = (PBox *)malloc((size_t)(ntri + 1u) * sizeof(PBox));
        pbed  = (PBox *)malloc((size_t)(nbed + 1u) * sizeof(PBox));
        if (!pspan || !ptri || !pbed) goto done;
        for (p = 0; p < nspan; p++) {
            const TD5_StripSpan *sp = &VEC_AT(b.span, TD5_StripSpan, p);
            const int L = (int)(sp->pad_02[1] & 0x0F);
            int j, row;
            pbox_init(&pspan[p]);
            for (row = 0; row < 2; row++) {
                const int vi = row ? sp->right_vertex_index : sp->left_vertex_index;
                for (j = 0; j <= L; j++) {
                    const TD5_StripVertex *v = &VEC_AT(b.vert, TD5_StripVertex, vi + j);
                    pbox_add(&pspan[p], gr, (double)(org_x + sp->origin_x + v->x),
                             (double)(org_z + sp->origin_z + v->z), tx, tz);
                }
            }
        }
        for (p = 0; p < ntri; p++) {
            const RoamTri *tr = &VEC_AT(b.tri, RoamTri, p);
            pbox_init(&ptri[p]);
            for (k = 0; k < 3; k++) {
                const TD5_StripVertex *v = &VEC_AT(b.vert, TD5_StripVertex, tr->v[k]);
                pbox_add(&ptri[p], gr, (double)(org_x + tr->ox + v->x), (double)(org_z + tr->oz + v->z), tx, tz);
            }
        }
        for (p = 0; p < nbed; p++) {
            const RoamBEdge *be = &VEC_AT(b.bedge, RoamBEdge, p);
            int ox2, oz2;
            pbox_init(&pbed[p]);
            if (be->prim & ROAM_PRIM_TRI) {
                const RoamTri *tr = &VEC_AT(b.tri, RoamTri, be->prim & 0x7FFF);
                ox2 = tr->ox; oz2 = tr->oz;
            } else {
                const TD5_StripSpan *sp = &VEC_AT(b.span, TD5_StripSpan, be->prim);
                ox2 = sp->origin_x; oz2 = sp->origin_z;
            }
            for (k = 0; k < 2; k++) {
                const TD5_StripVertex *v = &VEC_AT(b.vert, TD5_StripVertex, be->v[k]);
                pbox_add(&pbed[p], gr, (double)(org_x + ox2 + v->x), (double)(org_z + oz2 + v->z), tx, tz);
            }
        }
        for (p = 0; p < nspan + ntri; p++) {
            const PBox *pb = p < nspan ? &pspan[p] : &ptri[p - nspan];
            double o = 0.0;
            if (-pb->umin > o) o = -pb->umin;
            if (pb->umax - S > o) o = pb->umax - S;
            if (-pb->vmin > o) o = -pb->vmin;
            if (pb->vmax - S > o) o = pb->vmax - S;
            if (o > overhang) overhang = o;
            if (p < nspan && o > 0.0) st->border_quads++;
        }
        st->max_overhang_m = overhang / TD5_ROAM_UPM;
        margin = (int)ceil(overhang / cell);
        if (margin < 1) margin = 1;
        nx = nz = ROAM_GRID_CELLS + 2 * margin;
        ncell = nx * nz;
        start = (uint32_t *)calloc((size_t)ncell + 2u, sizeof(uint32_t));
        fill  = (uint32_t *)calloc((size_t)ncell + 2u, sizeof(uint32_t));
        if (!start || !fill) { free(start); free(fill); goto done; }
        /* pass 0 counts, pass 1 fills */
        {
            int pass;
            for (pass = 0; pass < 2; pass++) {
                if (pass == 1) {
                    size_t acc = 0;
                    int cc;
                    for (cc = 0; cc < ncell; cc++) { const uint32_t n0 = start[cc]; start[cc] = (uint32_t)acc; acc += n0; }
                    start[ncell] = (uint32_t)acc;
                    nitem = acc;
                    item = (uint32_t *)malloc((nitem ? nitem : 1u) * sizeof(uint32_t));
                    if (!item) { free(start); free(fill); goto done; }
                    memset(fill, 0, (size_t)(ncell + 1) * sizeof(uint32_t));
                }
                for (p = 0; p < nprim; p++) {
                    const PBox *pb;
                    uint32_t kind, idx;
                    int cu0, cu1, cv0, cv1, cu, cv;
                    if (p < nspan)             { pb = &pspan[p]; kind = ROAM_ITEM_SPAN; idx = p; }
                    else if (p < nspan + ntri) { pb = &ptri[p - nspan]; kind = ROAM_ITEM_TRI; idx = p - nspan; }
                    else                       { pb = &pbed[p - nspan - ntri]; kind = ROAM_ITEM_BEDGE; idx = p - nspan - ntri; }
                    cu0 = (int)floor(pb->umin / cell); cu1 = (int)floor(pb->umax / cell);
                    cv0 = (int)floor(pb->vmin / cell); cv1 = (int)floor(pb->vmax / cell);
                    if (cu0 < -margin) cu0 = -margin;
                    if (cv0 < -margin) cv0 = -margin;
                    if (cu1 > ROAM_GRID_CELLS - 1 + margin) cu1 = ROAM_GRID_CELLS - 1 + margin;
                    if (cv1 > ROAM_GRID_CELLS - 1 + margin) cv1 = ROAM_GRID_CELLS - 1 + margin;
                    for (cv = cv0; cv <= cv1; cv++) {
                        for (cu = cu0; cu <= cu1; cu++) {
                            const int cc = (cv + margin) * nx + (cu + margin);
                            if (pass == 0) start[cc]++;
                            else item[start[cc] + fill[cc]++] = (kind << 30) | idx;
                        }
                    }
                }
            }
        }
        /* keep the grid as a flat word array in the section staging */
        {
            const size_t words = 5u + (size_t)ncell + 1u + nitem;
            uint32_t *gw = (uint32_t *)calloc(words, sizeof(uint32_t));
            if (!gw) { free(start); free(fill); free(item); goto done; }
            gw[0] = (uint32_t)nx; gw[1] = (uint32_t)nz; gw[2] = (uint32_t)ROAM_GRID_CELL;
            gw[3] = (uint32_t)margin; gw[4] = 0u;
            memcpy(gw + 5, start, ((size_t)ncell + 1u) * sizeof(uint32_t));
            if (nitem) memcpy(gw + 5 + ncell + 1, item, nitem * sizeof(uint32_t));
            free(start); free(fill); free(item);

            /* ---------------- stamp text + serialise ---------------- */
            {
                char stamp[1024];
                RoamFileHeader hdr;
                RoamSectEnt se[ROAM_NSECT];
                size_t sz[ROAM_NSECT], cnt[ROAM_NSECT], off, total, i;
                uint8_t *img;
                uint32_t trailer;
                const void *src[ROAM_NSECT];
                size_t nnames = 0;
                const TD5_RoamGraphParams *pp = &c->prm;
                int sn;
                for (i = 0; i < b.names.n; i++) if (b.names.p[i] == 0) nnames++;
                sn = snprintf(stamp, sizeof stamp,
                    "TD5ROAM tile %d,%d place %s format %u rules %u input %08x\n"
                    "origin %d,%d lattice c=%.17g s=%.17g size %d ox=%.17g oz=%.17g\n"
                    "halo_m %.0f pad_touch_m %.0f spread_clamp_m %.2f exaggeration_x100 %d\n"
                    "graph join_defl %.1f merge %.1f cap %.1f kerb_minor %.1f kerb_major %.1f segs %d tri_max %.0f\n"
                    "counts spans %u verts %u tris %u bedges %u edges %u nodes %u arms %u signs %u names %u grid %dx%d margin %d items %u\n",
                    tx, tz, c->place, ROAM_FORMAT_VERSION, ROAM_RULES_VERSION, w->hash,
                    (int)org_x, (int)org_z, gr->c, gr->s, (int)gr->tile, gr->ox, gr->oz,
                    ROAM_HALO_M, ROAM_PAD_TOUCH_M, ROAM_SPREAD_CLAMP_M, c->ex100,
                    pp->join_max_defl_deg, pp->merge_len, pp->cap_diameter, pp->kerb_r_minor,
                    pp->kerb_r_major, pp->fillet_segs, pp->tri_edge_max,
                    nspan, (unsigned)b.vert.n, ntri, nbed, (unsigned)b.edgec.n, (unsigned)b.nodec.n,
                    (unsigned)b.armc.n, (unsigned)b.signc.n, (unsigned)nnames, nx, nz, margin, (unsigned)nitem);
                if (sn < 0 || sn >= (int)sizeof stamp) sn = (int)sizeof stamp - 1;
                sz[0] = b.span.n * sizeof(TD5_StripSpan);  cnt[0] = b.span.n;  src[0] = b.span.p;
                sz[1] = b.vert.n * sizeof(TD5_StripVertex); cnt[1] = b.vert.n; src[1] = b.vert.p;
                sz[2] = b.tri.n * sizeof(RoamTri);          cnt[2] = b.tri.n;  src[2] = b.tri.p;
                sz[3] = b.bedge.n * sizeof(RoamBEdge);      cnt[3] = b.bedge.n; src[3] = b.bedge.p;
                sz[4] = b.edgec.n * sizeof(RoamEdgeRec);    cnt[4] = b.edgec.n; src[4] = b.edgec.p;
                sz[5] = b.nodec.n * sizeof(RoamNodeRec);    cnt[5] = b.nodec.n; src[5] = b.nodec.p;
                sz[6] = b.armc.n * sizeof(RoamArmRec);      cnt[6] = b.armc.n;  src[6] = b.armc.p;
                sz[7] = words * sizeof(uint32_t);           cnt[7] = nitem;     src[7] = gw;
                sz[8] = b.signc.n * sizeof(RoamSignRec);    cnt[8] = b.signc.n; src[8] = b.signc.p;
                sz[9] = b.names.n;                          cnt[9] = nnames;    src[9] = b.names.p;
                sz[10] = (size_t)sn + 1u;                   cnt[10] = 1;        src[10] = stamp;
                off = round16(sizeof(RoamFileHeader) + (size_t)ROAM_NSECT * sizeof(RoamSectEnt));
                for (i = 0; i < ROAM_NSECT; i++) {
                    se[i].tag = k_sect_tags[i];
                    se[i].offset = (uint32_t)off;
                    se[i].bytes = (uint32_t)sz[i];
                    se[i].count = (uint32_t)cnt[i];
                    off += round16(sz[i]);
                }
                total = off + 4u;
                img = (uint8_t *)calloc(total, 1);
                if (!img) { free(gw); goto done; }
                memset(&hdr, 0, sizeof hdr);
                memcpy(hdr.magic, "TD5ROAM1", 8);
                hdr.version = ROAM_FORMAT_VERSION;
                hdr.rules = ROAM_RULES_VERSION;
                hdr.input_hash = w->hash;
                hdr.tx = tx; hdr.tz = tz; hdr.org_x = org_x; hdr.org_z = org_z;
                hdr.nsect = ROAM_NSECT;
                memcpy(img, &hdr, sizeof hdr);
                memcpy(img + sizeof hdr, se, sizeof se);
                for (i = 0; i < ROAM_NSECT; i++)
                    if (sz[i]) memcpy(img + se[i].offset, src[i], sz[i]);
                trailer = fnv_add(FNV_INIT, img, total - 4u);
                memcpy(img + total - 4u, &trailer, 4u);
                free(gw);
                {
                    char err[160];
                    if (!td5_roam_tile_from_image(out, img, total, 0u, err, sizeof err)) {
                        TD5_LOG_E(LOG_TAG, "roam tile %d,%d: own image rejected: %s", tx, tz, err);
                        free(img);
                        goto done;
                    }
                }
                st->bytes = (uint32_t)total;
            }
        }
    }
    ok = 1;
    st->ms_assemble = td5_plat_time_ms() - t0;

done:
    free(tspan); free(owned); free(pspan); free(ptri); free(pbed); free(cut_first); free(cut_last);
    tb_free(&b);
    return ok;
}

/* ============================================================================
 * SECTION: dev CLI (--RoamBuildTiles=...), compiled out of RELEASE
 * ========================================================================== */

#ifndef TD5RE_RELEASE

static int cli_get(const char *cmd, const char *key, char *out, size_t cap)
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
    int tx, tz;
    TD5_RoamTileStats st;
    int ok, rb_ok;
    char err[640];
} TileRow;

typedef struct {
    TD5_RoamTileCtx *ctx;
    int (*list)[2];
    int n;
    int reverse;
    int apply_env;
    TD5_FpEnv env;
    char outdir[400];
    TileRow *rows;
    int done;
} CliJob;

static void cli_run(CliJob *j)
{
    int i;
    for (i = 0; i < j->n; i++) {
        const int k = j->reverse ? j->n - 1 - i : i;
        TileRow *row = &j->rows[k];
        RoamTile t;
        char path[512], err[160];
        row->tx = j->list[k][0];
        row->tz = j->list[k][1];
        row->ok = td5_roam_tile_build(j->ctx, row->tx, row->tz, &t, &row->st);
        if (!row->ok) { snprintf(row->err, sizeof row->err, "build failed"); continue; }
        td5_roam_tile_path(path, sizeof path, j->outdir[0] ? j->outdir : NULL,
                           td5_roam_tile_ctx_grid(j->ctx)->place, row->tx, row->tz);
        if (!td5_roam_tile_write(path, &t)) { row->ok = 0; snprintf(row->err, sizeof row->err, "write failed: %s", path); }
        else {
            /* read it back through the reader: header, table, trailer, hash */
            RoamTile rb;
            row->rb_ok = td5_roam_tile_read(path, &rb, t.stamp, err, sizeof err);
            if (row->rb_ok) {
                row->rb_ok = rb.nspan == t.nspan && rb.nvert == t.nvert && rb.ntri == t.ntri &&
                             rb.nbedge == t.nbedge && rb.nedge == t.nedge && rb.nnode == t.nnode &&
                             memcmp(rb.arena, t.arena, t.arena_bytes) == 0;
                if (!row->rb_ok) snprintf(row->err, sizeof row->err, "readback differs");
                td5_roam_tile_free(&rb);
            } else snprintf(row->err, sizeof row->err, "readback: %s", err);
        }
        td5_roam_tile_free(&t);
    }
    j->done = 1;
}

static void cli_thread(void *arg)
{
    CliJob *j = (CliJob *)arg;
    if (j->apply_env) td5_plat_fpenv_apply(&j->env);
    cli_run(j);
}

static int cmp_tilerow(const void *a, const void *b)
{
    const TileRow *x = (const TileRow *)a, *y = (const TileRow *)b;
    if (x->tx != y->tx) return x->tx < y->tx ? -1 : 1;
    return x->tz < y->tz ? -1 : (x->tz > y->tz ? 1 : 0);
}

int td5_roam_tile_dev_cli(const char *cmdline)
{
    char val[256], place[64] = "la_plata_partido", outdir[400] = "", cens[400] = "";
    int (*list)[2] = NULL;
    int n = 0, i, k, threadmode = 0, reverse = 0, global = 0;
    TD5_RoamTileCtx *ctx;
    CliJob job;
    TileRow *rows;
    FILE *f;
    uint32_t t0;
    const char *dir;
    double gwin[4];
    char lines[4096];
    long tot_quads = 0, tot_tris = 0, tot_bed = 0, tot_verts = 0, tot_bytes = 0, tot_ed = 0, tot_cl = 0;
    long tot_ok = 0, tot_fail = 0, tot_rb = 0, tot_pass2 = 0, tot_cut = 0, tot_cutbad = 0;
    long tot_ovf = 0, tot_i16 = 0, tot_reorg = 0, tot_nobed = 0, tot_clfail = 0, tot_nohc = 0, tot_clamped = 0, tot_spread = 0;
    long tot_ramp = 0, tot_pin = 0, tot_abs = 0, tot_bad = 0, tot_hfail = 0, tot_border = 0, tot_sig = 0, tot_arms = 0;
    double max_spread = 0.0, max_blend = 0.0, max_pdy = 0.0, max_over = 0.0;
    long tot_crib = 0, tot_crow = 0, tot_bore = 0, tot_gover = 0, tot_cyc = 0;
    double max_cdy = 0.0, max_grade = 0.0, min_clear = 0.0;
    uint32_t max_ms = 0, sum_ms = 0;
    long hug[16], hist_spread[5] = { 0, 0, 0, 0, 0 }, hist_pdy[5] = { 0, 0, 0, 0, 0 };

    if (!cli_get(cmdline, "RoamBuildTiles", val, sizeof val)) return 0;
    cli_get(cmdline, "RoamPlace", place, sizeof place);
    cli_get(cmdline, "RoamOut", outdir, sizeof outdir);
    cli_get(cmdline, "RoamCensus", cens, sizeof cens);
    if (cli_get(cmdline, "RoamOrder", lines, sizeof lines)) reverse = (_strnicmp(lines, "rev", 3) == 0);
    if (cli_get(cmdline, "RoamThread", lines, sizeof lines)) threadmode = atoi(lines);
    if (cli_get(cmdline, "RoamGlobal", lines, sizeof lines)) global = atoi(lines);

    /* the global window = the casco bbox */
    gwin[0] = gwin[2] = k_casco[0][0];
    gwin[1] = gwin[3] = k_casco[0][1];
    for (i = 1; i < 4; i++) {
        if (k_casco[i][0] < gwin[0]) gwin[0] = k_casco[i][0];
        if (k_casco[i][1] < gwin[1]) gwin[1] = k_casco[i][1];
        if (k_casco[i][0] > gwin[2]) gwin[2] = k_casco[i][0];
        if (k_casco[i][1] > gwin[3]) gwin[3] = k_casco[i][1];
    }
    {
        /* the global arm covers every requested tile: grow the window to them */
        const double m = 1500.0 * TD5_ROAM_UPM;       /* a tile box reaches ~360 m past the casco */
        gwin[0] -= m; gwin[1] -= m; gwin[2] += m; gwin[3] += m;
    }
    ctx = td5_roam_tile_ctx_open(place, 1, global ? gwin : NULL);
    if (!ctx) { TD5_LOG_E(LOG_TAG, "roam tiles: context did not open"); return 1; }

    if (_strnicmp(val, "casco", 5) == 0) {
        int cap = 2048;
        list = (int (*)[2])malloc((size_t)cap * sizeof(int[2]));
        n = list ? td5_roam_casco_tiles(td5_roam_tile_ctx_grid(ctx), list, cap) : 0;
    } else {
        int x0, z0, x1, z1, tx, tz;
        if (sscanf(val, "%d,%d,%d,%d", &x0, &z0, &x1, &z1) != 4) {
            TD5_LOG_E(LOG_TAG, "roam tiles: --RoamBuildTiles wants x0,z0,x1,z1 or casco, got '%s'", val);
            td5_roam_tile_ctx_close(ctx);
            return 1;
        }
        list = (int (*)[2])malloc((size_t)((x1 - x0 + 1) * (z1 - z0 + 1) + 1) * sizeof(int[2]));
        for (tx = x0; tx <= x1 && list; tx++)
            for (tz = z0; tz <= z1; tz++) { list[n][0] = tx; list[n][1] = tz; n++; }
    }
    if (!list || n <= 0) { TD5_LOG_E(LOG_TAG, "roam tiles: no tiles selected"); free(list); td5_roam_tile_ctx_close(ctx); return 1; }
    rows = (TileRow *)calloc((size_t)n, sizeof(TileRow));
    memset(&job, 0, sizeof job);
    job.ctx = ctx; job.list = list; job.n = n; job.reverse = reverse; job.rows = rows;
    snprintf(job.outdir, sizeof job.outdir, "%s", outdir);
    TD5_LOG_I(LOG_TAG, "roam tiles: %d tile(s), place %s, order %s, thread mode %d, %s, out %s", n, place,
              reverse ? "reverse" : "forward", threadmode, global ? "GLOBAL window" : "per-tile windows",
              outdir[0] ? outdir : "(default)");
    t0 = td5_plat_time_ms();
    if (threadmode > 0) {
        void *th;
        td5_plat_fpenv_capture(&job.env);
        job.apply_env = (threadmode == 1);
        th = td5_plat_thread_create(cli_thread, &job);
        if (!th) { TD5_LOG_E(LOG_TAG, "roam tiles: worker thread did not start"); return 1; }
        td5_plat_thread_join(th);
    } else {
        cli_run(&job);
    }
    t0 = td5_plat_time_ms() - t0;

    memset(hug, 0, sizeof hug);
    qsort(rows, (size_t)n, sizeof(TileRow), cmp_tilerow);
    dir = td5_plat_log_dir();
    if (!cens[0]) snprintf(cens, sizeof cens, "%sroam_tile_census.txt", dir ? dir : "");
    f = fopen(cens, "w");
    if (f) {
        fprintf(f, "roam tile census (F1 T5), place %s, %d tile(s), order %s, thread mode %d, %s\n", place, n,
                reverse ? "reverse" : "forward", threadmode, global ? "global window" : "per-tile windows");
        fprintf(f, "lattice: %s", ctx->grid_json);
        fprintf(f, "\ntx tz quads tris bedges verts edges clusters arms bytes passes cache_tiles ms overhang_m "
                   "spread_m blend_m pdy_m cut_chk cut_bad ok\n");
    }
    for (i = 0; i < n; i++) {
        const TileRow *r = &rows[i];
        const TD5_RoamTileStats *s = &r->st;
        if (!r->ok || !r->rb_ok) tot_fail++; else tot_ok++;
        if (r->rb_ok) tot_rb++;
        tot_quads += s->quads; tot_tris += s->tris; tot_bed += s->bedges; tot_verts += s->verts;
        tot_bytes += s->bytes; tot_ed += s->edges_rec; tot_cl += s->clusters; tot_arms += s->arms;
        if (s->passes > 1) tot_pass2++;
        tot_cut += s->cut_checked; tot_cutbad += s->cut_mismatch; tot_ovf += s->overflow;
        tot_i16 += s->i16_overflow; tot_reorg += s->i16_reorigin; tot_nobed += s->bedge_no_tri; tot_clfail += s->cluster_fail;
        tot_nohc += s->clusters_no_height; tot_clamped += s->arm_clamped; tot_spread += s->spread_clusters;
        tot_ramp += s->ramp_short + s->ramp_fail; tot_pin += s->pin_miss; tot_abs += s->ribbon_absorbed;
        tot_bad += s->ribbon_bad; tot_hfail += s->ribbon_height_fail; tot_border += s->border_quads;
        tot_sig += s->signals;
        tot_crib += s->couple_ribbons; tot_crow += s->couple_rows; tot_bore += s->bore_cross;
        tot_gover += s->grade_over_cap; tot_cyc += s->dep_cycles;
        if (s->couple_max_dy_m > max_cdy) max_cdy = s->couple_max_dy_m;
        if (s->max_grade > max_grade) max_grade = s->max_grade;
        if (s->bore_cross > 0 && (min_clear == 0.0 || s->bore_min_clear_m < min_clear)) min_clear = s->bore_min_clear_m;
        if (s->max_spread_m > max_spread) max_spread = s->max_spread_m;
        if (s->max_blend_m > max_blend) max_blend = s->max_blend_m;
        if (s->max_patch_dy_m > max_pdy) max_pdy = s->max_patch_dy_m;
        if (s->max_overhang_m > max_over) max_over = s->max_overhang_m;
        if (s->ms > max_ms) max_ms = s->ms;
        sum_ms += s->ms;
        for (k = 0; k < 16; k++) hug[k] += s->hug_phase[k];
        for (k = 0; k < 5; k++) { hist_spread[k] += s->spread_hist[k]; hist_pdy[k] += s->pdy_hist[k]; }
        if (f)
            fprintf(f, "%d %d %d %d %d %d %d %d %d %u %d %d %u %.1f %.2f %.2f %.2f %d %d %s%s%s\n", r->tx, r->tz,
                    s->quads, s->tris, s->bedges, s->verts, s->edges_rec, s->clusters, s->arms, s->bytes,
                    s->passes, s->cache_tiles, (unsigned)s->ms, s->max_overhang_m, s->max_spread_m,
                    s->max_blend_m, s->max_patch_dy_m, s->cut_checked, s->cut_mismatch,
                    (r->ok && r->rb_ok) ? "ok" : "FAIL", r->err[0] ? " " : "", r->err);
    }
    snprintf(lines, sizeof lines,
        "roam tiles TOTAL: %d tile(s) %ld ok %ld failed (%ld read back identical); %ld quads %ld tris %ld verts "
        "%ld boundary edges %ld edges with owned quads %ld clusters %ld arms %ld signals; %ld bytes; second window pass on %ld "
        "tile(s); wall %u ms, per tile mean %u max %u ms",
        n, tot_ok, tot_fail, tot_rb, tot_quads, tot_tris, tot_verts, tot_bed, tot_ed, tot_cl, tot_arms, tot_sig,
        tot_bytes, tot_pass2, (unsigned)t0, (unsigned)(n ? sum_ms / (uint32_t)n : 0u), (unsigned)max_ms);
    TD5_LOG_I(LOG_TAG, "%s", lines);
    if (f) {
        fprintf(f, "\n%s\n", lines);
        fprintf(f, "max cluster ground spread %.2f m; clusters over %.1f m: %ld; arm pins clamped: %ld; "
                   "max |H_c (plane on a sloped cluster) - ground at an arm cut row| %.2f m; max |H_c at the arm - arm cut-row y| %.2f m\n",
                max_spread, ROAM_SPREAD_CLAMP_M, tot_spread, tot_clamped, max_blend, max_pdy);
        fprintf(f, "owned clusters by ground spread  (<=0.5 / <=1 / <=2 / <=4 / >4 m): %ld / %ld / %ld / %ld / %ld\n",
                hist_spread[0], hist_spread[1], hist_spread[2], hist_spread[3], hist_spread[4]);
        fprintf(f, "owned clusters by patch crest    (<=0.5 / <=1 / <=2 / <=4 / >4 m): %ld / %ld / %ld / %ld / %ld\n",
                hist_pdy[0], hist_pdy[1], hist_pdy[2], hist_pdy[3], hist_pdy[4]);
        fprintf(f, "patch cut vertices checked against their ribbon row: %ld, mismatched: %ld\n", tot_cut, tot_cutbad);
        fprintf(f, "ribbons: absorbed %ld bad %ld height-fail %ld; RAMP_SHORT/FAIL %ld; pin-miss %ld; "
                   "cluster FAIL (no triangles) %ld; clusters without a ground sample %ld\n",
                tot_abs, tot_bad, tot_hfail, tot_ramp, tot_pin, tot_clfail, tot_nohc);
        fprintf(f, "limits: overflow %ld, int16 offset overflow %ld, outline segment in no triangle %ld\n",
                tot_ovf, tot_i16, tot_nobed);
        fprintf(f, "[F1-T10] height coupling: %ld owned ribbon piece(s) follow a leader on %ld row(s), largest change %.2f m; "
                   "steepest owned quad %.4f (cap %.3f), quads over the cap %ld; bore crossings %ld, least clearance %.2f m "
                   "(want %.2f); build-order cycles broken %ld\n",
                tot_crib, tot_crow, max_cdy, max_grade, ROAM_GRADE_CAP, tot_gover, tot_bore, min_clear,
                ROAM_XSEP_LIFT / TD5_ROAM_UPM, tot_cyc);
        fprintf(f, "patch vertex origin moved off the centroid to the bbox centre (int16 reach, F1-T5b): %ld cluster(s)\n", tot_reorg);
        fprintf(f, "owned quads with a vertex outside their tile: %ld; max overhang %.2f m (GRID margin is sized from it)\n",
                tot_border, max_over);
        fprintf(f, "owned quad midpoints within 2 m of a tile border, per lattice phase (du,dv in S/4 steps):\n");
        for (k = 0; k < 16; k++)
            fprintf(f, "  phase %d,%d: %ld (%.1f%%)%s\n", k >> 2, k & 3, hug[k],
                    tot_quads ? 100.0 * (double)hug[k] / (double)tot_quads : 0.0, k == 0 ? "  <- lattice as written" : "");
        fclose(f);
    }
    for (i = 0; i < n; i++) if (rows[i].err[0]) TD5_LOG_E(LOG_TAG, "roam tile %d,%d: %s", rows[i].tx, rows[i].tz, rows[i].err);
    free(rows);
    free(list);
    td5_roam_tile_ctx_close(ctx);
    return 1;
}

#endif /* !TD5RE_RELEASE */
