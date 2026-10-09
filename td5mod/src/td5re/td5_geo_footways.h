/**
 * td5_geo_footways.h -- GEO TRACK: real OSM pedestrian ways (FOOTWAYS.JSON)
 *                       reader (PORT-ONLY, no original counterpart).
 *
 * The mapped pavement network of a real place: highway=footway / steps /
 * cycleway / path / pedestrian. Until round 1011 geo_fetch counted these as
 * `highway_nondrivable` and threw them away -- all 865 of La Plata's, which is
 * every pavement, plaza path and zebra line in the city centre.
 *
 * THESE ARE NOT ROADS AND NEVER BECOME ONE. They live in their own file and
 * their own pool precisely so no drivable reader can pick one up: the router,
 * the mouth table, the median detector and the carriageway all see exactly the
 * 2291 ways they saw before. A footway is geometry to DRAW on and a measurement
 * to READ, not a line to drive.
 *
 * COORDINATES are raw signed world units in the same frame as TG_Node,
 * ROUTE.JSON and SIGNALS.JSON -- geo_fetch rebuilds the conditioner's
 * projection before converting OSM, so no transform is applied here. Same
 * contract as td5_geo_roads.h.
 *
 * WIDTH is in METRES, not world units, for the same reason
 * td5_geo_roads_pavement_at returns metres: the cache's units_per_metre is a
 * property of the place and the caller already holds it. 27 of La Plata's
 * pedestrian ways carry a measured `width`; that is the ONLY real per-side
 * pavement measurement anywhere in the cache, since `sidewalk:width` is absent
 * on all 2291 roads (2026-10-08 attribute audit, section 2).
 *
 * BYTE IDENTITY. This module draws no tg_rand / tg_frand / tg_range and keeps
 * no RNG state, and every entry point returns empty when no place is loaded, so
 * a synthetic build cannot observe that it exists. Same promise as
 * td5_geo_roads.h:21-25.
 */
#ifndef TD5_GEO_FOOTWAYS_H
#define TD5_GEO_FOOTWAYS_H

/* What the way is FOR. `footway=sidewalk` / `=crossing` win over the highway
 * class, because a way tagged either of those is that thing whatever its class
 * says; everything else falls back to its class. La Plata: FOOTWAY 621,
 * CROSSING 108, STEPS 58, CYCLEWAY 38, PATH 26, SIDEWALK 8, PEDESTRIAN 6. */
#define TD5_GEO_FW_FOOTWAY     0   /* generic pavement, no subtype tagged */
#define TD5_GEO_FW_SIDEWALK    1   /* footway=sidewalk: a kerbside pavement */
#define TD5_GEO_FW_CROSSING    2   /* footway=crossing: a line across a road */
#define TD5_GEO_FW_STEPS       3
#define TD5_GEO_FW_CYCLEWAY    4
#define TD5_GEO_FW_PATH        5
#define TD5_GEO_FW_PEDESTRIAN  6   /* a pedestrianised street or plaza */
#define TD5_GEO_FW_KINDS       7

/* How a crossing is painted, decided in geo_fetch from `crossing` +
 * `crossing:markings` (see _crossing_paint there, which is the single home of
 * that rule). NONE on every way that is not a crossing. */
#define TD5_GEO_XP_NONE        0
#define TD5_GEO_XP_UNKNOWN     1   /* a crossing with no paint tag at all */
#define TD5_GEO_XP_UNMARKED    2   /* explicitly NOT painted */
#define TD5_GEO_XP_MARKED      3   /* paint a zebra */
#define TD5_GEO_XP_SIGNALS     4   /* signal-controlled, no marking stated */

/* Surface classes, same three buckets and the same spelling as
 * TD5_GEO_SURF_* in td5_geo_roads.h, so a caller that already maps a road
 * surface to a page maps a pavement surface with the same switch. */
#define TD5_GEO_FW_SURF_SMOOTH 0   /* asphalt, concrete, paving_stones  */
#define TD5_GEO_FW_SURF_COBBLE 1   /* sett, cobblestone, unhewn         */
#define TD5_GEO_FW_SURF_LOOSE  2   /* dirt, ground, gravel, sand, grass */

#define TD5_GEO_FOOTWAYS_MAX_WAY 512   /* points in one way, bound on a walk */

typedef struct {
    int    first, count;   /* slice of the shared point pool            */
    int    kind;           /* TD5_GEO_FW_*                              */
    int    paint;          /* TD5_GEO_XP_*, NONE unless kind==CROSSING  */
    int    surface;        /* TD5_GEO_FW_SURF_*                         */
    int    lit;            /* OSM lit=* was truthy                      */
    int    area;           /* a closed plaza/square rather than a line  */
    int    bridge, tunnel, covered;
    int    layer;
    int    step_count;     /* highway=steps only, 0 when untagged       */
    double width_m;        /* measured OSM width in METRES, 0 untagged  */
    double minx, minz, maxx, maxz;   /* bbox, for a cheap reject        */
} TD5_GeoFootway;

/* Load FOOTWAYS.JSON for `slug`, unloading any previous place. An empty or
 * NULL slug unloads and returns 0. A place with no FOOTWAYS.JSON (every cache
 * fetched before tag_schema 3) is NOT an error: the table stays empty and every
 * consumer falls back to whatever it did before. Idempotent for a slug already
 * loaded. Returns the number of ways held. */
int  td5_geo_footways_sync(const char *slug);
void td5_geo_footways_unload(void);

int  td5_geo_footways_count(void);
int  td5_geo_footways_points(void);          /* pool size, for the census log */
const char *td5_geo_footways_source(void);   /* path, "" when nothing loaded  */

const TD5_GeoFootway *td5_geo_footways_get(int i);
/* Point k of `f` in world units. 0 (outputs untouched) out of range. */
int  td5_geo_footways_point(const TD5_GeoFootway *f, int k, double *x, double *z);

/* How many ways of each TD5_GEO_FW_* kind are loaded. Out-of-range kind is 0. */
int  td5_geo_footways_kind_count(int kind);

/* ---------------------------------------------------- the C2 entry point ---
 *
 * NEAREST MAPPED KERBSIDE PAVEMENT to (x,z), searching only ways whose kind is
 * TD5_GEO_FW_SIDEWALK -- the 8 La Plata ways a mapper drew as a separate
 * pavement polyline rather than as a `sidewalk=*` tag on the road.
 *
 * This is the "mapped footway" rung of the per-side sidewalk-width ladder
 * (tag > mapped footway > facade > frontage rule). The caller holds the road
 * centreline and the side; the OFFSET returned here is the distance from the
 * query point to the pavement line, which is what a per-side width is measured
 * from. Call it with a point ON the centreline and the answer is
 * half-carriageway + pavement offset; call it with a point on the kerb and the
 * answer is the pavement width directly. Deciding which is the caller's.
 *
 *   x, z        query point, world units
 *   max_dist    search radius, world units. Beyond it the answer is 0.
 *   out_dist_u  distance to the nearest point ON the polyline, world units
 *   out_width_m the way's own measured `width` in METRES, 0 when untagged
 *   out_px/pz   that nearest point, world units (NULL to skip)
 *
 * Returns 1 on a hit, 0 otherwise (and leaves every output alone). O(ways)
 * behind a bbox reject, so it belongs in a prepass, not in a per-span loop --
 * same cost note as td5_geo_roads_pavement_at. */
int  td5_geo_footways_sidewalk_nearest(double x, double z, double max_dist,
                                       double *out_dist_u, double *out_width_m,
                                       double *out_px, double *out_pz);

/* The same search over EVERY kind in `kind_mask` (bit i = TD5_GEO_FW_ kind i),
 * for a caller that wants plaza paths or crossings rather than kerb pavement.
 * `out_kind` receives the kind that won. */
int  td5_geo_footways_nearest(double x, double z, double max_dist,
                              unsigned kind_mask, int *out_kind,
                              double *out_dist_u, double *out_width_m,
                              double *out_px, double *out_pz);

#endif /* TD5_GEO_FOOTWAYS_H */
