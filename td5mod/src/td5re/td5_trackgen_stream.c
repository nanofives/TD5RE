/**
 * td5_trackgen_stream.c -- streamed scenery worker (PORT-ONLY).
 *
 * See td5_trackgen_stream.h for the contract and the reasoning. This file owns
 * the THREAD and the LIFECYCLE only; the generator's half (a build that stops
 * after the geometry, plus the driver that runs the scenery phases later) is
 * td5_trackgen_stream_* in td5_trackgen.c, and the ingest half is
 * td5_track_scenery_* in td5_track.c.
 *
 * Modelled on td5_trackgen_preview.c, itself modelled on the td5_control.c
 * listener: CreateThread plus a one-way cancel flag. No critical section here,
 * unlike the preview worker, because there is no shared buffer to guard -- the
 * worker's only publication goes through td5_track_scenery_publish_entry,
 * which is fenced, and every flag crossing threads is single-writer and
 * monotone.
 */
#include <windows.h>
#include <string.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_track.h"
#include "td5_race_state.h"  /* read-only: the player's live span */
#include "td5_rt.h"          /* the RT-window-ready gate for the hold */
#include "td5_trackgen.h"
#include "td5_trackgen_stream.h"

#define LOG_TAG "track"

static HANDLE        s_thread;
/* One-way, single-writer flag: main thread sets it, worker polls it between
 * entries and never clears it. A plain volatile int is the right shape (and is
 * what td5_trackgen_stream_scenery takes) -- Interlocked would only matter for
 * read-modify-write, which nothing here does. */
static volatile int  s_cancel;
static volatile LONG s_worker_done;   /* worker set it and is about to exit   */
static volatile LONG s_worker_ok;

/* The main thread's FP control state, captured at _begin and installed on the
 * worker. NOT optional: the generator's output depends on the rounding mode
 * (this port runs _RC_DOWN|_PC_64 to match the original binary), and a new
 * thread starts with the process default instead. Skipping this produced a
 * MODELS.DAT 492 bytes different from the same seed on the main thread, with
 * 1-ULP float diffs throughout and 56 of 497 entries emitting different
 * geometry where a value crossed a threshold. See td5_platform.h. */
static TD5_FpEnv s_fpenv;

static int  s_active;        /* between _begin and the deferred passes        */
static int  s_nentries;
static int  s_passes_done;
static int  s_caught_up_warned;
static int  s_geom_ready;    /* near-grid road decorated (for the loading readout) */
static unsigned int s_hold_start_ms;
static unsigned int s_begin_ms;

/* ------------------------------------------------------------------ knobs -- */

int td5_tgstream_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_STREAM");
}

/* Entries of head start before the countdown is released. The renderer reaches
 * VIEW_DIST_FWD_SPANS (64) spans ahead and an entry is 4 spans, so 16 entries
 * is the minimum that keeps the grey ribbon off screen at the start line; the
 * default adds margin for the first seconds of acceleration. */
static int tgstream_lead(void)
{
    return td5_env_int("TD5RE_AUTOTRACK_STREAM_LEAD", 24, 1, 4096);
}

/* Upper bound on the countdown hold. The tutorial hold beside ours is
 * unbounded, but it waits on a PLAYER; a wedged worker has nobody to press a
 * button, so this one gives up and lets the race start on the ribbon rather
 * than hanging the game. */
static int tgstream_hold_cap_ms(void)
{
    return td5_env_int("TD5RE_AUTOTRACK_STREAM_HOLD_MS", 30000, 0, 600000);
}

/* ----------------------------------------------------------------- worker -- */

static DWORD WINAPI tgstream_thread_proc(LPVOID param)
{
    int ok;
    (void)param;
    /* FIRST, before any generation: adopt the main thread's rounding mode. */
    td5_plat_fpenv_apply(&s_fpenv);
    ok = td5_trackgen_stream_scenery(&s_cancel);
    InterlockedExchange(&s_worker_ok, ok ? 1 : 0);
    InterlockedExchange(&s_worker_done, 1);
    return 0;
}

static void tgstream_join(void)
{
    if (!s_thread) return;
    WaitForSingleObject(s_thread, INFINITE);
    CloseHandle(s_thread);
    s_thread = NULL;
}

/* ------------------------------------------------------- deferred passes -- */

/* The four table-walking passes InitRace normally runs at step 8, re-run once
 * the table is actually populated. They ran at load against an EMPTY table, so
 * without this a streamed track would have no derived normals, no banner-page
 * classification and no static tunnel lamps.
 *
 * ONCE, at completion -- deliberately not per entry. Two of them are
 * destructive at the top: td5_track_derive_missing_normals opens with
 * track_derived_norms_free_all(), so a per-entry call would free normal
 * streams already published to the renderer (a use-after-free, not a cosmetic
 * glitch), and td5_track_register_lamp_lights resets the lamp registry and the
 * material page-class cache.
 *
 * Order matches InitRace step 8: dim needs the per-page transparency table,
 * derive needs the dimming, banners and lamps need the finished meshes. */
static void tgstream_run_deferred_passes(void)
{
    if (s_passes_done) return;
    s_passes_done = 1;

    td5_track_scenery_stream_done();
    td5_track_dim_additive_billboard_meshes();
    td5_track_derive_missing_normals();
    td5_track_scan_banner_pages();
    td5_track_register_lamp_lights();

    /* The worker is finished with the stashed node list, so give it back
     * before anything else can start a build. */
    td5_trackgen_stream_discard();
}

/* --------------------------------------------------------------- lifecycle - */

int td5_tgstream_init(void)
{
    s_thread = NULL;
    s_active = 0;
    s_nentries = 0;
    s_passes_done = 0;
    s_caught_up_warned = 0;
    s_hold_start_ms = 0;
    s_cancel = 0;
    InterlockedExchange(&s_worker_done, 0);
    InterlockedExchange(&s_worker_ok, 0);
    return 1;
}

/* Drain the whole table on THIS thread. The fallback path: used when the
 * worker cannot start, and forced by TD5RE_AUTOTRACK_STREAM_FORCE_FALLBACK so
 * it is exercised rather than theoretical. Logs a warning either way --
 * silently degrading is how this ships broken. */
static void tgstream_drain_synchronously(const char *why)
{
    unsigned int t0 = td5_plat_time_ms();
    int ok = td5_trackgen_stream_scenery(NULL);
    TD5_LOG_W(LOG_TAG, "scenery stream: SYNCHRONOUS fallback (%s) -- drained "
              "%d entries on the main thread in %u ms, ok=%d", why,
              s_nentries, td5_plat_time_ms() - t0, ok);
    tgstream_run_deferred_passes();
    s_active = 0;
}

int td5_tgstream_begin(void)
{
    size_t blob_bytes;
    int nspans;

    /* A non-streamed build leaves nothing pending, so this is the no-op that
     * keeps every other track's load path untouched. */
    if (!td5_trackgen_stream_pending())
        return 0;

    s_nentries = td5_trackgen_stream_entry_count();
    nspans     = td5_trackgen_stream_span_count();
    if (s_nentries <= 0 || nspans <= 0) {
        td5_trackgen_stream_discard();
        return 0;
    }

    /* 8 KB/span against about 6 KB/span measured across three configs
     * (MARATHON: 19,098,312 B over 3187 spans). A streamed table cannot know
     * the real total until the last entry exists and must never realloc, so it
     * over-reserves; publish stops and warns if even this runs out. */
    blob_bytes = (size_t)nspans * 8192u;

    s_passes_done = 0;
    s_caught_up_warned = 0;
    s_cancel = 0;
    InterlockedExchange(&s_worker_done, 0);
    InterlockedExchange(&s_worker_ok, 0);

    if (!td5_track_scenery_reserve(s_nentries, blob_bytes)) {
        /* No table to publish into, so there is nothing to fall back TO --
         * the generator's blocks would have nowhere to go. Give up cleanly and
         * let the track race on the ribbon. */
        TD5_LOG_W(LOG_TAG, "scenery stream: reserve failed (%d entries, %zu B)"
                  " -- racing on the fallback ribbon, no scenery this race",
                  s_nentries, blob_bytes);
        td5_trackgen_stream_discard();
        return 0;
    }

    s_active = 1;
    s_begin_ms = td5_plat_time_ms();

    if (td5_env_flag_off("TD5RE_AUTOTRACK_STREAM_FORCE_FALLBACK")) {
        tgstream_drain_synchronously("forced by knob");
        return 0;
    }

    td5_plat_fpenv_capture(&s_fpenv);
    s_thread = CreateThread(NULL, 0, tgstream_thread_proc, NULL, 0, NULL);
    if (!s_thread) {
        tgstream_drain_synchronously("CreateThread failed");
        return 0;
    }

    TD5_LOG_I(LOG_TAG, "scenery stream: worker started, %d entries over %d "
              "spans, %zu B reserved, lead %d entries",
              s_nentries, nspans, blob_bytes, tgstream_lead());
    return 1;
}

void td5_tgstream_tick(void)
{
    if (!s_active) return;

    if (!InterlockedCompareExchange(&s_worker_done, 0, 0)) {
        /* Still publishing. One warning if the player has driven past the
         * watermark: the ribbon covers it, but it is visible and nothing else
         * would say the worker lost the race. */
        if (!s_caught_up_warned) {
            int from = td5_track_scenery_undecorated_from_span();
            int span = td5_game_get_slot_span(0);
            if (from >= 0 && span >= from) {
                s_caught_up_warned = 1;
                TD5_LOG_W(LOG_TAG, "scenery stream: the PLAYER CAUGHT UP -- "
                          "span %d is past the decorated tail (%d), so the "
                          "road ahead shows the untextured fallback ribbon "
                          "until the worker gets there", span, from);
            }
        }
        return;
    }

    tgstream_join();
    TD5_LOG_I(LOG_TAG, "scenery stream: worker finished in %u ms, ok=%d, "
              "%d/%d entries published",
              td5_plat_time_ms() - s_begin_ms,
              (int)InterlockedCompareExchange(&s_worker_ok, 0, 0),
              td5_track_scenery_ready_entries(), s_nentries);
    tgstream_run_deferred_passes();
    s_active = 0;
}

int td5_tgstream_hold(void)
{
    int ready, need, start_entry, geom_ready, rt_ready;

    if (!s_active) return 0;
    /* Worker finished but _tick has not run its passes yet: nothing left to
     * wait for. */
    if (InterlockedCompareExchange(&s_worker_done, 0, 0)) return 0;

    start_entry = (g_td5.track_start_span_index > 0)
                ? (int)(g_td5.track_start_span_index >> 2) : 0;
    need = start_entry + tgstream_lead();
    if (need > s_nentries) need = s_nentries;

    ready = td5_track_scenery_ready_entries();
    /* Two gates, both must clear before the lights go green:
     *  - geometry: the road around the grid is decorated (published);
     *  - RT: its ray-traced shadows are BUILT. Without this the window's first
     *    fill happened as a hitch in the first seconds of driving; now it fills
     *    here, on the loading screen, while the camera flies in. Reads 1 with RT
     *    off or on a track small enough not to window. */
    geom_ready = (ready >= need);
    rt_ready   = td5_rt_scenery_window_ready();
    s_geom_ready = geom_ready;
    if (geom_ready && rt_ready) {
        if (s_hold_start_ms) {
            TD5_LOG_I(LOG_TAG, "scenery stream: countdown held %u ms for "
                      "%d entries of lead", td5_plat_time_ms() - s_hold_start_ms,
                      need);
            s_hold_start_ms = 0;
        }
        return 0;
    }

    if (!s_hold_start_ms) {
        s_hold_start_ms = td5_plat_time_ms();
        if (s_hold_start_ms == 0) s_hold_start_ms = 1;   /* 0 means "not held" */
        TD5_LOG_I(LOG_TAG, "scenery stream: holding the countdown until entry "
                  "%d is decorated (ready %d)", need, ready);
        return 1;
    }

    {
        const int cap = tgstream_hold_cap_ms();
        if (cap > 0 && (int)(td5_plat_time_ms() - s_hold_start_ms) > cap) {
            TD5_LOG_W(LOG_TAG, "scenery stream: hold CAP hit after %d ms with "
                      "only %d/%d entries -- starting anyway; the road ahead "
                      "will show the fallback ribbon", cap, ready, need);
            s_hold_start_ms = 0;
            return 0;
        }
    }
    return 1;
}

int td5_tgstream_active(void) { return s_active; }

/* Loading-screen readout. Fills whichever non-NULL pointers the HUD passes:
 * entries published so far, total entries, and the two hold gates (near road
 * decorated / near RT shadows built). Returns 1 while a stream is in flight. */
int td5_tgstream_progress(int *ready, int *total, int *geom, int *rt)
{
    if (ready) *ready = s_active ? td5_track_scenery_ready_entries() : 0;
    if (total) *total = s_nentries;
    if (geom)  *geom  = s_geom_ready;
    if (rt)    *rt    = td5_rt_scenery_window_ready();
    return s_active;
}

void td5_tgstream_cancel_join(void)
{
    if (!s_thread && !s_active) return;

    if (s_thread) {
        s_cancel = 1;
        tgstream_join();
        s_cancel = 0;
        TD5_LOG_I(LOG_TAG, "scenery stream: cancelled at %d/%d entries",
                  td5_track_scenery_ready_entries(), s_nentries);
    }
    /* The node list must go back whether or not a worker ran: the next build
     * is about to overwrite the statics it points into. */
    td5_trackgen_stream_discard();
    s_active = 0;
    s_passes_done = 0;
    s_hold_start_ms = 0;
    s_nentries = 0;
}

void td5_tgstream_shutdown(void)
{
    td5_tgstream_cancel_join();
}
