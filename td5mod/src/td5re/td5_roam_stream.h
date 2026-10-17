/**
 * td5_roam_stream.h -- GEO FREE ROAM: tile streaming (PORT-ONLY, F3-A).
 * Design: docs/plans/GEO_FREE_ROAM_DESIGN.md 0 item 6 and 3.1 - 3.8.
 *
 * WHAT IT IS. In a roam session nothing is loaded up front any more. Two residency
 * layers follow the cars and the camera:
 *   ROAD     the ROAMTILE road layer in the physics resident set (td5_roam_physics.c):
 *            every tile within TD5RE_ROAM_STREAM_ROAD_R (1 = 3x3) of every racer, plus
 *            the same square round the point the racer reaches in
 *            TD5RE_ROAM_STREAM_LEAD_MS at its current velocity.
 *   SCENERY  the baked render meshes (td5_roam_render.c): every tile within
 *            TD5RE_ROAM_STREAM_VIEW_R (2 = 5x5) of the camera, plus a 3x3 round the
 *            camera's velocity lead.
 * The streamable tiles are the place's casco list (td5_roam_casco_tiles).
 *
 * WORKER (design 3.2). One thread, the main thread's FP env applied first. It takes
 * the best pending request (lowest distance to a racer / the camera; ROAD before
 * SCENERY), READS the ROAMTILE file, or BUILDS it with td5_roam_tile_build when the
 * file is missing or unreadable (rules version) and writes it back (temp name, then
 * rename), and BAKES the render meshes with its own height source. A tile installed
 * from disk is later VERIFIED at low priority (the input hash a build would stamp
 * now, td5_roam_tile_input_hash); a stale one is rebuilt and replaced.
 *
 * PUBLISH (design 3.3). td5_roam_stream_tick, called at the top of td5_game_tick
 * (between frames, never while panes record), installs at most
 * TD5RE_ROAM_STREAM_INSTALL_ROAD road tiles and _INSTALL_SCEN baked tiles per frame:
 * a pointer insert each (the bake happened on the worker), so a publish costs no
 * frame time to speak of. Physics sees a road tile from the next sim tick.
 *
 * UNLOAD (3.4, 3.5). A layer of a tile leaves the KEEP ring (radius + 1) and stays
 * out for TD5RE_ROAM_STREAM_LINGER_MS: it is then evictable, and evictable tiles are
 * dropped least-recently-wanted first while the layer is over its budget
 * (TD5RE_ROAM_STREAM_ROAD_MB, _SCEN_MB). TD5RE_ROAM_STREAM_LRU=0 drops every
 * evictable tile at once. Baked meshes are retired 3 ticks before they are freed.
 *
 * PHYSICS SAFETY. td5_roam_stream_guard runs once per racer per sim tick: a body
 * footprint point over a streamable tile that is not ROAD-resident is a
 * ROAM_STREAM_MISS: the tile file is read and installed right there (a hitch, never
 * a fall-through); if it has to be built first the car is HELD (the caller freezes
 * it and its wheels read flat ground at their own height) until the worker brings it.
 *
 * START GATE. td5_roam_stream_wait_area blocks the spawn until the spawn tile and its
 * ROAD neighbourhood are installed (the loading screen shows "Streaming streets").
 *
 * RT (design 3.7). The roam meshes are drawn immediate mode and are not in the strip
 * entry table the RT scene walks, so nothing feeds RT and nothing has to be freed
 * there. F3 runs roam with RT off; the RT tile feed is F3c.
 *
 * TD5RE_ROAM_STREAM=0: the F2 behaviour (every casco tile loaded at session start,
 * the render's own near-eye loader), for A/B.
 *
 * STATE. One heap session (td5_roam_stream.c), no other file-scope mutable state but
 * the loading-screen hook.
 */
#ifndef TD5_ROAM_STREAM_H
#define TD5_ROAM_STREAM_H

#include <stdint.h>
#include "td5_roam_tile.h"

struct TD5_RoamResident;
struct TD5_Actor;

/* the loading-screen line the start gate draws (td5_game.c registers it) */
typedef void (*TD5_RoamStreamLoadscreenFn)(const char *label);
void td5_roam_stream_set_loadscreen(TD5_RoamStreamLoadscreenFn fn);

int  td5_roam_stream_enabled(void);          /* TD5RE_ROAM_STREAM (default 1) */

/* Session (main thread). begin: `rs` is the physics resident set (already
 * td5_roam_resident_init'ed, world origin set); the stream adds / removes its tiles.
 * Starts the worker. 1 on success. end: stops the worker, frees every streamed
 * tile and baked mesh, ends the render session. */
int  td5_roam_stream_begin(const char *place, const RoamGrid *g, struct TD5_RoamResident *rs);
void td5_roam_stream_end(void);
int  td5_roam_stream_active(void);

/* Top of td5_game_tick: install, request, unload, stats. Inert outside a session. */
void td5_roam_stream_tick(void);

/* START GATE: block until every streamable tile within `ring` of the PLACE point
 * (px, pz) is ROAD-resident (or failed). Draws the loading screen line meanwhile.
 * Returns the tiles that are resident. */
int  td5_roam_stream_wait_area(int32_t px, int32_t pz, int ring);

/* DEV harnesses that pick from the whole casco (the corner matrix): load and pin every
 * casco ROAD tile (blocking). Returns the resident count. */
int  td5_roam_stream_pin_all(void);

/* PHYSICS SAFETY: once per racer per sim tick (idempotent within a tick). 0 = every
 * tile under the racer's footprint is resident; 1 = HOLD the car this tick (a tile
 * under it is still being built). */
int  td5_roam_stream_guard(struct TD5_Actor *a);
/* 1 when the 24.8 world point lies on a streamable tile that is not ROAD-resident
 * (a probe there must not fall: the caller holds it). */
int  td5_roam_stream_point_missing(int32_t x, int32_t z);

typedef struct {
    int      road_tiles, scen_tiles, road_min, road_max, scen_min, scen_max;
    double   road_mb, scen_mb;
    long     loaded, built, stale, verified, failed, evicted_road, evicted_scen;
    long     misses, holds, sync_loads;
    double   gate_ms;            /* the start gate's wait                         */
    double   frame_median_ms, frame_worst_ms, frame_worst_publish_ms, install_worst_ms;
    double   tick_worst_ms;      /* the whole td5_roam_stream_tick on the main thread */
} TD5_RoamStreamStats;
void td5_roam_stream_stats(TD5_RoamStreamStats *out);

#endif /* TD5_ROAM_STREAM_H */
