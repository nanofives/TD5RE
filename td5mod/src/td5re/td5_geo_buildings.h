/**
 * td5_geo_buildings.h -- GEO TRACK: real OSM building footprints and area
 *                        polygons for the auto-track (PORT-ONLY).
 *                        See docs/plans/GEO_TRACK_OSM_PLAN.md section 7 phase 5.
 *
 * Reads the two VECTOR files of the place cache re/tools/geo_fetch.py writes:
 *
 *   re/assets/geo/<slug>/BUILDINGS.JSON  footprint rings in world units, plus
 *                                        height_m and WHICH TAG it came from
 *                                        (osm_height / osm_levels / estimated),
 *                                        roof_shape, building:part, landmark
 *   re/assets/geo/<slug>/AREAS.JSON      plaza / park / pitch / grass polygons
 *
 * WHY A SEPARATE MODULE, not more of td5_geo.c. td5_geo.c is the RASTER +
 * ROUTE reader that phases 2 and 3 shipped, and four phase-5 workstreams run in
 * parallel over the same tree. Keeping the vector readers here means the
 * building/plaza work touches no file another workstream owns, exactly as
 * _archive/GEO_PHASE5_COMMON.md requires.
 *
 * BYTE-IDENTITY. Nothing here is reachable unless a place is loaded
 * (td5_geo_loaded()), so a synthetic build never calls it. It draws from NO
 * random stream -- the standing rule at td5_trackgen_internal.h:1290-1296 is
 * that one extra tg_rand call moves the road for every existing seed -- and
 * where a per-building choice is needed (which facade page a wall wears) it is
 * a hash of the OSM way id, which is a pure function of the data.
 *
 * FRAME. Ring coordinates are raw signed world units in the SAME frame as
 * TG_Node, because geo_fetch builds the vectors in the frame the conditioner
 * chose for ROUTE.JSON. Heights arrive in METRES and are converted here with
 * the cache's own projection.units_per_metre, so no caller does unit maths.
 *
 * HOST BINDING. A footprint is emitted by the span it stands beside, so every
 * polygon is bound once -- AT LOAD TIME -- to its nearest node of the
 * CONDITIONED ROUTE (td5_geo_route_node), with a side and a lateral distance.
 * Two reasons it is the route and not TG_Node:
 *
 *  - On the geo path the conditioned route IS the centreline: phase 3 stubbed
 *    the section loop of tg_build_centerline to walk ROUTE.JSON, so node i sits
 *    at route point i. The emitter asserts that rather than assuming it, and
 *    logs the worst deviation it finds.
 *  - Binding at load time is SINGLE-THREADED. The scenery loop is not -- a
 *    streamed build runs entries in a worker (td5_trackgen_stream.c) -- so a
 *    lazy first-call bind inside an emitter would be a race over a table that
 *    has to allocate. Nothing here allocates once a place is loaded.
 */
#ifndef TD5_GEO_BUILDINGS_H
#define TD5_GEO_BUILDINGS_H

/* Where a building's height came from. The plan's section 9 requires every
 * building's provenance to be recoverable, so a bad skyline can be attributed
 * to the estimator instead of hunted in the emitter. */
#define TD5_GEOB_HSRC_ESTIMATED  0   /* geo_fetch's area/class estimator */
#define TD5_GEOB_HSRC_OSM_LEVELS 1   /* building:levels x storey height  */
#define TD5_GEOB_HSRC_OSM_HEIGHT 2   /* a real height=* tag, measured    */

/* roof:shape, collapsed to the silhouettes the emitter can build.
 *
 * WIDENED 2026-09-30. The first cut folded gabled, hipped, pyramidal, round,
 * skillion and dome into ONE class and built a centroid pyramid for all of
 * them, which is a tent on every rectangular house and a tent on every shed.
 * The emitter now builds a RIDGE (td5_tg_city.c tg_geo_roof_ridge), and a
 * ridge of length 0 IS the pyramid, so the four cases below are one code path
 * with different ridge endpoints -- no new geometry kind, just a parameter.
 * Anything OSM tags that is not in this list still lands on APEX, which is
 * the safe silhouette for a dome, an onion or a cone. */
#define TD5_GEOB_ROOF_NONE     0     /* untagged -- emit a flat cap only    */
#define TD5_GEOB_ROOF_FLAT     1
#define TD5_GEOB_ROOF_APEX     2     /* pyramidal / dome / onion / round    */
#define TD5_GEOB_ROOF_MANSARD  3     /* mansard / gambrel -- truncated      */
#define TD5_GEOB_ROOF_GABLED   4     /* gabled / half-hipped -- full ridge  */
#define TD5_GEOB_ROOF_HIPPED   5     /* hipped -- ridge inset at both ends  */
#define TD5_GEOB_ROOF_SKILLION 6     /* one slope, high edge at the far end */

/* AREAS.JSON `kind`, collapsed to what the plaza emitter distinguishes. */
#define TD5_GEOA_KIND_OTHER   0
#define TD5_GEOA_KIND_PARK    1      /* leisure=park, village_green, common */
#define TD5_GEOA_KIND_GRASS   2      /* landuse=grass */
#define TD5_GEOA_KIND_PITCH   3      /* leisure=pitch -- a flat playing field */
#define TD5_GEOA_KIND_PLAY    4      /* leisure=playground */
#define TD5_GEOA_KIND_FOREST  5

/* WHICH TAG MADE A FOOTPRINT A LANDMARK.
 *
 * The 2026-09-30 census could say "22 landmarks" and nothing more, which is
 * exactly as informative as "22 hostels" -- and the old blanket
 * `tourism or historic` really had promoted a boutique hostel. geo_fetch now
 * writes `landmark_src` as "key=value" (docs/plans/GEO_TAG_AUDIT.md) and these
 * are its KEYS, so the build log can break the count down by deciding tag.
 *
 * FLAG is the pre-2026-10-07 case: a cache at `tag_schema` 1 carries the bool
 * and no reason, and saying so beats inventing one. */
#define TD5_GEOB_LMSRC_NONE       0
#define TD5_GEOB_LMSRC_FLAG       1   /* the cache's own bool, reason unknown */
#define TD5_GEOB_LMSRC_BUILDING   2
#define TD5_GEOB_LMSRC_GOVERNMENT 3
#define TD5_GEOB_LMSRC_OFFICE     4
#define TD5_GEOB_LMSRC_AMENITY    5
#define TD5_GEOB_LMSRC_HISTORIC   6
#define TD5_GEOB_LMSRC_HERITAGE   7
#define TD5_GEOB_LMSRC_MANMADE    8
#define TD5_GEOB_LMSRC_TOURISM    9
#define TD5_GEOB_LMSRC_COUNT     10

/* Human name of a TD5_GEOB_LMSRC_*, for the log. Never NULL. */
const char *td5_geob_lmsrc_name(int src);

/* Ring points kept per polygon. OSM rings run to 182 points here (the La Plata
 * cathedral); past this the ring is DECIMATED, never truncated, so the
 * silhouette survives and only its detail is lost. */
#define TD5_GEOB_RING_MAX 48

typedef struct {
    int    first, n;          /* [first, first+n) in the shared point pool */
    double cx, cz;            /* centroid, world units                     */
    double radius;            /* max vertex distance from the centroid     */
    double height;            /* world units, TOP of the mass above ground */
    double min_height;        /* world units, BASE of the mass, 0 = ground */
    double roof_height;       /* world units, 0 when roof:height is absent */
    double area_m2;           /* as OSM measures it, for the census        */
    unsigned int id_hash;     /* stable hash of the way id (page picks)    */
    int    host_span;         /* nearest centreline node, -1 unbound       */
    int    host_side;         /* +1 left of travel, -1 right, 0 unbound    */
    double host_lat;          /* |lateral| from the centreline, world units */
    unsigned char hsrc;       /* TD5_GEOB_HSRC_*                           */
    unsigned char roof;       /* TD5_GEOB_ROOF_*                           */
    unsigned char landmark;   /* OSM says this is a named landmark         */
    unsigned char lmsrc;      /* TD5_GEOB_LMSRC_* -- which tag decided it  */
    unsigned char part;       /* building:part -- a 3D-modelled sub-volume */
    /* [ROUND 1009 item 9] STOREY COUNTS, which the reader never kept.
     * BUILDINGS.JSON carries `levels` on every record (286 of La Plata's 2326
     * footprints have it from OSM, the rest from the estimator's `levels_est`)
     * plus `roof_levels` and `min_level`. They matter because the facade page's
     * own floor height is 2058 raw (4.79 m at 430 units/m) while OSM's storey
     * is 3 m: without the real count the emitter has to infer floors from the
     * page, which is what quantised every measured height. 0 = absent. */
    unsigned short levels;      /* building:levels, or the estimator's guess */
    unsigned short roof_levels; /* roof:levels                               */
    unsigned short min_level;   /* building:min_level                        */
} TD5_GeoBuilding;

/* An area's own BARRIER, straight off AREAS.JSON's `barrier` field.
 * MEASURED on the La Plata cache: 414 of 417 areas carry NOTHING, 2 a fence and
 * 1 a wall -- so an emitter that hedges every plaza is inventing 414 walls. */
#define TD5_GEOA_BARRIER_NONE  0
#define TD5_GEOA_BARRIER_HEDGE 1
#define TD5_GEOA_BARRIER_FENCE 2
#define TD5_GEOA_BARRIER_WALL  3

typedef struct {
    int    first, n;
    double cx, cz;
    double radius;
    unsigned int id_hash;
    int    host_span;
    int    host_side;
    double host_lat;
    unsigned char kind;       /* TD5_GEOA_KIND_* */
    unsigned char named;
    unsigned char barrier;    /* TD5_GEOA_BARRIER_* -- what OSM fences it with */
} TD5_GeoArea;

/* ------------------------------------------------------------- lifecycle --- */

/* Load the vectors for whatever place td5_geo.c currently has loaded, or drop
 * them when it has none. Cheap and idempotent: a no-op once the slug matches.
 * Returns 1 when polygons are available afterwards. */
int  td5_geob_sync(void);
void td5_geob_unload(void);
int  td5_geob_loaded(void);

/* ---------------------------------------------------------------- access --- */

int  td5_geob_building_count(void);
int  td5_geob_area_count(void);
const TD5_GeoBuilding *td5_geob_building(int i);
const TD5_GeoArea     *td5_geob_area(int i);

/* Which AREAS.JSON kinds become a plaza. FOREST and the landuse classes that
 * describe a whole residential or retail BLOCK are not squares and get nothing
 * -- laying a lawn over a mapped residential block would carpet the city.
 * Lives here rather than in the emitter because both the emitter and the
 * procedural stand-down gates must agree on it. */
int  td5_geob_area_is_plaza(const TD5_GeoArea *a);

/* Does ANY of `np` world points land inside a real polygon bound within `win`
 * spans of `span`? THE stand-down test: the procedural frontage / back row
 * hands over the points its own mass would occupy instead of guessing from a
 * centroid and a radius.
 *
 * A POINT SET rather than one point, because a procedural element occupies a
 * BAND of depth, not a plane, and the caller must be able to ask "would my mass
 * intersect real geometry anywhere across it" in ONE window walk. MEASURED
 * consequence of asking about a single mid-depth point: Plaza Mariano Moreno's
 * outline is 8.4 m off the route centreline while the street wall's mass runs
 * 4.3 to 11.3 m, so a probe at 7.8 m missed the plaza by 60 cm and the wall
 * stayed standing in front of it in the in-race framedump.
 *
 * The window exists because a polygon is bound to ONE span but occupies many:
 * that same plaza has a 183 m radius, 52 spans of frontage either side of the
 * span it is filed under. TD5_GEOB_WIN_A is sized for that; TD5_GEOB_WIN_B for
 * the largest footprint in the cache (31000 m2). */
#define TD5_GEOB_WIN_B 24
#define TD5_GEOB_WIN_A 72
int  td5_geob_points_in_building(int span, const double *px, const double *pz,
                                 int np, int win);
int  td5_geob_points_in_plaza(int span, const double *px, const double *pz,
                              int np, int win);

/* One ring vertex. `first` comes from the record; k is 0..n-1. The ring is
 * OPEN (the closing repeat of the source file is dropped) and wound
 * counter-clockwise in the world's XZ plane. */
void td5_geob_ring(int first, int k, double *x, double *z);

/* ------------------------------------------------------------- host bind --- */

/* How far off the route a polygon may stand and still be bound to a span, in
 * WORLD UNITS measured to the nearest ring vertex.
 *
 * Buildings 100 m: the frontage band is only 14 m (TD5_TG_GEO_FRONT_BAND), so
 * everything between the two is a real BACK-ROW mass standing where the
 * procedural receding block would otherwise be -- which is the better trade,
 * and it doubles the real content on the La Plata route (MEASURED: 23
 * footprints within 40 m, 54 within 100 m, and 1924 of the 2047 are more than
 * 500 m away, so widening further buys nothing).
 *
 * Areas 60 m: a designed plaza is a whole city block and its far edge is still
 * part of the square the driver sees. */
#define TD5_GEOB_BIND_MAX_B 43000.0
#define TD5_GEOB_BIND_MAX_A 25800.0

/* Per-span iteration over the bound polygons: `span` then `next` until -1.
 * Buildings and areas keep separate chains, each in ascending record order so
 * the emitted mesh sequence is a pure function of the cache file. A span with
 * nothing returns -1 immediately. */
int  td5_geob_span_building(int span);
int  td5_geob_next_building(int i);
int  td5_geob_span_area(int span);
int  td5_geob_next_area(int i);

/* How many spans the chains cover (the route's node count), and how many
 * polygons fell outside TD5_GEOB_BIND_MAX_* and are therefore never emitted. */
int  td5_geob_bound_spans(void);
void td5_geob_bind_stats(int *buildings_bound, int *buildings_far,
                         int *areas_bound, int *areas_far);

/* ------------------------------------------------------------- geometry ---- */

/* Fan-free triangulation of an arbitrary simple polygon by ear clipping, so a
 * CONCAVE footprint (an L-shaped block, the cathedral's 182-point outline)
 * gets a roof that stays inside its own walls -- a centroid fan does not.
 * Writes `ntri` index triples into tri[] and returns ntri, or 0 when the ring
 * is degenerate. Indices are 0..n-1 into the ring. */
int  td5_geob_triangulate(const double *x, const double *z, int n,
                          int *tri, int maxtri);

/* Signed area of a ring in world units squared; positive when CCW. */
double td5_geob_ring_area(const double *x, const double *z, int n);

/* Is (px,pz) inside the ring? Crossing-number test, boundary unspecified. */
int  td5_geob_point_in_ring(const double *x, const double *z, int n,
                            double px, double pz);

/* Is the ring SIMPLE -- no two non-adjacent edges crossing, and enough area to
 * be a building at all?
 *
 * The emitters assume a simple polygon everywhere: the ear clipper, the
 * point-in-ring stand-down probe and the roof ridge all return nonsense on a
 * bowtie, and a zero-area ring extrudes as a sheet with no inside. OSM does
 * contain both (a mis-drawn way, a footprint traced as a line). MEASURED on
 * the land_* fixture before this existed: the self-intersecting case emitted a
 * mesh with two roof faces wound against each other and the collinear case a
 * wall sheet with no roof.
 *
 * O(n^2) over at most TD5_GEOB_RING_MAX edges, run once per building at emit
 * time, which is nothing beside the ear clip that follows it. */
int  td5_geob_ring_simple(const double *x, const double *z, int n);

/* ---------------------------------------------------------------- census --- */

/* Counts for the build log, so "measured vs estimated" is a number rather than
 * a claim. Any pointer may be NULL. */
void td5_geob_census(int *buildings, int *measured, int *estimated,
                     int *landmarks, int *roofs, int *areas, int *plazas);

/* Landmarks broken down by the tag that decided them: `out` receives
 * TD5_GEOB_LMSRC_COUNT counts indexed by TD5_GEOB_LMSRC_*. The sum is the
 * `landmarks` the census reports. */
void td5_geob_landmark_sources(int *out, int n);

/* How many ring points the loader decimated away, and how many polygons it
 * had to decimate at all -- the honest cost of TD5_GEOB_RING_MAX. */
void td5_geob_decimation(int *polys, int *points_dropped);

/* [ROUND 1009 item 9] The cache's OWN storey height, in WORLD UNITS
 * (BUILDINGS.JSON `storey_height_m` x PLACE.JSON units_per_metre; 3 m x 430 =
 * 1290 at La Plata). The emitter needs it because the facade PAGE's floor
 * height is a texture property (2058 raw = 4.79 m) and must not be used as the
 * unit a measured height is rounded to. 0 when no place is loaded. */
double td5_geob_storey_units(void);

/* Units per metre of the loaded cache's frame, so a caller can convert a tagged
 * metre value without re-reading PLACE.JSON. 0 when no place is loaded. */
double td5_geob_units_per_m(void);

#endif /* TD5_GEO_BUILDINGS_H */
