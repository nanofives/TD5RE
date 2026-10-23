/**
 * td5_roam_furniture.h -- GEO FREE ROAM: street FURNITURE of one tile (PORT-ONLY,
 * roam F4c). Design: docs/plans/GEO_FREE_ROAM_DESIGN.md 4.3 / 4.4 ("plan rows change to
 * {edge, side, along, x, z}, emitters only swap the frame").
 *
 * WHAT IT BUILDS. For one ROAMTILE: traffic signals at the real OSM signal clusters
 * (the strip's layout rule: a mast arm with the live lamp cycle and a blue street-name
 * plate on the near RIGHT kerb of every approach, a plain post light on the near LEFT
 * kerb, a far mast arm across the junction where the opposite arm has no near arm of
 * its own), street lamps along the streets (spacing by class), bus stops with the
 * shelter / bench / bin a mapper surveyed, and zebra / limit-line decals where OSM has
 * a crossing. Everything is VISUAL (no collision, Decision "roads-only invisible
 * walls"): poles stand beside the carriageway, everything over it hangs above 4.4 m,
 * and the only thing painted on the surface is a decal lifted 20 raw.
 *
 * WHAT IS SHARED WITH THE STRIP. The mesh writers (tg_emit_box_mesh,
 * tg_emit_billboard_mesh, tg_write_quad_mesh), the geometry numbers of td5_tg_mastarm.c /
 * td5_tg_furniture.c / td5_tg_geo_street.c / td5_tg_city.c (copied value for value, the
 * mast arm port is line for line), the plate text planner (tg_plate_*), the lamp cycle
 * of td5_geo_signals.c and the renderer's tag hooks (TD5_MESH_TAG_SIGNAL_*). What is
 * roam's own: the frame (a point on a ROAD-layer ribbon: roam_nearest_edge /
 * roam_edge_frame), the street-name plate ATLAS (one per session, below) and the lamp
 * registry feed.
 *
 * PLATE ATLAS. A TEXTURES.DAT page is 64x64, so a plate is a 64x16 / 64x32 CELL. The host
 * level's own plate pages only hold the names of the strip route, so roam rasterises its
 * own: ONE atlas per session, built on the main thread BEFORE any tile build, from every
 * signalised cluster of the casco (every approach arm's own street and its cross street).
 * Cells are allocated in a fixed order (use count, then name), so the atlas is a pure
 * function of the ROAMTILE files. The pages live at TD5_TG_PAGE_COUNT +
 * TD5_TG_MASTARM_PAGES_MAX .. 799 (above anything a strip build of the host can write)
 * and are uploaded at roam render begin. A cell that does not fit falls back to the
 * other face's name, then to no plate (counted).
 *
 * LAMP CYCLE. td5_geo_signals.c's cycle read a wall clock; roam drives it from the
 * SESSION TIME (sim ticks / 30), set once per frame by td5_roam_furn_frame, so the
 * phase is a pure function of the tick and every observer (renderer, F8 traffic) sees
 * the same one.
 *
 * THREADING. A TD5_RoamFurnSource is owned by ONE thread (inside the scenery source). The
 * atlas is immutable after td5_roam_furn_session_begin / _atlas_prepare.
 */
#ifndef TD5_ROAM_FURNITURE_H
#define TD5_ROAM_FURNITURE_H

#include <stddef.h>
#include <stdint.h>
#include "td5_roam_tile.h"

/* Bumped BY HAND when a furniture rule changes (mixed into the scenery hash). */
#define TD5_ROAM_FURN_RULES  3u   /* 2: [F8] each approach's facing heads show its signal group
                                   * 3: [roam IE] motorway / expressway barriers (W-beam outside, New Jersey
                                   *    on the median, parapets on decks), embankments, bridge piers + abutments */

typedef struct TG_TileCtx TG_TileCtx;
typedef struct TD5_RoamFurnSource TD5_RoamFurnSource;

/* One tile's census. All counters are for the records the tile OWNS. */
typedef struct {
    /* signals */
    int  sig_clusters, sig_arms, sig_inbound;        /* clusters, their arms, arms with approach traffic */
    int  mast_near, post_left, mast_far, far_shared;  /* N, P, F placed; F not needed (opposite arm has an N) */
    int  plates, plate_one_face, plate_none;          /* mast arms: both faces named / one cell / no cell */
    int  sig_arms_absorbed;                           /* arms with no ribbon of their own (absorbed into the patch) */
    int  sig_drop_frame, sig_drop_road, sig_drop_dup; /* ribbon not located / pole on the road / pole too near another */
    /* lamps */
    int  lamps, lamp_glow, lamp_drop_road, lamp_skip_class;
    /* bus stops */
    int  stops, shelters, benches, bins, stop_drop_far, stop_drop_road;
    /* crossings */
    int  zebras, limit_lines, xing_unknown, xing_drop_far;
    /* [roam LC] unmarked skipped, unknown painted by the rule (zebra / lines), footway=crossing ways seen / already a node / drawn */
    int  xing_unmarked, xing_unk_zebra, xing_unk_lines, xing_ways, xing_way_dup, xing_way_drawn;
    int  xing_rail_drop;            /* [integ L] rule-painted / way crossings left off a rail bed (TD5RE_ROAM_XING_RAIL_VETO) */
    /* [roam IE] barriers (metres of kerb), segments dropped because a carriageway lies under them, patch
     * kerb edges railed; embankment metres; piers (and their columns), columns dropped on a road below, abutments */
    double bar_guard_m, bar_nj_m, bar_parapet_m, emb_m;
    int  bar_drop_road, bar_patch_edges, emb_drop_road;
    int  piers, pier_cols, pier_drop_road, abutments;
    /* [roam PA] bus / bike lane paint: spans + metres by kind, kerb boxes, ways met, spans with no way direction / too narrow */
    int  lane_bus_segs, lane_bike_segs, lane_track_segs, lane_kerbs, lane_ways, lane_no_dir, lane_narrow;
    int  lane_rail;                 /* [integ P] lane spans left bare: the carriageway meets a drawn rail bed */
    double lane_bus_m, lane_bike_m, lane_track_m;
    /* output */
    int  meshes;                                      /* furniture meshes appended */
    int  guard_hits;                                  /* furniture faces on the drivable surface, not overhead, not a decal: must be 0 */
} TD5_RoamFurnStats;

/* Session (MAIN thread, before any worker builds a tile): street-name plate atlas from
 * the ROAMTILE files + the GPU pages. upload = 0 builds the atlas only (dev CLI). 1 on
 * success; 0 leaves the furniture without plates (mast arms keep their heads). */
int  td5_roam_furn_atlas_prepare(const char *place, const RoamGrid *g);
int  td5_roam_furn_session_begin(const char *place, const RoamGrid *g);   /* prepare + upload */
void td5_roam_furn_session_end(void);
int  td5_roam_furn_plates_ready(void);
/* [roam LF] the atlas cell of a name (a station board): TEXTURES.DAT page id, the cell's top / bottom v (0..1, a
 * ready-to-use inset of 1.5 texels) and its row count (1 = 64x16, 2 = 64x32). 0 when the name has no cell. */
int  td5_roam_furn_plate_cell(const char *name, int *page_id, double *v_top, double *v_bot, int *rows);

int  td5_roam_furn_enabled(void);     /* TD5RE_ROAM_FURNITURE (default 1) */
int  td5_roam_night_active(void);    /* [F4f] a roam session at night: ambient dimming + street lamps on, no DarkMode / StreetLights needed */
int  td5_roam_furn_night(void);       /* lamps carry their glow: TD5RE_ROAM_NIGHT, else the roam sky's verdict */

TD5_RoamFurnSource *td5_roam_furn_open(const char *place);
void td5_roam_furn_close(TD5_RoamFurnSource *f);

/* Append the furniture of the ctx's tile to its sub-block buffers. lamps / nlamps: the
 * lantern positions (tile-local x, place y, tile-local z; 3 floats each) as a malloc'd
 * array the caller owns. 1 on success, 0 on out of memory. */
int  td5_roam_furn_emit(TD5_RoamFurnSource *f, TG_TileCtx *c, TD5_RoamFurnStats *st,
                        float **lamps, uint32_t *nlamps);

/* Once per frame, MAIN thread, before the lights are emitted: the signal cycle's session
 * clock and the street-lamp registry (the nearest resident lamps). No-op outside roam. */
void td5_roam_furn_frame(void);

#endif /* TD5_ROAM_FURNITURE_H */
