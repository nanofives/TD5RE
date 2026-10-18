/**
 * td5_roam_rebase.h -- GEO FREE ROAM: dynamic world-origin rebasing (PORT-ONLY, F9b).
 * Design: docs/plans/GEO_FREE_ROAM.md "Origin rebasing", GEO_FREE_ROAM_DESIGN.md 0 item 7,
 * GEO_REGION_EXTENT.md "Limits" (24.8 holds +-8.39 M units; a + b of two positions overflows past
 * 4.19 M, about 9.7 km from the origin).
 *
 * WHAT IT IS. The ONE place that moves the roam world origin. world = place - origin (integer
 * units, 430 per metre; actors keep the 24.8 form). F2 fixed the origin at the spawn tile; the
 * partido is ~49 km wide, so a long drive leaves the 24.8 range. td5_roam_rebase_tick runs at the
 * TOP of every sim tick (td5_game.c frame_run_sim_loop): when the player's lattice tile is more
 * than TD5RE_ROAM_REBASE_TILES (4) tiles (Chebyshev) from the origin's tile, the origin moves to
 * the (0,0) corner of the player's tile (roam_grid_origin, the ROAMTILE rounding), and every
 * stored WORLD position is shifted by s = old origin - new origin (a whole number of tiles, an
 * exact integer vector, so 24.8 values move by FP_SCALE(s) with no rounding):
 *
 *   mode origin            td5_roam_mode.c (the authority every place<->world conversion reads)
 *   physics provider       resident set origin + per-slot last-good / wall / hold positions
 *   every actor            world_pos, probes, body corners, wheel contacts, hi-res wheel points,
 *                          render_pos, saved_render_pos (racers, AI, traffic, parked)
 *   engine                 physics prev positions + wheel transforms, camera poses / eye / anchors,
 *                          particles, tire marks, prop-break queue, weather eye, sound listener,
 *                          lamp registry, cop-ram stuck memory, the replay buffer
 *   roam render            every resident tile's meshes / spheres / lanterns (baked tiles are drawn
 *                          with an offset, so a bake in flight on the worker never races a rebase)
 *   listeners              anything registered with td5_roam_rebase_listen (traffic, dev drivers)
 * Everything else in roam (route, race, AI, barriers, guidance, planner) is stored in the PLACE
 * frame and converts through the mode origin when it draws or probes, so it needs nothing.
 *
 * NOT IN A SPRINT GRID / COUNTDOWN. A rebase is a pure translation of every world position: the
 * physics is translation-invariant, so a rebase can happen at any sim tick. It is only skipped
 * while a DEV harness keeps world-frame paths (td5_roam_physics_rebase_inhibited).
 *
 * TD5RE_ROAM_REBASE_TILES=0 turns it off (the F2 fixed origin).
 */
#ifndef TD5_ROAM_REBASE_H
#define TD5_ROAM_REBASE_H

#include <stdint.h>

/* listener: add (sx, sz) world units to every stored world position (24.8 values: FP_SCALE). */
typedef void (*TD5_RoamRebaseFn)(int32_t sx, int32_t sz);
void td5_roam_rebase_listen(TD5_RoamRebaseFn fn);       /* idempotent; up to 16 */

/* Top of every sim tick (main thread). Inert outside an active roam session. */
void td5_roam_rebase_tick(void);
/* Force one rebase to the player's tile now (DEV / selftest), even inside the threshold. */
void td5_roam_rebase_force(void);
/* Self-test override of the threshold (tiles, -1 = back to TD5RE_ROAM_REBASE_TILES): the cops rows rebase every
 * tile so a pursuit runs across rebases. */
void td5_roam_rebase_force_tiles(int tiles);
/* A new roam session: counters reset. */
void td5_roam_rebase_session_reset(void);
/* Frame time feed (td5_roam_stream_tick): the worst frame that contained a rebase. */
void td5_roam_rebase_note_frame(double dt_ms);

typedef struct {
    long    count;                /* rebases this session                                  */
    long    last_tick;            /* sim tick of the last one                              */
    double  op_worst_ms;          /* the shift itself, main thread                         */
    double  frame_worst_ms;       /* the frame that contained a rebase (tick-to-tick)      */
    int     threshold_tiles;
    int32_t total_x, total_z;     /* sum of the origin moves, place units                  */
    double  max_world_m;          /* largest |world x|, |world z| of slot 0 seen at a tick */
    long    far_actors;           /* actors whose shifted position left +-4.19 M units     */
} TD5_RoamRebaseStats;
void td5_roam_rebase_stats(TD5_RoamRebaseStats *out);

#endif /* TD5_ROAM_REBASE_H */
