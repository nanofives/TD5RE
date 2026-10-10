/**
 * td5_geo_avenues.c -- GEO TRACK: DIVIDED AVENUES for a real place (PORT-ONLY).
 *                      Contract and rationale in td5_geo_avenues.h.
 *
 * Reads re/assets/geo/<slug>/_route/AVENUES.JSON, the file td5_geo_route.c
 * writes on BUILD, and answers one question per span for the generator: is this
 * span inside a divided avenue, and if so where is the opposite carriageway.
 *
 * VALIDATION, as in td5_geo_forks.c, is the point of a loader. The span table
 * is binary-searched, so it MUST be ascending and free of duplicates; an offset
 * smaller than the two half-carriageways would put the scenery road inside the
 * race road; and a span inside the start grid is geometry laid under the grid
 * lights. Every rejected row is logged with its reason and the rest are kept,
 * because losing one median is better than losing a whole track.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_geo.h"
#include "td5_config.h"
#include "td5_geo_avenues.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "geo"

/* Mirrors of the trackgen constants validated against, local for the same
 * reason td5_geo_forks.c keeps its own: this module exists so the dependency
 * runs one way. A drift here can only relax a check, and the emitter re-reads
 * the real node width anyway. */
#define GEOAV_GRID_SPAN     24       /* TD5_TG_GRID_SPAN  */
#define GEOAV_LANE_WIDTH  1500.0     /* TD5_TG_LANE_WIDTH */
/* A route is capped at 3000 spans (GR_MAX_SPANS); the writer caps its own
 * table at 1024 rows. */
#define GEOAV_MAX_SPANS   1024

typedef struct {
    int    span;
    int    av;
    int    lanes;
    int    open;
    double off;
} GeoAvSpan;

static GeoAvSpan s_rows[GEOAV_MAX_SPANS];
static int       s_n_rows;
static int       s_n_av;
static int       s_s0[TD5_GEO_AVENUES_MAX], s_s1[TD5_GEO_AVENUES_MAX];
static char      s_name[TD5_GEO_AVENUES_MAX][64];
static char      s_slug[64];
static char      s_source[512];

/* Same shape as td5_geo_forks.c's fk_slurp, duplicated for the same reason:
 * neither module should grow td5_geo.h a file-reading entry point it does not
 * otherwise need. The cap is larger here because the per-span table is the
 * bulk of the file (one small object per span of every avenue). */
static char *av_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;

    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)4 * 1024 * 1024) {
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

void td5_geo_avenues_unload(void)
{
    s_n_rows = 0;
    s_n_av   = 0;
    s_slug[0] = '\0';
    s_source[0] = '\0';
}

int td5_geo_avenues_count(void) { return s_n_av; }

const char *td5_geo_avenues_source(void) { return s_source; }

int td5_geo_avenue_range(int i, int *s0, int *s1, const char **name)
{
    if (i < 0 || i >= s_n_av) return 0;
    if (s0)   *s0   = s_s0[i];
    if (s1)   *s1   = s_s1[i];
    if (name) *name = s_name[i];
    return 1;
}

int td5_geo_avenue_at(int si, double *off, int *lanes, int *open)
{
    int lo = 0, hi = s_n_rows - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        if      (s_rows[mid].span < si) lo = mid + 1;
        else if (s_rows[mid].span > si) hi = mid - 1;
        else {
            if (off)   *off   = s_rows[mid].off;
            if (lanes) *lanes = s_rows[mid].lanes;
            if (open)  *open  = s_rows[mid].open;
            return 1;
        }
    }
    return 0;
}

static double av_num(const cJSON *o, const char *k, double dflt)
{
    const cJSON *j = o ? cJSON_GetObjectItem(o, k) : NULL;
    return (j && cJSON_IsNumber(j)) ? j->valuedouble : dflt;
}

int td5_geo_avenues_sync(void)
{
    const char *slug = td5_geo_loaded() ? td5_geo_place_slug() : "";
    char path[512];
    char *json;
    cJSON *root;
    const cJSON *arr;
    int n, i, kept_av = 0, bad = 0, route_spans = 0;

    if (!slug || !slug[0]) {           /* synthetic build: nothing to read */
        td5_geo_avenues_unload();
        return 0;
    }
    if (s_slug[0] && strcmp(s_slug, slug) == 0)
        return s_n_av;                 /* already this place */

    td5_geo_avenues_unload();
    td5_geo_place_path(path, sizeof path, slug, "AVENUES.JSON");
    /* Remember the slug EVEN WHEN THERE IS NO FILE, so "this place has no
     * divided avenue" is decided once per place rather than re-opening a
     * missing file on every span the emitter asks about. */
    snprintf(s_slug, sizeof s_slug, "%s", slug);

    json = av_slurp(path);
    if (!json) {
        /* Not an error: a place fetched before this feature existed, or a
         * route with no divided avenue on it. */
        TD5_LOG_I(LOG_TAG, "avenues: no %s (no divided avenue on this route)",
                  path);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_W(LOG_TAG, "avenues: %s is not valid JSON", path);
        return 0;
    }
    route_spans = (int)av_num(root, "spans", 0.0);
    arr = cJSON_GetObjectItem(root, "avenues");
    if (!arr || !cJSON_IsArray(arr)) {
        TD5_LOG_W(LOG_TAG, "avenues: %s has no \"avenues\" array", path);
        cJSON_Delete(root);
        return 0;
    }
    n = cJSON_GetArraySize(arr);

    for (i = 0; i < n; i++) {
        const cJSON *e  = cJSON_GetArrayItem(arr, i);
        const cJSON *sp = e ? cJSON_GetObjectItem(e, "spans") : NULL;
        const cJSON *nm = e ? cJSON_GetObjectItem(e, "name") : NULL;
        const int s0 = (int)av_num(e, "s0", -1.0);
        const int s1 = (int)av_num(e, "s1", -1.0);
        const int base = s_n_rows;
        int ns, q, kept = 0;

        if (kept_av >= TD5_GEO_AVENUES_MAX) {
            TD5_LOG_W(LOG_TAG, "avenues: %s holds %d avenue(s); the engine takes "
                      "%d, so the rest are dropped", path, n,
                      TD5_GEO_AVENUES_MAX);
            break;
        }
        if (!sp || !cJSON_IsArray(sp) || s0 < 0 || s1 < s0) {
            TD5_LOG_W(LOG_TAG, "avenues: entry %d has no usable span range "
                      "(s0=%d s1=%d) -- dropped", i, s0, s1);
            bad++;
            continue;
        }
        ns = cJSON_GetArraySize(sp);
        for (q = 0; q < ns; q++) {
            const cJSON *r = cJSON_GetArrayItem(sp, q);
            const int    sv = (int)av_num(r, "s", -1.0);
            const double ov = av_num(r, "off", 0.0);
            const int    lv = (int)av_num(r, "lanes", 2.0);
            GeoAvSpan *g;

            if (s_n_rows >= GEOAV_MAX_SPANS) {
                TD5_LOG_W(LOG_TAG, "avenues: the per-span table is full at %d "
                          "rows; the rest of %s is dropped", GEOAV_MAX_SPANS,
                          s_name[kept_av]);
                break;
            }
            if (sv < 0 || sv < s0 || sv > s1) { bad++; continue; }
            /* ASCENDING AND UNIQUE: td5_geo_avenue_at binary-searches this
             * table, so an out-of-order row would silently hide its
             * neighbours. The writer emits them in order; refusing here is
             * what makes a hand-edited file safe. */
            if (s_n_rows > 0 && sv <= s_rows[s_n_rows - 1].span) { bad++; continue; }
            /* Inside the start grid is geometry under the grid lights.
             * [ROUND 1014 C] ... except that the scenery carriageway is not
             * drivable and the writer (td5_geo_route.c) now starts an avenue at
             * span 1 so a race that begins on one shows the whole avenue; only
             * span 0, which no building emitter touches, stays refused.
             * TD5RE_GEO_AVENUE_GRID=0 restores the 24-span margin. */
            if (sv <= (td5_env_flag_on("TD5RE_GEO_AVENUE_GRID") ? 0 : GEOAV_GRID_SPAN)) {
                bad++; continue;
            }
            /* Past the ring is a span that does not exist. */
            if (route_spans > 0 && sv >= route_spans - 1) { bad++; continue; }
            /* An offset narrower than the two half-carriageways would put the
             * scenery road inside the race road. The emitter re-checks against
             * the REAL node width (this only knows the nominal lane width), so
             * this is the cheap backstop, not the authority. */
            if (ov > -GEOAV_LANE_WIDTH && ov < GEOAV_LANE_WIDTH) { bad++; continue; }

            g = &s_rows[s_n_rows++];
            g->span  = sv;
            g->av    = kept_av;
            g->off   = ov;
            g->lanes = (lv > 0 && lv <= 8) ? lv : 2;
            g->open  = av_num(r, "open", 0.0) != 0.0;
            kept++;
        }
        if (kept < 1) {
            s_n_rows = base;
            TD5_LOG_W(LOG_TAG, "avenues: entry %d (spans %d..%d) kept no usable "
                      "span -- dropped", i, s0, s1);
            bad++;
            continue;
        }
        snprintf(s_name[kept_av], sizeof s_name[0], "%s",
                 (nm && cJSON_IsString(nm) && nm->valuestring)
                     ? nm->valuestring : "");
        s_s0[kept_av] = s_rows[base].span;
        s_s1[kept_av] = s_rows[s_n_rows - 1].span;
        kept_av++;
    }
    cJSON_Delete(root);

    s_n_av = kept_av;
    snprintf(s_source, sizeof s_source, "%s", path);
    TD5_LOG_I(LOG_TAG, "avenues: %d divided avenue(s) over %d span(s) from %s "
              "(route %d spans)%s", s_n_av, s_n_rows, path, route_spans,
              bad ? " -- some rows dropped, see the warnings above" : "");
    for (i = 0; i < s_n_av; i++) {
        double lo = 1e30, hi = 0.0;
        int q, nopen = 0;
        for (q = 0; q < s_n_rows; q++) {
            const double a = s_rows[q].off < 0.0 ? -s_rows[q].off : s_rows[q].off;
            if (s_rows[q].av != i) continue;
            if (s_rows[q].open) nopen++;
            if (a < lo) lo = a;
            if (a > hi) hi = a;
        }
        TD5_LOG_I(LOG_TAG, "avenues:   %d: %s spans %d..%d, opposite carriageway "
                  "%.0f..%.0f units away, %d cross-street opening(s)", i,
                  s_name[i], s_s0[i], s_s1[i], lo, hi, nopen);
    }
    return s_n_av;
}
