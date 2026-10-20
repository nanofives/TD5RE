/**
 * td5_police.h -- POLICE PURSUIT for the normal races (PORT-ONLY, round H HH, 2026-10-19).
 *
 * Mariano: "remove the 'fine' and 'money' references like the ones in the busted logic, extend this new
 * functionality into the main game outside this mode". Free roam's police (td5_roam_cops.c: pursuit, heat 1..5,
 * reinforcements, roadblocks, bust meter + BUSTED hold, escape) brought to every race that has POLICE on. No fines
 * and no money anywhere.
 *
 * WHAT IT SITS ON. The stock traffic police (td5_ai.c "Police chase rewrite 2026-06-19"): every CopRatio-th dynamic
 * traffic spawn is a cop (g_cop_is_cop), it cruises as traffic until a HUMAN racer passes it above the speeding gate,
 * then chases that car (cop_drive, spans) and, once it has held a lead for 1 s, PULLS IT OVER (g_encounter_active:
 * the car is braked to a stop). This module keeps that driver and the pull-over, and adds the free-roam rules ON TOP,
 * per human racer slot:
 *
 *   PURSUIT     starts when a cop acquires the car (SPEEDING: the stock passed-at-speed trigger) or when the car
 *               rams a cop (contact, impact over TD5RE_POLICE_RAM_IMPACT): the rammed cop gives chase. Toast
 *               "POLICE PURSUIT: ...", the pursuit panel (heat boxes, cops on you, the EVADE bar).
 *   HEAT 1..5   1 at the start, +1 per further offence (passing another cop at speed, ramming a cop) and per 25 s.
 *   JOINS       while a pursuit runs, a cop the car passes joins at any speed; from heat 2 an idle cop within 200 m
 *               BEHIND the car joins too (a cop ahead waits to be passed: the span chaser would otherwise "overtake"
 *               at once and pull the car over).
 *   REINFORCE   pursuers wanted = 1 + heat: while fewer cops are on the road, the next traffic spawn (ahead, in the
 *               normal spawn window) is a cop, one per 3 s.
 *   ROADBLOCKS  from heat TD5RE_POLICE_BLOCK_HEAT (3): a block on the MAIN-RING SPAN 200..400 m ahead of the car
 *               (TD5RE_POLICE_BLOCK_MIN_M / _MAX_M), away from fork / loop mouths, across the span's road band
 *               (the lane quads, slow verge lanes trimmed). 2 cars at heat 3, 3 at heat 4-5, parked across the road
 *               with the lights on, held still, solid for everyone (normal V2V). A race always keeps a gap so the
 *               field can pass: 4.2 m at heat 3, 3.6 m from heat 4 (free roam's race rule). At most 2 blocks, 20 s
 *               apart. Removed once passed (35 m), out of range (560 m), at the end of the pursuit, or after 90 s.
 *               The cars are dynamic traffic slots (td5_ai_traffic_police_block_place) so the pool, the fade, the
 *               police mesh and the collision are the normal ones.
 *   BUST        the car under 10 km/h with a pursuer (or a block car) within 10 m fills the PULL OVER meter in 2 s
 *               (drains at the same rate). A completed stock pull-over (the cop held its lead, the car came to rest)
 *               is a bust too. BUSTED: the car is held braked 3 s under the banner (the stock pull-over hold, through
 *               td5_ai_is_encounter_active), then released where it stands; every pursuer drops back to traffic;
 *               15 s of grace. No game over, no fine.
 *   ESCAPE      no pursuer within sight (150 m, same road section) for 6 + 2 x heat seconds, or every pursuer
 *               farther than 320 m, or none left (all fell back / wrecked). The cops drop the chase, 5 s of grace.
 *
 * WHEN. A race with the POLICE option on (g_encounter_enabled), dynamic traffic running, not free roam, not the Cop
 * Chase mode (wanted mode: its own rules), not drag. TD5RE_POLICE_PURSUIT=0 turns the layer off (stock police only).
 * With POLICE off nothing here runs: no state, no hook changes a byte.
 *
 * DETERMINISM. Every decision is a pure function of replicated sim state (spans, positions, speeds, the tick), the
 * only random draw is this module's own LCG seeded at race init: lockstep-safe like the stock police.
 *
 * HUD. td5_police_hud_draw is the ONE drawing of the pursuit panel / PULL OVER meter / BUSTED banner / toast, used by
 * free roam (td5_roam_cops_draw_hud) and the normal races (td5_police_draw_hud, per split-screen pane).
 */
#ifndef TD5_POLICE_H
#define TD5_POLICE_H

#include <stdint.h>
#include <stddef.h>

/* ---- the shared HUD drawing (free roam + races) ------------------------------------ */
typedef struct {
    int         pursuit;        /* 1 = the PURSUIT panel (top right of the pane)         */
    int         heat;           /* 0..5                                                    */
    int         cops;           /* pursuers                                                */
    float       evade;          /* 0..1 the EVADE bar                                      */
    float       bust;           /* 0..1 the PULL OVER meter, 0 = hidden                    */
    int         busted;         /* 1 = the BUSTED banner (replaces the panel)              */
    const char *busted_line;    /* the line under BUSTED                                   */
    const char *toast;          /* NULL / "" = none                                        */
    uint32_t    toast_col;
    long        tick;           /* the flash phase                                         */
} TD5_PoliceHud;
/* Draws in the pane (x0, y0, w, h), screen pixels. Sets and restores the HUD render preset. */
void td5_police_hud_draw(const TD5_PoliceHud *h, float x0, float y0, float w, float h_px);

/* ---- race lifecycle (td5_game.c) ----------------------------------------------------- */
void td5_police_race_init(void);          /* InitializeRaceSession, after the pull-over latch reset */
int  td5_police_active(void);             /* the layer runs this race                              */
void td5_police_tick(void);               /* every sim tick, after td5_ai_update_special_encounter */
void td5_police_draw_hud(void);           /* HUD overlay pass, every pane; self-gated               */

/* ---- hooks from the stock police scheduler (td5_ai.c) -------------------------------- */
enum { TD5_POLICE_GATE_STOCK = 0, TD5_POLICE_GATE_SKIP = 1, TD5_POLICE_GATE_JOIN = 2 };
/* An idle cop `cop` looks at racer `target`, `gap` spans ahead of it (>0 = the racer is ahead). SKIP: leave it
 * (grace, BUSTED hold, finished); JOIN: acquire now whatever the speed gate says; STOCK: the stock gate decides. */
int  td5_police_acquire_gate(int cop, int target, int gap);
void td5_police_on_acquire(int cop, int target, int joined);       /* a cop started chasing `target`        */
void td5_police_on_pullover_stopped(int cop, int target);          /* a stock pull-over brought it to rest  */
int  td5_police_block_slot(int slot);     /* 1 = a roadblock car: the scheduler, the driver and the despawn skip it */
int  td5_police_block_lit(int slot);      /* 1 = light bar on (a roadblock car)                     */
void td5_police_block_hold(int slot);     /* the roadblock car's control for this tick (before physics) */

/* ---- traffic spawner (td5_ai_traffic.c) ---------------------------------------------- */
int  td5_police_want_cop_spawn(void);     /* 1 = make the next traffic spawn a cop (reinforcement)  */

/* ---- physics (td5_physics_collision.c) ----------------------------------------------- */
void td5_police_on_contact(int sa, int sb, int32_t impact_mag);   /* every V2V impulse                      */

/* ---- input (td5_input.c) ------------------------------------------------------------- */
int  td5_police_held(int slot);           /* 1 = BUSTED: the car is held braked                     */

/* ---- queries + counters ------------------------------------------------------------------ */
int  td5_police_heat(int slot);           /* 0 = no pursuit on that racer                           */
typedef struct {
    long pursuits, notice_speed, notice_ram, joins, joins_behind, offences, reinforcements;
    long escapes, escapes_los, escapes_dist, busts, busts_pullover, busts_meter;
    long blocks_placed, block_cars, blocks_gap, blocks_rammed, blocks_passed, blocks_removed, block_fails, block_hits;
    long hold_ticks, hold_moving_ticks;   /* BUSTED hold ticks / of them with the car above 3 km/h   */
    long releases;
    int  max_heat, max_pursuers, blocks_active;
    long block_ai_hits;                   /* AI racer contacts with a roadblock car                 */
} TD5_PoliceStats;
void td5_police_stats(TD5_PoliceStats *out);

#ifndef TD5RE_RELEASE
/* ---- DEV: the `police` self-test (td5_police_test.c) and its hooks ---------------------- */
void td5_police_dev_set_heat(int slot, int h);    /* make the heat at least h in a running pursuit           */
int  td5_police_dev_pursuers(int slot);           /* cops chasing `slot` now                                 */
int  td5_police_dev_busted(int slot);             /* 1 while the BUSTED hold runs                            */
int  td5_police_dev_pursuit(int slot);            /* 1 while a pursuit runs                                  */
int  td5_police_dev_block(int i, int *span, int *ncar, double *gap_m);   /* active block i (0..1), 0 = none  */
void td5_police_dev_slow_pursuers(int level);     /* 1 = the pursuers keep 40 m off, 2 = they brake (escape) */
int  td5_police_dev_slow(void);
#define TD5_POLICE_TEST_SMOKE 1
#define TD5_POLICE_TEST_FULL  2
void     td5_police_test_arm(int mode);
void     td5_police_test_disarm(void);
int      td5_police_test_mode(void);
uint32_t td5_police_test_bits(void);              /* slot 0 THROTTLE / BRAKE bits, 0 when idle               */
int      td5_police_test_done(void);
int      td5_police_test_verdict(char *note, size_t n);   /* 0 PASS / 1 WARN / 2 FAIL                         */
void     td5_police_test_tick(void);              /* from td5_police_tick                                    */
#endif

#endif /* TD5_POLICE_H */
