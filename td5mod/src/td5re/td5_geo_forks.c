/**
 * td5_geo_forks.c -- GEO TRACK: CONFIRMED FORKS for a real place (PORT-ONLY).
 *                    Contract and rationale in td5_geo_forks.h.
 *
 * Reads re/assets/geo/<slug>/FORKS.JSON, the file geo_selector.py writes on
 * SEND TO GAME / SAVE FORKS:
 *
 *   { "place": "la_plata", "spans": 1477, "corridor_spans": 984,
 *     "forks": [ { "id": "median:Avenida 53:102-109", "kind": "ISLAND",
 *                  "name": "Avenida 53", "F": 177, "len": 228,
 *                  "sep": 0.16, "lanes": 4, "length_m": 682.6,
 *                  "source": "paired one-way ways, same name (1 of them)",
 *                  "detail": "..." }, ... ] }
 *
 * VALIDATION IS THE POINT OF THIS FILE. The table feeds the trackgen's three
 * stateless fork gates, which run during the centreline walk and assume the
 * forks arrive in ASCENDING span order and do not overlap -- the walk widens
 * the road ahead of fork i and keeps lane changes out of its window, and a
 * table out of order would protect one span range while the placement loop
 * committed to another. That exact disagreement is the measured regression
 * recorded at tg_fork_first_off() in td5_tg_branch.c, so it is worth refusing
 * a bad file here rather than discovering it as moved geometry later. Every
 * rejected entry is logged with the reason; the rest are kept, because losing
 * one fork is better than losing a whole track.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_geo.h"
#include "td5_geo_forks.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "geo"

/* Mirrors of the trackgen constants this file validates against. Kept as local
 * defines rather than an include of td5_trackgen_internal.h: that header pulls
 * in the whole generator, and this module exists precisely so the dependency
 * runs one way. A drift would relax a check, never tighten one, and the
 * placement loop re-checks every one of them anyway. */
#define GEOFK_GRID_SPAN     24    /* TD5_TG_GRID_SPAN     */
#define GEOFK_BRANCH_WIDEN   6    /* TD5_TG_BRANCH_WIDEN  */
#define GEOFK_MIN_LEN        3    /* tg_fork_len_floored's ISLAND floor */

typedef struct {
    int    F, len, lanes;
    double sep;
    char   kind[16];
    char   name[64];
} GeoFork;

static GeoFork s_fk[TD5_GEO_FORKS_MAX];
static int     s_fk_count;
static char    s_fk_slug[64];
static char    s_fk_source[512];

/* Same shape as td5_geo_signals.c's sig_slurp, duplicated for the same reason:
 * td5_geo.c's is file-static and neither module should grow td5_geo.h a
 * file-reading entry point it does not otherwise need. */
static char *fk_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;

    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)1 * 1024 * 1024) {
        td5_plat_file_close(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) {
        td5_plat_file_close(f);
        return NULL;
    }
    if (td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) {
        free(buf);
        td5_plat_file_close(f);
        return NULL;
    }
    buf[n] = '\0';
    td5_plat_file_close(f);
    return buf;
}

/* The shapes tg_fork_kind_name() prints. A kind outside this set is a format
 * change or a typo in a hand-edited file, and becoming fork kind 0 silently is
 * the one outcome worth refusing. */
static int fk_kind_known(const char *k)
{
    static const char *const names[] =
        { "AVENUE", "ISLAND", "WIDE", "SLIP", "MAJOR", "BYPASS" };
    int i;
    if (!k || !k[0]) return 0;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (strcmp(names[i], k) == 0) return 1;
    return 0;
}

void td5_geo_forks_unload(void)
{
    s_fk_count = 0;
    s_fk_slug[0] = '\0';
    s_fk_source[0] = '\0';
}

int td5_geo_forks_count(void) { return s_fk_count; }

const char *td5_geo_forks_source(void) { return s_fk_source; }

int td5_geo_forks_get(int i, int *F, int *len, double *sep, int *lanes)
{
    if (i < 0 || i >= s_fk_count) return 0;
    if (F)     *F     = s_fk[i].F;
    if (len)   *len   = s_fk[i].len;
    if (sep)   *sep   = s_fk[i].sep;
    if (lanes) *lanes = s_fk[i].lanes;
    return 1;
}

const char *td5_geo_forks_kind(int i)
{
    return (i >= 0 && i < s_fk_count) ? s_fk[i].kind : "";
}

const char *td5_geo_forks_name(int i)
{
    return (i >= 0 && i < s_fk_count) ? s_fk[i].name : "";
}

static void fk_copy_str(char *dst, size_t cap, const cJSON *j, const char *dflt)
{
    const char *s = (j && cJSON_IsString(j) && j->valuestring) ? j->valuestring
                                                               : dflt;
    snprintf(dst, cap, "%s", s ? s : "");
}

int td5_geo_forks_sync(void)
{
    const char *slug = td5_geo_loaded() ? td5_geo_place_slug() : "";
    char path[512];
    char *json;
    cJSON *root;
    const cJSON *arr;
    int n, i, kept = 0, bad = 0, route_spans = 0;

    if (!slug || !slug[0]) {           /* synthetic build: nothing to read */
        td5_geo_forks_unload();
        return 0;
    }
    if (s_fk_slug[0] && strcmp(s_fk_slug, slug) == 0)
        return s_fk_count;             /* already this place */

    td5_geo_forks_unload();
    snprintf(path, sizeof path, "re/assets/geo/%s/FORKS.JSON", slug);
    /* Remember the slug EVEN WHEN THERE IS NO FILE, so "this place has no
     * confirmed forks" is decided once per place rather than re-opening a
     * missing file on every span the walk asks about. */
    snprintf(s_fk_slug, sizeof s_fk_slug, "%s", slug);

    json = fk_slurp(path);
    if (!json) {
        /* Not an error. No FORKS.JSON means the user confirmed nothing, and
         * the generator uses its own fork placement -- which is also what
         * keeps a place fetched before this feature existed working. */
        TD5_LOG_I(LOG_TAG, "forks: no %s (no confirmed forks for this place)",
                  path);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_W(LOG_TAG, "forks: %s is not valid JSON", path);
        return 0;
    }
    {
        const cJSON *sp = cJSON_GetObjectItem(root, "spans");
        if (sp && cJSON_IsNumber(sp)) route_spans = (int)sp->valuedouble;
    }
    arr = cJSON_GetObjectItem(root, "forks");
    if (!arr || !cJSON_IsArray(arr)) {
        TD5_LOG_W(LOG_TAG, "forks: %s has no \"forks\" array", path);
        cJSON_Delete(root);
        return 0;
    }
    n = cJSON_GetArraySize(arr);

    for (i = 0; i < n; i++) {
        const cJSON *e  = cJSON_GetArrayItem(arr, i);
        const cJSON *pF = e ? cJSON_GetObjectItem(e, "F")     : NULL;
        const cJSON *pL = e ? cJSON_GetObjectItem(e, "len")   : NULL;
        const cJSON *pS = e ? cJSON_GetObjectItem(e, "sep")   : NULL;
        const cJSON *pN = e ? cJSON_GetObjectItem(e, "lanes") : NULL;
        GeoFork g;
        int R;

        if (kept >= TD5_GEO_FORKS_MAX) {
            TD5_LOG_W(LOG_TAG, "forks: %s holds %d entries; the engine takes "
                      "%d, so the rest are dropped", path, n,
                      TD5_GEO_FORKS_MAX);
            break;
        }
        if (!pF || !pL || !cJSON_IsNumber(pF) || !cJSON_IsNumber(pL)) {
            TD5_LOG_W(LOG_TAG, "forks: entry %d has no numeric F/len", i);
            bad++;
            continue;
        }
        memset(&g, 0, sizeof g);
        g.F     = (int)pF->valuedouble;
        g.len   = (int)pL->valuedouble;
        g.sep   = (pS && cJSON_IsNumber(pS)) ? pS->valuedouble : 0.16;
        g.lanes = (pN && cJSON_IsNumber(pN)) ? (int)pN->valuedouble : 0;
        fk_copy_str(g.kind, sizeof g.kind, cJSON_GetObjectItem(e, "kind"),
                    "ISLAND");
        fk_copy_str(g.name, sizeof g.name, cJSON_GetObjectItem(e, "name"), "");
        R = g.F + 1 + g.len;

        if (!fk_kind_known(g.kind)) {
            TD5_LOG_W(LOG_TAG, "forks: entry %d kind \"%s\" is not a fork shape "
                      "this build knows -- dropped", i, g.kind);
            bad++;
            continue;
        }
        if (g.len < GEOFK_MIN_LEN) {
            TD5_LOG_W(LOG_TAG, "forks: entry %d (%s) is %d span(s) long, floor "
                      "is %d -- dropped", i, g.name, g.len, GEOFK_MIN_LEN);
            bad++;
            continue;
        }
        if (g.F <= GEOFK_GRID_SPAN + GEOFK_BRANCH_WIDEN + 2) {
            TD5_LOG_W(LOG_TAG, "forks: entry %d (%s) splits at span %d, inside "
                      "the grid and its widened approach -- dropped",
                      i, g.name, g.F);
            bad++;
            continue;
        }
        /* ASCENDING AND DISJOINT. See the header: the walk's gates and the
         * placement loop both read this table in order. */
        if (kept > 0 &&
            g.F - GEOFK_BRANCH_WIDEN - 2 <=
                s_fk[kept - 1].F + 1 + s_fk[kept - 1].len + 2) {
            TD5_LOG_W(LOG_TAG, "forks: entry %d (%s) at F=%d overlaps or is out "
                      "of order against the previous fork ending at %d -- "
                      "dropped", i, g.name, g.F,
                      s_fk[kept - 1].F + 1 + s_fk[kept - 1].len);
            bad++;
            continue;
        }
        if (route_spans > 0 && R + 24 >= route_spans) {
            TD5_LOG_W(LOG_TAG, "forks: entry %d (%s) rejoins at span %d, past "
                      "the ring's %d-span tail margin -- dropped",
                      i, g.name, R, route_spans);
            bad++;
            continue;
        }
        s_fk[kept++] = g;
    }
    cJSON_Delete(root);

    s_fk_count = kept;
    snprintf(s_fk_source, sizeof s_fk_source, "%s", path);
    TD5_LOG_I(LOG_TAG, "forks: %d confirmed fork(s) from %s (route %d spans)%s",
              s_fk_count, path, route_spans,
              bad ? " -- some entries dropped, see the warnings above" : "");
    for (i = 0; i < s_fk_count; i++)
        TD5_LOG_I(LOG_TAG, "forks:   %d: %s F=%d len=%d R=%d lanes=%d sep=%.2f "
                  "(%s)", i, s_fk[i].kind, s_fk[i].F, s_fk[i].len,
                  s_fk[i].F + 1 + s_fk[i].len, s_fk[i].lanes, s_fk[i].sep,
                  s_fk[i].name);
    return s_fk_count;
}
