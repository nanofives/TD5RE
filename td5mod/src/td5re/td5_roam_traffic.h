/**
 * td5_roam_traffic.h -- GEO FREE ROAM: ambient traffic on the street graph (PORT-ONLY, F8).
 * See docs/plans/GEO_FREE_ROAM.md (Decisions: ambient traffic obeying the real OSM signals, passes
 * THROUGH race barriers, traffic toggle per race; "Ambient traffic and cops"; F8 row) and
 * GEO_FREE_ROAM_DESIGN.md 5.1 item 7 (traffic slots skip the wall resolver: the lane follower keeps
 * them on the road, not walls).
 *
 * THE POOL. A roam session reserves traffic slots g_traffic_slot_base .. +pool-1 (6..29, the roam
 * physics side table holds 32 slots) and loads the traffic car set into them at race init. A slot is
 * PARKED (no physics, no collision, not drawn, silent: td5_ai_traffic_dynamic_parked /
 * td5_ai_traffic_get_draw_alpha answer for it) until the spawner puts a car on the road. The cars are
 * normal TD5 traffic actors: the simplified traffic dynamics, the traffic pose, the full V2V contact
 * with racers. Barriers are racer-only (td5_roam_race_wall.c), so traffic drives through them.
 *
 * THE RING. Cars spawn on resident ROAD tiles at 110..210 m from an ANCHOR (the player; in a race also
 * every live AI racer) while the anchor has fewer than its target within 250 m, and fade out past
 * 280 m from every anchor. Density: free roam TD5RE_ROAM_TRAFFIC (cars per anchor, 14, 0 = none);
 * a race reads its setup's TRAFFIC row (OFF / LOW / HIGH = TD5RE_ROAM_TRAFFIC_LOW 4 /
 * TD5RE_ROAM_TRAFFIC_HIGH 8 per anchor); never more than TD5RE_ROAM_TRAFFIC_MAX live cars.
 *
 * THE DRIVER. Each car follows a LANE of a directed edge of the planner graph (td5_roam_route.h:
 * the ribbon centre lines, one-way respected): right-hand traffic, the lane's offset from the
 * carriageway's lane count, kept on the drivable surface by the locator. At the end of an edge a
 * cubic connector carries it to a lane of the next edge, picked at random (straight preferred) from
 * the car's own seeded generator. Speed by street class, a braking profile into each connector, gap
 * keeping behind any car ahead, a stop at the STOP LINE (the arm's cut row = the edge's last row) when
 * the arm's signal head is red (or amber and it can stop), a crude first-come yield at unsignalised
 * junctions. A car stuck for 10 s, off the surface for 1 s, at a dead end or off the resident set is
 * removed.
 *
 * SIGNAL GROUPS. A signalised cluster runs two heads half a cycle apart (td5_geo_signal_phase_at,
 * cross = 0 / 1). td5_roam_signal_group says which head an approach ARM obeys; the furniture
 * (td5_roam_furniture.c) draws each approach's facing head with that same group, so the lamp a car
 * stops at is the lamp the driver sees.
 *
 * STATE. One file-scope struct, main thread only, inert outside a roam session.
 */
#ifndef TD5_ROAM_TRAFFIC_H
#define TD5_ROAM_TRAFFIC_H

#include <stdint.h>
#include <stddef.h>

#define TD5_ROAM_TRAFFIC_POOL_MAX  24

/* ---- race init (td5_game.c) ------------------------------------------------- */
int  td5_roam_traffic_pool(void);             /* traffic slots this session reserves (0 = traffic off) */
void td5_roam_traffic_load_assets(void);      /* step 5b in roam: the car set into the pool slots       */
void td5_roam_traffic_session_init(void);     /* end of race init: every pool slot parked               */
void td5_roam_traffic_session_end(void);
int  td5_roam_traffic_field_total(void);      /* actor count the field keeps with traffic (0 = none)    */

/* ---- per sim tick, BEFORE physics (td5_game.c, the roam branch of the AI tick) -- */
void td5_roam_traffic_tick(void);

/* ---- the parked / alpha answers for the shared traffic consumers ------------- */
int  td5_roam_traffic_parked(int slot);       /* -1 not a roam traffic slot, else 1 parked / 0 live     */
int  td5_roam_traffic_alpha(int slot);        /* -1 not a roam traffic slot, else 0..255                */

/* ---- the signal group rule (shared with td5_roam_furniture.c) ----------------- */
/* Arm `a` of a cluster whose n arms have bearings bear[] (ARM bearing12, tile order): 0 = the main head
 * (td5_geo_signal_phase_at cross 0), 1 = the cross head. Axis of arm 0 = group 0; an arm more than
 * 45 deg off that axis is group 1. Pure. */
int  td5_roam_signal_group(const int16_t *bear, int n, int a);

/* ---- tests / DEV ---------------------------------------------------------------- */
void td5_roam_traffic_force_race_level(int level);   /* -1 = the setup's; 0 off / 1 low / 2 high */
void td5_roam_traffic_force_density(int cars);       /* -1 = the knob; free-roam cars per anchor */

typedef struct {
    int    pool, live, target;          /* now                                                     */
    long   samples;                     /* ticks sampled                                           */
    int    live_min, live_max;          /* over the samples after the warm-up                       */
    double live_mean;
    long   spawns, despawns;
    long   despawn_far, despawn_dead_end, despawn_stuck, despawn_hold, despawn_off, despawn_unres, despawn_lost, despawn_wreck;
    long   wrecks;                      /* cars wrecked by a hard hit (stand 8 s, then removed)      */
    long   off_ticks;                   /* live-car ticks with the centre off the drivable surface */
    long   off_ticks_contact;           /* ...of those, within 2 s of a racer contact              */
    long   off_events;                  /* off for > 1 s (despawned)                               */
    long   red_violations;              /* crossed a stop line on red at a signalised cluster      */
    long   sig_cross, sig_stops;        /* stop-line crossings at signalised clusters / red stops   */
    long   stuck_live;                  /* car-ticks still stuck past 10 s and not despawned        */
    long   yield_waits;                 /* unsignalised junction waits started                     */
    long   turns[3];                    /* left / straight / right connectors                       */
    long   conn_off_plans;              /* turns planned with no connector fully on the patch       */
    long   barrier_passes;              /* traffic crossings of a race barrier (racer-only walls)   */
    long   lane_changes;                /* lane changes made before a turn                          */
    long   racer_launches;              /* DEV: a racer going up faster than 6 m/s (contact launch?)  */
    long   racer_near;                  /* car-ticks with a racer within 4 m (contacts)             */
    double max_lat_m;                   /* largest |offset| from the lane's target                  */
    double max_kmh;
    double max_ground_dy_m;             /* |actor y - y-aware ground| (bridge decks), max           */
} TD5_RoamTrafficStats;
void td5_roam_traffic_stats(TD5_RoamTrafficStats *out);
void td5_roam_traffic_stats_reset(void);
int  td5_roam_traffic_nearest(int32_t wx, int32_t wz, double *dist_m, int32_t *wy);   /* DEV */
/* DEV shots: a live car holding at a red stop line (slot, place xz of the cluster), 0 when none */
int  td5_roam_traffic_find_red_stop(int *slot, double *cx, double *cz);
#ifndef TD5RE_RELEASE
/* [F8e] DEV ram test: a stand-still traffic car at place (px,pz) yaw12 (slot or -1), released to physics, cleared */
int  td5_roam_traffic_dev_place(double px, double pz, int yaw12);
void td5_roam_traffic_dev_release(void);
void td5_roam_traffic_dev_clear(void);
/* The self-test verdict over the session so far: 0 PASS / 1 WARN (red-light violations, or private bytes up
 * more than 32 MB early-half vs late-half average) / 2 FAIL (density not held in free roam, a car off the
 * surface without a racer contact, a car stuck > 10 s not removed). race_row: only "traffic was on the road"
 * for density. */
int  td5_roam_traffic_verdict(char *note, size_t n, int race_row);
#endif

#endif /* TD5_ROAM_TRAFFIC_H */
