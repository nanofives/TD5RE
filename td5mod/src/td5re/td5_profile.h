#ifndef TD5_PROFILE_H
#define TD5_PROFILE_H

/* ============================================================================
 * td5_profile -- lightweight per-phase frame profiler.
 *
 * Gated by [Logging] Profile (off by default => ~zero cost when off, so it can
 * live in the code permanently). Bracket the sequential phases of a frame with
 * td5_profile_mark() between begin_frame()/end_frame(); each mark attributes the
 * time SINCE the previous mark to a named zone. Per-zone avg/max ms over a
 * 1-second window is logged ("PROFILE (ms avg/max)  zone=avg/max ...") to
 * engine.log, so when the always-on FPS/peak counter says "something is slow"
 * this says WHERE.
 *
 *   td5_profile_begin_frame();
 *   ... phase A ...   td5_profile_mark("a");
 *   ... phase B ...   td5_profile_mark("b");
 *   td5_profile_end_frame();
 *
 * High-resolution (QueryPerformanceCounter), so sub-millisecond phases resolve
 * (unlike the 1ms td5_plat_time_ms). Uses the actual mark string as the zone key
 * (string literals), so passing the trace-stage name straight through profiles
 * the whole race sim with one hook.
 * ========================================================================== */

void td5_profile_set_enabled(int on);
int  td5_profile_enabled(void);

void td5_profile_begin_frame(void);
void td5_profile_mark(const char *zone);
void td5_profile_end_frame(void);

/* [round UA 2026-11-08] FRAME-TIME PERCENTILES (dev measurement, env-gated,
 * independent of [Logging] Profile). TD5RE_FRAMESTATS=N records the wall-clock
 * interval between N consecutive race frames (after skipping the first
 * TD5RE_FRAMESTATS_SKIP frames, default 120 = loading + countdown settle), then
 * appends one block to log/framestats.txt: mean / p50 / p90 / p95 / p99 / p99.9
 * / max, the count of frames over 2x the median ("spikes") and over 33.3 ms,
 * plus the GPU per-pass summary + VRAM budget from the backend (needs
 * TD5RE_RT_TIMESTAMPS=2 for the per-pass part). TD5RE_FRAMESTATS_TAG labels the
 * block. Call once per race frame; returns 1 the frame the report was written
 * and TD5RE_FRAMESTATS_QUIT (default 1) asks for a clean quit. */
int td5_profile_race_frame(void);

#endif /* TD5_PROFILE_H */
