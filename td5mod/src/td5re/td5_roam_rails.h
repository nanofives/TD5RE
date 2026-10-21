/**
 * td5_roam_rails.h -- GEO FREE ROAM: RAILWAYS of one tile (PORT-ONLY, roam ID).
 *
 * WHAT IT BUILDS. From the RAILS tile layer (re/tools/geo_rails.py: OSM railway ways + the
 * rail x road crossings computed offline), for one ROAMTILE: the TRACK of every rail /
 * light_rail / disused way (ballast bed with sloped shoulders, sleepers every 0.65 m, two
 * rails at the OSM gauge; disused track is the same with rusty rails), RAIL BRIDGES (deck
 * slab, parapets, piers down to the ground, the ballast and track on top), embankment
 * ramps up to a bridge that a road passes under, LEVEL CROSSINGS (the road stays on top:
 * the rails are laid flush on the road surface, the bed blends down to it over 12 m) and the
 * PLATFORMS (polygon or line, a slab ~1.4 m over the ground). Abandoned / dismantled /
 * razed / proposed / miniature ways, yards and tunnels are not drawn.
 *
 * SOLIDITY. Visual only, like all roam scenery (Decision "roads-only invisible walls"): the
 * car meets track only where a road crosses it, and there it drives on the road, which is
 * untouched. A face of the track may lie over the drivable surface only as a flush rail at a
 * crossing; everything else is kept off it (narrow bed with no shoulders, or nothing) and
 * the census counts any face that is not (st->guard_hits, must be 0). A road deck high over
 * the track (road_over) or a road low under a rail deck (rail_over) is not a conflict: the
 * guard only counts drivable primitives within a height band around the face.
 *
 * HEIGHTS. A pure function of the place position and the data, never of the tile being
 * built, so two tiles meeting at a seam draw the same vertex at the same height:
 *   base(p)  = mean of the RAW height raster (no G4b road lowering, wet samples left out)
 *              over a 5 x 5 lattice of 6 m around p, + the rail-head height (0.45 m);
 *   lift(p)  = for every rail_over crossing k: the rail head must clear the road there by
 *              TD5_ROAM_RAIL_CLEAR; the requirement falls off at RL_GRADE (3 %) along the
 *              rail NETWORK (ways joined at shared nodes), a max-plus Dijkstra run once per
 *              source;
 *   head     = max(base, lift) (a bridge way: the straight line between its ends' heads,
 *              then max with lift); near a level crossing it blends to the road surface.
 *
 * THREADING. A TD5_RoamRailSource is owned by ONE thread (the scenery source).
 */
#ifndef TD5_ROAM_RAILS_H
#define TD5_ROAM_RAILS_H

#include <stdint.h>

/* Bumped BY HAND when a rail rule changes (the scenery rules version moves with it). */
#define TD5_ROAM_RAILS_RULES  1u

typedef struct TG_TileCtx TG_TileCtx;
typedef struct TD5_RoamRailSource TD5_RoamRailSource;

/* One tile's census, for the pieces the tile OWNS (a piece = up to 4 m of one way). */
typedef struct {
    int  ways;               /* ways with at least one owned piece                          */
    int  pieces;             /* owned track pieces drawn                                    */
    int  full, narrow, flush;/* full bed / no shoulders (a road beside it) / flush on a road */
    int  squeezed, nudged;   /* bed cut back to the rails (tight median) / moved <= 1.5 m off a parallel street */
    int  floored;            /* head raised to a street's kerb + 0.57 m (over its median / sidewalk slab) */
    int  flush_nodata;       /* flush pieces with no data crossing within 12 m (OSM offset) */
    int  blend;              /* pieces whose head blends to a level crossing's road         */
    int  bridge, piers, lifted;  /* bridge pieces, piers, pieces raised over base by lift    */
    int  under_deck;         /* pieces under a road deck (road_over: not a conflict)       */
    int  tunnel_skip;        /* pieces of a tunnel way, not drawn                           */
    int  sleepers;
    int  sleeper_cut, wall_cut;  /* sleeper ends / retaining walls left out where they would touch a road */
    int  platforms, plat_pieces, plat_drop;   /* polygons + line pieces drawn, faces off the road */
    int  meshes;
    int  guard_hits;         /* emitted faces on the drivable surface that are not flush rails: 0 */
    double lift_max_m;       /* highest head over base of an owned piece                    */
} TD5_RoamRailStats;

int  td5_roam_rails_enabled(void);   /* TD5RE_ROAM_RAILS (default 1) */

/* Read the RAILS rows of tiles/INDEX.JSON. NULL when the place has no RAILS layer or the knob is off.
 * The layer itself (every RAILS tile, ~0.9 MB for La Plata) is parsed and the lift field solved LAZILY,
 * on the first td5_roam_rails_emit, i.e. on the building thread once it carries the main FP env. */
TD5_RoamRailSource *td5_roam_rails_open(const char *place);
void td5_roam_rails_close(TD5_RoamRailSource *r);
/* source-wide facts for the log: RAILS tiles listed; once loaded, ways kept, vertices, rail_over lift sources, crossings */
void td5_roam_rails_summary(const TD5_RoamRailSource *r, int *ntiles, int *ways, int *verts, int *lifts, int *xings);

/* Append the railways of the ctx's tile to its sub-block buffers. 1 on success, 0 on out of memory. */
int  td5_roam_rails_emit(TD5_RoamRailSource *r, TG_TileCtx *c, TD5_RoamRailStats *st);

#endif /* TD5_ROAM_RAILS_H */
