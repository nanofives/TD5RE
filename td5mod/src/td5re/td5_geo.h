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

/* Vertical exaggeration applied to real relief, per the plan's section 3:
 * scale the real gradient, THEN let the road profile's own grade cap clip it.
 * At the default TD5RE_AUTOTRACK_GRADE (0.12) a real 8% slope becomes 12% and
 * lands exactly on the cap. Overridable with TD5RE_GEO_EXAGGERATION (x100). */
#define TD5_GEO_EXAGGERATION_X100_DEFAULT 150

#endif /* TD5_GEO_H */
