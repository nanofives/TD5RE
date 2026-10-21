/**
 * td5_tg_geo_bld.h -- GEO: the REAL-BUILDING extrusion core, shared by the strip
 * geo track and free roam (PORT-ONLY, roam F4a). See docs/plans/GEO_FREE_ROAM_DESIGN.md 4.3.
 *
 * WHAT IS SHARED. tg_geo_emit_one (td5_tg_city.c) used to be one function that both
 * cleared a footprint against its HOST SPAN and extruded it. The clearance half is
 * span-bound (host span frame, laid side streets, fork corridors) and stays in the
 * strip wrapper. The other half -- landmark apron, shipped set-piece fallback, height
 * and storeys, facade/roof pages, roof shape, walls with a per-vertex foundation, the
 * MODELS.DAT mesh write -- is a pure function of the (already cleared) ring plus the
 * few environment facts below, and is the core both callers run.
 *
 * STRIP BYTE IDENTITY. The strip wrapper fills the environment with exactly the values
 * the old code computed in place (tg_world_h, the host span's road height + kerb + 400
 * ceiling, the td5_geolm_* answers, td5_geob_storey_units...), in the same order, so the
 * emitted bytes are unchanged (geo_level_identity.py on level091).
 *
 * THREADING. The core keeps no file-scope state: every counter goes to e->st, every
 * scratch array is on the stack. Roam builds tiles on the stream worker with it.
 */
#ifndef TD5_TG_GEO_BLD_H
#define TD5_TG_GEO_BLD_H

#include "td5_trackgen_internal.h"
#include "td5_geo_buildings.h"

/* What one or more core calls did. Summed into the strip census by the strip
 * wrapper, kept per tile by roam. */
typedef struct {
    long   emitted, measured, estimated, parts, lm_real, lm_fallback;
    long   lm_prefab, lm_nofit, lm_styled, lm_apron, lm_apron_quads;
    long   lm_apron_pulled, lm_apron_refused;
    long   dropped_deg;
    long   roof_shaped, roof_concave, roof_kind[TD5_GEOB_ROOF_SKILLION + 1];
    long   found, fp_overture, h_overture, h_raster;
    long   h_n, h_levels, h_off;
    double h_err_sum, h_err_max, found_max, lm_apron_dev;
    /* [roam JC] the Simple 3D Buildings path (td5_tg_s3d.c); only free roam fills these */
    long   s3d_n, s3d_kind[TD5_GEOB_R3__N], s3d_how[6], s3d_roof_only, s3d_tint_roof, s3d_tint_wall;
    long   s3d_hidden, s3d_across;
} TG_GeoBldStats;

typedef struct {
    TG_Buf  *blk;             /* where meshes go: moff[(*nmesh)++] = blk->len before each */
    size_t  *moff;
    int     *nmesh, maxmesh;
    const TG_Biome *b;        /* facade cell, floor height, kerb */
    /* ground height (no kerb) at a world point, in the caller's frame */
    double (*ground)(void *u, double x, double z);
    void    *u;               /* passed to every callback                             */
    int      have_ceiling;    /* base-height ceiling of an extruded footprint          */
    double   ceiling_y;       /*   strip: host road y + kerb + 400                      */
    int      have_pf_ceiling; /* same, for a stamped set piece                          */
    double   pf_ceiling_y;
    double   upm;             /* units per metre (> 1), 430 when the place has none     */
    double   storey;          /* td5_geob_storey_units(): world units per OSM storey    */
    int      lm_kind;         /* TD5_GEOB_LMK_* of this footprint (td5_geolm_kind)      */
    int      lm_anchor;       /* the footprint is a landmark cluster anchor              */
    int      no_prefab;       /* 1: never stamp a shipped TD5 set piece (roam IB): the extruded ring stands */
    /* set-piece fallback: called once before the fit (may be NULL); a fitted corner
     * must pass corner_ok (NULL = any); the fitted rectangle must not stand on a road
     * (rect_on_road returns 1 = refuse; NULL = never) */
    void   (*prefab_begin)(void *u);
    int    (*corner_ok)(void *u, double x, double z);
    int    (*rect_on_road)(void *u, double cx, double cz, double ux, double uz,
                           double hx, double hz);
    /* apron vertex acceptable at (x, z) (NULL = any) */
    int    (*apron_ok)(void *u, double x, double z);
    int      si;              /* span for tg_acct + the [GEO LM] note; -1 = none        */
    int      acct;            /* call tg_acct                                           */
    TG_GeoBldStats *st;
    /* [roam F4b] apron piece (4 corners) acceptable (NULL = the strip: one quad per hull
     * edge, unchecked between its vertices). When set, each apron quad is cut into
     * pieces of at most one span length along the hull and a refused piece is left out,
     * so a long hull edge never lays its band across a road between two vertices. */
    int    (*apron_quad_ok)(void *u, const double *qx, const double *qz);
    /* [roam JC] 1: a building:part or a roof:shape-tagged footprint is built by the Simple 3D
     * Buildings builder (tg_s3d_build). The strip leaves it 0: its bytes cannot move. */
    int      s3d;
    /* [roam JC] 1: lay the landmark apron only, not the mass (an outline its building:parts
     * replace, Simple 3D Buildings: the outline is not rendered when parts exist) */
    int      s3d_skip_mass;
} TG_GeoBldEnv;

/* Extrude ONE cleared footprint (ring rx/rz[n_ring], CCW, centroid cx/cz, already moved
 * by `shift` for the note). Returns 0 only on a buffer write failure; a refusal is a
 * counted success. */
int tg_geo_emit_building_core(const TG_GeoBldEnv *e, const TD5_GeoBuilding *gb,
                              const double *rx, const double *rz, int n_ring,
                              double cx, double cz, double shift);

/* [roam JC] OSM Simple 3D Buildings mesh (td5_tg_s3d.c). */
#define TG_S3D_HOW_FLAT          0   /* flat / untagged: the eaves capped */
#define TG_S3D_HOW_RIDGE         1   /* gabled, hipped, half-hipped, skillion, gambrel on a convex ring */
#define TG_S3D_HOW_BARREL        2   /* round: a vault */
#define TG_S3D_HOW_CENTROID      3   /* pyramidal, cone, dome, onion on a star-shaped ring */
#define TG_S3D_HOW_OFFSET        4   /* mansard, or a sloped shape on a concave ring: mitred inset + deck */
#define TG_S3D_HOW_FLAT_FALLBACK 5   /* sloped shape, no offset fitted: flat */
typedef struct {
    float *v; unsigned int *light; int nv, cap;
    unsigned short cmd[6]; int ncmd;
    int how, rows, tint_roof, tint_wall;
    double rise;
} TG_S3dMesh;
int  tg_s3d_eligible(const TD5_GeoBuilding *gb);
/* by = the mass's base y (base sink applied), H = its height (sink added). 1 built, 0 degenerate,
 * -1 out of memory. m is owned by the caller afterwards: tg_s3d_free. */
int  tg_s3d_build(const TG_GeoBldEnv *e, const TD5_GeoBuilding *gb, const double *rx, const double *rz,
                  int n, double by, double H, double floor_h, double cell_w, double lm_row_h, int storeys,
                  int wall_page, int roof_page, TG_S3dMesh *m);
void tg_s3d_free(TG_S3dMesh *m);

/* One shipped set piece written into blk, no census counter (td5_tg_prefab.c). */
int tg_prefab_write_piece(TG_Buf *blk, int pf, double ox, double oy, double oz,
                          double ca, double sa);

/* OSM describes this landmark in 3D (height, levels, roof shape or parts). */
int tg_geo_landmark_is_real(const TD5_GeoBuilding *b);

#endif /* TD5_TG_GEO_BLD_H */
