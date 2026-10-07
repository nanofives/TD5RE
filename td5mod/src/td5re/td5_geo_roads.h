/**
 * td5_geo_roads.h -- GEO TRACK: the real OSM road graph (ROADS.JSON) reader
 *                    (PORT-ONLY). See docs/plans/GEO_TRACK_OSM_PLAN.md phase 5.
 *
 * WHY A SEPARATE MODULE. td5_geo.c's own header states its job is the TERRAIN
 * source: three rasters plus the conditioned route. The street network is a
 * different consumer (td5_tg_network.c, the (span,side) street authority) with
 * a different lifetime, and four Phase 5 workstreams run in parallel -- so each
 * vector layer gets its own reader rather than growing one shared file. This
 * module knows the file format; td5_tg_network.c knows the geometry.
 *
 * WHAT IT IS NOT. It does no matching against the route, no validation against
 * the occupancy raster and no span arithmetic. It answers "what roads did OSM
 * record here, in world units" and nothing else.
 *
 * FRAME. Coordinates are the `points` array of ROADS.JSON: raw signed world
 * units in the SAME frame as TG_Node and as the rasters (geo_fetch.py is re-run
 * with --frame-from ROUTE.JSON precisely so the two agree -- see td5_geo.h).
 * No offset search, no rotation applied here.
 *
 * BYTE-IDENTITY. Nothing here is reached unless a place is loaded: the only
 * entry point is td5_geo_roads_sync(slug), and td5_tg_network.c calls it only
 * on the geo path. It draws no tg_rand/tg_frand/tg_range (the standing rule at
 * td5_trackgen_internal.h:1290-1296) and keeps no RNG state of its own, so a
 * synthetic build cannot observe it.
 */
#ifndef TD5_GEO_ROADS_H
#define TD5_GEO_ROADS_H

/* OSM highway classes, collapsed and ORDERED BY IMPORTANCE. The order is the
 * contract: td5_tg_network.c sorts contending real streets by it, so when two
 * roads want the same (span, side) the bigger one wins and the build is
 * reproducible. `*_link` ways collapse onto their parent class; anything else
 * OSM offered lands on UNKNOWN. */
#define TD5_GEO_RC_UNKNOWN       0
#define TD5_GEO_RC_SERVICE       1
#define TD5_GEO_RC_LIVING        2
#define TD5_GEO_RC_RESIDENTIAL   3
#define TD5_GEO_RC_UNCLASSIFIED  4
#define TD5_GEO_RC_TERTIARY      5
#define TD5_GEO_RC_SECONDARY     6
#define TD5_GEO_RC_PRIMARY       7
#define TD5_GEO_RC_TRUNK         8
#define TD5_GEO_RC_MOTORWAY      9

/* OSM `surface=*`, collapsed to what the road pages can depict.
 *
 * The tag is on 2238 of La Plata's 2291 drivable ways and was being DROPPED by
 * this reader (the module header used to say so: "surface ... deliberately
 * skipped"), even though geo_fetch had always written it into ROADS.JSON --
 * gate 4 of docs/plans/GEO_TAG_AUDIT.md. La Plata's distribution: asphalt 999,
 * concrete 690, untagged 299, paved 194, sett 87, dirt 10, paving_stones 5,
 * unpaved 3, gravel 3, cobblestone 1. So the interesting minority is 104 ways,
 * which is small, real, and exactly the kind of thing that makes a real place
 * read as a real place.
 *
 * SMOOTH is the default and the no-op: every smooth value maps to it and a
 * SMOOTH street takes the span's own biome page, which is what every street
 * did before. Only LOOSE and COBBLE change anything. */
#define TD5_GEO_SURF_SMOOTH  0   /* asphalt, concrete, paved, paving_stones */
#define TD5_GEO_SURF_COBBLE  1   /* sett, cobblestone, unhewn_cobblestone   */
#define TD5_GEO_SURF_LOOSE   2   /* dirt, unpaved, gravel, ground, sand     */

typedef struct {
    int    first, count;   /* slice of the shared point pool                  */
    int    lanes;          /* 1..TD5_GEO_ROADS_LANES_MAX (see the note below) */
    int    klass;          /* TD5_GEO_RC_*                                    */
    int    oneway;
    int    oneway_dir;     /* +1 along the way, -1 against it, 0 two-way      */
    int    surface;        /* TD5_GEO_SURF_*                                  */
    int    roundabout;     /* OSM junction=roundabout / circular              */
    int    bridge, tunnel; /* OSM bridge=* / tunnel=*                         */
    int    layer;          /* OSM layer=*, 0 at grade                         */
    double width;          /* world units: lanes * TD5_TG_LANE_WIDTH          */
    double minx, minz, maxx, maxz;   /* bbox, so a caller can reject cheaply  */
} TD5_GeoRoad;

/* A street's frontage run is measured in SPANS, and the engine has
 * TD5_TG_SPAN_LENGTH == TD5_TG_LANE_WIDTH, so a lane count IS a span count.
 * Capped because the run has to fit inside the facade a mouth opens; a wider
 * real road is clamped, never dropped. */
#define TD5_GEO_ROADS_LANES_MAX 12

/* Make the loaded road graph match `slug` (NULL or "" unloads). Returns 1 when
 * roads for that slug are loaded. Reloads only when the slug actually changes,
 * so calling it once per build is cheap. */
int  td5_geo_roads_sync(const char *slug);
void td5_geo_roads_unload(void);

int  td5_geo_roads_count(void);
int  td5_geo_roads_points(void);            /* pool size, for the census log */
const char *td5_geo_roads_source(void);     /* path it came from, "" if none */

const TD5_GeoRoad *td5_geo_roads_get(int i);
/* Point k of road r in world units. Out-of-range k writes nothing and
 * returns 0, so a walk can use it as its own bound. */
int  td5_geo_roads_point(const TD5_GeoRoad *r, int k, double *x, double *z);

#endif /* TD5_GEO_ROADS_H */
