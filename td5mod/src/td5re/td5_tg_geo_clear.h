/**
 * td5_tg_geo_clear.h -- GEO TRACK: where a BUILDING may NOT stand (PORT-ONLY).
 *
 * [ROUND 1014 C] "this building is in the middle of the other lane of the
 * avenue" / "a building near ... is in the middle of the street".
 *
 * The generator keeps buildings off three things it KNOWS about: the race
 * carriageway, the avenue sidecar's opposite carriageway, and a fork corridor.
 * It does not know about the rest of the real street network, and a procedural
 * frontage (one wall per span, hash-chosen run/gap pattern) therefore stands
 * straight across any real road that leaves the route and is not one of
 * NETWORK.JSON's mouths: the far end of a divided avenue where the opposite
 * carriageway peels off to a plaza, a real avenue joining a plaza ring, a cross
 * street the network kept for a different span. MEASURED on Mariano's La Plata
 * route: 75 of 677 procedural frontage meshes (11%) had >= 25% of their roof
 * standing on a real foreign road (verify/geo_bld_audit.py --roads).
 *
 * Two pieces, kept out of td5_tg_city.c so the diff stays scoped:
 *
 *   FOREIGN ROAD PROBE  tg_geo_foreign_road_at(). "Is there a drivable way under
 *                       this point that is NOT the road the race runs on?" The
 *                       race road's own way (and the avenue partner carriageway)
 *                       are recognised geometrically -- near-parallel to the
 *                       span and within the carriageway authority's reach plus a
 *                       slack -- because the conditioner moves the road a few
 *                       metres off its OSM polyline, so an identity test by way
 *                       id or name would either miss that or also exempt the
 *                       avenue's own continuation.
 *   BUILDING CLEAR GAP  tg_geo_building_clear_gap(). tg_carriageway_clear_gap
 *                       plus the FAR FOOTWAY the avenue emitter lays outside the
 *                       opposite carriageway (td5_tg_avenue.c): a building is
 *                       set back to the back edge of that slab, not onto it.
 *
 * BYTE IDENTITY WITH SYNTHETIC. Every entry point returns 0 / the plain gap
 * before touching anything when no place is loaded, and nothing here draws from
 * the RNG. The synthetic MODELS.DAT hash is the gate that proves it.
 */
#ifndef TD5_TG_GEO_CLEAR_H
#define TD5_TG_GEO_CLEAR_H

#include "td5_trackgen_internal.h"

/* Single-threaded, once per build, BEFORE the scenery workers exist: index the
 * loaded place's drivable ways. Idempotent per build (it rebuilds). */
void tg_geo_clear_prepare(void);

/* 1 once tg_geo_clear_prepare indexed at least one way. */
int  tg_geo_clear_ready(void);

/* Is (x,z) standing on a drivable way that is not the race road?
 *   (ox,oz) (tx,tz)   the host span's node and UNIT tangent
 *   own_lat           lateral distance from that node out to which a PARALLEL
 *                     way is "the race road or its avenue partner", world units
 * Read-only after tg_geo_clear_prepare, safe from any scenery worker. */
int  tg_geo_foreign_road_at(double x, double z, double ox, double oz,
                            double tx, double tz, double own_lat);

/* The own-lateral for span `si` on `side` (+1 left): the carriageway authority's
 * reach plus the slack the conditioner's deviation needs. */
double tg_geo_own_lateral(const TG_NodeList *nl, int si, double side);

/* tg_carriageway_clear_gap for a BUILDING: also clears the avenue's far
 * footway. `sidewalk` is the pavement width the caller would pass. Identical to
 * tg_carriageway_clear_gap wherever no far footway is laid (and on every
 * synthetic build). */
double tg_geo_building_clear_gap(const TG_NodeList *nl, int si, double side,
                                 double sidewalk);

/* Does a procedural wall actually stand on side `left` of span si (the same
 * code path the emitter runs)? Defined in td5_tg_city.c, which owns the wall. */
int  tg_side_stands(const TG_NodeList *nl, int si, int left);

/* [ROUND 1014 C] A shipped set piece (tg_prefab_place) is sited by a FIXED
 * clearance from the road edge, so on a geo track it stood across the opposite
 * carriageway of a divided avenue and across real streets. These two let the
 * placer ask the same questions every other building asks:
 *   tg_geo_prefab_blocked  a real road under the piece's rectangle -> refuse the site
 *   tg_geo_prefab_seat     the piece's base: the LOWEST ground under that rectangle
 * Both are identity / no-ops on a synthetic build. `fx` / `fz` are the piece's
 * footprint extents along the road / across it, world units. */
int  tg_geo_prefab_blocked(const TG_NodeList *nl, int si, double side,
                           double x, double z, double fx, double fz);
double tg_geo_prefab_seat(const TG_NodeList *nl, int si, double x, double z,
                          double fx, double fz, double y_centre);
double tg_prefab_half_width(int pf);          /* td5_tg_prefab.c */

/* How many span-sides / real footprints the road probe removed, for the census. */
void tg_geo_clear_note_wall(void);
void tg_geo_clear_note_footprint(void);
void tg_geo_clear_report(void);

#endif
