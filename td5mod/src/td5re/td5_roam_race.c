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
#include "td5_roam_world.h"         /* [HF] world origin */
#include "td5_roam_physics.h"       /* [HF] td5_roam_ground */
#include "td5_roam_ai.h"           /* [F7] the opponents: grid, driver, positions */
#include "td5_roam_lap.h"          /* [F9a] circuits: total distance + lap count */
#include "td5_roam_handoff.h"      /* [F11a] the geo selector / Quick Race route, planted once the graph is up */
#include "td5_input.h"              /* [roam RA] TD5_INPUT_JS_AXIS_CENTER: a pad word's packed pedal axis */
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
#define RR_RUNOUT_TICKS 120         /* [HF] after the line the car rolls on this long before it brakes */

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

/* [roam RA] One per local human (racer slot h = human h). A single-player race has one; a split-screen race
 * one per pane's human. Each runs its own progress, checkpoints / laps, finish and result; the race state goes
 * FINISHED (results) once EVERY human is through. */
typedef struct {
    double   along, lateral_m, max_along;
    int      seg;
    int      cp_next;
    long     wrong_t, off_t;
    int      laps_done;
    TD5_LapTrack lt;
    int      finished;             /* crossed the line (its run-out follows) */
    long     fin_t;                /* [HF] ticks since this human crossed the line */
    TD5_RoamRaceResult res;
    uint32_t split_ms;
    char     split_txt[96];
    uint32_t split_col;
} RrHuman;

static struct {
    int      state;
    TD5_RoamRaceSetup setup;
    /* the race's own copy of the route */
    double  *px, *pz, *dist;
    int      npt;
    double   length_m;
    char     name[TD5_ROUTE_NAME_MAX];
    uint32_t hash;
    double   cp_along[TD5_RRACE_MAX_CP];
    int      ncp;
    long     ticks;
    int      start_yaw;
    int32_t  start_px, start_pz;
    /* best run of this route (routes/<name>.best) */
    long     best_ticks;
    long     best_split[TD5_RRACE_MAX_CP];
    int      best_ncp;
    /* [roam RA] the humans of this race (slots 0..nh-1) */
    int      nh;
    RrHuman  hm[TD5_ROAM_MAX_HUMANS];
    /* requests the game takes */
    int      req_pause_close, req_countdown;
    int      req_leave;            /* [roam HA] the results were dismissed: leave to the geo generator */
    int      cd_armed, rearm;
    /* [F7] the field's clock (runs on past the humans' finish while opponents race) */
    long     clock;
    int      bar_hold;             /* every human finished, barriers stay up for the opponents */
    /* [F9a] a circuit: the loop, laps to run, the race distance */
    int      circuit, laps;
    TD5_RoamLoop loop;
    double   finish_total;          /* the race distance: a sprint = the route length, a circuit = laps x lap */
    long     best_lap;              /* fastest lap saved for this route (circuit), 0 = none */
    long     best_file_laps;        /* laps the saved total best was run over */
    /* messages */
    uint32_t go_ms;
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

static int rr_car_place(int slot, double *px, double *pz)
{
    TD5_Actor *a = td5_game_get_actor(slot);
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

/* [roam RA] the human of racer slot `slot`, NULL for an opponent / no race */
static RrHuman *rr_human(int slot) { return (slot >= 0 && slot < R.nh) ? &R.hm[slot] : NULL; }

/* [roam RA] every human through the line */
static int rr_all_finished(void)
{
    int h;
    for (h = 0; h < R.nh; h++) if (!R.hm[h].finished) return 0;
    return R.nh > 0;
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
static void rr_best_save(const TD5_RoamRaceResult *res, int total_best)
{
    char path[512];
    FILE *f;
    int i;
    if (!rr_best_path(path, sizeof path, R.name) || !(f = fopen(path, "w"))) {
        TD5_LOG_W(LOG_TAG, "ROAM_RACE: cannot write the best time of '%s'", R.name);
        return;
    }
    if (R.circuit) {
        const long keep = total_best ? res->ticks : R.best_ticks;
        const long bl = (R.best_lap > 0 && (res->best_lap_ticks <= 0 || R.best_lap < res->best_lap_ticks)) ? R.best_lap : res->best_lap_ticks;
        fprintf(f, "best_ticks=%ld\nhash=%08X\nlength_m=%.1f\ncircuit=1\nlaps=%d\nbest_lap=%ld\nsplits=\n", keep, (unsigned)R.hash,
                R.length_m, R.laps, bl);
        fclose(f);
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: circuit best for '%s' saved (total %ld ticks over %d laps, best lap %ld ticks) -> %s", R.name,
                  keep, R.laps, bl, path);
        return;
    }
    fprintf(f, "best_ticks=%ld\nhash=%08X\nlength_m=%.1f\nsplits=", res->ticks, (unsigned)R.hash, R.length_m);
    for (i = 0; i < res->ncp; i++) fprintf(f, "%s%ld", i ? "," : "", res->split_ticks[i]);
    fprintf(f, "\n");
    fclose(f);
    TD5_LOG_I(LOG_TAG, "ROAM_RACE: new best for '%s' saved (%ld ticks) -> %s", R.name, res->ticks, path);
}

/* ============================================================ state =========== */

int td5_roam_race_state(void) { return R.state; }
int td5_roam_race_running(void) { return R.state == TD5_RRACE_COUNTDOWN || R.state == TD5_RRACE_RACING; }
int td5_roam_race_barriers_live(void)
{
    return td5_roam_active() && (td5_roam_race_running() || (R.state == TD5_RRACE_FINISHED && R.bar_hold));
}
const TD5_RoamRaceResult *td5_roam_race_result(void) { return &R.hm[0].res; }
const TD5_RoamRaceResult *td5_roam_race_result_slot(int slot)
{
    return (slot >= 0 && slot < TD5_ROAM_MAX_HUMANS) ? &R.hm[slot].res : NULL;
}
int td5_roam_race_humans(void) { return R.state >= TD5_RRACE_COUNTDOWN ? R.nh : 0; }

void td5_roam_race_progress_slot(int slot, double *along_m, double *length_m, double *lateral_m)
{
    const RrHuman *hm = rr_human(slot);
    const int on = R.state >= TD5_RRACE_COUNTDOWN && hm;
    if (along_m) *along_m = on ? hm->along : 0.0;
    if (length_m) *length_m = on ? R.finish_total : 0.0;
    if (lateral_m) *lateral_m = on ? hm->lateral_m : 0.0;
}

void td5_roam_race_progress(double *along_m, double *length_m, double *lateral_m)
{
    td5_roam_race_progress_slot(0, along_m, length_m, lateral_m);
}

int td5_roam_race_circuit(void) { return R.state >= TD5_RRACE_COUNTDOWN && R.circuit; }
int td5_roam_race_laps(void) { return td5_roam_race_circuit() ? R.laps : 0; }

static int rr_lap_now(const RrHuman *hm)
{
    int l;
    if (!td5_roam_race_circuit() || !hm) return 0;
    if (R.state == TD5_RRACE_COUNTDOWN) return 1;
    l = hm->laps_done + 1;
    return l > R.laps ? R.laps : l;
}

int td5_roam_race_lap_now(void) { return rr_lap_now(&R.hm[0]); }

static void rr_to_off(const char *why)
{
    if (R.state != TD5_RRACE_OFF)
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: -> OFF (%s)", why);
    R.state = TD5_RRACE_OFF;
    R.req_pause_close = 0; R.req_countdown = 0; R.cd_armed = 0; R.rearm = 0; R.req_leave = 0;
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
    int h;
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
    for (h = 0; h < TD5_ROAM_MAX_HUMANS; h++) memset(&R.hm[h].res, 0, sizeof R.hm[h].res);
    R.setup.laps = 1;
    R.setup.opponents = -1;          /* [F7] the default, once the session knows what it loaded */
}

/* [F7] the field (humans + opponents) into each human's result, rank order */
static void rr_field_snapshot(void)
{
    TD5_RoamStanding st[6];
    const int n = td5_roam_ai_standings(st, 6);
    int i, h;
    if (n <= 0) return;
    for (h = 0; h < R.nh; h++) {
        TD5_RoamRaceResult *res = &R.hm[h].res;
        res->nfield = n;
        res->player_rank = td5_roam_ai_human_rank(h);
        for (i = 0; i < n; i++) {
            res->field_slot[i] = st[i].slot;
            res->field_ticks[i] = st[i].finished ? st[i].finish_ticks : 0;
            res->field_along_m[i] = st[i].along_m;
            res->field_laps[i] = st[i].laps_done;
        }
    }
}

void td5_roam_race_abandon(void)
{
    int h;
    if (R.state == TD5_RRACE_OFF) return;
    if (td5_roam_race_running()) {
        for (h = 0; h < R.nh; h++) {
            if (R.hm[h].finished) continue;   /* [roam RA] a human already through keeps the run */
            R.hm[h].res.finished = 0;
            R.hm[h].res.ticks = R.ticks;
        }
    }
    rr_to_off("abandoned");
}

long td5_roam_race_fin_ticks(void) { return R.state == TD5_RRACE_FINISHED ? R.hm[0].fin_t : 0; }   /* [HF] dev */

int td5_roam_race_player_finished(void) { return td5_roam_active() && R.state == TD5_RRACE_FINISHED; }

int td5_roam_race_human_finished(int slot)
{
    const RrHuman *hm = rr_human(slot);
    return td5_roam_active() && hm && R.state >= TD5_RRACE_RACING && hm->finished;
}

int td5_roam_race_human_wrong_way(int slot)
{
    const RrHuman *hm = rr_human(slot);
    return td5_roam_active() && hm && R.state == TD5_RRACE_RACING && !hm->finished && hm->wrong_t > 15;
}

int td5_roam_race_wrong_way(void) { return td5_roam_race_human_wrong_way(0); }

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

/* [roam HA] R in the route map / pause menu: race the planned route NOW. There is no in-race setup
 * panel any more: opponents / traffic / cops are the frontend's race settings (td5_roam_prefs_get, the
 * geo generator's RACE IN THE CITY panel), type and laps are the planner's route (T on the map). */
int td5_roam_race_start_prefs(void)
{
    TD5_RoamRacePrefs p;
    TD5_RoamRaceSetup s;
    if (!rr_route_ready()) {
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: start refused (need a planned sprint: START, FINISH, every leg planned)");
        return 0;
    }
    if (R.state != TD5_RRACE_OFF) return 0;
    td5_roam_prefs_get(&p);
    memset(&s, 0, sizeof s);
    s.laps = td5_roam_route_type() == TD5_ROUTE_CIRCUIT ? td5_roam_route_laps() : 1;
    s.opponents = p.opponents > td5_roam_ai_available() ? td5_roam_ai_available() : p.opponents;
    s.traffic = p.traffic;
    s.cops = p.cops;
    return td5_roam_race_start(&s);
}

/* Copy the planned route, project the checkpoints. */
static int rr_take_route(void)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    const int nwp = td5_roam_route_wp_count();
    int i, k, h, seg = 0;
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
    for (h = 0; h < TD5_ROAM_MAX_HUMANS; h++) {
        R.hm[h].laps_done = 0;
        td5_roam_lap_reset(&R.hm[h].lt);
    }
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
    int h;
    for (h = 0; h < R.nh; h++) {
        TD5_Actor *a = td5_game_get_actor(h);
        int32_t px, pz;
        int yaw;
        if (!a) continue;
        /* [F7] with opponents the player stands on its drawn grid spot (START or behind it); [roam RA] every
         * human on its own spot of the same grid (td5_roam_ai_human_grid), queued behind START without one */
        if (td5_roam_ai_human_grid(h, &px, &pz, &yaw)) td5_roam_place_actor(h, a, px, pz, yaw);
        else if (h == 0) td5_roam_place_actor(0, a, R.start_px, R.start_pz, R.start_yaw);
        else {
            const double yr = (double)R.start_yaw * (2.0 * RR_PI / 4096.0), back = 8.0 * RR_M * (double)h;
            td5_roam_place_actor(h, a, (int32_t)floor((double)R.start_px - sin(yr) * back + 0.5),
                                 (int32_t)floor((double)R.start_pz - cos(yr) * back + 0.5), R.start_yaw);
        }
    }
}

int td5_roam_race_start(const TD5_RoamRaceSetup *s)
{
    int nb = 0, h;
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

    R.nh = td5_roam_humans();       /* [roam RA] 1, or the split-screen humans */
    for (h = 0; h < TD5_ROAM_MAX_HUMANS; h++) {
        RrHuman *hm = &R.hm[h];
        TD5_RoamRaceResult *res = &hm->res;
        memset(res, 0, sizeof *res);
        snprintf(res->name, sizeof res->name, "%s", R.name);
        res->hash = R.hash;
        res->length_m = R.length_m;
        res->ncp = R.ncp;
        res->circuit = R.circuit; res->laps = R.laps; res->lap_len_m = R.circuit ? R.length_m : 0.0;
        res->best_lap_prev = R.best_lap;
        res->best_ticks = R.best_ticks;
        res->setup_traffic = R.setup.traffic;
        res->setup_cops = R.setup.cops;
        hm->along = 0.0; hm->max_along = 0.0; hm->lateral_m = 0.0; hm->seg = 0; hm->cp_next = 0;
        hm->wrong_t = 0; hm->off_t = 0;
        hm->finished = 0; hm->fin_t = 0;
        hm->split_ms = 0;
    }

    R.ticks = 0;
    R.go_ms = 0;
    R.clock = 0; R.bar_hold = 0;

    td5_roam_ai_race_end();          /* F5 from the results: the last race's field and spots go */
    td5_roam_ai_set_circuit(R.circuit ? R.laps : 0, R.length_m);   /* [F9a] consumed by the next ai race start */
    rr_place_on_grid();
    /* [F7] the opponents on the grid behind START (a short field when the street is tight); [roam RA] the
     * other humans' spots are drawn on the same grid, opponents or not */
    if (td5_roam_ai_race_start(R.px, R.pz, R.dist, R.npt, R.length_m, R.start_px, R.start_pz, R.start_yaw,
                               R.setup.opponents) > 0 || R.nh > 1)
        rr_place_on_grid();          /* the humans' drawn spots */
    rr_field_snapshot();
    t_grid = td5_plat_time_ms();
    R.state = TD5_RRACE_COUNTDOWN;
    R.req_countdown = 1; R.cd_armed = 0; R.rearm = 0;
    R.req_pause_close = td5_game_is_pause_menu_active();
    if (td5_roam_route_ui_map_open()) td5_roam_route_ui_close_map();
    if (R.circuit)
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: CIRCUIT %d lap(s) of %.1f m = %.1f m, best lap %ld ticks, best total %ld ticks (%ld laps)",
                  R.laps, R.length_m, R.finish_total, R.best_lap, R.best_ticks, R.best_file_laps);
    if (R.nh > 1)
        TD5_LOG_I(LOG_TAG, "ROAM_RACE: SPLIT SCREEN %d humans (slots 0..%d), test players mask 0x%X", R.nh, R.nh - 1,
                  g_td5.mp_ai_player_mask);
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

int td5_roam_race_take_leave(void)
{
    const int r = R.req_leave;
    R.req_leave = 0;
    return r;
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

/* [roam RA] the pedals of an input word, whatever the device. A pad / wheel word carries the PACKED Y AXIS in
 * bits 9..17 (throttle / brake / recover alias that payload: reference_input_word_packed_axis_aliases_action_bits):
 * the axis is rewritten (full up = throttle, full down = brake, centre = neither). A keyboard word: the bits. */
static uint32_t rr_pedals(uint32_t bits, int throttle, int brake)
{
    if (bits & (uint32_t)TD5_INPUT_ANALOG_Y_FLAG) {
        int ay = TD5_INPUT_JS_AXIS_CENTER - (throttle ? TD5_INPUT_JS_AXIS_CENTER : 0) + (brake ? TD5_INPUT_JS_AXIS_CENTER : 0);
        if (ay < 0) ay = 0;
        if (ay > 0x1FF) ay = 0x1FF;
        return (bits & ~(0x1FFu << 9)) | ((uint32_t)ay << 9);
    }
    bits &= ~(uint32_t)(TD5_INPUT_THROTTLE | TD5_INPUT_RECOVER | TD5_INPUT_BRAKE);
    if (throttle) bits |= (uint32_t)TD5_INPUT_THROTTLE;
    if (brake) bits |= (uint32_t)TD5_INPUT_BRAKE;
    return bits;
}

uint32_t td5_roam_race_filter_bits(int slot, uint32_t bits)
{
    RrHuman *hm = rr_human(slot);
    if (!hm || R.state == TD5_RRACE_OFF || !td5_roam_active()) return bits;
#ifndef TD5RE_RELEASE
    if (slot == 0 && R.state == TD5_RRACE_RACING && R.ticks >= 150 && R.ticks < 330 && td5_env_int("TD5RE_ROAM_HF_WRONG", 0, 0, 1))
        return (uint32_t)TD5_INPUT_THROTTLE;
#endif
    if (hm->finished) {
        /* [HF] the run-out after the line: throttle off, brake while the car still rolls, and let go of the
         * brake once it is (almost) stopped. A held brake at standstill is REVERSE: that was "the car goes
         * backwards after finishing". */
        TD5_Actor *a = td5_game_get_actor(slot);
        int thr = 0, brk = 0;
        if (a) {
            const double vx = a->linear_velocity_x / 256.0, vz = a->linear_velocity_z / 256.0;
            const double sp = sqrt(vx * vx + vz * vz);
            if (hm->fin_t < RR_RUNOUT_TICKS) {
                /* roll on past the line at ~40 km/h first: a car stopped AT the line blocks the opponents still
                 * racing (the roam-sprint-ai verdict caught it: an opponent stuck 13 s behind it) */
                if (sp < 11.0 * RR_M / TD5_RRACE_TICK_HZ) thr = 1;
            } else if (sp > 1.5 * RR_M / TD5_RRACE_TICK_HZ) brk = 1;
        }
        return rr_pedals(bits, thr, brk);
    }
    if ((R.state == TD5_RRACE_COUNTDOWN && !R.cd_armed) || R.rearm)
        return rr_pedals(bits, 0, 1);
    return bits;
}

/* ============================================================ progress ======== */

static void rr_progress(int h, TD5_Actor *a)
{
    RrHuman *hm = &R.hm[h];
    double cx, cz, best = 1e300, ba = hm->along;
    int i, bs = -1, lo = hm->seg, hi = hm->seg;
    if (!rr_car_place(h, &cx, &cz) || R.npt < 2) return;
    if (R.circuit) {
        /* [F9a] the TOTAL distance round the closed loop: continuous across the line, windowed along the route
         * so a figure 8 never jumps, wrong way judged on the loop's own direction */
        td5_roam_lap_update(&R.loop, &hm->lt, cx, cz, RR_LOST_M);
        hm->lateral_m = hm->lt.lat_m;
        if (hm->lateral_m > hm->res.max_lateral_m) hm->res.max_lateral_m = hm->lateral_m;
        if (hm->lt.lost) return;
        hm->seg = hm->lt.seg;
        hm->along = hm->lt.total;
        hm->max_along = hm->lt.max_total;
        {
            double ux, uz;
            const double vx = a->linear_velocity_x / 256.0, vz = a->linear_velocity_z / 256.0;
            const double sp = sqrt(vx * vx + vz * vz);
            td5_roam_loop_dir(&R.loop, hm->lt.seg, &ux, &uz);
            if (sp > 4.0 * RR_M / TD5_RRACE_TICK_HZ && (vx * ux + vz * uz) < -0.5 * sp) hm->wrong_t++;
            else if (hm->wrong_t > 0) hm->wrong_t = hm->wrong_t > 3 ? hm->wrong_t - 3 : 0;
        }
        if (hm->lateral_m > RR_OFF_ROUTE_M) hm->off_t++;
        else hm->off_t = 0;
        return;
    }
    while (lo > 0 && R.dist[lo] > hm->along - RR_WIN_BACK_M) lo--;
    while (hi + 2 < R.npt && R.dist[hi + 1] < hm->along + RR_WIN_AHEAD_M) hi++;
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
    hm->lateral_m = sqrt(best) / RR_M;
    if (hm->lateral_m > hm->res.max_lateral_m) hm->res.max_lateral_m = hm->lateral_m;
    if (hm->lateral_m > RR_LOST_M) return;           /* lost: keep the last answer */
    hm->seg = bs;
    hm->along = ba;
    if (hm->along > hm->max_along) hm->max_along = hm->along;

    /* wrong way: moving against the route's direction at more than ~14 km/h */
    {
        const double sx = R.px[bs + 1] - R.px[bs], sz = R.pz[bs + 1] - R.pz[bs];
        const double l = sqrt(sx * sx + sz * sz);
        const double vx = a->linear_velocity_x / 256.0, vz = a->linear_velocity_z / 256.0;
        const double sp = sqrt(vx * vx + vz * vz);
        if (l > 0.0 && sp > 4.0 * RR_M / TD5_RRACE_TICK_HZ && (vx * sx + vz * sz) / l < -0.5 * sp) hm->wrong_t++;
        else if (hm->wrong_t > 0) hm->wrong_t = hm->wrong_t > 3 ? hm->wrong_t - 3 : 0;
    }
    if (hm->lateral_m > RR_OFF_ROUTE_M) hm->off_t++;
    else hm->off_t = 0;
}

static void rr_finish(int h)
{
    RrHuman *hm = &R.hm[h];
    TD5_RoamRaceResult *res = &hm->res;
    char t[32], b[32];
    hm->finished = 1;
    hm->fin_t = 0;
    res->finished = 1;
    res->ticks = R.ticks;
    res->best_ticks = R.best_ticks;
    res->new_best = R.name[0] && (R.best_ticks <= 0 || R.ticks < R.best_ticks);
    res->laps_done = hm->laps_done;
    res->new_best_lap = R.circuit && R.name[0] && res->best_lap_ticks > 0 && (R.best_lap <= 0 || res->best_lap_ticks < R.best_lap);
    if (res->new_best || res->new_best_lap) rr_best_save(res, res->new_best);
    if (R.nh > 1) {
        /* [roam RA] the next human through compares with the best this race just set */
        if (res->new_best) R.best_ticks = res->ticks;
        if (res->new_best_lap) R.best_lap = res->best_lap_ticks;
    }
    if (rr_all_finished()) {
        R.state = TD5_RRACE_FINISHED;
        if (td5_roam_ai_racing()) {
            R.bar_hold = 1;              /* [F7] opponents still run: their barriers stay */
        } else {
            td5_roam_race_wall_end();
            td5_roam_barriers_set_visible(0);
        }
    }
    rr_fmt_time(t, sizeof t, R.ticks);
    rr_fmt_time(b, sizeof b, res->best_ticks);
    TD5_LOG_I(LOG_TAG, "ROAM_RACE: FINISH%s route='%s' time=%s (%ld ticks) best=%s%s wrong_way_ticks=%ld off_route_ticks=%ld "
              "max_lateral=%.1f m length=%.1f m", R.nh > 1 ? (h == 0 ? " human 1" : h == 1 ? " human 2" : h == 2 ? " human 3" :
              h == 3 ? " human 4" : h == 4 ? " human 5" : " human 6") : "", R.name[0] ? R.name : "UNSAVED", t, R.ticks,
              res->best_ticks > 0 ? b : "-", res->new_best ? " NEW BEST" : "", res->wrong_way_ticks,
              res->off_route_ticks, res->max_lateral_m, R.length_m);
    if (R.nh > 1 && R.state == TD5_RRACE_FINISHED) TD5_LOG_I(LOG_TAG, "ROAM_RACE: every human is through (%ld ticks)", R.ticks);
}

/* [roam RA] the opponents' progress + positions with every human's distance / finish */
static void rr_ai_progress(long clock, int racing)
{
    double along[TD5_ROAM_MAX_HUMANS];
    int fin[TD5_ROAM_MAX_HUMANS];
    long fint[TD5_ROAM_MAX_HUMANS];
    int h;
    for (h = 0; h < R.nh; h++) {
        along[h] = R.hm[h].along;
        fin[h] = R.hm[h].finished;
        fint[h] = R.hm[h].res.ticks;
    }
    td5_roam_ai_progress(clock, racing, R.nh, along, fin, fint);
}

/* one human's tick in RACING: progress, laps, checkpoints, finish */
static void rr_tick_human(int h, TD5_Actor *a)
{
    RrHuman *hm = &R.hm[h];
    TD5_RoamRaceResult *res = &hm->res;
    rr_progress(h, a);
    if (hm->wrong_t > 15) res->wrong_way_ticks++;
    if (hm->off_t > 10) res->off_route_ticks++;
    if (R.circuit && R.state == TD5_RRACE_RACING && hm->lt.init) {
        /* [F9a] lap k is counted once, when the running maximum of the total distance first reaches k x lap */
        int now = td5_roam_lap_count(&R.loop, &hm->lt, 0.0);
        if (hm->lt.max_total >= R.finish_total - RR_FINISH_EPS_M) now = R.laps;
        if (now > R.laps) now = R.laps;
        if (now > hm->laps_done && hm->lateral_m < RR_CP_RADIUS_M) {
            if (now > hm->laps_done + 1) {
                res->lap_jumps++;
                TD5_LOG_W(LOG_TAG, "ROAM_RACE: lap count jumped %d -> %d (total %.1f m)", hm->laps_done, now, hm->lt.total);
            }
            while (hm->laps_done < now) {
                const int k = hm->laps_done;
                const long lt = R.ticks - (k > 0 ? res->lap_ticks[k - 1] : 0);
                char t[32], b[32];
                res->lap_ticks[k] = R.ticks;
                hm->laps_done++;
                res->laps_done = hm->laps_done;
                if (res->best_lap_ticks <= 0 || lt < res->best_lap_ticks) res->best_lap_ticks = lt;
                rr_fmt_time(t, sizeof t, lt);
                rr_fmt_time(b, sizeof b, R.best_lap);
                if (hm->laps_done < R.laps) {
                    if (R.best_lap > 0) {
                        const long d = lt - R.best_lap;
                        snprintf(hm->split_txt, sizeof hm->split_txt, "LAP %d   %s   %c%ld.%02ld", k + 1, t, d <= 0 ? '-' : '+',
                                 labs(d) / TD5_RRACE_TICK_HZ, (labs(d) % TD5_RRACE_TICK_HZ) * 100 / TD5_RRACE_TICK_HZ);
                        hm->split_col = d <= 0 ? 0xFF50E070 : 0xFFFF6A50;
                    } else {
                        snprintf(hm->split_txt, sizeof hm->split_txt, "LAP %d   %s", k + 1, t);
                        hm->split_col = 0xFFFFFFFF;
                    }
                    hm->split_ms = td5_plat_time_ms();
                }
                TD5_LOG_I(LOG_TAG, "ROAM_RACE: %slap %d/%d done in %s (%ld ticks), race %ld ticks, total %.1f m, best saved %s",
                          R.nh > 1 ? (h == 0 ? "human 1 " : h == 1 ? "human 2 " : h == 2 ? "human 3 " : h == 3 ? "human 4 " :
                          h == 4 ? "human 5 " : "human 6 ") : "",
                          k + 1, R.laps, t, lt, R.ticks, hm->lt.total, R.best_lap > 0 ? b : "-");
            }
            if (hm->laps_done >= R.laps) rr_finish(h);
        }
    }
#ifndef TD5RE_RELEASE
    if (s_dev_hold_finish) return;   /* [HE] DEV: the wall test teleports */
#endif
    while (!hm->finished && hm->cp_next < R.ncp &&
           hm->along >= R.cp_along[hm->cp_next] - (hm->cp_next == R.ncp - 1 ? RR_FINISH_EPS_M : 3.0) &&
           hm->lateral_m < RR_CP_RADIUS_M) {
        const int k = hm->cp_next++;
        char t[32];
        res->split_ticks[k] = R.ticks;
        rr_fmt_time(t, sizeof t, R.ticks);
        if (k < R.ncp - 1) {
            if (k < R.best_ncp && R.best_split[k] > 0) {
                const long d = R.ticks - R.best_split[k];
                snprintf(hm->split_txt, sizeof hm->split_txt, "CHECKPOINT %d   %s   %c%ld.%02ld", k + 1, t, d <= 0 ? '-' : '+',
                         labs(d) / TD5_RRACE_TICK_HZ, (labs(d) % TD5_RRACE_TICK_HZ) * 100 / TD5_RRACE_TICK_HZ);
                hm->split_col = d <= 0 ? 0xFF50E070 : 0xFFFF6A50;
            } else {
                snprintf(hm->split_txt, sizeof hm->split_txt, "CHECKPOINT %d   %s", k + 1, t);
                hm->split_col = 0xFFFFFFFF;
            }
            hm->split_ms = td5_plat_time_ms();
            TD5_LOG_I(LOG_TAG, "ROAM_RACE: %scheckpoint %d/%d at %.1f m, %s (%ld ticks)",
                      R.nh > 1 ? (h == 0 ? "human 1 " : h == 1 ? "human 2 " : h == 2 ? "human 3 " : h == 3 ? "human 4 " :
                      h == 4 ? "human 5 " : "human 6 ") : "", k + 1, R.ncp - 1, R.cp_along[k], t, R.ticks);
        } else {
            rr_finish(h);
            break;
        }
    }
}

void td5_roam_race_tick(void)
{
    TD5_Actor *a;
    int h;
    if (!td5_roam_active()) return;
    if (R.state == TD5_RRACE_FINISHED) {
        for (h = 0; h < R.nh; h++) R.hm[h].fin_t++;
#ifndef TD5RE_RELEASE
        /* [HF] dev: the player's speed every tick from the line until rest (TD5RE_ROAM_HF_FINLOG=1) */
        if (td5_env_int("TD5RE_ROAM_HF_FINLOG", 0, 0, 1)) {
            TD5_Actor *fa = td5_game_get_actor(0);
            if (fa) {
                const double vx = fa->linear_velocity_x / 256.0, vz = fa->linear_velocity_z / 256.0;
                TD5_LOG_I(LOG_TAG, "ROAM_FIN: t=%ld fwd_raw=%d speed_kmh=%.2f", R.hm[0].fin_t, (int)fa->longitudinal_speed,
                          sqrt(vx * vx + vz * vz) * TD5_RRACE_TICK_HZ * 3.6 / RR_M);
            }
        }
#endif
        /* [F7] the humans are through; the field's clock runs on until every opponent is */
        if (td5_roam_ai_racing()) {
            R.clock++;
            rr_ai_progress(R.clock, 1);
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
            double z[TD5_ROAM_MAX_HUMANS] = { 0 };
            int f[TD5_ROAM_MAX_HUMANS] = { 0 };
            long ft[TD5_ROAM_MAX_HUMANS] = { 0 };
            td5_roam_ai_progress(0, 0, R.nh, z, f, ft);   /* [F7] grid order */
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
#ifndef TD5RE_RELEASE
    if (td5_env_int("TD5RE_ROAM_HF_WRONG", 0, 0, 1) && R.ticks == 150) {
        /* [HF] dev: turn the car through 180 degrees on the spot, the filter then drives it straight ahead */
        double cx, cz;
        if (rr_car_place(0, &cx, &cz))
            td5_roam_place_actor(0, a, (int32_t)cx, (int32_t)cz, (int)((a->euler_accum.yaw >> 8) + 2048) & 0xFFF);
    }
#endif
    for (h = 0; h < R.nh; h++) {
        TD5_Actor *ha = td5_game_get_actor(h);
        if (R.hm[h].finished) { R.hm[h].fin_t++; continue; }   /* [roam RA] through: its run-out, the others race on */
        if (ha) rr_tick_human(h, ha);
    }
    /* [F7] opponents' progress + positions, the humans' finishes (this tick) included */
    rr_ai_progress(R.clock, 1);
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
        if (paused && !td5_roam_route_ui_map_open() && ke(K_R)) td5_roam_race_start_prefs();
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
        /* [roam HA] "after a sprint is complete go back to the main menu of the geospatial generator"
         * (sprint and circuit alike): ENTER on the results leaves the session, the game fades out
         * and lands on the geo generator (td5_game.c, the same exit as LEAVE FREE ROAM) */
        if (ke(K_ENTER) && !td5_env_int("TD5RE_ROAM_HF_FINLOG", 0, 0, 1)) { R.req_leave = 1; break; }
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

/* the HUD's scale for a pane `h` pixels tall (the full screen in single player) */
static float rr_scale(float h)
{
    const float s = h / 720.0f;
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

/* [F7] "YOU" / "CPU n"; [roam RA] split screen: "P1".."P6" for the humans, the opponents counted after them */
static const char *rr_racer_name(int slot, char *buf, size_t cap)
{
    if (R.nh > 1 && slot < R.nh) { snprintf(buf, cap, "P%d", slot + 1); return buf; }
    if (slot == 0) return "YOU";
    snprintf(buf, cap, "CPU %d", R.nh > 1 ? slot - R.nh + 1 : slot);
    return buf;
}

static uint32_t rr_name_col(int slot, int me, uint32_t other)
{
    if (slot == me) return 0xFFFFE070;
    if (R.nh > 1 && slot < R.nh) return 0xFF8FD3FF;   /* [roam RA] another human */
    return other;
}

/* [F7] the field under the results: rank, name, time and gap to the winner (or what is left to run) */
static float rr_draw_field(const TD5_RoamRaceResult *res, int me, float X0, float W, float y, float S)
{
    char line[160], t[32], nm[16];
    int i;
    const long win = res->nfield > 0 ? res->field_ticks[0] : 0;
    tx_c(X0 + W * 0.5f, y, "FINISHING ORDER", 0xFFFFB400, 16.0f * S);
    y += 24.0f * S;
    for (i = 0; i < res->nfield && i < 6; i++) {
        const int slot = res->field_slot[i];
        const long ft = res->field_ticks[i];
        const uint32_t col = rr_name_col(slot, me, 0xFFFFFFFF);
        if (ft > 0) {
            rr_fmt_time(t, sizeof t, ft);
            if (i == 0 || win <= 0) snprintf(line, sizeof line, "%d   %-6s   %s", i + 1, rr_racer_name(slot, nm, sizeof nm), t);
            else snprintf(line, sizeof line, "%d   %-6s   %s   GAP %ld.%02ld", i + 1, rr_racer_name(slot, nm, sizeof nm), t,
                          (ft - win) / TD5_RRACE_TICK_HZ, ((ft - win) % TD5_RRACE_TICK_HZ) * 100 / TD5_RRACE_TICK_HZ);
        } else if (td5_roam_ai_count() > 0) {
            const double togo = R.finish_total - res->field_along_m[i];
            if (res->circuit)
                snprintf(line, sizeof line, "%d   %-6s   RACING   LAP %d/%d", i + 1, rr_racer_name(slot, nm, sizeof nm),
                         res->field_laps[i] + 1 > R.laps ? R.laps : res->field_laps[i] + 1, R.laps);
            else
                snprintf(line, sizeof line, "%d   %-6s   RACING   %.2f KM TO GO", i + 1, rr_racer_name(slot, nm, sizeof nm),
                         (togo > 0.0 ? togo : 0.0) / 1000.0);
        } else {
            snprintf(line, sizeof line, "%d   %-6s   DNF", i + 1, rr_racer_name(slot, nm, sizeof nm));
        }
        tx_c(X0 + W * 0.5f, y, line, col, 15.0f * S);
        y += 22.0f * S;
    }
    return y;
}

/* [F7] position + the field, top right of the pane, while racing */
static void rr_draw_positions(int me, float X0, float Y0, float W, float S)
{
    TD5_RoamStanding st[6];
    const int n = td5_roam_ai_standings(st, 6);
    const RrHuman *hm = &R.hm[me];
    char line[64], nm[16];
    float x, y;
    int i, rank = 0;
    if (n <= 1) return;
    for (i = 0; i < n; i++) if (st[i].slot == me) rank = i + 1;
    x = X0 + W - 230.0f * S;
    y = Y0 + 6.0f * S;
    qb_rect(x - 8.0f * S, y - 2.0f * S, 222.0f * S, (52.0f + 20.0f * (float)n) * S, 0x90000000);
    snprintf(line, sizeof line, "POS %d/%d", rank, n);
    tx(x, y, line, 0xFFFFFFFF, 26.0f * S);
    y += 38.0f * S;
    for (i = 0; i < n; i++) {
        const double d = st[i].along_m - hm->along;
        const uint32_t col = rr_name_col(st[i].slot, me, 0xFFC8D0DA);
        if (st[i].slot == me) snprintf(line, sizeof line, "%d  %s", i + 1, rr_racer_name(me, nm, sizeof nm));
        else if (st[i].finished) snprintf(line, sizeof line, "%d  %-6s  FINISHED", i + 1, rr_racer_name(st[i].slot, nm, sizeof nm));
        else snprintf(line, sizeof line, "%d  %-6s  %.0f M %s", i + 1, rr_racer_name(st[i].slot, nm, sizeof nm), fabs(d),
                      d >= 0.0 ? "AHEAD" : "BEHIND");   /* the vector font has no '+' glyph */
        tx(x, y, line, col, 14.0f * S);
        y += 20.0f * S;
    }
}

static void rr_draw_results(float W, float H, float S)
{
    const TD5_RoamRaceResult *res = &R.hm[0].res;
    const float bw = 520.0f * S;
    const int circ = res->circuit;
    const int nrow = circ ? res->laps_done : (res->ncp > 1 ? res->ncp - 1 : 0);
    const int nmax = circ ? TD5_RRACE_MAX_LAPS : 8;
    const float rstep = circ ? 21.0f : 24.0f;
    const int nfld = res->nfield > 1 ? res->nfield : 0;
    const float bh = (circ ? 236.0f : 210.0f) * S + rstep * S * (float)(nrow > nmax ? nmax : nrow) + (nfld ? 34.0f + 22.0f * (float)nfld : 0.0f) * S;
    const float bx = (W - bw) * 0.5f, by = (H - bh) * 0.5f;
    char line[160], t[32], b[32];
    int i;
    qb_rect(bx - 3.0f * S, by - 3.0f * S, bw + 6.0f * S, bh + 6.0f * S, 0xFFFFB400);
    qb_rect(bx, by, bw, bh, 0xF011161D);
    if (nfld && res->player_rank > 0) {
        static const char *const k_ord[] = { "1ST", "2ND", "3RD", "4TH", "5TH", "6TH" };
        snprintf(line, sizeof line, "%s COMPLETE   %s OF %d", circ ? "CIRCUIT" : "SPRINT", k_ord[(res->player_rank - 1) % 6], nfld);
        tx_c(W * 0.5f, by + 14.0f * S, line, 0xFFFFB400, 26.0f * S);
    } else {
        tx_c(W * 0.5f, by + 14.0f * S, circ ? "CIRCUIT COMPLETE" : "SPRINT COMPLETE", 0xFFFFB400, 26.0f * S);
    }
    tx_c(W * 0.5f, by + 50.0f * S, R.name[0] ? R.name : "UNSAVED ROUTE", 0xFFB8C2CF, 16.0f * S);
    rr_fmt_time(t, sizeof t, res->ticks);
    snprintf(line, sizeof line, "TIME  %s", t);
    tx_c(W * 0.5f, by + 76.0f * S, line, 0xFFFFFFFF, 30.0f * S);
    rr_fmt_time(b, sizeof b, res->best_ticks);
    if (res->new_best && res->best_ticks > 0) snprintf(line, sizeof line, "NEW BEST   (WAS %s)", b);
    else if (res->new_best) snprintf(line, sizeof line, "NEW BEST");
    else if (res->best_ticks > 0) snprintf(line, sizeof line, "BEST  %s", b);
    else snprintf(line, sizeof line, "SAVE THE ROUTE TO KEEP A BEST TIME");
    tx_c(W * 0.5f, by + 116.0f * S, line, res->new_best ? 0xFF50E070 : 0xFFB8C2CF, 17.0f * S);
    if (circ)
        snprintf(line, sizeof line, "%d LAPS   %.2f KM   AVERAGE %.0f KM/H", res->laps, R.finish_total / 1000.0,
                 res->ticks > 0 ? R.finish_total / ((double)res->ticks / TD5_RRACE_TICK_HZ) * 3.6 : 0.0);
    else
        snprintf(line, sizeof line, "%.2f KM   AVERAGE %.0f KM/H", R.length_m / 1000.0,
                 res->ticks > 0 ? R.length_m / ((double)res->ticks / TD5_RRACE_TICK_HZ) * 3.6 : 0.0);
    tx_c(W * 0.5f, by + 142.0f * S, line, 0xFFB8C2CF, 15.0f * S);
    if (circ) {
        rr_fmt_time(t, sizeof t, res->best_lap_ticks);
        rr_fmt_time(b, sizeof b, res->best_lap_prev);
        if (res->new_best_lap && res->best_lap_prev > 0) snprintf(line, sizeof line, "BEST LAP  %s   NEW BEST  (WAS %s)", t, b);
        else if (res->new_best_lap) snprintf(line, sizeof line, "BEST LAP  %s   NEW BEST", t);
        else snprintf(line, sizeof line, "BEST LAP  %s", t);
        tx_c(W * 0.5f, by + 164.0f * S, line, res->new_best_lap ? 0xFF50E070 : 0xFFFFE070, 15.0f * S);
    }
    for (i = 0; i < nrow && i < nmax; i++) {
        if (circ) {
            const long lt = res->lap_ticks[i] - (i > 0 ? res->lap_ticks[i - 1] : 0);
            rr_fmt_time(t, sizeof t, lt);
            snprintf(line, sizeof line, "LAP %d   %s", i + 1, t);
            tx_c(W * 0.5f, by + (194.0f + rstep * (float)i) * S, line, lt == res->best_lap_ticks ? 0xFF50E070 : 0xFFFFFFFF, 14.0f * S);
        } else {
            rr_fmt_time(t, sizeof t, res->split_ticks[i]);
            snprintf(line, sizeof line, "CHECKPOINT %d   %s", i + 1, t);
            tx_c(W * 0.5f, by + (172.0f + 24.0f * (float)i) * S, line, 0xFFFFFFFF, 15.0f * S);
        }
    }
    if (nfld) rr_draw_field(res, 0, 0.0f, W, by + ((circ ? 200.0f : 178.0f) + rstep * (float)(nrow > nmax ? nmax : nrow)) * S, S);
    tx_c(W * 0.5f, by + bh - 28.0f * S, "ENTER BACK TO THE GENERATOR   M ROUTE MAP   F5 RACE AGAIN", 0xFFFFE070, 15.0f * S);
}

/* [roam RA] a human's splits on one line: checkpoint times (sprint) or lap times (circuit) */
static void rr_splits_line(const TD5_RoamRaceResult *res, char *out, size_t cap)
{
    char t[32];
    int i, n;
    size_t len;
    if (res->circuit) {
        snprintf(out, cap, "LAPS");
        n = res->laps_done;
    } else {
        snprintf(out, cap, "SPLITS");
        n = res->ncp > 1 ? res->ncp - 1 : 0;
    }
    if (!res->finished && n == 0) { snprintf(out, cap, "DID NOT FINISH"); return; }
    for (i = 0; i < n && i < 8; i++) {
        const long v = res->circuit ? res->lap_ticks[i] - (i > 0 ? res->lap_ticks[i - 1] : 0) : res->split_ticks[i];
        if (v <= 0) break;
        rr_fmt_time(t, sizeof t, v);
        len = strlen(out);
        if (len + 12 >= cap) break;
        snprintf(out + len, cap - len, "   %s", t);
    }
    if (n == 0 && !res->circuit) {
        len = strlen(out);
        snprintf(out + len, cap - len, "   NONE ON THIS ROUTE");
    }
}

/* [roam RA] the split-screen results: one panel over every pane, the whole field in finishing order, then each
 * human's total, best-time verdict and splits / laps */
static void rr_draw_results_mp(float W, float H, float S)
{
    const TD5_RoamRaceResult *r0 = &R.hm[0].res;
    const int circ = r0->circuit;
    const int nfld = r0->nfield > 0 ? r0->nfield : 0;
    const float bw = 680.0f * S;
    const float bh = (150.0f + 22.0f * (float)nfld + 46.0f * (float)R.nh + 40.0f) * S;
    const float bx = (W - bw) * 0.5f, by = (H - bh) * 0.5f;
    char line[200], t[32], nm[16];
    float y;
    int h;
    qb_rect(bx - 3.0f * S, by - 3.0f * S, bw + 6.0f * S, bh + 6.0f * S, 0xFFFFB400);
    qb_rect(bx, by, bw, bh, 0xF011161D);
    tx_c(W * 0.5f, by + 14.0f * S, circ ? "CIRCUIT COMPLETE" : "SPRINT COMPLETE", 0xFFFFB400, 26.0f * S);
    if (circ) snprintf(line, sizeof line, "%s   %d LAPS   %.2f KM", R.name[0] ? R.name : "UNSAVED ROUTE", R.laps, R.finish_total / 1000.0);
    else snprintf(line, sizeof line, "%s   %.2f KM", R.name[0] ? R.name : "UNSAVED ROUTE", R.length_m / 1000.0);
    tx_c(W * 0.5f, by + 50.0f * S, line, 0xFFB8C2CF, 16.0f * S);
    y = by + 80.0f * S;
    if (nfld) y = rr_draw_field(r0, -1, 0.0f, W, y, S) + 8.0f * S;
    for (h = 0; h < R.nh; h++) {
        const TD5_RoamRaceResult *res = &R.hm[h].res;
        static const char *const k_ord[] = { "1ST", "2ND", "3RD", "4TH", "5TH", "6TH" };
        rr_fmt_time(t, sizeof t, res->ticks);
        if (res->finished)
            snprintf(line, sizeof line, "%s   %s   %s%s", rr_racer_name(h, nm, sizeof nm),
                     res->player_rank > 0 ? k_ord[(res->player_rank - 1) % 6] : "-", t, res->new_best ? "   NEW BEST" :
                     (res->new_best_lap ? "   NEW BEST LAP" : ""));
        else
            snprintf(line, sizeof line, "%s   DID NOT FINISH", rr_racer_name(h, nm, sizeof nm));
        tx_c(W * 0.5f, y, line, rr_name_col(h, -1, 0xFFFFFFFF), 17.0f * S);
        y += 22.0f * S;
        rr_splits_line(res, line, sizeof line);
        tx_c(W * 0.5f, y, line, 0xFFC8D0DA, 13.0f * S);
        y += 24.0f * S;
    }
    tx_c(W * 0.5f, by + bh - 28.0f * S, "ENTER BACK TO THE GENERATOR   F5 RACE AGAIN", 0xFFFFE070, 15.0f * S);
}

/* the race HUD of human `me` inside the pane (X0, Y0, W, H) */
static void rr_draw_pane(int me, float X0, float Y0, float W, float H, uint32_t now)
{
    const RrHuman *hm = &R.hm[me];
    const float S = rr_scale(H);
    char line[160], t[32];
    if (hm->finished && R.state == TD5_RRACE_RACING) {
        /* [roam RA] through the line while another human still races: the pane holds its result */
        static const char *const k_ord[] = { "1ST", "2ND", "3RD", "4TH", "5TH", "6TH" };
        const float bw = 360.0f * S, bx = X0 + (W - bw) * 0.5f;
        qb_rect(bx, Y0 + 4.0f * S, bw, 64.0f * S, 0x90000000);
        rr_fmt_time(t, sizeof t, hm->res.ticks);
        snprintf(line, sizeof line, "FINISHED %s   %s", hm->res.player_rank > 0 ? k_ord[(hm->res.player_rank - 1) % 6] : "", t);
        tx_c(X0 + W * 0.5f, Y0 + 8.0f * S, line, 0xFF50E070, 22.0f * S);
        tx_c(X0 + W * 0.5f, Y0 + 40.0f * S, "WAITING FOR THE OTHER PLAYERS", 0xFFC8D0DA, 14.0f * S);
        rr_draw_positions(me, X0, Y0, W, S);
        return;
    }
    {
        const float bw = 360.0f * S, bx = X0 + (W - bw) * 0.5f;
        const double togo = R.finish_total - hm->along;
        qb_rect(bx, Y0 + 4.0f * S, bw, 64.0f * S, 0x90000000);
        rr_fmt_time(t, sizeof t, R.state == TD5_RRACE_RACING ? R.ticks : 0);
        tx_c(X0 + W * 0.5f, Y0 + 6.0f * S, t, 0xFFFFFFFF, 26.0f * S);
        if (R.circuit) {
            /* [F9a] the lap counter (big, top left) and the lap line: last lap, best lap */
            char lp[32], bt[32], ls[32];
            const int lnow = rr_lap_now(hm);
            const long last = hm->laps_done > 0 ? hm->res.lap_ticks[hm->laps_done - 1] - (hm->laps_done > 1 ? hm->res.lap_ticks[hm->laps_done - 2] : 0) : 0;
            const long bestl = (R.best_lap > 0 && (hm->res.best_lap_ticks <= 0 || R.best_lap < hm->res.best_lap_ticks)) ? R.best_lap : hm->res.best_lap_ticks;
            qb_rect(X0 + 10.0f * S, Y0 + 4.0f * S, 150.0f * S, 40.0f * S, 0x90000000);
            snprintf(lp, sizeof lp, "LAP %d/%d", lnow, R.laps);
            tx(X0 + 18.0f * S, Y0 + 8.0f * S, lp, 0xFFFFE070, 30.0f * S);
            rr_fmt_time(bt, sizeof bt, bestl);
            rr_fmt_time(ls, sizeof ls, last);
            snprintf(line, sizeof line, "LAST %s   BEST %s", last > 0 ? ls : "-", bestl > 0 ? bt : "-");
        } else if (hm->cp_next < R.ncp - 1)
            snprintf(line, sizeof line, "CHECKPOINT %d/%d   %.2f KM TO GO", hm->cp_next + 1, R.ncp - 1,
                     (togo > 0.0 ? togo : 0.0) / 1000.0);
        else
            snprintf(line, sizeof line, "FINISH   %.2f KM TO GO", (togo > 0.0 ? togo : 0.0) / 1000.0);
        tx_c(X0 + W * 0.5f, Y0 + 36.0f * S, line, 0xFFC8D0DA, 14.0f * S);
        /* progress bar */
        {
            const double la = R.circuit ? hm->along - (double)hm->laps_done * R.length_m : hm->along;
            const float f = R.length_m > 0.0 ? (float)(la / R.length_m) : 0.0f;
            qb_rect(bx + 10.0f * S, Y0 + 58.0f * S, bw - 20.0f * S, 5.0f * S, 0xFF2A323D);
            qb_rect(bx + 10.0f * S, Y0 + 58.0f * S, (bw - 20.0f * S) * (f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f)), 5.0f * S, 0xFFFFB400);
        }
        if (R.state == TD5_RRACE_RACING && now - R.go_ms < 1000u)
            tx_c(X0 + W * 0.5f, Y0 + H * 0.5f - 40.0f * S, "GO!", 0xFF50E070, 60.0f * S);
        if (hm->split_ms && now - hm->split_ms < 3000u) {
            const float w = tx_w(hm->split_txt, 18.0f * S);
            qb_rect(X0 + (W - w) * 0.5f - 12.0f * S, Y0 + 76.0f * S, w + 24.0f * S, 30.0f * S, 0xB0000000);
            tx(X0 + (W - w) * 0.5f, Y0 + 81.0f * S, hm->split_txt, hm->split_col, 18.0f * S);
        }
        /* [HF] WRONG WAY is the stock HUD's (td5_hud.c, from td5_roam_race_human_wrong_way) */
        if (hm->wrong_t <= 15 && hm->off_t > 10) tx_c(X0 + W * 0.5f, Y0 + H * 0.32f, "OFF ROUTE", 0xFFFFB400, 34.0f * S);
        rr_draw_positions(me, X0, Y0, W, S);   /* [F7] */
    }
}

void td5_roam_race_draw_hud(void)
{
    const float W = g_render_width_f, H = g_render_height_f, S = rr_scale(g_render_height_f);
    const uint32_t now = td5_plat_time_ms();
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
        if (R.nh <= 1) {
            rr_draw_pane(0, 0.0f, 0.0f, W, H, now);
        } else {
            /* [roam RA] each pane: the HUD of the human it shows */
            const int panes = td5_hud_pane_count();
            int v;
            for (v = 0; v < panes; v++) {
                const int slot = td5_game_get_player_slot(v);
                float x0, y0, pw, ph;
                if (slot < 0 || slot >= R.nh || !td5_hud_pane_rect(v, &x0, &y0, &pw, &ph)) continue;
                rr_draw_pane(slot, x0, y0, pw, ph, now);
            }
        }
    } else if (R.state == TD5_RRACE_FINISHED) {
        if (R.nh <= 1) rr_draw_results(W, H, S);
        else rr_draw_results_mp(W, H, S);
    }
    qb_flush();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* [F10] the COPS row of the race in progress */
int td5_roam_race_cops(void) { return R.hm[0].res.setup_cops ? 1 : 0; }

/* [F8] the TRAFFIC row of the race in progress: 0 off / 1 low / 2 high */
int td5_roam_race_traffic_level(void)
{
    const int t = R.hm[0].res.setup_traffic;
    return t < 0 ? 0 : (t > 2 ? 2 : t);
}

/* ============================================================ gantries (HF) ===== */
/* START / FINISH gantries across the road at the route's two ends: two posts and a banner (green START,
 * chequered FINISH, a circuit's one line is chequered), drawn as depth-tested world quads from the grid until
 * the race is left. Place units == render units (430 per metre). */

#define GT_QUADS 400
static TD5_D3DVertex gt_v[GT_QUADS * 4];
static uint16_t      gt_i[GT_QUADS * 6];
static int           gt_n;

static void gt_flush(void)
{
    if (gt_n <= 0) return;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT_ZTEST);
    td5_plat_render_bind_texture(HUD_WHITE_TEX_PAGE);
    td5_plat_render_draw_tris(gt_v, gt_n * 4, gt_i, gt_n * 6);
    gt_n = 0;
}

static void gt_quad(const float a[3], const float b[3], const float c[3], const float d[3], uint32_t col)
{
    TD5_D3DVertex *v;
    uint16_t *ix, base;
    if (gt_n >= GT_QUADS) gt_flush();
    v = &gt_v[gt_n * 4];
    if (!td5_render_project_world_vertex(a[0], a[1], a[2], col, &v[0]) ||
        !td5_render_project_world_vertex(b[0], b[1], b[2], col, &v[1]) ||
        !td5_render_project_world_vertex(c[0], c[1], c[2], col, &v[2]) ||
        !td5_render_project_world_vertex(d[0], d[1], d[2], col, &v[3])) return;
    base = (uint16_t)(gt_n * 4);
    ix = &gt_i[gt_n * 6];
    ix[0] = base; ix[1] = (uint16_t)(base + 1); ix[2] = (uint16_t)(base + 2);
    ix[3] = base; ix[4] = (uint16_t)(base + 2); ix[5] = (uint16_t)(base + 3);
    gt_n++;
}

/* a square post round (cx,cz), y0..y1 */
static void gt_post(float cx, float cz, float y0, float y1, float hw, uint32_t col)
{
    const float xs[5] = { cx - hw, cx + hw, cx + hw, cx - hw, cx - hw };
    const float zs[5] = { cz - hw, cz - hw, cz + hw, cz + hw, cz - hw };
    int k;
    for (k = 0; k < 4; k++) {
        const float a[3] = { xs[k], y0, zs[k] }, b[3] = { xs[k + 1], y0, zs[k + 1] };
        const float c[3] = { xs[k + 1], y1, zs[k + 1] }, d[3] = { xs[k], y1, zs[k] };
        gt_quad(a, b, c, d, col);
    }
}

/* one gantry at place point (px,pz) crossing the road perpendicular to heading (tx,tz); kind 0 START, else chequered */
static void gt_gantry(double px, double pz, double tx, double tz, int kind, int32_t ox, int32_t oz)
{
    const double half = 8.0 * RR_M, hgt = 6.2 * RR_M, ban = 1.5 * RR_M;
    const double nx = -tz, nz = tx;
    const double cx = px - (double)ox, cz = pz - (double)oz;
    const float M = (float)RR_M;
    const int cells = 16;
    float gy, y1, y0;
    int k, r;
    gy = (float)td5_roam_ground((int32_t)(cx * 256.0), (int32_t)(cz * 256.0), 0, 0, NULL) / 256.0f;
    y1 = gy + (float)hgt; y0 = y1 - (float)ban;
    gt_post((float)(cx + nx * half), (float)(cz + nz * half), gy, y1, 0.35f * M, 0xFF3C4048);
    gt_post((float)(cx - nx * half), (float)(cz - nz * half), gy, y1, 0.35f * M, 0xFF3C4048);
    for (r = 0; r < 2; r++) {
        for (k = 0; k < cells; k++) {
            const double s0 = -half + 2.0 * half * (double)k / cells, s1 = -half + 2.0 * half * (double)(k + 1) / cells;
            const float ya = y0 + (float)(ban * 0.5 * r), yb = ya + (float)(ban * 0.5);
            const int white = ((k + r) & 1) == 0;
            const uint32_t col = kind == 0 ? (white ? 0xFFF2F2F2 : 0xFF1FA84A) : (white ? 0xFFF2F2F2 : 0xFF181818);
            const float a[3] = { (float)(cx + nx * s0), ya, (float)(cz + nz * s0) };
            const float b[3] = { (float)(cx + nx * s1), ya, (float)(cz + nz * s1) };
            const float c[3] = { (float)(cx + nx * s1), yb, (float)(cz + nz * s1) };
            const float d[3] = { (float)(cx + nx * s0), yb, (float)(cz + nz * s0) };
            gt_quad(a, b, c, d, col);
        }
    }
    {
        const float a[3] = { (float)(cx + nx * half), y1, (float)(cz + nz * half) };
        const float b[3] = { (float)(cx - nx * half), y1, (float)(cz - nz * half) };
        const float c[3] = { (float)(cx - nx * half), y1 + 0.3f * M, (float)(cz - nz * half) };
        const float d[3] = { (float)(cx + nx * half), y1 + 0.3f * M, (float)(cz + nz * half) };
        gt_quad(a, b, c, d, 0xFF3C4048);
    }
}

void td5_roam_race_draw_world(int view)
{
    int32_t ox = 0, oz = 0;
    int j;
    double tx, tz, l;
    if ((view != 0 && R.nh <= 1) || !td5_roam_active() || R.state < TD5_RRACE_COUNTDOWN || R.npt < 2) return;   /* [roam RA] every pane */
    td5_roam_world_origin(&ox, &oz);
    j = 1;
    while (j + 1 < R.npt && R.dist[j] < 8.0) j++;
    tx = R.px[j] - R.px[0]; tz = R.pz[j] - R.pz[0]; l = sqrt(tx * tx + tz * tz);
    if (l > 0.0) gt_gantry(R.px[0], R.pz[0], tx / l, tz / l, R.circuit ? 1 : 0, ox, oz);
    if (!R.circuit) {
        j = R.npt - 2;
        while (j > 0 && R.length_m - R.dist[j] < 8.0) j--;
        tx = R.px[R.npt - 1] - R.px[j]; tz = R.pz[R.npt - 1] - R.pz[j]; l = sqrt(tx * tx + tz * tz);
        if (l > 0.0) gt_gantry(R.px[R.npt - 1], R.pz[R.npt - 1], tx / l, tz / l, 1, ox, oz);
    }
    gt_flush();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}
