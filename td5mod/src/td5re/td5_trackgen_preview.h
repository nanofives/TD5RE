/**
 * td5_trackgen_preview.h -- background route-preview worker (PORT-ONLY).
 *
 * Drives td5_trackgen_preview_route on a dedicated thread so the AUTO TRACK
 * STUDIO screen can draw a track's outline WHILE it is being generated,
 * without the menu dropping a frame. The generator itself is a 27k-line module
 * with ~100 mutable file-scope statics, so it is single-instance by
 * construction: this module owns the one worker and serialises every access.
 *
 * THREADING CONTRACT
 *   - At most one preview build is ever in flight.
 *   - The worker thread is the ONLY thread that calls into td5_trackgen.c
 *     while a preview is running.
 *   - A real build (td5_trackgen_regenerate, at race launch) walks the SAME
 *     module statics -- the private RNG and the biome grid -- so it must never
 *     overlap a preview. td5_asset_load_level calls td5_tgprev_cancel_join()
 *     before it regenerates. That join is not optional.
 *
 * Points are published incrementally and are append-only within one build, so
 * the caller keeps its own mirror and asks for the tail via td5_tgprev_fetch.
 * `generation` bumps on every request; when it changes, discard the mirror.
 */
#ifndef TD5_TRACKGEN_PREVIEW_H
#define TD5_TRACKGEN_PREVIEW_H

#include "td5_trackgen.h"

/* Ceiling on published points: the main ring (TD5_TG_MAX_SPANS + 1 nodes) plus
 * up to TD5_TG_BRANCH_MAX corridors re-laid over it. */
#define TD5_TGPREV_MAX_POINTS  8192

typedef struct {
    int generation;   /* bumps per request; mirror is stale when this changes */
    int running;      /* 1 = a worker is walking right now */
    int done;         /* 1 = finished, failed or cancelled */
    int ok;           /* valid once done: 1 = a full route was produced */
    int total;        /* points published so far (<= TD5_TGPREV_MAX_POINTS) */
    TD5_TrackGenPreviewStats stats;   /* valid once done */
} TD5_TgPrevStatus;

/* Module lifecycle (registered in g_td5re_modules right after "trackgen"). */
int  td5_tgprev_init(void);
void td5_tgprev_shutdown(void);

/* Cancel any in-flight build, join the worker, then start a fresh one for
 * `spec` (copied). Returns the new generation number, or 0 if the worker could
 * not be started. */
int  td5_tgprev_request(const TD5_TrackGenSpec *spec);

/* Cancel and join without starting anything. Safe to call when idle. MUST be
 * called before any real td5_trackgen_regenerate -- see the contract above. */
void td5_tgprev_cancel_join(void);

/* Main-thread reads. */
void td5_tgprev_status(TD5_TgPrevStatus *out);

/* Copy up to `max` points starting at `from_index` into `out`. Returns how
 * many were copied. Points never move or change once published within a
 * generation, so a caller that has N already can safely ask for the tail. */
int  td5_tgprev_fetch(int from_index, TD5_TrackGenPoint *out, int max);

#endif /* TD5_TRACKGEN_PREVIEW_H */
