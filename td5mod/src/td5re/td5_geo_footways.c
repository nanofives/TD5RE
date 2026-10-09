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

/* The uniform grid that makes the per-span-side query cheap. Sized so the
 * whole place is at most this many cells on a side. MEASURED on la_plata:
 * 3813 segments over a 129x114 grid of 26148 units, i.e. 60.8 m cells -- the
 * rotated route frame spans about 7.8 km, rather more than the fetch radius
 * suggests. A query asks about a few metres, so it touches one or two cells
 * and a few dozen segments instead of all 3813. */
#define GEO_FW_GRID_MAX   128
#define GEO_FW_CELL_MIN_M 10.0

typedef struct { int wi, k; } FwSeg;   /* segment k of way wi */

static struct {
    int             n, np;
    TD5_GeoFootway *way;
    double         *px, *pz;
    int             kind_n[TD5_GEO_FW_KINDS];
    double          upm;               /* units per metre, from PLACE.JSON */
    char            slug[64];
    char            source[320];

    /* segments, and the grid over them */
    FwSeg          *seg;
    int             nseg;
    double          gminx, gminz, gcell;
    int             gnx, gnz;
    int            *gstart;            /* gnx*gnz + 1 */
    int            *gitem;             /* segment indices, bucketed          */
    int             gnitem;
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
    free(s_fw.seg);
    free(s_fw.gstart);
    free(s_fw.gitem);
    memset(&s_fw, 0, sizeof(s_fw));
}

/* units_per_metre for this place, read through the SAME path resolver the
 * footways themselves came through. That matters: source and derived frames
 * each carry their own PLACE.JSON, and a scale taken from the other one would
 * silently mis-convert every metre this module reports. 430 is the value every
 * shipped place uses and the fallback when the field is missing. */
static double fw_units_per_metre(const char *slug)
{
    char path[512];
    char *json;
    cJSON *root, *pr, *u;
    double upm = 430.0;

    td5_geo_place_path(path, sizeof path, slug, "PLACE.JSON");
    json = fw_slurp(path);
    if (!json) return upm;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return upm;
    pr = cJSON_GetObjectItem(root, "projection");
    u  = pr ? cJSON_GetObjectItem(pr, "units_per_metre") : NULL;
    if (u && cJSON_IsNumber(u) && u->valuedouble > 1.0)
        upm = u->valuedouble;
    cJSON_Delete(root);
    return upm;
}

/* Build the segment list and the grid over it. A failure here is NOT fatal:
 * every query falls back to the linear sweep when s_fw.gstart is NULL, so the
 * answers are identical and only the cost changes. */
static void fw_grid_build(void)
{
    double minx, minz, maxx, maxz, ex, ez, span;
    int i, k, c, total;

    if (s_fw.n < 1) return;

    /* 1. flatten every way into segments. */
    s_fw.nseg = 0;
    for (i = 0; i < s_fw.n; i++) s_fw.nseg += s_fw.way[i].count - 1;
    if (s_fw.nseg < 1) { s_fw.nseg = 0; return; }
    s_fw.seg = (FwSeg *)malloc((size_t)s_fw.nseg * sizeof(FwSeg));
    if (!s_fw.seg) { s_fw.nseg = 0; return; }
    {
        int w = 0;
        for (i = 0; i < s_fw.n; i++)
            for (k = 0; k + 1 < s_fw.way[i].count; k++) {
                s_fw.seg[w].wi = i;
                s_fw.seg[w].k  = k;
                w++;
            }
        s_fw.nseg = w;
    }

    /* 2. bounds, from the way bboxes already computed at load. */
    minx = s_fw.way[0].minx; maxx = s_fw.way[0].maxx;
    minz = s_fw.way[0].minz; maxz = s_fw.way[0].maxz;
    for (i = 1; i < s_fw.n; i++) {
        if (s_fw.way[i].minx < minx) minx = s_fw.way[i].minx;
        if (s_fw.way[i].maxx > maxx) maxx = s_fw.way[i].maxx;
        if (s_fw.way[i].minz < minz) minz = s_fw.way[i].minz;
        if (s_fw.way[i].maxz > maxz) maxz = s_fw.way[i].maxz;
    }
    ex = maxx - minx;
    ez = maxz - minz;
    span = (ex > ez) ? ex : ez;
    if (!(span > 0.0)) { free(s_fw.seg); s_fw.seg = NULL; s_fw.nseg = 0; return; }

    s_fw.gcell = span / (double)GEO_FW_GRID_MAX;
    if (s_fw.gcell < GEO_FW_CELL_MIN_M * s_fw.upm)
        s_fw.gcell = GEO_FW_CELL_MIN_M * s_fw.upm;
    s_fw.gminx = minx;
    s_fw.gminz = minz;
    s_fw.gnx = (int)(ex / s_fw.gcell) + 1;
    s_fw.gnz = (int)(ez / s_fw.gcell) + 1;
    if (s_fw.gnx < 1) s_fw.gnx = 1;
    if (s_fw.gnz < 1) s_fw.gnz = 1;
    c = s_fw.gnx * s_fw.gnz;

    /* 3. counting sort. A segment goes in EVERY cell its bbox touches, so a
     * query that scans the cells over its own search box cannot miss one. */
    s_fw.gstart = (int *)calloc((size_t)c + 1u, sizeof(int));
    if (!s_fw.gstart) return;

#define FW_SEG_CELLS(BODY)                                                    \
    for (i = 0; i < s_fw.nseg; i++) {                                         \
        const TD5_GeoFootway *f = &s_fw.way[s_fw.seg[i].wi];                  \
        const int base = f->first + s_fw.seg[i].k;                            \
        const double ax = s_fw.px[base],     az = s_fw.pz[base];              \
        const double bx = s_fw.px[base + 1], bz = s_fw.pz[base + 1];          \
        const double lo_x = (ax < bx) ? ax : bx, hi_x = (ax < bx) ? bx : ax;  \
        const double lo_z = (az < bz) ? az : bz, hi_z = (az < bz) ? bz : az;  \
        int cx0 = (int)((lo_x - s_fw.gminx) / s_fw.gcell);                    \
        int cx1 = (int)((hi_x - s_fw.gminx) / s_fw.gcell);                    \
        int cz0 = (int)((lo_z - s_fw.gminz) / s_fw.gcell);                    \
        int cz1 = (int)((hi_z - s_fw.gminz) / s_fw.gcell);                    \
        int gx, gz;                                                           \
        if (cx0 < 0) cx0 = 0;                                                 \
        if (cz0 < 0) cz0 = 0;                                                 \
        if (cx1 >= s_fw.gnx) cx1 = s_fw.gnx - 1;                              \
        if (cz1 >= s_fw.gnz) cz1 = s_fw.gnz - 1;                              \
        for (gz = cz0; gz <= cz1; gz++)                                       \
            for (gx = cx0; gx <= cx1; gx++) { const int cc = gz * s_fw.gnx + gx; BODY } \
    }

    FW_SEG_CELLS( s_fw.gstart[cc + 1]++; )
    for (k = 0; k < c; k++) s_fw.gstart[k + 1] += s_fw.gstart[k];
    total = s_fw.gstart[c];
    s_fw.gitem = (int *)malloc((size_t)(total > 0 ? total : 1) * sizeof(int));
    if (!s_fw.gitem) { free(s_fw.gstart); s_fw.gstart = NULL; return; }
    {   /* Fill with a moving cursor, then rebuild the starts: the cursor
         * consumes gstart[], which is why it is recomputed rather than saved. */
        int *cur = (int *)malloc((size_t)c * sizeof(int));
        if (!cur) { free(s_fw.gstart); s_fw.gstart = NULL; return; }
        for (k = 0; k < c; k++) cur[k] = s_fw.gstart[k];
        FW_SEG_CELLS( s_fw.gitem[cur[cc]++] = i; )
        free(cur);
    }
#undef FW_SEG_CELLS
    s_fw.gnitem = total;
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
    s_fw.upm = fw_units_per_metre(slug);
    fw_grid_build();
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
        TD5_LOG_I(LOG_TAG, "geo: footways index: %d segment(s) over a %dx%d "
                  "grid of %.0f units (%.1f m)%s; scale %.1f units/m",
                  s_fw.nseg, s_fw.gnx, s_fw.gnz, s_fw.gcell,
                  s_fw.gcell / s_fw.upm,
                  s_fw.gstart ? "" : " [GRID BUILD FAILED -- linear sweep]",
                  s_fw.upm);
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

/* Test one segment against the query and keep it if it is the best so far.
 * Split out so the grid walk and the linear fallback share ONE copy of the
 * acceptance rule -- two copies is how a side test ends up applied on one
 * path and not the other. */
typedef struct {
    double   x, z;             /* query point                               */
    double   max2;             /* squared search radius, world units        */
    unsigned kind_mask;
    int      side;             /* 1 = apply the side + parallel tests       */
    double   nx, nz;           /* unit normal of the side being asked about */
    double   sin_par_max;      /* |sin(angle to the road)| ceiling          */
    /* results */
    double   best2, bx, bz, bw;
    int      bk, hit;
} FwQuery;

static void fw_try_seg(FwQuery *q, int wi, int k)
{
    const TD5_GeoFootway *f = &s_fw.way[wi];
    const int base = f->first + k;
    double ax, az, bx2, bz2, cx, cz, d2;

    if (f->kind < 0 || f->kind >= TD5_GEO_FW_KINDS) return;
    if (!(q->kind_mask & (1u << (unsigned)f->kind))) return;

    ax = s_fw.px[base];      az = s_fw.pz[base];
    bx2 = s_fw.px[base + 1]; bz2 = s_fw.pz[base + 1];

    d2 = fw_seg_d2(q->x, q->z, ax, az, bx2, bz2, &cx, &cz);
    if (d2 >= q->best2) return;

    if (q->side) {
        const double vx = cx - q->x, vz = cz - q->z;
        double dx = bx2 - ax, dz = bz2 - az, len;

        /* ON THAT SIDE. Without this the pavement across the street answers
         * for this one, and a two-sided street reports the same width twice. */
        if (vx * q->nx + vz * q->nz <= 0.0) return;

        /* ROUGHLY PARALLEL TO THE ROAD. The road direction is perpendicular
         * to the normal, so the angle to the road is read off the component
         * of the segment direction ALONG the normal: that component is
         * sin(angle). Without this the footway=crossing line running ACROSS
         * the road at a junction sits a metre away, passes the side test, and
         * collapses the reported pavement width to nothing. */
        len = sqrt(dx * dx + dz * dz);
        if (len <= 0.0) return;
        dx /= len; dz /= len;
        if (fabs(dx * q->nx + dz * q->nz) > q->sin_par_max) return;
    }

    q->best2 = d2;
    q->bx = cx; q->bz = cz;
    q->bw = f->width_m;
    q->bk = f->kind;
    q->hit = 1;
}

/* Walk the grid cells the search box covers, or every segment when the grid
 * could not be built. Both paths call fw_try_seg, so they cannot disagree. */
static void fw_run_query(FwQuery *q, double max_dist)
{
    int i;

    if (s_fw.gstart && s_fw.seg) {
        int cx0 = (int)((q->x - max_dist - s_fw.gminx) / s_fw.gcell);
        int cx1 = (int)((q->x + max_dist - s_fw.gminx) / s_fw.gcell);
        int cz0 = (int)((q->z - max_dist - s_fw.gminz) / s_fw.gcell);
        int cz1 = (int)((q->z + max_dist - s_fw.gminz) / s_fw.gcell);
        int gx, gz;

        if (cx0 < 0) cx0 = 0;
        if (cz0 < 0) cz0 = 0;
        if (cx1 >= s_fw.gnx) cx1 = s_fw.gnx - 1;
        if (cz1 >= s_fw.gnz) cz1 = s_fw.gnz - 1;
        for (gz = cz0; gz <= cz1; gz++)
            for (gx = cx0; gx <= cx1; gx++) {
                const int c = gz * s_fw.gnx + gx;
                int t;
                /* A segment sits in every cell its bbox touches, so it can be
                 * tested more than once here. Harmless: fw_try_seg keeps a
                 * minimum, and the duplicate work is bounded by the segment's
                 * own length in cells. */
                for (t = s_fw.gstart[c]; t < s_fw.gstart[c + 1]; t++) {
                    const FwSeg *s = &s_fw.seg[s_fw.gitem[t]];
                    fw_try_seg(q, s->wi, s->k);
                }
            }
        return;
    }

    for (i = 0; i < s_fw.n; i++) {
        const TD5_GeoFootway *f = &s_fw.way[i];
        int k;
        if (q->x < f->minx - max_dist || q->x > f->maxx + max_dist ||
            q->z < f->minz - max_dist || q->z > f->maxz + max_dist)
            continue;
        for (k = 0; k + 1 < f->count; k++) fw_try_seg(q, i, k);
    }
}

int td5_geo_footways_nearest(double x, double z, double max_dist,
                             unsigned kind_mask, int *out_kind,
                             double *out_dist_u, double *out_width_m,
                             double *out_px, double *out_pz)
{
    FwQuery q;

    if (s_fw.n < 1 || max_dist <= 0.0) return 0;
    memset(&q, 0, sizeof q);
    q.x = x; q.z = z;
    q.max2 = max_dist * max_dist;
    q.best2 = q.max2;
    q.kind_mask = kind_mask;
    q.bk = -1;
    fw_run_query(&q, max_dist);

    if (!q.hit) return 0;
    if (out_kind)    *out_kind    = q.bk;
    if (out_dist_u)  *out_dist_u  = sqrt(q.best2);
    if (out_width_m) *out_width_m = q.bw;
    if (out_px)      *out_px      = q.bx;
    if (out_pz)      *out_pz      = q.bz;
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

/* THE C2 ENTRY POINT. Contract in the header. Metres in and out; the query
 * point stays in world units because the caller holds a world-unit centreline
 * and does not hold the scale. STRONG, so it overrides the weak stub C2 ships
 * in its own branch. */
int td5_geo_footway_sidewalk_near(double x, double z, double nx, double nz,
                                  double max_m, double *out_dist_m,
                                  double *out_width_m)
{
    const double upm = (s_fw.upm > 1.0) ? s_fw.upm : 430.0;
    const double max_dist = max_m * upm;
    double len;
    FwQuery q;

    if (s_fw.n < 1 || max_m <= 0.0) return 0;

    /* The caller promises a unit normal; normalise anyway rather than trust
     * it, because the side and parallel tests are both dot products against
     * it and a vector of length 2 would silently double the parallel
     * tolerance. A zero vector names no side and is refused. */
    len = sqrt(nx * nx + nz * nz);
    if (!(len > 1e-9)) return 0;

    memset(&q, 0, sizeof q);
    q.x = x; q.z = z;
    q.max2 = max_dist * max_dist;
    q.best2 = q.max2;
    q.kind_mask = 1u << TD5_GEO_FW_SIDEWALK;
    q.side = 1;
    q.nx = nx / len;
    q.nz = nz / len;
    q.sin_par_max = sin(TD5_GEO_FW_PARALLEL_DEG * 3.14159265358979323846 / 180.0);
    q.bk = -1;
    fw_run_query(&q, max_dist);

    if (!q.hit) return 0;
    if (out_dist_m)  *out_dist_m  = sqrt(q.best2) / upm;
    if (out_width_m) *out_width_m = q.bw;
    return 1;
}
