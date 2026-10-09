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
/* OSM `sidewalk=*`, which this reader also used to drop (the module header
 * listed it with `latlon`/`name`/`maxspeed` as "deliberately skipped").
 *
 * MEASURED on the La Plata cache: 187 of 2291 ways carry it -- both 104,
 * right 61, left 8, no 14 -- plus 20 on the per-side spellings
 * (`sidewalk:left` 3, `sidewalk:right` 17). `sidewalk:width` is on NONE of
 * them, so there is no measured pavement width anywhere in this cache and the
 * width itself has to come from the highway class. LEFT/RIGHT are relative to
 * the WAY's own direction, which is why the query below hands the caller the
 * way's local direction instead of pretending to know the route's. */
#define TD5_GEO_SW_UNKNOWN   0   /* no sidewalk tag at all                  */
#define TD5_GEO_SW_NONE      1   /* sidewalk=no / none                      */
#define TD5_GEO_SW_LEFT      2   /* sidewalk=left                           */
#define TD5_GEO_SW_RIGHT     3   /* sidewalk=right                          */
#define TD5_GEO_SW_BOTH      4   /* sidewalk=both / yes / separate          */

/* [ROUND 1011 C2] THE STREET'S OWN KIND, from its NAME.
 *
 * A planned city states its building line per kind of street, and the kind is a
 * NAME prefix -- La Plata reserves 18 m for a `Calle` and 30 m for an `Avenida`
 * or a `Diagonal`. The highway class is only a proxy for that, and this route
 * proves it breaks: "Calle 14" is tagged highway=primary (90 route spans) and
 * "Calle 50" tertiary (43 spans), so both would take the avenue line.
 *
 * ONE INT, NOT THE NAME. The name is classified at LOAD and the string is
 * dropped. The module header is explicit that the long-lived pool holds the
 * minimum -- 8192 ways of name buffer would be half a megabyte to answer a
 * four-way question -- and it also means this needs no name field, so it does
 * not collide with the separate name work in the same round. */
#define TD5_GEO_NAMEK_UNKNOWN 0   /* no name, or none of the below          */
#define TD5_GEO_NAMEK_CALLE   1   /* Calle / Street / Rua / Via             */
#define TD5_GEO_NAMEK_AVENIDA 2   /* Avenida / Av. / Avenue / Boulevard     */
#define TD5_GEO_NAMEK_DIAGONAL 3  /* Diagonal                               */

#define TD5_GEO_SURF_SMOOTH  0   /* asphalt, concrete, paved, paving_stones */
#define TD5_GEO_SURF_COBBLE  1   /* sett, cobblestone, unhewn_cobblestone   */
#define TD5_GEO_SURF_LOOSE   2   /* dirt, unpaved, gravel, ground, sand     */

/* [ROUND 1011 C3] OSM `lit=*`, the highest-count attribute this reader used to
 * drop (1693 of La Plata's 2291 ways carry it).
 *
 * THREE STATES, NOT TWO, and the distinction is the whole point. OSM's `lit=no`
 * is a surveyed statement that a street has NO lighting; an ABSENT tag is a
 * statement about the survey, not about the street. La Plata's cache has 1693
 * `yes` and ZERO `no`, so the NO branch is inert on this place and only the
 * UNKNOWN default is observable here -- see TD5RE_GEO_LIT_DEFAULT in
 * td5_geo_attrs.c for what UNKNOWN does. */
#define TD5_GEO_LIT_UNKNOWN  0   /* no lit tag at all                       */
#define TD5_GEO_LIT_NO       1   /* lit=no / none / disused                 */
#define TD5_GEO_LIT_YES      2   /* lit=yes / 24-7 / automatic / sunset-... */

/* Longest unique name on the La Plata cache is 43 bytes ("Pasaje Profesor
 * Doctor Mario Egidio Teruggi"); 64 leaves room and keeps the interned pool a
 * flat array. A longer name is truncated on a UTF-8 BOUNDARY, never mid
 * sequence -- a dangling continuation byte would render as a replacement box. */
#define TD5_GEO_ROADS_NAME_MAX   64

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
    int    sidewalk;       /* TD5_GEO_SW_*, relative to the WAY's direction   */
    int    namek;          /* TD5_GEO_NAMEK_*, from `name` at load time       */
    int    lit;            /* TD5_GEO_LIT_*                                   */
    int    maxspeed_kph;   /* OSM maxspeed in km/h; 0 = untagged or unlimited */
    int    name_id;        /* interned name index, -1 when the way is unnamed */
    double width;          /* world units: lanes * TD5_TG_LANE_WIDTH          */
    double tag_width_m;    /* OSM width=*, METRES, 0 when untagged            */
    /* [ROUND 1011 C2] the MEASURED per-side pavement width, METRES, 0 when
     * untagged. Relative to the WAY's direction like `sidewalk` above. Filled
     * from sidewalk:left:width / sidewalk:right:width, else the both-sides
     * spellings sidewalk:both:width / sidewalk:width. ZERO ways carry any of
     * the four on the La Plata cache (verified against the raw Overpass bodies:
     * 0 of 17909 elements), so these are inert there by construction -- they
     * exist so a place that DOES measure its pavements gets the measurement
     * instead of the frontage rule. */
    double sw_tag_l_m, sw_tag_r_m;
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

/* [ROUND 1009 item 7] "Sidewalk widths are not being passed to the race."
 *
 * The pavement the race lays came entirely from the biome table
 * (tg_city_sidewalk_w reads TG_Biome.sidewalk), so every street in every mapped
 * place got the same 900-raw slab -- 2.1 m -- whatever OSM said. This is the
 * query that closes the gap: find the drivable way nearest (x, z) and report
 * the pavement width it implies, in METRES, for the way's OWN left and right.
 *
 * WHY METRES AND WHY THE WAY'S OWN SIDES. The caller converts with the cache's
 * units_per_metre, so this module needs no scale; and OSM's left/right are
 * relative to the way's direction, which has no fixed relation to the generated
 * route's direction, so the way's unit direction at the nearest segment comes
 * back too and the caller resolves the sense with one dot product.
 *
 * WHERE THE WIDTH COMES FROM. `sidewalk=*` says WHETHER there is a pavement,
 * never how wide (sidewalk:width is on none of La Plata's 2291 ways), so the
 * width is the highway class's own realistic default and the tag only chooses
 * which sides get it. Nothing is invented: a way with no sidewalk tag gets its
 * class default on both sides, which is the honest reading of an untagged urban
 * street.
 *
 * Returns 1 on a hit within `max_dist` world units, 0 otherwise (caller keeps
 * the biome width). O(ways) with a bbox reject, so it belongs in a prepass. */
int  td5_geo_roads_pavement_at(double x, double z, double max_dist,
                               double *left_m, double *right_m,
                               double *dirx, double *dirz);

/* The per-class pavement default in METRES, exposed so a report can print the
 * table it is actually using instead of restating it. */
double td5_geo_roads_pavement_default_m(int klass);

/* [ROUND 1011 C2] Classify a street NAME into a TD5_GEO_NAMEK_*. Public because
 * the route builder (td5_geo_route.c) has to reach the same verdict as this
 * reader when it sizes the carriageway, and two copies of a prefix table is how
 * they would quietly disagree. NULL / unrecognised is UNKNOWN. */
int td5_geo_roads_namek_of(const char *name);

/* [ROUND 1011 C2] THE RAW FACTS AT A POINT, with no width decided.
 *
 * td5_geo_roads_pavement_at above answers with ONE rule: the highway class's
 * default, with `sidewalk` choosing which sides get it. That rule is now the
 * LAST of five (see td5_geo_sidewalk.h), and the ones above it need facts this
 * reader has but that query throws away -- the measured tags, the carriageway,
 * the class. So this is the reader's half: find the nearest way and report what
 * OSM actually says there. Nothing is resolved here; td5_geo_sidewalk.c owns
 * the priority order, and the split is the point -- a reader that decides a
 * width cannot be asked what the data said.
 *
 * Returns 1 on a hit within `max_dist` world units, 0 otherwise (out untouched). */
typedef struct {
    double dirx, dirz;       /* the way's unit direction at the nearest segment */
    double half_carriage_m;  /* half the carriageway, METRES: the `width` tag
                              * when present (OSM defines it as the carriageway),
                              * else lanes * TD5_GEO_ROADS_LANE_M              */
    double tag_l_m, tag_r_m; /* sidewalk:<side>:width, METRES, 0 = untagged,
                              * relative to the WAY's direction                */
    int    klass;            /* TD5_GEO_RC_*                                   */
    int    namek;            /* TD5_GEO_NAMEK_*, the street's kind by name     */
    int    sidewalk;         /* TD5_GEO_SW_*, relative to the WAY's direction  */
    int    lanes;
} TD5_GeoPavementAt;

int  td5_geo_roads_pavement_facts_at(double x, double z, double max_dist,
                                     TD5_GeoPavementAt *out);

/* Metres of carriageway per lane. The figure the pre-1011 surplus branch in
 * td5_geo_roads_pavement_at already used, named rather than repeated. */
#define TD5_GEO_ROADS_LANE_M 3.5

/* ------------------------------------------ [ROUND 1011 C3] lit/speed/name -- */

/* THE INTERNED NAME POOL. OSM splits one street into a way per block -- La
 * Plata's 1937 named ways carry only 187 distinct names -- so the name is held
 * once and every way keeps an index. That makes "same street" a cheap integer
 * compare, which is what a junction blade and the minimap both need: they must
 * say "this cross street is NOT the one I am driving on" without strcmp, and
 * without being fooled by the way split.
 *
 * Both accessors return a pointer into a pool owned by this module, valid until
 * the next td5_geo_roads_sync()/_unload(). "" for an unnamed way or a bad id --
 * never NULL, so a caller can print it without a guard. */
int         td5_geo_roads_name_count(void);
const char *td5_geo_roads_name_by_id(int id);
const char *td5_geo_roads_name(const TD5_GeoRoad *r);

/* THE NEAREST DRIVABLE WAY to (x, z), or NULL past `max_dist` world units.
 *
 * This is the lookup td5_geo_roads_pavement_at always did, lifted out so the
 * lit / maxspeed / name queries cost one search instead of four. Same cost and
 * same tie-break (first way wins), so it belongs in a prepass.
 *
 * `skip_name_id >= 0` rejects every way carrying that interned name. That is
 * how a junction finds the CROSS street: pass the race street's name id and the
 * answer cannot be another block of the road you are already on.
 *
 * `dirx`/`dirz` come back as the way's unit direction at the nearest segment
 * (the same convention as pavement_at), `dist` as the distance in world units.
 * Any of the three may be NULL. */
const TD5_GeoRoad *td5_geo_roads_nearest(double x, double z, double max_dist,
                                         int skip_name_id,
                                         double *dirx, double *dirz,
                                         double *dist);

/* OSM `maxspeed=*` -> km/h, exposed for the ROUTE.JSON writer, which parses the
 * same free text from its own DOM. 0 means "no usable limit": untagged,
 * `none`, or a spelling this does not recognise. Never guesses a number for a
 * way that carries no tag. */
int td5_geo_roads_maxspeed_parse(const char *s);

#endif /* TD5_GEO_ROADS_H */
