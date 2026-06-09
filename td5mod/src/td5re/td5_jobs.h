/* td5_jobs.h — portable fork/join job system for the TD5RE source port.
 *
 * F1 of the multithreading work (re/analysis/multithreading_architecture_plan_2026-06-08.md).
 * A fixed Win32 worker pool with a single fork/join primitive (parallel_for).
 * Reused by:
 *   - Phase A (parallel asset decode: ZIP inflate / pack-on-load / texture decode)
 *   - Phase B (per-pane deferred-context render recording)
 *
 * Design constraints (deliberately minimal & robust):
 *   - parallel_for is a BARRIER: it blocks the caller until every index has run.
 *   - The calling (main) thread PARTICIPATES as a worker, so it is never idle
 *     and the primitive degrades to a plain serial loop when worker_count == 0
 *     (this is the always-correct serial fallback every phase relies on).
 *   - NOT reentrant: a job function must not itself call td5_jobs_parallel_for
 *     (there is a single global batch descriptor). Phases A and B do not nest.
 *   - Determinism: parallel_for makes NO ordering guarantee across indices.
 *     Only use it for work whose per-index effects are independent, or whose
 *     results are recombined later in a deterministic order by the caller.
 */
#ifndef TD5_JOBS_H
#define TD5_JOBS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Per-index work function. `index` is in [0, count); `ctx` is the opaque
 * pointer passed to td5_jobs_parallel_for (shared across all indices). */
typedef void (*td5_job_fn)(int index, void *ctx);

/* Start the worker pool. `requested_workers`:
 *   <= 0  -> auto (logical CPUs - 2, clamped to [1, TD5_JOBS_MAX_WORKERS]).
 *   == 1  -> still spins up 1 worker (parallel_for then uses worker+main).
 * Calling init twice is a no-op (returns the existing worker count).
 * Returns the number of worker threads actually created (0 if threads could
 * not be created -> parallel_for silently runs serially). */
int  td5_jobs_init(int requested_workers);

/* Tear down the pool, joining all workers. Safe to call if never inited. */
void td5_jobs_shutdown(void);

/* Number of background worker threads (NOT counting the main thread). 0 means
 * parallel_for runs everything on the calling thread. */
int  td5_jobs_worker_count(void);

/* Run fn(0..count-1, ctx) across the pool + the calling thread and block until
 * all have completed. count <= 0 is a no-op. With no workers, runs in-order on
 * the calling thread. */
void td5_jobs_parallel_for(int count, td5_job_fn fn, void *ctx);

/* Diagnostic: returns 0 if GCC __thread is per-worker isolated on the pool,
 * >0 (failure count) if __thread is shared/broken across workers. */
int  td5_jobs_selftest_tls(void);

#define TD5_JOBS_MAX_WORKERS 32

#ifdef __cplusplus
}
#endif

#endif /* TD5_JOBS_H */
