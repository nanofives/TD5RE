/**
 * td5_roam_race_drive.c -- GEO FREE ROAM: the `roam-sprint` self-test (PORT-ONLY, DEV-ONLY, F6-B).
 * Contract: td5_roam_race.h ("DEV: the roam-sprint self-test").
 *
 * WHAT IT DOES (one roam session, spawned at the route's START):
 *   1. waits for the planner graph, plans the scripted route through the F5-A edit API, SAVES it by
 *      name, clears the planner and LOADS it back (the "race a saved route" path; the reloaded edge
 *      hash must equal the saved one);
 *   2. opens the sprint SETUP panel (route map + pause menu, as the R key does), sets TRAFFIC on (stored,
 *      inert), presses START: the grid, the barriers, the pause close and the game's 3-2-1;
 *   3. drives the route by pure pursuit on the planned polyline, with a speed from the bend ahead;
 *   4. at N scripted junctions (2 full, 1 smoke) turns INTO a closed mouth instead: the barrier must stop
 *      it (barrier contact, the car centre never past the panel), then backs out and rejoins the route;
 *   5. judges: finished, the route's composition (distinct junction clusters, a diagonal, a plaza),
 *      every wrong turn blocked, the chassis never past a barrier, 0 pinches / rescues / teleports, never
 *      stuck (throttle held under 5 km/h for 3 s).
 *
 * The routes (lat/lon from the cached OSM roads, verified with _archive/roam/F6-B/roads_query.py):
 *   FULL : START on Diagonal 74 (northbound carriageway) between Calle 57 and Calle 16, a via point on
 *          Calle 12 at Plaza Moreno (the plaza's east side), FINISH on Diagonal 74 just past the Plaza
 *          Italia roundabout. ~2.3 km, Diagonal 74 + the Plaza Moreno ring + the Plaza Italia ring.
 *   SMOKE: Diagonal 74 northbound from Calle 58 to Calle 55, ~0.4 km, 3 crossings.
 *
 * ONE file-scope struct (D). Compiled out of RELEASE.
 */
#ifndef TD5RE_RELEASE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_fp.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_race_state.h"
#include "td5_roam_tile.h"
#include "td5_roam_locate.h"
#include "td5_roam_mode.h"
#include "td5_roam_physics.h"
#include "td5_roam_route.h"
#include "td5_roam_route_ui.h"
#include "td5_roam_barrier.h"
#include "td5_roam_race.h"
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define SD_M        430.0
#define SD_PI       3.14159265358979323846
#define SD_UPT_KMH  (430.0 / 30.0 / 3.6)     /* units per tick at 1 km/h */
#define SD_MAX_WT   4
#define SD_KEYS     256

static const double k_full_wp[][2] = {
    { -34.92700, -57.95411 },     /* START  Diagonal 74 nb, between Calle 57 and Calle 16 */
    { -34.92048, -57.95358 },     /* via    Calle 12, Plaza Moreno east side              */
    { -34.90950, -57.95520 },     /* FINISH Diagonal 74 nb, past the Plaza Italia ring    */
};
static const double k_smoke_wp[][2] = {
    { -34.92790, -57.95408 },     /* START  Diagonal 74 nb, between Calle 58 and Calle 57 */
    { -34.92420, -57.95432 },     /* FINISH Diagonal 74 nb, between Calle 55 and Calle 54 */
};

typedef struct {
    int    barrier;               /* index in td5_roam_barriers()                         */
    double mx, mz;                /* panel midpoint, world units                         */
    double cx, cz;                /* unit direction into the closed arm                  */
    double along_m;               /* where the route passes it                           */
    int    state;                 /* 0 waiting, 1 approach, 2 turning in, 3 backing out, 4 done */
    long   t0, hits0, stop_t;
    double max_past_m;            /* car centre past the panel line (toward the arm), m  */
    long   hits;
    int    at_mouth;              /* reached the arm mouth, now driving into the arm */
    int    blocked;
} SdWrongTurn;

static struct {
    int      mode;
    int      phase;               /* 0 graph, 1 planned/saved/loaded, 2 setup open, 3 racing, 4 done */
    int      done, aborted;
    char     why[96];
    long     frames, phase_frames;
    char     route_name[32];
    uint32_t saved_hash, loaded_hash;
    double   length_m;
    /* wrong turns */
    SdWrongTurn wt[SD_MAX_WT];
    int      nwt, want_wt;
    int      active_wt;           /* -1 none */
    /* drive */
    long     tick_last, t_go;
    int      hint;
    long     slow_t, rev_t, stuck_events, retries;
    double   kmh_max;
    /* composition */
    uint64_t keys[SD_KEYS];
    int      nkeys;
    int      saw_diag, saw_plaza;
    uint32_t ch_hint;
    /* baseline counters */
    long     pinch0, resc0, tele0;
    TD5_RoamBarrierStats bst;
    FILE    *trc;                 /* <log dir>roam_sprint_trace.csv, one row per driven tick */
    double   along_s;             /* distance along the driver path (m) */
    double   ltx, ltz;            /* last pursuit target, world units (trace) */
    int      shots;               /* TD5RE_ROAM_SPRINT_SHOTS=1: frame dumps at the key moments */
    char     shot_done[8][24];
    int      nshot;
    int      exit_ok;             /* ENTER on the results panel returned to free roam, barriers cleared */
} D;

/* one frame dump per name: <log dir>roam_sprint_<name>.png */
static void sd_shot(const char *name)
{
    char path[512];
    const char *dir = td5_plat_log_dir();
    int i;
    if (!D.shots) return;
    for (i = 0; i < D.nshot; i++) if (!strcmp(D.shot_done[i], name)) return;
    if (D.nshot < 8) snprintf(D.shot_done[D.nshot++], sizeof D.shot_done[0], "%s", name);
    snprintf(path, sizeof path, "%sroam_sprint_%s.png", dir ? dir : "", name);
    td5_plat_request_frame_dump(path);
    TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] frame dump %s", path);
}

/* the planned polyline + every barrier, place frame, to <log dir>roam_sprint_route.csv */
static void sd_dump_route(void)
{
    char path[512];
    const char *dir = td5_plat_log_dir();
    const TD5_RouteResult *r = td5_roam_route_result();
    int i, n = 0;
    int32_t ox, oz;
    const TD5_RoamBarrier *b = td5_roam_barriers(&n);
    FILE *f;
    snprintf(path, sizeof path, "%sroam_sprint_route.csv", dir ? dir : "");
    if (!(f = fopen(path, "w"))) return;
    td5_roam_world_origin(&ox, &oz);
    fprintf(f, "kind,i,x,z,x1,z1,dist_m,extra\n");
    for (i = 0; i < r->npt; i++) fprintf(f, "pt,%d,%.0f,%.0f,,,%.1f,\n", i, r->px[i], r->pz[i], r->dist_m[i]);
    for (i = 0; b && i < n; i++)
        fprintf(f, "bar,%d,%.0f,%.0f,%.0f,%.0f,,%d %016llx\n", i, b[i].x0 / 256.0 + ox, b[i].z0 / 256.0 + oz,
                b[i].x1 / 256.0 + ox, b[i].z1 / 256.0 + oz, b[i].kind, (unsigned long long)b[i].cluster_key);
    fclose(f);
}

int td5_roam_sprint_route_waypoints(int mode, double (*ll)[2], int max)
{
    const double (*src)[2] = mode == TD5_RSPRINT_SMOKE ? k_smoke_wp : k_full_wp;
    const int n = mode == TD5_RSPRINT_SMOKE ? (int)(sizeof k_smoke_wp / sizeof k_smoke_wp[0])
                                            : (int)(sizeof k_full_wp / sizeof k_full_wp[0]);
    int i;
    for (i = 0; i < n && i < max; i++) { ll[i][0] = src[i][0]; ll[i][1] = src[i][1]; }
    return n;
}

void td5_roam_sprint_test_arm(int mode)
{
    memset(&D, 0, sizeof D);
    D.mode = mode;
    D.active_wt = -1;
    D.hint = -1;
    D.ch_hint = ROAM_PRIM_NONE;
    D.want_wt = mode == TD5_RSPRINT_SMOKE ? 1 : 2;
    D.shots = td5_env_int("TD5RE_ROAM_SPRINT_SHOTS", 0, 0, 1);
    snprintf(D.route_name, sizeof D.route_name, "%s", mode == TD5_RSPRINT_SMOKE ? "selftest_sprint_smoke" : "selftest_sprint");
}

void td5_roam_sprint_test_disarm(void)
{
    if (D.trc) fclose(D.trc);
    memset(&D, 0, sizeof D);
}
int  td5_roam_sprint_test_mode(void) { return D.mode; }
int  td5_roam_sprint_test_done(void) { return D.mode && D.done; }

static void sd_finish(const char *why, int aborted)
{
    if (D.done) return;
    D.done = 1;
    D.aborted = aborted;
    if (D.trc) { fclose(D.trc); D.trc = NULL; }
    snprintf(D.why, sizeof D.why, "%s", why);
    TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] done: %s%s", why, aborted ? " (ABORTED)" : "");
}

/* ------------------------------------------------------------ frame steps --------- */

static int sd_plan_save_load(void)
{
    double ll[8][2];
    const int n = td5_roam_sprint_route_waypoints(D.mode, ll, 8);
    int i, ok = 1;
    td5_roam_route_clear();
    for (i = 0; i < n && ok; i++) {
        int32_t px, pz;
        if (!td5_roam_route_latlon_to_place(ll[i][0], ll[i][1], &px, &pz)) { ok = 0; break; }
        if (i == 0) ok = td5_roam_route_set_start_at(px, pz);
        else if (i == n - 1) ok = td5_roam_route_set_finish_at(px, pz);
        else ok = td5_roam_route_add_at(px, pz);
    }
    if (!ok || !td5_roam_route_result()->ok) { sd_finish("could not plan the scripted route", 1); return 0; }
    D.saved_hash = td5_roam_route_result()->hash;
    if (!td5_roam_route_save(D.route_name)) { sd_finish("could not save the route", 1); return 0; }
    td5_roam_route_clear();
    if (!td5_roam_route_load(D.route_name)) { sd_finish("could not load the saved route", 1); return 0; }
    D.loaded_hash = td5_roam_route_result()->hash;
    D.length_m = td5_roam_route_result()->length_m;
    TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] route '%s' planned + saved + loaded: %.1f m, %d edges, hash saved %08X loaded %08X",
              D.route_name, D.length_m, td5_roam_route_result()->nedge, (unsigned)D.saved_hash, (unsigned)D.loaded_hash);
    return 1;
}

/* The DRIVER's path: the planned polyline without its junction-connector spikes. The planner adds a
 * junction's node centre between two street pieces whose ends are not at the node (rr_connector), which
 * on a straight crossing is a short sideways jog (7 m at Calle 12 x Calle 50 x Diagonal 74): fine for
 * progress and drawing, but a pursuit driver at 10 m lookahead swerves into it. A point whose turn is
 * sharper than 90 degrees with a leg under 15 m is dropped. */
static TD5_RouteResult s_path;
static double *s_ppx, *s_ppz, *s_pd;

static void sd_build_path(void)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    int i, n = 0, dropped = 0;
    free(s_ppx); free(s_ppz); free(s_pd);
    memset(&s_path, 0, sizeof s_path);
    s_ppx = (double *)malloc((size_t)(r->npt + 1) * sizeof(double));
    s_ppz = (double *)malloc((size_t)(r->npt + 1) * sizeof(double));
    s_pd  = (double *)malloc((size_t)(r->npt + 1) * sizeof(double));
    if (!s_ppx || !s_ppz || !s_pd) return;
    for (i = 0; i < r->npt; i++) {
        /* a jog is two points that each turn back by more than 90 degrees: both go (judged on the
         * ORIGINAL neighbours, so the first one dropped cannot hide the second) */
        if (i >= 1 && i + 1 < r->npt) {
            const double ax = r->px[i] - r->px[i - 1], az = r->pz[i] - r->pz[i - 1];
            const double bx = r->px[i + 1] - r->px[i], bz = r->pz[i + 1] - r->pz[i];
            const double la = sqrt(ax * ax + az * az), lb = sqrt(bx * bx + bz * bz);
            if (la > 0.0 && lb > 0.0 && (ax * bx + az * bz) / (la * lb) < 0.0 &&
                (la < 15.0 * SD_M || lb < 15.0 * SD_M)) { dropped++; continue; }
        }
        s_ppx[n] = r->px[i]; s_ppz[n] = r->pz[i];
        s_pd[n] = n ? s_pd[n - 1] + sqrt((s_ppx[n] - s_ppx[n - 1]) * (s_ppx[n] - s_ppx[n - 1]) +
                                         (s_ppz[n] - s_ppz[n - 1]) * (s_ppz[n] - s_ppz[n - 1])) / SD_M : 0.0;
        n++;
    }
    s_path.px = s_ppx; s_path.pz = s_ppz; s_path.dist_m = s_pd; s_path.npt = n;
    s_path.length_m = n ? s_pd[n - 1] : 0.0; s_path.ok = n >= 2;
    TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] driver path: %d of %d points (%d connector spikes dropped), %.1f m", n, r->npt, dropped,
              s_path.length_m);
}

static const TD5_RouteResult *sd_path(void) { return s_path.npt >= 2 ? &s_path : td5_roam_route_result(); }

/* nearest point of the planned polyline to (px, pz) (place frame): distance along (m), lateral (units) */
static double sd_along(double px, double pz, double *lat_u)
{
    const TD5_RouteResult *r = sd_path();
    double best = 1e300, ba = 0.0;
    int i;
    for (i = 0; i + 1 < r->npt; i++) {
        const double sx = r->px[i + 1] - r->px[i], sz = r->pz[i + 1] - r->pz[i];
        const double l2 = sx * sx + sz * sz;
        double t = l2 > 0.0 ? ((px - r->px[i]) * sx + (pz - r->pz[i]) * sz) / l2 : 0.0, qx, qz, d;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        qx = r->px[i] + sx * t; qz = r->pz[i] + sz * t;
        d = (qx - px) * (qx - px) + (qz - pz) * (qz - pz);
        if (d < best) { best = d; ba = r->dist_m[i] + t * (r->dist_m[i + 1] - r->dist_m[i]); }
    }
    if (lat_u) *lat_u = sqrt(best);
    return ba;
}

/* the route's tangent at a distance along (unit, place frame) */
static void sd_tangent(double along, double *tx, double *tz)
{
    const TD5_RouteResult *r = sd_path();
    int i = 0;
    while (i + 2 < r->npt && r->dist_m[i + 1] < along) i++;
    {
        const double sx = r->px[i + 1] - r->px[i], sz = r->pz[i + 1] - r->pz[i], l = sqrt(sx * sx + sz * sz);
        *tx = l > 0.0 ? sx / l : 0.0; *tz = l > 0.0 ? sz / l : 1.0;
    }
}

/* Wrong-turn targets: mouth barriers across a side arm (roughly perpendicular to the route), spaced out. */
static void sd_pick_wrong_turns(void)
{
    int n = 0, i;
    const TD5_RoamBarrier *b = td5_roam_barriers(&n);
    int32_t ox, oz;
    double last = -1e9;
    const double lo = D.mode == TD5_RSPRINT_SMOKE ? 60.0 : 150.0, hi = D.length_m - (D.mode == TD5_RSPRINT_SMOKE ? 60.0 : 200.0);
    const double gap = D.mode == TD5_RSPRINT_SMOKE ? 0.0 : D.length_m * 0.35;
    td5_roam_world_origin(&ox, &oz);
    D.nwt = 0;
    /* candidates in route order: barrier index sorted by along */
    {
        double cand_al[512];
        int cand_i[512], nc = 0, a;
        for (i = 0; i < n && nc < 512; i++) {
            const double ax = b[i].x0 / 256.0, az = b[i].z0 / 256.0, bx = b[i].x1 / 256.0, bz = b[i].z1 / 256.0;
            const double mx = (ax + bx) * 0.5 + ox, mz = (az + bz) * 0.5 + oz;
            const double len = sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
            double lat, al, tx, tz, nx, nz, ex, ez, el, dot;
            if (b[i].kind != TD5_ROAM_BARRIER_MOUTH || len < 4.0 * SD_M) continue;
            al = sd_along(mx, mz, &lat);
            if (al < lo || al > hi || lat < 2.0 * SD_M || lat > 40.0 * SD_M) continue;
            /* into the arm: from the route's nearest point out through the panel, along its normal */
            sd_tangent(al, &tx, &tz);
            nx = -(bz - az) / len; nz = (bx - ax) / len;
            {
                const TD5_RouteResult *r = sd_path();
                int k = 0;
                while (k + 2 < r->npt && r->dist_m[k + 1] < al) k++;
                ex = mx - r->px[k]; ez = mz - r->pz[k];
            }
            el = sqrt(ex * ex + ez * ez);
            dot = nx * ex + nz * ez;
            if (el <= 0.0 || fabs(dot) < 0.5 * el) continue;          /* the panel faces away from the route sideways */
            if (dot < 0.0) { nx = -nx; nz = -nz; }
            if (fabs(nx * tx + nz * tz) > 0.8) continue;              /* along the route (the other carriageway): not a side arm */
            cand_al[nc] = al; cand_i[nc] = i; nc++;
        }
        /* insertion sort by along */
        for (a = 1; a < nc; a++) {
            double v = cand_al[a]; int vi = cand_i[a], j = a - 1;
            while (j >= 0 && cand_al[j] > v) { cand_al[j + 1] = cand_al[j]; cand_i[j + 1] = cand_i[j]; j--; }
            cand_al[j + 1] = v; cand_i[j + 1] = vi;
        }
        for (a = 0; a < nc && D.nwt < D.want_wt; a++) {
            SdWrongTurn *w;
            const TD5_RoamBarrier *bb = &b[cand_i[a]];
            const double ax = bb->x0 / 256.0, az = bb->z0 / 256.0, bx = bb->x1 / 256.0, bz = bb->z1 / 256.0;
            const double len = sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
            const TD5_RouteResult *r = sd_path();
            int k = 0;
            double nx = -(bz - az) / len, nz = (bx - ax) / len, ex, ez;
            if (cand_al[a] < last + gap) continue;
            while (k + 2 < r->npt && r->dist_m[k + 1] < cand_al[a]) k++;
            w = &D.wt[D.nwt++];
            memset(w, 0, sizeof *w);
            w->barrier = cand_i[a];
            w->mx = (ax + bx) * 0.5; w->mz = (az + bz) * 0.5;      /* world */
            ex = w->mx + ox - r->px[k]; ez = w->mz + oz - r->pz[k];
            if (nx * ex + nz * ez < 0.0) { nx = -nx; nz = -nz; }
            w->cx = nx; w->cz = nz;
            w->along_m = cand_al[a];
            last = cand_al[a];
            TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] wrong turn %d planned: barrier %d cluster %016llx at %.0f m along, panel %.1f m wide",
                      D.nwt, cand_i[a], (unsigned long long)bb->cluster_key, cand_al[a], len / SD_M);
        }
        TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] %d barriers, %d side-arm candidates, %d wrong turns planned (want %d)",
                  n, nc, D.nwt, D.want_wt);
    }
}

/* standalone dev run (no self-test director): TD5RE_ROAM_SPRINT_TEST=full|smoke with --Roam=1
 * --RoamSpawn=<the route START> arms the same script, logs the verdict and quits */
static struct { int checked, driven, reported; } E;

void td5_roam_sprint_test_frame(void)
{
    if (!E.checked && !D.mode && td5_roam_active()) {
        const char *m = getenv("TD5RE_ROAM_SPRINT_TEST");
        E.checked = 1;
        if (m && (!strcmp(m, "full") || !strcmp(m, "smoke"))) {
            td5_roam_sprint_test_arm(!strcmp(m, "smoke") ? TD5_RSPRINT_SMOKE : TD5_RSPRINT_FULL);
            E.driven = 1;
            TD5_LOG_W(LOG_TAG, "[ROAMSPRINT] standalone %s sprint test armed (TD5RE_ROAM_SPRINT_TEST)", m);
        }
    }
    if (E.driven && D.done && !E.reported) {
        char note[128];
        E.reported = 1;
        td5_roam_sprint_test_verdict(note, sizeof note);
        TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] standalone verdict: %s", note);
        g_td5.quit_requested = 1;
    }
    if (!D.mode || D.done || !td5_roam_active()) return;
    D.frames++;
    D.phase_frames++;
    switch (D.phase) {
    case 0:   /* planner graph */
        if (td5_roam_route_graph_ready() && D.frames > 20) {
            if (!sd_plan_save_load()) return;
            D.phase = 1; D.phase_frames = 0;
        } else if (D.frames > 6000) {
            sd_finish("planner graph never ready", 1);
        }
        break;
    case 1:   /* open the setup panel the way the R key does */
        if (D.phase_frames < 10) break;
        if (!td5_roam_race_open_setup()) { sd_finish("setup refused the loaded route", 1); return; }
        D.phase = 2; D.phase_frames = 0;
        break;
    case 2:   /* the panel is up over the map under the pause menu: TRAFFIC on (inert), START RACE */
        if (td5_roam_race_state() != TD5_RRACE_SETUP) { sd_finish("setup panel closed by itself", 1); return; }
        if (D.phase_frames == 25) sd_shot("1_setup");
        if (D.phase_frames < (D.shots ? 45 : 30)) break;
        if (!td5_game_is_pause_menu_active() || !td5_roam_route_ui_map_open()) {
            if (D.phase_frames > 600) sd_finish("pause menu / route map never opened for the setup", 1);
            break;
        }
        {
            TD5_RoamRaceSetup s;
            TD5_RoamPhysStats st;
            memset(&s, 0, sizeof s);
            s.laps = 1; s.traffic = 1; s.cops = 0;
            if (!td5_roam_race_start(&s)) { sd_finish("race start refused", 1); return; }
            td5_roam_barriers_stats(&D.bst);
            td5_roam_physics_stats(&st);
            D.pinch0 = st.pinches; D.resc0 = st.rescues + st.rescues_y; D.tele0 = st.teleports;
            sd_build_path();
            sd_pick_wrong_turns();
            sd_dump_route();
            {
                char path[512];
                const char *dir = td5_plat_log_dir();
                snprintf(path, sizeof path, "%sroam_sprint_trace.csv", dir ? dir : "");
                D.trc = fopen(path, "w");
                if (D.trc) fprintf(D.trc, "tick,along_m,lat_m,px,pz,kmh,bits,wt,wt_state,drv_along,tx,tz,yaw\n");
            }
            D.phase = 3; D.phase_frames = 0;
        }
        break;
    case 3:
        if (td5_roam_race_state() == TD5_RRACE_COUNTDOWN && td5_game_resume_countdown_ticks() > 0 &&
            td5_game_resume_countdown_ticks() <= 45)
            sd_shot("2_countdown");
        if (td5_roam_race_state() == TD5_RRACE_RACING && D.along_s > D.length_m * 0.30) sd_shot("3_midrace");
        if (td5_roam_race_state() == TD5_RRACE_RACING && td5_roam_race_result()->ncp > 1 &&
            td5_roam_race_result()->split_ticks[0] > 0 && td5_roam_race_result()->split_ticks[0] + 25 <= (long)
            (g_td5.simulation_tick_counter - D.t_go))
            sd_shot("4_split");
        if (td5_roam_race_state() == TD5_RRACE_FINISHED) {
            D.phase = 4; D.phase_frames = 0;
        } else if (td5_roam_race_state() == TD5_RRACE_OFF) {
            sd_finish("race ended without a finish", 1);
        }
        break;
    case 4:   /* results panel up: frame dump, then ENTER (keep roaming) must end the race at the finish */
        if (D.phase_frames == 60) sd_shot("6_results");
        if (D.phase_frames == 75) td5_plat_input_inject_key(0x1C, 1);
        if (D.phase_frames == 79) td5_plat_input_inject_key(0x1C, 0);
        if (D.phase_frames >= 100) {
            int nb = 0;
            td5_roam_barriers(&nb);
            D.exit_ok = td5_roam_race_state() == TD5_RRACE_OFF && nb == 0 && !td5_roam_barriers_visible();
            TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] results ENTER: race state %d, barriers %d, visible %d -> %s",
                      td5_roam_race_state(), nb, td5_roam_barriers_visible(), D.exit_ok ? "back to free roam" : "NOT EXITED");
            sd_finish("finished", 0);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ driving ------------- */

static double sd_kmh(const TD5_Actor *a)
{
    return sqrt((double)a->linear_velocity_x * a->linear_velocity_x + (double)a->linear_velocity_z * a->linear_velocity_z) /
           256.0 / SD_UPT_KMH;
}

static uint32_t sd_steer_to(const TD5_Actor *a, double tx, double tz, double kmh_target)
{
    const double px = a->world_pos.x / 256.0, pz = a->world_pos.z / 256.0;
    const double kmh = sd_kmh(a);
    double hx, hz, err;
    uint32_t bits = 0;
    if (kmh > 5.0) { const double s = kmh * SD_UPT_KMH; hx = a->linear_velocity_x / 256.0 / s; hz = a->linear_velocity_z / 256.0 / s; }
    else { const double y = (double)(a->display_angles.yaw & 0xFFF) * (2.0 * SD_PI / 4096.0); hx = sin(y); hz = cos(y); }
    err = atan2(tx - px, tz - pz) - atan2(hx, hz);
    while (err > SD_PI) err -= 2.0 * SD_PI;
    while (err < -SD_PI) err += 2.0 * SD_PI;
    if (err > 0.03) bits |= (uint32_t)TD5_INPUT_STEER_LEFT;
    else if (err < -0.03) bits |= (uint32_t)TD5_INPUT_STEER_RIGHT;
    if (kmh < kmh_target) bits |= (uint32_t)TD5_INPUT_THROTTLE;
    else if (kmh > kmh_target * 1.10 + 2.0) bits |= (uint32_t)TD5_INPUT_BRAKE;
    return bits;
}

/* pursuit target on the route `look` m ahead of `along`, world units; plus a speed from the bend ahead */
static void sd_route_target(double along, double look, double *tx, double *tz, double *kmh)
{
    const TD5_RouteResult *r = sd_path();
    int32_t ox, oz;
    int i = 0, j;
    double want = along + look, turn = 0.0, hx0 = 0.0, hz0 = 0.0;
    td5_roam_world_origin(&ox, &oz);
    if (want > r->length_m + 30.0) want = r->length_m + 30.0;
    while (i + 2 < r->npt && r->dist_m[i + 1] < want) i++;
    {
        const double seg = r->dist_m[i + 1] - r->dist_m[i];
        double t = seg > 0.0 ? (want - r->dist_m[i]) / seg : 0.0;
        if (t > 1.0 + 30.0 / (seg > 1.0 ? seg : 1.0)) t = 1.0;
        *tx = r->px[i] + (r->px[i + 1] - r->px[i]) * t - ox;
        *tz = r->pz[i] + (r->pz[i + 1] - r->pz[i]) * t - oz;
    }
    /* sharpest heading change within 35 m ahead */
    j = 0;
    while (j + 2 < r->npt && r->dist_m[j + 1] < along) j++;
    {
        const int j0 = j;
        double sx = r->px[j0 + 1] - r->px[j0], sz = r->pz[j0 + 1] - r->pz[j0], l = sqrt(sx * sx + sz * sz);
        if (l > 0.0) { hx0 = sx / l; hz0 = sz / l; }
        for (j = j0 + 1; j + 1 < r->npt && r->dist_m[j] < along + 35.0; j++) {
            double c;
            sx = r->px[j + 1] - r->px[j]; sz = r->pz[j + 1] - r->pz[j]; l = sqrt(sx * sx + sz * sz);
            if (l < 0.5 * SD_M) continue;
            c = (hx0 * sx + hz0 * sz) / l;
            if (c > 1.0) c = 1.0; else if (c < -1.0) c = -1.0;
            if (acos(c) > turn) turn = acos(c);
        }
    }
    *kmh = turn > 1.2 ? 22.0 : (turn > 0.7 ? 30.0 : (turn > 0.35 ? 42.0 : 62.0));
    if (along > r->length_m - 25.0) *kmh = 30.0;
}

static void sd_composition(const TD5_Actor *a)
{
    const TD5_RoamResident *rs = td5_roam_resident();
    TD5_RoamHit h;
    int i;
    if (!rs || !td5_roam_locate(rs, a->world_pos.x, a->world_pos.z, a->world_pos.y, 1, D.ch_hint, &h) ||
        h.prim == ROAM_PRIM_NONE || !h.inside)
        return;
    D.ch_hint = h.prim;
    if (h.is_patch) {
        for (i = 0; i < D.nkeys; i++) if (D.keys[i] == h.edge_key) break;
        if (i == D.nkeys && D.nkeys < SD_KEYS) D.keys[D.nkeys++] = h.edge_key;
        return;
    }
    {
        /* the street's OSM name from the tile's EDGE record */
        const uint32_t slot = ROAM_PRIM_SLOT(h.prim);
        const RoamTile *t = slot < (uint32_t)rs->nslot ? rs->tile[slot] : NULL;
        uint32_t k;
        if (!t) return;
        for (k = 0; k < t->nedge; k++) {
            if (t->edge[k].key != h.edge_key) continue;
            {
                const char *p = t->names, *e = t->names ? t->names + t->names_bytes : NULL;
                unsigned id = t->edge[k].name_id, q = 0;
                if (!p || id == 0xFFFFu) break;
                while (p < e && q < id) { p += strlen(p) + 1; q++; }
                if (p < e) {
                    if (!strncmp(p, "Diagonal", 8)) D.saw_diag = 1;
                    if (strstr(p, "Plaza")) D.saw_plaza = 1;
                }
            }
            break;
        }
    }
}

static uint32_t sd_bits(TD5_Actor *a);

uint32_t td5_roam_sprint_test_bits(TD5_Actor *a)
{
    const long last = D.tick_last;
    const uint32_t bits = sd_bits(a);
    if (D.trc && a && D.tick_last != last) {
        double along, lat;
        int32_t ox, oz;
        td5_roam_world_origin(&ox, &oz);
        td5_roam_race_progress(&along, NULL, &lat);
        fprintf(D.trc, "%ld,%.1f,%.1f,%.0f,%.0f,%.1f,%u,%d,%d,%.1f,%.0f,%.0f,%d\n", D.tick_last, along, lat,
                a->world_pos.x / 256.0 + ox, a->world_pos.z / 256.0 + oz, sd_kmh(a), (unsigned)bits, D.active_wt,
                D.active_wt >= 0 ? D.wt[D.active_wt].state : -1, D.along_s, D.ltx + ox, D.ltz + oz,
                (int)(a->display_angles.yaw & 0xFFF));
    }
    return bits;
}

static uint32_t sd_bits(TD5_Actor *a)
{
    const long tick = (long)g_td5.simulation_tick_counter;
    int32_t ox, oz;
    double px, pz, along, lat, tx, tz, kmh_t;
    uint32_t bits;
    TD5_RoamRaceWallStats ws;
    if (!D.mode || D.done || !a || D.phase != 3 || td5_roam_race_state() != TD5_RRACE_RACING) return 0;
    if (tick == D.tick_last) return 0;
    D.tick_last = tick;
    if (!D.t_go) D.t_go = tick;
    if (tick - D.t_go > (D.mode == TD5_RSPRINT_SMOKE ? 150L : 480L) * 30L) { sd_finish("sprint timeout", 1); return 0; }
    td5_roam_world_origin(&ox, &oz);
    px = a->world_pos.x / 256.0; pz = a->world_pos.z / 256.0;
    {
        /* the driver's own distance along ITS path, in a window round the last answer */
        const TD5_RouteResult *r = sd_path();
        double best = 1e300, ba = D.along_s;
        int i;
        for (i = 0; i + 1 < r->npt; i++) {
            const double sx = r->px[i + 1] - r->px[i], sz = r->pz[i + 1] - r->pz[i];
            const double l2 = sx * sx + sz * sz;
            double t, qx, qz, d;
            if (r->dist_m[i + 1] < D.along_s - 40.0 || r->dist_m[i] > D.along_s + 150.0) continue;
            t = l2 > 0.0 ? ((px + ox - r->px[i]) * sx + (pz + oz - r->pz[i]) * sz) / l2 : 0.0;
            if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
            qx = r->px[i] + sx * t; qz = r->pz[i] + sz * t;
            d = (qx - px - ox) * (qx - px - ox) + (qz - pz - oz) * (qz - pz - oz);
            if (d < best) { best = d; ba = r->dist_m[i] + t * (r->dist_m[i + 1] - r->dist_m[i]); }
        }
        D.along_s = ba;
        along = ba;
        lat = sqrt(best) / SD_M;
        (void)lat;
    }
    sd_composition(a);
    td5_roam_race_wall_stats(&ws);
    {
        const double k = sd_kmh(a);
        if (k > D.kmh_max) D.kmh_max = k;
    }

    /* scripted wrong turn */
    if (D.active_wt < 0) {
        int i;
        for (i = 0; i < D.nwt; i++)
            if (D.wt[i].state == 0 && along >= D.wt[i].along_m - 40.0) { D.active_wt = i; D.wt[i].state = 1; D.wt[i].t0 = tick; break; }
    }
    if (D.active_wt >= 0) {
        SdWrongTurn *w = &D.wt[D.active_wt];
        const double past = ((px - w->mx) * w->cx + (pz - w->mz) * w->cz) / SD_M;   /* + = into the arm */
        const double k = sd_kmh(a);
        if (w->state == 1) {
            /* approach on the route at 25 km/h until abreast of the panel */
            double tgx, tgz, kk;
            sd_route_target(along, 8.0, &tgx, &tgz, &kk);
            bits = sd_steer_to(a, tgx, tgz, 25.0);
            if (along >= w->along_m - 3.0) {
                w->state = 2; w->t0 = tick; w->hits0 = ws.hits_slot0; w->max_past_m = past;
                TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] wrong turn %d: turning into the closed arm (car %.1f m from the panel)",
                          D.active_wt + 1, -past);
            }
            return bits;
        }
        if (w->state == 2) {
            /* first to the arm's mouth 7 m before the panel (through the junction patch, not across the
             * block corner), then at the arm 15 m beyond the panel */
            if (!w->at_mouth && past > -2.5) w->at_mouth = 1;
            if (!w->at_mouth) {
                const double qx = w->mx - w->cx * 7.0 * SD_M, qz = w->mz - w->cz * 7.0 * SD_M;
                if ((px - qx) * (px - qx) + (pz - qz) * (pz - qz) < (3.5 * SD_M) * (3.5 * SD_M)) w->at_mouth = 1;
                bits = sd_steer_to(a, qx, qz, 20.0);
            } else {
                bits = sd_steer_to(a, w->mx + w->cx * 15.0 * SD_M, w->mz + w->cz * 15.0 * SD_M, 25.0);
            }
            D.ltx = w->at_mouth ? w->mx + w->cx * 15.0 * SD_M : w->mx - w->cx * 7.0 * SD_M;
            D.ltz = w->at_mouth ? w->mz + w->cz * 15.0 * SD_M : w->mz - w->cz * 7.0 * SD_M;
            if (past > w->max_past_m) w->max_past_m = past;
            w->hits = ws.hits_slot0 - w->hits0;
            if (w->hits > 0 && k < 3.0) w->stop_t++;
            if (w->stop_t == 6) sd_shot(D.active_wt == 0 ? "5_blocked_1" : "5_blocked_2");
            if ((w->hits > 0 && w->stop_t > 20) || tick - w->t0 > 8 * 30 || past > 4.0) {
                w->blocked = w->hits > 0 && w->max_past_m < 0.5;
                TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] wrong turn %d %s: barrier hits %ld, car centre max %.2f m %s the panel, %ld ticks",
                          D.active_wt + 1, w->blocked ? "BLOCKED" : "NOT BLOCKED", w->hits,
                          fabs(w->max_past_m), w->max_past_m > 0.0 ? "past" : "short of", tick - w->t0);
                w->state = 3; w->t0 = tick;
            }
            return bits;
        }
        if (w->state == 3) {
            /* back out: brake = reverse at a standstill, wheel straight, 2 s */
            if (tick - w->t0 < 60) return (uint32_t)TD5_INPUT_BRAKE;
            w->state = 4;
            D.active_wt = -1;
        }
    }

    /* pursuit on the route */
    sd_route_target(along, 10.0 + sd_kmh(a) * 0.12, &tx, &tz, &kmh_t);
    D.ltx = tx; D.ltz = tz;
    bits = sd_steer_to(a, tx, tz, kmh_t);
    /* a wedge: throttle held and under 4 km/h for 1.5 s -> back out 1.3 s with the wheel turned away */
    if (D.rev_t > 0) {
        D.rev_t--;
        return (uint32_t)TD5_INPUT_BRAKE | ((bits & TD5_INPUT_STEER_LEFT) ? (uint32_t)TD5_INPUT_STEER_RIGHT
                                           : (bits & TD5_INPUT_STEER_RIGHT) ? (uint32_t)TD5_INPUT_STEER_LEFT : 0u);
    }
    if ((bits & TD5_INPUT_THROTTLE) && sd_kmh(a) < 4.0) {
        D.slow_t++;
        if (D.slow_t == 45) { D.rev_t = 40; D.retries++; TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] wedge at %.0f m along: backing out", along); }
        if (D.slow_t == 90) D.stuck_events++;
    } else {
        D.slow_t = 0;
    }
    (void)ox; (void)oz;
    return bits;
}

/* ------------------------------------------------------------ verdict ------------- */

int td5_roam_sprint_test_verdict(char *note, size_t n)
{
    const TD5_RoamRaceResult *res = td5_roam_race_result();
    TD5_RoamRaceWallStats ws;
    TD5_RoamPhysStats st;
    char bad[120] = "", t[32];
    size_t bl = 0;
    int i, blocked = 0;
    const int want_jn = D.mode == TD5_RSPRINT_SMOKE ? 3 : 8;
    long pinch, resc, tele;
    if (!D.mode) { snprintf(note, n, "roam-sprint not armed"); return 2; }
    td5_roam_race_wall_stats(&ws);
    td5_roam_physics_stats(&st);
    pinch = st.pinches - D.pinch0; resc = st.rescues + st.rescues_y - D.resc0; tele = st.teleports - D.tele0;
    for (i = 0; i < D.nwt; i++) blocked += D.wt[i].blocked;
#define SD_BAD(...) do { if (bl < sizeof bad - 1) bl += (size_t)snprintf(bad + bl, sizeof bad - bl, __VA_ARGS__); } while (0)
    if (!D.done || D.aborted || !res->finished) SD_BAD("%s; ", D.why[0] ? D.why : "never finished");
    if (res->finished && !D.exit_ok) SD_BAD("results ENTER did not return to free roam; ");
    if (D.saved_hash != D.loaded_hash) SD_BAD("reload hash %08X != %08X; ", (unsigned)D.loaded_hash, (unsigned)D.saved_hash);
    if (D.nkeys < want_jn) SD_BAD("%d junctions < %d; ", D.nkeys, want_jn);
    if (D.mode == TD5_RSPRINT_FULL && !D.saw_diag) SD_BAD("no diagonal; ");
    if (D.mode == TD5_RSPRINT_FULL && !D.saw_plaza) SD_BAD("no plaza; ");
    if (D.nwt < D.want_wt) SD_BAD("%d/%d wrong turns planned; ", D.nwt, D.want_wt);
    if (blocked < D.nwt) SD_BAD("%d/%d wrong turns blocked; ", blocked, D.nwt);
    if (ws.chassis_past) SD_BAD("chassis past a barrier %ld ticks; ", ws.chassis_past);
    if (pinch) SD_BAD("pinches %ld; ", pinch);
    if (resc) SD_BAD("rescues %ld; ", resc);
    if (tele) SD_BAD("teleports %ld; ", tele);
    if (D.stuck_events) SD_BAD("stuck %ld; ", D.stuck_events);
#undef SD_BAD
    {
        long ct = res->ticks;
        const long cs = (ct * 100 + 15) / 30;
        snprintf(t, sizeof t, "%02ld:%02ld.%02ld", cs / 6000, (cs / 100) % 60, cs % 100);
    }
    if (bad[0]) snprintf(note, n, "sprint FAIL: %.100s", bad);
    else snprintf(note, n, "sprint %.2f km %s, %d jn, wrong turns %d/%d blocked, bar %d, pinch 0 stuck 0",
                  D.length_m / 1000.0, t, D.nkeys, blocked, D.nwt, D.bst.barriers_mouth);
    TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] VERDICT %s: route='%s' length=%.1f m time=%s (%ld ticks) junctions=%d diagonal=%d plaza=%d "
              "wrong_turns=%d/%d blocked (want %d) barrier_hits=%ld contacts=%ld chassis_past=%ld max_pen=%.2f m "
              "barriers mouth=%d turn=%d clusters=%d open_arms=%d across_route=%d pinches=%ld rescues=%ld teleports=%ld "
              "stuck=%ld retries=%ld vmax=%.0f km/h max_lateral=%.1f m wrong_way_ticks=%ld off_route_ticks=%ld "
              "hash saved=%08X loaded=%08X traffic=%d %s",
              bad[0] ? "FAIL" : "PASS", D.route_name, D.length_m, t, res->ticks, D.nkeys, D.saw_diag, D.saw_plaza,
              blocked, D.nwt, D.want_wt, ws.hits_slot0, ws.contacts, ws.chassis_past, ws.max_pen_m,
              D.bst.barriers_mouth, D.bst.barriers_turn, D.bst.clusters_passed, D.bst.arms_open, D.bst.across_route,
              pinch, resc, tele, D.stuck_events, D.retries, D.kmh_max, res->max_lateral_m, res->wrong_way_ticks,
              res->off_route_ticks, (unsigned)D.saved_hash, (unsigned)D.loaded_hash, res->setup_traffic, bad);
    return bad[0] ? 2 : 0;
}

#endif /* !TD5RE_RELEASE */
