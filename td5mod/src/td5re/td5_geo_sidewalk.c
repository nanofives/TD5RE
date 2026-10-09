/**
 * td5_geo_sidewalk.c -- GEO TRACK: where a pavement's WIDTH comes from
 *                       (PORT-ONLY). Contract and the five sources in
 *                       td5_geo_sidewalk.h.
 *
 * This file is pure: a width out of a TD5_GeoSwIn, plus the per-place building
 * line table. No geometry, no node list, no file IO, no RNG.
 */
#include <stdio.h>
#include <string.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"          /* td5_env_flag_on / _off */
#include "td5_geo_roads.h"       /* TD5_GEO_RC_*, the class default table */
#include "td5_geo_footways.h"    /* source 2, owned by round 1011 C4       */
#include "td5_geo_sidewalk.h"

#define LOG_TAG "geo"

/* Source 2 (a mapped footway=sidewalk line) is td5_geo_footway_sidewalk_near,
 * defined in td5_geo_footways.c (round 1011 C4). */

/* ------------------------------------------------- the building-line table --- */

/* THE BUILDING LINE, METRES, per highway class: the street's full reserved
 * width from building face to building face, pavements included.
 *
 * WHY A PLACE NEEDS ITS OWN ROW. A building line is a planning decision, not a
 * property of the road, and a planned city states it as a number. La Plata was
 * laid out in 1882 on a strict grid: its calles are reserved at 18 m and its
 * avenidas -- every sixth street -- and its diagonales at 30 m. With a calle's
 * carriageway tagged at 10 m that is (18/2) - 5 = 4 m of vereda a side, which
 * is what the city actually has and roughly double the 2.0 m the highway-class
 * default hands a `residential` way.
 *
 * HOW THE CLASS STANDS IN FOR THE NAME. The rule is really "calle vs avenida /
 * diagonal", which is a NAME prefix. La Plata's OSM classes track that split
 * closely -- the avenidas and diagonales carry secondary/primary, the calles
 * residential -- so the class is the honest proxy and needs no name reader. It
 * is a proxy and not the thing itself: a calle that OSM promoted to tertiary
 * for traffic reasons will take the 30 m row. Refining this with the street
 * name is the obvious next step and belongs with the name reader (round 1011
 * C3), not here.
 *
 * A DIVIDED AVENUE does not reach the 30 m row in practice: td5_tg_avenue.c
 * owns the span's inner edge and tg_pavement_side_width returns 0 there, so the
 * pavement outside the opposite carriageway is still not laid by this path.
 * That is stated at td5_tg_city.c (the ROUND 1010 AVENUES block) and is
 * unchanged by this round.
 *
 * 0 = "no rule for this class here", which turns source 4 off and lets the
 * class default answer. MOTORWAY is 0 everywhere on purpose: a motorway has no
 * building line and no pedestrian kerb. */
typedef struct {
    const char *slug;                        /* "" = the generic fallback */
    double      line_m[TD5_GEO_RC_MOTORWAY + 1];
    /* [1011 C2] BY NAME, which is what the plan actually states, indexed by
     * TD5_GEO_NAMEK_*. A non-zero entry BEATS the class row above: the class is
     * only a proxy for the name, and on La Plata's own route it gets "Calle 14"
     * (primary) and "Calle 50" (tertiary) wrong. 0 = no rule for that kind
     * here, which falls back to the class. */
    double      name_m[TD5_GEO_NAMEK_PLAZA + 1];
    /* [1011 C2] The REAL CARRIAGEWAY by street kind, METRES -- see
     * td5_geo_sw_carriageway_m. Same indexing as name_m; 0 = no rule. */
    double      carriage_m[TD5_GEO_NAMEK_PLAZA + 1];
} GeoSwPlaceRule;

/* Index by TD5_GEO_RC_*: UNKNOWN, SERVICE, LIVING, RESIDENTIAL, UNCLASSIFIED,
 * TERTIARY, SECONDARY, PRIMARY, TRUNK, MOTORWAY. */
static const GeoSwPlaceRule k_place_rules[] = {
    /* La Plata, 1882 plan: calle 18 m, avenida / diagonal 30 m. A service way
     * is an alley inside a block and has no reserved line, so it keeps the
     * class default. */
    { "la_plata",
      { 18.0,  0.0, 18.0, 18.0, 18.0, 30.0, 30.0, 30.0, 30.0,  0.0 },
      /* UNKNOWN, CALLE, AVENIDA, DIAGONAL, PLAZA -- the 1882 plan, stated by
       * name. A plaza states no building line of its own: the ring road round
       * it is a primary/secondary way, which takes the 30 m class row. */
      {  0.0, 18.0, 30.0, 30.0,  0.0 },
      /* Carriageway: 10 m, which is what 34 of the 37 measured calles say.
       * The avenidas and diagonales take the same per-carriageway figure --
       * a divided one is then 10 + median + 10, which is the 30 m line with
       * a 10 m median, and that is the cross-section the city has. */
      /* [1014 B items 11, 14] ... and a PLAZA's ring road takes it too. OSM
       * counts the ring's `lanes` as 2 (a 7 m road), narrower than every calle
       * that feeds it -- "the road bordering the whole plaza should be wider". */
      {  0.0, 10.0, 10.0, 10.0, 10.0 } },
    /* THE GENERIC ROW, for every other place. Deliberately a European/Latin
     * American town centre rather than a second La Plata: narrower on the small
     * classes, and it only ever has to beat the class default, which is the
     * thing it replaces. A place that wants its real plan adds a row above. */
    { "",
      { 16.0,  0.0, 14.0, 16.0, 18.0, 20.0, 25.0, 28.0, 30.0,  0.0 },
      /* No name rule generically: outside a planned grid a street called
       * "Avenue" carries no reserved width, so the class is the better guide.
       * And no carriageway rule either -- without a plan to point at, the
       * `lanes` field is the honest answer and the route keeps using it. */
      {  0.0,  0.0,  0.0,  0.0,  0.0 },
      {  0.0,  0.0,  0.0,  0.0,  0.0 } },
};

static const GeoSwPlaceRule *s_rule;
static char s_slug[64];

void td5_geo_sw_place(const char *slug)
{
    int i;
    const int n = (int)(sizeof k_place_rules / sizeof k_place_rules[0]);
    if (!slug) slug = "";
    snprintf(s_slug, sizeof s_slug, "%s", slug);
    s_rule = &k_place_rules[n - 1];               /* the generic row */
    for (i = 0; i < n; i++) {
        if (k_place_rules[i].slug[0] && !strcmp(k_place_rules[i].slug, slug)) {
            s_rule = &k_place_rules[i];
            break;
        }
    }
    TD5_LOG_I(LOG_TAG, "geo: sidewalk building-line table for '%s': %s row "
              "(calle-class %.0f m, avenue-class %.0f m)",
              s_slug, s_rule->slug[0] ? s_rule->slug : "generic",
              s_rule->line_m[TD5_GEO_RC_RESIDENTIAL],
              s_rule->line_m[TD5_GEO_RC_SECONDARY]);
}

const char *td5_geo_sw_place_name(void) { return s_slug; }

double td5_geo_sw_building_line_m(int klass)
{
    if (!s_rule) td5_geo_sw_place("");
    if (klass < 0 || klass > TD5_GEO_RC_MOTORWAY) return 0.0;
    return s_rule->line_m[klass];
}

double td5_geo_sw_carriageway_m(int klass, int namek)
{
    (void)klass;                 /* no class row today: the plan states a KIND */
    if (!s_rule) td5_geo_sw_place("");
    if (namek > TD5_GEO_NAMEK_UNKNOWN && namek <= TD5_GEO_NAMEK_PLAZA)
        return s_rule->carriage_m[namek];
    return 0.0;
}

double td5_geo_sw_building_line_for(int klass, int namek)
{
    if (!s_rule) td5_geo_sw_place("");
    if (namek > TD5_GEO_NAMEK_UNKNOWN && namek <= TD5_GEO_NAMEK_PLAZA) {
        const double m = s_rule->name_m[namek];
        if (m > 0.0) return m;        /* the plan states it by name: use that */
    }
    return td5_geo_sw_building_line_m(klass);
}

/* ------------------------------------------------------------ the sources --- */

const char *td5_geo_sw_source_name(int src)
{
    switch (src) {
    case TD5_GEO_SWSRC_TAG:      return "tag";
    case TD5_GEO_SWSRC_FOOTWAY:  return "footway";
    case TD5_GEO_SWSRC_FACADE:   return "facade";
    case TD5_GEO_SWSRC_FRONTAGE: return "frontage-rule";
    case TD5_GEO_SWSRC_CLASS:    return "class-default";
    case TD5_GEO_SWSRC_KERB:     return "kerb (sidewalk=no)";
    default:                     return "none";
    }
}

/* A width this module will believe, METRES. The clamp exists because three of
 * the five sources are DERIVED (a subtraction against a carriageway that may
 * itself be a class guess), and a subtraction can go negative or absurd on one
 * bad way without the result being obviously wrong anywhere else. Converting to
 * world units and clamping again in the caller is the second net. */
#define GEO_SW_SANE_MIN 0.3
#define GEO_SW_SANE_MAX 12.0

static int geo_sw_sane(double m) { return m > GEO_SW_SANE_MIN && m < GEO_SW_SANE_MAX; }

/* THE FRONTAGE RULE'S OWN CEILING, METRES, and why it is not the one above.
 *
 * A measurement may legitimately be wide -- if OSM says the pavement is 9 m,
 * it is 9 m. The RULE is different: it is a SUBTRACTION of one number from
 * another, and when the two describe different cross-sections the difference
 * is not a pavement, it is the error between them. Measured on La Plata, round
 * 1011: the avenue classes take the 30 m building line, which in this city
 * describes a DIVIDED avenue -- two carriageways and a median -- while the rule
 * subtracts the ONE 7 m carriageway the generator built, and hands back 11.5 m
 * a side. 652 primary + 170 secondary + 183 tertiary spans came out that way
 * and every one of them was then clamped to the engine's 6 m ceiling.
 *
 * Clamping is the wrong answer twice over: it reports `frontage-rule` for a
 * number the rule did not produce, and it lays a 6 m slab on a street that has
 * no such thing. So the rule DECLINES above this figure instead, and the class
 * default -- round 1009's answer, and a safe one -- takes the span.
 *
 * The ceiling is the engine's own clamp (TD5_TG_GEO_SW_MAX, 6 m) on purpose:
 * "if the rule's answer would have to be clamped, the rule is wrong here" is a
 * self-consistent test, where any other figure would be a second arbitrary one.
 *
 * NOT DONE, and this is where it would go: modelling the divided cross-section
 * properly. td5_geo_avenues.c already knows the offset to the opposite
 * carriageway and its lane count, so (line - near - median - far) / 2 is
 * computable. Until then a divided span declines on the same rule as any other
 * over-wide one, and `divided` below makes that decline explicit rather than
 * incidental. */
#define GEO_SW_FRONTAGE_MAX_M 6.0

double td5_geo_sw_resolve(const TD5_GeoSwIn *in, int *src_out)
{
    int src = TD5_GEO_SWSRC_NONE;
    double w = 0.0;

    if (!in) {
        if (src_out) *src_out = TD5_GEO_SWSRC_NONE;
        return 0.0;
    }

    /* PRESENCE FIRST, and it is not a width question. `sidewalk=no` on this
     * side is a mapper stating there is no footway, so no measurement and no
     * rule applies -- but the slab does not go to zero, because the kerb face,
     * the facade and the lamp posts stand on it. */
    if (!in->present) {
        if (src_out) *src_out = TD5_GEO_SWSRC_KERB;
        return TD5_GEO_SW_KERB_M;
    }

    /* 1. THE TAG. A measurement of the pavement itself: nothing beats it. */
    if (geo_sw_sane(in->tag_m)) {
        w = in->tag_m;
        src = TD5_GEO_SWSRC_TAG;
    }

    /* 2. THE MAPPED FOOTWAY.
     *
     * FOOTWAY HOOK -- deliberately live and deliberately unfed. `footway_m` is
     * the offset from the road centreline to a separately mapped
     * highway=footway running beside it, so the pavement width is that offset
     * minus half the carriageway: two real polylines, hence a measurement, and
     * hence above the facade. Round 1011 C4 owns getting footway geometry as
     * far as a reader; until it lands every caller passes 0 and this branch
     * never fires. When it does land, the ONLY change needed is the caller
     * filling this field -- the priority slot, the sanity clamp and the census
     * bucket are already here and already reported. */
    if (src == TD5_GEO_SWSRC_NONE && in->footway_m > 0.0) {
        /* The caller has already folded C4's two outputs into one reach --
         * dist + (width > 0 ? width/2 : 0) -- because `dist` reaches the
         * footway's CENTRELINE, not its near edge. half_road_m, not
         * half_carriage_m: the query is anchored on the span node, so the reach
         * is measured from the GENERATED centreline and the carriageway it has
         * to clear is the one the generator built. */
        const double m = in->footway_m - in->half_road_m;
        if (geo_sw_sane(m)) {
            w = m;
            src = TD5_GEO_SWSRC_FOOTWAY;
        }
    }

    /* 3. THE MEASURED FACADE. Real geometry, but only where the caller has
     * established that the facade IS the frontage -- a building set back behind
     * a forecourt, or one that happens to sit near a bend, measures a distance
     * that is not a pavement. `facade_ok` is that judgement; this module does
     * not second-guess it, it only subtracts the carriageway. */
    if (src == TD5_GEO_SWSRC_NONE && in->facade_ok && in->facade_m > 0.0) {
        /* half_road_m, not half_carriage_m: the probe marched out from the
         * GENERATED centreline, so the carriageway it has to clear is the one
         * the generator built. */
        const double m = in->facade_m - in->half_road_m;
        if (geo_sw_sane(m)) {
            w = m;
            src = TD5_GEO_SWSRC_FACADE;
        }
    }

    /* 4. THE BUILDING-LINE RULE. Half the street's reserved width, less half
     * the carriageway, is what is left for the pavement -- the arithmetic the
     * plan itself implies. Behind its own knob so the rule can be taken out of
     * the stack and the three measurements above tested on their own. */
    if (src == TD5_GEO_SWSRC_NONE
        && td5_env_flag_on("TD5RE_GEO_SW_FRONTAGE")) {
        const double line = td5_geo_sw_building_line_for(in->klass, in->namek);
        if (line > 0.0) {
            /* half_road_m, not OSM's: the line is placed against the road the
             * generator BUILT, so road + 2 x pavement == the building line
             * exactly and the facade cannot land anywhere but on the back edge
             * of the slab.
             *
             * A DIVIDED AVENUE SPENDS THE LINE ON FOUR THINGS, not two. Its
             * 30 m in La Plata covers pavement + near carriageway + median +
             * far carriageway + pavement, and subtracting only the near one is
             * what made this rule hand back 11.5 m a side. With the sidecar's
             * offset and far width the whole section is known, so the line can
             * be spent properly -- see TD5_GeoSwIn for the derivation.
             *
             * MEASURED on La Plata's 349 divided spans: |off| runs 7.4 to
             * 15.1 m with a median of 14.2, and the far carriageway is 2 lanes
             * on every one of them, which puts the pavement at roughly 3.1 to
             * 4.4 m. A span whose carriageways nearly touch (|off| 7.4 m, a
             * NEGATIVE median) produces an over-wide answer and declines on the
             * ceiling below, which is the right outcome for a cross-section
             * that does not physically fit. */
            const double m = (in->divided && in->av_reach_m > 0.0)
                           ? (line - in->av_reach_m - in->half_road_m) * 0.5
                           : line * 0.5 - in->half_road_m;
            if (geo_sw_sane(m) && m <= GEO_SW_FRONTAGE_MAX_M)
                { w = m; src = TD5_GEO_SWSRC_FRONTAGE; }
        }
    }

    /* 5. THE HIGHWAY CLASS DEFAULT. Round 1009's answer, kept as the floor: a
     * span none of the four above can speak for comes out exactly as it does
     * today rather than coming out zero. */
    if (src == TD5_GEO_SWSRC_NONE) {
        const double m = td5_geo_roads_pavement_default_m(in->klass);
        if (m > 0.0) {
            w = m;
            src = TD5_GEO_SWSRC_CLASS;
        }
    }

    if (src_out) *src_out = src;
    return (w > 0.0) ? w : 0.0;
}
