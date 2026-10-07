/* ========================================================================
 * td5_geo_tiles.h -- OUTBOUND-ONLY OSM raster tile client (PORT-ONLY,
 *                    GEOSPATIAL TRACK GENERATOR).
 *
 * [GEO GENERATOR 2026-10-07, round 1007 group L2] Fetches slippy-map tiles
 * over HTTPS with WinHTTP, caches them on disk, decodes them into one GPU
 * atlas page and hands the map screen (td5_fe_geo.c) the UV rect for any
 * (z, x, y) it asks for.
 *
 * WHY THIS EXISTS AT ALL. docs/plans/GEO_TRACK_OSM_PLAN.md section 0 ruled
 * in-game map tiles OUT and made the browser selector the only selector.
 * Mariano reversed that on 2026-10-07 for this feature specifically, so the
 * release exe can pick a route without a browser. The reversal is narrow:
 *
 *   ALLOWED      outbound HTTPS GETs from this module, while the map screen
 *                is open, for tiles the screen is about to draw.
 *   STILL FORBIDDEN  listening sockets of any kind, any edit to td5_net.c /
 *                td5_upnp.c / td5_fe_net.c, and any network step in the
 *                selftest. None of those are touched from here.
 *
 * OSM TILE USAGE POLICY -- the rules this module is built to keep:
 *   - Identifying User-Agent (TD5_GEO_TILE_UA below), never a generic one.
 *   - At most TD5_GEO_TILE_WORKERS (2) requests in flight, ever.
 *   - On-disk cache under re/assets/geo/_tiles/<z>/<x>/<y>.png, re-fetched
 *     only after TD5_GEO_TILE_MAX_AGE_DAYS. A cached tile costs no request.
 *   - NO prefetching and no bulk downloading: a tile is requested only when
 *     the screen asks to draw it, one view at a time.
 *   - Requests happen only between td5_geo_tiles_open and _close, i.e. only
 *     while the screen is on.
 *   - Zoom is capped at TD5_GEO_TILE_MAX_Z.
 *
 * ODbL: every surface that shows these tiles must carry TD5_GEO_CREDIT
 * (td5_geo.h). The screen draws it unconditionally.
 *
 * OFFLINE BEHAVIOUR IS NOT AN ERROR PATH. With no network the worker fails
 * each request, the slot goes FAILED, and the screen draws a grey placeholder.
 * Nothing blocks the frame: the main thread never does I/O for a tile it has
 * not already got on disk, and it decodes at most
 * TD5_GEO_TILE_DECODES_PER_FRAME of them per frame.
 * ======================================================================== */
#ifndef TD5_GEO_TILES_H
#define TD5_GEO_TILES_H

/* Slippy-map tile edge in pixels (the OSM standard). */
#define TD5_GEO_TILE_PX        256

/* Atlas: 8x8 tiles on one 2048x2048 BGRA page, the same shape and size as the
 * TTF glyph atlas (td5_font.c, page 984), which is the proven precedent for a
 * CPU-built page re-uploaded on change. 64 slots holds a 5x5 view plus enough
 * history that a slow pan does not thrash. */
#define TD5_GEO_TILE_ATLAS_DIM 2048
#define TD5_GEO_TILE_ATLAS_COLS (TD5_GEO_TILE_ATLAS_DIM / TD5_GEO_TILE_PX)
#define TD5_GEO_TILE_SLOTS      (TD5_GEO_TILE_ATLAS_COLS * TD5_GEO_TILE_ATLAS_COLS)

/* Page 985. 984 is the TTF glyph atlas and the frontend's own shared block
 * runs to 983; the next reserved range in td5_page_map.h is ENVMAP at 990.
 * Declared the same way td5_font.c declares 984 -- a local constant with its
 * neighbours named -- rather than in td5_page_map.h, which owns the *ranges*
 * the level zone must not reach, not these one-off persistent pages. */
#define TD5_GEO_TILE_ATLAS_PAGE 985

#define TD5_GEO_TILE_WORKERS            2    /* = max concurrent requests */
#define TD5_GEO_TILE_DECODES_PER_FRAME  2
#define TD5_GEO_TILE_MAX_AGE_DAYS      30
#define TD5_GEO_TILE_MIN_Z              2
#define TD5_GEO_TILE_MAX_Z             18

#define TD5_GEO_TILE_HOST "tile.openstreetmap.org"
#define TD5_GEO_TILE_UA   "TD5RE/1.0 (+https://github.com/nanofives/TD5RE)"

/* Open / close the client. Fetching happens only between these two calls.
 * _close stops the workers, joins them and releases the atlas memory; it is
 * safe to call without a matching _open. Both are main-thread only. */
void td5_geo_tiles_open(void);
void td5_geo_tiles_close(void);

/* Per-frame main-thread pump: decode tiles the workers have put on disk and
 * re-upload the atlas page if anything changed. Call once per frame while the
 * screen is up, BEFORE the draw. */
void td5_geo_tiles_frame(void);

/* Ask for a tile. Returns 1 and fills the atlas UV rect when the tile is
 * resident; returns 0 otherwise and (the first time) queues a fetch, so the
 * caller should draw a placeholder this frame. Calling it is what makes a
 * tile get downloaded -- there is no other request path, which is how the
 * "never prefetch" rule is kept structurally. */
int  td5_geo_tiles_lookup(int z, int x, int y,
                          float *u0, float *v0, float *u1, float *v1);

/* Status, for the screen's footer. */
int  td5_geo_tiles_pending(void);   /* queued + in flight            */
int  td5_geo_tiles_failed(void);    /* tiles that gave up (offline)  */

#endif /* TD5_GEO_TILES_H */
