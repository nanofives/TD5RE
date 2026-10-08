/**
 * td5_minimap_streets.h -- the SURROUNDING STREET NETWORK the in-race minimap
 *                          draws under the race route (PORT-ONLY).
 *
 * WHY THIS EXISTS. The shipped minimap draws the race route and nothing else,
 * because a shipped TD5 track IS nothing else: the strip is the world. A GEO
 * track is a real place, and a real place has a street grid the route only
 * threads through -- so the map a driver reads in La Plata should show the
 * cross streets they are passing, the way a road map does. Mariano, 2026-10-07:
 * "in race I should be able to see in the minimap the perpendicular streets
 * that are not part of the race."
 *
 * WHY A SEPARATE MODULE. td5_hud.c is already ~8k lines and knows nothing about
 * OSM or the generator. This module owns the one thing the HUD must not: the
 * decision of WHERE the streets come from. The HUD asks for a flat array of
 * world-space segments and draws them; it never learns that a geo track reads
 * ROADS.JSON and the auto track reads an in-memory edge list.
 *
 * FRAME. Segment coordinates are raw signed world units -- the SAME frame the
 * strip's span origins (+0x0C / +0x14) use, so the HUD applies exactly the
 * transform it already applies to a road quad and nothing else. That agreement
 * is not a coincidence and not an assumption: td5_trackgen.c's strip emitter
 * takes each origin-block origin straight from the centerline node
 * (`ox = tg_round(nl->v[s0].x)`), and on a geo build tg_geo_walk pushes the
 * ROUTE.JSON node coordinates as those nodes unchanged, while geo_fetch writes
 * ROADS.JSON in that same route frame (see td5_geo_roads.h, "FRAME").
 *
 * SHIPPED TRACKS ARE UNTOUCHED. The only two sources are the geo road graph and
 * the generator's street network, and both answer "nothing" off a generated
 * track, so a shipped track gets an empty array and the faithful minimap. The
 * module also holds no RNG and runs only when the HUD asks, so no build -- geo
 * or synthetic -- can observe it.
 */
#ifndef TD5_MINIMAP_STREETS_H
#define TD5_MINIMAP_STREETS_H

/* Draw weight, in importance order. The HUD turns this into a line width and a
 * colour; the two source vocabularies (OSM highway classes, TG_NE_* edge kinds)
 * are collapsed onto it HERE so the HUD has one scale to style. */
#define TD5_MMS_RANK_MINOR  0   /* service, back street, country lane        */
#define TD5_MMS_RANK_STREET 1   /* residential / unclassified / tertiary     */
#define TD5_MMS_RANK_MAJOR  2   /* secondary and up, avenues                 */
#define TD5_MMS_RANK_COUNT  3

typedef struct {
    float         x0, z0, x1, z1;   /* world units, one polyline chord       */
    float         minx, minz, maxx, maxz;  /* its bbox, so a cull is 4 compares */
    unsigned char rank;             /* TD5_MMS_RANK_*                         */
} TD5_MMStreetSeg;

/* Make the overlay match the track that is loaded now, rebuilding only when the
 * source actually changed (a cheap signature compare otherwise). Safe to call
 * every frame and from every split-screen pane. Returns the segment count.
 *
 * Returns 0 -- and costs one signature compare -- on a shipped track, on a
 * generated track whose street source is unavailable, and when the overlay is
 * switched off with TD5RE_MINIMAP_STREETS=0. */
int td5_mmstreets_sync(void);

int td5_mmstreets_count(void);
/* The segment array, or NULL when empty. Valid until the next sync() that
 * actually rebuilds, i.e. for the rest of the frame. */
const TD5_MMStreetSeg *td5_mmstreets_segs(void);
/* "geo:<slug>", "auto", or "" -- for the log line and for diagnostics. */
const char *td5_mmstreets_source(void);

/* Drop the overlay and force the next sync() to rebuild. For a caller that
 * knows the world changed under it (a regenerate that keeps the same slot). */
void td5_mmstreets_invalidate(void);

#endif /* TD5_MINIMAP_STREETS_H */
