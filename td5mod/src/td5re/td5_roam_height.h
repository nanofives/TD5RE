/**
 * td5_roam_height.h -- GEO FREE ROAM: height from the SOURCE rasters, by window
 * (PORT-ONLY, F1 task T2).
 *
 * WHY. td5_geo_height samples the DERIVED route-frame raster that the strip
 * BUILD commits (td5_geo.c:1190). The free-roam road world is laid over the
 * whole place, not over one route, so it reads the SOURCE HEIGHT.R16 and
 * WATER.R8 in the place frame instead. Each is ~346 MB and is never slurped:
 * a window of cells is read row by row with a seek (gr_raster_read_window).
 *
 * THE RULE. A point gives the same height here as td5_geo_height would give
 * on the same grid: bilinear on the cells, times the exaggeration, minus the
 * water depth where the WATER raster is wet. The arithmetic and the clamping
 * are copied from td5_geo.c (geo_cell, geo_i16_at, geo_sample_bilinear,
 * geo_sample_u8, td5_geo_is_water) in the same order, so the only difference
 * a caller can see is the grid the samples come from.
 *
 * OWNERSHIP. A context is owned by one caller and holds everything it needs.
 * There is no file-scope mutable state, so two contexts on two threads never
 * share a cache. The window LRU is TD5_ROAM_HEIGHT_WINDOWS slots deep, one
 * slot per (layer, rectangle).
 *
 * BYTE-IDENTITY. Nothing here runs on a synthetic or strip build. It is called
 * by the dev test below and, in F1, by nothing else.
 */
#ifndef TD5_ROAM_HEIGHT_H
#define TD5_ROAM_HEIGHT_H

#include <stdint.h>
#include "td5_geo_route.h"          /* GrRaster, gr_raster_read_window */

/* Resident windows. Each prefetch takes one slot per layer. */
#define TD5_ROAM_HEIGHT_WINDOWS 9

/* Cells read beyond the requested rectangle on each side. Two is more than the
 * bilinear footprint needs (one cell) and keeps the edge samples off the
 * clamp. */
#define TD5_ROAM_HEIGHT_PAD 2

/* Bed depth below a water surface, world units. Mirrors TD5_GEO_WATER_DEPTH in
 * td5_geo.c:84. Keep the two in step. */
#define TD5_ROAM_WATER_DEPTH 1400.0

typedef enum {
    TD5_ROAM_HEIGHT_LAYER_HEIGHT = 0,   /* HEIGHT.R16, int16 */
    TD5_ROAM_HEIGHT_LAYER_WATER  = 1    /* WATER.R8,   uint8 */
} TD5_RoamHeightLayer;

typedef struct {
    int         used;                   /* slot holds a window */
    int         layer;                  /* TD5_RoamHeightLayer */
    double      x0, z0, x1, z1;         /* the rectangle asked for, place frame */
    uint32_t    last_use;               /* context clock at last touch */
    GrRaster    r;                      /* the window, cells [origin, origin+w*cell) */
} TD5_RoamHeightWin;

typedef struct {
    char        height_path[512];
    char        water_path[512];
    int         has_water;              /* WATER.R8 opened and is uint8 */
    double      exaggeration;           /* 1.5 by default */
    uint32_t    clock;
    unsigned    reads;                  /* window reads from disk */
    uint64_t    read_us[2];             /* time spent reading, per layer */
    TD5_RoamHeightWin win[TD5_ROAM_HEIGHT_WINDOWS];
} TD5_RoamHeight;

/* Set up a context over one place's SOURCE rasters. `water_path` may be NULL
 * (no water). Reads only the 72-byte headers. Returns 1 when the height raster
 * is usable, 0 otherwise (the context is then unusable and must be closed). */
int  td5_roam_height_open(TD5_RoamHeight *c, const char *height_path,
                          const char *water_path, double exaggeration);

/* Free every resident window. The context may be reopened afterwards. */
void td5_roam_height_close(TD5_RoamHeight *c);

/* Make the windows for the rectangle [x0,x1] x [z0,z1] (place frame, world
 * units) resident, one per layer. A rectangle already resident is a hit and
 * costs no read. Returns 1 when every layer read, 0 on any failure. */
int  td5_roam_height_prefetch(TD5_RoamHeight *c, double x0, double z0,
                              double x1, double z1);

/* Height at a place-frame point, exactly as td5_geo_height would give it on
 * the same grid. Needs a resident window that covers the point (see prefetch).
 * Returns 1 and writes *out_h (and *out_water when non-NULL: 1 if the point is
 * wet) when covered, 0 when no resident window covers it. Touches the LRU. */
int  td5_roam_height_sample(TD5_RoamHeight *c, double x, double z,
                            double *out_h, int *out_water);

#ifndef TD5RE_RELEASE
/* Dev test, run by td5_geo_route_init when TD5RE_ROAM_HEIGHT_TEST=1. Samples
 * 1000 points of the derived route window through td5_geo_height and through
 * the source-window reader, and prints the differences. Returns 0 when the
 * test ran, 1 when there was no geo place to test against. */
int  td5_roam_height_selftest(void);
#endif

#endif /* TD5_ROAM_HEIGHT_H */
