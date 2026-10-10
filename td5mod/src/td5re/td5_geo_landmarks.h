/* td5_geo_landmarks.h -- GEO TRACK: which footprints belong to one LANDMARK
 * (PORT-ONLY). See docs/plans/GEO_LANDMARKS.md for the design this implements.
 *
 * A famous building is rarely ONE OSM way. The La Plata cathedral is the
 * building=cathedral outline (5574 m2, height=20) PLUS 25 building:part ways
 * inside it that carry the actual 3D model: two 110 m pyramidal spires, eight
 * 70 m pinnacles, a 50 m gabled nave. They arrive in BUILDINGS.JSON as 26
 * unrelated footprints, each bound to whatever route span happens to be nearest
 * its own vertices, and until round 1014 each was extruded like any office
 * block: office-window pages, house-roof pages, no sign that the 26 meshes are
 * one building. This module recovers the grouping, once per build, and answers
 * three questions for the emitter:
 *
 *   KIND      what kind of landmark is this footprint (a cluster anchor, or a
 *             part standing inside an anchor)?
 *   VETO      is this footprint a DUPLICATE of the landmark it stands inside (an
 *             Overture/footprint-conflation copy of a building the anchor's own
 *             parts already model)?
 *   HULL      the paved apron around an anchor: its convex hull pushed outward.
 *
 * Pure geo-side: no mesh, no texture page. Which pages a worship cluster wears
 * is the emitter's decision (td5_tg_city.c), so this file never has to be
 * regenerated when the page tables move.
 *
 * Everything here runs once in the single-threaded build prologue
 * (tg_geo_city_build_begin) and is read-only afterwards, so the scenery workers
 * may call the accessors freely.
 */
#ifndef TD5_GEO_LANDMARKS_H
#define TD5_GEO_LANDMARKS_H

#include "td5_geo_buildings.h"

/* Most anchors one place can group. La Plata has 6 landmarks within 100 m of the
 * route and one worship anchor; 32 is generous and a hard stop, not a tuning
 * knob (an overflow is counted and reported, never silent). */
#define TD5_GEOLM_ANCHOR_MAX 32

/* Group the footprints into landmark clusters. Called by td5_geo_buildings.c
 * from geob_bind_all, AFTER each footprint has its own nearest-node bind and
 * BEFORE the per-span chains are built, because it does one thing the bind
 * cannot: it ADOPTS a building:part that stands inside an anchor but was too
 * far from the route to bind itself.
 *
 * WHY ADOPT. A footprint binds only if one of its vertices is within
 * TD5_GEOB_BIND_MAX_B (100 m) of a route node. The La Plata cathedral is 117 m
 * long and its nearest vertex is 46 m from the road, so the outline binds --
 * but 9 of its 25 parts, the ones at the far end, are over 100 m out and were
 * never emitted: the building was drawn with its west half a bare 20 m box. A
 * part inherits its anchor's host span, side and lateral. TD5RE_GEO_LM_ADOPT=0
 * turns the adoption off for an A/B.
 *
 * Returns the number of parts adopted (the caller moves them from "far" to
 * "bound" in its census). TD5RE_GEO_LM_CLUSTER=0 leaves every building
 * ungrouped, i.e. the pre-round-1014 behaviour. */
int  td5_geolm_cluster(TD5_GeoBuilding *b, int nb);

/* Drop the grouping (the footprint set it described is gone). */
void td5_geolm_reset(void);

/* TD5_GEOB_LMK_* of this footprint: its own kind when it is an anchor, the
 * anchor's kind when it is a building:part standing inside one, else NONE. */
int  td5_geolm_kind(const TD5_GeoBuilding *gb);

/* 1 when `gb` is an anchor (a landmark outline that owns a cluster). */
int  td5_geolm_is_anchor(const TD5_GeoBuilding *gb);

/* 1 when `gb` stands inside an anchor but is neither a part nor itself a
 * landmark: a second source's copy of a building the anchor already models. The
 * emitter skips it. */
int  td5_geolm_vetoed(const TD5_GeoBuilding *gb);

/* Parts grouped under anchor `gb` (0 for a non-anchor). */
int  td5_geolm_part_count(const TD5_GeoBuilding *gb);

/* Census, for the [GEO LM] line: anchors found, parts grouped, duplicates
 * vetoed, anchors refused past TD5_GEOLM_ANCHOR_MAX, parts adopted. */
void td5_geolm_stats(int *anchors, int *parts, int *vetoed, int *overflow,
                     int *adopted);

/* The apron: convex hull of the ring (rx,rz)[n], every hull edge pushed outward
 * by `margin` world units (mitred corners), written to
 * (ox,oz)[*on]. `omax` is the capacity. Convex on purpose: the footprint can be
 * a 46-gon with a notched facade, and an apron that followed the notches would
 * be a second building outline rather than open ground. Returns 0 when the
 * ring has no area (fewer than 3 hull points). */
int  td5_geolm_apron(const double *rx, const double *rz, int n, double margin,
                     double *ox, double *oz, int *on, int omax);

#endif /* TD5_GEO_LANDMARKS_H */
