/**
 * td5_roam_barrier.h -- GEO FREE ROAM: NFSU2 route barriers (PORT-ONLY, F6-A).
 * See docs/plans/GEO_FREE_ROAM.md "Route, race and barriers" and the F6 row.
 *
 * WHAT IT IS. For a planned route (td5_roam_route_result()) the set of
 *   - MOUTH barriers (kind 0): one across every arm of every junction cluster the route passes
 *     that the route does NOT take, at the arm's cut row, kerb to kerb (a divided avenue's other
 *     carriageway is just another arm; a plaza ring's spokes and its far ring edge likewise);
 *   - TURN boards (kind 1): a chevron board on the outside of every turn sharper than ~30 deg,
 *     facing the approaching car, pointing the way the route turns.
 * Nothing is ever placed across the route itself. The start and finish clusters are sealed too
 * (every arm but the route's own), so a car cannot leave the route backwards at the grid.
 * [HE] The graph is not the surface. A mouth panel must END ON A WALL of the drivable surface (else it
 * slides out along its arm, or its ends are lengthened), and the SEAL PASS floods the car-passable surface
 * from the route over the build's own tiles, every panel a cut: a junction reached without an arm joins
 * the set, a street reached without an arm (a diagonal's ribbon overlapping a junction patch) gets a
 * SEAL PANEL across it (kind 0, cluster_key = that street's edge key). Repeats until no way out is left.
 * A short edge between two sealed junctions gets one panel, not two facing ones.
 *
 * FRAME (the frozen contract with F6-B, the race flow + racer-only collision):
 *   x0,z0,x1,z1  the two ends of the segment in the WORLD frame, 24.8 fixed point:
 *                world = (place - origin) << 8, origin = td5_roam_world_origin(), exactly the
 *                frame of actor world_pos. For kind 0 the segment runs kerb to kerb (the first
 *                and last lane-boundary point of the cut row, each pushed 20 cm into the kerb);
 *                for kind 1 it is the base line of the board.
 *   y0, y1       the ground height at (x0,z0) and (x1,z1), 24.8 world. A barrier is a VERTICAL
 *                WALL standing on that line, TD5_ROAM_BARRIER_H_M tall (3 m), so collision is a
 *                2-D segment test in xz (a car body is never above 3 m).
 *   kind         0 = mouth barrier: SOLID for racers (F6-B), ignored by traffic.
 *                1 = turn board: VISUAL ONLY, never collide.
 *   cluster_key  the junction cluster the barrier belongs to (RoamNodeRec.cluster_key).
 * The struct is frozen. Extra per-barrier data (arrow direction, colours) lives in a private
 * parallel array indexed like td5_roam_barriers().
 *
 * LIFE CYCLE. td5_roam_barriers_build(route) replaces the set (call again on a route change);
 * td5_roam_barriers_clear() empties it. The build reads ROAMTILE files itself (the route is not
 * bounded by the resident set), so it works with any streaming state; it is a one-off cost per
 * route (tens of ms), main thread. The set is stored in the PLACE frame and re-derived into the
 * WORLD frame lazily when td5_roam_world_origin() changes (origin rebasing, F9).
 *
 * DRAWING. td5_roam_barriers_draw(view) draws the visible set: glowing translucent panels
 * (alpha body + additive glow), chevrons marching along the route, depth-tested, drawn only for
 * barriers near the eye and in front of it. No texture pages (the shared white page).
 * Visible when TD5RE_ROAM_BARRIERS=1 (DEV: follows the route planned in F5-A, rebuilt on a route
 * change) or when F6-B turns it on with td5_roam_barriers_set_visible(1).
 */
#ifndef TD5_ROAM_BARRIER_H
#define TD5_ROAM_BARRIER_H

#include <stdint.h>
#include "td5_roam_route.h"

#define TD5_ROAM_BARRIER_MOUTH   0
#define TD5_ROAM_BARRIER_TURN    1
#define TD5_ROAM_BARRIER_H_M     3.0     /* wall height, metres */

/* The frozen contract. */
typedef struct {
    int32_t x0, z0, x1, z1;      /* WORLD frame 24.8 */
    int32_t y0, y1;              /* ground y at the two ends, 24.8 world */
    uint8_t kind;                /* TD5_ROAM_BARRIER_MOUTH / _TURN */
    uint8_t pad[3];
    uint64_t cluster_key;
} TD5_RoamBarrier;

/* The contract spells the route type TD5_RoamRouteResult; the F5-A type is TD5_RouteResult. */
typedef TD5_RouteResult TD5_RoamRouteResult;

int  td5_roam_barriers_build(const TD5_RoamRouteResult *route);   /* returns the count; rebuild on route change */
const TD5_RoamBarrier *td5_roam_barriers(int *count);              /* WORLD frame; valid until the next build/clear */
void td5_roam_barriers_clear(void);

/* ---- beyond the frozen contract (additive) ---------------------------------- */
void td5_roam_barriers_set_visible(int on);          /* F6-B: draw the set during a race */
int  td5_roam_barriers_visible(void);
void td5_roam_barriers_draw(int view);               /* per pane, in the world pass; self-gated */
/* [HE] the OPEN side of mouth barrier i (index into td5_roam_barriers()): a unit xz direction from the panel
 * back into the junction the route crosses, taken from the closed arm's own ribbon at build time (the frame is a
 * pure translation, so it holds in PLACE and WORLD). 0 for a turn board or when unknown. */
int  td5_roam_barrier_open_dir(int i, double *nx, double *nz);
/* [HE] the closed arm of mouth barrier i: its edge key and end. 0 for a turn board. */
int  td5_roam_barrier_arm(int i, uint64_t *edge_key, int *end);

typedef struct {
    int clusters_passed;      /* clusters the route goes through (start/finish pockets included) */
    int clusters_expanded;    /* neighbour clusters glued to a route cluster (no ribbon between): sealed like the rest */
    int passes;               /* route transitions through a cluster                           */
    int barriers_mouth, barriers_turn;
    int arms_total, arms_route, arms_blocked;   /* arms of those clusters: all / taken / barriered */
    int arms_linked;          /* arms left unbarriered on purpose: they lie along the route / have no ribbon row and only lead to another sealed cluster or a dead end */
    int arms_open;            /* arms neither taken nor barriered (must be 0)                   */
    int across_route;         /* barriers whose segment crosses the route polyline (must be 0)  */
    int missing_cluster;      /* a pass whose cluster record was not found in the tiles read    */
    int degree_open;          /* ribbon ends the PLANNER graph counts at those clusters that are neither taken nor barriered (must be 0) */
    int pass_gaps;            /* consecutive route edges that do not meet at one node key (must be 0)  */
    int ring_edges, oneway_edges;   /* route edges that are roundabout / one-way ribbons (plaza ring, divided avenue) */
    int slid, slide_fail;     /* mouth barriers moved outward along their arm (the cut row lay across the route) / none fit */
    int slid_wall;            /* [HE] ... further out, to a row whose two ends meet a wall of the drivable surface   */
    int ends_lengthened, ends_short;   /* [HE] no row had walls at both ends: panel ends lengthened to the wall / still short (the route is in the way) */
    int seal_panels;          /* [HE] panels the seal pass put across a street that overlaps the sealed surface   */
    int clusters_touching;    /* [HE] junctions the seal pass found reachable without an arm: sealed too          */
    int seal_iters, seal_entries_left, seal_fail;
    int pairs_single;         /* [HE] short edges between two sealed junctions closed by one panel instead of two facing ones */
    int seal_pockets, seal_escapes;   /* [HE] the last flood's unsealed entries: enclosed pockets / ways out (must be 0) */   /* [HE] floods run / entries the last flood still found (must be 0) / panels that did not fit */
    int boards_alt, boards_dropped;   /* turn boards put on the outer kerb of the exit arm / left out (ahead and kerb both on the route) */
    int arrows_with, arrows_side, arrows_against;   /* mouth-barrier chevrons vs the nearest route direction (against must be 0) */
    int arm_no_row;           /* arms with no ribbon row (edge without quads): nothing to close  */
    int tiles_read;
    double build_ms;
    uint32_t route_hash;
} TD5_RoamBarrierStats;
void td5_roam_barriers_stats(TD5_RoamBarrierStats *out);

#ifndef TD5RE_RELEASE
/* DEV: TD5RE_ROAM_BARRIER_TEST=1 -> plans the scripted census routes with the F5-A planner,
 * builds the barriers for each, logs + writes <log dir>/roam_barrier_census.txt and
 * roam_barrier_<route>.json (barriers + route polyline, place frame) and returns 1 (exit). */
int td5_roam_barriers_dev_test(void);
/* [HE] DEV: arm i of every cluster the last build sealed: status 0 taken by the route, 1 barriered, 2 linked (left
 * open on purpose: a twin carriageway / dead end), 3 OPEN; i < 0 returns the count. */
int td5_roam_barriers_dev_arm(int i, int *status, uint64_t *edge_key, uint64_t *cluster_key);
#endif

#endif /* TD5_ROAM_BARRIER_H */
