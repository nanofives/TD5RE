/**
 * td5_roam_furniture.c -- GEO FREE ROAM: street FURNITURE of one tile (PORT-ONLY,
 * roam F4c). Contract and rules: td5_roam_furniture.h. Design: GEO_FREE_ROAM_DESIGN.md 4.
 *
 * SECTIONS
 *   atlas        street-name plate cells from every signalised cluster of the casco
 *                (main thread, once), the GPU pages
 *   source       SIGNALS_<ix>_<iz>.JSON nodes (crossings, bus stops), per thread
 *   writers      the strip's emitters on a roam frame: mast arm, post light, lamp, bus
 *                stop, zebra / limit lines; the furniture census over the bytes
 *   signals      the layout rule per signalised cluster of the 3x3
 *   tile         lamps along the owned quads, bus stops, crossings, the whole tile
 *   frame        the signal clock and the street-lamp registry feed (main, per frame)
 *
 * FRAME CONVENTION (the strip's, so every number measured there carries over): a point
 * on the road centre, the HEADING of travel (tx, tz), side = +1 / -1 with the outward
 * lateral l = (tz * side, -tx * side). side = -1 is the driver's RIGHT kerb, side = +1 the
 * LEFT (verified on the strip: Argentina drives on the right, the near arm stands at
 * side = -1). [roam M] Until the geographic -> game reflection (td5_geo_frame.h) that was
 * the right AS DRAWN and the real left; the place frame is now the game frame (x = -east),
 * so side = -1 is the real right kerb too. All x / z here are TILE-LOCAL (place minus the
 * tile origin), y is place y.
 *
 * Every per-record decision keys off the record itself (edge key, quad row, node
 * position), never off the order tiles are built in; a cluster is evaluated by every tile
 * whose footprint it can reach and each tile emits only what stands in IT (anchor-owned).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_font.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_geo_signals.h"
#include "td5_trackgen.h"
#include "td5_trackgen_internal.h"
#include "td5_page_map.h"
#include "td5_asset.h"
#include "td5_race_state.h"      /* read-only race queries (td5_game_get_actor) */
#include "td5_light.h"
#include "td5_rt.h"
#include "td5_types.h"
#include "../../../re/include/td5_actor_struct.h" /* full TD5_Actor (world_pos) */
#include "deps/cjson/cJSON.h"
#include "td5_geo_frame.h"      /* [roam M] the geographic -> game reflection */
#include "td5_roam_tile.h"
#include "td5_roam_scenery.h"
#include "td5_roam_render.h"
#include "td5_roam_mode.h"
#include "td5_roam_furniture.h"
#include "td5_roam_traffic.h"   /* [F8] td5_roam_signal_group: the head an approach obeys */

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "roam"

#define FU_UPM          430.0
#define FU_M(m)         ((m) * TD5_TG_INFRA_M)        /* the generator's own metre (411 units)   */
#define FU_KERB_H       65.0                          /* kerb top over the road, 0.15 m          */
#define FU_OVERHEAD     (4.4 * FU_UPM)                /* nothing hangs lower than this over a road */

/* ---- signals: the strip's numbers (td5_tg_furniture.c / td5_tg_mastarm.c) ---- */
#define FU_SIG_GAP       (TD5_TG_R11_SIGN_GAP + 0.5 * FU_M(0.34))   /* pole centre past the carriageway edge */
#define FU_SIG_HEAD_W    FU_M(0.34)
#define FU_SIG_HEAD_H    FU_M(0.95)
#define FU_SIG_HEAD_D    FU_M(0.30)
#define FU_SIG_POLE_H    FU_M(3.00)
#define FU_SIG_POLE_W    FU_M(0.11)
#define FU_SIG_HEAD_OV   FU_M(0.06)
#define FU_SIG_LAMP_R    FU_M(0.13)
#define FU_SIG_LAMP_DY   FU_M(0.29)
#define FU_MA_POLE_TOP   FU_M(6.30)
#define FU_MA_POLE_HW    FU_M(0.10)
#define FU_MA_ARM_Y      FU_M(5.80)
#define FU_MA_ARM_HT     FU_M(0.07)
#define FU_MA_PLATE_W    FU_M(1.40)
#define FU_MA_PLATE_ROW  FU_M(0.35)
#define FU_MA_PLATE_OFF  FU_M(0.03)
#define FU_MA_HEAD_H     FU_M(0.95)
#define FU_MA_HEAD_W     FU_M(0.34)
#define FU_MA_HEAD_D     FU_M(0.30)
#define FU_MA_LAMP_R     FU_M(0.13)
#define FU_MA_LAMP_DY    FU_M(0.29)
#define FU_MA_BP_HW      FU_M(0.30)
#define FU_MA_BP_HH      FU_M(0.62)
#define FU_MA_BP_HD      FU_M(0.02)
#define FU_MA_MIN_WIDTH  (5.5 * FU_UPM)
/* where along an approach arm the pole stands: past the arm's cut row by this, walking out in steps */
#define FU_POLE_FIRST_M  2.5
#define FU_POLE_STEP_M   3.0
#define FU_POLE_STEPS    6
#define FU_POLE_SEP_M    2.0                          /* two poles of one cluster this close are one */
#define FU_ARM_PROBE_MAX_M 48.0
/* ---- lamps ---- */
#define FU_LAMP_H        2500.0                       /* TD5_TG_LAMP_H, the lantern height        */
#define FU_LAMP_REACH    468.75                       /* the strip's arm reach (W * (1 - POST_U)) */
/* ---- bus stops (td5_tg_geo_street.c) ---- */
#define FU_BS_POLE_H     FU_M(2.45)
#define FU_BS_POLE_W     FU_M(0.09)
#define FU_BS_FLAG_H     FU_M(0.34)
#define FU_BS_FLAG_W     FU_M(0.40)
#define FU_BS_FLAG_D     FU_M(0.04)
#define FU_BS_SHEL_W     FU_M(3.20)
#define FU_BS_SHEL_D     FU_M(1.30)
#define FU_BS_SHEL_H     FU_M(2.35)
#define FU_BS_ROOF_T     FU_M(0.10)
#define FU_BS_BACK_T     FU_M(0.07)
#define FU_BS_BENCH_W    FU_M(1.55)
#define FU_BS_BENCH_H    FU_M(0.46)
#define FU_BS_BENCH_D    FU_M(0.40)
#define FU_BS_BIN_W      FU_M(0.34)
#define FU_BS_BIN_H      FU_M(0.88)
#define FU_BS_NEAR_M     12.0                         /* a stop is on the kerb within this of a ribbon */
/* ---- crossings ---- */
#define FU_X_HALF        300.0                        /* half length of the painted band          */
#define FU_X_LIFT        20.0                         /* TD5_TG_CROSS_LIFT                        */
#define FU_X_NEAR_M      3.0                          /* a crossing node is on a ribbon within hw + this */
/* ---- atlas ---- */
#define FU_PL_W          64
#define FU_PL_ROW_H      16
#define FU_PL_ROWS       (TD5_TG_TEX_DIM / FU_PL_ROW_H)
#define FU_PL_PAD        3
#define FU_NAME_MAX      96
#define FU_CELLS_MAX     600
#define FU_PAGE_BASE     (TD5_TG_PAGE_COUNT + TD5_TG_MASTARM_PAGES_MAX)
#define FU_PAGES_MAX     (TD5_PAGE_LEVEL_CEIL - FU_PAGE_BASE - 1)   /* one slot is the digit display */
#define FU_SIGN_CACHE    12

/* ============================================================================
 * SECTION: small helpers
 * ========================================================================== */

static uint32_t fu_fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

int td5_roam_furn_enabled(void) { return td5_env_int("TD5RE_ROAM_FURNITURE", 1, 0, 1); }

int td5_roam_furn_night(void)
{
    const int k = td5_env_int("TD5RE_ROAM_NIGHT", -1, -1, 1);
    if (k >= 0) return k;
    return td5_trackgen_roam_is_night(td5_roam_host_track());   /* [F4f] the sky's verdict: no env knob needed */
}

int td5_roam_night_active(void) { return td5_roam_active() && td5_roam_furn_night(); }

static char *fu_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)64 * 1024 * 1024) { td5_plat_file_close(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { td5_plat_file_close(f); return NULL; }
    if (td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) { free(buf); td5_plat_file_close(f); return NULL; }
    buf[n] = '\0';
    td5_plat_file_close(f);
    return buf;
}

static int fu_grow(void **p, int *cap, int need, size_t elem)
{
    void *n;
    int nc;
    if (need <= *cap) return 1;
    nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    n = realloc(*p, (size_t)nc * elem);
    if (!n) return 0;
    *p = n; *cap = nc;
    return 1;
}

/* the i-th NUL-separated name of a tile's NAME section, NULL when out of range / unnamed */
static const char *fu_tile_name(const RoamTile *t, unsigned id)
{
    const char *p = t->names;
    uint32_t left = t->names_bytes;
    unsigned k;
    if (id == 0xFFFFu || !p || id >= t->nnames) return NULL;
    for (k = 0; k < id; k++) {
        const size_t l = strlen(p);
        if (l + 1 > left) return NULL;
        p += l + 1; left -= (uint32_t)(l + 1);
    }
    return (left > 0 && p[0]) ? p : NULL;
}

/* the arm bearing (12 bit, atan2(dz, dx)) as a unit vector */
static void fu_bearing(int b12, double *ux, double *uz)
{
    const double a = (double)b12 * 6.283185307179586 / 4096.0;
    *ux = cos(a); *uz = sin(a);
}

/* ============================================================================
 * SECTION: plate atlas
 * ========================================================================== */

typedef struct {
    char name[FU_NAME_MAX];
    int  page, y, h, lines, forced, uses;
    float cap;
} FuCell;

static struct {
    int           ready, uploaded;
    char          place[64];
    FuCell        cell[FU_CELLS_MAX];
    int           ncell;
    unsigned char pg[FU_PAGES_MAX][TD5_TG_TEX_TEXELS];
    int           pg_rows[FU_PAGES_MAX];
    int           npg;
    unsigned char dsp[TD5_TG_TEX_TEXELS];
    int           names, cells_wanted, cells_dropped, uses_dropped, clusters, arms, ms;
} s_at;

int td5_roam_furn_plates_ready(void) { return s_at.ready && s_at.ncell > 0; }

/* the 5x7 dot-matrix digits of td5_tg_mastarm.c (the countdown page) */
static const unsigned char k_fu_dot[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }
};

static void fu_build_display(void)
{
    int d, r, c;
    memset(s_at.dsp, 0, sizeof s_at.dsp);
    for (d = 0; d < 10; d++) {
        const int ox = (d % TD5_GEO_DIGIT_COLS) * TD5_GEO_DIGIT_CELL_W;
        const int oy = (d / TD5_GEO_DIGIT_COLS) * TD5_GEO_DIGIT_CELL_H;
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++) {
                int dx, dy;
                if (!((k_fu_dot[d][r] >> (4 - c)) & 1)) continue;
                for (dy = 0; dy < 2; dy++)
                    for (dx = 0; dx < 2; dx++)
                        s_at.dsp[(oy + r * 3 + dy) * TD5_TG_TEX_DIM + (ox + 1 + c * 3 + dx)] = 15;
            }
    }
}

static int fu_alloc_rows(int rows, int *row0)
{
    int p;
    for (p = 0; p < s_at.npg; p++)
        if (s_at.pg_rows[p] + rows <= FU_PL_ROWS) { *row0 = s_at.pg_rows[p]; s_at.pg_rows[p] += rows; return p; }
    if (s_at.npg >= FU_PAGES_MAX) return -1;
    p = s_at.npg++;
    memset(s_at.pg[p], 0, sizeof s_at.pg[p]);
    s_at.pg_rows[p] = rows;
    *row0 = 0;
    return p;
}

/* a wanted cell: (name, forced) with its use count */
static int fu_want(FuCell *w, int *nw, const char *name, int forced)
{
    int i;
    if (!name || !name[0]) return 1;
    for (i = 0; i < *nw; i++)
        if (w[i].forced == forced && strcmp(w[i].name, name) == 0) { w[i].uses++; return 1; }
    if (*nw >= FU_CELLS_MAX) return 0;
    memset(&w[*nw], 0, sizeof w[*nw]);
    snprintf(w[*nw].name, sizeof w[*nw].name, "%s", name);
    w[*nw].forced = forced; w[*nw].uses = 1; w[*nw].page = -1;
    (*nw)++;
    return 1;
}

static int fu_cell_cmp(const void *pa, const void *pb)
{
    const FuCell *a = (const FuCell *)pa, *b = (const FuCell *)pb;
    if (a->uses != b->uses) return b->uses - a->uses;
    if (a->forced != b->forced) return a->forced - b->forced;
    return strcmp(a->name, b->name);
}

/* one cell, rasterised exactly like td5_tg_mastarm.c pl_cell_for */
static int fu_raster_cell(FuCell *c)
{
    char up[FU_NAME_MAX], l1[FU_NAME_MAX], l2[FU_NAME_MAX];
    unsigned char cov[FU_PL_W * FU_PL_ROW_H * 2];
    int lines, abbr = 0, rows, h, pg, row0 = 0, x, y;
    float cap = 0.0f;
    tg_plate_upper(c->name, up, sizeof up);
    l2[0] = 0;
    lines = tg_plate_plan(up, l1, l2, sizeof l1, &abbr);
    if (c->forced && lines == 1) {
        char one[FU_NAME_MAX];
        snprintf(one, sizeof one, "%s", l1);
        if (tg_plate_split2(one, l1, l2, sizeof l1)) lines = 2;
    }
    rows = c->forced ? 2 : lines;
    h = rows * FU_PL_ROW_H;
    if (!td5_font_plate_raster(l1, lines == 2 ? l2 : NULL, FU_PL_W, h, FU_PL_PAD, cov, &cap)) return 0;
    pg = fu_alloc_rows(rows, &row0);
    if (pg < 0) return -1;
    for (y = 0; y < h; y++)
        for (x = 0; x < FU_PL_W; x++) {
            int v = (cov[y * FU_PL_W + x] * 15 + 127) / 255;
            const int ring = (x == 1 || x == FU_PL_W - 2 || y == 1 || y == h - 2) &&
                             x >= 1 && x <= FU_PL_W - 2 && y >= 1 && y <= h - 2;
            if (ring) v = 15;
            s_at.pg[pg][(row0 * FU_PL_ROW_H + y) * TD5_TG_TEX_DIM + x] = (unsigned char)v;
        }
    c->page = pg; c->y = row0 * FU_PL_ROW_H; c->h = h; c->lines = lines; c->cap = cap;
    return 1;
}

/* ---- the casco scan: every signalised cluster, its approach arms' names ---- */

typedef struct { uint64_t key; int name; } FuEdgeName;
typedef struct { int first, n; } FuScanCluster;
typedef struct { uint64_t key; int16_t bearing; } FuScanArm;

static int fu_edge_name_cmp(const void *pa, const void *pb)
{
    const FuEdgeName *a = (const FuEdgeName *)pa, *b = (const FuEdgeName *)pb;
    return a->key < b->key ? -1 : (a->key > b->key ? 1 : 0);
}

/* the cross street of arm `a`: the other-named arm closest to perpendicular, -1 when none.
 * Shared by the atlas scan and the emitter, so the cells the atlas holds are the cells asked for. */
static int fu_pick_cross(const int *nm, const int16_t *bear, int n, int a)
{
    int j, best = -1;
    double bs = -1.0;
    /* [F4f] an UNNAMED arm still gets a plate: it names the street it crosses (the best-named other arm) */
    for (j = 0; j < n; j++) {
        double s;
        if (j == a || nm[j] < 0 || nm[j] == nm[a]) continue;
        s = fabs(sin(((double)bear[j] - (double)bear[a]) * 6.283185307179586 / 4096.0));
        if (s > bs + 1e-9) { bs = s; best = j; }
    }
    return best;
}

int td5_roam_furn_atlas_prepare(const char *place, const RoamGrid *g)
{
    int (*list)[2] = NULL;
    int nt, i, k, nnames = 0, capn = 0, nen = 0, cape = 0, ncl = 0, capc = 0, narm = 0, capa = 0;
    char (*names)[FU_NAME_MAX] = NULL;
    FuEdgeName *en = NULL;
    FuScanCluster *cl = NULL;
    FuScanArm *arms = NULL;
    FuCell *want = NULL;
    int nw = 0, ok = 0;
    uint32_t t0 = td5_plat_time_ms();
    if (!place || !g) return 0;
    if (s_at.ready && strcmp(s_at.place, place) == 0) return td5_roam_furn_plates_ready();
    memset(&s_at, 0, sizeof s_at);
    snprintf(s_at.place, sizeof s_at.place, "%s", place);
    s_at.ready = 1;                              /* a failure below leaves an EMPTY atlas, never half of one */
    if (!td5_roam_furn_enabled()) return 0;
    list = (int (*)[2])malloc(2048 * sizeof(int[2]));
    want = (FuCell *)calloc(FU_CELLS_MAX, sizeof(FuCell));
    if (!list || !want) goto done;
    nt = td5_roam_casco_tiles(g, list, 2048);
    for (i = 0; i < nt; i++) {
        RoamTile t;
        char path[512], err[160];
        uint32_t e, nd, a;
        int *remap;
        memset(&t, 0, sizeof t);
        td5_roam_tile_path(path, sizeof path, NULL, place, list[i][0], list[i][1]);
        if (!td5_roam_tile_read(path, &t, 0, err, sizeof err)) continue;
        remap = (int *)malloc((size_t)(t.nnames ? t.nnames : 1u) * sizeof(int));
        if (!remap) { td5_roam_tile_free(&t); goto done; }
        for (k = 0; k < (int)t.nnames; k++) {
            const char *nm = fu_tile_name(&t, (unsigned)k);
            int j;
            remap[k] = -1;
            if (!nm) continue;
            for (j = 0; j < nnames; j++) if (strcmp(names[j], nm) == 0) break;
            if (j == nnames) {
                if (!fu_grow((void **)&names, &capn, nnames + 1, sizeof *names)) { free(remap); td5_roam_tile_free(&t); goto done; }
                snprintf(names[nnames], FU_NAME_MAX, "%s", nm);
                nnames++;
            }
            remap[k] = j;
        }
        for (e = 0; e < t.nedge; e++) {
            if (!fu_grow((void **)&en, &cape, nen + 1, sizeof *en)) { free(remap); td5_roam_tile_free(&t); goto done; }
            en[nen].key = t.edge[e].key;
            en[nen].name = (t.edge[e].name_id != 0xFFFFu && t.edge[e].name_id < t.nnames) ? remap[t.edge[e].name_id] : -1;
            nen++;
        }
        for (nd = 0; nd < t.nnode; nd++) {
            const RoamNodeRec *nr = &t.node[nd];
            int signalised = 0;
            uint32_t s;
            for (s = 0; s < t.nsign; s++) if (t.sign[s].cluster_key == nr->cluster_key) { signalised = 1; break; }
            if (!signalised) continue;
            if (!fu_grow((void **)&cl, &capc, ncl + 1, sizeof *cl)) { free(remap); td5_roam_tile_free(&t); goto done; }
            cl[ncl].first = narm; cl[ncl].n = 0;
            for (a = 0; a < nr->narm; a++) {
                const RoamArmRec *ar = &t.arm[nr->arm_first + a];
                if (!fu_grow((void **)&arms, &capa, narm + 1, sizeof *arms)) { free(remap); td5_roam_tile_free(&t); goto done; }
                arms[narm].key = ar->edge_key; arms[narm].bearing = ar->bearing12;
                narm++; cl[ncl].n++;
            }
            ncl++;
        }
        free(remap);
        td5_roam_tile_free(&t);
    }
    qsort(en, (size_t)nen, sizeof *en, fu_edge_name_cmp);
    for (i = 0; i < ncl; i++) {
        int nm[64]; int16_t bear[64];
        int n = cl[i].n < 64 ? cl[i].n : 64, a;
        for (a = 0; a < n; a++) {
            FuEdgeName key, *hit;
            key.key = arms[cl[i].first + a].key; key.name = -1;
            hit = (FuEdgeName *)bsearch(&key, en, (size_t)nen, sizeof *en, fu_edge_name_cmp);
            nm[a] = hit ? hit->name : -1;
            bear[a] = arms[cl[i].first + a].bearing;
        }
        for (a = 0; a < n; a++) {
            const int x = fu_pick_cross(nm, bear, n, a);
            const char *own = nm[a] >= 0 ? names[nm[a]] : NULL;
            const char *crs = x >= 0 ? names[nm[x]] : NULL;
            const char *fn = crs ? crs : own, *bn = own ? own : crs;
            if (!fn && !bn) continue;
            /* ONE cell per distinct name, at its natural row count: a face is as tall as its own cell (F4f) */
            if (!fu_want(want, &nw, fn, 0)) s_at.cells_dropped++;
            if (!fu_want(want, &nw, bn, 0)) s_at.cells_dropped++;
        }
        s_at.arms += n;
    }
    s_at.clusters = ncl;
    s_at.names = nnames;
    s_at.cells_wanted = nw;
    qsort(want, (size_t)nw, sizeof *want, fu_cell_cmp);
    for (i = 0; i < nw; i++) {
        int r;
        if (s_at.ncell >= FU_CELLS_MAX) { s_at.cells_dropped++; s_at.uses_dropped += want[i].uses; continue; }
        r = fu_raster_cell(&want[i]);
        if (r <= 0) { s_at.cells_dropped++; s_at.uses_dropped += want[i].uses; continue; }
        s_at.cell[s_at.ncell++] = want[i];
    }
    if (s_at.ncell > 0) fu_build_display();
    ok = s_at.ncell > 0;
done:
    s_at.ms = (int)(td5_plat_time_ms() - t0);
    TD5_LOG_I(LOG_TAG, "roam furniture: plate atlas %s | %d signalised cluster(s), %d arm(s), %d distinct street name(s) | "
              "%d cell(s) wanted, %d placed on %d page(s) (page ids %d..%d, display %d), %d dropped (%d use(s) fall back to the "
              "other face) | %d ms", ok ? "built" : "EMPTY", s_at.clusters, s_at.arms, s_at.names, s_at.cells_wanted,
              s_at.ncell, s_at.npg, FU_PAGE_BASE, FU_PAGE_BASE + (s_at.npg ? s_at.npg - 1 : 0), FU_PAGE_BASE + s_at.npg,
              s_at.cells_dropped, s_at.uses_dropped, s_at.ms);
    free(list); free(names); free(en); free(cl); free(arms); free(want);
    return ok;
}

static void fu_plate_bgra(int page, uint8_t *out)
{
    static const int k_bg[3] = { 150, 56, 22 };       /* B, G, R: the plate blue */
    int i;
    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int idx = (page < s_at.npg) ? s_at.pg[page][i] : s_at.dsp[i];
        if (page < s_at.npg) {
            out[i * 4 + 0] = (uint8_t)(k_bg[0] + (255 - k_bg[0]) * idx / 15);
            out[i * 4 + 1] = (uint8_t)(k_bg[1] + (255 - k_bg[1]) * idx / 15);
            out[i * 4 + 2] = (uint8_t)(k_bg[2] + (255 - k_bg[2]) * idx / 15);
        } else {
            out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = (uint8_t)(idx * 17);
        }
        out[i * 4 + 3] = 0xFF;
    }
}

int td5_roam_furn_session_begin(const char *place, const RoamGrid *g)
{
    int p, np;
    uint8_t *px;
    if (!td5_roam_furn_atlas_prepare(place, g) && s_at.ncell <= 0) return 0;
    px = (uint8_t *)malloc((size_t)TD5_TG_TEX_TEXELS * 4u);
    if (!px) return 0;
    np = s_at.npg + 1;                                /* the plates, then the digit display */
    for (p = 0; p < np; p++) {
        fu_plate_bgra(p, px);
        td5_asset_set_page_transparency(FU_PAGE_BASE + p, 0);
        td5_plat_render_upload_texture(FU_PAGE_BASE + p, px, TD5_TG_TEX_DIM, TD5_TG_TEX_DIM, 2);
    }
    free(px);
    s_at.uploaded = 1;
    TD5_LOG_I(LOG_TAG, "roam furniture: %d plate page(s) + the digit display uploaded at page ids %d..%d", s_at.npg,
              FU_PAGE_BASE, FU_PAGE_BASE + s_at.npg);
    return 1;
}

void td5_roam_furn_session_end(void) { s_at.uploaded = 0; }

static const FuCell *fu_cell_find(const char *name, int forced)
{
    int i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < s_at.ncell; i++)
        if (s_at.cell[i].forced == forced && strcmp(s_at.cell[i].name, name) == 0) return &s_at.cell[i];
    return NULL;
}

/* ============================================================================
 * SECTION: source (SIGNALS tiles: crossings and bus stops)
 * ========================================================================== */

/* paint: 1 marked, 2 signals, 0 unmarked (never painted), 3 unknown (the [roam LC] rule decides at emit time).
 * way: 1 = from a footway=crossing way (its midpoint), 0 = a highway=crossing node. */
typedef struct { double x, z; int paint, way; } FuXing;
typedef struct { double x, z; int shelter, bench, bin; } FuStop;
typedef struct { int ix, iz; double x0, z0, x1, z1; } FuRow;

typedef struct {
    int used, ix, iz, layer;                 /* [roam LC] layer 0 = SIGNALS, 1 = FOOTWAYS */
    uint32_t clock;
    FuXing *xs; int nx;
    FuStop *bs; int nb;
} FuSigTile;

struct TD5_RoamFurnSource {
    char       place[64];
    FuRow     *rows, *frows;                /* [roam LC] frows: the FOOTWAYS layer's tile rows */
    int        nrows, nfrows;
    int        xing_mode;                   /* [roam LC] TD5RE_ROAM_XING_UNKNOWN: 0 skip, 1 rule, 2 all zebra */
    int        xing_ways;                   /* [roam LC] TD5RE_ROAM_XING_WAYS */
    FuSigTile  tile[FU_SIGN_CACHE];
    uint32_t   clock;
    int        night;
    double     lamp_sw;                  /* pavement width, the lamp / pole stand-off (units) */
    int        on_signals, on_lamps, on_stops, on_xings, on_unknown_xings;
    int        on_barriers, on_piers;      /* [roam IE] TD5RE_ROAM_FURN_BARRIERS / _PIERS (default 1) */
};

static int fu_json_true(const cJSON *e, const char *key)
{
    const cJSON *v = e ? cJSON_GetObjectItem(e, key) : NULL;
    if (!v) return 0;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0 ? 1 : 0;
    if (cJSON_IsString(v)) return (v->valuestring[0] && strcmp(v->valuestring, "no") && strcmp(v->valuestring, "false") &&
                                   strcmp(v->valuestring, "0")) ? 1 : 0;
    return 0;
}

static void fu_sigtile_free(FuSigTile *t)
{
    free(t->xs); free(t->bs);
    memset(t, 0, sizeof *t);
}

static int fu_xing_cmp(const void *pa, const void *pb)
{
    const FuXing *a = (const FuXing *)pa, *b = (const FuXing *)pb;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    return a->z < b->z ? -1 : (a->z > b->z ? 1 : 0);
}
static int fu_stop_cmp(const void *pa, const void *pb)
{
    const FuStop *a = (const FuStop *)pa, *b = (const FuStop *)pb;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    return a->z < b->z ? -1 : (a->z > b->z ? 1 : 0);
}

static FuSigTile *fu_sigtile(TD5_RoamFurnSource *f, int layer, int ix, int iz)
{
    int i, victim = 0;
    char rel[96], path[512];
    char *json;
    cJSON *root;
    const cJSON *nd, *e;
    FuSigTile *t;
    int ncx = 0, ncb = 0;
    for (i = 0; i < FU_SIGN_CACHE; i++)
        if (f->tile[i].used && f->tile[i].layer == layer && f->tile[i].ix == ix && f->tile[i].iz == iz) { f->tile[i].clock = ++f->clock; return &f->tile[i]; }
    for (i = 0; i < FU_SIGN_CACHE; i++) {
        if (!f->tile[i].used) { victim = i; break; }
        if (f->tile[i].clock < f->tile[victim].clock) victim = i;
    }
    t = &f->tile[victim];
    fu_sigtile_free(t);
    t->used = 1; t->ix = ix; t->iz = iz; t->layer = layer; t->clock = ++f->clock;
    snprintf(rel, sizeof rel, "tiles/%s_%d_%d.JSON", layer ? "FOOTWAYS" : "SIGNALS", ix, iz);
    td5_geo_source_path(path, sizeof path, f->place, rel);
    json = fu_slurp(path);
    if (!json) return t;                                    /* cached as empty */
    root = cJSON_Parse(json);
    free(json);
    td5_geo_src_json_to_game(root);                         /* [roam M] source frame -> game frame */
    if (layer) {                            /* [roam LC] footway=crossing ways: one crossing at the midpoint */
        const cJSON *fw = root ? cJSON_GetObjectItem(root, "footways") : NULL;
        if (fw && cJSON_IsArray(fw)) {
            const int n = cJSON_GetArraySize(fw);
            t->xs = (FuXing *)calloc((size_t)(n > 0 ? n : 1), sizeof(FuXing));
            if (t->xs) {
                for (e = fw->child; e; e = e->next) {
                    const cJSON *pk = cJSON_GetObjectItem(e, "kind"), *pts = cJSON_GetObjectItem(e, "points"), *pp = cJSON_GetObjectItem(e, "paint");
                    const cJSON *a, *b;
                    const char *s;
                    if (!pk || !cJSON_IsString(pk) || strcmp(pk->valuestring, "crossing") || !pts || !cJSON_IsArray(pts)) continue;
                    a = pts->child;
                    if (!a) continue;
                    for (b = a; b->next; b = b->next) {}
                    if (b == a) continue;
                    {
                        const cJSON *ax = cJSON_GetObjectItem(a, "x"), *az = cJSON_GetObjectItem(a, "z");
                        const cJSON *bx = cJSON_GetObjectItem(b, "x"), *bz = cJSON_GetObjectItem(b, "z");
                        if (!ax || !az || !bx || !bz) continue;
                        s = (pp && cJSON_IsString(pp)) ? pp->valuestring : "";
                        t->xs[ncx].x = 0.5 * (ax->valuedouble + bx->valuedouble);
                        t->xs[ncx].z = 0.5 * (az->valuedouble + bz->valuedouble);
                        t->xs[ncx].paint = !strcmp(s, "marked") ? 1 : (!strcmp(s, "signals") ? 2 : (!strcmp(s, "unmarked") ? 0 : 3));
                        t->xs[ncx].way = 1;
                        ncx++;
                    }
                }
            }
            t->nx = ncx;
        }
        cJSON_Delete(root);
        return t;
    }
    nd = root ? cJSON_GetObjectItem(root, "nodes") : NULL;
    if (nd && cJSON_IsArray(nd)) {
        const int n = cJSON_GetArraySize(nd);
        t->xs = (FuXing *)calloc((size_t)(n > 0 ? n : 1), sizeof(FuXing));
        t->bs = (FuStop *)calloc((size_t)(n > 0 ? n : 1), sizeof(FuStop));
        if (t->xs && t->bs) {
            for (e = nd->child; e; e = e->next) {
                const cJSON *pk = cJSON_GetObjectItem(e, "kind"), *px = cJSON_GetObjectItem(e, "x"), *pz = cJSON_GetObjectItem(e, "z");
                const char *kind;
                if (!pk || !cJSON_IsString(pk) || !px || !pz || !cJSON_IsNumber(px) || !cJSON_IsNumber(pz)) continue;
                kind = pk->valuestring;
                if (!strcmp(kind, "crossing")) {
                    const cJSON *pp = cJSON_GetObjectItem(e, "paint");
                    const char *s = (pp && cJSON_IsString(pp)) ? pp->valuestring : "";
                    t->xs[ncx].x = px->valuedouble; t->xs[ncx].z = pz->valuedouble;
                    t->xs[ncx].paint = !strcmp(s, "marked") ? 1 : (!strcmp(s, "signals") ? 2 : (!strcmp(s, "unmarked") ? 0 : 3));   /* [roam LC] unmarked != unknown */
                    t->xs[ncx].way = 0;
                    ncx++;
                } else if (!strcmp(kind, "bus_stop")) {
                    t->bs[ncb].x = px->valuedouble; t->bs[ncb].z = pz->valuedouble;
                    t->bs[ncb].shelter = fu_json_true(e, "shelter");
                    t->bs[ncb].bench = fu_json_true(e, "bench");
                    t->bs[ncb].bin = fu_json_true(e, "bin");
                    ncb++;
                }
            }
        }
        t->nx = ncx; t->nb = ncb;
    }
    cJSON_Delete(root);
    return t;
}

static int fu_index_layer(TD5_RoamFurnSource *f, const char *layer, FuRow **rowsp, int *nrowsp)
{
    char path[512];
    char *json;
    cJSON *root;
    const cJSON *ly, *tl, *t;
    int n = 0;
    td5_geo_source_path(path, sizeof path, f->place, "tiles/INDEX.JSON");
    json = fu_slurp(path);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    ly = root ? cJSON_GetObjectItem(cJSON_GetObjectItem(root, "layers"), layer) : NULL;
    tl = ly ? cJSON_GetObjectItem(ly, "tiles") : NULL;
    if (!tl || !cJSON_IsArray(tl)) { cJSON_Delete(root); return 0; }
    *rowsp = (FuRow *)calloc((size_t)cJSON_GetArraySize(tl) + 1u, sizeof(FuRow));
    if (!*rowsp) { cJSON_Delete(root); return 0; }
    for (t = tl->child; t; t = t->next) {
        const cJSON *a = t->child;
        double v[7];
        int k;
        for (k = 0; k < 7 && a; k++, a = a->next) v[k] = a->valuedouble;
        if (k < 7) continue;
        td5_geo_src_bbox_to_game(&v[3], &v[5]);             /* [roam M] the extent in the game frame */
        (*rowsp)[n].ix = (int)v[0]; (*rowsp)[n].iz = (int)v[1];
        (*rowsp)[n].x0 = v[3]; (*rowsp)[n].z0 = v[4]; (*rowsp)[n].x1 = v[5]; (*rowsp)[n].z1 = v[6];
        n++;
    }
    *nrowsp = n;
    cJSON_Delete(root);
    return 1;
}

static int fu_index_load(TD5_RoamFurnSource *f)
{
    const int ok = fu_index_layer(f, "SIGNALS", &f->rows, &f->nrows);
    fu_index_layer(f, "FOOTWAYS", &f->frows, &f->nfrows);      /* [roam LC] the crossing ways */
    return ok;
}

TD5_RoamFurnSource *td5_roam_furn_open(const char *place)
{
    TD5_RoamFurnSource *f;
    if (!place || !td5_roam_furn_enabled()) return NULL;
    f = (TD5_RoamFurnSource *)calloc(1, sizeof *f);
    if (!f) return NULL;
    snprintf(f->place, sizeof f->place, "%s", place);
    fu_index_load(f);
    f->night = td5_roam_furn_night();
    f->lamp_sw = tg_city_sidewalk_w(&k_biomes[0]);
    f->on_signals = td5_env_int("TD5RE_ROAM_FURN_SIGNALS", 1, 0, 1);
    f->on_lamps   = td5_env_int("TD5RE_ROAM_FURN_LAMPS", 1, 0, 1);
    f->on_barriers = td5_env_int("TD5RE_ROAM_FURN_BARRIERS", 1, 0, 1);
    f->on_piers    = td5_env_int("TD5RE_ROAM_FURN_PIERS", 1, 0, 1);
    f->on_stops   = td5_env_int("TD5RE_ROAM_FURN_STOPS", 1, 0, 1);
    f->on_xings   = td5_env_int("TD5RE_ROAM_FURN_XINGS", 1, 0, 1);
    /* [roam LC] unknown paint: 0 skipped (old), 1 the rule (zebra on a main / wide road, plain lines elsewhere), 2 every
     * unknown a zebra (what TD5RE_GEO_XING_UNKNOWN=1 always meant). Unmarked is never painted in any mode. */
    f->xing_mode = td5_env_int("TD5RE_GEO_XING_UNKNOWN", 0, 0, 1) ? 2 : td5_env_int("TD5RE_ROAM_XING_UNKNOWN", 1, 0, 2);
    f->xing_ways = td5_env_int("TD5RE_ROAM_XING_WAYS", 1, 0, 1);
    return f;
}

void td5_roam_furn_close(TD5_RoamFurnSource *f)
{
    int i;
    if (!f) return;
    for (i = 0; i < FU_SIGN_CACHE; i++) fu_sigtile_free(&f->tile[i]);
    free(f->rows); free(f->frows);
    free(f);
}

/* ============================================================================
 * SECTION: writers (the strip's emitters on a roam frame) + the furniture census
 * ========================================================================== */

typedef struct { double x, z, y, tx, tz, hw; } FuPt;       /* road centre point, travel heading, half carriageway */

typedef struct {
    TG_TileCtx *c;
    TD5_RoamFurnStats *st;
    TD5_RoamFurnSource *f;
    int    sb;
    double road_y;
    int    oom;
} FuEm;

static uint32_t fu_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float    fu_rdf(const uint8_t *p)  { float v; memcpy(&v, p, 4); return v; }

/* The furniture census of one record just written: every face whose XZ footprint overlaps a
 * drivable primitive must hang at least FU_OVERHEAD above the road (an arm, a head, a plate),
 * or be a decal. 0 = clean. */
static int fu_census_record(const FuEm *em, const uint8_t *rec, size_t avail, int decal)
{
    const int tag = (int)(int16_t)(fu_rd32(rec) >> 16);
    const uint32_t ncmd = fu_rd32(rec + 4), nv = fu_rd32(rec + 8);
    const uint32_t cmd_off = fu_rd32(rec + TD5_MESH_DISK_OFF_COMMANDS), vtx_off = fu_rd32(rec + TD5_MESH_DISK_OFF_VERTICES);
    const float ox = fu_rdf(rec + 28) / 256.0f, oy = fu_rdf(rec + 32) / 256.0f, oz = fu_rdf(rec + 36) / 256.0f;
    uint32_t ci, cursor = 0;
    int bad = 0;
    if (decal) return 0;
    if (cmd_off + ncmd * 16u > avail || vtx_off + nv * 44u > avail) return 0;
    for (ci = 0; ci < ncmd; ci++) {
        const uint8_t *cm = rec + cmd_off + ci * 16u;
        const uint32_t tc = (uint32_t)(fu_rd32(cm + 8) & 0xFFFFu), qc = fu_rd32(cm + 8) >> 16;
        uint32_t p;
        for (p = 0; p < tc + qc; p++) {
            const int nvp = p < tc ? 3 : 4;
            double x[4], z[4], ymin = 1e30;
            int k;
            if (cursor + (uint32_t)nvp > nv) return bad;
            for (k = 0; k < nvp; k++) {
                const uint8_t *v = rec + vtx_off + (cursor + (uint32_t)k) * 44u;
                const double vy = (double)fu_rdf(v + 4) + (tag ? (double)oy : 0.0);
                x[k] = (double)fu_rdf(v) + (tag ? (double)ox : 0.0);
                z[k] = (double)fu_rdf(v + 8) + (tag ? (double)oz : 0.0);
                if (vy < ymin) ymin = vy;
            }
            cursor += (uint32_t)nvp;
            if (tag) { x[0] = (double)ox; z[0] = (double)oz; ymin = (double)oy; k = 1; }
            else {
                double fx[4], fz[4];
                int m = 0, a2;
                for (k = 0; k < nvp; k++) {
                    for (a2 = 0; a2 < m; a2++) if (fabs(fx[a2] - x[k]) < 1.0 && fabs(fz[a2] - z[k]) < 1.0) break;
                    if (a2 == m) { fx[m] = x[k]; fz[m] = z[k]; m++; }
                }
                memcpy(x, fx, sizeof fx); memcpy(z, fz, sizeof fz);
                k = m;
                if (k >= 3) {                       /* a degenerate (collinear) polygon is a wall: test its end points */
                    double ar = 0.0;
                    int q;
                    for (q = 0; q < k; q++) { const int q2 = (q + 1) % k; ar += x[q] * z[q2] - x[q2] * z[q]; }
                    if (fabs(0.5 * ar) < 4.0) k = 2;
                }
            }
            if (k >= 1 && k <= 4 && ymin < em->road_y + FU_OVERHEAD && roam_ctx_on_road(em->c, x, z, k)) {
                bad++;
                TD5_LOG_W(LOG_TAG, "ROAM_FURN_GUARD face (%d pts) at local (%.0f,%.0f) y %.0f over the road at y %.0f", k, x[0], z[0],
                          ymin, em->road_y);
            }
        }
    }
    return bad;
}

/* reserve a mesh in the item's sub-block; *off is the offset the record starts at */
static TG_Buf *fu_begin(FuEm *em, size_t *off)
{
    TG_Buf *b = (TG_Buf *)roam_ctx_mesh_begin(em->c, em->sb);
    if (!b) { em->oom = 1; return NULL; }
    *off = b->len;
    return b;
}

/* close it: count it, run the census over its bytes. `ok` = the writer's return. */
static int fu_end(FuEm *em, TG_Buf *b, size_t off, int ok, int decal)
{
    if (!ok || b->oom) { em->oom = 1; return 0; }
    em->st->meshes++;
    em->st->guard_hits += fu_census_record(em, b->b + off, b->len - off, decal);
    return 1;
}

static int fu_box(FuEm *em, double cx, double cy, double cz, double hx, double hy, double hz, double fx, double fz,
                  int page, double tile, unsigned int col)
{
    size_t off;
    TG_Buf *b = fu_begin(em, &off);
    if (!b) return 0;
    return fu_end(em, b, off, tg_emit_box_mesh(b, cx, cy, cz, hx, hy, hz, fx, fz, page, tile, col), 0);
}

static int fu_bill(FuEm *em, double wx, double wy, double wz, double half_w, double height, int page, int tag)
{
    size_t off;
    TG_Buf *b = fu_begin(em, &off);
    if (!b) return 0;
    return fu_end(em, b, off, tg_emit_billboard_mesh(b, wx, wy, wz, half_w, height, page, tag), 0);
}

static int fu_quad(FuEm *em, const double *px, const double *py, const double *pz, const double *uu, const double *vv,
                   int n, const int *seg_page, const int *seg_nq, int nseg, int decal)
{
    size_t off;
    TG_Buf *b = fu_begin(em, &off);
    if (!b) return 0;
    return fu_end(em, b, off, tg_write_quad_mesh(b, px, py, pz, uu, vv, n, seg_page, seg_nq, nseg), decal);
}

/* ---- the mast arm: td5_tg_mastarm.c tg_emit_mastarm, line for line (live heads, mid plate) ---- */

static int fu_emit_plate(FuEm *em, const double pole[2], const double l[2], const double t[2], double side, double u0,
                         double y_top, const FuCell *cf, const FuCell *cb)
{
    double px[8], py[8], pz[8], uu[8], vv[8];
    int seg_page[2], seg_nq[2];
    const FuCell *c[2];
    int face, k;
    c[0] = cf; c[1] = cb;
    for (face = 0; face < 2; face++) {
        const double h  = (double)c[face]->h / (double)FU_PL_ROW_H * FU_MA_PLATE_ROW;
        const double ua = u0, ub = u0 + FU_MA_PLATE_W;
        const int far_left = (face == 0) ? (side < 0.0) : (side > 0.0);
        const double ul = far_left ? ub : ua;
        const double ur = far_left ? ua : ub;
        const double off = (face == 0) ? -FU_MA_PLATE_OFF : FU_MA_PLATE_OFF;
        const double ty = y_top, by = y_top - h;
        const double U0 = 1.5 / 64.0, U1 = 62.5 / 64.0;
        const double V0 = ((double)c[face]->y + 1.5) / 64.0;
        const double V1 = ((double)(c[face]->y + c[face]->h) - 1.5) / 64.0;
        double qx[4], qz[4];
        const double us[4] = { ul, ur, ur, ul };
        for (k = 0; k < 4; k++) {
            qx[k] = pole[0] - l[0] * us[k] + t[0] * off;
            qz[k] = pole[1] - l[1] * us[k] + t[1] * off;
        }
        for (k = 0; k < 4; k++) {
            const int o = face * 4 + k;
            px[o] = qx[k]; pz[o] = qz[k];
            py[o] = (k < 2) ? by : ty;
            uu[o] = (k == 0 || k == 3) ? U0 : U1;
            vv[o] = (k < 2) ? V1 : V0;
        }
        seg_page[face] = FU_PAGE_BASE + c[face]->page;
        seg_nq[face] = 1;
    }
    return fu_quad(em, px, py, pz, uu, vv, 8, seg_page, seg_nq, 2, 0);
}

static int fu_mastarm(FuEm *em, const FuPt *n, double side, double gap, double base_y, const FuCell *cf, const FuCell *cb,
                      int grp)
{
    const double t[2] = { n->tx, n->tz };
    const double l[2] = { n->tz * side, -n->tx * side };
    const double hw = n->hw;
    const double pole[2] = { n->x + l[0] * (hw + gap), n->z + l[1] * (hw + gap) };
    const double road_y = n->y, arm_y = road_y + FU_MA_ARM_Y, top_y = road_y + FU_MA_POLE_TOP;
    double reach = gap + hw + 0.4 * FU_UPM, armlen, plate_u0, head_u[2], disp_u, tip;
    /* the plate centred over the middle of the pole-side half, one head either side (round 1030 D item 12) */
    const double dsep_min = FU_MA_PLATE_W * 0.5 + FU_MA_BP_HW + FU_M(0.12);
    const double u_min = FU_MA_POLE_HW + FU_MA_BP_HW + FU_M(0.10);
    double uc = hw + gap - hw * 0.5, dsep = hw * 0.25;
    int k, lamp;
    if (dsep < dsep_min) dsep = dsep_min;
    if (uc - dsep < u_min) uc = u_min + dsep;
    head_u[0] = uc - dsep; head_u[1] = uc + dsep;
    plate_u0 = uc - FU_MA_PLATE_W * 0.5;
    disp_u = head_u[1] + FU_M(0.62);
    tip = disp_u + FU_M(0.24) + FU_M(0.25) + FU_MA_HEAD_W * 0.5 + FU_M(0.22);
    if (reach < tip) reach = tip;
    armlen = reach;
    if (armlen < plate_u0 + FU_MA_PLATE_W + FU_M(0.3)) armlen = plate_u0 + FU_MA_PLATE_W + FU_M(0.3);
    em->road_y = road_y;

    if (!fu_box(em, pole[0], (base_y + top_y) * 0.5, pole[1], FU_MA_POLE_HW, (top_y - base_y) * 0.5, FU_MA_POLE_HW, t[0], t[1],
                TD5_TG_PAGE_R11_SIGN_POST, FU_MA_POLE_HW * 2.0, 0xFF606064u)) return 0;
    if (!fu_box(em, pole[0] - l[0] * armlen * 0.5, arm_y, pole[1] - l[1] * armlen * 0.5, armlen * 0.5, FU_MA_ARM_HT, FU_MA_ARM_HT,
                -l[1], l[0], TD5_TG_PAGE_R11_SIGN_POST, 1.0e7, 0xFF70747Cu)) return 0;
    for (k = 0; k < 2; k++) {
        const double u = head_u[k];
        const double hx = pole[0] - l[0] * u, hz = pole[1] - l[1] * u;
        const double head_top = arm_y - FU_MA_ARM_HT + FU_M(0.02);
        const double head_y = head_top - FU_MA_HEAD_H * 0.5;
        const double off = FU_MA_HEAD_D * 0.5 + FU_MA_LAMP_R * 0.35;
        if (!fu_box(em, hx, head_y, hz, FU_MA_HEAD_W * 0.5, FU_MA_HEAD_H * 0.5, FU_MA_HEAD_D * 0.5, t[0], t[1],
                    TD5_TG_PAGE_R11_SIGN_POST, FU_MA_HEAD_W, 0xFF242428u)) return 0;
        if (!fu_box(em, hx + t[0] * (FU_MA_HEAD_D * 0.5 + FU_MA_BP_HD), head_y, hz + t[1] * (FU_MA_HEAD_D * 0.5 + FU_MA_BP_HD),
                    FU_MA_BP_HW, FU_MA_BP_HH, FU_MA_BP_HD, t[0], t[1], TD5_TG_PAGE_R11_SIGN_POST, FU_MA_HEAD_W, 0xFF161618u)) return 0;
        for (lamp = 0; lamp < TD5_GEO_SIGNAL_PHASES; lamp++) {
            const double ly = head_y + (1.0 - (double)lamp) * FU_MA_LAMP_DY;
            if (!fu_bill(em, hx - t[0] * off, ly - FU_MA_LAMP_R, hz - t[1] * off, FU_MA_LAMP_R, FU_MA_LAMP_R * 2.0,
                         tg_prop_slot(PP_LAMP), (grp ? TD5_MESH_TAG_SIGNAL_CROSS : TD5_MESH_TAG_SIGNAL_LAMP) + lamp)) return 0;
        }
    }
    {   /* the cross-street head at the arm's tip, lit half a period behind */
        const double u = reach - FU_M(0.22);
        const double hx = pole[0] - l[0] * u, hz = pole[1] - l[1] * u;
        const double head_y = arm_y - FU_MA_ARM_HT + FU_M(0.02) - FU_MA_HEAD_H * 0.5;
        const double off = FU_MA_HEAD_D * 0.5 + FU_MA_LAMP_R * 0.35;
        if (!fu_box(em, hx, head_y, hz, FU_MA_HEAD_W * 0.5, FU_MA_HEAD_H * 0.5, FU_MA_HEAD_D * 0.5, l[0], l[1],
                    TD5_TG_PAGE_R11_SIGN_POST, FU_MA_HEAD_W, 0xFF242428u)) return 0;
        for (lamp = 0; lamp < TD5_GEO_SIGNAL_PHASES; lamp++) {
            const double ly = head_y + (1.0 - (double)lamp) * FU_MA_LAMP_DY;
            if (!fu_bill(em, hx - l[0] * off, ly - FU_MA_LAMP_R, hz - l[1] * off, FU_MA_LAMP_R, FU_MA_LAMP_R * 2.0,
                         tg_prop_slot(PP_LAMP), (grp ? TD5_MESH_TAG_SIGNAL_LAMP : TD5_MESH_TAG_SIGNAL_CROSS) + lamp)) return 0;
        }
    }
    /* [F8] the countdown counts the MAIN head: only on an approach that obeys it */
    if (s_at.ncell > 0 && !grp) {   /* the countdown: a black box, two digit billboards the renderer re-points every frame */
        const double u = disp_u;
        const double dx = pole[0] - l[0] * u, dz = pole[1] - l[1] * u;
        const double dh = FU_M(0.26);
        const double dy = arm_y - FU_MA_ARM_HT + FU_M(0.02) - dh * 0.5 - FU_M(0.06);
        const double dig_hw = FU_M(0.085), dig_h = FU_M(0.22), dig_dx = FU_M(0.105);
        const double far_sign = (side < 0.0) ? 1.0 : -1.0;
        int dgt;
        if (!fu_box(em, dx, dy, dz, FU_M(0.24), dh * 0.5 + FU_M(0.03), FU_M(0.08), t[0], t[1], TD5_TG_PAGE_R11_SIGN_POST,
                    FU_MA_HEAD_W, 0xFF161618u)) return 0;
        for (dgt = 0; dgt < TD5_GEO_SIGNAL_DIGITS; dgt++) {
            const double du = (dgt == 0 ? far_sign : -far_sign) * dig_dx;
            const double ux = pole[0] - l[0] * (u + du), uz = pole[1] - l[1] * (u + du);
            if (!fu_bill(em, ux - t[0] * FU_M(0.10), dy - dig_h * 0.5, uz - t[1] * FU_M(0.10), dig_hw, dig_h,
                         FU_PAGE_BASE + s_at.npg, TD5_MESH_TAG_SIGNAL_DIGIT + dgt)) return 0;
        }
    }
    if (cf && cb) {         /* the plate, hung below the arm from two brackets, both faces named */
        const double drop = FU_M(0.24);
        const double ytop = arm_y - FU_MA_ARM_HT - drop;
        const double bu[2] = { plate_u0 + FU_M(0.18), plate_u0 + FU_MA_PLATE_W - FU_M(0.18) };
        for (k = 0; k < 2; k++)
            if (!fu_box(em, pole[0] - l[0] * bu[k], ytop + drop * 0.5 + FU_M(0.02), pole[1] - l[1] * bu[k], FU_M(0.012),
                        drop * 0.5 + FU_M(0.02), FU_M(0.012), t[0], t[1], TD5_TG_PAGE_R11_SIGN_POST, 1.0e7, 0xFF606064u)) return 0;
        if (!fu_emit_plate(em, pole, l, t, side, plate_u0, ytop, cf, cb)) return 0;
    }
    return 1;
}

/* ---- the post light: tg_emit_geo_signals' plain head (pole, housing, three lamps) ---- */
static int fu_postlight(FuEm *em, const FuPt *n, double side, double gap, double base_y, int grp)
{
    const double lx = n->tz * side, lz = -n->tx * side;
    const double cx = n->x + lx * (n->hw + gap), cz = n->z + lz * (n->hw + gap);
    const double head_y = base_y + FU_SIG_POLE_H - FU_SIG_HEAD_OV + FU_SIG_HEAD_H * 0.5;
    const double off = FU_SIG_HEAD_D * 0.5 + FU_SIG_LAMP_R * 0.35;
    const double fx = cx - n->tx * off, fz = cz - n->tz * off;
    int lamp;
    em->road_y = n->y;
    if (!fu_box(em, cx, base_y + FU_SIG_POLE_H * 0.5, cz, FU_SIG_POLE_W * 0.5, FU_SIG_POLE_H * 0.5, FU_SIG_POLE_W * 0.5, n->tx, n->tz,
                TD5_TG_PAGE_R11_SIGN_POST, FU_SIG_POLE_W, 0xFF606064u)) return 0;
    if (!fu_box(em, cx, head_y, cz, FU_SIG_HEAD_W * 0.5, FU_SIG_HEAD_H * 0.5, FU_SIG_HEAD_D * 0.5, n->tx, n->tz,
                TD5_TG_PAGE_R11_SIGN_POST, FU_SIG_HEAD_W, 0xFF242428u)) return 0;
    for (lamp = 0; lamp < TD5_GEO_SIGNAL_PHASES; lamp++) {
        const double ly = head_y + (1.0 - (double)lamp) * FU_SIG_LAMP_DY;
        if (!fu_bill(em, fx, ly - FU_SIG_LAMP_R, fz, FU_SIG_LAMP_R, FU_SIG_LAMP_R * 2.0, tg_prop_slot(PP_LAMP),
                     (grp ? TD5_MESH_TAG_SIGNAL_CROSS : TD5_MESH_TAG_SIGNAL_LAMP) + lamp)) return 0;
    }
    return 1;
}

/* ============================================================================
 * SECTION: signals (the layout rule per signalised cluster of the 3x3)
 * ========================================================================== */

#define FU_MAX_ARMS 16

typedef struct {
    uint64_t key;
    int      bearing;
    double   ux, uz;                 /* outward, unit, cluster-local                              */
    const RoamEdgeRec *rec;
    const RoamTile *tile;
    const char *name;                /* NULL = unnamed                                            */
    int      name_id;                /* index into the cluster's own distinct-name list, -1 none  */
    int      inbound;                /* approach traffic reaches the cluster on this arm          */
    int      found;                  /* the ribbon was located                                    */
    double   d_cut;                  /* metres from the centroid to the ribbon's first row        */
    FuPt     at[FU_POLE_STEPS];      /* the centre line points the poles may stand at             */
    int      nat;
    int      row_out;                /* the edge's row direction points away from the cluster     */
} FuArm;

static const RoamEdgeRec *fu_find_edge(const TG_TileCtx *c, uint64_t key, const RoamTile **tile)
{
    int dz, dx;
    uint32_t e;
    for (dz = -1; dz <= 1; dz++)
        for (dx = -1; dx <= 1; dx++) {
            const RoamTile *t = roam_ctx_tile(c, dx, dz);
            if (!t) continue;
            for (e = 0; e < t->nedge; e++)
                if (t->edge[e].key == key) { *tile = t; return &t->edge[e]; }
        }
    return NULL;
}

/* Walk out along arm `a` from the centroid (px, pz): the first point that lies on its ribbon is
 * the cut row; poles stand FU_POLE_FIRST_M past it and walk out in steps. */
static void fu_arm_points(const TG_TileCtx *c, double px, double pz, FuArm *a)
{
    double d;
    int k;
    a->found = 0; a->nat = 0; a->d_cut = 0.0;
    for (d = 2.0; d <= FU_ARM_PROBE_MAX_M; d += 1.0) {
        const double qx = px + a->ux * d * FU_UPM, qz = pz + a->uz * d * FU_UPM;
        int seg, side;
        double along, lat;
        RoamFrame fr;
        if (!roam_nearest_edge(c, qx, qz, 8.0 * FU_UPM, &seg, &side, &along, &lat)) continue;
        if (!roam_edge_frame(c, seg, 1, along, &fr) || fr.key != a->key) continue;
        /* the key already says it is OUR ribbon; the lateral gate only rejects a far part of it. A divided avenue's
         * carriageway is offset from the cluster axis by half the median, 6+ m on Diagonal 74. */
        if (fabs(lat) > 12.0 * FU_UPM) continue;
        a->d_cut = d; a->found = 1;
        break;
    }
    if (!a->found) return;
    for (k = 0; k < FU_POLE_STEPS; k++) {
        const double dd = a->d_cut + FU_POLE_FIRST_M + FU_POLE_STEP_M * (double)k;
        const double qx = px + a->ux * dd * FU_UPM, qz = pz + a->uz * dd * FU_UPM;
        int seg, side;
        double along, lat;
        RoamFrame fr;
        FuPt *p = &a->at[a->nat];
        if (!roam_nearest_edge(c, qx, qz, 8.0 * FU_UPM, &seg, &side, &along, &lat)) break;
        if (!roam_edge_frame(c, seg, 1, along, &fr) || fr.key != a->key) break;
        p->x = fr.px; p->z = fr.pz; p->y = fr.py; p->hw = fr.half_carriage;
        /* the heading of an APPROACH: toward the cluster */
        if (fr.tx * a->ux + fr.tz * a->uz > 0.0) { p->tx = -fr.tx; p->tz = -fr.tz; a->row_out = 1; }
        else                                     { p->tx = fr.tx;  p->tz = fr.tz; a->row_out = 0; }
        a->nat++;
    }
}

/* Approach traffic reaches the cluster on arm `a`: two-way, or a one-way that heads toward it.
 * oneway = +1 moves along the edge's row direction t, -1 against it; row_out says t points away
 * from the cluster, so +1 is inbound exactly when t points IN. */
static int fu_arm_inbound(const FuArm *a)
{
    if (!a->rec || !a->nat) return 0;
    if (a->rec->oneway == 0) return 1;
    return ((int8_t)a->rec->oneway > 0) ? !a->row_out : a->row_out;
}

static int fu_dist_ok(const double (*pts)[2], int n, double x, double z, double minsep)
{
    int i;
    for (i = 0; i < n; i++)
        if (hypot(pts[i][0] - x, pts[i][1] - z) < minsep) return 0;
    return 1;
}

/* Where one head stands: the first of the arm's points whose pole is off the road, clear of the poles
 * already taken at this cluster, and wide enough. 1 and *out / *base_y / *pole on success. */
static int fu_stand(FuEm *em, const FuArm *a, const double hflip, double side, double gap, double minhw, FuPt *out,
                    double *base_y, double pole[2], const double (*taken)[2], int ntaken, TD5_RoamFurnStats *drops,
                    int count_drops)
{
    int k;
    for (k = 0; k < a->nat; k++) {
        FuPt p = a->at[k];
        double lx, lz, cx, cz;
        const double h = 0.30 * FU_UPM;
        double sx[4], sz[4];
        if (hflip < 0.0) { p.tx = -p.tx; p.tz = -p.tz; }
        lx = p.tz * side; lz = -p.tx * side;
        cx = p.x + lx * (p.hw + gap); cz = p.z + lz * (p.hw + gap);
        sx[0] = cx - h; sx[1] = cx + h; sx[2] = cx + h; sx[3] = cx - h;
        sz[0] = cz - h; sz[1] = cz - h; sz[2] = cz + h; sz[3] = cz + h;
        if (p.hw < minhw) return 0;
        if (roam_ctx_on_road(em->c, sx, sz, 4)) { if (count_drops) drops->sig_drop_road++; continue; }
        if (!fu_dist_ok(taken, ntaken, cx, cz, FU_POLE_SEP_M * FU_UPM)) { if (count_drops) drops->sig_drop_dup++; continue; }
        *out = p;
        *base_y = p.y + FU_KERB_H;
        pole[0] = cx; pole[1] = cz;
        return 1;
    }
    return 0;
}

static int fu_emit_cluster(FuEm *em, const RoamTile *t, const RoamNodeRec *nr, double px, double pz)
{
    TG_TileCtx *c = em->c;
    TD5_RoamFurnStats *st = em->st;
    FuArm arm[FU_MAX_ARMS];
    int nm[FU_MAX_ARMS];
    int16_t bear[FU_MAX_ARMS];
    const char *dn[FU_MAX_ARMS];
    int ndn = 0, na = (int)nr->narm, a, owned_c = roam_ctx_owned(c, px, pz);
    double taken[FU_MAX_ARMS * 3][2];
    int ntaken = 0;
    if (na > FU_MAX_ARMS) na = FU_MAX_ARMS;
    memset(arm, 0, sizeof arm);
    for (a = 0; a < na; a++) {
        const RoamArmRec *ar = &t->arm[nr->arm_first + (uint32_t)a];
        FuArm *m = &arm[a];
        int j;
        m->key = ar->edge_key; m->bearing = ar->bearing12;
        fu_bearing(ar->bearing12, &m->ux, &m->uz);
        m->rec = fu_find_edge(c, ar->edge_key, &m->tile);
        m->name = m->rec ? fu_tile_name(m->tile, m->rec->name_id) : NULL;
        m->name_id = -1;
        if (m->name) {
            for (j = 0; j < ndn; j++) if (strcmp(dn[j], m->name) == 0) break;
            if (j == ndn) dn[ndn++] = m->name;
            m->name_id = j;
        }
        nm[a] = m->name_id; bear[a] = ar->bearing12;
        fu_arm_points(c, px, pz, m);
        m->inbound = fu_arm_inbound(m);
        if (owned_c && td5_env_int("TD5RE_ROAM_FURN_DBG", 0, 0, 2) == 1) {
            int seg = -1, sd = 0;
            double al = 0, lt = 0, dd;
            unsigned long long nk = 0;
            for (dd = 4.0; dd <= 30.0; dd += 6.0) {
                RoamFrame fr;
                if (roam_nearest_edge(c, px + m->ux * dd * FU_UPM, pz + m->uz * dd * FU_UPM, 8.0 * FU_UPM, &seg, &sd, &al, &lt) &&
                    roam_edge_frame(c, seg, 1, al, &fr)) nk = (unsigned long long)fr.key;
                else nk = 0;
                TD5_LOG_I(LOG_TAG, "FURN_DBG cluster %lld arm %d key %llu rec=%d spans=%d klass=%d oneway=%d cut_span=%u | probe d=%.0f nearest key %llu lat %.0f",
                          (long long)nr->cluster_key, a, (unsigned long long)m->key, m->rec != NULL, m->rec ? m->rec->span_count : -1,
                          m->rec ? m->rec->klass : -1, m->rec ? (int)(int8_t)m->rec->oneway : 0, (unsigned)t->arm[nr->arm_first + (uint32_t)a].cut_span,
                          dd, nk, lt);
            }
        }
        if (owned_c) {
            st->sig_arms++;
            if (m->rec && !m->found) st->sig_drop_frame++;      /* an arm WITH a ribbon whose frame was not found */
            if (!m->rec) st->sig_arms_absorbed++;
            if (m->inbound) st->sig_inbound++;
        }
    }
    if (owned_c) st->sig_clusters++;
    for (a = 0; a < na; a++) {
        const FuArm *m = &arm[a];
        const int xarm = fu_pick_cross(nm, bear, na, a);
        const char *own = m->name;
        const char *crs = xarm >= 0 ? arm[xarm].name : NULL;
        const char *fn = crs ? crs : own, *bn = own ? own : crs;
        const FuCell *cf = NULL, *cb = NULL;
        FuPt at;
        double base_y, pole[2];
        int e, best_e = -1;
        double best_dot = 0.82;                  /* within ~35 degrees of straight through */
        /* [F8] the head this approach obeys: the ambient traffic stops at the same lamp (td5_roam_traffic.c) */
        const int grp = td5_roam_signal_group(bear, na, a);
        if (!m->inbound) continue;
        if (fn || bn) {
            cf = fu_cell_find(fn, 0); cb = fu_cell_find(bn, 0);
            if (!cf) cf = cb;
            if (!cb) cb = cf;
        }
        /* N: the near arm on the RIGHT kerb (plate, heads facing the approaching driver); a carriageway too
         * narrow for an arm keeps the plain post */
        if (fu_stand(em, m, 1.0, -1.0, FU_SIG_GAP, 0.0, &at, &base_y, pole, (const double (*)[2])taken, ntaken, st, owned_c)) {
            taken[ntaken][0] = pole[0]; taken[ntaken][1] = pole[1]; ntaken++;
            if (roam_ctx_owned(c, pole[0], pole[1])) {
                em->sb = roam_ctx_sb_of(c, pole[0], pole[1]);
                if (at.hw * 2.0 >= FU_MA_MIN_WIDTH) {
                    if (!fu_mastarm(em, &at, -1.0, FU_SIG_GAP, base_y, cf, cb, grp)) return 0;
                    st->mast_near++;
#ifndef TD5RE_RELEASE
                    if (td5_env_int("TD5RE_ROAM_FURN_DBG", 0, 0, 2) == 2) {   /* [F4f] where to stand to read this plate */
                        int ttx, ttz; int32_t ox, oz; double lat = 0, lon = 0;
                        const double b12 = atan2(at.tz, at.tx) * 4096.0 / 6.283185307179586;
                        roam_ctx_origin(c, &ttx, &ttz, &ox, &oz);
                        td5_roam_place_to_latlon("la_plata_partido", (at.x - at.tx * 11.0 * FU_UPM) + ox,
                                                 (at.z - at.tz * 11.0 * FU_UPM) + oz, &lat, &lon);
                        TD5_LOG_I(LOG_TAG, "FURN_PLATE spawn=%.6f,%.6f yaw=%d front='%s' back='%s'", lat, lon,
                                  ((1024 - (int)floor(b12 + 0.5)) & 0xFFF), cf ? cf->name : "-", cb ? cb->name : "-");
                    }
#endif
                    if (cf && cb) { st->plates++; if (cf == cb && fn && bn && strcmp(fn, bn) != 0) st->plate_one_face++; }
                    else st->plate_none++;
                } else {
                    if (!fu_postlight(em, &at, -1.0, FU_SIG_GAP, base_y, grp)) return 0;
                    st->post_left++;
                }
            }
        }
        /* P: the plain post light on the LEFT kerb before the junction */
        if (fu_stand(em, m, 1.0, 1.0, FU_SIG_GAP, 0.0, &at, &base_y, pole, (const double (*)[2])taken, ntaken, st, owned_c)) {
            taken[ntaken][0] = pole[0]; taken[ntaken][1] = pole[1]; ntaken++;
            if (roam_ctx_owned(c, pole[0], pole[1])) {
                em->sb = roam_ctx_sb_of(c, pole[0], pole[1]);
                if (!fu_postlight(em, &at, 1.0, FU_SIG_GAP, base_y, grp)) return 0;
                st->post_left++;
            }
        }
        /* F: the far arm across the junction, on the far LEFT corner of the arm that carries the approach straight
         * through. Where that arm has an approach of its own its near arm already stands on this very corner. */
        for (e = 0; e < na; e++) {
            const double d = arm[e].ux * -m->ux + arm[e].uz * -m->uz;
            if (e == a) continue;
            if (d > best_dot) { best_dot = d; best_e = e; }
        }
        if (best_e >= 0 && arm[best_e].found) {
            if (arm[best_e].inbound) { if (owned_c) st->far_shared++; }
            else if (fu_stand(em, &arm[best_e], -1.0, 1.0, FU_SIG_GAP, FU_MA_MIN_WIDTH * 0.5, &at, &base_y, pole,
                              (const double (*)[2])taken, ntaken, st, owned_c)) {
                taken[ntaken][0] = pole[0]; taken[ntaken][1] = pole[1]; ntaken++;
                if (roam_ctx_owned(c, pole[0], pole[1])) {
                    em->sb = roam_ctx_sb_of(c, pole[0], pole[1]);
                    if (!fu_mastarm(em, &at, 1.0, FU_SIG_GAP, base_y, cf, cb, grp)) return 0;
                    st->mast_far++;
                }
            }
        }
    }
    return 1;
}

static int fu_cluster_signalised(const RoamTile *t, int64_t key)
{
    uint32_t s;
    for (s = 0; s < t->nsign; s++) if (t->sign[s].cluster_key == key) return 1;
    return 0;
}

static int fu_emit_signals(FuEm *em)
{
    TG_TileCtx *c = em->c;
    const RoamGrid *g = roam_ctx_grid(c);
    int tx, tz, dx, dz, k;
    int32_t ox, oz;
    double corner[4][2], x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
    const double margin = 80.0 * FU_UPM;
    roam_ctx_origin(c, &tx, &tz, &ox, &oz);
    roam_grid_corners(g, tx, tz, corner);
    for (k = 0; k < 4; k++) {
        const double x = corner[k][0] - (double)ox, z = corner[k][1] - (double)oz;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (z < z0) z0 = z;
        if (z > z1) z1 = z;
    }
    for (dz = -1; dz <= 1; dz++)
        for (dx = -1; dx <= 1; dx++) {
            const RoamTile *t = roam_ctx_tile(c, dx, dz);
            uint32_t n;
            if (!t) continue;
            for (n = 0; n < t->nnode; n++) {
                const RoamNodeRec *nr = &t->node[n];
                const double px = (double)nr->x + (double)(t->org_x - ox), pz = (double)nr->z + (double)(t->org_z - oz);
                if (px < x0 - margin || px > x1 + margin || pz < z0 - margin || pz > z1 + margin) continue;
                if (!fu_cluster_signalised(t, nr->cluster_key)) continue;
                if (!fu_emit_cluster(em, t, nr, px, pz)) return 0;
            }
        }
    return 1;
}

/* ============================================================================
 * SECTION: lamps (along the owned quads)
 * ========================================================================== */

typedef struct { float *p; uint32_t n, cap; } FuLamps;

static int fu_lamp_push(FuLamps *l, double x, double y, double z)
{
    if (l->n == l->cap) {
        const uint32_t nc = l->cap ? l->cap * 2u : 64u;
        float *np = (float *)realloc(l->p, (size_t)nc * 3u * sizeof(float));
        if (!np) return 0;
        l->p = np; l->cap = nc;
    }
    l->p[l->n * 3 + 0] = (float)x; l->p[l->n * 3 + 1] = (float)y; l->p[l->n * 3 + 2] = (float)z;
    l->n++;
    return 1;
}

static int fu_lamp_period(int klass)
{
    if (klass >= TD5_GEO_RC_SECONDARY) return 8;           /* avenues: every 28 m                    */
    if (klass >= TD5_GEO_RC_UNCLASSIFIED) return 10;       /* tertiary / unclassified: every 35 m    */
    if (klass >= TD5_GEO_RC_LIVING) return 11;             /* residential / living: every 38 m       */
    return 0;                                              /* service, unknown: none                  */
}

/* ONE mesh per lamp: two crossed quads of post, one flat quad of arm over the road (the strip's lantern
 * art, td5_tg_city.c tg_city_emit_lamp, honestly modelled: nothing low hangs over the carriageway). */
static int fu_lamp(FuEm *em, double px, double pz, double lx, double lz, double tx, double tz, double base_y, double top_y,
                   double reach, double *hx, double *hz)
{
    double qx[12], qy[12], qz[12], uu[12], vv[12];
    const double h = 0.5 * FU_M(0.08), ah = 0.5 * FU_M(0.10);
    int seg_page = TD5_TG_PAGE_R11_SIGN_POST, seg_nq = 3;
    const double ex = px - lx * reach, ez = pz - lz * reach;
    static const double u4[4] = { 0.0, 1.0, 1.0, 0.0 }, v4[4] = { 1.0, 1.0, 0.0, 0.0 };
    int k;
    /* post A (spans the lateral axis) */
    qx[0] = px - lx * h; qy[0] = base_y; qz[0] = pz - lz * h;
    qx[1] = px + lx * h; qy[1] = base_y; qz[1] = pz + lz * h;
    qx[2] = px + lx * h; qy[2] = top_y;  qz[2] = pz + lz * h;
    qx[3] = px - lx * h; qy[3] = top_y;  qz[3] = pz - lz * h;
    /* post B (spans the along axis) */
    qx[4] = px - tx * h; qy[4] = base_y; qz[4] = pz - tz * h;
    qx[5] = px + tx * h; qy[5] = base_y; qz[5] = pz + tz * h;
    qx[6] = px + tx * h; qy[6] = top_y;  qz[6] = pz + tz * h;
    qx[7] = px - tx * h; qy[7] = top_y;  qz[7] = pz - tz * h;
    /* arm: flat, from the post top to the lantern, over the road */
    qx[8] = px - tx * ah;  qy[8] = top_y; qz[8] = pz - tz * ah;
    qx[9] = px + tx * ah;  qy[9] = top_y; qz[9] = pz + tz * ah;
    qx[10] = ex + tx * ah; qy[10] = top_y; qz[10] = ez + tz * ah;
    qx[11] = ex - tx * ah; qy[11] = top_y; qz[11] = ez - tz * ah;
    for (k = 0; k < 12; k++) { uu[k] = u4[k & 3]; vv[k] = v4[k & 3]; }
    *hx = ex; *hz = ez;
    return fu_quad(em, qx, qy, qz, uu, vv, 12, &seg_page, &seg_nq, 1, 0);
}

static int fu_emit_lamps(FuEm *em, TD5_RoamFurnSource *f, FuLamps *out)
{
    TG_TileCtx *c = em->c;
    const RoamTile *t = roam_ctx_tile(c, 0, 0);
    uint32_t e, i;
    if (!t) return 1;
    for (e = 0; e < t->nedge; e++) {
        const RoamEdgeRec *er = &t->edge[e];
        const int P = fu_lamp_period(er->klass);
        uint32_t hsh;
        if (P <= 0) { em->st->lamp_skip_class += (int)er->span_count; continue; }
        if (er->flags & (ROAM_EF_DECK | ROAM_EF_BORE | ROAM_EF_PASSAGE)) continue;
        hsh = fu_fnv(2166136261u, &er->key, sizeof er->key) % (uint32_t)P;
        for (i = 0; i < er->span_count; i++) {
            const uint32_t si = (uint32_t)er->span_first + i;
            const uint32_t row = (uint32_t)er->row0 + i;
            const TD5_StripSpan *sp;
            uint32_t L, R;
            int n, idx, side;
            double ax, ay, az, bx, bz, by, hw, len, tx, tz, lx, lz, stand, post, cx, cz, base_y, top_y, headx, headz, y;
            double sx[4], sz[4];
            double l0x, l0y, l0z, ln_x, ln_y, ln_z, r0x, r0y, r0z, rn_x, rn_y, rn_z;
            const TD5_StripVertex *v;
            if (si >= t->nspan) break;
            if (((row + hsh) % (uint32_t)P) != 0u) continue;
            sp = &t->span[si];
            L = sp->left_vertex_index; R = sp->right_vertex_index;
            n = (int)(sp->pad_02[1] & 0x0Fu);
            if (n < 1 || L + (uint32_t)n >= t->nvert || R + (uint32_t)n >= t->nvert) continue;
            v = &t->vert[L];
            l0x = (double)sp->origin_x + v->x; l0y = (double)sp->origin_y + v->y; l0z = (double)sp->origin_z + v->z;
            v = &t->vert[L + (uint32_t)n];
            ln_x = (double)sp->origin_x + v->x; ln_y = (double)sp->origin_y + v->y; ln_z = (double)sp->origin_z + v->z;
            v = &t->vert[R];
            r0x = (double)sp->origin_x + v->x; r0y = (double)sp->origin_y + v->y; r0z = (double)sp->origin_z + v->z;
            v = &t->vert[R + (uint32_t)n];
            rn_x = (double)sp->origin_x + v->x; rn_y = (double)sp->origin_y + v->y; rn_z = (double)sp->origin_z + v->z;
            ax = 0.5 * (l0x + ln_x); az = 0.5 * (l0z + ln_z); ay = 0.5 * (l0y + ln_y);
            bx = 0.5 * (r0x + rn_x); bz = 0.5 * (r0z + rn_z); by = 0.5 * (r0y + rn_y);
            (void)r0y; (void)rn_y; (void)ln_y; (void)l0y;
            hw = 0.25 * (hypot(ln_x - l0x, ln_z - l0z) + hypot(rn_x - r0x, rn_z - r0z));
            len = hypot(bx - ax, bz - az);
            if (!(len > 1e-6)) continue;
            tx = (bx - ax) / len; tz = (bz - az) / len;
            cx = 0.5 * (ax + bx); cz = 0.5 * (az + bz); y = 0.5 * (ay + by);
            idx = (int)((row + hsh) / (uint32_t)P);
            if ((int8_t)er->oneway > 0)      side = -1;     /* travel along t: the right kerb is side -1      */
            else if ((int8_t)er->oneway < 0) side = 1;      /* travel against t                               */
            else                             side = (idx & 1) ? 1 : -1;
            lx = tz * (double)side; lz = -tx * (double)side;
            stand = f->lamp_sw * 0.35;
            post = hw + stand;
            sx[0] = cx + lx * post; sz[0] = cz + lz * post;       /* the post's foot */
            {
                const double h = 0.25 * FU_UPM;
                const double qx[4] = { sx[0] - h, sx[0] + h, sx[0] + h, sx[0] - h };
                const double qz[4] = { sz[0] - h, sz[0] - h, sz[0] + h, sz[0] + h };
                if (roam_ctx_on_road(c, qx, qz, 4)) { em->st->lamp_drop_road++; continue; }
            }
            em->sb = roam_ctx_sb_of(c, sx[0], sz[0]);
            em->road_y = y;
            base_y = y + FU_KERB_H; top_y = y + FU_LAMP_H;
            if (!fu_lamp(em, sx[0], sz[0], lx, lz, tx, tz, base_y, top_y, FU_LAMP_REACH, &headx, &headz)) return 0;
            em->st->lamps++;
            if (f->night) {   /* the lantern's additive glow, the strip's PP_LAMP billboard (k_prop_pages[PP_LAMP]) */
                const TG_PropPage *pp = &k_prop_pages[PP_LAMP];
                if (!fu_bill(em, headx, y + (double)pp->y_off, headz, (double)pp->w * 0.5, (double)pp->h, tg_prop_slot(PP_LAMP), pp->tag))
                    return 0;
                em->st->lamp_glow++;
            }
            if (!fu_lamp_push(out, headx, top_y - 60.0, headz)) return 0;
        }
    }
    return 1;
}

/* ============================================================================
 * SECTION: bus stops and crossings (OSM nodes of the SIGNALS tiles)
 * ========================================================================== */

#define FU_XWAY_DUP_M   9.0      /* [roam LC] a way's midpoint this near a crossing node is the same crossing */

/* 1 when a crossing NODE (any resident SIGNALS tile, place frame) lies within `r` units of the place point */
static int fu_node_near(TD5_RoamFurnSource *f, double x, double z, double r)
{
    int k;
    for (k = 0; k < f->nrows; k++) {
        const FuRow *row = &f->rows[k];
        FuSigTile *st;
        int i;
        if (x < row->x0 - r || x > row->x1 + r || z < row->z0 - r || z > row->z1 + r) continue;
        st = fu_sigtile(f, 0, row->ix, row->iz);
        for (i = 0; i < st->nx; i++)
            if (hypot(st->xs[i].x - x, st->xs[i].z - z) < r) return 1;
    }
    return 0;
}

/* the SIGNALS tiles that meet the tile's box (+ margin), their nodes owned by the tile, in (x, z) order */
static int fu_collect_nodes(FuEm *em, TD5_RoamFurnSource *f, FuXing **xs, int *nx, FuStop **bs, int *nb)
{
    TG_TileCtx *c = em->c;
    const RoamGrid *g = roam_ctx_grid(c);
    int tx, tz, k, r, capx = 0, capb = 0;
    int32_t ox, oz;
    double corner[4][2], x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
    roam_ctx_origin(c, &tx, &tz, &ox, &oz);
    roam_grid_corners(g, tx, tz, corner);
    for (k = 0; k < 4; k++) {
        if (corner[k][0] < x0) x0 = corner[k][0];
        if (corner[k][0] > x1) x1 = corner[k][0];
        if (corner[k][1] < z0) z0 = corner[k][1];
        if (corner[k][1] > z1) z1 = corner[k][1];
    }
    for (r = 0; r < f->nrows; r++) {
        const FuRow *row = &f->rows[r];
        FuSigTile *st;
        int i;
        if (row->x1 < x0 || row->x0 > x1 || row->z1 < z0 || row->z0 > z1) continue;
        st = fu_sigtile(f, 0, row->ix, row->iz);
        for (i = 0; i < st->nx; i++) {
            const double lx = st->xs[i].x - (double)ox, lz = st->xs[i].z - (double)oz;
            if (!roam_ctx_owned(c, lx, lz)) continue;
            if (!fu_grow((void **)xs, &capx, *nx + 1, sizeof **xs)) return 0;
            (*xs)[*nx] = st->xs[i]; (*xs)[*nx].x = lx; (*xs)[*nx].z = lz; (*nx)++;
        }
        for (i = 0; i < st->nb; i++) {
            const double lx = st->bs[i].x - (double)ox, lz = st->bs[i].z - (double)oz;
            if (!roam_ctx_owned(c, lx, lz)) continue;
            if (!fu_grow((void **)bs, &capb, *nb + 1, sizeof **bs)) return 0;
            (*bs)[*nb] = st->bs[i]; (*bs)[*nb].x = lx; (*bs)[*nb].z = lz; (*nb)++;
        }
    }
    if (f->xing_ways) {                                  /* [roam LC] footway=crossing ways, minus those a node already covers */
        const double dup = FU_XWAY_DUP_M * FU_UPM;
        FuXing *wy = NULL;
        int nwy = 0, capw = 0, j;
        for (r = 0; r < f->nfrows; r++) {                /* copy first: fu_node_near below may evict the FOOTWAYS tile */
            const FuRow *row = &f->frows[r];
            FuSigTile *st;
            int i;
            if (row->x1 < x0 || row->x0 > x1 || row->z1 < z0 || row->z0 > z1) continue;
            st = fu_sigtile(f, 1, row->ix, row->iz);
            for (i = 0; i < st->nx; i++) {
                const double lx = st->xs[i].x - (double)ox, lz = st->xs[i].z - (double)oz;
                if (!roam_ctx_owned(c, lx, lz)) continue;
                if (!fu_grow((void **)&wy, &capw, nwy + 1, sizeof *wy)) { free(wy); return 0; }
                wy[nwy] = st->xs[i]; wy[nwy].x = lx; wy[nwy].z = lz; nwy++;
            }
        }
        for (j = 0; j < nwy; j++) {
            em->st->xing_ways++;
            if (fu_node_near(f, wy[j].x + (double)ox, wy[j].z + (double)oz, dup)) { em->st->xing_way_dup++; continue; }
            if (!fu_grow((void **)xs, &capx, *nx + 1, sizeof **xs)) { free(wy); return 0; }
            (*xs)[(*nx)++] = wy[j];
        }
        free(wy);
    }
    if (*nx > 1) qsort(*xs, (size_t)*nx, sizeof **xs, fu_xing_cmp);
    if (*nb > 1) qsort(*bs, (size_t)*nb, sizeof **bs, fu_stop_cmp);
    return 1;
}

static int fu_emit_stops(FuEm *em, const FuStop *bs, int nb)
{
    TG_TileCtx *c = em->c;
    TD5_RoamFurnStats *st = em->st;
    double taken[64][3];
    int ntaken = 0, i;
    for (i = 0; i < nb; i++) {
        const FuStop *b = &bs[i];
        int seg, side;
        double along, lat, lx, lz, cx, cz, base_y, gap = TD5_TG_R11_SIGN_GAP + 0.5 * FU_BS_POLE_W;
        RoamFrame fr;
        int shelter = b->shelter, j, dup = 0;
        if (!roam_nearest_edge(c, b->x, b->z, FU_BS_NEAR_M * FU_UPM, &seg, &side, &along, &lat) ||
            !roam_edge_frame(c, seg, 1, along, &fr) || fabs(lat) > fr.half_carriage + 8.0 * FU_UPM) { st->stop_drop_far++; continue; }
        side = lat >= 0.0 ? 1 : -1;
        lx = fr.tz * (double)side; lz = -fr.tx * (double)side;
        cx = fr.px + lx * (fr.half_carriage + gap); cz = fr.pz + lz * (fr.half_carriage + gap);
        {   /* the pole itself */
            const double h = 0.30 * FU_UPM;
            const double qx[4] = { cx - h, cx + h, cx + h, cx - h }, qz[4] = { cz - h, cz - h, cz + h, cz + h };
            if (roam_ctx_on_road(c, qx, qz, 4)) { st->stop_drop_road++; continue; }
        }
        for (j = 0; j < ntaken && !dup; j++)
            if (taken[j][2] == (double)side && hypot(taken[j][0] - cx, taken[j][1] - cz) < 8.0 * FU_UPM) dup = 1;
        if (dup) { st->stop_drop_far++; continue; }
        if (ntaken < 64) { taken[ntaken][0] = cx; taken[ntaken][1] = cz; taken[ntaken][2] = (double)side; ntaken++; }
        if (shelter) {   /* a shelter is 3.2 x 1.3 m BEHIND the pole: refused where it would stand on a road */
            const double d0 = fr.half_carriage + gap, d1 = d0 + FU_BS_SHEL_D, hw2 = FU_BS_SHEL_W * 0.5;
            const double qx[4] = { fr.px + lx * d0 - fr.tx * hw2, fr.px + lx * d0 + fr.tx * hw2,
                                   fr.px + lx * d1 + fr.tx * hw2, fr.px + lx * d1 - fr.tx * hw2 };
            const double qz[4] = { fr.pz + lz * d0 - fr.tz * hw2, fr.pz + lz * d0 + fr.tz * hw2,
                                   fr.pz + lz * d1 + fr.tz * hw2, fr.pz + lz * d1 - fr.tz * hw2 };
            if (roam_ctx_on_road(c, qx, qz, 4)) shelter = 0;
        }
        base_y = fr.py + FU_KERB_H;
        em->sb = roam_ctx_sb_of(c, cx, cz);
        em->road_y = fr.py;
        if (!fu_box(em, cx, base_y + FU_BS_POLE_H * 0.5, cz, FU_BS_POLE_W * 0.5, FU_BS_POLE_H * 0.5, FU_BS_POLE_W * 0.5, fr.tx, fr.tz,
                    TD5_TG_PAGE_R11_SIGN_POST, FU_BS_POLE_W, 0xFF6E7276u)) return 0;
        if (!fu_box(em, cx, base_y + FU_BS_POLE_H - FU_BS_FLAG_H * 0.5, cz, FU_BS_FLAG_W * 0.5, FU_BS_FLAG_H * 0.5, FU_BS_FLAG_D * 0.5,
                    fr.tx, fr.tz, TD5_TG_PAGE_R11_SIGN_POST, FU_BS_FLAG_W, 0xFFC03028u)) return 0;
        if (shelter) {
            const double mid = fr.half_carriage + gap + FU_BS_SHEL_D * 0.5, bak = fr.half_carriage + gap + FU_BS_SHEL_D;
            if (!fu_box(em, fr.px + lx * mid, base_y + FU_BS_SHEL_H + FU_BS_ROOF_T * 0.5, fr.pz + lz * mid, FU_BS_SHEL_W * 0.5,
                        FU_BS_ROOF_T * 0.5, FU_BS_SHEL_D * 0.5, lx, lz, TD5_TG_PAGE_R11_SIGN_POST, FU_BS_SHEL_W, 0xFF3A3E44u)) return 0;
            if (!fu_box(em, fr.px + lx * bak, base_y + FU_BS_SHEL_H * 0.5, fr.pz + lz * bak, FU_BS_SHEL_W * 0.5, FU_BS_SHEL_H * 0.5,
                        FU_BS_BACK_T * 0.5, lx, lz, TD5_TG_PAGE_R11_SIGN_POST, FU_BS_SHEL_W, 0xFF8A9096u)) return 0;
            st->shelters++;
        }
        if (b->bench) {
            const double d = fr.half_carriage + gap + (shelter ? FU_BS_SHEL_D * 0.62 : FU_BS_BENCH_D);
            const double ex = fr.px + lx * d + fr.tx * FU_BS_SHEL_W * 0.22, ez = fr.pz + lz * d + fr.tz * FU_BS_SHEL_W * 0.22;
            if (!roam_ctx_on_road(c, &ex, &ez, 1)) {
                if (!fu_box(em, ex, base_y + FU_BS_BENCH_H * 0.5, ez, FU_BS_BENCH_W * 0.5, FU_BS_BENCH_H * 0.5, FU_BS_BENCH_D * 0.5,
                            lx, lz, TD5_TG_PAGE_R11_SIGN_POST, FU_BS_BENCH_W, 0xFF6B4A2Eu)) return 0;
                st->benches++;
            }
        }
        if (b->bin) {
            const double ix = cx - fr.tx * FU_BS_SHEL_W * 0.40, iz = cz - fr.tz * FU_BS_SHEL_W * 0.40;
            if (!roam_ctx_on_road(c, &ix, &iz, 1)) {
                if (!fu_box(em, ix, base_y + FU_BS_BIN_H * 0.5, iz, FU_BS_BIN_W * 0.5, FU_BS_BIN_H * 0.5, FU_BS_BIN_W * 0.5, fr.tx, fr.tz,
                            TD5_TG_PAGE_R11_SIGN_POST, FU_BS_BIN_W, 0xFF2F4A34u)) return 0;
                st->bins++;
            }
        }
        st->stops++;
        if (td5_env_int("TD5RE_ROAM_FURN_DBG", 0, 0, 2) == 2) {
            int32_t ox, oz;
            roam_ctx_origin(c, NULL, NULL, &ox, &oz);
            TD5_LOG_I(LOG_TAG, "FURN_STOP place %.0f %.0f shelter %d bench %d bin %d", cx + (double)ox, cz + (double)oz, shelter, b->bench, b->bin);
        }
    }
    return 1;
}

static int fu_emit_xings(FuEm *em, TD5_RoamFurnSource *f, const FuXing *xs, int nx)
{
    TG_TileCtx *c = em->c;
    TD5_RoamFurnStats *st = em->st;
    int i;
    for (i = 0; i < nx; i++) {
        const FuXing *x = &xs[i];
        int seg, side, lanes;
        double along, lat, L, px[4], py[4], pz[4], uu[4], vv[4];
        int seg_page = TD5_TG_PAGE_CROSSING, seg_nq = 1;
        RoamFrame fr, f0, f1;
        int paint = x->paint;
        if (paint == 0) { st->xing_unmarked++; continue; }                   /* [roam LC] explicitly unmarked: never painted */
        if (paint == 3 && f->xing_mode == 0) { st->xing_unknown++; continue; }
        if (!roam_nearest_edge(c, x->x, x->z, 12.0 * FU_UPM, &seg, &side, &along, &lat) ||
            !roam_edge_frame(c, seg, 1, along, &fr) ||
            hypot(x->x - fr.px, x->z - fr.pz) > fr.half_carriage + FU_X_NEAR_M * FU_UPM) { st->xing_drop_far++; continue; }
        if (!roam_edge_frame(c, seg, 1, along - FU_X_HALF, &f0) || !roam_edge_frame(c, seg, 1, along + FU_X_HALF, &f1)) {
            st->xing_drop_far++; continue;
        }
        (void)side;
        lanes = (int)(2.0 * fr.half_carriage / 1500.0 + 0.5);
        if (lanes < 1) lanes = 1;
        if (paint == 3) {
            /* [roam LC] the rule for a crossing OSM never said how to paint: a zebra on a main road (tertiary and up)
             * or a wide carriageway (3+ lanes), plain limit lines on a residential street. Mode 2: always a zebra. */
            const int main_road = fr.klass >= TD5_GEO_RC_TERTIARY || lanes >= 3;
            paint = (f->xing_mode == 2 || main_road) ? 1 : 2;
            if (paint == 1) st->xing_unk_zebra++; else st->xing_unk_lines++;
        }
        L = (double)lanes;
        /* the band across the whole carriageway: right edge, left edge, left edge ahead, right edge ahead */
        px[0] = f0.px - f0.nx * fr.half_carriage; pz[0] = f0.pz - f0.nz * fr.half_carriage; py[0] = f0.py + FU_X_LIFT;
        px[1] = f0.px + f0.nx * fr.half_carriage; pz[1] = f0.pz + f0.nz * fr.half_carriage; py[1] = f0.py + FU_X_LIFT;
        px[2] = f1.px + f1.nx * fr.half_carriage; pz[2] = f1.pz + f1.nz * fr.half_carriage; py[2] = f1.py + FU_X_LIFT;
        px[3] = f1.px - f1.nx * fr.half_carriage; pz[3] = f1.pz - f1.nz * fr.half_carriage; py[3] = f1.py + FU_X_LIFT;
        if (paint != 2) {                     /* a ZEBRA: u across the lanes, so the bars run along travel */
            uu[0] = 0.0; vv[0] = 0.0; uu[1] = L; vv[1] = 0.0; uu[2] = L; vv[2] = 1.0; uu[3] = 0.0; vv[3] = 1.0;
        } else {                              /* LIMIT LINES: the same page transposed, two transverse bars */
            uu[0] = 0.0; vv[0] = 0.0; uu[1] = 0.0; vv[1] = L; uu[2] = 1.0; vv[2] = L; uu[3] = 1.0; vv[3] = 0.0;
        }
        em->sb = roam_ctx_sb_of(c, fr.px, fr.pz);
        em->road_y = fr.py;
        if (!fu_quad(em, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1, 1)) return 0;
        if (paint != 2) st->zebras++; else st->limit_lines++;
        if (x->way) st->xing_way_drawn++;
        if (td5_env_int("TD5RE_ROAM_FURN_DBG", 0, 0, 2) == 2) {
            int32_t ox, oz;
            roam_ctx_origin(c, NULL, NULL, &ox, &oz);
            TD5_LOG_I(LOG_TAG, "FURN_XING place %.0f %.0f paint %d lanes %d", fr.px + (double)ox, fr.pz + (double)oz, paint, lanes);
        }
    }
    return 1;
}

/* ============================================================================
 * SECTION: the tile
 * ========================================================================== */

/* ============================================================================
 * SECTION: [roam IE] motorway barriers, bridge parapets, piers, embankments
 *
 * "if it is a motorway the road should have barriers on the sides and in the middle" and
 * "bridges are a raised segment and nothing else" (Mariano, 2026-10-20).
 *
 * BARRIER ROADS: highway=motorway / motorway_link (TD5_GEO_RC_MOTORWAY) and the rural
 * dual-carriageway trunks (one-way trunk with maxspeed >= 80: Autovia 2, Ruta 6, Av. 44
 * past the city). The urban 60 km/h trunks (Circunvalacion 31/32/72/122, the boulevards,
 * Av. 44 in town) are city avenues with sidewalks and junctions every block: none. The rural
 * trunks keep their pavement on the outer kerb too (td5_roam_scenery.c pv_width gives every
 * non-motorway class one), so they take the median barrier (and deck parapets) only.
 * Along every KERB / DECK boundary edge of such a ribbon, and of a junction patch whose
 * arms are all barrier roads (merges, diverges, ramps):
 *   outer edge   steel W-beam guardrail on posts (TD5_TG_PAGE_R4_GUARDRAIL)
 *   median edge  New Jersey concrete (the LEFT kerb of a one-way carriageway; La Plata
 *                drives on the right)
 *   deck edge    a concrete parapet on a cantilevered ledge, with a fascia below
 * The road face stands FU_BAR_OFF outside the kerb line, past the wall code's kerb
 * overhang (ROAM_KERB_OVERHANG, 0.35 m): the invisible wall at the kerb (roam's roads-only
 * collision, racers and cops) stops a car before its body reaches the barrier, so nothing
 * clips. Traffic is held on its lane by the lane follower. A gap opens only where the
 * kerb itself opens: a junction mouth (a real link).
 * EMBANKMENTS: where a deck floor lifted a barrier road (ROAM_EF_APPROACH) more than
 * FU_EMB_MIN over the ground, a grass slope (1 : FU_EMB_SLOPE) from the barrier down to it.
 * PIERS: under every deck ribbon (ROAM_EF_DECK) more than FU_PIER_MIN over the ground, a
 * pier every FU_PIER_ROWS rows (one or two columns and a headstock), never on a road below
 * (moved half a row, else dropped); an ABUTMENT wall at each end of a deck run whose
 * neighbour row is not a deck.
 * One winding: the renderer does not cull (D3D12_CULL_MODE_NONE). Consecutive kerb segments that bend
 * less than FU_MERGE_DEG merge into pieces up to FU_MERGE_LEN, so a straight motorway costs a few quads
 * per 24 m.
 * ========================================================================== */

#define FU_BAR_OFF      (0.40 * FU_UPM)
#define FU_WB_Y0        (0.45 * FU_UPM)
#define FU_WB_Y1        (0.78 * FU_UPM)
#define FU_WB_T         (0.10 * FU_UPM)
#define FU_WB_POST_W    (0.15 * FU_UPM)
#define FU_WB_REPEAT    (4.0 * FU_UPM)
#define FU_NJ_W         (0.66 * FU_UPM)
#define FU_PAR_H        (1.00 * FU_UPM)
#define FU_PAR_T        (0.30 * FU_UPM)
#define FU_DECK_T       (1.20 * FU_UPM)
#define FU_EMB_MIN      (0.50 * FU_UPM)
#define FU_EMB_SLOPE    1.5
#define FU_PIER_MIN     (1.00 * FU_UPM)
#define FU_PIER_ROWS    7
#define FU_PIER_HW      (0.55 * FU_UPM)
#define FU_PIER_SINK    (0.40 * FU_UPM)
#define FU_WB_POST_EVERY (7.0 * FU_UPM)
#define FU_MERGE_DEG    1.5                /* kerb segments bending less than this merge into one piece */
#define FU_MERGE_LEN    (24.0 * FU_UPM)
#define FU_MERGE_DY     (0.03 * FU_UPM)
#define FU_MERGE_DEV    (0.04 * FU_UPM)
#define FU_QB_MAX       512
#define FU_END_JOIN     2                  /* TD5_ROAM_END_JOIN (td5_roam_world.h) */

/* quads of one mesh, by page (each page one command) */
typedef struct {
    double x[FU_QB_MAX * 4], y[FU_QB_MAX * 4], z[FU_QB_MAX * 4], u[FU_QB_MAX * 4], v[FU_QB_MAX * 4];
    int    page[FU_QB_MAX];
    int    nq;
} FuQB;

static void fu_qb_quad(FuQB *q, int page, const double p[4][3], const double uv[4][2])
{
    int k;
    if (q->nq >= FU_QB_MAX) return;            /* one winding: the renderer does not cull (D3D12_CULL_MODE_NONE) */
    for (k = 0; k < 4; k++) {
        const int o = q->nq * 4 + k;
        q->x[o] = p[k][0]; q->y[o] = p[k][1]; q->z[o] = p[k][2];
        q->u[o] = uv[k][0]; q->v[o] = uv[k][1];
    }
    q->page[q->nq++] = page;
}

/* write the quads as ONE mesh (sorted into page commands) in sub-block sb; census skipped
 * for `nocensus` (piers stand under their own deck, the census reads that as "on the road") */
static int fu_qb_flush(FuEm *em, FuQB *q, int sb, int nocensus)
{
    double *px, *py, *pz, *pu, *pv;
    int seg_page[16], seg_nq[16], nseg = 0, n = 0, i, k, pg, ok;
    int pages[16], np = 0;
    if (q->nq <= 0) return 1;
    px = (double *)malloc((size_t)q->nq * 4u * 5u * sizeof(double));
    if (!px) { em->oom = 1; return 0; }
    py = px + q->nq * 4; pz = py + q->nq * 4; pu = pz + q->nq * 4; pv = pu + q->nq * 4;
    for (i = 0; i < q->nq; i++) {
        for (k = 0; k < np; k++) if (pages[k] == q->page[i]) break;
        if (k == np && np < 16) pages[np++] = q->page[i];
    }
    for (pg = 0; pg < np; pg++) {
        int c = 0;
        for (i = 0; i < q->nq; i++) {
            if (q->page[i] != pages[pg]) continue;
            for (k = 0; k < 4; k++) {
                px[n] = q->x[i * 4 + k]; py[n] = q->y[i * 4 + k]; pz[n] = q->z[i * 4 + k];
                pu[n] = q->u[i * 4 + k]; pv[n] = q->v[i * 4 + k]; n++;
            }
            c++;
        }
        seg_page[nseg] = pages[pg]; seg_nq[nseg] = c; nseg++;
    }
    em->sb = sb;
    q->nq = 0;
    ok = fu_quad(em, px, py, pz, pu, pv, n, seg_page, seg_nq, nseg, nocensus);
    free(px);
    return ok;
}

/* barrier class of an edge record: 0 none, 1 barrier road */
static int fu_bar_road(const RoamEdgeRec *er)
{
    if (!er) return 0;
    if (er->klass == TD5_GEO_RC_MOTORWAY) return 1;
    if (er->klass == TD5_GEO_RC_TRUNK && (int8_t)er->oneway != 0 && er->maxspeed_kph >= 80) return 1;
    return 0;
}

/* a cluster (NODE record of tile t) whose arms are all barrier roads */
static int fu_bar_cluster(const TG_TileCtx *c, const RoamTile *t, uint32_t ni)
{
    const RoamNodeRec *nr;
    uint32_t k;
    if (ni >= t->nnode) return 0;
    nr = &t->node[ni];
    if (nr->narm == 0) return 0;
    for (k = 0; k < nr->narm; k++) {
        const RoamTile *et = NULL;
        const uint32_t ai = (uint32_t)nr->arm_first + k;
        const RoamEdgeRec *er;
        if (ai >= t->narm) return 0;
        er = fu_find_edge(c, t->arm[ai].edge_key, &et);
        if (!fu_bar_road(er)) return 0;
    }
    return 1;
}

/* one profile face swept along a->b: lateral offsets o0 -> o1, heights h0 -> h1 over the
 * kerb line (a, b: tile-local kerb points with their road y; n: outward unit normal) */
static void fu_sweep(FuQB *q, int page, const double a[3], const double b[3], const double n[2],
                     double o0, double h0, double o1, double h1, double u0, double u1, double v0, double v1)
{
    double p[4][3], uv[4][2];
    p[0][0] = a[0] + n[0] * o0; p[0][1] = a[1] + h0; p[0][2] = a[2] + n[1] * o0;
    p[1][0] = b[0] + n[0] * o0; p[1][1] = b[1] + h0; p[1][2] = b[2] + n[1] * o0;
    p[2][0] = b[0] + n[0] * o1; p[2][1] = b[1] + h1; p[2][2] = b[2] + n[1] * o1;
    p[3][0] = a[0] + n[0] * o1; p[3][1] = a[1] + h1; p[3][2] = a[2] + n[1] * o1;
    uv[0][0] = u0; uv[0][1] = v0; uv[1][0] = u0; uv[1][1] = v1;
    uv[2][0] = u1; uv[2][1] = v1; uv[3][0] = u1; uv[3][1] = v0;
    fu_qb_quad(q, page, (const double (*)[3])p, (const double (*)[2])uv);
}

/* a drivable primitive (ribbon quad) of the 3x3 under the footprint: mode 0 = one BELOW y_top (a road under a deck),
 * mode 1 = one at about y_top (the deck itself or a carriageway beside it) */
static int fu_road_near(const TG_TileCtx *c, const double *qx, const double *qz, int n, double y_top, int mode)
{
    int dx, dz, k;
    for (dz = -1; dz <= 1; dz++) for (dx = -1; dx <= 1; dx++) {
        const RoamTile *t = roam_ctx_tile(c, dx, dz);
        int32_t ctx, ctz, cox, coz;
        double sx, sz;
        uint32_t i;
        if (!t) continue;
        roam_ctx_origin(c, &ctx, &ctz, &cox, &coz);
        sx = (double)t->org_x - cox; sz = (double)t->org_z - coz;
        for (i = 0; i < t->nspan; i++) {
            const TD5_StripSpan *sp = &t->span[i];
            const uint32_t L = sp->left_vertex_index, R = sp->right_vertex_index, m = (uint32_t)(sp->pad_02[1] & 0x0Fu);
            double px[4], pz[4], ymax = -1e30, mnx = 1e30, mxx = -1e30, mnz = 1e30, mxz = -1e30;
            uint32_t vi[4];
            int j;
            if (m < 1 || L + m >= t->nvert || R + m >= t->nvert) continue;
            vi[0] = L; vi[1] = L + m; vi[2] = R + m; vi[3] = R;
            for (j = 0; j < 4; j++) {
                const TD5_StripVertex *v = &t->vert[vi[j]];
                px[j] = sx + sp->origin_x + v->x; pz[j] = sz + sp->origin_z + v->z;
                if ((double)sp->origin_y + v->y > ymax) ymax = (double)sp->origin_y + v->y;
                mnx = fmin(mnx, px[j]); mxx = fmax(mxx, px[j]);
                mnz = fmin(mnz, pz[j]); mxz = fmax(mxz, pz[j]);
            }
            if (mode == 0 && ymax > y_top - 2.5 * FU_UPM) continue;          /* not below: the deck itself */
            if (mode == 1 && fabs(ymax - y_top) > 2.0 * FU_UPM) continue;    /* another level */
            for (k = 0; k < n; k++) {
                int a2, inside = 1;
                if (qx[k] < mnx || qx[k] > mxx || qz[k] < mnz || qz[k] > mxz) continue;
                for (a2 = 0; a2 < 4 && inside; a2++) {
                    const int b2 = (a2 + 1) & 3;
                    const double cr = (px[b2] - px[a2]) * (qz[k] - pz[a2]) - (pz[b2] - pz[a2]) * (qx[k] - px[a2]);
                    if (a2 == 0 && cr < 0.0) inside = -1;            /* winding of this quad */
                    if ((inside == -1 && cr > 0.0) || (inside == 1 && cr < 0.0)) inside = 0;
                }
                if (inside) return 1;
            }
        }
    }
    return 0;
}

typedef struct {
    double a[3], b[3], n[2], len;      /* kerb piece (tile-local, road y), outward normal */
    int    kind;                       /* 1 guardrail, 2 New Jersey, 3 deck parapet */
    int    emb;                        /* an approach: embankment allowed */
    double dev;                        /* running bound on the merged joints' offset from the chord */
} FuBarSeg;

static double fu_bar_width(int kind)
{
    return kind == 2 ? FU_NJ_W : (kind == 3 ? FU_PAR_T : FU_WB_T + FU_WB_POST_W);
}

/* 1 when the barrier footprint of segment s (its own width past FU_BAR_OFF) lies on a drivable primitive */
static int fu_bar_on_road(const TG_TileCtx *c, const FuBarSeg *s)
{
    const double o = FU_BAR_OFF, w = fu_bar_width(s->kind);
    double fx[4], fz[4];
    fx[0] = s->a[0] + s->n[0] * (o + 2.0); fz[0] = s->a[2] + s->n[1] * (o + 2.0);
    fx[1] = s->b[0] + s->n[0] * (o + 2.0); fz[1] = s->b[2] + s->n[1] * (o + 2.0);
    fx[2] = s->b[0] + s->n[0] * (o + w);   fz[2] = s->b[2] + s->n[1] * (o + w);
    fx[3] = s->a[0] + s->n[0] * (o + w);   fz[3] = s->a[2] + s->n[1] * (o + w);
    return roam_ctx_on_road(c, fx, fz, 4);
}

/* a deck parapet's footprint on a road at the deck's own level (the road UNDER the deck is not in the way) */
static int fu_par_on_road(const TG_TileCtx *c, const FuBarSeg *s)
{
    const double o0 = 0.05 * FU_UPM, o1 = FU_BAR_OFF + FU_PAR_T;
    double fx[4], fz[4];
    fx[0] = s->a[0] + s->n[0] * o0; fz[0] = s->a[2] + s->n[1] * o0;
    fx[1] = s->b[0] + s->n[0] * o0; fz[1] = s->b[2] + s->n[1] * o0;
    fx[2] = s->b[0] + s->n[0] * o1; fz[2] = s->b[2] + s->n[1] * o1;
    fx[3] = s->a[0] + s->n[0] * o1; fz[3] = s->a[2] + s->n[1] * o1;
    return fu_road_near(c, fx, fz, 4, 0.5 * (s->a[1] + s->b[1]), 1);
}

/* one barrier piece (one or more merged kerb segments) */
static int fu_bar_seg_emit(FuEm *em, FuQB *q, const FuBarSeg *s, double v0)
{
    TG_TileCtx *c = em->c;
    const double v1 = v0 + s->len / FU_WB_REPEAT;
    const double o = FU_BAR_OFF, w = fu_bar_width(s->kind);
    em->road_y = s->a[1] < s->b[1] ? s->a[1] : s->b[1];
    switch (s->kind) {
    case 1: {   /* W-beam rail on posts every FU_WB_POST_EVERY */
        double t[2], d;
        const double l = s->len > 1e-6 ? s->len : 1.0;
        fu_sweep(q, TD5_TG_PAGE_R4_GUARDRAIL, s->a, s->b, s->n, o, FU_WB_Y0, o, FU_WB_Y1, 0.0, 1.0, v0, v1);
        t[0] = (s->b[0] - s->a[0]) / l; t[1] = (s->b[2] - s->a[2]) / l;
        for (d = 0.5 * FU_UPM; d + FU_WB_POST_W < l; d += FU_WB_POST_EVERY) {
            double pa[3], pb[3];
            const double f0 = d / l, f1 = (d + FU_WB_POST_W) / l;
            pa[0] = s->a[0] + t[0] * d; pa[1] = s->a[1] + (s->b[1] - s->a[1]) * f0; pa[2] = s->a[2] + t[1] * d;
            pb[0] = s->a[0] + t[0] * (d + FU_WB_POST_W); pb[1] = s->a[1] + (s->b[1] - s->a[1]) * f1; pb[2] = s->a[2] + t[1] * (d + FU_WB_POST_W);
            fu_sweep(q, TD5_TG_PAGE_R11_SIGN_POST, pa, pb, s->n, o + FU_WB_T, -FU_PIER_SINK, o + FU_WB_T, FU_WB_Y1, 0.0, 1.0, 0.0, 1.0);
        }
        em->st->bar_guard_m += s->len / FU_UPM;
        break;
    }
    case 2: {   /* New Jersey: road face (two slopes), crown, back */
        const int pg = TD5_TG_PAGE_R4_PIER;
        fu_sweep(q, pg, s->a, s->b, s->n, o, 0.0, o + 0.20 * FU_UPM, 0.33 * FU_UPM, 0.0, 0.3, v0, v1);
        fu_sweep(q, pg, s->a, s->b, s->n, o + 0.20 * FU_UPM, 0.33 * FU_UPM, o + 0.25 * FU_UPM, 0.81 * FU_UPM, 0.3, 0.6, v0, v1);
        fu_sweep(q, pg, s->a, s->b, s->n, o + 0.25 * FU_UPM, 0.81 * FU_UPM, o + 0.41 * FU_UPM, 0.81 * FU_UPM, 0.6, 0.7, v0, v1);
        fu_sweep(q, pg, s->a, s->b, s->n, o + 0.41 * FU_UPM, 0.81 * FU_UPM, o + w, 0.0, 0.7, 1.0, v0, v1);
        em->st->bar_nj_m += s->len / FU_UPM;
        break;
    }
    default: {  /* deck: ledge, parapet face, crown, outer face + fascia down to the soffit */
        const int pg = TD5_TG_PAGE_R4_PIER;
        fu_sweep(q, pg, s->a, s->b, s->n, 0.02 * FU_UPM, 0.0, o, 0.0, 0.0, 0.3, v0, v1);
        fu_sweep(q, pg, s->a, s->b, s->n, o, 0.0, o, FU_PAR_H, 0.3, 0.6, v0, v1);
        fu_sweep(q, pg, s->a, s->b, s->n, o, FU_PAR_H, o + FU_PAR_T, FU_PAR_H, 0.6, 0.7, v0, v1);
        fu_sweep(q, pg, s->a, s->b, s->n, o + FU_PAR_T, FU_PAR_H, o + FU_PAR_T, -FU_DECK_T, 0.7, 1.0, v0, v1);
        em->st->bar_parapet_m += s->len / FU_UPM;
        break;
    }
    }
    /* embankment: a grass slope from behind the barrier down to the ground (not under another road) */
    if (s->emb && s->kind != 3) {
        const double ob = o + w;
        double ga, gb, ha, hb, fa, fb;
        ga = roam_ctx_ground(c, s->a[0] + s->n[0] * ob, s->a[2] + s->n[1] * ob);
        gb = roam_ctx_ground(c, s->b[0] + s->n[0] * ob, s->b[2] + s->n[1] * ob);
        ha = s->a[1] - ga; hb = s->b[1] - gb;
        if (ha > FU_EMB_MIN || hb > FU_EMB_MIN) {
            double p[4][3], uv[4][2], fx[4], fz[4];
            const double sink = 0.3 * FU_UPM;
            int k;
            if (ha < 0.0) ha = 0.0;
            if (hb < 0.0) hb = 0.0;
            fa = ob + FU_EMB_SLOPE * (ha + sink); fb = ob + FU_EMB_SLOPE * (hb + sink);
            p[0][0] = s->a[0] + s->n[0] * ob; p[0][1] = s->a[1] - 0.05 * FU_UPM; p[0][2] = s->a[2] + s->n[1] * ob;
            p[1][0] = s->b[0] + s->n[0] * ob; p[1][1] = s->b[1] - 0.05 * FU_UPM; p[1][2] = s->b[2] + s->n[1] * ob;
            p[2][0] = s->b[0] + s->n[0] * fb; p[2][2] = s->b[2] + s->n[1] * fb;
            p[3][0] = s->a[0] + s->n[0] * fa; p[3][2] = s->a[2] + s->n[1] * fa;
            for (k = 0; k < 4; k++) { fx[k] = p[k][0]; fz[k] = p[k][2]; }
            if (roam_ctx_on_road(c, fx, fz, 4)) { em->st->emb_drop_road++; }
            else {
                p[2][1] = roam_ctx_ground(c, p[2][0], p[2][2]) - sink;
                p[3][1] = roam_ctx_ground(c, p[3][0], p[3][2]) - sink;
                uv[0][0] = 0.0; uv[0][1] = v0; uv[1][0] = 0.0; uv[1][1] = v1;
                uv[2][0] = 1.0; uv[2][1] = v1; uv[3][0] = 1.0; uv[3][1] = v0;
                fu_qb_quad(q, TD5_TG_PAGE_R3_BLOCK + 0, (const double (*)[3])p, (const double (*)[2])uv);
                /* the shoulder between the kerb and the barrier foot, at road level */
                fu_sweep(q, TD5_TG_PAGE_R4_PIER, s->a, s->b, s->n, 0.02 * FU_UPM, -0.03 * FU_UPM, o, -0.03 * FU_UPM, 0.0, 0.3, v0, v1);
                em->st->emb_m += s->len / FU_UPM;
            }
        }
    }
    return 1;
}

/* kerb segment of boundary edge be: tile-local ends with road y, outward normal (away from
 * the primitive's own inside point `ix, iz`) */
static int fu_bedge_seg(const RoamTile *t, const RoamBEdge *be, int32_t ox, int32_t oy, int32_t oz,
                        double ix, double iz, FuBarSeg *s)
{
    const TD5_StripVertex *va = &t->vert[be->v[0]], *vb = &t->vert[be->v[1]];
    double dx, dz, l, nx, nz;
    s->a[0] = (double)ox + va->x; s->a[1] = (double)oy + va->y; s->a[2] = (double)oz + va->z;
    s->b[0] = (double)ox + vb->x; s->b[1] = (double)oy + vb->y; s->b[2] = (double)oz + vb->z;
    dx = s->b[0] - s->a[0]; dz = s->b[2] - s->a[2];
    l = hypot(dx, dz);
    if (!(l > 1.0)) return 0;
    nx = dz / l; nz = -dx / l;
    if (nx * (ix - s->a[0]) + nz * (iz - s->a[2]) > 0.0) { nx = -nx; nz = -nz; }
    s->n[0] = nx; s->n[1] = nz; s->len = l; s->dev = 0.0;
    return 1;
}

/* orient s to run the same way as the piece it may extend (its a at the piece's b) */
static void fu_seg_flip(FuBarSeg *s)
{
    double t[3];
    memcpy(t, s->a, sizeof t); memcpy(s->a, s->b, sizeof t); memcpy(s->b, t, sizeof t);
}

/* extend piece p by segment s when they join end to start, keep the kind, bend under FU_MERGE_DEG,
 * stay under FU_MERGE_LEN and stay within FU_MERGE_DY of a straight grade. 1 = merged. */
static int fu_seg_merge(FuBarSeg *p, FuBarSeg *s)
{
    double ux, uz, vx, vz, l, lp, f, ym;
    if (p->kind != s->kind || p->emb != s->emb) return 0;
    if (hypot(s->a[0] - p->b[0], s->a[2] - p->b[2]) > 2.0) {
        if (hypot(s->b[0] - p->b[0], s->b[2] - p->b[2]) > 2.0) return 0;
        fu_seg_flip(s);
    }
    lp = p->len;
    ux = (p->b[0] - p->a[0]) / lp; uz = (p->b[2] - p->a[2]) / lp;
    vx = (s->b[0] - s->a[0]) / s->len; vz = (s->b[2] - s->a[2]) / s->len;
    if (ux * vx + uz * vz < cos(FU_MERGE_DEG * 3.14159265358979323846 / 180.0)) return 0;
    l = hypot(s->b[0] - p->a[0], s->b[2] - p->a[2]);
    if (l > FU_MERGE_LEN) return 0;
    f = lp / l;
    ym = p->a[1] + (s->b[1] - p->a[1]) * f;
    if (fabs(ym - p->b[1]) > FU_MERGE_DY) return 0;
    {
        /* the joint (and, by the running bound, every earlier one) within FU_MERGE_DEV of the new chord */
        const double cx = (s->b[0] - p->a[0]) / l, cz = (s->b[2] - p->a[2]) / l;
        const double dev = fabs((p->b[0] - p->a[0]) * cz - (p->b[2] - p->a[2]) * cx);
        if (dev + p->dev > FU_MERGE_DEV) return 0;
        p->dev += dev;
    }
    memcpy(p->b, s->b, sizeof p->b);
    p->len = l;
    {
        const double nx = (p->b[2] - p->a[2]) / l, nz = -(p->b[0] - p->a[0]) / l;
        if (nx * p->n[0] + nz * p->n[1] < 0.0) { p->n[0] = -nx; p->n[1] = -nz; } else { p->n[0] = nx; p->n[1] = nz; }
    }
    return 1;
}

static int fu_emit_barriers(FuEm *em)
{
    TG_TileCtx *c = em->c;
    const RoamTile *t = roam_ctx_tile(c, 0, 0);
    int32_t *sbe[2] = { NULL, NULL };   /* per span: its boundary edge on side 0 (j = 0) / 1 (j = lanes) */
    uint32_t e, i, k;
    FuQB *q = NULL;
    int ok = 0;
    if (!t || t->nspan == 0) return 1;
    sbe[0] = (int32_t *)malloc((size_t)t->nspan * sizeof(int32_t));
    sbe[1] = (int32_t *)malloc((size_t)t->nspan * sizeof(int32_t));
    q = (FuQB *)malloc(sizeof *q);
    if (!sbe[0] || !sbe[1] || !q) goto done;
    q->nq = 0;
    for (i = 0; i < t->nspan; i++) sbe[0][i] = sbe[1][i] = -1;
    for (k = 0; k < t->nbedge; k++) {
        const RoamBEdge *be = &t->bedge[k];
        const TD5_StripSpan *sp;
        uint32_t L, R, n;
        if (be->prim & ROAM_PRIM_TRI || be->prim >= t->nspan) continue;
        if (be->kind != ROAM_BEDGE_KERB && be->kind != ROAM_BEDGE_DECK) continue;
        sp = &t->span[be->prim];
        n = (uint32_t)(sp->pad_02[1] & 0x0Fu);
        L = sp->left_vertex_index; R = sp->right_vertex_index;
        if ((be->v[0] == L && be->v[1] == R) || (be->v[0] == R && be->v[1] == L)) sbe[0][be->prim] = (int32_t)k;
        else if ((be->v[0] == L + n && be->v[1] == R + n) || (be->v[0] == R + n && be->v[1] == L + n)) sbe[1][be->prim] = (int32_t)k;
    }
    /* ribbons: per edge record, per side; consecutive kerb segments merge into pieces */
    for (e = 0; e < t->nedge; e++) {
        const RoamEdgeRec *er = &t->edge[e];
        int side;
        if (!fu_bar_road(er)) continue;
        for (side = 0; side < 2; side++) {
            /* the median side of a one-way carriageway: the LEFT kerb of the travel direction.
             * j = 0 lies at +lateral (tz, -tx) of the row direction = its LEFT */
            const int ow = (int8_t)er->oneway;
            const int median = (ow > 0 && side == 0) || (ow < 0 && side == 1);
            const int deck = (er->flags & ROAM_EF_DECK) != 0;   /* parapets: checked by fu_par_on_road instead */
            /* an expressway trunk keeps its pavement outside: New Jersey on the median kerb (and parapets on a deck) only */
            if (er->klass != TD5_GEO_RC_MOTORWAY && !median && !deck) continue;
            double v0 = 0.0;
            FuBarSeg cur;
            int open = 0;
            memset(&cur, 0, sizeof cur);
            for (i = 0; i <= er->span_count; i++) {
                const uint32_t si = (uint32_t)er->span_first + i;
                FuBarSeg s;
                int have = 0;
                if (i < er->span_count && si < t->nspan && sbe[side][si] >= 0) {
                    const TD5_StripSpan *sp = &t->span[si];
                    const uint32_t n = (uint32_t)(sp->pad_02[1] & 0x0Fu), L = sp->left_vertex_index;
                    const TD5_StripVertex *vo = &t->vert[side ? L : L + n];
                    const double ix = (double)sp->origin_x + vo->x, iz = (double)sp->origin_z + vo->z;
                    if (fu_bedge_seg(t, &t->bedge[sbe[side][si]], sp->origin_x, sp->origin_y, sp->origin_z, ix, iz, &s)) {
                        s.kind = (t->bedge[sbe[side][si]].kind == ROAM_BEDGE_DECK || (er->flags & ROAM_EF_DECK)) ? 3 : (median ? 2 : 1);
                        s.emb = (er->flags & ROAM_EF_APPROACH) != 0;
                        if (s.kind != 3 ? fu_bar_on_road(c, &s) : fu_par_on_road(c, &s)) em->st->bar_drop_road++;
                        else have = 1;
                    }
                }
                if (open && have && fu_seg_merge(&cur, &s)) continue;
                if (open) {
                    if (!fu_bar_seg_emit(em, q, &cur, v0)) goto done;
                    v0 += cur.len / FU_WB_REPEAT;
                    if (q->nq + 64 > FU_QB_MAX && !fu_qb_flush(em, q, roam_ctx_sb_of(c, cur.a[0], cur.a[2]), deck)) goto done;
                    open = 0;
                }
                if (have) { cur = s; open = 1; }
            }
            if (q->nq > 0 && !fu_qb_flush(em, q, roam_ctx_sb_of(c, cur.a[0], cur.a[2]), deck)) goto done;
        }
    }
    /* junction patches of barrier roads: a guardrail round the kerb outline */
    for (k = 0; k < t->nbedge; k++) {
        const RoamBEdge *be = &t->bedge[k];
        const uint32_t ix = (uint32_t)(be->prim & 0x7FFFu);
        const RoamTri *tr;
        FuBarSeg s;
        double cx = 0.0, cz = 0.0;
        int j;
        if (!(be->prim & ROAM_PRIM_TRI) || ix >= t->ntri || be->kind != ROAM_BEDGE_KERB) continue;
        tr = &t->tri[ix];
        if (!fu_bar_cluster(c, t, tr->patch)) continue;
        for (j = 0; j < 3; j++) { cx += (double)tr->ox + t->vert[tr->v[j]].x; cz += (double)tr->oz + t->vert[tr->v[j]].z; }
        cx /= 3.0; cz /= 3.0;
        if (!fu_bedge_seg(t, be, tr->ox, tr->oy, tr->oz, cx, cz, &s)) continue;
        s.kind = 1; s.emb = 0;
        if (fu_bar_on_road(c, &s)) { em->st->bar_drop_road++; continue; }
        if (!fu_bar_seg_emit(em, q, &s, 0.0)) goto done;
        em->st->bar_patch_edges++;
        if (q->nq + 64 > FU_QB_MAX && !fu_qb_flush(em, q, roam_ctx_sb_of(c, s.a[0], s.a[2]), 0)) goto done;
    }
    if (q->nq > 0 && !fu_qb_flush(em, q, roam_ctx_sb_of(c, 0.0, 0.0), 0)) goto done;
    ok = 1;
done:
    free(sbe[0]); free(sbe[1]); free(q);
    return ok;
}

/* one pier column (a box from under the ground to the soffit), unless it stands on a road below */
static int fu_pier_box(FuEm *em, FuQB *q, double x, double z, double tx, double tz, double soffit, double deck_y)
{
    TG_TileCtx *c = em->c;
    const double h = FU_PIER_HW;
    double fx[4], fz[4], g, p[4][3], uv[4][2];
    int k;
    fx[0] = x - tx * h - tz * h; fz[0] = z - tz * h + tx * h;
    fx[1] = x + tx * h - tz * h; fz[1] = z + tz * h + tx * h;
    fx[2] = x + tx * h + tz * h; fz[2] = z + tz * h - tx * h;
    fx[3] = x - tx * h + tz * h; fz[3] = z - tz * h - tx * h;
    if (fu_road_near(c, fx, fz, 4, deck_y, 0)) return 0;
    g = roam_ctx_ground(c, x, z) - FU_PIER_SINK;
    if (soffit - g < FU_PIER_MIN) return 0;
    for (k = 0; k < 4; k++) {
        const int k2 = (k + 1) & 3;
        p[0][0] = fx[k];  p[0][1] = g;      p[0][2] = fz[k];
        p[1][0] = fx[k2]; p[1][1] = g;      p[1][2] = fz[k2];
        p[2][0] = fx[k2]; p[2][1] = soffit; p[2][2] = fz[k2];
        p[3][0] = fx[k];  p[3][1] = soffit; p[3][2] = fz[k];
        uv[0][0] = 0.0; uv[0][1] = (soffit - g) / (2.0 * FU_UPM); uv[1][0] = 1.0; uv[1][1] = uv[0][1];
        uv[2][0] = 1.0; uv[2][1] = 0.0; uv[3][0] = 0.0; uv[3][1] = 0.0;
        fu_qb_quad(q, TD5_TG_PAGE_R4_PIER, (const double (*)[3])p, (const double (*)[2])uv);
    }
    return 1;
}

/* The deck's end row in span sp: 0 = its left (first) row, 1 = its right row, -1 none. An end
 * is a JOIN node (decoded from its 10-unit key) where the other way is not a deck. */
static int fu_deck_end_row(const TG_TileCtx *c, const RoamTile *t, const RoamEdgeRec *er, const TD5_StripSpan *sp)
{
    int32_t ctx, ctz, cox, coz;
    int k, r;
    const uint32_t n = (uint32_t)(sp->pad_02[1] & 0x0Fu);
    roam_ctx_origin(c, &ctx, &ctz, &cox, &coz);
    for (k = 0; k < 2; k++) {
        const int64_t key = er->end_key[k];
        const double nx = (double)(int32_t)(key >> 32) * 10.0 - (double)cox, nz = (double)(int32_t)(uint32_t)key * 10.0 - (double)coz;
        int dx, dz, other_deck = 0;
        if (er->end_kind[k] != FU_END_JOIN) continue;
        for (r = 0; r < 2; r++) {
            const uint32_t base = r ? sp->right_vertex_index : sp->left_vertex_index;
            const TD5_StripVertex *a = &t->vert[base], *b = &t->vert[base + n];
            const double mx = sp->origin_x + 0.5 * (a->x + b->x), mz = sp->origin_z + 0.5 * (a->z + b->z);
            if (base + n >= t->nvert || hypot(mx - nx, mz - nz) > 1.0 * FU_UPM) continue;
            for (dz = -1; dz <= 1 && !other_deck; dz++) for (dx = -1; dx <= 1 && !other_deck; dx++) {
                const RoamTile *o = roam_ctx_tile(c, dx, dz);
                uint32_t e;
                if (!o) continue;
                for (e = 0; e < o->nedge; e++) {
                    const RoamEdgeRec *q = &o->edge[e];
                    if (q->key == er->key || !(q->flags & ROAM_EF_DECK)) continue;
                    if ((q->end_kind[0] == FU_END_JOIN && q->end_key[0] == key) ||
                        (q->end_kind[1] == FU_END_JOIN && q->end_key[1] == key)) { other_deck = 1; break; }
                }
            }
            if (!other_deck) return r;
        }
    }
    return -1;
}

static int fu_emit_piers(FuEm *em)
{
    TG_TileCtx *c = em->c;
    const RoamTile *t = roam_ctx_tile(c, 0, 0);
    FuQB *q;
    uint32_t e, i;
    if (!t) return 1;
    q = (FuQB *)malloc(sizeof *q);
    if (!q) return 0;
    q->nq = 0;
    for (e = 0; e < t->nedge; e++) {
        const RoamEdgeRec *er = &t->edge[e];
        if (!(er->flags & ROAM_EF_DECK)) continue;
        for (i = 0; i < er->span_count; i++) {
            const uint32_t si = (uint32_t)er->span_first + i, row = (uint32_t)er->row0 + i;
            const TD5_StripSpan *sp;
            const TD5_StripVertex *l0, *ln, *r0, *rn;
            uint32_t L, R, n;
            double ax, az, bx, bz, y, hw, tx, tz, len, cx, cz, soffit, g;
            int abut = -1, cols, kk, placed = 0;
            if (si >= t->nspan) break;
            sp = &t->span[si];
            abut = fu_deck_end_row(c, t, er, sp);
            if (abut < 0 && (row % FU_PIER_ROWS) != 3) continue;
            n = (uint32_t)(sp->pad_02[1] & 0x0Fu);
            L = sp->left_vertex_index; R = sp->right_vertex_index;
            if (n < 1 || L + n >= t->nvert || R + n >= t->nvert) continue;
            l0 = &t->vert[L]; ln = &t->vert[L + n]; r0 = &t->vert[R]; rn = &t->vert[R + n];
            ax = sp->origin_x + 0.5 * (l0->x + ln->x); az = sp->origin_z + 0.5 * (l0->z + ln->z);
            bx = sp->origin_x + 0.5 * (r0->x + rn->x); bz = sp->origin_z + 0.5 * (r0->z + rn->z);
            y = sp->origin_y + 0.25 * (l0->y + ln->y + r0->y + rn->y);
            hw = 0.25 * (hypot(ln->x - l0->x, ln->z - l0->z) + hypot(rn->x - r0->x, rn->z - r0->z));
            len = hypot(bx - ax, bz - az);
            if (!(len > 1e-6)) continue;
            tx = (bx - ax) / len; tz = (bz - az) / len;
            cx = 0.5 * (ax + bx); cz = 0.5 * (az + bz);
            soffit = y - FU_DECK_T;
            g = roam_ctx_ground(c, cx, cz);
            if (soffit - g < FU_PIER_MIN) continue;
            em->road_y = y;
            if (abut >= 0) {
                /* an abutment wall across the deck at its end row (from under the ground to the soffit) */
                const double ex = abut == 0 ? ax : bx, ez = abut == 0 ? az : bz;
                double fx[2], fz[2], p[4][3], uv[4][2];
                const double lx = tz, lz = -tx, hh = hw + FU_PAR_T + FU_BAR_OFF;
                fx[0] = ex + lx * hh; fz[0] = ez + lz * hh;
                fx[1] = ex - lx * hh; fz[1] = ez - lz * hh;
                p[0][0] = fx[0]; p[0][1] = roam_ctx_ground(c, fx[0], fz[0]) - FU_PIER_SINK; p[0][2] = fz[0];
                p[1][0] = fx[1]; p[1][1] = roam_ctx_ground(c, fx[1], fz[1]) - FU_PIER_SINK; p[1][2] = fz[1];
                p[2][0] = fx[1]; p[2][1] = soffit; p[2][2] = fz[1];
                p[3][0] = fx[0]; p[3][1] = soffit; p[3][2] = fz[0];
                uv[0][0] = 0.0; uv[0][1] = 1.0; uv[1][0] = 2.0; uv[1][1] = 1.0; uv[2][0] = 2.0; uv[2][1] = 0.0; uv[3][0] = 0.0; uv[3][1] = 0.0;
                fu_qb_quad(q, TD5_TG_PAGE_R4_PIER, (const double (*)[3])p, (const double (*)[2])uv);
                em->st->abutments++;
                if (!fu_qb_flush(em, q, roam_ctx_sb_of(c, cx, cz), 1)) { free(q); return 0; }
                if ((row % FU_PIER_ROWS) != 3) continue;
            }
            /* columns: two under a wide deck, one under a narrow one; a column that would stand on a
             * road below moves half a row along, else it is dropped */
            cols = (2.0 * hw > 9.0 * FU_UPM) ? 2 : 1;
            for (kk = 0; kk < cols; kk++) {
                const double off = cols == 2 ? (kk ? 1.0 : -1.0) * (hw - 1.6 * FU_UPM) : 0.0;
                const double px = cx + tz * off, pz = cz - tx * off;
                int tries;
                for (tries = 0; tries < 3; tries++) {
                    const double sh = tries == 0 ? 0.0 : (tries == 1 ? 0.5 : -0.5) * ROAM_TILE_UNITS / 64.0;
                    if (fu_pier_box(em, q, px + tx * sh, pz + tz * sh, tx, tz, soffit, y)) { placed++; break; }
                }
                if (tries == 3) em->st->pier_drop_road++;
            }
            if (placed) {
                /* the headstock under the soffit, across the deck */
                double p[4][3], uv[4][2];
                const double lx = tz, lz = -tx, hh = hw, hd = 0.6 * FU_UPM, y1 = soffit, y0 = soffit - 0.9 * FU_UPM;
                int f;
                for (f = 0; f < 2; f++) {
                    const double sx2 = f ? hd : -hd;
                    p[0][0] = cx + lx * hh + tx * sx2; p[0][1] = y0; p[0][2] = cz + lz * hh + tz * sx2;
                    p[1][0] = cx - lx * hh + tx * sx2; p[1][1] = y0; p[1][2] = cz - lz * hh + tz * sx2;
                    p[2][0] = cx - lx * hh + tx * sx2; p[2][1] = y1; p[2][2] = cz - lz * hh + tz * sx2;
                    p[3][0] = cx + lx * hh + tx * sx2; p[3][1] = y1; p[3][2] = cz + lz * hh + tz * sx2;
                    uv[0][0] = 0.0; uv[0][1] = 0.5; uv[1][0] = 2.0; uv[1][1] = 0.5; uv[2][0] = 2.0; uv[2][1] = 0.0; uv[3][0] = 0.0; uv[3][1] = 0.0;
                    fu_qb_quad(q, TD5_TG_PAGE_R4_PIER, (const double (*)[3])p, (const double (*)[2])uv);
                }
                p[0][0] = cx + lx * hh - tx * hd; p[0][1] = y0; p[0][2] = cz + lz * hh - tz * hd;
                p[1][0] = cx - lx * hh - tx * hd; p[1][1] = y0; p[1][2] = cz - lz * hh - tz * hd;
                p[2][0] = cx - lx * hh + tx * hd; p[2][1] = y0; p[2][2] = cz - lz * hh + tz * hd;
                p[3][0] = cx + lx * hh + tx * hd; p[3][1] = y0; p[3][2] = cz + lz * hh + tz * hd;
                fu_qb_quad(q, TD5_TG_PAGE_R4_PIER, (const double (*)[3])p, (const double (*)[2])uv);
                em->st->piers++;
                em->st->pier_cols += placed;
            }
            if (q->nq > 0 && !fu_qb_flush(em, q, roam_ctx_sb_of(c, cx, cz), 1)) { free(q); return 0; }
        }
    }
    free(q);
    return 1;
}

int td5_roam_furn_emit(TD5_RoamFurnSource *f, TG_TileCtx *c, TD5_RoamFurnStats *st, float **lamps, uint32_t *nlamps)
{
    FuEm em;
    FuLamps lp;
    FuXing *xs = NULL;
    FuStop *bs = NULL;
    int nx = 0, nb = 0, ok = 1;
    memset(&em, 0, sizeof em);
    memset(&lp, 0, sizeof lp);
    em.c = c; em.st = st; em.f = f;
    if (!f || !c || !st) return 1;
    if (f->on_signals && !fu_emit_signals(&em)) ok = 0;
    if (ok && f->on_lamps && !fu_emit_lamps(&em, f, &lp)) ok = 0;
    if (ok && f->on_barriers && !fu_emit_barriers(&em)) ok = 0;     /* [roam IE] */
    if (ok && f->on_piers && !fu_emit_piers(&em)) ok = 0;           /* [roam IE] */
    if (ok && (f->on_stops || f->on_xings)) {
        if (!fu_collect_nodes(&em, f, &xs, &nx, &bs, &nb)) ok = 0;
        if (ok && f->on_stops && !fu_emit_stops(&em, bs, nb)) ok = 0;
        if (ok && f->on_xings && !fu_emit_xings(&em, f, xs, nx)) ok = 0;
    }
    free(xs); free(bs);
    if (!ok) { free(lp.p); return 0; }
    *lamps = lp.p; *nlamps = lp.n;
    return 1;
}

/* ============================================================================
 * SECTION: frame (main thread, per frame)
 * ========================================================================== */

#define FU_FRAME_LAMP_CAP   256
#define FU_FRAME_LAMP_R     (140.0f * 430.0f)

void td5_roam_furn_frame(void)
{
    static float lamp[FU_FRAME_LAMP_CAP][3];
    static int s_log = -1;
    TD5_Actor *p;
    double ss;
    int n, i;
    if (!td5_roam_active()) return;
    /* the lamp cycle follows the SESSION: sim ticks at 30 Hz */
    ss = (double)g_td5.simulation_tick_counter / 30.0;
    td5_geo_signal_set_session_time(ss);
    if (s_log < 0) s_log = td5_env_int("TD5RE_ROAM_FURN_LOG", 0, 0, 3600);
    if (s_log > 0) {
        static int s_last = -1;
        const int bucket = (int)(ss / (double)s_log);
        if (bucket != s_last) {
            s_last = bucket;
            TD5_LOG_I(LOG_TAG, "ROAM_SIGNAL session_t=%.3f tick=%d phase_main=%d phase_cross=%d render_phase=%d countdown=%d", ss,
                      (int)g_td5.simulation_tick_counter, td5_geo_signal_phase_at(ss, 0), td5_geo_signal_phase_at(ss, 1),
                      td5_geo_signal_lamp_phase_now(), td5_geo_signal_countdown(NULL));
        }
    }
    /* street lamps: the nearest resident lanterns become the light registry; the light system promotes the
     * nearest few (TD5RE_LAMP_COUNT) to real lights, gated on the environment-dark verdict */
    if (!td5_light_street_lights() && !td5_rt_active() && !td5_roam_furn_night()) return;
    p = td5_game_get_actor(0);
    if (!p) return;
    td5_light_lamps_reset();
    n = td5_roam_render_lamps((float)p->world_pos.x * (1.0f / 256.0f), (float)p->world_pos.z * (1.0f / 256.0f), FU_FRAME_LAMP_R,
                              lamp, FU_FRAME_LAMP_CAP);
    for (i = 0; i < n; i++) td5_light_lamps_add(lamp[i][0], lamp[i][1], lamp[i][2]);
}
