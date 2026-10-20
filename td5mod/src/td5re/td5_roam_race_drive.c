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
#include "td5_roam_ai.h"          /* [F7] the AI rows: standings, braking, counters */
#include "td5_roam_lap.h"         /* [F9a] the circuit rows: the lap tracker simulation */
#include "td5_roam_rebase.h"      /* [F9b] world-frame memories follow a rebase; the partido rows judge them */
#include "td5_roam_stream.h"      /* [F9b] the partido rows judge stream misses / holds */
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
/* [F7] the AI sprint: the FULL route started 580 m further south on Diagonal 74 (between the two
 * Hipolito Yrigoyen crossings, 85 m of clear carriageway behind START for the grid), ~3.1 km */
static const double k_full_ai_wp[][2] = {
    { -34.93215, -57.95397 },     /* START  Diagonal 74 nb, between the Yrigoyen crossings  */
    { -34.92048, -57.95358 },     /* via    Calle 12, Plaza Moreno east side              */
    { -34.90950, -57.95520 },     /* FINISH Diagonal 74 nb, past the Plaza Italia ring    */
};
/* [F9a] the circuits (lat/lon from the cached OSM roads, found with a directed-graph search over the casco and
 * checked in the game's own planner: _archive/roam/F9a/). FULL: ~1.5 km round Plaza Italia (the roundabout ring,
 * Avenida 7, Calle 48, Calle 9, Avenida 44, back into the ring), 3 laps. SMOKE: a 0.6 km block loop (Calle 42,
 * Calle 6, Calle 41, Avenida 7), 2 laps. FIG8: two block loops sharing the Calle 42 x Calle 10 junction, driven
 * twice through it in two directions (census only: barriers + the lap tracker on the polyline). */
static const double k_circ_full_wp[][2] = {
    { -34.911916, -57.953726 },   /* START  Avenida 7, 150 m after the Plaza Italia ring       */
    { -34.914504, -57.952042 },   /* via    Calle 48                                           */
    { -34.914242, -57.955114 },   /* via    Calle 9                                            */
    { -34.911784, -57.956370 },   /* FINISH Avenida 44, 112 m before the ring; the lap closes through it */
};
static const double k_circ_smoke_wp[][2] = {
    { -34.908033, -57.956484 },
    { -34.907202, -57.957607 },
    { -34.908129, -57.958627 },
    { -34.908899, -57.957775 },
};
static const double k_fig8_wp[][2] = {
    { -34.912231, -57.960060 },   /* START  Calle 10, 70 m after the crossing */
    { -34.912721, -57.959587 },
    { -34.913288, -57.960211 },
    { -34.913345, -57.960866 },
    { -34.912832, -57.961556 },
    { -34.912274, -57.961140 },
    { -34.911941, -57.960773 },
    { -34.911211, -57.959965 },
    { -34.910756, -57.959793 },
    { -34.910333, -57.960361 },
    { -34.910204, -57.960911 },
    { -34.910671, -57.961424 },
    { -34.911122, -57.961553 },
    { -34.911484, -57.961065 },
    { -34.911635, -57.960862 },   /* FINISH Calle 10, just before the crossing: the lap closes through it */
};
/* [F9b] the partido rows: out of the casco on tiles built on demand, across world-origin rebases. Via points
 * keep the scripted driver on the main roads (Diagonal 74, Camino Parque Centenario): the shortest path out of
 * Plaza Moreno turns from Calle 12 into the divided Avenida 44, the median-nose corner the scripted driver wedges
 * on (F2-E open item), which is not what these rows test.
 * FULL: Plaza Moreno -> Diagonal 74 -> Centenario -> City Bell centre -> back the same way -> Plaza Moreno.
 * SHORT: Plaza Moreno -> Diagonal 74 -> Centenario at Gonnet (out of the casco, north-west). */
static const double k_partido_wp[][2] = {
    { -34.92048, -57.95358 },     /* START  Calle 12, Plaza Moreno east side   */
    { -34.90500, -57.95450 },     /* via    Diagonal 74                        */
    { -34.88123, -58.00851 },     /* via    Camino Parque Centenario           */
    { -34.86452, -58.04562 },     /* via    Centenario at City Bell centre     */
    { -34.87771, -58.01661 },     /* via    Centenario, back                   */
    { -34.90500, -57.95450 },     /* via    Diagonal 74, back                  */
    { -34.92125, -57.95475 },     /* FINISH Plaza Moreno                       */
};
static const double k_partido_short_wp[][2] = {
    { -34.92048, -57.95358 },     /* START  Calle 12, Plaza Moreno east side   */
    { -34.90500, -57.95450 },     /* via    Diagonal 74                        */
    { -34.88658, -58.00139 },     /* FINISH Camino Parque Centenario, Gonnet   */
};
#define SD_IS_PARTIDO(m) ((m) == TD5_RSPRINT_PARTIDO || (m) == TD5_RSPRINT_PARTIDO_SHORT)
#define SD_IS_CIRC(m) ((m) == TD5_RCIRCUIT_FULL || (m) == TD5_RCIRCUIT_SMOKE)
#define SD_IS_AI(m) ((m) == TD5_RSPRINT_FULL_AI || (m) == TD5_RSPRINT_SMOKE_AI || SD_IS_CIRC(m))
#define SD_IS_SMOKE(m) ((m) == TD5_RSPRINT_SMOKE || (m) == TD5_RSPRINT_SMOKE_AI || (m) == TD5_RCIRCUIT_SMOKE)
#define SD_AI_N 5
#define SD_CIRC_AI_N 3

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
    char     shot_done[12][24];
    int      nshot;
    int      exit_ok;             /* ENTER on the results panel returned to free roam, barriers cleared */
    int      rear_cd;             /* [F7] rear view held on the grid (the field is behind START) */
    int      ai_done;             /* [F7] every opponent through the line before the results ENTER */
    /* [F9a] circuit rows */
    int      laps;                /* laps of the scripted circuit                                  */
    int      circ_closed;         /* the reloaded route is a closed circuit with the saved type + laps */
    char     f8_note[760];        /* figure-8 census verdict ("" = not run)                        */
    int      f8_bad;              /* figure-8 census failures                                      */
    double   lap_len;             /* the loop length (m)                                           */
    long     tick_min_lap;        /* best lap seen by the monitor (ticks)                          */
    int      mon_laps;            /* the monitor's last player lap count                           */
    long     mon_jumps;           /* monitor: ticks the player's lap count rose by more than 1     */
    int      mon_ai_laps[8];
    long     mon_ai_jumps;
    int      mapping;             /* [F9b] partido rows: waypoints set, waiting for the region graph */
    int      free_drive;          /* [F9b] partido rows: the route driven in free roam (no race, no barriers) */
    int      arrived;             /* [F9b] reached the end of the route */
    long     t_arrive;
} D;

/* one frame dump per name: <log dir>roam_sprint_<name>.png */
static void sd_shot(const char *name)
{
    char path[512];
    const char *dir = td5_plat_log_dir();
    int i;
    if (!D.shots) return;
    for (i = 0; i < D.nshot; i++) if (!strcmp(D.shot_done[i], name)) return;
    if (D.nshot < 12) snprintf(D.shot_done[D.nshot++], sizeof D.shot_done[0], "%s", name);
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

/* TD5RE_ROAM_CIRCUIT_WP="lat,lon;lat,lon;..." replaces the circuit's waypoints (START first, FINISH last) */
static int sd_env_wp(double (*ll)[2], int max)
{
    const char *e = getenv("TD5RE_ROAM_CIRCUIT_WP");
    int n = 0;
    while (e && *e && n < max) {
        char *end;
        const double la = strtod(e, &end);
        double lo;
        if (end == e || *end != ',') break;
        lo = strtod(end + 1, &end);
        ll[n][0] = la; ll[n][1] = lo; n++;
        e = end;
        if (*e == ';') e++;
    }
    return n;
}

int td5_roam_sprint_route_waypoints(int mode, double (*ll)[2], int max)
{
    if (SD_IS_CIRC(mode)) {
        const double (*cs)[2] = mode == TD5_RCIRCUIT_SMOKE ? k_circ_smoke_wp : k_circ_full_wp;
        const int cn = mode == TD5_RCIRCUIT_SMOKE ? (int)(sizeof k_circ_smoke_wp / sizeof k_circ_smoke_wp[0])
                                                  : (int)(sizeof k_circ_full_wp / sizeof k_circ_full_wp[0]);
        int i, n = sd_env_wp(ll, max);
        if (n >= 2) return n;
        for (i = 0; i < cn && i < max; i++) { ll[i][0] = cs[i][0]; ll[i][1] = cs[i][1]; }
        return cn;
    }
    if (SD_IS_PARTIDO(mode)) {
        const double (*ps)[2] = mode == TD5_RSPRINT_PARTIDO ? k_partido_wp : k_partido_short_wp;
        const int pn = mode == TD5_RSPRINT_PARTIDO ? (int)(sizeof k_partido_wp / sizeof k_partido_wp[0])
                                                   : (int)(sizeof k_partido_short_wp / sizeof k_partido_short_wp[0]);
        int i, n = sd_env_wp(ll, max);          /* TD5RE_ROAM_CIRCUIT_WP replaces them here too */
        if (n >= 2) return n;
        for (i = 0; i < pn && i < max; i++) { ll[i][0] = ps[i][0]; ll[i][1] = ps[i][1]; }
        return pn;
    }
    {
    const double (*src)[2] = SD_IS_SMOKE(mode) ? k_smoke_wp : (mode == TD5_RSPRINT_FULL_AI ? k_full_ai_wp : k_full_wp);
    const int n = SD_IS_SMOKE(mode) ? (int)(sizeof k_smoke_wp / sizeof k_smoke_wp[0])
                : mode == TD5_RSPRINT_FULL_AI ? (int)(sizeof k_full_ai_wp / sizeof k_full_ai_wp[0])
                                              : (int)(sizeof k_full_wp / sizeof k_full_wp[0]);
    int i;
    for (i = 0; i < n && i < max; i++) { ll[i][0] = src[i][0]; ll[i][1] = src[i][1]; }
    return n;
    }
}

/* [F9b] World-origin rebase: the wrong-turn panels and the last pursuit target are world units */
static void sd_world_shift(int32_t sx, int32_t sz)
{
    int i;
    for (i = 0; i < D.nwt; i++) { D.wt[i].mx += sx; D.wt[i].mz += sz; }
    D.ltx += sx; D.ltz += sz;
}

void td5_roam_sprint_test_arm(int mode)
{
    memset(&D, 0, sizeof D);
    D.mode = mode;
    D.active_wt = -1;
    D.hint = -1;
    D.ch_hint = ROAM_PRIM_NONE;
    D.want_wt = (SD_IS_AI(mode) || SD_IS_PARTIDO(mode)) ? 0 : (mode == TD5_RSPRINT_SMOKE ? 1 : 2);   /* [F7] AI rows: no wrong turns */
    td5_roam_rebase_listen(sd_world_shift);
    D.shots = td5_env_int("TD5RE_ROAM_SPRINT_SHOTS", 0, 0, 1);
    D.laps = SD_IS_CIRC(mode) ? td5_env_int("TD5RE_ROAM_CIRCUIT_LAPS", mode == TD5_RCIRCUIT_SMOKE ? 2 : 3, 1, TD5_RRACE_MAX_LAPS) : 1;
    td5_roam_ai_force_player_spot(SD_IS_CIRC(mode) ? 1 : (SD_IS_AI(mode) ? 3 : -1));   /* [F7] mid-pack, repeatable (play: random) */
    snprintf(D.route_name, sizeof D.route_name, "%s",
             mode == TD5_RCIRCUIT_FULL ? "selftest_circuit" : mode == TD5_RCIRCUIT_SMOKE ? "selftest_circuit_smoke"
             : mode == TD5_RSPRINT_SMOKE ? "selftest_sprint_smoke" : mode == TD5_RSPRINT_FULL_AI ? "selftest_sprint_ai"
             : mode == TD5_RSPRINT_SMOKE_AI ? "selftest_sprint_ai_smoke" : mode == TD5_RSPRINT_PARTIDO ? "selftest_partido"
             : mode == TD5_RSPRINT_PARTIDO_SHORT ? "selftest_partido_short" : "selftest_sprint");
}

void td5_roam_sprint_test_disarm(void)
{
    if (D.trc) fclose(D.trc);
    memset(&D, 0, sizeof D);
    td5_roam_ai_force_player_spot(-1);
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
    if (SD_IS_PARTIDO(D.mode) && D.mapping) {
        /* [F9b] the waypoints are in; the planner region is being mapped (tiles built on the stream worker,
         * the graph swapped in): wait for the route, then save / load it like every row */
        if (!td5_roam_route_result()->ok || !td5_roam_route_graph_settled()) {
            if (D.phase_frames > 30 * 60 * 6) { sd_finish("the partido route never planned (region not mapped)", 1); return 0; }
            return -1;
        }
        TD5_LOG_I(LOG_TAG, "[ROAMPARTIDO] route planned after %ld frames of mapping: %.1f m, %d edges, graph gen %ld, %ld tiles built",
                  D.phase_frames, td5_roam_route_result()->length_m, td5_roam_route_result()->nedge, td5_roam_route_graph_gen(),
                  td5_roam_stream_plan_built());
        goto planned;
    }
    td5_roam_route_clear();
    for (i = 0; i < n && ok; i++) {
        int32_t px, pz;
        if (!td5_roam_route_latlon_to_place(ll[i][0], ll[i][1], &px, &pz)) { ok = 0; break; }
        if (i == 0) ok = td5_roam_route_set_start_at(px, pz);
        else if (i == n - 1) ok = td5_roam_route_set_finish_at(px, pz);
        else ok = td5_roam_route_add_at(px, pz);
    }
    /* [F9a] a circuit closes FINISH -> START. [F8b] a sprint says SPRINT too: clear keeps the planner's type, and a
     * circuit row run before this one left CIRCUIT x 3 (the smoke sprint was raced as 3 laps of a 1.2 km loop) */
    if (ok) td5_roam_route_set_type(SD_IS_CIRC(D.mode) ? TD5_ROUTE_CIRCUIT : TD5_ROUTE_SPRINT, D.laps);
    if (ok && SD_IS_PARTIDO(D.mode) && !td5_roam_route_result()->ok) {
        D.mapping = 1;
        D.phase_frames = 0;
        TD5_LOG_I(LOG_TAG, "[ROAMPARTIDO] %d waypoints set, the region is being mapped (%d tiles queued)", n,
                  td5_roam_route_graph_pending());
        return -1;
    }
planned:
    if (!ok || !td5_roam_route_result()->ok) { sd_finish("could not plan the scripted route", 1); return 0; }
    if (SD_IS_CIRC(D.mode) && !td5_roam_route_result()->closed) { sd_finish("the circuit has no closing leg back to START", 1); return 0; }
    D.saved_hash = td5_roam_route_result()->hash;
    if (!td5_roam_route_save(D.route_name)) { sd_finish("could not save the route", 1); return 0; }
    td5_roam_route_clear();
    if (!td5_roam_route_load(D.route_name)) { sd_finish("could not load the saved route", 1); return 0; }
    D.loaded_hash = td5_roam_route_result()->hash;
    D.length_m = td5_roam_route_result()->length_m;
    if (SD_IS_CIRC(D.mode)) {
        /* [F9a] save/load keeps the type and the laps: the reloaded route is still a closed circuit of D.laps laps */
        D.circ_closed = td5_roam_route_type() == TD5_ROUTE_CIRCUIT && td5_roam_route_laps() == D.laps &&
                        td5_roam_route_result()->closed;
        D.lap_len = D.length_m;
        TD5_LOG_I(LOG_TAG, "[ROAMCIRCUIT] reloaded: type=%d laps=%d (want %d) closed=%d lap=%.1f m",
                  td5_roam_route_type(), td5_roam_route_laps(), D.laps, td5_roam_route_result()->closed, D.length_m);
    }
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
#define SD_UTURN_CAP 96                 /* points one U-turn arc may add */
static double *s_ppx, *s_ppz, *s_pd;

static void sd_build_path(void)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    int i, n = 0, dropped = 0;
    const int copies = SD_IS_CIRC(D.mode) ? D.laps + 1 : 1;     /* [F9a] a circuit: the lap unrolled, one lap more than the race */
    /* [F9b] a via point snapped beside a junction leaves a ~16 m out-and-back spike to the node; the partido drive has
     * several of them, the casco rows keep the F6 15 m rule */
    const double jog_m = SD_IS_PARTIDO(D.mode) ? 30.0 : 15.0;
    int c, nu = 0;
    size_t cap;
    free(s_ppx); free(s_ppz); free(s_pd);
    memset(&s_path, 0, sizeof s_path);
    for (i = 0; i < r->npt; i++) nu += td5_roam_route_is_uturn(r->px, r->pz, r->npt, i);
    cap = (size_t)(r->npt + 1 + nu * SD_UTURN_CAP) * (size_t)copies;
    s_ppx = (double *)malloc(cap * sizeof(double));
    s_ppz = (double *)malloc(cap * sizeof(double));
    s_pd  = (double *)malloc(cap * sizeof(double));
    if (!s_ppx || !s_ppz || !s_pd) return;
    for (c = 0; c < copies; c++)
    for (i = 0; i < r->npt; i++) {
        /* a jog is two points that each turn back by more than 90 degrees: both go (judged on the
         * ORIGINAL neighbours, so the first one dropped cannot hide the second) */
        if (i >= 1 && i + 1 < r->npt) {
            const double ax = r->px[i] - r->px[i - 1], az = r->pz[i] - r->pz[i - 1];
            const double bx = r->px[i + 1] - r->px[i], bz = r->pz[i + 1] - r->pz[i];
            const double la = sqrt(ax * ax + az * az), lb = sqrt(bx * bx + bz * bz);
            if (la > 0.0 && lb > 0.0 && (ax * bx + az * bz) / (la * lb) < 0.0 &&
                (la < 15.0 * SD_M || lb < 15.0 * SD_M)) {
                /* [roam M] a U-turn (the other carriageway, the opposite way): its drivable arc, not a chord over the nose */
                const int na = td5_roam_route_uturn_arc(r->px, r->pz, r->npt, i, s_ppx + n, s_ppz + n, SD_UTURN_CAP);
                int q;
                if (na <= 0) { dropped++; continue; }
                for (q = 0; q < na; q++, n++)
                    s_pd[n] = n ? s_pd[n - 1] + sqrt((s_ppx[n] - s_ppx[n - 1]) * (s_ppx[n] - s_ppx[n - 1]) +
                                                     (s_ppz[n] - s_ppz[n - 1]) * (s_ppz[n] - s_ppz[n - 1])) / SD_M : 0.0;
                continue;
            }
            /* [F9b] partido: an out-and-back spike to a junction node beside a via point (the points before and after
             * coincide), up to jog_m long */
            if (jog_m > 15.0 && la > 0.0 && lb > 0.0 && la < jog_m * SD_M && lb < jog_m * SD_M &&
                (r->px[i + 1] - r->px[i - 1]) * (r->px[i + 1] - r->px[i - 1]) +
                (r->pz[i + 1] - r->pz[i - 1]) * (r->pz[i + 1] - r->pz[i - 1]) < (3.0 * SD_M) * (3.0 * SD_M)) { dropped++; continue; }
        }
        if (c > 0 && i == 0) continue;                  /* the lap's first point is the previous lap's last */
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

/* [F7] the AI rows' frame dumps while racing: 3_pack = two or more opponents 8..90 m ahead of the camera
 * car (after 5 s); 5_ai_braking = an opponent 10..70 m ahead braking within 30 m of a bend tighter than
 * 50 km/h */
static void sd_ai_shots(void)
{
    TD5_RoamStanding st[6];
    double me = 0.0;
    int n, i, ahead = 0;
    if (!D.shots) return;
    n = td5_roam_ai_standings(st, 6);
    for (i = 0; i < n; i++) if (st[i].slot == 0) me = st[i].along_m;
    for (i = 0; i < n; i++) {
        const double d = st[i].along_m - me;
        if (st[i].slot == 0 || st[i].finished) continue;
        if (d > 8.0 && d < 90.0) ahead++;
        if (d > 10.0 && d < 70.0 && td5_roam_ai_braking_for_bend(st[i].slot, 30.0)) sd_shot("5_ai_braking");
    }
    if (ahead >= 2 && (long)g_td5.simulation_tick_counter - D.t_go > 150) sd_shot("3_pack");
}

/* ------------------------------------------------------------ [F9a] figure 8 + lap tracker ---- */

static int sd_seg_cross(double ax, double az, double bx, double bz, double cx, double cz, double dx, double dz)
{
    const double d1x = bx - ax, d1z = bz - az, d2x = dx - cx, d2z = dz - cz;
    const double den = d1x * d2z - d1z * d2x;
    double t, u;
    if (fabs(den) < 1e-9) return 0;
    t = ((cx - ax) * d2z - (cz - az) * d2x) / den;
    u = ((cx - ax) * d1z - (cz - az) * d1x) / den;
    return t > 0.0 && t < 1.0 && u > 0.0 && u < 1.0;
}

/* proper crossings between non-adjacent segments of the polyline (a figure 8 has at least one) */
static int sd_selfcross(const TD5_RouteResult *r)
{
    int i, j, n = 0;
    for (i = 0; i + 1 < r->npt; i++)
        for (j = i + 3; j + 1 < r->npt; j++) {
            /* the closing seam: the last segments meet the first ones, that is the lap, not a crossing */
            if (i <= 2 && j + 3 >= r->npt) continue;
            if (sd_seg_cross(r->px[i], r->pz[i], r->px[i + 1], r->pz[i + 1], r->px[j], r->pz[j], r->px[j + 1], r->pz[j + 1])) n++;
            /* a junction crossed twice: the planner threads both passes through the SAME centre point, so the two
             * polylines touch at a shared vertex instead of crossing in a segment interior */
            else if (hypot(r->px[i + 1] - r->px[j], r->pz[i + 1] - r->pz[j]) < 1.0 * SD_M && j > i + 3) n++;
        }
    return n;
}

/* The lap tracker on a polyline, no car: positions along the loop every 2 m with 1.5 m of sideways noise, from
 * 30 m behind the line. The tracked TOTAL must follow the true arc length within 6 m the whole way (a jump at a
 * self-crossing would break it), the lap count must rise by exactly 1 at each line crossing, backing 25 m behind the
 * line after lap 1 and dithering 10 m each way over it 6 times after lap 2 must not count a lap twice. */
static int sd_lap_sim(const TD5_RouteResult *r, int laps, char *note, size_t cap)
{
    TD5_RoamLoop lp;
    TD5_LapTrack t;
    double *seq = NULL;
    int ns = 0, cap_s, i, k, bad = 0, prev_cnt = 0, jumps = 0, incs = 0, final_cnt = 0;
    double worst = 0.0;
    uint32_t rng = 20261018u;
    if (!td5_roam_loop_init(&lp, r->px, r->pz, r->dist_m, r->npt, SD_M)) { snprintf(note, cap, "loop init failed"); return 1; }
    cap_s = (int)(lp.L * (double)(laps + 2) / 2.0) + 800;
    seq = (double *)malloc((size_t)cap_s * sizeof(double));
    if (!seq) { td5_roam_loop_free(&lp); snprintf(note, cap, "out of memory"); return 1; }
    {
        double sv;
        for (sv = -30.0; sv <= lp.L * (double)laps + 30.0 && ns < cap_s - 200; sv += 2.0) {
            int lap_k;
            seq[ns++] = sv;
            for (lap_k = 1; lap_k <= laps && lap_k <= 2; lap_k++) {
                const double line = lp.L * (double)lap_k;
                double q;
                if (!(sv < line && sv + 2.0 >= line)) continue;
                if (lap_k == 1) {
                    for (q = line + 6.0; q >= line - 25.0; q -= 2.0) seq[ns++] = q;
                    for (q = line - 25.0; q <= line + 6.0; q += 2.0) seq[ns++] = q;
                } else {
                    int rep;
                    for (rep = 0; rep < 6; rep++) {
                        for (q = line + 10.0; q >= line - 10.0; q -= 2.0) seq[ns++] = q;
                        for (q = line - 10.0; q <= line + 10.0; q += 2.0) seq[ns++] = q;
                    }
                }
            }
        }
    }
    td5_roam_lap_reset(&t);
    for (i = 0; i < ns; i++) {
        double w = fmod(seq[i], lp.L), x, z, ux, uz, off, dd;
        int seg = 0, j;
        double sl, f;
        if (w < 0.0) w += lp.L;
        while (seg + 1 < lp.n && lp.d[seg + 1] <= w) seg++;
        j = (seg + 1) % lp.n;
        sl = seg + 1 < lp.n ? lp.d[seg + 1] - lp.d[seg] : lp.L - lp.d[lp.n - 1];
        f = sl > 0.0 ? (w - lp.d[seg]) / sl : 0.0;
        x = lp.px[seg] + (lp.px[j] - lp.px[seg]) * f;
        z = lp.pz[seg] + (lp.pz[j] - lp.pz[seg]) * f;
        td5_roam_loop_dir(&lp, seg, &ux, &uz);
        rng = rng * 1664525u + 1013904223u;
        off = ((double)((rng >> 8) & 0xFFFF) / 65535.0 * 2.0 - 1.0) * 1.5;
        x += -uz * off * SD_M; z += ux * off * SD_M;
        td5_roam_lap_update(&lp, &t, x, z, 60.0);
        dd = fabs(t.total - seq[i]);
        if (dd > worst) worst = dd;
        if (dd > 6.0) {
            if (bad < 3) TD5_LOG_W(LOG_TAG, "[ROAMCIRCUIT] lap sim: sample %d true %.1f m tracked %.1f m", i, seq[i], t.total);
            bad++;
        }
        k = td5_roam_lap_count(&lp, &t, 0.0);
        if (k > prev_cnt + 1) jumps++;
        if (k > prev_cnt) incs++;
        prev_cnt = k;
        final_cnt = k;
    }
    snprintf(note, cap, "lap sim %d samples, L=%.1f m, worst |total-true|=%.2f m, bad=%d, jumps=%d, increments=%d, final laps=%d (want %d)",
             ns, lp.L, worst, bad, jumps, incs, final_cnt, laps);
    free(seq);
    td5_roam_loop_free(&lp);
    return bad || jumps || incs != laps || final_cnt != laps;
}

/* The figure-8 census: plan the figure 8 as a circuit, build its barriers, check that nothing is left open and no barrier
 * sits across the route (both crossing directions stay open), then run the lap tracker over its polyline. */
static void sd_fig8_census(void)
{
    const int n = (int)(sizeof k_fig8_wp / sizeof k_fig8_wp[0]);
    TD5_RoamBarrierStats bs;
    const TD5_RouteResult *r;
    char lapnote[200];
    int i, ok = 1, cross = 0, nb = 0;
    td5_roam_route_clear();
    for (i = 0; i < n && ok; i++) {
        int32_t px, pz;
        if (!td5_roam_route_latlon_to_place(k_fig8_wp[i][0], k_fig8_wp[i][1], &px, &pz)) { ok = 0; break; }
        if (i == 0) ok = td5_roam_route_set_start_at(px, pz);
        else if (i == n - 1) ok = td5_roam_route_set_finish_at(px, pz);
        else ok = td5_roam_route_add_at(px, pz);
    }
    if (ok) td5_roam_route_set_type(TD5_ROUTE_CIRCUIT, 3);
    r = td5_roam_route_result();
    D.f8_bad = 0;
    if (!ok || !r->ok || !r->closed) {
        snprintf(D.f8_note, sizeof D.f8_note, "figure 8 not planned (ok %d, route ok %d, closed %d)", ok, r->ok, r->closed);
        D.f8_bad = 1;
    } else {
        cross = sd_selfcross(r);
        nb = td5_roam_barriers_build(r);
        td5_roam_barriers_stats(&bs);
        if (cross < 1) D.f8_bad++;
        if (bs.arms_open || bs.degree_open || bs.across_route || bs.pass_gaps || bs.missing_cluster || nb < 1) D.f8_bad++;
        D.f8_bad += sd_lap_sim(r, 3, lapnote, sizeof lapnote);
        snprintf(D.f8_note, sizeof D.f8_note,
                 "figure 8 %.0f m: self-crossings %d, barriers %d (clusters %d, passes %d), arms open %d, planner-degree open %d, across route %d, pass gaps %d | %s",
                 r->length_m, cross, nb, bs.clusters_passed, bs.passes, bs.arms_open, bs.degree_open, bs.across_route, bs.pass_gaps, lapnote);
    }
    TD5_LOG_I(LOG_TAG, "[ROAMCIRCUIT] FIG8 %s: %s", D.f8_bad ? "FAIL" : "PASS", D.f8_note);
    td5_roam_barriers_clear();
    td5_roam_route_clear();
}

/* [F9a] per frame while a circuit races: the player's and every opponent's lap count may only rise one at a time */
static void sd_circ_monitor(void)
{
    const TD5_RoamRaceResult *rr = td5_roam_race_result();
    int k;
    if (rr->laps_done > D.mon_laps + 1) {
        D.mon_jumps++;
        TD5_LOG_W(LOG_TAG, "[ROAMCIRCUIT] player lap count jumped %d -> %d", D.mon_laps, rr->laps_done);
    }
    if (rr->laps_done > D.mon_laps) D.mon_laps = rr->laps_done;
    for (k = 0; k < 8; k++) {
        const int l = td5_roam_ai_car_laps(k);
        if (l > D.mon_ai_laps[k] + 1) {
            D.mon_ai_jumps++;
            TD5_LOG_W(LOG_TAG, "[ROAMCIRCUIT] opponent %d lap count jumped %d -> %d", k, D.mon_ai_laps[k], l);
        }
        if (l > D.mon_ai_laps[k]) D.mon_ai_laps[k] = l;
    }
}

/* standalone dev run (no self-test director): TD5RE_ROAM_SPRINT_TEST=full|smoke with --Roam=1
 * --RoamSpawn=<the route START> arms the same script, logs the verdict and quits */
static struct { int checked, driven, reported; } E;

void td5_roam_sprint_test_frame(void)
{
    td5_roam_wall_test_frame();   /* [HE] the roam-walls test (self-gated) */
    if (!E.checked && !D.mode && td5_roam_active()) {
        const char *m = getenv("TD5RE_ROAM_SPRINT_TEST");
        E.checked = 1;
        if (m && (!strcmp(m, "full") || !strcmp(m, "smoke") || !strcmp(m, "ai") || !strcmp(m, "ai_smoke") ||
                  !strcmp(m, "circuit") || !strcmp(m, "circuit_smoke") || !strcmp(m, "partido") || !strcmp(m, "partido_short"))) {
            td5_roam_sprint_test_arm(!strcmp(m, "smoke") ? TD5_RSPRINT_SMOKE : !strcmp(m, "ai") ? TD5_RSPRINT_FULL_AI
                                     : !strcmp(m, "ai_smoke") ? TD5_RSPRINT_SMOKE_AI : !strcmp(m, "circuit") ? TD5_RCIRCUIT_FULL
                                     : !strcmp(m, "circuit_smoke") ? TD5_RCIRCUIT_SMOKE : !strcmp(m, "partido") ? TD5_RSPRINT_PARTIDO
                                     : !strcmp(m, "partido_short") ? TD5_RSPRINT_PARTIDO_SHORT : TD5_RSPRINT_FULL);
            td5_roam_ai_force_player_spot(-1);   /* [F7] standalone: the player's grid spot is drawn as in play */
            E.driven = 1;
            TD5_LOG_W(LOG_TAG, "[ROAMSPRINT] standalone %s sprint test armed (TD5RE_ROAM_SPRINT_TEST)", m);
        }
    }
    if (E.driven && D.done && !E.reported && !(td5_env_int("TD5RE_ROAM_HF_FINLOG", 0, 0, 1) && td5_roam_race_fin_ticks() < 420)) {
        char note[128];
        E.reported = 1;
        td5_roam_sprint_test_verdict(note, sizeof note);
        TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] standalone verdict: %s", note);
        /* [roam HA] DEV: TD5RE_ROAM_SPRINT_LINGER=1 keeps the game up after the verdict, so the ENTER on the
         * results can be followed into the frontend (the geo generator) */
        if (!td5_env_int("TD5RE_ROAM_SPRINT_LINGER", 0, 0, 1)) g_td5.quit_requested = 1;
    }
    if (!D.mode || D.done || !td5_roam_active()) return;
    D.frames++;
    D.phase_frames++;
    switch (D.phase) {
    case 0:   /* planner graph */
        if (td5_roam_route_graph_ready() && D.frames > 20) {
            if (D.mode == TD5_RCIRCUIT_FULL) sd_fig8_census();    /* [F9a] before the real circuit is planned */
            {
                const int pr = sd_plan_save_load();
                if (pr == 0) return;
                if (pr < 0) break;            /* [F9b] mapping the partido region */
            }
            D.phase = 1; D.phase_frames = 0;
        } else if (D.frames > 6000) {
            sd_finish("planner graph never ready", 1);
        }
        break;
    case 1:   /* open the setup panel the way the R key does */
        if (SD_IS_PARTIDO(D.mode)) {
            /* [F9b] the partido rows drive the planned route in FREE ROAM: the subject is the place (tiles built
             * on demand, world-origin rebases, residency over a long drive), not the race barriers */
            TD5_RoamPhysStats st;
            char path[512];
            const char *dir = td5_plat_log_dir();
            if (D.shots) {
                /* the route map over the partido (streets of the whole place, its outline, the route), then drive */
                if (D.phase_frames == 1) { td5_roam_route_ui_open_map(); break; }
                if (D.phase_frames == 6) { td5_roam_route_ui_fit(); break; }
                if (D.phase_frames == 40) { sd_shot("p_map"); break; }
                if (D.phase_frames == 50) { td5_roam_route_ui_close_map(); break; }
                if (D.phase_frames < 60) break;
                if (td5_game_is_pause_menu_active() || td5_roam_route_ui_map_open()) {
                    if (D.phase_frames == 120) td5_plat_input_inject_key(0x01, 1);   /* ESC: back to driving */
                    if (D.phase_frames == 124) td5_plat_input_inject_key(0x01, 0);
                    if (D.phase_frames > 600) { sd_finish("the route map / pause menu never closed", 1); return; }
                    break;
                }
            }
            if (D.phase_frames < 10) break;
            td5_roam_physics_stats(&st);
            D.pinch0 = st.pinches; D.resc0 = st.rescues + st.rescues_y; D.tele0 = st.teleports;
            sd_build_path();
            sd_dump_route();
            snprintf(path, sizeof path, "%sroam_sprint_trace.csv", dir ? dir : "");
            D.trc = fopen(path, "w");
            if (D.trc) fprintf(D.trc, "tick,along_m,lat_m,px,pz,kmh,bits,wt,wt_state,drv_along,tx,tz,yaw\n");
            D.free_drive = 1;
            D.phase = 3; D.phase_frames = 0;
            TD5_LOG_I(LOG_TAG, "[ROAMPARTIDO] free-roam drive of the %.1f m route begins", s_path.length_m);
            break;
        }
        if (SD_IS_CIRC(D.mode) && D.shots) {
            /* [F9a] the planner map with the circuit (closing leg included) before the setup panel covers it */
            if (D.phase_frames == 1) { td5_roam_route_ui_open_map(); break; }
            if (D.phase_frames == 6) { td5_roam_route_ui_fit(); break; }
            if (D.phase_frames == 36) { sd_shot("0_planner"); break; }
            if (D.phase_frames < 45) break;
        }
        if (D.phase_frames < 10) break;
        /* [roam HA] no setup panel any more: the race starts with the settings the test fills in */
        {
            TD5_RoamRaceSetup s;
            TD5_RoamPhysStats st;
            memset(&s, 0, sizeof s);
            s.laps = D.laps; s.traffic = 1;
            s.cops = td5_env_int("TD5RE_ROAM_SPRINT_COPS", 0, 0, 1);   /* [F10] DEV: the sprint with COPS on */
            s.opponents = SD_IS_CIRC(D.mode) ? SD_CIRC_AI_N : (SD_IS_AI(D.mode) ? SD_AI_N : 0);   /* [F7] [F9a] */
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
        if (D.free_drive) {
            if (D.along_s > s_path.length_m * 0.45 && D.along_s < s_path.length_m * 0.55) sd_shot("p_midway");
            if (D.arrived) sd_finish("arrived", 0);
            break;
        }
        if (SD_IS_AI(D.mode)) {
            /* [F7] the grid seen through the rear view (the field is behind START), the pack ahead, an
             * opponent braking for a bend ahead of the camera car */
            const int cd = td5_game_resume_countdown_ticks();
            D.rear_cd = td5_roam_race_state() == TD5_RRACE_COUNTDOWN && cd > 30 && cd <= 75;
            if (td5_roam_race_state() == TD5_RRACE_COUNTDOWN && cd > 30 && cd <= 45) sd_shot("2_grid");
            if (td5_roam_race_state() == TD5_RRACE_RACING) sd_ai_shots();
        } else if (td5_roam_race_state() == TD5_RRACE_COUNTDOWN && td5_game_resume_countdown_ticks() > 0 &&
            td5_game_resume_countdown_ticks() <= 45)
            sd_shot("2_countdown");
        if (td5_roam_race_state() == TD5_RRACE_RACING && D.along_s > D.length_m * 0.30) sd_shot("3_midrace");
        if (SD_IS_CIRC(D.mode)) {
            double al = 0.0;
            td5_roam_race_progress(&al, NULL, NULL);
            sd_circ_monitor();
            if (td5_roam_race_state() == TD5_RRACE_RACING && D.lap_len > 0.0) {
                if (al > D.lap_len + 6.0 && al < D.lap_len + 16.0) sd_shot("7_start_line");
                if (al > D.lap_len * 1.45 && al < D.lap_len * 1.9) sd_shot("8_hud_lap2");
            }
        }
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
    case 4:   /* results panel up: frame dump, then ENTER ([roam HA] back to the geo generator: the game fades out and ends the session) */
        if (SD_IS_CIRC(D.mode)) sd_circ_monitor();
        if (SD_IS_AI(D.mode) && !D.ai_done) {
            /* [F7] every opponent through the line (or 6 minutes of frames) before the results are judged */
            if (!td5_roam_ai_racing() || D.phase_frames > 21600) {
                D.ai_done = 1;
                TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] opponents %s after the player's finish (%ld frames)",
                          td5_roam_ai_racing() ? "STILL RUNNING" : "all through", D.phase_frames);
                D.phase_frames = 0;
            }
            break;
        }
        if (D.phase_frames == 60) sd_shot("6_results");
        if (D.phase_frames == 75) td5_plat_input_inject_key(0x1C, 1);
        if (D.phase_frames == 79) td5_plat_input_inject_key(0x1C, 0);
        if (D.phase_frames >= 90) {
            /* the leave is a fade-out (td5_game.c "Roam race results dismissed" log line) that ends the session
             * after this verdict is read: judge it at the results, the session is still up here */
            int nb = 0;
            td5_roam_barriers(&nb);
            D.exit_ok = td5_roam_active() && td5_roam_race_state() == TD5_RRACE_FINISHED;
            TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] results ENTER pressed: race state %d, barriers %d -> %s",
                      td5_roam_race_state(), nb, D.exit_ok ? "leaving to the geo generator (see the game log)" : "NOT ON THE RESULTS");
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
/* [F9b] the straight line from the car to a world-frame target stays on the drivable surface (8 samples) */
static int sd_los_ok(const TD5_Actor *a, double tx, double tz)
{
    const TD5_RoamResident *rs = td5_roam_resident();
    const double px = a->world_pos.x / 256.0, pz = a->world_pos.z / 256.0;
    int k;
    if (!rs) return 1;
    for (k = 1; k <= 8; k++) {
        const double f = (double)k / 8.0;
        TD5_RoamHit h;
        if (!td5_roam_locate(rs, (int32_t)floor((px + (tx - px) * f) * 256.0 + 0.5), (int32_t)floor((pz + (tz - pz) * f) * 256.0 + 0.5),
                             a->world_pos.y, 1, ROAM_PRIM_NONE, &h))
            return 0;
        if (h.prim == ROAM_PRIM_NONE || !h.inside) return 0;
    }
    return 1;
}

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
    uint32_t bits;
    if (td5_roam_wall_test_bits(a, &bits)) return bits;   /* [HE] the roam-walls test */
    bits = sd_bits(a);
    if (D.rear_cd && td5_roam_race_state() == TD5_RRACE_COUNTDOWN) return (uint32_t)TD5_INPUT_REAR_VIEW;   /* [F7] */
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
    if (!D.mode || D.done || !a || D.phase != 3 || (td5_roam_race_state() != TD5_RRACE_RACING && !D.free_drive)) return 0;
    if (tick == D.tick_last) return 0;
    D.tick_last = tick;
    if (!D.t_go) D.t_go = tick;
    {
        /* [F9a] a circuit gets laps x lap at a crawl of 25 km/h plus 90 s */
        const long lim_s = SD_IS_CIRC(D.mode) ? (long)((double)D.laps * D.lap_len / 6.9) + 90L
                         : SD_IS_PARTIDO(D.mode) ? (long)(D.length_m / 6.9) + 120L : (D.mode == TD5_RSPRINT_SMOKE ? 150L : 480L);
        if (tick - D.t_go > lim_s * 30L) { sd_finish("sprint timeout", 1); return 0; }
    }
    td5_roam_world_origin(&ox, &oz);
    px = a->world_pos.x / 256.0; pz = a->world_pos.z / 256.0;
    {
        /* the driver's own distance along ITS path, in a window round the last answer */
        const TD5_RouteResult *r = sd_path();
        double best = 1e300, ba = D.along_s;
        /* [roam M] the window ahead follows the car's speed (6 m + 0.5 s): in a U-turn the return carriageway runs
         * 8 m from the approach and ~20 m further along, and a 150 m window let the answer jump across to it, the
         * driver took the turn as done and left the road at 62 km/h. Lost (> 12 m off it): the old wide window. */
        const double ahead = 6.0 + sd_kmh(a) / 3.6 * 0.5;
        int i, pass;
        for (pass = 0; pass < 2; pass++) {
        const double lo = pass ? D.along_s - 40.0 : D.along_s - 10.0, hi = pass ? D.along_s + 150.0 : D.along_s + ahead;
        if (pass && best <= (12.0 * SD_M) * (12.0 * SD_M)) break;
        for (i = 0; i + 1 < r->npt; i++) {
            const double sx = r->px[i + 1] - r->px[i], sz = r->pz[i + 1] - r->pz[i];
            const double l2 = sx * sx + sz * sz;
            double t, qx, qz, d;
            if (r->dist_m[i + 1] < lo || r->dist_m[i] > hi) continue;
            t = l2 > 0.0 ? ((px + ox - r->px[i]) * sx + (pz + oz - r->pz[i]) * sz) / l2 : 0.0;
            if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
            qx = r->px[i] + sx * t; qz = r->pz[i] + sz * t;
            d = (qx - px - ox) * (qx - px - ox) + (qz - pz - oz) * (qz - pz - oz);
            if (d < best) { best = d; ba = r->dist_m[i] + t * (r->dist_m[i + 1] - r->dist_m[i]); }
        }
        }
        D.along_s = ba;
        along = ba;
        lat = sqrt(best) / SD_M;
        (void)lat;
    }
    if (D.free_drive && !D.arrived && along >= s_path.length_m - 12.0) {
        D.arrived = 1;
        D.t_arrive = tick;
        TD5_LOG_I(LOG_TAG, "[ROAMPARTIDO] arrived: %.1f m in %ld ticks", along, tick - D.t_go);
        return (uint32_t)TD5_INPUT_BRAKE;
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
    if (D.free_drive) {
        /* [F9b] the partido drive meets tiny roundabouts and tight block loops the casco rows never see: shorten
         * the lookahead until the straight line to the target stays on the road (never across a ring's island) */
        double look = 10.0 + sd_kmh(a) * 0.12;
        while (look > 4.0 && !sd_los_ok(a, tx, tz)) {
            double k2;
            look *= 0.6;
            sd_route_target(along, look, &tx, &tz, &k2);
            if (kmh_t > 25.0) kmh_t = 25.0;
        }
    }
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
        if (D.slow_t == 45) {
            D.rev_t = 40; D.retries++;
            TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] wedge at %.0f m along: backing out", along);
            if (D.free_drive && along > 20.0) sd_shot("p_wedge");     /* [F9b] the first wedge of a partido drive past the start, for a look */
        }
        if (D.slow_t == 90) D.stuck_events++;
    } else {
        D.slow_t = 0;
    }
    (void)ox; (void)oz;
    return bits;
}

/* ------------------------------------------------------------ verdict ------------- */

/* [F7] the AI rows' verdict line: finishing order with the gap to the winner, the opponents' counters */
static int sd_ai_verdict(char *note, size_t n, const char *bad, const TD5_RoamRaceResult *res,
                         const TD5_RoamRaceWallStats *ws, long pinch, long resc, long tele)
{
    TD5_RoamAiStats as;
    char ord[160] = "", full[400] = "";
    size_t ol = 0, fl = 0;
    int i;
    const long win = res->nfield > 0 ? res->field_ticks[0] : 0;
    td5_roam_ai_stats(&as);
    for (i = 0; i < res->nfield && i < 6; i++) {
        const int slot = res->field_slot[i];
        const long ft = res->field_ticks[i];
        char who[8];
        if (slot == 0) snprintf(who, sizeof who, "YOU");
        else snprintf(who, sizeof who, "C%d", slot);
        if (ft <= 0) {
            if (ol < sizeof ord) ol += (size_t)snprintf(ord + ol, sizeof ord - ol, "%s%d.%s DNF", i ? " " : "", i + 1, who);
            if (fl < sizeof full) fl += (size_t)snprintf(full + fl, sizeof full - fl, " %d:%s=DNF(%.0fm)", i + 1, who,
                                                         res->field_along_m[i]);
        } else if (i == 0) {
            const long cs = (ft * 100 + 15) / 30;
            if (ol < sizeof ord) ol += (size_t)snprintf(ord + ol, sizeof ord - ol, "1.%s %ld:%02ld.%02ld", who, cs / 6000,
                                                        (cs / 100) % 60, cs % 100);
            if (fl < sizeof full) fl += (size_t)snprintf(full + fl, sizeof full - fl, " 1:%s=%ld", who, ft);
        } else {
            const long g = ((ft - win) * 100 + 15) / 30;
            if (ol < sizeof ord) ol += (size_t)snprintf(ord + ol, sizeof ord - ol, " %d.%s+%ld.%02ld", i + 1, who, g / 100, g % 100);
            if (fl < sizeof full) fl += (size_t)snprintf(full + fl, sizeof full - fl, " %d:%s=%ld(+%.2fs)", i + 1, who, ft,
                                                         (double)(ft - win) / 30.0);
        }
    }
    if (bad[0]) snprintf(note, n, "sprint-ai FAIL: %.100s", bad);
    else if (as.respawns) snprintf(note, n, "%.2f km, %d jn, AI %d/%d fin, stuck 0, respawn %ld | %.70s", D.length_m / 1000.0, D.nkeys,
                                   as.finished, as.n, as.respawns, ord);   /* [F8b] */
    else snprintf(note, n, "%.2f km, %d jn, AI %d/%d fin, stuck 0 | %.80s", D.length_m / 1000.0, D.nkeys, as.finished, as.n, ord);
    TD5_LOG_I(LOG_TAG, "[ROAMSPRINT] VERDICT %s (AI): route='%s' length=%.1f m junctions=%d diagonal=%d plaza=%d order:%s | "
              "opponents=%d finished=%d stuck_long=%ld still_max=%.1f s recoveries=%ld passes=%ld follow_ticks=%ld "
              "lane_shrinks=%ld brake_ticks=%ld vmax=%.0f km/h max_lateral=%.1f m grid_rows=%d single=%d | barrier hits=%ld "
              "contacts=%ld chassis_past=%ld max_pen=%.2f m | pinches=%ld rescues=%ld teleports=%ld player stuck=%ld %s",
              bad[0] ? "FAIL" : "PASS", D.route_name, D.length_m, D.nkeys, D.saw_diag, D.saw_plaza, full, as.n, as.finished,
              as.stuck_long, as.still_max_ticks / 30.0, as.recoveries, as.passes, as.follow_ticks, as.lane_shrinks,
              as.brake_ticks, as.vmax_kmh, as.max_lateral_m, as.grid_rows, as.grid_single, ws->hits, ws->contacts,
              ws->chassis_past, ws->max_pen_m, pinch, resc, tele, D.stuck_events, bad);
    for (i = 0; i < as.n; i++) {
        int slot;
        long ft, sm, rc;
        if (td5_roam_ai_car_info(i, &slot, &ft, &sm, &rc))
            TD5_LOG_I(LOG_TAG, "[ROAMSPRINT]   opponent slot %d: finish %ld ticks, longest still %.1f s, recoveries %ld",
                      slot, ft, sm / 30.0, rc);
    }
    return bad[0] ? 2 : 0;
}

/* [F9a] the circuit rows' verdict: laps, lap splits, the opponents' laps, the figure-8 census */
static int sd_circuit_verdict(char *note, size_t n, const char *bad, const TD5_RoamRaceResult *res,
                              const TD5_RoamRaceWallStats *ws, long pinch, long resc, long tele)
{
    TD5_RoamAiStats as;
    char laps_s[200] = "", ord[200] = "";
    size_t ll = 0, ol = 0;
    int i;
    td5_roam_ai_stats(&as);
    for (i = 0; i < res->laps_done && i < TD5_RRACE_MAX_LAPS; i++) {
        const long lt = res->lap_ticks[i] - (i > 0 ? res->lap_ticks[i - 1] : 0);
        if (ll < sizeof laps_s) ll += (size_t)snprintf(laps_s + ll, sizeof laps_s - ll, "%s%.2fs", i ? " " : "", (double)lt / 30.0);
    }
    for (i = 0; i < res->nfield && i < 6; i++) {
        if (ol < sizeof ord)
            ol += (size_t)snprintf(ord + ol, sizeof ord - ol, "%s%d.%s L%d %.1fs", i ? " " : "", i + 1,
                                   res->field_slot[i] == 0 ? "YOU" : "CPU", res->field_laps[i],
                                   res->field_ticks[i] > 0 ? (double)res->field_ticks[i] / 30.0 : 0.0);
    }
    if (bad[0]) snprintf(note, n, "circuit FAIL: %.100s", bad);
    else snprintf(note, n, "circuit %.2f km x %d laps, laps %s, AI %d/%d fin, stuck 0, lap jumps 0, fig8 %s%s", D.lap_len / 1000.0, D.laps,
                  laps_s, as.finished, as.n, D.mode == TD5_RCIRCUIT_FULL ? "ok" : "-", as.respawns ? ", respawn" : "");   /* [F8b] */
    TD5_LOG_I(LOG_TAG, "[ROAMCIRCUIT] VERDICT %s: route='%s' lap=%.1f m laps=%d player_laps=%d lap_ticks=[%s] best_lap=%ld total=%ld ticks "
              "closed_after_reload=%d lap_jumps player=%ld/%ld opponents=%ld/%ld | opponents=%d finished=%d stuck_long=%ld recoveries=%ld "
              "max_lateral=%.1f m | order: %s | barrier hits=%ld chassis_past=%ld pinches=%ld rescues=%ld teleports=%ld stuck=%ld "
              "junctions=%d plaza=%d | %s | %s",
              bad[0] ? "FAIL" : "PASS", D.route_name, D.lap_len, D.laps, res->laps_done, laps_s, res->best_lap_ticks, res->ticks,
              D.circ_closed, res->lap_jumps, D.mon_jumps, as.lap_jumps, D.mon_ai_jumps, as.n, as.finished, as.stuck_long, as.recoveries,
              as.max_lateral_m, ord, ws->hits, ws->chassis_past, pinch, resc, tele, D.stuck_events, D.nkeys, D.saw_plaza,
              D.f8_note[0] ? D.f8_note : "(no figure-8 census in this row)", bad);
    return bad[0] ? 2 : 0;
}

int td5_roam_sprint_test_verdict(char *note, size_t n)
{
    const TD5_RoamRaceResult *res = td5_roam_race_result();
    TD5_RoamRaceWallStats ws;
    TD5_RoamPhysStats st;
    char bad[240] = "", t[32];
    size_t bl = 0;
    int i, blocked = 0;
    const int want_jn = SD_IS_CIRC(D.mode) ? (D.mode == TD5_RCIRCUIT_SMOKE ? 3 : 6)
                      : D.mode == TD5_RSPRINT_PARTIDO ? 60 : D.mode == TD5_RSPRINT_PARTIDO_SHORT ? 15
                      : (SD_IS_SMOKE(D.mode) ? 3 : (D.mode == TD5_RSPRINT_FULL_AI ? 10 : 8));
    const int full = D.mode == TD5_RSPRINT_FULL || D.mode == TD5_RSPRINT_FULL_AI;
    long pinch, resc, tele;
    if (!D.mode) { snprintf(note, n, "roam-sprint not armed"); return 2; }
    td5_roam_race_wall_stats(&ws);
    td5_roam_physics_stats(&st);
    pinch = st.pinches - D.pinch0; resc = st.rescues + st.rescues_y - D.resc0; tele = st.teleports - D.tele0;
    for (i = 0; i < D.nwt; i++) blocked += D.wt[i].blocked;
#define SD_BAD(...) do { if (bl < sizeof bad - 1) bl += (size_t)snprintf(bad + bl, sizeof bad - bl, __VA_ARGS__); } while (0)
    if (!D.done || D.aborted || (!res->finished && !D.arrived)) SD_BAD("%s; ", D.why[0] ? D.why : "never finished");
    if (res->finished && !D.exit_ok) SD_BAD("results ENTER did not return to free roam; ");
    if (D.saved_hash != D.loaded_hash) SD_BAD("reload hash %08X != %08X; ", (unsigned)D.loaded_hash, (unsigned)D.saved_hash);
    if (D.nkeys < want_jn) SD_BAD("%d junctions < %d; ", D.nkeys, want_jn);
    if (full && !D.saw_diag) SD_BAD("no diagonal; ");
    if ((full || D.mode == TD5_RCIRCUIT_FULL) && !D.saw_plaza) SD_BAD("no plaza; ");
    if (D.nwt < D.want_wt) SD_BAD("%d/%d wrong turns planned; ", D.nwt, D.want_wt);
    if (blocked < D.nwt) SD_BAD("%d/%d wrong turns blocked; ", blocked, D.nwt);
    if (ws.chassis_past) SD_BAD("chassis past a barrier %ld ticks; ", ws.chassis_past);
    if (pinch) SD_BAD("pinches %ld; ", pinch);
    if (resc) SD_BAD("rescues %ld; ", resc);
    if (tele) SD_BAD("teleports %ld; ", tele);
    if (D.stuck_events) SD_BAD("stuck %ld; ", D.stuck_events);
    if (SD_IS_CIRC(D.mode)) {
        /* [F9a] every racer round the loop D.laps times, laps never jumping, the reload kept type + laps */
        TD5_RoamAiStats as;
        int k;
        td5_roam_ai_stats(&as);
        if (!D.circ_closed) SD_BAD("reload lost type/laps/closure; ");
        if (D.mode == TD5_RCIRCUIT_FULL && D.f8_bad) SD_BAD("figure-8 census failed; ");
        if (res->laps_done != D.laps) SD_BAD("player laps %d/%d; ", res->laps_done, D.laps);
        if (res->lap_jumps || D.mon_jumps) SD_BAD("player lap jumps %ld/%ld; ", res->lap_jumps, D.mon_jumps);
        if (res->finished && res->laps_done == D.laps && res->lap_ticks[D.laps - 1] != res->ticks) SD_BAD("last lap tick != total; ");
        if (as.n < SD_CIRC_AI_N) SD_BAD("grid %d/%d opponents; ", as.n, SD_CIRC_AI_N);
        if (as.finished < as.n) SD_BAD("opponents finished %d/%d; ", as.finished, as.n);
        if (as.stuck_long) SD_BAD("opponents stuck>5s %ld; ", as.stuck_long);
        if (as.lap_jumps || D.mon_ai_jumps) SD_BAD("opponent lap jumps %ld/%ld; ", as.lap_jumps, D.mon_ai_jumps);
        for (k = 0; k < as.n; k++)
            if (td5_roam_ai_car_laps(k) != D.laps) SD_BAD("opponent %d laps %d/%d; ", k, td5_roam_ai_car_laps(k), D.laps);
    } else if (SD_IS_AI(D.mode)) {
        /* [F7] every opponent placed and through the line, none stuck > 5 s */
        TD5_RoamAiStats as;
        td5_roam_ai_stats(&as);
        if (as.n < SD_AI_N) SD_BAD("grid %d/%d opponents; ", as.n, SD_AI_N);
        if (as.finished < as.n) SD_BAD("opponents finished %d/%d; ", as.finished, as.n);
        if (as.stuck_long) SD_BAD("opponents stuck>5s %ld; ", as.stuck_long);
    }
    if (SD_IS_PARTIDO(D.mode)) {
        /* [F9b] off the casco on tiles built on demand, across rebases, without a stall */
        TD5_RoamStreamStats ss;
        TD5_RoamRebaseStats rb;
        td5_roam_stream_stats(&ss);
        td5_roam_rebase_stats(&rb);
        if (ss.misses) SD_BAD("stream misses %ld; ", ss.misses);
        if (ss.holds) SD_BAD("stream holds %ld; ", ss.holds);
        if (rb.count < 1) SD_BAD("no rebase; ");
        if (rb.far_actors) SD_BAD("actors past 4.19 M units %ld; ", rb.far_actors);
        if (D.length_m < (D.mode == TD5_RSPRINT_PARTIDO ? 20000.0 : 3000.0)) SD_BAD("route %.0f m too short; ", D.length_m);
    }
#undef SD_BAD
    if (SD_IS_PARTIDO(D.mode)) {
        TD5_RoamStreamStats ss;
        TD5_RoamRebaseStats rb;
        long ct = D.arrived ? D.t_arrive - D.t_go : res->ticks;
        const long cs = (ct * 100 + 15) / 30;
        td5_roam_stream_stats(&ss);
        td5_roam_rebase_stats(&rb);
        snprintf(t, sizeof t, "%02ld:%02ld.%02ld", cs / 6000, (cs / 100) % 60, cs % 100);
        if (bad[0]) snprintf(note, n, "partido FAIL: %.100s", bad);
        else snprintf(note, n, "partido %.2f km %s, %d jn, rebases %ld (worst frame %.0f ms), built %ld, miss 0 hold 0",
                      D.length_m / 1000.0, t, D.nkeys, rb.count, rb.frame_worst_ms, ss.built);
        TD5_LOG_I(LOG_TAG, "[ROAMPARTIDO] VERDICT %s: route='%s' length=%.1f m time=%s (%ld ticks) junctions=%d rebases=%ld "
                  "rebase_op_worst=%.3f ms rebase_frame_worst=%.1f ms max_world=%.0f m far_actors=%ld | stream loaded=%ld built=%ld "
                  "miss=%ld hold=%ld sync=%ld frame median=%.1f worst=%.1f ms | pinches=%ld rescues=%ld teleports=%ld stuck=%ld "
                  "vmax=%.0f km/h max_lateral=%.1f m off_route_ticks=%ld barriers mouth=%d hash saved=%08X loaded=%08X %s",
                  bad[0] ? "FAIL" : "PASS", D.route_name, D.length_m, t, ct, D.nkeys, rb.count, rb.op_worst_ms,
                  rb.frame_worst_ms, rb.max_world_m, rb.far_actors, ss.loaded, ss.built, ss.misses, ss.holds, ss.sync_loads,
                  ss.frame_median_ms, ss.frame_worst_ms, pinch, resc, tele, D.stuck_events, D.kmh_max, res->max_lateral_m,
                  res->off_route_ticks, D.bst.barriers_mouth, (unsigned)D.saved_hash, (unsigned)D.loaded_hash, bad);
        return bad[0] ? 2 : 0;
    }
    if (SD_IS_CIRC(D.mode)) return sd_circuit_verdict(note, n, bad, res, &ws, pinch, resc, tele);
    if (SD_IS_AI(D.mode)) return sd_ai_verdict(note, n, bad, res, &ws, pinch, resc, tele);
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
