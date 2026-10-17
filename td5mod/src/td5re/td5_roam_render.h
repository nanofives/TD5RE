/**
 * td5_roam_render.h -- GEO FREE ROAM: plain road + ground render (PORT-ONLY,
 * F2 task C). See docs/plans/GEO_FREE_ROAM_DESIGN.md 3.6 and td5_roam_render.c.
 *
 * WHAT IT DRAWS. Per resident ROAMTILE: the road surface (one quad per lane of
 * every ribbon span, plus the junction patch triangles) on ONE shared asphalt
 * page, a coarse ground heightfield under it from the source HEIGHT.R16, and a
 * grey skirt down every boundary edge (kerb / deck side). No per-tile texture
 * pages: every face names a page the loaded level already owns.
 *
 * HOW. Each tile is baked ONCE (td5_roam_render_add_tile, main thread) into up to
 * 16 ordinary TD5_MeshHeader meshes, one per 64 m sub-block, in the MODELS.DAT
 * mesh format, and drawn through td5_render_span_display_list (per-mesh sphere
 * cull, fog, page binding) by td5_roam_render_tiles. Nothing here touches the
 * strip: with roam off the hook is one predictable branch.
 *
 * CONTRACT with the rest of F2:
 *   - sibling A (td5_roam_mode.h): td5_roam_active(), td5_roam_world_origin().
 *     World = place - origin, so the frame is {ox, oz} from there.
 *   - sibling B (resident set): calls td5_roam_render_add_tile for every tile it
 *     makes resident and td5_roam_render_remove_tile when it drops one. Until B
 *     lands, a DEV loader inside td5_roam_render.c reads ROAMTILE files itself
 *     (TD5RE_ROAM_RENDER_TILES=near|all); it is clearly marked there and goes
 *     away when B feeds the set.
 */
#ifndef TD5_ROAM_RENDER_H
#define TD5_ROAM_RENDER_H

#include <stdint.h>
#include <stddef.h>
#include "td5_roam_tile.h"
#include "td5_roam_height.h"     /* [F3] TD5_RoamHeight: the worker's own ground source */

/* The frame the baked meshes live in. x/z: render = place - (ox, oz); y: render =
 * place y - dy. DEV route mode (route != 0): x/z come from the place -> route
 * chain of td5_roam_view.c instead, and dy is the strip's height datum, so the
 * tiles land on a loaded geo level's strip (overlay checks, doc tours). */
typedef struct {
    int32_t ox, oz;
    double  dy;
    int     route;
} TD5_RoamRenderFrame;

typedef struct {
    int      tiles;               /* resident (baked) tiles                          */
    int      meshes;              /* baked meshes, all tiles                         */
    uint32_t road_quads, patch_tris, ground_quads, skirt_quads;
    double   bytes_mb;            /* CPU memory of the baked meshes (the draw is
                                   * immediate mode: no GPU buffers are retained)   */
    /* last drawn frame */
    int      drawn_tiles, drawn_meshes;
    uint32_t drawn_quads;
    double   last_ms;             /* CPU time of the last td5_roam_render_tiles      */
    double   avg_ms;              /* running mean over drawn frames                  */
    double   max_ms;
    uint32_t frames;
} TD5_RoamRenderStats;

/* Session. begin opens the place's grid and the height source; returns 1.
 * Idempotent for the same place. end frees every baked mesh. */
int  td5_roam_render_begin(const char *place, const TD5_RoamRenderFrame *frame);
void td5_roam_render_end(void);
int  td5_roam_render_began(void);

/* Bake / drop one tile. add reads the tile (it keeps no pointer into it) and
 * replaces a baked tile with the same (tx, tz). Returns 1 on success. */
int  td5_roam_render_add_tile(const RoamTile *t);
void td5_roam_render_remove_tile(int tx, int tz);

/* ---- [F3] streamed residency (td5_roam_stream.c) ----------------------------
 * The stream worker BAKES (td5_roam_render_bake, any thread, with its own height
 * source from td5_roam_render_open_height) and the main thread INSTALLS between
 * frames (td5_roam_render_install, O(1): no bake on the main thread). A removed or
 * replaced tile is retired and freed after 3 main-thread ticks
 * (td5_roam_render_retire_tick), so no recorded frame reads freed vertices. The
 * session's configuration is fixed at begin; the bake reads only that. */
typedef struct TD5_RoamBaked TD5_RoamBaked;
int            td5_roam_render_ensure_begun(void);           /* main: begin for the active roam session */
void           td5_roam_render_set_streamed(int on);         /* 1 = no DEV loader (tiles come from the stream) */
int            td5_roam_render_open_height(TD5_RoamHeight *hc);  /* a caller-owned height source; 0 = no ground */
TD5_RoamBaked *td5_roam_render_bake(const RoamTile *t, TD5_RoamHeight *hc);   /* any thread; NULL = nothing */
int            td5_roam_render_install(TD5_RoamBaked *b);    /* main; takes ownership (replaces same tx,tz) */
void           td5_roam_render_free_baked(TD5_RoamBaked *b);
size_t         td5_roam_render_baked_bytes(const TD5_RoamBaked *b);
int            td5_roam_render_has_tile(int tx, int tz);
/* [F4a] the same bake plus the tile's SCENERY layer, built with `scn` (the caller's
 * td5_roam_scenery.h source, one per thread; NULL = road + ground only). */
struct TD5_RoamScnSource;
TD5_RoamBaked *td5_roam_render_bake_ex(const RoamTile *t, TD5_RoamHeight *hc, struct TD5_RoamScnSource *scn);
/* the session's lattice / place (NULL before begin): the worker opens its scenery source with them */
const RoamGrid *td5_roam_render_grid(void);
const char    *td5_roam_render_place(void);
void           td5_roam_render_retire_tick(void);            /* main, once per game tick */

/* Scene hook: draw every baked tile near the eye. Called from
 * td5_render_actors_for_view right after the strip's span loop. */
void td5_roam_render_tiles(int view);

/* 1 when the hook should run: a roam session is active (td5_roam_active) or the
 * DEV overlay is on (TD5RE_ROAM_RENDER=1). Cheap, called every frame. */
int  td5_roam_render_wanted(void);

void td5_roam_render_stats(TD5_RoamRenderStats *out);

#ifndef TD5RE_RELEASE
/* DEV: TD5RE_ROAM_RENDER_HIDE_STRIP=1 skips the strip's span walk so only the roam
 * surface shows (what a real roam session draws). */
int  td5_roam_render_hides_strip(void);
#else
#define td5_roam_render_hides_strip() 0
#endif

#ifndef TD5RE_RELEASE
/* DEV: TD5RE_ROAM_RENDER_POSES=<file> -> <file>.tour.txt (free-cam tour poses from lat/lon). 1 = handled, exit. */
int td5_roam_render_dev_poses(void);
#endif

#endif /* TD5_ROAM_RENDER_H */
