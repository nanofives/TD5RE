/**
 * td5_roam_tile.h -- GEO FREE ROAM: tile assembly + the ROAMTILE file
 * (PORT-ONLY, F1 task T5). See docs/plans/GEO_FREE_ROAM_DESIGN.md 1.4 - 1.10.
 *
 * WHAT IT IS. The step that turns T3's street graph + junction patches and T4's
 * ribbons into ONE tile of the ROAD layer: the lattice (ROAMGRID.JSON), the
 * per-tile build from (tile + halo), ownership, the in-memory RoamTile and the
 * ROAMTILE_<tx>_<tz>.BIN file (writer + reader). Pure builder, no file-scope
 * mutable state: a TD5_RoamTileCtx is owned by its caller (one per thread; the
 * T1 cache session it drives is the one process-wide object, so only one ctx is
 * live at a time).
 *
 * LATTICE. 256 m (110080 units) tiles rotated to the casco axes (-42.13 deg),
 * defined by re/assets/geo/<place>/roam/ROAMGRID.JSON:
 *     u =  c*(x-ox) + s*(z-oz)      v = -s*(x-ox) + c*(z-oz)
 *     tx = floor(u / S)             tz = floor(v / S)
 * The rotation is used only to pick an owner (quad by its centre-segment
 * midpoint, junction cluster by its centroid) and to bucket the locator grid.
 * Every stored coordinate is an integer of the PLACE frame minus the tile
 * origin (the lattice corner rounded with the explicit td5_roam_quant).
 *
 * BUILD OF ONE TILE. (1) window = the tile's bounding box, halo 300 m; (2) the
 * edges that touch the tile (plus the lower edges under a bridge that touches it)
 * are collected and, when one reaches outside the window, the window grows to
 * hold it and the graph is rebuilt, so the row phase and the end clusters of every
 * edge a tile owns a quad of are the true ones; (3) cluster heights, ribbons
 * (T4), bridge crossings, patch heights; (4) only the owned quads / clusters are
 * relocated into the tile table. Iteration is over sorted arrays only; nothing
 * reads a load order, a clock or a pointer.
 *
 * FILE (little endian, sections 16-byte aligned, offsets from the file start):
 *   header 64 B { "TD5ROAM1", version, rules, input_hash, tx, tz, org_x, org_z,
 *                 nsect, reserved[6] }
 *   section table nsect x { tag, offset, bytes, count }
 *   SPAN TD5_StripSpan[]     the STRIP.DAT record; origin_x/z TILE-LOCAL, origin_y
 *                            absolute; vertex indices / links relocated into this
 *                            tile (link_next/prev = neighbour quad of the same
 *                            ribbon IN THIS TILE or -1)
 *   VERT TD5_StripVertex[]   int16 offsets from the owning span's / tri's origin
 *   TRI  RoamTri[]           junction patch triangles (vertices in VERT, per-tri
 *                            origin = the cluster centroid, tile-local x/z; when a
 *                            patch reaches > 76 m from it (int16 limit, wide UNION
 *                            patches) the midpoint of the patch bounding box, so the
 *                            vertex origin is ALWAYS RoamTri.ox/oy/oz, never NODE.x/y/z)
 *   BEDG RoamBEdge[]         wall edges (kerbs, deck sides, dead-end caps)
 *   EDGE RoamEdgeRec[]       one record per contiguous run of owned quads
 *   NODE RoamNodeRec[]       owned junction clusters
 *   ARM  RoamArmRec[]        their arms
 *   GRID u32 nx,nz,cell,margin,pad ; u32 start[nx*nz+1] ; u32 item[]
 *   SIGN RoamSignRec[]       traffic signals at owned clusters
 *   NAME NUL-separated street names (sorted), EDGE.name_id indexes them
 *   STMP build identity text (deterministic: no time, no exe hash)
 *   trailer u32: FNV-1a of every byte before it
 *
 * DEVIATIONS FROM DESIGN 1.7 (also in the commit message): EDGE carries the two
 * end keys (cluster key for a CLUSTER end, node key otherwise) instead of two
 * uint32 node indices; the file header is 64 B with reserved[6]; GRID items are
 * uint32 (kind in the top 2 bits: 0 span, 1 tri, 2 bedge), the grid margin is a
 * header field sized from the tile's measured overhang (>= 1 cell) instead of a
 * fixed one cell, and GRID also indexes BEDG so the locator needs no side table.
 */
#ifndef TD5_ROAM_TILE_H
#define TD5_ROAM_TILE_H

#include <stddef.h>
#include <stdint.h>
#include "td5_types.h"          /* TD5_StripSpan, TD5_StripVertex */

#define ROAM_FORMAT_VERSION   1u
/* Bumped BY HAND when a build rule changes (design 5.1 item 4). The determinism
 * harness (verify/roam/determinism.py) is the guard that it was bumped. */
#define ROAM_RULES_VERSION    2u    /* 2: F1-T9 divided-carriageway lanes, short-ribbon
                                     * pins, absorbed edges on the patch, SPLIT fallbacks,
                                     * ROW_COVERED pins at H_c */
#define ROAM_TILE_UNITS       110080          /* 256 m x 430                    */
#define ROAM_GRID_CELL        3440            /* 8 m locator cell               */
#define ROAM_GRID_CELLS       32              /* cells per tile side, no margin */
#define ROAM_HALO_M           300.0           /* build halo (T3 locality is exact there) */
#define ROAM_SPREAD_CLAMP_M   0.5             /* design risk 3 arm-blend clamp  */
#define ROAM_NSECT            11

#define ROAM_SECT_SPAN  0x4E415053u   /* 'SPAN' little endian */
#define ROAM_SECT_VERT  0x54524556u   /* 'VERT' */
#define ROAM_SECT_TRI   0x20495254u   /* 'TRI ' */
#define ROAM_SECT_BEDG  0x47444542u   /* 'BEDG' */
#define ROAM_SECT_EDGE  0x45474445u   /* 'EDGE' */
#define ROAM_SECT_NODE  0x45444F4Eu   /* 'NODE' */
#define ROAM_SECT_ARM   0x204D5241u   /* 'ARM ' */
#define ROAM_SECT_GRID  0x44495247u   /* 'GRID' */
#define ROAM_SECT_SIGN  0x4E474953u   /* 'SIGN' */
#define ROAM_SECT_NAME  0x454D414Eu   /* 'NAME' */
#define ROAM_SECT_STMP  0x504D5453u   /* 'STMP' */

/* GRID item: (kind << 30) | index */
#define ROAM_ITEM_SPAN   0u
#define ROAM_ITEM_TRI    1u
#define ROAM_ITEM_BEDGE  2u
#define ROAM_ITEM_KIND(it)  ((uint32_t)(it) >> 30)
#define ROAM_ITEM_INDEX(it) ((uint32_t)(it) & 0x3FFFFFFFu)

/* BEDG.prim: span index, or (ROAM_PRIM_TRI | tri index) */
#define ROAM_PRIM_TRI    0x8000u
/* BEDG.kind */
#define ROAM_BEDGE_KERB  1    /* kerb / patch kerb / dead-end cap               */
#define ROAM_BEDGE_DECK  2    /* side of a bridge deck (layer >= 1)             */

/* EDGE.flags */
#define ROAM_EF_DECK        0x01u   /* bridge, layer >= 1                       */
#define ROAM_EF_BORE        0x02u   /* tunnel = yes                             */
#define ROAM_EF_PASSAGE     0x04u   /* tunnel = building_passage                */
#define ROAM_EF_ROUNDABOUT  0x08u
#define ROAM_EF_RAMP_SHORT  0x10u   /* T4 flags of the ribbon                   */
#define ROAM_EF_PIN_MISS    0x20u
/* EDGE.end_kind[] values: TD5_ROAM_END_* of td5_roam_world.h */

/* SIGN.kind */
#define ROAM_SIGN_SIGNAL    1

#pragma pack(push, 1)
typedef struct { uint16_t v[3]; uint16_t patch; uint8_t surface, flags;
                 int32_t ox, oy, oz; } RoamTri;                          /* 22 B */
typedef struct { uint16_t v[2]; uint16_t prim; uint8_t kind, layer; } RoamBEdge;  /* 8 B */
typedef struct {
    uint64_t key;                 /* (way_id << 16) | first vertex index        */
    int64_t  end_key[2];          /* cluster key (CLUSTER end) else node key    */
    uint8_t  klass, lanes, oneway, surface;
    int8_t   layer;  uint8_t flags;  uint16_t name_id;      /* 0xFFFF = unnamed */
    int32_t  length;              /* whole edge, units                          */
    uint16_t span_first, span_count, row0;
    uint8_t  end_kind[2];
    uint8_t  maxspeed_kph;  uint8_t pad;
    uint16_t pad2;
} RoamEdgeRec;                                                           /* 48 B */
typedef struct {
    int64_t  cluster_key;
    int32_t  x, y, z;             /* centroid x/z tile-local, y = patch centre  */
    uint8_t  narm, pad0;  uint16_t flags;                    /* TD5_ROAM_CL_*   */
    uint16_t tri_first, tri_count, arm_first, vert_first;
} RoamNodeRec;                                                           /* 32 B */
typedef struct {
    uint64_t edge_key;
    int16_t  bearing12;           /* 4096 = full circle, atan2(dz, dx) of the arm */
    uint16_t cut_span;            /* first owned quad of the arm's edge, 0xFFFF   */
    uint8_t  end, lanes, flags, pad;
} RoamArmRec;                                                            /* 16 B */
typedef struct { int64_t cluster_key; uint8_t arm, kind; uint16_t pad; } RoamSignRec;  /* 12 B */
#pragma pack(pop)

_Static_assert(sizeof(RoamTri) == 22, "RoamTri is a file record");
_Static_assert(sizeof(RoamBEdge) == 8, "RoamBEdge is a file record");
_Static_assert(sizeof(RoamEdgeRec) == 48, "RoamEdgeRec is a file record");
_Static_assert(sizeof(RoamNodeRec) == 32, "RoamNodeRec is a file record");
_Static_assert(sizeof(RoamArmRec) == 16, "RoamArmRec is a file record");
_Static_assert(sizeof(RoamSignRec) == 12, "RoamSignRec is a file record");

/* ------------------------------------------------------------ lattice -------- */

typedef struct {
    char    place[64];
    double  ox, oz;               /* lattice origin, place frame                */
    double  c, s;                 /* cos / sin of the lattice angle, exact pair */
    double  angle_deg;            /* informational; c, s are the authority      */
    int32_t tile;                 /* tile side, units                           */
} RoamGrid;

/* Lattice coordinates (units, relative to the lattice origin). */
void roam_grid_uv(const RoamGrid *g, double x, double z, double *u, double *v);
/* The tile that owns a place-frame point. */
void roam_grid_tile_of(const RoamGrid *g, double x, double z, int *tx, int *tz);
/* The four corners of tile (tx,tz), place frame, CCW from the (0,0) corner. */
void roam_grid_corners(const RoamGrid *g, int tx, int tz, double out[4][2]);
/* Tile origin: the (0,0) corner rounded with td5_roam_quant. */
void roam_grid_origin(const RoamGrid *g, int tx, int tz, int32_t *ox, int32_t *oz);
/* Parse the bytes of a ROAMGRID.JSON. 1 on success. */
int  roam_grid_parse(const char *json, RoamGrid *g);
/* Text of a ROAMGRID.JSON for these parameters (17 significant digits). */
int  roam_grid_format(const RoamGrid *g, char *out, size_t cap);
/* The casco default: S = 110080, angle -42.13 deg, origin (0, 0). */
void roam_grid_default(const char *place, RoamGrid *g);

/* ------------------------------------------------------------ the tile ------- */

typedef struct {
    int32_t   tx, tz;
    int32_t   org_x, org_z;       /* tile origin, PLACE frame units             */
    uint32_t  rules, stamp;       /* ROAM_RULES_VERSION, input hash             */
    TD5_StripSpan   *span;   uint32_t nspan;
    TD5_StripVertex *vert;   uint32_t nvert;
    RoamTri         *tri;    uint32_t ntri;
    RoamBEdge       *bedge;  uint32_t nbedge;
    RoamEdgeRec     *edge;   uint32_t nedge;
    RoamNodeRec     *node;   uint32_t nnode;
    RoamArmRec      *arm;    uint32_t narm;
    RoamSignRec     *sign;   uint32_t nsign;
    uint32_t        *grid;   uint32_t ngrid_words;   /* raw GRID section        */
    uint32_t         grid_nx, grid_nz, grid_cell, grid_margin;
    const uint32_t  *grid_start;                     /* nx*nz+1                 */
    const uint32_t  *grid_item;  uint32_t nitem;
    const char      *names;  uint32_t names_bytes, nnames;
    const char      *stamp_text;
    uint8_t         *arena;  size_t arena_bytes;     /* ONE allocation = the file image */
} RoamTile;

void td5_roam_tile_free(RoamTile *t);

/* Parse a file image (takes ownership of `image` on success; the caller frees it
 * on failure). Checks magic, version, rules, section table bounds, trailer. When
 * expect_hash != 0 the header input_hash must match. Returns 1, or 0 with a
 * reason in err. */
int  td5_roam_tile_from_image(RoamTile *t, uint8_t *image, size_t bytes,
                              uint32_t expect_hash, char *err, size_t errn);
int  td5_roam_tile_read(const char *path, RoamTile *t, uint32_t expect_hash,
                        char *err, size_t errn);
int  td5_roam_tile_write(const char *path, const RoamTile *t);
/* "re/assets/geo/<place>/roam/ROAMTILE_<tx>_<tz>.BIN" under `dir` (NULL = default) */
void td5_roam_tile_path(char *out, size_t cap, const char *dir, const char *place,
                        int tx, int tz);
/* Create the directory (and parents), 1 when it exists afterwards. */
int  td5_roam_mkdirs(const char *dir);

/* ------------------------------------------------------------ the builder ---- */

typedef struct {
    /* what was built */
    int      quads, tris, bedges, verts, edges_rec, clusters, arms, signals;
    uint32_t bytes;
    int      passes;              /* 1, or 2 when the window had to grow         */
    int      edges_built;         /* ribbons built (touching + bridge partners)  */
    int      cache_tiles;         /* cache tiles in the final window             */
    /* quality / risk counters */
    int      ribbon_bad, ribbon_height_fail, ribbon_absorbed;
    int      ramp_short, ramp_fail, pin_miss;
    int      cluster_fail;        /* TD5_ROAM_CL_FAIL: no triangles              */
    int      clusters_no_height;  /* a ground sample was missing                 */
    int      spread_clusters;     /* ground spread > ROAM_SPREAD_CLAMP_M         */
    double   max_spread_m;        /* max (max-min) of a cluster's ground samples */
    double   max_blend_m;         /* max |H_c - ground at an arm cut row|        */
    double   max_patch_dy_m;      /* max |arm cut-row y - patch centre y|        */
    int      arm_clamped;         /* arms whose pin was clamped to ground +- 0.5 m */
    /* per owned cluster, buckets <=0.5 m, <=1, <=2, <=4, >4: ground spread, and the
     * patch crest (largest |arm cut-row y - patch centre y|) */
    int      spread_hist[5], pdy_hist[5];
    int      cut_checked, cut_mismatch;   /* patch cut vertex vs ribbon row      */
    int      i16_overflow;        /* a stored offset left int16                  */
    int      i16_reorigin;        /* clusters whose vertex origin moved off the centroid (F1-T5b) */
    int      bedge_no_tri;        /* outline segment found in no triangle        */
    int      absorbed_row_edges;  /* absorbed edges that needed a shared row y   */
    double   max_overhang_m;      /* furthest an owned vertex lies outside the tile */
    int      border_quads;        /* owned quads with a vertex outside the tile  */
    int      hug_quads;           /* owned quad midpoints within 2 m of a border */
    int      hug_phase[16];       /* same, for 16 lattice phases (S/4 steps)     */
    int      overflow;            /* > 32767 spans/tris or > 65535 vertices      */
    uint32_t ms;                  /* wall time of the build (not in the file)    */
    uint32_t ms_graph, ms_ribbon, ms_assemble;
} TD5_RoamTileStats;

typedef struct TD5_RoamTileCtx TD5_RoamTileCtx;

/* Open a ctx for `place`: reads ROAMGRID.JSON (writes the default when absent and
 * `create_grid`), PLACE.JSON, tiles/INDEX.JSON, the raster headers, and begins the
 * T1 cache session. NULL on failure. `global_win` (place frame {minx,minz,maxx,maxz},
 * may be NULL) selects GLOBAL mode: the graph and every ribbon are built ONCE over
 * that window and tiles are sliced out of it (a cross-check arm for the windowed
 * build, not the production path). */
TD5_RoamTileCtx *td5_roam_tile_ctx_open(const char *place, int create_grid,
                                        const double *global_win);
void td5_roam_tile_ctx_close(TD5_RoamTileCtx *c);
const RoamGrid *td5_roam_tile_ctx_grid(const TD5_RoamTileCtx *c);

/* Build tile (tx,tz). On success *out owns one arena (free with
 * td5_roam_tile_free) and *st (optional) holds the census. Returns 1, or 0 on
 * out of memory / a missing raster window. */
int  td5_roam_tile_build(TD5_RoamTileCtx *c, int tx, int tz, RoamTile *out,
                         TD5_RoamTileStats *st);

/* Tile indices whose square meets the casco polygon (F0a's four ring corners),
 * sorted by (tx, tz). Returns the count written (<= cap). */
int  td5_roam_casco_tiles(const RoamGrid *g, int (*out)[2], int cap);

#ifndef TD5RE_RELEASE
/* DEV ONLY. Scans the command line for --RoamBuildTiles=x0,z0,x1,z1 | casco and
 * runs the tile builder: writes the files, a census (log + roam_tile_census.txt)
 * and returns 1 (the caller exits). Other flags: --RoamOut=<dir>, --RoamOrder=
 * fwd|rev, --RoamThread=0|1|2 (1 = worker with the main FP env applied, 2 = worker
 * WITHOUT it, the negative control), --RoamGlobal=1, --RoamPlace=<slug>,
 * --RoamCensus=<path>. 0 when --RoamBuildTiles is absent. */
int  td5_roam_tile_dev_cli(const char *cmdline);
#endif

#endif /* TD5_ROAM_TILE_H */
