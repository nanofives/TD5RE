/**
 * td5_roam_ai.h -- GEO FREE ROAM: AI racers on the planned route (PORT-ONLY, F7).
 * See docs/plans/GEO_FREE_ROAM.md (Decisions: up to 5 AI racers follow the planned route; "AI racers":
 * the Driver Model fed the route polyline instead of the span table, rubber band and positions on
 * distance along).
 *
 * THE FIELD. A roam session loads its opponents' cars once, at race init (td5_roam_ai_preload(): the
 * game spawns 1 + preload racers like a Quick Race), and parks them at the end of race init
 * (td5_roam_ai_session_init -> td5_game_roam_set_racer_field(0)): only the player exists while
 * roaming. A sprint start asks for the setup's opponent count; they go live on a grid BEHIND the
 * route's START (rows of two across the carriageway, single file where the street is narrow), drive
 * the race, and are parked again when the race goes back to OFF.
 *
 * THE DRIVER (one per AI slot, the closed-loop Driver Model's shape on a polyline):
 *   - PATH: the race's copy of the route polyline without its junction-connector jogs, extended
 *     behind START (the grid) and past FINISH (the run-out) along the snapped street, resampled
 *     every 1.5 m and lightly smoothed. Arc length, heading, right normal per point.
 *   - SPEED PROFILE: per point v = sqrt(a_lat * R), R the radius of the heading change over a
 *     +-9 m window (a city corner is a vertex of the street graph: the window is its fillet), capped
 *     at a top speed, then a BACKWARD braking pass (v^2 = v_next^2 + 2 a_brake ds), so a junction
 *     turn has a braking point before it.
 *   - CONTROL: pure pursuit to an aim point s + L ahead, L = L0 + k * speed (clamped), offset by the
 *     car's lane; steering = atan(2 WB sin(alpha) / L) with yaw damping and a rate limit; throttle and
 *     brake from a PI on the speed error. The same five actor command fields as the span Driver
 *     Model (steering_command, encounter_steering_cmd, brake_flag, handbrake_flag, throttle_state),
 *     so physics, collisions and the roam walls are the normal AI path.
 *   - LANES: each car keeps a lane offset (its grid column), shrunk wherever the aim point or the
 *     body beside it is off the drivable surface (the locator), never past the kerb.
 *   - RACECRAFT: a car ahead in my lane and slower: follow (speed cap) and pass on the free side.
 *   - RECOVERY: throttle held under 2 km/h for 1.3 s, or pointing more than 110 deg off the path at a
 *     crawl: reverse 1.2 s with the wheel turned away, then drive on. Barriers are walls for racers
 *     (td5_roam_race_wall.c), so a car never leaves the route through one.
 *   - RUBBER BAND on distance along against the player: behind -> up to +BOOST % target speed, ahead
 *     -> up to -TRIM %, both scaled by the CATCHUP level (td5_ai_get_catchup_level, 0 = off), with
 *     the span band's shape (boost reaches its ceiling at a fixed gap, trim softened by 176/256).
 *
 * POSITIONS. Every racer's distance along the race's own polyline (the same measure the player's
 * progress uses); finished racers by finish time first. race_position (0-based) is written to each
 * actor. The standings feed the HUD and the results.
 *
 * STATE. One file-scope struct. Main thread only. Inert outside a roam session.
 */
#ifndef TD5_ROAM_AI_H
#define TD5_ROAM_AI_H

#include <stdint.h>
#include <stddef.h>

#define TD5_ROAM_AI_MAX   5      /* opponents (racer slots 1..5; [roam RA] humans..5 in split screen) */

/* ---- session ------------------------------------------------------------------ */
int  td5_roam_ai_preload(void);              /* opponents a roam session loads (TD5RE_ROAM_AI_PRELOAD, 0..5, 5) */
int  td5_roam_ai_default_opponents(void);    /* the setup's default (TD5RE_ROAM_AI_OPPONENTS, 3) */
void td5_roam_ai_session_init(void);         /* end of race init: park every AI slot */
int  td5_roam_ai_available(void);            /* opponents loaded this session (0 = none) */

/* ---- race ------------------------------------------------------------------------ */
/* Race start: the race's copy of the route (PLACE frame, distance along in metres), the player's
 * grid point / yaw, the opponents wanted. Builds the path + speed profile, puts the field on the
 * grid. Returns the opponents placed. */
int  td5_roam_ai_race_start(const double *px, const double *pz, const double *dist, int npt, double length_m,
                            int32_t start_px, int32_t start_pz, int start_yaw, int n_ai);
void td5_roam_ai_regrid(void);               /* the sprint's 3-2-1 re-ran: back on the grid */
void td5_roam_ai_race_end(void);             /* the race went OFF: park the opponents */
int  td5_roam_ai_count(void);                /* opponents in this race */
/* [F9a] The NEXT td5_roam_ai_race_start is a circuit of `laps` laps on a closed route whose lap is
 * `lap_len_m` long (0 = a sprint, the default; consumed by the next race start). The route polyline passed
 * to race_start is then ONE lap, closed. The driver's path is the lap unrolled `laps` times (a run-up of
 * the lap's tail behind START, a run-out of the lap's start past the line), and each opponent's progress
 * is the TOTAL distance of td5_roam_lap.h, so it laps, ranks and finishes on (laps, distance). */
void td5_roam_ai_set_circuit(int laps, double lap_len_m);
int  td5_roam_ai_car_laps(int k);            /* laps opponent k has completed (circuit), 0 for a sprint */
int  td5_roam_ai_racing(void);               /* opponents still running (not finished) */

/* Once per sim tick BEFORE physics (td5_game.c, the roam branch of the AI tick): commands. */
void td5_roam_ai_tick(void);
/* Once per sim tick AFTER physics (td5_roam_race_tick): progress, finishes, positions. `racing` = the
 * race clock runs (RACING, or FINISHED for the humans while opponents still run). [roam RA] One entry per
 * local human (slot h): distance along, crossed the line, race ticks at the line (1 entry in single player). */
void td5_roam_ai_progress(long race_ticks, int racing, int nh, const double *h_along, const int *h_finished,
                          const long *h_finish_ticks);

/* ---- standings (player + opponents, rank order) -------------------------------- */
typedef struct {
    int    slot;             /* 0 = the player ([roam RA] 0..humans-1 = the local humans) */
    int    finished;
    long   finish_ticks;     /* race ticks at the line */
    double along_m;          /* distance along the route (a circuit: TOTAL distance, laps included) */
    int    car_index;
    int    laps_done;        /* [F9a] circuit: laps completed (0 for a sprint) */
} TD5_RoamStanding;
int  td5_roam_ai_standings(TD5_RoamStanding *out, int max);   /* count written (0 outside a race) */
int  td5_roam_ai_player_rank(void);                           /* 1-based, 0 = no field */
int  td5_roam_ai_human_rank(int h);                            /* [roam RA] human h's rank, 1-based, 0 = no field */

/* ---- counters (self-test, logs) --------------------------------------------------- */
typedef struct {
    int    n, finished;
    long   stuck_long;           /* episodes: under 3 km/h for more than 5 s while racing     */
    long   still_max_ticks;      /* the longest such run, any car                              */
    long   recoveries;           /* reverse-and-retry manoeuvres                               */
    long   passes;               /* pass manoeuvres started                                    */
    long   follow_ticks;         /* ticks capped behind a slower car                           */
    long   lane_shrinks;         /* aim offsets pulled in by the kerb check                    */
    double vmax_kmh;
    double max_lateral_m;        /* farthest any opponent got from the route polyline          */
    long   brake_ticks;          /* ticks with the brake on, before a bend (profile braking)   */
    int    grid_rows, grid_single;
    long   lap_jumps;            /* [F9a] ticks an opponent's lap count rose by more than 1 (must be 0) */
    int    circuit_laps;         /* [F9a] laps of this race (0 = sprint)                                 */
    long   respawns;             /* [F8b] put back on the path after 4 recoveries at one spot (pinned)   */
} TD5_RoamAiStats;
void td5_roam_ai_stats(TD5_RoamAiStats *out);
/* per opponent k (0..n-1): slot, finish ticks (0 = running), still-max ticks, recoveries */
int  td5_roam_ai_car_info(int k, int *slot, long *finish_ticks, long *still_max, long *recoveries);
/* The player's grid spot this race (PLACE frame, yaw). START plus the spots behind it are one grid; the
 * player draws a spot at random (TD5RE_ROAM_GRID_PLAYER=0..5 fixes it, 0 = pole). 0 without opponents. */
int  td5_roam_ai_player_grid(int32_t *px, int32_t *pz, int *yaw12);
/* [roam RA] Human h's grid spot (h = 0: the player's above). The humans of a split-screen race take consecutive
 * spots of the same grid from the drawn one on; built even with no opponents when there are 2+ humans. A human
 * that is a simulated test player (td5_roam_human_ai_driven) is then driven by this module's driver (a PILOT: it
 * races as a human, the rubber band and the opponents' counters leave it out). 0 = no spot this race. */
int  td5_roam_ai_human_grid(int h, int32_t *px, int32_t *pz, int *yaw12);
/* DEV self-test: pin the player's spot (-1 = back to knob / random). */
void td5_roam_ai_force_player_spot(int spot);
/* 1 when opponent slot `slot` has crossed the finish: no longer a racer, the barriers let it through */
int  td5_roam_ai_slot_finished(int slot);
/* 1 when opponent slot `slot` is braking within `m` metres of a bend tighter than 50 km/h (DEV shots) */
int  td5_roam_ai_braking_for_bend(int slot, double m);

#endif /* TD5_ROAM_AI_H */
