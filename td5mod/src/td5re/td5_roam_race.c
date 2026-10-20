/**
 * td5_roam_race.c -- GEO FREE ROAM: the sprint race flow (PORT-ONLY, F6-B).
 * Contract: td5_roam_race.h. Route model: td5_roam_route.h (F5-A). Barriers: td5_roam_barrier.h (F6-A).
 * The racer-only barrier collision layer is td5_roam_race_wall.c, the DEV self-test driver
 * td5_roam_race_drive.c.
 *
 * PROGRESS. The race keeps its OWN copy of the route polyline (place frame) taken at START, so the
 * planner can be edited, saved or cleared mid-race without moving the finish line. The car's
 * distance along it is the projection on the nearest polyline segment inside a window round the
 * last answer (-40 m .. +150 m), so a route that crosses itself never jumps ahead. Checkpoints are
 * the via points' distances along the polyline (projected in order), the last one is the FINISH.
 *
 * TIME. Race time is the number of sim ticks td5_roam_race_tick() ran in RACING (the hook runs once
 * per sim tick that is not frozen: pause menu, the 3-2-1 and the stream hold do not count).
 *
 * ONE file-scope struct (R) holds the state. Main thread only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_fp.h"
#include "td5_geo.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_race_state.h"
#include "td5_hud.h"
#include "td5_render.h"         /* g_render_width_f / g_render_height_f */
#include "td5_vectorui.h"
#include "td5_page_map.h"          /* HUD_WHITE_TEX_PAGE */
#include "td5_roam_mode.h"
#include "td5_roam_route.h"
#include "td5_roam_route_ui.h"
#include "td5_roam_barrier.h"
#include "td5_roam_race.h"
#include "td5_roam_ai.h"           /* [F7] the opponents: grid, driver, positions */
#include "td5_roam_lap.h"          /* [F9a] circuits: total distance + lap count */
#include "td5_roam_handoff.h"      /* [F11a] the geo selector / Quick Race route, planted once the graph is up */
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RR_M            430.0       /* place units per metre */
#define RR_PI           3.14159265358979323846
#define RR_CD_PER_DIGIT 30          /* 3-2-1: one second per digit */
#define RR_WIN_BACK_M   40.0        /* progress search window behind / ahead of the last answer */
#define RR_WIN_AHEAD_M  150.0
#define RR_LOST_M       60.0        /* farther than this from the polyline: progress keeps its value */
#define RR_OFF_ROUTE_M  18.0        /* OFF ROUTE warning past this lateral distance */
#define RR_CP_RADIUS_M  25.0        /* a checkpoint counts only this close to the polyline */
#define RR_FINISH_EPS_M 2.0

/* DIK scan codes */
#define K_ESC    0x01
#define K_BACK   0x0E
#define K_ENTER  0x1C
#define K_A      0x1E
#define K_R      0x13
#define K_M      0x32
#define K_F5     0x3F
#define K_UP     0xC8
#define K_DOWN   0xD0
#define K_LEFT   0xCB
#define K_RIGHT  0xCD

enum { ROW_TYPE, ROW_LAPS, ROW_OPP, ROW_TRAFFIC, ROW_COPS, ROW_START, ROW_CANCEL, ROW_N };

static struct {
    int      state;
    TD5_RoamRaceSetup setup;
    int      cursor;
    long     setup_age;            /* frames since the setup panel opened (the pause menu opens a tick later) */
    /* the race's own copy of the route */
    double  *px, *pz, *dist;
    int      npt;
    double   length_m;
    char     name[TD5_ROUTE_NAME_MAX];
    uint32_t hash;
    double   cp_along[TD5_RRACE_MAX_CP];
    int      ncp;
    /* progress */
    double   along, lateral_m, max_along;
    int      seg;
    int      cp_next;
    long     ticks;
    long     wrong_t, off_t;
    int      start_yaw;
    int32_t  start_px, start_pz;
    /* best run of this route (routes/<name>.best) */
    long     best_ticks;
    long     best_split[TD5_RRACE_MAX_CP];
    int      best_ncp;
    TD5_RoamRaceResult res;
    /* requests the game takes */
    int      req_pause_close, req_countdown;
    int      cd_armed, rearm;
    /* [F7] the field's clock (runs on past the player's finish while opponents race) */
    long     clock;
    int      bar_hold;             /* the player finished, barriers stay up for the opponents */
    /* [F9a] a circuit: the loop, the player's lap track, laps to run, the race distance */
    int      circuit, laps, laps_done;
    TD5_RoamLoop loop;
    TD5_LapTrack lt;
    double   finish_total;          /* the race distance: a sprint = the route length, a circuit = laps x lap */
    long     best_lap;              /* fastest lap saved for this route (circuit), 0 = none */
    long     best_file_laps;        /* laps the saved total best was run over */
    /* messages */
    uint32_t go_ms, split_ms;
    char     split_txt[96];
    uint32_t split_col;
    /* [HE] a pause-menu RESTART re-inits the roam session: the race is run again from the grid */
    int      restart;              /* 1 = start the race again once the new session is up */
    long     restart_frames;
    TD5_RoamRaceSetup restart_setup;
    /* keys (own edge detection, once per frame) */
    uint8_t  cur[256], prev[256];
} R;

static int ke(int sc) { return R.cur[sc] && !R.prev[sc]; }

#ifndef TD5RE_RELEASE
static int s_dev_hold_finish;    /* [HE] DEV: no checkpoints / finish (the roam-walls test puts the car anywhere on the route) */
void td5_roam_race_dev_hold_finish(int on) { s_dev_hold_finish = on ? 1 : 0; }
#endif

/* ============================================================ small helpers === */

static void rr_free_route(void)
{
    free(R.px); free(R.pz); free(R.dist);
    R.px = R.pz = R.dist = NULL;
    R.npt = 0;
    td5_roam_loop_free(&R.loop);
}

static int rr_car_place(double *px, double *pz)
{
    TD5_Actor *a = td5_game_get_actor(0);
    int32_t ox, oz;
    if (!a) return 0;
    td5_roam_world_origin(&ox, &oz);
    *px = (double)a->world_pos.x / 256.0 + (double)ox;
    *pz = (double)a->world_pos.z / 256.0 + (double)oz;
    return 1;
}

static void rr_fmt_time(char *out, size_t cap, long ticks)
{
    const long cs = (ticks * 100 + TD5_RRACE_TICK_HZ / 2) / TD5_RRACE_TICK_HZ;
    snprintf(out, cap, "%02ld:%02ld.%02ld", cs / 6000, (cs / 100) % 60, cs % 100);
}

/* ============================================================ best times ====== */

static int rr_best_path(char *out, size_t cap, const char *name)
{
    char rel[96];
    if (!name || !name[0]) return 0;
    snprintf(rel, sizeof rel, "routes/%s.best", name);
    return td5_geo_source_path(out, cap, td5_roam_route_place(), rel) != NULL;
}

static void rr_best_load(void)
{
    char path[512], line[512];
    FILE *f;
    int file_circuit = 0;
    R.best_ticks = 0; R.best_ncp = 0; R.best_lap = 0; R.best_file_laps = 0;
    if (!rr_best_path(path, sizeof path, R.name) || !(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "best_ticks=", 11)) R.best_ticks = atol(line + 11);
        else if (!strncmp(line, "circuit=", 8)) file_circuit = atoi(line + 8) != 0;
        else if (!strncmp(line, "laps=", 5)) R.best_file_laps = atol(line + 5);
        else if (!strncmp(line, "best_lap=", 9)) R.best_lap = atol(line + 9);
        else if (!strncmp(line, "splits=", 7)) {
            const char *p = line + 7;
            R.best_ncp = 0;
            while (*p && R.best_ncp < TD5_RRACE_MAX_CP) {
                char *e;
                const long v = strtol(p, &e, 10);
                if (e == p) break;
                R.best_split[R.best_ncp++] = v;
                p = (*e == ',') ? e + 1 : e;
            }
        }
    }
    fclose(f);
    /* [F9a] a circuit's total only compares with a total over the same laps, a sprint ignores circuit files */
    if (td5_roam_route_type() == TD5_ROUTE_CIRCUIT) {
        if (!file_circuit) { R.best_lap = 0; R.best_ticks = 0; }
        else if (R.best_file_laps != td5_roam_route_laps()) R.best_ticks = 0;
        R.best_ncp = 0;
    } else {
        if (file_circuit) { R.best_ticks = 0; R.best_ncp = 0; }
        R.best_lap = 0;
    }
}

/* total_best: this run's total is the new best; a circuit may also save a new best lap */
static void rr_best_save(int total_best)
{
    char path[512];
    FILE *f;
    int i;
    if (!rr_best_path(path, sizeof path, R.name) || !(f = fopen(path, "w"))) {
        TD5_LOG_W(LOG_TAG, "ROAM_RACE: cannot write the best time of '%s'", R.name);
        return;
    }
    if (R.circuit) {
        const long keep = total_best ? R.res.ticks : R.best_ticks;
        const long bl = (R.best_lap > 0 && (R.res.best_lap_ticks <= 0 || R.best_lap < R.res.best_lap_ticks)) ? R.best_lap : R.res.best_lap_ticks;
        fprintf(f, "best_ticks=%ld\nhash=%08X\nlength_m=%.1f\ncircuit=1\nlaps=%d\nbest_lap=%ld\nsplits=\n", keep, (unsigned)R.hash,
                R.length_m, R.laps, bl);
        fclose(f);
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: circuit best for '%s' saved (total %ld ticks over %d laps, best lap %ld ticks) -> %s", R.name,
                  keep, R.laps, bl, path);
        return;
    }
    fprintf(f, "best_ticks=%ld\nhash=%08X\nlength_m=%.1f\nsplits=", R.res.ticks, (unsigned)R.hash, R.length_m);
    for (i = 0; i < R.res.ncp; i++) fprintf(f, "%s%ld", i ? "," : "", R.res.split_ticks[i]);
    fprintf(f, "\n");
    fclose(f);
    TD5_LOG_I(LOG_TAG, "ROAM_RACE: new best for '%s' saved (%ld ticks) -> %s", R.name, R.res.ticks, path);
}

/* ============================================================ state =========== */

int td5_roam_race_state(void) { return R.state; }
int td5_roam_race_running(void) { return R.state == TD5_RRACE_COUNTDOWN || R.state == TD5_RRACE_RACING; }
int td5_roam_race_barriers_live(void)
{
    return td5_roam_active() && (td5_roam_race_running() || (R.state == TD5_RRACE_FINISHED && R.bar_hold));
}
const TD5_RoamRaceResult *td5_roam_race_result(void) { return &R.res; }
int td5_roam_race_setup_open(void) { return R.state == TD5_RRACE_SETUP; }

void td5_roam_race_progress(double *along_m, double *length_m, double *lateral_m)
{
    const int on = R.state >= TD5_RRACE_COUNTDOWN;
    if (along_m) *along_m = on ? R.along : 0.0;
    if (length_m) *length_m = on ? R.finish_total : 0.0;
    if (lateral_m) *lateral_m = on ? R.lateral_m : 0.0;
}

int td5_roam_race_circuit(void) { return R.state >= TD5_RRACE_COUNTDOWN && R.circuit; }
int td5_roam_race_laps(void) { return td5_roam_race_circuit() ? R.laps : 0; }
int td5_roam_race_lap_now(void)
{
    int l;
    if (!td5_roam_race_circuit()) return 0;
    if (R.state == TD5_RRACE_COUNTDOWN) return 1;
    l = R.laps_done + 1;
    return l > R.laps ? R.laps : l;
}

static void rr_to_off(const char *why)
{
    if (R.state != TD5_RRACE_OFF)
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: -> OFF (%s)", why);
    R.state = TD5_RRACE_OFF;
    R.req_pause_close = 0; R.req_countdown = 0; R.cd_armed = 0; R.rearm = 0;
    R.bar_hold = 0;
    td5_roam_race_wall_end();
    td5_roam_barriers_set_visible(0);
    td5_roam_barriers_clear();
    td5_roam_ai_race_end();          /* [F7] the opponents are parked again */
}

void td5_roam_race_reset(void)
{
    /* [HE] td5_roam_session_start runs this on a pause-menu RESTART too (the session stays active, the route
     * survives): a race that was on is started again from the grid once the new session is up, so its barriers
     * come back and the 3-2-1 runs. td5_roam_end clears active first, so leaving roam never restarts anything. */
    int was_live = R.state >= TD5_RRACE_COUNTDOWN;
#ifndef TD5RE_RELEASE
    if (!td5_env_int("TD5RE_ROAM_RACE_RESTART", 1, 0, 1)) was_live = 0;   /* DEV A/B: 0 = a restart drops the race (F6-B) */
#endif
    if (was_live && td5_roam_active()) {
        R.restart = 1;
        R.restart_frames = 0;
        R.restart_setup = R.setup;
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: restart: the race runs again once the session is up (opponents=%d traffic=%d cops=%d)",
                  R.setup.opponents, R.setup.traffic, R.setup.cops);
    } else if (!td5_roam_active()) {
        R.restart = 0;
    }
    rr_to_off("session");
    rr_free_route();
    memset(&R.res, 0, sizeof R.res);
    R.setup.laps = 1;
    R.setup.opponents = -1;          /* [F7] the default, once the session knows what it loaded */
}

/* [F7] the field (player + opponents) into the result, rank order */
static void rr_field_snapshot(void)
{
    TD5_RoamStanding st[6];
    const int n = td5_roam_ai_standings(st, 6);
    int i;
    if (n <= 0) return;
    R.res.nfield = n;
    R.res.player_rank = td5_roam_ai_player_rank();
    for (i = 0; i < n; i++) {
        R.res.field_slot[i] = st[i].slot;
        R.res.field_ticks[i] = st[i].finished ? st[i].finish_ticks : 0;
        R.res.field_along_m[i] = st[i].along_m;
        R.res.field_laps[i] = st[i].laps_done;
    }
}

void td5_roam_race_abandon(void)
{
    if (R.state == TD5_RRACE_OFF) return;
    if (td5_roam_race_running()) {
        R.res.finished = 0;
        R.res.ticks = R.ticks;
    }
    rr_to_off("abandoned");
}

int td5_roam_race_hide_route_hint(void) { return R.state >= TD5_RRACE_COUNTDOWN; }

/* ============================================================ entry =========== */

static int rr_route_ready(void)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    const int n = td5_roam_route_wp_count();
    const TD5_RouteWp *w0 = td5_roam_route_wp(0), *wl = td5_roam_route_wp(n - 1);
    return td5_roam_active() && n >= 2 && r->ok && r->npt >= 2 && r->dist_m && w0 && wl &&
           w0->kind == TD5_ROUTE_WP_START && wl->kind == TD5_ROUTE_WP_FINISH &&
           (td5_roam_route_type() == TD5_ROUTE_SPRINT || r->closed);   /* [F9a] a circuit needs its closing leg */
}

int td5_roam_race_open_setup(void)
{
    if (!rr_route_ready()) {
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: setup refused (need a planned sprint: START, FINISH, every leg planned)");
        return 0;
    }
    if (R.state != TD5_RRACE_OFF) return 0;
    if (!td5_roam_route_ui_map_open()) td5_roam_route_ui_open_map();
    R.setup.laps = td5_roam_route_type() == TD5_ROUTE_CIRCUIT ? td5_roam_route_laps() : 1;
    if (R.setup.opponents < 0) R.setup.opponents = td5_roam_ai_default_opponents();
    if (R.setup.opponents > td5_roam_ai_available()) R.setup.opponents = td5_roam_ai_available();
    R.cursor = ROW_START;
    R.setup_age = 0;
    R.state = TD5_RRACE_SETUP;
    snprintf(R.name, sizeof R.name, "%s", td5_roam_route_current_name());
    rr_best_load();
    TD5_LOG_I(LOG_TAG, "ROAM_RACE: setup open (route '%s', %.0f m, best %ld ticks)", R.name[0] ? R.name : "UNSAVED",
              td5_roam_route_result()->length_m, R.best_ticks);
    return 1;
}

/* Copy the planned route, project the checkpoints. */
static int rr_take_route(void)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    const int nwp = td5_roam_route_wp_count();
    int i, k, seg = 0;
    rr_free_route();
    R.px = (double *)malloc((size_t)r->npt * sizeof(double));
    R.pz = (double *)malloc((size_t)r->npt * sizeof(double));
    R.dist = (double *)malloc((size_t)r->npt * sizeof(double));
    if (!R.px || !R.pz || !R.dist) { rr_free_route(); return 0; }
    memcpy(R.px, r->px, (size_t)r->npt * sizeof(double));
    memcpy(R.pz, r->pz, (size_t)r->npt * sizeof(double));
    memcpy(R.dist, r->dist_m, (size_t)r->npt * sizeof(double));
    R.npt = r->npt;
    R.length_m = r->length_m;
    R.hash = r->hash;
    snprintf(R.name, sizeof R.name, "%s", td5_roam_route_current_name());
    /* [F9a] a circuit: the polyline is ONE closed lap, progress is the total distance of td5_roam_lap.h */
    R.circuit = td5_roam_route_type() == TD5_ROUTE_CIRCUIT;
    R.laps = R.circuit ? td5_roam_route_laps() : 1;
    R.laps_done = 0;
    td5_roam_lap_reset(&R.lt);
    if (R.circuit) {
        if (!r->closed || !td5_roam_loop_init(&R.loop, R.px, R.pz, R.dist, R.npt, RR_M)) { rr_free_route(); return 0; }
        R.length_m = R.loop.L;
    }
    R.finish_total = R.length_m * (double)R.laps;
    /* checkpoints: every via point in order (nearest polyline point at or after the previous one),
     * then the finish at the end of the polyline (a circuit has none: its laps are the splits) */
    R.ncp = 0;
    for (k = 1; !R.circuit && k < nwp - 1 && R.ncp < TD5_RRACE_MAX_CP - 1; k++) {
        const TD5_RouteWp *w = td5_roam_route_wp(k);
        const double wx = w->snapped ? w->sx : (double)w->px, wz = w->snapped ? w->sz : (double)w->pz;
        double best = 1e300, ba = R.dist[seg];
        int bs = seg;
        for (i = seg; i + 1 < R.npt; i++) {
            const double sx = R.px[i + 1] - R.px[i], sz = R.pz[i + 1] - R.pz[i];
            const double l2 = sx * sx + sz * sz;
            double t = l2 > 0.0 ? ((wx - R.px[i]) * sx + (wz - R.pz[i]) * sz) / l2 : 0.0, qx, qz, d;
            if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
            qx = R.px[i] + sx * t; qz = R.pz[i] + sz * t;
            d = (qx - wx) * (qx - wx) + (qz - wz) * (qz - wz);
            if (d < best - 1e-6) { best = d; bs = i; ba = R.dist[i] + t * (R.dist[i + 1] - R.dist[i]); }
        }
        seg = bs;
        R.cp_along[R.ncp++] = ba;
    }
    if (!R.circuit) R.cp_along[R.ncp++] = R.length_m;
    /* the grid: START point, facing the route 8 m on */
    {
        int j = 1;
        double dx, dz;
        while (j + 1 < R.npt && R.dist[j] < 8.0) j++;
        dx = R.px[j] - R.px[0]; dz = R.pz[j] - R.pz[0];
        R.start_yaw = (int)floor(atan2(dx, dz) * (4096.0 / (2.0 * RR_PI)) + 0.5) & 0xFFF;
        R.start_px = (int32_t)floor(R.px[0] + 0.5);
        R.start_pz = (int32_t)floor(R.pz[0] + 0.5);
    }
    return 1;
}

static void rr_place_on_grid(void)
{
    TD5_Actor *a = td5_game_get_actor(0);
    int32_t px, pz;
    int yaw;
    if (!a) return;
    /* [F7] with opponents the player stands on its drawn grid spot (START or behind it) */
    if (td5_roam_ai_player_grid(&px, &pz, &yaw)) td5_roam_place_actor(0, a, px, pz, yaw);
    else td5_roam_place_actor(0, a, R.start_px, R.start_pz, R.start_yaw);
}

int td5_roam_race_start(const TD5_RoamRaceSetup *s)
{
    int nb = 0;
    TD5_RoamBarrierStats bs;
    uint32_t t0, t_bar, t_grid;
    if (!rr_route_ready()) {
        TD5_LOG_W(LOG_TAG, "ROAM_RACE: start refused (no planned sprint)");
        return 0;
    }
    if (s) R.setup = *s;
    R.setup.laps = td5_roam_route_type() == TD5_ROUTE_CIRCUIT ? td5_roam_route_laps() : 1;   /* a sprint is one run A -> B */
    if (R.setup.opponents < 0) R.setup.opponents = td5_roam_ai_default_opponents();
    t0 = td5_plat_time_ms();
    if (!rr_take_route()) return 0;
    rr_best_load();

    /* barriers for this route, visible and solid (racers only) from the grid on */
    nb = td5_roam_barriers_build(td5_roam_route_result());
    td5_roam_barriers_set_visible(1);
    td5_roam_race_wall_begin();
    td5_roam_barriers_stats(&bs);
    t_bar = td5_plat_time_ms();

    memset(&R.res, 0, sizeof R.res);
    snprintf(R.res.name, sizeof R.res.name, "%s", R.name);
    R.res.hash = R.hash;
    R.res.length_m = R.length_m;
    R.res.ncp = R.ncp;
    R.res.circuit = R.circuit; R.res.laps = R.laps; R.res.lap_len_m = R.circuit ? R.length_m : 0.0;
    R.res.best_lap_prev = R.best_lap;
    R.res.best_ticks = R.best_ticks;
    R.res.setup_traffic = R.setup.traffic;
    R.res.setup_cops = R.setup.cops;

    R.along = 0.0; R.max_along = 0.0; R.lateral_m = 0.0; R.seg = 0; R.cp_next = 0;
    R.ticks = 0; R.wrong_t = 0; R.off_t = 0;
    R.split_ms = 0; R.go_ms = 0;
    R.clock = 0; R.bar_hold = 0;

    td5_roam_ai_race_end();          /* F5 from the results: the last race's field and spots go */
    td5_roam_ai_set_circuit(R.circuit ? R.laps : 0, R.length_m);   /* [F9a] consumed by the next ai race start */
    rr_place_on_grid();
    /* [F7] the opponents on the grid behind START (a short field when the street is tight) */
    if (td5_roam_ai_race_start(R.px, R.pz, R.dist, R.npt, R.length_m, R.start_px, R.start_pz, R.start_yaw,
                               R.setup.opponents) > 0)
        rr_place_on_grid();          /* the player's drawn spot */
    rr_field_snapshot();
    t_grid = td5_plat_time_ms();
    R.state = TD5_RRACE_COUNTDOWN;
    R.req_countdown = 1; R.cd_armed = 0; R.rearm = 0;
    R.req_pause_close = td5_game_is_pause_menu_active();
    if (td5_roam_route_ui_map_open()) td5_roam_route_ui_close_map();
    if (R.circuit)
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: CIRCUIT %d lap(s) of %.1f m = %.1f m, best lap %ld ticks, best total %ld ticks (%ld laps)",
                  R.laps, R.length_m, R.finish_total, R.best_lap, R.best_ticks, R.best_file_laps);
    TD5_LOG_I(LOG_TAG, "ROAM_RACE: START route='%s' hash=%08X length=%.1f m points=%d checkpoints=%d grid place=(%d,%d) "
              "yaw=%d barriers=%d (mouth %d, turn %d, clusters %d, open arms %d, across route %d) traffic=%d cops=%d "
              "opponents=%d/%d best=%ld ticks | barrier build %u ms (stats %.0f ms, %d tiles read), grid %u ms",
              R.name[0] ? R.name : "UNSAVED", (unsigned)R.hash, R.length_m, R.npt, R.ncp, (int)R.start_px,
              (int)R.start_pz, R.start_yaw, nb, bs.barriers_mouth, bs.barriers_turn, bs.clusters_passed, bs.arms_open,
              bs.across_route, R.setup.traffic, R.setup.cops, td5_roam_ai_count(), R.setup.opponents, R.best_ticks,
              (unsigned)(t_bar - t0), bs.build_ms,
              bs.tiles_read, (unsigned)(t_grid - t_bar));
    return 1;
}

int td5_roam_race_take_pause_close(void)
{
    const int r = R.req_pause_close;
    R.req_pause_close = 0;
    return r;
}

int td5_roam_race_take_countdown(void)
{
    if (!R.req_countdown) return 0;
    R.req_countdown = 0;
    R.cd_armed = 1;
    TD5_LOG_I(LOG_TAG, "ROAM_RACE: countdown armed (%d ticks per digit)", RR_CD_PER_DIGIT);
    return RR_CD_PER_DIGIT;
}

uint32_t td5_roam_race_filter_bits(int slot, uint32_t bits)
{
    if (slot != 0 || R.state == TD5_RRACE_OFF || R.state == TD5_RRACE_SETUP || !td5_roam_active()) return bits;
    if (R.state == TD5_RRACE_FINISHED || (R.state == TD5_RRACE_COUNTDOWN && !R.cd_armed) || R.rearm)
        return (bits & ~(uint32_t)(TD5_INPUT_THROTTLE | TD5_INPUT_RECOVER)) | (uint32_t)TD5_INPUT_BRAKE;
    return bits;
}

/* ============================================================ progress ======== */

static void rr_progress(TD5_Actor *a)
{
    double cx, cz, best = 1e300, ba = R.along;
    int i, bs = -1, lo = R.seg, hi = R.seg;
    if (!rr_car_place(&cx, &cz) || R.npt < 2) return;
    if (R.circuit) {
        /* [F9a] the TOTAL distance round the closed loop: continuous across the line, windowed along the route
         * so a figure 8 never jumps, wrong way judged on the loop's own direction */
        td5_roam_lap_update(&R.loop, &R.lt, cx, cz, RR_LOST_M);
        R.lateral_m = R.lt.lat_m;
        if (R.lateral_m > R.res.max_lateral_m) R.res.max_lateral_m = R.lateral_m;
        if (R.lt.lost) return;
        R.seg = R.lt.seg;
        R.along = R.lt.total;
        R.max_along = R.lt.max_total;
        {
            double ux, uz;
            const double vx = a->linear_velocity_x / 256.0, vz = a->linear_velocity_z / 256.0;
            const double sp = sqrt(vx * vx + vz * vz);
            td5_roam_loop_dir(&R.loop, R.lt.seg, &ux, &uz);
            if (sp > 4.0 * RR_M / TD5_RRACE_TICK_HZ && (vx * ux + vz * uz) < -0.5 * sp) R.wrong_t++;
            else if (R.wrong_t > 0) R.wrong_t = R.wrong_t > 3 ? R.wrong_t - 3 : 0;
        }
        if (R.lateral_m > RR_OFF_ROUTE_M) R.off_t++;
        else R.off_t = 0;
        return;
    }
    while (lo > 0 && R.dist[lo] > R.along - RR_WIN_BACK_M) lo--;
    while (hi + 2 < R.npt && R.dist[hi + 1] < R.along + RR_WIN_AHEAD_M) hi++;
    for (i = lo; i <= hi && i + 1 < R.npt; i++) {
        const double sx = R.px[i + 1] - R.px[i], sz = R.pz[i + 1] - R.pz[i];
        const double l2 = sx * sx + sz * sz;
        double t = l2 > 0.0 ? ((cx - R.px[i]) * sx + (cz - R.pz[i]) * sz) / l2 : 0.0, qx, qz, d;
        /* [F7] behind START (a grid spot) the first segment extends back: negative distance along,
         * the same measure as the opponents', so the grid order is right before GO */
        if (t < 0.0 && i > 0) t = 0.0; else if (t > 1.0) t = 1.0;
        qx = R.px[i] + sx * t; qz = R.pz[i] + sz * t;
        d = (qx - cx) * (qx - cx) + (qz - cz) * (qz - cz);
        if (d < best) { best = d; bs = i; ba = R.dist[i] + t * (R.dist[i + 1] - R.dist[i]); }
    }
    if (bs < 0) return;
    R.lateral_m = sqrt(best) / RR_M;
    if (R.lateral_m > R.res.max_lateral_m) R.res.max_lateral_m = R.lateral_m;
    if (R.lateral_m > RR_LOST_M) return;           /* lost: keep the last answer */
    R.seg = bs;
    R.along = ba;
    if (R.along > R.max_along) R.max_along = R.along;

    /* wrong way: moving against the route's direction at more than ~14 km/h */
    {
        const double sx = R.px[bs + 1] - R.px[bs], sz = R.pz[bs + 1] - R.pz[bs];
        const double l = sqrt(sx * sx + sz * sz);
        const double vx = a->linear_velocity_x / 256.0, vz = a->linear_velocity_z / 256.0;
        const double sp = sqrt(vx * vx + vz * vz);
        if (l > 0.0 && sp > 4.0 * RR_M / TD5_RRACE_TICK_HZ && (vx * sx + vz * sz) / l < -0.5 * sp) R.wrong_t++;
        else if (R.wrong_t > 0) R.wrong_t = R.wrong_t > 3 ? R.wrong_t - 3 : 0;
    }
    if (R.lateral_m > RR_OFF_ROUTE_M) R.off_t++;
    else R.off_t = 0;
}

static void rr_finish(void)
{
    char t[32], b[32];
    R.state = TD5_RRACE_FINISHED;
    R.res.finished = 1;
    R.res.ticks = R.ticks;
    R.res.best_ticks = R.best_ticks;
    R.res.new_best = R.name[0] && (R.best_ticks <= 0 || R.ticks < R.best_ticks);
    R.res.laps_done = R.laps_done;
    R.res.new_best_lap = R.circuit && R.name[0] && R.res.best_lap_ticks > 0 && (R.best_lap <= 0 || R.res.best_lap_ticks < R.best_lap);
    if (R.res.new_best || R.res.new_best_lap) rr_best_save(R.res.new_best);
    if (td5_roam_ai_racing()) {
        R.bar_hold = 1;              /* [F7] opponents still run: their barriers stay */
    } else {
        td5_roam_race_wall_end();
        td5_roam_barriers_set_visible(0);
    }
    rr_fmt_time(t, sizeof t, R.ticks);
    rr_fmt_time(b, sizeof b, R.best_ticks);
    TD5_LOG_I(LOG_TAG, "ROAM_RACE: FINISH route='%s' time=%s (%ld ticks) best=%s%s wrong_way_ticks=%ld off_route_ticks=%ld "
              "max_lateral=%.1f m length=%.1f m", R.name[0] ? R.name : "UNSAVED", t, R.ticks,
              R.best_ticks > 0 ? b : "-", R.res.new_best ? " NEW BEST" : "", R.res.wrong_way_ticks,
              R.res.off_route_ticks, R.res.max_lateral_m, R.length_m);
}

void td5_roam_race_tick(void)
{
    TD5_Actor *a;
    if (!td5_roam_active()) return;
    if (R.state == TD5_RRACE_FINISHED) {
        /* [F7] the player is through; the field's clock runs on until every opponent is */
        if (td5_roam_ai_racing()) {
            R.clock++;
            td5_roam_ai_progress(R.clock, 1, R.along, 1, R.res.ticks);
            rr_field_snapshot();
            if (!td5_roam_ai_racing()) TD5_LOG_I(LOG_TAG, "ROAM_RACE: every opponent is through (%ld ticks)", R.clock);
        }
        if (R.bar_hold && !td5_roam_ai_racing()) {
            R.bar_hold = 0;
            td5_roam_race_wall_end();
            td5_roam_barriers_set_visible(0);
        }
        return;
    }
    if (R.state < TD5_RRACE_COUNTDOWN) return;
    a = td5_game_get_actor(0);
    if (!a) return;
    if (R.state == TD5_RRACE_COUNTDOWN) {
        /* the first unfrozen tick after the 3-2-1 the game armed is GO */
        if (!R.cd_armed || R.rearm || td5_game_resume_countdown_ticks() > 0) {
            td5_roam_ai_progress(0, 0, 0.0, 0, 0);   /* [F7] grid order */
            return;
        }
        R.state = TD5_RRACE_RACING;
        R.go_ms = td5_plat_time_ms();
        R.ticks = 0;
        R.clock = 0;
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: GO (sim tick %u)", (unsigned)g_td5.simulation_tick_counter);
    }
    R.ticks++;
    R.clock = R.ticks;
    rr_progress(a);
    if (R.wrong_t > 15) R.res.wrong_way_ticks++;
    if (R.off_t > 10) R.res.off_route_ticks++;
    if (R.circuit && R.state == TD5_RRACE_RACING && R.lt.init) {
        /* [F9a] lap k is counted once, when the running maximum of the total distance first reaches k x lap */
        int now = td5_roam_lap_count(&R.loop, &R.lt, 0.0);
        if (R.lt.max_total >= R.finish_total - RR_FINISH_EPS_M) now = R.laps;
        if (now > R.laps) now = R.laps;
        if (now > R.laps_done && R.lateral_m < RR_CP_RADIUS_M) {
            if (now > R.laps_done + 1) {
                R.res.lap_jumps++;
                TD5_LOG_W(LOG_TAG, "ROAM_RACE: lap count jumped %d -> %d (total %.1f m)", R.laps_done, now, R.lt.total);
            }
            while (R.laps_done < now) {
                const int k = R.laps_done;
                const long lt = R.ticks - (k > 0 ? R.res.lap_ticks[k - 1] : 0);
                char t[32], b[32];
                R.res.lap_ticks[k] = R.ticks;
                R.laps_done++;
                R.res.laps_done = R.laps_done;
                if (R.res.best_lap_ticks <= 0 || lt < R.res.best_lap_ticks) R.res.best_lap_ticks = lt;
                rr_fmt_time(t, sizeof t, lt);
                rr_fmt_time(b, sizeof b, R.best_lap);
                if (R.laps_done < R.laps) {
                    if (R.best_lap > 0) {
                        const long d = lt - R.best_lap;
                        snprintf(R.split_txt, sizeof R.split_txt, "LAP %d   %s   %c%ld.%02ld", k + 1, t, d <= 0 ? '-' : '+',
                                 labs(d) / TD5_RRACE_TICK_HZ, (labs(d) % TD5_RRACE_TICK_HZ) * 100 / TD5_RRACE_TICK_HZ);
                        R.split_col = d <= 0 ? 0xFF50E070 : 0xFFFF6A50;
                    } else {
                        snprintf(R.split_txt, sizeof R.split_txt, "LAP %d   %s", k + 1, t);
                        R.split_col = 0xFFFFFFFF;
                    }
                    R.split_ms = td5_plat_time_ms();
                }
                TD5_LOG_I(LOG_TAG, "ROAM_RACE: lap %d/%d done in %s (%ld ticks), race %ld ticks, total %.1f m, best saved %s",
                          k + 1, R.laps, t, lt, R.ticks, R.lt.total, R.best_lap > 0 ? b : "-");
            }
            if (R.laps_done >= R.laps) rr_finish();
        }
    }
#ifndef TD5RE_RELEASE
    if (s_dev_hold_finish) { td5_roam_ai_progress(R.clock, 1, R.along, 0, R.ticks); return; }   /* [HE] DEV: the wall test teleports */
#endif
    while (R.cp_next < R.ncp && R.along >= R.cp_along[R.cp_next] - (R.cp_next == R.ncp - 1 ? RR_FINISH_EPS_M : 3.0) &&
           R.lateral_m < RR_CP_RADIUS_M) {
        const int k = R.cp_next++;
        char t[32];
        R.res.split_ticks[k] = R.ticks;
        rr_fmt_time(t, sizeof t, R.ticks);
        if (k < R.ncp - 1) {
            if (k < R.best_ncp && R.best_split[k] > 0) {
                const long d = R.ticks - R.best_split[k];
                snprintf(R.split_txt, sizeof R.split_txt, "CHECKPOINT %d   %s   %c%ld.%02ld", k + 1, t, d <= 0 ? '-' : '+',
                         labs(d) / TD5_RRACE_TICK_HZ, (labs(d) % TD5_RRACE_TICK_HZ) * 100 / TD5_RRACE_TICK_HZ);
                R.split_col = d <= 0 ? 0xFF50E070 : 0xFFFF6A50;
            } else {
                snprintf(R.split_txt, sizeof R.split_txt, "CHECKPOINT %d   %s", k + 1, t);
                R.split_col = 0xFFFFFFFF;
            }
            R.split_ms = td5_plat_time_ms();
            TD5_LOG_I(LOG_TAG, "ROAM_RACE: checkpoint %d/%d at %.1f m, %s (%ld ticks)", k + 1, R.ncp - 1, R.cp_along[k], t, R.ticks);
        } else {
            rr_finish();
            break;
        }
    }
    /* [F7] opponents' progress + positions, the player's finish (this tick) included */
    td5_roam_ai_progress(R.clock, 1, R.along, R.state == TD5_RRACE_FINISHED, R.ticks);
    rr_field_snapshot();
}

/* ============================================================ frame =========== */

void td5_roam_race_frame(void)
{
    int i;
    const int paused = td5_game_is_pause_menu_active();
    if (!td5_roam_active()) { if (R.state != TD5_RRACE_OFF) td5_roam_race_reset(); return; }
    memcpy(R.prev, R.cur, sizeof R.prev);
    for (i = 0; i < 256; i++) R.cur[i] = (uint8_t)(td5_plat_input_key_pressed(i) ? 1 : 0);

#ifndef TD5RE_RELEASE
    td5_roam_sprint_test_frame();
#endif
    td5_roam_handoff_frame();    /* [F11a] plant the handed-over route, then the setup panel / the race */

    if (R.restart && R.state == TD5_RRACE_OFF && !paused) {
        /* [HE] after a pause-menu RESTART: the same route, the same setup, from the grid (barriers + 3-2-1) */
        R.restart_frames++;
        if (rr_route_ready() && R.restart_frames >= 5) {
            R.restart = 0;
            if (td5_roam_race_start(&R.restart_setup))
                TD5_LOG_I(LOG_TAG, "ROAM_RACE: restarted after the pause-menu RESTART (%ld frames)", R.restart_frames);
            else
                TD5_LOG_W(LOG_TAG, "ROAM_RACE: restart refused; free roam goes on");
        } else if (R.restart_frames > 30 * 60) {
            R.restart = 0;
            TD5_LOG_W(LOG_TAG, "ROAM_RACE: restart dropped (the route never came back in 60 s)");
        }
    }

    switch (R.state) {
    case TD5_RRACE_OFF:
        /* from the pause menu: R = race the planned route */
        if (paused && !td5_roam_route_ui_map_open() && ke(K_R)) td5_roam_race_open_setup();
        break;
    case TD5_RRACE_SETUP:
        R.setup_age++;
        if ((!paused || !td5_roam_route_ui_map_open()) && R.setup_age > 15) {
            R.state = TD5_RRACE_OFF;
            TD5_LOG_I(LOG_TAG, "ROAM_RACE: setup closed");
            break;
        }
        if (ke(K_UP))   { R.cursor = R.cursor <= ROW_TYPE ? ROW_CANCEL : R.cursor - 1; }
        if (ke(K_DOWN)) { R.cursor = R.cursor >= ROW_CANCEL ? ROW_TYPE : R.cursor + 1; }
        if (R.cursor == ROW_TYPE && (ke(K_LEFT) || ke(K_RIGHT) || ke(K_ENTER))) {
            /* [F9a] SPRINT <-> CIRCUIT: the planner replans (a circuit adds the closing leg back to START) */
            td5_roam_route_set_type(td5_roam_route_type() == TD5_ROUTE_CIRCUIT ? TD5_ROUTE_SPRINT : TD5_ROUTE_CIRCUIT,
                                    td5_roam_route_laps());
            R.setup.laps = td5_roam_route_type() == TD5_ROUTE_CIRCUIT ? td5_roam_route_laps() : 1;
            rr_best_load();
        } else if (R.cursor == ROW_LAPS && td5_roam_route_type() == TD5_ROUTE_CIRCUIT && (ke(K_LEFT) || ke(K_RIGHT) || ke(K_ENTER))) {
            int l = td5_roam_route_laps();
            if (ke(K_LEFT)) l = l <= 1 ? TD5_ROUTE_MAX_LAPS : l - 1;
            else l = l >= TD5_ROUTE_MAX_LAPS ? 1 : l + 1;
            td5_roam_route_set_type(TD5_ROUTE_CIRCUIT, l);
            R.setup.laps = l;
            rr_best_load();
        } else if (R.cursor == ROW_OPP && (ke(K_LEFT) || ke(K_RIGHT) || ke(K_ENTER))) {
            /* [F7] 0..the cars this session loaded, wrapping */
            const int mx = td5_roam_ai_available();
            if (R.setup.opponents < 0) R.setup.opponents = td5_roam_ai_default_opponents();
            if (ke(K_LEFT)) R.setup.opponents = R.setup.opponents <= 0 ? mx : R.setup.opponents - 1;
            else R.setup.opponents = R.setup.opponents >= mx ? 0 : R.setup.opponents + 1;
        } else if (ke(K_LEFT) || ke(K_RIGHT) || ke(K_ENTER)) {
            /* [F8] traffic OFF / LOW / HIGH */
            if (R.cursor == ROW_TRAFFIC) R.setup.traffic = ke(K_LEFT) ? (R.setup.traffic + 2) % 3 : (R.setup.traffic + 1) % 3;
            else if (R.cursor == ROW_COPS) R.setup.cops = !R.setup.cops;
            else if (ke(K_ENTER) && R.cursor == ROW_START) { td5_roam_race_start(&R.setup); break; }
            else if (ke(K_ENTER) && R.cursor == ROW_CANCEL) { R.state = TD5_RRACE_OFF; break; }
        }
        if (ke(K_BACK)) R.state = TD5_RRACE_OFF;     /* back to the map */
        break;
    case TD5_RRACE_COUNTDOWN:
        /* the pause menu cancels the game's 3-2-1: run it again once the menu closes */
        if (paused && R.cd_armed && !R.req_pause_close) { R.rearm = 1; R.cd_armed = 0; }
        if (!paused && R.rearm) { R.rearm = 0; R.req_countdown = 1; rr_place_on_grid(); td5_roam_ai_regrid(); }
        /* fall through */
    case TD5_RRACE_RACING:
        if (paused && td5_roam_route_ui_map_open() && ke(K_A)) { td5_roam_race_abandon(); break; }
        if (paused && !td5_roam_route_ui_map_open() && ke(K_A)) { td5_roam_race_abandon(); break; }
        break;
    case TD5_RRACE_FINISHED:
        if (td5_roam_route_ui_map_open()) { rr_to_off("route map"); break; }
        if (paused) break;
        if (ke(K_ENTER)) { rr_to_off("keep roaming"); break; }
        if (ke(K_F5)) { td5_roam_race_start(&R.setup); break; }
        break;
    default:
        break;
    }
}

/* ============================================================ drawing ========= */

#define QB_QUADS 64
static TD5_D3DVertex qb_v[QB_QUADS * 4];
static uint16_t      qb_i[QB_QUADS * 6];
static int           qb_n;

static void qb_flush(void)
{
    if (qb_n > 0) { td5_hud_prim_tris(qb_v, qb_n * 4, qb_i, qb_n * 6); qb_n = 0; }
}

static void qb_rect(float x, float y, float w, float h, uint32_t col)
{
    TD5_D3DVertex *v;
    uint16_t *ix;
    const uint16_t b = (uint16_t)(qb_n * 4);
    int k;
    if (qb_n >= QB_QUADS) qb_flush();
    v = &qb_v[qb_n * 4];
    ix = &qb_i[qb_n * 6];
    v[0].screen_x = x;     v[0].screen_y = y;
    v[1].screen_x = x + w; v[1].screen_y = y;
    v[2].screen_x = x + w; v[2].screen_y = y + h;
    v[3].screen_x = x;     v[3].screen_y = y + h;
    for (k = 0; k < 4; k++) {
        v[k].depth_z = 0.0f; v[k].rhw = 1.0f; v[k].diffuse = col; v[k].specular = 0;
        v[k].tex_u = 0.5f; v[k].tex_v = 0.5f;
    }
    ix[0] = b; ix[1] = (uint16_t)(b + 1); ix[2] = (uint16_t)(b + 2);
    ix[3] = b; ix[4] = (uint16_t)(b + 2); ix[5] = (uint16_t)(b + 3);
    qb_n++;
}

static float rr_scale(void)
{
    const float s = g_render_height_f / 720.0f;
    return s < 0.6f ? 0.6f : s;
}

static void tx(float x, float y, const char *s, uint32_t col, float px)
{
    qb_flush();
    td5_vui_text(x, y, s, col, px / 26.0f, px / 26.0f);
}
static void tx_c(float cx, float y, const char *s, uint32_t col, float px)
{
    qb_flush();
    td5_vui_text_centered(cx, y, s, col, px / 26.0f, px / 26.0f);
}
static float tx_w(const char *s, float px) { return td5_vui_text_width(s, px / 26.0f); }

void td5_roam_race_draw_setup(void)
{
    const float W = g_render_width_f, H = g_render_height_f, S = rr_scale();
    const TD5_RouteResult *r = td5_roam_route_result();
    const float bw = 520.0f * S, bh = 406.0f * S, bx = (W - bw) * 0.5f, by = (H - bh) * 0.5f;
    static const char *const k_rows[ROW_N] = { "TYPE", "LAPS", "OPPONENTS", "TRAFFIC", "COPS", "START RACE", "CANCEL" };
    char line[160], best[32];
    int i;
    if (R.state != TD5_RRACE_SETUP || W < 2.0f) return;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR_HUD);
    qb_rect(0, 0, W, H, 0x90000000);
    qb_rect(bx - 3.0f * S, by - 3.0f * S, bw + 6.0f * S, bh + 6.0f * S, 0xFFFFB400);
    qb_rect(bx, by, bw, bh, 0xFF11161D);
    qb_rect(bx + 14.0f * S, by + (118.0f + 34.0f * (float)R.cursor) * S, bw - 28.0f * S, 30.0f * S, 0xFF25507A);
    const int circ = td5_roam_route_type() == TD5_ROUTE_CIRCUIT;
    tx(bx + 18.0f * S, by + 14.0f * S, circ ? "CIRCUIT RACE" : "SPRINT RACE", 0xFFFFB400, 24.0f * S);
    if (circ)
        snprintf(line, sizeof line, "%s   LAP %.2f KM   %d LAPS = %.2f KM", R.name[0] ? R.name : "UNSAVED ROUTE", r->length_m / 1000.0,
                 td5_roam_route_laps(), r->length_m * (double)td5_roam_route_laps() / 1000.0);
    else
        snprintf(line, sizeof line, "%s   %.2f KM   %d STREETS", R.name[0] ? R.name : "UNSAVED ROUTE", r->length_m / 1000.0, r->nedge);
    tx(bx + 18.0f * S, by + 50.0f * S, line, 0xFFFFFFFF, 17.0f * S);
    rr_fmt_time(best, sizeof best, R.best_ticks);
    if (circ) {
        char bl[32];
        rr_fmt_time(bl, sizeof bl, R.best_lap);
        snprintf(line, sizeof line, "BEST LAP %s   BEST TOTAL %s", R.best_lap > 0 ? bl : "NONE", R.best_ticks > 0 ? best : "NONE");
        if (!r->closed) snprintf(line, sizeof line, "NO ROUTE BACK TO START   MOVE OR ADD A POINT");
    } else {
        snprintf(line, sizeof line, "%d CHECKPOINTS   BEST %s", td5_roam_route_wp_count() - 1,
                 R.best_ticks > 0 ? best : (R.name[0] ? "NONE YET" : "SAVE THE ROUTE TO KEEP ONE"));
    }
    tx(bx + 18.0f * S, by + 76.0f * S, line, circ && !r->closed ? 0xFFFF6A50 : 0xFFB8C2CF, 15.0f * S);
    for (i = 0; i < ROW_N; i++) {
        const float y = by + (122.0f + 34.0f * (float)i) * S;
        const int fixed = i == ROW_LAPS && !circ;
        const uint32_t col = fixed ? 0xFF8A94A0 : 0xFFFFFFFF;
        tx(bx + 30.0f * S, y, k_rows[i], col, 18.0f * S);
        line[0] = '\0';
        if (i == ROW_TYPE) snprintf(line, sizeof line, "%s", circ ? "CIRCUIT  LAPS ROUND A LOOP" : "SPRINT  A TO B");
        else if (i == ROW_LAPS) {
            if (circ) snprintf(line, sizeof line, "%d", td5_roam_route_laps());
            else snprintf(line, sizeof line, "1   (CIRCUITS ONLY)");
        }
        else if (i == ROW_OPP) {
            const int mx = td5_roam_ai_available();
            const int n = R.setup.opponents < 0 ? td5_roam_ai_default_opponents() : R.setup.opponents;
            if (mx > 0) snprintf(line, sizeof line, "%d   (UP TO %d)", n, mx);
            else snprintf(line, sizeof line, "0   (NO CARS LOADED)");
        }
        else if (i == ROW_TRAFFIC) snprintf(line, sizeof line, "%s", R.setup.traffic >= 2 ? "HIGH" : (R.setup.traffic == 1 ? "LOW" : "OFF"));
        else if (i == ROW_COPS) snprintf(line, sizeof line, "%s", R.setup.cops ? "ON" : "OFF");
        if (line[0]) tx(bx + 200.0f * S, y, line, col, 18.0f * S);
    }
    tx(bx + 18.0f * S, by + bh - 26.0f * S, "UP / DOWN   LEFT / RIGHT CHANGE   ENTER SELECT   BKSP MAP   ESC DRIVE", 0xFFB8C2CF, 13.0f * S);
    qb_flush();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* [F7] "YOU" / "CPU n" */
static const char *rr_racer_name(int slot, char *buf, size_t cap)
{
    if (slot == 0) return "YOU";
    snprintf(buf, cap, "CPU %d", slot);
    return buf;
}

/* [F7] the field under the results: rank, name, time and gap to the winner (or what is left to run) */
static float rr_draw_field(float W, float y, float S)
{
    char line[160], t[32], nm[16];
    int i;
    const long win = R.res.nfield > 0 ? R.res.field_ticks[0] : 0;
    tx_c(W * 0.5f, y, "FINISHING ORDER", 0xFFFFB400, 16.0f * S);
    y += 24.0f * S;
    for (i = 0; i < R.res.nfield && i < 6; i++) {
        const int slot = R.res.field_slot[i];
        const long ft = R.res.field_ticks[i];
        const uint32_t col = slot == 0 ? 0xFFFFE070 : 0xFFFFFFFF;
        if (ft > 0) {
            rr_fmt_time(t, sizeof t, ft);
            if (i == 0 || win <= 0) snprintf(line, sizeof line, "%d   %-6s   %s", i + 1, rr_racer_name(slot, nm, sizeof nm), t);
            else snprintf(line, sizeof line, "%d   %-6s   %s   GAP %ld.%02ld", i + 1, rr_racer_name(slot, nm, sizeof nm), t,
                          (ft - win) / TD5_RRACE_TICK_HZ, ((ft - win) % TD5_RRACE_TICK_HZ) * 100 / TD5_RRACE_TICK_HZ);
        } else if (td5_roam_ai_count() > 0) {
            const double togo = R.finish_total - R.res.field_along_m[i];
            if (R.res.circuit)
                snprintf(line, sizeof line, "%d   %-6s   RACING   LAP %d/%d", i + 1, rr_racer_name(slot, nm, sizeof nm),
                         R.res.field_laps[i] + 1 > R.laps ? R.laps : R.res.field_laps[i] + 1, R.laps);
            else
                snprintf(line, sizeof line, "%d   %-6s   RACING   %.2f KM TO GO", i + 1, rr_racer_name(slot, nm, sizeof nm),
                         (togo > 0.0 ? togo : 0.0) / 1000.0);
        } else {
            snprintf(line, sizeof line, "%d   %-6s   DNF", i + 1, rr_racer_name(slot, nm, sizeof nm));
        }
        tx_c(W * 0.5f, y, line, col, 15.0f * S);
        y += 22.0f * S;
    }
    return y;
}

/* [F7] position + the field, top right, while racing */
static void rr_draw_positions(float W, float S)
{
    TD5_RoamStanding st[6];
    const int n = td5_roam_ai_standings(st, 6);
    char line[64], nm[16];
    float x, y;
    int i, rank = 0;
    if (n <= 1) return;
    for (i = 0; i < n; i++) if (st[i].slot == 0) rank = i + 1;
    x = W - 230.0f * S;
    y = 6.0f * S;
    qb_rect(x - 8.0f * S, y - 2.0f * S, 222.0f * S, (52.0f + 20.0f * (float)n) * S, 0x90000000);
    snprintf(line, sizeof line, "POS %d/%d", rank, n);
    tx(x, y, line, 0xFFFFFFFF, 26.0f * S);
    y += 38.0f * S;
    for (i = 0; i < n; i++) {
        const double d = st[i].along_m - R.along;
        const uint32_t col = st[i].slot == 0 ? 0xFFFFE070 : 0xFFC8D0DA;
        if (st[i].slot == 0) snprintf(line, sizeof line, "%d  %s", i + 1, rr_racer_name(0, nm, sizeof nm));
        else if (st[i].finished) snprintf(line, sizeof line, "%d  %-6s  FINISHED", i + 1, rr_racer_name(st[i].slot, nm, sizeof nm));
        else snprintf(line, sizeof line, "%d  %-6s  %.0f M %s", i + 1, rr_racer_name(st[i].slot, nm, sizeof nm), fabs(d),
                      d >= 0.0 ? "AHEAD" : "BEHIND");   /* the vector font has no '+' glyph */
        tx(x, y, line, col, 14.0f * S);
        y += 20.0f * S;
    }
}

static void rr_draw_results(float W, float H, float S)
{
    const float bw = 520.0f * S;
    const int circ = R.res.circuit;
    const int nrow = circ ? R.res.laps_done : (R.res.ncp > 1 ? R.res.ncp - 1 : 0);
    const int nmax = circ ? TD5_RRACE_MAX_LAPS : 8;
    const float rstep = circ ? 21.0f : 24.0f;
    const int nfld = R.res.nfield > 1 ? R.res.nfield : 0;
    const float bh = (circ ? 236.0f : 210.0f) * S + rstep * S * (float)(nrow > nmax ? nmax : nrow) + (nfld ? 34.0f + 22.0f * (float)nfld : 0.0f) * S;
    const float bx = (W - bw) * 0.5f, by = (H - bh) * 0.5f;
    char line[160], t[32], b[32];
    int i;
    qb_rect(bx - 3.0f * S, by - 3.0f * S, bw + 6.0f * S, bh + 6.0f * S, 0xFFFFB400);
    qb_rect(bx, by, bw, bh, 0xF011161D);
    if (nfld && R.res.player_rank > 0) {
        static const char *const k_ord[] = { "1ST", "2ND", "3RD", "4TH", "5TH", "6TH" };
        snprintf(line, sizeof line, "%s COMPLETE   %s OF %d", circ ? "CIRCUIT" : "SPRINT", k_ord[(R.res.player_rank - 1) % 6], nfld);
        tx_c(W * 0.5f, by + 14.0f * S, line, 0xFFFFB400, 26.0f * S);
    } else {
        tx_c(W * 0.5f, by + 14.0f * S, circ ? "CIRCUIT COMPLETE" : "SPRINT COMPLETE", 0xFFFFB400, 26.0f * S);
    }
    tx_c(W * 0.5f, by + 50.0f * S, R.name[0] ? R.name : "UNSAVED ROUTE", 0xFFB8C2CF, 16.0f * S);
    rr_fmt_time(t, sizeof t, R.res.ticks);
    snprintf(line, sizeof line, "TIME  %s", t);
    tx_c(W * 0.5f, by + 76.0f * S, line, 0xFFFFFFFF, 30.0f * S);
    rr_fmt_time(b, sizeof b, R.res.best_ticks);
    if (R.res.new_best && R.res.best_ticks > 0) snprintf(line, sizeof line, "NEW BEST   (WAS %s)", b);
    else if (R.res.new_best) snprintf(line, sizeof line, "NEW BEST");
    else if (R.res.best_ticks > 0) snprintf(line, sizeof line, "BEST  %s", b);
    else snprintf(line, sizeof line, "SAVE THE ROUTE TO KEEP A BEST TIME");
    tx_c(W * 0.5f, by + 116.0f * S, line, R.res.new_best ? 0xFF50E070 : 0xFFB8C2CF, 17.0f * S);
    if (circ)
        snprintf(line, sizeof line, "%d LAPS   %.2f KM   AVERAGE %.0f KM/H", R.res.laps, R.finish_total / 1000.0,
                 R.res.ticks > 0 ? R.finish_total / ((double)R.res.ticks / TD5_RRACE_TICK_HZ) * 3.6 : 0.0);
    else
        snprintf(line, sizeof line, "%.2f KM   AVERAGE %.0f KM/H", R.length_m / 1000.0,
                 R.res.ticks > 0 ? R.length_m / ((double)R.res.ticks / TD5_RRACE_TICK_HZ) * 3.6 : 0.0);
    tx_c(W * 0.5f, by + 142.0f * S, line, 0xFFB8C2CF, 15.0f * S);
    if (circ) {
        rr_fmt_time(t, sizeof t, R.res.best_lap_ticks);
        rr_fmt_time(b, sizeof b, R.res.best_lap_prev);
        if (R.res.new_best_lap && R.res.best_lap_prev > 0) snprintf(line, sizeof line, "BEST LAP  %s   NEW BEST  (WAS %s)", t, b);
        else if (R.res.new_best_lap) snprintf(line, sizeof line, "BEST LAP  %s   NEW BEST", t);
        else snprintf(line, sizeof line, "BEST LAP  %s", t);
        tx_c(W * 0.5f, by + 164.0f * S, line, R.res.new_best_lap ? 0xFF50E070 : 0xFFFFE070, 15.0f * S);
    }
    for (i = 0; i < nrow && i < nmax; i++) {
        if (circ) {
            const long lt = R.res.lap_ticks[i] - (i > 0 ? R.res.lap_ticks[i - 1] : 0);
            rr_fmt_time(t, sizeof t, lt);
            snprintf(line, sizeof line, "LAP %d   %s", i + 1, t);
            tx_c(W * 0.5f, by + (194.0f + rstep * (float)i) * S, line, lt == R.res.best_lap_ticks ? 0xFF50E070 : 0xFFFFFFFF, 14.0f * S);
        } else {
            rr_fmt_time(t, sizeof t, R.res.split_ticks[i]);
            snprintf(line, sizeof line, "CHECKPOINT %d   %s", i + 1, t);
            tx_c(W * 0.5f, by + (172.0f + 24.0f * (float)i) * S, line, 0xFFFFFFFF, 15.0f * S);
        }
    }
    if (nfld) rr_draw_field(W, by + ((circ ? 200.0f : 178.0f) + rstep * (float)(nrow > nmax ? nmax : nrow)) * S, S);
    tx_c(W * 0.5f, by + bh - 28.0f * S, "ENTER KEEP ROAMING   M ROUTE MAP   F5 RACE AGAIN", 0xFFFFE070, 15.0f * S);
}

void td5_roam_race_draw_hud(void)
{
    const float W = g_render_width_f, H = g_render_height_f, S = rr_scale();
    const uint32_t now = td5_plat_time_ms();
    char line[160], t[32];
    if (!td5_roam_active() || W < 2.0f) return;
    if (td5_roam_route_ui_map_open()) return;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR_HUD);
    if (td5_game_is_pause_menu_active()) {
        /* the pause menu's hint line for the race (the route planner draws its own above it) */
        const char *h = NULL;
        if (R.state == TD5_RRACE_OFF && rr_route_ready()) h = "R  RACE THIS ROUTE";
        else if (td5_roam_race_running()) h = "A  ABANDON THE SPRINT";
        if (h) {
            const float w = tx_w(h, 16.0f * S);
            qb_rect((W - w) * 0.5f - 12.0f * S, H - 88.0f * S, w + 24.0f * S, 30.0f * S, 0xB0000000);
            tx((W - w) * 0.5f, H - 82.0f * S, h, 0xFFFFE070, 16.0f * S);
        }
        qb_flush();
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        return;
    }
    if (R.state == TD5_RRACE_COUNTDOWN || R.state == TD5_RRACE_RACING) {
        const float bw = 360.0f * S, bx = (W - bw) * 0.5f;
        const double togo = R.finish_total - R.along;
        qb_rect(bx, 4.0f * S, bw, 64.0f * S, 0x90000000);
        rr_fmt_time(t, sizeof t, R.state == TD5_RRACE_RACING ? R.ticks : 0);
        tx_c(W * 0.5f, 6.0f * S, t, 0xFFFFFFFF, 26.0f * S);
        if (R.circuit) {
            /* [F9a] the lap counter (big, top left) and the lap line: last lap, best lap */
            char lp[32], bt[32], ls[32];
            const int lnow = td5_roam_race_lap_now();
            const long last = R.laps_done > 0 ? R.res.lap_ticks[R.laps_done - 1] - (R.laps_done > 1 ? R.res.lap_ticks[R.laps_done - 2] : 0) : 0;
            const long bestl = (R.best_lap > 0 && (R.res.best_lap_ticks <= 0 || R.best_lap < R.res.best_lap_ticks)) ? R.best_lap : R.res.best_lap_ticks;
            qb_rect(10.0f * S, 4.0f * S, 150.0f * S, 40.0f * S, 0x90000000);
            snprintf(lp, sizeof lp, "LAP %d/%d", lnow, R.laps);
            tx(18.0f * S, 8.0f * S, lp, 0xFFFFE070, 30.0f * S);
            rr_fmt_time(bt, sizeof bt, bestl);
            rr_fmt_time(ls, sizeof ls, last);
            snprintf(line, sizeof line, "LAST %s   BEST %s", last > 0 ? ls : "-", bestl > 0 ? bt : "-");
        } else if (R.cp_next < R.ncp - 1)
            snprintf(line, sizeof line, "CHECKPOINT %d/%d   %.2f KM TO GO", R.cp_next + 1, R.ncp - 1,
                     (togo > 0.0 ? togo : 0.0) / 1000.0);
        else
            snprintf(line, sizeof line, "FINISH   %.2f KM TO GO", (togo > 0.0 ? togo : 0.0) / 1000.0);
        tx_c(W * 0.5f, 36.0f * S, line, 0xFFC8D0DA, 14.0f * S);
        /* progress bar */
        {
            const double la = R.circuit ? R.along - (double)R.laps_done * R.length_m : R.along;
            const float f = R.length_m > 0.0 ? (float)(la / R.length_m) : 0.0f;
            qb_rect(bx + 10.0f * S, 58.0f * S, bw - 20.0f * S, 5.0f * S, 0xFF2A323D);
            qb_rect(bx + 10.0f * S, 58.0f * S, (bw - 20.0f * S) * (f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f)), 5.0f * S, 0xFFFFB400);
        }
        if (R.state == TD5_RRACE_RACING && now - R.go_ms < 1000u)
            tx_c(W * 0.5f, H * 0.5f - 40.0f * S, "GO!", 0xFF50E070, 60.0f * S);
        if (R.split_ms && now - R.split_ms < 3000u) {
            const float w = tx_w(R.split_txt, 18.0f * S);
            qb_rect((W - w) * 0.5f - 12.0f * S, 76.0f * S, w + 24.0f * S, 30.0f * S, 0xB0000000);
            tx((W - w) * 0.5f, 81.0f * S, R.split_txt, R.split_col, 18.0f * S);
        }
        if (R.wrong_t > 15) tx_c(W * 0.5f, H * 0.32f, "WRONG WAY", 0xFFFF3B3B, 40.0f * S);
        else if (R.off_t > 10) tx_c(W * 0.5f, H * 0.32f, "OFF ROUTE", 0xFFFFB400, 34.0f * S);
        rr_draw_positions(W, S);   /* [F7] */
    } else if (R.state == TD5_RRACE_FINISHED) {
        rr_draw_results(W, H, S);
    }
    qb_flush();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* [F10] the COPS row of the race in progress */
int td5_roam_race_cops(void) { return R.res.setup_cops ? 1 : 0; }

/* [F8] the TRAFFIC row of the race in progress: 0 off / 1 low / 2 high */
int td5_roam_race_traffic_level(void)
{
    const int t = R.res.setup_traffic;
    return t < 0 ? 0 : (t > 2 ? 2 : t);
}
