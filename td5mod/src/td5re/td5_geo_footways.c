/**
 * td5_geo_footways.c -- GEO TRACK: real OSM pedestrian ways (FOOTWAYS.JSON)
 *                       reader (PORT-ONLY). Contract in td5_geo_footways.h.
 *
 * Reads the `footways` array re/tools/geo_fetch.py writes into
 * re/assets/geo/<slug>/FOOTWAYS.JSON:
 *
 *   {"footways":[{"kind":"sidewalk","class":"footway","footway":"sidewalk",
 *                 "paint":null,"width_m":null,"surface":"paving_stones",
 *                 "lit":false,"points":[{"x":-229630.27,"z":-19756.59}, ...],
 *                 ...}, ...]}
 *
 * Deliberately skipped, for the same reason td5_geo_roads.c skips its own
 * list: `latlon`, `name` and the raw `tags` sub-object. A 873 KB file parses
 * into several MB of cJSON DOM and the pool below is the long-lived copy, so
 * it holds the reduced form -- kind, paint and surface as small ints, width as
 * one double -- and nothing that would have to be freed per record.
 *
 * `kind` AND `paint` ARE READ, NOT RE-DERIVED. geo_fetch resolves both from the
 * raw tags (see _crossing_paint there) and writes the verdict. Re-deriving them
 * here from `class` + `footway` + `crossing` + `crossing:markings` would be a
 * second spelling of one rule, which is the mistake the 2026-09-30 min_height
 * bug was made of. A cache that predates those fields simply reports kind
 * FOOTWAY and paint NONE, which every consumer reads as "nothing special here".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_geo.h"              /* td5_geo_place_path: SOURCE vs DERIVED */
#include "td5_geo_footways.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "geo"

/* Caps. Measured on La Plata (2713 m radius, the densest fixture shipped):
 * 865 ways / 4678 points, longest way 105 points. Sized for roughly 4x that so a
 * denser centre still loads, and ENFORCED rather than trusted -- an oversized
 * file is truncated with a warning, never allowed to run off the pool. */
#define GEO_FW_MAX       4096
#define GEO_FW_MAX_PTS   32768
#define GEO_FW_MAX_FILE  (32 * 1024 * 1024)

static struct {
    int             n, np;
    TD5_GeoFootway *way;
    double         *px, *pz;
    int             kind_n[TD5_GEO_FW_KINDS];
    char            slug[64];
    char            source[320];
} s_fw;

/* ------------------------------------------------------------------- io --- */

/* Same shape as td5_geo_roads.c's geo_roads_slurp, which is static there and
 * carries its own (larger) cap. Duplicated rather than exported so neither
 * module gains surface for the other's benefit. */
static char *fw_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;

    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)GEO_FW_MAX_FILE) {
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

static const char *fw_str(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (v && cJSON_IsString(v) && v->valuestring[0]) ? v->valuestring : NULL;
}

static int fw_bool(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    if (!v) return 0;
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v) ? 1 : 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0 ? 1 : 0;
    return 0;
}

static int fw_int(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (v && cJSON_IsNumber(v)) ? v->valueint : 0;
}

static int fw_kind_of(const char *s)
{
    if (!s)                            return TD5_GEO_FW_FOOTWAY;
    if (!strcmp(s, "sidewalk"))        return TD5_GEO_FW_SIDEWALK;
    if (!strcmp(s, "crossing"))        return TD5_GEO_FW_CROSSING;
    if (!strcmp(s, "steps"))           return TD5_GEO_FW_STEPS;
    if (!strcmp(s, "cycleway"))        return TD5_GEO_FW_CYCLEWAY;
    if (!strcmp(s, "path"))            return TD5_GEO_FW_PATH;
    if (!strcmp(s, "pedestrian"))      return TD5_GEO_FW_PEDESTRIAN;
    return TD5_GEO_FW_FOOTWAY;         /* "footway", and anything new */
}

static int fw_paint_of(const char *s)
{
    if (!s)                            return TD5_GEO_XP_NONE;
    if (!strcmp(s, "marked"))          return TD5_GEO_XP_MARKED;
    if (!strcmp(s, "unmarked"))        return TD5_GEO_XP_UNMARKED;
    if (!strcmp(s, "signals"))         return TD5_GEO_XP_SIGNALS;
    if (!strcmp(s, "unknown"))         return TD5_GEO_XP_UNKNOWN;
    return TD5_GEO_XP_UNKNOWN;
}

/* The same three buckets geo_roads_surface uses, over the values that actually
 * occur on a pavement. La Plata: 619 untagged, paving_stones 121, concrete 33,
 * asphalt 23, sett 22, dirt 12. An untagged pavement is SMOOTH, which is what
 * the generator already assumes everywhere. */
static int fw_surface_of(const char *s)
{
    if (!s) return TD5_GEO_FW_SURF_SMOOTH;
    if (!strcmp(s, "sett") || !strcmp(s, "cobblestone") ||
        !strcmp(s, "unhewn_cobblestone") || !strcmp(s, "pebblestone") ||
        !strcmp(s, "bricks"))
        return TD5_GEO_FW_SURF_COBBLE;
    if (!strcmp(s, "dirt") || !strcmp(s, "ground") || !strcmp(s, "earth") ||
        !strcmp(s, "gravel") || !strcmp(s, "sand") || !strcmp(s, "grass") ||
        !strcmp(s, "fine_gravel") || !strcmp(s, "compacted") ||
        !strcmp(s, "mud") || !strcmp(s, "woodchips"))
        return TD5_GEO_FW_SURF_LOOSE;
    return TD5_GEO_FW_SURF_SMOOTH;     /* asphalt, concrete, paving_stones... */
}

/* ----------------------------------------------------------------- load --- */

void td5_geo_footways_unload(void)
{
    free(s_fw.way);
    free(s_fw.px);
    free(s_fw.pz);
    memset(&s_fw, 0, sizeof(s_fw));
}

static int fw_load(const char *slug)
{
    char path[320];
    char *json;
    cJSON *root, *arr;
    int n, i, dropped_short = 0, truncated = 0;

    td5_geo_footways_unload();
    td5_geo_place_path(path, sizeof path, slug, "FOOTWAYS.JSON");
    json = fw_slurp(path);
    if (!json) {
        /* NOT an error, and said once at INFO so a later census of zero is
         * attributable to the CACHE rather than to a gate. Every place fetched
         * before tag_schema 3 lands here. */
        TD5_LOG_I(LOG_TAG, "geo: no %s (this place carries no mapped "
                  "pavements -- cache predates tag_schema 3)", path);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_W(LOG_TAG, "geo: %s is not valid JSON", path);
        return 0;
    }
    arr = cJSON_GetObjectItem(root, "footways");
    if (!arr || !cJSON_IsArray(arr) || (n = cJSON_GetArraySize(arr)) < 1) {
        TD5_LOG_W(LOG_TAG, "geo: %s has no footways[]", path);
        cJSON_Delete(root);
        return 0;
    }
    if (n > GEO_FW_MAX) { n = GEO_FW_MAX; truncated = 1; }

    s_fw.way = (TD5_GeoFootway *)malloc((size_t)n * sizeof(TD5_GeoFootway));
    s_fw.px  = (double *)malloc((size_t)GEO_FW_MAX_PTS * sizeof(double));
    s_fw.pz  = (double *)malloc((size_t)GEO_FW_MAX_PTS * sizeof(double));
    if (!s_fw.way || !s_fw.px || !s_fw.pz) {
        TD5_LOG_E(LOG_TAG, "geo: out of memory for %d footway(s)", n);
        cJSON_Delete(root);
        td5_geo_footways_unload();
        return 0;
    }

    for (i = 0; i < n; i++) {
        const cJSON *r   = cJSON_GetArrayItem(arr, i);
        const cJSON *pts = r ? cJSON_GetObjectItem(r, "points") : NULL;
        const cJSON *w   = r ? cJSON_GetObjectItem(r, "width_m") : NULL;
        TD5_GeoFootway *out;
        int k, npt, kept = 0;

        if (!pts || !cJSON_IsArray(pts)) continue;
        npt = cJSON_GetArraySize(pts);
        if (npt < 2) { dropped_short++; continue; }
        if (npt > TD5_GEO_FOOTWAYS_MAX_WAY) {
            npt = TD5_GEO_FOOTWAYS_MAX_WAY;
            truncated = 1;
        }
        if (s_fw.np + npt > GEO_FW_MAX_PTS) { truncated = 1; break; }

        out = &s_fw.way[s_fw.n];
        memset(out, 0, sizeof(*out));
        out->first = s_fw.np;
        for (k = 0; k < npt; k++) {
            const cJSON *p  = cJSON_GetArrayItem(pts, k);
            const cJSON *px = p ? cJSON_GetObjectItem(p, "x") : NULL;
            const cJSON *pz = p ? cJSON_GetObjectItem(p, "z") : NULL;
            double x, z;

            if (!px || !pz || !cJSON_IsNumber(px) || !cJSON_IsNumber(pz))
                continue;
            x = px->valuedouble;
            z = pz->valuedouble;
            /* Drop a repeated vertex. A zero-length segment has no bearing,
             * and the nearest-point walk below divides by its length. */
            if (kept > 0) {
                const double dx = x - s_fw.px[out->first + kept - 1];
                const double dz = z - s_fw.pz[out->first + kept - 1];
                if (dx * dx + dz * dz < 1.0) continue;
            }
            s_fw.px[out->first + kept] = x;
            s_fw.pz[out->first + kept] = z;
            if (kept == 0) {
                out->minx = out->maxx = x;
                out->minz = out->maxz = z;
            } else {
                if (x < out->minx) out->minx = x;
                if (x > out->maxx) out->maxx = x;
                if (z < out->minz) out->minz = z;
                if (z > out->maxz) out->maxz = z;
            }
            kept++;
        }
        if (kept < 2) { dropped_short++; continue; }

        out->count      = kept;
        out->kind       = fw_kind_of(fw_str(r, "kind"));
        out->paint      = fw_paint_of(fw_str(r, "paint"));
        out->surface    = fw_surface_of(fw_str(r, "surface"));
        out->lit        = fw_bool(r, "lit");
        out->area       = fw_bool(r, "area");
        out->bridge     = fw_bool(r, "bridge");
        out->tunnel     = fw_bool(r, "tunnel");
        out->covered    = fw_bool(r, "covered");
        out->layer      = fw_int(r, "layer");
        out->step_count = fw_int(r, "step_count");
        /* geo_fetch parses the free-text OSM width and clamps it to
         * 0.2..40 m, so a number here is already sane. Re-checked anyway
         * because a hand-edited cache is a real thing. */
        out->width_m = (w && cJSON_IsNumber(w) && w->valuedouble > 0.0
                        && w->valuedouble < 100.0) ? w->valuedouble : 0.0;

        if (out->kind >= 0 && out->kind < TD5_GEO_FW_KINDS)
            s_fw.kind_n[out->kind]++;
        s_fw.np += kept;
        s_fw.n++;
    }
    cJSON_Delete(root);

    if (s_fw.n < 1) {
        TD5_LOG_W(LOG_TAG, "geo: %s held no usable footway", path);
        td5_geo_footways_unload();
        return 0;
    }
    snprintf(s_fw.slug, sizeof s_fw.slug, "%s", slug);
    snprintf(s_fw.source, sizeof s_fw.source, "%s", path);
    {
        int nw = 0, i2;
        for (i2 = 0; i2 < s_fw.n; i2++)
            if (s_fw.way[i2].width_m > 0.0) nw++;
        TD5_LOG_I(LOG_TAG, "geo: footways %s loaded: %d way(s) %d point(s)%s "
                  "(%d too short) | footway=%d sidewalk=%d crossing=%d "
                  "steps=%d cycleway=%d path=%d pedestrian=%d | %d with a "
                  "measured width",
                  path, s_fw.n, s_fw.np,
                  truncated ? " [TRUNCATED at a cap]" : "", dropped_short,
                  s_fw.kind_n[TD5_GEO_FW_FOOTWAY],
                  s_fw.kind_n[TD5_GEO_FW_SIDEWALK],
                  s_fw.kind_n[TD5_GEO_FW_CROSSING],
                  s_fw.kind_n[TD5_GEO_FW_STEPS],
                  s_fw.kind_n[TD5_GEO_FW_CYCLEWAY],
                  s_fw.kind_n[TD5_GEO_FW_PATH],
                  s_fw.kind_n[TD5_GEO_FW_PEDESTRIAN], nw);
    }
    return 1;
}

int td5_geo_footways_sync(const char *slug)
{
    if (!slug) slug = "";
    if (!slug[0]) {
        if (s_fw.n) td5_geo_footways_unload();
        return 0;
    }
    if (s_fw.n && strcmp(s_fw.slug, slug) == 0)
        return s_fw.n;                  /* already this place */
    fw_load(slug);
    return s_fw.n;
}

/* --------------------------------------------------------------- access --- */

int td5_geo_footways_count(void)  { return s_fw.n; }
int td5_geo_footways_points(void) { return s_fw.np; }

const char *td5_geo_footways_source(void)
{
    return s_fw.source[0] ? s_fw.source : "";
}

const TD5_GeoFootway *td5_geo_footways_get(int i)
{
    if (i < 0 || i >= s_fw.n) return NULL;
    return &s_fw.way[i];
}

int td5_geo_footways_point(const TD5_GeoFootway *f, int k, double *x, double *z)
{
    if (!f || k < 0 || k >= f->count) return 0;
    if (x) *x = s_fw.px[f->first + k];
    if (z) *z = s_fw.pz[f->first + k];
    return 1;
}

int td5_geo_footways_kind_count(int kind)
{
    if (kind < 0 || kind >= TD5_GEO_FW_KINDS) return 0;
    return s_fw.kind_n[kind];
}

/* ---------------------------------------------------------- the search --- */

/* Squared distance from (x,z) to segment (ax,az)-(bx,bz), and the closest
 * point on it. The ordinary projection-and-clamp; written out rather than
 * shared with td5_geo_roads.c's because that one is static there and folding
 * the two would export a helper neither module wants in its header. */
static double fw_seg_d2(double x, double z, double ax, double az,
                        double bx, double bz, double *cx, double *cz)
{
    const double dx = bx - ax, dz = bz - az;
    const double len2 = dx * dx + dz * dz;
    double t, px, pz;

    if (len2 <= 0.0) {
        px = ax; pz = az;
    } else {
        t = ((x - ax) * dx + (z - az) * dz) / len2;
        if (t < 0.0) t = 0.0;
        else if (t > 1.0) t = 1.0;
        px = ax + dx * t;
        pz = az + dz * t;
    }
    if (cx) *cx = px;
    if (cz) *cz = pz;
    return (x - px) * (x - px) + (z - pz) * (z - pz);
}

int td5_geo_footways_nearest(double x, double z, double max_dist,
                             unsigned kind_mask, int *out_kind,
                             double *out_dist_u, double *out_width_m,
                             double *out_px, double *out_pz)
{
    const double max2 = max_dist * max_dist;
    double best2 = max2, bx = 0.0, bz = 0.0, bw = 0.0;
    int bk = -1, i, hit = 0;

    if (s_fw.n < 1 || max_dist <= 0.0) return 0;

    for (i = 0; i < s_fw.n; i++) {
        const TD5_GeoFootway *f = &s_fw.way[i];
        int k;

        if (f->kind < 0 || f->kind >= TD5_GEO_FW_KINDS) continue;
        if (!(kind_mask & (1u << (unsigned)f->kind))) continue;
        /* Bbox reject, grown by the search radius. This is what keeps an
         * O(ways x points) sweep affordable enough to run in a prepass. */
        if (x < f->minx - max_dist || x > f->maxx + max_dist ||
            z < f->minz - max_dist || z > f->maxz + max_dist)
            continue;

        for (k = 0; k + 1 < f->count; k++) {
            double cx, cz;
            const double d2 = fw_seg_d2(x, z,
                                        s_fw.px[f->first + k],
                                        s_fw.pz[f->first + k],
                                        s_fw.px[f->first + k + 1],
                                        s_fw.pz[f->first + k + 1], &cx, &cz);
            if (d2 < best2) {
                best2 = d2;
                bx = cx; bz = cz;
                bw = f->width_m;
                bk = f->kind;
                hit = 1;
            }
        }
    }
    if (!hit) return 0;
    if (out_kind)    *out_kind    = bk;
    if (out_dist_u)  *out_dist_u  = sqrt(best2);
    if (out_width_m) *out_width_m = bw;
    if (out_px)      *out_px      = bx;
    if (out_pz)      *out_pz      = bz;
    return 1;
}

int td5_geo_footways_sidewalk_nearest(double x, double z, double max_dist,
                                      double *out_dist_u, double *out_width_m,
                                      double *out_px, double *out_pz)
{
    return td5_geo_footways_nearest(x, z, max_dist,
                                    1u << TD5_GEO_FW_SIDEWALK, NULL,
                                    out_dist_u, out_width_m, out_px, out_pz);
}
