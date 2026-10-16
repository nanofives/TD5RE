/**
 * td5_roam_locate.h -- GEO FREE ROAM: the road locator (PORT-ONLY, F1 task T6b).
 * See docs/plans/GEO_FREE_ROAM_DESIGN.md 1.9 and 2.1 - 2.5.
 *
 * WHAT IT IS. Given a point (and optionally the probe's Y and the previous hit),
 * find the drivable primitive of the resident ROAD tiles under it and answer what
 * the physics contact path needs: ground height, unit normal, surface, the street
 * piece, lateral position, an inside flag and, when outside, the push to the
 * nearest BOUNDARY edge (no lateral-projection hack: design 2.4).
 *
 * HEIGHTS ARE THE STRIP'S OWN INTEGER MATH. A ribbon quad goes through
 * td5_track_quad_height_v and a junction-patch triangle through
 * td5_track_tri_height_v (T6a), so the result is bit-identical to what the strip
 * code returns for the same vertices. Patch triangles are T3's CCW north-up order,
 * which makes the plane normal point DOWN (ny < 0); the locator swaps the two
 * trailing vertices when it evaluates one so the normal points up. Same plane,
 * same integer height (the solve divides two negated numbers), exact normal sign.
 *
 * NO FILE-SCOPE MUTABLE STATE. Everything lives in a TD5_RoamResident the caller
 * owns (the tiles themselves stay the caller's: the set only points at them).
 * Reads are const, so any number of threads may locate against one set while no
 * thread adds or removes tiles.
 *
 * FRAMES. The caller's WORLD frame is the place frame shifted by (org_x, org_z,
 * org_y), all in integer world units (1/430 m):  place = FP_TRUNC(world) + org.
 * Inputs and ground_y are 24.8 world; push_* / lateral / along are plain integer
 * world units (the strip's geo_locate convention).
 */
#ifndef TD5_ROAM_LOCATE_H
#define TD5_ROAM_LOCATE_H

#include <stddef.h>
#include <stdint.h>
#include "td5_roam_tile.h"

#define ROAM_PRIM_NONE   0xFFFFFFFFu
#define ROAM_RES_MAX     1024        /* resident slots (slot lives in 12 bits of a prim id) */
#define ROAM_RES_HASH    4096        /* (tx,tz) -> slot table, power of two, > 2 x RES_MAX  */
#define ROAM_LOC_CAND    32          /* containing primitives gathered per probe            */

/* prim id: (slot << 20) | (is_tri << 19) | index */
#define ROAM_PRIM_MAKE(slot, is_tri, idx) \
    (((uint32_t)(slot) << 20) | ((uint32_t)((is_tri) ? 1 : 0) << 19) | ((uint32_t)(idx) & 0x7FFFFu))
#define ROAM_PRIM_SLOT(p)    ((uint32_t)(p) >> 20)
#define ROAM_PRIM_IS_TRI(p)  (((uint32_t)(p) >> 19) & 1u)
#define ROAM_PRIM_INDEX(p)   ((uint32_t)(p) & 0x7FFFFu)

#define ROAM_STEP_UP         (430 * 256)        /* 24.8: a probe climbs 1 m onto a surface   */
#define ROAM_DECK_DROP       (645 * 256)        /* 24.8: 1.5 m below a deck probe = off it  */
#define ROAM_KERB_OVERHANG   150                /* units, 0.35 m: wall code uses it (2.5)   */

typedef struct {
    uint32_t prim;        /* ROAM_PRIM_NONE when nothing was found                          */
    uint64_t edge_key;    /* street piece (way_id << 16 | first vertex) or cluster key      */
    int32_t  ground_y;    /* 24.8 world, exactly triangle_height()'s return                 */
    int16_t  normal[3];   /* int16 unit normal (4096), exactly triangle_height()'s          */
    uint8_t  surface;     /* grip nibble (ribbon: span attr & 15 as the strip does)         */
    uint8_t  inside;      /* on the drivable surface of the chosen deck                     */
    uint8_t  capped;      /* the upward extrapolation cap fired (same rule as the strip)    */
    uint8_t  is_patch;
    int8_t   lane;        /* ribbon lane, -1 on a patch                                     */
    int8_t   layer;       /* ribbon edge layer; 0 on a patch                                */
    int32_t  along;       /* units from row 0 of the edge (ribbon), 0 on a patch            */
    int32_t  lateral;     /* signed offset from the ribbon centreline, + toward higher lane */
    int32_t  push_x, push_z, push_dist;   /* to the nearest boundary edge when !inside      */
    uint32_t wall;        /* BEDG index of that boundary edge, in prim's tile (outside only) */
} TD5_RoamHit;

/* The resident set. Caller-owned, no allocation inside. Slots are stable: removing a
 * tile leaves a hole that the next add reuses, so an outstanding hint either still
 * names its tile or names an empty slot (the locator treats that as "no hint"). */
typedef struct TD5_RoamResident {
    RoamGrid         grid;                    /* the lattice (ROAMGRID.JSON)                */
    int32_t          org_x, org_z, org_y;     /* place coordinates of the world origin      */
    const RoamTile  *tile[ROAM_RES_MAX];      /* NULL = free slot                           */
    int              nslot;                   /* high-water mark of used slots              */
    int              ntile;                   /* live tiles                                 */
    double           max_margin;              /* widest tile grid margin, lattice units     */
    int16_t          hash[ROAM_RES_HASH];     /* open addressing, -1 = empty                */
} TD5_RoamResident;

void td5_roam_resident_init(TD5_RoamResident *rs, const RoamGrid *g,
                            int32_t org_x, int32_t org_z, int32_t org_y);
/* Add a tile (the pointer must outlive its residency). Slot, or -1 when full / the
 * (tx,tz) is already resident. */
int  td5_roam_resident_add(TD5_RoamResident *rs, const RoamTile *t);
/* Remove the tile at (tx,tz). 1 when it was resident. */
int  td5_roam_resident_remove(TD5_RoamResident *rs, int tx, int tz);
/* Slot of (tx,tz) or -1. */
int  td5_roam_resident_find(const TD5_RoamResident *rs, int tx, int tz);

/* x, z, y in 24.8 WORLD frame. has_y = 0 for XZ-only callers (camera, shadow).
 * hint = the previous hit's prim for continuity (ROAM_PRIM_NONE ok). Returns 0 only
 * when no resident tile covers the point (or holds a single wall to push toward);
 * *out is always written. */
int td5_roam_locate(const TD5_RoamResident *rs, int32_t x, int32_t z, int32_t y,
                    int has_y, uint32_t hint, TD5_RoamHit *out);

/* One containing primitive, for callers that want the whole stack (bridge decks). */
typedef struct {
    uint32_t prim;
    int32_t  h;           /* 24.8 world */
    int16_t  normal[3];
    uint8_t  lane, capped;
} TD5_RoamCand;
/* Every drivable primitive containing the point (edges inclusive), up to cap, in a
 * resident-order-independent order (tz, tx, span before tri, index). Returns the
 * count written. */
int td5_roam_collect(const TD5_RoamResident *rs, int32_t x, int32_t z,
                     TD5_RoamCand *out, int cap);

/* The plane of one named primitive evaluated at (x, z): height, normal, surface,
 * lane, lateral info; inside = the point is on that primitive. The same code the
 * locator's outside path uses (a wheel past a kerb reads the extrapolated plane).
 * 1 on success, 0 for a bad prim id. */
int td5_roam_prim_eval(const TD5_RoamResident *rs, uint32_t prim, int32_t x, int32_t z,
                       TD5_RoamHit *out);

/* Percentage (0..100) of boundary edge `bi` of resident slot `slot` that lies inside another drivable
 * primitive of about the same level (sampled every 11 cm). The wall rule is the boundary of the UNION
 * of the drivable surface: an edge, or the part of one, inside another prim (overlapping carriageways,
 * Y splits side by side) is not a wall and td5_roam_locate never pushes toward it. 100 = never a wall. */
int td5_roam_wall_coverage(const TD5_RoamResident *rs, uint32_t slot, uint32_t bi);

/* Layer of a primitive (ribbon edge layer; 0 for a patch), INT8_MIN for a bad id. */
int td5_roam_prim_layer(const TD5_RoamResident *rs, uint32_t prim);
/* The EDGE record of a ribbon prim, or NULL (patch / bad id). */
const RoamEdgeRec *td5_roam_prim_edge(const TD5_RoamResident *rs, uint32_t prim);

#ifndef TD5RE_RELEASE
/* DEV ONLY. TD5RE_ROAM_LOCATE_TEST=1: loads every casco ROAMTILE of the place, runs
 * the locator self-test (centroids, outside pushes, tile borders, timing, bridge
 * decks), writes <log dir>/roam_locate_test.txt and returns 1 (the caller exits).
 * 0 when the knob is off. */
int td5_roam_locate_dev_test(void);
#endif

#endif /* TD5_ROAM_LOCATE_H */
