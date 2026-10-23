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
#include "td5_page_map.h"            /* [roam PE] ROAM_SHOP_PAGE_BASE: the shop-name bank */
#include "td5_font.h"                /* [roam PE] td5_font_plate_raster                 */
#include "td5_asset.h"               /* [roam PE] td5_asset_set_page_transparency        */

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
    double clear;                          /* [roam PE] deck underside over gy (SH_CAN_CLEAR for a forecourt-point site) */
    unsigned int col;
    int islands;                           /* the pump islands are clear too */
    int site;                              /* [roam PE] the fuel site of a canopy drawn on its footprint, -1 = LB's */
    int posts;                             /* bit k: post k stands on clear ground (drawn) */
    int sb;
} ShCanopy;

struct TD5_RoamShopSource {
    char   place[64];
    int    loaded, use_pois, awnings, canopy, log;
    int    canopy_fp, abut, inf_guard, names;  /* [roam PE] knobs (td5_roam_shops.h) */
    char   **bname;                        /* [roam PE] buildings[].name, parallel to bid (NULL = unnamed) */
    double *fbld;                          /* [roam PE] each fuel site's joined footprint id (fhas_bld[i] = 1) */
    unsigned char *fhas_bld;
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
int td5_roam_shops_abut(const TD5_RoamShopSource *s) { return s && s->abut; }
int td5_roam_shops_infill_guard(const TD5_RoamShopSource *s) { return s && s->inf_guard; }
int td5_roam_shops_canopy_fp_on(const TD5_RoamShopSource *s) { return s && s->canopy && s->canopy_fp; }

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
    s->canopy_fp = td5_env_int("TD5RE_ROAM_SHOP_CANOPY_FP", 1, 0, 1);
    s->abut      = td5_env_int("TD5RE_ROAM_SHOP_ABUT", 1, 0, 1);
    s->inf_guard = td5_env_int("TD5RE_ROAM_SHOP_INFILL_GUARD", 1, 0, 1);
    s->names     = td5_env_int("TD5RE_ROAM_SHOP_NAMES", 1, 0, 1);
    return s;
}

void td5_roam_shops_close(TD5_RoamShopSource *s)
{
    if (!s) return;
    if (s->bname) { int i; for (i = 0; i < s->nb; i++) free(s->bname[i]); free(s->bname); }
    free(s->fbld); free(s->fhas_bld);
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
        char *nm = s->bname ? s->bname[i] : NULL;
        for (k = i - 1; k >= 0 && s->bid[k] > id; k--) {
            s->bid[k + 1] = s->bid[k]; s->bcat[k + 1] = s->bcat[k]; s->bpt[k + 1] = s->bpt[k];
            s->bx[k + 1] = s->bx[k]; s->bz[k + 1] = s->bz[k];
            if (s->bname) s->bname[k + 1] = s->bname[k];
        }
        s->bid[k + 1] = id; s->bcat[k + 1] = c; s->bpt[k + 1] = p; s->bx[k + 1] = x; s->bz[k + 1] = z;
        if (s->bname) s->bname[k + 1] = nm;
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
        s->bname = (char **)calloc((size_t)n, sizeof(char *));
        if (!s->bid || !s->bx || !s->bz || !s->bcat || !s->bpt || !s->bname) n = 0;
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
        {   /* [roam PE] the shop's name, for the fascia sign */
            const char *nm = sh_str(e, "name");
            if (nm && nm[0] && (s->bname[i] = (char *)malloc(strlen(nm) + 1)) != NULL) memcpy(s->bname[i], nm, strlen(nm) + 1);
        }
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
        s->fbld = (double *)malloc((size_t)n * sizeof(double));
        s->fhas_bld = (unsigned char *)malloc((size_t)n);
        if (s->fx && s->fz && s->fcol && s->fring0 && s->fringn && s->rx && s->rz && s->fbld && s->fhas_bld) {
            i = 0;
            for (e = arr->child; e; e = e->next, i++) {
                const cJSON *r = cJSON_GetObjectItem(e, "ring"), *p;
                s->fx[i] = sh_num(e, "x", 0.0);
                s->fz[i] = sh_num(e, "z", 0.0);
                s->fcol[i] = sh_brand_col(sh_str(e, "brand"), sh_str(e, "name"));
                s->fhas_bld[i] = (unsigned char)cJSON_IsNumber(cJSON_GetObjectItem(e, "bld"));
                s->fbld[i] = sh_num(e, "bld", 0.0);
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

static int sh_find(const TD5_RoamShopSource *s, double id);

/* ---------------------------------------------------------------- [roam PE] shop-name atlas
 *
 * One 64 x 16 cell per distinct NAME of POIS.JSON's footprints (buildings[].name), on pages of their own
 * (ROAM_SHOP_PAGE_BASE, ROAM_SHOP_PAGE_COUNT): a street plate or a station board never loses its cell to a shop. The bank
 * holds ROAM_SHOP_PAGE_COUNT * 4 cells; the names of casco footprints go first (a POI of POIS.JSON's pois[] with
 * casco = true joined to it), then the names used by the most footprints (the chains: Loteria de la Provincia, Banco
 * Provincia, Subway), then the shorter, then by name. Upper case like the plates (tg_plate_upper), ONE
 * line (a 0.45 m fascia sign is too low for two), the cap at least SN_MIN_CAP px: a longer name loses its leading words
 * to initials ("FARMACIA DEL PUEBLO" -> "F. DEL PUEBLO") until it fits, else it is skipped. Dark letters on a white
 * cell: the sign quad's vertex colour (the fascia tint, lightened) is the board colour. Built on the MAIN thread before
 * any tile builds (the cells are a pure function of POIS.JSON, so every tile agrees), read-only afterwards. */
#define SN_CELL_W    64
#define SN_CELL_H    16
#define SN_ROWS      (TD5_TG_TEX_DIM / SN_CELL_H)
#define SN_PAD       3
#define SN_MIN_CAP   5.5f
#define SN_NAME_MAX  96
#define SN_CELLS     (ROAM_SHOP_PAGE_COUNT * SN_ROWS)

typedef struct { char name[SN_NAME_MAX]; int uses, casco, page, y; } SnCell;

static struct {
    int     ready, uploaded;
    char    place[64];
    SnCell  cell[SN_CELLS];
    int     ncell, npg;
    unsigned char pg[ROAM_SHOP_PAGE_COUNT][TD5_TG_TEX_TEXELS];
    int     wanted, cap_drop, long_drop, abbrev;
} s_sn;

static int sn_utf8_n(unsigned char ch) { return ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4; }

/* the leftmost word that is not the last and not already an initial becomes its initial; 0 = nothing left to shorten */
static int sn_abbrev(char *up)
{
    char out[SN_NAME_MAX];
    char *w[16];
    int nw = 0, i, done = 0;
    char *q = up;
    while (*q && nw < 16) {
        while (*q == ' ') q++;
        if (!*q) break;
        w[nw++] = q;
        while (*q && *q != ' ') q++;
        if (*q) { *q = 0; q++; }
    }
    out[0] = 0;
    for (i = 0; i < nw; i++) {
        const size_t l = strlen(w[i]);
        if (out[0]) strncat(out, " ", sizeof out - strlen(out) - 1);
        if (!done && i < nw - 1 && l > 2 && w[i][l - 1] != '.') {
            const int n1 = sn_utf8_n((unsigned char)w[i][0]);
            char ini[8];
            memcpy(ini, w[i], (size_t)n1); ini[n1] = '.'; ini[n1 + 1] = 0;
            strncat(out, ini, sizeof out - strlen(out) - 1);
            done = 1;
        } else {
            strncat(out, w[i], sizeof out - strlen(out) - 1);
        }
    }
    snprintf(up, SN_NAME_MAX, "%s", out);
    return done;
}

static int sn_cell_cmp(const void *pa, const void *pb)
{
    const SnCell *a = (const SnCell *)pa, *b = (const SnCell *)pb;
    const size_t la = strlen(a->name), lb = strlen(b->name);
    if (a->casco != b->casco) return b->casco - a->casco;      /* the casco's shops first: where the city is driven */
    if (a->uses != b->uses) return b->uses - a->uses;
    if (la != lb) return la < lb ? -1 : 1;
    return strcmp(a->name, b->name);
}

static int sn_name_cmp(const void *pa, const void *pb)
{
    return strcmp(((const SnCell *)pa)->name, ((const SnCell *)pb)->name);
}

static int sn_dbl_cmp(const void *pa, const void *pb)
{
    const double a = *(const double *)pa, b = *(const double *)pb;
    return a < b ? -1 : (a > b);
}

int td5_roam_shops_atlas_prepare(const char *place)
{
    char path[512];
    char *json;
    cJSON *root;
    const cJSON *arr, *e;
    SnCell *want = NULL;
    double *cid = NULL;
    int nw = 0, capw = 0, i, ncid = 0;
    if (!place) return 0;
    if (s_sn.ready && strcmp(s_sn.place, place) == 0) return s_sn.ncell > 0;
    memset(&s_sn, 0, sizeof s_sn);
    snprintf(s_sn.place, sizeof s_sn.place, "%s", place);
    s_sn.ready = 1;
    if (!td5_roam_shops_enabled() || !td5_env_int("TD5RE_ROAM_SHOP_NAMES", 1, 0, 1)) return 0;
    td5_geo_source_path(path, sizeof path, place, "POIS.JSON");
    json = td5_geo_roads_slurp(path);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    arr = cJSON_GetObjectItem(root, "pois");                    /* the footprints a casco POI joins */
    if (cJSON_IsArray(arr) && (cid = (double *)malloc((size_t)cJSON_GetArraySize(arr) * sizeof(double) + 8)) != NULL) {
        for (e = arr->child; e; e = e->next)
            if (cJSON_IsTrue(cJSON_GetObjectItem(e, "casco")) && cJSON_IsNumber(cJSON_GetObjectItem(e, "bld")))
                cid[ncid++] = sh_num(e, "bld", 0.0);
        qsort(cid, (size_t)ncid, sizeof(double), sn_dbl_cmp);
    }
    arr = cJSON_GetObjectItem(root, "buildings");
    for (e = cJSON_IsArray(arr) ? arr->child : NULL; e; e = e->next) {
        const char *nm = sh_str(e, "name");
        if (!nm || !nm[0] || td5_geob_poi_cat_of_name(sh_str(e, "cat")) == TD5_GEOB_POI_NONE) continue;
        if (strlen(nm) >= SN_NAME_MAX) continue;
        if (nw >= capw) {
            const int nc = capw ? capw * 2 : 1024;
            SnCell *nwp = (SnCell *)realloc(want, (size_t)nc * sizeof(SnCell));
            if (!nwp) break;
            want = nwp; capw = nc;
        }
        memset(&want[nw], 0, sizeof want[nw]);
        snprintf(want[nw].name, sizeof want[nw].name, "%s", nm);
        want[nw].uses = 1;
        {
            const double id = sh_num(e, "id", 0.0);
            want[nw].casco = cid && ncid > 0 && bsearch(&id, cid, (size_t)ncid, sizeof(double), sn_dbl_cmp) != NULL;
        }
        nw++;
    }
    free(cid);
    cJSON_Delete(root);
    /* one entry per distinct name, with its use count */
    qsort(want, (size_t)nw, sizeof *want, sn_name_cmp);
    {
        int o = 0;
        for (i = 0; i < nw; i++) {
            if (o > 0 && strcmp(want[o - 1].name, want[i].name) == 0) {
                want[o - 1].uses++;
                want[o - 1].casco |= want[i].casco;
                continue;
            }
            want[o++] = want[i];
        }
        nw = o;
    }
    s_sn.wanted = nw;
    qsort(want, (size_t)nw, sizeof *want, sn_cell_cmp);
    for (i = 0; i < nw; i++) {
        char up[SN_NAME_MAX];
        unsigned char cov[SN_CELL_W * SN_CELL_H];
        float cap = 0.0f;
        int tries, abbr = 0, x, y, pg, row;
        if (s_sn.ncell >= SN_CELLS) { s_sn.cap_drop++; continue; }
        tg_plate_upper(want[i].name, up, sizeof up);
        for (tries = 0; tries < 6 && td5_font_plate_fit_cap(up, SN_CELL_W, SN_PAD) < SN_MIN_CAP; tries++) {
            if (!sn_abbrev(up)) break;
            abbr = 1;
        }
        if (td5_font_plate_fit_cap(up, SN_CELL_W, SN_PAD) < SN_MIN_CAP) { s_sn.long_drop++; continue; }
        if (!td5_font_plate_raster(up, NULL, SN_CELL_W, SN_CELL_H, SN_PAD, cov, &cap)) { s_sn.long_drop++; continue; }
        pg = s_sn.ncell / SN_ROWS; row = s_sn.ncell % SN_ROWS;
        if (row == 0) { memset(s_sn.pg[pg], 0, sizeof s_sn.pg[pg]); s_sn.npg = pg + 1; }
        for (y = 0; y < SN_CELL_H; y++)
            for (x = 0; x < SN_CELL_W; x++)
                s_sn.pg[pg][(row * SN_CELL_H + y) * TD5_TG_TEX_DIM + x] = (unsigned char)((cov[y * SN_CELL_W + x] * 15 + 127) / 255);
        s_sn.cell[s_sn.ncell] = want[i];
        s_sn.cell[s_sn.ncell].page = pg;
        s_sn.cell[s_sn.ncell].y = row * SN_CELL_H;
        s_sn.ncell++;
        s_sn.abbrev += abbr;
    }
    free(want);
    qsort(s_sn.cell, (size_t)s_sn.ncell, sizeof(SnCell), sn_name_cmp);   /* bsearch by name */
    TD5_LOG_I(LOG_TAG, "roam shops: shop-name atlas, %d distinct name(s), %d cell(s) on %d page(s) (ids %d..), %d left out by "
              "the cap, %d unreadable even abbreviated, %d abbreviated", s_sn.wanted, s_sn.ncell, s_sn.npg, ROAM_SHOP_PAGE_BASE,
              s_sn.cap_drop, s_sn.long_drop, s_sn.abbrev);
    return s_sn.ncell > 0;
}

int td5_roam_shops_session_begin(const char *place)
{
    uint8_t *px;
    int p, i;
    if (!td5_roam_shops_atlas_prepare(place) || s_sn.npg <= 0) return 0;
    px = (uint8_t *)malloc((size_t)TD5_TG_TEX_TEXELS * 4u);
    if (!px) return 0;
    for (p = 0; p < s_sn.npg; p++) {
        for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {           /* white board, near-black letters (BGRA) */
            const int v = 255 - (255 - 24) * s_sn.pg[p][i] / 15;
            px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = (uint8_t)v;
            px[i * 4 + 3] = 0xFF;
        }
        td5_asset_set_page_transparency(ROAM_SHOP_PAGE_BASE + p, 0);
        td5_plat_render_upload_texture(ROAM_SHOP_PAGE_BASE + p, px, TD5_TG_TEX_DIM, TD5_TG_TEX_DIM, 2);
    }
    free(px);
    s_sn.uploaded = 1;
    return 1;
}

int td5_roam_shops_name_cell(const char *name, int *page_id, double *v_top, double *v_bot)
{
    SnCell key;
    const SnCell *c;
    if (!name || !name[0] || s_sn.ncell <= 0 || strlen(name) >= SN_NAME_MAX) return 0;
    snprintf(key.name, sizeof key.name, "%s", name);
    c = (const SnCell *)bsearch(&key, s_sn.cell, (size_t)s_sn.ncell, sizeof(SnCell), sn_name_cmp);
    if (!c) return 0;
    if (page_id) *page_id = ROAM_SHOP_PAGE_BASE + c->page;
    if (v_top) *v_top = ((double)c->y + 1.5) / 64.0;
    if (v_bot) *v_bot = ((double)(c->y + SN_CELL_H) - 1.5) / 64.0;
    return 1;
}

void td5_roam_shops_atlas_census(int *wanted, int *placed, int *cap_drop, int *long_drop, int *abbrev)
{
    if (wanted) *wanted = s_sn.wanted;
    if (placed) *placed = s_sn.ncell;
    if (cap_drop) *cap_drop = s_sn.cap_drop;
    if (long_drop) *long_drop = s_sn.long_drop;
    if (abbrev) *abbrev = s_sn.abbrev;
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
    double len, kerb_d;         /* units (len: of the dressed stretch) */
    double nx, nz;              /* outward unit normal */
    double t0, t1;              /* [roam PE] the dressed stretch of the edge (0..1); 0, 1 = the whole wall */
} ShFront;

/* [roam PE] Round N integ census: 45 casco shopfronts stood more than 5 cm inside another footprint's mass (a neighbour
 * whose ring overlaps the shop's wall, Overture blobs over OSM ways mostly, or an infill lot, td5_roam_scenery.c). The
 * glass keeps to the stretch of the wall whose outside no other footprint covers (probed every 0.25 m): the
 * longest free run, if it is still SH_EDGE_MIN long, else the wall is no shopfront. Probed 0.08 m out (SH_FREE_OUT). */
#define SH_FREE_STEP  SH_M(0.25)
#define SH_FREE_OUT   SH_M(0.08)          /* the glass stands 0.06 m out: a mass reaching 0.08 m covers it */
static int sh_free_run(const TD5_RoamShopBld *b, double ax, double az, double bx, double bz, double nx, double nz,
                       double len, double *t0, double *t1)
{
    int ns = (int)(len / SH_FREE_STEP), i, run0 = -1, best0 = 0, best1 = -1;
    if (ns < 2) ns = 2;
    for (i = 0; i <= ns; i++) {
        const double t = (double)i / (double)ns;
        const int free_ = !b->in_bld(b->u, ax + (bx - ax) * t + nx * SH_FREE_OUT, az + (bz - az) * t + nz * SH_FREE_OUT);
        if (free_ && run0 < 0) run0 = i;
        if ((!free_ || i == ns) && run0 >= 0) {
            const int end = free_ ? i : i - 1;
            if (end - run0 > best1 - best0) { best0 = run0; best1 = end; }
            run0 = -1;
        }
    }
    if (best1 < 0) return 0;
    *t0 = (double)best0 / (double)ns; *t1 = (double)best1 / (double)ns;
    if (best0 > 0) *t0 += 0.5 / (double)ns;           /* half a step clear of the covered sample */
    if (best1 < ns) *t1 -= 0.5 / (double)ns;
    return 1;
}

/* Is ring edge k a street wall? Probe just outside its midpoint: the nearest street within reach,
 * the wall facing it, nothing built between them. */
static int sh_street_wall(const TD5_RoamShopSource *s, TG_TileCtx *c, const TD5_RoamShopBld *b, int k, double sgn,
                          ShFront *out, TD5_RoamShopStats *st)
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
    out->t0 = 0.0; out->t1 = 1.0;
    if (s->abut && b->in_bld) {
        double t0, t1;
        if (!sh_free_run(b, ax, az, bx, bz, nx, nz, len, &t0, &t1) || (t1 - t0) * len < SH_EDGE_MIN) { st->abut_drop++; return 0; }
        if (t0 > 0.0 || t1 < 1.0) { out->t0 = t0; out->t1 = t1; out->len = (t1 - t0) * len; st->abut_trim++; }
    }
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
                           unsigned int h, double glass_top, double fascia_top, int cross, const char *name, ShQB *q,
                           TD5_RoamShopStats *st)
{
    const int k = fr->k, j = (k + 1) % b->n;
    /* [roam PE] the dressed stretch of the wall (the whole edge unless sh_free_run trimmed it) */
    const double ax = fr->t0 > 0.0 ? b->rx[k] + (b->rx[j] - b->rx[k]) * fr->t0 : b->rx[k];
    const double az = fr->t0 > 0.0 ? b->rz[k] + (b->rz[j] - b->rz[k]) * fr->t0 : b->rz[k];
    const double bx = fr->t1 < 1.0 ? b->rx[k] + (b->rx[j] - b->rx[k]) * fr->t1 : b->rx[j];
    const double bz = fr->t1 < 1.0 ? b->rz[k] + (b->rz[j] - b->rz[k]) * fr->t1 : b->rz[j];
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
    /* [roam PE] the shop's NAME: a board 2 cm proud of the fascia, centred on the front, as wide as its 4:1 cell allows */
    if (s->names) {
        int pid;
        double vt, vb;
        if (!name) st->name_none++;
        else if (!td5_roam_shops_name_cell(name, &pid, &vt, &vb)) st->name_nocell++;
        else {
            const double sh = (fascia_top - glass_top) - SH_M(0.08), y0 = glass_top + SH_M(0.04);
            double w = 4.0 * sh;
            if (w > fr->len - SH_M(0.4)) w = fr->len - SH_M(0.4);
            if (sh >= SH_M(0.2) && w >= SH_M(1.0)) {
                const double mx = 0.5 * (ax + bx), mz = 0.5 * (az + bz), o2 = SH_OFF_FASCIA + SH_M(0.02);
                /* the reader's right, facing the wall (forward = -n; game x = -east): the ring's winding flips the edge
                 * tangent, so the text runs along (nz, -nx), never along the edge (MEASURED: mirrored on Avenida 7) */
                const double rx_ = fr->nz, rz_ = -fr->nx;
                /* the board keeps the cell's 4:1 shape: a short front shows the middle of the cell, never squeezed */
                const double uw = 0.5 * (w / (4.0 * sh)) * (62.5 - 1.5) / 64.0, um = 0.5;
                unsigned int r = (fcol >> 16) & 255u, g = (fcol >> 8) & 255u, bl = fcol & 255u;
                r += (255u - r) * 3u / 5u; g += (255u - g) * 3u / 5u; bl += (255u - bl) * 3u / 5u;
                p[0][0] = mx - rx_ * 0.5 * w + fr->nx * o2; p[0][1] = y0;      p[0][2] = mz - rz_ * 0.5 * w + fr->nz * o2;
                p[1][0] = mx + rx_ * 0.5 * w + fr->nx * o2; p[1][1] = y0;      p[1][2] = mz + rz_ * 0.5 * w + fr->nz * o2;
                p[2][0] = p[1][0];                         p[2][1] = y0 + sh; p[2][2] = p[1][2];
                p[3][0] = p[0][0];                         p[3][1] = y0 + sh; p[3][2] = p[0][2];
                uv[0][0] = um - uw; uv[0][1] = vb; uv[1][0] = um + uw; uv[1][1] = vb;
                uv[2][0] = um + uw; uv[2][1] = vt; uv[3][0] = um - uw; uv[3][1] = vt;
                sh_quad(q, pid, p, uv, 0xFF000000u | (r << 16) | (g << 8) | bl);
                st->name_signs++;
            } else st->name_nocell++;
        }
    }

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
            /* [roam NA] the valance alone too: its face projects onto the awning's outer edge, and an edge touching a
             * road prim can pass the quad test (a facade moved onto the municipal line, tile 14,53) */
            if (roam_ctx_on_road(c, ox, oz, 4) || roam_ctx_on_road(c, &ox[2], &oz[2], 2)) {
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
        /* 1.0 m across, its centre at the fascia's top (a 0.25 m bracket out of the wall) */
        const double yc = fascia_top;
        const double o0 = SH_M(0.25), o1 = SH_M(1.25), om = 0.5 * (o0 + o1), arm = 0.5 * (o1 - o0), bar = SH_M(0.17);
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
        if (sh_street_wall(s, c, b, k, area2 > 0.0 ? 1.0 : -1.0, &fr[nfr], st)) nfr++;
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
            const double el = hypot(dx, dz);   /* the edge (fr[k].len is the dressed stretch since roam PE) */
            double t = ((px - b->rx[a]) * dx + (pz - b->rz[a]) * dz) / (el * el), d;
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
    {
        const int bi = s->names ? sh_find(s, b->id) : -1;           /* [roam PE] the POI's name, for the sign */
        const char *nm = (bi >= 0 && s->bname) ? s->bname[bi] : NULL;
        for (k = 0; k < npick; k++)
            sh_dress_front(s, c, b, &pick[k], cat, h, glass_top, fascia_top,
                           cat == TD5_GEOB_POI_PHARMACY && k == 0, nm, q, st);
    }
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

/* [roam PD] see td5_roam_shops.h */
int td5_roam_shops_fuel_at(const TD5_RoamShopSource *s, double x, double z)
{
    int i;
    if (!s) return 0;
    for (i = 0; i < s->nf; i++) {
        const double d = hypot(x - s->fx[i], z - s->fz[i]);
        if (s->fringn[i] >= 3) {
            if (d < SH_M(120.0) && sh_pip(&s->rx[s->fring0[i]], &s->rz[s->fring0[i]], s->fringn[i], x, z)) return 1;
        } else if (d < SH_M(15.0)) return 1;
    }
    return 0;
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

void td5_roam_shops_tile_begin(TD5_RoamShopSource *s) { if (s) s->npend = 0; }

/* [roam PE] THE CANOPY OSM ALREADY HAS. LB placed a 16 x 9 m canopy round each fuel point and kept it off every footprint,
 * but the casco's forecourts are mostly mapped AS the canopy: the 13 forecourt ways are building=roof, and 4 node sites
 * join a building=roof footprint (POIS.JSON fuel[].bld). That footprint was drawn as a roof slab (s3d roof-only) or a box,
 * and LB's canopy found no clear spot beside it (23 of 41 owned sites). A footprint joined to a fuel site (its bld, or a
 * roof inside the forecourt ring) that is building=roof (40-1000 m2) or a low box (< 7 m, 80-400 m2, no other business)
 * is the canopy: drawn with LB's deck, brand fascia, posts and pump islands on its own rectangle instead of a building. */
int td5_roam_shops_canopy_fp(TD5_RoamShopSource *s, double id, const TD5_GeoBuilding *gb, int *site)
{
    int i;
    if (!s || !s->canopy || !s->canopy_fp || !gb || gb->part || gb->rel || gb->n < 3) return 0;
    sh_load(s);
    for (i = 0; i < s->nf; i++) {
        int hit = s->fhas_bld && s->fhas_bld[i] && s->fbld[i] == id;
        if (!hit && gb->roof_only && s->fringn[i] >= 3 && hypot(gb->cx - s->fx[i], gb->cz - s->fz[i]) < SH_M(80.0))
            hit = sh_pip(&s->rx[s->fring0[i]], &s->rz[s->fring0[i]], s->fringn[i], gb->cx, gb->cz);
        if (!hit) continue;
        if (gb->roof_only ? (gb->area_m2 >= 40.0 && gb->area_m2 <= 1000.0)
                          : (gb->height < SH_M(7.0) && gb->area_m2 >= 80.0 && gb->area_m2 <= 400.0 && !gb->landmark &&
                             !gb->civic && (gb->poi_cat == TD5_GEOB_POI_NONE || gb->poi_cat == TD5_GEOB_POI_FUEL))) {
            if (site) *site = i;
            return 1;
        }
    }
    return 0;
}

int td5_roam_shops_canopy_from_fp(TD5_RoamShopSource *s, TG_TileCtx *c, int site, const TD5_GeoBuilding *gb,
                                  const double *rx, const double *rz, int n, TD5_RoamShopStats *st)
{
    static const double scale[5] = { 1.0, 0.9, 0.8, 0.7, 0.6 };
    int why_deck = 0, why_post = 0;
    double best = 1e300, bcx = 0.0, bcz = 0.0, btx = 1.0, btz = 0.0, bhl = 0.0, bhw = 0.0, clear;
    int k, j, sc;
    if (!s || s->npend >= SH_CAN_MAX || site < 0 || site >= s->nf || n < 3) { if (st) st->canopy_fp_kept++; return 0; }
    /* the smallest rectangle on one of the ring's edge directions */
    for (k = 0; k < n; k++) {
        const int k2 = (k + 1) % n;
        const double dx = rx[k2] - rx[k], dz = rz[k2] - rz[k], l = hypot(dx, dz);
        double tx, tz, a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300;
        if (l < SH_M(1.0)) continue;
        tx = dx / l; tz = dz / l;
        for (j = 0; j < n; j++) {
            const double a = rx[j] * tx + rz[j] * tz, bb = -rx[j] * tz + rz[j] * tx;
            if (a < a0) a0 = a;
            if (a > a1) a1 = a;
            if (bb < b0) b0 = bb;
            if (bb > b1) b1 = bb;
        }
        if ((a1 - a0) * (b1 - b0) < best) {
            const double am = 0.5 * (a0 + a1), bm = 0.5 * (b0 + b1);
            best = (a1 - a0) * (b1 - b0);
            bcx = am * tx - bm * tz; bcz = am * tz + bm * tx;
            btx = tx; btz = tz; bhl = 0.5 * (a1 - a0); bhw = 0.5 * (b1 - b0);
        }
    }
    if (!(best < 1e300) || bhl < SH_M(2.5) || bhw < SH_M(2.5)) { st->canopy_fp_kept++; return 0; }
    if (bhw > bhl) { const double t = bhl, ox = btx; bhl = bhw; bhw = t; btx = -btz; btz = ox; }   /* t along the long side */
    clear = gb->height - SH_CAN_T;                     /* the mapped (or measured) top is the deck's top */
    if (clear < SH_M(4.2)) clear = SH_M(4.2);
    if (clear > SH_M(6.0)) clear = SH_M(6.0);
    /* each scale at the rectangle's centre, then moved up to 3 m along and across it (a forecourt roof mapped over the
     * pavement edge: the deck slides back off the street) */
    for (sc = 0; sc < 5 * 9; sc++) {
        static const double k_off[9][2] = { { 0, 0 }, { 0, 1.5 }, { 0, -1.5 }, { 1.5, 0 }, { -1.5, 0 },
                                             { 0, 3.0 }, { 0, -3.0 }, { 3.0, 0 }, { -3.0, 0 } };
        const double hl = bhl * scale[sc / 9], hw = bhw * scale[sc / 9], nx = -btz, nz = btx;
        const double cx = bcx + btx * SH_M(k_off[sc % 9][0]) + nx * SH_M(k_off[sc % 9][1]);
        const double cz = bcz + btz * SH_M(k_off[sc % 9][0]) + nz * SH_M(k_off[sc % 9][1]);
        int a, bb, ok = 1, posts = 0, nposts = 0, isl = 1;
        for (a = -2; a <= 2 && ok; a++)
            for (bb = -2; bb <= 2 && ok; bb++)
                if (!sh_deck_pt_ok(c, cx + btx * hl * a * 0.5 + nx * hw * bb * 0.5, cz + btz * hl * a * 0.5 + nz * hw * bb * 0.5))
                    ok = 0;
        if (!ok) { why_deck++; continue; }
        for (k = 0; k < 4; k++) {
            const double sl = (k & 1) ? 1.0 : -1.0, sw = (k & 2) ? 1.0 : -1.0;
            if (sh_rect_clear(c, cx + btx * sl * SH_POST_L(hl) + nx * sw * SH_POST_W(hw),
                              cz + btz * sl * SH_POST_L(hl) + nz * sw * SH_POST_W(hw), btx, btz, SH_M(0.4), SH_M(0.4)))
                { posts |= 1 << k; nposts++; }
        }
        if (nposts < 2) { why_post++; continue; }
        if (hl - SH_M(3.0) < SH_M(1.0)) isl = 0;
        for (k = 0; k < 2 && isl; k++) {
            const double sw = k ? 1.0 : -1.0;
            if (!sh_rect_clear(c, cx + nx * sw * SH_ISL_W(hw), cz + nz * sw * SH_ISL_W(hw), btx, btz, hl - SH_M(3.0), SH_M(0.8)))
                isl = 0;
        }
        {
            ShCanopy *cp = &s->pend[s->npend++];
            cp->cx = cx; cp->cz = cz; cp->tx = btx; cp->tz = btz; cp->hl = hl; cp->hw = hw;
            cp->gy = roam_ctx_ground(c, cx, cz) + SH_M(0.15);
            cp->clear = clear;
            cp->site = site;
            cp->col = s->fcol[site];
            cp->islands = isl;
            cp->posts = posts;
            cp->sb = roam_ctx_sb_of(c, cx, cz);
        }
        st->canopy_fp++;
        if (s->log) TD5_LOG_I(LOG_TAG, "ROAM_SHOP_FUEL_FP site %d (%.0f,%.0f) footprint %s %.0f m2 h %.1f m: canopy %.1f x %.1f m "
                              "clear %.1f m (scale %.1f, posts %d, islands %d)", site, s->fx[site], s->fz[site],
                              gb->roof_only ? "roof" : "box", gb->area_m2, gb->height / SH_UPM, 2.0 * hl / SH_UPM,
                              2.0 * hw / SH_UPM, clear / SH_UPM, scale[sc / 9], nposts, isl);
        return 1;
    }
    st->canopy_fp_kept++;
    if (s->log) TD5_LOG_I(LOG_TAG, "ROAM_SHOP_FUEL_FP site %d (%.0f,%.0f) footprint %s %.0f m2: KEPT as a building (rect %.1f x %.1f m, "
                          "scales refused: deck over a street %d, posts on a road %d)", site, s->fx[site], s->fz[site],
                          gb->roof_only ? "roof" : "box", gb->area_m2, 2.0 * bhl / SH_UPM, 2.0 * bhw / SH_UPM, why_deck, why_post);
    return 0;
}

int td5_roam_shops_fuel(TD5_RoamShopSource *s, TG_TileCtx *c, int (*in_bld)(void *u, double x, double z), void *u,
                        int (*site_fp)(void *u, int site), TD5_RoamShopStats *st)
{
    int i, tx, tz;
    int32_t ox, oz;
    if (!s) return 1;
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
        if (site_fp && site_fp(u, i)) {    /* [roam PE] its footprint is the canopy, drawn by this tile */
            int q, drawn = 0;
            for (q = 0; q < s->npend; q++) if (s->pend[q].site == i) drawn = 1;
            if (drawn) { st->canopy_fp_site++; continue; }
        }
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
                            cp->clear = SH_CAN_CLEAR;
                            cp->site = -1;
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
        sh_box(q, cp->cx, cp->cz, gy + cp->clear, gy + cp->clear + SH_CAN_T, cp->tx, cp->tz, cp->hl, cp->hw,
               cp->col, 0xFFD8D8D8u, 1);
        for (pk = 0; pk < 4; pk++) {
            const double sl = (pk & 1) ? 1.0 : -1.0, sw = (pk & 2) ? 1.0 : -1.0;
            if (!(cp->posts & (1 << pk))) continue;
            const double qx = cp->cx + cp->tx * sl * SH_POST_L(cp->hl) + nnx * sw * SH_POST_W(cp->hw);
            const double qz = cp->cz + cp->tz * sl * SH_POST_L(cp->hl) + nnz * sw * SH_POST_W(cp->hw);
            sh_box(q, qx, qz, gy - SH_M(0.3), gy + cp->clear, cp->tx, cp->tz, SH_M(0.2), SH_M(0.2),
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
