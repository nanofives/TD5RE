/**
 * td5_tg_world.h -- auto-track WORLD: the terrain that exists BEFORE the road.
 *
 * [TOPOLOGY-FIRST 2026-09-08] The generator used to be road-first: the walk
 * laid XZ, a sine profile invented Y, bridges/tunnels were hashed onto spans
 * and "terrain" was a per-span drop profile hung off the road edge. This
 * module inverts that. A seeded, PURE height function h_base(x,z) describes a
 * whole region (continental shelf + mountain ridges + hills + detail, a sea
 * level below which everything is water, river channels carved into the
 * land), and the road is laid ONTO it (td5_tg_road.c), streets are planned
 * on it (td5_tg_network.c) and scenery samples it.
 *
 * REPRESENTATION. Not a dense lattice: the walk keeps its heading within
 * +-80..88 deg of +Z but X can drift 0.98 units per unit of Z, so a
 * 3000-span track can cover +-4.5M world units in BOTH axes and a dense
 * 1500-unit grid over that is 36 MB. Instead:
 *   - h_base(x,z) is evaluated on demand (value-noise fBm, no storage);
 *   - a SPARSE OVERLAY of 32x32-cell chunks (cell = one span = 1500 units)
 *     is allocated only where something wrote to it -- road-bed conform
 *     (cut/fill), occupancy bits (ROAD/STREET/DRIVABLE/...), painted biome.
 *   Memory follows road length (~1-3 MB), not extent.
 *
 * COORDINATES. World units, same frame as TG_Node. The start line is at the
 * origin and must sit at y = 0 on flat land (the grid spawns cars at y~0):
 * tg_world_build searches a deterministic spiral of sampling offsets until it
 * finds one where the origin is dry, flat and the +Z lead-in is walkable, and
 * subtracts h at the origin so tg_world_h(0,0) == 0 exactly. Sea level is
 * therefore a NEGATIVE number on most seeds.
 *
 * DETERMINISM. Everything here is a pure function of (seed, TD5RE_* knobs):
 * no tg_rand() (the walk owns that stream), no clock. Reads are thread-safe
 * after tg_world_freeze(): a frozen world never allocates, a missing chunk
 * reads as "base terrain, no occupancy". The MT terrain side-buffer prepass
 * reads this 8-way, so freezing before scenery is mandatory.
 */
#ifndef TD5_TG_WORLD_H
#define TD5_TG_WORLD_H

/* Terrain class at a point. Ordered so that "at least this rough" compares. */
typedef enum {
    TG_WC_SEA = 0,      /* below sea level                                   */
    TG_WC_RIVER,        /* inside a carved river channel (own water surface) */
    TG_WC_SHORE,        /* dry, within TG_WORLD_SHORE_BAND above the sea     */
    TG_WC_FLAT,         /* slope < TG_WORLD_HILL_SLOPE                       */
    TG_WC_HILL,         /* slope < TG_WORLD_STEEP_SLOPE                      */
    TG_WC_MOUNTAIN      /* slope >= TG_WORLD_STEEP_SLOPE (tan 30 deg)        */
} TG_WorldClass;

/* Occupancy bits held per overlay cell. TG_WO_ prefix: TG_OCC_* is already
 * the lateral-reach mask in td5_tg_streets.c and must not be confused with
 * a raster bit. */
enum {
    TG_WO_ROAD     = 0x01,   /* main carriageway (+margin)                   */
    TG_WO_STREET   = 0x02,   /* scenery street tarmac                        */
    TG_WO_DRIVABLE = 0x04,   /* a fork corridor / drivable street            */
    TG_WO_WATER    = 0x08,   /* cached: cell centre is water                 */
    TG_WO_STEEP    = 0x10,   /* cached: cell slope >= TG_WORLD_STEEP_SLOPE   */
    TG_WO_NEAR     = 0x20,   /* within FAR_REACH of some road (trim keeps)   */
    TG_WO_RESERVED = 0x40    /* planner reservation (junction, landmark)     */
};

#define TG_WORLD_CELL          1500.0   /* one span                           */
#define TG_WORLD_CHUNK         32       /* cells per chunk side               */
#define TG_WORLD_STEEP_SLOPE   0.5774   /* tan 30 deg: road cannot follow     */
#define TG_WORLD_HILL_SLOPE    0.12     /* above this the road must cut/fill  */
#define TG_WORLD_SHORE_BAND    1200.0   /* dry land this close to sea = SHORE */

/* Lifecycle. build() is called once per generate, BEFORE tg_srand, and reads
 * only the seed + knobs. free() at the end of every build path (the regen
 * selfcheck rebuilds twice in one process and must not inherit chunks). */
int  tg_world_build(unsigned int seed, int target_spans);
void tg_world_free(void);
int  tg_world_ready(void);
void tg_world_freeze(void);      /* reads become pure; writes are refused    */

/* Height authorities. h = base(x,z) blended with any conform overlay. */
double tg_world_h(double x, double z);
double tg_world_h_base(double x, double z);
double tg_world_sea_y(void);
/* Water surface at (x,z): the river surface inside a channel, else the sea
 * level. Water exists where tg_world_h < tg_world_water_y. */
double tg_world_water_y(double x, double z);
int    tg_world_is_water(double x, double z);
/* |grad h| by central difference over one cell. */
double tg_world_slope(double x, double z);
/* Directional slope of the BASE terrain along unit (dx,dz): (h(p+d)-h(p-d))/2d.
 * Signed: positive = rising in that direction. */
double tg_world_slope_along(double x, double z, double dx, double dz);
TG_WorldClass tg_world_class(double x, double z);

/* Conform: ask the terrain to meet height y at (x,z), fully inside r_flat and
 * fading to the natural surface at r_flat + r_blend. Max-compositing on the
 * per-cell weight, so overlapping stamps never fight. Segment form walks the
 * segment in half-cell steps. Refused (no-op) after freeze. */
void tg_world_conform(double x, double z, double y, double r_flat, double r_blend);
void tg_world_conform_seg(double x0, double z0, double y0,
                          double x1, double z1, double y1,
                          double r_flat, double r_blend);

/* Occupancy raster (per cell). set/seg/disc OR bits in; occ() reads them. */
unsigned tg_world_occ(double x, double z);
void     tg_world_occ_set(double x, double z, unsigned bits);
void     tg_world_occ_disc(double x, double z, double radius, unsigned bits);
void     tg_world_occ_seg(double x0, double z0, double x1, double z1,
                          double half_w, unsigned bits);
/* Does any cell within `radius` of (x,z) carry any of `bits`? */
int      tg_world_occ_near(double x, double z, double radius, unsigned bits);

/* Painted biome (hard cell index) per cell; -1 where nothing painted. */
int  tg_world_biome(double x, double z);
void tg_world_biome_paint(double x, double z, double radius, int biome);

/* Diagnostics: chunk count / bytes, and the dev heightmap dump
 * (TD5RE_TG_WORLD_DUMP=1 -> <dir>/WORLD.PGM over the road's bounding box). */
int  tg_world_chunk_count(void);
void tg_world_log_stats(const char *tag, double x0, double z0, double x1, double z1);
void tg_world_dump(const char *dir, double x0, double z0, double x1, double z1);

#endif /* TD5_TG_WORLD_H */
