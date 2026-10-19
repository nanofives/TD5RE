/**
 * td5_roam_lap.h -- GEO FREE ROAM: lap counting on a CLOSED route polyline (PORT-ONLY, F9a).
 * See docs/plans/GEO_FREE_ROAM.md (Decisions: sprint first, then circuits with laps; F9 row).
 *
 * WHAT IT IS. A circuit's route result is ONE lap (td5_roam_route_result()->closed): START ... FINISH
 * and the planner's closing leg back to START, last point = first point. A racer's progress is the
 * TOTAL distance, an unwrapped number that grows by a lap length L every time the racer goes round:
 *
 *      total = laps_completed * L + distance along this lap      (negative on the grid, behind START)
 *
 * It is a CONTINUOUS function of where the car is, never a crossing detector, so
 *   - crossing the line slowly, stopping on it or reversing over it cannot count twice: a lap is
 *     counted when the running MAXIMUM of total first reaches k * L, once per k;
 *   - a route that crosses itself (a figure 8) never jumps: the nearest-point search is windowed round
 *     the last answer measured ALONG the route (-40 m .. +150 m), so the other pass through the
 *     crossing is 100s of metres away on the polyline and never competes;
 *   - positions are ordered by total (= by laps, then distance), the same measure for the player and
 *     every AI racer.
 * The update adds the SIGNED wrapped difference of the new nearest point to the old one (shortest way
 * round, |delta| < L / 2), so the unwrap survives the closing seam.
 *
 * Units: polyline in the PLACE frame (430 units per metre, doubles), distance in metres.
 * No globals: the caller owns a TD5_RoamLoop (the polyline) and one TD5_LapTrack per racer.
 */
#ifndef TD5_ROAM_LAP_H
#define TD5_ROAM_LAP_H

typedef struct {
    double *px, *pz;     /* polyline, place frame (a private copy)                         */
    double *d;           /* distance along (m) of point i; d[0] = 0                         */
    int     n;
    double  L;           /* lap length (m), the closing segment last point -> first included */
    double  upm;         /* place units per metre                                           */
} TD5_RoamLoop;

typedef struct {
    int     init;
    double  total;       /* unwrapped distance along, m (negative behind the line)          */
    double  max_total;   /* running maximum of total                                         */
    double  wrapped;     /* distance along this lap, [0, L)                                  */
    double  lat_m;       /* distance from the polyline, m                                    */
    int     seg;         /* segment of the last answer (i -> (i + 1) % n)                    */
    int     lost;        /* the last update was farther than the lost distance: not applied  */
} TD5_LapTrack;

/* Copy a route result's polyline. `dist_m` may be NULL (re-measured). 1 on success. */
int  td5_roam_loop_init(TD5_RoamLoop *lp, const double *px, const double *pz, const double *dist_m, int npt, double upm);
void td5_roam_loop_free(TD5_RoamLoop *lp);
/* Unit direction of segment `seg` (place frame). */
void td5_roam_loop_dir(const TD5_RoamLoop *lp, int seg, double *ux, double *uz);

void td5_roam_lap_reset(TD5_LapTrack *t);
/* Move the track to the car at place point (x, z). The first call after a reset searches the whole
 * loop and puts a car in the second half of the lap BEHIND the line (grid: total < 0), else past it.
 * Returns 1 when the track was updated (0 = lost: farther than `lost_m` from the polyline). */
int  td5_roam_lap_update(const TD5_RoamLoop *lp, TD5_LapTrack *t, double x, double z, double lost_m);
/* Whole laps counted so far: floor(max_total / L), 0 while behind the line. `eps_m` finishes a hair early. */
int  td5_roam_lap_count(const TD5_RoamLoop *lp, const TD5_LapTrack *t, double eps_m);

#endif /* TD5_ROAM_LAP_H */
