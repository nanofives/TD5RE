/**
 * td5_geo_sidewalk.h -- GEO TRACK: where a pavement's WIDTH comes from
 *                       (PORT-ONLY). Implementation in td5_geo_sidewalk.c.
 *
 * [ROUND 1011 C2] "implement per-span, PER-SIDE sidewalk widths".
 *
 * Round 1009 item 7 gave the pavement one width per span: the nearest OSM way's
 * highway class default, with `sidewalk=*` choosing which sides got it and the
 * WIDER of the two sides winning (td5_tg_city.c, the prepass table). That was
 * one source and one number. This module is the other four sources and the
 * order they are asked in, stated once, per span and PER SIDE:
 *
 *   1 TAG       sidewalk:<side>:width / sidewalk:both:width / sidewalk:width.
 *               A measurement. Beats everything. ZERO ways carry it on the La
 *               Plata cache (0 of 17909 raw Overpass elements), so it is inert
 *               there -- supported, not pretended.
 *   2 FOOTWAY   a separately mapped highway=footway beside the road. Also a
 *               measurement: the distance between two real polylines. NOT
 *               LANDED -- the footway geometry is round 1011 C4's half and is
 *               not in ROADS.JSON yet. The input field exists and the priority
 *               slot is live, so landing it is a caller change, not a change
 *               here. See the FOOTWAY HOOK note in the .c.
 *   3 FACADE    a measured real building frontage: the distance from the road
 *               centreline out to the first building polygon, minus half the
 *               carriageway. Real geometry, but only where the facade genuinely
 *               IS the frontage -- the caller decides that (straight, parallel,
 *               consistent along the block) and says so with `facade_ok`.
 *   4 FRONTAGE  the place's BUILDING LINE rule: (building line / 2) - half the
 *               carriageway. La Plata is an 1882 plan town and its building
 *               line is a published number, 18 m on a calle and 30 m on an
 *               avenida or a diagonal. This is a RULE, not a measurement, which
 *               is why it sits below the three above and above the one below.
 *   5 CLASS     the highway class's own default (td5_geo_roads_pavement_default_m).
 *               What round 1009 shipped, kept as the floor so a span the four
 *               sources above cannot answer for comes out exactly as it does
 *               today rather than coming out zero.
 *
 * And then presence: `sidewalk=no` on a side leaves a KERB strip, not nothing.
 * The facades, the kerb face and the lamp posts all stand on that slab.
 *
 * WHY A MODULE AND NOT A FUNCTION IN td5_tg_city.c. The priority order is the
 * whole design and it is worth being able to read it in one place, without the
 * span arithmetic, the biome table and the emitter gates around it. The split
 * also keeps this testable from inputs alone: everything here is a pure
 * function of a TD5_GeoSwIn, with no geometry, no node list and no file IO.
 *
 * BYTE-IDENTITY. Nothing here is reached without a geo place loaded -- the only
 * caller is the geo prepass in td5_tg_city.c, behind its existing `s_geo_city`
 * gate. No tg_rand / tg_frand / tg_range is drawn (the standing rule at
 * td5_trackgen_internal.h:1290-1296), so a synthetic build cannot observe it.
 */
#ifndef TD5_GEO_SIDEWALK_H
#define TD5_GEO_SIDEWALK_H

/* Which of the five answered. Ordered by priority so a census prints in the
 * order the sources are asked, and so `a < b` means "a is the better source". */
#define TD5_GEO_SWSRC_NONE      0   /* no answer at all (no way in range)    */
#define TD5_GEO_SWSRC_TAG       1   /* 1: sidewalk:*:width, a measurement    */
#define TD5_GEO_SWSRC_FOOTWAY   2   /* 2: a mapped footway way (not landed)  */
#define TD5_GEO_SWSRC_FACADE    3   /* 3: a measured real building frontage  */
#define TD5_GEO_SWSRC_FRONTAGE  4   /* 4: the place's building-line rule     */
#define TD5_GEO_SWSRC_CLASS     5   /* 5: the highway class default (1009)   */
#define TD5_GEO_SWSRC_KERB      6   /* sidewalk=no on this side: kerb only   */
#define TD5_GEO_SWSRC__N        7

/* Human name for a log line / the census. Never NULL. */
const char *td5_geo_sw_source_name(int src);

/* A kerb, METRES. What a side tagged `sidewalk=no` gets: narrow, but not zero,
 * because the facade and the lamp posts stand on it. Same figure the round-1009
 * resolver in td5_geo_roads.c uses; stated here because this module is now the
 * one that decides it. */
#define TD5_GEO_SW_KERB_M 0.75

/* Everything the resolution needs, for ONE span-side. All distances METRES.
 * Every field is a fact about the real world or about the caller's confidence
 * in one -- no span indices, no world units, no geometry. */
typedef struct {
    int    klass;            /* TD5_GEO_RC_* of the nearest way              */
    int    present;          /* 0 = sidewalk=no HERE: kerb strip, no pavement */
    double half_carriage_m;  /* half the carriageway (width tag, else lanes)  */
    double tag_m;            /* 1: this side's measured tag, 0 = untagged     */
    double footway_m;        /* 2: mapped footway offset, 0 = none (not landed) */
    double facade_m;         /* 3: centreline -> first building, 0 = unmeasured */
    int    facade_ok;        /* 3: the facade genuinely IS the frontage       */
} TD5_GeoSwIn;

/* Select the per-place building-line table. NULL/"" selects the generic one.
 * Idempotent; call it once per build before resolving. */
void td5_geo_sw_place(const char *slug);

/* The BUILDING LINE for the selected place and this highway class, METRES --
 * the street's full reserved width, kerb line to kerb line INCLUDING both
 * pavements. 0 means the place has no rule for this class, which turns source 4
 * off for it. Exposed so a report can print the table it is actually using. */
double td5_geo_sw_building_line_m(int klass);

/* The slug the table above is currently selected for. Never NULL. */
const char *td5_geo_sw_place_name(void);

/* Resolve ONE span-side. Returns the width in METRES (never negative) and
 * writes the source that won to *src_out when it is non-NULL. A NULL `in`
 * returns 0 with TD5_GEO_SWSRC_NONE. */
double td5_geo_sw_resolve(const TD5_GeoSwIn *in, int *src_out);

#endif /* TD5_GEO_SIDEWALK_H */
