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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"      /* [1015 A] the real cross streets, for the median openings */
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

/* [ROUND 1015 A item 17] "there should be a drivable avenue on the street at level091 e224"
 * (Avenida 60, span 899).
 *
 * The route commit starts an avenue's per-span table at the span its detector run begins
 * on, which is the run's first route VERTEX. Avenida 60 begins at the exit of Plaza Maximo
 * Paz: the route leaves the ring at span 896 and the first vertex on the avenue is span 904,
 * so spans 896..903 are on the avenue's own first segment, with its opposite carriageway
 * 12.3 m away, and carried no row: no scenery road, no median, and (because the fork starts
 * at the first span of the table) no fork either. This walks each avenue's start BACKWARD on
 * the same road graph, one span at a time, for as long as an anti-parallel way of the
 * avenue's own street keeps the same side and stays within the per-span continuity step the
 * commit uses (GR_AV_MAX_STEP, 375 units), up to GEOAV_BACKFILL spans and never into the
 * previous avenue or the start grid's span 0. Needs no re-BUILD: it reads the existing
 * cache. TD5RE_GEO_AVENUE_BACKFILL=0 keeps the file's first span. */
#define GEOAV_BACKFILL        12
#define GEOAV_STEP_MAX        375.0
#define GEOAV_ANTI_COS        (-0.80)       /* peer direction . route tangent below this */
#define GEOAV_MIN_MEDIAN      150.0
static int av_peer_at(int name_id, double x, double z, double tx, double tz, int side,
                      double prev_d, double own_half, double *off_out, int *lanes_out)
{
    int j, best_w = -1;
    double best_err = 1e30, best_d = 0.0;
    for (j = 0; j < td5_geo_roads_count(); j++) {
        const TD5_GeoRoad *r = td5_geo_roads_get(j);
        int k;
        if (!r || r->name_id != name_id || r->count < 2) continue;
        if (!r->oneway) continue;                    /* a half of a pair is one-way */
        for (k = 0; k + 1 < r->count; k++) {
            double ax, az, bx, bz, dx, dz, l2, t, cx, cz, lat, d, err, sx, sz, sl, dot;
            if (!td5_geo_roads_point(r, k, &ax, &az) || !td5_geo_roads_point(r, k + 1, &bx, &bz))
                continue;
            dx = bx - ax; dz = bz - az;
            l2 = dx * dx + dz * dz;
            if (l2 < 1.0) continue;
            t = ((x - ax) * dx + (z - az) * dz) / l2;
            if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
            cx = ax + dx * t; cz = az + dz * t;
            /* left of travel is (tz, -tx) */
            lat = (cx - x) * tz - (cz - z) * tx;
            if ((lat < 0.0 ? -1 : 1) != side) continue;
            d = lat < 0.0 ? -lat : lat;
            sl = sqrt(l2);
            sx = dx / sl; sz = dz / sl;
            if (r->oneway_dir < 0) { sx = -sx; sz = -sz; }
            dot = sx * tx + sz * tz;
            if (dot > GEOAV_ANTI_COS) continue;      /* not running against the route */
            err = d > prev_d ? d - prev_d : prev_d - d;
            if (err > GEOAV_STEP_MAX) continue;
            if (d < own_half + (double)(r->lanes > 0 ? r->lanes : 2) * GEOAV_LANE_WIDTH * 0.5
                    + GEOAV_MIN_MEDIAN) continue;    /* the two carriageways would touch */
            if (err < best_err) { best_err = err; best_d = d; best_w = j; }
        }
    }
    if (best_w < 0) return 0;
    *off_out = (double)side * best_d;
    if (lanes_out) { const TD5_GeoRoad *r = td5_geo_roads_get(best_w);
                     *lanes_out = (r && r->lanes > 0 && r->lanes <= 8) ? r->lanes : 2; }
    return 1;
}

static void av_backfill_starts(const char *slug)
{
    int i, total = 0;
    if (!td5_env_flag_on("TD5RE_GEO_AVENUE_BACKFILL")) return;
    if (td5_geo_route_count() < 3) return;
    if (!td5_geo_roads_sync(slug) || td5_geo_roads_count() < 1) return;

    for (i = 0; i < s_n_av; i++) {
        int base = -1, q, name_id = -1, k, added = 0;
        GeoAvSpan add[GEOAV_BACKFILL];
        double prev_d, x, z, xa, za, xb, zb, tx, tz, len;
        int side, lq = 0;
        for (q = 0; q < s_n_rows; q++) if (s_rows[q].av == i) { base = q; break; }
        if (base < 0) continue;
        for (k = 0; k < td5_geo_roads_name_count(); k++)
            if (strcmp(td5_geo_roads_name_by_id(k), s_name[i]) == 0) { name_id = k; break; }
        if (name_id < 0) continue;
        side   = s_rows[base].off < 0.0 ? -1 : 1;
        prev_d = s_rows[base].off < 0.0 ? -s_rows[base].off : s_rows[base].off;
        for (k = 1; k <= GEOAV_BACKFILL; k++) {
            const int sp = s_rows[base].span - k;
            double off = 0.0;
            int lanes = 2, own_lanes = 2;
            if (sp < 1) break;
            if (base > 0 && sp <= s_rows[base - 1].span) break;     /* the previous avenue */
            if (td5_geo_route_node(sp - 1, &xa, &za, &lq) < 0) break;
            td5_geo_route_node(sp,     &x,  &z,  &own_lanes);
            td5_geo_route_node(sp + 1, &xb, &zb, &lq);
            tx = xb - xa; tz = zb - za;
            len = sqrt(tx * tx + tz * tz);
            if (len < 1.0) break;
            tx /= len; tz /= len;
            if (!av_peer_at(name_id, x, z, tx, tz, side, prev_d,
                            (double)(own_lanes > 0 ? own_lanes : 2) * GEOAV_LANE_WIDTH * 0.5,
                            &off, &lanes)) break;
            add[added].span = sp; add[added].av = i; add[added].lanes = lanes;
            add[added].open = 0;  add[added].off = off;
            prev_d = off < 0.0 ? -off : off;
            added++;
        }
        if (added < 1) continue;
        if (s_n_rows + added > GEOAV_MAX_SPANS) continue;
        /* the new rows are DESCENDING in add[]; insert them ascending in front of `base` */
        memmove(&s_rows[base + added], &s_rows[base],
                (size_t)(s_n_rows - base) * sizeof s_rows[0]);
        for (k = 0; k < added; k++) s_rows[base + k] = add[added - 1 - k];
        s_n_rows += added;
        s_s0[i] = s_rows[base].span;
        total += added;
        TD5_LOG_I(LOG_TAG, "avenues: %s starts at span %d, %d span(s) earlier than the file "
                  "(its opposite carriageway is mapped from there)", s_name[i], s_s0[i], added);
    }
    if (total)
        TD5_LOG_I(LOG_TAG, "avenues: %d span(s) back-filled at the start of the avenues", total);
}

/* [ROUND 1015 A item 13] THE MEDIAN OPENINGS, measured on the geometry the race is built
 * from.
 *
 * AVENUES.JSON's `open` flag is written by the route commit (td5_geo_route.c,
 * gr_median_opening_at), which asks "is a differently named way within half a span of the
 * median midline" at a point of the RAW route polyline. Two things make that miss real cross
 * streets. The point is located by arclength FRACTION, and the conditioner smooths and
 * re-parameterises the route, so the point can sit a few spans away from the span it is
 * written against (Calle 46 / Calle 17 across Diagonal 73 land at conditioned spans 398..400
 * and 402..404; the file opens 395, 396 and 399). And half a span is 1.7 m from a street's
 * CENTRE LINE, while a calle is 7 m wide and meets the avenue at 45 degrees: its footprint on
 * the median is three spans long and a sampler that narrow catches one of them or none.
 *
 * So it is recomputed here, at load, from the CONDITIONED route nodes (the very nodes the
 * generator walks) and the same road graph: span s is open when the nearest way that is not
 * the avenue's own street passes within that way's own half carriageway (never under the old
 * half span) of the median midline, at an angle to the avenue of at least GEOAV_OPEN_PARA_DEG
 * (a way running ALONGSIDE the avenue is not a crossing). Needs no re-BUILD: it reads the
 * existing cache. TD5RE_GEO_AVENUE_OPEN_REFINE=0 keeps the file's flags. */
#define GEOAV_OPEN_PARA_COS  0.906          /* cos 25 deg */
static void av_refine_openings(const char *slug)
{
    int i, q, changed = 0, now_open = 0, was_open = 0;
    if (!td5_env_flag_on("TD5RE_GEO_AVENUE_OPEN_REFINE")) return;
    if (td5_geo_route_count() < 3) return;
    if (!td5_geo_roads_sync(slug) || td5_geo_roads_count() < 1) return;

    for (i = 0; i < s_n_av; i++) {
        int name_id = -1, k;
        for (k = 0; k < td5_geo_roads_name_count(); k++)
            if (strcmp(td5_geo_roads_name_by_id(k), s_name[i]) == 0) { name_id = k; break; }
        for (q = 0; q < s_n_rows; q++) {
            GeoAvSpan *g = &s_rows[q];
            double xa, za, xc, zc, xb, zb, tx, tz, len, mx, mz, dx = 0.0, dz = 0.0, dist = 0.0;
            const TD5_GeoRoad *r;
            int op = 0, lq = 0;
            if (g->av != i) continue;
            if (g->span < 1 || g->span + 1 >= td5_geo_route_count()) continue;
            td5_geo_route_node(g->span - 1, &xa, &za, &lq);
            td5_geo_route_node(g->span,     &xc, &zc, &lq);
            td5_geo_route_node(g->span + 1, &xb, &zb, &lq);
            tx = xb - xa; tz = zb - za;
            len = sqrt(tx * tx + tz * tz);
            if (len < 1.0) continue;
            tx /= len; tz /= len;
            /* left of travel is (tz, -tx); `off` is signed, + = left */
            mx = xc + tz * (g->off * 0.5);
            mz = zc - tx * (g->off * 0.5);
            r = td5_geo_roads_nearest(mx, mz, 4.0 * GEOAV_LANE_WIDTH, name_id, &dx, &dz, &dist);
            if (r) {
                const double hw0 = 0.5 * GEOAV_LANE_WIDTH;           /* the old half span */
                double hw = (double)(r->lanes > 0 ? r->lanes : 2) * GEOAV_LANE_WIDTH * 0.5;
                const double dot = dx * tx + dz * tz;
                if (hw < hw0) hw = hw0;
                op = (dist <= hw) && ((dot < 0.0 ? -dot : dot) < GEOAV_OPEN_PARA_COS);
            }
            if (g->open) was_open++;
            if (op) now_open++;
            if ((g->open != 0) != (op != 0)) changed++;
            g->open = op;
        }
    }
    TD5_LOG_I(LOG_TAG, "avenues: median openings re-measured on the conditioned route: "
              "%d span(s) open (the file said %d), %d row(s) changed", now_open, was_open,
              changed);
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
    av_backfill_starts(slug);          /* [ROUND 1015 A item 17] */
    av_refine_openings(slug);          /* [ROUND 1015 A item 13] */
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
