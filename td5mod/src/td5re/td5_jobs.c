/* td5_jobs.c — fork/join worker pool (Win32 CONDITION_VARIABLE based).
 *
 * See td5_jobs.h for the contract. Correctness model:
 *   - One global batch descriptor guarded by a CRITICAL_SECTION.
 *   - Workers sleep on cv_work; a new batch bumps `generation` and wakes them.
 *   - Each thread (workers + the caller) claims indices via next++ under the
 *     lock, runs fn OUTSIDE the lock, then bumps `done` under the lock.
 *   - The last finisher (done == count) wakes cv_done; the caller sleeps on
 *     cv_done until done == count, then returns.
 * This avoids counting-semaphore token accounting entirely and is reentrancy-
 * safe against spurious wakeups (workers compare `generation`).
 */
#include "td5_jobs.h"
#include "td5_platform.h"

/* CONDITION_VARIABLE / SleepConditionVariableCS require Vista+ (0x0600). MinGW
 * may default _WIN32_WINNT lower and hide the declarations, so pin it here
 * before windows.h. Defensive #undef in case a prior TU set it lower. */
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
#  undef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define LOG_TAG "platform"

static struct {
    int                inited;
    int                worker_count;
    HANDLE             threads[TD5_JOBS_MAX_WORKERS];

    CRITICAL_SECTION   lock;
    CONDITION_VARIABLE cv_work;   /* workers wait here for a new batch        */
    CONDITION_VARIABLE cv_done;   /* the caller waits here for batch finish    */

    volatile int       running;   /* 0 -> workers should exit                  */
    unsigned long      generation;/* bumped once per batch; workers compare    */

    /* current batch (valid only while a parallel_for is in flight) */
    td5_job_fn         fn;
    void              *ctx;
    int                count;
    int                next;      /* next index to claim                       */
    int                done;      /* completed indices                         */
} g;

/* Drain remaining indices of the current batch. MUST be called with `lock`
 * held; returns with `lock` held. Runs fn() with the lock released. */
static void jobs_run_current_locked(void)
{
    for (;;) {
        if (g.next >= g.count)
            break;
        int i = g.next++;
        td5_job_fn fn = g.fn;
        void *ctx = g.ctx;
        LeaveCriticalSection(&g.lock);
        fn(i, ctx);
        EnterCriticalSection(&g.lock);
        if (++g.done == g.count)
            WakeAllConditionVariable(&g.cv_done);
    }
}

static DWORD WINAPI jobs_worker_main(LPVOID arg)
{
    (void)arg;
    EnterCriticalSection(&g.lock);
    unsigned long seen = g.generation;
    for (;;) {
        /* Wait for a new batch (or shutdown). Guard against spurious wakeups
         * by comparing the generation counter. */
        while (g.running && g.generation == seen)
            SleepConditionVariableCS(&g.cv_work, &g.lock, INFINITE);
        if (!g.running)
            break;
        seen = g.generation;
        jobs_run_current_locked();
    }
    LeaveCriticalSection(&g.lock);
    return 0;
}

int td5_jobs_init(int requested_workers)
{
    if (g.inited)
        return g.worker_count;

    int n = requested_workers;
    int env_set = 0;

    /* Env kill-switch / override: TD5RE_JOBS=N forces N workers (0 = run every
     * parallel_for serially on the caller). Useful as an A/B knob and a safety
     * escape hatch if the pool ever misbehaves on a given machine. */
    {
        char buf[16];
        DWORD got = GetEnvironmentVariableA("TD5RE_JOBS", buf, sizeof(buf));
        if (got > 0 && got < sizeof(buf)) {
            int v = atoi(buf);
            if (v >= 0) {
                n = v;
                env_set = 1;
                TD5_LOG_I(LOG_TAG, "td5_jobs_init: TD5RE_JOBS=%d overrides worker count", v);
            }
        }
    }

    if (!env_set && n <= 0) {
        /* auto: no explicit request and no env override */
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = (int)si.dwNumberOfProcessors - 2;   /* leave main + 1 spare */
    }
    if (n < 0) n = 0;   /* 0 = explicit serial (TD5RE_JOBS=0); pool inits with no workers */
    if (n > TD5_JOBS_MAX_WORKERS) n = TD5_JOBS_MAX_WORKERS;

    InitializeCriticalSection(&g.lock);
    InitializeConditionVariable(&g.cv_work);
    InitializeConditionVariable(&g.cv_done);
    g.running = 1;
    g.generation = 0;
    g.fn = NULL; g.ctx = NULL; g.count = 0; g.next = 0; g.done = 0;

    int created = 0;
    for (int i = 0; i < n; i++) {
        g.threads[i] = CreateThread(NULL, 0, jobs_worker_main, NULL, 0, NULL);
        if (g.threads[i] == NULL)
            break;
        created++;
    }
    g.worker_count = created;
    g.inited = 1;

    if (created == 0)
        TD5_LOG_W(LOG_TAG, "td5_jobs_init: no worker threads created; parallel_for runs serially");
    else
        TD5_LOG_I(LOG_TAG, "td5_jobs_init: %d worker thread(s) (requested=%d)", created, requested_workers);
    return created;
}

void td5_jobs_shutdown(void)
{
    if (!g.inited)
        return;
    EnterCriticalSection(&g.lock);
    g.running = 0;
    g.generation++;                      /* break workers out of the cv wait */
    WakeAllConditionVariable(&g.cv_work);
    LeaveCriticalSection(&g.lock);

    if (g.worker_count > 0)
        WaitForMultipleObjects((DWORD)g.worker_count, g.threads, TRUE, INFINITE);
    for (int i = 0; i < g.worker_count; i++) {
        if (g.threads[i]) CloseHandle(g.threads[i]);
        g.threads[i] = NULL;
    }
    DeleteCriticalSection(&g.lock);
    g.worker_count = 0;
    g.inited = 0;
    TD5_LOG_I(LOG_TAG, "td5_jobs_shutdown: pool torn down");
}

int td5_jobs_worker_count(void)
{
    return g.inited ? g.worker_count : 0;
}

/* [Phase B render-transform] Self-test: confirm GCC __thread (TLS) is genuinely
 * per-worker on the CreateThread pool (MinGW emutls can misbehave). Each job sets
 * a __thread var to its index, busy-spins to force concurrent overlap, then checks
 * the value survived. Returns the number of failures (0 = TLS is properly isolated;
 * >0 = __thread is shared/broken across workers -> must use Win32 TLS instead). */
static __thread volatile int s_tls_probe;
static volatile LONG s_tls_fail;
static void tls_probe_job(int i, void *ctx)
{
    volatile long sink = 0;
    (void)ctx;
    s_tls_probe = i;
    for (long k = 0; k < 3000000; k++) sink += k;   /* ~ms spin: ensure overlap */
    if (s_tls_probe != i) InterlockedIncrement(&s_tls_fail);
}
int td5_jobs_selftest_tls(void)
{
    s_tls_fail = 0;
    td5_jobs_parallel_for(8, tls_probe_job, NULL);
    TD5_LOG_W(LOG_TAG, "jobs TLS selftest: %ld failure(s) over 8 jobs / %d workers (0 = __thread isolated)",
              (long)s_tls_fail, g.worker_count);
    return (int)s_tls_fail;
}

void td5_jobs_parallel_for(int count, td5_job_fn fn, void *ctx)
{
    if (count <= 0 || fn == NULL)
        return;

    /* Serial fallback: no pool, single index, or pool not initialized. */
    if (!g.inited || g.worker_count == 0 || count == 1) {
        for (int i = 0; i < count; i++)
            fn(i, ctx);
        return;
    }

    EnterCriticalSection(&g.lock);
    g.fn = fn;
    g.ctx = ctx;
    g.count = count;
    g.next = 0;
    g.done = 0;
    g.generation++;
    WakeAllConditionVariable(&g.cv_work);

    /* Main thread participates as a worker, then waits for completion. */
    jobs_run_current_locked();
    while (g.done < g.count)
        SleepConditionVariableCS(&g.cv_done, &g.lock, INFINITE);

    /* Clear the batch so a stray late wakeup finds no work. */
    g.fn = NULL; g.ctx = NULL; g.count = 0; g.next = 0; g.done = 0;
    LeaveCriticalSection(&g.lock);
}
