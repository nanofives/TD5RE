/**
 * td5_roam_handoff.c -- GEO FREE ROAM: a route handed from the frontend to the next roam session
 * (PORT-ONLY, F11a). Contract: td5_roam_handoff.h.
 *
 * Two phases once the session is active: PLANT (the planner graph is ready: the waypoints go in, the
 * planner region grows round them) and PLANNED (every leg planned: save, then the setup panel or the
 * race). A route that never plans leaves the player in free roam with the pins on the map, so the
 * route map shows what went wrong instead of a silent nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_roam_mode.h"
#include "td5_roam_route.h"
#include "td5_roam_race.h"
#include "td5_roam_ai.h"
#include "td5_roam_handoff.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "roam"

/* How long PLANNED may take: a partido route waits for its region's tiles on the stream worker
 * (F9b measured ~25 s for the 27 km City Bell route). */
#define HO_PLAN_TIMEOUT_MS  120000u

static struct {
    int      pending;
    int      phase;            /* 0 wait for the graph, 1 wait for the plan */
    double   ll[TD5_ROUTE_MAX_WP][2];
    int      n;
    int      type, laps, mode;
    int      opponents, traffic, cops;
    uint32_t t_set, t_plant, t_active;   /* t_active: the first frame of the active session */
    int      frames;
} H;

int td5_roam_strip_build_enabled(void)
{
    return td5_env_int("TD5RE_GEO_STRIP_BUILD", 0, 0, 1);
}

int td5_roam_handoff_set(const double (*ll)[2], int n, int type, int laps, int mode,
                         int opponents, int traffic, int cops)
{
    int i;
    if (!ll || n < 2 || n > TD5_ROUTE_MAX_WP) {
        TD5_LOG_W(LOG_TAG, "HANDOFF: refused %d waypoint(s) (2..%d)", n, TD5_ROUTE_MAX_WP);
        return 0;
    }
    memset(&H, 0, sizeof H);
    for (i = 0; i < n; i++) { H.ll[i][0] = ll[i][0]; H.ll[i][1] = ll[i][1]; }
    H.n = n;
    H.type = (type == TD5_ROUTE_CIRCUIT) ? TD5_ROUTE_CIRCUIT : TD5_ROUTE_SPRINT;
    H.laps = laps < 1 ? 1 : (laps > TD5_ROUTE_MAX_LAPS ? TD5_ROUTE_MAX_LAPS : laps);
    H.mode = mode;
    H.opponents = opponents; H.traffic = traffic; H.cops = cops;
    H.t_set = td5_plat_time_ms();
    H.pending = 1;
    TD5_LOG_I(LOG_TAG, "HANDOFF: %d waypoint(s) %s laps=%d -> %s (opponents=%d traffic=%d cops=%d)", n,
              H.type == TD5_ROUTE_CIRCUIT ? "circuit" : "sprint", H.laps,
              mode == TD5_HANDOFF_AUTOSTART ? "autostart" : "setup panel", opponents, traffic, cops);
    return 1;
}

void td5_roam_handoff_clear(void)
{
    if (H.pending) TD5_LOG_I(LOG_TAG, "HANDOFF: dropped (phase %d)", H.phase);
    memset(&H, 0, sizeof H);
}

int td5_roam_handoff_pending(void) { return H.pending; }

static int ho_plant(void)
{
    int i, ok = 1;
    td5_roam_route_clear();
    for (i = 0; i < H.n && ok; i++) {
        int32_t px, pz;
        if (!td5_roam_route_latlon_to_place(H.ll[i][0], H.ll[i][1], &px, &pz)) { ok = 0; break; }
        if (i == 0) ok = td5_roam_route_set_start_at(px, pz);
        else if (i == H.n - 1) ok = td5_roam_route_set_finish_at(px, pz);
        else ok = td5_roam_route_add_at(px, pz);
        if (!ok)
            TD5_LOG_W(LOG_TAG, "HANDOFF: waypoint %d (%.6f,%.6f) has no street within reach", i, H.ll[i][0], H.ll[i][1]);
    }
    /* clear keeps the planner's type: say it every time (F8b: a circuit left over raced a sprint as 3 laps) */
    if (ok) td5_roam_route_set_type(H.type, H.laps);
    return ok;
}

void td5_roam_handoff_frame(void)
{
    const TD5_RouteResult *r;
    if (!H.pending || !td5_roam_active()) return;
    H.frames++;
    if (H.frames == 1) H.t_active = td5_plat_time_ms();
    if (H.phase == 0) {
        if (!td5_roam_route_graph_ready() || H.frames < 5) return;
        if (!ho_plant()) {
            TD5_LOG_W(LOG_TAG, "HANDOFF: the route could not be placed; free roam goes on (open the map with M)");
            H.pending = 0;
            return;
        }
        H.phase = 1;
        H.t_plant = td5_plat_time_ms();
        TD5_LOG_I(LOG_TAG, "HANDOFF: %d waypoint(s) planted %u ms after the session's first frame (%d planner tile(s) pending)",
                  td5_roam_route_wp_count(), (unsigned)(H.t_plant - H.t_active), td5_roam_route_graph_pending());
        return;
    }
    r = td5_roam_route_result();
    if (!r->ok || (H.type == TD5_ROUTE_CIRCUIT && !r->closed)) {
        if (td5_plat_time_ms() - H.t_plant > HO_PLAN_TIMEOUT_MS) {
            TD5_LOG_W(LOG_TAG, "HANDOFF: the route never planned (%d of %d legs) in %u s; free roam goes on with the pins on the map",
                      r->legs_ok, r->legs, HO_PLAN_TIMEOUT_MS / 1000u);
            H.pending = 0;
        }
        return;
    }
    H.pending = 0;
    TD5_LOG_I(LOG_TAG, "HANDOFF: route planned %.1f m, %d edges, hash %08X, %u ms after planting",
              r->length_m, r->nedge, (unsigned)r->hash, (unsigned)(td5_plat_time_ms() - H.t_plant));
    if (!td5_roam_route_save(TD5_HANDOFF_ROUTE_NAME))
        TD5_LOG_W(LOG_TAG, "HANDOFF: could not save the route as '%s' (racing it unsaved)", TD5_HANDOFF_ROUTE_NAME);
    if (H.mode == TD5_HANDOFF_AUTOSTART) {
        TD5_RoamRaceSetup s;
        memset(&s, 0, sizeof s);
        s.laps = H.laps;
        s.opponents = H.opponents < 0 ? td5_roam_ai_default_opponents() : H.opponents;
        if (s.opponents > td5_roam_ai_available()) s.opponents = td5_roam_ai_available();
        s.traffic = H.traffic < 0 ? 0 : (H.traffic > 2 ? 2 : H.traffic);
        s.cops = H.cops ? 1 : 0;
        if (!td5_roam_race_start(&s))
            TD5_LOG_W(LOG_TAG, "HANDOFF: race start refused; free roam goes on");
        else
            TD5_LOG_I(LOG_TAG, "HANDOFF: race started %u ms after the session's first frame", (unsigned)(td5_plat_time_ms() - H.t_active));
    } else if (!td5_roam_race_open_setup()) {
        TD5_LOG_W(LOG_TAG, "HANDOFF: the setup panel refused the route; free roam goes on");
    } else {
        TD5_LOG_I(LOG_TAG, "HANDOFF: setup panel open %u ms after the session's first frame", (unsigned)(td5_plat_time_ms() - H.t_active));
    }
}

int td5_roam_handoff_read_waypoints(const char *path, double (*ll)[2], int max)
{
    TD5_File *f;
    int64_t sz;
    char *txt;
    cJSON *root, *wps, *w;
    int n = 0;
    if (!path || !ll || max <= 0) return 0;
    f = td5_plat_file_open(path, "rb");
    if (!f) return 0;
    sz = td5_plat_file_size(f);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { td5_plat_file_close(f); return 0; }
    txt = (char *)malloc((size_t)sz + 1u);
    if (!txt) { td5_plat_file_close(f); return 0; }
    if (td5_plat_file_read(f, txt, (size_t)sz) != (size_t)sz) { free(txt); td5_plat_file_close(f); return 0; }
    txt[sz] = '\0';
    td5_plat_file_close(f);
    root = cJSON_Parse(txt);
    free(txt);
    if (!root) return 0;
    wps = cJSON_GetObjectItem(root, "waypoints");
    if (cJSON_IsArray(wps)) {
        cJSON_ArrayForEach(w, wps) {
            cJSON *la = NULL, *lo = NULL;
            if (n >= max) break;
            if (cJSON_IsArray(w)) { la = cJSON_GetArrayItem(w, 0); lo = cJSON_GetArrayItem(w, 1); }
            else if (cJSON_IsObject(w)) { la = cJSON_GetObjectItem(w, "lat"); lo = cJSON_GetObjectItem(w, "lon"); }
            if (!cJSON_IsNumber(la) || !cJSON_IsNumber(lo)) continue;
            ll[n][0] = la->valuedouble;
            ll[n][1] = lo->valuedouble;
            n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

int td5_roam_handoff_last_route(const char *place, double (*ll)[2], int max, char *src, size_t src_cap)
{
    char path[512];
    int n;
    if (!place || !place[0]) return 0;
    if (td5_geo_source_path(path, sizeof path, place, "routes/" TD5_HANDOFF_ROUTE_NAME ".json")) {
        n = td5_roam_handoff_read_waypoints(path, ll, max);
        if (n >= 2) { if (src) snprintf(src, src_cap, "%s", path); return n; }
    }
    return 0;   /* [F11b] no fallback to a strip BUILD's _route/ROUTE_RAW.JSON (D4) */
}
