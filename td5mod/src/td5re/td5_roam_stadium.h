/**
 * td5_roam_stadium.h -- GEO FREE ROAM: STADIUMS and SPORTS PITCHES of one tile (PORT-ONLY, roam PB).
 *
 * SOURCE. Nothing of its own: td5_roam_scenery.c hands over what the tile layers already hold.
 *   STADIUM  = an AREAS / PARK_RELATIONS leisure=stadium outline the tile owns, its main pitch (the
 *              largest leisure=pitch inside it) and its STANDS: every footprint of the 3x3 whose class is
 *              stadium / grandstand / bleachers and whose centroid is inside the outline (td5_roam_scenery.c
 *              sc_sport_claim, the same test the building pass uses to leave them to this module). An
 *              Overture / Open Buildings blob whose centroid falls inside a stand is the same structure traced
 *              twice and is not drawn at all.
 *   PITCH    = an AREAS leisure=pitch the tile owns, with its sport= and surface= codes (TD5_RPS_* / _RPF_*).
 * re/tools/geo_stadia.py fetches everything OSM maps inside the stadiums (STADIA.JSON). This module does
 * not read it: in La Plata OSM maps no tier / roof part, only the outlines (_archive/roam/PB/REPORT.md).
 *
 * WHAT IS DRAWN (VISUAL only, no collision: Decision "roads-only invisible walls")
 *   BOWL (a stand outline with a courtyard, the pitch inside it: the Estadio Unico, S3D relation 19838691):
 *     stepped tiers from a front line round the pitch (its rectangle + a margin, rounded corners) up to the
 *     outline, a band of boxes half way up, the outer wall to the mapped height with dark entrance portals,
 *     and the membrane roof from the outline to the courtyard ring (the Unico's courtyard IS its roof
 *     opening, the two intersecting circles), folded on radial ridges, with a steel edge ring on each side.
 *   OPEN STAND (any other stand footprint): the outline split into the chain facing the pitch and the
 *     chain away from it (the two cut edges = the ends); stepped tiers between them, a back wall with
 *     portals, stepped end walls; a stand 15 m or taller also gets a cantilever roof (a rule: OSM maps none).
 *   PITCH: the surface (grass in mowing stripes, clay, hard court, acrylic, artificial turf, sand) over the
 *     lawn, and the markings of its sport on the pitch's rectangle (the outline's minimum-area box):
 *     soccer (lines, circles, boxes, arcs, spots, goals), tennis (one or more courts tiled in the box, nets),
 *     padel, basketball (keys, arcs, hoops), volleyball (net), field hockey (circles, goals), rugby (H posts).
 *     Sizes are the regulation ones, scaled down to fit a smaller box (a futbol 5 pitch).
 * LEFT OUT (the round rule: tiny counts nobody notices): pitches under SMALL_M2 (150 m2), athletics (9 in the
 *   partido: jump pits), equestrian, skateboard, bowls, pelota, baseball (1-3 each); a pitch with neither a
 *   known sport nor a surface keeps its lawn. Counted in skip_small / skip_sport / skip_none.
 *
 * PLACEMENT. Bases from the lawn under a point, else the pavement slab, else the drawn ground. Every face is
 * audited against the drivable surface (guard_hits must stay 0); a pitch touching the road is not painted.
 *
 * THREADING. Pure function of the inputs; no state.
 */
#ifndef TD5_ROAM_STADIUM_H
#define TD5_ROAM_STADIUM_H

typedef struct TG_TileCtx TG_TileCtx;

/* sport= of a pitch (first token of a ;-list), as the AREAS reader stores it (one byte per record) */
enum { TD5_RPS_NONE = 0, TD5_RPS_SOCCER, TD5_RPS_TENNIS, TD5_RPS_PADEL, TD5_RPS_BASKET, TD5_RPS_VOLLEY, TD5_RPS_HOCKEY,
       TD5_RPS_RUGBY, TD5_RPS_BEACH, TD5_RPS_OTHER, TD5_RPS__N, TD5_RPS_STADIUM = 255 };
/* surface= of a pitch */
enum { TD5_RPF_NONE = 0, TD5_RPF_GRASS, TD5_RPF_CLAY, TD5_RPF_HARD, TD5_RPF_ACRYLIC, TD5_RPF_TURF, TD5_RPF_SAND,
       TD5_RPF_OTHER, TD5_RPF__N };

int td5_roam_sport_of(const char *sport);       /* NULL / "" -> TD5_RPS_NONE */
int td5_roam_surface_of(const char *surface);   /* NULL / "" -> TD5_RPF_NONE */

/* One stand footprint, tile-local x/z (units). Courtyards (a relation outline) concatenated in hx/hz. */
typedef struct {
    const double *px, *pz;
    int n;
    int nh;
    const int *hn;
    const double *hx, *hz;
    double height;          /* units: top of the mass (OSM, else the estimator) */
    double id;
} TD5_RoamStand;

typedef struct {
    double osm_id;          /* the OSM way / relation id (the per-stadium seat colours) */
    const double *rx, *rz;  /* the leisure=stadium outline, tile-local */
    int n;
    const double *qx, *qz;  /* its main pitch's outline, tile-local; qn 0 = none mapped */
    int qn;
    const TD5_RoamStand *st;
    int nst;
} TD5_RoamStadium;

typedef struct {
    double id;
    int sport, surface;     /* TD5_RPS_* / TD5_RPF_* */
    const double *rx, *rz;  /* tile-local outline */
    int n;
} TD5_RoamPitch;

/* One tile's census (what the tile OWNS) */
typedef struct {
    int stadiums, bowls, stands, stand_roofs, roofs, portals, tier_quads;
    int openings;           /* walls standing on a drivable way, opened SP_CLEAR over it (or left out) */
    int pitches, by_sport[TD5_RPS__N], by_surface[TD5_RPF__N], surface_only;
    int skip_small, skip_sport, skip_none, drop_road;
    int courts, goals, nets, hoops, line_quads;
    int meshes;
    int guard_hits;         /* emitted faces on the drivable surface: must be 0 */
} TD5_RoamSportStats;

int td5_roam_stadium_enabled(void);   /* TD5RE_ROAM_STADIUM (default 1; 0 = stand footprints built as buildings) */
int td5_roam_pitch_enabled(void);     /* TD5RE_ROAM_PITCH_LINES (default 1; 0 = pitches stay plain lawn) */

/* Append the stadiums and pitches the ctx's tile owns to its sub-block buffers. 1 on success, 0 on OOM. */
int td5_roam_sport_emit(TG_TileCtx *c, const TD5_RoamStadium *sd, int nsd, const TD5_RoamPitch *pt, int npt,
                        TD5_RoamSportStats *st);

#endif /* TD5_ROAM_STADIUM_H */
