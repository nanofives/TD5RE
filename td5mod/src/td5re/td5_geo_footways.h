/**
 * td5_geo_footways.h -- GEO TRACK: separately mapped pavements (PORT-ONLY).
 *
 * OWNED BY ROUND 1011 C4. This header was pre-created by C2 so that the
 * pavement-width resolver could be written against the agreed contract before
 * the geometry landed; C4's module REPLACES this file and its implementation.
 * The signature below is the orchestrator's, fixed for both children -- do not
 * change it here without telling the other side.
 *
 * WHY IT MATTERS TO C2. A separately mapped footway beside a road is a
 * MEASUREMENT: the distance between two real polylines. That puts it above a
 * measured facade and far above the building-line rule in the priority order
 * stated in td5_geo_sidewalk.h. Until C4 lands, a WEAK definition in
 * td5_geo_sidewalk.c answers 0 for every query, which the resolver reads as
 * "no footway here" and falls through unchanged. When C4's strong definition
 * is linked it wins and the weak one drops out, with no edit on this side.
 */
#ifndef TD5_GEO_FOOTWAYS_H
#define TD5_GEO_FOOTWAYS_H

/* Is there a mapped footway=sidewalk on the side (nx,nz) points to?
 *
 * (x,z) and the unit normal (nx,nz) are in the place cache's ROUTE-FRAME units,
 * the same frame as ROADS.JSON points. `max_m` bounds the search in METRES.
 *
 * Returns 1 when a footway line lies on that side within `max_m` of (x,z) and
 * runs roughly parallel to the road (within about 20 degrees), and then writes:
 *   *out_dist_m   perpendicular distance from (x,z) to the footway centreline,
 *                 METRES;
 *   *out_width_m  its own width tag in METRES, or 0 when untagged.
 * Returns 0 otherwise, and writes nothing. Either out pointer may be NULL.
 *
 * The pavement width the caller derives from this is
 *     (dist + (width > 0 ? width / 2 : 0)) - the carriageway half-width
 * because `dist` reaches the footway's CENTRELINE, not its near edge. */
int td5_geo_footway_sidewalk_near(double x, double z, double nx, double nz,
                                  double max_m, double *out_dist_m,
                                  double *out_width_m);

#endif /* TD5_GEO_FOOTWAYS_H */
