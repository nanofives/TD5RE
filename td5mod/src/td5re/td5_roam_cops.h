/**
 * td5_roam_cops.h -- GEO FREE ROAM: police on the street graph (PORT-ONLY, F10).
 * See docs/plans/GEO_FREE_ROAM.md (Decisions: street life = ambient traffic, then cops; traffic / cops toggle per
 * race, cops OFF unless the setup turns them on; F10 row: police on the graph in roam, per-race toggle).
 *
 * THE POOL. A roam session reserves TD5RE_ROAM_COPS_POOL cop slots (6, max 8) right after the ambient traffic pool
 * (td5_roam_traffic.h: 6..29, so cops are 30..35) and loads the host level's police model into them. They are
 * traffic-type actors: the simplified traffic dynamics and traffic pose, the full V2V contact with racers, the cop
 * rules of the traffic police (g_cop_is_cop: the police mesh, tough in a crash, no lift). A slot is PARKED (no
 * physics, no collision, not drawn) until the module puts a car on the road (td5_roam_traffic_parked answers for
 * these slots through td5_roam_cops_parked).
 *
 * PATROL. A few cops per anchor (the player): TD5RE_ROAM_COPS in free roam (2), TD5RE_ROAM_COPS_RACE in a race with
 * COPS on (2), none in a race with COPS off. They spawn 120..220 m out on the street graph like traffic, drive an A*
 * path (td5_roam_route_plan_path) to a random street point 250..600 m away at patrol speed in the right lane, stop
 * at red lamps (td5_geo_signal_phase_at, the traffic's signal groups), keep a gap behind cars, and fade out past
 * 330 m. In a race they patrol the ROUTE CORRIDOR only: spawned on the route ahead of the player, every path on the
 * route's own edges (the planner's edge mask), so a cop never meets a closed mouth from outside.
 *
 * NOTICE. A patrolling cop with line of sight (the straight line to the player stays on the drivable surface, so a
 * building block hides the player) starts a PURSUIT when the player is SPEEDING within 60 m (faster than
 * max(TD5RE_ROAM_COPS_LIMIT_KMH 60, 1.5 x the street class speed)), RUNS A RED within 80 m (the player crosses the
 * stop line of a signalised arm while its head is red), or RAMS a cop (contact faster than 2.5 m/s closing).
 *
 * PURSUIT. Siren (the single tracked-vehicle voice, on the nearest pursuing cop) and the red / blue light bar
 * (render_tracked_actor_marker). Each pursuer re-plans an A* path to the player every 0.67 s and drives it with the
 * Driver Model's shape on a polyline (resampled path, corner speed sqrt(a_lat R), a backward braking pass, pure
 * pursuit steering, the traffic throttle law), up to TD5RE_ROAM_COPS_VMAX (125 km/h); within 150 m of the player a
 * pursuer may take a one-way against the flow, and a player behind it is reached by turning round. Inside 30 m with
 * line of sight it drives at a box point beside the player instead. HEAT 1..5: 1 at the start, +1 per further
 * offence and per 25 s of chase. Pursuers wanted = 1 + heat (pool permitting): reinforcements spawn 120..220 m out, behind or beside the
 * player (they join the chase, they do not meet the car head-on), and patrols within 200 m join from heat 2.
 *
 * ESCAPE when no pursuer has seen the player for 6 + 2 x heat seconds, or every pursuer is farther than 320 m. The
 * cops switch off and go back to patrolling.
 *
 * BUST. The player stopped (under 8 km/h) with a pursuer within 9 m fills a bust meter in 2 s (it drains at twice
 * that when the player gets going). Full = BUSTED: the car is held braked for 3 s under a BUSTED banner, then
 * - free roam: a fine (100 x heat + 50 per offence) is shown, the car is put back on the nearest street centre
 *   facing along it, the pursuers leave, 10 s of grace;
 * - a race: the car is released where it stands (the 3 s are the penalty), the pursuers drop back to patrol, 15 s
 *   of grace. No game over in either.
 *
 * WALLS. Cops are racer-like. The ROAD EDGE is a wall for them: traffic slots skip the roam wall resolver, so this
 * module pushes a cop's body corners back from the drivable-surface edge (the locator's push vector, the racers'
 * 0.35 m kerb overhang, one push per tick: the deepest corner's, or the vector sum when corners push opposite ways). A race's mouth BARRIERS block them too
 * (td5_roam_race_wall_probe_cop on the 4 body corners); traffic passes through them. Both pushes are applied before
 * physics. A cop whose centre is still off the surface for 1 s is put back on a street (pursuit) or removed
 * (patrol), counted either way.
 *
 * STATE. One file-scope struct, main thread only, inert outside a roam session.
 */
#ifndef TD5_ROAM_COPS_H
#define TD5_ROAM_COPS_H

#include <stdint.h>
#include <stddef.h>

struct TD5_Actor;

#define TD5_ROAM_COPS_POOL_MAX  8

/* ---- race init (td5_game.c) ------------------------------------------------- */
int  td5_roam_cops_pool(void);               /* cop slots this session reserves (0 = cops off) */
int  td5_roam_cops_base(void);               /* first cop slot (after the traffic pool) */
void td5_roam_cops_load_assets(void);        /* step 5b in roam, AFTER td5_roam_traffic_load_assets */
void td5_roam_cops_session_init(void);       /* end of race init: every cop slot parked */
void td5_roam_cops_session_end(void);
int  td5_roam_cops_field_total(void);        /* actor count the field keeps with the cops (0 = none) */

/* ---- per sim tick, BEFORE physics (after td5_roam_traffic_tick) ------------- */
void td5_roam_cops_tick(void);

/* ---- answers for the shared consumers ------------------------------------------ */
int  td5_roam_cops_parked(int slot);         /* -1 not a cop slot, else 1 parked / 0 live */
int  td5_roam_cops_alpha(int slot);          /* -1 not a cop slot, else 0..255 */
int  td5_roam_cops_glow(int slot);           /* light-bar intensity for the slot, 0 = off */
int  td5_roam_cops_siren_slot(int32_t wx, int32_t wz);   /* nearest cop with the siren on (WORLD 24.8), -1 */
int  td5_roam_cops_siren_slots(int32_t wx, int32_t wz, int max, int *out);   /* nearest pursuers, nearest first (G3) */
uint32_t td5_roam_cops_filter_bits(int slot, uint32_t bits);   /* BUSTED: the car is held braked */
void td5_roam_cops_draw_hud(void);           /* HUD overlay pass: pursuit panel, bust meter, toasts */
int  td5_roam_cops_heat(void);               /* 0 = no pursuit */

/* ---- counters (self-test, logs) -------------------------------------------------- */
typedef struct {
    int    pool, live, patrol, pursuing;      /* now                                                  */
    long   spawns, despawns, reinforcements;
    long   pursuits, notice_speed, notice_red, notice_ram, joins;
    long   escapes, escapes_los, escapes_dist;
    long   busts;
    long   fines;                             /* total fined, $                                       */
    long   red_runs;                          /* red lamps the player ran (seen or not)               */
    long   recoveries, stuck_episodes, stuck_removed;   /* stuck: under 1 km/h for > 5 s, not by the player */
    long   off_ticks, off_ticks_contact, off_events, off_rescues;
    long   barrier_hits;                      /* cop body probes pushed back by a race barrier        */
    long   road_pushes, road_pinches;         /* cop ticks pushed back from the road edge (pinch: opposite pushes, summed) */
    long   path_fixes;                        /* path samples moved back onto the road (median / corner cuts) */
    long   plan_offroad, bad_edges;           /* legs refused (no road under the line) / edges barred for cop legs */
    long   plans, plan_fails, wrecks;
    long   pursuit_ticks;
    int    max_heat, max_pursuers;
    double max_kmh;
    long   player_respawns;                   /* free roam busts: put back on a street                 */
    int    respawn_on_road;                   /* the last respawn landed on the drivable surface       */
} TD5_RoamCopsStats;
void td5_roam_cops_stats(TD5_RoamCopsStats *out);

#ifndef TD5RE_RELEASE
/* ---- DEV: the `roam-cops` self-test (td5_roam_cops_test.c) and its hooks here ---- */
/* Put a cop on the road at a PLACE point, facing yaw (0..4095), in PATROL state; hold = 1 parks it there (0 km/h)
 * until it starts a pursuit. Returns the slot, -1 when no slot is free. */
int  td5_roam_cops_dev_place(double px, double pz, int yaw12, int hold);
/* Overrides (-1 = the knob): patrol cops per anchor, pursuit top speed km/h. */
void td5_roam_cops_dev_force(int patrol_per_anchor, int vmax_kmh);
int  td5_roam_cops_dev_busted(void);         /* 1 while the BUSTED hold runs */
int  td5_roam_cops_dev_pursuer_behind(double min_m, double max_m);   /* a lit pursuer that far behind the player */

#define TD5_RCOPS_FULL   1
#define TD5_RCOPS_SMOKE  2
void     td5_roam_cops_test_arm(int mode);
void     td5_roam_cops_test_disarm(void);
int      td5_roam_cops_test_mode(void);
uint32_t td5_roam_cops_test_bits(struct TD5_Actor *a);   /* slot 0 control bits, 0 when idle */
int      td5_roam_cops_test_done(void);
int      td5_roam_cops_test_verdict(char *note, size_t n);   /* 0 PASS / 1 WARN / 2 FAIL */
#endif

#endif /* TD5_ROAM_COPS_H */
