/**
 * td5_roam_lanes.c -- GEO FREE ROAM: bus-lane / bike-lane paint data (PORT-ONLY, roam PA).
 * Contract: td5_roam_lanes.h. The reader of <place>/LANES.JSON (re/tools/geo_lane_paint.py).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "td5_platform.h"
#include "td5_geo.h"
#include "td5_geo_frame.h"
#include "td5_roam_lanes.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "roam"

static struct {
#ifdef _WIN32
    SRWLOCK          lock;
#endif
    char             place[64];
    int              done;
    TD5_RoamLaneWay *w;                 /* sorted by id */
    int              n;
} L;                                    /* zero-initialised: SRWLOCK_INIT is {0} */

static int ln_cmp(const void *a, const void *b)
{
    const long long x = ((const TD5_RoamLaneWay *)a)->id, y = ((const TD5_RoamLaneWay *)b)->id;
    return x < y ? -1 : (x > y);
}

static int ln_kind(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
    if (!s) return TD5_LANE_NONE;
    if (!strcmp(s, "lane")) return TD5_LANE_LANE;
    if (!strcmp(s, "track")) return TD5_LANE_TRACK;
    if (!strcmp(s, "shared")) return TD5_LANE_SHARED;
    return TD5_LANE_NONE;
}

static char *ln_slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (64L << 20)) { fclose(f); return NULL; }
    b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

static void ln_parse(const char *place)
{
    char path[512];
    char *json;
    cJSON *root, *ways, *it;
    int cap, n = 0;
    TD5_RoamLaneWay *w;
    L.w = NULL; L.n = 0;
    td5_geo_source_path(path, sizeof path, place, "LANES.JSON");
    json = ln_slurp(path);
    if (!json) { TD5_LOG_I(LOG_TAG, "ROAM_LANES: no %s (lane paint off)", path); return; }
    root = cJSON_Parse(json);
    free(json);
    if (!root) { TD5_LOG_W(LOG_TAG, "ROAM_LANES: %s does not parse", path); return; }
    td5_geo_src_json_to_game(root);          /* source frame -> game frame (every {x, z} object) */
    ways = cJSON_GetObjectItem(root, "ways");
    cap = cJSON_GetArraySize(ways);
    w = cap > 0 ? (TD5_RoamLaneWay *)calloc((size_t)cap, sizeof *w) : NULL;
    for (it = ways ? ways->child : NULL; it && w && n < cap; it = it->next) {
        TD5_RoamLaneWay *q = &w[n];
        const cJSON *bus = cJSON_GetObjectItem(it, "bus"), *cyc = cJSON_GetObjectItem(it, "cycle");
        const cJSON *pts = cJSON_GetObjectItem(it, "pts");
        int np = cJSON_GetArraySize(pts), k = 0;
        double *px, *pz;
        const cJSON *p;
        q->id = atoll(it->string ? it->string : "0");
        q->bus[0] = (unsigned char)ln_kind(bus, "left");  q->bus[1] = (unsigned char)ln_kind(bus, "right");
        q->cyc[0] = (unsigned char)ln_kind(cyc, "left");  q->cyc[1] = (unsigned char)ln_kind(cyc, "right");
        q->bus_n[0] = (unsigned char)(int)cJSON_GetNumberValue(cJSON_GetObjectItem(bus, "left_n"));
        q->bus_n[1] = (unsigned char)(int)cJSON_GetNumberValue(cJSON_GetObjectItem(bus, "right_n"));
        q->contraflow = cJSON_IsTrue(cJSON_GetObjectItem(bus, "contraflow")) ? 1 : 0;
        if (np < 2) continue;                /* no polyline, no direction: unusable */
        px = (double *)malloc(sizeof(double) * 2 * (size_t)np);
        if (!px) continue;
        pz = px + np;
        for (p = pts->child; p && k < np; p = p->next, k++) {
            px[k] = cJSON_GetNumberValue(cJSON_GetObjectItem(p, "x"));
            pz[k] = cJSON_GetNumberValue(cJSON_GetObjectItem(p, "z"));
        }
        q->np = k; q->px = px; q->pz = pz;
        n++;
    }
    cJSON_Delete(root);
    if (n > 1) qsort(w, (size_t)n, sizeof *w, ln_cmp);
    L.w = w; L.n = n;
    TD5_LOG_I(LOG_TAG, "ROAM_LANES: %s: %d way(s) with a bus / bike lane and a polyline", path, n);
}

const TD5_RoamLaneWay *td5_roam_lanes_way(const char *place, long long way_id, int *n_ways)
{
    const TD5_RoamLaneWay *out = NULL;
    int lo, hi;
    if (n_ways) *n_ways = 0;
    if (!place || !place[0]) return NULL;
#ifdef _WIN32
    AcquireSRWLockExclusive(&L.lock);
#endif
    if (!L.done || strcmp(L.place, place) != 0) {
        snprintf(L.place, sizeof L.place, "%s", place);
        ln_parse(place);                     /* an older list is left allocated: a caller may still hold it */
        L.done = 1;
    }
    lo = 0; hi = L.n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (L.w[mid].id == way_id) { out = &L.w[mid]; break; }
        if (L.w[mid].id < way_id) lo = mid + 1; else hi = mid - 1;
    }
    if (n_ways) *n_ways = L.n;
#ifdef _WIN32
    ReleaseSRWLockExclusive(&L.lock);
#endif
    return out;
}
