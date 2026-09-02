/**
 * td5_trackgen_preview.c -- background route-preview worker (PORT-ONLY).
 *
 * See td5_trackgen_preview.h for the threading contract. The short version:
 * one worker thread, never concurrent with a real build, points published
 * append-only under a critical section and drained by the main thread.
 *
 * Modelled on the listener in td5_control.c (CreateThread + CRITICAL_SECTION +
 * an InterlockedExchange stop flag), which is the port's existing pattern for
 * a long-lived background thread.
 */
#include <windows.h>
#include <string.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_trackgen.h"
#include "td5_trackgen_preview.h"

#define LOG_TAG "track"

static CRITICAL_SECTION s_lock;
static int              s_lock_init;

static HANDLE           s_thread;
static volatile LONG    s_cancel;

/* All of the following are guarded by s_lock. */
static TD5_TrackGenPoint s_points[TD5_TGPREV_MAX_POINTS];
static int               s_count;
static int               s_generation;
static int               s_running;
static int               s_done;
static int               s_ok;
static TD5_TrackGenPreviewStats s_stats;

/* Worker-owned: the spec is copied at request time and only the worker reads
 * it afterwards, so it needs no lock. */
static TD5_TrackGenSpec s_spec;

/* ------------------------------------------------------------ sink hooks -- */

static void tgprev_on_points(const TD5_TrackGenPoint *pts, int n, void *ctx)
{
    (void)ctx;
    if (n <= 0) return;
    EnterCriticalSection(&s_lock);
    {
        int room = TD5_TGPREV_MAX_POINTS - s_count;
        if (n > room) n = room;
        if (n > 0) {
            memcpy(&s_points[s_count], pts, (size_t)n * sizeof(*pts));
            s_count += n;
        }
    }
    LeaveCriticalSection(&s_lock);
}

static int tgprev_should_cancel(void *ctx)
{
    (void)ctx;
    return (int)InterlockedCompareExchange(&s_cancel, 0, 0);
}

/* ---------------------------------------------------------------- worker -- */

static DWORD WINAPI tgprev_thread_proc(LPVOID param)
{
    TD5_TrackGenPreviewSink sink;
    TD5_TrackGenPreviewStats stats;
    int ok;

    (void)param;

    sink.on_points     = tgprev_on_points;
    sink.should_cancel = tgprev_should_cancel;
    sink.ctx           = NULL;

    memset(&stats, 0, sizeof(stats));
    ok = td5_trackgen_preview_route(&s_spec, &sink, &stats);

    EnterCriticalSection(&s_lock);
    s_stats   = stats;
    s_ok      = ok;
    s_done    = 1;
    s_running = 0;
    LeaveCriticalSection(&s_lock);

    if (ok)
        TD5_LOG_I(LOG_TAG, "tgprev: seed=%u previewed %d points "
                  "(%d spans, ring %d, %d forks)",
                  s_spec.seed, s_count, stats.span_count, stats.ring_len,
                  stats.fork_count);
    else if (stats.cancelled)
        TD5_LOG_I(LOG_TAG, "tgprev: seed=%u cancelled", s_spec.seed);

    return 0;
}

/* Join whatever is running. Caller must NOT hold s_lock -- the worker takes it
 * on its way out, so holding it here would deadlock the join. */
static void tgprev_join(void)
{
    if (!s_thread) return;
    /* The walk polls the cancel flag at every centerline node, so this returns
     * promptly. The generous timeout is only a wedge guard. */
    if (WaitForSingleObject(s_thread, 5000) == WAIT_TIMEOUT)
        TD5_LOG_E(LOG_TAG, "tgprev: worker did not exit in 5s; leaking the "
                  "handle rather than killing it mid-generator");
    else
        CloseHandle(s_thread);
    s_thread = NULL;
}

/* ------------------------------------------------------------------ API --- */

int td5_tgprev_init(void)
{
    if (!s_lock_init) {
        InitializeCriticalSection(&s_lock);
        s_lock_init = 1;
    }
    s_count = 0;
    s_generation = 0;
    s_running = s_done = s_ok = 0;
    return 1;
}

void td5_tgprev_shutdown(void)
{
    td5_tgprev_cancel_join();
    if (s_lock_init) {
        DeleteCriticalSection(&s_lock);
        s_lock_init = 0;
    }
}

void td5_tgprev_cancel_join(void)
{
    if (!s_lock_init) return;
    InterlockedExchange(&s_cancel, 1);
    tgprev_join();
    InterlockedExchange(&s_cancel, 0);
    EnterCriticalSection(&s_lock);
    s_running = 0;
    LeaveCriticalSection(&s_lock);
}

int td5_tgprev_request(const TD5_TrackGenSpec *spec)
{
    int gen;

    if (!spec || !s_lock_init) return 0;

    /* Cancel + join FIRST: the generator is single-instance, so a second walk
     * cannot start until the previous one has fully left it. */
    td5_tgprev_cancel_join();

    s_spec = *spec;

    EnterCriticalSection(&s_lock);
    s_count   = 0;
    s_done    = 0;
    s_ok      = 0;
    s_running = 1;
    gen       = ++s_generation;
    memset(&s_stats, 0, sizeof(s_stats));
    LeaveCriticalSection(&s_lock);

    s_thread = CreateThread(NULL, 0, tgprev_thread_proc, NULL, 0, NULL);
    if (!s_thread) {
        TD5_LOG_E(LOG_TAG, "tgprev: CreateThread failed");
        EnterCriticalSection(&s_lock);
        s_running = 0;
        s_done    = 1;
        LeaveCriticalSection(&s_lock);
        return 0;
    }
    return gen;
}

void td5_tgprev_status(TD5_TgPrevStatus *out)
{
    if (!out) return;
    if (!s_lock_init) {
        memset(out, 0, sizeof(*out));
        return;
    }
    EnterCriticalSection(&s_lock);
    out->generation = s_generation;
    out->running    = s_running;
    out->done       = s_done;
    out->ok         = s_ok;
    out->total      = s_count;
    out->stats      = s_stats;
    LeaveCriticalSection(&s_lock);
}

int td5_tgprev_fetch(int from_index, TD5_TrackGenPoint *out, int max)
{
    int n = 0;
    if (!out || max <= 0 || from_index < 0 || !s_lock_init) return 0;
    EnterCriticalSection(&s_lock);
    if (from_index < s_count) {
        n = s_count - from_index;
        if (n > max) n = max;
        memcpy(out, &s_points[from_index], (size_t)n * sizeof(*out));
    }
    LeaveCriticalSection(&s_lock);
    return n;
}
