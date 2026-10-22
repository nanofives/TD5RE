/**
 * td5_roam_scenery.h -- GEO FREE ROAM: the SCENERY layer of one tile (PORT-ONLY,
 * roam F4a). Design: docs/plans/GEO_FREE_ROAM_DESIGN.md 0 item 9, 3.6, 4.1 - 4.4.
 *
 * WHAT IT BUILDS. For one ROAMTILE (tx, tz): the real OSM BUILDINGS whose centroid
 * the tile owns (heights, roofs, landmark set pieces, the worship apron), the mapped
 * FOOTPATHS (footway / path / pedestrian / cycleway pieces whose midpoint it owns)
 * and the PLAZA LAWNS + PLAZA TREES of the AREAS polygons whose centroid it owns,
 * and [F4b] the PAVEMENT along every kerb of the drivable surface the tile owns
 * (slab at kerb height, kerb face, back face, fence or deck railing; junction corners
 * follow the patch outline and its fillets; medians and traffic islands are filled
 * and kerbed both sides; widths per (edge, side) from td5_geo_sw_resolve),
 * and [HD] the CITY LOTS (every ground cell of the city that no OSM green or ring plaza holds, laid again in
 * concrete just over the drawn ground) and the INFILL masses (frontage lots on a block's building line with no
 * real footprint on them; td5_roam_scenery.c SECTION lots),
 * as MODELS.DAT mesh records (the generator's own writers), one record list per
 * 64 m sub-block (4 x 4 per tile). No texture page is created: every face names a
 * page of the geo host level's TEXTURES.DAT (TD5_TG_PAGE_*).
 *
 * WHAT IS SHARED WITH THE STRIP. The building extrusion is the strip's own code
 * (tg_geo_emit_building_core, td5_tg_geo_bld.h), the records are parsed with the
 * strip's rules (td5_geob_parse_building / _area, td5_geo_footways_kind_of), the
 * lawn constants, the tree lattice step, the canopy threshold, the facade / roof
 * / sidewalk / lawn pages and the set-piece table are the strip's. What is roam's
 * own: the frame a footprint is cleared against (TG_TileCtx + roam_nearest_edge on
 * the ROAD layer instead of a host span) and the GUARD RULE.
 *
 * THE GUARD RULE (no scenery on the drivable surface). Scenery is visual only
 * (Decision: roads-only invisible walls), so nothing may stand on what the car
 * drives on: every footprint, lawn triangle, footpath piece, set piece, apron
 * quad and tree trunk square is tested against the drivable primitives (ribbon
 * quads + junction patch triangles) of the tile and its 8 neighbours, exactly
 * (polygon overlap, not a sample). A building is nudged off the nearest street
 * first (the strip's carriageway + pavement clearance, TD5_TG_GEO_MAX_SHIFT cap; a
 * landmark set skips this nudge, TD5RE_ROAM_LM_WINS, F4b),
 * then dropped if it still overlaps; lawn triangles are refined and the
 * overlapping pieces dropped; footpath pieces on a road are cut. The census
 * re-checks every emitted footprint at the end (st.guard_overlaps, must be 0).
 *
 * FRAME. Records are in the PLACE frame; every vertex is written TILE-LOCAL (place
 * minus the tile origin, roam_grid_origin), heights absolute. The bytes are
 * therefore a pure function of (tile, the 3x3 ROAD tiles, the OSM cache, the
 * rasters, TD5_ROAM_SCENERY_RULES): same bytes in any order, on any thread
 * (td5_roam_scn_dev_cli + verify/roam/determinism.py --scenery).
 *
 * THREADING. A TD5_RoamScnSource is owned by ONE thread (the stream worker); it
 * caches parsed OSM cache tiles (LRU) and neighbour ROAD tiles. Nothing here
 * touches file-scope mutable state.
 */
#ifndef TD5_ROAM_SCENERY_H
#define TD5_ROAM_SCENERY_H

#include <stddef.h>
#include <stdint.h>
#include "td5_roam_tile.h"
#include "td5_roam_height.h"
#include "td5_roam_furniture.h"
#include "td5_roam_rails.h"

/* Bumped BY HAND when a scenery rule changes (the determinism hash includes it).
 * 2: F4b pavement (slab, kerb, back face, fence, junction corners, medians) and the
 *    landmark set that is cleared off the drivable surface only (no pavement gap). */
#define TD5_ROAM_SCENERY_RULES  16u  /* F4b pavement + F4c furniture combined; 4 = G4b ground lowered under the road;
                                     * 5 = G5 ground lowered per road triangle, footpaths on top of slabs / islands / lawns
                                     * 6 = roam HC verge: the ground beside every kerb rises at most 8 % from the road edge
                                     *     (HD: city lots paved (concrete) off the OSM greens + infill buildings on the frontage)
                                     * 7 = round H merge: HC verge + HD lots together
                                     * 8 = roam M: the place frame is the GAME frame (x = -east,
                                     *     td5_geo_frame.h), La Plata no longer mirrored
                                     * 9 = roam M: patches without acute wedge tips (ROAM_RULES 6), no island spikes
                                     * 10 = roam IA: a gap of <= 25 m to another carriageway with no building in it is an
                                     *      island filled to the middle (any streets), island slabs keep their back face
                                     * 10 = roam IB: no kerb fences, no stamped TD5 landmark prefabs
                                     * 10 = roam IC: avenue medians + named-ring plazas green, Overture footprints
                                     *     not built on them / on parks, dropped footprints free the frontage,
                                     *     infill 1-3 floors (taller on avenues) 12-24 m deep, AREAS spikes
                                     * 10 = roam ID: railways (td5_roam_rails.c, the RAILS tile layer)
                                     * 11 = round I integration: every round I branch combined (each bumped 9 -> 10 on its own,
                                     *      11 so no tile cached by a single branch is reused)
                                     * 12 = roam JA: pavement joins never on the road side of a kerb, islands filled to the
                                     *      medial line at one level, a narrow kerbed median between a same-name pair
                                     * 12 = roam JC: OSM Simple 3D Buildings (td5_tg_s3d.c): every building:part and
                                     *      roof:shape footprint gets its real roof on any ring, roof-only parts,
                                     *      roof / wall colours, outlines replaced by their parts not drawn
                                     * 13 = roam JC2: S3D_RELATIONS.JSON outlines with courtyards, Overture blobs
                                     *      under them dropped, civic stone / plaster facades, bases under parts
                                     * 14 = round J integration: roam JA (12) + roam JC (12, 13) combined,
                                     *      14 so no tile cached by a single branch is reused
                                     * 15 = integ J: a carriage arch opens every stretch of a part / relation wall that stands on
                                     *      a service drive (not only where its centreline crosses), up to 128 drive segments
                                     * 16 = roam LA: street trees from CANOPY.R8 along the pavement band + lot trees on the
                                     *      world lattice where the canopy says so (td5_roam_scenery.c SECTION trees);
                                     *      canopy green: wooded unmapped ground stays lawn, takes no infill */

#define TD5_ROAM_SCN_SUB   4                    /* sub-blocks per tile side (64 m) */
#define TD5_ROAM_SCN_NSB   (TD5_ROAM_SCN_SUB * TD5_ROAM_SCN_SUB)

/* the frame every emitter asks for (design 4.3), replaces tg_city_edge_frame */
typedef struct {
    double px, pz, py;          /* point on the edge centreline at `along`, tile-local x/z */
    double tx, tz;              /* unit tangent (row direction)                            */
    double nx, nz;              /* unit outward normal for `side`                          */
    double half_carriage;       /* units                                                   */
    double kerb_y;              /* road surface height at the kerb                         */
    int    edge;                /* ctx edge index (roam_nearest_edge)                      */
    uint64_t key;               /* RoamEdgeRec.key of that edge                            */
    int    klass;               /* TD5_GEO_RC_*                                            */
} RoamFrame;

typedef struct TG_TileCtx TG_TileCtx;

/* Nearest street piece to the tile-local point (x, z) within max_d units of its
 * centreline: edge (ctx index), side (+1 = left of the row direction, -1 right),
 * along (units from the edge's row 0) and the signed lateral offset from the
 * centreline. 1 on a hit, 0 when no street is that near. */
int roam_nearest_edge(const TG_TileCtx *c, double x, double z, double max_d,
                      int *edge, int *side, double *along, double *lateral);
/* The frame of edge `edge` at `along` on `side`. 1 on success. */
int roam_edge_frame(const TG_TileCtx *c, int edge, int side, double along, RoamFrame *out);

/* ---- [F4c] the ctx as the furniture emitters see it (td5_roam_furniture.c) ---- */
/* The ROAD tile of the 3x3 at (dx, dz) in -1..1 (0,0 = the tile being built), NULL if absent. */
const RoamTile *roam_ctx_tile(const TG_TileCtx *c, int dx, int dz);
const RoamGrid *roam_ctx_grid(const TG_TileCtx *c);
/* the tile being built: its index and the place-frame origin every x/z here is relative to */
void   roam_ctx_origin(const TG_TileCtx *c, int *tx, int *tz, int32_t *org_x, int32_t *org_z);
/* 1 when the polygon (n = 1 point, 2 segment, 3 / 4 polygon; tile-local) overlaps a drivable primitive */
int    roam_ctx_on_road(const TG_TileCtx *c, const double *x, const double *z, int n);
/* 1 when the tile-local point lies in the tile being built (ownership of a record anchored there) */
int    roam_ctx_owned(const TG_TileCtx *c, double x, double z);
/* the 64 m sub-block (0..TD5_ROAM_SCN_NSB-1) holding a tile-local point */
int    roam_ctx_sb_of(const TG_TileCtx *c, double x, double z);
/* ground height at a tile-local point (the scenery's own height source) */
double roam_ctx_ground(const TG_TileCtx *c, double x, double z);
/* [roam ID] the RAW height raster at a tile-local point (no G4b lowering under the road): 1 + *h (and
 * *wet when non-NULL) when the tile's window covers it */
int    roam_ctx_ground_raw(const TG_TileCtx *c, double x, double z, double *h, int *wet);
/* [roam ID] the drivable surface height at a tile-local point: of the primitives under (x, z), the one
 * whose height there is nearest ref_y (a deck over a street answers by its height). 1 on a hit. */
int    roam_ctx_road_y(const TG_TileCtx *c, double x, double z, double ref_y, double *y);
/* [roam ID] roam_ctx_on_road counting only the primitives whose height range meets [y_lo, y_hi]: a road
 * deck high over the polygon or a street far under it is not "on" it */
int    roam_ctx_on_road_band(const TG_TileCtx *c, const double *x, const double *z, int n, double y_lo, double y_hi);
/* [roam ID] the pavement slab (this tile's, emitted before) / plaza lawn plane (the 3x3's) height at a
 * tile-local point; 1 on a hit */
int    roam_ctx_slab_y(const TG_TileCtx *c, double x, double z, double *y);
int    roam_ctx_lawn_y(const TG_TileCtx *c, double x, double z, double *y);
/* Reserve one mesh record in sub-block `sb`: returns the TG_Buf* to write it into (its offset is
 * recorded; void* because TG_Buf lives in td5_trackgen_internal.h), NULL on out of memory. The
 * caller appends exactly one MODELS.DAT mesh record. */
void  *roam_ctx_mesh_begin(TG_TileCtx *c, int sb);

typedef struct {
    /* buildings */
    int  bld_seen, bld_emitted, bld_prefab, bld_drop_road, bld_drop_shift, bld_drop_ring;
    int  bld_veto_plaza, bld_veto_lm, bld_nudged, bld_lm, bld_apron;
    double bld_shift_max;           /* units */
    /* footpaths */
    int  fp_ways, fp_pieces, fp_cut_road, fp_merged, fp_struct;
    int  fp_on_pv;          /* [G5] footpath vertices lifted onto a pavement slab / island they cross */
    /* plazas */
    int  areas, lawn_tris, lawn_cut, trees, trees_cut;
    /* [roam LA] STREET + LOT TREES from CANOPY.R8 (SECTION trees): planted on the pavement band, planted on the lot
     * lattice; band stations walked, refused: no canopy >= 3 m there, band too narrow, facade too close, slab not
     * found / on a road / a driveway mouth; refused by street furniture or a rail bed after every along-kerb dodge,
     * moved along the kerb to dodge one, too near the tile edge (a neighbour's furniture could stand there); lot points
     * hidden behind a facade, lot points refused (road, slab, building, plaza / green, water); planted tree origins on
     * the drivable surface (the bytes census: 0); canopy >= 3 m cells of the owned tile, and those under a crown of
     * any tree (street, lot or plaza) */
    int  st_band, st_lot, st_stations, st_nocanopy, st_narrow, st_facade, st_noslab;
    int  st_furn_drop, st_furn_moved, st_border, st_lot_hidden, st_lot_refused, st_on_road;
    int  canopy_cells, canopy_cov;
    /* [roam LA] CANOPY GREEN (sc_canopy_green): city lot cells left lawn instead of paved, infill lots refused, their
     * frontage (m) */
    int  lot_canopy_green, inf_drop_canopy;
    double inf_m_canopy;
    /* [F4b] pavement: owned kerb pieces, those inside another drivable primitive, chains,
     * emitted pieces (corner = on a junction patch outline), vertices merged into a corner
     * point, island / median pieces, vertices clamped to half a gap, fences, deck railings,
     * kerb faces, pieces cut back / dropped by the guard, kerb faces dropped, facade clips,
     * ribbon kerbs whose way the cache lacks, (edge, side) widths per TD5_GEO_SWSRC_* */
    int  pv_bedges, pv_interior, pv_chains, pv_pieces, pv_corner, pv_collapsed, pv_island, pv_median;
    int  pv_island_fill;          /* [roam IA] kerb vertices made an island by the no-building gap rule */
    int  pv_island_tops, pv_island_top_refused;   /* [roam IA] island loop top triangles emitted, loops left to the pieces */
    int  pv_free_clamp, pv_fence, pv_rail, pv_kerb, pv_cut, pv_drop, pv_kerb_cut, pv_noway;
    int  pv_src[7];
    uint32_t ms_pv;
    /* [F4b] landmark sets let stand over a dead-end stub drawn to their wall (quads), and the
     * census faces that meet only such a quad (booked here, NOT in guard_overlaps) */
    int  bld_lm_stub, guard_lm;
    /* output */
    int  meshes, guard_overlaps;    /* guard_overlaps = emitted footprints on a road: 0 */
    uint32_t bytes, hash;
    uint32_t ms, ms_src, ms_emit;   /* worker time: total, OSM source, emit           */
    int  road_tiles;                /* ROAD tiles of the 3x3 that were available       */
    TD5_RoamFurnStats furn;         /* [F4c] street furniture census (td5_roam_furniture.c) */
    TD5_RoamRailStats rail;         /* [roam ID] railways census (td5_roam_rails.c) */
    /* [HD] CITY LOTS: the tile's ground cells (the render's own 16 m lattice) paved with concrete, kept green under an
     * OSM green, left green outside the city (not casco, not built-up cover), skipped (water, bore hole, no height);
     * paved cells that reach under the drivable surface (the lot lies under the road there, as the ground does).
     * Areas (m2) of the NON-ROAD ground of the tile's cells (the cell refined against the road, 2 m pieces): paved,
     * left as LAWN although OSM maps no green there, under an OSM green. */
    int  lot_cells, lot_green, lot_rural, lot_skip, lot_under_road;
    int  lot_rings;                 /* [HD] ring plazas (a roundabout's inner kerb loop) kept green */
    double lot_paved_m2, lot_lawn_m2, lot_green_m2;
    /* [HD] INFILL: frontage lots along the pavement's building line with no real footprint on them -> a built mass
     * (candidates owned here, emitted, refused: a real building stands on the front, a road / another lot, an area
     * that takes none, a lot too shallow or bent) and the footprint area added (m2) */
    int  inf_lots, inf_emitted, inf_drop_real, inf_drop_road, inf_drop_area, inf_drop_shape;
    double inf_m2;
    /* [IC] avenue medians (a rambla's kerb loop) kept green + taking no building; Overture footprints not built on a
     * park / ring plaza / median polygon (the green veto); footprints the pavement + infill stopped treating as
     * standing because the emitter drops them (sc_bld_drops). Frontage METRES of the owned infill candidates by
     * outcome: built, taken by a real footprint, refused by an area (green / open / not city), other (road, shape) */
    int  lot_medians, bld_veto_green, pv_bld_gone, lot_cover_green;
    double inf_m_built, inf_m_real, inf_m_area, inf_m_other;
    /* [integ-roamI] infill candidates refused because a drawn railway bed / platform reaches them (counted in
     * inf_drop_area too) and their frontage metres */
    int  inf_drop_rail;
    double inf_m_rail;
    int  inf_rail_moved, inf_rail_shallow;   /* ... built anyway: front moved back behind the bed / shallower */
    /* [JC] Simple 3D Buildings: footprints built by tg_s3d_build (building:part or roof:shape), by how the roof was
     * made (TG_S3D_HOW_*: flat, ridge, barrel, centroid, offset, flat fallback), roof-only parts, tinted surfaces,
     * outlines not drawn because their building:parts replace them */
    int  bld_s3d, s3d_how[6], s3d_roof_only, s3d_tint, bld_s3d_hidden;
    int  s3d_tagged, s3d_sloped;   /* footprints with part / roof:shape / roof-only drawn (either builder), with a sloped roof */
    /* [JC2] relation outlines owned / built, courtyard rings walled, holed caps / lidded, civic facades, bases under
     * parts, Overture blobs not built because a relation outline covers them */
    int  bld_rel, s3d_rel_built, s3d_holes, s3d_cap_holed, s3d_cap_lidded, s3d_civic, s3d_base, bld_rel_ovt_drop;
    int  s3d_portals;              /* [JC2] wall openings where a service way passes under an outline */
} TD5_RoamScnStats;

/* The scenery layer of one tile. Owned by the caller; free with td5_roam_scn_free. */
typedef struct TD5_RoamScenery {
    int      tx, tz;
    int32_t  org_x, org_z;                          /* tile origin, place frame        */
    uint8_t *blob;                                   /* every sub-block's records       */
    size_t   blob_bytes;
    uint32_t sb_first[TD5_ROAM_SCN_NSB], sb_count[TD5_ROAM_SCN_NSB];   /* into rec_off */
    uint32_t *rec_off;                               /* record offsets in blob          */
    uint32_t nrec;
    float    *lamp;                                  /* [F4c] lantern positions: tile-local x, place y, tile-local z */
    uint32_t  nlamp;
    TD5_RoamScnStats st;
} TD5_RoamScenery;

void td5_roam_scn_free(TD5_RoamScenery *sc);

/* A neighbour ROAD tile the source has no file for: the caller builds it (the
 * stream worker's builder). NULL = absent. The returned tile is owned by the source
 * afterwards (freed with td5_roam_tile_free + free). */
typedef RoamTile *(*TD5_RoamScnBuildFn)(void *u, int tx, int tz);

typedef struct TD5_RoamScnSource TD5_RoamScnSource;
/* One per thread. `build` may be NULL (a missing neighbour file is then absent). */
TD5_RoamScnSource *td5_roam_scn_open(const char *place, const RoamGrid *g,
                                     TD5_RoamScnBuildFn build, void *u);
void td5_roam_scn_close(TD5_RoamScnSource *s);

/* Build the scenery of `self` (its ROAD tile). hc = the caller's height source
 * (TD5_RoamHeight, prefetched here). NULL on out of memory or when scenery is off
 * (TD5RE_ROAM_SCENERY=0); a tile with nothing to draw gives an empty layer. */
TD5_RoamScenery *td5_roam_scn_build(TD5_RoamScnSource *s, const RoamTile *self,
                                    TD5_RoamHeight *hc);

int  td5_roam_scn_enabled(void);                 /* TD5RE_ROAM_SCENERY (default 1) */
/* [G4] neighbour ROAD tile (tx, tz) through the source (its LRU, the file, else its builder); NULL = absent.
 * Owned by the source, valid until the next 24 asks. */
const RoamTile *td5_roam_scn_road(TD5_RoamScnSource *s, int tx, int tz);

#ifndef TD5RE_RELEASE
/* DEV ONLY. --RoamBuildScenery=casco|x0,z0,x1,z1 [--RoamOut=<dir>] [--RoamOrder=rev]
 * [--RoamThread=0|1|2] [--RoamCensus=<path>]: builds the scenery layer of those tiles
 * from the ROAMTILE files, writes SCENERY_<tx>_<tz>.BIN (the blob + record table) into
 * <dir> and a census line per tile, returns 1 (the caller exits). 0 when absent. */
int  td5_roam_scn_dev_cli(const char *cmdline);
#endif

#endif /* TD5_ROAM_SCENERY_H */
