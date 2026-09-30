/**
 * td5_geo.h -- GEO TRACK: real-world terrain source for the auto-track
 *              (PORT-ONLY). See docs/plans/GEO_TRACK_OSM_PLAN.md.
 *
 * Reads a place cache produced by re/tools/geo_fetch.py -- real OpenStreetMap
 * geometry and a real DEM, projected into world units -- so the auto-track
 * generator can build a track from an actual location on Earth instead of a
 * seeded synthetic world.
 *
 * WHY THIS IS A SEPARATE MODULE. td5_tg_world.c's own header states that it is
 * the ONLY place that knows how terrain is synthesised, and all 130 of its
 * callers go through tg_world_h / tg_world_class / tg_world_is_water. Keeping
 * the file-format reader here means that contract survives: td5_tg_world.c gains
 * three small branches and learns nothing about headers, bilinear sampling or
 * cache layout.
 *
 * SELECTION. `TD5RE_GEO_PLACE=<slug>` names a directory under re/assets/geo/.
 * Unset or empty means the synthetic world, unchanged. This is deliberately an
 * env knob and not an INI key: the generator's own knobs are TD5RE_-prefixed so
 * they land in the GENSTAMP env hash, and a geo build must be distinguishable
 * from a synthetic one with the same seed.
 *
 * BYTE-IDENTITY. The geo path must never perturb a synthetic build. It adds no
 * tg_rand/tg_frand/tg_range call (the standing rule at
 * td5_trackgen_internal.h:1290-1296 -- one extra draw moves the road for every
 * existing seed), and every branch is an early test on td5_geo_loaded(), which
 * is 0 unless the knob is set. Verify with verify/topo_gen.ps1 -Seed N: a
 * synthetic build must produce byte-identical MODELS.DAT.
 *
 * FRAME. Coordinates are raw signed world units in the SAME frame as TG_Node.
 * The conditioner rotates the route so its start tangent lands on +X and
 * translates it so node 0 sits at the origin, and geo_fetch builds the rasters
 * in that same frame -- so a geo place needs no offset search and
 * td5_geo_height(0,0) is the ground under the start line. The rasters carry
 * their rotation so the agreement can be asserted rather than assumed; a
 * mismatch there once put 896 of 1451 route nodes outside their own terrain
 * with nothing raising an error.
 */
#ifndef TD5_GEO_H
#define TD5_GEO_H

/* ESA WorldCover class ids, the vocabulary COVER.R8 speaks (the cache fills it
 * from OSM landuse today and from WorldCover later; the ids do not change). */
#define TD5_GEO_COVER_NONE     0
#define TD5_GEO_COVER_TREE    10
#define TD5_GEO_COVER_SHRUB   20
#define TD5_GEO_COVER_GRASS   30
#define TD5_GEO_COVER_CROP    40
#define TD5_GEO_COVER_BUILT   50
#define TD5_GEO_COVER_BARE    60
#define TD5_GEO_COVER_SNOW    70
#define TD5_GEO_COVER_WATER   80
#define TD5_GEO_COVER_WETLAND 90

/* Module lifecycle (registered in g_td5re_modules before "trackgen", because
 * tg_world_build asks whether a place is loaded). Loading is driven entirely by
 * TD5RE_GEO_PLACE; with the knob unset init succeeds and loads nothing, which
 * costs one getenv. */
int  td5_geo_init(void);
void td5_geo_shutdown(void);

/* Load / drop a place explicitly. `slug` names re/assets/geo/<slug>/.
 * Returns 1 on success, 0 on failure (and leaves nothing loaded). */
int  td5_geo_load(const char *slug);
void td5_geo_unload(void);

int         td5_geo_loaded(void);
const char *td5_geo_place_name(void);   /* "" when nothing is loaded */
const char *td5_geo_place_slug(void);

/* ------------------------------------------------------------- sampling ---
 * All take raw world units and are safe to call off the edge of the cache:
 * outside the grid they clamp to the border sample, which keeps the terrain
 * continuous rather than dropping to zero at the boundary. Use
 * td5_geo_in_bounds to tell real data from a clamped edge. */

/* Ground height in world units, exaggerated (see TD5_GEO_EXAGGERATION) and with
 * water beds already lowered. This is what tg_w_raw returns on the geo path. */
double td5_geo_height(double x, double z);

/* Height as the DEM has it: no exaggeration, no water lowering. For the water
 * surface and for diagnostics. */
double td5_geo_height_raw_m_units(double x, double z);

/* Water surface in world units, or a value far below the terrain when this cell
 * is dry -- so `h < water_y` answers "is this water" without a second lookup. */
double td5_geo_water_y(double x, double z);
int    td5_geo_is_water(double x, double z);

/* Land-cover class, or TD5_GEO_COVER_NONE where the cache has no opinion. */
int    td5_geo_cover(double x, double z);

/* Real sea level (0 m) expressed in this cache's world units, BEFORE the
 * origin shift that tg_world_build applies. */
double td5_geo_sea_y_raw(void);

int    td5_geo_in_bounds(double x, double z);

/* Grid description, for logging and for asserting frame agreement. */
void   td5_geo_grid(int *out_w, int *out_h, double *out_cell,
                    double *out_origin_x, double *out_origin_z,
                    double *out_rotation_rad);

/* ---------------------------------------------------------------- route ---
 * [GEO PHASE 3 2026-09-30] The conditioned centerline geo_condition.py writes
 * (ROUTE.JSON: points[] of {x, z, lanes}, node 0 at the origin, a straight
 * +X lead-in, chord spacing of exactly span_length). When one is loaded,
 * tg_build_centerline pushes these nodes instead of walking sections, and the
 * track length follows the route.
 *
 * SOURCE, in order: TD5RE_GEO_ROUTE=<path to a ROUTE.JSON> (works with no place
 * loaded, over the synthetic world -- the offline fixture path), else
 * re/assets/geo/<slug>/ROUTE.JSON of the loaded place. Neither present means
 * no route and the generator walks as before, byte-identical.
 *
 * The loader VALIDATES the contract rather than trusting it: node 0 at the
 * origin, span_length matching TD5_TG_SPAN_LENGTH, and every chord within
 * tolerance of it. A route that fails is dropped with an error, never half
 * used -- uneven spacing is silently mis-measured by the engine, not rejected. */
int  td5_geo_route_count(void);            /* nodes, 0 when no route */
int  td5_geo_route_node(int i, double *x, double *z, int *lanes);
const char *td5_geo_route_source(void);    /* path it came from, "" if none */

/* ------------------------------------------------------- place selection ---
 * [GEO PHASE 4 2026-09-30] The selected place is the env knob TD5RE_GEO_PLACE,
 * mirrored to re/assets/geo/SELECTED.TXT so the browser selector
 * (re/tools/geo_selector.py) and the studio's LOCATION row agree across
 * launches: at init, an unset knob is filled from the file.
 *
 * td5_geo_sync() makes the loaded place + route match the knob. The generator
 * calls it before folding the spec (build, studio preview, streamed rederive),
 * so a LOCATION change takes effect on the next build with no restart.
 * TD5RE_GEO_ROUTE, when set, still overrides the place's own ROUTE.JSON. */
void td5_geo_sync(void);
void td5_geo_select(const char *slug);   /* "" = synthetic; writes SELECTED.TXT */

/* Places under re/assets/geo/ that carry a ROUTE.JSON (a place without a route
 * cannot be raced yet). Rescanned on td5_geo_places_rescan(); index order is
 * directory order, stable between rescans with no disk change. */
int         td5_geo_places_rescan(void);
int         td5_geo_places_count(void);
const char *td5_geo_places_slug(int i);
const char *td5_geo_places_name(int i);  /* PLACE.JSON name, else the slug */

/* Attribution every geo track must show (ODbL). */
#define TD5_GEO_CREDIT "MAP DATA (C) OPENSTREETMAP CONTRIBUTORS"

/* Vertical exaggeration applied to real relief, per the plan's section 3:
 * scale the real gradient, THEN let the road profile's own grade cap clip it.
 * At the default TD5RE_AUTOTRACK_GRADE (0.12) a real 8% slope becomes 12% and
 * lands exactly on the cap. Overridable with TD5RE_GEO_EXAGGERATION (x100). */
#define TD5_GEO_EXAGGERATION_X100_DEFAULT 150

#endif /* TD5_GEO_H */
