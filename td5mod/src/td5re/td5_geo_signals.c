/**
 * td5_geo_signals.c -- GEO TRACK: traffic-signal nodes + the lamp cycle
 *                      (PORT-ONLY). Contract and rationale in td5_geo_signals.h.
 *
 * Reads re/assets/geo/<slug>/SIGNALS.JSON, the file geo_fetch.py writes from
 * OSM highway=traffic_signals nodes:
 *
 *   { "signals": [ { "x": <world>, "z": <world>, "direction": null }, ... ] }
 *
 * `direction` is the approach bearing where OSM tags one. Every La Plata entry
 * is null (566 of 566), so it is READ BUT NOT USED: the emitter takes its
 * facing from the route span's own tangent instead, which is the direction
 * traffic actually arrives from on the drivable line and is available for every
 * node rather than the tagged minority. The field is parsed anyway so a place
 * that does carry it fails loudly on a type change rather than silently.
 *
 * 2026-10-07, the tag round (docs/plans/GEO_TAG_AUDIT.md). The Overpass query
 * used to ask only for `node[highway=traffic_signals]`, so crossings, stop
 * lines, give-ways and speed humps were 0 of 566 nodes -- not absent from the
 * city, never requested. They are fetched now, and they go into a SEPARATE
 * `nodes[]` array in the same file rather than into `signals[]`, because this
 * module masts a traffic light at every `signals[]` entry: folding them in
 * would grow a lamp on every zebra crossing in La Plata. `signals[]`
 * membership is therefore byte-identical to before.
 *
 * This reader counts `nodes[]` and reports it, and reads nothing from it. A
 * count in the log is how the next workstream finds out the data is already on
 * disk; a silent array is how it gets fetched twice.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_geo.h"
#include "td5_geo_signals.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "geo"

/* A place with more signals than this is not a place, it is a parse error. The
 * whole of La Plata inside a 2.7 km radius is 566. */
#define GEO_SIGNALS_MAX 20000

typedef struct {
    double x, z;
} GeoSignal;

static GeoSignal *s_sig;
static int        s_sig_count;
static char       s_sig_slug[64];
static char       s_sig_source[512];

/* Read a whole file into a malloc'd NUL-terminated buffer, for cJSON. Same
 * shape as td5_geo.c's geo_slurp; duplicated rather than exported because that
 * one is file-static and this module is meant to add no surface to td5_geo.h. */
static char *sig_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;

    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)8 * 1024 * 1024) {
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

void td5_geo_signals_unload(void)
{
    free(s_sig);
    s_sig = NULL;
    s_sig_count = 0;
    s_sig_slug[0] = '\0';
    s_sig_source[0] = '\0';
}

int td5_geo_signals_count(void) { return s_sig_count; }

const char *td5_geo_signals_source(void) { return s_sig_source; }

int td5_geo_signals_get(int i, double *x, double *z)
{
    if (i < 0 || i >= s_sig_count) return 0;
    if (x) *x = s_sig[i].x;
    if (z) *z = s_sig[i].z;
    return 1;
}

int td5_geo_signals_sync(void)
{
    const char *slug = td5_geo_loaded() ? td5_geo_place_slug() : "";
    char path[512];
    char *json;
    cJSON *root = NULL;
    const cJSON *arr;
    int n, i, kept = 0, bad = 0, other = 0;

    if (!slug || !slug[0]) {           /* synthetic build: nothing to read */
        td5_geo_signals_unload();
        return 0;
    }
    if (s_sig_slug[0] && strcmp(s_sig_slug, slug) == 0)
        return s_sig_count;            /* already this place */

    td5_geo_signals_unload();
    snprintf(path, sizeof path, "re/assets/geo/%s/SIGNALS.JSON", slug);

    json = sig_slurp(path);
    if (!json) {
        /* A place is allowed to carry no signals. Say so once, at INFO, so a
         * zero census below is attributable to the DATA and not to a gate. */
        TD5_LOG_I(LOG_TAG, "signals: no %s (place has no traffic-signal data)",
                  path);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_W(LOG_TAG, "signals: %s is not valid JSON", path);
        return 0;
    }

    {   /* Counted before the signals walk so an early return still reports
         * it: a cache at tag_schema 1 simply has no `nodes` and reports 0. */
        const cJSON *nd = cJSON_GetObjectItem(root, "nodes");
        if (nd && cJSON_IsArray(nd)) other = cJSON_GetArraySize(nd);
    }
    arr = cJSON_GetObjectItem(root, "signals");
    if (!arr || !cJSON_IsArray(arr)) {
        TD5_LOG_W(LOG_TAG, "signals: %s has no \"signals\" array", path);
        cJSON_Delete(root);
        return 0;
    }
    n = cJSON_GetArraySize(arr);
    if (n <= 0) {
        cJSON_Delete(root);
        return 0;
    }
    if (n > GEO_SIGNALS_MAX) {
        TD5_LOG_W(LOG_TAG, "signals: %s claims %d nodes (cap %d) -- refusing",
                  path, n, GEO_SIGNALS_MAX);
        cJSON_Delete(root);
        return 0;
    }

    s_sig = (GeoSignal *)malloc((size_t)n * sizeof(GeoSignal));
    if (!s_sig) {
        cJSON_Delete(root);
        return 0;
    }

    for (i = 0; i < n; i++) {
        const cJSON *e  = cJSON_GetArrayItem(arr, i);
        const cJSON *px = e ? cJSON_GetObjectItem(e, "x") : NULL;
        const cJSON *pz = e ? cJSON_GetObjectItem(e, "z") : NULL;
        const cJSON *pd = e ? cJSON_GetObjectItem(e, "direction") : NULL;

        if (!px || !pz || !cJSON_IsNumber(px) || !cJSON_IsNumber(pz)) {
            bad++;
            continue;
        }
        /* Parsed to keep the contract honest: null and absent are both fine
         * (every shipped place is null today), a number is fine and reserved
         * for a future per-node facing, anything else is a format change we
         * want to hear about rather than ignore. */
        if (pd && !cJSON_IsNull(pd) && !cJSON_IsNumber(pd))
            bad++;

        s_sig[kept].x = px->valuedouble;
        s_sig[kept].z = pz->valuedouble;
        kept++;
    }
    cJSON_Delete(root);

    s_sig_count = kept;
    snprintf(s_sig_slug, sizeof s_sig_slug, "%s", slug);
    snprintf(s_sig_source, sizeof s_sig_source, "%s", path);

    TD5_LOG_I(LOG_TAG, "signals: %d node(s) from %s%s",
              s_sig_count, path,
              bad ? " (some entries skipped, see the file)" : "");
    if (bad)
        TD5_LOG_W(LOG_TAG, "signals: %d of %d entries were malformed", bad, n);
    if (other > 0)
        TD5_LOG_I(LOG_TAG, "signals: the cache also carries %d non-signal "
                  "highway node(s) (crossings / stop / give_way / humps) in "
                  "nodes[]; nothing reads them yet -- they are NOT masted as "
                  "lamps, which is why they are a separate array", other);
    return s_sig_count;
}

/* ------------------------------------------------------------- run time --- */

/* One full cycle, seconds. Phase boundaries are the ordinary four-step
 * sequence a driver expects -- red, red-to-green amber, green, green-to-red
 * amber -- rather than an even three-way split, which reads as a decoration
 * rather than a traffic light. Nothing depends on these numbers: no AI or
 * physics code can see the lamp state, so they are chosen to look right. */
#define SIG_T_RED_END    4.0f
#define SIG_T_AMBER1_END 5.5f
#define SIG_T_GREEN_END  9.5f
#define SIG_T_PERIOD    11.0f

/* Lit and unlit brightness. The lamp quads sit on an ADDITIVE page, so an
 * unlit lamp must not be black-and-invisible (the housing behind it would show
 * a hole where a lens belongs) nor bright enough to read as a second lit lamp.
 * 12% is the value that still shows three distinct lenses at head-on distance
 * without any of them competing with the lit one. */
#define SIG_GAIN_ON   1.00f
#define SIG_GAIN_OFF  0.12f

/* Base colours, ARGB. Amber is pulled toward red rather than yellow because an
 * additive blend washes toward white as it brightens. */
static const unsigned int k_sig_rgb[TD5_GEO_SIGNAL_PHASES] = {
    0x00FF2010u,   /* 0 red   */
    0x00FFA000u,   /* 1 amber */
    0x0030FF40u    /* 2 green */
};

/* Monotonic seconds, wrapped well inside float precision. Same idiom as
 * td5_vfx_anim_time() (td5_vfx.c): a wall clock, NOT the simulation tick, so a
 * paused or rewound race does not freeze or rewind the lights and nothing in
 * the cycle can ever feed back into the sim. */
static float sig_now_s(void)
{
    return (float)(td5_plat_time_us() % 3600000000ULL) * 1.0e-6f;
}

int td5_geo_signal_lamp_phase_now(void)
{
    const float t = fmodf(sig_now_s(), SIG_T_PERIOD);
    if (t < SIG_T_RED_END)    return 0;   /* red   */
    if (t < SIG_T_AMBER1_END) return 1;   /* amber, red -> green */
    if (t < SIG_T_GREEN_END)  return 2;   /* green */
    return 1;                             /* amber, green -> red */
}

unsigned int td5_geo_signal_lamp_colour(int phase)
{
    unsigned int rgb;
    float gain;
    unsigned int r, g, b;

    if (phase < 0 || phase >= TD5_GEO_SIGNAL_PHASES) return 0xFF000000u;

    rgb  = k_sig_rgb[phase];
    gain = (phase == td5_geo_signal_lamp_phase_now()) ? SIG_GAIN_ON
                                                      : SIG_GAIN_OFF;
    r = (unsigned int)((float)((rgb >> 16) & 0xFFu) * gain);
    g = (unsigned int)((float)((rgb >>  8) & 0xFFu) * gain);
    b = (unsigned int)((float)( rgb        & 0xFFu) * gain);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}
