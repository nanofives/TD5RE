/**
 * td5_roam_race.h -- GEO FREE ROAM: the SPRINT RACE inside the free-roam world (PORT-ONLY, F6-B).
 * See docs/plans/GEO_FREE_ROAM.md (Decisions: race inside the free-roam world with live walls,
 * sprint A to B first, traffic / cops toggles per race; F6 row).
 *
 * WHAT IT IS. A race on the route planned in F5-A (td5_roam_route.h), run in the SAME roam session:
 * no level reload, no strip. The flow:
 *
 *   OFF --(R in the route map / pause, R on a saved route in the load list)--> SETUP
 *   SETUP: a panel over the route map (sprint, 1 lap, TRAFFIC / COPS toggles stored but inert until
 *          F8 / F10). START RACE --> the car is put on START facing the route, the barriers are built
 *          (td5_roam_barrier.h, F6-A), the pause menu closes and the game's 3-2-1 freezes the field.
 *   COUNTDOWN --(GO)--> RACING: progress is the DISTANCE ALONG the route polyline (no spans):
 *          checkpoints at the route's via points, wrong-way and off-route warnings, distance to the
 *          finish, checkpoint splits against the best run.
 *   RACING --(distance along reaches the FINISH)--> FINISHED: results panel (time, splits, best per
 *          route name in routes/<name>.best), the car brakes to a stop. ENTER keeps roaming from the
 *          finish point, M opens the route map, F5 races the route again.
 *   Any state --(A in the route map during a race, LEAVE FREE ROAM, restart, session end)--> OFF.
 *
 * BARRIERS ARE RACE-TIME ONLY. td5_roam_race_barriers_live() is 1 in COUNTDOWN and RACING; the roam wall
 * resolver (td5_roam_physics.c) asks td5_roam_race_wall_probe() for every racer body probe, and the
 * deeper of the road push and the barrier push wins, inside the same ONE combined push per actor per
 * tick (pinch / tip rules included). Traffic (slot >= 6) never collides with a barrier. Turn boards
 * (kind 1) never collide.
 *
 * GAME HOOKS (td5_game.c, td5_input.c, td5_roam_route_ui.c, td5_roam_physics.c; each a one-line
 * call, self-gated: a no-op outside a roam session). The game is driven by REQUESTS this module
 * raises and the game takes (td5_roam_race_take_*), so the module needs no td5_game.h mutator.
 *
 * STATE. One file-scope struct per .c (td5_roam_race.c: flow; td5_roam_race_wall.c: barrier
 * collision layer). Main thread only.
 */
#ifndef TD5_ROAM_RACE_H
#define TD5_ROAM_RACE_H

#include <stddef.h>
#include <stdint.h>

struct TD5_Actor;

#define TD5_RRACE_OFF        0
#define TD5_RRACE_SETUP      1
#define TD5_RRACE_COUNTDOWN  2
#define TD5_RRACE_RACING     3
#define TD5_RRACE_FINISHED   4

#define TD5_RRACE_MAX_CP     24      /* checkpoints = via points + the finish */
#define TD5_RRACE_MAX_LAPS   10      /* [F9a] circuit laps 1..10 (= TD5_ROUTE_MAX_LAPS) */
#define TD5_RRACE_TICK_HZ    30      /* sim ticks per second */

typedef struct {
    int laps;        /* 1 for a sprint; a circuit: 1..10 (mirrors the route's laps, td5_roam_route_set_type) */
    int traffic;     /* [F8] 0 off / 1 low / 2 high (td5_roam_traffic.c) */
    int cops;        /* stored, inert until F10 */
    int opponents;   /* [F7] AI racers 0..5 (td5_roam_ai.h); 0 = a solo sprint */
} TD5_RoamRaceSetup;

/* The last finished (or abandoned) run, for the results panel and the self-test. */
typedef struct {
    int      finished;               /* 1 = crossed the finish                          */
    char     name[32];               /* route name ("" = unsaved)                       */
    uint32_t hash;                   /* route edge hash                                 */
    double   length_m;
    long     ticks;                  /* race time in sim ticks                          */
    int      ncp;                    /* checkpoints incl. the finish                    */
    long     split_ticks[TD5_RRACE_MAX_CP];
    long     best_ticks;             /* best for this route before this run, 0 = none   */
    int      new_best;
    long     wrong_way_ticks, off_route_ticks;
    double   max_lateral_m;          /* farthest the car got from the route polyline    */
    int      setup_traffic, setup_cops;
    /* [F7] the field: player + opponents in finishing order (rank 1 first), refreshed every tick */
    int      nfield;
    int      field_slot[6];
    long     field_ticks[6];          /* finish time, 0 = not finished (yet)           */
    double   field_along_m[6];
    int      player_rank;             /* 1-based, 0 = solo                             */
    /* [F9a] a CIRCUIT: laps of the route's closed loop (td5_roam_lap.h). ncp / split_ticks stay 0 / unused. */
    int      circuit;                 /* 1 = a circuit race                                              */
    int      laps;                    /* laps to run                                                     */
    int      laps_done;               /* laps the player completed                                       */
    long     lap_ticks[TD5_RRACE_MAX_LAPS];   /* race ticks at the END of each completed lap (cumulative) */
    long     best_lap_ticks;          /* the player's fastest lap this race, 0 = none                    */
    long     best_lap_prev;           /* fastest lap saved for this route before this race, 0 = none     */
    int      new_best_lap;
    long     lap_jumps;               /* ticks the lap count rose by more than 1 (must be 0)             */
    int      field_laps[6];           /* laps each field entry completed                                 */
    double   lap_len_m;               /* the loop length                                                 */
} TD5_RoamRaceResult;

/* ---- state ------------------------------------------------------------------ */
int  td5_roam_race_state(void);
int  td5_roam_race_traffic_level(void);      /* [F8] the running race's TRAFFIC row: 0 off / 1 low / 2 high */
int  td5_roam_race_running(void);            /* COUNTDOWN or RACING */
int  td5_roam_race_barriers_live(void);      /* barrier collision on (COUNTDOWN, RACING; FINISHED while opponents still run) */
void td5_roam_race_reset(void);              /* roam session start / end: back to OFF, barriers cleared */
const TD5_RoamRaceResult *td5_roam_race_result(void);
/* Distance along the route (m), its length, lateral offset (m). 0 when not racing. */
void td5_roam_race_progress(double *along_m, double *length_m, double *lateral_m);
/* [F9a] a circuit race is on / its laps; `along_m` above is then the TOTAL distance (laps included)
 * and `length_m` the race distance (laps x lap). Lap this racer is on, 1-based (0 = not a circuit). */
int  td5_roam_race_circuit(void);
int  td5_roam_race_laps(void);
int  td5_roam_race_lap_now(void);

/* ---- entry points ------------------------------------------------------------ */
/* Open the setup panel for the current route (opens the route map when it is not up). 1 when the
 * route is a planned sprint (START, FINISH, every leg planned). */
int  td5_roam_race_open_setup(void);
int  td5_roam_race_setup_open(void);         /* the panel is up: the route map gives it the keys */
/* Start now with `s` (NULL = defaults): grid, barriers, countdown. 1 on success. */
int  td5_roam_race_start(const TD5_RoamRaceSetup *s);
void td5_roam_race_abandon(void);            /* back to OFF, the car stays where it is */

/* ---- game hooks -------------------------------------------------------------- */
void     td5_roam_race_frame(void);          /* once per rendered frame, after td5_roam_route_ui_frame */
void     td5_roam_race_tick(void);           /* once per sim tick, after physics (normal tick path) */
void     td5_roam_race_draw_hud(void);       /* HUD overlay pass */
void     td5_roam_race_draw_setup(void);     /* inside the route map overlay (td5_roam_route_ui.c) */
uint32_t td5_roam_race_filter_bits(int slot, uint32_t bits);   /* FINISHED: brake to a stop */
int      td5_roam_race_take_pause_close(void);   /* 1 once: the game closes its pause menu now */
int      td5_roam_race_take_countdown(void);     /* ticks per digit to arm the 3-2-1 with, once; 0 = none */
int      td5_roam_race_hide_route_hint(void);    /* 1 = the route planner's HUD hint line stays off */

/* ---- the racer-only barrier collision layer (td5_roam_race_wall.c) ----------- */
/* Called by td5_roam_resolve_walls for body probe `probe` (0..7) of racer `slot` at (x, z, y) (24.8
 * WORLD). When the probe is in a live MOUTH barrier (inside the panel or past it on the closed side,
 * within the segment), writes the push back to the open side (plain world units, the locator's
 * push_* convention) and returns 1. 0 for traffic, turn boards, or no race. */
int  td5_roam_race_wall_probe(int slot, int probe, int32_t x, int32_t z, int32_t y,
                              int32_t *push_x, int32_t *push_z, int32_t *push_dist);
void td5_roam_race_wall_begin(void);         /* race start: drop the side table (barriers rebuilt) */
void td5_roam_race_wall_end(void);
void td5_roam_race_wall_note_hit(int slot);  /* the resolver applied a barrier push to `slot` this tick */

typedef struct {
    long probes_tested;       /* probe x barrier tests near enough to matter                   */
    long contacts;            /* probe contacts (pushes returned)                              */
    long hits;                /* resolver ticks where a barrier push was applied (any racer)   */
    long hits_slot0;
    long side_patch, side_route, side_fallback;   /* how each barrier's open side was decided  */
    long chassis_past;        /* ticks a racer CHASSIS centre was past a barrier (closed side)  */
    double max_pen_m;         /* deepest probe penetration past the panel face, metres         */
    double max_chassis_past_m;
    int  barriers, mouths;
} TD5_RoamRaceWallStats;
void td5_roam_race_wall_stats(TD5_RoamRaceWallStats *out);

#ifndef TD5RE_RELEASE
/* ---- DEV: the `roam-sprint` self-test (td5_roam_race_drive.c) ----------------- */
#define TD5_RSPRINT_FULL      1
#define TD5_RSPRINT_SMOKE     2
#define TD5_RSPRINT_FULL_AI   3   /* [F7] 5 AI on a 3 km casco sprint, no scripted wrong turns */
#define TD5_RSPRINT_SMOKE_AI  4   /* [F7] the smoke sprint with 5 AI */
#define TD5_RCIRCUIT_FULL     5   /* [F9a] 3-lap ~1.5 km circuit incl. a plaza ring, 3 AI */
#define TD5_RCIRCUIT_SMOKE    6   /* [F9a] the same loop, 2 laps, 3 AI */
/* Arm before the roam session launches. The test plans + saves its route once the planner graph is
 * ready, loads it back by name, starts the race and drives it (pure pursuit on the polyline), with
 * scripted wrong turns into closed mouths that must be stopped by a barrier. */
void     td5_roam_sprint_test_arm(int mode);
void     td5_roam_sprint_test_frame(void);   /* once per frame from td5_roam_race_frame: plan, setup, start */
void     td5_roam_sprint_test_disarm(void);
int      td5_roam_sprint_test_mode(void);
uint32_t td5_roam_sprint_test_bits(struct TD5_Actor *a);   /* slot 0 control bits, 0 when idle */
int      td5_roam_sprint_test_done(void);
int      td5_roam_sprint_test_verdict(char *note, size_t n);   /* 0 PASS / 1 WARN / 2 FAIL */
/* Waypoints of the scripted routes (lat/lon, START first, FINISH last), for the census tools. */
int      td5_roam_sprint_route_waypoints(int mode, double (*ll)[2], int max);
#endif

#endif /* TD5_ROAM_RACE_H */
