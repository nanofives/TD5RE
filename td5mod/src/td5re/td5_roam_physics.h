/**
 * td5_roam_physics.h -- GEO FREE ROAM: the contact provider + the resident set
 * (PORT-ONLY, F2 task B). Design: docs/plans/GEO_FREE_ROAM_DESIGN.md 2.1 - 2.6,
 * 0 item 7. Span-consumer audit: docs/plans/geo_free_roam/F0b_PHYSICS_ENTRYPOINTS.md.
 *
 * WHAT IT IS. In a roam session there is no strip: the physics contact data a racer
 * needs every tick (4 wheel heights + normals + capped + surface, 4 body-corner
 * heights, the chassis heading normal, the wall push) comes from the roam ROAD
 * layer through td5_roam_locate instead of the span walker. The td5_track.c contact
 * functions early-out to this module when td5_roam_active(); the wheel and corner
 * loops of td5_physics_refresh_wheel_contacts call it directly because they have
 * the probe's Y (a wheel under a bridge needs it to tell the deck from the road).
 *
 * RESIDENT SET (F2: no streaming). td5_roam_physics_session_begin loads every casco
 * ROAMTILE of the session's place once and registers the set with
 * td5_roam_set_resident. The WORLD frame is world = place - td5_roam_world_origin()
 * (the spawn tile origin, design 0 item 7); tiles keep their place origins and the
 * locator adds the world origin back, so no stored coordinate is rewritten.
 * td5_roam_physics_live() also begins the session lazily (and ends it once roam is
 * no longer active), so a missing explicit call cannot leave the car on Y = 0.
 *
 * WALLS (design 2.5). ONE combined push per actor per tick, from the deepest body
 * corner; two outside corners with opposing push normals are a pinch: no translation
 * that tick (impulse only) + a ROAM_WALL_PINCH log line. A body corner is a wall
 * contact only past ROAM_KERB_OVERHANG (0.35 m); wheels never get that tolerance.
 * The wall is the boundary of the UNION of the drivable surface (the locator's rule).
 * Then the off-surface backstop: a chassis more than ROAM_RESCUE_DIST outside every
 * surface is snapped back inside the nearest boundary (ROAM_RESCUE), a chassis over
 * no tile at all goes back to its last good position (ROAM_TELEPORT).
 *
 * MIRROR (design 2.6 stage A, DEV knob TD5RE_ROAM_MIRROR=1). On a normal strip race
 * a one-tile roam road layer is built as a 1:1 copy of the strip's type-1 spans and
 * every racer's contact data (wheels, corners, heading normal) is taken from the
 * provider while AI / camera / render / walls stay on the strip. Each answer is
 * compared with the strip's and counted (roam_phys.txt in the log dir).
 *
 * NO FILE-SCOPE MUTABLE STATE outside the one session struct in the .c.
 */
#ifndef TD5_ROAM_PHYSICS_H
#define TD5_ROAM_PHYSICS_H

#include <stdint.h>

struct TD5_Actor;

#define ROAM_RESCUE_DIST   645     /* units (1.5 m): chassis this far outside = off the road */

/* Session. begin loads + registers the resident set (idempotent: a second call
 * while loaded is a no-op); end frees it and clears the registration. 1 = ready. */
int  td5_roam_physics_session_begin(void);
void td5_roam_physics_session_end(void);

/* 1 when the provider owns racer contacts: a roam session (tiles loaded), or the
 * DEV mirror. */
int  td5_roam_physics_live(void);
/* 1 when the DEV mirror runs (strip loaded, contacts compared + replaced). */
int  td5_roam_physics_mirror(void);

/* ---- physics call sites (td5_physics_suspension.c) -------------------------
 * Wheel i (0..3) at the transformed contact point (24.8 world; y = the wheel's
 * transformed Y, the deck pick). In the mirror the in/out values hold the strip's
 * answer on entry (compared, then replaced); *surface < 0 on entry = not compared.
 * Returns 1 when it wrote an answer. */
int  td5_roam_wheel_contact(struct TD5_Actor *a, int i, int32_t x, int32_t z, int32_t y,
                            int32_t *ground_y, int16_t normal[3], int *surface, int *capped);
/* Body corner i (0..3) at (x, z, y): ground height at the corner into *y_io.
 * strip_valid = *y_io holds the strip's answer (mirror compare). Returns 1 when it
 * wrote *y_io. */
int  td5_roam_corner_height(struct TD5_Actor *a, int i, int32_t x, int32_t z, int32_t y,
                            int32_t *y_io, int strip_valid);

/* Spawn: after td5_physics_reset_actor_state, make sure the car stands on the street
 * under it (locator height + 0.5 m when the reset's sentinel snap did not land it). */
void td5_roam_physics_spawn_snap(struct TD5_Actor *a);

/* ---- td5_track.c early-outs (roam session only) ---------------------------- */
/* chassis locate: side table, slot edge, span fields kept at -1. */
void    td5_roam_actor_locate(struct TD5_Actor *a);
/* heading_normal from the chassis primitive. compare = mirror (the strip value is
 * already in a->heading_normal). Returns 1 when it wrote. Never writes y == 0. */
int     td5_roam_heading_normal(struct TD5_Actor *a, int compare);
/* one combined wall push + the off-surface backstop. */
void    td5_roam_resolve_walls(struct TD5_Actor *a);
/* surface of probe 0..3 (body corners) / 4..7 (wheels) from the side table. */
int     td5_roam_surface_type(struct TD5_Actor *a, int probe_index);
/* the capped flag of the last provider height. */
int     td5_roam_last_capped(void);
/* Ground (camera, shadow, HUD, scripted-mode callers): height (24.8 world), normal
 * (may be NULL). has_y = 0 picks the top surface. 0 when nothing is resident there. */
int32_t td5_roam_ground(int32_t x, int32_t z, int32_t y, int has_y, int16_t *normal);
#define td5_roam_ground_xz(x, z, n) td5_roam_ground((x), (z), 0, 0, (n))

/* Counters (harnesses, the roam_phys.txt summary). */
typedef struct {
    long ticks, wall_hits, pinches, rescues, rescues_y, teleports, nohit_wheels, outside_wheels;
    long wall_side_flips, pinch_rescues;   /* [F2-E] thin-wall pushes flipped back, pinch rescues */
    long pinch_tips;                       /* [F2-E] wall-tip pinches resolved by one translation */
    long wall_corner_pen_max;
    long mirror_cmp, mirror_diff_h, mirror_diff_n, mirror_diff_cap, mirror_diff_surf, mirror_scmp;
    long mirror_fallback, mirror_corner_cmp, mirror_corner_diff, mirror_corner_fallback;
    long mirror_head_cmp, mirror_head_diff, mirror_head_fallback;
    long mirror_dh_max;
    long mirror_diff_walker, mirror_diff_same, mirror_corner_walker, mirror_corner_same;
    /* slot 0 ground gaps (24.8): transformed wheel bottom - ground, chassis - ground */
    long gap_n, rest_n, chassis_seen;
    int32_t gap_min, rest_min, rest_max, chassis_min_above;
    int64_t rest_sum;
} TD5_RoamPhysStats;
void td5_roam_physics_stats(TD5_RoamPhysStats *out);

#ifndef TD5RE_RELEASE
/* DEV: autopilot bits for slot 0 (td5_input.c overlays them like the input script).
 * TD5RE_ROAM_DRIVE=street (follow the street the car is on, TD5RE_ROAM_DRIVE_KMH,
 * TD5RE_ROAM_DRIVE_SECS) | corners (the 90-degree corner matrix, design 2.5).
 * 0 when off or not in roam. */
uint32_t td5_roam_dev_drive_bits(struct TD5_Actor *a);
/* [F2-E] Dev harness helper (the corner selftest). (px, pz)[*np] is a corner path in world units:
 * [0] the start, [*np - 2] the exit end row, [*np - 1] `lout` further out along (outx, outz). When it
 * runs off the drivable surface it is replaced by an A* route on the paved mask (the F2-E corner
 * harness's rd_route) of at most `cap` points. Returns the metres of the INPUT path off the road;
 * *routed = 0 on the road, 1 routed, 2 off the road but no route (path kept). */
double   td5_roam_dev_route_path(double *px, double *pz, int *np, int cap, double cx, double cz,
                                 double outx, double outz, double lout, int *routed);
#endif

#endif /* TD5_ROAM_PHYSICS_H */
