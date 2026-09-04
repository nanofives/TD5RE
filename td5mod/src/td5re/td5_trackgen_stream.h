/**
 * td5_trackgen_stream.h -- streamed scenery worker for the auto-generated
 * track (PORT-ONLY, see td5_trackgen_stream.c).
 *
 * THE POINT. A generated track's geometry (STRIP + route tables + LEVELINF +
 * sky) costs about 300 ms; its scenery (MODELS.DAT) costs about 20 s, i.e.
 * 99.4 percent of the build. The simulation never reads scenery -- there is no
 * s_models_* reference anywhere in physics, collision, suspension, AI, traffic,
 * HUD, camera or lane assist -- so the race can start on the geometry and have
 * the decoration published in behind the player, entry by entry.
 *
 * Generation runs about 11 ms/span against about 70-140 ms/span of driving, so
 * the worker is 6-12x faster than the player before counting the countdown as
 * head start. The countdown hold exists for the case where it is not.
 *
 * WHAT THE PLAYER SEES IF IT FALLS BEHIND: undecorated road, painted with the
 * fallback strip ribbon, which is untextured by design (flat grey). That is a
 * visible seam, not a hole -- see the ribbon gate in td5_render_mesh.c.
 *
 * NETPLAY IS SAFE. All geometry exists at start, so span indices, route
 * tables, lap/finish and AI arithmetic are final and identical on both peers.
 * Only decoration TIMING varies, and its content is a pure function of
 * (seed, span). A slower peer sees scenery appear later. No desync.
 *
 * THREADING CONTRACT
 *  - ONE worker thread, ever. The generator keeps ~102 mutable file-scope
 *    statics and is single-instance, so this worker, the studio's route
 *    preview (td5_trackgen_preview.c) and any synchronous build are mutually
 *    exclusive. td5_asset_load_level joins both workers before regenerating.
 *  - The worker only ever calls td5_track_scenery_publish_entry, which is
 *    single-producer by contract and publishes each entry behind a release
 *    fence. Readers gate on the entry's own count. Nothing else is shared.
 *  - Completion work (the four table-walking passes) runs on the MAIN thread
 *    in _tick, because two of those passes touch the RT wrapper and the
 *    lighting registry.
 */
#ifndef TD5_TRACKGEN_STREAM_H
#define TD5_TRACKGEN_STREAM_H

int  td5_tgstream_init(void);

/* Is streaming enabled at all? TD5RE_AUTOTRACK_STREAM, default ON; =0 restores
 * the synchronous build-everything-then-load path (the A/B). */
int  td5_tgstream_enabled(void);

/* Reserve the scenery table and start the worker. Call AFTER the level load,
 * because the load's MODELS.DAT parse would otherwise free what we publish.
 * No-op unless a streamed build actually left scenery pending. Returns 1 if
 * the worker is running; 0 means the caller must fall back (and _begin will
 * already have drained synchronously if it could). */
int  td5_tgstream_begin(void);

/* Main thread, once per frame. Detects worker completion, joins it and runs
 * the deferred passes. Cheap when idle. */
void td5_tgstream_tick(void);

/* Countdown gate: 1 while the road ahead of the grid is not decorated yet.
 * Sits beside the existing !td5_tutorial_is_active() gates, which already
 * hold the countdown for an unbounded time. */
int  td5_tgstream_hold(void);

/* Is a stream in flight (worker running, or finished but passes not yet run)? */
int  td5_tgstream_active(void);

/* Cancel and join. MUST be called before any generator build or route preview,
 * and on level teardown. Safe to call when idle. */
void td5_tgstream_cancel_join(void);

void td5_tgstream_shutdown(void);

#endif /* TD5_TRACKGEN_STREAM_H */
