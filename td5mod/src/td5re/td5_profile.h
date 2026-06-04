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

#endif /* TD5_PROFILE_H */
