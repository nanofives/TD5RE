/**
 * td5_roam_drive.h -- GEO FREE ROAM: scripted drive + verdict for the `roam` selftest
 * rows (PORT-ONLY, DEV-ONLY, F2 task D).
 *
 * The self-test director (td5_selftest.c) arms one of two scripted drives, the input
 * layer asks it for slot 0's control bits every sim tick (td5_input.c, same overlay as
 * the other dev drives), the director polls td5_roam_drive_done() and finally reads the
 * verdict. Everything the verdict judges comes from the locator and the F2-B contact
 * provider's counters (td5_roam_physics_stats), measured every sim tick.
 *
 *   TD5_RDRV_ROUTE    junction tour: street follower with a turn script (straight / left
 *                     / straight / right ..., homing back past 1.4 km), >= 20 distinct
 *                     junction clusters, then brake to rest and sample the rest height.
 *                     Invariants: never below the surface, never > 5 cm above it at rest,
 *                     never stuck (> 3 s under 5 km/h with throttle), never off the surface
 *                     (rescues 0 incl. the spawn), pinches 0, teleports 0.
 *   TD5_RDRV_CORNERS  the design 2.5 corner matrix: up to N junctions of each kind (one-lane/
 *                     service, calle x calle, avenida x calle, diagonal x avenida), each
 *                     driven on a planned path (F2-B's picker + pure pursuit; a path that
 *                     leaves the road is routed on the paved mask) in three passes: held
 *                     40 km/h, held 80 km/h, and a BRAKING driver that arrives at 80 km/h
 *                     and slows for the bend. Pinches and rescues fail on every pass; exit
 *                     and stuck are judged on the braking pass only (a held-speed 135 deg
 *                     city corner is not a fair stuck test, orchestrator 2026-10-17).
 *                     TD5RE_ROAM_SELFTEST_CORNER_BRAKE=0 = the old two held passes, all judged.
 *
 * Knobs (dev): TD5RE_ROAM_SELFTEST_JUNCTIONS (24), _KMH (36), _MAX_SECS (300),
 * _CORNERS_PER_KIND (20), _CORNER_GAP_M (120).
 *
 * One file-scope struct holds the whole state. Compiled out of RELEASE.
 */
#ifndef TD5_ROAM_DRIVE_H
#define TD5_ROAM_DRIVE_H

#include <stddef.h>
#include <stdint.h>

struct TD5_Actor;

#define TD5_RDRV_OFF      0
#define TD5_RDRV_ROUTE    1
#define TD5_RDRV_CORNERS  2

#ifndef TD5RE_RELEASE
void     td5_roam_drive_arm(int mode);      /* before the roam race launches (resets all counters) */
void     td5_roam_drive_disarm(void);
/* [F8] after arm: the TOUR (a square of side_m round the spawn for secs, then rest) without the env knobs */
void     td5_roam_drive_set_tour(double side_m, int secs);
/* [F8] after arm: ambient traffic shares the road (rest / stuck / off-surface / rescues reported, not judged) */
void     td5_roam_drive_set_traffic(int on);
int      td5_roam_drive_mode(void);
/* slot 0's control bits for this sim tick (0 when not armed / not in roam / done) */
uint32_t td5_roam_drive_bits(struct TD5_Actor *a);
/* 1 once the script has finished (route done, matrix done, or aborted) */
int      td5_roam_drive_done(void);
/* The verdict. Returns 0 PASS / 1 WARN / 2 FAIL (the selftest's ST_* values) and a note that
 * fits 120 bytes. */
int      td5_roam_drive_verdict(char *note, size_t n);
#endif

#endif /* TD5_ROAM_DRIVE_H */
