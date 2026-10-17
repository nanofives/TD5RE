#ifndef TD5_HITCHWATCH_H
#define TD5_HITCHWATCH_H

/* ============================================================================
 * td5_hitchwatch -- DEV-only main-thread stall sampler (PORT-ONLY, roam F3-B).
 *
 * TD5RE_HITCH_WATCH_MS=N (default 0 = off) starts a watchdog thread on the
 * first heartbeat. The main thread beats at every profiler mark / frame begin
 * (td5_profile.c calls td5_hitch_beat, whether or not the profiler is on).
 * When the main thread has not beaten for N ms, the watchdog suspends it,
 * captures its context, unwinds its stack (RtlVirtualUnwind over the x64 .pdata),
 * resumes it, and writes one record to log/hitch.txt: the zone it was in, how
 * long, CPU cycles the main thread used since the beat (computing vs blocked),
 * process page faults since the beat, what the roam stream worker was doing,
 * and the frames as module+offset (symbolize with addr2line, base 0x140000000).
 * It samples again every TD5RE_HITCH_WATCH_EVERY_MS (50) while the stall lasts.
 * One record per sample; nothing is allocated or written while the thread is
 * suspended.
 *
 * Compiled out of RELEASE (the calls become no-ops).
 * ========================================================================== */

#ifndef TD5RE_RELEASE
void td5_hitch_beat(const char *zone);
/* Optional: a "what is the background worker doing" string, read from the
 * watchdog thread (must return a pointer to a static string). */
typedef const char *(*TD5_HitchWorkerFn)(void);
void td5_hitch_set_worker_probe(TD5_HitchWorkerFn fn);
/* Count of samples taken so far this process (for selftest/report lines). */
long td5_hitch_samples(void);
#else
#define td5_hitch_beat(zone)             ((void)0)
#define td5_hitch_set_worker_probe(fn)   ((void)0)
#define td5_hitch_samples()              (0L)
#endif

#endif /* TD5_HITCHWATCH_H */
