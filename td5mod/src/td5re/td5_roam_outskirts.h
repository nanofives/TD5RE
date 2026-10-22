/**
 * td5_roam_outskirts.h -- GEO FREE ROAM: what stands OUTSIDE the casco (PORT-ONLY, roam LH).
 *
 * From the OUTSKIRTS tile layer (re/tools/geo_outskirts.py: OSM power poles / towers / lines,
 * barrier=guard_rail ways, road gates), for one ROAMTILE that is NOT one of the casco's:
 *   - POWER POLES (wood / concrete by tag, a hash of the OSM id where untagged) and TOWERS (a
 *     tapered mast with two cross arms), a cross arm per pole, and the WIRES between two
 *     consecutive poles of one line (three conductors, a parabolic sag of 1.2 % of the span);
 *   - GUARD RAILS: a W-beam band on posts every 4 m along each barrier=guard_rail way;
 *   - ROAD GATES: an OPEN gate (two posts a lane-and-a-half either side of the road axis, a
 *     raised boom for a lift_gate). The car never meets a closed gate: roam scenery is visual.
 * Substation portals and every gate not within 6 m of a road are left out (see the fetch tool).
 *
 * SOLIDITY / GUARD. Visual only. Every footprint is tested against the drivable surface
 * (roam_ctx_on_road_band) before it is written; one that touches is skipped and counted
 * (st->on_road_skipped), so the emitted count on a road is 0 by construction (st->guard_hits
 * counts faces written on one anyway: must be 0).
 *
 * THREADING. A TD5_RoamOutSource is owned by ONE thread (the scenery source); its layer is parsed
 * lazily on the first emit (the building thread carries the main FP env, like td5_roam_rails.c).
 */
#ifndef TD5_ROAM_OUTSKIRTS_H
#define TD5_ROAM_OUTSKIRTS_H

#include <stdint.h>

/* Bumped BY HAND when an outskirts rule changes (TD5_ROAM_SCENERY_RULES moves with it). */
#define TD5_ROAM_OUTSKIRTS_RULES  1u

typedef struct TG_TileCtx TG_TileCtx;
typedef struct TD5_RoamOutSource TD5_RoamOutSource;

typedef struct {
    int  poles, wood, concrete, towers;   /* owned poles drawn (towers counted in poles too) */
    int  wires, wire_quads;               /* wire spans drawn */
    double wire_m;                        /* metres of conductor span, per tile owned */
    int  guards, guard_posts;             /* guard-rail segments (1-6 m) + posts */
    double guard_m;
    int  gates, gates_lift;               /* open gates drawn */
    int  on_road_skipped;                 /* footprints left out: on the drivable surface */
    int  portal_skipped;                  /* substation portals (not drawn) */
    int  guard_hits;                      /* emitted faces on the drivable surface: 0 */
    int  quads, meshes;
} TD5_RoamOutStats;

int  td5_roam_out_enabled(void);          /* TD5RE_ROAM_OUTSKIRTS (default 1; 0 = nothing drawn) */

/* Read INDEX.JSON layers.OUTSKIRTS. NULL when the place has no layer or the knob is off. */
TD5_RoamOutSource *td5_roam_out_open(const char *place);
void td5_roam_out_close(TD5_RoamOutSource *o);
/* source-wide facts for the log: tiles listed; once loaded poles / wire spans / guard segments / gates */
void td5_roam_out_summary(const TD5_RoamOutSource *o, int *ntiles, int *poles, int *spans, int *guards, int *gates);

/* Append the outskirts of the ctx's tile to its sub-block buffers. `casco` = 1 when the tile is one of
 * the casco's (nothing is drawn there: the casco is unchanged). 1 on success, 0 on out of memory. */
int  td5_roam_out_emit(TD5_RoamOutSource *o, TG_TileCtx *c, TD5_RoamOutStats *st, int casco);

#endif /* TD5_ROAM_OUTSKIRTS_H */
